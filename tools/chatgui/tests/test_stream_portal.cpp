/* Изолированный D-Bus portal проверяет согласование курсора/отмену; отдельный PipeWire отдаёт настоящие RGB-кадры. */
#include "../src/stream_portal_capture.h"
#include "../../../lib/debug_config.h"
#include <QtTest>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMetaType>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

namespace {
const QString service = "org.freedesktop.portal.Desktop", root = "/org/freedesktop/portal/desktop";
struct PortalStream { uint node; QVariantMap properties; };
using PortalStreams = QList<PortalStream>;
QDBusArgument& operator<<(QDBusArgument& arg, const PortalStream& stream) {
    arg.beginStructure(); arg << stream.node << stream.properties; arg.endStructure(); return arg;
}
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalStream& stream) {
    arg.beginStructure(); arg >> stream.node >> stream.properties; arg.endStructure(); return arg;
}
}
Q_DECLARE_METATYPE(PortalStream)
Q_DECLARE_METATYPE(PortalStreams)

class CaptureRequest : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    using QObject::QObject;
    bool closed = false;
public slots:
    void Close() { closed = true; }
signals:
    void Response(uint code, const QVariantMap& results);
};
class CaptureSession : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
public:
    using QObject::QObject;
    bool closed = false;
public slots:
    void Close() { closed = true; }
signals:
    void Closed(const QVariantMap& details);
};
class CapturePortal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")
    Q_PROPERTY(uint version READ version)
    Q_PROPERTY(uint AvailableCursorModes MEMBER modes)
    Q_PROPERTY(uint AvailableSourceTypes READ sourceTypes)
