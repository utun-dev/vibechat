/* Desktop по умолчанию запускает core/chat; явный UTUN независим от создания TUN. */
#include "../transport/utun_node.h"
#include "../transport/gui_bridge.h"
#include "../src/statuspage.h"
#include "../src/startupfailure_dialog.h"
#include "db_storage.h"
#include "db_schema.h"
#include <QPushButton>
#include <QtTest>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>

extern "C" {
#include "utun_instance.h"
#include "ntp_time.h"
#include "tun_if.h"
#include "routing_layer/topo_group.h"
#include "transport_layer/node_conn_direct.h"
#include "u_async.h"
#include "debug_config.h"
#include "call/call_video_port.h"
#include "stream/stream_publisher.h"
#include "stream/stream_viewer.h"
#include "mem.h"
}

#ifdef UTUN_TEST_NODE_STREAM_FAILURES
static std::atomic<int> streamFailure{0};
extern "C" int __real_stream_publisher_create(struct stream_publisher**, UTUN_INSTANCE*);
extern "C" int __real_stream_viewers_create(struct stream_viewers**, UTUN_INSTANCE*);
extern "C" int __wrap_stream_publisher_create(struct stream_publisher** out, UTUN_INSTANCE* inst) {
    if (streamFailure == 1) {
        streamFailure = 0; DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "[INJECT] desktop publisher creation failure"); return -1;
    }
    return __real_stream_publisher_create(out, inst);
}
extern "C" int __wrap_stream_viewers_create(struct stream_viewers** out, UTUN_INSTANCE* inst) {
    if (streamFailure == 2) {
        streamFailure = 0; DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "[INJECT] desktop viewers creation failure"); return -1;
    }
    return __real_stream_viewers_create(out, inst);
}
#endif

static const QByteArray identity =
    "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
    "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n";

class UtunNodeTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { gui_bridge_init(); }
    void cleanup() { debug_disable_file_output(); }

    void routeChangesStillNotifyPmPresence() {
        static int notifications;
        notifications = 0;
        gui_bridge_set_network_changed_cb([](int type) { if (type == GUI_EVT_NODEINFO_UPDATE) notifications++; });
        TOPO_GROUP_NODE peer{}; peer.node_id = 7;
        gui_nodeinfo_cb_impl(nullptr, &peer, nullptr);
        QTRY_COMPARE(notifications, 1);
        gui_bridge_set_network_changed_cb(nullptr);
    }

    void incompatibleDatabaseIsReportedToGuiWithoutChangingProfile() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg"), dbPath = directory.filePath("old/chats.db");
        QVERIFY(QDir().mkpath(directory.filePath("old")));
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity + "[chatserver]\ngroup_autoconnect=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        sqlite3* db = nullptr; QCOMPARE(sqlite3_open(dbPath.toUtf8().constData(), &db), SQLITE_OK);
        QCOMPARE(sqlite3_exec(db, "CREATE TABLE peers_42(node_id INTEGER PRIMARY KEY,userinfo TEXT);"
            "INSERT INTO peers_42 VALUES(7,'retained profile')", nullptr, nullptr, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_close(db), SQLITE_OK);
        QFile original(dbPath); QVERIFY(original.open(QIODevice::ReadOnly)); const QByteArray before = original.readAll(); original.close();
        UtunNode node; node.setDbPath(directory.filePath("old")); node.setDebugFile(directory.filePath("node.log"));
        node.setDebugCategories("sys=none,bgp=none");
        QSignalSpy errors(&node, &UtunNode::error), started(&node, &UtunNode::started);
        QVERIFY(!node.start(path)); QVERIFY(!node.isRunning() && !node.instance());
        QCOMPARE(errors.count(), 1); QCOMPARE(started.count(), 0);
        const QString reason = errors.first().first().toString();
        QCOMPARE(reason, node.startupError()); QVERIFY(reason.contains(dbPath) && reason.contains("peers_42"));
        QVERIFY(node.startupDatabaseIncompatible());
        QVERIFY(reason.contains(QString::fromUtf8("база не изменена")));
        QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), before); original.close();
        QVERIFY(!QFile::exists(dbPath + "-wal"));
        QCOMPARE(db_storage_clear(directory.filePath("old").toUtf8().constData()), 0);
        QVERIFY(node.start(path)); QVERIFY(!node.startupDatabaseIncompatible()); node.stop();
    }

    void startupDialogRequiresExplicitRecreateChoice() {
        using Kind = StartupFailureDialog::Kind;
        using Action = StartupFailureDialog::Action;
        StartupFailureDialog exitDialog("schema differs", "diagnostics", Kind::Database);
        auto* exit = exitDialog.findChild<QPushButton*>("exitStartup");
        QVERIFY(exit && exitDialog.defaultButton() == exit && exitDialog.escapeButton() == exit);
        QVERIFY(exitDialog.findChild<QPushButton*>("recreateDatabase"));
        QTimer::singleShot(0, &exitDialog, [&] { QTest::keyClick(&exitDialog, Qt::Key_Escape); });
        exitDialog.exec(); QCOMPARE(exitDialog.choice(), Action::Exit);
        StartupFailureDialog recreateDialog("schema differs", "diagnostics", Kind::Database);
        auto* recreate = recreateDialog.findChild<QPushButton*>("recreateDatabase"); QVERIFY(recreate);
        QTimer::singleShot(0, recreate, &QPushButton::click);
        recreateDialog.exec(); QCOMPARE(recreateDialog.choice(), Action::RecreateDatabase);
        StartupFailureDialog ioFailure("access denied", "diagnostics", Kind::Configuration);
        QVERIFY(!ioFailure.findChild<QPushButton*>("recreateDatabase") && !ioFailure.findChild<QPushButton*>("replaceKey"));
        StartupFailureDialog keyDialog("old key", "diagnostics", Kind::Identity);
        QVERIFY(!keyDialog.findChild<QPushButton*>("recreateDatabase"));
        auto* replace = keyDialog.findChild<QPushButton*>("replaceKey"); QVERIFY(replace);
        QTimer::singleShot(0, replace, &QPushButton::click);
        keyDialog.exec(); QCOMPARE(keyDialog.choice(), Action::ReplaceKey);
    }

    void oversizedDatabaseDirectoryCannotOfferResetOfAnotherPath() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity + "[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        UtunNode node;
        node.setDbPath(directory.path() + '/' + QString(120, 'a') + '/' + QString(120, 'b') + '/' + QString(30, 'c'));
        QVERIFY(!node.start(path) && !node.isRunning() && !node.startupDatabaseIncompatible());
        QVERIFY(node.startupError().contains("too long"));
    }

    void services_data() {
        QTest::addColumn<QByteArray>("settings");
        QTest::addColumn<bool>("withUtun");
        QTest::addColumn<bool>("withTun");
        QTest::newRow("chat-default") << QByteArray() << false << false;
        QTest::newRow("tun-disabled") << QByteArray("tun_enabled=no\ntun_ifname=unused\ntun_ip=198.18.0.1\n") << false << false;
        QTest::newRow("utun-explicit") << QByteArray("[gui]\nutun_enabled=yes\n") << true << false;
        QTest::newRow("tun-does-not-enable-utun") << QByteArray("tun_enabled=yes\ntun_ifname=unused\n") << false << false;
#ifndef Q_OS_WIN
        QTest::newRow("tun-queues") << QByteArray("tun_enabled=yes\ntun_test_mode=1\ntun_ifname=vibetest\ntun_ip=198.18.0.1\n"
            "[gui]\nutun_enabled=yes\n") << true << true;
#endif
        if (qEnvironmentVariableIsSet("UTUN_TEST_REAL_TUN"))
            QTest::newRow("tun-real") << QByteArray("tun_enabled=yes\ntun_ifname=vibetest\ntun_ip=198.18.0.1\n"
                "[gui]\nutun_enabled=yes\n") << true << true;
    }

    void services() {
        QFETCH(QByteArray, settings); QFETCH(bool, withUtun); QFETCH(bool, withTun);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity + settings + "[server:local]\naddr=127.0.0.1:0\ntype=private\n"
            "[client:seed]\npeer_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\nlink=local:127.0.0.1:9\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        UtunNode node;
        node.setDbPath(directory.filePath("db")); node.setDebugFile(directory.filePath("node.log"));
        node.setDebugCategories("sys=info,tun=debug,general=info");
        QSignalSpy started(&node, &UtunNode::started);
        for (int attempt = 0; attempt < 2; ++attempt) {
            const bool running = node.start(path);
            if (!running) {
                directory.setAutoRemove(false);
                qWarning() << "Startup diagnostics:" << directory.filePath("node.log");
            }
            QVERIFY(running); QCOMPARE(started.count(), attempt + 1);
            QVERIFY(node.isRunning() && gui_bridge_is_uasync_ready());
            bool ready = false, valid = false, transport = false;
            struct Probe { QObject* receiver; bool* ready; bool* valid; bool* transport; bool utun, tun; }
                probe{this, &ready, &valid, &transport, withUtun, withTun};
            auto inspect = [](void* arg) {
                auto* p = static_cast<Probe*>(arg);
                auto* inst = gui_bridge_get_inst();
                const bool ok = inst && inst->core_started && inst->utun_started == p->utun && inst->chat_started &&
                    uasync_is_running(inst->ua) && (topo_groups_get_default(inst->topo_groups) != nullptr) == p->utun &&
                    inst->config_conn_handles && inst->config_conn_handles->transport &&
                    (inst->config_conn_handles->request != nullptr) == p->utun &&
                    (inst->tun != nullptr) == p->tun;
                const bool connected = inst && inst->config_conn_handles && node_conn_direct_get_conn(inst->config_conn_handles->transport);
                DEBUG_INFO(DEBUG_CATEGORY_TUN, "desktop startup test: services=%d expected_tun=%d transport_created=%d", ok, p->tun, connected);
                QMetaObject::invokeMethod(p->receiver, [p, ok, connected] {
                    *p->valid = ok; *p->transport = connected; *p->ready = true;
                }, Qt::QueuedConnection);
            };
            gui_bridge_post_uasync_fn(inspect, &probe);
            QTRY_VERIFY_WITH_TIMEOUT(ready, 3000); QVERIFY(valid);
            /* Принятый NCD handle ждёт SQL; отдельно подтверждаем создание транспорта после done. */
            QElapsedTimer deadline; deadline.start();
            while (!transport && deadline.elapsed() < 3000) {
                ready = false; gui_bridge_post_uasync_fn(inspect, &probe);
                QTRY_VERIFY_WITH_TIMEOUT(ready, 3000); QVERIFY(valid);
            }
            QVERIFY(transport);
            node.stop();
            QVERIFY(!node.isRunning() && !node.instance() && !gui_bridge_get_inst() && !gui_bridge_is_uasync_ready());
        }
    }

    void coreStopFinishesOwnerThread_data() {
        QTest::addColumn<bool>("direct");
        QTest::newRow("core-stop") << false; QTest::newRow("GUI-stop") << true;
    }
    void coreStopFinishesOwnerThread() {
        QFETCH(bool, direct);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity + "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        UtunNode node;
        node.setDbPath(directory.filePath("db")); node.setDebugFile(directory.filePath("node.log"));
        QSignalSpy stopped(&node, &UtunNode::stopped), stopping(&node, &UtunNode::stopping);
        sqlite3* borrowed = nullptr; bool liveDuringDetach = false;
        connect(&node, &UtunNode::stopping, &node, [&] {
            sqlite3_stmt* statement = nullptr;
            if (borrowed && QThread::currentThread() == qApp->thread()) {
                int rc = sqlite3_prepare_v2(borrowed, "SELECT ed25519_pubkey FROM local_identity WHERE id=1", -1, &statement, nullptr);
                if (rc == SQLITE_OK) rc = sqlite3_step(statement);
                const int final = sqlite3_finalize(statement); liveDuringDetach = rc == SQLITE_ROW && final == SQLITE_OK;
            }
            borrowed = nullptr;
        });
        QVERIFY(node.start(path));
        borrowed = node.databaseHandle(); QVERIFY(borrowed);
        QCOMPARE(node.databasePath(), directory.filePath("db"));
        if (direct) node.stop();
        else gui_bridge_post_uasync_fn([](void*) { utun_instance_stop(gui_bridge_get_inst()); }, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
        QCOMPARE(stopping.count(), 1); QVERIFY(liveDuringDetach); QVERIFY(!borrowed); QVERIFY(!node.databaseHandle());
        node.stop();
        QVERIFY(!node.isRunning() && !node.instance() && !gui_bridge_get_inst() && !gui_bridge_is_uasync_ready());
    }

    void restartKeepsLogPathAndDoesNotLeakFiles() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg"), logPath = directory.filePath("node.log");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity +
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
#ifdef Q_OS_LINUX
        const int descriptors = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
#endif
        {
            UtunNode node;
            node.setDbPath(directory.filePath("db")); node.setDebugFile(logPath);
            for (int i = 0; i < 3; i++) {
                QVERIFY(node.start(path));
                node.stop();
                // Путь хранится logger-ом до закрытия файла, включая время после stop.
                QCOMPARE(QByteArray(g_debug_config.output_file), logPath.toUtf8());
                QCOMPARE(debug_reopen_log(), 0);
            }
        }
        debug_disable_file_output();
#ifdef Q_OS_LINUX
        QCOMPARE(QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), descriptors);
