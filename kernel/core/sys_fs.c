/* File-system related system calls. */
#include <kernel/uaccess.h>
#include <kernel/syscall.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <arch/stat.h>
#include <kernel/pmm.h>
#include <kernel/blk.h>

#define PATH_MAX 4096
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

static inline bool cloexec_get(struct process *p, int fd) { return p->cloexec[fd / 64] & (1ULL << (fd % 64)); }
static inline void cloexec_set(struct process *p, int fd, bool v) {
    if (v) p->cloexec[fd / 64] |= 1ULL << (fd % 64); else p->cloexec[fd / 64] &= ~(1ULL << (fd % 64));
}

/*
 * Borrowed lookup (M24): the file is pinned until the current syscall returns
 * (fd_borrow_release() in syscall_dispatch and the exit paths), so a concurrent close() by
 * another thread can no longer free it under a syscall, with or without the BKL.
 */
struct file *fd_get(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return nullptr;
    struct thread *t = current;
    if (!t || t->nborrow >= (int)(sizeof t->fd_borrow / sizeof t->fd_borrow[0])) {
        static bool warned;
        if (!warned) { warned = true; pr_warn("fd_get: borrow table full in %s\n", t ? t->name : "?"); }
        return curproc->fds[fd];          /* unpinned (only safe while close needs the BKL) */
    }
    struct file *f = fd_get_ref(fd);
    if (f) t->fd_borrow[t->nborrow++] = f;
    return f;
}

void fd_borrow_release(void) {
    struct thread *t = current;
    if (!t) return;
    while (t->nborrow > 0) {
        struct file *f = t->fd_borrow[--t->nborrow];
        t->fd_borrow[t->nborrow] = nullptr;
        vfs_close(f);
    }
}

/* reference-taking lookup for syscalls running without the BKL; drop with vfs_close() */
struct file *fd_get_ref(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return nullptr;
    struct process *p = curproc;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct file *f = p->fds[fd];
    if (f) file_get(f);
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return f;
}

/* swap a slot under fd_lock; returns the previous file (caller closes it) */
struct file *fd_slot_set(struct process *p, int fd, struct file *f) {
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct file *old = p->fds[fd];
    p->fds[fd] = f;
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return old;
}

/* All fd-table mutations (slots and close-on-exec bits) happen under p->fd_lock. */
int fd_install(int fd, struct file *f, bool cloexec) {
    struct process *p = curproc;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct file *old = p->fds[fd];
    p->fds[fd] = f;
    cloexec_set(p, fd, cloexec);
    spin_unlock_irqrestore(&p->fd_lock, fl);
    if (old) vfs_close(old);
    return fd;
}

int fd_alloc(struct file *f, int min, bool cloexec) {
    struct process *p = curproc;
    if (min < 0) return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    for (int i = min; i < MAX_FDS; i++)
        if (!p->fds[i]) {
            p->fds[i] = f;
            cloexec_set(p, i, cloexec);
            spin_unlock_irqrestore(&p->fd_lock, fl);
            return i;
        }
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return -EMFILE;
}

int fd_close(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    struct process *p = curproc;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    struct file *f = p->fds[fd];
    p->fds[fd] = nullptr;
    cloexec_set(p, fd, false);
    spin_unlock_irqrestore(&p->fd_lock, fl);
    if (!f) return -EBADF;
    vfs_close(f);
    return 0;
}

bool fd_cloexec_get(int fd) {
    struct process *p = curproc;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    bool v = fd >= 0 && fd < MAX_FDS && cloexec_get(p, fd);
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return v;
}
int fd_cloexec_set(int fd, bool v) {
    struct process *p = curproc;
    uint64_t fl = spin_lock_irqsave(&p->fd_lock);
    int r = fd >= 0 && fd < MAX_FDS && p->fds[fd] ? 0 : -EBADF;
    if (!r) cloexec_set(p, fd, v);
    spin_unlock_irqrestore(&p->fd_lock, fl);
    return r;
}

void files_close_on_exec(struct process *p) {
    for (int i = 0; i < MAX_FDS; i++) {
        uint64_t fl = spin_lock_irqsave(&p->fd_lock);
        struct file *f = nullptr;
        if (p->fds[i] && cloexec_get(p, i)) { f = p->fds[i]; p->fds[i] = nullptr; cloexec_set(p, i, false); }
        spin_unlock_irqrestore(&p->fd_lock, fl);
        if (f) vfs_close(f);
    }
}

int user_path(const char *upath, char *kpath) {
    if (!upath) return -EFAULT;
    int64_t r = strncpy_from_user(kpath, upath, PATH_MAX);
    if (r < 0) return (int)r;
    return 0;
}

int dirfd_base(int dirfd, const char *path, struct inode **base) {
    *base = nullptr;
    if (path[0] == '/' || dirfd == AT_FDCWD) return 0;
    struct file *f = fd_get(dirfd);
    if (!f) return -EBADF;
    if (!S_ISDIR(f->inode->mode) && path[0]) return -ENOTDIR;
    *base = f->inode;
    return 0;
}

#define WITH_PATH(upath, kp) \
    char *kp = kmalloc(PATH_MAX); \
    int64_t __r = user_path(upath, kp); \
    if (__r) { kfree(kp); return __r; }

int64_t sys_openat(int dirfd, const char *upath, int flags, uint32_t mode) {
    WITH_PATH(upath, path);
    struct inode *base;
    int64_t r = dirfd_base(dirfd, path, &base);
    struct file *f = nullptr;
    if (!r) r = vfs_open_at(base, path, flags, mode, &f);
    kfree(path);
    if (r) return r;
    r = fd_alloc(f, 0, flags & O_CLOEXEC);
    if (r < 0) vfs_close(f);
    return r;
}
int64_t sys_open(const char *p, int flags, uint32_t mode) { return sys_openat(AT_FDCWD, p, flags, mode); }
int64_t sys_creat(const char *p, uint32_t mode) { return sys_openat(AT_FDCWD, p, O_CREAT | O_WRONLY | O_TRUNC, mode); }
int64_t sys_close(int fd) { return fd_close(fd); }

