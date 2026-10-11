/* M35+: loop devices and block hot-plug. /dev/loop-control GET_FREE/ADD/REMOVE create and
 * remove /dev/loopN with add/remove uevents and /sys/block/loopN; LOOP_SET_FD/CONFIGURE bind a
 * file (size, sysfs loop/backing_file, "change" uevent, read-only, offset); data written
 * through the loop reaches the file and vice versa; LOOP_GET/SET_STATUS64; removing a bound
 * device is EBUSY; CLR_FD on a device that is still open detaches at the last close
 * (autoclear); an ext2 filesystem on a loop device mounts; busybox losetup works. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("looptest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)

static int rd(const char *p, char *b, size_t cap) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, b, cap - 1);
    close(fd);
    if (n < 0) return -1;
    b[n] = 0;
    if (n && b[n - 1] == '\n') b[--n] = 0;
    return n;
}
static int uev;
/* wait for "ACTION@...DEVPATH ending in /name" */
static bool uevent_seen(const char *action, const char *name) {
    char b[4096], want[64];
    snprintf(want, sizeof want, "%s@", action);
    for (int i = 0; i < 50; i++) {
        struct pollfd p = { uev, POLLIN, 0 };
        if (poll(&p, 1, 2000) <= 0) return false;
        ssize_t n = recv(uev, b, sizeof b - 1, 0);
        if (n <= 0) return false;
        b[n] = 0;
        size_t l = strlen(b), nl = strlen(name);
        if (!strncmp(b, want, strlen(want)) && l > nl && !strcmp(b + l - nl, name) && b[l - nl - 1] == '/') return true;
    }
    return false;
}
static void drain(void) { char b[4096]; while (recv(uev, b, sizeof b, MSG_DONTWAIT) > 0) {} }

