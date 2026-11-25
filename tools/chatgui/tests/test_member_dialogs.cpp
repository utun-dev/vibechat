/* Реальные Qt-диалоги, ядро в отдельном uasync-потоке и SQLite: commit, отказы и lifetime результата. */
#include "../src/renamedialog.h"
#include "../src/memberpropsdialog.h"
#include "../transport/member_edit.h"
#include "../transport/gui_bridge.h"
#include "../db/db_manager.h"
#include <QtTest>
#include <QLabel>
#include <QTemporaryDir>
#include <future>
#include <thread>
#include <condition_variable>
#include <memory>
#include <cstring>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_devgroup.h"
#include "routing_layer/topo_group.h"
#include "debug_config.h"
#include "mem.h"
}

#ifdef UTUN_TEST_MEMBER_ALLOC_FAILURES
static int failMalloc, failCalloc;
extern "C" void* __real_u_malloc_impl(uint32_t, const char*);
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    if (failMalloc && strstr(where, "gui_bridge_impl.cpp") && !--failMalloc) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "[INJECT] member snapshot allocation failure"); return nullptr;
    }
    return __real_u_malloc_impl(bytes, where);
}
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (failCalloc && strstr(where, "gui_bridge_impl.cpp") && !--failCalloc) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "[INJECT] member queue allocation failure"); return nullptr;
    }
    return __real_u_calloc_impl(count, bytes, where);
}
#endif

struct MemberCore {
    QTemporaryDir directory;
    DbManager db;
    UTUN_INSTANCE* instance = nullptr;
    UASYNC* ua = nullptr;
    QString channel;
    QByteArray ed, transportKey;
    uint8_t flags = 0; bool isSelf = false;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool paused = true, active = false, stopping = false;

    void stop() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; wake.notify_all(); }
        if (worker.joinable()) worker.join();
        db.setDb(nullptr);
    }
    ~MemberCore() { stop(); }
    bool start() {
        std::promise<bool> ready; auto result = ready.get_future();
        worker = std::thread([this, &ready] {
            ua = uasync_create();
            instance = ua ? utun_instance_create_from_str(ua,
                "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
                "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
                "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n"
                "local_invites_enabled=0\nlocal_broadcasts_enabled=0\n[ntp]\nenabled=no\n") : nullptr;
            bool ok = instance && !utun_core_start(instance) && !chat_service_start(instance);
            uint64_t id = ok ? chat_core_create_channel_auto(instance, "Members") : 0;
            ok = ok && id;
            if (ok) {
                channel = QString::fromLatin1(topo_groups_channel(instance->topo_groups, id));
                ed = QByteArray(reinterpret_cast<const char*>(instance->my_ed25519_pubkey), 32);
                transportKey = QByteArray(reinterpret_cast<const char*>(instance->my_keys.public_key), 32).toHex();
                const QByteArray sql = QString("UPDATE peers_%1 SET local_nick='Old nickname' WHERE x25519_pubkey=X'%2' AND record_state=0")
                    .arg(channel, QString::fromLatin1(transportKey)).toUtf8();
                ok = channel.size() == 64 && sqlite3_exec(instance->topo_sqlite_db, sql.constData(), nullptr, nullptr, nullptr) == SQLITE_OK;
                uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE]; chat_member_display member{};
                ok = ok && !chat_core_get_single_member(instance, qPrintable(channel), instance->my_ed25519_pubkey, wire) &&
                    !chat_core_deserialize_member(wire, &member);
                flags = member.flags; isSelf = member.is_self;
                gui_bridge_set_uasync(ua); gui_bridge_set_inst(instance);
            }
            ready.set_value(ok);
            if (ok) {
                std::unique_lock<std::mutex> lock(mutex);
                while (!stopping) {
                    wake.wait(lock, [this] { return !paused || stopping; });
                    if (stopping) break;
                    active = true; lock.unlock(); uasync_poll(ua, 10); lock.lock(); active = false; wake.notify_all();
                }
            }
            gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
            if (ua) uasync_mark_stopped(ua);
            if (instance) utun_instance_destroy(instance);
            if (ua) uasync_destroy(ua, 0);
        });
        if (!result.get()) return false;
        db.setDb(instance->topo_sqlite_db); return true;
    }
    void resume() { std::lock_guard<std::mutex> lock(mutex); paused = false; wake.notify_all(); }
    void pause() { std::unique_lock<std::mutex> lock(mutex); paused = true; wake.wait(lock, [this] { return !active; }); }
    int run(std::function<int()> function) {
        auto task = std::make_unique<std::packaged_task<int()>>(std::move(function)); auto result = task->get_future();
        gui_bridge_post_uasync_fn([](void* arg) {
            std::unique_ptr<std::packaged_task<int()>> task(static_cast<std::packaged_task<int()>*>(arg)); (*task)();
        }, task.release());
        resume();
        if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) qFatal("Member test core command timed out");
        int value = result.get(); pause(); return value;
    }
    QByteArray snapshot() {
        sqlite3_stmt* st = nullptr;
        QByteArray sql = QString("SELECT coalesce(local_nick,''),coalesce(adm_tags,''),hex(adm_tags_sig) FROM peers_%1 "
            "WHERE x25519_pubkey=X'%2' AND record_state=0").arg(channel, QString::fromLatin1(transportKey)).toUtf8();
        if (sqlite3_prepare_v2(db.m_db, sql.constData(), -1, &st, nullptr) != SQLITE_OK) qFatal("Snapshot prepare failed");
        QByteArray result;
        if (sqlite3_step(st) == SQLITE_ROW) {
            for (int i = 0; i < 3; ++i) { result += DbManager::colText(st, i).toUtf8(); result += '\n'; }
        }
        if (sqlite3_finalize(st) != SQLITE_OK) qFatal("Snapshot finalize failed");
        return result;
    }
};

