/*
 * wlkms: a small Wayland compositor for 9os.
 *   output : KMS through libdrm (one dumb buffer, damage-clipped repaint + DIRTYFB clips,
 *            vblank-event-paced frame callbacks)
 *   render : pixman (background, windows with title bars, cursor), copying surface contents
 *   input  : evdev (/dev/input/event*, grabbed): keyboard → focused wl_keyboard,
 *            tablet/mouse → cursor, wl_pointer enter/motion/button, click-to-focus/raise,
 *            drag title bars (or xdg_toplevel.move) to move windows
 *   protocols: wl_compositor, wl_subcompositor-less, wl_shm, wl_seat, wl_output, xdg_wm_base
 * usage: wlkms [seconds]   (0 = run until Ctrl+Alt+Backspace)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <pixman.h>
#include <wayland-server.h>
#include "xdg-shell-server-protocol.h"

#define TITLE_H 22
#define BORDER 2

struct fbuf { uint32_t handle, fb, pitch; uint64_t size; uint32_t *map; pixman_image_t *img; };
struct surface {
    struct wl_list link;                 /* stacking order, bottom first */
    struct wl_resource *res, *xdg, *toplevel;
    struct wl_resource *pending_buf;
    struct wl_list frame_cbs, pending_cbs;
    pixman_image_t *img;                 /* copy of the last committed buffer */
    int w, h, x, y, mapped;
    char title[64];
    uint32_t configure_serial;
};

static struct {
    struct wl_display *dpy;
    struct wl_event_loop *loop;
    int drm, crtc, conn;
    drmModeModeInfo mode;
    int W, H;
    struct fbuf buf[1];
    int back, vbl_pending, dirty, frames;
    pixman_region32_t dmg;               /* screen damage since the last repaint */
    int cur_x, cur_y;                    /* where the cursor was last drawn */
    uint64_t pixels;                     /* repainted pixel count (stats) */
    double render_ms;
    struct wl_list surfaces;
    struct wl_list pointers, keyboards;  /* resources */
    struct surface *focus, *hover, *drag;
    int cx, cy, drag_dx, drag_dy, buttons;
    int ctrl, alt;
    uint32_t serial;
    int quit;
    pixman_image_t *bg, *cursor;
} C;

static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* ------------------------------------------------------------------ KMS */
static int kms_init(void) {
    C.drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (C.drm < 0) { perror("wlkms: /dev/dri/card0"); return -1; }
    drmSetMaster(C.drm);
    drmModeRes *r = drmModeGetResources(C.drm);
    if (!r) return -1;
    drmModeConnector *conn = NULL;
    for (int i = 0; i < r->count_connectors; i++) {
        conn = drmModeGetConnector(C.drm, r->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes) break;
        drmModeFreeConnector(conn); conn = NULL;
    }
    if (!conn) { fprintf(stderr, "wlkms: no connected output\n"); return -1; }
    C.conn = conn->connector_id;
    C.mode = conn->modes[0];
    C.W = C.mode.hdisplay; C.H = C.mode.vdisplay;
    drmModeEncoder *enc = drmModeGetEncoder(C.drm, conn->encoder_id ? conn->encoder_id : conn->encoders[0]);
    C.crtc = r->crtcs[0];
    for (int i = 0; enc && i < r->count_crtcs; i++) if (enc->possible_crtcs & (1u << i)) { C.crtc = r->crtcs[i]; break; }
    drmModeFreeEncoder(enc); drmModeFreeConnector(conn); drmModeFreeResources(r);
    for (int i = 0; i < 1; i++) {
        struct drm_mode_create_dumb cd = { .width = C.W, .height = C.H, .bpp = 32 };
        if (drmIoctl(C.drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) return -1;
        struct fbuf *b = &C.buf[i];
        b->handle = cd.handle; b->pitch = cd.pitch; b->size = cd.size;
        if (drmModeAddFB(C.drm, C.W, C.H, 24, 32, b->pitch, b->handle, &b->fb)) return -1;
        struct drm_mode_map_dumb md = { .handle = b->handle };
        if (drmIoctl(C.drm, DRM_IOCTL_MODE_MAP_DUMB, &md)) return -1;
        b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, C.drm, md.offset);
        if (b->map == MAP_FAILED) return -1;
        b->img = pixman_image_create_bits(PIXMAN_x8r8g8b8, C.W, C.H, b->map, b->pitch);
    }
    return 0;
}

