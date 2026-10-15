#!/bin/sh
# M40: QtWayland raster/shm client only. No compositor, EGL, QML/Quick or KDE claim.
. "$(dirname "$0")/common.sh"
V=6.8.3
T=$DL/qtwayland-everywhere-src-$V.tar.xz
"$UL/fetch.sh" "$T" \
    "https://distfiles.alpinelinux.org/distfiles/v3.22/qtwayland-everywhere-src-$V.tar.xz" \
    "https://download.qt.io/archive/qt/6.8/$V/submodules/qtwayland-everywhere-src-$V.tar.xz"
# Independently pinned against Alpine 3.22 qt6-qtwayland/APKBUILD.
printf '%s  %s\n' 2edb6d3e85c06bcf689c3acfcc38b6ff0505569dfeec8bfb5a434fad563087ff917875ea56e639405af3cca8adc98206ab9f68dfaf8e2449300d010ffbb16699 "$T" | sha512sum -c -
S=$PORTS_SRC/qtwayland-everywhere-src-$V
[ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
HOST=${HOST_TOOLS:-${TOOLS_DIR:-/data/tools}/host}
H=$HOST/qt-$V
HB=$UL/build/qtwayland-host-$V
# Export a real native executable target using Qt's expected tools package name.
# The generator needs native Qt Core only; do not build or run target tools.
env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR -u PKG_CONFIG_PATH \
cmake -S "$UL/ports/qtwayland-host" -B "$HB" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$H" \
    -DCMAKE_INSTALL_PREFIX="$H" -DQTWAYLAND_SOURCE_DIR="$S" \
    > "$HB.log" 2>&1 || { tail -40 "$HB.log"; exit 1; }
cmake --build "$HB" -j"${JOBS:-2}" >> "$HB.log" 2>&1 || { tail -40 "$HB.log"; exit 1; }
cmake --install "$HB" >> "$HB.log" 2>&1 || { tail -40 "$HB.log"; exit 1; }
B=$PORTS_BUILD/qtwayland
export PATH="$HOST/bin:$PATH"
cmake -S "$S" -B "$B" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PREFIX_ROOT/usr/lib/cmake/Qt6/qt.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
    -DQT_HOST_PATH="$H" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF \
    -DWaylandScanner_EXECUTABLE="$HOST/bin/wayland-scanner" \
    -DFEATURE_wayland_client=ON -DFEATURE_wayland_server=OFF \
    -DFEATURE_wayland_egl=OFF -DFEATURE_wayland_decoration_adwaita=OFF \
    > "$B.log" 2>&1 || { tail -60 "$B.log"; exit 1; }
cmake --build "$B" -j"${JOBS:-2}" >> "$B.log" 2>&1 || { tail -50 "$B.log"; exit 1; }
DESTDIR="$PREFIX_ROOT" cmake --install "$B" >> "$B.log" 2>&1 || { tail -40 "$B.log"; exit 1; }
# Qt's configure can otherwise succeed while silently skipping this whole repo.
test -f "$PREFIX_ROOT/usr/plugins/platforms/libqwayland-generic.so"
test -f "$PREFIX_ROOT/usr/lib/libQt6WaylandClient.so"
"$UL/musl-dcxx" -std=c++17 -O2 -fPIC -fexceptions -frtti \
    -I"$PREFIX_ROOT/usr/include" -I"$PREFIX_ROOT/usr/include/QtCore" \
    -I"$PREFIX_ROOT/usr/include/QtGui" \
    -I"$PREFIX_ROOT/usr/include/QtGui/$V" -I"$PREFIX_ROOT/usr/include/QtGui/$V/QtGui" \
    "$UL/ports/src/qtwaylandtest.cc" -L"$PREFIX_ROOT/usr/lib" \
    -lQt6Gui -lQt6Core -lwayland-client -o "$PREFIX_ROOT/usr/bin/qtwaylandtest"
install -m755 "$UL/ports/scripts/qtwaylandsmoke" "$PREFIX_ROOT/usr/bin/qtwaylandsmoke"
echo "Qt $V Wayland raster/shm client installed (no EGL/Quick/compositor)"
