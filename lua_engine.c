#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <stddef.h>
#include <inttypes.h>

#include "lua_engine.h"

#include <memcached/util.h>
#include <memcached/config_parser.h>

#define INIT_FREE_STACK_SIZE 8
#define KEY_BUFFER_MAX       260

#ifdef UNUSED
#elif defined(__GNUC__)
# define UNUSED(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
# define UNUSED(x) /*@unused@*/ x
#else
# define UNUSED(x) x
#endif

static const char* luaeng_engine_info(ENGINE_HANDLE* handle);
static ENGINE_ERROR_CODE luaeng_engine_initialize(ENGINE_HANDLE* handle,
                                            const char* config_str);
static void luaeng_engine_destroy(ENGINE_HANDLE* handle);
static ENGINE_ERROR_CODE luaeng_item_allocate(ENGINE_HANDLE* handle,
                                               const void* cookie,
                                               item **item,
                                               const void* key,
                                               const size_t nkey,
                                               const size_t nbytes,
                                               const int flags,
                                               const rel_time_t exptime);
static void luaeng_item_release(ENGINE_HANDLE* handle, const void *cookie,
                                 item* item);
static ENGINE_ERROR_CODE luaeng_item_get(ENGINE_HANDLE* handle,
                                     const void* cookie,
                                     item** item,
                                     const void* key,
                                     const int nkey);
static ENGINE_ERROR_CODE luaeng_item_store(ENGINE_HANDLE* handle,
                                       const void *cookie,
                                       item* item,
                                       uint64_t *cas,
                                       ENGINE_STORE_OPERATION operation);
static ENGINE_ERROR_CODE luaeng_item_remove(ENGINE_HANDLE* handle,
                                             const void* cookie,
                                             const void* key,
                                             const size_t nkey,
                                             uint64_t cas);

static ENGINE_ERROR_CODE luaeng_item_arithmetic(ENGINE_HANDLE* handle,
                                            const void* cookie,
                                            const void* key,
                                            const int nkey,
                                            const bool increment,
                                            const bool create,
                                            const uint64_t delta,
                                            const uint64_t initial,
                                            const rel_time_t exptime,
                                            uint64_t *cas,
                                            uint64_t *result);
static ENGINE_ERROR_CODE luaeng_stats(ENGINE_HANDLE* handle,
                  const void *cookie,
                  const char *stat_key,
                  int nkey,
                  ADD_STAT add_stat);
static void luaeng_reset_stats(ENGINE_HANDLE* handle, const void *cookie);
static ENGINE_ERROR_CODE luaeng_flush(ENGINE_HANDLE* handle,
                                       const void* cookie, time_t when);
static ENGINE_ERROR_CODE initalize_configuration(struct luaeng *se,
                                                 const char *cfg_str);
static ENGINE_ERROR_CODE luaeng_unknown_command(ENGINE_HANDLE* handle,
                                                 const void* cookie,
                                                 protocol_binary_request_header *request,
                                                 ADD_RESPONSE response);

ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                  GET_SERVER_API get_server_api,
                                  ENGINE_HANDLE **handle) {
   SERVER_HANDLE_V1 *api = get_server_api(1);
   if (interface != 1 || api == NULL) {
      return ENGINE_ENOTSUP;
   }

   struct luaeng *engine = calloc(1, sizeof(*engine));
   if (engine == NULL) {
      return ENGINE_ENOMEM;
   }

   struct luaeng luaeng = {
      .engine = {
         .interface = {
            .interface = 1
         },
         .get_info = luaeng_engine_info,
         .initialize = luaeng_engine_initialize,
         .destroy = luaeng_engine_destroy,
         .allocate = luaeng_item_allocate,
         .release = luaeng_item_release,
         .get = luaeng_item_get,
         .store = luaeng_item_store,
         .remove = luaeng_item_remove,
         .arithmetic = luaeng_item_arithmetic,
         .flush = luaeng_flush,
         .get_stats = luaeng_stats,
         .reset_stats = luaeng_reset_stats,
         .unknown_command = luaeng_unknown_command,
         .item_get_cas = item_get_cas,
         .item_set_cas = item_set_cas,
         .item_get_key = item_get_key,
         .item_get_data = item_get_data,
         .item_get_clsid = item_get_clsid
      },
      .server = *api,
      .initialized = true,
      .lock = PTHREAD_MUTEX_INITIALIZER,
      .stats = {
         .lock = PTHREAD_MUTEX_INITIALIZER
      },
      .config = {
         .verbose = 0,
         .script = NULL
      }
   };

   luaeng.server = *api;
   *engine = luaeng;

   if (pthread_key_create(&engine->tld, NULL) != 0) {
      return ENGINE_ENOMEM;
   }

   *handle = (ENGINE_HANDLE*)&engine->engine;
   return ENGINE_SUCCESS;
}

