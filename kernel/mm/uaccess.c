/*
 * M27 hardening, architecture-independent parts:
 *  - the user-access gate: user_access_begin/end open the SMAP/PAN/SUM window per thread
 *    (nesting count in struct thread, re-applied after every context switch);
 *  - the exception table built from __ex_table entries in the arch __copy_user routines,
 *    and kernel-mode fault triage (SMEP/PXN execution, accesses outside the window);
 *  - the stack-protector guard (-fstack-protector-strong) and __stack_chk_fail;
 *  - ASLR offsets (cmdline norandmaps disables) and the user W^X policy (wx=strict|off);
 *  - a boot-time audit that no kernel page is both writable and executable.
 */
#include <kernel/uaccess.h>
#include <kernel/sched.h>
#include <kernel/mm.h>
#include <kernel/process.h>
#include <kernel/vmm.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <arch/uaccess.h>

struct harden_stats harden_stats;
int randomize_va_space = 2;
int wx_policy = 1;

void user_access_begin(void) {
    struct thread *t = current;
    if (!t) { arch_uaccess_set(true); return; }
    if (t->uaccess++ == 0) arch_uaccess_set(true);
}

void user_access_end(void) {
    struct thread *t = current;
    if (!t) { arch_uaccess_set(false); return; }
    if (--t->uaccess == 0) arch_uaccess_set(false);
}

/* runs on the incoming thread right after a context switch (sched_finish_switch) */
void uaccess_restore_after_switch(void) {
    struct thread *t = current;
    arch_uaccess_set(t && t->uaccess > 0);
}

struct extable_entry { uintptr_t insn, fixup; };
extern const struct extable_entry __start___ex_table[], __stop___ex_table[];

bool extable_fixup(uintptr_t *pc) {
    for (const struct extable_entry *e = __start___ex_table; e < __stop___ex_table; e++)
        if (e->insn == *pc) {
            *pc = e->fixup;
            __atomic_fetch_add(&harden_stats.extable_fixups, 1, __ATOMIC_RELAXED);
            return true;
        }
    return false;
}

void dump_frame(struct trap_frame *f);

bool kernel_fault_check(struct trap_frame *f, vaddr_t addr, bool exec) {
    if (trap_from_user(f) || addr >= USER_TOP) return false;
    if (exec) {
        printk("\nkernel attempted to execute user address %lx\n", addr);
        dump_frame(f);
        panic("kernel exec of a user page (SMEP/PXN)");
    }
    if (arch_frame_uaccess(f)) return false;    /* inside the window: normal demand paging */
    __atomic_fetch_add(&harden_stats.uaccess_violations, 1, __ATOMIC_RELAXED);
    if (extable_fixup(arch_frame_pc(f))) return true;
    printk("\nkernel access to user address %lx outside user_access_begin/end (%s)\n", addr, ARCH_UACCESS_NAME);
    dump_frame(f);
    panic("%s violation", ARCH_UACCESS_NAME);
}

bool kernel_fault_fixup(struct trap_frame *f) {
    return !trap_from_user(f) && extable_fixup(arch_frame_pc(f));
}

/* ---- stack protector ---- */
uintptr_t __stack_chk_guard = 0x595e9fbd94fda766ULL;

__noreturn void __stack_chk_fail(void) {
    panic("stack protector: canary corrupted in function returning to %p", __builtin_return_address(0));
}

/* Called first thing in kmain (which never returns, so no live frame checks the old value). */
__attribute__((no_stack_protector)) void stack_guard_init(void) {
    uint64_t g = arch_entropy();
    g ^= g >> 31; g *= 0xbf58476d1ce4e5b9ULL; g ^= g >> 29;
    g &= ~0xffULL;              /* a NUL low byte stops string-copy overflows from forging it */
    __stack_chk_guard = g ? g : 0x595e9fbd94fda700ULL;
}

/* ---- ASLR / W^X ---- */
uint64_t random_u64(void);

uint64_t aslr_offset(unsigned bits) {
    if (!randomize_va_space) return 0;
    return (random_u64() & ((1ULL << bits) - 1)) * PAGE_SIZE;
}

int wx_check(unsigned prot) {
    if ((prot & (VM_WRITE | VM_EXEC)) != (VM_WRITE | VM_EXEC) || !wx_policy) return 0;
    if (wx_policy == 2) {
        __atomic_fetch_add(&harden_stats.wx_denied, 1, __ATOMIC_RELAXED);
        return -EACCES;
    }
    if (__atomic_fetch_add(&harden_stats.wx_mappings, 1, __ATOMIC_RELAXED) < 4 && current && current->proc)
        pr_info("harden: %s (pid %d) created a writable+executable mapping\n", current->proc->name, current->proc->pid);
    return 0;
}

static bool cmdline_word(const char *w) {
    size_t n = strlen(w);
    for (const char *p = boot_cmdline(); (p = strstr(p, w)); p += n)
        if ((p == boot_cmdline() || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
    return false;
}

uint64_t arch_kernel_wx_pages(void);
void rng_mix(uint64_t v);

void harden_init(void) {
    rng_mix(arch_entropy());
    if (cmdline_word("norandmaps")) randomize_va_space = 0;
    if (cmdline_word("wx=strict")) wx_policy = 2;
    else if (cmdline_word("wx=off")) wx_policy = 0;
    arch_harden_cpu();
    harden_stats.kernel_wx_pages = arch_kernel_wx_pages();
    pr_info("harden: %s, stack protector, ASLR %s, user W^X %s, kernel W+X pages %lu\n",
            arch_harden_features(), randomize_va_space ? "on" : "off",
            wx_policy == 2 ? "strict" : wx_policy ? "warn" : "off", harden_stats.kernel_wx_pages);
    if (harden_stats.kernel_wx_pages) panic("harden: kernel mappings violate W^X");
}
