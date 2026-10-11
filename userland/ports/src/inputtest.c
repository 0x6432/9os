/* M35: the input/seat stack end to end.
 *  xkbcommon + xkeyboard-config: compile "evdev/pc105/us,de", keysyms with shift and group 2
 *  uinput (via libevdev-uinput): create a keyboard and a mouse; devnode, sysfs, libevdev read back
 *  mtdev: open the new keyboard
 *  libwacom: database lookup by usb id, lookup of a uinput tablet by node; libinput tablet-tool device
 *  libinput (udev backend over libudev-zero, fed by a private udevshim daemon): the uinput
 *  devices are hot-plugged with the right capabilities, injected key/motion/button events
 *  arrive (KEY_A -> xkb "a"), destroying the devices sends DEVICE_REMOVED; path backend too
 *  libseat: through seatd (LIBSEAT_BACKEND=seatd) and the builtin backend, open an evdev node
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <libinput.h>
#include <libwacom/libwacom.h>
#include <libseat.h>
#include <libudev.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <mtdev.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("inputtest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000; }

/* ---------------------------------------------------------------- xkb */
static struct xkb_context *xctx;
static struct xkb_keymap *keymap;
static void test_xkb(void) {
    xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    CHECK(xctx != NULL, "xkb context");
    struct xkb_rule_names names = { .rules = "evdev", .model = "pc105", .layout = "us,de", .variant = "", .options = "grp:alt_shift_toggle" };
    keymap = xkb_keymap_new_from_names(xctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    CHECK(keymap != NULL, "keymap us,de from /usr/share/X11/xkb");
    if (!keymap) return;
    CHECK(xkb_keymap_num_layouts(keymap) == 2, "layouts %u", xkb_keymap_num_layouts(keymap));
    struct xkb_state *st = xkb_state_new(keymap);
    char b[16];
    xkb_state_key_get_utf8(st, KEY_A + 8, b, sizeof b);
    CHECK(!strcmp(b, "a"), "KEY_A -> '%s'", b);
    xkb_state_update_key(st, KEY_LEFTSHIFT + 8, XKB_KEY_DOWN);
    xkb_state_key_get_utf8(st, KEY_A + 8, b, sizeof b);
    CHECK(!strcmp(b, "A"), "shift+KEY_A -> '%s'", b);
    xkb_state_update_key(st, KEY_LEFTSHIFT + 8, XKB_KEY_UP);
    CHECK(xkb_state_key_get_one_sym(st, KEY_Y + 8) == XKB_KEY_y, "us KEY_Y");
    xkb_state_update_mask(st, 0, 0, 0, 0, 0, 1);              /* locked group 2: de */
    CHECK(xkb_state_key_get_one_sym(st, KEY_Y + 8) == XKB_KEY_z, "de KEY_Y -> z");
    CHECK(xkb_state_key_get_one_sym(st, KEY_SEMICOLON + 8) == XKB_KEY_odiaeresis, "de KEY_SEMICOLON -> odiaeresis");
    xkb_state_unref(st);
}

/* ---------------------------------------------------------------- uinput */
static struct libevdev_uinput *make_dev(bool kbd) {
    struct libevdev *d = libevdev_new();
    libevdev_set_name(d, kbd ? "9os test keyboard" : "9os test mouse");
    libevdev_set_id_bustype(d, BUS_VIRTUAL);
    libevdev_set_id_vendor(d, 0x9050);
    libevdev_set_id_product(d, kbd ? 1 : 2);
    if (kbd) {
        libevdev_enable_event_type(d, EV_KEY);
        for (unsigned k = KEY_ESC; k <= KEY_MICMUTE; k++) libevdev_enable_event_code(d, EV_KEY, k, NULL);
        libevdev_enable_event_type(d, EV_REP);
    } else {
        libevdev_enable_event_type(d, EV_REL);
        libevdev_enable_event_code(d, EV_REL, REL_X, NULL);
        libevdev_enable_event_code(d, EV_REL, REL_Y, NULL);
        libevdev_enable_event_code(d, EV_REL, REL_WHEEL, NULL);
        libevdev_enable_event_type(d, EV_KEY);
        libevdev_enable_event_code(d, EV_KEY, BTN_LEFT, NULL);
        libevdev_enable_event_code(d, EV_KEY, BTN_RIGHT, NULL);
        libevdev_enable_event_code(d, EV_KEY, BTN_MIDDLE, NULL);
    }
    struct libevdev_uinput *u = NULL;
    int r = libevdev_uinput_create_from_device(d, LIBEVDEV_UINPUT_OPEN_MANAGED, &u);
    CHECK(r == 0, "uinput create (%d)", r);
    libevdev_free(d);
    if (r) return NULL;
    const char *node = libevdev_uinput_get_devnode(u), *sp = libevdev_uinput_get_syspath(u);
    CHECK(node && !strncmp(node, "/dev/input/event", 16), "devnode %s", node ? node : "-");
    CHECK(sp && !strncmp(sp, "/sys/devices/virtual/input/input", 32), "syspath %s", sp ? sp : "-");
    if (node) {
        int fd = open(node, O_RDONLY | O_NONBLOCK);
        struct libevdev *e = NULL;
        CHECK(fd >= 0 && libevdev_new_from_fd(fd, &e) == 0, "libevdev on %s", node);
        if (e) {
            CHECK(!strcmp(libevdev_get_name(e), kbd ? "9os test keyboard" : "9os test mouse"), "name %s", libevdev_get_name(e));
            CHECK(libevdev_get_id_vendor(e) == 0x9050, "vendor");
            CHECK(kbd ? libevdev_has_event_code(e, EV_KEY, KEY_A) : libevdev_has_event_code(e, EV_REL, REL_WHEEL), "codes");
            libevdev_free(e);
        }
        if (kbd && fd >= 0) {
            struct mtdev m;
            CHECK(mtdev_open(&m, fd) == 0, "mtdev_open");
            mtdev_close(&m);
        }
        if (fd >= 0) close(fd);
    }
    return u;
}
static void emit(struct libevdev_uinput *u, int type, int code, int value) {
    libevdev_uinput_write_event(u, (unsigned)type, (unsigned)code, value);
}

/* ---------------------------------------------------------------- libinput */
static int open_restricted(const char *path, int flags, void *data) { int fd = open(path, flags); return fd < 0 ? -errno : fd; }
static void close_restricted(int fd, void *data) { close(fd); }
static const struct libinput_interface iface = { open_restricted, close_restricted };

struct seen { int added_kbd, added_mouse, removed, keys, key_a, motion, button, initial; char sym[8]; };
static void handle(struct libinput *li, struct seen *s, struct xkb_state *xs) {
    libinput_dispatch(li);
    struct libinput_event *ev;
    while ((ev = libinput_get_event(li))) {
        enum libinput_event_type t = libinput_event_get_type(ev);
        struct libinput_device *d = libinput_event_get_device(ev);
        const char *name = libinput_device_get_name(d);
        bool ours = !strncmp(name, "9os test ", 9);
        if (t == LIBINPUT_EVENT_DEVICE_ADDED) {
            if (!ours) s->initial++;
            else if (strstr(name, "keyboard")) {
                s->added_kbd++;
                CHECK(libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_KEYBOARD), "keyboard capability");
                CHECK(!strcmp(libinput_seat_get_physical_name(libinput_device_get_seat(d)), "seat0"), "seat0");
            } else {
                s->added_mouse++;
                CHECK(libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_POINTER), "pointer capability");
            }
        } else if (t == LIBINPUT_EVENT_DEVICE_REMOVED && ours) s->removed++;
        else if (t == LIBINPUT_EVENT_KEYBOARD_KEY && ours) {
            struct libinput_event_keyboard *k = libinput_event_get_keyboard_event(ev);
            uint32_t key = libinput_event_keyboard_get_key(k);
            bool down = libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;
            s->keys++;
            if (xs) {
                if (down && key == KEY_A) xkb_state_key_get_utf8(xs, key + 8, s->sym, sizeof s->sym);
                xkb_state_update_key(xs, key + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
            }
            if (key == KEY_A) s->key_a++;
        } else if (t == LIBINPUT_EVENT_POINTER_MOTION && ours) {
            struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
            if (libinput_event_pointer_get_dx_unaccelerated(p) > 0) s->motion++;
        } else if (t == LIBINPUT_EVENT_POINTER_BUTTON && ours) {
            struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
            if (libinput_event_pointer_get_button(p) == BTN_LEFT) s->button++;
        }
        libinput_event_destroy(ev);
    }
}
static void pump(struct libinput *li, struct seen *s, struct xkb_state *xs, int ms, bool (*done)(struct seen *)) {
    long end = now_ms() + ms;
    while (now_ms() < end && !(done && done(s))) {
        struct pollfd p = { libinput_get_fd(li), POLLIN, 0 };
        poll(&p, 1, 50);
        handle(li, s, xs);
    }
}
static bool both_added(struct seen *s) { return s->added_kbd && s->added_mouse; }
static bool got_input(struct seen *s) { return s->key_a >= 2 && s->motion && s->button >= 2; }
static bool both_removed(struct seen *s) { return s->removed >= 2; }

