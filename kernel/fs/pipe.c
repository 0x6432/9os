/* Anonymous pipes. */
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/process.h>
#include <kernel/signal.h>

#define PIPE_SIZE 65536

struct pipe {
    char *buf;
    size_t head, tail;         /* tail - head = bytes stored */
    int readers, writers;
    struct wait_queue rq, wq;
};

static size_t pcount(struct pipe *p) { return p->tail - p->head; }

static ssize_t pipe_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct pipe *p = f->inode->priv;
    if (!n) return 0;
    while (!pcount(p)) {
        if (!p->writers) return 0;
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        int r = wait_until(&p->rq, pcount(p) || !p->writers);
        if (r) return r;
    }
    size_t got = 0;
    while (got < n && pcount(p)) ((char *)buf)[got++] = p->buf[p->head++ % PIPE_SIZE];
    wake_up(&p->wq);
    poll_notify();
    return got;
}

static ssize_t pipe_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct pipe *p = f->inode->priv;
    size_t done = 0;
    while (done < n) {
        if (!p->readers) {
            signal_send(curproc, SIGPIPE);
            return done ? (ssize_t)done : -EPIPE;
        }
        if (pcount(p) == PIPE_SIZE) {
            if (f->flags & O_NONBLOCK) return done ? (ssize_t)done : -EAGAIN;
            int r = wait_until(&p->wq, pcount(p) < PIPE_SIZE || !p->readers);
            if (r) return done ? (ssize_t)done : r;
            continue;
        }
        while (done < n && pcount(p) < PIPE_SIZE) p->buf[p->tail++ % PIPE_SIZE] = ((const char *)buf)[done++];
        wake_up(&p->rq);
        poll_notify();
    }
    return done;
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
    if ((f->flags & O_ACCMODE) == O_RDONLY) p->readers--; else p->writers--;
    wake_up(&p->rq); wake_up(&p->wq);
    poll_notify();
}

static void pipe_evict(struct inode *i) {
    struct pipe *p = i->priv;
    kfree(p->buf);
    kfree(p);
}

static const struct file_ops pipe_fops = { .read = pipe_read, .write = pipe_write, .poll = pipe_poll, .release = pipe_release };
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
