/* Standalone interpreter composition root. The vendored main stays unchanged;
 * only its standard-library initialization call is wrapped for CLI capabilities.
 * The engine links lua_lib and never compiles this wrapper or its fs module. */
#define lua_c
#include "lprefix.h"
#include "lua.h"
#include "lualib.h"
void caesura_cli_openlibs(lua_State *L);
#define luaL_openlibs caesura_cli_openlibs
#include "lua.c"
#undef luaL_openlibs