public:
    uint version() const { return 6; }
    uint sourceTypes() const { return 1; }
    uint modes = 3, cursor = 0, startCode = 0;
    bool holdStart = true, starting = false;
    QString session, socket;
    PortalStreams streams{{42, {}}};
    CaptureRequest* pending = nullptr;
    CaptureSession* sessionObject = nullptr;
    QDBusObjectPath answer(const QVariantMap& options, uint code, const QVariantMap& result, bool hold = false) {
        QString sender = message().service().mid(1).replace('.', '_');
        QString path = root + "/request/" + sender + "/" + options.value("handle_token").toString();
        pending = new CaptureRequest(this);
        QDBusConnection::sessionBus().registerObject(path, pending, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
        if (!hold) QTimer::singleShot(0, pending, [request = pending, code, result] { emit request->Response(code, result); });
        return QDBusObjectPath(path);
    }
public slots:
    QDBusObjectPath CreateSession(const QVariantMap& options) {
        QString sender = message().service().mid(1).replace('.', '_');
        session = root + "/session/" + sender + "/" + options.value("session_handle_token").toString();
        sessionObject = new CaptureSession(this);
        QDBusConnection::sessionBus().registerObject(session, sessionObject, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
        return answer(options, 0, {{"session_handle", session}});
    }
    QDBusObjectPath SelectSources(const QDBusObjectPath&, const QVariantMap& options) {
        cursor = options.value("cursor_mode").toUInt();
        if (options.value("types").toUInt() != 1 || options.value("multiple").toBool()) cursor = 0;
        return answer(options, 0, {});
    }
    QDBusObjectPath Start(const QDBusObjectPath&, const QString&, const QVariantMap& options) {
        starting = true;
        return answer(options, startCode, {{"streams", QVariant::fromValue(streams)}}, holdStart);
    }
    QDBusUnixFileDescriptor OpenPipeWireRemote(const QDBusObjectPath&, const QVariantMap&) {
        QLocalSocket remote; remote.connectToServer(socket);
        if (!remote.waitForConnected(2000)) { sendErrorReply("org.freedesktop.portal.Error.Failed", remote.errorString()); return {}; }
        return QDBusUnixFileDescriptor(remote.socketDescriptor());
    }
};

class StreamPortalTest : public QObject {
    Q_OBJECT
    std::unique_ptr<CapturePortal> portal;
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_DEBUG);
        qDBusRegisterMetaType<PortalStream>(); qDBusRegisterMetaType<PortalStreams>();
        QVERIFY(QDBusConnection::sessionBus().registerService(service));
    }
    void init() {
        portal = std::make_unique<CapturePortal>();
        QVERIFY(QDBusConnection::sessionBus().registerObject(root, portal.get(),
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals | QDBusConnection::ExportAllProperties));
    }
    void cleanup() {
        QDBusConnection::sessionBus().unregisterObject(root, QDBusConnection::UnregisterTree); portal.reset();
    }
    void cursorModesAndCancellation_data() {
        QTest::addColumn<bool>("visible"); QTest::newRow("shown") << true; QTest::newRow("hidden") << false;
    }
    void cursorModesAndCancellation() {
        QFETCH(bool, visible);
        auto capture = std::make_unique<StreamPortalCapture>();
        QSignalSpy errors(capture.get(), &StreamPortalCapture::errorOccurred);
        capture->start(visible); QTRY_VERIFY(portal->starting);
        // Дожидаемся Start, затем уничтожаем захват, пока системный диалог ещё открыт.
        QCOMPARE(portal->cursor, visible ? 2u : 1u); QVERIFY(errors.isEmpty());
        CaptureRequest* pending = portal->pending;
        capture.reset(); QTRY_VERIFY(pending->closed); QTRY_VERIFY(portal->sessionObject->closed);
    }
    void unsupportedCursor() {
        portal->modes = 1;
        StreamPortalCapture capture; QSignalSpy errors(&capture, &StreamPortalCapture::errorOccurred);
        capture.start(true); QTRY_COMPARE(errors.size(), 1); QVERIFY(portal->session.isEmpty());
    }
    void selectionCancelled() {
        portal->holdStart = false; portal->startCode = 1;
        StreamPortalCapture capture; QSignalSpy errors(&capture, &StreamPortalCapture::errorOccurred);
        capture.start(true); QTRY_COMPARE(errors.size(), 1); QTRY_VERIFY(portal->sessionObject->closed);
    }
    void systemStopsSelection() {
        StreamPortalCapture capture; QSignalSpy errors(&capture, &StreamPortalCapture::errorOccurred);
        capture.start(true); QTRY_VERIFY(portal->starting);
        emit portal->sessionObject->Closed({}); QTRY_COMPARE(errors.size(), 1);
    }
    void realPipeWireFrames_data() {
        QTest::addColumn<bool>("serial"); QTest::addColumn<bool>("visible");
        QTest::newRow("serial-shown") << true << true;
        QTest::newRow("node-shown") << false << true;
        QTest::newRow("node-hidden") << false << false;
    }
    void realPipeWireFrames() {
        QFETCH(bool, serial); QFETCH(bool, visible);
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        QFile config(runtime.filePath("capture.conf")); QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(R"(
context.properties = { core.daemon = true core.name = pipewire-0 }
context.spa-libs = { support.* = support/libspa-support videotestsrc = videotestsrc/libspa-videotestsrc }
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-access args = { access.legacy = true } }
]
context.objects = [
    { factory = spa-node-factory args = { factory.name = videotestsrc node.name = cursor-test node.description = cursor-test
        media.class = Video/Source node.param.Props = { live = true } } }
]
)"); config.close();
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PIPEWIRE_RUNTIME_DIR", runtime.path());
        QProcess server; server.setProcessChannelMode(QProcess::MergedChannels);
        auto serverEnvironment = environment; serverEnvironment.insert("PIPEWIRE_DEBUG", "3"); server.setProcessEnvironment(serverEnvironment);
        server.start("pipewire", {"-c", config.fileName()}); QVERIFY(server.waitForStarted());
        auto stop = qScopeGuard([&] { server.terminate(); if (!server.waitForFinished(3000)) { server.kill(); server.waitForFinished(); } });
        portal->socket = runtime.filePath("pipewire-0");
        QTRY_VERIFY(QFile::exists(portal->socket));
        QProcess dump; dump.setProcessEnvironment(environment); dump.start("pw-dump");
        bool dumped = QTest::qWaitFor([&] { return dump.state() == QProcess::NotRunning; }, 5000);
        if (!dumped || dump.exitCode()) {
            qWarning().noquote() << "PipeWire server:" << server.readAll() << "dump:" << dump.readAllStandardError();
            if (!dumped) { dump.kill(); dump.waitForFinished(); }
        }
        QVERIFY(dumped); QCOMPARE(dump.exitCode(), 0);
        uint node = 0; QVariantMap props;
        for (const auto& value : QJsonDocument::fromJson(dump.readAllStandardOutput()).array()) {
            auto object = value.toObject(), properties = object.value("info").toObject().value("props").toObject();
            if (properties.value("node.name").toString() != "cursor-test") continue;
            node = object.value("id").toInt(); props["pipewire-serial"] = properties.value("object.serial").toVariant();
        }
        QVERIFY(node); if (!serial) props.clear(); portal->streams = {{node, props}}; portal->holdStart = false;
        StreamPortalCapture capture; QSignalSpy frames(&capture, &StreamPortalCapture::frameReady), errors(&capture, &StreamPortalCapture::errorOccurred);
        capture.start(visible);
        QProcess link; link.setProcessEnvironment(environment);
        // Без менеджера сессий соединяем только тестовые video ports, сохраняя настоящий транспорт PipeWire.
        QElapsedTimer elapsed; elapsed.start();
        while (frames.isEmpty() && errors.isEmpty() && elapsed.elapsed() < 6000) {
            link.start("pw-link", {"cursor-test:capture_1", "vibechat-screen:input_1"});
            if (!QTest::qWaitFor([&] { return link.state() == QProcess::NotRunning; }, 1000)) { link.kill(); link.waitForFinished(); }
            QTest::qWait(50);
        }
        if (!errors.isEmpty() || frames.isEmpty()) {
            qWarning().noquote() << "PipeWire server:" << server.readAll() << "link:" << link.readAllStandardError();
            link.start("pw-link", {"-io"});
            if (!QTest::qWaitFor([&] { return link.state() == QProcess::NotRunning; }, 1000)) { link.kill(); link.waitForFinished(); }
            qWarning().noquote() << "PipeWire ports:" << link.readAllStandardOutput();
        }
        QVERIFY(errors.isEmpty()); QVERIFY(!frames.isEmpty()); QCOMPARE(portal->cursor, visible ? 2u : 1u);
        auto frame = frames.last().front().value<QVideoFrame>(); QVERIFY(frame.isValid()); QVERIFY(!frame.toImage().isNull());
        QTRY_VERIFY(frames.size() >= 3);
        emit portal->sessionObject->Closed({}); QTRY_COMPARE(errors.size(), 1);
    }
};
QTEST_GUILESS_MAIN(StreamPortalTest)
#include "test_stream_portal.moc"
