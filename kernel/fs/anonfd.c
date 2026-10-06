#include <kernel/mutex.h>
/*
 * Anonymous-inode file descriptors used by event loops (libwayland, wlroots, glib, systemd-ish
 * code): eventfd, timerfd, signalfd, epoll and memfd.
 *
 * Readiness is reported through file_ops.poll; waiters sleep on the global poll_wq (woken by
 * poll_notify()) like poll/select. epoll keeps no reference on registered files: an item is
 * only live while the registering fd still refers to the same struct file in the caller's fd
 * table, which gives the "closed fds disappear from the set" behaviour without UAF.
 */
#include <kernel/vfs.h>
#include <kernel/syscall.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/time.h>
#include <kernel/mm.h>
#include <kernel/arch.h>

static int anon_fd(const struct file_ops *ops, void *priv, int flags, unsigned mode) {
    struct inode *i = inode_alloc(mode);
    if (!i) return -ENOMEM;
    i->fops = ops;
    struct file *f = file_open_inode(i, O_RDWR | (flags & O_NONBLOCK));
    iput(i);
    if (!f) return -ENOMEM;
    f->priv = priv;
    int fd = fd_alloc(f, 0, flags & O_CLOEXEC);
    if (fd < 0) vfs_close(f);
    return fd;
}

/* wait for cond with poll_wq; honours O_NONBLOCK and signals */
#define WAIT_READY(f, cond) ({ int __r = 0; \
    while (!(cond)) { \
        if ((f)->flags & O_NONBLOCK) { __r = -EAGAIN; break; } \
        uint64_t __fl = arch_irq_save(); \
        if (!(cond)) __r = wait_event(&poll_wq); \
        arch_irq_restore(__fl); \
        if (__r) break; \
    } __r; })

/* ------------------------------------------------------------------ eventfd */
/* eventfd runs without the BKL (file_ops.nobkl): the counter is updated with compare-and-swap,
 * the 8-byte value is copied with copy_{to,from}_user (or memcpy for kernel buffers), and
 * blocking uses wait_until_sl() on poll_wq, whose condition is re-checked under sched_lock;
 * every counter change is followed by poll_notify(), so no wakeup can be lost. */
struct eventfd { uint64_t count; bool semaphore; };

static int efd_copy_out(void *dst, const void *src) {
    if ((vaddr_t)dst >= USER_TOP) { memcpy(dst, src, 8); return 0; }
    return copy_to_user(dst, src, 8) ? -EFAULT : 0;
}
static int efd_copy_in(void *dst, const void *src) {
    if ((vaddr_t)src >= USER_TOP) { memcpy(dst, src, 8); return 0; }
    return copy_from_user(dst, src, 8) ? -EFAULT : 0;
}
static uint64_t efd_count(struct eventfd *e) { return __atomic_load_n(&e->count, __ATOMIC_ACQUIRE); }

