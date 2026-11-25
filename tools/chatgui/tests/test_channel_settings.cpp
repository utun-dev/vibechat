/* Настоящие Qt-диалоги/SQLite/bridge: отказы очереди и владение вложенными окнами. */
#include "../src/channelsettingsdialog.h"
#include "../src/avatars/groupavatarmanager.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include <QtTest>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QLabel>
#include <QPointer>
#include <QScopeGuard>
#include <QBuffer>
#include <QLineEdit>
#include <QDialogButtonBox>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "chat/group_avatar.h"
#include "chat/group_info.h"
#include "mem.h"
#include "debug_config.h"
}

#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
static int failAllocation;
static const char* allocationFile;
static bool rejectAllocation(const char* where) {
    if (!failAllocation || !strstr(where, allocationFile) || --failAllocation) return false;
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "[INJECT] channel settings allocation failure at %s", where); return true;
}
extern "C" void* __real_u_malloc_impl(uint32_t, const char*);
extern "C" void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    return rejectAllocation(where) ? nullptr : __real_u_malloc_impl(bytes, where);
}
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    return rejectAllocation(where) ? nullptr : __real_u_calloc_impl(count, bytes, where);
}
#endif

static QByteArray avatarResult;

struct SettingsCore {
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE* inst = nullptr;
    DbManager db;
    QString channel;
    ~SettingsCore() {
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        if (ua) uasync_mark_stopped(ua);
        db.setDb(nullptr);
        if (inst) utun_instance_destroy(inst);
        if (ua) uasync_destroy(ua, 0);
    }
    bool start(bool attach = true) {
        if (!ua) return false;
        const char* config = "[global]\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n";
        inst = utun_instance_create_from_str(ua, config);
        if (!inst || utun_core_start(inst) || chat_service_start(inst)) return false;
        chat_core_create_channel_auto(inst, "Settings");
        db.setDb(inst->topo_sqlite_db);
        if (db.getChannels().size() != 1) return false;
        channel = db.getChannels()[0].channelId;
        if (attach) { gui_bridge_set_uasync(ua); gui_bridge_set_inst(inst); }
        return true;
    }
};

class ChannelSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); gui_bridge_init();
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }

    void pendingSaveIsOwned() {
        SettingsCore core; QVERIFY(core.start());
        ChannelSettingsDialog dialog(core.channel, &core.db);
        const auto baseline = u_get_allocated_count();
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onSave", Qt::DirectConnection));
        QCOMPARE(accepted.count(), 1);
        gui_bridge_set_inst(nullptr);
        QCOMPARE(u_get_allocated_count(), baseline);
    }

    void savesBothValues() {
        SettingsCore core; QVERIFY(core.start());
        ChannelSettingsDialog dialog(core.channel, &core.db);
        const auto checks = dialog.findChildren<QCheckBox*>(); QCOMPARE(checks.size(), 2);
        QVERIFY(!checks[0]->isChecked()); QVERIFY(checks[1]->isChecked());
        checks[0]->setChecked(true); checks[1]->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onSave", Qt::DirectConnection));
        uasync_poll(core.ua, 0);
        QCOMPARE(core.db.getUiStateInt("ch_" + core.channel + "_auto_play_voice", -1), 1);
        QCOMPARE(core.db.getUiStateInt("ch_" + core.channel + "_sound_on_message", -1), 0);
    }

    void renameWaitsForCommitAndPreservesDraftOnConflict() {
        SettingsCore core; QVERIFY(core.start());
        uasync_mark_running(core.ua);
        chat_event_set_handler(core.inst, [](UTUN_INSTANCE*, int type, const uint8_t* data, int length) {
            gui_bridge_post(type, data, length);
        });
        ChannelSettingsDialog settings(core.channel, &core.db);
        settings.findChild<QPushButton*>("renameGroup")->click();
        auto* dialog = settings.findChild<QDialog*>("groupRenameDialog"); QVERIFY(dialog);
        auto* name = dialog->findChild<QLineEdit*>("groupName"); QVERIFY(name);
        auto* buttons = dialog->findChild<QDialogButtonBox*>(); QVERIFY(buttons);
        auto* save = buttons->button(QDialogButtonBox::Save);
        QVERIFY(!save->isEnabled() && name->isReadOnly());
        QTRY_VERIFY(([&] { uasync_poll(core.ua, 0); return name->text() == "Settings"; })());
        name->setText(QString::fromUtf8("Новое имя 😀")); QVERIFY(save->isEnabled()); save->click();
        QCOMPARE(core.db.getChannels()[0].name, QString("Settings")); QVERIFY(!save->isEnabled());
        QTRY_VERIFY(([&] { uasync_poll(core.ua, 0); return core.db.getChannels()[0].name == name->text(); })());
        QCoreApplication::processEvents();
        name->setText("My draft"); QVERIFY(save->isEnabled());
        char* result = nullptr;
        QCOMPARE(group_info_command(core.inst, qPrintable(core.channel), "Remote name", nullptr, -1, nullptr, &result), 0); u_free(result);
        save->click();
        QTRY_VERIFY(([&] { uasync_poll(core.ua, 0); return save->isEnabled(); })());
        QCOMPARE(name->text(), QString("My draft")); QCOMPARE(core.db.getChannels()[0].name, QString("Remote name"));
        save->click();
        QTRY_VERIFY(([&] { uasync_poll(core.ua, 0); return core.db.getChannels()[0].name == "My draft"; })());
        delete dialog;
    }

    void renameWithoutGroupKeyIsReadOnly() {
        SettingsCore core; QVERIFY(core.start());
        uasync_mark_running(core.ua);
        QCOMPARE(sqlite3_exec(core.inst->topo_sqlite_db, "UPDATE channels SET ed25519_privkey=NULL", nullptr, nullptr, nullptr), SQLITE_OK);
        chat_event_set_handler(core.inst, [](UTUN_INSTANCE*, int type, const uint8_t* data, int length) {
            gui_bridge_post(type, data, length);
        });
        ChannelSettingsDialog settings(core.channel, &core.db);
        settings.findChild<QPushButton*>("renameGroup")->click();
        auto* dialog = settings.findChild<QDialog*>("groupRenameDialog"); QVERIFY(dialog);
        auto* name = dialog->findChild<QLineEdit*>("groupName"); QVERIFY(name);
        QTRY_VERIFY(([&] { uasync_poll(core.ua, 0); return name->text() == "Settings"; })());
        QVERIFY(name->isReadOnly());
        QVERIFY(!dialog->findChild<QCheckBox*>("allowMemberClipboard")->isEnabled());
        QVERIFY(!dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
        delete dialog;
    }

    void clipboardPermissionCommitsAndSurvivesRename() {
        SettingsCore core; QVERIFY(core.start()); uasync_mark_running(core.ua);
        chat_event_set_handler(core.inst, [](UTUN_INSTANCE*, int type, const uint8_t* data, int length) { gui_bridge_post(type, data, length); });
        ChannelSettingsDialog settings(core.channel, &core.db); settings.findChild<QPushButton*>("renameGroup")->click();
        auto* dialog = settings.findChild<QDialog*>("groupRenameDialog"); QVERIFY(dialog);
        auto* flag = dialog->findChild<QCheckBox*>("allowMemberClipboard"); QVERIFY(flag);
        auto* save = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        QTRY_VERIFY(([&] { uasync_poll(core.ua, 0); return flag->isEnabled(); })());
        QVERIFY(!flag->isChecked()); QVERIFY(!core.db.clipboardAllowed(core.channel));
        flag->setChecked(true); QVERIFY(save->isEnabled()); save->click();
        QVERIFY(!core.db.clipboardAllowed(core.channel));
        QTRY_VERIFY(([&] { uasync_poll(core.ua, 0); return core.db.clipboardAllowed(core.channel); })());
        QCoreApplication::processEvents(); auto* name = dialog->findChild<QLineEdit*>("groupName"); name->setText("New name");
        QVERIFY(save->isEnabled()); save->click();
        QTRY_VERIFY(([&] { uasync_poll(core.ua, 0); return core.db.getChannels()[0].name == "New name"; })());
        QVERIFY(core.db.clipboardAllowed(core.channel)); QCoreApplication::processEvents();
        flag->setChecked(false); QVERIFY(save->isEnabled()); save->click();
        QTRY_VERIFY(([&] { uasync_poll(core.ua, 0); return !core.db.clipboardAllowed(core.channel); })()); delete dialog;
    }

    void stoppedCoreKeepsDialogOpen() {
        SettingsCore core; QVERIFY(core.start(false));
        ChannelSettingsDialog dialog(core.channel, &core.db);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onSave", Qt::DirectConnection));
        QVERIFY(accepted.isEmpty());
        auto* status = dialog.findChild<QLabel*>("channelSettingsStatus"); QVERIFY(status && !status->text().isEmpty());
    }

#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
    void allocationFailure_data() {
        QTest::addColumn<QByteArray>("source"); QTest::addColumn<int>("position");
        for (const auto* source : {"channelsettingsdialog.cpp", "gui_bridge_impl.cpp"})
            for (int position : {1, 2}) QTest::newRow(qPrintable(QString("%1-%2").arg(source).arg(position))) << QByteArray(source) << position;
    }
    void allocationFailure() {
        QFETCH(QByteArray, source); QFETCH(int, position);
        SettingsCore core; QVERIFY(core.start());
        ChannelSettingsDialog dialog(core.channel, &core.db);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        const auto baseline = u_get_allocated_count();
        allocationFile = source.constData(); failAllocation = position;
        auto reset = qScopeGuard([] { failAllocation = 0; allocationFile = nullptr; });
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onSave", Qt::DirectConnection));
        QCOMPARE(failAllocation, 0); QVERIFY(accepted.isEmpty());
        gui_bridge_set_inst(nullptr); QCOMPARE(u_get_allocated_count(), baseline);
        auto* status = dialog.findChild<QLabel*>("channelSettingsStatus"); QVERIFY(status && !status->text().isEmpty());
    }

    void retryAfterPartialQueueAcceptance() {
        SettingsCore core; QVERIFY(core.start());
        ChannelSettingsDialog dialog(core.channel, &core.db);
        const auto checks = dialog.findChildren<QCheckBox*>(); QCOMPARE(checks.size(), 2);
        checks[0]->setChecked(true); checks[1]->setChecked(false);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        allocationFile = "gui_bridge_impl.cpp"; failAllocation = 2;
        auto reset = qScopeGuard([] { failAllocation = 0; allocationFile = nullptr; });
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onSave", Qt::DirectConnection));
        QCOMPARE(failAllocation, 0); QVERIFY(accepted.isEmpty());
        uasync_poll(core.ua, 0);
        QCOMPARE(core.db.getUiStateInt("ch_" + core.channel + "_auto_play_voice", -1), 1);
        QCOMPARE(chat_core_get_sound_on_message(core.inst, qPrintable(core.channel)), 1);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onSave", Qt::DirectConnection));
        QCOMPARE(accepted.count(), 1); uasync_poll(core.ua, 0);
        QCOMPARE(core.db.getUiStateInt("ch_" + core.channel + "_auto_play_voice", -1), 1);
        QCOMPARE(chat_core_get_sound_on_message(core.inst, qPrintable(core.channel)), 0);
    }
#endif

    void invalidChannel_data() {
        QTest::addColumn<QString>("channel");
        for (const auto* value : {"", "0", "01", "+1", "1 ", "dm:1", "9223372036854775808", "18446744073709551615", "18446744073709551616"})
            QTest::newRow(value) << QString::fromLatin1(value);
        QTest::newRow("embedded-nul") << QString::fromLatin1("1\0tail", 6);
    }
    void invalidChannel() {
        QFETCH(QString, channel);
        SettingsCore core; QVERIFY(core.start());
        ChannelSettingsDialog dialog(channel, &core.db);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QVERIFY(!dialog.findChild<QPushButton*>("deleteChannel")->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onSave", Qt::DirectConnection));
        QVERIFY(accepted.isEmpty());
    }

    void borrowedDbDestruction() {
        SettingsCore core; QVERIFY(core.start());
        auto* db = new DbManager; db->setDb(core.inst->topo_sqlite_db);
        ChannelSettingsDialog dialog(core.channel, db);
        QSignalSpy rejected(&dialog, &QDialog::rejected); delete db;
        QCOMPARE(rejected.count(), 1);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onSave", Qt::DirectConnection)); QVERIFY(accepted.isEmpty());
    }

    void confirmationSurvivesOwnerDeletion() {
        SettingsCore core; QVERIFY(core.start());
        QPointer<ChannelSettingsDialog> dialog = new ChannelSettingsDialog(core.channel, &core.db);
        dialog->show();
        QTimer::singleShot(0, [&] { QVERIFY(qobject_cast<QMessageBox*>(QApplication::activeModalWidget())); delete dialog.data(); });
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "onDelete", Qt::DirectConnection));
        QVERIFY(!dialog); QCOMPARE(core.db.getChannels().size(), 1);
    }

    void confirmationOutcome_data() {
        QTest::addColumn<int>("change");
        QTest::newRow("confirm-owned-request") << 0;
        QTest::newRow("cancel") << 1;
        QTest::newRow("removed-during-confirmation") << 2;
        QTest::newRow("core-stopped") << 3;
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        QTest::newRow("queue-rejected") << 4;
#endif
    }
    void confirmationOutcome() {
        QFETCH(int, change);
        SettingsCore core; QVERIFY(core.start());
        ChannelSettingsDialog dialog(core.channel, &core.db); dialog.show();
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        const auto baseline = u_get_allocated_count();
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        auto reset = qScopeGuard([] { failAllocation = 0; allocationFile = nullptr; });
#endif
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); QVERIFY(box);
            if (change == 1) { box->button(QMessageBox::Cancel)->click(); return; }
            if (change == 2) chat_core_delete_channel(core.inst, qPrintable(core.channel));
            if (change == 3) gui_bridge_set_inst(nullptr);
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
            if (change == 4) { allocationFile = "gui_bridge_impl.cpp"; failAllocation = 1; }
