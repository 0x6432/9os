#!/bin/sh
# M40 Qt base first slice: Core + DBus. GUI/Wayland/Quick follow separately.
. "$(dirname "$0")/common.sh"
V=6.8.3
T=$DL/qtbase-everywhere-src-$V.tar.xz
"$UL/fetch.sh" "$T" \
    "https://distfiles.alpinelinux.org/distfiles/v3.22/qtbase-everywhere-src-$V.tar.xz" \
    "https://download.qt.io/archive/qt/6.8/$V/submodules/qtbase-everywhere-src-$V.tar.xz"
printf '%s  %s\n' ef364f939f23b622f67d21833c2dbf1fb74531d9a1e25b6d2e94ea5d747a40f20c6c3a24abef1e9710287366b7cb54dd090350d315601317779235c20743cc81 "$T" | sha512sum -c -
S=$PORTS_SRC/qtbase-everywhere-src-$V
[ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
H=${HOST_TOOLS:-${TOOLS_DIR:-/data/tools}/host}/qt-$V
HB=$UL/build/qt-host-$V
if [ ! -x "$H/libexec/moc" ] || [ ! -x "$H/bin/qdbuscpp2xml" ] || [ ! -x "$H/bin/qdbusxml2cpp" ]; then
    env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR -u PKG_CONFIG_PATH \
    cmake -S "$S" -B "$HB" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$H" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF \
        -DFEATURE_gui=OFF -DFEATURE_network=OFF -DFEATURE_sql=OFF \
        -DFEATURE_dbus=ON -DFEATURE_dbus_linked=OFF -DFEATURE_icu=OFF -DFEATURE_glib=OFF \
        -DFEATURE_system_pcre2=OFF -DFEATURE_system_zlib=OFF \
        > "$HB.log" 2>&1 || { tail -40 "$HB.log"; exit 1; }
    cmake --build "$HB" -j"${JOBS:-2}" >> "$HB.log" 2>&1 || { tail -40 "$HB.log"; exit 1; }
    cmake --install "$HB" >> "$HB.log" 2>&1 || { tail -40 "$HB.log"; exit 1; }
fi
B=$PORTS_BUILD/qtbase
TC=$PORTS_BUILD/qt-toolchain.cmake
cat > "$TC" <<EOT
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $ARCH)
set(CMAKE_C_COMPILER "$DCCBIN")
set(CMAKE_CXX_COMPILER "$UL/musl-dcxx")
set(CMAKE_AR "$(command -v llvm-ar)")
set(CMAKE_RANLIB "$(command -v llvm-ranlib)")
set(CMAKE_STRIP "$(command -v llvm-strip)")
set(CMAKE_FIND_ROOT_PATH "$PREFIX_ROOT;$SYSROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_FLAGS_INIT "-I$PREFIX_ROOT/usr/include")
set(CMAKE_CXX_FLAGS_INIT "-I$PREFIX_ROOT/usr/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L$PREFIX_ROOT/usr/lib -Wl,-rpath-link,$PREFIX_ROOT/usr/lib")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L$PREFIX_ROOT/usr/lib -Wl,-rpath-link,$PREFIX_ROOT/usr/lib")
EOT
cmake -S "$S" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
    -DQT_HOST_PATH="$H" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF \
    -DFEATURE_gui=ON -DFEATURE_widgets=OFF -DFEATURE_network=OFF -DFEATURE_sql=OFF \
    -DFEATURE_opengl=OFF -DFEATURE_vulkan=OFF -DFEATURE_xcb=OFF \
    -DFEATURE_eglfs=OFF -DFEATURE_linuxfb=OFF -DFEATURE_evdev=OFF -DFEATURE_libinput=OFF \
    -DFEATURE_xkbcommon=OFF -DFEATURE_fontconfig=OFF \
    -DFEATURE_system_freetype=OFF -DFEATURE_system_harfbuzz=OFF \
    -DFEATURE_system_png=OFF -DFEATURE_system_jpeg=OFF \
    -DFEATURE_dbus=ON -DFEATURE_dbus_linked=ON -DFEATURE_icu=OFF -DFEATURE_glib=OFF \
    -DFEATURE_system_pcre2=OFF -DFEATURE_system_zlib=OFF \
    > "$B.log" 2>&1 || { tail -50 "$B.log"; exit 1; }
cmake --build "$B" -j"${JOBS:-2}" >> "$B.log" 2>&1 || { tail -50 "$B.log"; exit 1; }
DESTDIR="$PREFIX_ROOT" cmake --install "$B" >> "$B.log" 2>&1 || { tail -40 "$B.log"; exit 1; }
"$H/libexec/moc" "$UL/ports/src/qttest.cc" -o "$PORTS_BUILD/qttest.moc"
"$UL/musl-dcxx" -std=c++17 -O2 -fPIC -fexceptions -frtti \
    -I"$PREFIX_ROOT/usr/include" -I"$PREFIX_ROOT/usr/include/QtCore" \
    -I"$PREFIX_ROOT/usr/include/QtDBus" -I"$PORTS_BUILD" \
    "$UL/ports/src/qttest.cc" -L"$PREFIX_ROOT/usr/lib" -lQt6DBus -lQt6Core \
    -o "$PREFIX_ROOT/usr/bin/qttest"
install -m755 "$UL/ports/scripts/qtsmoke" "$PREFIX_ROOT/usr/bin/qtsmoke"
"$UL/musl-dcxx" -std=c++17 -O2 -fPIC -fexceptions -frtti \
    -I"$PREFIX_ROOT/usr/include" -I"$PREFIX_ROOT/usr/include/QtCore" \
    -I"$PREFIX_ROOT/usr/include/QtGui" \
    "$UL/ports/src/qtguitest.cc" -L"$PREFIX_ROOT/usr/lib" -lQt6Gui -lQt6Core \
    -o "$PREFIX_ROOT/usr/bin/qtguitest"
install -m755 "$UL/ports/scripts/qtguismoke" "$PREFIX_ROOT/usr/bin/qtguismoke"
echo "Qt $V Core + DBus + raster/offscreen GUI installed (Wayland/GL/Quick not yet built)"
