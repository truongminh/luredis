/**
 ** LUREDIS: a redis client for Lua.
 ** (C) 2013 by Nguyen Truong Minh at minhnt@live
 **
 **/

#include "luredis.h"
#include "hiredis/sds.h"
#include "hiredis/hiredis.h"
#include "hiredis/async.h"
#include "hiredis/dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>


/* Lua-JIT provides better faster Lua-Vitural-Machine (LVM) */

#ifndef LUAJIT_2_0
#define LUAJIT_2_0
#include <luajit-2.0/lua.h>
#endif


#include <stdlib.h>

typedef redisContext* redisContextPtr;
typedef redisAsyncContext* redisAsyncContextPtr;
// Disconnect:
// a/ Fail to connect after trying 8 consecutive times
// b/ Once disconnected, current time not exceed a target time in the future
typedef struct _luredisConnection {
    char* hostport;
    redisContext *pContext;
    unsigned int okCommands;
    double avgCommandTime;
    double totalTime;
    unsigned char failedCommands;
    // Variables used in control connection
    time_t startTime;
    time_t disableTime;
    time_t reconnectTime;
    unsigned int remainConnectFailTimes;
} luredisConnection;


void dictLuredisKeyDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    free(val);
}

void dictLuredisConnectionDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    luredisConnection *pConn = (luredisConnection*)val;
    if(pConn) {
        free(pConn->hostport);
        if(pConn->pContext)  redisFree(pConn->pContext);
        free(pConn);
    }
}

int dictLuredisKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);
    register char *s1 = (char*)key1;
    register char *s2 = (char*)key2;
    while (*s1 && (*s1++ == *s2++)) {
            ;
        }
    return *s1 == *s2;  /* == '\0' */
}

unsigned int dictLuredisKeyHash(const void *key) {
    register unsigned int hash = 5381;
    register char* ptr = (char*)key;
    while (*ptr)
        hash = ((hash << 5) + hash) + (*ptr++); /* hash * 33 + c */
    return hash;
}

/* Command table. sds string -> command struct pointer. */
dictType mapRedisConnectionDictType = {
    dictLuredisKeyHash,           /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictLuredisKeyCompare,     /* key compare */
    dictLuredisKeyDestructor,         /* key destructor */
    dictLuredisConnectionDestructor /* val destructor */
};

typedef struct {
    dict *connections;
    const char * argv[LUREDIS_MAXARGS+1]; /* in case we use an additional buffer */
    size_t argvlen[LUREDIS_MAXARGS+1];
    char buffer[LUREDIS_MAX_BUF_LEN];
    size_t buf_len;
    struct timeval timeout;
    unsigned int maxConnectFailTimes;
} redisWorker;


/******************************************************************************/
/*Set the worker into the talbe LIB_LUREDIS with the key LUREDIS_REDIS_WORKER*/
/******************************************************************************/
static inline void luredis_set_redisWorker(lua_State *L,
                                           redisWorker* worker) {
    lua_getglobal(L,LIB_LUREDIS); // push table t on stack
    lua_pushlightuserdata(L,worker); // push value v on stack
    // stack[-2] = t; stack[-1] = v
    // so t[key] = req
    lua_setfield(L,-2,LUREDIS_REDIS_WORKER);
    lua_pop(L,1); // pop table
}
/******************************************************************************/
/*get the worker pointer from the table LIB_LUREDIS with the key: LUREDIS_REDIS_WORKWER*/
/******************************************************************************/
static inline redisWorker * luredis_get_redisWorker(lua_State* L){
    redisWorker *worker;
    lua_getglobal(L,LIB_LUREDIS); // push table t on stack
    lua_getfield(L,-1,LUREDIS_REDIS_WORKER);
    worker = (redisWorker*)lua_touserdata(L,-1); // value is on top
    lua_pop(L,2); // pop table and value
    return worker;
}


static inline luredisConnection *luredis_get_luredis_connection(lua_State* L,
                                                                const char* hostport){
    redisWorker *worker = luredis_get_redisWorker(L);
    return worker? (luredisConnection*)dictFetchValue(worker->connections,hostport):NULL;
}


