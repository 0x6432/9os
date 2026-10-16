// Real QML scene + Qt Quick software rendering, actual Wayland frame callbacks.
#include <QColor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QtGui/qguiapplication_platform.h>
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#include <cstdio>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("qtquickwaylandtest: FAIL %d: %s\n",__LINE__,#c); } } while(0)
template<class F> static bool until(F ready,wl_display *display=nullptr,wl_event_queue *queue=nullptr) {
    QElapsedTimer timer;timer.start();
    while(!ready() && timer.elapsed()<10000) {
        QGuiApplication::processEvents();
        if(display && queue && wl_display_dispatch_queue_pending(display,queue)<0)return false;
        QThread::msleep(5);
    }
    return ready();
}
struct Frame { bool done=false; };
static void done(void *data,wl_callback *callback,uint32_t) {
    static_cast<Frame *>(data)->done=true;wl_callback_destroy(callback);
}
static const wl_callback_listener listener={done};
int main(int argc,char **argv) {
    alarm(60);setvbuf(stdout,nullptr,_IONBF,0);
    QGuiApplication app(argc,argv);app.setDesktopFileName("qtwaylandsmoke");
    CHECK(QGuiApplication::platformName()=="wayland");
    auto *native=app.nativeInterface<QNativeInterface::QWaylandApplication>();
    CHECK(native && native->display());if(!native || !native->display())return 1;
    wl_display *display=native->display();wl_event_queue *queue=wl_display_create_queue(display);
    CHECK(queue);if(!queue)return 1;
    {
        QQuickView view;view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.setTitle("9os source Quick Wayland");view.resize(320,200);
        view.setSource(QUrl::fromLocalFile("/usr/qml/9os-tests/qtquick-wayland.qml"));
        for(const auto &error:view.errors())std::fprintf(stderr,"%s\n",qPrintable(error.toString()));
        CHECK(view.status()==QQuickView::Ready && view.rootObject());
        if(view.status()!=QQuickView::Ready || !view.rootObject()){wl_event_queue_destroy(queue);return 1;}
        auto *root=view.rootObject();
        CHECK(root->property("derived").toInt()==42);
        CHECK(root->setProperty("basis",8));
        CHECK(until([&]{return root->property("derived").toInt()==48;}));
        view.show();CHECK(until([&]{return view.isExposed();}));
        CHECK(view.rendererInterface()->graphicsApi()==QSGRendererInterface::Software);
        auto *platform=QGuiApplication::platformNativeInterface();
        auto *surface=platform ? static_cast<wl_surface *>(platform->nativeResourceForWindow("surface",&view)):nullptr;
        CHECK(surface);if(!surface || !view.isExposed()){wl_event_queue_destroy(queue);return 1;}
        const QColor colors[]={Qt::red,Qt::green,Qt::blue};
        for(int i=0;i<3;i++) {
            if(i==1){view.resize(360,240);CHECK(until([&]{return view.size()==QSize(360,240) && view.isExposed();}));}
            auto *wrapper=reinterpret_cast<wl_surface *>(wl_proxy_create_wrapper(surface));
            CHECK(wrapper);if(!wrapper)break;
            wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper),queue);
            Frame frame;wl_callback *callback=wl_surface_frame(wrapper);wl_proxy_wrapper_destroy(wrapper);
            CHECK(callback);if(!callback)break;
            int rc=wl_callback_add_listener(callback,&listener,&frame);CHECK(rc==0);
            if(rc){wl_callback_destroy(callback);break;}
            CHECK(root->setProperty("color",colors[i]));view.update();
            CHECK(until([&]{return frame.done;},display,queue));
            if(!frame.done){wl_callback_destroy(callback);break;}
            CHECK(wl_display_get_error(display)==0);
            QImage image=view.grabWindow();CHECK(!image.isNull() && image.size()==view.size());
            // Every captured pixel must match, not merely a corner sample.
            bool pixels=!image.isNull();
            for(int y=0;pixels && y<image.height();y++)for(int x=0;x<image.width();x++)
                if(image.pixelColor(x,y)!=colors[i]){pixels=false;break;}
            CHECK(pixels);
            std::printf("qtquickwaylandtest: compositor frame %d\n",i+1);
        }
        QElapsedTimer hold;hold.start();
        while(hold.elapsed()<3000){QGuiApplication::processEvents();QThread::msleep(10);}
        view.hide();CHECK(!view.isVisible());
    }
    wl_event_queue_destroy(queue);CHECK(wl_display_get_error(display)==0);alarm(0);
    std::printf("qtquickwaylandtest: %s (%d checks, %d failures), Qt %s\n",fails?"FAILED":"PASSED",checks,fails,qVersion());
    return fails!=0;
}
