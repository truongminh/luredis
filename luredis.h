/*
  luredis connector v1.0
  luredis provide a LUA API that facilitates interaction with redis server.
  Based on: hiredis, the offical C API to redis server.
  BSD license.
  Date: 08/29/12
  (C) minhnt@live.com

  How to use luredis in LUA?
  [1] Create a new connection: c = luredis.new_connection("host",port)
      If the connection cannot be established, the app will exit.
  [2] Send a command to redis server. luredis.sendCommand(c,<command>)
      The returned value is pushed onto Lua Stack.
      Remember check the type of returned value before proceed.
*/

#ifndef LUREDIS_H
#define LUREDIS_H

#include "lvm_config.h"


/******************************************************************************/
/* Constants                                                                  */
/******************************************************************************/

#define LUREDIS_MAXARGS 32
/* Internal buffer used in high-performance clients */
#define LUREDIS_MAX_BUF_LEN 4096 // 4KB

#define LIB_LUREDIS "luredis"
#ifdef ENABLE_REDIS_CONTEXT
    #define REDIS_CONTEXT "luredis_context"
#endif

#define LUREDIS_VERSION     "luredis 1.0"
#define LUREDIS_COPYRIGHT   "Copyright (C) 2013 Nguyen Truong Minh (minhnt@live.com)"
#define LUREDIS_DESCRIPTION "Bindings for hiredis Redis-client library"
#define LUREDIS_REDIS_WORKER "worker"
// #define LUREDIS_ENABLE_ASYNC_REDIS_CONTEXT // remain buggy
#define DEFAULT_REDIS_SEND_RECV_TIMEOUT 2 // seconds
#define MAX_REDIS_CONNECT_FAIL_TIMES 4
#define DEFAULT_LUREDIS_DISABLE_TIME 5 // seconds
#define MAX_LUREDIS_DISABLE_TIME 60 // seconds
#define LUREDIS_MAP_CONNECTION_DICT_SIZE 64 // bigger dict size, more efficient hash table
#define LUREDIS_LOG_FILE "/tmp/luredis.log"


    /******************************************************************************/
    /* Luredis API                                                                */
    /******************************************************************************/
    int lvm_register_luredis (lua_State *L);
    void lvm_close_luredis(lua_State *L);


#endif // LUREDIS_H