int main(void) {
    uev = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 };
    CHECK(uev >= 0 && bind(uev, (struct sockaddr *)&a, sizeof a) == 0, "uevent socket");
    /* backing file: 4 MiB, sector k starts with "S<k>" */
    int bf = open("/tmp/loop.img", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(bf >= 0 && ftruncate(bf, 4 << 20) == 0, "backing file");
    char sec[512];
    for (int k = 0; k < 8192; k += 97) { memset(sec, 0, sizeof sec); snprintf(sec, sizeof sec, "S%d", k); pwrite(bf, sec, sizeof sec, (off_t)k * 512); }
    int ctl = open("/dev/loop-control", O_RDWR);
    CHECK(ctl >= 0, "loop-control");
    drain();
    int n = ioctl(ctl, LOOP_CTL_GET_FREE);
    CHECK(n >= 0, "GET_FREE %d", n);
    char name[16], dev[32], p[128], b[512];
    snprintf(name, sizeof name, "loop%d", n);
    snprintf(dev, sizeof dev, "/dev/loop%d", n);
    CHECK(uevent_seen("add", name), "add uevent");
    struct stat st;
    CHECK(stat(dev, &st) == 0 && S_ISBLK(st.st_mode) && major(st.st_rdev) == 7 && minor(st.st_rdev) == (unsigned)n, "node %s", dev);
    snprintf(p, sizeof p, "/sys/block/%s/size", name);
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "0"), "unbound size %s", b);
    CHECK(ioctl(ctl, LOOP_CTL_GET_FREE) == n, "GET_FREE returns the same free device");

    int lf = open(dev, O_RDWR);
    CHECK(lf >= 0, "open %s", dev);
    struct loop_info64 li;
    CHECK(ioctl(lf, LOOP_GET_STATUS64, &li) < 0 && errno == ENXIO, "status unbound");
    CHECK(ioctl(lf, LOOP_SET_FD, bf) == 0, "SET_FD");
    CHECK(uevent_seen("change", name), "change uevent");
    CHECK(ioctl(lf, LOOP_SET_FD, bf) < 0 && errno == EBUSY, "SET_FD twice");
    uint64_t sz = 0;
    CHECK(ioctl(lf, BLKGETSIZE64, &sz) == 0 && sz == 4 << 20, "size %llu", (unsigned long long)sz);
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "8192"), "sysfs size %s", b);
    snprintf(p, sizeof p, "/sys/block/%s/loop/backing_file", name);
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "/tmp/loop.img"), "backing_file %s", b);
    CHECK(ioctl(lf, LOOP_GET_STATUS64, &li) == 0 && li.lo_number == (unsigned)n && !strcmp((char *)li.lo_file_name, "/tmp/loop.img"), "GET_STATUS64");
    CHECK(pread(lf, sec, 512, 970 * 512) == 512 && !strcmp(sec, "S970"), "read through loop '%s'", sec);
    memset(sec, 0, sizeof sec); strcpy(sec, "written via loop");
    CHECK(pwrite(lf, sec, 512, 5 * 512) == 512 && ioctl(lf, BLKFLSBUF) == 0, "write through loop");
    memset(sec, 0, sizeof sec);
    CHECK(pread(bf, sec, 512, 5 * 512) == 512 && !strcmp(sec, "written via loop"), "reached the file '%s'", sec);
    /* SET_STATUS64: offset */
    li.lo_offset = 97 * 512;
    CHECK(ioctl(lf, LOOP_SET_STATUS64, &li) == 0 && ioctl(lf, BLKGETSIZE64, &sz) == 0 && sz == (4 << 20) - 97 * 512, "offset size %llu", (unsigned long long)sz);
    CHECK(pread(lf, sec, 512, 0) == 512 && !strcmp(sec, "S97"), "offset read '%s'", sec);
    li.lo_offset = 0;
    ioctl(lf, LOOP_SET_STATUS64, &li);
    CHECK(ioctl(ctl, LOOP_CTL_REMOVE, n) < 0 && errno == EBUSY, "remove bound device");
    int ng = ioctl(ctl, LOOP_CTL_GET_FREE);
    CHECK(ng >= 0 && ng != n, "GET_FREE skips the bound device (%d)", ng);

    /* ext2 on the loop device */
    if (!access("/sbin/mke2fs", X_OK) || !access("/bin/mke2fs", X_OK) || !access("/usr/sbin/mke2fs", X_OK)) {
        char cmd[96];
        snprintf(cmd, sizeof cmd, "mke2fs -q %s >/dev/null 2>&1", dev);
        CHECK(system(cmd) == 0, "mke2fs");
        mkdir("/tmp/loopmnt", 0755);
        CHECK(mount(dev, "/tmp/loopmnt", "ext2", 0, NULL) == 0, "mount ext2 on %s", dev);
        int wf = open("/tmp/loopmnt/hello", O_WRONLY | O_CREAT, 0644);
        CHECK(wf >= 0 && write(wf, "hi\n", 3) == 3, "file on loop fs");
        if (wf >= 0) close(wf);
        CHECK(ioctl(ctl, LOOP_CTL_REMOVE, n) < 0 && errno == EBUSY, "remove mounted");
        CHECK(umount("/tmp/loopmnt") == 0, "umount");
        CHECK(mount(dev, "/tmp/loopmnt", "ext2", 0, NULL) == 0 && rd("/tmp/loopmnt/hello", b, sizeof b) == 2 && !strcmp(b, "hi"), "remount: %s", b);
        umount("/tmp/loopmnt");
    }
    /* detach and hot-unplug */
    CHECK(ioctl(lf, LOOP_CLR_FD) == 0 && ioctl(lf, BLKGETSIZE64, &sz) == 0 && sz == 0, "CLR_FD");
    CHECK(ioctl(ctl, LOOP_CTL_REMOVE, n) < 0 && errno == EBUSY, "remove while open");
    close(lf);
    drain();
    CHECK(ioctl(ctl, LOOP_CTL_REMOVE, n) == 0, "REMOVE");
    CHECK(uevent_seen("remove", name), "remove uevent");
    snprintf(p, sizeof p, "/sys/block/%s", name);
    CHECK(access(dev, F_OK) < 0 && access(p, F_OK) < 0, "node and sysfs gone");
    CHECK(ioctl(ctl, LOOP_CTL_REMOVE, n) < 0 && errno == ENODEV, "remove twice");
    CHECK(ioctl(ctl, LOOP_CTL_ADD, n) == n && access(dev, F_OK) == 0, "ADD again");

    /* autoclear: CLR_FD with two openers detaches at the last close */
    int l1 = open(dev, O_RDWR), l2 = open(dev, O_RDONLY);
    struct loop_config cfg = { .fd = (uint32_t)bf, .info = { .lo_flags = LO_FLAGS_READ_ONLY } };
    CHECK(l1 >= 0 && l2 >= 0 && ioctl(l1, LOOP_CONFIGURE, &cfg) == 0, "CONFIGURE");
    snprintf(p, sizeof p, "/sys/block/%s/ro", name);
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "1") && pwrite(l1, sec, 512, 0) < 0 && errno == EROFS, "read-only");
    CHECK(ioctl(l1, LOOP_CLR_FD) == 0 && ioctl(l2, LOOP_GET_STATUS64, &li) == 0 && (li.lo_flags & LO_FLAGS_AUTOCLEAR), "deferred clear");
    close(l1);
    CHECK(ioctl(l2, LOOP_GET_STATUS64, &li) == 0, "still bound with one opener");
    close(l2);
    l1 = open(dev, O_RDONLY);
    CHECK(l1 >= 0 && ioctl(l1, LOOP_GET_STATUS64, &li) < 0 && errno == ENXIO, "cleared at the last close");
    close(l1);
    /* busybox losetup */
    if (!access("/sbin/losetup", X_OK)) {
        FILE *pf = popen("losetup -f 2>&1", "r");
        char out[64] = "";
        if (pf) { fgets(out, sizeof out, pf); pclose(pf); }
        out[strcspn(out, "\n")] = 0;
        CHECK(!strncmp(out, "/dev/loop", 9), "losetup -f: %s", out);
        char cmd[256];
        snprintf(cmd, sizeof cmd, "losetup %s /tmp/loop.img && losetup %s | grep -q /tmp/loop.img && losetup -d %s", out, out, out);
        CHECK(system(cmd) == 0, "losetup show/detach");
    }
    for (int i = 0; i < 16; i++) ioctl(ctl, LOOP_CTL_REMOVE, i);
    close(ctl);
    close(bf);
    unlink("/tmp/loop.img");
    if (fails) { printf("looptest: %d failures\n", fails); return 1; }
    printf("looptest: all passed\n");
    return 0;
}
