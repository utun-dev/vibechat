/* Настоящие процессы и локальные сокеты: один владелец профиля, ACK, startup-race,
 * аварийный выход и показ окна без запуска сетевого ядра/аудиоустройств. */
#include "../src/single_instance.h"
#include "debug_config.h"
#include <QtTest>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <vector>

// Дочерний процесс создаёт окно только после определения роли.
static int peerMain(QApplication& app, const QString& profile) {
    SingleInstance instance(profile);
    const auto role = instance.start();
    if (role == SingleInstance::Error) return 2;
    if (role == SingleInstance::Activated) { qInfo("PEER_FORWARDED"); return 0; }
    QWidget window;
    instance.setWindow(&window);
    qInfo("PEER_PRIMARY");
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &window, [&] {
        if (window.isVisible()) { qInfo("PEER_VISIBLE"); watch.stop(); }
    });
    watch.start(10);
    QTimer::singleShot(15000, &app, &QCoreApplication::quit);
    return app.exec();
}

// Процесс всегда завершается до удаления временного каталога, включая ранний выход QVERIFY.
struct Peer {
    QProcess process;
    QByteArray output;
    explicit Peer(const QString& profile) {
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QCoreApplication::applicationFilePath(), {"--peer", profile});
    }
    ~Peer() {
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); }
    }
    bool saw(const QByteArray& marker) { output += process.readAll(); return output.contains(marker); }
};

