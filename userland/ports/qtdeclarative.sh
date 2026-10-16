#!/bin/sh
# M40 source QML/Quick candidate: native generators + signed build-host qsb.
. "$(dirname "$0")/common.sh"
V=6.8.3
T=$DL/qtdeclarative-everywhere-src-$V.tar.xz
"$UL/fetch.sh" "$T" \
    "https://distfiles.alpinelinux.org/distfiles/v3.22/qtdeclarative-everywhere-src-$V.tar.xz" \
    "https://download.qt.io/archive/qt/6.8/$V/submodules/qtdeclarative-everywhere-src-$V.tar.xz"
# Independently pinned against Alpine 3.22 qt6-qtdeclarative/APKBUILD.
printf '%s  %s\n' 75c9fdc8ba26ef95ae8ff0c82b18589b74d7ff3065e86e5bd8427bc36336e569eaa22fa07863f99d0592eec0c1a69c3409c7b3941eae14361ff523c058e56084 "$T" | sha512sum -c -
S=$PORTS_SRC/qtdeclarative-everywhere-src-$V
[ -d "$S" ] || tar xJf "$T" -C "$PORTS_SRC"
# Keep real native generators mandatory, but do not import optional GUI host
# tools (including qmltestrunner) absent from headless native Qt. qmlscene stays enabled.
python3 "$UL/ports/qtdeclarative-cross-tools.py" "$S"
H=${HOST_TOOLS:-${TOOLS_DIR:-/data/tools}/host}/qt-$V
HB=$UL/build/qtdeclarative-host-$V
# Build real upstream native QML tools; never run a target tool in a cross build.
# qml_python means the required BUILD-HOST interpreter, not a target dependency.
# ShaderTools discovery is explicitly disabled so a cached host qsb cannot
# silently turn this headless prerequisite into an unvalidated Qt Quick port.
if [ ! -x "$H/libexec/qmltyperegistrar" ] || [ ! -x "$H/libexec/qmlcachegen" ] || [ ! -f "$H/lib/cmake/Qt6QmlTools/Qt6QmlToolsConfig.cmake" ]; then
    env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR -u PKG_CONFIG_PATH \
    cmake -S "$S" -B "$HB" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$H" -DCMAKE_INSTALL_PREFIX="$H" \
        -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DCMAKE_DISABLE_FIND_PACKAGE_Qt6ShaderToolsTools=ON \
        -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF \
        -DFEATURE_qml_jit=OFF -DFEATURE_qml_network=OFF -DFEATURE_qml_debug=OFF \
        -DFEATURE_qml_python=ON -DFEATURE_qml_xmllistmodel=OFF \
        > "$HB.log" 2>&1 || { tail -60 "$HB.log"; exit 1; }
    cmake --build "$HB" -j"${JOBS:-2}" >> "$HB.log" 2>&1 || { tail -50 "$HB.log"; exit 1; }
    cmake --install "$HB" >> "$HB.log" 2>&1 || { tail -40 "$HB.log"; exit 1; }
fi
# qsb runs on the BUILD HOST only. Its isolated signed Qt libraries are never
# added to the target or source-native Qt search prefixes.
QSB_CMAKE=$(python3 "$UL/ports/host-qsb.py")
B=$PORTS_BUILD/qtdeclarative
cmake -S "$S" -B "$B" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PREFIX_ROOT/usr/lib/cmake/Qt6/qt.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DCMAKE_DISABLE_FIND_PACKAGE_Qt6ShaderToolsTools=OFF \
    -DQt6ShaderToolsTools_DIR="$QSB_CMAKE" \
    -DQT_HOST_PATH="$H" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF \
    -DFEATURE_qml_animation=ON -DFEATURE_qml_jit=OFF -DFEATURE_qml_network=OFF -DFEATURE_qml_debug=OFF \
    -DFEATURE_qml_python=ON -DFEATURE_qml_xmllistmodel=OFF \
    > "$B.log" 2>&1 || { tail -60 "$B.log"; exit 1; }
# This must be the portable interpreter on all targets, including RISC-V.
grep -qx 'QT_FEATURE_qml_jit:INTERNAL=OFF' "$B/CMakeCache.txt"
cmake --build "$B" -j"${JOBS:-2}" >> "$B.log" 2>&1 || { tail -60 "$B.log"; exit 1; }
DESTDIR="$PREFIX_ROOT" cmake --install "$B" >> "$B.log" 2>&1 || { tail -40 "$B.log"; exit 1; }
test -f "$PREFIX_ROOT/usr/lib/libQt6Qml.so"
test -f "$PREFIX_ROOT/usr/lib/libQt6Quick.so"
test -f "$PREFIX_ROOT/usr/qml/QtQuick/qmldir"
test -x "$PREFIX_ROOT/usr/bin/qmlscene"
test -f "$PREFIX_ROOT/usr/qml/QtQml/qmldir"
test -f "$PREFIX_ROOT/usr/qml/QtQml/Models/qmldir"
test -f "$PREFIX_ROOT/usr/qml/QtQml/WorkerScript/qmldir"
"$H/libexec/moc" "$UL/ports/src/qmltest.cc" -o "$PORTS_BUILD/qmltest.moc"
"$UL/musl-dcxx" -std=c++17 -O2 -fPIC -fexceptions -frtti \
    -I"$PREFIX_ROOT/usr/include" -I"$PREFIX_ROOT/usr/include/QtCore" \
    -I"$PREFIX_ROOT/usr/include/QtQml" -I"$PREFIX_ROOT/usr/include/QtQmlIntegration" \
    -I"$PORTS_BUILD" "$UL/ports/src/qmltest.cc" -L"$PREFIX_ROOT/usr/lib" \
    -lQt6Qml -lQt6Core -o "$PREFIX_ROOT/usr/bin/qmltest"
install -m755 "$UL/ports/scripts/qmlsmoke" "$PREFIX_ROOT/usr/bin/qmlsmoke"
mkdir -p "$PREFIX_ROOT/usr/qml/9os-tests"
install -m644 "$UL/ports/src/qtquick.qml" "$PREFIX_ROOT/usr/qml/9os-tests/qtquick.qml"
install -m755 "$UL/ports/scripts/qtquicksmoke" "$PREFIX_ROOT/usr/bin/qtquicksmoke"
echo "Qt $V source QML/Quick candidate installed (interpreter, no JS JIT/network/KDE claim)"
