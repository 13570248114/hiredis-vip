
#include "fmacros.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include<ctype.h>

#include "hircluster.h"
#include "hiutil.h"
#include "hiarray.h"
#include "command.h"
#include "dict.c"
#include "async.h"

#define REDIS_COMMAND_CLUSTER_NODES "CLUSTER NODES"
#define REDIS_COMMAND_CLUSTER_SLOTS "CLUSTER SLOTS"

#define IP_PORT_SEPARATOR ":"

#define CLUSTER_ADDRESS_SEPARATOR ","

#define CLUSTER_DEFAULT_MAX_REDIRECT_COUNT 5

static void cluster_node_deinit(cluster_node *node);

unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

void dictClusterNodeDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    cluster_node_deinit(val);
}

/* Cluster nodes hash table, mapping nodes addresses 1.2.3.4:6379 to
 * clusterNode structures. */
dictType clusterNodesDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictClusterNodeDestructor   /* val destructor */
};

void listCommandFree(void *command)
{
	struct cmd *cmd = command;
	command_destroy(cmd);
}

/* Defined in hiredis.c */
void __redisSetError(redisContext *c, int type, const char *str);

/* Forward declaration of function in hiredis.c */
int __redisAppendCommand(redisContext *c, const char *cmd, size_t len);

/* Helper function for the redisClusterCommand* family of functions.
 *
 * Write a formatted command to the output buffer. If the given context is
 * blocking, immediately read the reply into the "reply" pointer. When the
 * context is non-blocking, the "reply" pointer will not be used and the
 * command is simply appended to the write buffer.
 *
 * Returns the reply when a reply was succesfully retrieved. Returns NULL
 * otherwise. When NULL is returned in a blocking context, the error field
 * in the context will be set.
 */
static void *__redisBlockForReply(redisContext *c) {
    void *reply;

    if (c->flags & REDIS_BLOCK) {
        if (redisGetReply(c,&reply) != REDIS_OK)
            return NULL;
        return reply;
    }
    return NULL;
}


/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
static unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing betweeen {} ? Hash the whole key. */
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

static void __redisClusterSetError(redisClusterContext *cc, int type, const char *str) {
    size_t len;

    cc->err = type;
    if (str != NULL) {
        len = strlen(str);
        len = len < (sizeof(cc->errstr)-1) ? len : (sizeof(cc->errstr)-1);
        memcpy(cc->errstr,str,len);
        cc->errstr[len] = '\0';
    } else {
        /* Only REDIS_ERR_IO may lack a description! */
        assert(type == REDIS_ERR_IO);
        __redis_strerror_r(errno, cc->errstr, sizeof(cc->errstr));
    }
}

static int cluster_node_init(cluster_node *node)
{
	node->name = NULL;
	node->addr = NULL;
	node->host = NULL;
	node->port = 0;
	node->master = 1;
	node->count = 0;
	node->slave_of = NULL;
	node->con = NULL;
	node->acon = NULL;
	node->slots = NULL;
	
	return REDIS_OK;
}

static void cluster_node_deinit(cluster_node *node)
{	
	if(node == NULL)
	{
		return;
	}

	if(node->count > 0)
	{
		return;
	}
	
	sdsfree(node->name);
	sdsfree(node->addr);
	sdsfree(node->host);
	node->port = 0;
	node->master = 1;
	node->slave_of = NULL;

	if(node->con != NULL)
	{
		redisFree(node->con);
	}

	if(node->acon != NULL)
	{
		redisAsyncFree(node->acon);
	}

	if(node->slots != NULL)
	{
		listRelease(node->slots);
	}
	
}

static int cluster_slot_init(cluster_slot *slot, cluster_node *node)
{
	slot->start = 0;
	slot->end = 0;
	if(node != NULL)
	{
		node->count ++;			
	}
	slot->node = node;
	
	return REDIS_OK;
}

static int cluster_slot_deinit(cluster_slot *slot)
{
	cluster_node *node;
	slot->start = 0;
	slot->end = 0;
	if(slot->node != NULL)
	{
		node = slot->node;
		node->count --;
		slot->node = NULL;
	}
	
	hi_free(slot);
	
	return REDIS_OK;
}

static int cluster_slot_ref_node(cluster_slot *slot, cluster_node *node)
{
	cluster_node *node_old;
	
	if(slot->node != NULL)
	{
		node_old = slot->node;
		node_old->count --;
	}

	if(node != NULL)
	{
		node->count ++;
		listAddNodeTail(node->slots, slot);
	}

	slot->node = node;
	
	return REDIS_OK;
}



static int
cluster_slot_start_cmp(const void *t1, const void *t2)
{
    const cluster_slot **s1 = t1, **s2 = t2;

    return (*s1)->start > (*s2)->start?1:-1;
}

static int 
cluster_update_route_with_slots(redisClusterContext *cc,
	const char *ip, int port)
{
	redisContext *c;
	redisReply *reply = NULL;
	redisReply *elem;
	redisReply *elem_slots_begin, *elem_slots_end;
	redisReply *elem_node_master;
	redisReply *elem_ip, *elem_port;
	struct array *slots = NULL;
	cluster_slot *slot;
	cluster_node *node;
	const char *errstr = NULL;
	int err = 0;
	unsigned int i, idx;

	if(cc == NULL)
	{
		return REDIS_ERR;
	}
	
	if(cc->flags & REDIS_BLOCK)
	{
		if(cc->timeout)
		{
			c = redisConnectWithTimeout(ip, port, *cc->timeout);
		}
		else
		{
			c = redisConnect(ip, port);
		}
	}
	else
	{
		c = redisConnectNonBlock(ip, port);
	}
	
	if (c == NULL)
    {
		err = REDIS_ERR_OTHER;
		errstr = "init redis context error(return NULL)!\0";
		goto error;
    }
	else if(c->err)
	{
		err = c->err;
		errstr = c->errstr;
		goto error;
	}

	reply = redisCommand(c, REDIS_COMMAND_CLUSTER_SLOTS);

	if(reply == NULL)
	{
		err = REDIS_ERR_OTHER;
		errstr = "command(cluster slots) reply error(NULL)!\0";
		goto error;
	}
	
	if(reply->type != REDIS_REPLY_ARRAY || reply->elements <= 0)
	{
		err = REDIS_ERR_OTHER;
		errstr = "command(cluster slots) reply"
			" error(level 0 type is not array)!\0";
		goto error;
	}
	
	slots = array_create(reply->elements, sizeof(cluster_slot));
	if(slots == NULL)
	{
		err = REDIS_ERR_OTHER;
		errstr = "array create error!\0";
		goto error;
	}

	for(i = 0; i < reply->elements; i ++)
	{
		elem = reply->element[i];
		if(elem->type != REDIS_REPLY_ARRAY || elem->elements <= 0)
		{
			err = REDIS_ERR_OTHER;
			errstr = "command(cluster slots) reply"
				" error(level 1 type is not array)!\0";
			goto error;
		}

		slot = array_push(slots);
		if(slot == NULL)
		{
			err = REDIS_ERR_OTHER;
			errstr = "slot push in array error!\0";
			goto error;
		}

		node = hi_alloc(sizeof(cluster_node));
		if(node == NULL)
		{
			err = REDIS_ERR_OTHER;
			errstr = "alloc cluster node error!\0";
			goto error;
		}
		
		cluster_node_init(node);
		cluster_slot_ref_node(slot, node);
		
		for(idx = 0; idx < elem->elements; idx ++)
		{
			if(idx == 0)
			{
				elem_slots_begin = elem->element[idx];
				if(elem_slots_begin->type != REDIS_REPLY_INTEGER)
				{	
					err = REDIS_ERR_OTHER;
					errstr = "command(cluster slots) reply"
						" error(slot begin is not integer)!\0";
					goto error;
				}
				slot->start = (int)(elem_slots_begin->integer);
			}
			else if(idx == 1)
			{
				elem_slots_end = elem->element[idx];
				if(elem_slots_end->type != REDIS_REPLY_INTEGER)
				{
					err = REDIS_ERR_OTHER;
					errstr = "command(cluster slots) reply"
						" error(slot end is not integer)!\0";
					goto error;
				}
				slot->end = (int)(elem_slots_end->integer);

				if(slot->start > slot->end)
				{
					err = REDIS_ERR_OTHER;
					errstr = "command(cluster slots) reply"
						" error(slot begin is bigger than slot end)!\0";
					goto error;
				}
			}
			else if(idx == 2)
			{
				elem_node_master = elem->element[idx];
				if(elem_node_master->type != REDIS_REPLY_ARRAY || 
					elem_node_master->elements != 2)
				{
					err = REDIS_ERR_OTHER;
					errstr = "command(cluster slots) reply"
						" error(master line is not array)!\0";
					goto error;
				}

				elem_ip = elem_node_master->element[0];
				elem_port = elem_node_master->element[1];

				if(elem_ip->type != REDIS_REPLY_STRING ||
					elem_ip->len <= 0)
				{
					err = REDIS_ERR_OTHER;
					errstr = "command(cluster slots) reply"
						" error(master ip is not string)!\0";
					goto error;
				}

				if(elem_port->type != REDIS_REPLY_INTEGER ||
					elem_port->integer <= 0)
				{
					err = REDIS_ERR_OTHER;
					errstr = "command(cluster slots) reply"
						" error(master port is not integer)!\0";
					goto error;
				}

				node->host = sdsnewlen(elem_ip->str, elem_ip->len);
				node->port = (int)(elem_port->integer);

				node->addr = sdsnewlen(elem_ip->str, elem_ip->len);
				sdscatlen(node->addr, ":", 1);
				node->addr = sdscatfmt(node->addr, "%I", node->port);
			}
			else
			{
				continue;
			}
		}
	}
	
	cc->slots = slots;

	array_sort(cc->slots, cluster_slot_start_cmp);

	freeReplyObject(reply);

	if (c != NULL)
    {
		redisFree(c);
	}

	return REDIS_OK;

error:

	cc->err = err;
	memcpy(cc->errstr, errstr, strlen(errstr));

	if(slots != NULL)
	{		
		while(array_n(slots))
		{
			slot = array_pop(slots);
			cluster_slot_deinit(slot);
		}
		
		array_destroy(slots);
	}

	if(reply != NULL)
	{
		freeReplyObject(reply);
		reply = NULL;
	}

	if (c != NULL)
    {
		redisFree(c);
	}
	return REDIS_ERR;
}