/******************************************************************************/
/* Private Functions prototypes                                               */
/******************************************************************************/
static void moveReplyToLua(lua_State *L, redisReply *reply);
static int load_args(lua_State * L,int idx,const char ** argv,size_t * argvlen);
static int processReply(lua_State *L, redisReply *pReply, luredisConnection *pConn);
static luredisConnection *_getLuredisConnection(const char *hostport, redisWorker *worker);
#ifdef LUREDIS_ENABLE_ASYNC_REDIS_CONTEXT
/* async redis context has not supported */
    static redisAsyncContextPtr getAsyncRedisContext(std::string &hostport, redisWorker *worker);
#endif
static void _luredis_create_table_state(lua_State* L, luredisConnection *pConn);
static luredisConnection* _allocateLuredisConnection(dict *conDict, const char *key);
static void _resetLuredisConnectionState(luredisConnection *pConn);
static void _luredis_log(const char *fn, const char *fmt, ...);

/******************************************************************************
* LUA Public API
 * Call from LUA:
 *   region,rtype = luredis.send(redisConn.hostport,"hget",redisConn.queue,ipnum);
 *
******************************************************************************/

static int luredis_send (lua_State* L) {
    int ret = 0;
    if(lua_isstring(L,1)) {
        // Don't free hostport as they will be re-used by the dict
        const char* hostport = luaL_checkstring(L,1);
        redisWorker *worker = luredis_get_redisWorker(L);
        luredisConnection *pConn = worker? _getLuredisConnection(hostport,worker):NULL;
        // if connection has been found or already created
        if(pConn) {
            if(pConn->pContext) {
                int nargs = load_args(L,2,worker->argv,worker->argvlen);
                // If it is unable to load args, the entire operation must be aborted
                if (nargs >= 0) {
                    clock_t start = clock();
                    redisReply *pReply = (redisReply *)redisCommandArgv(
                                pConn->pContext,
                                nargs,
                                worker->argv,
                                worker->argvlen);
                    pConn->totalTime += (double)(clock() - start) / CLOCKS_PER_SEC;
                    ret = processReply(L,pReply,pConn);
                }
            }
            else pConn->failedCommands++;
        }
    }
    return ret;
}

//==================================================================================================
//From Lua: sendbuf(target_host,"lpush",target_queue)
//Process:
//  1. Get redisWorker*: which contains a dictionary of connection
//  2. Get the connectionn pointer from the worker, using the key is host::post
//==================================================================================================
static int luredis_send_buf (lua_State* L) {
    int ret = 0;
    if(lua_isstring(L,1)) {        
        const char* hostport = luaL_checkstring(L,1);
        redisWorker *worker = luredis_get_redisWorker(L);
        luredisConnection *pConn = worker? _getLuredisConnection(hostport,worker):NULL;
        // if connection has been found or already created
        if(pConn) {
            if(pConn->pContext) {
                const char **argv = worker->argv;
                size_t *argvlen = worker->argvlen;
                int nargs = load_args(L,2,argv,argvlen);
                /*
                 There is no need to free argv[i] as argv[i] points to a string owned by Lua
                 There is no need to free argv, argvlen, or worker->buffer as we will reuse them latter
                */
                // If it is unable to load args, the entire operation must be aborted
                if (nargs >= 0) {
                    argv[nargs] = worker->buffer;
                    argvlen[nargs] = worker->buf_len;
                    nargs++;
                    clock_t start = clock();
                    redisReply * pReply = (redisReply *)redisCommandArgv(
                                pConn->pContext,
                                nargs,
                                argv,
                                argvlen);
                    pConn->totalTime += (double)(clock() - start) / CLOCKS_PER_SEC;
                    ret = processReply(L,pReply,pConn);
                }
            }
            else pConn->failedCommands++;
        }
    }
    return ret;
}

static int luredis_send_only_buf (lua_State* L) {
    int ret = 0;
    if(lua_isstring(L,1)) {
        // Don't free hostport as they will be re-used by the dict
        const char* hostport = luaL_checkstring(L,1);
        redisWorker *worker = luredis_get_redisWorker(L);
        luredisConnection *pConn = worker? _getLuredisConnection(hostport,worker):NULL;
        // if connection has been found or already created
        if(pConn) {
            if(pConn->pContext&&worker->buf_len>0) {
                clock_t start = clock();
                worker->buffer[worker->buf_len] = '\0';
                redisReply * pReply = (redisReply *)redisCommand(
                            pConn->pContext,
                            worker->buffer);
                pConn->totalTime += (double)(clock() - start) / CLOCKS_PER_SEC;
                ret = processReply(L,pReply,pConn);
            }
            else pConn->failedCommands++;
        }
    }
    return ret;
}

