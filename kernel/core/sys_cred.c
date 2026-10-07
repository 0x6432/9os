/*
 * User/group identity and capability syscalls (M31). Each call changes the calling thread's
 * credentials (Linux semantics); musl applies set*id to every thread of the process.
 */
#include <kernel/cred.h>
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/syscall.h>
#include <kernel/errno.h>
#include <kernel/string.h>
#include <kernel/kmalloc.h>

#define NOCHG ((uint32_t)-1)

int64_t sys_getuid(void) { return current_cred()->uid; }
int64_t sys_geteuid(void) { return current_cred()->euid; }
int64_t sys_getgid(void) { return current_cred()->gid; }
int64_t sys_getegid(void) { return current_cred()->egid; }

int64_t sys_getresuid(uint32_t *r, uint32_t *e, uint32_t *s) {
    const struct cred *c = current_cred();
    if (copy_to_user(r, &c->uid, 4) || copy_to_user(e, &c->euid, 4) || copy_to_user(s, &c->suid, 4)) return -EFAULT;
    return 0;
}
int64_t sys_getresgid(uint32_t *r, uint32_t *e, uint32_t *s) {
    const struct cred *c = current_cred();
    if (copy_to_user(r, &c->gid, 4) || copy_to_user(e, &c->egid, 4) || copy_to_user(s, &c->sgid, 4)) return -EFAULT;
    return 0;
}

static bool uid_is_one_of(uint32_t u, const struct cred *c) { return u == c->uid || u == c->euid || u == c->suid; }
static bool gid_is_one_of(uint32_t g, const struct cred *c) { return g == c->gid || g == c->egid || g == c->sgid; }

/* setresuid: without CAP_SETUID each new id must be one of the current real/effective/saved */
static int64_t do_setresuid(uint32_t r, uint32_t e, uint32_t s, bool fs_follow) {
    const struct cred *o = current_cred();
    if (!cred_capable(o, CAP_SETUID) &&
        ((r != NOCHG && !uid_is_one_of(r, o)) || (e != NOCHG && !uid_is_one_of(e, o)) || (s != NOCHG && !uid_is_one_of(s, o))))
        return -EPERM;
    struct cred *n = cred_prepare();
    if (!n) return -ENOMEM;
    if (r != NOCHG) n->uid = r;
    if (e != NOCHG) n->euid = e;
    if (s != NOCHG) n->suid = s;
    if (fs_follow) n->fsuid = n->euid;
    cred_fix_caps_after_setuid(n, o);
    cred_commit(n);
    return 0;
}
static int64_t do_setresgid(uint32_t r, uint32_t e, uint32_t s) {
    const struct cred *o = current_cred();
    if (!cred_capable(o, CAP_SETGID) &&
        ((r != NOCHG && !gid_is_one_of(r, o)) || (e != NOCHG && !gid_is_one_of(e, o)) || (s != NOCHG && !gid_is_one_of(s, o))))
        return -EPERM;
    struct cred *n = cred_prepare();
    if (!n) return -ENOMEM;
    if (r != NOCHG) n->gid = r;
    if (e != NOCHG) n->egid = e;
    if (s != NOCHG) n->sgid = s;
    n->fsgid = n->egid;
    cred_commit(n);
    return 0;
}

/* ids as 64-bit: zero- or sign-extended -1 must both mean "unchanged" (riscv64 ABI) */
int64_t sys_setresuid(uint64_t r, uint64_t e, uint64_t s) { return do_setresuid((uint32_t)r, (uint32_t)e, (uint32_t)s, true); }
int64_t sys_setresgid(uint64_t r, uint64_t e, uint64_t s) { return do_setresgid((uint32_t)r, (uint32_t)e, (uint32_t)s); }

/* setuid: privileged sets all three; otherwise only the effective id (to real or saved) */
int64_t sys_setuid(uint64_t u_) {
    uint32_t u = (uint32_t)u_;
    const struct cred *o = current_cred();
    if (u == NOCHG) return -EINVAL;
    if (cred_capable(o, CAP_SETUID)) return do_setresuid(u, u, u, true);
    if (u != o->uid && u != o->suid) return -EPERM;
    return do_setresuid(NOCHG, u, NOCHG, true);
}
int64_t sys_setgid(uint64_t g_) {
    uint32_t g = (uint32_t)g_;
    const struct cred *o = current_cred();
    if (g == NOCHG) return -EINVAL;
    if (cred_capable(o, CAP_SETGID)) return do_setresgid(g, g, g);
    if (g != o->gid && g != o->sgid) return -EPERM;
    return do_setresgid(NOCHG, g, NOCHG);
}

/* setreuid: real may become effective or real; effective any of real/effective/saved.
 * The saved id follows the new effective one if the real id was set or euid != old ruid. */