static int 
cluster_update_route_with_nodes(redisClusterContext *cc, 
	const char *ip, int port)
{
	redisContext *c = NULL;
	redisReply *reply = NULL;
	struct array *slots = NULL;
	dict *nodes = NULL;
	cluster_node *node;
	cluster_slot **slot;
	char *pos, *start, *end, *line_start, *line_end;
	char *role;
	int role_len;
	uint8_t myself = 0;
	int slot_start, slot_end;
	const char *errstr = NULL;
	int err = 0;
	sds *part = NULL, *ip_port = NULL, *slot_start_end = NULL;
	int count_part = 0, count_ip_port = 0, count_slot_start_end = 0;
	int j, k;
	int len;
	cluster_node *table[REDIS_CLUSTER_SLOTS] = {NULL};

	if(cc == NULL)
	{
		return REDIS_ERR;
	}

	if(ip == NULL || port <= 0)
	{
		err = REDIS_ERR_OTHER;
		errstr = "ip or port error!\0";
		goto error;
	}

	if(cc->timeout)
	{
		c = redisConnectWithTimeout(ip, port, *cc->timeout);
	}
	else
	{
		c = redisConnect(ip, port);
	}
		
	if (c == NULL)
    {
		err = REDIS_ERR_OTHER;
		errstr = "init redis context error(return NULL)!\0";
		goto error;
    }
	else if(c->err)
	{
		err = c->err;
		errstr = c->errstr;
		goto error;
	}

	reply = redisCommand(c, REDIS_COMMAND_CLUSTER_NODES);

	if(reply == NULL)
	{
		err = REDIS_ERR_OTHER;
		errstr = "command(cluster nodes) reply error(NULL)!\0";
		goto error;
	}
	else if(reply->type != REDIS_REPLY_STRING)
	{
		err = REDIS_ERR_OTHER;
		if(reply->type == REDIS_REPLY_ERROR)
		{
			errstr = reply->str;
		}
		else
		{
			errstr = "command(cluster nodes) reply error(type is not string)!\0";
		}
		
		goto error;
	}

	nodes = dictCreate(&clusterNodesDictType, NULL);
	
	slots = array_create(10, sizeof(cluster_slot*));
	if(slots == NULL)
	{
		err = REDIS_ERR_OTHER;
		errstr = "array create error!\0";
		goto error;
	}

	start = reply->str;
	end = start + reply->len;
	
	line_start = start;

	for(pos = start; pos < end; pos ++)
	{
		if(*pos == '\n')
		{
			line_end = pos - 1;
			len = line_end - line_start;
			
			part = sdssplitlen(line_start, len + 1, " ", 1, &count_part);

			if(part == NULL || count_part < 8)
			{
				err = REDIS_ERR_OTHER;
				errstr = "split cluster nodes error!\0";
				goto error;
			}

			if(sdslen(part[2]) >= 7 && memcmp(part[2], "myself,", 7) == 0)
			{
				role_len = sdslen(part[2]) - 7;
				role = part[2] + 7;
				myself = 1;
			}
			else
			{
				role_len = sdslen(part[2]);
				role = part[2];
			}

			if(role_len >= 6 && memcmp(role, "master", 6) == 0)
			{
				if(count_part < 8)
				{
					err = REDIS_ERR_OTHER;
					errstr = "master node part number error!\0";
					goto error;
				}

				node = hi_alloc(sizeof(cluster_node));
				if(node == NULL)
				{
					err = REDIS_ERR_OTHER;
					errstr = "alloc cluster node error!\0";
					goto error;
				}

				cluster_node_init(node);
				
				node->slots = listCreate();
				if(node->slots == NULL)
				{
					hi_free(node);
					err = REDIS_ERR_OTHER;
					errstr = "slots for node listCreate error!\0";
					goto error;
				}
				
				node->name = part[0];

				node->addr = part[1];

				dictAdd(nodes, sdsnewlen(node->addr, sdslen(node->addr)), node);
				
				ip_port = sdssplitlen(part[1], sdslen(part[1]), 
					IP_PORT_SEPARATOR, strlen(IP_PORT_SEPARATOR), &count_ip_port);
				if(ip_port == NULL || count_ip_port != 2)
				{
					err = REDIS_ERR_OTHER;
					errstr = "split ip port error!\0";
					goto error;
				}
				node->host = ip_port[0];
				node->port = hi_atoi(ip_port[1], sdslen(ip_port[1]));
				node->master = 1;
				if(myself == 1)
				{
					node->con = c;
					c = NULL;
					myself = 0;
				}
				
				sdsfree(ip_port[1]);
				free(ip_port);
				count_ip_port = 0;
				ip_port = NULL;
				
				for(k = 8; k < count_part; k ++)
				{
					slot_start_end = sdssplitlen(part[k], 
						sdslen(part[k]), "-", 1, &count_slot_start_end);
					
					if(slot_start_end == NULL)
					{
						err = REDIS_ERR_OTHER;
						errstr = "split slot start end error(NULL)!\0";
						goto error;
					}
					else if(count_slot_start_end == 1)
					{
						slot_start = hi_atoi(slot_start_end[0], sdslen(slot_start_end[0]));
						slot_end = slot_start;
					}
					else if(count_slot_start_end == 2)
					{
						slot_start = hi_atoi(slot_start_end[0], sdslen(slot_start_end[0]));;
						slot_end = hi_atoi(slot_start_end[1], sdslen(slot_start_end[1]));;
					}
					else
					{
						slot_start = -1;
						slot_end = -1;
					}
					
					sdsfreesplitres(slot_start_end, count_slot_start_end);
					count_slot_start_end = 0;
					slot_start_end = NULL;

					if(slot_start < 0 || slot_end < 0 || 
						slot_start > slot_end || slot_end >= REDIS_CLUSTER_SLOTS)
					{
						continue;
					}

					for(j = slot_start; j <= slot_end; j ++)
					{
						if(table[j] != NULL)
						{
							err = REDIS_ERR_OTHER;
							errstr = "diffent node hold a same slot!\0";
							goto error;
						}
						table[j] = node;
					}
					
					slot = array_push(slots);
					if(slot == NULL)
					{
						err = REDIS_ERR_OTHER;
						errstr = "slot push in array error!\0";
						goto error;
					}

					*slot = hi_alloc(sizeof(**slot));
					if(*slot == NULL)
					{
						err = REDIS_ERR_OTHER;
						errstr = "alloc slot error!\0";
						goto error;
					}
					
					cluster_slot_init(*slot, NULL);

					(*slot)->start = (uint32_t)slot_start;
					(*slot)->end = (uint32_t)slot_end;
					cluster_slot_ref_node(*slot, node);
					
				}

				for(k = 2; k < count_part; k ++)
				{
					sdsfree(part[k]);
				}
				free(part);
				count_part = 0;
				part = NULL;
			}
			else
			{
				if(myself == 1)
				{
					myself = 0;
				}
				sdsfreesplitres(part, count_part);
				count_part = 0;
				part = NULL;
			}
			
			start = pos + 1;
			line_start = start;
			pos = start;
		}
	}

	if(cc->slots != NULL)
	{
		while(array_n(cc->slots))
		{
			slot = array_pop(cc->slots);
			cluster_slot_deinit(*slot);
		}
		
		array_destroy(cc->slots);
		cc->slots = NULL;
	}
	cc->slots = slots;

	if(cc->nodes != NULL)
	{
		dictRelease(cc->nodes);
	}
	cc->nodes = nodes;

	array_sort(cc->slots, cluster_slot_start_cmp);

	memcpy(cc->table, table, REDIS_CLUSTER_SLOTS*sizeof(cluster_node *));
	
	freeReplyObject(reply);

	if(c != NULL)
	{
		redisFree(c);
	}
	
	return REDIS_OK;

error:

	__redisClusterSetError(cc, err, errstr);
		
	if(part != NULL)
	{
		sdsfreesplitres(part, count_part);
		count_part = 0;
		part = NULL;
	}

	if(ip_port != NULL)
	{
		sdsfreesplitres(ip_port, count_ip_port);
		count_ip_port = 0;
		ip_port = NULL;
	}

	if(slot_start_end != NULL)
	{
		sdsfreesplitres(slot_start_end, count_slot_start_end);
		count_slot_start_end = 0;
		slot_start_end = NULL;
	}

	if(slots != NULL)
	{
		if(slots == cc->slots)
		{
			cc->slots = NULL;
		}
		
		while(array_n(slots))
		{
			slot = array_pop(slots);
			cluster_slot_deinit(*slot);
		}
		
		array_destroy(slots);
	}

	if(nodes != NULL)
	{
		if(nodes == cc->nodes)
		{
			cc->nodes = NULL;
		}

		dictRelease(cc->nodes);
	}

	if(reply != NULL)
	{
		freeReplyObject(reply);
		reply = NULL;
	}

	if(c != NULL)
	{
		redisFree(c);
	}
	
	return REDIS_ERR;
}

