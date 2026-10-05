#!/bin/sh
# SQLite 3 amalgamation: libsqlite3.so + dynamically linked sqlite3 shell.
. "$(dirname "$0")/common.sh"
V=3470200
Z=$(fetch https://www.sqlite.org/2024/sqlite-amalgamation-$V.zip)
B=$PORTS_BUILD/sqlite-amalgamation-$V; rm -rf "$B"; (cd "$PORTS_BUILD" && unzip -qo "$Z")
cd "$B"
"$CC" -O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_MATH_FUNCTIONS -c sqlite3.c -o sqlite3.o
mkso libsqlite3.so.0 sqlite3.o
$DCC -O2 -DSQLITE_OMIT_LOAD_EXTENSION -o sqlite3 shell.c -L. -l:libsqlite3.so.0 -lm
cp libsqlite3.so.0 "$PREFIX_ROOT/usr/lib/" && llvm-strip "$PREFIX_ROOT/usr/lib/libsqlite3.so.0"
cp sqlite3 "$PREFIX_ROOT/usr/bin/" && llvm-strip "$PREFIX_ROOT/usr/bin/sqlite3"
cp sqlite3.h "$PREFIX_ROOT/usr/include/"
echo "sqlite $V installed"