static ssize_t efd_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct eventfd *e = f->priv;
    if (n < 8) return -EINVAL;
    for (;;) {
        uint64_t c = efd_count(e);
        if (c) {
            uint64_t v = e->semaphore ? 1 : c;
            if (!__atomic_compare_exchange_n(&e->count, &c, c - v, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
            poll_notify();
            return efd_copy_out(buf, &v) ?: 8;
        }
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        int r = wait_until_sl(&poll_wq, efd_count(e) != 0);
        if (r) return r;
    }
}
static ssize_t efd_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct eventfd *e = f->priv;
    if (n < 8) return -EINVAL;
    uint64_t v;
    if (efd_copy_in(&v, buf)) return -EFAULT;
    if (v == UINT64_MAX) return -EINVAL;
    for (;;) {
        uint64_t c = efd_count(e);
        if (UINT64_MAX - 1 - c >= v) {
            if (!__atomic_compare_exchange_n(&e->count, &c, c + v, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
            if (v) poll_notify();
            return 8;
        }
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        int r = wait_until_sl(&poll_wq, UINT64_MAX - 1 - efd_count(e) >= v);
        if (r) return r;
    }
}
static unsigned efd_poll(struct file *f) {
    struct eventfd *e = f->priv;
    uint64_t c = efd_count(e);
    return (c ? POLLIN | POLLRDNORM : 0) | (c < UINT64_MAX - 1 ? POLLOUT | POLLWRNORM : 0);
}
static void priv_release(struct file *f) { kfree(f->priv); f->priv = nullptr; }
static const struct file_ops eventfd_fops = { .nobkl = true, .read = efd_read, .write = efd_write, .poll = efd_poll, .release = priv_release };

#define EFD_SEMAPHORE 1
int64_t sys_eventfd2(unsigned initval, int flags) {
    struct eventfd *e = kzalloc(sizeof *e);
    if (!e) return -ENOMEM;
    e->count = initval; e->semaphore = flags & EFD_SEMAPHORE;
    int fd = anon_fd(&eventfd_fops, e, flags, 0600);
    if (fd < 0) kfree(e);
    return fd;
}
int64_t sys_eventfd(unsigned initval) { return sys_eventfd2(initval, 0); }

/* ------------------------------------------------------------------ timerfd */
struct itimerspec_k { struct timespec interval, value; };
struct timerfd {
    struct list_node node;      /* armed list */
    int clock;
    bool armed;
    uint64_t expires, interval; /* monotonic ns */
    uint64_t ticks;
};
static struct list_node armed_timers = LIST_INIT(armed_timers);
static volatile uint64_t next_expiry = UINT64_MAX;

static void recompute_next(void) {
    uint64_t n = UINT64_MAX;
    list_for_each(it, &armed_timers) {
        struct timerfd *t = list_entry(it, struct timerfd, node);
        if (t->expires < n) n = t->expires;
    }
    __atomic_store_n(&next_expiry, n, __ATOMIC_RELEASE);
    sched_timer_changed();
}
uint64_t timerfd_next_deadline(void) { return __atomic_load_n(&next_expiry, __ATOMIC_ACQUIRE); }

/* called from the cpu0 timer tick */
void timerfd_tick(uint64_t now) {
    if (now < next_expiry) return;
    bool fired = false;
    list_for_each_safe(it, tmp, &armed_timers) {
        struct timerfd *t = list_entry(it, struct timerfd, node);
        if (now < t->expires) continue;
        uint64_t n = 1;
        if (t->interval) { n += (now - t->expires) / t->interval; t->expires += n * t->interval; }
        else { t->armed = false; list_del(&t->node); }
        t->ticks += n;
        fired = true;
    }
    recompute_next();
    if (fired) poll_notify();
}

static uint64_t ts_ns(const struct timespec *ts) { return ts->tv_sec * 1000000000ull + ts->tv_nsec; }
static struct timespec ns_ts(uint64_t ns) { return (struct timespec){ ns / 1000000000ull, ns % 1000000000ull }; }
static uint64_t clock_offset(int clock) { return clock == 0 ? (uint64_t)boot_epoch * 1000000000ull : 0; } /* REALTIME */

static ssize_t tfd_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct timerfd *t = f->priv;
    if (n < 8) return -EINVAL;
    int r = WAIT_READY(f, t->ticks > 0);
    if (r) return r;
    uint64_t f0 = arch_irq_save();
    uint64_t v = t->ticks; t->ticks = 0;
    arch_irq_restore(f0);
    memcpy(buf, &v, 8);
    return 8;
}
static unsigned tfd_poll(struct file *f) { return ((struct timerfd *)f->priv)->ticks ? POLLIN | POLLRDNORM : 0; }
static void tfd_release(struct file *f) {
    struct timerfd *t = f->priv;
    uint64_t fl = arch_irq_save();
    if (t->armed) list_del(&t->node);
    recompute_next();
    arch_irq_restore(fl);
    kfree(t);
}
static const struct file_ops timerfd_fops = { .read = tfd_read, .poll = tfd_poll, .release = tfd_release };

int64_t sys_timerfd_create(int clock, int flags) {
    if (clock != 0 && clock != 1 && clock != 7 && clock != 8 && clock != 9) return -EINVAL;
    struct timerfd *t = kzalloc(sizeof *t);
    if (!t) return -ENOMEM;
    t->clock = clock;
    int fd = anon_fd(&timerfd_fops, t, flags, 0600);
    if (fd < 0) kfree(t);
    return fd;
}

static struct timerfd *get_timerfd(int fd) {
    struct file *f = fd_get(fd);
    return f && f->fops == &timerfd_fops ? f->priv : nullptr;
}

static void timer_value(struct timerfd *t, struct itimerspec_k *out) {
    uint64_t now = time_ns();
    out->interval = ns_ts(t->interval);
    out->value = ns_ts(t->armed ? (t->expires > now ? t->expires - now : 1) : 0);
}

int64_t sys_timerfd_settime(int fd, int flags, const struct itimerspec_k *unew, struct itimerspec_k *uold) {
    struct timerfd *t = get_timerfd(fd);
    if (!t) return -EBADF;
    struct itimerspec_k nv;
    if (copy_from_user(&nv, unew, sizeof nv)) return -EFAULT;
    if (uold) { struct itimerspec_k o; timer_value(t, &o); if (copy_to_user(uold, &o, sizeof o)) return -EFAULT; }
    uint64_t fl = arch_irq_save();
    if (t->armed) { list_del(&t->node); t->armed = false; }
    t->ticks = 0;
    uint64_t v = ts_ns(&nv.value);
    if (v) {
        uint64_t now = time_ns();
        if (flags & 1) {                                  /* TFD_TIMER_ABSTIME */
            uint64_t off = clock_offset(t->clock);
            t->expires = v > off ? v - off : 0;
        } else t->expires = now + v;
        t->interval = ts_ns(&nv.interval);
        t->armed = true;
        list_add_tail(&armed_timers, &t->node);
    }
    recompute_next();
    arch_irq_restore(fl);
    return 0;
}
int64_t sys_timerfd_gettime(int fd, struct itimerspec_k *ucur) {
    struct timerfd *t = get_timerfd(fd);
    if (!t) return -EBADF;
    struct itimerspec_k o; timer_value(t, &o);
    return copy_to_user(ucur, &o, sizeof o) ? -EFAULT : 0;
}

/* ------------------------------------------------------------------ signalfd */
struct signalfd { uint64_t mask; };
struct signalfd_siginfo { uint32_t signo; int32_t err, code; uint32_t pid, uid; int32_t fd; uint32_t tid, band, overrun, trapno;
    int32_t status, int_; uint64_t ptr, utime, stime, addr; uint16_t addr_lsb; uint8_t pad[46]; };
_Static_assert(sizeof(struct signalfd_siginfo) == 128, "signalfd_siginfo ABI");

static uint64_t sfd_pending(struct signalfd *s) {
    return (current->sig_pending | (curproc ? curproc->sig_pending : 0)) & s->mask;
}
static ssize_t sfd_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct signalfd *s = f->priv;
    if (n < sizeof(struct signalfd_siginfo)) return -EINVAL;
    int r = WAIT_READY(f, sfd_pending(s));
    if (r) return r;
    size_t done = 0;
    while (done + sizeof(struct signalfd_siginfo) <= n) {
        uint64_t fl = arch_irq_save();
        uint64_t p = sfd_pending(s);
        if (!p) { arch_irq_restore(fl); break; }
        int sig = __builtin_ctzll(p) + 1;
        uint64_t bit = SIGBIT(sig);
        if (current->sig_pending & bit) current->sig_pending &= ~bit; else curproc->sig_pending &= ~bit;
        arch_irq_restore(fl);
        struct signalfd_siginfo si = { .signo = sig };
        memcpy((uint8_t *)buf + done, &si, sizeof si);
        done += sizeof si;
    }
    return done;
}
static unsigned sfd_poll(struct file *f) { return sfd_pending(f->priv) ? POLLIN | POLLRDNORM : 0; }
static const struct file_ops signalfd_fops = { .read = sfd_read, .poll = sfd_poll, .release = priv_release };