/* ------------------------------------------------------------------ rendering */
static const char *cursor_bits[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
    "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   ",
};
static void make_assets(void) {
    C.bg = pixman_image_create_bits(PIXMAN_x8r8g8b8, C.W, C.H, NULL, 0);
    uint32_t *p = pixman_image_get_data(C.bg);
    int stride = pixman_image_get_stride(C.bg) / 4;
    for (int y = 0; y < C.H; y++)
        for (int x = 0; x < C.W; x++) {
            int r = 20 + 30 * y / C.H, g = 40 + 60 * y / C.H, b = 90 + 100 * y / C.H;
            if (((x / 40) + (y / 40)) % 2 == 0) { r += 6; g += 6; b += 8; }
            p[y * stride + x] = 0xff000000u | r << 16 | g << 8 | b;
        }
    C.cursor = pixman_image_create_bits(PIXMAN_a8r8g8b8, 12, 19, NULL, 0);
    uint32_t *c = pixman_image_get_data(C.cursor);
    for (int y = 0; y < 19; y++)
        for (int x = 0; x < 12; x++)
            c[y * 12 + x] = cursor_bits[y][x] == 'X' ? 0xff000000 : cursor_bits[y][x] == '.' ? 0xffffffff : 0;
}

static void fill(pixman_image_t *dst, uint32_t argb, int x, int y, int w, int h) {
    pixman_color_t col = { ((argb >> 16) & 0xff) * 257, ((argb >> 8) & 0xff) * 257, (argb & 0xff) * 257, ((argb >> 24) & 0xff) * 257 };
    pixman_box32_t box = { x, y, x + w, y + h };
    pixman_image_fill_boxes(PIXMAN_OP_OVER, dst, &col, 1, &box);
}

static void dmg(int x, int y, int w, int h) {
    pixman_region32_union_rect(&C.dmg, &C.dmg, x, y, w, h);
    C.dirty = 1;
}
static void win_dmg(struct surface *s) {       /* frame + shadow */
    if (s && s->mapped) dmg(s->x, s->y, s->w + 2 * BORDER + 6, s->h + TITLE_H + BORDER + 6);
}
static void render(void) {
    struct fbuf *b = &C.buf[0];
    pixman_region32_intersect_rect(&C.dmg, &C.dmg, 0, 0, C.W, C.H);
    pixman_image_set_clip_region32(b->img, &C.dmg);
    pixman_image_composite32(PIXMAN_OP_SRC, C.bg, NULL, b->img, 0, 0, 0, 0, 0, 0, C.W, C.H);
    struct surface *s;
    wl_list_for_each(s, &C.surfaces, link) {
        if (!s->mapped || !s->img) continue;
        int focused = s == C.focus;
        fill(b->img, 0x60000000, s->x + 6, s->y + 6, s->w + 2 * BORDER, s->h + TITLE_H + BORDER);       /* shadow */
        fill(b->img, focused ? 0xff3b6ea8 : 0xff5a5a66, s->x, s->y, s->w + 2 * BORDER, s->h + TITLE_H + BORDER);
        int tl = strlen(s->title);                       /* "text": little blocks per character */
        for (int i = 0; i < tl && i * 7 < s->w - 30; i++)
            if (s->title[i] != ' ') fill(b->img, 0xffe8eef6, s->x + 8 + i * 7, s->y + 7, 5, 8);
        fill(b->img, 0xffd05050, s->x + s->w + 2 * BORDER - 18, s->y + 5, 12, 12);                    /* close box */
        pixman_image_composite32(PIXMAN_OP_OVER, s->img, NULL, b->img, 0, 0, 0, 0,
                                 s->x + BORDER, s->y + TITLE_H, s->w, s->h);
    }
    pixman_image_composite32(PIXMAN_OP_OVER, C.cursor, NULL, b->img, 0, 0, 0, 0, C.cx, C.cy, 12, 19);
    C.cur_x = C.cx; C.cur_y = C.cy;
    pixman_image_set_clip_region32(b->img, NULL);
}

