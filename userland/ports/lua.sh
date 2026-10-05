#!/bin/sh
# Lua 5.4: liblua.so (shared) + dynamically linked lua and luac.
. "$(dirname "$0")/common.sh"
V=5.4.7
T=$(fetch https://www.lua.org/ftp/lua-$V.tar.gz)
B=$PORTS_BUILD/lua-$V; rm -rf "$B"; mkdir -p "$B"; tar xzf "$T" -C "$PORTS_BUILD"
cd "$B/src"
CF="-O2 -fPIC -DLUA_USE_POSIX -DLUA_USE_DLOPEN"
for f in *.c; do "$CC" $CF -c "$f" -o "${f%.c}.o"; done
mkso liblua.so.5.4 $(ls *.o | grep -v -e '^lua.o$' -e '^luac.o$')
$DCC -O2 -o lua lua.o -L. -l:liblua.so.5.4 -lm
$DCC -O2 -o luac luac.o $(ls *.o | grep -v -e '^lua.o$' -e '^luac.o$') -lm
cp liblua.so.5.4 "$PREFIX_ROOT/usr/lib/" && llvm-strip "$PREFIX_ROOT/usr/lib/liblua.so.5.4"
ln -sf liblua.so.5.4 "$PREFIX_ROOT/usr/lib/liblua.so"
cp lua luac "$PREFIX_ROOT/usr/bin/" && llvm-strip "$PREFIX_ROOT/usr/bin/lua" "$PREFIX_ROOT/usr/bin/luac"
cp lua.h luaconf.h lualib.h lauxlib.h lua.hpp "$PREFIX_ROOT/usr/include/"
echo "lua $V installed"