int64_t sys_signalfd4(int fd, const uint64_t *umask, size_t sz, int flags) {
    uint64_t mask;
    if (sz != 8) return -EINVAL;
    if (copy_from_user(&mask, umask, 8)) return -EFAULT;
    mask &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    if (fd != -1) {
        struct file *f = fd_get(fd);
        if (!f || f->fops != &signalfd_fops) return -EINVAL;
        ((struct signalfd *)f->priv)->mask = mask;
        return fd;
    }
    struct signalfd *s = kzalloc(sizeof *s);
    if (!s) return -ENOMEM;
    s->mask = mask;
    int r = anon_fd(&signalfd_fops, s, flags, 0600);
    if (r < 0) kfree(s);
    return r;
}
int64_t sys_signalfd(int fd, const uint64_t *umask, size_t sz) { return sys_signalfd4(fd, umask, sz, 0); }

/* ------------------------------------------------------------------ epoll */
#define EPOLLIN 0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLLRDHUP 0x2000
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP (1u << 29)
#define EPOLLONESHOT (1u << 30)
#define EPOLLET (1u << 31)

#ifdef __x86_64__
struct epoll_event_u { uint32_t events; uint64_t data; } __attribute__((packed));
#else
struct epoll_event_u { uint32_t events; uint64_t data; };
#endif

