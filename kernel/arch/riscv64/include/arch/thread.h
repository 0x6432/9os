#pragma once
#include <kernel/types.h>
struct arch_thread {
    uint64_t sp;               /* saved kernel stack pointer */
    uint64_t fpu[33];          /* f0-f31 + fcsr */
};
