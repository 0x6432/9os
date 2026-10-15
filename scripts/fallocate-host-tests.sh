#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-cc} -O2 -Wall -Wextra userland/tests/fallocatetest.c -o "$D/test"
"$D/test"
