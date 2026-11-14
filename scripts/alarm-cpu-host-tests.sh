#!/bin/sh
# Exact busy-user signal consumer on Linux; not guest timer proof.
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-cc} -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror userland/tests/alarmcputest.c -o "$D/test"
timeout 10 "$D/test"