#endif
    }

    void mediaPortFailureRejectsStartupAndAllowsRetry() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity +
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        struct PortOwner {
            UTUN_INSTANCE instance{};
            ~PortOwner() { call_video_port_detach(&instance); }
        } owner;
        QCOMPARE(call_video_port_attach(&owner.instance), 0);
        UtunNode node;
        node.setDbPath(directory.filePath("db")); node.setDebugFile(directory.filePath("node.log"));
        QSignalSpy started(&node, &UtunNode::started), errors(&node, &UtunNode::error);
        const bool running = node.start(path);
        if (running) node.stop(); // Старая реализация не должна оставлять поток после FAIL теста.
        QVERIFY(!running);
        QVERIFY(!node.isRunning() && !node.instance() && !gui_bridge_get_inst() && !gui_bridge_is_uasync_ready());
        QCOMPARE(started.count(), 0); QCOMPARE(errors.count(), 1);
        QVERIFY(node.startupError().contains("call_video_port_attach"));
        // Откат не снимает чужую привязку.
        QVERIFY(call_video_port_attach(&owner.instance) < 0);
        call_video_port_detach(&owner.instance);
        QVERIFY(node.start(path)); node.stop();
    }

    void restartDiscardsPreviousStoppedAndCopiesCoreState() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity +
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        UtunNode node;
        node.setDbPath(directory.filePath("db")); node.setDebugFile(directory.filePath("node.log"));
        node.setDebugCategories("sys=none,bgp=none");
        QSignalSpy stopped(&node, &UtunNode::stopped), errors(&node, &UtunNode::error);
        QVERIFY(node.start(path));
        QCOMPARE(g_debug_config.category_levels[DEBUG_CATEGORY_SYS], DEBUG_LEVEL_NONE);
        QCOMPARE(g_debug_config.category_levels[DEBUG_CATEGORY_BGP], DEBUG_LEVEL_NONE);
        const QString id = node.nodeIdHex(); QCOMPARE(id, QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(node.instance()->my_ed25519_pubkey), 32).toHex()));
        QCOMPARE(node.pubKeyHex(), QStringLiteral("7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f"));
        QVERIFY(!node.start(path)); QCOMPARE(errors.count(), 1); QVERIFY(node.isRunning());
        node.stop();
        QVERIFY(node.nodeIdHex().isEmpty() && node.pubKeyHex().isEmpty());
        QVERIFY(!node.ntpEnabled() && !node.ntpSynced() && node.ntpOffsetUs() == 0);
        QVERIFY(node.start(path)); QCOMPARE(node.nodeIdHex(), id);
        QCoreApplication::processEvents(); QCOMPARE(stopped.count(), 0); QVERIFY(node.isRunning());
        gui_bridge_post_uasync_fn([](void*) {
            auto* inst = gui_bridge_get_inst();
            ntp_time_record_offset(inst, NTP_TIME_SERVER, 1234567); inst->ntp.enabled = 1;
        }, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(node.ntpOffsetUs(), int64_t(1234567), 3000);
        QVERIFY(node.ntpSynced() && node.ntpEnabled());
        gui_bridge_post_uasync_fn([](void*) { utun_instance_stop(gui_bridge_get_inst()); }, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
        QVERIFY(!node.instance()); QCOMPARE(node.nodeIdHex(), id); // Копия переживает ядро до stop.
        node.stop();
        QVERIFY(node.nodeIdHex().isEmpty() && !node.ntpSynced());
    }

    void statusPageRefreshBeforeStartWhileRunningAndAfterStop() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity +
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        UtunNode node;
        node.setDbPath(directory.filePath("db")); node.setDebugFile(directory.filePath("node.log"));
        StatusPage page(&node);
        auto* status = page.findChild<QPlainTextEdit*>(); QVERIFY(status);
        page.refreshStatus(); QCOMPARE(status->toPlainText(), QStringLiteral("Node is not running."));
        QVERIFY(node.start(path));
        page.refreshStatus();
        QTRY_VERIFY_WITH_TIMEOUT(status->toPlainText().contains("=== NTP ==="), 3000);
        QVERIFY(status->toPlainText().contains("=== Connections (0) ==="));
        node.stop();
        page.refreshStatus(); QCOMPARE(status->toPlainText(), QStringLiteral("Node is not running."));
        page.setText(QStringLiteral("late snapshot from stopped core"));
        QCoreApplication::processEvents();
        QCOMPARE(status->toPlainText(), QStringLiteral("Node is not running."));
    }

    void statusPageSurvivesDeletedNodeAndUsesLocalClock() {
        auto* node = new UtunNode;
        StatusPage page(node);
        delete node;
        page.refreshStatus();
        auto* status = page.findChild<QPlainTextEdit*>(); QVERIFY(status);
        QCOMPARE(status->toPlainText(), QStringLiteral("Node is not running."));
        bool local = false;
        for (auto* label : page.findChildren<QLabel*>()) local |= label->text().contains("[LOCAL]");
        QVERIFY(local);
    }