int64_t sys_close_range(unsigned first, unsigned last, unsigned flags) {
    for (unsigned i = first; i <= last && i < MAX_FDS; i++) {
        if (flags & 4) fd_cloexec_set(i, true);
        else fd_close(i);
    }
    return 0;
}

/*
 * read/write are dispatched without the BKL (lockfree_names in syscall.c.in). Files whose
 * ops are marked nobkl (pipes) run as-is and copy with copy_{to,from}_user; everything else
 * takes the BKL here, and its drivers may then touch the (pre-faulted) user buffer directly,
 * inside a user-access window (SMAP/PAN/SUM open, M27).
 */
#define uaccess_call(e) ({ user_access_begin(); int64_t __r = (e); user_access_end(); __r; })
int64_t sys_read(int fd, void *buf, size_t n) {
    if (!access_ok(buf, n)) return -EFAULT;      /* file ops treat kernel addresses as kernel buffers */
    struct file *f = fd_get_ref(fd);
    if (!f) return -EBADF;
    int64_t r;
    if (f->fops && f->fops->nobkl) r = vfs_read(f, buf, n);
    else {
        bkl_enter();
        r = n && !user_range_ok(buf, n, true) ? -EFAULT : uaccess_call(vfs_read(f, buf, n));
        bkl_exit();
    }
    vfs_close(f);
    return r;
}

int64_t sys_write(int fd, const void *buf, size_t n) {
    if (!access_ok(buf, n)) return -EFAULT;      /* file ops treat kernel addresses as kernel buffers */
    struct file *f = fd_get_ref(fd);
    if (!f) return -EBADF;
    int64_t r;
    if (f->fops && f->fops->nobkl) r = vfs_write(f, buf, n);
    else {
        bkl_enter();
        r = n && !user_range_ok(buf, n, false) ? -EFAULT : uaccess_call(vfs_write(f, buf, n));
        bkl_exit();
    }
    vfs_close(f);
    return r;
}

/* lock-free like read/write: nobkl files (tmpfs) run as-is, others take the BKL */
int64_t sys_pread64(int fd, void *buf, size_t n, off_t off) {
    if (!access_ok(buf, n)) return -EFAULT;      /* file ops treat kernel addresses as kernel buffers */
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!S_ISREG(f->inode->mode) && !S_ISBLK(f->inode->mode)) return -ESPIPE;
    if (f->fops && f->fops->nobkl) return vfs_pread(f, buf, n, off);
    bkl_enter();
    int64_t r = n && !user_range_ok(buf, n, true) ? -EFAULT : uaccess_call(vfs_pread(f, buf, n, off));
    bkl_exit();
    return r;
}

int64_t sys_pwrite64(int fd, const void *buf, size_t n, off_t off) {
    if (!access_ok(buf, n)) return -EFAULT;      /* file ops treat kernel addresses as kernel buffers */
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!S_ISREG(f->inode->mode) && !S_ISBLK(f->inode->mode)) return -ESPIPE;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!f->fops || !f->fops->write) return -EINVAL;
    file_remove_privs(f);
    if (f->fops->nobkl) return f->fops->write(f, buf, n, &off);
    bkl_enter();
    int64_t r = n && !user_range_ok(buf, n, false) ? -EFAULT : uaccess_call(f->fops->write(f, buf, n, &off));
    bkl_exit();
    return r;
}

struct iovec { void *base; size_t len; };

int64_t sys_readv(int fd, const struct iovec *uiov, int cnt) {
    if (cnt < 0 || cnt > 1024) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        struct iovec v;
        if (copy_from_user(&v, &uiov[i], sizeof v)) return -EFAULT;
        if (!v.len) continue;
        int64_t r = sys_read(fd, v.base, v.len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((size_t)r < v.len) break;
    }
    return total;
}

int64_t sys_writev(int fd, const struct iovec *uiov, int cnt) {
    if (cnt < 0 || cnt > 1024) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        struct iovec v;
        if (copy_from_user(&v, &uiov[i], sizeof v)) return -EFAULT;
        if (!v.len) continue;
        int64_t r = sys_write(fd, v.base, v.len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((size_t)r < v.len) break;
    }
    return total;
}

int64_t sys_lseek(int fd, off_t off, int whence) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (S_ISFIFO(f->inode->mode) || S_ISCHR(f->inode->mode)) return S_ISCHR(f->inode->mode) ? 0 : -ESPIPE;
    off_t n;
    switch (whence) {
    case SEEK_SET: n = off; break;
    case SEEK_CUR: n = f->pos + off; break;
    case SEEK_END: n = (off_t)(S_ISBLK(f->inode->mode) ? blkdev_size(f->inode->rdev) : f->inode->size) + off; break;
    default: return -EINVAL;
    }
    if (n < 0) return -EINVAL;
    f->pos = n;
    return n;
}

int64_t sys_sendfile(int out, int in, off_t *uoff, size_t count) {
    struct file *fi = fd_get(in), *fo = fd_get(out);
    if (!fi || !fo) return -EBADF;
    off_t off = fi->pos;
    if (uoff && copy_from_user(&off, uoff, sizeof off)) return -EFAULT;
    char *buf = kmalloc(65536);
    int64_t total = 0;
    while (count) {
        ssize_t r = vfs_pread(fi, buf, MIN(count, 65536), off);
        if (r <= 0) { if (r < 0 && !total) total = r; break; }
        ssize_t w = vfs_write(fo, buf, r);
        if (w < 0) { if (!total) total = w; break; }
        off += w; total += w; count -= w;
        if (w < r) break;
    }
    kfree(buf);
    if (total >= 0) {
        if (uoff) copy_to_user(uoff, &off, sizeof off);
        else fi->pos = off;
    }
    return total;
}

