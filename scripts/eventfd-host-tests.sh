#!/bin/sh
# Exact guest consumer on the Linux oracle, then production-function queue model.
set -eu
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT HUP INT TERM
${CC:-cc} -O2 -Wall -Wextra -Werror -pthread userland/tests/eventfdwaketest.c -o "$b/eventfdwaketest"
"$b/eventfdwaketest"
python3 tests/eventfd-host/wake-model.py
python3 tests/eventfd-host/install-model.py
