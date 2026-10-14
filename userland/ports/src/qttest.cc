// M40: real Qt Core event/process/thread/file APIs and Qt DBus service/FD transport.
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSocketNotifier>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>
#include <QtDBus/QDBusUnixFileDescriptor>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
static int checks, fails;
#define CHECK(x) do { ++checks; if (!(x)) { ++fails; std::fprintf(stderr,"qttest: FAIL %d: %s\n",__LINE__,#x); } } while (0)
class Service : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.nineos.QtTest")
public slots:
    QString Echo(const QString &s) { return s; }
    QString ReadFD(const QDBusUnixFileDescriptor &fd) {
        char buf[32]={}; ssize_t n=pread(fd.fileDescriptor(),buf,sizeof(buf)-1,0);
        return n<0 ? QStringLiteral("pread failed") : QString::fromUtf8(buf,n);
    }
    void Quit() { QTimer::singleShot(0,QCoreApplication::instance(),&QCoreApplication::quit); }
};
class Receiver : public QObject {
    Q_OBJECT
public:
    QString received;
signals:
    void done();
public slots:
    void notice(const QString &s) { received=s; emit done(); }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc,argv);
    if (app.arguments().contains("--server")) {
        auto bus=QDBusConnection::sessionBus(); Service svc;
        if (!bus.isConnected() || !bus.registerService("org.nineos.QtTest") ||
            !bus.registerObject("/org/nineos/QtTest",&svc,QDBusConnection::ExportAllSlots)) return 10;
        std::puts("READY"); std::fflush(stdout);
        QTimer::singleShot(50000,&app,&QCoreApplication::quit);
        return app.exec();
    }
    const QString text=QString::fromUtf8("9os \xce\xbb \xe4\xb8\x96\xe7\x95\x8c");
    CHECK(QString::fromUtf8(text.toUtf8())==text && text.size()==8);
    QByteArray bytes(4096,'q'); bytes.append("payload");
    CHECK(QByteArray::fromBase64(bytes.toBase64())==bytes);
    CHECK(qUncompress(qCompress(bytes))==bytes);
    QJsonObject obj{{"os","9os"},{"milestone",40}};
    CHECK(QJsonDocument::fromJson(QJsonDocument(obj).toJson()).object()==obj);
    QRegularExpression re("^9os ([0-9]+)$");
    CHECK(re.match("9os 40").captured(1)=="40");
    QTemporaryDir dir; CHECK(dir.isValid());
    if (dir.isValid()) {
        QSaveFile f(dir.filePath("atomic"));
        CHECK(f.open(QIODevice::WriteOnly) && f.write(bytes)==bytes.size() && f.commit());
        QFile r(dir.filePath("atomic")); CHECK(r.open(QIODevice::ReadOnly) && r.readAll()==bytes);
    }
    bool threaded=false;
    QThread *worker=QThread::create([&] { threaded=true; }); worker->start();
    CHECK(worker->wait(5000) && threaded); delete worker;
    QEventLoop loop; bool timer=false;
    QTimer::singleShot(20,&loop,[&] { timer=true; loop.quit(); }); loop.exec(); CHECK(timer);
    int fds[2]; int rc=pipe2(fds,O_CLOEXEC); CHECK(rc==0);
    if (!rc) {
        bool notified=false; QSocketNotifier notify(fds[0],QSocketNotifier::Read);
        QObject::connect(&notify,&QSocketNotifier::activated,&loop,[&] {
            char c; notified=read(fds[0],&c,1)==1 && c=='Q'; notify.setEnabled(false); loop.quit();
        });
        QTimer deadline; deadline.setSingleShot(true);
        QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
        CHECK(write(fds[1],"Q",1)==1); deadline.start(5000); loop.exec();
        CHECK(notified); close(fds[0]); close(fds[1]);
    }
    QProcess proc; proc.start("/bin/sh", {"-c","printf qt-child; exit 7"});
    CHECK(proc.waitForFinished(5000) && proc.exitStatus()==QProcess::NormalExit &&
          proc.exitCode()==7 && proc.readAllStandardOutput()=="qt-child");
    auto bus=QDBusConnection::sessionBus(); CHECK(bus.isConnected());
    if (!bus.isConnected()) return 1;
    CHECK(bus.connectionCapabilities() & QDBusConnection::UnixFileDescriptorPassing);
    QDBusInterface daemon("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus",bus);
    QDBusReply<QStringList> names=daemon.call("ListNames");
    CHECK(names.isValid() && names.value().contains("org.freedesktop.DBus"));
    QDBusReply<uint> uid=daemon.call("GetConnectionUnixUser",bus.baseService());
    CHECK(uid.isValid() && uid.value()==getuid());
    Receiver recv;
    CHECK(bus.connect(QString(),"/org/nineos/Signal","org.nineos.QtTest","Notice",&recv,SLOT(notice(QString))));
    auto signal=QDBusMessage::createSignal("/org/nineos/Signal","org.nineos.QtTest","Notice"); signal << QStringLiteral("qt-signal");
    CHECK(bus.send(signal));
    QObject::connect(&recv,&Receiver::done,&loop,&QEventLoop::quit);
    QTimer deadline; deadline.setSingleShot(true); QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    deadline.start(5000); loop.exec(); CHECK(recv.received=="qt-signal");
    QProcess server; server.start(QCoreApplication::applicationFilePath(), {"--server"});
    bool ready=server.waitForStarted(5000) && server.waitForReadyRead(5000) && server.readAllStandardOutput().contains("READY");
    CHECK(ready);
    if (ready) {
        QDBusInterface service("org.nineos.QtTest","/org/nineos/QtTest","org.nineos.QtTest",bus);
        QDBusReply<QString> echo=service.call("Echo",text); CHECK(echo.isValid() && echo.value()==text);
        QDBusReply<uint> pid=daemon.call("GetConnectionUnixProcessID",QStringLiteral("org.nineos.QtTest"));
        CHECK(pid.isValid() && pid.value()==uint(server.processId()));
        int fd=memfd_create("qt-fd",MFD_CLOEXEC); CHECK(fd>=0);
        if (fd>=0) {
            CHECK(write(fd,"qt-passed-fd",12)==12);
            QDBusUnixFileDescriptor passed(fd); CHECK(passed.isValid()); close(fd);
            QDBusReply<QString> reply=service.call("ReadFD",QVariant::fromValue(passed));
            CHECK(reply.isValid() && reply.value()=="qt-passed-fd");
        }
        auto q=QDBusMessage::createMethodCall("org.nineos.Missing","/missing","org.nineos.QtTest","Echo");
        auto error=bus.call(q,QDBus::Block,5000);
        CHECK(error.type()==QDBusMessage::ErrorMessage && error.errorName()=="org.freedesktop.DBus.Error.ServiceUnknown");
        QDBusReply<void> quit=service.call("Quit"); CHECK(quit.isValid());
        CHECK(server.waitForFinished(5000) && server.exitStatus()==QProcess::NormalExit && server.exitCode()==0);
    }
    if (server.state()!=QProcess::NotRunning) { server.kill(); server.waitForFinished(5000); }
    std::printf("qttest: %s (%d checks, %d failures), Qt %s\n",fails ? "FAILED" : "PASSED",checks,fails,qVersion());
    return fails!=0;
}
#include "qttest.moc"
