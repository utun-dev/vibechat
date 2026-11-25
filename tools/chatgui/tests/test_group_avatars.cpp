#include "../src/avatars/groupavatarmanager.h"
#include "../src/channelsettingsdialog.h"
#include "../src/channellist.h"
#include "../src/channeldelegate.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QPushButton>
#include <QMessageBox>
#include <QAbstractItemView>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "chat/group_avatar.h"
#include "dm/dm_core.h"
#include "routing_layer/topo_node_sqlite.h"
#include "mem.h"
#include "debug_config.h"
}
static UTUN_INSTANCE* instance;
static QStringList deletedChannels;
extern "C" int gui_bridge_is_uasync_ready(void) { return instance != nullptr; }
extern "C" UTUN_INSTANCE* gui_bridge_get_inst(void) { return instance; }
extern "C" void gui_bridge_post_uasync_fn(void (*fn)(void*), void* arg) { uasync_call_soon(instance->ua, arg, fn); }
extern "C" int gui_bridge_post_uasync_owned_fn(void (*fn)(void*), void* arg) {
    if (!instance) return -1;
    uasync_call_soon(instance->ua, arg, fn); return 0;
}
extern "C" int gui_bridge_post_bound_fn(void (*fn)(void*), void* arg, UTUN_INSTANCE** binding) {
    if (!instance) return -1;
    *binding = instance; return gui_bridge_post_uasync_owned_fn(fn, arg);
}
extern "C" void gui_bridge_set_group_policy_result_cb(gui_dm_messages_fn) {}
extern "C" void gui_bridge_set_group_info_result_cb(gui_dm_messages_fn) {}
static void coreEvent(UTUN_INSTANCE*, int type, const uint8_t* data, int length) {
    if (type == CHAT_EVT_GROUP_AVATAR_RESULT && GroupAvatarManager::instance()) GroupAvatarManager::instance()->onResult(data, length);
    if (type == CHAT_EVT_CHANNEL_DELETED && length > 1 && length == 1 + data[0])
        deletedChannels.append(QString::fromUtf8((const char*)data + 1, data[0]));
}
class GroupAvatarTest : public QObject {
    Q_OBJECT
private slots:
    void preparePublishDisplayPermissionAndClear() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
        UASYNC* ua = uasync_create(); QVERIFY(ua); QTemporaryDir directory; QVERIFY(directory.isValid());
        const QByteArray config = "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\nmember_files_path=" + directory.path().toUtf8() + "/files\n";
        instance = utun_instance_create_from_str(ua, config.constData()); QVERIFY(instance);
        QCOMPARE(utun_core_start(instance), 0); QCOMPARE(chat_service_start(instance), 0); chat_core_create_channel_auto(instance, "avatars");
        DbManager db; db.setDb(instance->topo_sqlite_db);
        QCOMPARE(db.getChannels().size(), 1); const QString channel = db.getChannels()[0].channelId;
        QTimer poll; connect(&poll, &QTimer::timeout, this, [ua] { uasync_poll(ua, 0); }); poll.start(1);
        chat_event_set_handler(instance, coreEvent);
        {
            GroupAvatarManager manager(&db);
            ChannelSettingsDialog owner(channel, &db); QVERIFY(owner.findChild<QPushButton*>("uploadGroupAvatar")->isEnabled());
            QImage image(512, 400, QImage::Format_RGB32);
            for (int y = 0; y < image.height(); y++) for (int x = 0; x < image.width(); x++)
                image.setPixel(x, y, qRgb((x * 73 + y * 39) & 255, (x * 19 + y * 81) & 255, (x * 17 + y * 53) & 255));
            const QString path = directory.path() + "/photo.png"; QVERIFY(image.save(path));
            QSignalSpy result(&manager, &GroupAvatarManager::status); manager.upload(channel, path);
            QTRY_VERIFY_WITH_TIMEOUT(db.getChannels()[0].avatarVersion > 0, 5000);
            QVERIFY(db.getChannelAvatar(channel).size() <= GROUP_AVATAR_MAX);
            QTRY_VERIFY_WITH_TIMEOUT(!manager.image(channel).isNull(), 5000);
            QVERIFY(manager.image(channel).width() <= 128); QCOMPARE(manager.image(channel).width(), manager.image(channel).height());
            const QByteArray saved = db.getChannelAvatar(channel);
            const ChannelRow original = db.getChannels()[0];
            quint64 version = db.getChannels()[0].avatarVersion; QVERIFY(result.size() >= 2);
            QSignalSpy changed(&manager, &GroupAvatarManager::changed);
            for (int i = 0; i < 10; i++) QVERIFY(!manager.image(channel).isNull()); QCOMPARE(changed.count(), 0);
            sqlite3_stmt* st; QCOMPARE(sqlite3_prepare_v2(instance->topo_sqlite_db,
                "UPDATE channels SET ed25519_privkey=NULL WHERE channel_id=?", -1, &st, nullptr), SQLITE_OK);
            const QByteArray ch = channel.toLatin1(); sqlite3_bind_text(st, 1, ch.constData(), ch.size(), SQLITE_TRANSIENT);
            QCOMPARE(sqlite3_step(st), SQLITE_DONE); sqlite3_finalize(st);
            ChannelSettingsDialog reader(channel, &db);
            QVERIFY(!reader.findChild<QPushButton*>("uploadGroupAvatar")->isEnabled()); QVERIFY(!reader.findChild<QPushButton*>("removeGroupAvatar")->isEnabled());
            manager.remove(channel); QTest::qWait(50); QCOMPARE(db.getChannels()[0].avatarVersion, version); QCOMPARE(db.getChannelAvatar(channel), saved);
            QCOMPARE(topo_node_sqlite_channel_put(instance->topo_sqlite_db, ch.constData(), original.name.toUtf8().constData(), reinterpret_cast<const uint8_t*>(original.ownerNodeId.constData()),
                (const uint8_t*)original.x25519_pubkey.constData(), nullptr, (const uint8_t*)original.ed25519_pubkey.constData(),
                (const uint8_t*)original.ed25519_privkey.constData(), (const uint8_t*)original.signature.constData()), 0);
            manager.remove(channel); QTRY_VERIFY_WITH_TIMEOUT(db.getChannels()[0].avatarVersion > version, 5000);
            QVERIFY(db.getChannelAvatar(channel).isEmpty()); QVERIFY(manager.image(channel).isNull());
        }
        poll.stop(); chat_event_set_handler(instance, nullptr); db.setDb(nullptr);
        utun_instance_destroy(instance); instance = nullptr; uasync_destroy(ua, 0);
    }

    void channelDeletionRequiresConfirmationAndKeepsOtherChannels() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
        UASYNC* ua = uasync_create(); QVERIFY(ua);
        const char* config = "[global]\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n";
        instance = utun_instance_create_from_str(ua, config); QVERIFY(instance);
        QCOMPARE(utun_core_start(instance), 0); QCOMPARE(chat_service_start(instance), 0);
        chat_core_create_channel_auto(instance, "Delete me"); chat_core_create_channel_auto(instance, "Keep me");
        DbManager db; db.setDb(instance->topo_sqlite_db);
        QCOMPARE(db.getChannels().size(), 2);
        const QString channel = db.getChannels()[0].channelId, survivor = db.getChannels()[1].channelId;
        ChannelList list(&db); list.loadChannels(); list.selectChannel(channel);
        auto* view = list.findChild<QAbstractItemView*>(); QVERIFY(view);
        const QByteArray sql = "UPDATE channels SET ed25519_privkey=NULL WHERE channel_id='" + channel.toLatin1() + "'";
        QCOMPARE(sqlite3_exec(instance->topo_sqlite_db, sql.constData(), nullptr, nullptr, nullptr), SQLITE_OK);
        deletedChannels.clear(); chat_event_set_handler(instance, coreEvent);
        QTimer poll; connect(&poll, &QTimer::timeout, this, [ua] { uasync_poll(ua, 0); }); poll.start(1);
        ChannelSettingsDialog dialog(channel, &db); dialog.show();
        auto* remove = dialog.findChild<QPushButton*>("deleteChannel"); QVERIFY(remove && remove->isEnabled());
        bool confirmed = false;
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); QVERIFY(box);
            QVERIFY(box->text().contains("Delete me"));
            QCOMPARE(box->defaultButton(), box->button(QMessageBox::Cancel));
            box->button(QMessageBox::Cancel)->click();
        });
        QTest::mouseClick(remove, Qt::LeftButton);
        QCoreApplication::processEvents();
        QCOMPARE(db.getChannels().size(), 2); QVERIFY(deletedChannels.isEmpty());
        QTimer::singleShot(0, &dialog, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); QVERIFY(box);
            auto* button = box->findChild<QPushButton*>("confirmDeleteChannel"); QVERIFY(button);
            confirmed = true; button->click();
        });
        QTest::mouseClick(remove, Qt::LeftButton); QVERIFY(confirmed);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QTRY_COMPARE(db.getChannels().size(), 1);
        QCOMPARE(db.getChannels()[0].channelId, survivor); QCOMPARE(deletedChannels, QStringList({channel}));
        list.loadChannels(); QCOMPARE(view->currentIndex().data(ChannelChannelIdRole).toString(), survivor);
        uint8_t peerX[32], peerEd[32], privateX[32], seed[32];
        QCOMPARE(sc_identity_generate(peerX, privateX, peerEd, seed), SC_OK);
        QCOMPARE(dm_start(instance, sc_derive_node_id_from_pubkey(peerX), peerX, peerEd, "PM", nullptr, DM_MODE_DEVICE, nullptr), 0);
        chat_core_delete_channel(instance, survivor.toLatin1().constData());
        list.loadChannels();
        QVERIFY(!view->currentIndex().data(ChannelIsSectionRole).toBool());
        QVERIFY(view->currentIndex().data(ChannelChannelIdRole).toString().startsWith("dm:"));
        poll.stop(); chat_event_set_handler(instance, nullptr); db.setDb(nullptr);
        utun_instance_destroy(instance); instance = nullptr; uasync_destroy(ua, 0);
    }
};
QTEST_MAIN(GroupAvatarTest)
#include "test_group_avatars.moc"
