// QtWayland raster/shm client: real compositor callbacks, not an offscreen test.
#include <QBackingStore>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#include <cstdio>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("qtwaylandtest: FAIL %d: %s\n", __LINE__, #c); } else std::printf("qtwaylandtest: ok %d: %s\n", __LINE__, #c); } while (0)

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
    static_cast<Frame *>(data)->done = true;
    wl_callback_destroy(callback);
}
static const wl_callback_listener listener = { done };

int main(int argc, char **argv) {
    alarm(60); setvbuf(stdout, nullptr, _IONBF, 0);
    QGuiApplication app(argc, argv);
    app.setDesktopFileName("qtwaylandsmoke");
    CHECK(QGuiApplication::platformName() == "wayland");
    CHECK(app.primaryScreen() && app.primaryScreen()->geometry().width() > 0);
    auto *native = app.nativeInterface<QNativeInterface::QWaylandApplication>();
    CHECK(native && native->display());
    if (!native || !native->display()) return 1;
    wl_display *display = native->display();
    wl_event_queue *queue = wl_display_create_queue(display);
    CHECK(queue);
    if (!queue) return 1;
    {
        QWindow window;
        window.setSurfaceType(QSurface::RasterSurface);
        window.setTitle("9os QtWayland raster");
        window.resize(320, 200);
        QBackingStore backing(&window);
        window.show();
        CHECK(until([&] { return window.isExposed(); }));
        auto *interface = QGuiApplication::platformNativeInterface();
        auto *surface = interface ? static_cast<wl_surface *>(interface->nativeResourceForWindow("surface", &window)) : nullptr;
        CHECK(surface && window.surfaceType() == QSurface::RasterSurface);
        if (!surface || !window.isExposed()) { wl_event_queue_destroy(queue); return 1; }
        const QColor colors[] = { Qt::red, Qt::green, Qt::blue };
        for (int i = 0; i < 3; ++i) {
            if (i == 1) {
                window.resize(360, 240);
                CHECK(until([&] { return window.size() == QSize(360, 240) && window.isExposed(); }));
                window.setTitle("9os QtWayland resized");
            }
            // A private callback queue avoids stealing events from Qt's reader
            // threads. The callback is fulfilled by the actual Qt buffer commit.
            auto *wrapper = reinterpret_cast<wl_surface *>(wl_proxy_create_wrapper(surface));
            CHECK(wrapper);
            if (!wrapper) break;
            wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
            Frame frame;
            wl_callback *callback = wl_surface_frame(wrapper);
            wl_proxy_wrapper_destroy(wrapper);
            CHECK(callback && wl_callback_add_listener(callback, &listener, &frame) == 0);
            if (!callback) break;
            backing.resize(window.size());
            QRegion region(QRect(QPoint(), window.size()));
            backing.beginPaint(region);
            QPaintDevice *device = backing.paintDevice();
            CHECK(device && device->devType() == QInternal::Image);
            {
                QPainter painter(device);
                CHECK(painter.isActive());
                painter.fillRect(QRect(QPoint(), window.size()), colors[i]);
                painter.setPen(Qt::white);
                painter.drawText(40, 70, "9os Qt Wayland / shared memory");
            }
            if (device && device->devType() == QInternal::Image)
                CHECK(static_cast<QImage *>(device)->pixelColor(4, 4) == colors[i]);
            backing.endPaint();
            backing.flush(region);
            CHECK(until([&] { return frame.done; }, display, queue));
            if (!frame.done) { wl_callback_destroy(callback); break; }
            CHECK(wl_display_get_error(display) == 0);
            std::printf("qtwaylandtest: compositor frame %d\n", i + 1);
        }
        // Keep pumping events while the smoke harness verifies app_id via IPC.
        QElapsedTimer hold; hold.start();
        while (hold.elapsed() < 3000) { QGuiApplication::processEvents(); QThread::msleep(10); }
        window.hide();
        CHECK(!window.isVisible());
    }
    wl_event_queue_destroy(queue);
    CHECK(wl_display_get_error(display) == 0);
    alarm(0);
    std::printf("qtwaylandtest: %s (%d checks, %d failures), Qt %s\n", fails ? "FAILED" : "PASSED", checks, fails, qVersion());
    return fails != 0;
}
