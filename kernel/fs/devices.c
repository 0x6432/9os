/* Character device registry and simple devices (null, zero, random). */
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include <kernel/printk.h>

struct chrdev { unsigned major, minor; const struct file_ops *ops; };
static struct chrdev chrdevs[32];
static int nchrdevs;

void chrdev_register(unsigned major, unsigned minor, const struct file_ops *ops) {
    if (nchrdevs < 32) chrdevs[nchrdevs++] = (struct chrdev){ major, minor, ops };
}
const struct file_ops *chrdev_get(uint64_t rdev) {
    for (int i = 0; i < nchrdevs; i++)
        if (chrdevs[i].major == MAJOR(rdev) && chrdevs[i].minor == MINOR(rdev)) return chrdevs[i].ops;
    return nullptr;
}

static ssize_t null_read(struct file *f, void *b, size_t n, off_t *o) { return 0; }
static ssize_t null_write(struct file *f, const void *b, size_t n, off_t *o) { return n; }
static ssize_t zero_read(struct file *f, void *b, size_t n, off_t *o) { memset(b, 0, n); return n; }
static unsigned always_ready(struct file *f) { return POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM; }

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
uint64_t random_u64(void) {
    rng_state ^= time_ns();
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static ssize_t random_read(struct file *f, void *b, size_t n, off_t *o) {
    uint8_t *p = b;
    for (size_t i = 0; i < n; i += 8) {
        uint64_t r = random_u64();
        memcpy(p + i, &r, MIN(8, n - i));
    }
    return n;
}

static const struct file_ops null_ops = { .read = null_read, .write = null_write, .poll = always_ready };
static const struct file_ops zero_ops = { .read = zero_read, .write = null_write, .poll = always_ready };
static const struct file_ops random_ops = { .read = random_read, .write = null_write, .poll = always_ready };

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
        { "/dev/tty0", 4, 0, 0620 }, { "/dev/tty1", 4, 1, 0620 }, { "/dev/ttyS0", 4, 64, 0660 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++)
        vfs_mknod_at(nullptr, nodes[i].name, S_IFCHR | nodes[i].mode, MKDEV(nodes[i].ma, nodes[i].mi));
    vfs_mkdir_at(nullptr, "/dev/pts", 0755);
    vfs_mkdir_at(nullptr, "/dev/shm", 01777);
    vfs_symlink_at(nullptr, "/proc/self/fd", "/dev/fd");
    vfs_symlink_at(nullptr, "/proc/self/fd/0", "/dev/stdin");
    vfs_symlink_at(nullptr, "/proc/self/fd/1", "/dev/stdout");
    vfs_symlink_at(nullptr, "/proc/self/fd/2", "/dev/stderr");
    vfs_mkdir_at(nullptr, "/tmp", 01777);
}
