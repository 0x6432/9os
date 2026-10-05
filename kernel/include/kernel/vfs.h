#pragma once
#include <kernel/types.h>
#include <kernel/sched.h>

#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

#define O_ACCMODE 3
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_NOCTTY 0400
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_SYNC 04010000
#if defined(__aarch64__)
#define O_DIRECTORY 040000
#define O_NOFOLLOW 0100000
#define O_LARGEFILE 0400000
#else
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_LARGEFILE 0100000
#endif
#define O_CLOEXEC 02000000
#define O_PATH 010000000

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EMPTY_PATH 0x1000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100

#define MKDEV(ma, mi) (((uint64_t)(ma) << 8) | (mi))
#define MAJOR(d) (((d) >> 8) & 0xfff)
#define MINOR(d) ((d) & 0xff)

struct timespec { int64_t tv_sec, tv_nsec; };

struct inode;
struct file;

typedef int (*filldir_t)(void *ctx, const char *name, size_t len, uint64_t ino, unsigned type);

struct inode_ops {
    int (*lookup)(struct inode *dir, const char *name, struct inode **out);
    int (*create)(struct inode *dir, const char *name, uint32_t mode, uint64_t rdev, struct inode **out);
    int (*unlink)(struct inode *dir, const char *name, bool rmdir);
    int (*symlink)(struct inode *dir, const char *name, const char *target);
    int (*readlink)(struct inode *ino, char *buf, size_t size);
    int (*link)(struct inode *dir, const char *name, struct inode *target);
    int (*rename)(struct inode *odir, const char *oname, struct inode *ndir, const char *nname);
    int (*truncate)(struct inode *ino, uint64_t size);
    /* iterate directory entries starting at *pos; stop when filldir returns non-zero */
    int (*iterate)(struct inode *dir, uint64_t *pos, filldir_t fill, void *ctx);
    void (*evict)(struct inode *ino);
    /* magic links (procfs): resolve directly to an inode */
    int (*follow_link)(struct inode *ino, struct inode **out);
};

struct file_ops {
    bool nobkl;             /* read/write are safe without the BKL (see pipe.c) */
    int (*open)(struct inode *ino, struct file *f);
    ssize_t (*read)(struct file *f, void *buf, size_t n, off_t *off);
    ssize_t (*write)(struct file *f, const void *buf, size_t n, off_t *off);
    int (*ioctl)(struct file *f, uint64_t cmd, uint64_t arg);
    unsigned (*poll)(struct file *f);
    void (*release)(struct file *f);
    /* device memory mapping: physical address backing [off, off+len), or -errno */
    int (*mmap)(struct file *f, uint64_t off, size_t len, paddr_t *pa);
    /* MAP_SHARED of page-cache backed files: page at pgoff (caller takes a reference) */
    int (*mmap_page)(struct file *f, uint64_t pgoff, paddr_t *pa);
};

struct inode {
    uint32_t mode, uid, gid, nlink;
    uint64_t ino, size, rdev, dev;
    struct timespec atime, mtime, ctime;
    const struct inode_ops *iops;
    const struct file_ops *fops;
    struct inode *parent;      /* directories: parent directory ("..") */
    struct inode *mounted;     /* mount point: root of mounted filesystem */
    struct inode *covered;     /* fs root: the mount point it covers */
    void *priv;
    int refcount;
};

struct file {
    struct inode *inode;
    const struct file_ops *fops;
    off_t pos;
    uint32_t flags;
    int refcount;
    void *priv;
    char *path;               /* path used at open (for /proc/pid/fd) */
};

struct kstat {
    uint64_t dev, ino, nlink, rdev;
    uint32_t mode, uid, gid;
    int64_t size, blksize, blocks;
    struct timespec atime, mtime, ctime;
};

extern struct inode *vfs_root;
/* global wait queue for poll/select: woken on any I/O readiness change */
extern struct wait_queue poll_wq;
void poll_notify(void);
extern uint64_t poll_seq;
int poll_wait_seq(uint64_t seq, uint64_t ns);
#define poll_seq_read() __atomic_load_n(&poll_seq, __ATOMIC_SEQ_CST)