int64_t sys_setreuid(uint64_t r_, uint64_t e_) {
    uint32_t r = (uint32_t)r_, e = (uint32_t)e_;
    const struct cred *o = current_cred();
    bool cap = cred_capable(o, CAP_SETUID);
    if (r != NOCHG && !cap && r != o->uid && r != o->euid) return -EPERM;
    if (e != NOCHG && !cap && !uid_is_one_of(e, o)) return -EPERM;
    uint32_t ne = e != NOCHG ? e : o->euid;
    uint32_t s = (r != NOCHG || (e != NOCHG && e != o->uid)) ? ne : NOCHG;
    struct cred *n = cred_prepare();
    if (!n) return -ENOMEM;
    if (r != NOCHG) n->uid = r;
    n->euid = ne;
    if (s != NOCHG) n->suid = s;
    n->fsuid = n->euid;
    cred_fix_caps_after_setuid(n, o);
    cred_commit(n);
    return 0;
}
int64_t sys_setregid(uint64_t r_, uint64_t e_) {
    uint32_t r = (uint32_t)r_, e = (uint32_t)e_;
    const struct cred *o = current_cred();
    bool cap = cred_capable(o, CAP_SETGID);
    if (r != NOCHG && !cap && r != o->gid && r != o->egid) return -EPERM;
    if (e != NOCHG && !cap && !gid_is_one_of(e, o)) return -EPERM;
    uint32_t ne = e != NOCHG ? e : o->egid;
    struct cred *n = cred_prepare();
    if (!n) return -ENOMEM;
    if (r != NOCHG) n->gid = r;
    n->egid = ne;
    if (r != NOCHG || (e != NOCHG && e != o->gid)) n->sgid = ne;
    n->fsgid = n->egid;
    cred_commit(n);
    return 0;
}

/* setfsuid returns the previous fsuid whether or not it changed */
int64_t sys_setfsuid(uint64_t u_) {
    uint32_t u = (uint32_t)u_;
    const struct cred *o = current_cred();
    uint32_t old = o->fsuid;
    if (u == NOCHG || u == old) return old;
    if (!cred_capable(o, CAP_SETUID) && !uid_is_one_of(u, o)) return old;
    struct cred *n = cred_prepare();
    if (!n) return old;
    n->fsuid = u;
    cred_fix_caps_after_setuid(n, o);
    cred_commit(n);
    return old;
}
int64_t sys_setfsgid(uint64_t g_) {
    uint32_t g = (uint32_t)g_;
    const struct cred *o = current_cred();
    uint32_t old = o->fsgid;
    if (g == NOCHG || g == old) return old;
    if (!cred_capable(o, CAP_SETGID) && !gid_is_one_of(g, o)) return old;
    struct cred *n = cred_prepare();
    if (!n) return old;
    n->fsgid = g;
    cred_commit(n);
    return old;
}

int64_t sys_getgroups(int size, uint32_t *list) {
    const struct cred *c = current_cred();
    if (size < 0) return -EINVAL;
    if (!size) return c->ngroups;
    if (size < c->ngroups) return -EINVAL;
    if (c->ngroups && copy_to_user(list, c->groups, c->ngroups * 4u)) return -EFAULT;
    return c->ngroups;
}

int64_t sys_setgroups(int size, const uint32_t *list) {
    if (!capable(CAP_SETGID)) return -EPERM;
    if (size < 0 || size > NGROUPS_MAX) return -EINVAL;
    uint32_t g[NGROUPS_MAX];
    if (size && copy_from_user(g, list, size * 4u)) return -EFAULT;
    for (int i = 1; i < size; i++)              /* insertion sort, drop duplicates below */
        for (int j = i; j > 0 && g[j - 1] > g[j]; j--) { uint32_t t = g[j]; g[j] = g[j - 1]; g[j - 1] = t; }
    struct cred *n = cred_prepare();
    if (!n) return -ENOMEM;
    n->ngroups = 0;
    for (int i = 0; i < size; i++) if (!n->ngroups || n->groups[n->ngroups - 1] != g[i]) n->groups[n->ngroups++] = g[i];
    cred_commit(n);
    return 0;
}

/* capget/capset: _LINUX_CAPABILITY_VERSION_1 (one u32 set) and _2/_3 (two) */
struct cap_header { uint32_t version; int pid; };
struct cap_data { uint32_t effective, permitted, inheritable; };
#define CAP_V1 0x19980330
#define CAP_V2 0x20071026
#define CAP_V3 0x20080522

static int cap_words(struct cap_header *h, const struct cap_header *uh) {
    if (copy_from_user(h, uh, sizeof *h)) return -EFAULT;
    if (h->version == CAP_V1) return 1;
    if (h->version == CAP_V2 || h->version == CAP_V3) return 2;
    h->version = CAP_V3;
    copy_to_user((void *)uh, h, sizeof *h);
    return -EINVAL;
}

