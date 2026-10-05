// wlterm: a small Wayland terminal for 9os. A pty running a shell, an 80x25 cell grid with
// the 8x16 console font, a VT100/ANSI subset (cursor motion, erase, SGR colours, scroll
// regions are not supported), US keyboard mapping from raw evdev keycodes.
// usage: wlterm [-e command] [cols rows]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

extern const uint8_t font8x16[128][16];
static int COLS = 80, ROWS = 25, W, H;
struct cell { char ch; uint8_t fg, bg; };
static struct cell *grid;
static int cx, cy, cur_fg = 7, cur_bg = 0, bold, inverse, cursor_on = 1, dirty = 1, frame_pending, running = 1;
static int esc_state, esc_args[16], esc_n, esc_priv;
static int ptm = -1;
static pid_t child;
static int shift, ctrl;

static const uint32_t palette[16] = {
    0x1c1c22, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xd0d0d0,
    0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
};

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_surface *surf;
static int configured;
struct buf { struct wl_buffer *b; uint32_t *px; int busy; };
static struct buf bufs[2];

/* ------------------------------------------------------------------ terminal state */
static struct cell *at(int x, int y) { return &grid[y * COLS + x]; }
static void clear_range(int x0, int y0, int x1, int y1) {   /* cells [y0*COLS+x0, y1*COLS+x1) */
    for (int i = y0 * COLS + x0; i < y1 * COLS + x1 && i < COLS * ROWS; i++) grid[i] = (struct cell){ ' ', 7, cur_bg };
}
static void scroll_up(void) {
    memmove(grid, grid + COLS, sizeof *grid * COLS * (ROWS - 1));
    clear_range(0, ROWS - 1, 0, ROWS);
}
static void newline(void) { if (++cy >= ROWS) { cy = ROWS - 1; scroll_up(); } }
static void put(char c) {
    if (cx >= COLS) { cx = 0; newline(); }
    int fg = cur_fg + (bold && cur_fg < 8 ? 8 : 0), bg = cur_bg;
    if (inverse) { int t = fg; fg = bg; bg = t; }
    *at(cx, cy) = (struct cell){ c, fg, bg };
    cx++;
}
static int arg(int i, int def) { return i < esc_n && esc_args[i] ? esc_args[i] : def; }
static void csi(char f) {
    switch (f) {
    case 'A': cy -= arg(0, 1); break;
    case 'B': case 'e': cy += arg(0, 1); break;
    case 'C': case 'a': cx += arg(0, 1); break;
    case 'D': cx -= arg(0, 1); break;
    case 'E': cy += arg(0, 1); cx = 0; break;
    case 'F': cy -= arg(0, 1); cx = 0; break;
    case 'G': case '`': cx = arg(0, 1) - 1; break;
    case 'd': cy = arg(0, 1) - 1; break;
    case 'H': case 'f': cy = arg(0, 1) - 1; cx = arg(1, 1) - 1; break;
    case 'J': {
        int m = arg(0, 0);
        if (m == 0) clear_range(cx, cy, 0, ROWS);
        else if (m == 1) clear_range(0, 0, cx + 1, cy);
        else clear_range(0, 0, 0, ROWS);
        break;
    }
    case 'K': {
        int m = arg(0, 0);
        if (m == 0) clear_range(cx, cy, 0, cy + 1);
        else if (m == 1) clear_range(0, cy, cx + 1, cy);
        else clear_range(0, cy, 0, cy + 1);
        break;
    }
    case 'P': {                                   /* delete chars */
        int n = arg(0, 1);
        for (int x = cx; x < COLS; x++) *at(x, cy) = x + n < COLS ? *at(x + n, cy) : (struct cell){ ' ', 7, cur_bg };
        break;
    }
    case '@': {                                   /* insert blanks */
        int n = arg(0, 1);
        for (int x = COLS - 1; x >= cx; x--) *at(x, cy) = x - n >= cx ? *at(x - n, cy) : (struct cell){ ' ', 7, cur_bg };
        break;
    }
    case 'X': for (int i = 0; i < arg(0, 1) && cx + i < COLS; i++) *at(cx + i, cy) = (struct cell){ ' ', 7, cur_bg }; break;
    case 'm':
        if (!esc_n) esc_args[esc_n++] = 0;
        for (int i = 0; i < esc_n; i++) {
            int a = esc_args[i];
            if (a == 0) { cur_fg = 7; cur_bg = 0; bold = inverse = 0; }
            else if (a == 1) bold = 1;
            else if (a == 22) bold = 0;
            else if (a == 7) inverse = 1;
            else if (a == 27) inverse = 0;
            else if (a >= 30 && a <= 37) cur_fg = a - 30;
            else if (a == 39) cur_fg = 7;
            else if (a >= 40 && a <= 47) cur_bg = a - 40;
            else if (a == 49) cur_bg = 0;
            else if (a >= 90 && a <= 97) cur_fg = a - 90 + 8;
            else if (a >= 100 && a <= 107) cur_bg = a - 100 + 8;
        }
        break;
    case 'h': case 'l': if (esc_priv && arg(0, 0) == 25) cursor_on = f == 'h'; break;
    case 'n':
        if (arg(0, 0) == 6) { char r[32]; int n = snprintf(r, sizeof r, "\033[%d;%dR", cy + 1, cx + 1); write(ptm, r, n); }
        break;
    }
    if (cx < 0) cx = 0;
    if (cx >= COLS) cx = COLS - 1;
    if (cy < 0) cy = 0;
    if (cy >= ROWS) cy = ROWS - 1;
}
static void term_input(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (esc_state == 1) {
            if (c == '[') { esc_state = 2; esc_n = 0; esc_priv = 0; memset(esc_args, 0, sizeof esc_args); continue; }
            if (c == ']') { esc_state = 3; continue; }
            if (c == 'c') { clear_range(0, 0, 0, ROWS); cx = cy = 0; }
            if (c == 'M') { if (cy > 0) cy--; }
            if (c == 'D') newline();
            esc_state = 0;
            continue;
        }
        if (esc_state == 2) {
            if (c == '?' || c == '>') { esc_priv = 1; continue; }
            if (c >= '0' && c <= '9') { if (!esc_n) esc_n = 1; esc_args[esc_n - 1] = esc_args[esc_n - 1] * 10 + c - '0'; continue; }
            if (c == ';') { if (!esc_n) esc_n = 1; if (esc_n < 16) esc_n++; continue; }
            esc_state = 0;
            csi(c);
            continue;
        }
        if (esc_state == 3) { if (c == 7 || c == '\\') esc_state = 0; continue; }   /* OSC: ignore */
        switch (c) {
        case 27: esc_state = 1; break;
        case '\r': cx = 0; break;
        case '\n': case 11: case 12: newline(); break;
        case '\b': if (cx > 0) cx--; break;
        case '\t': cx = (cx + 8) & ~7; if (cx >= COLS) cx = COLS - 1; break;
        case 7: break;
        default: if (c >= 32 && c < 127) put(c); else if (c >= 128) put('?');
        }
    }
    dirty = 1;
}