static void send_frame_done(void) {
    struct surface *s;
    uint32_t t = now_ms();
    wl_list_for_each(s, &C.surfaces, link) {
        struct wl_resource *cb, *tmp;
        wl_resource_for_each_safe(cb, tmp, &s->frame_cbs) {
            wl_callback_send_done(cb, t);
            wl_resource_destroy(cb);
        }
    }
}

static void wait_vblank(void) {
    drmVBlank v = { .request = { .type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT, .sequence = 1 } };
    if (drmWaitVBlank(C.drm, &v) == 0) C.vbl_pending = 1;
}
static void repaint(void) {
    if (C.vbl_pending || !C.dirty) return;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    render();
    int n = 0;
    pixman_box32_t *bx = pixman_region32_rectangles(&C.dmg, &n);
    drmModeClip clips[64];
    if (n > 64) { bx = pixman_region32_extents(&C.dmg); n = 1; }
    for (int i = 0; i < n; i++) {
        clips[i] = (drmModeClip){ bx[i].x1, bx[i].y1, bx[i].x2, bx[i].y2 };
        C.pixels += (uint64_t)(bx[i].x2 - bx[i].x1) * (bx[i].y2 - bx[i].y1);
    }
    if (n) drmModeDirtyFB(C.drm, C.buf[0].fb, clips, n);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    C.render_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    pixman_region32_clear(&C.dmg);
    C.dirty = 0;
    C.frames++;
    wait_vblank();
}

static void vblank_handler(int fd, unsigned seq, unsigned sec, unsigned usec, void *data) {
    C.vbl_pending = 0;
    send_frame_done();
    repaint();
}
static int on_drm(int fd, uint32_t mask, void *data) {
    drmEventContext ev = { .version = 2, .vblank_handler = vblank_handler };
    drmHandleEvent(fd, &ev);
    return 0;
}
static int on_idle_timer(void *data) {          /* frame callbacks keep flowing while idle */
    if (!C.vbl_pending) { send_frame_done(); repaint(); }
    wl_event_source_timer_update(*(struct wl_event_source **)data, 16);
    return 0;
}