static int luredis_get_connection_state(lua_State* L) {
    if(lua_isstring(L,1)) {
        const char* hostport = luaL_checkstring(L,1);
        luredisConnection *pConn = luredis_get_luredis_connection(L,hostport);
        if(pConn){
            _luredis_create_table_state(L,pConn);
        }
    }
    else lua_newtable(L);
    return 1;
}

static int luredis_get_worker_state(lua_State* L) {
    redisWorker *worker = luredis_get_redisWorker(L);
    if(worker&&worker->connections) {
        lua_createtable(L, 4, 0); // preallocate 4 table entries
        dictIterator *di = dictGetIterator(worker->connections);
        dictEntry *de;
        while((de = dictNext(di)) != NULL) {
            char *hostport = (char*)dictGetEntryKey(de);
            luredisConnection *pConn = (luredisConnection*)dictGetEntryVal(de);
            if(pConn&&hostport) {
                _luredis_create_table_state(L,pConn);
                // table name
                lua_setfield(L,-2,hostport);
            }
        }
        dictReleaseIterator(di);
    }
    else lua_newtable(L);
    return 1;
}
static int luredis_reset_connection_state(lua_State* L) {
    if(lua_isstring(L,1)) {
        const char* hostport = luaL_checkstring(L,1);
        luredisConnection *pConn = luredis_get_luredis_connection(L,hostport);
        if(pConn) _resetLuredisConnectionState(pConn);
    }
    else lua_newtable(L);
    return 0;
}

static int luredis_reset_worker_state(lua_State* L) {
    redisWorker *worker = luredis_get_redisWorker(L);
    if(worker&&worker->connections) {
        dictIterator *di = dictGetIterator(worker->connections);
        dictEntry *de;
        while((de = dictNext(di)) != NULL) {
            luredisConnection *pConn = (luredisConnection*)dictGetEntryVal(de);
            if(pConn) _resetLuredisConnectionState(pConn);
        }
        dictReleaseIterator(di);
    }
    return 0;
}
static int luredis_setRedisTimeout(lua_State* L) {
    if(lua_isstring(L,1)&&lua_isnumber(L,2)) {
        const char* hostport = luaL_checkstring(L,1);
        luredisConnection *pConn = luredis_get_luredis_connection(L,hostport);
        if(pConn){
            struct timeval timeout;
            timeout.tv_sec = luaL_checknumber(L, 2);
            timeout.tv_usec = luaL_optint(L, 3, 0);
            redisSetTimeout(pConn->pContext,timeout);
        }
    }
    return 0;
}

static int luredis_setGlobalRedisTimeout(lua_State* L) {
    redisWorker *worker = luredis_get_redisWorker(L);
    if(worker) {
        struct timeval timeout;
        timeout.tv_sec = luaL_optint(L, 1, DEFAULT_REDIS_SEND_RECV_TIMEOUT);
        timeout.tv_usec = luaL_optint(L, 2, 0);
        worker->timeout = timeout;
    }
    return 0;
}

static int luredis_close(lua_State* L) {
    if(lua_isstring(L,1)) {
        const char* hostport = luaL_checkstring(L,1);
        redisWorker *worker = luredis_get_redisWorker(L);
        // free dict entry including: free key, free redis context, free hostport
        if(worker) dictDelete(worker->connections,hostport);
    }
    return 0;
}

static int luredis_close_all(lua_State* L) {
    redisWorker *worker = luredis_get_redisWorker(L);
    if(worker&&worker->connections){
        dictRelease(worker->connections);
        worker->connections = dictCreate(&mapRedisConnectionDictType,NULL);
        dictExpand(worker->connections,LUREDIS_MAP_CONNECTION_DICT_SIZE);
    }
    return 0;
}


static int luredis_clear_buf(lua_State* L) {
    redisWorker *worker = luredis_get_redisWorker(L);
    if(worker) worker->buf_len = 0;
    return 0;
}

