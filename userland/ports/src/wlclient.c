// wlclient: simple xdg-shell Wayland client for 9os. Animated pattern drawn into
// double-buffered wl_shm buffers on frame callbacks; click changes palette; 'q' quits.
// usage: wlclient [seconds]
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static int W = 320, H = 240, configured, running = 1, palette;
static unsigned frames;
struct buf { struct wl_buffer *b; uint32_t *px; int busy; };
static struct buf bufs[2];

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void buf_release(void *d, struct wl_buffer *b) { (void)b; ((struct buf *)d)->busy = 0; }
static const struct wl_buffer_listener buf_l = { buf_release };

static void make_bufs(void) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("wlclient", 0);
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

static void draw(uint32_t t);
static void frame_done(void *d, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_l = { frame_done };

static void redraw(uint32_t t) {
    struct buf *b = !bufs[0].busy ? &bufs[0] : !bufs[1].busy ? &bufs[1] : 0;
    struct wl_callback *cb = wl_surface_frame(surf);
    wl_callback_add_listener(cb, &frame_l, 0);
    if (b) {
        uint32_t *px = b->px;
        int off = t / 8;
        static const uint32_t pal[3][2] = { { 0x2060c0, 0xffc040 }, { 0x30a050, 0xe04080 }, { 0x603090, 0x40e0e0 } };
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int v = ((x + off) ^ (y + off / 2)) & 63;
                uint32_t a = pal[palette][0], c = pal[palette][1];
                uint32_t r = (((a >> 16) & 255) * (63 - v) + ((c >> 16) & 255) * v) / 63;
                uint32_t g = (((a >> 8) & 255) * (63 - v) + ((c >> 8) & 255) * v) / 63;
                uint32_t bl = ((a & 255) * (63 - v) + (c & 255) * v) / 63;
                px[y * W + x] = r << 16 | g << 8 | bl;
            }
        wl_surface_attach(surf, b->b, 0, 0);
        wl_surface_damage_buffer(surf, 0, 0, W, H);
        b->busy = 1;
        frames++;
    }
    wl_surface_commit(surf);
}
static void draw(uint32_t t) { redraw(t); }
static void frame_done(void *d, struct wl_callback *cb, uint32_t t) { (void)d; wl_callback_destroy(cb); draw(t); }

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { (void)d; xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
static void xs_configure(void *d, struct xdg_surface *x, uint32_t s) {
    (void)d; xdg_surface_ack_configure(x, s);
    if (!configured) { configured = 1; draw(0); }
}
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; running = 0; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)s; (void)sf; (void)x; (void)y; }
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) { (void)d; (void)p; (void)s; (void)sf; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; }
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
    (void)d; (void)p; (void)s; (void)t; (void)b;
    if (st == WL_POINTER_BUTTON_STATE_PRESSED) { palette = (palette + 1) % 3; printf("wlclient: click -> palette %d\n", palette); }
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_axis_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_axis_rel(void *d, struct wl_pointer *p, uint32_t a, uint32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_l = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_axis_source, p_axis_stop, p_axis_discrete, p_axis_v120, p_axis_rel };

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) { (void)d; (void)k; (void)f; (void)s; close(fd); }
static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *a) { (void)d; (void)k; (void)s; (void)sf; (void)a; }
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d; (void)k; (void)s; (void)sf; }
static void k_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) {
    (void)d; (void)k; (void)s; (void)t;
    if (st == WL_KEYBOARD_KEY_STATE_PRESSED) { printf("wlclient: key %u\n", key); if (key == 16 /* KEY_Q */) running = 0; }
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)s; (void)a; (void)b; (void)c; (void)g; }
static void k_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) { (void)d; (void)k; (void)r; (void)dl; }
static const struct wl_keyboard_listener kbd_l = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    static int done;
    if (done) return;
    done = 1;
    if (caps & WL_SEAT_CAPABILITY_POINTER) wl_pointer_add_listener(wl_seat_get_pointer(s), &ptr_l, 0);
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) wl_keyboard_add_listener(wl_seat_get_keyboard(s), &kbd_l, 0);
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_l = { seat_caps, seat_name };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, v < 4 ? v : 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wm_l, 0); }
    else if (!strcmp(iface, wl_seat_interface.name)) { seat = wl_registry_bind(r, name, &wl_seat_interface, v < 5 ? v : 5); wl_seat_add_listener(seat, &seat_l, 0); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 0;
    if (argc > 2) W = atoi(argv[2]);
    if (argc > 3) H = atoi(argv[3]);
    struct wl_display *dpy = wl_display_connect(0);
    if (!dpy) { fprintf(stderr, "wlclient: cannot connect to compositor\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, 0);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "wlclient: missing globals\n"); return 1; }
    wl_display_roundtrip(dpy);
    make_bufs();
    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xsurf, &xs_l, 0);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_l, 0);
    char title[32];
    snprintf(title, sizeof title, "wlclient %d", getpid());
    xdg_toplevel_set_title(top, title);
    wl_surface_commit(surf);
    double t0 = now();
    while (running && wl_display_dispatch(dpy) != -1)
        if (secs > 0 && now() - t0 >= secs) break;
    double el = now() - t0;
    printf("wlclient: %u frames in %.2fs (%.1f fps)\n", frames, el, frames / (el > 0 ? el : 1));
    wl_display_disconnect(dpy);
    return 0;
}
