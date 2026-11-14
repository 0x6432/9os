#!/bin/sh
# Run the exact guest WAKE_OP/bitset/requeue regressions against the Linux ABI oracle.
set -eu
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT HUP INT TERM
for test in futexoptest futexbitsettest futexrequeuetest; do
    ${CC:-cc} -O2 -pthread "userland/tests/$test.c" -o "$b/$test"
    "$b/$test"
done

python3 tests/futex-requeue-host/list-model.py