// Return number of appended strings
static int luredis_append_buf(lua_State* L) {
    redisWorker *worker = luredis_get_redisWorker(L);
    size_t pushed = 0;
    if(worker) {
        size_t buf_len = worker->buf_len;
        char *buf = worker->buffer+buf_len; // secured because buffer is a constant char pointer
        size_t len;
        const char *ptr;
        int argc = lua_gettop(L);
        size_t num = 1;
        while(argc--) { /* # if argc = 1, (argc--) true but (--argc) false */
            if (lua_isstring(L, num)) {
                ptr = luaL_checklstring(L, num,&len);
                if (buf_len+len>LUREDIS_MAX_BUF_LEN) break;
                memcpy(buf,ptr,len);
                buf_len += len;
                buf += len;
                pushed++;
            }
            num++;
        }
        worker->buf_len = buf_len;
        //printf("buf %s\n",worker->buffer);
    }
    lua_pushnumber(L,pushed);
    return 1;
}

static int luredis_concat_buf(lua_State* L) {
    redisWorker *worker = luredis_get_redisWorker(L);
    size_t pushed = 0;

    if(worker) {        
        size_t buf_len = worker->buf_len;
        char *buf = worker->buffer+buf_len;
        size_t len;
        const char *ptr;
        const char* seperator;
        size_t lenSeperator;
        int argc = lua_gettop(L)-1;
        size_t idx = 2;
        if (lua_isstring(L, 1)&&argc>0) {
            seperator = luaL_checklstring(L, 1,&lenSeperator);
            while(--argc) {
                if (lua_isstring(L, idx)) {
                    ptr = luaL_checklstring(L, idx,&len);
                    if (buf_len+len+lenSeperator>LUREDIS_MAX_BUF_LEN) break;
                    memcpy(buf,ptr,len);
                    buf += len;
                    buf_len += len;
                    pushed++;
                }
                memcpy(buf,seperator,lenSeperator);
                buf += lenSeperator;
                buf_len += lenSeperator;
                idx++;
            }
            if (lua_isstring(L, idx)) {
                ptr = luaL_checklstring(L, idx,&len);
                if (buf_len+len < LUREDIS_MAX_BUF_LEN) {
                    memcpy(buf,ptr,len);
                    buf += len;
                    buf_len += len;
                    idx++;
                }
            }
            worker->buf_len = buf_len;
            //printf("buf %s\n",worker->buffer);
        }
    }
    lua_pushnumber(L,pushed);    
    return 1;
}

static int luredis_get_buf(lua_State* L) {
    redisWorker *worker = luredis_get_redisWorker(L);
    if(worker) lua_pushlstring(L, worker->buffer,worker->buf_len);
    return 1;
}

static const luaL_reg luredis_methods[] = {
        { "send", luredis_send},
        { "sendbuf", luredis_send_buf},
        { "sendonlybuf", luredis_send_only_buf},   
        { "set_timeout", luredis_setRedisTimeout},
        { "set_global_timeout", luredis_setGlobalRedisTimeout},
        { "close", luredis_close},
        { "close_all", luredis_close_all},
        { "appbuf", luredis_append_buf },
        { "conbuf", luredis_concat_buf },
        { "getbuf", luredis_get_buf },
        { "clrbuf", luredis_clear_buf },
        {"reset_state",luredis_reset_connection_state},
        {"get_state",luredis_get_connection_state},
        {"get_all_state", luredis_get_worker_state},
        {"reset_all_state", luredis_reset_worker_state},
        { NULL, NULL } };

int lvm_register_luredis(lua_State *L) {
    luaL_register(L, LIB_LUREDIS, luredis_methods);
    lua_pushliteral(L, LUREDIS_VERSION);
    lua_setfield(L, -2, "_VERSION");

    lua_pushliteral(L, LUREDIS_COPYRIGHT);
    lua_setfield(L, -2, "_COPYRIGHT");

    lua_pushliteral(L, LUREDIS_DESCRIPTION);
    lua_setfield(L, -2, "_DESCRIPTION");

    // Redis reply type
    lua_pushnumber(L,REDIS_REPLY_INTEGER);
    lua_setfield(L,-2,"integer");
    lua_pushnumber(L,REDIS_REPLY_ARRAY);
    lua_setfield(L,-2,"array");
    lua_pushnumber(L,REDIS_REPLY_STRING);
    lua_setfield(L,-2,"string");
    lua_pushnumber(L,REDIS_REPLY_ERROR);
    lua_setfield(L,-2,"error");
    lua_pushnumber(L,REDIS_REPLY_STATUS);
    lua_setfield(L,-2,"status");
    lua_pushnumber(L,REDIS_REPLY_NIL);
    lua_setfield(L,-2,"rnil");

    lua_pop(L,1);
    // Redis Worker
    redisWorker *worker = (redisWorker*)malloc(sizeof(redisWorker));
    //signal(SIGPIPE, SIG_IGN); // set up aeMain
    worker->connections = dictCreate(&mapRedisConnectionDictType,NULL);
    dictExpand(worker->connections,LUREDIS_MAP_CONNECTION_DICT_SIZE); // improve hashing efficiency
    worker->buf_len = 0;
    (worker->timeout).tv_sec = DEFAULT_REDIS_SEND_RECV_TIMEOUT;
    (worker->timeout).tv_usec = 0;
    worker->maxConnectFailTimes = MAX_REDIS_CONNECT_FAIL_TIMES;
    luredis_set_redisWorker(L,worker);
    return 1;
}