#ifdef UTUN_TEST_NODE_STREAM_FAILURES
    void streamFailureRollsBackAndAllowsRetry_data() {
        QTest::addColumn<int>("stage");
        QTest::newRow("publisher") << 1;
        QTest::newRow("viewers-after-publisher") << 2;
    }

    void streamFailureRollsBackAndAllowsRetry() {
        QFETCH(int, stage);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity +
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        const size_t allocated = u_get_allocated_count();
        UtunNode node;
        node.setDbPath(directory.filePath("db")); node.setDebugFile(directory.filePath("node.log"));
        QSignalSpy started(&node, &UtunNode::started), stopped(&node, &UtunNode::stopped), errors(&node, &UtunNode::error);
        streamFailure = stage;
        QVERIFY(!node.start(path)); QCOMPARE(streamFailure.load(), 0);
        QCOMPARE(started.count(), 0); QCOMPARE(errors.count(), 1);
        QVERIFY(node.startupError().contains("stream bridge"));
        QVERIFY(!node.isRunning() && !node.instance() && !gui_bridge_get_inst() && !gui_bridge_is_uasync_ready());
        QCoreApplication::processEvents(); QCOMPARE(stopped.count(), 0);
        QCOMPARE(u_get_allocated_count(), allocated);
        QVERIFY(node.start(path)); node.stop(); QCOMPARE(started.count(), 1);
        QCOMPARE(u_get_allocated_count(), allocated);
    }