static int stat_out(struct inode *i, void *ubuf) {
    struct kstat k;
    struct linux_stat st;
    vfs_stat(i, &k);
    kstat_to_linux(&k, &st);
    return copy_to_user(ubuf, &st, sizeof st);
}

int64_t sys_newfstatat(int dirfd, const char *upath, void *ubuf, int flags) {
    WITH_PATH(upath, path);
    struct inode *base, *ino;
    int64_t r;
    if (!path[0] && (flags & AT_EMPTY_PATH)) {
        kfree(path);
        if (dirfd == AT_FDCWD) {
            vfs_ns_lock(); struct inode *c = curproc->cwd; iget(c); vfs_ns_unlock();
            r = stat_out(c, ubuf);
            iput(c);
            return r;
        }
        struct file *f = fd_get(dirfd);
        return f ? stat_out(f->inode, ubuf) : -EBADF;
    }
    r = dirfd_base(dirfd, path, &base);
    if (!r) r = vfs_lookup_at(base, path, !(flags & AT_SYMLINK_NOFOLLOW), &ino);
    kfree(path);
    if (r) return r;
    r = stat_out(ino, ubuf);
    iput(ino);
    return r;
}
int64_t sys_stat(const char *p, void *b) { return sys_newfstatat(AT_FDCWD, p, b, 0); }
int64_t sys_lstat(const char *p, void *b) { return sys_newfstatat(AT_FDCWD, p, b, AT_SYMLINK_NOFOLLOW); }
int64_t sys_fstat(int fd, void *b) {
    struct file *f = fd_get(fd);
    return f ? stat_out(f->inode, b) : -EBADF;
}

int64_t sys_statx(int dirfd, const char *upath, int flags, unsigned mask, void *ubuf) {
    WITH_PATH(upath, path);
    struct inode *base, *ino = nullptr;
    int64_t r = 0;
    if (!path[0] && (flags & AT_EMPTY_PATH)) {
        struct file *f = dirfd == AT_FDCWD ? nullptr : fd_get(dirfd);
        if (dirfd == AT_FDCWD) { vfs_ns_lock(); ino = curproc->cwd; iget(ino); vfs_ns_unlock(); }
        else if (f) { ino = f->inode; iget(ino); }
        else r = -EBADF;
    } else {
        r = dirfd_base(dirfd, path, &base);
        if (!r) r = vfs_lookup_at(base, path, !(flags & AT_SYMLINK_NOFOLLOW), &ino);
    }
    kfree(path);
    if (r) return r;
    struct kstat k;
    vfs_stat(ino, &k);
    iput(ino);
    struct {
        uint32_t mask, blksize; uint64_t attributes;
        uint32_t nlink, uid, gid; uint16_t mode, _p0;
        uint64_t ino, size, blocks, attributes_mask;
        struct { int64_t sec; uint32_t nsec; int32_t _r; } atime, btime, ctime, mtime;
        uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
        uint64_t spare[14];
    } sx;
    memset(&sx, 0, sizeof sx);
    sx.mask = 0x7ff; sx.blksize = k.blksize; sx.nlink = k.nlink; sx.uid = k.uid; sx.gid = k.gid;
    sx.mode = k.mode; sx.ino = k.ino; sx.size = k.size; sx.blocks = k.blocks;
    sx.atime.sec = k.atime.tv_sec; sx.atime.nsec = k.atime.tv_nsec;
    sx.mtime.sec = k.mtime.tv_sec; sx.mtime.nsec = k.mtime.tv_nsec;
    sx.ctime.sec = k.ctime.tv_sec; sx.ctime.nsec = k.ctime.tv_nsec;
    sx.btime = sx.ctime;
    sx.rdev_major = MAJOR(k.rdev); sx.rdev_minor = MINOR(k.rdev);
    sx.dev_major = 0; sx.dev_minor = k.dev;
    return copy_to_user(ubuf, &sx, sizeof sx);
}

struct gd_ctx { uint8_t *buf; size_t size, used; bool full; bool old; };
static int gd_fill(void *c, const char *name, size_t len, uint64_t ino, unsigned type) {
    struct gd_ctx *g = c;
    size_t reclen = ALIGN_UP((g->old ? 18 + 2 : 19) + len + 1, 8);
    if (g->used + reclen > g->size) { g->full = true; return 1; }
    uint8_t rec[512];
    memset(rec, 0, reclen);
    *(uint64_t *)rec = ino;
    *(int64_t *)(rec + 8) = g->used + reclen;
    *(uint16_t *)(rec + 16) = reclen;
    if (g->old) { memcpy(rec + 18, name, len); rec[reclen - 1] = type; }
    else { rec[18] = type; memcpy(rec + 19, name, len); }
    if (copy_to_user(g->buf + g->used, rec, reclen)) { g->full = true; return 1; }
    g->used += reclen;
    return 0;
}

static int64_t do_getdents(int fd, void *buf, size_t size, bool old) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!S_ISDIR(f->inode->mode)) return -ENOTDIR;
    if (!f->inode->iops || !f->inode->iops->iterate) return 0;
    struct gd_ctx g = { buf, size, 0, false, old };
    vfs_ns_lock();                        /* directory contents */
    uint64_t pos = f->pos;
    f->inode->iops->iterate(f->inode, &pos, gd_fill, &g);
    f->pos = pos;
    vfs_ns_unlock();
    if (g.full && !g.used) return -EINVAL;
    return g.used;
}
int64_t sys_getdents64(int fd, void *buf, size_t size) { return do_getdents(fd, buf, size, false); }
int64_t sys_getdents(int fd, void *buf, size_t size) { return do_getdents(fd, buf, size, true); }

