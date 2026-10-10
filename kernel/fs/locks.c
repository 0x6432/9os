/* Advisory file locks (M33): flock(2), POSIX record locks (fcntl F_GETLK/F_SETLK/F_SETLKW) and
 * open file description locks (F_OFD_*).
 *
 * Locks live in a small hash table keyed by inode. flock locks belong to an open file and
 * never conflict with record locks; POSIX locks belong to a process (released when it closes
 * any descriptor of the file, and at exit), OFD locks to an open file (released with its last
 * reference). POSIX and OFD record locks conflict with each other. Ranges are inclusive
 * [start, end], end = INT64_MAX means "to EOF and beyond". Blocking requests sleep on one wait
 * queue and re-check after every unlock (generation counter, lost-wakeup free); F_SETLKW
 * detects deadlocks between POSIX owners (EDEADLK).
 *
 * Leases (F_SETLEASE/F_GETLEASE) belong to an open file of a regular file the caller owns (or
 * CAP_LEASE). A read lease needs no other writers, a write lease no other open files. An open
 * for writing or truncate(2) breaks every lease, a read-only open breaks write leases to read:
 * the holder gets SIGIO (or its F_SETSIG signal) and has /proc/sys/fs/lease-break-time seconds
 * to downgrade or release; the opener waits that long (O_NONBLOCK: EWOULDBLOCK at once), then
 * the lease is broken by force. Mandatory locks are not supported (removed from Linux 5.15). */
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <kernel/cpu.h>
#include <kernel/signal.h>
#include <kernel/cred.h>
#define F_GETLEASE 1025

#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#define OFF_MAX INT64_MAX

enum { FL_FLOCK = 1, FL_POSIX, FL_OFD, FL_LEASE };

struct flock_k {
    struct list_node node;
    struct inode *inode;
    int kind, type;
    int64_t start, end;
    void *owner;                 /* FL_POSIX: struct process *, else struct file * */
    int pid;
    int brk;                     /* FL_LEASE: type being broken to, -1 if not breaking */
    uint64_t deadline;           /* FL_LEASE: forced break at this time_ns() */
};
struct uflock { int16_t type, whence; int64_t start, len; int32_t pid; };

static const struct lock_class locks_class = { "file_locks", LR_FD + 1, false };
static spinlock_t locks_lock = SPINLOCK_INIT_CLASS(&locks_class);
static struct wait_queue locks_wq = WAIT_QUEUE_INIT(locks_wq);
static volatile uint64_t locks_gen;
#define NBUCKETS 64
static struct list_node buckets[NBUCKETS];
static bool buckets_ready;
static uint64_t nlocks, nleases;
int sysctl_lease_break_time = 45;

static struct list_node *bucket(struct inode *i) {
    if (!buckets_ready) { for (int b = 0; b < NBUCKETS; b++) list_init(&buckets[b]); buckets_ready = true; }
    return &buckets[((uintptr_t)i >> 6) % NBUCKETS];
}
#define for_each_lock(l, ino, tmp) \
    list_for_each_safe(__it, tmp, bucket(ino)) \
        for (struct flock_k *l = list_entry(__it, struct flock_k, node); l && l->inode == (ino); l = nullptr)

static void lock_free(struct flock_k *l) { if (l->kind == FL_LEASE) nleases--; list_del(&l->node); nlocks--; kfree(l); }

static bool record(int kind) { return kind == FL_POSIX || kind == FL_OFD; }
static bool overlap(const struct flock_k *a, int64_t s, int64_t e) { return a->start <= e && s <= a->end; }

/* first lock of another owner that conflicts with (kind, type, range) */
static struct flock_k *conflict(struct inode *ino, int kind, void *owner, int type, int64_t s, int64_t e) {
    for_each_lock(l, ino, tmp) {
        if (l->owner == owner && l->kind == kind) continue;
        if (kind == FL_FLOCK ? l->kind != FL_FLOCK : !record(l->kind)) continue;
        if (kind != FL_FLOCK && !overlap(l, s, e)) continue;
        if (l->type == F_WRLCK || type == F_WRLCK) return l;
    }
    return nullptr;
}