static inline struct luaeng* get_handle(ENGINE_HANDLE* handle) {
   return (struct luaeng*)handle;
}

static const char* luaeng_engine_info(ENGINE_HANDLE* UNUSED(handle)) {
   return "Lua engine v0.1";
}

static lua_State* create_lua(struct luaeng* luaeng) {
   lua_State* L = lua_open();
   if (L != NULL) {
      luaL_openlibs(L);

      char *script = luaeng->config.script;
      if (script == NULL) {
         script = (char *) "./memcached.lua";
      }

      int err = luaL_dofile(L, script);
      if (err != 0) {
         fprintf(stderr, "script error: %s\n", lua_tostring(L, -1));
         exit(EXIT_FAILURE);
      }
   }

   return L;
}

static lua_State* acquire_lua(struct luaeng* luaeng) {
   lua_State* L = NULL;

   struct luaeng_tld* tld = pthread_getspecific(luaeng->tld);
   if (tld == NULL) {
      tld = calloc(1, sizeof(*tld));
      pthread_setspecific(luaeng->tld, tld);
   }

   if (tld != NULL &&
       tld->free_stack != NULL &&
       tld->free_stack_top >= 0) {
      L = tld->free_stack[tld->free_stack_top];
      tld->free_stack[tld->free_stack_top] = NULL;
      tld->free_stack_top--;
   }

   if (L == NULL) {
      L = create_lua(luaeng);
   }

   return L;
}

static void release_lua(struct luaeng* luaeng, lua_State* L) {
   struct luaeng_tld* tld = pthread_getspecific(luaeng->tld);
   if (tld != NULL) {
      tld->free_stack_top++;
      assert(tld->free_stack_top >= 0);
      if (tld->free_stack_top >= tld->free_stack_size) {
         tld->free_stack_size = tld->free_stack_size * 2;
         if (tld->free_stack_size < INIT_FREE_STACK_SIZE) {
            tld->free_stack_size = INIT_FREE_STACK_SIZE;
         }
         tld->free_stack = realloc(tld->free_stack, tld->free_stack_size * sizeof(lua_State*));
      }
      tld->free_stack[tld->free_stack_top] = L;
   }
}

/**
 * call_lua_va(L, "f", "dd>d", x, y, &z);
 *
 * The string "dd>d" means "two arguments of type double, one result of type double".
 */
static void call_lua_va(lua_State *L, const char *func, const char *sig, ...) {
  va_list vl;
  int narg, nres;  /* number of arguments and results */

  const char *sig_in = sig;

  va_start(vl, sig);

  lua_getglobal(L, func);  /* get function */

  /* push arguments */
  narg = 0;
  while (*sig) {  /* push arguments */
    switch (*sig++) {
    case 'd':  /* double argument */
      lua_pushnumber(L, va_arg(vl, double));
      break;

    case 'i':  /* int argument */
      lua_pushnumber(L, va_arg(vl, int));
      break;

    case 's':  /* string argument */
      lua_pushstring(L, va_arg(vl, char *));
      break;

    case '>':
      goto endwhile;

    default:
      fprintf(stderr, "call_lua_va: invalid option (%c) calling %s (%s)\n", *(sig - 1), func, sig_in);
      exit(EXIT_FAILURE);
    }
    narg++;
    luaL_checkstack(L, 1, "call_lua_va: too many arguments");
  } endwhile:

  /* do the call */
  nres = strlen(sig);  /* number of expected results */
  if (lua_pcall(L, narg, nres, 0) != 0) { /* do the call */
    fprintf(stderr, "call_lua_va: error running function %s (%s): %s\n",
            func, sig_in, lua_tostring(L, -1));
    exit(EXIT_FAILURE);
  }

  /* retrieve results */
  nres = -nres;  /* stack index of first result */
  while (*sig) {  /* get results */
    switch (*sig++) {
    case 'd':  /* double result */
      if (!lua_isnumber(L, nres)) {
        fprintf(stderr, "call_lua_va: wrong result type after calling %s (%s) - double was expected\n", func, sig_in);
        exit(EXIT_FAILURE);
      }
      *va_arg(vl, double *) = lua_tonumber(L, nres);
      break;

    case 'i':  /* int result */
      if (!lua_isnumber(L, nres)) {
        fprintf(stderr, "call_lua_va: wrong result type after calling %s (%s) - int was expected\n", func, sig_in);
        exit(EXIT_FAILURE);
      }
      *va_arg(vl, int *) = (int)lua_tonumber(L, nres);
      break;

    case 's':  /* string result */
      if (!lua_isstring(L, nres)) {
        fprintf(stderr, "call_lua_va: wrong result type after calling %s (%s)- string was expected\n", func, sig_in);
        exit(EXIT_FAILURE);
      }
      *va_arg(vl, const char **) = lua_tostring(L, nres);
      break;

    default:
      fprintf(stderr, "call_lua_va: invalid option (%c) calling %s (%s)\n", *(sig - 1), func, sig_in);
      exit(EXIT_FAILURE);
    }
    nres++;
  }

  va_end(vl);
}