static pid_t start(const char *const argv[], const char *wait_path) {
    pid_t p = fork();
    if (!p) { execvp(argv[0], (char *const *)argv); _exit(127); }
    for (int i = 0; wait_path && i < 150 && access(wait_path, F_OK); i++) usleep(20000);
    usleep(100000);
    return p;
}
static void stop(pid_t p) { if (p > 0) { kill(p, SIGTERM); waitpid(p, NULL, 0); } }

static void test_libinput(void) {
    unlink("/run/udev/control");
    pid_t ud = start((const char *const[]){ "udevshim", "-d", "-f", NULL }, "/run/udev/control");
    struct udev *udev = udev_new();
    struct libinput *li = libinput_udev_create_context(&iface, NULL, udev);
    CHECK(li != NULL, "libinput udev context");
    if (!li) { stop(ud); return; }
    CHECK(libinput_udev_assign_seat(li, "seat0") == 0, "assign seat0");
    struct seen s = { 0 };
    struct xkb_state *xs = keymap ? xkb_state_new(keymap) : NULL;
    pump(li, &s, xs, 300, NULL);
    CHECK(s.initial >= 1, "boot devices (virtio/ps2) added: %d", s.initial);
    struct libevdev_uinput *kb = make_dev(true), *ms = make_dev(false);
    pump(li, &s, xs, 5000, both_added);
    CHECK(s.added_kbd == 1 && s.added_mouse == 1, "hotplug: keyboard %d mouse %d", s.added_kbd, s.added_mouse);
    if (kb && ms) {
        for (int i = 0; i < 2; i++) {
            emit(kb, EV_KEY, KEY_A, 1); emit(kb, EV_SYN, SYN_REPORT, 0);
            emit(kb, EV_KEY, KEY_A, 0); emit(kb, EV_SYN, SYN_REPORT, 0);
            emit(ms, EV_KEY, BTN_LEFT, 1); emit(ms, EV_SYN, SYN_REPORT, 0);
            emit(ms, EV_KEY, BTN_LEFT, 0); emit(ms, EV_SYN, SYN_REPORT, 0);
        }
        emit(ms, EV_REL, REL_X, 10); emit(ms, EV_SYN, SYN_REPORT, 0);
        pump(li, &s, xs, 3000, got_input);
        CHECK(s.key_a >= 2 && (!xs || !strcmp(s.sym, "a")), "key events %d sym '%s'", s.key_a, s.sym);
        CHECK(s.motion >= 1 && s.button >= 2, "pointer motion %d buttons %d", s.motion, s.button);
    }
    if (kb) libevdev_uinput_destroy(kb);
    if (ms) libevdev_uinput_destroy(ms);
    pump(li, &s, xs, 5000, both_removed);
    CHECK(s.removed == 2, "hot-unplug: %d removed", s.removed);
    libinput_unref(li);
    udev_unref(udev);

    /* path backend on a fresh uinput device */
    kb = make_dev(true);
    struct libinput *lp = libinput_path_create_context(&iface, NULL);
    struct libinput_device *pd = kb ? libinput_path_add_device(lp, libevdev_uinput_get_devnode(kb)) : NULL;
    CHECK(pd && libinput_device_has_capability(pd, LIBINPUT_DEVICE_CAP_KEYBOARD), "path backend");
    libinput_unref(lp);
    if (kb) libevdev_uinput_destroy(kb);
    if (xs) xkb_state_unref(xs);
    stop(ud);
}

