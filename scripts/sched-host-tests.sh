#!/bin/sh
# Fast deterministic tests of the actual scheduler queues, without QEMU or userland.
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-clang}
mkdir -p build/sched-host
for policy in RR MLFQ; do
    "$CC" -std=c2x -O1 -g -ffunction-sections -fdata-sections \
        -fsanitize=undefined -fsanitize-trap=all -fno-omit-frame-pointer \
        -Wall -Wextra -Wno-unused-parameter -DCONFIG_SCHED_$policy=1 \
        -Itests/sched-host -Ikernel/include -Ikernel/arch/x86_64/include -Ithird_party/limine \
        tests/sched-host/queues.c -Wl,--gc-sections -o "build/sched-host/$policy"
    "build/sched-host/$policy"
done