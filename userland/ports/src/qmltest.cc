// Headless Qt QML/JavaScript interpreter, installed imports and a real worker.
#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <memory>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("qmltest: FAIL %d: %s\n", __LINE__, #c); } else std::printf("qmltest: ok %d: %s\n", __LINE__, #c); } while (0)
template<class F> static bool until(F ready) {
    QElapsedTimer t; t.start();
    while (!ready() && t.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(5); }
    return ready();
}
class Bridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(int value READ value WRITE setValue NOTIFY valueChanged)
public:
    int value() const { return v; }
    void setValue(int n) { if (v != n) { v = n; emit valueChanged(); } }
    Q_INVOKABLE int add(int a, int b) { return a + b; }
    QString received;
public slots:
    void notice(const QString &s) { received = s; }
signals:
    void valueChanged();
private:
    int v = 7;
};
static std::unique_ptr<QObject> create(QQmlComponent &c, const QByteArray &source, const QUrl &url) {
    c.setData(source, url);
    CHECK(until([&] { return !c.isLoading(); }) && c.isReady());
    if (!c.isReady()) {
        for (const auto &e : c.errors()) std::printf("qmltest: component error: %s\n", qPrintable(e.toString()));
        return {};
    }
    std::unique_ptr<QObject> obj(c.create()); CHECK(obj);
    return obj;
}
int main(int argc, char **argv) {
    alarm(60); setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    std::printf("qmltest: headless interpreter start, UID %u\n", unsigned(geteuid()));
    Bridge bridge;
    QQmlEngine::setObjectOwnership(&bridge, QQmlEngine::CppOwnership);
    {
        QJSEngine js;
        js.globalObject().setProperty("api", js.newQObject(&bridge));
        CHECK(js.evaluate("[1,2,3,4].map(x => x*2).reduce((a,b) => a+b,0)").toInt() == 20);
        CHECK(js.evaluate("(() => { const m=new Map([['a',3],['b',4]]); return m.get('a')+m.get('b'); })()").toInt() == 7);
        CHECK(js.evaluate("new Uint8Array([1,2,255])[2]").toInt() == 255);
        CHECK(js.evaluate("JSON.parse(JSON.stringify({word:'\u0936\u093e\u0928\u094d\u0924\u093f',n:9})).n").toInt() == 9);
        CHECK(js.evaluate("/([a-z]+)-(\\d+)/.exec('qml-40')[2]").toString() == "40");
        CHECK(js.evaluate("api.add(19,23)").toInt() == 42);
        CHECK(js.evaluate("api.value=9; api.value").toInt() == 9 && bridge.value() == 9);
        QJSValue error = js.evaluate("throw new Error('expected-qml-error')", "expected.js");
        CHECK(error.isError() && error.property("message").toString() == "expected-qml-error");
        CHECK(!error.property("stack").toString().isEmpty());
        CHECK(js.evaluate("function {").isError());
        // Qt 6.8 implements ES7; globalThis is not part of that contract.
        QJSValue promise = js.evaluate("var promiseResult=0; Promise.resolve(21).then(x=>{promiseResult=x*2;});");
        CHECK(!promise.isError());
        CHECK(until([&] { return js.globalObject().property("promiseResult").toInt() == 42; }));
        js.collectGarbage(); CHECK(bridge.value() == 9);
    }
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("api", &bridge);
    QTemporaryDir dir; CHECK(dir.isValid());
    {
        QQmlComponent c(&engine);
        auto obj = create(c, R"QML(import QtQml
QtObject {
    id: root
    property int base: 7
    property int derived: base * 3
    property int fromCpp: api.value * 2
    property bool fired: false
    property Timer timer: Timer { interval: 20; running: true; onTriggered: root.fired = true }
    signal notified(string message)
    function bump(n) { base += n; notified("qml-signal"); return derived }
})QML", QUrl::fromLocalFile(dir.filePath("bindings.qml")));
        if (!obj) return 1;
        CHECK(obj->property("derived").toInt() == 21 && obj->property("fromCpp").toInt() == 18);
        CHECK(QObject::connect(obj.get(), SIGNAL(notified(QString)), &bridge, SLOT(notice(QString))));
        QVariant result;
        CHECK(QMetaObject::invokeMethod(obj.get(), "bump", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, 5)));
        CHECK(result.toInt() == 36 && bridge.received == "qml-signal");
        bridge.setValue(11); CHECK(obj->property("fromCpp").toInt() == 22);
        QQmlExpression expression(engine.rootContext(), obj.get(), "derived + api.value");
        CHECK(expression.evaluate().toInt() == 47 && !expression.hasError());
        CHECK(until([&] { return obj->property("fired").toBool(); }));
    }
    {
        QQmlComponent c(&engine);
        auto obj = create(c, R"QML(import QtQml
import QtQml.Models
QtObject {
    property ListModel items: ListModel {
        ListElement { name: "alpha"; score: 3 }
        ListElement { name: "beta"; score: 4 }
    }
    function appendRow() { items.append({name:"gamma", score:5}); return items.count }
})QML", QUrl::fromLocalFile(dir.filePath("model.qml")));
        if (!obj) return 1;
        auto *model = qobject_cast<QAbstractItemModel *>(obj->property("items").value<QObject *>());
        CHECK(model && model->rowCount() == 2);
        if (!model) return 1;
        int name = model->roleNames().key("name", -1); CHECK(name >= 0);
        CHECK(model->data(model->index(1,0), name).toString() == "beta");
        QVariant result;
        CHECK(QMetaObject::invokeMethod(obj.get(), "appendRow", Q_RETURN_ARG(QVariant, result)));
        CHECK(result.toInt() == 3 && model->rowCount() == 3);
        CHECK(model->data(model->index(2,0), name).toString() == "gamma");
    }
    {
        QFile script(dir.filePath("worker.js"));
        const QByteArray code = "WorkerScript.onMessage = function(m) { WorkerScript.sendMessage({text:m.text.toUpperCase(), total:m.numbers.reduce(function(a,b){return a+b;},0)}); };";
        CHECK(script.open(QIODevice::WriteOnly) && script.write(code) == code.size()); script.close();
        QQmlComponent c(&engine);
        auto obj = create(c, R"QML(import QtQml
import QtQml.WorkerScript
QtObject {
    id: root
    property bool done: false
    property string text: ""
    property int total: 0
    property WorkerScript worker: WorkerScript {
        source: "worker.js"
        onMessage: function(message) { root.text=message.text; root.total=message.total; root.done=true }
    }
    Component.onCompleted: worker.sendMessage({text:"worker", numbers:[1,2,3,4]})
})QML", QUrl::fromLocalFile(dir.filePath("worker.qml")));
        if (!obj) return 1;
        CHECK(until([&] { return obj->property("done").toBool(); }));
        CHECK(obj->property("text").toString() == "WORKER" && obj->property("total").toInt() == 10);
    }
    {
        QFile f(dir.filePath("disk.qml"));
        QByteArray code = "import QtQml\nQtObject { property int answer: 6 * 7 }\n";
        CHECK(f.open(QIODevice::WriteOnly) && f.write(code) == code.size()); f.close();
        QQmlComponent c(&engine, QUrl::fromLocalFile(f.fileName()));
        CHECK(until([&] { return !c.isLoading(); }) && c.isReady());
        std::unique_ptr<QObject> obj(c.create()); CHECK(obj && obj->property("answer").toInt() == 42);
        QQmlComponent broken(&engine);
        broken.setData("import QtQml\nQtObject { property int n: }", QUrl::fromLocalFile(dir.filePath("broken.qml")));
        CHECK(broken.isError() && !broken.errors().isEmpty());
    }
    engine.collectGarbage(); CHECK(bridge.value() == 11);
    alarm(0);
    std::printf("qmltest: %s (%d checks, %d failures), Qt %s\n", fails ? "FAILED" : "PASSED", checks, fails, qVersion());
    return fails != 0;
}
#include "qmltest.moc"