int64_t sys_ioctl(int fd, uint64_t cmd, uint64_t arg) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (cmd == 0x5421) {           /* FIONBIO */
        int v;
        if (copy_from_user(&v, (void *)arg, sizeof v)) return -EFAULT;
        if (v) __atomic_fetch_or(&f->flags, O_NONBLOCK, __ATOMIC_RELAXED);
        else __atomic_fetch_and(&f->flags, ~O_NONBLOCK, __ATOMIC_RELAXED);
        return 0;
    }
    if (cmd == 0x5451) return fd_cloexec_set(fd, true);   /* FIOCLEX */
    if (cmd == 0x5450) return fd_cloexec_set(fd, false);  /* FIONCLEX */
    if (!f->fops || !f->fops->ioctl) return -ENOTTY;
    /* ioctl is dispatched lock-free; driver ioctls still run under the BKL */
    bool took = !bkl_held();
    if (took) bkl_enter();
    int64_t r = f->fops->ioctl(f, cmd, arg);
    if (took) bkl_exit();
    return r;
}

int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case F_DUPFD: case F_DUPFD_CLOEXEC: {
        if (arg >= MAX_FDS) return -EINVAL;
        int r = fd_alloc(file_get(f), (int)arg, cmd == F_DUPFD_CLOEXEC);
        if (r < 0) vfs_close(f);
        return r;
    }
    case F_GETFD: return fd_cloexec_get(fd) ? FD_CLOEXEC : 0;
    case F_SETFD: return fd_cloexec_set(fd, arg & FD_CLOEXEC);
    case F_GETFL: return f->flags;
    case F_SETFL: {                       /* atomic: lock-free readers/writers test O_NONBLOCK */
        uint32_t o = __atomic_load_n(&f->flags, __ATOMIC_RELAXED), n;
        do n = (o & ~(uint32_t)(O_APPEND | O_NONBLOCK)) | (uint32_t)(arg & (O_APPEND | O_NONBLOCK));
        while (!__atomic_compare_exchange_n(&f->flags, &o, n, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
        return 0;
    }
    case F_GETLK: {
        struct { int16_t type, whence; int64_t start, len; int32_t pid; } fl;
        if (copy_from_user(&fl, (void *)arg, sizeof fl)) return -EFAULT;
        fl.type = 2;   /* F_UNLCK */
        return copy_to_user((void *)arg, &fl, sizeof fl);
    }
    case F_SETLK: case F_SETLKW: return 0;
    default: return -EINVAL;
    }
}

int64_t sys_dup(int fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    int r = fd_alloc(file_get(f), 0, false);
    if (r < 0) vfs_close(f);
    return r;
}
int64_t sys_dup3(int old, int new, int flags) {
    struct file *f = fd_get(old);
    if (!f) return -EBADF;
    if (new < 0 || new >= MAX_FDS) return -EBADF;
    if (old == new) return -EINVAL;
    return fd_install(new, file_get(f), flags & O_CLOEXEC);
}
int64_t sys_dup2(int old, int new) {
    if (old == new) return fd_get(old) ? new : -EBADF;
    return sys_dup3(old, new, 0);
}

int64_t sys_pipe2(int *ufds, int flags) {
    struct file *r, *w;
    if (!pipe_create(&r, &w)) return -ENOMEM;
    r->flags |= flags & O_NONBLOCK; w->flags |= flags & O_NONBLOCK;
    int fds[2];
    fds[0] = fd_alloc(r, 0, flags & O_CLOEXEC);
    if (fds[0] < 0) { vfs_close(r); vfs_close(w); return fds[0]; }
    fds[1] = fd_alloc(w, 0, flags & O_CLOEXEC);
    if (fds[1] < 0) { fd_close(fds[0]); vfs_close(w); return fds[1]; }
    if (copy_to_user(ufds, fds, sizeof fds)) { fd_close(fds[0]); fd_close(fds[1]); return -EFAULT; }
    return 0;
}
int64_t sys_pipe(int *ufds) { return sys_pipe2(ufds, 0); }

int64_t sys_chdir(const char *upath) {
    WITH_PATH(upath, path);
    struct inode *i;
    int r = vfs_lookup(path, true, &i);
    kfree(path);
    if (r) return r;
    if (!S_ISDIR(i->mode)) { iput(i); return -ENOTDIR; }
    if ((r = inode_permission(i, MAY_EXEC))) { iput(i); return r; }
    vfs_ns_lock();
    iput(curproc->cwd);
    curproc->cwd = i;
    vfs_ns_unlock();
    return 0;
}
int64_t sys_fchdir(int fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!S_ISDIR(f->inode->mode)) return -ENOTDIR;
    int r = inode_permission(f->inode, MAY_EXEC);
    if (r) return r;
    iget(f->inode);
    vfs_ns_lock();
    iput(curproc->cwd);
    curproc->cwd = f->inode;
    vfs_ns_unlock();
    return 0;
}
int64_t sys_chroot(const char *upath) {
    if (!capable(CAP_SYS_CHROOT)) return -EPERM;
    WITH_PATH(upath, path);
    struct inode *i;
    int r = vfs_lookup(path, true, &i);
    kfree(path);
    if (r) return r;
    if (!S_ISDIR(i->mode)) { iput(i); return -ENOTDIR; }
    if ((r = inode_permission(i, MAY_EXEC))) { iput(i); return r; }
    vfs_ns_lock();
    iput(curproc->root);
    curproc->root = i;
    vfs_ns_unlock();
    return 0;
}

