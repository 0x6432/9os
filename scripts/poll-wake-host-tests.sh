#!/bin/sh
# Run the exact small guest consumer against Linux; timings are informational.
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-cc} -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror -pthread userland/tests/pollwakebench.c -o "$D/test"
"$D/test"
"$D/test" --inotify