/* ------------------------------------------------------------------ surfaces */
static void surf_free(struct wl_resource *r) {
    struct surface *s = wl_resource_get_user_data(r);
    wl_list_remove(&s->link);
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->frame_cbs) wl_resource_destroy(cb);
    wl_resource_for_each_safe(cb, tmp, &s->pending_cbs) wl_resource_destroy(cb);
    win_dmg(s);
    if (s->img) pixman_image_unref(s->img);
    if (C.focus == s) C.focus = NULL;
    if (C.hover == s) C.hover = NULL;
    if (C.drag == s) C.drag = NULL;
    free(s);
}
static void res_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void surf_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y) {
    struct surface *s = wl_resource_get_user_data(r); s->pending_buf = buf;
}
static void surf_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void cb_unlink(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surface *s = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(cb, NULL, NULL, cb_unlink);
    wl_list_insert(s->pending_cbs.prev, wl_resource_get_link(cb));
}
static void surf_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) {}
static void surf_commit(struct wl_client *c, struct wl_resource *r) {
    struct surface *s = wl_resource_get_user_data(r);
    if (s->pending_buf) {
        struct wl_shm_buffer *b = wl_shm_buffer_get(s->pending_buf);
        if (b) {
            int w = wl_shm_buffer_get_width(b), h = wl_shm_buffer_get_height(b);
            uint32_t fmt = wl_shm_buffer_get_format(b);
            if (!s->img || s->w != w || s->h != h) {
                win_dmg(s);
                if (s->img) pixman_image_unref(s->img);
                s->img = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, NULL, 0);
                s->w = w; s->h = h;
            }
            wl_shm_buffer_begin_access(b);
            pixman_image_t *src = pixman_image_create_bits(fmt == WL_SHM_FORMAT_XRGB8888 ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8,
                                                           w, h, wl_shm_buffer_get_data(b), wl_shm_buffer_get_stride(b));
            pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, s->img, 0, 0, 0, 0, 0, 0, w, h);
            pixman_image_unref(src);
            wl_shm_buffer_end_access(b);
            if (!s->mapped && s->toplevel) {                         /* cascade new windows */
                static int n;
                s->x = 60 + (n % 6) * 60; s->y = 50 + (n % 6) * 50; n++;
                C.focus = s;
            }
            s->mapped = 1;
            win_dmg(s);
        }
        wl_buffer_send_release(s->pending_buf);
        s->pending_buf = NULL;
    }
    wl_list_insert_list(s->frame_cbs.prev, &s->pending_cbs);
    wl_list_init(&s->pending_cbs);
    repaint();
}
static void surf_i32(struct wl_client *c, struct wl_resource *r, int32_t v) {}
static void surf_dmg_buf(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void surf_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {}
static const struct wl_surface_interface surf_impl = {
    res_destroy, surf_attach, surf_damage, surf_frame, surf_region, surf_region, surf_commit,
    surf_i32, surf_i32, surf_dmg_buf, surf_offset,
};
static void reg_op(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static const struct wl_region_interface region_impl = { res_destroy, reg_op, reg_op };
static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surface *s = calloc(1, sizeof *s);
    s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &surf_impl, s, surf_free);
    wl_list_init(&s->frame_cbs); wl_list_init(&s->pending_cbs);
    wl_list_insert(C.surfaces.prev, &s->link);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *s = wl_resource_create(c, &wl_region_interface, 1, id);
    wl_resource_set_implementation(s, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface comp_impl = { comp_create_surface, comp_create_region };
static void bind_comp(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, ver < 5 ? ver : 5, id);
    wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}

/* ------------------------------------------------------------------ xdg-shell */
static void raise_focus(struct surface *s);
static void tl_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) {}
static void tl_set_title(struct wl_client *c, struct wl_resource *r, const char *t) {
    struct surface *s = wl_resource_get_user_data(r); snprintf(s->title, sizeof s->title, "%s", t); win_dmg(s);
}
static void tl_set_app_id(struct wl_client *c, struct wl_resource *r, const char *t) {}
static void tl_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {}
static void tl_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
    struct surface *s = wl_resource_get_user_data(r);
    if (C.buttons) { C.drag = s; C.drag_dx = C.cx - s->x; C.drag_dy = C.cy - s->y; }
}
static void tl_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t e) {}
static void tl_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {}
static void tl_noop(struct wl_client *c, struct wl_resource *r) {}
static void tl_set_fs(struct wl_client *c, struct wl_resource *r, struct wl_resource *out) {}
static const struct xdg_toplevel_interface toplevel_impl = {
    res_destroy, tl_set_parent, tl_set_title, tl_set_app_id, tl_menu, tl_move, tl_resize, tl_size, tl_size,
    tl_noop, tl_noop, tl_set_fs, tl_noop, tl_noop,
};
static void toplevel_gone(struct wl_resource *r) {
    struct surface *s = wl_resource_get_user_data(r);
    if (s) { win_dmg(s); s->toplevel = NULL; s->mapped = 0; }
}
static void xs_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surface *s = wl_resource_get_user_data(r);
    s->toplevel = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->toplevel, &toplevel_impl, s, toplevel_gone);
    struct wl_array states; wl_array_init(&states);
    *(uint32_t *)wl_array_add(&states, sizeof(uint32_t)) = XDG_TOPLEVEL_STATE_ACTIVATED;
    xdg_toplevel_send_configure(s->toplevel, 0, 0, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(r, s->configure_serial = ++C.serial);
}
static void xs_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent, struct wl_resource *pos) {
    wl_resource_post_error(r, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT, "popups not supported");
}
static void xs_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t serial) {}
static const struct xdg_surface_interface xdg_surface_impl = { res_destroy, xs_get_toplevel, xs_get_popup, xs_geometry, xs_ack };
static void pos_noop_i(struct wl_client *c, struct wl_resource *r, int32_t a, int32_t b) {}
static void pos_noop_r(struct wl_client *c, struct wl_resource *r, int32_t a, int32_t b, int32_t w, int32_t h) {}
static void pos_noop_u(struct wl_client *c, struct wl_resource *r, uint32_t a) {}
static void pos_noop(struct wl_client *c, struct wl_resource *r) {}
static const struct xdg_positioner_interface pos_impl = {
    res_destroy, pos_noop_i, pos_noop_r, pos_noop_u, pos_noop_u, pos_noop_u, pos_noop_i, pos_noop, pos_noop_i, pos_noop_u,
};
static void wm_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(p, &pos_impl, NULL, NULL);
}
static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf) {
    struct surface *s = wl_resource_get_user_data(surf);
    s->xdg = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->xdg, &xdg_surface_impl, s, NULL);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) {}
