#!/bin/sh
# Same real pthread/procfs contract as the guest, not a mocked proc tree.
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-cc} -O2 -Wall -Wextra -pthread userland/tests/proctasktest.c -o "$D/test"
"$D/test"