/* POSIX deadlock detection: does the owner of `blocker` (transitively) wait for `me`? */
struct waiter { struct list_node node; struct process *who; struct inode *ino; int type; int64_t s, e; };
static struct list_node waiters = LIST_INIT(waiters);
static bool deadlock(struct process *me, struct flock_k *blocker) {
    for (int depth = 0; blocker && depth < 16; depth++) {
        if (blocker->kind != FL_POSIX) return false;
        struct process *o = blocker->owner;
        if (o == me) return true;
        struct flock_k *next = nullptr;
        list_for_each(it, &waiters) {
            struct waiter *w = list_entry(it, struct waiter, node);
            if (w->who != o) continue;
            next = conflict(w->ino, FL_POSIX, o, w->type, w->s, w->e);
            break;
        }
        blocker = next;
    }
    return false;
}

/* apply (type or F_UNLCK) for owner on [s, e]; *spare: preallocated nodes (consumed) */
static void apply_record(struct inode *ino, int kind, void *owner, int pid, int type, int64_t s, int64_t e,
                         struct flock_k **spare) {
    for_each_lock(l, ino, tmp) {
        if (l->kind != kind || l->owner != owner) continue;
        bool adj = (e != OFF_MAX && l->start == e + 1) || (s > 0 && l->end == s - 1);
        if (!overlap(l, s, e) && !adj) continue;
        if (l->type == type) {                              /* merge */
            if (l->start < s) s = l->start;
            if (l->end > e) e = l->end;
            lock_free(l);
            continue;
        }
        if (!overlap(l, s, e)) continue;
        if (l->start < s && l->end > e) {                   /* split: keep both sides */
            struct flock_k *r = *spare; *spare = nullptr;
            if (!r) r = spare[1], spare[1] = nullptr;
            *r = *l;
            r->start = e + 1;
            list_add_tail(bucket(ino), &r->node); nlocks++;
            l->end = s - 1;
        } else if (l->start < s) l->end = s - 1;
        else if (l->end > e) l->start = e + 1;
        else lock_free(l);
    }
    if (type == F_UNLCK) return;
    struct flock_k *n = spare[0] ? spare[0] : spare[1];
    if (spare[0]) spare[0] = nullptr; else spare[1] = nullptr;
    n->inode = ino; n->kind = kind; n->type = type; n->start = s; n->end = e; n->owner = owner; n->pid = pid;
    list_add_tail(bucket(ino), &n->node); nlocks++;
}

static void wake_lockers(void) {
    __atomic_add_fetch(&locks_gen, 1, __ATOMIC_RELEASE);
    wake_up(&locks_wq);
}

/* sleep until the lock table changed after generation g; -EINTR on a signal */
static int wait_change(uint64_t g) {
    uint64_t f = sched_wait_lock();
    if (__atomic_load_n(&locks_gen, __ATOMIC_ACQUIRE) != g) { sched_wait_unlock(f); return 0; }
    return wait_event_locked(&locks_wq, f);
}

