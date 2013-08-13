#ifndef LVM_CONFIG_H
#define LVM_CONFIG_H

#define LVM_LOG_DEBUG 0
#define LVM_LOG_INFO 1
#define LVM_LOG_WARNING 2
#define LVM_LOG_CRITICAL 3

#ifndef LUAJIT_2_0
#define LUAJIT_2_0
#include <luajit-2.0/lua.h>
#include <luajit-2.0/lauxlib.h>
#include <luajit-2.0/lualib.h>
#endif

#endif // LVM_CONFIG_H
