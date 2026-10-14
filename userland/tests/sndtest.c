/* Direct Linux PCM ABI regression; deliberately no alsa-lib dependency. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../kernel/include/kernel/pcm_abi.h"
static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { printf("sndtest: FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); fails++; } } while (0)
static struct pcm_hw_params any(void) {
    struct pcm_hw_params p = {0};
    for (unsigned i = 0; i < 3; i++) for (unsigned j = 0; j < 8; j++) p.masks[i].bits[j] = ~0u;
    for (unsigned i = 0; i < 12; i++) p.intervals[i].max = ~0u;
    p.rmask = ~0u; return p;
}
static int configure(int fd) {
    struct pcm_hw_params p = any(); int r = ioctl(fd, PCM_IO_HW_PARAMS, &p);
    if (!r) r = ioctl(fd, PCM_IO_PREPARE);
    return r;
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--absent")) {
        int fd = open("/dev/snd/pcmC0D0p", O_WRONLY);
        CHECK(fd == -1 && errno == ENOENT); if (fd >= 0) close(fd);
        goto end;
    }
    int fd = open("/dev/snd/pcmC0D0p", O_WRONLY); CHECK(fd >= 0); if (fd < 0) goto end;
    int other = open("/dev/snd/pcmC0D0p", O_WRONLY); CHECK(other == -1 && errno == EBUSY); if (other >= 0) close(other);
    unsigned version = 0; CHECK(ioctl(fd, PCM_IO_VERSION, &version) == 0 && version == PCM_VERSION);
    struct pcm_info info; CHECK(ioctl(fd, PCM_IO_INFO, &info) == 0 && !strcmp((char *)info.id,"9os-virtio") && !info.stream);
    struct stat st; CHECK(stat("/dev/snd/pcmC0D0p", &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(access("/sys/class/sound/pcmC0D0p/dev", R_OK) == 0);
    CHECK(ioctl(fd, 0xdead) == -1 && errno == ENOTTY);
    CHECK(ioctl(fd, PCM_IO_VERSION, (void *)1) == -1 && errno == EFAULT);
    short samples[PCM_PERIOD * 2];
    CHECK(write(fd, samples, sizeof samples) == -1 && errno == EBADFD);
    struct pcm_hw_params p = any(); CHECK(ioctl(fd, PCM_IO_HW_REFINE, &p) == 0);
    CHECK(p.masks[0].bits[0] == (1u << 3) && p.masks[1].bits[0] == (1u << 2));
    CHECK(p.intervals[3].min == 48000 && p.intervals[2].min == 2 && !(p.info & 3));
    p = any(); p.intervals[3].min = p.intervals[3].max = 44100;
    CHECK(ioctl(fd, PCM_IO_HW_PARAMS, &p) == -1 && errno == EINVAL);
    p = any(); p.masks[1].bits[0] = 1; CHECK(ioctl(fd, PCM_IO_HW_PARAMS, &p) == -1 && errno == EINVAL);
    CHECK(configure(fd) == 0);
    CHECK(write(fd, samples, 3) == -1 && errno == EINVAL);
    CHECK(write(fd, (void *)1, sizeof samples) == -1 && errno == EFAULT);
    /* 500 Hz square wave, left positive while right negative. Host WAV test checks it. */
    for (unsigned period = 0; period < 64; period++) {
        for (unsigned i = 0; i < PCM_PERIOD; i++) { short v = ((period * PCM_PERIOD + i) / 48 % 2) ? -10000 : 10000; samples[i * 2] = v; samples[i * 2 + 1] = -v; }
        if (period & 1) { struct pcm_xferi x = { .buf = samples, .frames = PCM_PERIOD }; CHECK(ioctl(fd, PCM_IO_WRITEI, &x) == 0 && x.result == PCM_PERIOD); }
        else CHECK(write(fd, samples, sizeof samples) == sizeof samples);
    }
    struct pcm_status status;
    CHECK(ioctl(fd, PCM_IO_STATUS, &status) == 0 && status.state == PCM_RUNNING && status.appl_ptr == 64 * PCM_PERIOD);
    CHECK(status.hw_ptr <= status.appl_ptr && status.delay >= 0 && status.delay <= PCM_BUFFER);
    CHECK(ioctl(fd, PCM_IO_DRAIN) == 0);
    CHECK(ioctl(fd, PCM_IO_STATUS, &status) == 0 && status.state == PCM_SETUP && !status.delay && status.hw_ptr == status.appl_ptr);
    CHECK(ioctl(fd, PCM_IO_PREPARE) == 0);
    /* Explicit start + bounded nonblocking queue; no device completion before START. */
    struct pcm_sw_params sw = { .period_step = 1, .avail_min = PCM_PERIOD, .xfer_align = 1, .start_threshold = PCM_BUFFER + 1, .stop_threshold = PCM_BUFFER, .boundary = 1ul << 60 };
    CHECK(ioctl(fd, PCM_IO_SW_PARAMS, &sw) == 0);
    int flags = fcntl(fd, F_GETFL); CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    for (unsigned i = 0; i < PCM_PERIODS; i++) CHECK(write(fd, samples, sizeof samples) == sizeof samples);
    CHECK(write(fd, samples, sizeof samples) == -1 && errno == EAGAIN);
    struct pollfd pollfd = { .fd = fd, .events = POLLOUT }; CHECK(poll(&pollfd, 1, 0) == 0);
    CHECK(ioctl(fd, PCM_IO_START) == 0); CHECK(ioctl(fd, PCM_IO_DRAIN) == 0);
    CHECK(ioctl(fd, PCM_IO_PREPARE) == 0);
    CHECK(write(fd, samples, sizeof samples) == sizeof samples);
    CHECK(ioctl(fd, PCM_IO_DROP) == 0); /* release flushes TX; descriptor pages reusable */
    CHECK(ioctl(fd, PCM_IO_HW_FREE) == 0);
    CHECK(close(fd) == 0);
    fd = open("/dev/snd/pcmC0D0p", O_WRONLY); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(configure(fd) == 0); CHECK(write(fd, samples, sizeof samples) == sizeof samples); CHECK(close(fd) == 0); }
end:
    printf("sndtest: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails); return fails != 0;
}