/* generic set: kind/owner/type/range; wait = block on conflicts */
static int set_lock(struct inode *ino, int kind, void *owner, int pid, int type, int64_t s, int64_t e, bool wait) {
    struct flock_k *spare[2] = { kzalloc(sizeof(struct flock_k)), kzalloc(sizeof(struct flock_k)) };
    if (!spare[0] || !spare[1]) { kfree(spare[0]); kfree(spare[1]); return -ENOLCK; }
    struct waiter w = { .who = owner, .ino = ino, .type = type, .s = s, .e = e };
    bool queued = false;
    int r = 0;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&locks_lock);
        uint64_t g = locks_gen;
        struct flock_k *c = type == F_UNLCK ? nullptr : conflict(ino, kind, owner, type, s, e);
        if (!c) {
            if (kind == FL_FLOCK) {                          /* convert/replace this file's flock */
                for_each_lock(l, ino, tmp) if (l->kind == FL_FLOCK && l->owner == owner) lock_free(l);
                if (type != F_UNLCK) {
                    struct flock_k *n = spare[0]; spare[0] = nullptr;
                    n->inode = ino; n->kind = kind; n->type = type; n->start = 0; n->end = OFF_MAX; n->owner = owner; n->pid = pid;
                    list_add_tail(bucket(ino), &n->node); nlocks++;
                }
            } else apply_record(ino, kind, owner, pid, type, s, e, spare);
            if (queued) list_del(&w.node);
            spin_unlock_irqrestore(&locks_lock, fl);
            wake_lockers();
            break;
        }
        if (!wait) { r = kind == FL_FLOCK ? -EWOULDBLOCK : -EAGAIN; }
        else if (kind == FL_POSIX && deadlock(owner, c)) r = -EDEADLK;
        if (r) { if (queued) list_del(&w.node); spin_unlock_irqrestore(&locks_lock, fl); break; }
        if (kind == FL_POSIX && !queued) { list_add_tail(&waiters, &w.node); queued = true; }
        spin_unlock_irqrestore(&locks_lock, fl);
        if ((r = wait_change(g))) {
            fl = spin_lock_irqsave(&locks_lock);
            if (queued) list_del(&w.node);
            spin_unlock_irqrestore(&locks_lock, fl);
            break;
        }
    }
    kfree(spare[0]); kfree(spare[1]);
    return r;
}

/* ---- release hooks ---- */

/* process p closes a descriptor of f: drop all of p's POSIX locks on the inode */
void locks_close_posix(struct process *p, struct file *f) {
    if (!f || !f->inode || !nlocks) return;
    bool any = false;
    uint64_t fl = spin_lock_irqsave(&locks_lock);
    for_each_lock(l, f->inode, tmp) if (l->kind == FL_POSIX && l->owner == p) { lock_free(l); any = true; }
    spin_unlock_irqrestore(&locks_lock, fl);
    if (any) wake_lockers();
}

/* last reference of open file f: drop its flock and OFD locks */
void locks_release_file(struct file *f) {
    if (!f->inode || !nlocks) return;
    bool any = false;
    uint64_t fl = spin_lock_irqsave(&locks_lock);
    for_each_lock(l, f->inode, tmp) if (l->kind != FL_POSIX && l->owner == f) { lock_free(l); any = true; }
    spin_unlock_irqrestore(&locks_lock, fl);
    if (any) wake_lockers();
}

/* ---- syscalls ---- */

int64_t sys_flock(int fd, int op) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    int type;
    switch (op & ~LOCK_NB) {
    case LOCK_SH: type = F_RDLCK; break;
    case LOCK_EX: type = F_WRLCK; break;
    case LOCK_UN: type = F_UNLCK; break;
    default: return -EINVAL;
    }
    return set_lock(f->inode, FL_FLOCK, f, curproc->pid, type, 0, OFF_MAX, !(op & LOCK_NB));
}

static int range(struct file *f, const struct uflock *u, int64_t *s, int64_t *e) {
    int64_t base;
    switch (u->whence) {
    case 0: base = 0; break;
    case 1: base = f->pos; break;
    case 2: base = (int64_t)f->inode->size; break;
    default: return -EINVAL;
    }
    int64_t start = base + u->start;
    if (start < 0) return -EINVAL;
    if (u->len > 0) {
        if (u->len - 1 > OFF_MAX - start) return -EOVERFLOW;
        *s = start; *e = start + u->len - 1;
    } else if (u->len == 0) {
        *s = start; *e = OFF_MAX;
    } else {
        if (start + u->len < 0) return -EINVAL;
        *s = start + u->len; *e = start - 1;
    }
    return 0;
}

