/*
 * Pseudo terminals (included by tty.c so they reuse the line discipline):
 * /dev/ptmx (5:2) allocates a pair; the slave appears as /dev/pts/N (136:N).
 * Master writes are fed to the slave's line discipline (tty_input); slave output (and echo)
 * is post-processed (OPOST/ONLCR) into the master's buffer.
 */
#define PTY_MAX 64
#define PTY_BUF 16384

struct pty {
    struct tty t;
    char mbuf[PTY_BUF]; size_t mhead, mtail;
    bool master_open, locked, slave_ever;
    int slave_opens, index;
};
static struct pty *ptys[PTY_MAX];

static size_t m_count(struct pty *p) { return p->mtail - p->mhead; }

/* p->t.lock held. Post-processes and queues as much of s as fits; returns bytes consumed. */
static size_t pty_put_locked(struct pty *p, const char *s, size_t n) {
    struct tty *t = &p->t;
    size_t done = 0;
    while (done < n) {
        char c = s[done];
        bool crlf = c == '\n' && (t->t.c_oflag & OPOST) && (t->t.c_oflag & ONLCR);
        size_t need = crlf ? 2 : 1;
        if (PTY_BUF - m_count(p) < need) break;
        if (crlf) p->mbuf[p->mtail++ % PTY_BUF] = '\r';
        p->mbuf[p->mtail++ % PTY_BUF] = c;
        done++;
    }
    return done;
}

/* Echo (may_block false) comes from the line discipline with t->lock held and drops what
 * does not fit; slave writes (kernel buffer from tty_write) block until the master reads. */
static ssize_t pty_out(struct tty *t, const char *s, size_t n, bool may_block) {
    struct pty *p = t->drv;
    if (!may_block) {
        if (!p->master_open) return -EIO;
        size_t d = pty_put_locked(p, s, n);
        poll_notify();
        return d;
    }
    size_t done = 0;
    while (done < n) {
        uint64_t seq = poll_seq_read();
        uint64_t f = tlock(t);
        if (!p->master_open) { tunlock(t, f); return done ? (ssize_t)done : -EIO; }
        size_t d = pty_put_locked(p, s + done, n - done);
        tunlock(t, f);
        done += d;
        if (d) { poll_notify(); continue; }
        int r = poll_wait_seq(seq, UINT64_MAX);
        if (r) return done ? (ssize_t)done : r;
    }
    return done;
}

static void pty_clear_ctty(struct process *pr, void *c) { if (pr->ctty == c) pr->ctty = nullptr; }

static void pty_maybe_free(struct pty *p) {
    if (p->master_open || p->slave_opens) return;
    process_list(pty_clear_ctty, &p->t);
    ptys[p->index] = nullptr;
    kfree(p);
}