/* ------------------------------------------------------------------ keyboard (evdev codes) */
static const char lower[] = { 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 127, '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k',
    'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ' };
static const char upper[] = { 0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 127, '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K',
    'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ' };
static void key(uint32_t code, int pressed) {
    if (code == 42 || code == 54) { shift = pressed; return; }
    if (code == 29 || code == 97) { ctrl = pressed; return; }
    if (!pressed) return;
    const char *seq = NULL;
    switch (code) {
    case 103: seq = "\033[A"; break;  case 108: seq = "\033[B"; break;
    case 106: seq = "\033[C"; break;  case 105: seq = "\033[D"; break;
    case 102: seq = "\033[H"; break;  case 107: seq = "\033[F"; break;
    case 111: seq = "\033[3~"; break; case 104: seq = "\033[5~"; break; case 109: seq = "\033[6~"; break;
    }
    if (seq) { write(ptm, seq, strlen(seq)); return; }
    if (code >= sizeof lower) return;
    char c = (shift ? upper : lower)[code];
    if (!c) return;
    if (ctrl && c >= 'a' && c <= 'z') c -= 96;
    else if (ctrl && c >= 'A' && c <= 'Z') c -= 64;
    write(ptm, &c, 1);
}

/* ------------------------------------------------------------------ drawing */
static void buf_release(void *d, struct wl_buffer *b) { (void)b; ((struct buf *)d)->busy = 0; }
static const struct wl_buffer_listener buf_l = { buf_release };
static void make_bufs(void) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("wlterm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size * 2) < 0) { perror("memfd"); exit(1); }
    uint32_t *p = mmap(0, size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size * 2);
    for (int i = 0; i < 2; i++) {
        bufs[i].b = wl_shm_pool_create_buffer(pool, i * size, W, H, stride, WL_SHM_FORMAT_XRGB8888);
        bufs[i].px = p + i * W * H;
        wl_buffer_add_listener(bufs[i].b, &buf_l, &bufs[i]);
    }
    wl_shm_pool_destroy(pool);
    close(fd);
}
static void frame_done(void *d, struct wl_callback *cb, uint32_t t) { (void)d; (void)t; wl_callback_destroy(cb); frame_pending = 0; }
static const struct wl_callback_listener frame_l = { frame_done };
static void draw(void) {
    struct buf *b = !bufs[0].busy ? &bufs[0] : !bufs[1].busy ? &bufs[1] : NULL;
    if (!b) return;
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            struct cell c = *at(x, y);
            uint32_t fg = palette[c.fg & 15], bg = palette[c.bg & 15];
            if (cursor_on && x == cx && y == cy) { uint32_t t = fg; fg = bg; bg = t == bg ? 0xd0d0d0 : t; }
            const uint8_t *g = font8x16[(unsigned char)c.ch < 128 ? (unsigned char)c.ch : '?'];
            for (int r = 0; r < 16; r++) {
                uint32_t *row = b->px + (y * 16 + r) * W + x * 8;
                for (int i = 0; i < 8; i++) row[i] = g[r] & (0x80 >> i) ? fg : bg;
            }
        }
    struct wl_callback *cb = wl_surface_frame(surf);
    wl_callback_add_listener(cb, &frame_l, NULL);
    frame_pending = 1;
    wl_surface_attach(surf, b->b, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    b->busy = 1;
    dirty = 0;
}