/* inotify hooks (kernel/fs/anonfd.c); free while nobody watches anything */
extern int fsnotify_nwatches;
void fsnotify_dirent_(struct inode *dir, const char *name, uint32_t mask, bool isdir, uint32_t cookie);
void fsnotify_inode_(struct inode *i, uint32_t mask);
void fsnotify_file_(struct file *f, uint32_t mask);
void fsnotify_unlinked_(struct inode *i);
void fsnotify_path_(struct inode *base, const char *path, struct inode *i, uint32_t mask);
#define fsnotify_path(b, p, i, m) do { if (fsnotify_nwatches) fsnotify_path_(b, p, i, m); } while (0)
uint32_t fsnotify_cookie(void);
#define fsnotify_dirent(d, n, m, isdir, c) do { if (fsnotify_nwatches) fsnotify_dirent_(d, n, m, isdir, c); } while (0)
#define fsnotify_inode(i, m) do { if (fsnotify_nwatches) fsnotify_inode_(i, m); } while (0)
#define fsnotify_file(f, m) do { if (fsnotify_nwatches) fsnotify_file_(f, m); } while (0)
#define fsnotify_unlinked(i) do { if (fsnotify_nwatches) fsnotify_unlinked_(i); } while (0)
#define IN_ACCESS 0x1
#define IN_MODIFY_ 0x2
#define IN_ATTRIB 0x4
#define IN_CLOSE_WRITE_ 0x8
#define IN_CLOSE_NOWRITE 0x10
#define IN_OPEN 0x20
#define IN_MOVED_FROM 0x40
#define IN_MOVED_TO 0x80
#define IN_CREATE 0x100
#define IN_DELETE 0x200
#define IN_MOVE_SELF 0x800

void vfs_init(void);
struct inode *inode_alloc(uint32_t mode);
void iget(struct inode *i);
void iput(struct inode *i);
struct timespec now_timespec(void);

/* Path resolution relative to dirfd-style base (nullptr = cwd). */
int vfs_lookup_at(struct inode *base, const char *path, bool follow, struct inode **out);
int vfs_lookup(const char *path, bool follow, struct inode **out);
int vfs_lookup_parent_at(struct inode *base, const char *path, struct inode **dir, char *last);

int vfs_open_at(struct inode *base, const char *path, int flags, uint32_t mode, struct file **out);
int vfs_open(const char *path, int flags, uint32_t mode, struct file **out);
struct file *file_open_inode(struct inode *ino, int flags);
#define CHRDEV_ANY_MINOR 0xffffffffu
void vfs_close(struct file *f);
static inline struct file *file_get(struct file *f) { __atomic_add_fetch(&f->refcount, 1, __ATOMIC_RELAXED); return f; }
ssize_t vfs_read(struct file *f, void *buf, size_t n);
ssize_t vfs_write(struct file *f, const void *buf, size_t n);
ssize_t vfs_pread(struct file *f, void *buf, size_t n, off_t off);
int vfs_mknod_at(struct inode *base, const char *path, uint32_t mode, uint64_t rdev);
int vfs_mkdir_at(struct inode *base, const char *path, uint32_t mode);
int vfs_unlink_at(struct inode *base, const char *path, bool rmdir);
int vfs_symlink_at(struct inode *base, const char *target, const char *path);
int vfs_link_at(struct inode *ob, const char *opath, struct inode *nb, const char *npath, bool follow);
int vfs_rename_at(struct inode *ob, const char *opath, struct inode *nb, const char *npath);
int vfs_readlink_at(struct inode *base, const char *path, char *buf, size_t size);
void vfs_stat(struct inode *i, struct kstat *st);
int vfs_mount(const char *path, struct inode *root);
int vfs_getcwd(struct inode *cwd, char *buf, size_t size);

/* filesystems */
struct inode *tmpfs_create_root(void);
void initramfs_load(void);
struct inode *pipe_create(struct file **rd, struct file **wr);
struct inode *procfs_create_root(void);

/* character devices */
void chrdev_register(unsigned major, unsigned minor, const struct file_ops *ops);
const struct file_ops *chrdev_get(uint64_t rdev);
void devices_init(void);