/* fcntl lock commands (sys_fs.c) */
int64_t fcntl_lock(struct file *f, int cmd, void *uarg) {
    struct uflock u;
    if (copy_from_user(&u, uarg, sizeof u)) return -EFAULT;
    bool ofd = cmd >= F_OFD_GETLK;
    int kind = ofd ? FL_OFD : FL_POSIX;
    void *owner = ofd ? (void *)f : (void *)curproc;
    int64_t s, e;
    int r = range(f, &u, &s, &e);
    if (r) return r;
    if (u.type != F_RDLCK && u.type != F_WRLCK && u.type != F_UNLCK) return -EINVAL;
    if (ofd && u.pid != 0) return -EINVAL;
    if (cmd == F_GETLK || cmd == F_OFD_GETLK) {
        if (u.type == F_UNLCK) return -EINVAL;
        uint64_t fl = spin_lock_irqsave(&locks_lock);
        struct flock_k *c = conflict(f->inode, kind, owner, u.type, s, e);
        if (c) {
            u.type = (int16_t)c->type; u.whence = 0; u.start = c->start;
            u.len = c->end == OFF_MAX ? 0 : c->end - c->start + 1;
            u.pid = c->kind == FL_OFD ? -1 : c->pid;
        } else u.type = F_UNLCK;
        spin_unlock_irqrestore(&locks_lock, fl);
        return copy_to_user(uarg, &u, sizeof u) ? -EFAULT : 0;
    }
    uint32_t acc = f->flags & O_ACCMODE;
    if (u.type == F_RDLCK && acc == O_WRONLY) return -EBADF;
    if (u.type == F_WRLCK && acc == O_RDONLY) return -EBADF;
    return set_lock(f->inode, kind, owner, curproc->pid, u.type, s, e, cmd == F_SETLKW || cmd == F_OFD_SETLKW);
}

/* /proc/locks */
int locks_report(char *buf, int cap) {
    int n = 0, i = 1;
    uint64_t fl = spin_lock_irqsave(&locks_lock);
    for (int b = 0; buckets_ready && b < NBUCKETS && n < cap - 96; b++)
        list_for_each(it, &buckets[b]) {
            struct flock_k *l = list_entry(it, struct flock_k, node);
            if (n >= cap - 96) break;
            n += snprintf(buf + n, cap - n, "%d: %s %s  %s %d %02x:%02x:%lu %ld %s\n", i++,
                          l->kind == FL_FLOCK ? "FLOCK " : l->kind == FL_OFD ? "OFDLCK" : l->kind == FL_LEASE ? "LEASE " : "POSIX ",
                          l->kind != FL_LEASE ? "ADVISORY" : l->brk >= 0 ? "BREAKING" : "ACTIVE  ",
                          l->type == F_WRLCK ? "WRITE" : "READ ", l->kind == FL_OFD ? -1 : l->pid,
                          (unsigned)(l->inode->dev >> 8) & 0xff, (unsigned)l->inode->dev & 0xff,
                          (unsigned long)l->inode->ino, (long)l->start, l->end == OFF_MAX ? "EOF" : "");
            if (l->end != OFF_MAX) { n--; n += snprintf(buf + n, cap - n, "%ld\n", (long)l->end); }
        }
    spin_unlock_irqrestore(&locks_lock, fl);
    return n;
}

/* ---- leases ---- */
static struct flock_k *lease_of(struct inode *ino, struct file *f) {
    for_each_lock(l, ino, tmp) if (l->kind == FL_LEASE && l->owner == f) return l;
    return nullptr;
}