void lvm_close_luredis(lua_State *L) {
    redisWorker *worker = luredis_get_redisWorker(L);
    if(worker&&worker->connections) {
        dictRelease(worker->connections);
        free(worker);
    }
    luredis_set_redisWorker(L,NULL); // prevent any further operation
}


/************************************************************************************/
/*                                                                                  */
/*  PRIVATE FUNCTIONS                                                               */
/*                                                                                  */
/************************************************************************************/

static void moveReplyToLua(lua_State *L, redisReply *reply) {
    if(reply) {
        redisReply *r = reply;
        switch (r->type) {

        case REDIS_REPLY_INTEGER:
            lua_pushnumber(L, reply->integer);
            break; /* Nothing to free */

        case REDIS_REPLY_ARRAY:
            if (r->element != NULL) {
                /* Table; check lua stack */
                // luaL_checkstack(L,sz,msg);
                lua_createtable(L,r->elements,0);
                unsigned int j;
                for (j = 0; j < r->elements; j++)
                    if (r->element[j] != NULL) {                        
                        moveReplyToLua(L, r->element[j]);
                        lua_rawseti(L,-2,j+1);
                    }
                free(r->element);
            }
            break;

        case REDIS_REPLY_STRING:            
        case REDIS_REPLY_ERROR:
            /*
            lua_Debug ar;
            lua_getstack(L, 1, &ar);
            lua_getinfo(L, "nSl", &ar);
            printf("[LUREDIS ERROR REPLY] In %s, line %d: %s \n",ar.short_src,ar.currentline,r->str);
            */
        case REDIS_REPLY_STATUS:
            if (r->str != NULL) {
                lua_pushlstring(L, r->str,r->len);                
                free(r->str);
            } else
                lua_pushnil(L);
            break;
        case REDIS_REPLY_NIL:
            lua_pushnil(L);
            break;
        default:
            lua_pushnil(L);
            break;
        }
        free(r);
    }
}

static int load_args(lua_State * L,int idx,const char ** argv,size_t * argvlen)
{    
    int nargs = lua_gettop(L) - idx + 1;
    if (nargs>LUREDIS_MAXARGS) {
        luaL_error(L,"Number of Args: %d > %d",nargs,LUREDIS_MAXARGS);
        return 0;
    }
    int rvalue = nargs;    
    // argv and argvlen is secured
    while(nargs--)
        if (lua_isstring(L,idx)) *(argv++) = lua_tolstring(L, idx++, argvlen++);
    return rvalue;
}