static const struct xdg_wm_base_interface wm_impl = { res_destroy, wm_create_positioner, wm_get_xdg_surface, wm_pong };
static void bind_wm(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, ver < 5 ? ver : 5, id);
    wl_resource_set_implementation(r, &wm_impl, NULL, NULL);
}

/* ------------------------------------------------------------------ seat + output */
static void unlink_res(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *s, int32_t x, int32_t y) {}
static const struct wl_pointer_interface pointer_impl = { ptr_set_cursor, res_destroy };
static const struct wl_keyboard_interface keyboard_impl = { res_destroy };
static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(p, &pointer_impl, NULL, unlink_res);
    wl_list_insert(&C.pointers, wl_resource_get_link(p));
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(k, &keyboard_impl, NULL, unlink_res);
    wl_list_insert(&C.keyboards, wl_resource_get_link(k));
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 0);
    close(fd);
    if (wl_resource_get_version(k) >= 4) wl_keyboard_send_repeat_info(k, 25, 400);
    if (C.focus && wl_resource_get_client(C.focus->res) == c) {
        struct wl_array keys; wl_array_init(&keys);
        wl_keyboard_send_enter(k, ++C.serial, C.focus->res, &keys);
        wl_array_release(&keys);
    }
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id) {}
static const struct wl_seat_interface seat_impl = { seat_get_pointer, seat_get_keyboard, seat_get_touch, res_destroy };
static void bind_seat(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, ver < 7 ? ver : 7, id);
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (ver >= 2) wl_seat_send_name(r, "seat0");
}
static const struct wl_output_interface output_impl = { res_destroy };
static void bind_output(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, ver < 3 ? ver : 3, id);
    wl_resource_set_implementation(r, &output_impl, NULL, NULL);
    wl_output_send_geometry(r, 0, 0, C.W * 254 / 960, C.H * 254 / 960, WL_OUTPUT_SUBPIXEL_UNKNOWN, "9os", "Virtual-1", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, C.W, C.H, 60000);
    if (ver >= 2) { wl_output_send_scale(r, 1); wl_output_send_done(r); }
}

