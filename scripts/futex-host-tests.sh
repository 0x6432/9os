#!/bin/sh
# Run the exact guest WAKE_OP regression against the host Linux ABI as an oracle.
set -eu
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT HUP INT TERM
${CC:-cc} -O2 -pthread userland/tests/futexoptest.c -o "$b/futexoptest"
"$b/futexoptest"
