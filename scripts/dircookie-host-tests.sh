#!/bin/sh
# Same directory-resume consumer as the guest, against real Linux.
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-cc} -O2 -Wall -Wextra -Werror userland/tests/dircookietest.c -o "$D/test"
"$D/test" /proc/self /proc/self/fd /proc/self/task /sys/class
