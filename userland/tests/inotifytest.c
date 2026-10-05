/* inotifytest: create/modify/close/attrib/move/delete events, IN_ONESHOT, rm_watch, poll, FIONREAD. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("  FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); fails++; } } while (0)

struct ev { int wd; unsigned mask, cookie; char name[64]; };
static struct ev evs[64]; static int nev;

static void drain(int fd) {
    char buf[4096] __attribute__((aligned(8)));
    nev = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (nev < 64) { evs[nev].wd = e->wd; evs[nev].mask = e->mask; evs[nev].cookie = e->cookie;
                snprintf(evs[nev].name, 64, "%s", e->len ? e->name : ""); nev++; }
            if (getenv("V")) printf("    wd %d mask %#x cookie %u name '%s'\n", e->wd, e->mask, e->cookie, e->len ? e->name : "");
            p += sizeof *e + e->len;
        }
    }
}
static int has(int wd, unsigned mask, const char *name) {
    for (int i = 0; i < nev; i++)
        if (evs[i].wd == wd && (evs[i].mask & mask) == mask && (!name || !strcmp(evs[i].name, name))) return i + 1;
    return 0;
}

int main(void) {
    mkdir("/tmp/in", 0755);
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    int wd = inotify_add_watch(fd, "/tmp/in", IN_ALL_EVENTS);
    CHECK(wd > 0);
    struct pollfd p = { fd, POLLIN, 0 };
    CHECK(poll(&p, 1, 0) == 0);

    int f = open("/tmp/in/a.txt", O_CREAT | O_WRONLY, 0644);
    write(f, "hello", 5); write(f, "!", 1);
    close(f);
    CHECK(poll(&p, 1, 100) == 1);
    int avail = 0; CHECK(ioctl(fd, FIONREAD, &avail) == 0 && avail > 0);
    drain(fd);
    CHECK(has(wd, IN_CREATE, "a.txt"));
    CHECK(has(wd, IN_OPEN, "a.txt"));
    CHECK(has(wd, IN_MODIFY, "a.txt"));
    CHECK(has(wd, IN_CLOSE_WRITE, "a.txt"));
    CHECK(has(wd, IN_CREATE, "a.txt") < has(wd, IN_MODIFY, "a.txt") && has(wd, IN_MODIFY, "a.txt") < has(wd, IN_CLOSE_WRITE, "a.txt"));

    int fw = inotify_add_watch(fd, "/tmp/in/a.txt", IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF | IN_CLOSE_NOWRITE);
    CHECK(fw > 0 && fw != wd);
    chmod("/tmp/in/a.txt", 0600);
    rename("/tmp/in/a.txt", "/tmp/in/b.txt");
    mkdir("/tmp/in/sub", 0755);
    drain(fd);
    CHECK(has(fw, IN_ATTRIB, NULL));
    CHECK(has(wd, IN_ATTRIB, "a.txt"));
    int from = has(wd, IN_MOVED_FROM, "a.txt"), to = has(wd, IN_MOVED_TO, "b.txt");
    CHECK(from && to && evs[from - 1].cookie && evs[from - 1].cookie == evs[to - 1].cookie);
    CHECK(has(fw, IN_MOVE_SELF, NULL));
    CHECK(has(wd, IN_CREATE | IN_ISDIR, "sub"));

    f = open("/tmp/in/b.txt", O_RDONLY); close(f);
    unlink("/tmp/in/b.txt");
    rmdir("/tmp/in/sub");
    drain(fd);
    CHECK(has(fw, IN_CLOSE_NOWRITE, NULL));
    CHECK(has(wd, IN_DELETE, "b.txt"));
    CHECK(has(fw, IN_DELETE_SELF, NULL));
    CHECK(has(fw, IN_IGNORED, NULL));
    CHECK(has(wd, IN_DELETE | IN_ISDIR, "sub"));

    /* oneshot + rm_watch */
    int w1 = inotify_add_watch(fd, "/tmp/in", IN_CREATE | IN_ONESHOT);
    CHECK(w1 == wd);                                    /* same inode → same wd, mask replaced */
    close(open("/tmp/in/c", O_CREAT | O_WRONLY, 0644));
    close(open("/tmp/in/d", O_CREAT | O_WRONLY, 0644));
    drain(fd);
    CHECK(has(wd, IN_CREATE, "c") && !has(wd, IN_CREATE, "d") && has(wd, IN_IGNORED, NULL));
    int w2 = inotify_add_watch(fd, "/tmp/in", IN_DELETE);
    CHECK(w2 > 0 && inotify_rm_watch(fd, w2) == 0);
    unlink("/tmp/in/c");
    drain(fd);
    CHECK(has(w2, IN_IGNORED, NULL) && !has(w2, IN_DELETE, "c"));
    CHECK(inotify_rm_watch(fd, w2) == -1 && errno == EINVAL);
    CHECK(inotify_add_watch(fd, "/nonexistent", IN_ALL_EVENTS) == -1 && errno == ENOENT);
    CHECK(inotify_add_watch(fd, "/tmp/in/d", IN_ALL_EVENTS | IN_ONLYDIR) == -1 && errno == ENOTDIR);
    char small[8];
    w2 = inotify_add_watch(fd, "/tmp/in", IN_CREATE);
    close(open("/tmp/in/a-long-name", O_CREAT | O_WRONLY, 0644));
    CHECK(read(fd, small, sizeof small) == -1 && errno == EINVAL);
    close(fd);
    unlink("/tmp/in/d"); unlink("/tmp/in/a-long-name"); rmdir("/tmp/in");
    printf("inotifytest: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
