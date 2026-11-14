#!/bin/sh
# Actual production functions; mock locks/user copies, not hardware/SMP proof.
set -eu
cd "$(dirname "$0")/.."
python3 tests/evdev-host/frame-model.py
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
# Both minimal-UAPI and system-UAPI consumers must compile cleanly on the host.
${CC:-cc} -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror userland/tests/evdevframetest.c -o "$D/minimal"
${CC:-cc} -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror -DEVDEV_USE_SYSTEM_UAPI userland/tests/evdevframetest.c -o "$D/system"
echo 'evdev consumer: minimal/system Linux UAPI builds passed (execution uses Linux VM)'