/* ---------------------------------------------------------------- libwacom */
static void wlog(struct libinput *li, enum libinput_log_priority p, const char *fmt, va_list ap) { printf("inputtest: libinput: "); vprintf(fmt, ap); }
static void test_wacom(void) {
    WacomError *err = libwacom_error_new();
    WacomDeviceDatabase *db = libwacom_database_new();
    CHECK(db != NULL, "libwacom database (/usr/share/libwacom)");
    if (!db) { libwacom_error_free(&err); return; }
    WacomDevice *w = libwacom_new_from_usbid(db, 0x056a, 0x0357, err);
    CHECK(w && !strcmp(libwacom_get_name(w), "Wacom Intuos Pro M"), "usbid 056a:0357 -> %s", w ? libwacom_get_name(w) : libwacom_error_get_message(err));
    if (w) {
        CHECK(libwacom_get_num_buttons(w) > 0 && libwacom_has_stylus(w), "intuos pro buttons %d", libwacom_get_num_buttons(w));
        libwacom_destroy(w);
    }
    /* uinput tablet with the same ids: lookup by device node goes through gudev -> libudev -> sysfs;
     * libinput only takes udev-initialized devices, so a udevshim daemon has to write the db entry */
    unlink("/run/udev/control");
    pid_t ud = start((const char *const[]){ "udevshim", "-d", "-f", NULL }, "/run/udev/control");
    struct libevdev *d = libevdev_new();
    libevdev_set_name(d, "Wacom Intuos Pro M Pen");
    libevdev_set_id_bustype(d, BUS_USB);
    libevdev_set_id_vendor(d, 0x056a);
    libevdev_set_id_product(d, 0x0357);
    struct input_absinfo ax = { .maximum = 44800, .resolution = 200 }, ay = { .maximum = 29600, .resolution = 200 }, ap = { .maximum = 8191 };
    libevdev_enable_event_code(d, EV_ABS, ABS_X, &ax);
    libevdev_enable_event_code(d, EV_ABS, ABS_Y, &ay);
    libevdev_enable_event_code(d, EV_ABS, ABS_PRESSURE, &ap);
    libevdev_enable_event_code(d, EV_KEY, BTN_TOOL_PEN, NULL);
    libevdev_enable_event_code(d, EV_KEY, BTN_TOUCH, NULL);
    libevdev_enable_event_code(d, EV_KEY, BTN_STYLUS, NULL);
    libevdev_enable_property(d, INPUT_PROP_POINTER);
    struct libevdev_uinput *u = NULL;
    int r = libevdev_uinput_create_from_device(d, LIBEVDEV_UINPUT_OPEN_MANAGED, &u);
    libevdev_free(d);
    CHECK(r == 0, "uinput tablet (%d)", r);
    if (!r) {
        const char *node = libevdev_uinput_get_devnode(u);
        w = node ? libwacom_new_from_path(db, node, WFALLBACK_NONE, err) : NULL;
        CHECK(w && libwacom_get_vendor_id(w) == 0x056a && libwacom_get_product_id(w) == 0x0357,
              "new_from_path %s: %s", node ? node : "-", w ? libwacom_get_name(w) : libwacom_error_get_message(err));
        if (w) libwacom_destroy(w);
        struct libinput *li = libinput_path_create_context(&iface, NULL);
        if (li) { libinput_log_set_handler(li, wlog); libinput_log_set_priority(li, LIBINPUT_LOG_PRIORITY_ERROR); }
        struct libinput_device *ld = li && node ? libinput_path_add_device(li, node) : NULL;
        CHECK(ld && libinput_device_has_capability(ld, LIBINPUT_DEVICE_CAP_TABLET_TOOL), "libinput tablet-tool capability (%s)", ld ? "added" : "not added");
        if (li) libinput_unref(li);
        libevdev_uinput_destroy(u);
    }
    stop(ud);
    libwacom_database_destroy(db);
    libwacom_error_free(&err);
}

