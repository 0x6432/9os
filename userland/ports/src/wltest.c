/*
 * wltest: libwayland server + client in one program. The server (parent) exposes wl_shm and a
 * minimal wl_compositor; the client (child) shares a memfd pool through SCM_RIGHTS, attaches a
 * buffer and commits; the server reads the pixels through wl_shm_buffer and releases it.
 * Exercises AF_UNIX + SCM_RIGHTS, epoll, timerfd, signalfd, memfd + MAP_SHARED across processes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <wayland-server.h>
#include <wayland-client.h>

#define W 64
#define H 32
#define PIXEL 0xff336699u

/* ------------------------------------------------------------ server */
static int committed, released_ok, timer_ticks, clients_gone;
static uint32_t seen_pixel;
static struct wl_display *sdpy;

static void res_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static struct wl_resource *pending;
static void surf_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y) { pending = buf; }
static void surf_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    wl_callback_send_done(cb, 1234);
    wl_resource_destroy(cb);
}
static void surf_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) {}
static void surf_commit(struct wl_client *c, struct wl_resource *r) {
    if (!pending) return;
    struct wl_shm_buffer *b = wl_shm_buffer_get(pending);
    if (b && wl_shm_buffer_get_width(b) == W && wl_shm_buffer_get_height(b) == H) {
        wl_shm_buffer_begin_access(b);
        uint32_t *px = wl_shm_buffer_get_data(b);
        seen_pixel = px[W * H - 1];
        wl_shm_buffer_end_access(b);
    }
    wl_buffer_send_release(pending);
    pending = NULL;
    committed++;
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
    struct wl_resource *s = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s, &surf_impl, NULL, NULL);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *s = wl_resource_create(c, &wl_region_interface, 1, id);
    wl_resource_set_implementation(s, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface comp_impl = { comp_create_surface, comp_create_region };
static void bind_comp(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, ver, id);
    wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}
static int on_timer(void *data) { timer_ticks++; wl_event_source_timer_update(*(struct wl_event_source **)data, 10); return 0; }
static int on_sigchld(int sig, void *data) { clients_gone++; return 0; }
static void client_destroyed(struct wl_listener *l, void *data) { clients_gone++; }
static struct wl_listener destroy_listener = { .notify = client_destroyed };
static void client_created(struct wl_listener *l, void *data) { wl_client_add_destroy_listener(data, &destroy_listener); }
static struct wl_listener create_listener = { .notify = client_created };

/* ------------------------------------------------------------ client */
static struct wl_compositor *comp;
static struct wl_shm *shm;
static int formats, release_seen;
static void shm_format(void *d, struct wl_shm *s, uint32_t f) { formats++; }
static const struct wl_shm_listener shm_listener = { shm_format };
static void global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
    if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) { shm = wl_registry_bind(reg, name, &wl_shm_interface, 1); wl_shm_add_listener(shm, &shm_listener, NULL); }
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_listener = { global, global_remove };
static void buf_release(void *d, struct wl_buffer *b) { release_seen++; }
static const struct wl_buffer_listener buf_listener = { buf_release };
static int frame_done;
static void frame_cb(void *d, struct wl_callback *cb, uint32_t t) { frame_done = t; wl_callback_destroy(cb); }
static const struct wl_callback_listener frame_listener = { frame_cb };

static int run_client(void) {
    struct wl_display *d = NULL;
    for (int i = 0; i < 50 && !(d = wl_display_connect("wayland-9")); i++) usleep(20000);
    if (!d) { printf("client: connect failed\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    if (!comp || !shm) { printf("client: missing globals\n"); return 2; }
    int fd = memfd_create("wl-buffer", MFD_CLOEXEC);
    ftruncate(fd, W * H * 4);
    uint32_t *px = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < W * H; i++) px[i] = PIXEL;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, W * H * 4);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(buf, &buf_listener, NULL);
    struct wl_surface *s = wl_compositor_create_surface(comp);
    wl_surface_attach(s, buf, 0, 0);
    wl_surface_damage(s, 0, 0, W, H);
    wl_callback_add_listener(wl_surface_frame(s), &frame_listener, NULL);
    wl_surface_commit(s);
    wl_display_roundtrip(d);
    int ok = formats >= 2 && release_seen == 1 && frame_done == 1234;
    printf("client: %d shm formats, buffer released %d, frame done %d\n", formats, release_seen, frame_done);
    wl_buffer_destroy(buf); wl_shm_pool_destroy(pool); wl_surface_destroy(s);
    wl_display_disconnect(d);
    return ok ? 0 : 3;
}

int main(void) {
    setenv("XDG_RUNTIME_DIR", "/tmp", 0);
    sdpy = wl_display_create();
    if (wl_display_add_socket(sdpy, "wayland-9")) { perror("add_socket"); return 1; }
    wl_display_init_shm(sdpy);
    wl_global_create(sdpy, &wl_compositor_interface, 4, NULL, bind_comp);
    wl_display_add_client_created_listener(sdpy, &create_listener);
    struct wl_event_loop *loop = wl_display_get_event_loop(sdpy);
    static struct wl_event_source *timer;
    timer = wl_event_loop_add_timer(loop, on_timer, &timer);
    wl_event_source_timer_update(timer, 10);
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGCHLD); sigprocmask(SIG_BLOCK, &m, NULL);
    wl_event_loop_add_signal(loop, SIGCHLD, on_sigchld, NULL);
    pid_t c = fork();
    if (c == 0) { sigprocmask(SIG_UNBLOCK, &m, NULL); _exit(run_client()); }
    for (int i = 0; i < 400 && clients_gone < 2; i++) {     /* client destroyed + SIGCHLD */
        wl_display_flush_clients(sdpy);
        wl_event_loop_dispatch(loop, 20);
    }
    int st = 0; waitpid(c, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && committed == 1 && seen_pixel == PIXEL && clients_gone >= 2;
    printf("server: commits %d, pixel %#x, timer ticks %s, client exit %d\n", committed, seen_pixel,
           timer_ticks > 0 ? "ok" : "none", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    wl_display_destroy(sdpy);
    printf("wltest: %s\n", ok ? "PASSED" : "FAILED");
    return !ok;
}
