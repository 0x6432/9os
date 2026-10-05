/* Anonymous pipes. */
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/mm.h>

#define PIPE_SIZE 65536

/*
 * Pipes run without the BKL (file_ops.nobkl): ring state is guarded by p->lock, an IRQ-off
 * lock spun with spin_lock_ipi() because user copies (mm lock, possibly TLB shootdowns) are
 * done while holding it. Sleeps use wait_until_sl(), which re-checks the condition under
 * sched_lock, so a waker on another CPU cannot slip between the check and the sleep.
 * Writes of at most PIPE_BUF bytes are atomic.
 */
#define PIPE_BUF 4096

struct pipe {
    char *buf;
    size_t head, tail;         /* tail - head = bytes stored */
    int readers, writers;
    struct wait_queue rq, wq;
    spinlock_t lock;
};

static size_t pcount(struct pipe *p) { return __atomic_load_n(&p->tail, __ATOMIC_ACQUIRE) - __atomic_load_n(&p->head, __ATOMIC_ACQUIRE); }
static uint64_t plock(struct pipe *p) { uint64_t f = arch_irq_save(); spin_lock_ipi(&p->lock); return f; }
static void punlock(struct pipe *p, uint64_t f) { spin_unlock(&p->lock); arch_irq_restore(f); }

/* buffers are user pointers for read(2)/write(2), kernel pointers for sendfile/splice */
static int copy_out(void *dst, const void *src, size_t n) {
    if ((vaddr_t)dst >= USER_TOP) { memcpy(dst, src, n); return 0; }
    return copy_to_user(dst, src, n);
}
static int copy_in(void *dst, const void *src, size_t n) {
    if ((vaddr_t)src >= USER_TOP) { memcpy(dst, src, n); return 0; }
    return copy_from_user(dst, src, n);
}

static ssize_t pipe_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct pipe *p = f->inode->priv;
    if (!n) return 0;
    for (;;) {
        uint64_t fl = plock(p);
        size_t avail = p->tail - p->head;
        if (avail) {
            size_t take = MIN(n, avail), h = p->head % PIPE_SIZE, first = MIN(take, PIPE_SIZE - h);
            int e = copy_out(buf, p->buf + h, first);
            if (!e && take > first) e = copy_out((char *)buf + first, p->buf, take - first);
            if (e) { punlock(p, fl); return -EFAULT; }
            __atomic_store_n(&p->head, p->head + take, __ATOMIC_RELEASE);
            punlock(p, fl);
            wake_up(&p->wq);
            poll_notify();
            return (ssize_t)take;
        }
        int writers = p->writers;
        punlock(p, fl);
        if (!writers) return 0;
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        int r = wait_until_sl(&p->rq, pcount(p) || !__atomic_load_n(&p->writers, __ATOMIC_ACQUIRE));
        if (r) return r;
    }
}

static ssize_t pipe_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct pipe *p = f->inode->priv;
    size_t done = 0;
    while (done < n) {
        size_t need = n <= PIPE_BUF ? n : 1;     /* small writes go in whole */
        uint64_t fl = plock(p);
        if (!p->readers) {
            punlock(p, fl);
            bkl_enter();
            signal_send(curproc, SIGPIPE);
            bkl_exit();
            return done ? (ssize_t)done : -EPIPE;
        }
        size_t space = PIPE_SIZE - (p->tail - p->head);
        if (space >= need) {
            size_t put = MIN(n - done, space), t = p->tail % PIPE_SIZE, first = MIN(put, PIPE_SIZE - t);
            int e = copy_in(p->buf + t, (const char *)buf + done, first);
            if (!e && put > first) e = copy_in(p->buf, (const char *)buf + done + first, put - first);
            if (e) { punlock(p, fl); return done ? (ssize_t)done : -EFAULT; }
            __atomic_store_n(&p->tail, p->tail + put, __ATOMIC_RELEASE);
            done += put;
            punlock(p, fl);
            wake_up(&p->rq);
            poll_notify();
            continue;
        }
        punlock(p, fl);
        if (f->flags & O_NONBLOCK) return done ? (ssize_t)done : -EAGAIN;
        int r = wait_until_sl(&p->wq, PIPE_SIZE - pcount(p) >= need || !__atomic_load_n(&p->readers, __ATOMIC_ACQUIRE));
        if (r) return done ? (ssize_t)done : r;
    }
    return (ssize_t)done;
}

static unsigned pipe_poll(struct file *f) {
    struct pipe *p = f->inode->priv;
    unsigned r = 0;
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        if (pcount(p)) r |= POLLIN | POLLRDNORM;
        if (!p->writers) r |= POLLHUP;
    } else {
        if (pcount(p) < PIPE_SIZE) r |= POLLOUT | POLLWRNORM;
        if (!p->readers) r |= POLLERR;
    }
    return r;
}

static void pipe_release(struct file *f) {
    struct pipe *p = f->inode->priv;
    uint64_t fl = plock(p);
    if ((f->flags & O_ACCMODE) == O_RDONLY) p->readers--; else p->writers--;
    punlock(p, fl);
    wake_up(&p->rq); wake_up(&p->wq);
    poll_notify();
}

static void pipe_evict(struct inode *i) {
    struct pipe *p = i->priv;
    kfree(p->buf);
    kfree(p);
}

static const struct file_ops pipe_fops = { .nobkl = true, .read = pipe_read, .write = pipe_write, .poll = pipe_poll, .release = pipe_release };
static const struct inode_ops pipe_iops = { .evict = pipe_evict };

struct inode *pipe_create(struct file **rd, struct file **wr) {
    struct pipe *p = kzalloc(sizeof *p);
    if (!p) return nullptr;
    p->buf = kmalloc(PIPE_SIZE);
    if (!p->buf) { kfree(p); return nullptr; }
    wait_queue_init(&p->rq); wait_queue_init(&p->wq);
    p->readers = p->writers = 1;
    struct inode *i = inode_alloc(S_IFIFO | 0600);
    i->priv = p;
    i->fops = &pipe_fops;
    i->iops = &pipe_iops;
    *rd = file_open_inode(i, O_RDONLY);
    *wr = file_open_inode(i, O_WRONLY);
    iput(i);       /* files hold the references; nlink 0 so last close frees it */
    return i;
}
