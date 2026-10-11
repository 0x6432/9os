/* M35: /dev/uinput without libraries. UI_DEV_SETUP/UI_ABS_SETUP and the legacy
 * uinput_user_dev write create devices with the requested name/id/bits/absinfo; events
 * written to uinput are read from the new /dev/input/eventN (EVIOCG* agree, sysfs under
 * /sys/devices/virtual/input/inputN); "add"/"remove" uevents; destroying the device gives
 * open readers ENODEV and POLLHUP, removes the node and the sysfs entries, frees the event
 * slot for the next device (inputN keeps counting); bad requests fail cleanly. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("uinputtest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)

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
static int uev = -1;
static bool uev_wait(const char *prefix) {
    char b[4096];
    for (int i = 0; i < 50; i++) {
        struct pollfd p = { uev, POLLIN, 0 };
        if (poll(&p, 1, 2000) <= 0) return false;
        ssize_t n = recv(uev, b, sizeof b - 1, 0);
        if (n <= 0) return false;
        b[n] = 0;
        if (!strncmp(b, prefix, strlen(prefix))) return true;
    }
    return false;
}
/* sysname "inputN" -> its eventN node */
static bool node_of(const char *sysname, char *node, size_t cap) {
    for (int i = 0; i < 32; i++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/virtual/input/%s/event%d", sysname, i);
        if (!access(p, F_OK)) { snprintf(node, cap, "/dev/input/event%d", i); return true; }
    }
    return false;
}
static int create(const char *name, bool legacy, char *sys, size_t cap) {
    int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    CHECK(u >= 0, "open /dev/uinput");
    if (u < 0) return -1;
    int ver = 0;
    CHECK(ioctl(u, UI_GET_VERSION, &ver) == 0 && ver >= 5, "version %d", ver);
    CHECK(ioctl(u, UI_DEV_CREATE) < 0 && errno == EINVAL, "create without a name");
    ioctl(u, UI_SET_EVBIT, EV_KEY);
    ioctl(u, UI_SET_KEYBIT, KEY_A);
    ioctl(u, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(u, UI_SET_EVBIT, EV_ABS);
    ioctl(u, UI_SET_ABSBIT, ABS_X);
    ioctl(u, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    CHECK(ioctl(u, UI_SET_KEYBIT, KEY_MAX + 1) < 0 && errno == EINVAL, "bit out of range");
    ioctl(u, UI_SET_PHYS, "uinputtest/1");
    if (legacy) {
        struct uinput_user_dev ud = { 0 };
        snprintf(ud.name, sizeof ud.name, "%s", name);
        ud.id = (struct input_id){ BUS_USB, 0x1234, 0x5678, 1 };
        ud.absmin[ABS_X] = 0; ud.absmax[ABS_X] = 1000;
        CHECK(write(u, &ud, sizeof ud) == sizeof ud, "legacy setup write");
    } else {
        struct uinput_setup s = { .id = { BUS_USB, 0x1234, 0x5678, 1 } };
        snprintf(s.name, sizeof s.name, "%s", name);
        CHECK(ioctl(u, UI_DEV_SETUP, &s) == 0, "UI_DEV_SETUP");
        struct uinput_abs_setup a = { .code = ABS_X, .absinfo = { .minimum = 0, .maximum = 1000, .resolution = 10 } };
        CHECK(ioctl(u, UI_ABS_SETUP, &a) == 0, "UI_ABS_SETUP");
    }
    CHECK(ioctl(u, UI_DEV_CREATE) == 0, "UI_DEV_CREATE");
    CHECK(ioctl(u, UI_SET_KEYBIT, KEY_B) < 0 && errno == EINVAL, "configure after create");
    CHECK(ioctl(u, UI_GET_SYSNAME(cap), sys) > 0 && !strncmp(sys, "input", 5), "sysname %s", sys);
    return u;
}

int main(void) {
    uev = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 };
    CHECK(uev >= 0 && bind(uev, (struct sockaddr *)&a, sizeof a) == 0, "uevent socket");
    char sys[32], sys2[32], node[64], node2[64], p[160], b[256];
    int u = create("uinputtest dev", false, sys, sizeof sys);
    if (u < 0) return 1;
    snprintf(p, sizeof p, "add@/devices/virtual/input/%s", sys);
    CHECK(uev_wait(p), "add uevent for %s", sys);
    CHECK(node_of(sys, node, sizeof node), "event node of %s", sys);
    snprintf(p, sizeof p, "/sys/devices/virtual/input/%s/name", sys);
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "uinputtest dev"), "sysfs name %s", b);
    snprintf(p, sizeof p, "/sys/devices/virtual/input/%s/phys", sys);
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "uinputtest/1"), "sysfs phys %s", b);
    snprintf(p, sizeof p, "/sys/devices/virtual/input/%s/id/vendor", sys);
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "1234"), "vendor %s", b);
    int e = open(node, O_RDONLY | O_NONBLOCK);
    CHECK(e >= 0, "open %s", node);
    char nm[64] = "";
    struct input_absinfo ai = { 0 };
    unsigned long prop = 0;
    CHECK(ioctl(e, EVIOCGNAME(sizeof nm), nm) > 0 && !strcmp(nm, "uinputtest dev"), "EVIOCGNAME %s", nm);
    CHECK(ioctl(e, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum == 1000 && ai.resolution == 10, "EVIOCGABS max %d res %d", ai.maximum, ai.resolution);
    CHECK(ioctl(e, EVIOCGPROP(sizeof prop), &prop) >= 0 && (prop & 1u << INPUT_PROP_DIRECT), "EVIOCGPROP %lx", prop);
    struct input_event w[3] = { { .type = EV_KEY, .code = KEY_A, .value = 1 }, { .type = EV_ABS, .code = ABS_X, .value = 500 },
                                { .type = EV_SYN, .code = SYN_REPORT } };
    CHECK(write(u, w, sizeof w) == sizeof w, "inject");
    struct input_event r[8];
    ssize_t n = read(e, r, sizeof r);
    CHECK(n == 3 * (ssize_t)sizeof r[0] && r[0].code == KEY_A && r[0].value == 1 && r[1].code == ABS_X && r[1].value == 500 &&
          r[2].type == EV_SYN, "read back %zd", n);
    unsigned char keys[KEY_MAX / 8 + 1] = { 0 };
    CHECK(ioctl(e, EVIOCGKEY(sizeof keys), keys) >= 0 && (keys[KEY_A / 8] >> (KEY_A % 8) & 1), "EVIOCGKEY");
    CHECK(write(u, w, 5) < 0 && errno == EINVAL, "short event write");
    /* destroy: readers see ENODEV/POLLHUP, node and sysfs go away */
    CHECK(ioctl(u, UI_DEV_DESTROY) == 0, "UI_DEV_DESTROY");
    snprintf(p, sizeof p, "remove@/devices/virtual/input/%s", sys);
    CHECK(uev_wait(p), "remove uevent");
    struct pollfd pf = { e, POLLIN, 0 };
    CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLHUP), "POLLHUP %x", pf.revents);
    CHECK(read(e, r, sizeof r) < 0 && errno == ENODEV, "read after destroy");
    CHECK(ioctl(e, EVIOCGNAME(sizeof nm), nm) < 0 && errno == ENODEV, "ioctl after destroy");
    CHECK(access(node, F_OK) < 0, "node %s removed", node);
    snprintf(p, sizeof p, "/sys/devices/virtual/input/%s", sys);
    CHECK(access(p, F_OK) < 0 && errno == ENOENT, "sysfs entry removed");
    CHECK(write(u, w, sizeof w) < 0 && errno == ENODEV, "inject after destroy");
    close(e);
    close(u);
    /* legacy setup; the event slot is reused, inputN is not */
    u = create("uinputtest legacy", true, sys2, sizeof sys2);
    if (u >= 0) {
        CHECK(strcmp(sys, sys2) && atoi(sys2 + 5) > atoi(sys + 5), "new input number %s after %s", sys2, sys);
        CHECK(node_of(sys2, node2, sizeof node2) && !strcmp(node, node2), "slot reused: %s vs %s", node2, node);
        e = open(node2, O_RDONLY);
        struct input_absinfo a2 = { 0 };
        CHECK(e >= 0 && ioctl(e, EVIOCGABS(ABS_X), &a2) == 0 && a2.maximum == 1000, "legacy absmax %d", a2.maximum);
        close(u);                                     /* close = destroy */
        CHECK(read(e, r, sizeof r) < 0 && errno == ENODEV, "reader after uinput close");
        close(e);
    }
    /* churn: many create/destroy cycles must not run out of slots */
    for (int i = 0; i < 40; i++) {
        int x = create("uinputtest churn", false, sys2, sizeof sys2);
        if (x < 0) break;
        close(x);
    }
    int x = create("uinputtest last", false, sys2, sizeof sys2);
    CHECK(x >= 0, "after churn");
    if (x >= 0) close(x);
    if (fails) { printf("uinputtest: %d failures\n", fails); return 1; }
    printf("uinputtest: all passed\n");
    return 0;
}
