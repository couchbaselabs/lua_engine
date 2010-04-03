/*
 * Summary: Lua engine implementation.
 *
 * Copy: See Copyright for the status of this software.
 *
 * Author: Trond Norbye <trond.norbye@sun.com>
 */
#ifndef MEMCACHED_LUA_ENGINE_H
#define MEMCACHED_LUA_ENGINE_H

#include "config.h"

#include <pthread.h>
#include <stdbool.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <memcached/engine.h>

#include <memcached/util.h>

#ifndef PUBLIC

#if defined (__SUNPRO_C) && (__SUNPRO_C >= 0x550)
#define PUBLIC __global
#elif defined __GNUC__
#define PUBLIC __attribute__ ((visibility("default")))
#else
#define PUBLIC
#endif

#endif

/* Forward decl */
struct luaeng;

#ifdef __cplusplus
extern "C" {
#endif

PUBLIC
ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                  GET_SERVER_API get_server_api,
                                  ENGINE_HANDLE **handle);

struct luaeng_config {
   size_t verbose;
   char *script;
};

/**
 * Statistic information collected by the lua engine
 */
struct luaeng_stats {
   pthread_mutex_t lock;
   uint64_t total_items;
};

/**
 * Definition of the private instance data used by the lua engine.
 *
 * This is currently "work in progress" so it is not as clean as it should be.
 */
struct luaeng {
   ENGINE_HANDLE_V1 engine;
   SERVER_HANDLE_V1 server;

   /**
    * Is the engine initalized or not
    */
   bool initialized;

   pthread_mutex_t lock;

   lua_State **free_stack;      // Array of unused lua interpreters.
   int         free_stack_top;  // 0-based index to first free entry in the free_lua_arr, -1 when empty.
   int         free_stack_size; // Total number of entries in the free_stack.

   struct luaeng_config config;
   struct luaeng_stats stats;
};

char* item_get_data(const item* item);
const char* item_get_key(const item* item);
void item_set_cas(item* item, uint64_t val);
uint64_t item_get_cas(const item* item);
uint8_t item_get_clsid(const item* item);

#endif