#endif
            auto* button = box->findChild<QPushButton*>("confirmDeleteChannel"); QVERIFY(button); button->click();
        });
        QVERIFY(QMetaObject::invokeMethod(&dialog, "onDelete", Qt::DirectConnection));
        QCOMPARE(accepted.count(), int(change == 0));
        gui_bridge_set_inst(nullptr);
        if (change != 2) QCOMPARE(u_get_allocated_count(), baseline);
        QCOMPARE(core.db.getChannels().size(), change == 2 ? 0 : 1);
    }

    void avatarChooserSurvivesOwnerDeletion() {
        SettingsCore core; QVERIFY(core.start()); GroupAvatarManager avatars(&core.db);
        QPointer<ChannelSettingsDialog> dialog = new ChannelSettingsDialog(core.channel, &core.db);
        dialog->show(); auto* upload = dialog->findChild<QPushButton*>("uploadGroupAvatar"); QVERIFY(upload && upload->isEnabled());
        QTimer::singleShot(0, [&] { QVERIFY(qobject_cast<QFileDialog*>(QApplication::activeModalWidget())); delete dialog.data(); });
        upload->click(); QVERIFY(!dialog);
    }

    void avatarManagerDisappearsDuringChoice() {
        SettingsCore core; QVERIFY(core.start());
        QPointer<GroupAvatarManager> avatars = new GroupAvatarManager(&core.db);
        auto release = qScopeGuard([&] { delete avatars.data(); });
        ChannelSettingsDialog dialog(core.channel, &core.db); dialog.show();
        auto* upload = dialog.findChild<QPushButton*>("uploadGroupAvatar"); QVERIFY(upload && upload->isEnabled());
        QTimer::singleShot(0, [&] { QVERIFY(qobject_cast<QFileDialog*>(QApplication::activeModalWidget())); delete avatars.data(); });
        upload->click(); QVERIFY(!avatars); QVERIFY(!upload->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton*>("removeGroupAvatar")->isEnabled());
    }

    void avatarPublicationIsOwned() {
        SettingsCore core; QVERIFY(core.start()); GroupAvatarManager avatars(&core.db);
        const auto baseline = u_get_allocated_count();
        avatars.remove(core.channel);
        QVERIFY(u_get_allocated_count() > baseline);
        gui_bridge_set_inst(nullptr); QCOMPARE(u_get_allocated_count(), baseline);
    }

    void invalidAvatarReplyKeepsPendingRequest() {
        SettingsCore core; QVERIFY(core.start()); GroupAvatarManager avatars(&core.db);
        QSignalSpy statuses(&avatars, &GroupAvatarManager::status);
        avatarResult.clear();
        chat_event_set_handler(core.inst, [](UTUN_INSTANCE*, int type, const uint8_t* data, int length) {
            if (type == CHAT_EVT_GROUP_AVATAR_RESULT) avatarResult = QByteArray((const char*)data, length);
        });
        avatars.remove(core.channel); QCOMPARE(statuses.count(), 1);
        uasync_poll(core.ua, 0); QCOMPARE(avatarResult.size(), 41);
        QByteArray wrong = avatarResult; wrong[8] = char(uint8_t(wrong[8]) ^ 1);
        avatars.onResult((const uint8_t*)wrong.constData(), wrong.size()); QCOMPARE(statuses.count(), 1);
        wrong = avatarResult; wrong[40] = 2;
        avatars.onResult((const uint8_t*)wrong.constData(), wrong.size()); QCOMPARE(statuses.count(), 1);
        avatars.onResult((const uint8_t*)avatarResult.constData(), avatarResult.size());
        QCOMPARE(statuses.count(), 2); QCOMPARE(statuses.last()[1].toString(), QString("Group avatar updated"));
        avatars.onResult((const uint8_t*)avatarResult.constData(), avatarResult.size()); QCOMPARE(statuses.count(), 2);
    }

    void avatarNullReplyAndBorrowedDb() {
        SettingsCore core; QVERIFY(core.start());
        auto* db = new DbManager; db->setDb(core.inst->topo_sqlite_db);
        GroupAvatarManager avatars(db); delete db;
        avatars.onResult(nullptr, 41); avatars.onResult(nullptr, 0);
        QVERIFY(avatars.image(core.channel).isNull());
    }

    void preparingSignalCanDeleteManager() {
        SettingsCore core; QVERIFY(core.start());
        QPointer<GroupAvatarManager> avatars = new GroupAvatarManager(&core.db);
        connect(avatars, &GroupAvatarManager::status, this, [&](const QString&, const QString&) { delete avatars.data(); });
        avatars->upload(core.channel, QStringLiteral("/nonexistent/avatar.png"));
        QVERIFY(!avatars); QVERIFY(!GroupAvatarManager::instance());
        QCoreApplication::processEvents();
    }

    void oldAvatarOwnerDoesNotClearCurrentOwner() {
        SettingsCore core; QVERIFY(core.start());
        auto* old = new GroupAvatarManager(&core.db);
        GroupAvatarManager current(&core.db); delete old;
        QCOMPARE(GroupAvatarManager::instance(), &current);
    }

    void avatarReadFailureCanBeRetried_data() {
        QTest::addColumn<bool>("prepare");
        QTest::newRow("prepare-denied") << true;
        QTest::newRow("step-interrupted") << false;
    }
    void avatarReadFailureCanBeRetried() {
        QFETCH(bool, prepare);
        SettingsCore core; QVERIFY(core.start());
        QImage image(32, 32, QImage::Format_RGB32); image.fill(Qt::green);
        QByteArray jpeg; QBuffer output(&jpeg); QVERIFY(output.open(QIODevice::WriteOnly)); QVERIFY(image.save(&output, "JPEG"));
        QCOMPARE(group_avatar_set(core.inst, topo_groups_channel_slot(core.inst->topo_groups, qPrintable(core.channel)), (const uint8_t*)jpeg.constData(), size_t(jpeg.size())), 0);
        const auto row = core.db.getChannels()[0];
        GroupAvatarManager avatars(&core.db); QSignalSpy changed(&avatars, &GroupAvatarManager::changed);
        bool denied = false;
        if (prepare) QCOMPARE(sqlite3_set_authorizer(core.inst->topo_sqlite_db,
                [](void* context, int action, const char*, const char* column, const char*, const char*) {
                    if (action != SQLITE_READ || !column || strcmp(column, "avatar_data")) return SQLITE_OK;
                    *static_cast<bool*>(context) = true; return SQLITE_DENY;
                }, &denied), SQLITE_OK);
        else sqlite3_progress_handler(core.inst->topo_sqlite_db, 1, [](void* context) { *static_cast<bool*>(context) = true; return 1; }, &denied);
        auto restore = qScopeGuard([&] {
            sqlite3_set_authorizer(core.inst->topo_sqlite_db, nullptr, nullptr); sqlite3_progress_handler(core.inst->topo_sqlite_db, 0, nullptr, nullptr);
        });
        QVERIFY(avatars.image(core.channel, row.avatarVersion, row.avatarHash).isNull()); QVERIFY(denied);
        QCOMPARE(sqlite3_set_authorizer(core.inst->topo_sqlite_db, nullptr, nullptr), SQLITE_OK);
        sqlite3_progress_handler(core.inst->topo_sqlite_db, 0, nullptr, nullptr);
        avatars.image(core.channel, row.avatarVersion, row.avatarHash);
        QTRY_VERIFY_WITH_TIMEOUT(!avatars.currentImage(core.channel).isNull(), 1000);
        QCOMPARE(changed.count(), 1);
    }

    void avatarBlobTypesAndLimits_data() {
        QTest::addColumn<QByteArray>("value"); QTest::addColumn<bool>("valid"); QTest::addColumn<int>("bytes");
        QTest::newRow("empty-blob") << QByteArray("zeroblob(0)") << true << 0;
        QTest::newRow("maximum-blob") << QByteArray("zeroblob(16384)") << true << 16384;
        QTest::newRow("oversized-blob") << QByteArray("zeroblob(16385)") << false << 0;
        QTest::newRow("null") << QByteArray("NULL") << false << 0;
        QTest::newRow("text") << QByteArray("'jpeg'") << false << 0;
        QTest::newRow("integer") << QByteArray("1") << false << 0;
    }
    void avatarBlobTypesAndLimits() {
        QFETCH(QByteArray, value); QFETCH(bool, valid); QFETCH(int, bytes);
        SettingsCore core; QVERIFY(core.start());
        const QByteArray sql = "UPDATE channels SET avatar_version=1,avatar_hash=zeroblob(32),avatar_data=" + value;
        QCOMPARE(sqlite3_exec(core.inst->topo_sqlite_db, sql.constData(), nullptr, nullptr, nullptr), SQLITE_OK);
        bool ok = !valid; const QByteArray result = core.db.getChannelAvatar(core.channel, &ok);
        QCOMPARE(ok, valid); QCOMPARE(result.size(), bytes);
        QVERIFY(core.db.getChannelAvatar("missing", &ok).isEmpty()); QVERIFY(!ok);
    }

    void unpublishedAvatarIsAnEmptyRead() {
        SettingsCore core; QVERIFY(core.start());
        bool ok = false; QVERIFY(core.db.getChannelAvatar(core.channel, &ok).isEmpty()); QVERIFY(ok);
    }

#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
    void avatarAllocationFailure_data() {
        QTest::addColumn<QByteArray>("source");
        QTest::newRow("request") << QByteArray("groupavatarmanager.cpp");
        QTest::newRow("bridge") << QByteArray("gui_bridge_impl.cpp");
    }
    void avatarAllocationFailure() {
        QFETCH(QByteArray, source);
        SettingsCore core; QVERIFY(core.start()); GroupAvatarManager avatars(&core.db);
        QSignalSpy statuses(&avatars, &GroupAvatarManager::status); const auto baseline = u_get_allocated_count();
        allocationFile = source.constData(); failAllocation = 1;
        auto reset = qScopeGuard([] { failAllocation = 0; allocationFile = nullptr; });
        avatars.remove(core.channel); QCOMPARE(failAllocation, 0);
        QCOMPARE(statuses.count(), 1); QCOMPARE(statuses[0][1].toString(), QString("Cannot update group avatar"));
        gui_bridge_set_inst(nullptr); QCOMPARE(u_get_allocated_count(), baseline);
    }
#endif
};

QTEST_MAIN(ChannelSettingsTest)
#include "test_channel_settings.moc"