static int
cluster_update_route(redisClusterContext *cc)
{
	int ret;
	int flag_err_not_set = 1;
	cluster_node *node;
	dictIterator *it;
    dictEntry *de;
	
	if(cc == NULL)
	{
		return REDIS_ERR;
	}

	if(cc->ip != NULL && cc->port > 0)
	{
		ret = cluster_update_route_with_nodes(cc, cc->ip, cc->port);
		if(ret == REDIS_OK)
		{
			return REDIS_OK;
		}

		flag_err_not_set = 0;
	}

	if(cc->nodes == NULL)
	{
		if(flag_err_not_set)
		{
			__redisClusterSetError(cc, REDIS_ERR_OTHER, "no server address");
		}
		
		return REDIS_ERR;
	}

	it = dictGetIterator(cc->nodes);
	while ((de = dictNext(it)) != NULL)
	{
        node = dictGetEntryVal(de);
		if(node == NULL || node->host == NULL || node->port < 0)
		{
			continue;
		}

		ret = cluster_update_route_with_nodes(cc, node->host, node->port);
		if(ret == REDIS_OK)
		{
			if(cc->err)
			{
				cc->err = 0;
				memset(cc->errstr, '\0', strlen(cc->errstr));
			}
			return REDIS_OK;
		}

		flag_err_not_set = 0;
	}
	
	dictReleaseIterator(it);

	if(flag_err_not_set)
	{
		__redisClusterSetError(cc, REDIS_ERR_OTHER, "no valid server address");
	}

	return REDIS_ERR;
}


static redisClusterContext *redisClusterContextInit(void) {
    redisClusterContext *cc;
	unsigned int i;

    cc = calloc(1,sizeof(redisClusterContext));
    if (cc == NULL)
        return NULL;

    cc->err = 0;
    cc->errstr[0] = '\0';
	cc->ip = NULL;
	cc->port = 0;
	cc->flags = 0;
    cc->timeout = NULL;
	cc->nodes = NULL;
	cc->slots = NULL;
	cc->max_redirect_count = CLUSTER_DEFAULT_MAX_REDIRECT_COUNT;
	cc->retry_count = 0;
	cc->requests = NULL;

	cc->nodes = NULL;
	for(i = 0; i < REDIS_CLUSTER_SLOTS; i ++)
	{
		cc->table[i] = NULL;
	}
	
    return cc;
}

void redisClusterFree(redisClusterContext *cc) {

	unsigned int i;
	cluster_slot **slot;
	
	if (cc == NULL)
        return;

	if(cc->ip)
	{
		sdsfree(cc->ip);
		cc->ip = NULL;
	}

    if (cc->timeout)
    {
        free(cc->timeout);
    }

	for(i = 0; i < REDIS_CLUSTER_SLOTS; i ++)
	{
		cc->table[i] = NULL;
	}

	if(cc->slots != NULL)
	{
		while(array_n(cc->slots))
		{
			slot = array_pop(cc->slots);
			cluster_slot_deinit(*slot);
		}
		
		array_destroy(cc->slots);
		cc->slots = NULL;
	}

	if(cc->nodes != NULL)
	{
		dictRelease(cc->nodes);
	}

	if(cc->requests != NULL)
	{
		listRelease(cc->requests);
	}
	
    free(cc);
}

static int redisClusterAddNode(redisClusterContext *cc, const char *addr)
{
	dictEntry *node_entry;
	cluster_node *node;
	sds *ip_port = NULL;
	int ip_port_count = 0;
	sds ip;
	int port;
	
	if(cc == NULL)
	{
		return REDIS_ERR;
	}

	if(cc->nodes == NULL)
	{
		cc->nodes = dictCreate(&clusterNodesDictType, NULL);
		if(cc->nodes == NULL)
		{
			return REDIS_ERR;
		}
	}

	node_entry = dictFind(cc->nodes, addr);
	if(node_entry == NULL)
	{
		ip_port = sdssplitlen(addr, strlen(addr), 
			IP_PORT_SEPARATOR, strlen(IP_PORT_SEPARATOR), &ip_port_count);
		if(ip_port == NULL || ip_port_count != 2 || 
			sdslen(ip_port[0]) <= 0 || sdslen(ip_port[1]) <= 0)
		{
			if(ip_port != NULL)
			{
				sdsfreesplitres(ip_port, ip_port_count);
			}
			__redisClusterSetError(cc,REDIS_ERR_OTHER,"server address is error(correct is like: 127.0.0.1:1234)");
			return REDIS_ERR;
		}

		ip = ip_port[0];
		port = hi_atoi(ip_port[1], sdslen(ip_port[1]));

		if(port <= 0)
		{
			sdsfreesplitres(ip_port, ip_port_count);
			__redisClusterSetError(cc,REDIS_ERR_OTHER,"server port is error");
			return REDIS_ERR;
		}

		sdsfree(ip_port[1]);
		free(ip_port);
		ip_port = NULL;
	
		node = hi_alloc(sizeof(cluster_node));
		if(node == NULL)
		{
			sdsfree(ip);
			__redisClusterSetError(cc,REDIS_ERR_OTHER,"alloc cluster node error");
			return REDIS_ERR;
		}

		cluster_node_init(node);

		node->addr = sdsnew(addr);
		if(node->addr == NULL)
		{
			sdsfree(ip);
			hi_free(node);
			__redisClusterSetError(cc,REDIS_ERR_OTHER,"new node address error");
			return REDIS_ERR;
		}

		node->host = ip;
		node->port = port;

		dictAdd(cc->nodes, sdsnewlen(node->addr, sdslen(node->addr)), node);
	}
	
	return REDIS_OK;
}


/* Connect to a Redis cluster. On error the field error in the returned
 * context will be set to the return value of the error function.
 * When no set of reply functions is given, the default set will be used. */
static redisClusterContext *_redisClusterConnect(redisClusterContext *cc, const char *addrs) {

	int ret;
	sds *address = NULL;
	int address_count = 0;
	int i;

	if(cc == NULL)
	{
		return NULL;
	}
	

	address = sdssplitlen(addrs, strlen(addrs), CLUSTER_ADDRESS_SEPARATOR, 
		strlen(CLUSTER_ADDRESS_SEPARATOR), &address_count);
	if(address == NULL || address_count <= 0)
	{
		__redisClusterSetError(cc,REDIS_ERR_OTHER,"servers address is error(correct is like: 127.0.0.1:1234,127.0.0.2:5678)");
		return cc;
	}

	for(i = 0; i < address_count; i ++)
	{
		ret = redisClusterAddNode(cc, address[i]);
		if(ret != REDIS_OK)
		{
			sdsfreesplitres(address, address_count);
			return cc;
		}
	}

	sdsfreesplitres(address, address_count);
	
	cluster_update_route(cc);

    return cc;
}