int64_t sys_getcwd(char *ubuf, size_t size) {
    char *k = kmalloc(PATH_MAX);
    vfs_ns_lock();
    int r = vfs_getcwd(curproc->cwd, k, MIN(size, PATH_MAX));
    vfs_ns_unlock();
    if (r > 0 && copy_to_user(ubuf, k, r)) r = -EFAULT;
    kfree(k);
    return r;
}

int64_t sys_mkdirat(int dirfd, const char *upath, uint32_t mode) {
    WITH_PATH(upath, path);
    struct inode *base;
    int64_t r = dirfd_base(dirfd, path, &base);
    if (!r) r = vfs_mkdir_at(base, path, mode);
    kfree(path);
    return r;
}
int64_t sys_mkdir(const char *p, uint32_t mode) { return sys_mkdirat(AT_FDCWD, p, mode); }

int64_t sys_mknodat(int dirfd, const char *upath, uint32_t mode, uint64_t dev) {
    WITH_PATH(upath, path);
    struct inode *base;
    int64_t r = dirfd_base(dirfd, path, &base);
    if (!(mode & S_IFMT)) mode |= S_IFREG;
    if (!r) r = vfs_mknod_at(base, path, mode & ~curproc->umask, dev);
    kfree(path);
    return r;
}
int64_t sys_mknod(const char *p, uint32_t mode, uint64_t dev) { return sys_mknodat(AT_FDCWD, p, mode, dev); }

int64_t sys_unlinkat(int dirfd, const char *upath, int flags) {
    WITH_PATH(upath, path);
    struct inode *base;
    int64_t r = dirfd_base(dirfd, path, &base);
    if (!r) r = vfs_unlink_at(base, path, flags & AT_REMOVEDIR);
    kfree(path);
    return r;
}
int64_t sys_unlink(const char *p) { return sys_unlinkat(AT_FDCWD, p, 0); }
int64_t sys_rmdir(const char *p) { return sys_unlinkat(AT_FDCWD, p, AT_REMOVEDIR); }

int64_t sys_symlinkat(const char *utarget, int dirfd, const char *upath) {
    WITH_PATH(upath, path);
    char *target = kmalloc(PATH_MAX);
    int64_t r = user_path(utarget, target);
    struct inode *base;
    if (!r) r = dirfd_base(dirfd, path, &base);
    if (!r) r = vfs_symlink_at(base, target, path);
    kfree(target); kfree(path);
    return r;
}
int64_t sys_symlink(const char *t, const char *p) { return sys_symlinkat(t, AT_FDCWD, p); }

int64_t sys_readlinkat(int dirfd, const char *upath, char *ubuf, size_t size) {
    WITH_PATH(upath, path);
    struct inode *base;
    char *k = kmalloc(PATH_MAX);
    int64_t r = dirfd_base(dirfd, path, &base);
    if (!r) r = vfs_readlink_at(base, path, k, MIN(size, PATH_MAX));
    if (r > 0 && copy_to_user(ubuf, k, r)) r = -EFAULT;
    kfree(k); kfree(path);
    return r;
}
int64_t sys_readlink(const char *p, char *b, size_t s) { return sys_readlinkat(AT_FDCWD, p, b, s); }

int64_t sys_linkat(int od, const char *uold, int nd, const char *unew, int flags) {
    WITH_PATH(uold, op);
    char *np = kmalloc(PATH_MAX);
    int64_t r = user_path(unew, np);
    struct inode *ob, *nb;
    if (!r) r = dirfd_base(od, op, &ob);
    if (!r) r = dirfd_base(nd, np, &nb);
    if (!r) r = vfs_link_at(ob, op, nb, np, flags & AT_SYMLINK_FOLLOW);
    kfree(np); kfree(op);
    return r;
}
int64_t sys_link(const char *o, const char *n) { return sys_linkat(AT_FDCWD, o, AT_FDCWD, n, 0); }

int64_t sys_renameat2(int od, const char *uold, int nd, const char *unew, unsigned flags) {
    WITH_PATH(uold, op);
    char *np = kmalloc(PATH_MAX);
    int64_t r = user_path(unew, np);
    struct inode *ob, *nb;
    if (!r) r = dirfd_base(od, op, &ob);
    if (!r) r = dirfd_base(nd, np, &nb);
    if (!r) r = vfs_rename_at(ob, op, nb, np);
    kfree(np); kfree(op);
    return r;
}
int64_t sys_renameat(int od, const char *o, int nd, const char *n) { return sys_renameat2(od, o, nd, n, 0); }
int64_t sys_rename(const char *o, const char *n) { return sys_renameat2(AT_FDCWD, o, AT_FDCWD, n, 0); }

/* access(2) checks with the real uid/gid (and, for a non-root real uid, no capabilities) unless
 * AT_EACCESS; the lookup itself also runs with those credentials, like Linux */