/* ---- master (/dev/ptmx) */
static ssize_t ptm_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct pty *p = f->priv;
    uint64_t fl;
    for (;;) {
        uint64_t seq = poll_seq_read();
        fl = tlock(&p->t);
        if (m_count(p)) break;                              /* keep the lock */
        bool hup = p->slave_ever && !p->slave_opens;
        tunlock(&p->t, fl);
        if (hup) return -EIO;
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        int r = poll_wait_seq(seq, UINT64_MAX);
        if (r) return r;
    }
    char kb[512];
    size_t got = 0, lim = MIN(n, sizeof kb);
    while (got < lim && m_count(p)) kb[got++] = p->mbuf[p->mhead++ % PTY_BUF];
    tunlock(&p->t, fl);
    poll_notify();                                          /* blocked slave writers */
    if (copy_out(buf, kb, got)) return -EFAULT;
    return got;
}
static ssize_t ptm_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct pty *p = f->priv;
    char kb[256];
    for (size_t done = 0; done < n; ) {
        size_t c = MIN(n - done, sizeof kb);
        if (copy_in(kb, (const char *)buf + done, c)) return done ? (ssize_t)done : -EFAULT;
        for (size_t i = 0; i < c; i++) tty_input_ctx(&p->t, kb[i], true);
        done += c;
    }
    poll_notify();
    return n;
}
static unsigned ptm_poll(struct file *f) {
    struct pty *p = f->priv;
    unsigned r = POLLOUT | POLLWRNORM;
    if (m_count(p)) r |= POLLIN | POLLRDNORM;
    if (p->slave_ever && !p->slave_opens) r |= POLLHUP;
    return r;
}
static int pts_open_peer(struct pty *p, int flags);
static int ptm_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct pty *p = f->priv;
    switch (cmd) {
    case 0x80045430: return copy_to_user((void *)arg, &p->index, sizeof(int)) ? -EFAULT : 0;   /* TIOCGPTN */
    case 0x40045431: {                                                                       /* TIOCSPTLCK */
        int v; if (copy_from_user(&v, (void *)arg, sizeof v)) return -EFAULT;
        p->locked = v != 0; return 0;
    }
    case 0x5441: return pts_open_peer(p, (int)arg);                                          /* TIOCGPTPEER */
    case 0x541B: { int v = (int)m_count(p); return copy_to_user((void *)arg, &v, sizeof v) ? -EFAULT : 0; }
    case 0x540E: case 0x5422: return 0;              /* the master is never a controlling tty */
    default: return tty_ioctl_t(&p->t, f, cmd, arg);
    }
}
static void ptm_release(struct file *f) {
    struct pty *p = f->priv;
    uint64_t fl = tlock(&p->t);
    p->master_open = false;
    p->t.hup = true;
    tunlock(&p->t, fl);
    wake_up(&p->t.rq);
    if (p->t.pgrp > 0) signal_send_pgrp(p->t.pgrp, SIGHUP);
    char path[32]; snprintf(path, sizeof path, "/dev/pts/%d", p->index);
    struct cred *oc = cred_override(&init_cred);     /* the kernel removes the node */
    vfs_unlink_at(nullptr, path, false);
    cred_revert(oc);
    poll_notify();
    pty_maybe_free(p);
}
static int ptm_open(struct inode *ino, struct file *f) {
    int idx = -1;
    for (int i = 0; i < PTY_MAX; i++) if (!ptys[i]) { idx = i; break; }
    if (idx < 0) return -ENOSPC;
    struct pty *p = kzalloc(sizeof *p);
    if (!p) return -ENOMEM;
    tty_init_struct(&p->t);
    p->t.output = pty_out;
    p->t.drv = p;
    p->t.ws = (struct winsize){ 24, 80, 0, 0 };
    p->index = idx;
    p->master_open = true;
    p->locked = true;
    ptys[idx] = p;
    f->priv = p;
    char path[32]; snprintf(path, sizeof path, "/dev/pts/%d", idx);
    /* M31: the node is made by the kernel, owned by the opener, group tty (5), mode 0620 */
    uint32_t owner = current_cred()->fsuid;
    struct cred *oc = cred_override(&init_cred);
    vfs_mknod_at(nullptr, path, S_IFCHR | 0620, MKDEV(136, idx));
    struct inode *n;
    if (!vfs_lookup(path, false, &n)) { n->uid = owner; n->gid = 5; n->mode = S_IFCHR | 0620; iput(n); }
    cred_revert(oc);
    return 0;
}
static const struct file_ops ptmx_ops = {
    .nobkl = true, .open = ptm_open, .read = ptm_read, .write = ptm_write, .poll = ptm_poll, .ioctl = ptm_ioctl, .release = ptm_release,
};

/* ---- slave (/dev/pts/N) */
static int pts_open(struct inode *ino, struct file *f) {
    unsigned idx = MINOR(ino->rdev);
    struct pty *p = idx < PTY_MAX ? ptys[idx] : nullptr;
    if (!p || !p->master_open || p->locked) return -EIO;
    p->slave_opens++; p->slave_ever = true;
    f->priv = &p->t;
    maybe_acquire_ctty(&p->t, f);
    return 0;
}
static void pts_release(struct file *f) {
    struct tty *t = f->priv;
    struct pty *p = t->drv;
    p->slave_opens--;
    poll_notify();
    pty_maybe_free(p);
}
static const struct file_ops pts_ops = {
    .nobkl = true, .open = pts_open, .read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .poll = tty_poll, .release = pts_release,
};

static int pts_open_peer(struct pty *p, int flags) {
    char path[32]; snprintf(path, sizeof path, "/dev/pts/%d", p->index);
    struct file *f;
    int r = vfs_open_at(nullptr, path, (flags & (O_ACCMODE | O_NONBLOCK)) | O_NOCTTY, 0, &f);
    if (r) return r;
    int fd = fd_alloc(f, 0, flags & O_CLOEXEC);
    if (fd < 0) vfs_close(f);
    return fd;
}

static void pty_register(void) {
    chrdev_register(5, 2, &ptmx_ops);
    chrdev_register(136, CHRDEV_ANY_MINOR, &pts_ops);
}