redisClusterContext *redisClusterConnect(const char *addrs)
{
	redisClusterContext *cc;

	cc = redisClusterContextInit();

	if(cc == NULL)
	{
		return NULL;
	}

	cc->flags |= REDIS_BLOCK;

	return _redisClusterConnect(cc, addrs);
}

redisClusterContext *redisClusterConnectWithTimeout(const char *addrs, const struct timeval tv)
{
	redisClusterContext *cc;

	cc = redisClusterContextInit();

	if(cc == NULL)
	{
		return NULL;
	}

	cc->flags |= REDIS_BLOCK;

    if (cc->timeout == NULL)
    {
        cc->timeout = malloc(sizeof(struct timeval));
    }
	
    memcpy(cc->timeout, &tv, sizeof(struct timeval));
	
	return _redisClusterConnect(cc, addrs);
}

redisClusterContext *redisClusterConnectNonBlock(const char *addrs) {

	redisClusterContext *cc;

	cc = redisClusterContextInit();

	if(cc == NULL)
	{
		return NULL;
	}

	cc->flags &= ~REDIS_BLOCK;

	return _redisClusterConnect(cc, addrs);
}

redisContext *ctx_get_by_node(cluster_node *node, 
	const struct timeval *timeout, int flags)
{
	redisContext *c = NULL;
	if(node == NULL)
	{
		return NULL;
	}

	c = node->con;
	if(c != NULL)
	{
		if(c->err)
		{
			redisReconnect(c);
		}

		return c;
	}

	if(node->host == NULL || node->port <= 0)
	{
		__redisSetError(c, REDIS_ERR_OTHER, "node host or port is error");
		return c;
	}

	if(flags & REDIS_BLOCK)
	{
		if(timeout)
		{
			c = redisConnectWithTimeout(node->host, node->port, *timeout);
		}
		else
		{
			c = redisConnect(node->host, node->port);
		}
	}
	else
	{
		c = redisConnectNonBlock(node->host, node->port);
	}

	node->con = c;

	return c;
}

static cluster_node *node_get_by_slot(redisClusterContext *cc, uint32_t slot_num)
{
	struct array *slots;
	uint32_t slot_count;
	cluster_slot **slot;
	uint32_t middle, start, end;
	uint8_t stop = 0;
	
	if(cc == NULL)
	{
		return NULL;
	}

	if(slot_num >= REDIS_CLUSTER_SLOTS)
	{
		return NULL;
	}

	slots = cc->slots;
	if(slots == NULL)
	{
		return NULL;
	}
	slot_count = array_n(slots);

	start = 0;
	end = slot_count - 1;
	middle = 0;

	do{
		if(start >= end)
		{
			stop = 1;
			middle = end;
		}
		else
		{
			middle = start + (end - start)/2;
		}

		ASSERT(middle >= 0 && middle < slot_count);

		slot = array_get(slots, middle);
		if((*slot)->start > slot_num)
		{
			end = middle - 1;
		}
		else if((*slot)->end < slot_num)
		{
			start = middle + 1;
		}
		else
		{
			return (*slot)->node;
		}
			
		
	}while(!stop);

	printf("slot_num : %d\n", slot_num);
	printf("slot_count : %d\n", slot_count);
	printf("start : %d\n", start);
	printf("end : %d\n", end);
	printf("middle : %d\n", middle);

	return NULL;
}


static cluster_node *node_get_by_table(redisClusterContext *cc, uint32_t slot_num)
{	
	if(cc == NULL)
	{
		return NULL;
	}

	if(slot_num >= REDIS_CLUSTER_SLOTS)
	{
		return NULL;
	}

	return cc->table[slot_num];
	
}

static cluster_node *node_get_witch_connected(redisClusterContext *cc)
{
	dictIterator *di;
	dictEntry *de;
	struct cluster_node *node;
	redisContext *c = NULL;
	redisReply *reply = NULL;

	if(cc == NULL || cc->nodes == NULL)
	{
		return NULL;
	}

	di = dictGetIterator(cc->nodes);
	while((de = dictNext(di)) != NULL)
	{
		node = dictGetEntryVal(de);
		if(node == NULL)
		{
			continue;
		}
		
		c = ctx_get_by_node(node, cc->timeout, cc->flags);
		if(c == NULL || c->err)
		{
			continue;
		}

		reply = redisCommand(c, "ping");
		//printf("reply->type: %d\n", reply->type);
		//printf("reply->str: %s\n", reply->str==NULL?"NULL":reply->str);
		
		if(reply != NULL && reply->type == REDIS_REPLY_STATUS &&
			reply->str != NULL && strcmp(reply->str, "PONG") == 0)
		{
			freeReplyObject(reply);
			reply = NULL;
			
			dictReleaseIterator(di);			
		
			return node;
		}
		else if(reply != NULL)
		{
			freeReplyObject(reply);
			reply = NULL;
		}
	}

	dictReleaseIterator(di);

	return NULL;
}

static int slot_get_by_command(redisClusterContext *cc, char *cmd, int len)
{
	struct cmd *command = NULL;
	struct keypos *kp;
	int key_count;
	uint32_t i;
	int slot_num = -1;

	if(cc == NULL || cmd == NULL || len <= 0)
	{
		goto done;
	}

	command = command_get();
	if(command == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
        goto done;
	}
	
	command->cmd = cmd;
	command->clen = len;
	redis_parse_req(command);
	if(command->result != CMD_PARSE_OK)
	{
		__redisClusterSetError(cc, REDIS_ERR_PROTOCOL, "parse command error");
		goto done;
	}

	key_count = array_n(command->keys);

	if(key_count <= 0)
	{
		__redisClusterSetError(cc, REDIS_ERR_OTHER, "no keys in command(must have keys for redis cluster mode)");
		goto done;
	}
	else if(key_count == 1)
	{
		kp = array_get(command->keys, 0);
		slot_num = keyHashSlot(kp->start, kp->end - kp->start);

		goto done;
	}
	
	for(i = 0; i < array_n(command->keys); i ++)
	{
		kp = array_get(command->keys, i);

		slot_num = keyHashSlot(kp->start, kp->end - kp->start);
	}

done:
	
	if(command != NULL)
	{
		command->cmd = NULL;
		command_destroy(command);
	}
	
	return slot_num;
}

/* Helper function for the redisClusterAppendCommand* family of functions.
 *
 * Write a formatted command to the output buffer. When this family
 * is used, you need to call redisGetReply yourself to retrieve
 * the reply (or replies in pub/sub).
 */
static int __redisClusterAppendCommand(redisClusterContext *cc, 
	struct cmd *command) {

	cluster_node *node;
	redisContext *c = NULL;

	if(cc == NULL || command == NULL)
	{
		return REDIS_ERR;
	}
	
	node = node_get_by_table(cc, (uint32_t)command->slot_num);
	if(node == NULL)
	{
		__redisClusterSetError(cc, REDIS_ERR_OTHER, "node get by slot error");
		return REDIS_ERR;
	}

	c = ctx_get_by_node(node, cc->timeout, cc->flags);
	if(c == NULL)
	{
		__redisClusterSetError(cc, REDIS_ERR_OTHER, "ctx get by node is null");
		return REDIS_ERR;
	}
	else if(c->err)
	{
		__redisClusterSetError(cc, c->err, c->errstr);
		return REDIS_ERR;
	}

	if (__redisAppendCommand(c,command->cmd, command->clen) != REDIS_OK) 
	{
		__redisClusterSetError(cc, c->err, c->errstr);
		return REDIS_ERR;
	}
	
    return REDIS_OK;
}

/* Helper function for the redisClusterGetReply* family of functions.
 */

int __redisClusterGetReply(redisClusterContext *cc, int slot_num, void **reply)
{
	cluster_node *node;
	redisContext *c;
	
	if(cc == NULL || slot_num < 0)
	{
		return REDIS_ERR;
	}

	node = node_get_by_table(cc, (uint32_t)slot_num);
	if(node == NULL)
	{
		__redisClusterSetError(cc, REDIS_ERR_OTHER, "node get by table is null");
		return REDIS_ERR;
	}

	c = ctx_get_by_node(node, cc->timeout, cc->flags);
	if(c == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
		return REDIS_ERR;
	}
	else if(c->err)
	{
		__redisClusterSetError(cc, c->err, c->errstr);
		return REDIS_ERR;
	}

	if(redisGetReply(c, reply) != REDIS_OK)
	{
		__redisClusterSetError(cc, c->err, c->errstr);
		return REDIS_ERR;
	}

	return REDIS_OK;
}