/* ------------------------------------------------------------------ input */
static void send_kbd_focus(struct surface *old, struct surface *new) {
    struct wl_resource *k;
    struct wl_array keys; wl_array_init(&keys);
    wl_resource_for_each(k, &C.keyboards) {
        struct wl_client *kc = wl_resource_get_client(k);
        if (old && wl_resource_get_client(old->res) == kc) wl_keyboard_send_leave(k, ++C.serial, old->res);
        if (new && wl_resource_get_client(new->res) == kc) wl_keyboard_send_enter(k, ++C.serial, new->res, &keys);
    }
    wl_array_release(&keys);
}
static void raise_focus(struct surface *s) {
    win_dmg(C.focus); win_dmg(s);
    if (s != C.focus) { send_kbd_focus(C.focus, s); C.focus = s; }
    wl_list_remove(&s->link); wl_list_insert(C.surfaces.prev, &s->link);
}
static struct surface *surface_at(int x, int y, int *in_title) {
    struct surface *s;
    wl_list_for_each_reverse(s, &C.surfaces, link) {
        if (!s->mapped) continue;
        if (x >= s->x && x < s->x + s->w + 2 * BORDER && y >= s->y && y < s->y + s->h + TITLE_H + BORDER) {
            *in_title = y < s->y + TITLE_H;
            return s;
        }
    }
    return NULL;
}
static void pointer_motion(void) {
    int in_title = 0;
    struct surface *s = C.drag ? C.drag : surface_at(C.cx, C.cy, &in_title);
    if (C.drag) { win_dmg(C.drag); C.drag->x = C.cx - C.drag_dx; C.drag->y = C.cy - C.drag_dy; win_dmg(C.drag); }
    struct wl_resource *p;
    uint32_t t = now_ms();
    if (s != C.hover) {
        wl_resource_for_each(p, &C.pointers) {
            struct wl_client *pc = wl_resource_get_client(p);
            if (C.hover && wl_resource_get_client(C.hover->res) == pc) wl_pointer_send_leave(p, ++C.serial, C.hover->res);
            if (s && wl_resource_get_client(s->res) == pc)
                wl_pointer_send_enter(p, ++C.serial, s->res, wl_fixed_from_int(C.cx - s->x - BORDER), wl_fixed_from_int(C.cy - s->y - TITLE_H));
        }
        C.hover = s;
    } else if (s) {
        wl_resource_for_each(p, &C.pointers)
            if (wl_resource_get_client(p) == wl_resource_get_client(s->res))
                wl_pointer_send_motion(p, t, wl_fixed_from_int(C.cx - s->x - BORDER), wl_fixed_from_int(C.cy - s->y - TITLE_H));
    }
    wl_resource_for_each(p, &C.pointers) if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    dmg(C.cur_x, C.cur_y, 12, 19); dmg(C.cx, C.cy, 12, 19);
}
static void pointer_button(int code, int pressed) {
    C.buttons += pressed ? 1 : -1;
    if (C.buttons < 0) C.buttons = 0;
    int in_title = 0;
    struct surface *s = surface_at(C.cx, C.cy, &in_title);
    if (pressed && s) {
        raise_focus(s);
        if (in_title) {
            if (C.cx >= s->x + s->w + 2 * BORDER - 18 && s->toplevel) { xdg_toplevel_send_close(s->toplevel); return; }
            C.drag = s; C.drag_dx = C.cx - s->x; C.drag_dy = C.cy - s->y;
            return;
        }
    }
    if (!pressed && C.drag) { C.drag = NULL; return; }
    if (s && !in_title) {
        struct wl_resource *p;
        wl_resource_for_each(p, &C.pointers)
            if (wl_resource_get_client(p) == wl_resource_get_client(s->res)) {
                wl_pointer_send_button(p, ++C.serial, now_ms(), code, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
            }
    }
}
static void key_event(int code, int value) {
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) C.ctrl = value != 0;
    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) C.alt = value != 0;
    if (value == 1 && code == KEY_BACKSPACE && C.ctrl && C.alt) { C.quit = 1; return; }
    if (value == 2 || !C.focus) return;                  /* clients do their own key repeat */
    struct wl_resource *k;
    wl_resource_for_each(k, &C.keyboards)
        if (wl_resource_get_client(k) == wl_resource_get_client(C.focus->res))
            wl_keyboard_send_key(k, ++C.serial, now_ms(), code, value ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}
struct evdev { int fd; int absmax[2]; int abs; };
static int on_input(int fd, uint32_t mask, void *data) {
    struct evdev *d = data;
    struct input_event ev[32];
    ssize_t n = read(fd, ev, sizeof ev);
    for (int i = 0; i < n / (ssize_t)sizeof ev[0]; i++) {
        struct input_event *e = &ev[i];
        if (e->type == EV_KEY && e->code >= BTN_MOUSE && e->code < BTN_JOYSTICK) pointer_button(e->code, e->value);
        else if (e->type == EV_KEY) key_event(e->code, e->value);
        else if (e->type == EV_ABS && e->code <= ABS_Y && d->absmax[e->code] > 0) {
            int v = (int)((long)e->value * (e->code == ABS_X ? C.W - 1 : C.H - 1) / d->absmax[e->code]);
            if (e->code == ABS_X) C.cx = v; else C.cy = v;
        } else if (e->type == EV_REL && e->code == REL_X) C.cx += e->value;
        else if (e->type == EV_REL && e->code == REL_Y) C.cy += e->value;
        else if (e->type == EV_SYN) {
            if (C.cx < 0) C.cx = 0;
            if (C.cy < 0) C.cy = 0;
            if (C.cx >= C.W) C.cx = C.W - 1;
            if (C.cy >= C.H) C.cy = C.H - 1;
            pointer_motion();
        }
    }
    repaint();
    return 0;
}
static void input_init(void) {
    for (int i = 0; i < 16; i++) {
        char path[32]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) break;
        ioctl(fd, EVIOCGRAB, 1);
        struct evdev *d = calloc(1, sizeof *d);
        d->fd = fd;
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) d->absmax[0] = ai.maximum;
        if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) d->absmax[1] = ai.maximum;
        wl_event_loop_add_fd(C.loop, fd, WL_EVENT_READABLE, on_input, d);
    }
}

