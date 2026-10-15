// M40 Qt EGL/GLES client: actual shader rendering, swaps and compositor callbacks.
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QScreen>
#include <QSurfaceFormat>
#include <QThread>
#include <QVector4D>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("qtgltest: FAIL %d: %s\n", __LINE__, #c); } else std::printf("qtgltest: ok %d: %s\n", __LINE__, #c); } while (0)
template<class F> static bool until(F ready, wl_display *display = nullptr, wl_event_queue *queue = nullptr) {
    QElapsedTimer t; t.start();
    while (!ready() && t.elapsed() < 10000) {
        QGuiApplication::processEvents();
        if (display && queue && wl_display_dispatch_queue_pending(display, queue) < 0) return false;
        QThread::msleep(5);
    }
    return ready();
}
struct Frame { bool done = false; };
static void done(void *data, wl_callback *callback, uint32_t) {
    static_cast<Frame *>(data)->done = true; wl_callback_destroy(callback);
}
static const wl_callback_listener listener = { done };

int main(int argc, char **argv) {
    alarm(60); setvbuf(stdout, nullptr, _IONBF, 0);
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES); format.setVersion(2, 0);
    format.setDepthBufferSize(0); format.setStencilBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    app.setDesktopFileName("qtwaylandsmoke");
    CHECK(QGuiApplication::platformName() == "wayland-egl");
    auto *native = app.nativeInterface<QNativeInterface::QWaylandApplication>();
    CHECK(native && native->display());
    if (!native || !native->display()) return 1;
    wl_display *display = native->display();
    wl_event_queue *queue = wl_display_create_queue(display);
    CHECK(queue);
    if (!queue) return 1;
    {
        QWindow window;
        window.setSurfaceType(QSurface::OpenGLSurface); window.setFormat(format);
        window.setTitle("9os Qt EGL/GLES shader"); window.resize(320, 200); window.show();
        CHECK(until([&] { return window.isExposed(); }));
        QOpenGLContext context; context.setFormat(format);
        CHECK(context.create() && context.isValid());
        bool current = context.makeCurrent(&window);
        CHECK(current && context.isOpenGLES());
        if (!current) { wl_event_queue_destroy(queue); return 1; }
        QOpenGLFunctions *gl = context.functions(); gl->initializeOpenGLFunctions();
        const char *renderer = reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER));
        const char *version = reinterpret_cast<const char *>(gl->glGetString(GL_VERSION));
        std::printf("qtgltest: renderer %s; GL %s\n", renderer ? renderer : "(null)", version ? version : "(null)");
        CHECK(renderer && version && std::strstr(version, "OpenGL ES"));
        const char *driver = std::getenv("GALLIUM_DRIVER");
        CHECK(driver && renderer && std::strstr(renderer, driver));
        auto *interface = QGuiApplication::platformNativeInterface();
        auto *surface = interface ? static_cast<wl_surface *>(interface->nativeResourceForWindow("surface", &window)) : nullptr;
        CHECK(surface && window.surfaceType() == QSurface::OpenGLSurface);
        if (!surface) { context.doneCurrent(); wl_event_queue_destroy(queue); return 1; }
        {
            QOpenGLShaderProgram program;
            CHECK(program.addShaderFromSourceCode(QOpenGLShader::Vertex,
                "attribute vec2 position; void main() { gl_Position=vec4(position,0.0,1.0); }"));
            CHECK(program.addShaderFromSourceCode(QOpenGLShader::Fragment,
                "precision mediump float; uniform vec4 color; void main() { gl_FragColor=color; }"));
            CHECK(program.link() && program.bind());
            int position = program.attributeLocation("position"); CHECK(position >= 0);
            GLuint buffer = 0; gl->glGenBuffers(1, &buffer); CHECK(buffer != 0);
            gl->glBindBuffer(GL_ARRAY_BUFFER, buffer);
            const GLfloat vertices[] = { -1.f,-1.f, 3.f,-1.f, -1.f,3.f };
            gl->glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
            if (position >= 0) {
                gl->glEnableVertexAttribArray(GLuint(position));
                gl->glVertexAttribPointer(GLuint(position), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            }
            for (int i = 0; i < 3; ++i) {
                if (i == 1) {
                    window.resize(360, 240);
                    CHECK(until([&] { return window.size() == QSize(360, 240) && window.isExposed(); }));
                    CHECK(context.makeCurrent(&window));
                }
                auto *wrapper = reinterpret_cast<wl_surface *>(wl_proxy_create_wrapper(surface));
                CHECK(wrapper); if (!wrapper) break;
                wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
                Frame frame;
                wl_callback *callback = wl_surface_frame(wrapper); wl_proxy_wrapper_destroy(wrapper);
                CHECK(callback && wl_callback_add_listener(callback, &listener, &frame) == 0);
                if (!callback) break;
                QSize pixels = window.size() * window.devicePixelRatio();
                gl->glViewport(0, 0, pixels.width(), pixels.height());
                gl->glClearColor(0, 0, 0, 1); gl->glClear(GL_COLOR_BUFFER_BIT);
                program.setUniformValue("color", QVector4D(i == 0, i == 1, i == 2, 1));
                gl->glDrawArrays(GL_TRIANGLES, 0, 3);
                unsigned char pixel[4] = {};
                gl->glReadPixels(pixels.width()/2, pixels.height()/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                CHECK(pixel[0] == (i == 0 ? 255 : 0) && pixel[1] == (i == 1 ? 255 : 0) && pixel[2] == (i == 2 ? 255 : 0));
                CHECK(gl->glGetError() == GL_NO_ERROR);
                context.swapBuffers(&window);
                CHECK(until([&] { return frame.done; }, display, queue));
                if (!frame.done) { wl_callback_destroy(callback); break; }
                CHECK(wl_display_get_error(display) == 0);
                std::printf("qtgltest: compositor frame %d\n", i + 1);
            }
            if (position >= 0) gl->glDisableVertexAttribArray(GLuint(position));
            gl->glBindBuffer(GL_ARRAY_BUFFER, 0); gl->glDeleteBuffers(1, &buffer); program.release();
            CHECK(gl->glGetError() == GL_NO_ERROR);
        }
        QElapsedTimer hold; hold.start();
        while (hold.elapsed() < 3000) { QGuiApplication::processEvents(); QThread::msleep(10); }
        context.doneCurrent(); window.hide(); CHECK(!window.isVisible());
    }
    wl_event_queue_destroy(queue); CHECK(wl_display_get_error(display) == 0);
    alarm(0);
    std::printf("qtgltest: %s (%d checks, %d failures), Qt %s\n", fails ? "FAILED" : "PASSED", checks, fails, qVersion());
    return fails != 0;
}