static void *redis_cluster_command_execute(redisClusterContext *cc, 
	struct cmd *command)
{
	int ret;
	void *reply = NULL;
	redisReply *reply_check = NULL;
	cluster_node *node;
	redisContext *c = NULL;

moved_retry:
	
	node = node_get_by_table(cc, (uint32_t)command->slot_num);
	if(node == NULL)
	{
		__redisClusterSetError(cc, REDIS_ERR_OTHER, "node get by slot error");
		return NULL;
	}

	c = ctx_get_by_node(node, cc->timeout, cc->flags);
	if(c == NULL)
	{
		__redisClusterSetError(cc, REDIS_ERR_OTHER, "ctx get by node is null");
		return NULL;
	}
	else if(c->err)
	{
		node = node_get_witch_connected(cc);
		if(node == NULL)
		{
			__redisClusterSetError(cc, REDIS_ERR_OTHER, "no reachable node in cluster");
			return NULL;
		}

		cc->retry_count ++;
		if(cc->retry_count > cc->max_redirect_count)
		{
			__redisClusterSetError(cc, REDIS_ERR_CLUSTER_TOO_MANY_REDIRECT, 
				"too many cluster redirect");
			return NULL;
		}

		c = ctx_get_by_node(node, cc->timeout, cc->flags);
		if(c == NULL || c->err)
		{
			__redisClusterSetError(cc, c->err, c->errstr);
			return NULL;
		}
	}

ask_retry:

	if (__redisAppendCommand(c,command->cmd, command->clen) != REDIS_OK) 
	{
		__redisClusterSetError(cc, c->err, c->errstr);
		return NULL;
	}
	
	reply = __redisBlockForReply(c);
	if(reply == NULL)
	{
		__redisClusterSetError(cc, c->err, c->errstr);
		return NULL;
	}

	reply_check = reply;
	if(reply_check->type == REDIS_REPLY_ERROR)
	{
		if((int)strlen(REDIS_ERROR_MOVED) < reply_check->len && 
			strncmp(reply_check->str, REDIS_ERROR_MOVED, strlen(REDIS_ERROR_MOVED)) == 0)
		{
			freeReplyObject(reply);
			reply = NULL;
			ret = cluster_update_route(cc);
			if(ret != REDIS_OK)
			{
				__redisClusterSetError(cc, REDIS_ERR_OTHER, 
					"route update error, please recreate redisClusterContext!");
				return NULL;
			}

			cc->retry_count ++;
			if(cc->retry_count > cc->max_redirect_count)
			{
				__redisClusterSetError(cc, REDIS_ERR_CLUSTER_TOO_MANY_REDIRECT, 
					"too many cluster redirect");
				return NULL;
			}
			
			goto moved_retry;
		}
		else if((int)strlen(REDIS_ERROR_ASK) < reply_check->len && 
			strncmp(reply_check->str, REDIS_ERROR_ASK, strlen(REDIS_ERROR_ASK)) == 0)
		{
			sds *part = NULL, *ip_port = NULL;
			int part_len = 0, ip_port_len;
			dictEntry *de;
			
			part = sdssplitlen(reply_check->str, reply_check->len, 
				" ", 1, &part_len);

			if(part != NULL && part_len == 3)
			{
				ip_port = sdssplitlen(part[2], sdslen(part[2]), 
					":", 1, &ip_port_len);

				if(ip_port != NULL && ip_port_len == 2)
				{
					de = dictFind(cc->nodes, part[2]);
					if(de == NULL)
					{
						node = hi_alloc(sizeof(cluster_node));
						if(node == NULL)
						{
							__redisClusterSetError(cc, REDIS_ERR_OTHER, 
								"alloc cluster node error!");
							sdsfreesplitres(part, part_len);
							part = NULL;
							sdsfreesplitres(ip_port, ip_port_len);
							ip_port = NULL;
							
							return NULL;
						}

						cluster_node_init(node);
						node->addr = part[1];
						node->host = ip_port[0];
						node->port = hi_atoi(ip_port[1], sdslen(ip_port[1]));
						node->master = 1;

						dictAdd(cc->nodes, sdsnewlen(node->addr, sdslen(node->addr)), node);
						
						part = NULL;
						ip_port = NULL;
					}
					else
					{
						node = de->val;

						sdsfreesplitres(part, part_len);
						part = NULL;
						sdsfreesplitres(ip_port, ip_port_len);
						ip_port = NULL;
					}

					c = ctx_get_by_node(node, cc->timeout, cc->flags);
					if(c == NULL)
					{
						__redisClusterSetError(cc, REDIS_ERR_OTHER, "ctx get by node error");
						return NULL;
					}

				}

				if(part != NULL)
				{
					sdsfreesplitres(part, part_len);
					part = NULL;
				}

				if(ip_port != NULL)
				{
					sdsfreesplitres(ip_port, ip_port_len);
					ip_port = NULL;
				}
			}

			freeReplyObject(reply);
			reply = NULL;

			reply = redisCommand(c, "asking");

			freeReplyObject(reply);
			reply = NULL;

			cc->retry_count ++;
			if(cc->retry_count > cc->max_redirect_count)
			{
				__redisClusterSetError(cc, REDIS_ERR_CLUSTER_TOO_MANY_REDIRECT, 
					"too many cluster redirect");
				return NULL;
			}
			
			goto ask_retry;
		}	
	}


	return reply;
}

static int command_pre_fragment(redisClusterContext *cc, 
	struct cmd *command, list *commands)
{
	
	struct keypos *kp, *sub_kp;
	uint32_t key_count;
	uint32_t i, j;
	uint32_t idx;
	uint32_t key_len;
	int slot_num = -1;
	struct cmd *sub_command;
	struct cmd **sub_commands = NULL;
	char num_str[12];
	uint8_t num_str_len;
	

	if(command == NULL || commands == NULL)
	{
		goto done;
	}

	key_count = array_n(command->keys);

	sub_commands = hi_zalloc(REDIS_CLUSTER_SLOTS * sizeof(*sub_commands));
    if (sub_commands == NULL) 
	{
		__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
		goto done;
    }

	command->frag_seq = hi_alloc(key_count * sizeof(*command->frag_seq));
	if(command->frag_seq == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
		goto done;
	}
	
	
	for(i = 0; i < key_count; i ++)
	{
		kp = array_get(command->keys, i);

		slot_num = keyHashSlot(kp->start, kp->end - kp->start);

		if(slot_num < 0 || slot_num >= REDIS_CLUSTER_SLOTS)
		{
			__redisClusterSetError(cc,REDIS_ERR_OTHER,"keyHashSlot return error");
			goto done;
		}

		if (sub_commands[slot_num] == NULL) {
            sub_commands[slot_num] = command_get();
            if (sub_commands[slot_num] == NULL) {
                __redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
				slot_num = -1;
				goto done;
            }
        }

		command->frag_seq[i] = sub_command = sub_commands[slot_num];

		sub_command->narg++;

		sub_kp = array_push(sub_command->keys);
        if (sub_kp == NULL) {
            __redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
			slot_num = -1;
			goto done;
        }
		
        sub_kp->start = kp->start;
        sub_kp->end = kp->end;

		key_len = (uint32_t)(kp->end - kp->start);

		sub_command->clen += key_len + uint_len(key_len);

		sub_command->slot_num = slot_num;

		if (command->type == CMD_REQ_REDIS_MSET) {
			uint32_t len = 0;
			char *p;

			for (p = sub_kp->end + 1; !isdigit(*p); p++){}
			
			p = sub_kp->end + 1;
			while(!isdigit(*p))
			{
				p ++;
			}

			for (; isdigit(*p); p++) {				
            	len = len * 10 + (uint32_t)(*p - '0');
        	}
			
			len += CRLF_LEN * 2;
			len += (p - sub_kp->end);
			sub_kp->remain_len = len;
			sub_command->clen += len;
		}
	}