/* ------------------------------------------------------------------ wayland plumbing */
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { (void)d; xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
static void xs_configure(void *d, struct xdg_surface *x, uint32_t s) { (void)d; xdg_surface_ack_configure(x, s); configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; running = 0; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) { (void)d; (void)k; (void)f; (void)s; close(fd); }
static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *a) { (void)d; (void)k; (void)s; (void)sf; (void)a; }
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d; (void)k; (void)s; (void)sf; shift = ctrl = 0; }
static void k_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t code, uint32_t st) { (void)d; (void)k; (void)s; (void)t; key(code, st == WL_KEYBOARD_KEY_STATE_PRESSED); }
static void k_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)s; (void)a; (void)b; (void)c; (void)g; }
static void k_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) { (void)d; (void)k; (void)r; (void)dl; }
static const struct wl_keyboard_listener kbd_l = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    static int done;
    if (!done && (caps & WL_SEAT_CAPABILITY_KEYBOARD)) { done = 1; wl_keyboard_add_listener(wl_seat_get_keyboard(s), &kbd_l, NULL); }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_l = { seat_caps, seat_name };
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, v < 4 ? v : 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wm_l, NULL); }
    else if (!strcmp(iface, wl_seat_interface.name)) { seat = wl_registry_bind(r, name, &wl_seat_interface, v < 5 ? v : 5); wl_seat_add_listener(seat, &seat_l, NULL); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void spawn(const char *cmd) {
    ptm = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (ptm < 0 || grantpt(ptm) || unlockpt(ptm)) { perror("wlterm: ptmx"); exit(1); }
    struct winsize ws = { .ws_row = ROWS, .ws_col = COLS, .ws_xpixel = W, .ws_ypixel = H };
    ioctl(ptm, TIOCSWINSZ, &ws);
    char *name = ptsname(ptm);
    child = fork();
    if (child == 0) {
        setsid();
        int s = open(name, O_RDWR);
        if (s < 0) _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        setenv("TERM", "linux", 1);
        if (cmd) execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        else execl("/bin/sh", "-sh", (char *)NULL);
        _exit(127);
    }
    fcntl(ptm, F_SETFL, O_NONBLOCK);
}

int main(int argc, char **argv) {
    const char *cmd = NULL;
    int ai = 1;
    if (ai + 1 < argc && !strcmp(argv[ai], "-e")) { cmd = argv[ai + 1]; ai += 2; }
    if (ai + 1 < argc) { COLS = atoi(argv[ai]); ROWS = atoi(argv[ai + 1]); }
    if (COLS < 10 || COLS > 300) COLS = 80;
    if (ROWS < 4 || ROWS > 120) ROWS = 25;
    W = COLS * 8; H = ROWS * 16;
    grid = calloc(COLS * ROWS, sizeof *grid);
    clear_range(0, 0, 0, ROWS);
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "wlterm: cannot connect to compositor\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "wlterm: missing globals\n"); return 1; }
    wl_display_roundtrip(dpy);
    make_bufs();
    surf = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, cmd ? cmd : "wlterm");
    xdg_toplevel_set_app_id(top, "wlterm");
    wl_surface_commit(surf);
    while (!configured && wl_display_dispatch(dpy) != -1) {}
    spawn(cmd);
    signal(SIGPIPE, SIG_IGN);
    int child_done = 0;
    while (running) {
        if (dirty && !frame_pending) draw();
        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct pollfd p[2] = { { wl_display_get_fd(dpy), POLLIN, 0 }, { ptm, POLLIN, 0 } };
        int np = child_done ? 1 : 2;
        if (poll(p, np, child_done ? 100 : -1) < 0 && errno != EINTR) { wl_display_cancel_read(dpy); break; }
        if (p[0].revents & POLLIN) wl_display_read_events(dpy); else wl_display_cancel_read(dpy);
        if (wl_display_dispatch_pending(dpy) < 0) break;
        if (np > 1 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char b[4096];
            ssize_t n = read(ptm, b, sizeof b);
            if (n > 0) term_input(b, n);
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) child_done = 1;
        }
        if (child_done && !dirty && !frame_pending) break;   /* show the last output, then exit */
    }
    if (child > 0) { kill(child, SIGHUP); waitpid(child, NULL, 0); }
    wl_display_disconnect(dpy);
    return 0;
}