int64_t fcntl_lease(struct file *f, int fd, int cmd, int arg) {
    struct inode *ino = f->inode;
    if (cmd == F_GETLEASE) {
        uint64_t fl = spin_lock_irqsave(&locks_lock);
        struct flock_k *l = lease_of(ino, f);
        int r = !l ? F_UNLCK : l->brk >= 0 ? l->brk : l->type;
        spin_unlock_irqrestore(&locks_lock, fl);
        return r;
    }
    if (!S_ISREG(ino->mode) || (f->flags & O_PATH)) return -EINVAL;
    if (arg != F_RDLCK && arg != F_WRLCK && arg != F_UNLCK) return -EINVAL;
    if (current_cred()->fsuid != ino->uid && !capable(CAP_LEASE)) return -EACCES;
    struct flock_k *n = kzalloc(sizeof *n);
    if (!n) return -ENOLCK;
    int r = 0;
    bool self_w = (f->flags & O_ACCMODE) != O_RDONLY;
    uint64_t fl = spin_lock_irqsave(&locks_lock);
    struct flock_k *mine = lease_of(ino, f);
    if (arg == F_UNLCK) {
        if (mine) lock_free(mine); else r = -EAGAIN;
        goto out;
    }
    if (arg == F_RDLCK && __atomic_load_n(&ino->i_nwrite, __ATOMIC_RELAXED) > (self_w ? 1 : 0)) { r = -EAGAIN; goto out; }
    if (arg == F_WRLCK && __atomic_load_n(&ino->i_nopen, __ATOMIC_RELAXED) > 1) { r = -EAGAIN; goto out; }
    for_each_lock(l, ino, tmp)
        if (l->kind == FL_LEASE && l != mine && (l->brk >= 0 || arg == F_WRLCK || l->type == F_WRLCK)) { r = -EAGAIN; goto out; }
    if (mine) {
        if (mine->brk >= 0 && (arg == F_WRLCK || mine->brk == F_UNLCK)) { r = -EAGAIN; goto out; }
        mine->type = arg;
        mine->brk = -1;
        goto out;
    }
    n->inode = ino; n->kind = FL_LEASE; n->type = arg; n->start = 0; n->end = OFF_MAX;
    n->owner = f; n->pid = curproc->pid; n->brk = -1;
    list_add_tail(bucket(ino), &n->node); nlocks++; nleases++;
    n = nullptr;
out:
    spin_unlock_irqrestore(&locks_lock, fl);
    kfree(n);
    if (!r) wake_lockers();
    if (!r && arg != F_UNLCK) { fasync_setown(f, 1, curproc->pid); f->async_fd = fd; }   /* as Linux */
    return r;
}

/* an open (flags) or truncate of ino: break conflicting leases, wait for their holders */
int lease_break(struct inode *ino, int flags) {
    if (!__atomic_load_n(&nleases, __ATOMIC_RELAXED)) return 0;
    bool wr = (flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC);
    int target = wr ? F_UNLCK : F_RDLCK;
    for (;;) {
        struct file *sg[8];
        int ns = 0;
        bool any = false, changed = false;
        uint64_t now = time_ns(), dl = UINT64_MAX;
        uint64_t fl = spin_lock_irqsave(&locks_lock);
        uint64_t g = locks_gen;
        for_each_lock(l, ino, tmp) {
            if (l->kind != FL_LEASE || (!wr && l->type != F_WRLCK)) continue;
            if (l->brk < 0 || (target == F_UNLCK && l->brk == F_RDLCK)) {
                if (l->brk < 0) l->deadline = now + (uint64_t)sysctl_lease_break_time * 1000000000ull;
                l->brk = target;
                if (ns < 8 && file_get_live(l->owner)) sg[ns++] = l->owner;
            }
            if (now >= l->deadline) {                    /* the holder ran out of time */
                if (l->brk == F_UNLCK) lock_free(l); else { l->type = F_RDLCK; l->brk = -1; }
                changed = true;
                continue;
            }
            any = true;
            if (l->deadline < dl) dl = l->deadline;
        }
        spin_unlock_irqrestore(&locks_lock, fl);
        if (changed) wake_lockers();
        if (ns) {
            bool took = !bkl_held();
            if (took) bkl_enter();
            for (int k = 0; k < ns; k++) fasync_signal(sg[k], 3, 0x441);   /* POLL_MSG, POLLIN|RDNORM|MSG */
            if (took) bkl_exit();
            for (int k = 0; k < ns; k++) vfs_close(sg[k]);
        }
        if (!any) return 0;
        if (flags & O_NONBLOCK) return -EWOULDBLOCK;
        uint64_t sf = sched_wait_lock();
        if (__atomic_load_n(&locks_gen, __ATOMIC_ACQUIRE) != g) { sched_wait_unlock(sf); continue; }
        now = time_ns();
        int r = wait_event_timeout_locked(&locks_wq, dl > now ? dl - now : 0, sf);
        if (r == -EINTR) return -EINTR;
    }
}
