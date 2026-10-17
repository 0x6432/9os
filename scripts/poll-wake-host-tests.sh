#!/bin/sh
# Run the exact small guest consumer against Linux; timings are informational.
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-cc} -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror -pthread userland/tests/pollwakebench.c -o "$D/test"
"$D/test"
"$D/test" --inotify
"$D/test" --unix
"$D/test" --unix-inotify
for arg in --bad --unix-extra ''; do
    set +e
    "$D/test" "$arg" > "$D/invalid.out" 2>&1
    rc=$?
    set -e
    test "$rc" = 2
    ! grep -q '^pollwakebench: transfers=' "$D/invalid.out"
done
set +e
"$D/test" --unix --inotify > "$D/invalid.out" 2>&1
rc=$?
set -e
test "$rc" = 2
echo 'poll-wake-host: PASS four exact transport modes and four invalid CLI controls; no timing thresholds'