struct epitem { struct list_node node; int fd; struct file *f; uint32_t events; uint64_t data; bool disabled; unsigned last; };
/*
 * epoll without the BKL (M24): the item list and per-item edge state are protected by the
 * instance's sleeping mutex. Items hold no file reference; a scan pins each candidate with
 * fd_get_ref() and drops items whose fd no longer names the registered file. ->poll methods
 * that still need the BKL get it lazily for the rest of the scan (BKL inside a mutex is fine:
 * a mutex sleep drops the BKL). Nested epoll instances are locked outer -> inner; epoll_ctl
 * refuses loops (ELOOP) so that order is acyclic.
 */
struct epoll { struct list_node items; struct mutex mtx; };
static const struct lock_class epoll_class = { "epoll", LR_MUTEX_EPOLL, true };

static const struct file_ops epoll_fops;

/* scan state: whether this scan took the BKL for a non-nobkl ->poll */
struct ep_scan_ctx { bool took_bkl; int depth; };

static unsigned ep_file_poll(struct file *f, struct ep_scan_ctx *x);

/* Pin the item's file if its fd still refers to it; nullptr if the registration is stale. */
static struct file *item_get(struct epitem *it) {
    struct file *f = fd_get_ref(it->fd);
    if (f && f != it->f) { vfs_close(f); f = nullptr; }
    return f;
}

/* Collect ready events. Edge-triggered items are level-triggered for input (consumers drain
 * to EAGAIN anyway) but only report EPOLLOUT on a not-writable -> writable transition.
 * out == nullptr: peek only (no edge/oneshot state change), stop at the first ready item. */
static int ep_scan_locked(struct epoll *ep, struct epoll_event_u *out, int max, struct ep_scan_ctx *x) {
    int n = 0;
    list_for_each_safe(i, tmp, &ep->items) {
        struct epitem *it = list_entry(i, struct epitem, node);
        struct file *f = item_get(it);
        if (!f) { list_del(&it->node); kfree(it); continue; }
        if (it->disabled) { vfs_close(f); continue; }
        unsigned ready = ep_file_poll(f, x) & (it->events | EPOLLERR | EPOLLHUP);
        vfs_close(f);
        if (!out) {
            if (ready & ~EPOLLET) { n = 1; break; }
            continue;
        }
        if (it->events & EPOLLET) {
            unsigned prev = it->last;
            it->last = ready;
            if ((ready & EPOLLOUT) && (prev & EPOLLOUT)) ready &= ~EPOLLOUT;
        }
        if (!ready) continue;
        if (n >= max) break;
        out[n] = (struct epoll_event_u){ ready, it->data };
        if (it->events & EPOLLONESHOT) it->disabled = true;
        n++;
    }
    return n;
}

static unsigned ep_file_poll(struct file *f, struct ep_scan_ctx *x) {
    if (!f->fops || !f->fops->poll) return POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM;
    if (f->fops == &epoll_fops) {                   /* nested instance: peek, outer -> inner */
        struct epoll *in = f->priv;
        if (x->depth >= 4 || mutex_owned(&in->mtx)) return 0;
        x->depth++;
        mutex_lock(&in->mtx);
        int r = ep_scan_locked(in, nullptr, 0, x);
        mutex_unlock(&in->mtx);
        x->depth--;
        return r ? POLLIN | POLLRDNORM : 0;
    }
    if (!f->fops->nobkl && !x->took_bkl && !bkl_held()) { bkl_enter(); x->took_bkl = true; }
    return f->fops->poll(f);
}

