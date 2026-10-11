/* M36: virtual terminals. tty1..tty6 with their own line discipline and text console
 * (/dev/vcsN reads the shadow buffer), /dev/tty0 = the foreground VT, VT_GETSTATE/OPENQRY,
 * VT_ACTIVATE/WAITACTIVE, per-VT KD and keyboard modes, /sys/class/tty/tty0/active,
 * VT_PROCESS switching (relsig/acqsig, VT_RELDISP refuse/accept/ACKACQ), an owner that died
 * is reset to VT_AUTO, and Alt+Fn / typing on a uinput keyboard reach the foreground VT. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/uinput.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("vttest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)

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
static int active(int t0) { struct vt_stat st = { 0 }; ioctl(t0, VT_GETSTATE, &st); return st.v_active; }
static void on_alarm(int s) { (void)s; }
static int waitactive(int fd, int n) {         /* VT_WAITACTIVE with a 5 s timeout */
    signal(SIGALRM, on_alarm);
    siginterrupt(SIGALRM, 1);
    alarm(5);
    int r = ioctl(fd, VT_WAITACTIVE, n);
    alarm(0);
    return r;
}
static bool vcs_has(const char *dev, const char *s) {
    static char b[1 << 16];
    int fd = open(dev, O_RDONLY);
    if (fd < 0) return false;
    int n = 0, r;
    while (n < (int)sizeof b - 1 && (r = (int)read(fd, b + n, sizeof b - 1 - n)) > 0) n += r;
    close(fd);
    b[n] = 0;
    return memmem(b, n, s, strlen(s)) != NULL;
}

/* VT_PROCESS owner of tty3: answers the switch protocol, reports over a pipe */
static volatile sig_atomic_t rel, acq;
static void on_rel(int s) { (void)s; rel++; }
static void on_acq(int s) { (void)s; acq++; }
static void owner(int out) {
    int t = open("/dev/tty3", O_RDWR | O_NOCTTY);
    signal(SIGUSR1, on_rel);
    signal(SIGUSR2, on_acq);
    struct vt_mode m = { .mode = VT_PROCESS, .relsig = SIGUSR1, .acqsig = SIGUSR2 };
    if (t < 0 || ioctl(t, VT_SETMODE, &m)) { write(out, "E", 1); _exit(1); }
    write(out, "R", 1);
    int refused = 0;
    for (int i = 0; i < 1000; i++) {
        if (acq) { acq = 0; ioctl(t, VT_RELDISP, VT_ACKACQ); write(out, "A", 1); }
        if (rel) {
            rel = 0;
            if (!refused++) { ioctl(t, VT_RELDISP, 0); write(out, "N", 1); }
            else { ioctl(t, VT_RELDISP, 1); write(out, "Y", 1); _exit(0); }
        }
        usleep(10000);
    }
    _exit(2);
}
static char next(int fd) { char c = 0; return read(fd, &c, 1) == 1 ? c : 0; }

static void key(int u, int code, int v) {
    struct input_event e[2] = { { .type = EV_KEY, .code = code, .value = v }, { .type = EV_SYN } };
    write(u, e, sizeof e);
}

