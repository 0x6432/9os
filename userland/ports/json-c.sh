#!/bin/sh
# json-c (sway IPC). Upstream builds with CMake only; this compiles the sources directly
# with a hand-written config.h (musl, 64-bit).
. "$(dirname "$0")/common.sh"
V=0.18
T=$(fetch https://s3.amazonaws.com/json-c_releases/releases/json-c-$V.tar.gz)
S=$PORTS_SRC/json-c-$V; [ -d "$S" ] || tar xzf "$T" -C "$PORTS_SRC"
B=$PORTS_BUILD/json-c; rm -rf "$B"; mkdir -p "$B"
cat > "$B/config.h" <<'EOC'
#define HAVE_DLFCN_H 1
#define HAVE_ENDIAN_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYSLOG_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_RANDOM_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ATOMIC_BUILTINS 1
#define HAVE_DECL_INFINITY 1
#define HAVE_DECL_ISINF 1
#define HAVE_DECL_ISNAN 1
#define HAVE_DECL_NAN 1
#define HAVE_OPEN 1
#define HAVE_REALLOC 1
#define HAVE_SETLOCALE 1
#define HAVE_SNPRINTF 1
#define HAVE_STRCASECMP 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRNCASECMP 1
#define HAVE_USELOCALE 1
#define HAVE_VASPRINTF 1
#define HAVE_VPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_VSYSLOG 1
#define HAVE_GETRANDOM 1
#define HAVE_GETRUSAGE 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE___THREAD 1
#define SPEC___THREAD __thread
#define JSON_C_HAVE_INTTYPES_H 1
#define SIZEOF_INT 4
#define SIZEOF_INT64_T 8
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_SIZE_T 8
#define SIZEOF_SSIZE_T 8
#define STDC_HEADERS 1
#define ENABLE_THREADING 1
#define PACKAGE_STRING "json-c 0.18"
#define PACKAGE_VERSION "0.18"
EOC
printf '#define JSON_C_HAVE_INTTYPES_H 1\n#define JSON_C_HAVE_STDINT_H 1\n' > "$B/json_config.h"
sed -e 's|@JSON_H_JSON_POINTER@|#include "json_pointer.h"|' -e 's|@JSON_H_JSON_PATCH@|#include "json_patch.h"|' "$S/json.h.cmakein" > "$B/json.h"
SRCS="arraylist debug json_c_version json_object json_object_iterator json_tokener json_util json_visit linkhash printbuf random_seed strerror_override json_pointer json_patch"
objs=
for f in $SRCS; do
    $DCCBIN -O2 -fPIC -fvisibility=default -D_GNU_SOURCE -I"$B" -I"$S" -c "$S/$f.c" -o "$B/$f.o"
    objs="$objs $B/$f.o"
done
$DCCBIN -shared -Wl,-soname,libjson-c.so.5 -o "$B/libjson-c.so.5" $objs
I=$PREFIX_ROOT/usr/include/json-c; mkdir -p "$I" "$PREFIX_ROOT/usr/lib/pkgconfig"
cp "$B/libjson-c.so.5" "$PREFIX_ROOT/usr/lib/"; ln -sf libjson-c.so.5 "$PREFIX_ROOT/usr/lib/libjson-c.so"
cp "$B/json_config.h" "$B/json.h" "$I/"
for h in arraylist debug json_c_version json_inttypes json_object json_object_iterator json_tokener json_types json_util json_visit linkhash printbuf json_pointer json_patch; do cp "$S/$h.h" "$I/"; done
cat > "$PREFIX_ROOT/usr/lib/pkgconfig/json-c.pc" <<EOC
prefix=/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: json-c
Description: A JSON implementation in C
Version: $V
Libs: -L\${libdir} -ljson-c
Cflags: -I\${includedir} -I\${includedir}/json-c
EOC
echo "json-c $V installed"
