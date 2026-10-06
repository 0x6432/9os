/* Character device registry and simple devices (null, zero, random). */
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include <kernel/printk.h>
#include <kernel/mm.h>

struct chrdev { unsigned major, minor; const struct file_ops *ops; };
static struct chrdev chrdevs[32];
static int nchrdevs;

void chrdev_register(unsigned major, unsigned minor, const struct file_ops *ops) {
    if (nchrdevs < 32) chrdevs[nchrdevs++] = (struct chrdev){ major, minor, ops };
}
const struct file_ops *chrdev_get(uint64_t rdev) {
    for (int i = 0; i < nchrdevs; i++)
        if (chrdevs[i].major == MAJOR(rdev) && (chrdevs[i].minor == MINOR(rdev) || chrdevs[i].minor == CHRDEV_ANY_MINOR)) return chrdevs[i].ops;
    return nullptr;
}

/*
 * null/zero/random run without the BKL (file_ops.nobkl): the buffer may be a user pointer,
 * so output is staged in a small stack buffer and copied out with copy_to_user (kernel
 * callers, e.g. exec of /dev/zero mappings, pass kernel pointers and get a memcpy).
 */
static int dev_copy_out(void *dst, const void *src, size_t n) {
    if ((uintptr_t)dst < USER_TOP) return copy_to_user(dst, src, n) ? -EFAULT : 0;
    memcpy(dst, src, n);
    return 0;
}
typedef void (*fill_fn)(uint8_t *, size_t);
static ssize_t fill_read(void *b, size_t n, fill_fn fill) {
    uint8_t tmp[256];
    size_t done = 0;
    while (done < n) {
        size_t c = MIN(sizeof tmp, n - done);
        fill(tmp, c);
        if (dev_copy_out((uint8_t *)b + done, tmp, c)) return done ? (ssize_t)done : -EFAULT;
        done += c;
    }
    return done;
}
static void fill_zero(uint8_t *p, size_t n) { memset(p, 0, n); }

/* lock-free splitmix64 over an atomic counter, perturbed by the clock */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
void rng_mix(uint64_t v) {
    __atomic_fetch_xor(&rng_state, v * 0xff51afd7ed558ccdULL, __ATOMIC_RELAXED);
}
uint64_t random_u64(void) {
    uint64_t x = __atomic_add_fetch(&rng_state, 0x9e3779b97f4a7c15ULL, __ATOMIC_RELAXED) ^ time_ns();
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
static void fill_random(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i += 8) {
        uint64_t r = random_u64();
        memcpy(p + i, &r, MIN(8, n - i));
    }
}

static ssize_t null_read(struct file *f, void *b, size_t n, off_t *o) { return 0; }
static ssize_t null_write(struct file *f, const void *b, size_t n, off_t *o) { return n; }
static ssize_t zero_read(struct file *f, void *b, size_t n, off_t *o) { return fill_read(b, n, fill_zero); }
static ssize_t random_read(struct file *f, void *b, size_t n, off_t *o) { return fill_read(b, n, fill_random); }
static unsigned always_ready(struct file *f) { return POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM; }

static const struct file_ops null_ops = { .nobkl = true, .read = null_read, .write = null_write, .poll = always_ready };
static const struct file_ops zero_ops = { .nobkl = true, .read = zero_read, .write = null_write, .poll = always_ready };
static const struct file_ops random_ops = { .nobkl = true, .read = random_read, .write = null_write, .poll = always_ready };

void tty_register_devices(void);

void devices_init(void) {
    chrdev_register(1, 3, &null_ops);
    chrdev_register(1, 5, &zero_ops);
    chrdev_register(1, 8, &random_ops);
    chrdev_register(1, 9, &random_ops);
    tty_register_devices();

    vfs_mkdir_at(nullptr, "/dev", 0755);
    static const struct { const char *name; unsigned ma, mi; uint32_t mode; } nodes[] = {
        { "/dev/null", 1, 3, 0666 }, { "/dev/zero", 1, 5, 0666 }, { "/dev/random", 1, 8, 0666 },
        { "/dev/urandom", 1, 9, 0666 }, { "/dev/tty", 5, 0, 0666 }, { "/dev/console", 5, 1, 0620 },
        { "/dev/tty0", 4, 0, 0620 }, { "/dev/ptmx", 5, 2, 0666 }, { "/dev/tty1", 4, 1, 0620 }, { "/dev/ttyS0", 4, 64, 0660 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++)
        vfs_mknod_at(nullptr, nodes[i].name, S_IFCHR | nodes[i].mode, MKDEV(nodes[i].ma, nodes[i].mi));
    vfs_mkdir_at(nullptr, "/dev/pts", 0755);
    vfs_mknod_at(nullptr, "/dev/shm", S_IFDIR | 01777, 0);
    vfs_symlink_at(nullptr, "/proc/self/fd", "/dev/fd");
    vfs_symlink_at(nullptr, "/proc/self/fd/0", "/dev/stdin");
    vfs_symlink_at(nullptr, "/proc/self/fd/1", "/dev/stdout");
    vfs_symlink_at(nullptr, "/proc/self/fd/2", "/dev/stderr");
    vfs_mknod_at(nullptr, "/tmp", S_IFDIR | 01777, 0);
}