#endif

    void tunFailureRejectsStartup() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        // Нет tun_ip: отказ до обращения к ОС воспроизводится и на Windows без установленного драйвера.
        const QByteArray text = "[global]\n" + identity +
            "tun_enabled=yes\n[gui]\nutun_enabled=yes\n[server:local]\naddr=127.0.0.1:0\ntype=private\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        UtunNode node;
        node.setDbPath(directory.filePath("db")); node.setDebugFile(directory.filePath("node.log"));
        QSignalSpy started(&node, &UtunNode::started), errors(&node, &UtunNode::error);
        QVERIFY(!node.start(path)); QCOMPARE(started.count(), 0);
        QVERIFY(!node.isRunning() && !node.instance() && !gui_bridge_get_inst() && !gui_bridge_is_uasync_ready());
        QTRY_COMPARE(errors.count(), 1);
        const QString error = errors.first().first().toString();
        QVERIFY2(error.contains("utun_service_start"), qPrintable(error));
        QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QByteArray disabled = text; disabled.replace("tun_enabled=yes", "tun_enabled=no");
        QCOMPARE(config.write(disabled), disabled.size()); config.close();
        QVERIFY(node.start(path)); QCOMPARE(started.count(), 1);
        node.stop(); QVERIFY(!node.isRunning() && !gui_bridge_is_uasync_ready());
    }

    void invalidConfigRejectsStartup_data() {
        QTest::addColumn<QByteArray>("invalid");
        QTest::newRow("removed-radio-option") << QByteArray("[chatserver]\nradio_vad_enabled=1\n");
        QTest::newRow("removed-storage-option") << QByteArray("[chat]\nstorage_maxsize_gb=1\n");
        QTest::newRow("unsupported-fingerprint") << QByteArray("[client:peer]\nfingerprint=chrome\n");
    }

    void invalidConfigRejectsStartup() {
        QFETCH(QByteArray, invalid);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("invalid.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n" + identity + invalid;
        QCOMPARE(config.write(text), text.size()); config.close();
        UtunNode node;
        node.setDbPath(directory.filePath("db")); node.setDebugFile(directory.filePath("node.log"));
        QSignalSpy started(&node, &UtunNode::started), stopped(&node, &UtunNode::stopped), errors(&node, &UtunNode::error);
        QVERIFY(!node.start(path));
        QVERIFY(!node.isRunning() && !node.instance() && !gui_bridge_is_uasync_ready());
        QCOMPARE(started.count(), 0); QCOMPARE(errors.count(), 1);
        QVERIFY(node.startupError().contains(path));
        QCOMPARE(errors.first().first().toString(), node.startupError());
        node.requestStop(); node.stop(); QCoreApplication::processEvents();
        QCOMPARE(stopped.count(), 0); // При отказе запуска ждать stopped нельзя: работающего worker уже нет.
        QCOMPARE(errors.count(), 1);
    }
};
QTEST_MAIN(UtunNodeTest)
#include "test_utun_node.moc"
