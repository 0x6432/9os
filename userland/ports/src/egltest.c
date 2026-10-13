/* M38: softpipe EGL/GLES2 surfaceless rendering, including shader compilation. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
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
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1); setenv("GALLIUM_DRIVER", "softpipe", 1);
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    CHECK(d != EGL_NO_DISPLAY, "surfaceless display");
    EGLint major = 0, minor = 0;
    CHECK(eglInitialize(d, &major, &minor), "eglInitialize");
    if (fails) return 1;
    printf("egltest: EGL %d.%d %s\n", major, minor, eglQueryString(d, EGL_VENDOR));
    CHECK(eglBindAPI(EGL_OPENGL_ES_API), "bind GLES API");
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint n = 0;
    CHECK(eglChooseConfig(d, attrs, &config, 1, &n) && n == 1, "RGBA8 pbuffer config");
    if (fails) { eglTerminate(d); return 1; }
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLint pa[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLContext c = eglCreateContext(d, config, EGL_NO_CONTEXT, ca);
    EGLSurface p = eglCreatePbufferSurface(d, config, pa);
    CHECK(c != EGL_NO_CONTEXT && p != EGL_NO_SURFACE, "context and pbuffer");
    CHECK(eglMakeCurrent(d, p, p, c), "make current");
    if (fails) { eglTerminate(d); return 1; }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("egltest: GL %s, renderer %s\n", glGetString(GL_VERSION), renderer ? renderer : "NULL");
    CHECK(renderer && strstr(renderer, "softpipe"), "softpipe renderer");
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
    CHECK(eglSwapBuffers(d, p), "pbuffer swap");
    glDeleteBuffers(1, &b); glDeleteProgram(prog); glDeleteShader(vs); glDeleteShader(fs);
    CHECK(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "unbind context");
    CHECK(eglDestroySurface(d, p) && eglDestroyContext(d, c), "destroy surface/context");
    CHECK(eglTerminate(d), "terminate display"); eglReleaseThread();
    printf("egltest: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
