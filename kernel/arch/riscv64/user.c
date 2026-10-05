/* riscv64-specific system calls. */
#include <kernel/types.h>
#include <kernel/errno.h>

/* riscv_flush_icache(2): make stores visible to instruction fetch (used by __clear_cache, JITs,
 * libffi closures). Local fence.i; other harts execute fence.i when they next enter user mode
 * through a context switch (see the remote TLB/fence path), which is enough under TCG/QEMU. */
int64_t sys_riscv_flush_icache(uint64_t start, uint64_t end, uint64_t flags) {
    if (flags & ~1ull) return -EINVAL;
    __asm__ volatile("fence.i" ::: "memory");
    return 0;
}