	for (i = 0; i < REDIS_CLUSTER_SLOTS; i++) {     /* prepend command header */
        sub_command = sub_commands[i];
        if (sub_command == NULL) {
            continue;
        }

		idx = 0;			
        if (command->type == CMD_REQ_REDIS_MGET) {
            //"*%d\r\n$4\r\nmget\r\n"
            
			sub_command->clen += 5*sub_command->narg;

			sub_command->narg ++;

			hi_itoa(num_str, sub_command->narg);
			num_str_len = (uint8_t)(strlen(num_str));

			sub_command->clen += 13 + num_str_len;

			sub_command->cmd = hi_zalloc(sub_command->clen * sizeof(*sub_command->cmd));
			if(sub_command->cmd == NULL)
			{
				__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
				slot_num = -1;
				goto done;
			}

			sub_command->cmd[idx++] = '*';
			memcpy(sub_command->cmd + idx, num_str, num_str_len);
			idx += num_str_len;
			memcpy(sub_command->cmd + idx, "\r\n$4\r\nmget\r\n", 12);
			idx += 12;
			
			for(j = 0; j < array_n(sub_command->keys); j ++)
			{
				kp = array_get(sub_command->keys, j);
				key_len = (uint32_t)(kp->end - kp->start);
				hi_itoa(num_str, key_len);
				num_str_len = strlen(num_str);

				sub_command->cmd[idx++] = '$';
				memcpy(sub_command->cmd + idx, num_str, num_str_len);
				idx += num_str_len;
				memcpy(sub_command->cmd + idx, CRLF, CRLF_LEN);
				idx += CRLF_LEN;
				memcpy(sub_command->cmd + idx, kp->start, key_len);
				idx += key_len;
				memcpy(sub_command->cmd + idx, CRLF, CRLF_LEN);
				idx += CRLF_LEN;
			}
		} else if (command->type == CMD_REQ_REDIS_DEL) {
            //"*%d\r\n$3\r\ndel\r\n"
            
			sub_command->clen += 5*sub_command->narg;

			sub_command->narg ++;

			hi_itoa(num_str, sub_command->narg);
			num_str_len = (uint8_t)strlen(num_str);
			
			sub_command->clen += 12 + num_str_len;

			sub_command->cmd = hi_zalloc(sub_command->clen * sizeof(*sub_command->cmd));
			if(sub_command->cmd == NULL)
			{
				__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
				slot_num = -1;
				goto done;
			}

			sub_command->cmd[idx++] = '*';
			memcpy(sub_command->cmd + idx, num_str, num_str_len);
			idx += num_str_len;
			memcpy(sub_command->cmd + idx, "\r\n$3\r\ndel\r\n", 11);
			idx += 11;

			for(j = 0; j < array_n(sub_command->keys); j ++)
			{
				kp = array_get(sub_command->keys, j);
				key_len = (uint32_t)(kp->end - kp->start);
				hi_itoa(num_str, key_len);
				num_str_len = strlen(num_str);

				sub_command->cmd[idx++] = '$';
				memcpy(sub_command->cmd + idx, num_str, num_str_len);
				idx += num_str_len;
				memcpy(sub_command->cmd + idx, CRLF, CRLF_LEN);
				idx += CRLF_LEN;
				memcpy(sub_command->cmd + idx, kp->start, key_len);
				idx += key_len;
				memcpy(sub_command->cmd + idx, CRLF, CRLF_LEN);
				idx += CRLF_LEN;
			}
		} else if (command->type == CMD_REQ_REDIS_MSET) {
            //"*%d\r\n$4\r\nmset\r\n"
            
			sub_command->clen += 3*sub_command->narg;

			sub_command->narg *= 2;

			sub_command->narg ++;

			hi_itoa(num_str, sub_command->narg);
			num_str_len = (uint8_t)strlen(num_str);
		
			sub_command->clen += 13 + num_str_len;

			sub_command->cmd = hi_zalloc(sub_command->clen * sizeof(*sub_command->cmd));
			if(sub_command->cmd == NULL)
			{
				__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
				slot_num = -1;
				goto done;
			}

			sub_command->cmd[idx++] = '*';
			memcpy(sub_command->cmd + idx, num_str, num_str_len);
			idx += num_str_len;
			memcpy(sub_command->cmd + idx, "\r\n$4\r\nmset\r\n", 12);
			idx += 12;
			
			for(j = 0; j < array_n(sub_command->keys); j ++)
			{
				kp = array_get(sub_command->keys, j);
				key_len = (uint32_t)(kp->end - kp->start);
				hi_itoa(num_str, key_len);
				num_str_len = strlen(num_str);

				sub_command->cmd[idx++] = '$';
				memcpy(sub_command->cmd + idx, num_str, num_str_len);
				idx += num_str_len;
				memcpy(sub_command->cmd + idx, CRLF, CRLF_LEN);
				idx += CRLF_LEN;
				memcpy(sub_command->cmd + idx, kp->start, key_len + kp->remain_len);
				idx += key_len + kp->remain_len;
				
			}
		} else {
            NOT_REACHED();
        }

		//printf("len : %d\n", sub_command->clen);
		//print_string_with_length_fix_CRLF(sub_command->cmd, sub_command->clen);
		
        sub_command->type = command->type;

		listAddNodeTail(commands, sub_command);
    }

done:

	if(sub_commands != NULL)
	{
		hi_free(sub_commands);
	}

	if(slot_num >= 0 && commands != NULL 
		&& listLength(commands) == 1)
	{
		listNode *list_node = listFirst(commands);
		command_destroy(list_node->value);
		listDelNode(commands, list_node);
		if(command->frag_seq)
		{
			hi_free(command->frag_seq);
			command->frag_seq = NULL;
		}

		command->slot_num = slot_num;
	}

	return slot_num;
}

static void *command_post_fragment(redisClusterContext *cc, 
	struct cmd *command, list *commands)
{
	struct cmd *sub_command;
	listNode *list_node;
	listIter *list_iter;
	redisReply *reply, *sub_reply;
	long long count = 0;
	
	list_iter = listGetIterator(commands, AL_START_HEAD);
	while((list_node = listNext(list_iter)) != NULL)
	{
		sub_command = list_node->value;
		reply = sub_command->reply;
		if(reply == NULL)
		{
			return NULL;
		}
		else if(reply->type == REDIS_REPLY_ERROR)
		{
			return reply;
		}

		if (command->type == CMD_REQ_REDIS_MGET) {
			if(reply->type != REDIS_REPLY_ARRAY)
			{
				__redisClusterSetError(cc,REDIS_ERR_OTHER,"reply type is error(here only can be array)");
				return NULL;
			}
		}else if(command->type == CMD_REQ_REDIS_DEL){
			if(reply->type != REDIS_REPLY_INTEGER)
			{
				__redisClusterSetError(cc,REDIS_ERR_OTHER,"reply type is error(here only can be integer)");
				return NULL;
			}

			count += reply->integer;
		}else if(command->type == CMD_REQ_REDIS_MSET){
			if(reply->type != REDIS_REPLY_STATUS ||
				reply->len != 2 || strcmp(reply->str, REDIS_STATUS_OK) != 0)
			{
				__redisClusterSetError(cc,REDIS_ERR_OTHER,"reply type is error(here only can be status and ok)");
				return NULL;
			}
		}else {
			NOT_REACHED();
		}
	}

	reply = hi_calloc(1,sizeof(*reply));

    if (reply == NULL)
    {
    	__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
		return NULL;
	}

	if (command->type == CMD_REQ_REDIS_MGET) {
		int i;
		uint32_t key_count;

		reply->type = REDIS_REPLY_ARRAY;

		key_count = array_n(command->keys);

		reply->elements = key_count;
		reply->element = hi_calloc(key_count, sizeof(*reply));
		if (reply->element == NULL) {
			freeReplyObject(reply);
            __redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
			return NULL;
        }
			
		for (i = key_count - 1; i >= 0; i--) {      /* for each key */
	        sub_reply = command->frag_seq[i]->reply;        	/* get it's reply */
	        if (sub_reply == NULL) {
				freeReplyObject(reply);
	            __redisClusterSetError(cc,REDIS_ERR_OTHER,"sub reply is null");
				return NULL;
	        }

			if(sub_reply->type == REDIS_REPLY_STRING)
			{
				reply->element[i] = sub_reply;
			}
			else if(sub_reply->type == REDIS_REPLY_ARRAY)
			{
				if(sub_reply->elements == 0)
				{
					freeReplyObject(reply);
					__redisClusterSetError(cc,REDIS_ERR_OTHER,"sub reply elements error");
					return NULL;
				}
				
				reply->element[i] = sub_reply->element[sub_reply->elements - 1];
				sub_reply->elements --;
			}
	    }
	}else if(command->type == CMD_REQ_REDIS_DEL){
		reply->type = REDIS_REPLY_INTEGER;
		reply->integer = count;
	}else if(command->type == CMD_REQ_REDIS_MSET){
		reply->type = REDIS_REPLY_STATUS;
		uint32_t str_len = strlen(REDIS_STATUS_OK);
		reply->str = hi_alloc((str_len + 1) * sizeof(char*));
		if(reply->str == NULL)
		{
			freeReplyObject(reply);
			__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
			return NULL;
		}

		reply->len = str_len;
		memcpy(reply->str, REDIS_STATUS_OK, str_len);
		reply->str[str_len] = '\0';
	}else {
		NOT_REACHED();
	}

