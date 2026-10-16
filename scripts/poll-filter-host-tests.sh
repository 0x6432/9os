#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT HUP INT TERM
${CC:-cc} -O2 -Wall -Wextra -Werror -pthread userland/tests/pollfiltertest.c -o "$b/pollfiltertest"
"$b/pollfiltertest"
python3 tests/poll-filter-host/model.py
