/* Credentials and capabilities (M31): see kernel/cred.h. */
#include <kernel/cred.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>
#include <kernel/printk.h>

struct cred init_cred = {
    .refcount = 1 << 30,
    .cap_perm = CAP_FULL, .cap_eff = CAP_FULL, .cap_bset = CAP_FULL,
};

/* protects p->cred replacement against foreign readers taking a reference (leaf lock) */
static const struct lock_class cred_class = { "cred", LR_CRED, false };
static spinlock_t cred_lock = SPINLOCK_INIT_CLASS(&cred_class);

struct cred *cred_get(struct cred *c) {
    __atomic_add_fetch(&c->refcount, 1, __ATOMIC_RELAXED);
    return c;
}

void cred_put(struct cred *c) {
    if (c && __atomic_sub_fetch(&c->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        if (c == &init_cred) panic("cred: init_cred freed");
        kfree(c);
    }
}

const struct cred *current_cred(void) {
    struct thread *t = current;
    return t && t->cred ? t->cred : &init_cred;
}

struct cred *cred_prepare(void) {
    struct cred *n = kmalloc(sizeof *n);
    if (!n) return nullptr;
    memcpy(n, current_cred(), sizeof *n);
    n->refcount = 1;
    return n;
}

void cred_abort(struct cred *c) { kfree(c); }

void cred_commit(struct cred *n) {
    struct thread *t = current;
    struct cred *old = t->cred, *oldp = nullptr;
    t->cred = n;
    if (t->proc) {
        cred_get(n);
        uint64_t f = spin_lock_irqsave(&cred_lock);
        oldp = t->proc->cred;
        t->proc->cred = n;
        spin_unlock_irqrestore(&cred_lock, f);
    }
    cred_put(old);
    cred_put(oldp);
}

struct cred *cred_override(struct cred *c) {
    struct thread *t = current;
    if (!t) return nullptr;
    struct cred *old = t->cred;
    t->cred = cred_get(c);
    return old;
}

void cred_revert(struct cred *old) {
    struct thread *t = current;
    if (!t) return;
    cred_put(t->cred);
    t->cred = old;
}

struct cred *proc_cred(struct process *p) {
    uint64_t f = spin_lock_irqsave(&cred_lock);
    struct cred *c = cred_get(p->cred ? p->cred : &init_cred);
    spin_unlock_irqrestore(&cred_lock, f);
    return c;
}

void cred_thread_init(struct thread *t, struct thread *parent) {
    t->cred = cred_get(parent && parent->cred ? parent->cred : &init_cred);
}

void cred_thread_free(struct thread *t) {
    cred_put(t->cred);
    t->cred = nullptr;
}

bool cred_capable(const struct cred *c, int cap) { return (c->cap_eff >> cap) & 1; }
bool capable(int cap) { return cred_capable(current_cred(), cap); }

bool in_group(const struct cred *c, uint32_t gid) {
    if (gid == c->fsgid) return true;
    int lo = 0, hi = c->ngroups - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (c->groups[m] == gid) return true;
        if (c->groups[m] < gid) lo = m + 1; else hi = m - 1;
    }
    return false;
}

/*
 * Linux's "root is special" capability rules on uid changes:
 * - had a 0 among real/effective/saved and has none now: lose permitted+effective (unless
 *   SECBIT_KEEP_CAPS, which keeps permitted), and ambient;
 * - euid 0 -> nonzero: effective cleared; nonzero -> 0: effective = permitted;
 * - fsuid 0 -> nonzero: drop the filesystem capabilities from effective, and back.
 */
void cred_fix_caps_after_setuid(struct cred *n, const struct cred *o) {
    bool had_root = !o->uid || !o->euid || !o->suid, has_root = !n->uid || !n->euid || !n->suid;
    if (had_root && !has_root) {
        if (!(n->securebits & SECBIT_KEEP_CAPS)) n->cap_perm = 0;
        n->cap_eff = 0;
        n->cap_amb = 0;
    }
    if (!o->euid && n->euid) n->cap_eff = 0;
    if (o->euid && !n->euid) n->cap_eff = n->cap_perm;
    if (!o->fsuid && n->fsuid) n->cap_eff &= ~CAP_FS_MASK;
    if (o->fsuid && !n->fsuid) n->cap_eff |= n->cap_perm & CAP_FS_MASK;
}

void cred_proc_status(struct process *p, char *buf, size_t max) {
    struct cred *c = proc_cred(p);
    size_t n = (size_t)snprintf(buf, max, "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nGroups:\t",
                                c->uid, c->euid, c->suid, c->fsuid, c->gid, c->egid, c->sgid, c->fsgid);
    for (int i = 0; i < c->ngroups && n < max; i++) n += (size_t)snprintf(buf + n, max - n, "%u ", c->groups[i]);
    if (n < max)
        snprintf(buf + n, max - n, "\nCapInh:\t%016lx\nCapPrm:\t%016lx\nCapEff:\t%016lx\nCapBnd:\t%016lx\nCapAmb:\t%016lx\n",
                 (unsigned long)c->cap_inh, (unsigned long)c->cap_perm, (unsigned long)c->cap_eff,
                 (unsigned long)c->cap_bset, (unsigned long)c->cap_amb);
    cred_put(c);
}