static unsigned ep_poll(struct file *f) {
    struct epoll *ep = f->priv;
    if (mutex_owned(&ep->mtx)) return 0;
    struct ep_scan_ctx x = { false, 1 };
    mutex_lock(&ep->mtx);
    int n = ep_scan_locked(ep, nullptr, 0, &x);
    mutex_unlock(&ep->mtx);
    if (x.took_bkl) bkl_exit();
    return n ? POLLIN | POLLRDNORM : 0;
}
static void ep_release(struct file *f) {
    struct epoll *ep = f->priv;
    list_for_each_safe(i, tmp, &ep->items) { list_del(i); kfree(list_entry(i, struct epitem, node)); }
    kfree(ep);
}
static const struct file_ops epoll_fops = { .nobkl = true, .poll = ep_poll, .release = ep_release };

int64_t sys_epoll_create1(int flags) {
    if (flags & ~O_CLOEXEC) return -EINVAL;
    struct epoll *ep = kzalloc(sizeof *ep);
    if (!ep) return -ENOMEM;
    list_init(&ep->items);
    mutex_init(&ep->mtx, &epoll_class);
    int fd = anon_fd(&epoll_fops, ep, flags & O_CLOEXEC, 0600);
    if (fd < 0) kfree(ep);
    return fd;
}
int64_t sys_epoll_create(int size) { return size <= 0 ? -EINVAL : sys_epoll_create1(0); }

/* does epoll instance 'from' (transitively) watch instance 'target'? */
static bool ep_reaches(struct epoll *from, struct epoll *target, int depth) {
    if (from == target) return true;
    if (depth > 4) return true;                     /* too deep: treat as a loop */
    bool hit = false;
    mutex_lock(&from->mtx);
    list_for_each(i, &from->items) {
        struct epitem *it = list_entry(i, struct epitem, node);
        struct file *f = item_get(it);
        if (!f) continue;
        if (f->fops == &epoll_fops) hit = ep_reaches(f->priv, target, depth + 1);
        vfs_close(f);
        if (hit) break;
    }
    mutex_unlock(&from->mtx);
    return hit;
}

int64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event_u *uev) {
    struct epoll_event_u ev = {0};
    if (op != 2 && copy_from_user(&ev, uev, sizeof ev)) return -EFAULT;
    struct file *ef = fd_get_ref(epfd);
    if (!ef) return -EBADF;
    struct file *f = fd_get_ref(fd);
    int64_t r = 0;
    if (!f) { vfs_close(ef); return -EBADF; }
    if (ef->fops != &epoll_fops || f == ef) { r = -EINVAL; goto out; }
    if (f->fops && !f->fops->poll && f->inode && (S_ISREG(f->inode->mode) || S_ISDIR(f->inode->mode))) { r = -EPERM; goto out; }
    struct epoll *ep = ef->priv;
    if (op == 1 && f->fops == &epoll_fops && ep_reaches(f->priv, ep, 0)) { r = -ELOOP; goto out; }
    mutex_lock(&ep->mtx);
    struct epitem *found = nullptr;
    list_for_each_safe(i, tmp, &ep->items) {
        struct epitem *it = list_entry(i, struct epitem, node);
        if (it->fd == fd && it->f == f) { found = it; continue; }
        struct file *g = item_get(it);
        if (!g) { list_del(&it->node); kfree(it); continue; }
        vfs_close(g);
    }
    switch (op) {
    case 1:                                                   /* EPOLL_CTL_ADD */
        if (found) { r = -EEXIST; break; }
        found = kzalloc(sizeof *found);
        if (!found) { r = -ENOMEM; break; }
        found->fd = fd; found->f = f;
        found->events = ev.events; found->data = ev.data;
        list_add_tail(&ep->items, &found->node);
        break;
    case 2:                                                   /* EPOLL_CTL_DEL */
        if (!found) { r = -ENOENT; break; }
        list_del(&found->node); kfree(found);
        break;
    case 3:                                                   /* EPOLL_CTL_MOD */
        if (!found) { r = -ENOENT; break; }
        found->events = ev.events; found->data = ev.data; found->disabled = false; found->last = 0;
        break;
    default: r = -EINVAL;
    }
    mutex_unlock(&ep->mtx);
    if (!r) poll_notify();
out:
    vfs_close(f);
    vfs_close(ef);
    return r;
}

static void ep_set_mask(uint64_t m) {     /* signal_send() samples sig_mask under the BKL */
    bkl_enter();
    current->sig_mask = m;
    bkl_exit();
}