int main(void) {
    int t0 = open("/dev/tty0", O_RDWR | O_NOCTTY);
    CHECK(t0 >= 0, "open tty0");
    struct vt_stat st = { 0 };
    CHECK(ioctl(t0, VT_GETSTATE, &st) == 0 && st.v_active == 1 && (st.v_state & 2), "GETSTATE active %d state %#x", st.v_active, st.v_state);
    char b[256];
    CHECK(rd("/sys/class/tty/tty0/active", b, sizeof b) > 0 && !strcmp(b, "tty1"), "sysfs active %s", b);
    int t2 = open("/dev/tty2", O_RDWR | O_NOCTTY);
    CHECK(t2 >= 0, "open tty2");
    int q = 0;
    CHECK(ioctl(t0, VT_OPENQRY, &q) == 0 && q == 3, "OPENQRY %d", q);
    CHECK(ioctl(t0, VT_GETSTATE, &st) == 0 && (st.v_state & 6) == 6, "state %#x", st.v_state);

    /* separate consoles */
    CHECK(write(t2, "hello from vt2\n", 15) == 15, "write tty2");
    CHECK(vcs_has("/dev/vcs2", "hello from vt2"), "vcs2 has the text");
    CHECK(!vcs_has("/dev/vcs1", "hello from vt2"), "vcs1 does not");
    CHECK(vcs_has("/dev/vcs", "") && !vcs_has("/dev/vcs", "hello from vt2"), "vcs = foreground (vt1)");
    struct winsize ws = { 0 };
    CHECK(ioctl(t2, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 80, "tty2 winsize %d", ws.ws_col);

    /* per-VT modes */
    int m = -1;
    CHECK(ioctl(t2, KDGKBMODE, &m) == 0 && m == K_XLATE, "tty2 kbmode %d", m);
    CHECK(ioctl(t2, KDSKBMODE, K_OFF) == 0, "KDSKBMODE tty2");
    CHECK(ioctl(t0, KDGKBMODE, &m) == 0 && m == K_XLATE, "tty1 kbmode untouched %d", m);
    ioctl(t2, KDSKBMODE, K_XLATE);
    CHECK(ioctl(t2, KDSETMODE, KD_GRAPHICS) == 0 && ioctl(t2, KDGETMODE, &m) == 0 && m == KD_GRAPHICS, "KDSETMODE tty2");
    CHECK(ioctl(t0, KDGETMODE, &m) == 0 && m == KD_TEXT, "tty1 text %d", m);
    ioctl(t2, KDSETMODE, KD_TEXT);

    /* VT_AUTO switching */
    CHECK(ioctl(t0, VT_ACTIVATE, 2) == 0 && waitactive(t0, 2) == 0 && active(t0) == 2, "activate 2: %d", active(t0));
    CHECK(rd("/sys/class/tty/tty0/active", b, sizeof b) > 0 && !strcmp(b, "tty2"), "sysfs active %s", b);
    CHECK(vcs_has("/dev/vcs", "hello from vt2"), "vcs follows the foreground");
    int t0b = open("/dev/tty0", O_RDWR | O_NOCTTY);
    CHECK(t0b >= 0 && write(t0b, "via tty0\n", 9) == 9 && vcs_has("/dev/vcs2", "via tty0"), "tty0 opens the foreground");
    close(t0b);
    CHECK(ioctl(t0, VT_ACTIVATE, 7) < 0 && errno == ENXIO, "activate 7");
    CHECK(ioctl(t0, VT_ACTIVATE, 1) == 0 && waitactive(t0, 1) == 0 && active(t0) == 1, "back to 1");

    /* VT_PROCESS */
    int pp[2];
    pipe(pp);
    pid_t c = fork();
    if (!c) { close(pp[0]); owner(pp[1]); }
    close(pp[1]);
    CHECK(next(pp[0]) == 'R', "owner ready");
    int t3 = open("/dev/tty3", O_RDWR | O_NOCTTY);
    struct vt_mode vm = { 0 };
    CHECK(ioctl(t3, VT_GETMODE, &vm) == 0 && vm.mode == VT_PROCESS && vm.relsig == SIGUSR1 && vm.acqsig == SIGUSR2, "GETMODE %d", vm.mode);
    CHECK(ioctl(t0, VT_RELDISP, 1) < 0 && errno == EINVAL, "RELDISP on an auto VT");
    CHECK(ioctl(t0, VT_ACTIVATE, 3) == 0 && waitactive(t0, 3) == 0, "activate 3");
    CHECK(next(pp[0]) == 'A', "acqsig acknowledged");
    CHECK(ioctl(t0, VT_ACTIVATE, 1) == 0 && next(pp[0]) == 'N', "relsig, refused");
    usleep(50000);
    CHECK(active(t0) == 3, "still on 3 after refusal (%d)", active(t0));
    CHECK(ioctl(t0, VT_ACTIVATE, 1) == 0 && next(pp[0]) == 'Y', "relsig, released");
    CHECK(waitactive(t0, 1) == 0 && active(t0) == 1, "switched to 1 (%d)", active(t0));
    int stc = -1;
    waitpid(c, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "owner status %#x", stc);
    /* the owner is gone: switching to and away from its VT resets it to VT_AUTO */
    CHECK(ioctl(t0, VT_ACTIVATE, 3) == 0 && waitactive(t0, 3) == 0, "activate dead owner's VT");
    CHECK(ioctl(t3, VT_GETMODE, &vm) == 0 && vm.mode == VT_AUTO, "reset to VT_AUTO (%d)", vm.mode);
    CHECK(ioctl(t0, VT_ACTIVATE, 1) == 0 && waitactive(t0, 1) == 0, "back to 1 freely");
    close(t3);

    /* keyboard: Alt+F2 switches, typing goes to the foreground VT, Ctrl+Alt+F1 back */
    int u = open("/dev/uinput", O_RDWR);
    CHECK(u >= 0, "uinput");
    ioctl(u, UI_SET_EVBIT, EV_KEY);
    for (int k = KEY_ESC; k <= KEY_F12; k++) ioctl(u, UI_SET_KEYBIT, k);
    struct uinput_setup us = { .id = { BUS_VIRTUAL, 1, 1, 1 } };
    strcpy(us.name, "vttest keyboard");
    CHECK(ioctl(u, UI_DEV_SETUP, &us) == 0 && ioctl(u, UI_DEV_CREATE) == 0, "create keyboard");
    tcflush(t2, TCIFLUSH);
    key(u, KEY_LEFTALT, 1); key(u, KEY_F2, 1); key(u, KEY_F2, 0); key(u, KEY_LEFTALT, 0);
    CHECK(waitactive(t0, 2) == 0 && active(t0) == 2, "Alt+F2 (%d)", active(t0));
    key(u, KEY_H, 1); key(u, KEY_H, 0); key(u, KEY_I, 1); key(u, KEY_I, 0); key(u, KEY_ENTER, 1); key(u, KEY_ENTER, 0);
    memset(b, 0, sizeof b);
    int fl = fcntl(t2, F_GETFL);
    fcntl(t2, F_SETFL, fl | O_NONBLOCK);
    CHECK(read(t2, b, sizeof b) == 3 && !strcmp(b, "hi\n"), "typed into tty2: '%s'", b);
    CHECK(vcs_has("/dev/vcs2", "hi"), "echo on vt2");
    ioctl(t2, KDSKBMODE, K_OFF);                    /* a display server: no switching, no typing */
    key(u, KEY_LEFTALT, 1); key(u, KEY_F1, 1); key(u, KEY_F1, 0); key(u, KEY_LEFTALT, 0);
    key(u, KEY_X, 1); key(u, KEY_X, 0);
    usleep(100000);
    CHECK(active(t0) == 2 && read(t2, b, sizeof b) < 0 && errno == EAGAIN, "K_OFF: no switch, no input");
    ioctl(t2, KDSKBMODE, K_XLATE);
    key(u, KEY_LEFTCTRL, 1); key(u, KEY_LEFTALT, 1); key(u, KEY_F1, 1); key(u, KEY_F1, 0);
    key(u, KEY_LEFTALT, 0); key(u, KEY_LEFTCTRL, 0);
    CHECK(waitactive(t0, 1) == 0 && active(t0) == 1, "Ctrl+Alt+F1 (%d)", active(t0));
    ioctl(u, UI_DEV_DESTROY);
    close(u);
    close(t2);
    close(t0);
    if (fails) { printf("vttest: %d failures\n", fails); return 1; }
    printf("vttest: all passed\n");
    return 0;
}