int64_t sys_faccessat2(int dirfd, const char *upath, int mode, int flags) {
    if (mode & ~7) return -EINVAL;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_EMPTY_PATH)) return -EINVAL;
    WITH_PATH(upath, path);
    const struct cred *c = current_cred();
    struct cred *ov = nullptr, *old = nullptr;
    if (!(flags & AT_EACCESS) && (c->fsuid != c->uid || c->fsgid != c->gid || (c->uid && c->cap_eff) || (!c->uid && c->cap_eff != c->cap_perm))) {
        ov = cred_prepare();
        if (!ov) { kfree(path); return -ENOMEM; }
        ov->fsuid = ov->uid; ov->fsgid = ov->gid;
        ov->cap_eff = ov->uid ? 0 : ov->cap_perm;
        old = cred_override(ov);
        cred_put(ov);
    }
    struct inode *base, *i;
    int64_t r = dirfd_base(dirfd, path, &base);
    if (!r) r = vfs_lookup_at(base, path, !(flags & AT_SYMLINK_NOFOLLOW), &i);
    kfree(path);
    if (!r) {
        int mask = (mode & 4 ? MAY_READ : 0) | (mode & 2 ? MAY_WRITE : 0) | (mode & 1 ? MAY_EXEC : 0);
        if (mask) r = inode_permission(i, mask);
        if (r == -EROFS && !S_ISREG(i->mode) && !S_ISDIR(i->mode) && !S_ISLNK(i->mode)) r = 0;
        iput(i);
    }
    if (ov) cred_revert(old);
    return r;
}
int64_t sys_faccessat(int d, const char *p, int m) { return sys_faccessat2(d, p, m, 0); }
int64_t sys_access(const char *p, int m) { return sys_faccessat2(AT_FDCWD, p, m, 0); }

static int64_t chmod_inode(struct inode *i, uint32_t mode) { return vfs_setattr_mode(i, mode); }
int64_t sys_fchmodat(int dirfd, const char *upath, uint32_t mode) {
    WITH_PATH(upath, path);
    struct inode *base, *i;
    int64_t r = dirfd_base(dirfd, path, &base);
    if (!r) r = vfs_lookup_at(base, path, true, &i);
    if (r) { kfree(path); return r; }
    r = chmod_inode(i, mode);
    if (!r) fsnotify_path(base, path, i, IN_ATTRIB);
    kfree(path);
    iput(i);
    return r;
}
int64_t sys_chmod(const char *p, uint32_t m) { return sys_fchmodat(AT_FDCWD, p, m); }
int64_t sys_fchmod(int fd, uint32_t m) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    int r = chmod_inode(f->inode, m);
    if (!r) fsnotify_file(f, IN_ATTRIB);
    return r;
}

/* uid_t arguments arrive zero-extended from musl (unsigned) while the riscv64 ABI expects
 * 32-bit values sign-extended in registers: take them as 64-bit and truncate explicitly */
int64_t sys_fchownat(int dirfd, const char *upath, uint64_t uid_, uint64_t gid_, int flags) {
    uint32_t uid = (uint32_t)uid_, gid = (uint32_t)gid_;
    WITH_PATH(upath, path);
    struct inode *base, *i;
    int64_t r = dirfd_base(dirfd, path, &base);
    if (!r) r = vfs_lookup_at(base, path, !(flags & AT_SYMLINK_NOFOLLOW), &i);
    if (r) { kfree(path); return r; }
    r = vfs_setattr_owner(i, uid, gid);
    if (!r) fsnotify_path(base, path, i, IN_ATTRIB);
    kfree(path);
    iput(i);
    return r;
}
int64_t sys_chown(const char *p, uint64_t u, uint64_t g) { return sys_fchownat(AT_FDCWD, p, u, g, 0); }
int64_t sys_lchown(const char *p, uint64_t u, uint64_t g) { return sys_fchownat(AT_FDCWD, p, u, g, AT_SYMLINK_NOFOLLOW); }
int64_t sys_fchown(int fd, uint64_t u, uint64_t g) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    int r = vfs_setattr_owner(f->inode, (uint32_t)u, (uint32_t)g);
    if (!r) fsnotify_file(f, IN_ATTRIB);
    return r;
}

int64_t sys_utimensat(int dirfd, const char *upath, const struct timespec *utimes, int flags) {
    struct inode *i;
    int64_t r;
    if (!upath) {
        struct file *f = fd_get(dirfd);
        if (!f) return -EBADF;
        i = f->inode; iget(i);
    } else {
        WITH_PATH(upath, path);
        struct inode *base;
        r = dirfd_base(dirfd, path, &base);
        if (!r) r = vfs_lookup_at(base, path, !(flags & AT_SYMLINK_NOFOLLOW), &i);
        if (!r) fsnotify_path(base, path, i, IN_ATTRIB);
        kfree(path);
        if (r) return r;
    }
    struct timespec ts[2], now = now_timespec();
    if (i->sb && (i->sb->flags & SB_RDONLY)) { iput(i); return -EROFS; }
    if (utimes) {
        if (copy_from_user(ts, utimes, sizeof ts)) { iput(i); return -EFAULT; }
        bool all_now = true, all_omit = true;
        for (int k = 0; k < 2; k++) {
            if (ts[k].tv_nsec != (1L << 30) - 1) all_now = false;
            if (ts[k].tv_nsec != (1L << 30) - 2) all_omit = false;
        }
        if (all_omit) { iput(i); return 0; }
        /* M31: explicit times need ownership; "now" also write access */
        if (!inode_owner_or_capable(i) && (!all_now || inode_permission(i, MAY_WRITE))) { iput(i); return all_now ? -EACCES : -EPERM; }
        for (int k = 0; k < 2; k++) {
            if (ts[k].tv_nsec == (1L << 30) - 1) ts[k] = now;           /* UTIME_NOW */
            else if (ts[k].tv_nsec == (1L << 30) - 2) ts[k] = k ? i->mtime : i->atime;  /* UTIME_OMIT */
        }
    } else {
        if (!inode_owner_or_capable(i) && inode_permission(i, MAY_WRITE)) { iput(i); return -EACCES; }
        ts[0] = ts[1] = now;
    }
    i->atime = ts[0]; i->mtime = ts[1]; i->ctime = now;
    mark_inode_dirty(i);
    if (!upath) fsnotify_inode(i, IN_ATTRIB);
    iput(i);
    return 0;
}