int64_t sys_epoll_pwait(int epfd, struct epoll_event_u *uev, int max, int timeout_ms, const uint64_t *usig, size_t sz) {
    if (max <= 0 || max > 4096) return -EINVAL;
    uint64_t m = 0;
    if (usig && copy_from_user(&m, usig, 8)) return -EFAULT;
    struct file *ef = fd_get_ref(epfd);
    if (!ef) return -EBADF;
    if (ef->fops != &epoll_fops) { vfs_close(ef); return -EINVAL; }
    struct epoll *ep = ef->priv;
    struct epoll_event_u *ev = kmalloc(sizeof *ev * max);
    if (!ev) { vfs_close(ef); return -ENOMEM; }
    uint64_t oldmask = current->sig_mask;
    if (usig) ep_set_mask(m & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP)));
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : time_ns() + (uint64_t)timeout_ms * 1000000ull;
    int64_t r;
    for (;;) {
        uint64_t pseq = poll_seq_read();
        struct ep_scan_ctx x = { false, 1 };
        mutex_lock(&ep->mtx);
        r = ep_scan_locked(ep, ev, max, &x);
        mutex_unlock(&ep->mtx);
        if (x.took_bkl) bkl_exit();
        if (r || timeout_ms == 0) break;
        uint64_t now = time_ns();
        if (now >= deadline) { r = 0; break; }
        int w = poll_wait_seq(pseq, deadline == UINT64_MAX ? UINT64_MAX : deadline - now);
        if (w == -EINTR) { r = -EINTR; break; }
    }
    if (r > 0 && copy_to_user(uev, ev, sizeof *ev * r)) r = -EFAULT;
    kfree(ev);
    vfs_close(ef);
    if (usig) {
        if (r == -EINTR) { current->saved_mask = oldmask; current->restore_mask = true; }
        else ep_set_mask(oldmask);
    }
    return r;
}
int64_t sys_epoll_wait(int epfd, struct epoll_event_u *uev, int max, int timeout_ms) {
    return sys_epoll_pwait(epfd, uev, max, timeout_ms, nullptr, 8);
}
int64_t sys_epoll_pwait2(int epfd, struct epoll_event_u *uev, int max, const struct timespec *uts, const uint64_t *usig, size_t sz) {
    int ms = -1;
    if (uts) { struct timespec ts; if (copy_from_user(&ts, uts, sizeof ts)) return -EFAULT; ms = ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000; }
    return sys_epoll_pwait(epfd, uev, max, ms, usig, sz);
}

/* ------------------------------------------------------------------ memfd */
struct inode *tmpfs_create_anon(uint32_t mode);
#define MFD_CLOEXEC 1
int64_t sys_memfd_create(const char *uname, unsigned flags) {
    char name[250];
    if (strncpy_from_user(name, uname, sizeof name) < 0) return -EFAULT;
    struct inode *i = tmpfs_create_anon(S_IFREG | 0777);
    if (!i) return -ENOMEM;
    struct file *f = file_open_inode(i, O_RDWR);
    iput(i);
    if (!f) return -ENOMEM;
    size_t l = strlen(name);
    f->path = kmalloc(l + 8);
    if (f->path) { memcpy(f->path, "/memfd:", 7); memcpy(f->path + 7, name, l + 1); }
    int fd = fd_alloc(f, 0, flags & MFD_CLOEXEC);
    if (fd < 0) vfs_close(f);
    return fd;
}

/* ------------------------------------------------------------------ inotify */
/*
 * Watches pin their inode (like Linux). VFS operations call the fsnotify_* hooks (cheap no-ops
 * while no watch exists): events about a directory entry go to watches on the directory (with
 * the name), events about an object go to watches on the object itself.
 */
#define IN_MODIFY 0x2
#define IN_CLOSE_WRITE 0x8
#define IN_Q_OVERFLOW 0x4000
#define IN_IGNORED 0x8000
#define IN_ONLYDIR 0x1000000
#define IN_DONT_FOLLOW 0x2000000
#define IN_MASK_CREATE 0x10000000
#define IN_MASK_ADD 0x20000000
#define IN_ISDIR 0x40000000
#define IN_ONESHOT 0x80000000u
#define IN_ALL_EVENTS 0xfff
#define INOTIFY_MAX_EVENTS 16384

struct inotify_ev { struct list_node node; int wd; uint32_t mask, cookie, len; char name[]; };
struct inotify;
struct iwatch { struct list_node node, inode_node; struct inotify *in; struct inode *ino; int wd; uint32_t mask; };
struct inotify { struct list_node watches, events; size_t nevents, bytes; int next_wd; };

