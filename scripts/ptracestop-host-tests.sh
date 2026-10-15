#!/bin/sh
# Run the SAME portable stop/SEIZE regression against host Linux.
set -eu
cd "$(dirname "$0")/.."
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT HUP INT TERM
${CC:-cc} -O2 -Wall -Wextra userland/tests/ptracestoptest.c -o "$d/ptracestoptest"
"$d/ptracestoptest"
