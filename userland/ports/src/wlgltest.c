/* M38: xdg-shell Wayland EGL client; present frames and await compositor callbacks. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static struct wl_compositor *comp;
static struct xdg_wm_base *shell;
static int configured, closed, frames, checks, fails;
#define CHECK(c, s) do { checks++; if (!(c)) { printf("wlgltest: FAIL %s (EGL %#x GL %#x)\n", s, eglGetError(), glGetError()); fails++; } } while (0)
static void ping(void *p, struct xdg_wm_base *s, uint32_t serial) { xdg_wm_base_pong(s, serial); }
static const struct xdg_wm_base_listener shell_listener = {.ping = ping};
static void global(void *p, struct wl_registry *r, uint32_t n, const char *s, uint32_t v) {
    if (!strcmp(s, "wl_compositor")) comp = wl_registry_bind(r, n, &wl_compositor_interface, v < 4 ? v : 4);
    if (!strcmp(s, "xdg_wm_base")) {
        shell = wl_registry_bind(r, n, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(shell, &shell_listener, NULL);
    }
}
static void removed(void *p, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_listener = {global, removed};
static void configure(void *p, struct xdg_surface *s, uint32_t serial) { xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener surface_listener = {configure};
static void top_configure(void *p, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *a) {}
static void top_close(void *p, struct xdg_toplevel *t) { closed = 1; }
static const struct xdg_toplevel_listener top_listener = {.configure = top_configure, .close = top_close};
static void done(void *p, struct wl_callback *cb, uint32_t t) { frames++; wl_callback_destroy(cb); }
static const struct wl_callback_listener frame_listener = {done};
int main(void) {
    /* Blocking Wayland dispatch must never stall the regression suite forever. */
    alarm(60); setvbuf(stdout, NULL, _IONBF, 0);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1); setenv("GALLIUM_DRIVER", "softpipe", 0);
    struct wl_display *wd = wl_display_connect(NULL); CHECK(wd != NULL, "connect compositor");
    if (!wd) return 1;
    struct wl_registry *reg = wl_display_get_registry(wd); wl_registry_add_listener(reg, &reg_listener, NULL);
    CHECK(wl_display_roundtrip(wd) >= 0 && comp && shell, "compositor and xdg-shell globals");
    if (fails) return 1;
    struct wl_surface *ws = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(shell, ws);
    xdg_surface_add_listener(xs, &surface_listener, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_app_id(top, "eglesmoke"); xdg_toplevel_set_title(top, "9os Wayland EGL regression");
    wl_surface_commit(ws);
    while (!configured && !closed && wl_display_dispatch(wd) >= 0) {}
    CHECK(configured && !closed, "initial xdg configure");
    if (fails) return 1;
    struct wl_egl_window *win = wl_egl_window_create(ws, 64, 64);
    CHECK(win != NULL, "Wayland EGL native window");
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wd, NULL);
    EGLint major, minor; CHECK(d != EGL_NO_DISPLAY && eglInitialize(d, &major, &minor), "Wayland EGL display");
    if (fails) return 1;
    CHECK(eglBindAPI(EGL_OPENGL_ES_API), "GLES API");
    EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint n = 0; CHECK(eglChooseConfig(d, attrs, &config, 1, &n) && n, "window config");
    if (fails) return 1;
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, ca);
    EGLSurface surface = eglCreatePlatformWindowSurface(d, config, win, NULL);
    CHECK(ctx != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE && eglMakeCurrent(d, surface, surface, ctx), "window context");
    if (fails) return 1;
    printf("wlgltest: EGL %d.%d, GL renderer %s\n", major, minor, glGetString(GL_RENDERER));
    glViewport(0, 0, 64, 64);
    for (int i = 0; i < 3 && !fails && !closed; i++) {
        unsigned char px[4] = {0};
        glClearColor(i == 0, i == 1, i == 2, 1); glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        CHECK(px[0] == (i == 0 ? 255 : 0) && px[1] == (i == 1 ? 255 : 0) && px[2] == (i == 2 ? 255 : 0), "rendered RGB frame");
        wl_callback_add_listener(wl_surface_frame(ws), &frame_listener, NULL);
        CHECK(eglSwapBuffers(d, surface), "present Wayland frame");
        while (frames <= i && !closed && wl_display_dispatch(wd) >= 0) {}
        CHECK(frames > i && !closed, "compositor frame callback");
    }
    CHECK(glGetError() == GL_NO_ERROR, "GLES error state");
    const char *hold = getenv("WLGLTEST_HOLD_SECONDS");
    if (!fails && hold) { int s = atoi(hold); if (s > 0 && s <= 30) sleep(s); }
    CHECK(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "unbind");
    CHECK(eglDestroySurface(d, surface) && eglDestroyContext(d, ctx) && eglTerminate(d), "EGL teardown");
    eglReleaseThread(); wl_egl_window_destroy(win); xdg_toplevel_destroy(top); xdg_surface_destroy(xs);
    wl_surface_destroy(ws); xdg_wm_base_destroy(shell); wl_compositor_destroy(comp); wl_registry_destroy(reg);
    wl_display_flush(wd); wl_display_disconnect(wd); alarm(0);
    printf("wlgltest: %s (%d frames, %d checks, %d failures)\n", fails ? "FAILED" : "PASSED", frames, checks, fails);
    return fails != 0;
}