static struct list_node all_watches = { &all_watches, &all_watches };
int fsnotify_nwatches;
static uint32_t next_cookie = 1;

static void in_queue(struct inotify *in, int wd, uint32_t mask, uint32_t cookie, const char *name) {
    size_t nl = name ? strlen(name) : 0, len = nl ? ALIGN_UP(nl + 1, 16) : 0;
    if (!list_empty(&in->events)) {        /* coalesce identical back-to-back events */
        struct inotify_ev *last = list_entry(in->events.prev, struct inotify_ev, node);
        if (last->wd == wd && last->mask == mask && last->cookie == cookie && last->len == len &&
            (!len || !strcmp(last->name, name))) return;
    }
    if (in->nevents >= INOTIFY_MAX_EVENTS) {
        if (in->nevents > INOTIFY_MAX_EVENTS) return;
        wd = -1; mask = IN_Q_OVERFLOW; cookie = 0; nl = len = 0;
    }
    struct inotify_ev *e = kzalloc(sizeof *e + len);
    if (!e) return;
    e->wd = wd; e->mask = mask; e->cookie = cookie; e->len = len;
    if (nl) memcpy(e->name, name, nl);
    list_add_tail(&in->events, &e->node);
    in->nevents++; in->bytes += 16 + len;
    poll_notify();
}

static void watch_remove(struct iwatch *w, bool notify) {
    if (notify) in_queue(w->in, w->wd, IN_IGNORED, 0, nullptr);
    list_del(&w->node); list_del(&w->inode_node);
    iput(w->ino);
    kfree(w);
    fsnotify_nwatches--;
}

static void notify(struct inode *i, uint32_t mask, uint32_t cookie, const char *name) {
    list_for_each_safe(it, tmp, &all_watches) {
        struct iwatch *w = list_entry(it, struct iwatch, inode_node);
        if (w->ino != i || !(w->mask & mask & IN_ALL_EVENTS)) continue;
        in_queue(w->in, w->wd, mask & (IN_ALL_EVENTS | IN_ISDIR), cookie, name);
        if (w->mask & IN_ONESHOT) watch_remove(w, true);
    }
}

void fsnotify_dirent_(struct inode *dir, const char *name, uint32_t mask, bool isdir, uint32_t cookie) {
    notify(dir, mask | (isdir ? IN_ISDIR : 0), cookie, name);
}
void fsnotify_inode_(struct inode *i, uint32_t mask) {
    notify(i, mask | (S_ISDIR(i->mode) ? IN_ISDIR : 0), 0, nullptr);
}
/* object event, also reported to the parent directory with the entry name (from f->path) */
static void fsnotify_file_locked(struct file *f, uint32_t mask);
/* watch lists and path walks still need the BKL; lock-free read/write paths take it here */
void fsnotify_file_(struct file *f, uint32_t mask) {
    bool took = !bkl_held();
    if (took) bkl_enter();
    fsnotify_file_locked(f, mask);
    if (took) bkl_exit();
}
static void fsnotify_file_locked(struct file *f, uint32_t mask) {
    struct inode *i = f->inode;
    if (!i) return;
    fsnotify_inode_(i, mask);
    if (S_ISDIR(i->mode) || !f->path || f->path[0] != '/') return;
    struct inode *dir; char last[256];
    if (vfs_lookup_parent_at(nullptr, f->path, &dir, last)) return;
    notify(dir, mask, 0, last);
    iput(dir);
}
/* object event for a path-based syscall: the object plus its parent directory with the name */
void fsnotify_path_(struct inode *base, const char *path, struct inode *i, uint32_t mask) {
    fsnotify_inode_(i, mask);
    struct inode *dir; char last[256];
    if (vfs_lookup_parent_at(base, path, &dir, last)) return;
    if (dir != i) notify(dir, mask | (S_ISDIR(i->mode) ? IN_ISDIR : 0), 0, last);
    iput(dir);
}
/* the inode lost a link: DELETE_SELF + IGNORED once it is gone */
void fsnotify_unlinked_(struct inode *i) {
    if (S_ISDIR(i->mode) || i->nlink == 0) {
        fsnotify_inode_(i, 0x400 /* IN_DELETE_SELF */);
        list_for_each_safe(it, tmp, &all_watches) {
            struct iwatch *w = list_entry(it, struct iwatch, inode_node);
            if (w->ino == i) watch_remove(w, true);
        }
    } else {
        fsnotify_inode_(i, 0x4 /* IN_ATTRIB */);
    }
}
uint32_t fsnotify_cookie(void) { return next_cookie++; }

