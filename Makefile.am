ACLOCAL_AMFLAGS = -I m4 --force

lib_LTLIBRARIES = lua_engine.la

lua_engine_la_SOURCES = \
    lua_engine.c lua_engine.h

lua_engine_la_DEPENDENCIES=
lua_engine_la_LIBADD= -llua
lua_engine_la_LDFLAGS= -module -dynamic
