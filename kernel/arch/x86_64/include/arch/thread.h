#pragma once
#include <kernel/types.h>

struct arch_thread {
    uint64_t rsp;              /* saved kernel stack pointer */
    uint64_t fs_base;
    uint8_t fpu[512] __attribute__((aligned(16)));
};
