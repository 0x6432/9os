/* M38: software EGL/GLES2 rendering, GBM and PRIME/EGL-image sharing. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("egltest: FAIL %s (EGL %#x GL %#x)\n", msg, eglGetError(), glGetError()); fails++; } } while (0)
static GLuint shader(GLenum type, const char *s) {
    GLuint sh = glCreateShader(type); glShaderSource(sh, 1, &s, NULL); glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    CHECK(ok, "shader compilation");
    if (!ok) { char b[1024]; glGetShaderInfoLog(sh, sizeof b, NULL, b); puts(b); }
    return sh;
}
static struct gbm_device *gd;
static struct gbm_surface *gs;
static int drmfd = -1;
static void gbm_cleanup(void) {
    if (gs) { gbm_surface_destroy(gs); gs = NULL; }
    if (gd) { gbm_device_destroy(gd); gd = NULL; }
    if (drmfd >= 0) { close(drmfd); drmfd = -1; }
}
static void dmabuf_test(EGLDisplay d, struct gbm_bo *bo) {
    const char *ext = eglQueryString(d, EGL_EXTENSIONS);
    CHECK(ext && strstr(ext, "EGL_EXT_image_dma_buf_import"), "EGL dma-buf import extension");
    if (!ext || !strstr(ext, "EGL_EXT_image_dma_buf_import")) return;
    PFNEGLCREATEIMAGEKHRPROC create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    CHECK(create && destroy && target, "EGL image entry points");
    if (!create || !destroy || !target) return;
    int fd = gbm_bo_get_fd(bo);
    CHECK(fd >= 0, "PRIME export fd");
    if (fd < 0) return;
    const EGLint attrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo), EGL_NONE};
    EGLImageKHR image = create(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    /* The imported image must hold its own reference after the export fd closes. */
    close(fd);
    CHECK(image != EGL_NO_IMAGE_KHR, "import PRIME fd as EGL image");
    if (image == EGL_NO_IMAGE_KHR) return;
    GLuint texture, fb;
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    target(GL_TEXTURE_2D, image);
    CHECK(glGetError() == GL_NO_ERROR, "bind imported image as texture");
    glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    int complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    CHECK(complete, "imported image framebuffer");
    if (complete) {
        unsigned char px[4] = {0};
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        CHECK(px[0] == 0 && px[1] == 255 && px[2] == 0 && px[3] == 255, "imported green pixel after fd close");
        glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
        uint32_t stride = 0; void *map_data = NULL;
        unsigned char *map = gbm_bo_map(bo, 0, 0, 64, 64, GBM_BO_TRANSFER_READ, &stride, &map_data);
        CHECK(map != NULL, "map original after imported render");
        if (map) {
            unsigned char *blue = map + 32 * stride + 32 * 4;
            CHECK(blue[0] == 255 && blue[1] == 0 && blue[2] == 0, "imported render aliases original GBM buffer");
            gbm_bo_unmap(bo, map_data);
        }
        CHECK(glGetError() == GL_NO_ERROR, "imported render error state");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fb); glDeleteTextures(1, &texture);
    CHECK(destroy(d, image), "destroy imported EGL image");
}
int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "gbm") && strcmp(argv[1], "dmabuf"))) {
        fprintf(stderr, "usage: egltest [gbm|dmabuf] (GALLIUM_DRIVER=softpipe|llvmpipe)\n"); return 2;
    }
    int use_dmabuf = argc == 2 && !strcmp(argv[1], "dmabuf");
    int use_gbm = argc == 2;
    const char *driver = getenv("GALLIUM_DRIVER");
    if (!driver || !*driver) driver = "softpipe";
    atexit(gbm_cleanup);
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1); setenv("GALLIUM_DRIVER", driver, 1);
    driver = getenv("GALLIUM_DRIVER");
    if (use_gbm) {
        drmfd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        CHECK(drmfd >= 0, "open DRM card");
        if (fails) return 1;
        gd = gbm_create_device(drmfd);
        CHECK(gd != NULL, "GBM device");
        if (fails) return 1;
    }
    EGLDisplay d = eglGetPlatformDisplay(use_gbm ? EGL_PLATFORM_GBM_KHR : EGL_PLATFORM_SURFACELESS_MESA, use_gbm ? (void *)gd : EGL_DEFAULT_DISPLAY, NULL);
    CHECK(d != EGL_NO_DISPLAY, "platform display");
    EGLint major = 0, minor = 0;
    CHECK(eglInitialize(d, &major, &minor), "eglInitialize");
    if (fails) return 1;
    printf("egltest: EGL %d.%d %s\n", major, minor, eglQueryString(d, EGL_VENDOR));
    CHECK(eglBindAPI(EGL_OPENGL_ES_API), "bind GLES API");
    const EGLint attrs[] = {EGL_SURFACE_TYPE, use_gbm ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, use_gbm ? 0 : 8, EGL_NONE};
    EGLConfig config = NULL, configs[64]; EGLint n = 0;
    CHECK(eglChooseConfig(d, attrs, configs, 64, &n) && n > 0, "renderable config");
    for (int i = 0; i < n; i++) {
        EGLint visual = 0; eglGetConfigAttrib(d, configs[i], EGL_NATIVE_VISUAL_ID, &visual);
        if (!use_gbm || visual == GBM_FORMAT_XRGB8888) { config = configs[i]; break; }
    }
    CHECK(config != NULL, "compatible native visual");
    if (fails) { eglTerminate(d); return 1; }
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLint pa[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLContext c = eglCreateContext(d, config, EGL_NO_CONTEXT, ca);
    if (use_gbm) {
        gs = gbm_surface_create(gd, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        CHECK(gs != NULL, "GBM surface");
        if (fails) { eglTerminate(d); return 1; }
    }
    EGLSurface p = use_gbm ? eglCreatePlatformWindowSurface(d, config, gs, NULL) : eglCreatePbufferSurface(d, config, pa);
    CHECK(c != EGL_NO_CONTEXT && p != EGL_NO_SURFACE, "context and pbuffer");
    CHECK(eglMakeCurrent(d, p, p, c), "make current");
    if (fails) { eglTerminate(d); return 1; }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("egltest: GL %s, renderer %s\n", glGetString(GL_VERSION), renderer ? renderer : "NULL");
    CHECK(renderer && strstr(renderer, driver), "requested software renderer");
    glViewport(0, 0, 64, 64); glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    GLuint vs = shader(GL_VERTEX_SHADER, "attribute vec2 pos; void main(){gl_Position=vec4(pos,0.0,1.0);}");
    GLuint fs = shader(GL_FRAGMENT_SHADER, "precision mediump float; void main(){gl_FragColor=vec4(0.0,1.0,0.0,1.0);}");
    GLuint prog = glCreateProgram(); glAttachShader(prog, vs); glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos"); glLinkProgram(prog);
    GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked); CHECK(linked, "program link");
    glUseProgram(prog);
    const GLfloat v[] = {-0.8f,-0.8f, 0.8f,-0.8f, 0.0f,0.8f};
    GLuint b; glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL); glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
    unsigned char center[4] = {0}, corner[4] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    CHECK(center[0] == 0 && center[1] == 255 && center[2] == 0 && center[3] == 255, "green triangle pixel");
    CHECK(corner[0] == 255 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255, "red clear pixel");
    CHECK(glGetError() == GL_NO_ERROR, "GLES error state");
    CHECK(eglSwapBuffers(d, p), "surface swap");
    if (use_gbm) {
        struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
        CHECK(bo != NULL, "GBM front buffer");
        if (bo) {
            CHECK(gbm_bo_get_width(bo) == 64 && gbm_bo_get_height(bo) == 64 && gbm_bo_get_stride(bo) >= 256, "GBM buffer geometry");
            CHECK(gbm_bo_get_handle(bo).u32 != 0, "DRM GEM handle");
            uint32_t stride = 0; void *map_data = NULL;
            unsigned char *pixels = gbm_bo_map(bo, 0, 0, 64, 64, GBM_BO_TRANSFER_READ, &stride, &map_data);
            CHECK(pixels != NULL, "GBM front buffer map");
            if (pixels) {
                unsigned char *px = pixels + 32 * stride + 32 * 4;
                CHECK(px[0] == 0 && px[1] == 255 && px[2] == 0, "GBM rendered front buffer pixel");
                gbm_bo_unmap(bo, map_data);
            }
            if (use_dmabuf) dmabuf_test(d, bo);
            gbm_surface_release_buffer(gs, bo);
        }
    }
    glDeleteBuffers(1, &b); glDeleteProgram(prog); glDeleteShader(vs); glDeleteShader(fs);
    CHECK(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "unbind context");
    CHECK(eglDestroySurface(d, p) && eglDestroyContext(d, c), "destroy surface/context");
    CHECK(eglTerminate(d), "terminate display"); eglReleaseThread();
    /* GBM still owns a Gallium screen. Destroy it before Mesa's later-registered
     * ORC singleton atexit handler; otherwise GBM cleanup uses a dead JIT.
     * The early-error atexit fallback remains safe and idempotent. */
    gbm_cleanup();
    printf("egltest: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