static ENGINE_ERROR_CODE luaeng_engine_initialize(ENGINE_HANDLE* handle,
                                            const char* config_str) {
   struct luaeng* se = get_handle(handle);

   ENGINE_ERROR_CODE ret = initalize_configuration(se, config_str);
   if (ret != ENGINE_SUCCESS) {
      return ret;
   }

   return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE initalize_configuration(struct luaeng *se,
                                                 const char *cfg_str) {
   ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;

   if (cfg_str != NULL) {
      struct config_item items[] = {
         { .key = "verbose",
           .datatype = DT_SIZE,
           .value.dt_size = &se->config.verbose },
         { .key = "script",
           .datatype = DT_STRING,
           .value.dt_string = &se->config.script },
         { .key = NULL }
      };

      ret = se->server.parse_config(cfg_str, items, stderr);
   }

   return ENGINE_SUCCESS;
}

static void luaeng_engine_destroy(ENGINE_HANDLE* handle) {
   struct luaeng* se = get_handle(handle);

   if (se->initialized) {
      pthread_mutex_destroy(&se->lock);
      pthread_mutex_destroy(&se->stats.lock);
      se->initialized = false;
      free(se);
   }
}

static ENGINE_ERROR_CODE luaeng_item_allocate(ENGINE_HANDLE* UNUSED(handle),
                                              const void* UNUSED(cookie),
                                              item **item_out,
                                              const void* key,
                                              const size_t nkey,
                                              const size_t nbytes,
                                              const int flags,
                                              const rel_time_t exptime) {
   size_t ntotal = sizeof(item) + nkey + nbytes;
   item *it = malloc(ntotal);
   if (it != NULL) {
      it->exptime = exptime;
      it->nbytes = nbytes;
      it->flags = flags;
      it->nkey = nkey;
      it->iflag = 0;
      memcpy((void*) (it + 1), key, nkey);
      *item_out = it;
      return ENGINE_SUCCESS;
   } else {
      return ENGINE_ENOMEM;
   }
}

static void luaeng_item_release(ENGINE_HANDLE* UNUSED(handle),
                                const void* UNUSED(cookie),
                                item* it) {
  free(it);
}

static ENGINE_ERROR_CODE luaeng_item_get(ENGINE_HANDLE* handle,
                                         const void* cookie,
                                         item** it,
                                         const void* key,
                                         const int nkey) {
   struct luaeng* se = get_handle(handle);
   lua_State *L = acquire_lua(se);

   ENGINE_ERROR_CODE res = ENGINE_KEY_ENOENT;

   *it = NULL;

   lua_getglobal(L, "memcached_get");
   lua_pushlstring(L, key, nkey);

   int nres = 3;

   if (lua_pcall(L, 1, nres, 0) != 0) {
      fprintf(stderr, "memcached_get lua error: %s\n", lua_tostring(L, -1));
      exit(EXIT_FAILURE);
   }

   nres = -nres;

   size_t val_len = 0;
   const char* val = lua_tolstring(L, nres++, &val_len);
   if (val != NULL) {
      int it_flg = (int) lua_tonumber(L, nres++);
      int it_exp = (int) lua_tonumber(L, nres++);

      if (luaeng_item_allocate(handle, cookie, it, key, nkey, val_len, it_flg, it_exp) == ENGINE_SUCCESS) {
         memcpy(item_get_data(*it), val, val_len);
         res = ENGINE_SUCCESS;
      }
   }

   release_lua(se, L);
   return res;
}

static ENGINE_ERROR_CODE luaeng_item_store(ENGINE_HANDLE* handle,
                                           const void* UNUSED(cookie),
                                           item* it,
                                           uint64_t* UNUSED(cas),
                                           ENGINE_STORE_OPERATION operation) {
   struct luaeng* se = get_handle(handle);
   lua_State *L = acquire_lua(se);

   ENGINE_ERROR_CODE res = ENGINE_NOT_STORED;

   lua_getglobal(L, "memcached_store");

   lua_pushlstring(L, item_get_key(it), it->nkey);
   lua_pushnumber(L, operation);
   lua_pushlstring(L, item_get_data(it), it->nbytes);
   lua_pushnumber(L, it->flags);
   lua_pushnumber(L, it->exptime);

   if (lua_pcall(L, 5, 1, 0) != 0) {
      fprintf(stderr, "memcached_store lua error: %s\n", lua_tostring(L, -1));
      exit(EXIT_FAILURE);
   }

   release_lua(se, L);
   return res;
}

static ENGINE_ERROR_CODE luaeng_item_remove(ENGINE_HANDLE* handle,
                                            const void* UNUSED(cookie),
                                            const void* key,
                                            const size_t nkey,
                                            uint64_t UNUSED(cas)) {
   struct luaeng* se = get_handle(handle);
   lua_State *L = acquire_lua(se);

   ENGINE_ERROR_CODE res = ENGINE_SUCCESS;

   lua_getglobal(L, "memcached_remove");
   lua_pushlstring(L, key, nkey);

   if (lua_pcall(L, 1, 1, 0) != 0) {
      fprintf(stderr, "memcached_remove lua error: %s\n", lua_tostring(L, -1));
      exit(EXIT_FAILURE);
   }

   release_lua(se, L);
   return res;
}

static ENGINE_ERROR_CODE luaeng_item_arithmetic(ENGINE_HANDLE* handle,
                                                const void* UNUSED(cookie),
                                                const void* UNUSED(key),
                                                const int UNUSED(nkey),
                                                const bool UNUSED(increment),
                                                const bool UNUSED(create),
                                                const uint64_t UNUSED(delta),
                                                const uint64_t UNUSED(initial),
                                                const rel_time_t UNUSED(exptime),
                                                uint64_t* UNUSED(cas),
                                                uint64_t* UNUSED(result)) {
   struct luaeng* se = get_handle(handle);
   lua_State *L = acquire_lua(se);

   ENGINE_ERROR_CODE res = ENGINE_KEY_ENOENT;

   release_lua(se, L);
   return res;
}

static ENGINE_ERROR_CODE luaeng_flush(ENGINE_HANDLE* handle,
                                      const void* UNUSED(cookie),
                                      time_t when) {
   struct luaeng* se = get_handle(handle);
   lua_State *L = acquire_lua(se);

   ENGINE_ERROR_CODE res = ENGINE_SUCCESS;

   int i = 0;

   call_lua_va(L, "memcached_flush", "i>i", when, &i);

   release_lua(se, L);
   return res;
}

static ENGINE_ERROR_CODE luaeng_stats(ENGINE_HANDLE* handle,
                                      const void* cookie,
                                      const char* stat_key,
                                      int UNUSED(nkey),
                                      ADD_STAT add_stat) {
   struct luaeng* se = get_handle(handle);
   lua_State *L = acquire_lua(se);

   ENGINE_ERROR_CODE res = ENGINE_SUCCESS;

   if (stat_key == NULL) {
      char val[128];
      int len;

      pthread_mutex_lock(&se->stats.lock);
      len = sprintf(val, "%"PRIu64, (uint64_t)se->stats.total_items);
      add_stat("total_items", 11, val, len, cookie);
      pthread_mutex_unlock(&se->stats.lock);
   } else {
      res = ENGINE_KEY_ENOENT;
   }

   release_lua(se, L);
   return res;
}

static void luaeng_reset_stats(ENGINE_HANDLE* handle,
                               const void* UNUSED(cookie)) {
   struct luaeng* se = get_handle(handle);
   lua_State *L = acquire_lua(se);

   pthread_mutex_lock(&se->stats.lock);
   se->stats.total_items = 0;
   pthread_mutex_unlock(&se->stats.lock);

   release_lua(se, L);
}

static ENGINE_ERROR_CODE luaeng_unknown_command(ENGINE_HANDLE* handle,
                                                const void* cookie,
                                                protocol_binary_request_header* UNUSED(request),
                                                ADD_RESPONSE response) {
   struct luaeng* se = get_handle(handle);
   lua_State *L = acquire_lua(se);

   ENGINE_ERROR_CODE res = ENGINE_FAILED;

   if (response(NULL, 0, NULL, 0, NULL, 0,
                PROTOCOL_BINARY_RAW_BYTES,
                PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, 0, cookie)) {
      res = ENGINE_SUCCESS;
   } else {
      res = ENGINE_FAILED;
   }

   release_lua(se, L);
   return res;
}

uint64_t item_get_cas(const item* UNUSED(it))
{
    return 0;
}

void item_set_cas(item* UNUSED(it), uint64_t UNUSED(val))
{
}

const char* item_get_key(const item* it)
{
    return (char*)(it + 1);
}

char* item_get_data(const item* it)
{
    return ((char*)item_get_key(it)) + it->nkey;
}

uint8_t item_get_clsid(const item* UNUSED(it))
{
    return 0;
}