int64_t sys_capget(struct cap_header *uh, struct cap_data *ud) {
    struct cap_header h;
    int w = cap_words(&h, uh);
    if (w < 0) return ud ? w : (w == -EINVAL ? 0 : w);   /* version probe with null data */
    struct cred *c;
    if (h.pid && h.pid != curproc->pid) {
        struct process *p = process_find(h.pid);
        if (!p) return -ESRCH;
        c = proc_cred(p);
    } else c = cred_get((struct cred *)current_cred());
    struct cap_data d[2];
    for (int i = 0; i < 2; i++) {
        d[i].effective = (uint32_t)(c->cap_eff >> (32 * i));
        d[i].permitted = (uint32_t)(c->cap_perm >> (32 * i));
        d[i].inheritable = (uint32_t)(c->cap_inh >> (32 * i));
    }
    cred_put(c);
    if (!ud) return 0;
    return copy_to_user(ud, d, w * sizeof d[0]);
}

/* only for the caller: permitted may only shrink, effective ⊆ permitted, inheritable needs
 * CAP_SETPCAP to grow beyond inheritable ∪ permitted (and never beyond the bounding set) */
int64_t sys_capset(struct cap_header *uh, const struct cap_data *ud) {
    struct cap_header h;
    int w = cap_words(&h, uh);
    if (w < 0) return w;
    if (h.pid && h.pid != curproc->pid && h.pid != current->tid) return -EPERM;
    struct cap_data d[2] = {0};
    if (copy_from_user(d, ud, w * sizeof d[0])) return -EFAULT;
    uint64_t eff = d[0].effective | (uint64_t)d[1].effective << 32;
    uint64_t perm = d[0].permitted | (uint64_t)d[1].permitted << 32;
    uint64_t inh = d[0].inheritable | (uint64_t)d[1].inheritable << 32;
    if (w == 1) {   /* v1 cannot express the high word: keep it */
        const struct cred *o = current_cred();
        eff |= o->cap_eff & ~0xffffffffull; perm |= o->cap_perm & ~0xffffffffull; inh |= o->cap_inh & ~0xffffffffull;
    }
    eff &= CAP_FULL; perm &= CAP_FULL; inh &= CAP_FULL;
    const struct cred *o = current_cred();
    if (perm & ~o->cap_perm) return -EPERM;
    if (eff & ~perm) return -EPERM;
    if (inh & ~(o->cap_inh | o->cap_perm) && !cred_capable(o, CAP_SETPCAP)) return -EPERM;
    if (inh & ~(o->cap_inh | o->cap_bset)) return -EPERM;
    struct cred *n = cred_prepare();
    if (!n) return -ENOMEM;
    n->cap_eff = eff; n->cap_perm = perm; n->cap_inh = inh;
    n->cap_amb &= perm & inh;
    cred_commit(n);
    return 0;
}

/* prctl pieces that concern credentials (called from sys_prctl) */
int64_t cred_prctl(int opt, uint64_t a2) {
    const struct cred *o = current_cred();
    switch (opt) {
    case 7: return !!(o->securebits & SECBIT_KEEP_CAPS);              /* PR_GET_KEEPCAPS */
    case 8: {                                                         /* PR_SET_KEEPCAPS */
        if (a2 > 1) return -EINVAL;
        struct cred *n = cred_prepare();
        if (!n) return -ENOMEM;
        if (a2) n->securebits |= SECBIT_KEEP_CAPS; else n->securebits &= ~SECBIT_KEEP_CAPS;
        cred_commit(n);
        return 0;
    }
    case 23: return a2 > CAP_LAST_CAP ? -EINVAL : (int64_t)((o->cap_bset >> a2) & 1);   /* PR_CAPBSET_READ */
    case 24: {                                                        /* PR_CAPBSET_DROP */
        if (a2 > CAP_LAST_CAP) return -EINVAL;
        if (!cred_capable(o, CAP_SETPCAP)) return -EPERM;
        struct cred *n = cred_prepare();
        if (!n) return -ENOMEM;
        n->cap_bset &= ~CAP_BIT(a2);
        cred_commit(n);
        return 0;
    }
    case 27: return o->securebits;                                    /* PR_GET_SECUREBITS */
    case 47: return a2 == 2 /* IS_SET */ ? 0 : a2 == 4 /* CLEAR_ALL */ ? 0 : -EINVAL;   /* PR_CAP_AMBIENT */
    }
    return -ENOSYS;
}

/* setting the clock is privileged; 9os keeps no adjustable wall-clock offset, so a permitted
 * call is accepted and ignored (as before M31) */
int64_t sys_clock_settime(int clk, const void *ts) {
    (void)clk; (void)ts;
    return capable(CAP_SYS_TIME) ? 0 : -EPERM;
}
