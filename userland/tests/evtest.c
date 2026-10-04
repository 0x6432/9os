/* evtest: list /dev/input/event* devices (EVIOCG* ioctls) and print events for N seconds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
#define TEST(b, a) ((a[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

static int count_bits(const unsigned long *a, int n) { int c = 0; for (int i = 0; i < n; i++) c += TEST(i, a); return c; }

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 0;
    struct pollfd fds[16]; int n = 0;
    for (int i = 0; i < 16; i++) {
        char path[32]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) break;
        char name[128] = "?"; struct input_id id; int ver = 0;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        ioctl(fd, EVIOCGID, &id);
        ioctl(fd, EVIOCGVERSION, &ver);
        unsigned long ev[NBITS(EV_MAX + 1)] = {0}, key[NBITS(KEY_MAX + 1)] = {0}, abs[NBITS(ABS_MAX + 1)] = {0}, rel[NBITS(REL_MAX + 1)] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
        printf("%s: \"%s\" bus %04x vendor %04x product %04x ver %x evdev %x; ev 0x%lx keys %d rel %d abs %d",
               path, name, id.bustype, id.vendor, id.product, id.version, ver, ev[0],
               count_bits(key, KEY_MAX + 1), count_bits(rel, REL_MAX + 1), count_bits(abs, ABS_MAX + 1));
        if (TEST(ABS_X, abs)) { struct input_absinfo ai; ioctl(fd, EVIOCGABS(ABS_X), &ai); printf(" X[%d..%d]", ai.minimum, ai.maximum); }
        printf("\n");
        int clk = CLOCK_MONOTONIC; ioctl(fd, EVIOCSCLOCKID, &clk);
        fds[n].fd = fd; fds[n].events = POLLIN; n++;
    }
    if (!n) { printf("evtest: no input devices\n"); return 1; }
    int total = 0;
    for (int ms = secs * 1000; ms > 0; ms -= 100) {
        if (poll(fds, n, 100) <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (!(fds[i].revents & POLLIN)) continue;
            struct input_event e[16];
            ssize_t r = read(fds[i].fd, e, sizeof e);
            for (int j = 0; j < r / (ssize_t)sizeof e[0]; j++) {
                if (e[j].type == EV_SYN) continue;
                printf("event%d: type %d code %d value %d\n", i, e[j].type, e[j].code, e[j].value);
                total++;
            }
        }
    }
    if (secs) printf("evtest: %d events\n", total);
    return 0;
}
