# Shared helpers for userland/ports/*.sh (sourced). Ports install into $PREFIX_ROOT/usr,
# which mkroot.sh copies into the root filesystem.
set -e
ARCH=${ARCH:-x86_64}; export ARCH
UL=$(cd "$(dirname "$0")/.." && pwd)
SYSROOT=$UL/sysroot/$ARCH
CC="$UL/musl-cc"
DCC="env DYNAMIC=1 $UL/musl-cc"
PORTS_SRC=$UL/build/ports-src
PORTS_BUILD=$UL/build/ports-$ARCH
PREFIX_ROOT=$UL/build/ports-root-$ARCH
DL=${DL:-/data/tools/dl}
mkdir -p "$PORTS_SRC" "$PORTS_BUILD" "$PREFIX_ROOT/usr/bin" "$PREFIX_ROOT/usr/lib" "$PREFIX_ROOT/usr/include" "$DL"
# fetch URL [file]: download once into $DL
fetch() { f=${2:-$(basename "$1")}; [ -s "$DL/$f" ] || curl -sfL -o "$DL/$f" "$1"; echo "$DL/$f"; }
# build a shared library from objects: mkso libname.so obj...
mkso() { so=$1; shift; "$CC" -shared -nostdlib -Wl,-soname,"$so" -o "$so" "$@" -L"$SYSROOT/lib" -lc; }
DCCBIN=$UL/musl-dcc
# pkg-config sees only the ports root (cross libraries), with paths rewritten into it
export PKG_CONFIG_LIBDIR="$PREFIX_ROOT/usr/lib/pkgconfig:$PREFIX_ROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$PREFIX_ROOT"
export PKG_CONFIG_PATH=
case $ARCH in x86_64) CPUFAM=x86_64;; riscv64) CPUFAM=riscv64;; aarch64) CPUFAM=aarch64;; esac
CROSS=$PORTS_BUILD/meson-cross.txt
cat > "$CROSS" <<EOC
[binaries]
c = '$DCCBIN'
cpp = '$DCCBIN'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-I$PREFIX_ROOT/usr/include']
c_link_args = ['-L$PREFIX_ROOT/usr/lib', '-Wl,-rpath-link,$PREFIX_ROOT/usr/lib']
[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$PREFIX_ROOT/usr/lib/pkgconfig', '$PREFIX_ROOT/usr/share/pkgconfig']
sys_root = '$PREFIX_ROOT'
[host_machine]
system = 'linux'
cpu_family = '$CPUFAM'
cpu = '$CPUFAM'
endian = 'little'
EOC
# meson_port SRCDIR BUILDDIR [meson options...]: configure, build, install into the ports root
meson_port() {
    s=$1; b=$2; shift 2
    rm -rf "$b"
    env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_PATH="${HOST_PC_PATH:-}" \
    meson setup "$b" "$s" --cross-file "$CROSS" --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared "$@" >"$b.log" 2>&1 || { tail -30 "$b.log"; exit 1; }
    ninja -C "$b" >>"$b.log" 2>&1 || { tail -30 "$b.log"; exit 1; }
    DESTDIR="$PREFIX_ROOT" meson install -C "$b" --no-rebuild >>"$b.log" 2>&1 || { tail -30 "$b.log"; exit 1; }
}
# autotools_port SRCDIR [configure options...] (builds in-tree)
autotools_port() {
    s=$1; shift
    (cd "$s" && ./configure --host=$ARCH-linux-musl --build=x86_64-pc-linux-gnu --prefix=/usr --libdir=/usr/lib \
        CC="$DCCBIN" AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip CFLAGS="-O2 -fPIC" "$@" >config.out 2>&1 || { tail -30 config.out; exit 1; }
     make -j"$(nproc)" >make.out 2>&1 || { tail -30 make.out; exit 1; }
     make DESTDIR="$PREFIX_ROOT" install >>make.out 2>&1 || { tail -30 make.out; exit 1; })
}
# fix .la/.pc leftovers that would point at host paths
cleanup_la() { rm -f "$PREFIX_ROOT"/usr/lib/*.la; }