int64_t sys_ftruncate(int fd, off_t len) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (len < 0) return -EINVAL;
    if (!S_ISREG(f->inode->mode) || !f->inode->iops->truncate) return -EINVAL;
    if ((f->flags & O_ACCMODE) == O_RDONLY || (f->flags & O_PATH)) return -EINVAL;
    file_remove_privs(f);
    int r = f->inode->iops->truncate(f->inode, len);
    if (!r) fsnotify_file(f, IN_MODIFY_);
    return r;
}
int64_t sys_truncate(const char *upath, off_t len) {
    WITH_PATH(upath, path);
    struct inode *i;
    int64_t r = vfs_lookup(path, true, &i);
    kfree(path);
    if (r) return r;
    if (S_ISDIR(i->mode)) { iput(i); return -EISDIR; }
    if ((r = inode_permission(i, MAY_WRITE))) { iput(i); return r; }
    r = S_ISREG(i->mode) && i->iops->truncate ? i->iops->truncate(i, len) : -EINVAL;
    if (!r) fsnotify_inode(i, IN_MODIFY_);
    iput(i);
    return r;
}

int64_t sys_fsync(int fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    return vfs_fsync(f->inode, false);
}
int64_t sys_fdatasync(int fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    return vfs_fsync(f->inode, true);
}
int64_t sys_syncfs(int fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    vfs_sync_all();
    return 0;
}
int64_t sys_sync(void) { vfs_sync_all(); return 0; }

static int statfs_out(struct inode *i, void *ubuf) {
    struct kstatfs k;
    int r = vfs_statfs(i, &k);
    if (r) return r;
    struct { int64_t type, bsize; uint64_t blocks, bfree, bavail, files, ffree; int32_t fsid[2]; int64_t namelen, frsize, flags, spare[4]; } s;
    memset(&s, 0, sizeof s);
    s.type = (int64_t)k.type; s.bsize = s.frsize = (int64_t)k.bsize;
    s.blocks = k.blocks; s.bfree = k.bfree; s.bavail = k.bavail;
    s.files = k.files; s.ffree = k.ffree;
    s.namelen = (int64_t)k.namelen; s.flags = (int64_t)k.flags;
    if (i) { s.fsid[0] = (int32_t)i->dev; }
    return copy_to_user(ubuf, &s, sizeof s) ? -EFAULT : 0;
}
int64_t sys_statfs(const char *upath, void *ubuf) {
    WITH_PATH(upath, path);
    struct inode *i;
    int64_t r = vfs_lookup(path, true, &i);
    kfree(path);
    if (r) return r;
    r = statfs_out(i, ubuf);
    iput(i);
    return r;
}
int64_t sys_fstatfs(int fd, void *ubuf) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    return statfs_out(f->inode, ubuf);
}

int64_t sys_mount(const char *usrc, const char *utarget, const char *utype, uint64_t flags, const void *udata) {
    if (!capable(CAP_SYS_ADMIN)) return -EPERM;
    char *src = nullptr, *type = nullptr, *data = nullptr;
    char *target = kmalloc(4096);
    int64_t r = user_path(utarget, target);
    if (!r && usrc) { src = kmalloc(4096); r = user_path(usrc, src); if (r == -ENOENT) { src[0] = 0; r = 0; } }
    if (!r && utype) { type = kmalloc(64); r = strncpy_from_user(type, utype, 64) < 0 ? -EFAULT : 0; type[63] = 0; }
    if (!r && udata) { data = kmalloc(256); if (strncpy_from_user(data, udata, 256) < 0) { kfree(data); data = nullptr; } else data[255] = 0; }
    if (!r) r = vfs_do_mount(src, target, type, flags, data);
    kfree(src); kfree(type); kfree(data); kfree(target);
    return r;
}
int64_t sys_umount2(const char *utarget, int flags) {
    if (!capable(CAP_SYS_ADMIN)) return -EPERM;
    WITH_PATH(utarget, path);
    int64_t r = vfs_do_umount(path, flags);
    kfree(path);
    return r;
}

/* ---- poll / select ---- */

struct pollfd { int fd; int16_t events, revents; };

/*
 * Lock-free readiness scan (M24): each fd is pinned with fd_get_ref() so a concurrent close
 * cannot free it under us. ->poll methods marked nobkl (pipes, eventfd, AF_UNIX) run without
 * the BKL; the first fd whose poll still needs it takes the BKL for the rest of this scan.
 * Callers sleep on poll_seq, which every wake-up source bumps.
 */
static int poll_once(struct pollfd *pf, size_t n) {
    int count = 0;
    bool took = false;
    for (size_t i = 0; i < n; i++) {
        pf[i].revents = 0;
        if (pf[i].fd < 0) continue;
        struct file *f = fd_get_ref(pf[i].fd);
        if (!f) { pf[i].revents = POLLNVAL; count++; continue; }
        unsigned ev = POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM;
        if (f->fops && f->fops->poll) {
            if (!f->fops->nobkl && !took && !bkl_held()) { bkl_enter(); took = true; }
            ev = f->fops->poll(f);
        }
        vfs_close(f);
        pf[i].revents = ev & (pf[i].events | POLLERR | POLLHUP | POLLNVAL);
        if (pf[i].revents) count++;
    }
    if (took) bkl_exit();
    return count;
}

/* ppoll/pselect mask swap: signal_send() samples sig_mask under the BKL */
static void poll_set_mask(uint64_t m) {
    bkl_enter();
    current->sig_mask = m;
    bkl_exit();
}