/* ---------------------------------------------------------------- libseat */
static bool enabled;
static void on_enable(struct libseat *s, void *d) { enabled = true; }
static void on_disable(struct libseat *s, void *d) { enabled = false; libseat_disable_seat(s); }
static int active_vt(void) {
    char b[16] = "";
    int fd = open("/sys/class/tty/tty0/active", O_RDONLY);
    if (fd < 0) return -1;
    read(fd, b, sizeof b - 1);
    close(fd);
    return strncmp(b, "tty", 3) ? -1 : atoi(b + 3);
}
/* libseat_switch_session -> VT_ACTIVATE; seatd answers relsig (disable, VT_RELDISP) and, when
 * VT1 comes back (VT_ACTIVATE from outside), acqsig re-enables the session */
static void seat_vt_switch(struct libseat *seat, const char *backend) {
    if (active_vt() != 1) return;
    CHECK(libseat_switch_session(seat, 2) == 0, "%s: switch_session(2)", backend);
    for (int i = 0; i < 100 && (active_vt() != 2 || enabled); i++) libseat_dispatch(seat, 50);
    CHECK(active_vt() == 2 && !enabled, "%s: on VT2 (%d), session disabled (%d)", backend, active_vt(), enabled);
    int t0 = open("/dev/tty0", O_RDWR | O_NOCTTY);
    CHECK(t0 >= 0 && ioctl(t0, VT_ACTIVATE, 1) == 0, "%s: VT_ACTIVATE 1", backend);
    if (t0 >= 0) close(t0);
    for (int i = 0; i < 100 && !enabled; i++) libseat_dispatch(seat, 50);
    CHECK(active_vt() == 1 && enabled, "%s: back on VT1 (%d), session enabled (%d)", backend, active_vt(), enabled);
}
static void seat_round(const char *backend) {
    setenv("LIBSEAT_BACKEND", backend, 1);
    enabled = false;
    static struct libseat_seat_listener l = { on_enable, on_disable };
    struct libseat *seat = libseat_open_seat(&l, NULL);
    CHECK(seat != NULL, "%s: open seat", backend);
    if (!seat) return;
    for (int i = 0; i < 100 && !enabled; i++) libseat_dispatch(seat, 50);
    CHECK(enabled, "%s: seat enabled", backend);
    CHECK(!strcmp(libseat_seat_name(seat), "seat0"), "%s: name %s", backend, libseat_seat_name(seat));
    int fd = -1, id = libseat_open_device(seat, "/dev/input/event0", &fd);
    char name[128] = "";
    CHECK(id >= 0 && fd >= 0 && ioctl(fd, EVIOCGNAME(sizeof name), name) > 0, "%s: open event0 (%d/%d)", backend, id, fd);
    if (id >= 0) CHECK(libseat_close_device(seat, id) == 0, "%s: close device", backend);
    if (fd >= 0) close(fd);
    if (!access("/dev/dri/card0", F_OK)) {
        id = libseat_open_device(seat, "/dev/dri/card0", &fd);
        CHECK(id >= 0 && fd >= 0, "%s: open card0", backend);
        if (id >= 0) { libseat_close_device(seat, id); close(fd); }
    }
    seat_vt_switch(seat, backend);
    libseat_close_seat(seat);
}
static void test_seat(void) {
    unlink("/run/seatd.sock");
    pid_t sd = start((const char *const[]){ "seatd", "-l", "error", NULL }, "/run/seatd.sock");
    seat_round("seatd");
    stop(sd);
    seat_round("builtin");
}

/* keys typed on our uinput keyboards reach the foreground VT (like Linux): keep them off the
 * console that runs the test */
static int kb_saved = -1, kb_fd = -1;
static void console_keys_off(void) {
    kb_fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
    if (kb_fd >= 0 && ioctl(kb_fd, KDGKBMODE, &kb_saved) == 0) ioctl(kb_fd, KDSKBMODE, K_OFF);
}
static void console_keys_restore(void) { if (kb_fd >= 0 && kb_saved >= 0) ioctl(kb_fd, KDSKBMODE, kb_saved); }

int main(int argc, char **argv) {
    console_keys_off();
    atexit(console_keys_restore);
    setvbuf(stdout, NULL, _IONBF, 0);
    test_xkb();
    test_libinput();
    test_wacom();
    test_seat();
    if (fails) { printf("inputtest: %d failures\n", fails); return 1; }
    printf("inputtest: all passed\n");
    return 0;
}
