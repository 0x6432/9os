#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
${CC:-cc} -O2 -Wall -Wextra userland/tests/mountinfotest.c -o "$D/test"
"$D/test"
# Opt in where isolated user/mount namespaces are permitted. No host mounts
# escape the private namespace; an unavailable prerequisite must fail explicitly.
if [ "${MOUNTINFO_HOST_MUTATE:-0}" = 1 ];then
    unshare --user --map-root-user --mount sh -c 'mount --make-rprivate / && exec "$1" --mutate' sh "$D/test"
fi
