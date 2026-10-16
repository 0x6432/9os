#!/bin/sh
# Run the exact guest procfs identity/zombie contract against real host Linux.
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-cc} -O2 -Wall -Wextra userland/tests/procinfotest.c -o "$D/test"
"$D/test"