	return reply;
}

static int command_format_by_slot(redisClusterContext *cc, 
	struct cmd *command, list *commands)
{
	struct keypos *kp;
	int key_count;
	int slot_num = -1;

	if(cc == NULL || commands == NULL ||
		command == NULL || 
		command->cmd == NULL || command->clen <= 0)
	{
		goto done;
	}

	
	redis_parse_req(command);
	if(command->result != CMD_PARSE_OK)
	{
		__redisClusterSetError(cc, REDIS_ERR_PROTOCOL, "parse command error");
		goto done;
	}

	key_count = array_n(command->keys);

	if(key_count <= 0)
	{
		__redisClusterSetError(cc, REDIS_ERR_OTHER, "no keys in command(must have keys for redis cluster mode)");
		goto done;
	}
	else if(key_count == 1)
	{
		kp = array_get(command->keys, 0);
		slot_num = keyHashSlot(kp->start, kp->end - kp->start);
		command->slot_num = slot_num;

		goto done;
	}

	slot_num = command_pre_fragment(cc, command, commands);

done:
	
	return slot_num;
}


void redisClusterSetMaxRedirect(redisClusterContext *cc, int max_redirect_count)
{
	if(cc == NULL || max_redirect_count <= 0)
	{
		return;
	}

	cc->max_redirect_count = max_redirect_count;
}

void *redisClusterCommand(redisClusterContext *cc, const char *format, ...) {
	
	va_list ap;
    redisReply *reply = NULL;
	char *cmd = NULL;
	int slot_num;
	int len;
	struct cmd *command = NULL, *sub_command;
	list *commands = NULL;
	listNode *list_node;
	listIter *list_iter = NULL;
	
    va_start(ap,format);

	if(cc == NULL)
	{
		return NULL;
	}

	if(cc->err)
	{
		cc->err = 0;
		memset(cc->errstr, '\0', strlen(cc->errstr));
	}

    len = redisvFormatCommand(&cmd,format,ap);

	va_end(ap);

	if (len == -1) {
        __redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
        return NULL;
    } else if (len == -2) {
        __redisClusterSetError(cc,REDIS_ERR_OTHER,"Invalid format string");
        return NULL;
    }	
	
	command = command_get();
	if(command == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
        return NULL;
	}
	
	command->cmd = cmd;
	command->clen = len;

	commands = listCreate();
	if(commands == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
		goto error;
	}

	commands->free = listCommandFree;

	slot_num = command_format_by_slot(cc, command, commands);

	if(slot_num < 0)
	{
		goto error;
	}
	else if(slot_num >= REDIS_CLUSTER_SLOTS)
	{
		__redisClusterSetError(cc,REDIS_ERR_OTHER,"slot_num is out of range");
		goto error;
	}

	//all keys belong to one slot
	if(listLength(commands) == 0)
	{
		reply = redis_cluster_command_execute(cc, command);
		goto done;
	}

	ASSERT(listLength(commands) != 1);

	list_iter = listGetIterator(commands, AL_START_HEAD);
	while((list_node = listNext(list_iter)) != NULL)
	{
		sub_command = list_node->value;
		
		reply = redis_cluster_command_execute(cc, sub_command);
		if(reply == NULL)
		{
			goto error;
		}
		else if(reply->type == REDIS_REPLY_ERROR)
		{
			goto done;
		}

		sub_command->reply = reply;
	}

	reply = command_post_fragment(cc, command, commands);
	
done:

	command_destroy(command);

	if(commands != NULL)
	{
		listRelease(commands);
	}

	if(list_iter != NULL)
	{
		listReleaseIterator(list_iter);
	}

	cc->retry_count = 0;
	
    return reply;

error:

	if(command != NULL)
	{
		command_destroy(command);
	}
	else if(cmd != NULL)
	{
		free(cmd);
	}

	if(commands != NULL)
	{
		listRelease(commands);
	}

	if(list_iter != NULL)
	{
		listReleaseIterator(list_iter);
	}

	cc->retry_count = 0;
	
	return NULL;
}

int redisClusterAppendCommand(redisClusterContext *cc, 
	const char *format, ...) {

	va_list ap;
	int len;
	int slot_num;
	struct cmd *command = NULL, *sub_command;
	list *commands = NULL;

	char *cmd;
	listNode *list_node;
	listIter *list_iter = NULL;

	if(cc == NULL || format == NULL)
	{
		return REDIS_ERR;
	}

	if(cc->requests == NULL)
	{
		cc->requests = listCreate();
		if(cc->requests == NULL)
		{
			__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
			goto error;
		}

		cc->requests->free = listCommandFree;
	}
	
	va_start(ap,format);

	len = redisvFormatCommand(&cmd,format,ap);	
	if (len == -1) {
		__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
		goto error;
	} else if (len == -2) {
		__redisClusterSetError(cc,REDIS_ERR_OTHER,"Invalid format string");
		goto error;
	}	
	
	command = command_get();
	if(command == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
		goto error;
	}
	
	command->cmd = cmd;
	command->clen = len;

	commands = listCreate();
	if(commands == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OOM,"Out of memory");
		goto error;
	}

	commands->free = listCommandFree;

	slot_num = command_format_by_slot(cc, command, commands);

	if(slot_num < 0)
	{
		goto error;
	}
	else if(slot_num >= REDIS_CLUSTER_SLOTS)
	{
		__redisClusterSetError(cc,REDIS_ERR_OTHER,"slot_num is out of range");
		goto error;
	}

	//all keys belong to one slot
	if(listLength(commands) == 0)
	{
		if(__redisClusterAppendCommand(cc, command) == REDIS_OK)
		{
			goto done;
		}
		else
		{
			goto error;
		}
	}

	ASSERT(listLength(commands) != 1);

	list_iter = listGetIterator(commands, AL_START_HEAD);
	while((list_node = listNext(list_iter)) != NULL)
	{
		sub_command = list_node->value;
		
		if(__redisClusterAppendCommand(cc, sub_command) == REDIS_OK)
		{
			continue;
		}
		else
		{
			goto error;
		}
	}

done:

	va_end(ap);

	if(command->cmd != NULL)
	{
		free(command->cmd);
		command->cmd = NULL;
	}
	else
	{
		goto error;
	}

	if(commands != NULL && listLength(commands) > 0)
	{
		if(listLength(commands) > 0)
		{
			command->sub_commands = commands;
		}
		else
		{
			listRelease(commands);
		}
	}

	if(list_iter != NULL)
	{
		listReleaseIterator(list_iter);
	}

	listAddNodeTail(cc->requests, command);
	
	return REDIS_OK;

error:

	va_end(ap);

	if(command != NULL)
	{
		command_destroy(command);
	}
	else if(cmd != NULL)
	{
		free(cmd);
	}

	if(commands != NULL)
	{
		listRelease(commands);
	}

	if(list_iter != NULL)
	{
		listReleaseIterator(list_iter);
	}

	/* Attention: mybe here we must pop the 
	  sub_commands that had append to the nodes.  
	  But now we do not handle it. */
	
	return REDIS_ERR;
}

int redisClusterAppendCommandArgv(redisClusterContext *cc, 
	int argc, const char **argv) {

	int j;

	for (j=0; j < argc; j++) {
        if(redisClusterAppendCommand(cc, argv[j]) != REDIS_OK)
        {
			return REDIS_ERR;
		}
    }

    return REDIS_OK;
}


int redisClusterGetReply(redisClusterContext *cc, void **reply) {

	struct cmd *command, *sub_command;
	list *commands = NULL;
	listNode *list_command, *list_sub_command;
	listIter *list_iter;
	int slot_num;
	void *sub_reply;

	if(cc == NULL || cc->requests == NULL)
	{
		return REDIS_ERR;
	}

	list_command = listFirst(cc->requests);
	if(list_command == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OTHER,
			"no more reply");
		return REDIS_ERR;
	}
	
	command = list_command->value;
	if(command == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OTHER,
			"command in the requests list is null");
		goto error;
	}

	slot_num = command->slot_num;
	if(slot_num >= 0)
	{
		listDelNode(cc->requests, list_command);
		return __redisClusterGetReply(cc, slot_num, reply);
	}

	commands = command->sub_commands;
	if(commands == NULL)
	{
		__redisClusterSetError(cc,REDIS_ERR_OTHER,
			"sub_commands in command is null");
		goto error;
	}

	ASSERT(listLength(commands) != 1);

	list_iter = listGetIterator(commands, AL_START_HEAD);
	while((list_sub_command = listNext(list_iter)) != NULL)
	{
		sub_command = list_sub_command->value;
		if(sub_command == NULL)
		{
			__redisClusterSetError(cc,REDIS_ERR_OTHER,
				"sub_command is null");
			goto error;
		}
		
		slot_num = sub_command->slot_num;
		if(slot_num < 0)
		{
			__redisClusterSetError(cc,REDIS_ERR_OTHER,
				"sub_command slot_num is less then zero");
			goto error;
		}
		
		if(__redisClusterGetReply(cc, slot_num, &sub_reply) != REDIS_OK)
		{
			goto error;
		}

		sub_command->reply = sub_reply;
	}

	
	
	*reply = command_post_fragment(cc, command, commands);
	if(*reply == NULL)
	{
		goto error;
	}

	listDelNode(cc->requests, list_command);
	return REDIS_OK;

