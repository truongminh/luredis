#include "luredis.h"
#include <stdlib.h>

int loadScript(char *fn, lua_State *L);

int main(int argc, char *argv[])
{
    if(argc < 2) {
        printf("USAGE: ./luredis <script name>\n");
        exit(EXIT_FAILURE);
    }
    lua_State  *L = luaL_newstate();
    luaL_openlibs(L);
    lvm_register_luredis(L);

    if(loadScript(argv[1],L)) {
        printf("Cannot Load script %s\n",argv[1]);
    }
    lvm_close_luredis(L);
    exit(EXIT_SUCCESS);
}

int loadScript(char *fn, lua_State *L)
{
    int load_result = luaL_loadfile(L, fn); /* load script */
    if (load_result == 0) {
        /* call the loaded program */
        if (lua_pcall(L, 0, LUA_MULTRET, 0)) {
          printf("[ERROR] failed to load Master: \n\t %s\n", lua_tostring(L, -1));
          lua_pop(L, -1);
          return 1;
        }
        return 0;
    }
    else {
       if (load_result == LUA_ERRSYNTAX)
            printf("[ERROR] syntax error during pre-compilation.\n");
       else if (load_result == LUA_ERRMEM)
            printf("[ERROR] Memory Allocation error.\n");
       else
            printf("[ERROR] Cannot open file %s.\n", fn);
    }
    return 1;
}
