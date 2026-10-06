#pragma once
/*
 * M27 hardening: the user-access gate (SMAP / PAN / SUM), exception-table fixups for the user
 * copy routine, stack-protector guard, ASLR and W^X policy (mm/uaccess.c).
 */
#include <kernel/types.h>
#include <kernel/mm.h>

struct trap_frame;

/* the whole range lies in the user half (syscall-entry check for buffers that file ops,
 * which also serve in-kernel callers with kernel buffers, will touch) */
static inline bool access_ok(const void *p, size_t n) {
    uintptr_t a = (uintptr_t)p;
    return a + n >= a && a + n <= USER_TOP;
}

/* arch asm: copies n bytes, returns the number NOT copied (a fault is fixed up via __ex_table) */
size_t __copy_user(void *dst, const void *src, size_t n);

/* Open/close the user-access window (nests per thread; restored across context switches).
 * Every kernel dereference of a user pointer must sit inside one. */
void user_access_begin(void);
void user_access_end(void);
void uaccess_restore_after_switch(void);

/* exception table: redirect *pc to its fixup if it is a registered faulting instruction */
bool extable_fixup(uintptr_t *pc);
/* Kernel-mode fault triage, called before the generic fault paths: catches kernel execution
 * of user pages (SMEP/PXN) and user accesses outside the uaccess window (SMAP/PAN/SUM).
 * Returns true when the fault was fixed up through the exception table. */
bool kernel_fault_check(struct trap_frame *f, vaddr_t addr, bool exec);
/* last resort for an unresolvable kernel-mode fault: exception-table fixup */
bool kernel_fault_fixup(struct trap_frame *f);

struct harden_stats {
    uint64_t extable_fixups, uaccess_violations, wx_mappings, wx_denied, kernel_wx_pages;
};
extern struct harden_stats harden_stats;
extern int randomize_va_space;      /* 0 off, 2 mmap/stack/PIE/brk (cmdline norandmaps) */
extern int wx_policy;               /* 0 allow, 1 warn+count (default), 2 deny (cmdline wx=strict) */

void harden_init(void);             /* boot CPU, after vmm_init: cmdline, guard, W^X scan */
void arch_harden_cpu(void);         /* per CPU: SMEP/SMAP, PAN, SUM */
const char *arch_harden_features(void);
uint64_t aslr_offset(unsigned bits);   /* random page-aligned offset < 2^bits pages, 0 if disabled */
int wx_check(unsigned prot);        /* user mapping prot check: 0 or -EACCES */
void stack_guard_init(void);