static int processReply(lua_State *L, redisReply *pReply, luredisConnection *pConn) {
    if(pReply) {
        int reply_type = pReply->type;
        moveReplyToLua(L,pReply); // already free Reply
        lua_pushnumber(L,reply_type);
        pConn->okCommands++; // pConn is secured
        return 2;
    }
    else {
        // connection die
        redisFree(pConn->pContext); // pContext is secured
        pConn->pContext = NULL;
        pConn->failedCommands++;
        _luredis_log(LUREDIS_LOG_FILE ,"[REDIS %s] Disconnected from server.",pConn->hostport);
        return 0;
    }
}
//============================================================================================================
// Search the dictionary in the worker for the connectino with the key = hostpost
// if not found ==> creat a new connection
//============================================================================================================
static luredisConnection* _getLuredisConnection(const char* hostport, redisWorker *worker)
{    
    luredisConnection *pConn = NULL;
    if(worker&&worker->connections) {
        pConn = (luredisConnection*)dictFetchValue(worker->connections,hostport);
        if(!pConn) {
            // It is required that the key belong to the dict,
            // so we create a new key here by calling strdup.
            pConn = _allocateLuredisConnection(worker->connections,hostport);
            if(!pConn) return NULL;
        }
        // Create new redis context
        // Remain Connect Fail Times > 0
        // Current Time > Start Time
        if(!pConn->pContext){
            if(pConn->remainConnectFailTimes||(time(NULL)>pConn->reconnectTime)) {
                char *host = strdup(hostport);
                char *port = host+8;
                while(*port&&*port++!=':');
                if(!*port){
                    _luredis_log(LUREDIS_LOG_FILE, "[REDIS %s] not conform to pattern <host:port>",hostport);
                    return pConn;
                }
                *(port-1) = '\0';
                redisContextPtr pContext = redisConnectWithTimeout(host, atoi(port),worker->timeout);
                free(host);
                if (pContext->err) {
                    _luredis_log(LUREDIS_LOG_FILE, "[REDIS %s] %s",hostport,pContext->errstr);
                    redisFree(pContext);
                    pConn->pContext = NULL;
                    if(pConn->remainConnectFailTimes==0){
                        pConn->reconnectTime = time(NULL) + pConn->disableTime;
                        if(pConn->disableTime<MAX_LUREDIS_DISABLE_TIME)
                            pConn->disableTime+=DEFAULT_LUREDIS_DISABLE_TIME; // increase wait time
                    }
                    else pConn->remainConnectFailTimes--;
                    return pConn;
                }
                redisSetTimeout(pContext,worker->timeout);
                pConn->pContext = pContext;
                // As the connection is re-established, all control variable are reset.
                pConn->remainConnectFailTimes = MAX_REDIS_CONNECT_FAIL_TIMES;
                pConn->disableTime = DEFAULT_LUREDIS_DISABLE_TIME;
            }
        }
    }
    return pConn;
}
//============================================================================================================
// Add a new luredisConnection into the dictionary with the key
// and initialize the first state for redis
//============================================================================================================
static luredisConnection* _allocateLuredisConnection(dict *conDict, const char* key) {
    luredisConnection *pConn = (luredisConnection*)malloc(sizeof(luredisConnection));
    char *hp = strdup(key);
    // conDict and key are secured by callers
    if(dictAdd(conDict,hp,pConn) != DICT_OK) {
        free(hp);
        free(pConn);
        return NULL;
    }
    pConn->hostport = strdup(hp);
    _resetLuredisConnectionState(pConn);
    return pConn;
}

static void _resetLuredisConnectionState(luredisConnection *pConn) {
    pConn->pContext = NULL;
    pConn->avgCommandTime = 0;
    pConn->failedCommands = 0;
    pConn->okCommands = 0;
    pConn->totalTime = 0;
    pConn->reconnectTime = pConn->startTime = time(NULL);
    pConn->remainConnectFailTimes = MAX_REDIS_CONNECT_FAIL_TIMES;
    pConn->disableTime = DEFAULT_LUREDIS_DISABLE_TIME;
}

static void _luredis_create_table_state(lua_State* L, luredisConnection *pConn) {
    // pConn is secured
    lua_createtable(L, 4, 0); // preallocate 4 table entries
    unsigned int total = pConn->okCommands + pConn->failedCommands;
    // Redis reply type
    pConn->avgCommandTime = (total>0)? pConn->totalTime/total:-1;
    lua_pushnumber(L,pConn->avgCommandTime);
    lua_setfield(L,-2,"avg");
    lua_pushnumber(L,pConn->failedCommands);
    lua_setfield(L,-2,"failed");
    lua_pushnumber(L,pConn->okCommands);
    lua_setfield(L,-2,"ok");
    lua_pushnumber(L,pConn->startTime);
    lua_setfield(L,-2,"start");
    lua_pushnumber(L,pConn->reconnectTime);
    lua_setfield(L,-2,"recon");
}

static void _luredis_log(const char* fn, const char *fmt, ...) {
    time_t now = time(NULL);
    va_list ap;
    FILE *fp;
    char buf[64]; /* Time */
    char msg[1024];

    fp = (fn == NULL) ? stdout : fopen(fn,"a");
    if (!fp) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    /* Get local time in string */
    strftime(buf,sizeof(buf),"%d %b %H:%M:%S",localtime(&now));
    fprintf(fp,"%s %s\n",buf,msg);
    fflush(fp);
    if(fn) fclose(fp);
}

