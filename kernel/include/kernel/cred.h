#pragma once
/*
 * Credentials (M31). A struct cred is immutable once published and reference counted.
 * Every thread has a subjective cred (current->cred: what it acts as; may be temporarily
 * overridden, e.g. by access(2) or kernel-internal node creation), the process keeps the
 * objective cred last committed by one of its threads (p->cred: what others see in kill(2),
 * /proc, SO_PEERCRED). Changing credentials = cred_prepare() a copy, edit it, cred_commit().
 * POSIX's process-wide set*id comes from musl's __synccall running the syscall in every thread.
 */
#include <kernel/types.h>

#define NGROUPS_MAX 64

/* Linux capability numbers (the subset 9os checks) */
enum {
    CAP_CHOWN = 0, CAP_DAC_OVERRIDE = 1, CAP_DAC_READ_SEARCH = 2, CAP_FOWNER = 3, CAP_FSETID = 4,
    CAP_KILL = 5, CAP_SETGID = 6, CAP_SETUID = 7, CAP_SETPCAP = 8, CAP_NET_BIND_SERVICE = 10,
    CAP_NET_ADMIN = 12, CAP_NET_RAW = 13, CAP_IPC_LOCK = 14, CAP_IPC_OWNER = 15, CAP_SYS_CHROOT = 18,
    CAP_SYS_PTRACE = 19, CAP_SYS_ADMIN = 21, CAP_SYS_BOOT = 22, CAP_SYS_NICE = 23, CAP_SYS_RESOURCE = 24,
    CAP_SYS_TIME = 25, CAP_SYS_TTY_CONFIG = 26, CAP_MKNOD = 27, CAP_LEASE = 28, CAP_LAST_CAP = 40,
};
#define CAP_FULL ((1ull << (CAP_LAST_CAP + 1)) - 1)
#define CAP_BIT(c) (1ull << (c))
#define CAP_FS_MASK (CAP_BIT(CAP_CHOWN) | CAP_BIT(CAP_DAC_OVERRIDE) | CAP_BIT(CAP_DAC_READ_SEARCH) | \
                     CAP_BIT(CAP_FOWNER) | CAP_BIT(CAP_FSETID) | CAP_BIT(CAP_MKNOD))

/* prctl securebits subset */
#define SECBIT_KEEP_CAPS 0x10

struct cred {
    int refcount;
    uint32_t uid, euid, suid, fsuid;
    uint32_t gid, egid, sgid, fsgid;
    int ngroups;
    uint32_t groups[NGROUPS_MAX];        /* sorted */
    uint64_t cap_inh, cap_perm, cap_eff, cap_bset, cap_amb;
    uint32_t securebits;
};

extern struct cred init_cred;           /* root, all capabilities: init and kernel threads */
struct thread;
struct process;

struct cred *cred_get(struct cred *c);
void cred_put(struct cred *c);
struct cred *cred_prepare(void);        /* private copy of current's cred (refcount 1) */
void cred_commit(struct cred *c);       /* install as current's subjective + process cred; consumes c */
void cred_abort(struct cred *c);
struct cred *cred_override(struct cred *c);   /* temporarily act as c (borrowed); returns the old */
void cred_revert(struct cred *old);
struct cred *proc_cred(struct process *p);    /* objective cred of p (reference) */
void cred_thread_init(struct thread *t, struct thread *parent);   /* parent may be null: kernel thread */
void cred_thread_free(struct thread *t);

const struct cred *current_cred(void);
bool capable(int cap);
bool cred_capable(const struct cred *c, int cap);
bool in_group(const struct cred *c, uint32_t gid);    /* fsgid or a supplementary group */
/* uid changes (setuid family): Linux cap_task_fix_setuid rules */
void cred_fix_caps_after_setuid(struct cred *n, const struct cred *o);
void cred_proc_status(struct process *p, char *buf, size_t max);   /* /proc/pid/status lines */
