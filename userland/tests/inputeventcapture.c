/* Read-only hardware evdev diagnostic. Never grabs a device, injects input or
 * substitutes for a genuine terminal/physical-display acceptance check.
 * Public 64-bit Linux UAPI layout, no distro linux/input.h dependency. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
struct input_event_abi { int64_t sec, usec; uint16_t type, code; int32_t value; };
_Static_assert(sizeof(struct input_event_abi) == 24, "64-bit evdev event ABI");
static uint64_t mono(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("capture clock"); exit(1); }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
int main(int argc, char **argv) {
    char *end; long seconds = 45;
    if (argc > 2) return 2;
    if (argc == 2) {
        errno = 0; seconds = strtol(argv[1], &end, 10);
        if (errno || end == argv[1] || *end || seconds < 1 || seconds > 120) return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0); alarm((unsigned)seconds + 10);
    struct pollfd fds[32]; int indices[32], count = 0;
    for (int i = 0; i < 32; i++) {
        char path[48]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { if (errno != ENOENT) perror(path); continue; }
        char name[128] = {0}; int clockid = CLOCK_MONOTONIC;
        int named = ioctl(fd, _IOC(_IOC_READ, 'E', 0x06, sizeof name), name);
        int clocked = ioctl(fd, _IOW('E', 0xa0, int), &clockid);
        name[sizeof name - 1] = 0;
        printf("INPUT_CAPTURE_DEVICE index=%d named=%d monotonic=%d name=%s\n", i, named >= 0, clocked == 0, name);
        indices[count] = i; fds[count++] = (struct pollfd){fd, POLLIN, 0};
    }
    if (!count) { puts("INPUT_CAPTURE_ERROR no devices"); return 1; }
    unsigned total = 0, repeats = 0, drops = 0; int failed = 0;
    uint64_t deadline = mono() + (uint64_t)seconds * 1000000000ull;
    printf("INPUT_CAPTURE_READY devices=%d\n", count);
    while (mono() < deadline) {
        int ready = poll(fds, (nfds_t)count, 100);
        if (ready < 0) { if (errno == EINTR) continue; perror("capture poll"); failed = 1; break; }
        for (int i = 0; i < count; i++) {
            if (fds[i].revents & (POLLNVAL | POLLHUP | POLLERR)) { failed = 1; goto done; }
            if (!(fds[i].revents & POLLIN)) continue;
            struct input_event_abi events[128];
            ssize_t n = read(fds[i].fd, events, sizeof events);
            uint64_t received = mono();
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n <= 0 || n % (ssize_t)sizeof events[0]) { failed = 1; goto done; }
            for (ssize_t j = 0; j < n / (ssize_t)sizeof events[0]; j++) {
                struct input_event_abi *e = &events[j]; total++;
                if (e->type == 1 && e->value == 2) repeats++;
                if (e->type == 0 && e->code == 3) drops++;
                printf("INPUT_CAPTURE_EVENT device=%d sec=%lld usec=%lld type=%u code=%u value=%d received_ns=%llu\n",
                       indices[i], (long long)e->sec, (long long)e->usec,
                       e->type, e->code, e->value, (unsigned long long)received);
            }
        }
    }
done:
    for (int i = 0; i < count; i++) close(fds[i].fd);
    printf("INPUT_CAPTURE_DONE events=%u repeats=%u dropped=%u error=%d\n", total, repeats, drops, failed);
    return failed || drops;
}