static std::unique_ptr<QDialog> makeDialog(MemberCore& core, bool properties) {
    if (properties) return std::make_unique<MemberPropsDialog>(&core.db, core.channel, core.ed, "Member", core.flags, core.isSelf);
    return std::make_unique<RenameDialog>(&core.db, core.channel, core.ed, "Member");
}
static QLabel* errorLabel(QDialog* dialog) { return dialog->findChild<QLabel*>("memberSaveError"); }
static bool save(QDialog* dialog) { return QMetaObject::invokeMethod(dialog, "onSave", Qt::DirectConnection); }

class MemberDialogTest : public QObject {
    Q_OBJECT
    size_t allocationBaseline = 0;
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG); gui_bridge_init();
    }
    void init() { allocationBaseline = u_get_allocated_count(); }
    void cleanup() { QCoreApplication::processEvents(); QCOMPARE(u_get_allocated_count(), allocationBaseline); }
    void deviceActionMatchesSelfAndDisconnectsCommittedGroup() {
        MemberCore core; QVERIFY(core.start());
        QVERIFY(!core.run([&] { return chat_devgroup_rename(core.instance, "Devices"); }));
        auto dialog = makeDialog(core, true);
        auto* button = dialog->findChild<QPushButton*>("memberAddMyDevice"); QVERIFY(button && button->isEnabled());
        QCOMPARE(button->text(), QString("Disconnect from my devices")); button->click(); QVERIFY(!button->isEnabled());
        QCOMPARE(core.run([&] {
            devgroup_record record; uint8_t ed[32], x[32]; return devgroup_load_local(core.instance, &record, ed, x);
        }), 0);
        QCoreApplication::processEvents();
        auto ungrouped = makeDialog(core, true);
        QVERIFY(!ungrouped->findChild<QPushButton*>("memberAddMyDevice")->isEnabled());
        core.isSelf = false; auto other = makeDialog(core, true);
        QVERIFY(!other->findChild<QPushButton*>("memberAddMyDevice"));
    }

    void saves_data() {
        QTest::addColumn<bool>("properties"); QTest::addColumn<QString>("failure");
        for (bool properties : {false, true}) for (const char* failure : {"none", "prepare", "step", "commit", "missing", "changed-key"})
            QTest::newRow(qPrintable(QString("%1-%2").arg(properties ? "properties" : "rename", failure))) << properties << QString(failure);
    }
    void saves() {
        QFETCH(bool, properties); QFETCH(QString, failure);
        MemberCore core; QVERIFY(core.start()); auto dialog = makeDialog(core, properties); auto before = core.snapshot();
        struct Probe { bool deny; int updates = 0; bool gui = false, memberTransaction = false; int commitRefusals = 0; } probe{failure == "prepare"};
        QCOMPARE(core.run([&] {
            if (failure == "step") {
                QByteArray sql = QString("CREATE TRIGGER refuse_nick BEFORE UPDATE OF local_nick ON peers_%1 "
                    "BEGIN SELECT RAISE(ABORT,'injected nickname failure'); END").arg(core.channel).toUtf8();
                if (sqlite3_exec(core.db.m_db, sql.constData(), nullptr, nullptr, nullptr) != SQLITE_OK) return -1;
            }
            if (failure == "changed-key") {
                QByteArray sql = QString("UPDATE peers_%1 SET ed25519_pubkey=randomblob(32) WHERE x25519_pubkey=X'%2' AND record_state=0")
                    .arg(core.channel, QString::fromLatin1(core.transportKey)).toUtf8();
                if (sqlite3_exec(core.db.m_db, sql.constData(), nullptr, nullptr, nullptr) != SQLITE_OK) return -1;
            }
            if (failure == "missing") {
                QByteArray sql = QString("DELETE FROM peers_%1").arg(core.channel).toUtf8();
                if (sqlite3_exec(core.db.m_db, sql.constData(), nullptr, nullptr, nullptr) != SQLITE_OK) return -1;
            }
            if (failure == "commit") {
                sqlite3_trace_v2(core.db.m_db, SQLITE_TRACE_STMT, [](unsigned, void* arg, void* statement, void*) {
                    auto& probe = *static_cast<Probe*>(arg); const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
                    if (sql && !strncmp(sql, "BEGIN", 5)) probe.memberTransaction =
                        !strcmp(sql, "BEGIN IMMEDIATE /* member-save */");
                    return 0;
                }, &probe);
                sqlite3_commit_hook(core.db.m_db, [](void* arg) {
                    auto& probe = *static_cast<Probe*>(arg);
                    if (!probe.memberTransaction) return 0;
                    probe.memberTransaction = false; ++probe.commitRefusals;
                    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "[INJECT] member commit refused"); return 1;
                }, &probe);
            }
            return sqlite3_set_authorizer(core.db.m_db, [](void* arg, int action, const char*, const char* column, const char*, const char*) {
                auto& probe = *static_cast<Probe*>(arg);
                if (action != SQLITE_UPDATE || !column || strcmp(column, "local_nick")) return SQLITE_OK;
                ++probe.updates; probe.gui |= QThread::currentThread() == qApp->thread();
                DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "[PROBE] nickname UPDATE gui_thread=%d deny=%d", probe.gui, probe.deny);
                return probe.deny ? SQLITE_DENY : SQLITE_OK;
            }, &probe);
        }), SQLITE_OK);
        if (failure == "missing") before = core.snapshot();
        dialog->findChild<QLineEdit*>()->setText(QString::fromUtf8("  Новый ник  "));
        QSignalSpy accepted(dialog.get(), &QDialog::accepted);
        QVERIFY(save(dialog.get())); QVERIFY(save(dialog.get())); QCOMPARE(accepted.count(), 0);
        QCOMPARE(core.snapshot(), before); // Ядро приостановлено: GUI ничего не записал.
        core.resume();
        if (failure == "none") QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 1, 3000);
        else QTRY_VERIFY_WITH_TIMEOUT(!errorLabel(dialog.get())->text().isEmpty(), 3000);
        core.pause(); QVERIFY(!probe.gui);
        QCOMPARE(probe.updates, (failure == "missing" || failure == "changed-key") && properties ? 0 : 1);
        if (failure == "commit") QCOMPARE(probe.commitRefusals, 1);
        if (failure == "none") QVERIFY(core.snapshot().startsWith(QString::fromUtf8("Новый ник\n").toUtf8()));
        else {
            QCOMPARE(accepted.count(), 0); QCOMPARE(core.snapshot(), before);
            QVERIFY(dialog->findChild<QLineEdit*>()->text().contains(QString::fromUtf8("Новый ник")));
            QVERIFY(sqlite3_get_autocommit(core.db.m_db));
        }
        QVERIFY(sqlite3_next_stmt(core.db.m_db, nullptr) == nullptr);
        QCOMPARE(core.run([&] {
            sqlite3_set_authorizer(core.db.m_db, nullptr, nullptr); sqlite3_commit_hook(core.db.m_db, nullptr, nullptr);
            sqlite3_trace_v2(core.db.m_db, 0, nullptr, nullptr);
            return sqlite3_exec(core.db.m_db, "DROP TRIGGER IF EXISTS refuse_nick", nullptr, nullptr, nullptr);
        }), SQLITE_OK);
        if (failure != "none" && failure != "missing" && failure != "changed-key") {
            QVERIFY(save(dialog.get())); core.resume(); QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 1, 3000); core.pause();
            QVERIFY(core.snapshot().startsWith(QString::fromUtf8("Новый ник\n").toUtf8()));
        }
    }
    void lifetime_data() {
        QTest::addColumn<bool>("properties"); QTest::addColumn<QString>("mode");
        for (bool properties : {false, true}) for (const char* mode : {"close", "destroy", "stop", "disconnected", "other-db"})
            QTest::newRow(qPrintable(QString("%1-%2").arg(properties ? "properties" : "rename", mode))) << properties << QString(mode);
    }
    void lifetime() {
        QFETCH(bool, properties); QFETCH(QString, mode);
        MemberCore core; QVERIFY(core.start()); auto dialog = makeDialog(core, properties);
        dialog->findChild<QLineEdit*>()->setText("Later nickname"); QSignalSpy accepted(dialog.get(), &QDialog::accepted);
        sqlite3* other = nullptr;
        if (mode == "disconnected") core.db.setDb(nullptr);
        if (mode == "other-db") { QCOMPARE(sqlite3_open(":memory:", &other), SQLITE_OK); core.db.setDb(other); }
        QVERIFY(save(dialog.get())); QCOMPARE(accepted.count(), 0);
        if (mode == "destroy") dialog.reset();
        else if (mode == "close") dialog->reject();
        if (mode == "stop") core.stop();
        else if (mode != "disconnected") { core.run([] { return 0; }); }
        QCoreApplication::processEvents();
        if (mode == "close" || mode == "destroy") {
            QVERIFY(core.snapshot().startsWith("Later nickname\n")); QCOMPARE(accepted.count(), 0);
            if (dialog) QCOMPARE(dialog->result(), int(QDialog::Rejected));
        } else { QCOMPARE(accepted.count(), 0); QVERIFY(!errorLabel(dialog.get())->text().isEmpty()); }
        if (other) { core.db.setDb(core.instance->topo_sqlite_db); QCOMPARE(sqlite3_close(other), SQLITE_OK); }
    }
    void failedRead_data() {
        QTest::addColumn<bool>("properties");
        QTest::newRow("rename") << false; QTest::newRow("properties") << true;
    }
    void failedRead() {
        QFETCH(bool, properties); MemberCore core; QVERIFY(core.start()); const auto before = core.snapshot();
        QCOMPARE(core.run([&] { return sqlite3_set_authorizer(core.db.m_db,
            [](void*, int action, const char*, const char* column, const char*, const char*) {
                if (action == SQLITE_READ && column && !strcmp(column, "local_nick")) {
                    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "[INJECT] initial nickname read denied"); return SQLITE_DENY;
                }
                return SQLITE_OK;
            }, nullptr); }), SQLITE_OK);
        auto dialog = makeDialog(core, properties); QSignalSpy accepted(dialog.get(), &QDialog::accepted);
        QCOMPARE(core.run([&] { return sqlite3_set_authorizer(core.db.m_db, nullptr, nullptr); }), SQLITE_OK);
        QVERIFY(save(dialog.get())); QCOMPARE(accepted.count(), 0); QVERIFY(!errorLabel(dialog.get())->text().isEmpty());
        QCOMPARE(core.snapshot(), before);
    }
    void staleOwnerAndUnchangedOwner() {
        MemberCore core; QVERIFY(core.start()); auto dialog = makeDialog(core, true);
        QCOMPARE(core.run([&] { return chat_core_save_member(core.instance, qPrintable(core.channel), core.instance->my_ed25519_pubkey,
            "Newer nickname", "{\"owner_test\":\"newer\"}"); }), 0);
        const auto before = core.snapshot(); QSignalSpy accepted(dialog.get(), &QDialog::accepted);
        dialog->findChild<QLineEdit*>()->setText("Stale nickname"); QVERIFY(save(dialog.get())); core.resume();
        QTRY_VERIFY_WITH_TIMEOUT(!errorLabel(dialog.get())->text().isEmpty(), 3000); core.pause();
        QCOMPARE(accepted.count(), 0); QCOMPARE(core.snapshot(), before);
        int result = -99; QObject context;
        const QByteArray ed(reinterpret_cast<const char*>(core.instance->my_ed25519_pubkey), 32);
        QVERIFY(gui_bridge_save_member(core.db.m_db, core.channel, ed, "", "{}", &context, [&](int rc) { result = rc; }));
        core.resume(); QTRY_COMPARE_WITH_TIMEOUT(result, 0, 3000); core.pause();
        QVERIFY(core.snapshot().startsWith('\n')); // No-op owner-блок не мешает снятию локального ника.
    }
    void validation() {
        MemberCore core; QVERIFY(core.start()); QObject context; bool called = false;
        const auto callback = [&](int) { called = true; };
        const QByteArray ed(reinterpret_cast<const char*>(core.instance->my_ed25519_pubkey), 32);
        QVERIFY(!gui_bridge_save_member(core.db.m_db, "01", ed, "Nick", {}, &context, callback));
        QVERIFY(!gui_bridge_save_member(core.db.m_db, core.channel, QByteArray(), "Nick", {}, &context, callback));
        QVERIFY(!gui_bridge_save_member(core.db.m_db, core.channel, ed, QString(QChar(0)), {}, &context, callback));
        QVERIFY(!gui_bridge_save_member(core.db.m_db, core.channel, ed, "Nick", QByteArray(8193, 'x'), &context, callback));
        QVERIFY(!gui_bridge_save_member(core.db.m_db, core.channel, ed, "Nick", {}, nullptr, callback));
        QCoreApplication::processEvents(); QVERIFY(!called);
    }
    void roleSnapshot() {
        MemberCore core; QVERIFY(core.start());
        MemberPropsDialog dialog(&core.db, core.channel, core.ed, "Member",
            core.flags ^ (CHAT_MEMBER_FLAG_ADMIN | CHAT_MEMBER_FLAG_SUPERNODE), core.isSelf);
        const auto boxes = dialog.findChildren<QCheckBox*>(); QVERIFY(boxes.size() >= 2);
        QCOMPARE(boxes[0]->isChecked(), bool(core.flags & CHAT_MEMBER_FLAG_ADMIN));
        QCOMPARE(boxes[1]->isChecked(), bool(core.flags & CHAT_MEMBER_FLAG_SUPERNODE));
    }
    void missingSelectedKey() {
        MemberCore core; QVERIFY(core.start());
        const auto before = core.snapshot();
        core.ed[31] ^= 1;
        for (bool properties : {false, true}) {
            auto dialog = makeDialog(core, properties);
            QVERIFY(!errorLabel(dialog.get())->text().isEmpty());
            QSignalSpy accepted(dialog.get(), &QDialog::accepted);
            QVERIFY(save(dialog.get()));
            QCOMPARE(accepted.count(), 0);
        }
        QCOMPARE(core.snapshot(), before);
    }
    void transferOwnership() {
        MemberCore core; QVERIFY(core.start());
        const QByteArray ed(reinterpret_cast<const char*>(core.instance->my_ed25519_pubkey), 32);
        QVERIFY(!gui_bridge_transfer_admin(nullptr, core.channel, ed));
        QVERIFY(!gui_bridge_transfer_admin(core.db.m_db, "01", ed));
#ifdef UTUN_TEST_MEMBER_ALLOC_FAILURES
        for (int allocation : {1, 2}) {
            failCalloc = allocation; QVERIFY(!gui_bridge_transfer_admin(core.db.m_db, core.channel, ed));
            QCOMPARE(failCalloc, 0);
        }
#endif
        QVERIFY(gui_bridge_transfer_admin(core.db.m_db, core.channel, ed));
        core.stop(); QCoreApplication::processEvents(); // Owned transfer освобождается при отмене, не отправляя ключ.
    }
#ifdef UTUN_TEST_MEMBER_ALLOC_FAILURES
    void allocations_data() {
        QTest::addColumn<bool>("properties"); QTest::addColumn<bool>("queue");
        for (bool properties : {false, true}) for (bool queue : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(properties).arg(queue))) << properties << queue;
    }
    void allocations() {
        QFETCH(bool, properties); QFETCH(bool, queue);
        MemberCore core; QVERIFY(core.start()); auto dialog = makeDialog(core, properties);
        auto before = core.snapshot(); QSignalSpy accepted(dialog.get(), &QDialog::accepted);
        failMalloc = !queue; failCalloc = queue; QVERIFY(save(dialog.get()));
        QCOMPARE(failMalloc, 0); QCOMPARE(failCalloc, 0); QCOMPARE(accepted.count(), 0); QCOMPARE(core.snapshot(), before);
        QVERIFY(!errorLabel(dialog.get())->text().isEmpty());
        QVERIFY(save(dialog.get())); core.resume(); QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 1, 3000); core.pause();
    }
#endif
};
QTEST_MAIN(MemberDialogTest)
#include "test_member_dialogs.moc"