static int on_quit_timer(void *data) { C.quit = 1; return 0; }

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 0;
    if (!getenv("XDG_RUNTIME_DIR")) { mkdir("/run", 0755); mkdir("/run/user", 0755); mkdir("/run/user/0", 0700); setenv("XDG_RUNTIME_DIR", "/run/user/0", 1); };
    wl_list_init(&C.surfaces); wl_list_init(&C.pointers); wl_list_init(&C.keyboards);
    if (kms_init()) return 1;
    make_assets();
    C.cx = C.W / 2; C.cy = C.H / 2;
    C.dpy = wl_display_create();
    C.loop = wl_display_get_event_loop(C.dpy);
    const char *sock = wl_display_add_socket_auto(C.dpy);
    if (!sock) { fprintf(stderr, "wlkms: no socket\n"); return 1; }
    wl_display_init_shm(C.dpy);
    wl_global_create(C.dpy, &wl_compositor_interface, 5, NULL, bind_comp);
    wl_global_create(C.dpy, &xdg_wm_base_interface, 5, NULL, bind_wm);
    wl_global_create(C.dpy, &wl_seat_interface, 7, NULL, bind_seat);
    wl_global_create(C.dpy, &wl_output_interface, 3, NULL, bind_output);
    wl_event_loop_add_fd(C.loop, C.drm, WL_EVENT_READABLE, on_drm, NULL);
    input_init();
    static struct wl_event_source *idle;
    idle = wl_event_loop_add_timer(C.loop, on_idle_timer, &idle);
    wl_event_source_timer_update(idle, 16);
    if (secs) wl_event_source_timer_update(wl_event_loop_add_timer(C.loop, on_quit_timer, NULL), secs * 1000);
    /* first frame: modeset */
    pixman_region32_init_rect(&C.dmg, 0, 0, C.W, C.H);
    render();
    pixman_region32_clear(&C.dmg);
    if (drmModeSetCrtc(C.drm, C.crtc, C.buf[0].fb, 0, 0, (uint32_t *)&C.conn, 1, &C.mode)) { perror("wlkms: SetCrtc"); return 1; }
    printf("wlkms: %dx%d on WAYLAND_DISPLAY=%s\n", C.W, C.H, sock);
    fflush(stdout);
    uint32_t t0 = now_ms();
    while (!C.quit) {
        wl_display_flush_clients(C.dpy);
        wl_event_loop_dispatch(C.loop, 100);
    }
    uint32_t dt = now_ms() - t0;
    int nwin = 0; struct surface *s; wl_list_for_each(s, &C.surfaces, link) nwin += s->mapped;
    printf("wlkms: %d frames in %.1f s (%.1f fps), %.1f Mpixels repainted (%.1f ms/frame), %d windows at exit\n", C.frames, dt / 1000.0,
           C.frames * 1000.0 / (dt ? dt : 1), C.pixels / 1e6, C.frames ? C.render_ms / C.frames : 0, nwin);
    wl_display_destroy_clients(C.dpy);
    wl_display_destroy(C.dpy);
    drmModeSetCrtc(C.drm, C.crtc, 0, 0, 0, NULL, 0, NULL);   /* give the console back */
    return 0;
}
