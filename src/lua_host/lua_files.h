// Force-included into the Lua library's sources (CMakeLists.txt). Lua opens files with fopen, which reads a path in
// the ANSI code page, but the paths a mod uses are UTF-8 (emf.dir, package.path, the mod's own strings): a mod folder
// with non-ASCII letters in its name could not be read. These take UTF-8 and use the wide-character functions; a name that is not valid
// UTF-8 goes to the ANSI function as before. Covers require, loadfile, dofile, io.open, io.lines, os.remove, os.rename.
#pragma once
#include <stdio.h>

FILE* emf_lua_fopen(const char* name, const char* mode);
FILE* emf_lua_freopen(const char* name, const char* mode, FILE* f);
int emf_lua_remove(const char* name);
int emf_lua_rename(const char* from, const char* to);

#define fopen emf_lua_fopen
#define freopen emf_lua_freopen
#define remove emf_lua_remove
#define rename emf_lua_rename