static int64_t do_poll(struct pollfd *upf, size_t n, int64_t timeout_ns) {
    if (n > MAX_FDS * 4) return -EINVAL;
    struct pollfd *pf = kmalloc(sizeof(*pf) * (n ? n : 1));
    if (copy_from_user(pf, upf, sizeof(*pf) * n)) { kfree(pf); return -EFAULT; }
    uint64_t deadline = timeout_ns < 0 ? UINT64_MAX : time_ns() + timeout_ns;
    int64_t r;
    for (;;) {
        uint64_t pseq = poll_seq_read();
        r = poll_once(pf, n);
        if (r || timeout_ns == 0) break;
        uint64_t now = time_ns();
        if (now >= deadline) { r = 0; break; }
        int w = poll_wait_seq(pseq, deadline == UINT64_MAX ? UINT64_MAX : deadline - now);
        if (w == -EINTR) { r = -EINTR; break; }
    }
    if (r >= 0 && copy_to_user(upf, pf, sizeof(*pf) * n)) r = -EFAULT;
    kfree(pf);
    return r;
}

int64_t sys_poll(struct pollfd *upf, size_t n, int timeout_ms) {
    return do_poll(upf, n, timeout_ms < 0 ? -1 : (int64_t)timeout_ms * 1000000);
}

int64_t sys_ppoll(struct pollfd *upf, size_t n, const struct timespec *uts, const uint64_t *usig, size_t sz) {
    int64_t t = -1;
    if (uts) {
        struct timespec ts;
        if (copy_from_user(&ts, uts, sizeof ts)) return -EFAULT;
        t = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }
    uint64_t old = current->sig_mask;
    if (usig) {
        uint64_t m;
        if (copy_from_user(&m, usig, 8)) return -EFAULT;
        poll_set_mask(m);
    }
    int64_t r = do_poll(upf, n, t);
    if (usig) {
        if (r == -EINTR) { current->saved_mask = old; current->restore_mask = true; }
        else poll_set_mask(old);
    }
    return r;
}

static int64_t do_select(int nfds, uint64_t *ur, uint64_t *uw, uint64_t *ue, int64_t timeout_ns) {
    if (nfds < 0 || nfds > MAX_FDS) return -EINVAL;
    uint64_t r[4] = {0}, w[4] = {0}, e[4] = {0};
    size_t bytes = ALIGN_UP((size_t)nfds, 64) / 8;
    if (ur && copy_from_user(r, ur, bytes)) return -EFAULT;
    if (uw && copy_from_user(w, uw, bytes)) return -EFAULT;
    if (ue && copy_from_user(e, ue, bytes)) return -EFAULT;
    struct pollfd *pf = kmalloc(sizeof(*pf) * (nfds ? nfds : 1));
    int n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int16_t ev = 0;
        if (r[fd / 64] & (1ULL << (fd % 64))) ev |= POLLIN;
        if (w[fd / 64] & (1ULL << (fd % 64))) ev |= POLLOUT;
        if (e[fd / 64] & (1ULL << (fd % 64))) ev |= POLLPRI;
        if (ev) { pf[n].fd = fd; pf[n].events = ev; n++; }
    }
    uint64_t deadline = timeout_ns < 0 ? UINT64_MAX : time_ns() + timeout_ns;
    int64_t res;
    for (;;) {
        uint64_t pseq = poll_seq_read();
        res = poll_once(pf, n);
        if (res || timeout_ns == 0) break;
        uint64_t now = time_ns();
        if (now >= deadline) { res = 0; break; }
        int wr = poll_wait_seq(pseq, deadline == UINT64_MAX ? UINT64_MAX : deadline - now);
        if (wr == -EINTR) { res = -EINTR; break; }
    }
    if (res >= 0) {
        uint64_t orr[4] = {0}, ow[4] = {0}, oe[4] = {0};
        res = 0;
        for (int i = 0; i < n; i++) {
            int fd = pf[i].fd;
            if (pf[i].revents & POLLNVAL) { kfree(pf); return -EBADF; }
            if ((pf[i].events & POLLIN) && (pf[i].revents & (POLLIN | POLLHUP | POLLERR))) { orr[fd / 64] |= 1ULL << (fd % 64); res++; }
            if ((pf[i].events & POLLOUT) && (pf[i].revents & (POLLOUT | POLLERR))) { ow[fd / 64] |= 1ULL << (fd % 64); res++; }
            if ((pf[i].events & POLLPRI) && (pf[i].revents & POLLPRI)) { oe[fd / 64] |= 1ULL << (fd % 64); res++; }
        }
        if (ur) copy_to_user(ur, orr, bytes);
        if (uw) copy_to_user(uw, ow, bytes);
        if (ue) copy_to_user(ue, oe, bytes);
    }
    kfree(pf);
    return res;
}

int64_t sys_select(int nfds, uint64_t *r, uint64_t *w, uint64_t *e, int64_t *utv) {
    int64_t t = -1;
    if (utv) {
        int64_t tv[2];
        if (copy_from_user(tv, utv, sizeof tv)) return -EFAULT;
        t = tv[0] * 1000000000LL + tv[1] * 1000;
    }
    return do_select(nfds, r, w, e, t);
}

int64_t sys_pselect6(int nfds, uint64_t *r, uint64_t *w, uint64_t *e, const struct timespec *uts, const uint64_t *usig) {
    int64_t t = -1;
    if (uts) {
        struct timespec ts;
        if (copy_from_user(&ts, uts, sizeof ts)) return -EFAULT;
        t = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }
    uint64_t old = current->sig_mask;
    uint64_t sigdata[2];
    if (usig && !copy_from_user(sigdata, usig, 16) && sigdata[0]) {
        uint64_t m;
        if (copy_from_user(&m, (void *)sigdata[0], 8)) return -EFAULT;
        poll_set_mask(m);
    }
    int64_t res = do_select(nfds, r, w, e, t);
    if (current->sig_mask != old) {
        if (res == -EINTR) { current->saved_mask = old; current->restore_mask = true; }
        else poll_set_mask(old);
    }
    return res;
}
