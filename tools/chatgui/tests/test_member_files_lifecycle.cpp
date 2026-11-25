#include "member_files_fixture.h"
/* Реальные batch/browser и bridge: отказ очереди, detach до trampoline и итог без утечек.
 * Минимальное ядро без файлового сервиса даёт штатный отказ при исполнении команды. */
#include "../src/member_files/browser.h"
#include "../transport/gui_bridge.h"
#include "../db/db_manager.h"
#include <QtTest>
#include <QLabel>
#include <QDir>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QFileDialog>
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <cstring>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "routing_layer/topo_group.h"
#include "member_files/member_files.h"
#include "debug_config.h"
#include "mem.h"
}

#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
static int failAllocation;
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (failAllocation && strstr(where, "gui_bridge_impl.cpp") && !--failAllocation) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "[INJECT] file bridge allocation failure at %s", where); return nullptr;
    }
    return __real_u_calloc_impl(count, size, where);
}
#endif

struct FileCore {
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE instance{};
    void attach() {
        instance.ua = ua;
        memcpy(instance.my_ed25519_pubkey, fileKey(22).constData(), 32);
        chat_event_set_handler(&instance, [](UTUN_INSTANCE*, int event, const uint8_t* data, int size) {
            gui_bridge_post(event, data, size);
        });
        uasync_mark_running(ua); gui_bridge_set_uasync(ua); gui_bridge_set_inst(&instance);
    }
    ~FileCore() {
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        if (ua) { uasync_mark_stopped(ua); uasync_destroy(ua, 0); }
    }
};

static QPointer<MemberFileBatch> activeBatch;
static QPointer<MemberFileBrowser> activeBrowser;
static QList<QByteArray> responses, transfers;

static int pending(UASYNC* ua) {
    int count = 0;
    for (auto* task = ua->posted_tasks_head; task; task = task->next) count++;
    return count;
}

class FileLifecycleTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_TRACE);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        // Тест намеренно приостанавливает poll для detach/диалогов; предупреждения о stall здесь ожидаемы.
        debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
        gui_bridge_init();
        gui_bridge_set_member_files_response_cb([](const uint8_t* data, int length) {
            QVERIFY(QThread::currentThread() == qApp->thread()); responses.append(QByteArray((const char*)data, length));
            if (activeBrowser) activeBrowser->onResponse(data, length);
            mf_message message{};
            if (activeBatch && length >= MF_EVENT_RESPONSE_PREFIX && !mf_decode(data + MF_EVENT_RESPONSE_PREFIX, length - MF_EVENT_RESPONSE_PREFIX, &message)) activeBatch->response(message);
        });
        gui_bridge_set_member_files_transfer_cb([](const uint8_t* data, int length) {
            QVERIFY(QThread::currentThread() == qApp->thread()); transfers.append(QByteArray((const char*)data, length));
            if (activeBrowser) activeBrowser->onTransfer(data, length);
            if (activeBatch) activeBatch->transfer(data, length);
        });
    }
    void cleanupTestCase() {
        gui_bridge_set_member_files_response_cb(nullptr); gui_bridge_set_member_files_transfer_cb(nullptr);
    }
    void cleanup() {
        activeBatch = nullptr; activeBrowser = nullptr;
        QCoreApplication::processEvents(); responses.clear(); transfers.clear();
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
        failAllocation = 0;
#endif
    }

    void batchRequest_data() {
        QTest::addColumn<QString>("scenario");
        for (const char* scenario : {"detach", "unavailable", "stopped", "executed-error"}) QTest::newRow(scenario) << QString(scenario);
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
        QTest::newRow("request-allocation") << QString("request-allocation");
        QTest::newRow("post-allocation") << QString("post-allocation");
#endif
    }
    void batchRequest() {
        QFETCH(QString, scenario);
        const size_t before = u_get_allocated_count();
        {
            FileCore core; QVERIFY(core.ua); core.attach();
            if (scenario == "unavailable") gui_bridge_set_inst(nullptr);
            if (scenario == "stopped") uasync_mark_stopped(core.ua);
            MemberFileBatch batch(QByteArray::fromHex(TEST_FILE_GROUP), fileKey(33), fileKey(22), MemberFileBatch::Remove, nullptr); activeBatch = &batch;
            int completed = 0; bool success = true; QString summary;
            batch.progress = [](const QString&, int) {};
            batch.finished = [&](bool ok, const QString& message, const QStringList& roots) {
                completed++; success = ok; summary = message; QVERIFY(roots.isEmpty());
            };
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
            if (scenario == "request-allocation") failAllocation = 1;
            if (scenario == "post-allocation") failAllocation = 2;
#endif
            batch.start({{"selected.txt", {}, QByteArray(16, '\7'), false}});
            if (scenario == "detach") gui_bridge_set_inst(nullptr);
            if (scenario == "executed-error") uasync_poll(core.ua, 0);
            QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 1000);
            QVERIFY(!success); QVERIFY(!summary.isEmpty()); QCOMPARE(pending(core.ua), 0);
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
            QCOMPARE(failAllocation, 0);
#endif
            if (scenario == "detach" || scenario == "executed-error") {
                QCOMPARE(responses.size(), 1); mf_message message{};
                QCOMPARE(mf_decode((const uint8_t*)responses[0].constData() + MF_EVENT_RESPONSE_PREFIX, responses[0].size() - MF_EVENT_RESPONSE_PREFIX, &message), 0);
                QCOMPARE(message.status, uint16_t(scenario == "detach" ? MF_CANCELLED : MF_ACCESS_DENIED));
                QCOMPARE(message.command, uint8_t(MF_STAT)); QCOMPARE(QString::fromUtf8(message.path), QString("selected.txt"));
            } else QVERIFY(responses.isEmpty());
            batch.cancel(); QCoreApplication::processEvents(); QCOMPARE(completed, 1);
        }
        QCOMPARE(u_get_allocated_count(), before);
    }

    void batchDownload_data() { batchRequest_data(); }
    void batchDownload() {
        QFETCH(QString, scenario);
        const size_t before = u_get_allocated_count();
        {
            FileCore core; QVERIFY(core.ua); core.attach(); QTemporaryDir root; QVERIFY(root.isValid());
            if (scenario == "unavailable") gui_bridge_set_inst(nullptr);
            if (scenario == "stopped") uasync_mark_stopped(core.ua);
            MemberFileBatch batch(QByteArray::fromHex(TEST_FILE_GROUP), fileKey(33), fileKey(22), MemberFileBatch::Download, nullptr); activeBatch = &batch;
            int completed = 0; bool success = true;
            batch.progress = [](const QString&, int) {};
            batch.finished = [&](bool ok, const QString&, const QStringList& roots) {
                completed++; success = ok; QVERIFY(roots.isEmpty());
            };
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
            if (scenario == "request-allocation") failAllocation = 1;
            if (scenario == "post-allocation") failAllocation = 2;
#endif
            batch.start({{"selected.txt", {}, QByteArray(16, '\7'), false}}, root.path(), true);
            if (scenario == "detach" || scenario == "executed-error") {
                QTRY_COMPARE_WITH_TIMEOUT(pending(core.ua), 1, 1000);
                if (scenario == "detach") gui_bridge_set_inst(nullptr);
                else uasync_poll(core.ua, 0);
            }
            QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 1000); QVERIFY(!success); QCOMPARE(pending(core.ua), 0);
            QCOMPARE(QDir(root.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
            QCOMPARE(failAllocation, 0);
#endif
            if (scenario == "detach" || scenario == "executed-error") {
                QCOMPARE(transfers.size(), 1); const auto* data = (const uint8_t*)transfers[0].constData();
                QCOMPARE(transfers[0].size(), MF_EVENT_TRANSFER_SIZE); QCOMPARE(fileKeyValue(data + MF_EVENT_TRANSFER_GROUP).toHex(), QByteArray(TEST_FILE_GROUP));
                QCOMPARE(fileKeyValue(data + MF_EVENT_TRANSFER_SERVER), fileKey(33)); QCOMPARE(fileKeyValue(data + MF_EVENT_TRANSFER_OWNER), fileKey(22));
                QCOMPARE(unsigned(data[MF_EVENT_TRANSFER_STATUS]) | unsigned(data[MF_EVENT_TRANSFER_STATUS + 1]) << 8, unsigned(scenario == "detach" ? MF_CANCELLED : MF_ACCESS_DENIED));
                QCOMPARE(data[MF_EVENT_TRANSFER_STATE], uint8_t(2));
            } else QVERIFY(transfers.isEmpty());
            batch.cancel(); QCoreApplication::processEvents(); QCOMPARE(completed, 1);
        }
        QCOMPARE(u_get_allocated_count(), before);
    }

    void snapshotsSurviveCallerChangesAndDoNotUseSuppliedInstance() {
        const size_t before = u_get_allocated_count();
        {
            FileCore core; QVERIFY(core.ua); core.attach();
            mf_message message{}; message.kind = MF_REQUEST; message.command = MF_STAT;
            message.request_id = UINT64_C(0x0102030405060708); memcpy(message.owner, fileKey(22).constData(), 32); strcpy(message.path, "original.txt");
            QCOMPARE(gui_bridge_member_files_request(fileKeyData(QByteArray::fromHex(TEST_FILE_GROUP)), fileKeyData(fileKey(33)), &message), 0);
            message = {};
            mf_transfer_request transfer{}; transfer.id = 123; memcpy(transfer.group_key, QByteArray::fromHex(TEST_FILE_GROUP).constData(), 32); memcpy(transfer.server, fileKey(33).constData(), 32); memcpy(transfer.owner, fileKey(22).constData(), 32);
            transfer.inst = reinterpret_cast<UTUN_INSTANCE*>(quintptr(1)); // Bridge обязан заменить этот указатель.
            strcpy(transfer.remote, "original.txt"); strcpy(transfer.local, "/unused/local.txt");
            QCOMPARE(gui_bridge_member_files_transfer(&transfer), 0); transfer = {};
            gui_bridge_set_inst(nullptr); QCoreApplication::processEvents();
            QCOMPARE(responses.size(), 1); QCOMPARE(transfers.size(), 1); mf_message result{};
            const auto& bytes = responses[0];
            QCOMPARE(fileKeyValue((const uint8_t*)bytes.constData()).toHex(), QByteArray(TEST_FILE_GROUP));
            QCOMPARE(fileKeyValue((const uint8_t*)bytes.constData() + 32), fileKey(33));
            QCOMPARE(mf_decode((const uint8_t*)bytes.constData() + MF_EVENT_RESPONSE_PREFIX, bytes.size() - MF_EVENT_RESPONSE_PREFIX, &result), 0);
            QCOMPARE(result.request_id, UINT64_C(0x0102030405060708)); QCOMPARE(fileKeyValue(result.owner), fileKey(22));
            QCOMPARE(result.status, uint16_t(MF_CANCELLED)); QCOMPARE(QString::fromUtf8(result.path), QString("original.txt"));
            QCOMPARE(mf_get64((const uint8_t*)transfers[0].constData()), uint64_t(123));
            responses.clear(); transfers.clear(); core.attach();
            transfer.id = 124; memcpy(transfer.group_key, QByteArray::fromHex(TEST_FILE_GROUP).constData(), 32); memcpy(transfer.server, fileKey(33).constData(), 32); memcpy(transfer.owner, fileKey(22).constData(), 32); transfer.upload = 1;
            transfer.inst = reinterpret_cast<UTUN_INSTANCE*>(quintptr(1));
            strcpy(transfer.remote, "original.txt"); strcpy(transfer.local, "/unused/local.txt");
            QCOMPARE(gui_bridge_member_files_transfer(&transfer), 0);
            uasync_poll(core.ua, 0); QCoreApplication::processEvents();
            QCOMPARE(transfers.size(), 1); QCOMPARE(mf_get64((const uint8_t*)transfers[0].constData()), uint64_t(124));
            QCOMPARE(transfers[0][MF_EVENT_TRANSFER_STATE], char(2));
            QCOMPARE(transfer.inst, reinterpret_cast<UTUN_INSTANCE*>(quintptr(1)));
        }
        QCOMPARE(u_get_allocated_count(), before);
    }

    void cancelCommandsReleaseMemoryOnExecutionDetachAndRejection() {
        const size_t before = u_get_allocated_count();
        {
            FileCore core; QVERIFY(core.ua); core.attach(); const size_t baseline = u_get_allocated_count();
            QCOMPARE(gui_bridge_member_files_cancel(99), 0); uasync_poll(core.ua, 0);
            QCOMPARE(u_get_allocated_count(), baseline);
            QCOMPARE(gui_bridge_member_files_cancel(100), 0); gui_bridge_set_inst(nullptr);
            QCOMPARE(u_get_allocated_count(), baseline); QVERIFY(gui_bridge_member_files_cancel(101) < 0);
            core.attach(); QCOMPARE(gui_bridge_member_files_cancel(0), 0);
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
            for (int point : {1, 2}) {
                failAllocation = point; QVERIFY(gui_bridge_member_files_cancel(102) < 0); QCOMPARE(failAllocation, 0);
                QCOMPARE(u_get_allocated_count(), baseline); QCOMPARE(pending(core.ua), 0);
            }
#endif
            QCoreApplication::processEvents(); QVERIFY(responses.isEmpty()); QVERIFY(transfers.isEmpty());
        }
        QCOMPARE(u_get_allocated_count(), before);
    }

    void browserRejectsOrCancelsListAndCanRetry_data() { batchRequest_data(); }
    void browserRejectsOrCancelsListAndCanRetry() {
        QFETCH(QString, scenario);
        const size_t before = u_get_allocated_count();
        {
            FileCore core; QVERIFY(core.ua); core.attach(); DbManager db; sqlite3* sql = nullptr;
            QCOMPARE(sqlite3_open(":memory:", &sql), SQLITE_OK);
            const auto close = qScopeGuard([&] { db.setDb(nullptr); sqlite3_close(sql); });
            QCOMPARE(sqlite3_exec(sql, "CREATE TABLE accounts(node_id INTEGER,display_name TEXT);"
                "INSERT INTO accounts VALUES(22,'Me'),(33,'Storage');"
                "CREATE TABLE peers_" TEST_FILE_GROUP "(node_id INTEGER,adm_tags TEXT,adm_tags_sig BLOB,deleted INTEGER,source INTEGER);"
                "INSERT INTO peers_" TEST_FILE_GROUP " VALUES(22,'{}',NULL,0,0),(33,'{\"storage\":\"yes\"}',zeroblob(64),0,0);"
                "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN record_state INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN ed25519_pubkey BLOB;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN local_nick TEXT;"
            "UPDATE peers_" TEST_FILE_GROUP " SET ed25519_pubkey=CASE node_id WHEN 11 THEN x'" TEST_FILE_ED_11 "'"
            " WHEN 22 THEN x'" TEST_FILE_ED_22 "' WHEN 33 THEN x'" TEST_FILE_ED_33 "' END,"
            " local_nick=(SELECT display_name FROM accounts a WHERE a.node_id=peers_" TEST_FILE_GROUP ".node_id);",
                nullptr, nullptr, nullptr), SQLITE_OK);
            db.setDb(sql);
            MemberFileBrowser browser(&db); activeBrowser = &browser;
            if (scenario == "unavailable") gui_bridge_set_inst(nullptr);
            if (scenario == "stopped") uasync_mark_stopped(core.ua);
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
            if (scenario == "request-allocation") failAllocation = 1;
            if (scenario == "post-allocation") failAllocation = 2;
#endif
            browser.setChannel(TEST_FILE_GROUP);
            if (scenario == "detach") gui_bridge_set_inst(nullptr);
            if (scenario == "executed-error") uasync_poll(core.ua, 0);
            QCoreApplication::processEvents(); QCOMPARE(pending(core.ua), 0);
            QStringList labels; for (auto* label : browser.findChildren<QLabel*>()) labels.append(label->text());
            const QString expected = scenario == "detach" ? QString::fromUtf8(mf_status_text(MF_CANCELLED)) :
                scenario == "executed-error" ? QString::fromUtf8(mf_status_text(MF_ACCESS_DENIED)) : "Unable to request directory";
            QVERIFY2(labels.contains(expected), qPrintable(labels.join('\n')));
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
            QCOMPARE(failAllocation, 0);
#endif
            core.attach();
            QPushButton* refresh = nullptr;
            for (auto* button : browser.findChildren<QPushButton*>()) if (button->text() == "Refresh") refresh = button;
            QVERIFY(refresh); refresh->click(); QCOMPARE(pending(core.ua), 1);
            uasync_poll(core.ua, 0); QCoreApplication::processEvents(); QCOMPARE(pending(core.ua), 0);
            QVERIFY(browser.findChild<QTreeWidget*>()->isEnabled());
        }
        QCOMPARE(u_get_allocated_count(), before);
    }

    void browserSingleDownload_data() {
        QTest::addColumn<int>("failure");
        QTest::newRow("detach") << 0;
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
        QTest::newRow("request-allocation") << 1;
        QTest::newRow("post-allocation") << 2;
#endif
    }
    void browserSingleDownload() {
        QFETCH(int, failure);
        const size_t before = u_get_allocated_count();
        {
            FileCore core; QVERIFY(core.ua); core.attach(); QTemporaryDir root; QVERIFY(root.isValid());
            DbManager db; sqlite3* sql = nullptr; QCOMPARE(sqlite3_open(":memory:", &sql), SQLITE_OK);
            const auto close = qScopeGuard([&] { db.setDb(nullptr); sqlite3_close(sql); });
            QCOMPARE(sqlite3_exec(sql, "CREATE TABLE accounts(node_id INTEGER,display_name TEXT);"
                "INSERT INTO accounts VALUES(22,'Me'),(33,'Storage');"
                "CREATE TABLE peers_" TEST_FILE_GROUP "(node_id INTEGER,adm_tags TEXT,adm_tags_sig BLOB,deleted INTEGER,source INTEGER);"
                "INSERT INTO peers_" TEST_FILE_GROUP " VALUES(22,'{}',NULL,0,0),(33,'{\"storage\":\"yes\"}',zeroblob(64),0,0);"
                "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN record_state INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN ed25519_pubkey BLOB;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN local_nick TEXT;"
            "UPDATE peers_" TEST_FILE_GROUP " SET ed25519_pubkey=CASE node_id WHEN 11 THEN x'" TEST_FILE_ED_11 "'"
            " WHEN 22 THEN x'" TEST_FILE_ED_22 "' WHEN 33 THEN x'" TEST_FILE_ED_33 "' END,"
            " local_nick=(SELECT display_name FROM accounts a WHERE a.node_id=peers_" TEST_FILE_GROUP ".node_id);",
                nullptr, nullptr, nullptr), SQLITE_OK);
            db.setDb(sql);
            MemberFileBrowser browser(&db); activeBrowser = &browser; browser.setChannel(TEST_FILE_GROUP, fileKey(22));
            uasync_poll(core.ua, 0); QCoreApplication::processEvents();
            auto* tree = browser.findChild<QTreeWidget*>(); QVERIFY(tree);
            auto* item = new QTreeWidgetItem(tree, {"selected.txt"}); item->setData(0, Qt::UserRole, "selected.txt");
            item->setData(0, Qt::UserRole + 1, MF_FILE); item->setData(0, Qt::UserRole + 2, QByteArray(16, '\7'));
            tree->setCurrentItem(item); browser.show();
            QPushButton* download = nullptr; QPushButton* cancel = nullptr;
            for (auto* button : browser.findChildren<QPushButton*>()) {
                if (button->text() == "Download…") download = button;
                if (button->text() == "Cancel transfer") cancel = button;
            }
            QVERIFY(download); QVERIFY(cancel); QVERIFY(download->isEnabled());
            bool accepted = false;
            const QString destination = root.filePath("download.txt");
            QTimer::singleShot(0, [&] {
                auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                dialog->selectFile(destination); accepted = QMetaObject::invokeMethod(dialog, "accept");
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
                // Включить отказ после выбора пути, чтобы QUOTA-таймер внутри диалога не перехватил инъекцию.
                failAllocation = failure;
#endif
            });
            download->click(); QVERIFY(accepted);
            if (!failure) {
                QVERIFY(cancel->isEnabled()); QVERIFY(!tree->isEnabled());
                gui_bridge_set_inst(nullptr); QCoreApplication::processEvents();
            }
            QVERIFY(!cancel->isEnabled()); QVERIFY(tree->isEnabled()); QVERIFY(!QFile::exists(destination));
            QCOMPARE(pending(core.ua), 0);
#ifdef UTUN_TEST_FILE_ALLOC_FAILURES
            QCOMPARE(failAllocation, 0);
#endif
            core.attach(); item->setSelected(false); item->setSelected(true); QVERIFY(download->isEnabled());
            QTimer::singleShot(0, [&] {
                auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                if (dialog) { dialog->selectFile(destination); QMetaObject::invokeMethod(dialog, "accept"); }
            });
            download->click(); QVERIFY(cancel->isEnabled());
            uasync_poll(core.ua, 0); QCoreApplication::processEvents();
            QVERIFY(!cancel->isEnabled()); QVERIFY(tree->isEnabled()); QVERIFY(!QFile::exists(destination));
        }
        QCOMPARE(u_get_allocated_count(), before);
    }

    void realStorageRoundTrip() {
        const size_t before = u_get_allocated_count();
        {
            QTemporaryDir root; QVERIFY(root.isValid());
            UASYNC* ua = uasync_create(); QVERIFY(ua); UTUN_INSTANCE* instance = nullptr; QTimer poll;
            const auto cleanup = qScopeGuard([&] {
                activeBatch = nullptr; poll.stop(); gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
                uasync_mark_stopped(ua);
                if (instance) utun_instance_destroy(instance);
                uasync_destroy(ua, 0); QCoreApplication::processEvents();
            });
            const QByteArray config = "[global]\ndb_path=" + root.path().toUtf8() + "\nauto_sockets=no\ntun_enabled=no\n"
                "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
                "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
                "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=1\n"
                "local_invites_enabled=0\nlocal_broadcasts_enabled=0\nmember_files_path=" + root.path().toUtf8() +
                "/storage\n[ntp]\nenabled=no\n";
            instance = utun_instance_create_from_str(ua, config.constData()); QVERIFY(instance);
            QCOMPARE(utun_core_start(instance), 0); QCOMPARE(chat_service_start(instance), 0);
            const uint64_t group = chat_core_create_channel_auto(instance, "Files"); QVERIFY(group);
            const QByteArray channel = topo_groups_channel(instance->topo_groups, group); const uint64_t node = instance->node_id; const auto ownEd = fileKeyValue(instance->my_ed25519_pubkey);
            auto* tags = chat_member_tags_load(instance, channel.constData(), instance->my_ed25519_pubkey); QVERIFY(tags);
            const int set = chat_member_tags_set(tags, "storage", "yes");
            const int commit = set ? set : chat_member_tags_commit(instance, tags); chat_member_tags_free(tags); QCOMPARE(commit, 0);
            chat_event_set_handler(instance, [](UTUN_INSTANCE*, int event, const uint8_t* data, int size) {
                gui_bridge_post(event, data, size);
            });
            uasync_mark_running(ua); gui_bridge_set_uasync(ua); gui_bridge_set_inst(instance);
            connect(&poll, &QTimer::timeout, this, [ua] { uasync_poll(ua, 0); }); poll.start(1);
            // Дождаться ответа готового storage, не подменяя асинхронный старт его worker.
            bool ready = false;
            for (int attempt = 0; attempt < 20 && !ready; attempt++) {
                mf_message q{}; q.kind = MF_REQUEST; q.command = MF_QUOTA; q.request_id = 100 + attempt; memcpy(q.owner, instance->my_ed25519_pubkey, 32);
                const auto count = responses.size(); QCOMPARE(gui_bridge_member_files_request(fileKeyData(QByteArray::fromHex(channel)), instance->my_ed25519_pubkey, &q), 0);
                QTRY_COMPARE_WITH_TIMEOUT(responses.size(), count + 1, 3000); mf_message r{};
                const auto& bytes = responses.last();
                QCOMPARE(mf_decode((const uint8_t*)bytes.constData() + MF_EVENT_RESPONSE_PREFIX, bytes.size() - MF_EVENT_RESPONSE_PREFIX, &r), 0);
                QVERIFY(r.status == MF_OK || r.status == MF_BUSY); ready = r.status == MF_OK;
            }
            QVERIFY(ready);
            const QByteArray payload(20000, 'f'); const QString source = root.filePath("source.txt");
            { QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(payload), qint64(payload.size())); }
            int completed = 0; bool success = false; QString summary;
            auto finished = [&](bool ok, const QString& text, const QStringList&) { completed++; success = ok; summary = text; };
            const QString stored = root.filePath("storage/" + QString::fromLatin1(channel) + '/' + QString::fromLatin1(ownEd.toHex()) + "/source.txt");
            {
                MemberFileBatch batch(QByteArray::fromHex(channel), ownEd, ownEd, MemberFileBatch::Upload, nullptr); activeBatch = &batch;
                batch.progress = [](const QString&, int) {}; batch.finished = finished;
                batch.upload({source}, {}); QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 5000); QVERIFY2(success, qPrintable(summary));
            }
            { QFile file(stored); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), payload); }
            mf_message q{}; q.kind = MF_REQUEST; q.command = MF_STAT; q.request_id = 1000; memcpy(q.owner, instance->my_ed25519_pubkey, 32); strcpy(q.path, "source.txt");
            const auto count = responses.size(); QCOMPARE(gui_bridge_member_files_request(fileKeyData(QByteArray::fromHex(channel)), instance->my_ed25519_pubkey, &q), 0);
            QTRY_COMPARE_WITH_TIMEOUT(responses.size(), count + 1, 3000); mf_message info{};
            const auto bytes = responses.last();
            QCOMPARE(mf_decode((const uint8_t*)bytes.constData() + MF_EVENT_RESPONSE_PREFIX, bytes.size() - MF_EVENT_RESPONSE_PREFIX, &info), 0); QCOMPARE(info.status, uint16_t(MF_OK));
            const QString destination = root.filePath("download.txt");
            const QByteArray version((const char*)info.version, 16); completed = 0; success = false;
            {
                MemberFileBatch batch(QByteArray::fromHex(channel), ownEd, ownEd, MemberFileBatch::Download, nullptr); activeBatch = &batch;
                batch.progress = [](const QString&, int) {}; batch.finished = finished;
                batch.start({{"source.txt", destination, version, false}});
                QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 5000); QVERIFY2(success, qPrintable(summary));
            }
            { QFile file(destination); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), payload); }
            completed = 0; success = false;
            {
                MemberFileBatch batch(QByteArray::fromHex(channel), ownEd, ownEd, MemberFileBatch::Remove, nullptr); activeBatch = &batch;
                batch.progress = [](const QString&, int) {}; batch.finished = finished;
                batch.start({{"source.txt", {}, version, false}});
                QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 5000); QVERIFY2(success, qPrintable(summary));
            }
            QVERIFY(!QFile::exists(stored)); QVERIFY(QFile::exists(source)); QVERIFY(QFile::exists(destination));
        }
        QCOMPARE(u_get_allocated_count(), before);
    }
};

QTEST_MAIN(FileLifecycleTest)
#include "test_member_files_lifecycle.moc"