class InstanceTest : public QObject {
    Q_OBJECT
private slots:
    void forwardsToExistingProcess() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        QString path = profile.filePath(QString::fromUtf8("profile space-кириллица")); QVERIFY(QDir().mkpath(path));
        Peer first(path); QTRY_VERIFY(first.saw("PEER_PRIMARY"));
        Peer second(path); QTRY_COMPARE(second.process.state(), QProcess::NotRunning);
        QCOMPARE(second.process.exitCode(), 0); QVERIFY(second.saw("PEER_FORWARDED"));
        QVERIFY(!second.output.contains("PEER_PRIMARY")); QTRY_VERIFY(first.saw("PEER_VISIBLE"));
        QVERIFY(!QFileInfo::exists(QDir(path).filePath("vibechat.cfg")));
    }

    void simultaneousStartup() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        std::vector<std::unique_ptr<Peer>> peers;
        for (int i = 0; i < 4; i++) peers.push_back(std::make_unique<Peer>(profile.path()));
        for (const auto& peer : peers) QTRY_VERIFY(peer->saw("PEER_PRIMARY") || peer->saw("PEER_FORWARDED"));
        unsigned primary = 0, forwarded = 0;
        for (const auto& peer : peers) {
            if (peer->output.contains("PEER_PRIMARY")) { primary++; QTRY_VERIFY(peer->saw("PEER_VISIBLE")); }
            else {
                QTRY_COMPARE(peer->process.state(), QProcess::NotRunning);
                QCOMPARE(peer->process.exitCode(), 0); forwarded++;
            }
        }
        QCOMPARE(primary, 1U); QCOMPARE(forwarded, 3U);
    }

    void differentProfiles() {
        QTemporaryDir a, b; QVERIFY(a.isValid() && b.isValid());
        Peer first(a.path()), second(b.path());
        QTRY_VERIFY(first.saw("PEER_PRIMARY")); QTRY_VERIFY(second.saw("PEER_PRIMARY"));
        QVERIFY(!first.output.contains("PEER_FORWARDED") && !second.output.contains("PEER_FORWARDED"));
    }

    void recoversAfterCrash() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        Peer first(profile.path()); QTRY_VERIFY(first.saw("PEER_PRIMARY"));
        first.process.kill(); QVERIFY(first.process.waitForFinished(3000));
        Peer replacement(profile.path()); QTRY_VERIFY(replacement.saw("PEER_PRIMARY"));
        Peer secondary(profile.path()); QTRY_COMPARE(secondary.process.state(), QProcess::NotRunning);
        QCOMPARE(secondary.process.exitCode(), 0); QTRY_VERIFY(replacement.saw("PEER_VISIBLE"));
    }

    void queuesActivationUntilWindowExists() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        SingleInstance instance(profile.path()); QCOMPARE(instance.start(), SingleInstance::Primary);
        Peer secondary(profile.path()); QTRY_COMPARE(secondary.process.state(), QProcess::NotRunning);
        QCOMPARE(secondary.process.exitCode(), 0);
        QWidget window; QVERIFY(!window.isVisible());
        instance.setWindow(&window); window.showMinimized();
        QTRY_VERIFY(window.isVisible() && !window.isMinimized());
    }

    void waitsForServerStartup() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        QLockFile lock(profile.filePath("vibechat.lock")); QVERIFY(lock.tryLock());
        SingleInstance identity(profile.path()); QLocalServer server;
        QTimer::singleShot(250, &server, [&] { QVERIFY(server.listen(identity.serverName())); });
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection(); QVERIFY(socket);
            connect(socket, &QLocalSocket::readyRead, socket, [socket, request = QByteArray{}]() mutable {
                request += socket->readAll();
                if (!request.endsWith('\n')) return;
                QCOMPARE(request, QByteArray("ACTIVATE\n"));
                QCOMPARE(socket->write("OK\n"), 3LL); socket->disconnectFromServer();
            });
        });
        Peer secondary(profile.path()); QTRY_COMPARE(secondary.process.state(), QProcess::NotRunning);
        QCOMPARE(secondary.process.exitCode(), 0); QVERIFY(secondary.saw("PEER_FORWARDED")); QVERIFY(lock.isLocked());
    }

    void liveOwnerWithoutServerTimesOut() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        QLockFile lock(profile.filePath("vibechat.lock")); QVERIFY(lock.tryLock());
        QElapsedTimer time; time.start();
        Peer secondary(profile.path()); QTRY_COMPARE_WITH_TIMEOUT(secondary.process.state(), QProcess::NotRunning, 7000);
        QCOMPARE(secondary.process.exitCode(), 2); QVERIFY(secondary.saw("did not acknowledge"));
        QVERIFY(time.elapsed() < 7000 && lock.isLocked()); QVERIFY(!secondary.output.contains("PEER_PRIMARY"));
    }

    void rejectsMissingAndInvalidAcknowledgement_data() {
        QTest::addColumn<QByteArray>("reply");
        QTest::newRow("invalid") << QByteArray("NO\n");
        QTest::newRow("disconnected") << QByteArray();
        QTest::newRow("truncated") << QByteArray("O");
    }

    void rejectsMissingAndInvalidAcknowledgement() {
        QFETCH(QByteArray, reply);
        QTemporaryDir profile; QVERIFY(profile.isValid());
        QLockFile lock(profile.filePath("vibechat.lock")); QVERIFY(lock.tryLock());
        SingleInstance identity(profile.path()); QLocalServer server; QVERIFY(server.listen(identity.serverName()));
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection(); QVERIFY(socket);
            connect(socket, &QLocalSocket::readyRead, socket, [socket, reply] {
                socket->readAll();
                if (!reply.isEmpty()) QCOMPARE(socket->write(reply), qint64(reply.size()));
                socket->disconnectFromServer();
            });
        });
        Peer secondary(profile.path()); QTRY_COMPARE(secondary.process.state(), QProcess::NotRunning);
        QCOMPARE(secondary.process.exitCode(), 2); QVERIFY(!secondary.saw("PEER_PRIMARY")); QVERIFY(lock.isLocked());
    }

    void validatesCommandsAndPartialReads() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        SingleInstance instance(profile.path()); QCOMPARE(instance.start(), SingleInstance::Primary);
        QWidget window; instance.setWindow(&window);
        QLocalSocket bad; bad.connectToServer(instance.serverName()); QVERIFY(bad.waitForConnected(1000));
        QCOMPARE(bad.write("BAD\n"), 4LL); QVERIFY(bad.waitForBytesWritten(1000));
        QTRY_COMPARE(bad.state(), QLocalSocket::UnconnectedState); QVERIFY(!window.isVisible());
        QLocalSocket good; good.connectToServer(instance.serverName()); QVERIFY(good.waitForConnected(1000));
        QCOMPARE(good.write("ACT"), 3LL); QVERIFY(good.waitForBytesWritten(1000));
        QTest::qWait(30); QVERIFY(!window.isVisible()); QCOMPARE(good.bytesAvailable(), 0LL);
        QCOMPARE(good.write("IVATE\n"), 6LL); QVERIFY(good.waitForBytesWritten(1000));
        QTRY_VERIFY(window.isVisible()); QTRY_COMPARE(good.bytesAvailable(), 3LL); QCOMPARE(good.readAll(), QByteArray("OK\n"));
    }

    void missingDirectoryFails() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        SingleInstance instance(profile.filePath("missing")); QCOMPARE(instance.start(), SingleInstance::Error);
        QVERIFY(instance.errorString().contains("does not exist"));
        QVERIFY(!QFileInfo::exists(profile.filePath("missing")));
    }

    void forwardsUrlAndDefersUntilHandlerReady() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        const QString first = QStringLiteral("utun://chat/Команда@") + QString(64, 'c') + QStringLiteral("/@") + QString(64, 'a');
        const QString last = QStringLiteral("utun://kanban/Разработка@") + QString(64, 'd') + QStringLiteral("/Задача@") + QString(64, 'b');
        SingleInstance instance(profile.path()); QCOMPARE(instance.start(first), SingleInstance::Primary);
        QWidget window; instance.setWindow(&window); QStringList received;
        QLocalSocket socket; socket.connectToServer(instance.serverName()); QVERIFY(socket.waitForConnected(1000));
        QByteArray request = QByteArray("OPEN ") + last.toUtf8() + '\n';
        QCOMPARE(socket.write(request.left(9)), 9LL); QVERIFY(socket.waitForBytesWritten(1000));
        QTest::qWait(20); QCOMPARE(socket.bytesAvailable(), 0LL);
        QCOMPARE(socket.write(request.mid(9)), qint64(request.size() - 9)); QVERIFY(socket.waitForBytesWritten(1000));
        QTRY_COMPARE(socket.bytesAvailable(), 3LL); QCOMPARE(socket.readAll(), QByteArray("OK\n"));
        QVERIFY(received.isEmpty());
        instance.setUrlHandler([&](const QString& url) { received.append(url); });
        QTRY_COMPARE(received, QStringList{last});
        instance.setWindow(&window); QTest::qWait(20); QCOMPARE(received.size(), 1);
    }

    void rejectsUrlControlsAndOversizedFrames() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        SingleInstance instance(profile.path()); QCOMPARE(instance.start(), SingleInstance::Primary);
        QWidget window; instance.setWindow(&window); int opened = 0;
        instance.setUrlHandler([&](const QString&) { opened++; });
        for (const auto& request : {QByteArray("OPEN https://example.com/\n"), QByteArray("OPEN utun://a\nb\n"),
            QByteArray("OPEN utun://") + QByteArray(4096, 'a') + '\n'}) {
            QLocalSocket socket; socket.connectToServer(instance.serverName()); QVERIFY(socket.waitForConnected(1000));
            QCOMPARE(socket.write(request), qint64(request.size())); QVERIFY(socket.waitForBytesWritten(1000));
            QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState); QCOMPARE(opened, 0);
        }
    }

    void restoresWindowState() {
        QWidget window;
        showAndActivateWindow(&window); QVERIFY(window.isVisible()); QVERIFY(!window.isMinimized());
        window.setWindowState(Qt::WindowMaximized | Qt::WindowMinimized);
        showAndActivateWindow(&window); QVERIFY(window.isVisible()); QVERIFY(window.isMaximized()); QVERIFY(!window.isMinimized());
        window.hide(); showAndActivateWindow(&window); QVERIFY(window.isVisible() && window.isMaximized());
    }

    void activatesModalDialog() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        SingleInstance instance(profile.path()); QCOMPARE(instance.start(), SingleInstance::Primary);
        QWidget window; instance.setWindow(&window); window.show();
        QDialog modal(&window); modal.setModal(true); modal.show();
        QTRY_COMPARE(QApplication::activeModalWidget(), &modal);
        Peer secondary(profile.path()); QTRY_COMPARE(secondary.process.state(), QProcess::NotRunning);
        QCOMPARE(secondary.process.exitCode(), 0); QVERIFY(modal.isVisible());
        QCOMPARE(QApplication::activeModalWidget(), &modal);
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    if (app.arguments().size() == 3 && app.arguments()[1] == QStringLiteral("--peer"))
        return peerMain(app, app.arguments()[2]);
    InstanceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_single_instance.moc"
