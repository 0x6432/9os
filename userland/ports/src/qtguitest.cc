// M40 raster/offscreen GUI prerequisite only, not a Wayland/Quick/KDE test.
#include <QGuiApplication>
#include <QBackingStore>
#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QRegion>
#include <QThread>
#include <QWindow>
#include <cstdio>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::fprintf(stderr, "qtguitest: FAIL %d: %s\n", __LINE__, #c); } else std::printf("qtguitest: ok %d: %s\n", __LINE__, #c); } while (0)

template<class F> static bool until(F ready) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < 5000) {
        QGuiApplication::processEvents(); QThread::msleep(5);
    }
    return ready();
}

int main(int argc, char **argv) {
    alarm(60); setvbuf(stdout, nullptr, _IONBF, 0);
    QGuiApplication app(argc, argv);
    CHECK(QGuiApplication::platformName() == "offscreen");
    QScreen *screen = app.primaryScreen();
    CHECK(screen && screen->geometry().width() > 0 && screen->depth() >= 24);

    QImage image(64, 64, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    {
        QPainter p(&image); CHECK(p.isActive());
        p.fillRect(0, 0, 32, 64, Qt::red);
        p.fillRect(32, 0, 32, 64, Qt::green);
    }
    CHECK(image.pixelColor(8, 8) == QColor(Qt::red));
    CHECK(image.pixelColor(48, 8) == QColor(Qt::green));
    QImage scaled = image.scaled(128, 128);
    CHECK(scaled.size() == QSize(128, 128) && scaled.pixelColor(96, 16) == QColor(Qt::green));
    QPixmap pixmap = QPixmap::fromImage(image);
    CHECK(!pixmap.isNull() && pixmap.toImage().pixelColor(8, 8) == QColor(Qt::red));

    QByteArray png; QBuffer buffer(&png);
    CHECK(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"));
    QImage decoded = QImage::fromData(png, "PNG");
    CHECK(!decoded.isNull() && decoded.size() == image.size() && decoded.pixelColor(48, 8) == QColor(Qt::green));

    CHECK(QFontDatabase::families().contains("DejaVu Sans"));
    int fontId = QFontDatabase::addApplicationFont("/usr/share/fonts/dejavu/DejaVuSans.ttf");
    CHECK(fontId >= 0);
    QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    CHECK(!families.isEmpty());
    if (!families.isEmpty()) {
        QFont font(families.front(), 18); QFontMetrics metrics(font);
        CHECK(metrics.horizontalAdvance("9os GUI") > 0 && metrics.height() > 0);
        QImage glyphs(200, 80, QImage::Format_ARGB32_Premultiplied); glyphs.fill(Qt::white);
        { QPainter p(&glyphs); p.setFont(font); p.setPen(Qt::black); p.drawText(5, 45, "9os GUI"); }
        int ink = 0;
        for (int y=0; y<glyphs.height(); ++y) for (int x=0; x<glyphs.width(); ++x)
            if (glyphs.pixelColor(x,y) != QColor(Qt::white)) ++ink;
        CHECK(ink > 0);
    }
    app.clipboard()->setText("9os offscreen clipboard");
    CHECK(app.clipboard()->text() == "9os offscreen clipboard");

    QWindow window; window.setSurfaceType(QSurface::RasterSurface);
    window.resize(64,64); window.setTitle("9os raster prerequisite");
    QBackingStore store(&window); window.show();
    CHECK(until([&] { return window.isExposed(); }));
    store.resize(window.size());
    QRegion region(QRect(QPoint(0,0),window.size()));
    store.beginPaint(region);
    { QPainter p(store.paintDevice()); CHECK(p.isActive()); p.fillRect(0,0,64,64,Qt::blue); }
    store.endPaint(); store.flush(region);
    QGuiApplication::processEvents();
    if (screen) {
        QImage captured = screen->grabWindow(window.winId()).toImage();
        CHECK(!captured.isNull() && captured.size() == window.size() && captured.pixelColor(8,8) == QColor(Qt::blue));
    }
    window.resize(96,48);
    CHECK(until([&] { return window.size() == QSize(96,48); }));
    window.hide(); CHECK(!window.isVisible());
    if (fontId >= 0) CHECK(QFontDatabase::removeApplicationFont(fontId));
    std::printf("qtguitest: %s (%d checks, %d failures), Qt %s (offscreen only)\n", fails ? "FAILED" : "PASSED", checks, fails, qVersion());
    return fails != 0;
}
