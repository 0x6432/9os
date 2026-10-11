#!/bin/sh
# inputtest: xkbcommon/xkeyboard-config, uinput+libevdev, mtdev, libinput (udev + path), libseat
. "$(dirname "$0")/common.sh"
$DCC -O2 -Wall -o "$PREFIX_ROOT/usr/bin/inputtest" "$(dirname "$0")/src/inputtest.c" -I"$PREFIX_ROOT/usr/include" \
    -I"$PREFIX_ROOT/usr/include/libevdev-1.0" -L"$PREFIX_ROOT/usr/lib" -Wl,-rpath-link,"$PREFIX_ROOT/usr/lib" \
    -linput -levdev -lmtdev -lxkbcommon -lseat -ludev
echo "inputtest built"