static ssize_t in_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct inotify *in = f->priv;
    int r = WAIT_READY(f, in->nevents > 0);
    if (r) return r;
    size_t done = 0;
    while (!list_empty(&in->events)) {
        struct inotify_ev *e = list_entry(in->events.next, struct inotify_ev, node);
        size_t sz = 16 + e->len;
        if (done + sz > n) break;
        if (copy_to_user((char *)buf + done, &e->wd, 16) ||
            (e->len && copy_to_user((char *)buf + done + 16, e->name, e->len))) return done ? (ssize_t)done : -EFAULT;
        done += sz;
        list_del(&e->node); in->nevents--; in->bytes -= sz; kfree(e);
    }
    return done ? (ssize_t)done : -EINVAL;
}
static unsigned in_poll(struct file *f) { struct inotify *in = f->priv; return in->nevents ? POLLIN | POLLRDNORM : 0; }
static int in_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct inotify *in = f->priv;
    if (cmd != 0x541B) return -ENOTTY;    /* FIONREAD */
    int v = (int)in->bytes;
    return copy_to_user((void *)arg, &v, sizeof v);
}
static void in_release(struct file *f) {
    struct inotify *in = f->priv;
    list_for_each_safe(it, tmp, &in->watches) watch_remove(list_entry(it, struct iwatch, node), false);
    list_for_each_safe(it, tmp, &in->events) kfree(list_entry(it, struct inotify_ev, node));
    kfree(in);
}
static const struct file_ops inotify_fops = { .read = in_read, .poll = in_poll, .ioctl = in_ioctl, .release = in_release };

int64_t sys_inotify_init1(int flags) {
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
    struct inotify *in = kzalloc(sizeof *in);
    if (!in) return -ENOMEM;
    list_init(&in->watches); list_init(&in->events);
    in->next_wd = 1;
    int fd = anon_fd(&inotify_fops, in, flags, 0600);
    if (fd < 0) kfree(in);
    return fd;
}
int64_t sys_inotify_init(void) { return sys_inotify_init1(0); }

int user_path(const char *upath, char *kpath);
int64_t sys_inotify_add_watch(int fd, const char *upath, uint32_t mask) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->fops != &inotify_fops) return -EINVAL;
    if (!(mask & IN_ALL_EVENTS)) return -EINVAL;
    if ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)) return -EINVAL;
    struct inotify *in = f->priv;
    char *kp = kmalloc(4096);
    int r = user_path(upath, kp);
    struct inode *i = nullptr;
    if (!r) r = vfs_lookup(kp, !(mask & IN_DONT_FOLLOW), &i);
    kfree(kp);
    if (r) return r;
    if ((mask & IN_ONLYDIR) && !S_ISDIR(i->mode)) { iput(i); return -ENOTDIR; }
    list_for_each(it, &in->watches) {
        struct iwatch *w = list_entry(it, struct iwatch, node);
        if (w->ino != i) continue;
        iput(i);
        if (mask & IN_MASK_CREATE) return -EEXIST;
        w->mask = (mask & IN_MASK_ADD) ? w->mask | (mask & ~IN_MASK_ADD) : mask;
        return w->wd;
    }
    struct iwatch *w = kzalloc(sizeof *w);
    if (!w) { iput(i); return -ENOMEM; }
    w->in = in; w->ino = i; w->wd = in->next_wd++; w->mask = mask & ~(IN_MASK_ADD | IN_MASK_CREATE);
    list_add_tail(&in->watches, &w->node);
    list_add_tail(&all_watches, &w->inode_node);
    fsnotify_nwatches++;
    return w->wd;
}

int64_t sys_inotify_rm_watch(int fd, int wd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->fops != &inotify_fops) return -EINVAL;
    struct inotify *in = f->priv;
    list_for_each(it, &in->watches) {
        struct iwatch *w = list_entry(it, struct iwatch, node);
        if (w->wd == wd) { watch_remove(w, true); return 0; }
    }
    return -EINVAL;
}