error:

	listDelNode(cc->requests, list_command);
	return REDIS_ERR;
}

void redisCLusterReset(redisClusterContext *cc)
{
	dictIterator *di;
	dictEntry *de;
	struct cluster_node *node;
	redisContext *c = NULL;
	
	if(cc == NULL || cc->nodes == NULL)
	{
		return;
	}

	di = dictGetIterator(cc->nodes);
	while((de = dictNext(di)) != NULL)
	{
		node = dictGetEntryVal(de);
		if(node == NULL)
		{
			continue;
		}
		
		c = ctx_get_by_node(node, cc->timeout, cc->flags);
		if(c == NULL)
		{
			continue;
		}

		sdsfree(c->obuf);
    	redisReaderFree(c->reader);

    	c->obuf = sdsempty();
    	c->reader = redisReaderCreate();
	}

	dictReleaseIterator(di);

	if(cc->requests)
	{
		listRelease(cc->requests);
		cc->requests = NULL;
	}
}

/*############redis cluster async############*/

/* We want the error field to be accessible directly instead of requiring
 * an indirection to the redisContext struct. */
static void __redisClusterAsyncCopyError(redisClusterAsyncContext *acc) {
    if (!acc)
        return;

    redisClusterContext *cc = acc->cc;
    acc->err = cc->err;
	memcpy(acc->errstr, cc->errstr, 128);
}

static void __redisClusterAsyncSetError(redisClusterAsyncContext *acc, 
	int type, const char *str) {
	
    size_t len;

    acc->err = type;
    if (str != NULL) {
        len = strlen(str);
        len = len < (sizeof(acc->errstr)-1) ? len : (sizeof(acc->errstr)-1);
        memcpy(acc->errstr,str,len);
        acc->errstr[len] = '\0';
    } else {
        /* Only REDIS_ERR_IO may lack a description! */
        assert(type == REDIS_ERR_IO);
        __redis_strerror_r(errno, acc->errstr, sizeof(acc->errstr));
    }
}

static redisClusterAsyncContext *redisClusterAsyncInitialize(redisClusterContext *cc) {
    redisClusterAsyncContext *acc;

	if(cc == NULL)
	{
		return NULL;
	}

    acc = hi_alloc(sizeof(redisClusterAsyncContext));
    if (acc == NULL)
        return NULL;

	acc->cc = cc;

    acc->err = 0;
    acc->data = NULL;
	acc->adapter = NULL;
	acc->attach_fn = NULL;

    acc->onConnect = NULL;
    acc->onDisconnect = NULL;

    return acc;
}

static redisAsyncContext * actx_get_by_node(redisClusterAsyncContext *acc, 
	cluster_node *node)
{
	redisAsyncContext *ac;
	
	if(node == NULL)
	{
		return NULL;
	}

	ac = node->acon;
	if(ac != NULL)
	{
		if(ac->c.err)
		{
			redisReconnect(&ac->c);
		}

		return ac;
	}

	if(node->host == NULL || node->port <= 0)
	{
		__redisClusterAsyncSetError(acc, REDIS_ERR_OTHER, "node host or port is error");
		return NULL;
	}

	ac = redisAsyncConnect(node->host, node->port);
	if(ac == NULL)
	{
		__redisClusterAsyncSetError(acc, REDIS_ERR_OTHER, "node host or port is error");
		return NULL;
	}

	if(acc->adapter)
	{
		acc->attach_fn(ac, acc->adapter);
	}

	if(acc->onConnect)
	{
    	redisAsyncSetConnectCallback(ac, acc->onConnect);
	}

	if(acc->onDisconnect)
	{
		redisAsyncSetDisconnectCallback(ac, acc->onDisconnect);
	}
	
	node->acon = ac;

	return ac;
}

redisClusterAsyncContext *redisClusterAsyncConnect(const char *addrs) {

    redisClusterContext *cc;
    redisClusterAsyncContext *acc;

	cc = redisClusterConnectNonBlock(addrs);
	if(cc == NULL)
	{
		return NULL;
	}

	acc = redisClusterAsyncInitialize(cc);
    if (acc == NULL) {
        redisClusterFree(cc);
        return NULL;
    }
	
	__redisClusterAsyncCopyError(acc);
	
    return acc;
}


int redisClusterAsyncSetConnectCallback(redisClusterAsyncContext *acc, redisConnectCallback *fn) {
    if (acc->onConnect == NULL) {
        acc->onConnect = fn;
        return REDIS_OK;
    }
    return REDIS_ERR;
}

int redisClusterAsyncSetDisconnectCallback(redisClusterAsyncContext *acc, redisDisconnectCallback *fn) {
    if (acc->onDisconnect == NULL) {
        acc->onDisconnect = fn;
        return REDIS_OK;
    }
    return REDIS_ERR;
}

int redisClusterAsyncCommand(redisClusterAsyncContext *acc, 
	redisCallbackFn *fn, void *privdata, const char *format, ...) {

	va_list ap;
	redisClusterContext *cc;
	int status = REDIS_OK;
	char *cmd = NULL;
	int len;
	int slot_num;
	cluster_node *node;
	redisAsyncContext *ac;
	
    va_start(ap,format);

	if(acc == NULL)
	{
		return REDIS_ERR;
	}

	cc = acc->cc;

	len = redisvFormatCommand(&cmd,format,ap);

	if (len == -1) {
        __redisClusterAsyncSetError(acc,REDIS_ERR_OOM,"Out of memory");
        return REDIS_ERR;
    } else if (len == -2) {
        __redisClusterAsyncSetError(acc,REDIS_ERR_OTHER,"Invalid format string");
        return REDIS_ERR;
    }

	slot_num = slot_get_by_command(cc, cmd, len);

	if(slot_num < 0)
	{
		status = REDIS_ERR;
		goto done;
	}
	else if(slot_num >= REDIS_CLUSTER_SLOTS)
	{
		__redisClusterAsyncSetError(acc,REDIS_ERR_OTHER,"slot_num is out of range");
		status = REDIS_ERR;
		goto done;
	}

	node = node_get_by_table(cc, (uint32_t) slot_num);
	if(node == NULL)
	{
		__redisClusterAsyncSetError(acc, REDIS_ERR_OTHER, "node get by slot error");
		status = REDIS_ERR;
		goto done;
	}
	
	ac = actx_get_by_node(acc, node);
	if(ac == NULL)
	{
		__redisClusterAsyncSetError(acc, REDIS_ERR_OTHER, "actx get by slot error");
		status = REDIS_ERR;
		goto done;
	}
	else if(ac->err)
	{
		__redisClusterAsyncSetError(acc, ac->err, ac->errstr);
		status = REDIS_ERR;
		goto done;
	}

	va_end(ap);
	va_start(ap,format);
    status = redisvAsyncCommand(ac,fn,privdata,format,ap);

done:

	va_end(ap);

	if(cmd != NULL)
	{
		free(cmd);
	}
	
    return status;
}

void redisClusterAsyncDisconnect(redisClusterAsyncContext *acc) {

	redisClusterContext *cc;
	redisAsyncContext *ac;
	dictIterator *di;
	dictEntry *de;
	dict *nodes;
	struct cluster_node *node;

	if(acc == NULL)
	{
		return;
	}

	cc = acc->cc;

	nodes = cc->nodes;

	if(nodes == NULL)
	{
		return;
	}
	
	di = dictGetIterator(nodes);

	while((de = dictNext(di)) != NULL) 
	{
    	node = dictGetEntryVal(de);

		ac = node->acon;

		if(ac == NULL)
		{
			continue;
		}

		redisAsyncDisconnect(ac);

		node->acon = NULL;
	}
}

void redisClusterAsyncFree(redisClusterAsyncContext *acc)
{
	redisClusterContext *cc;
	
	if(acc == NULL)
	{
		return;
	}

	cc = acc->cc;

	redisClusterFree(cc);

	hi_free(acc);
}
