/* Настоящие worker, Qt bridge и модель: речь, обновление списка, локальный mute и три формы динамика. */
#include "../transport/utun_node.h"
#include "../transport/gui_bridge.h"
#include "../src/accountdelegate.h"
#include "../src/memberlistmodel.h"
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QListView>
#include <QScopeGuard>

extern "C" {
#include "utun_instance.h"
#include "chat/chat_channel.h"
#include "chat/chat_core.h"
#include "chat/chat_member.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "u_async.h"
#include "debug_config.h"
#include "db_async.h"
#include "mem.h"
}

struct Probe {
    QObject* receiver = nullptr;
    MemberListModel* model = nullptr;
    QString channel;
    char channel_id[65] = {};
    bool ready = false, setup_ok = false, polling_running = false;
    int updates = 0;
    QList<bool> results;
};
static Probe* current;

static void channel_ready(void* arg, const db_async_result* result) {
    auto* probe = static_cast<Probe*>(arg);
    UTUN_INSTANCE* inst = gui_bridge_get_inst();
    const bool ok = !result->status;
    const QString id = QString::fromLatin1(probe->channel_id);
    const int running = uasync_is_running(inst->ua);
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio: desktop bridge probe setup=%d poll_running=%d inst=%p ua=%p ch=%s",
               ok, running, (void*)inst, (void*)inst->ua, qPrintable(id));
    QMetaObject::invokeMethod(probe->receiver, [probe, ok, running, id] {
        probe->setup_ok = ok; probe->polling_running = running; probe->channel = id; probe->ready = true;
    }, Qt::QueuedConnection);
}

static void prepare_channel(void* arg) {
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_DEBUG); debug_enable_console(1);
    chat_channel_create_auto_submit(gui_bridge_get_inst(), "radio mute bridge", "chat", [](void* arg, const chat_channel_result* result) {
        auto* probe = static_cast<Probe*>(arg);
        if (result->status) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio bridge setup: channel creation failed rc=%d", result->sqlite_code);
            QMetaObject::invokeMethod(probe->receiver, [probe] { probe->ready = true; }, Qt::QueuedConnection);
            return;
        }
        qstrncpy(probe->channel_id, result->channel, sizeof(probe->channel_id));
        db_async_submit(gui_bridge_get_inst()->db_worker, "radio bridge peers", DB_ASYNC_WRITE, [](sqlite3* db, void* arg) {
            const char* channel = static_cast<Probe*>(arg)->channel_id;
            const uint8_t key[32] = {1}, other_key[32] = {2};
            int rc = topo_node_sqlite_member_placeholder_write(db, channel, 42, key, key);
            return rc ? rc : topo_node_sqlite_member_placeholder_write(db, channel, 43, other_key, other_key);
        }, probe, channel_ready);
    }, arg);
}

static void member_updated(const uint8_t* data, int len) {
    if (!current || !data || len < 2 || len != data[0] + 2 + CHAT_MEMBER_DISPLAY_SIZE) return;
    chat_member_display member{};
    if (chat_core_deserialize_member(data + data[0] + 2, &member) < 0) return;
    const uint8_t fixtureEd[32] = {1};
    if (!memcmp(member.ed25519, fixtureEd, 32)) ++current->updates;
    const QString channel = QString::fromUtf8((const char*)data + 1, data[0]);
    current->model->onMemberUpdated(channel, data + data[0] + 2);
}

static void member_list(const uint8_t* data, int len) {
    if (!current || !data || len < 3 || len < data[0] + 3) return;
    uint16_t count; memcpy(&count, data + data[0] + 1, 2);
    current->model->onMemberList(QString::fromUtf8((const char*)data + 1, data[0]), count, data + data[0] + 3, len - data[0] - 3);
}

/* Реальный queued bridge, без сетевых пакетов: проверяем доставку START/STOP в GUI. */
static QByteArray senderKey(int id) { return QByteArray(1, char(id - 41)) + QByteArray(31, '\0'); }

static void post_talk(const QString& group, const QByteArray& source, bool on) {
    uint8_t data[67] = {0};
    memcpy(data, QByteArray::fromHex(group.toLatin1()).constData(), 32); memcpy(data + 32, source.constData(), 32); data[66] = on;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio members test: post group=%s peer=%s on=%d",
               qPrintable(group), source.toHex().constData(), on);
    gui_bridge_post(GUI_EVT_RADIO_TALK, data, sizeof(data));
}

static const uint8_t mute_ed[32] = {1};
static void mute_result(const uint8_t* data, int len) {
    if (current && len == CHAT_RADIO_MUTE_RESULT_SIZE) current->results.append(data[CHAT_RADIO_MUTE_RESULT_SIZE - 1] != 0);
}

class RadioMuteBridgeTest : public QObject {
    Q_OBJECT
private slots:
    void pendingMemberRequestsAreFreedBeforeCoreDestroy() {
        gui_bridge_init();
        const auto before = u_get_allocated_count();
        {
            UTUN_INSTANCE instance{};
            instance.ua = uasync_create(); QVERIFY(instance.ua);
            const auto release = qScopeGuard([&] {
                gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr); uasync_destroy(instance.ua, 1);
            });
            gui_bridge_set_uasync(instance.ua); gui_bridge_set_inst(&instance);
            const auto coreOnly = u_get_allocated_count();
            MemberListModel model; model.setChannel("123", false); model.refreshRtt();
            QVERIFY(u_get_allocated_count() > coreOnly);
            gui_bridge_set_inst(nullptr);
            QCOMPARE(u_get_allocated_count(), coreOnly);
            QVERIFY(!instance.ua->posted_tasks_head);
        }
        QCOMPARE(u_get_allocated_count(), before);
    }

    void cleanup() {
        gui_bridge_set_member_updated_cb(nullptr); gui_bridge_set_radio_mute_result_cb(nullptr);
        gui_bridge_set_member_list_cb(nullptr); gui_bridge_set_radio_talk_cb(nullptr);
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr); current = nullptr;
        debug_disable_file_output();
    }

    void desktopWorkerAcceptsIconToggleAndReturnsConfirmedState() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("node.cfg");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        gui_bridge_init();
        MemberListModel model;
        Probe probe; probe.receiver = this; probe.model = &model; current = &probe;
        gui_bridge_set_member_updated_cb(member_updated); gui_bridge_set_radio_mute_result_cb(mute_result);
        gui_bridge_set_member_list_cb(member_list);
        gui_bridge_set_radio_talk_cb([](const uint8_t group[32], const uint8_t source[32], uint8_t on) {
            if (current) current->model->onRadioTalk(QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(group), 32).toHex()), QByteArray(reinterpret_cast<const char*>(source), 32), on != 0);
        });
        UtunNode node; node.setDbPath(directory.filePath("db"));
        node.setDebugFile(qEnvironmentVariable("UTUN_TEST_DIAGNOSTIC_LOG", directory.filePath("diagnostic.log")));
        node.setDebugLevel("warn"); node.setDebugCategories("radio=debug,general=debug");
        QVERIFY(node.start(path)); QTRY_VERIFY_WITH_TIMEOUT(gui_bridge_is_uasync_ready(), 3000);
        gui_bridge_post_uasync_fn(prepare_channel, &probe);
        QTRY_VERIFY_WITH_TIMEOUT(probe.ready, 3000); QVERIFY(probe.setup_ok);
        const QString group = probe.channel;
        model.setChannel(probe.channel, false);
        model.setRadioState(group, true, false);
        model.onRadioTalk(group, senderKey(43), true); // START до загрузки строки не должен потеряться.
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 3, 3000);
        auto findMember = [&](const QByteArray& key) {
            for (int row = 0; row < model.rowCount(); ++row) if (model.memberKeyAt(row) == key) return model.index(row, 0);
            return QModelIndex();
        };
        auto memberIndex = [&](quint64 id) { return findMember(senderKey(id)); };
        const QModelIndex source = memberIndex(42), other = memberIndex(43);
        QVERIFY(source.isValid() && other.isValid());
        QVERIFY(other.data(MemberListModel::RadioSpeakingRole).toBool());
        QListView view; AccountDelegate delegate;
        view.setModel(&model); view.setItemDelegate(&delegate); view.resize(340, 160); view.show(); QTest::qWait(30);
        int accepted = -2;
        connect(&delegate, &AccountDelegate::radioMuteRequested, &view, [&](const QModelIndex&) {
            accepted = gui_bridge_toggle_member_radio_mute(probe.channel.toUtf8().constData(), mute_ed);
        });
        const QRect row = view.visualRect(source); const QPoint button(row.right() - 18, row.center().y());
        auto iconImage = [&] {
            return view.viewport()->grab().toImage().copy(QRect(row.right() - 31, row.center().y() - 14, 28, 28));
        };
        auto iconHasColor = [&](const QColor& color) {
            const QImage image = view.viewport()->grab().toImage();
            for (int y = row.top(); y <= row.bottom(); ++y)
                for (int x = row.right() - 31; x < row.right() - 3; ++x) if (image.pixelColor(x, y) == color) return true;
            return false;
        };
        QVERIFY(iconHasColor(QColor("#9E9E9E"))); QVERIFY(!iconHasColor(QColor("#4CAF50")));
        const QImage silent = iconImage();
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        post_talk(group, senderKey(42), true);
        QTRY_VERIFY(source.data(MemberListModel::RadioSpeakingRole).toBool());
        QVERIFY(iconHasColor(QColor("#4CAF50"))); QVERIFY(iconImage() != silent);
        QVERIFY(!changed.isEmpty()); QCOMPARE(qvariant_cast<QModelIndex>(changed.last()[0]), source);
        QVERIFY(other.data(MemberListModel::RadioSpeakingRole).toBool());
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QCOMPARE(accepted, 0); QVERIFY(probe.polling_running);
        QTRY_COMPARE_WITH_TIMEOUT(probe.results.size(), 1, 3000); QVERIFY(probe.results[0]);
        QCOMPARE(probe.updates, 1); QVERIFY(source.data(Qt::UserRole + 3).toInt() & CHAT_MEMBER_FLAG_RADIO_MUTED);
        QVERIFY(iconHasColor(QColor("#EF5350"))); QVERIFY(!iconHasColor(QColor("#4CAF50")));
        const QImage mutedSpeaking = iconImage();
        auto redPixel = [](const QImage& image, int x, int y) {
            const QColor color = image.pixelColor(x, y);
            return color.red() > color.green() + 50;
        };
        QVERIFY(redPixel(mutedSpeaking, 21, 5)); // Красное перечёркивание правее корпуса динамика.
        QVERIFY(redPixel(mutedSpeaking, 23, 13)); // Волна видна и при локальном mute.
        post_talk(group, senderKey(42), false);
        QTRY_VERIFY(!source.data(MemberListModel::RadioSpeakingRole).toBool());
        const QImage mutedSilent = iconImage();
        QVERIFY(mutedSilent != mutedSpeaking);
        QVERIFY(redPixel(mutedSilent, 21, 5)); QVERIFY(!redPixel(mutedSilent, 23, 13));
        QVERIFY(other.data(MemberListModel::RadioSpeakingRole).toBool());
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QCOMPARE(accepted, 0); QTRY_COMPARE_WITH_TIMEOUT(probe.results.size(), 2, 3000); QVERIFY(probe.results[1]);
        QCOMPARE(probe.updates, 2); QVERIFY(!(source.data(Qt::UserRole + 3).toInt() & CHAT_MEMBER_FLAG_RADIO_MUTED));
        QVERIFY(iconHasColor(QColor("#9E9E9E"))); QVERIFY(!iconHasColor(QColor("#EF5350")));
        post_talk(group, senderKey(42), true);
        QTRY_VERIFY(source.data(MemberListModel::RadioSpeakingRole).toBool());
        const int beforeDuplicate = changed.size();
        post_talk(group, senderKey(42), true); QTest::qWait(20); QCOMPARE(changed.size(), beforeDuplicate);
        model.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 3, 3000);
        QVERIFY(memberIndex(42).data(MemberListModel::RadioSpeakingRole).toBool());
        QVERIFY(memberIndex(43).data(MemberListModel::RadioSpeakingRole).toBool());
        model.refreshRtt(); QTest::qWait(50);
        QVERIFY(memberIndex(42).data(MemberListModel::RadioSpeakingRole).toBool());
        post_talk(QString(64, '1'), senderKey(42), false); QTest::qWait(20);
        QVERIFY(memberIndex(42).data(MemberListModel::RadioSpeakingRole).toBool());

        QModelIndex self;
        for (int i = 0; i < model.rowCount(); ++i) if (model.index(i, 0).data(Qt::UserRole + 2).toBool()) self = model.index(i, 0);
        QVERIFY(self.isValid());
        model.setRadioState(group, true, true); QVERIFY(self.data(MemberListModel::RadioSpeakingRole).toBool());
        const QRect selfRow = view.visualRect(self);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(selfRow.right() - 17, selfRow.center().y()));
        QCOMPARE(probe.results.size(), 2); // Собственный динамик не переключает mute.
        model.setRadioState(group, true, false); QVERIFY(!self.data(MemberListModel::RadioSpeakingRole).toBool());

        // Удаление первой строки сдвигает индексы; STOP следующего источника должен попасть в его новую строку.
        const QByteArray removed = model.index(0).data(MemberListModel::AvatarEdRole).toByteArray();
        model.onMemberRemoved(probe.channel, removed); QCOMPARE(model.rowCount(), 2);
        QVERIFY(!model.memberKeyAt(0).isEmpty() && !model.memberKeyAt(1).isEmpty());
        const QByteArray remaining = model.memberKeyAt(1);
        post_talk(group, model.index(1).data(MemberListModel::AvatarEdRole).toByteArray(), true); QTest::qWait(20);
        post_talk(group, model.index(1).data(MemberListModel::AvatarEdRole).toByteArray(), false); QTest::qWait(20);
        QVERIFY(!findMember(remaining).data(MemberListModel::RadioSpeakingRole).toBool());
        model.refresh(); QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 3, 3000);
        post_talk(group, senderKey(42), true); QTRY_VERIFY(memberIndex(42).data(MemberListModel::RadioSpeakingRole).toBool());
        model.setRadioState({}, false, false);
        QVERIFY(!memberIndex(42).data(MemberListModel::RadioSpeakingRole).toBool());
        post_talk(group, senderKey(42), true); QTest::qWait(20);
        QVERIFY(!memberIndex(42).data(MemberListModel::RadioSpeakingRole).toBool());
        model.setRadioState(group, true, false); post_talk(group, senderKey(42), true);
        QTRY_VERIFY(memberIndex(42).data(MemberListModel::RadioSpeakingRole).toBool());
        model.setChannel(QString(), false); QCOMPARE(model.rowCount(), 0);
        model.setChannel(probe.channel, false); QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 3, 3000);
        QVERIFY(!memberIndex(42).data(MemberListModel::RadioSpeakingRole).toBool());
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio members test: shapes, speech while muted, multiple speakers, refresh, self and reset PASS");
        node.stop(); QVERIFY(!gui_bridge_is_uasync_ready());
        QCOMPARE(gui_bridge_toggle_member_radio_mute(probe.channel.toUtf8().constData(), mute_ed), -1);
        gui_bridge_set_member_updated_cb(nullptr); gui_bridge_set_radio_mute_result_cb(nullptr); current = nullptr;
        debug_disable_file_output();
    }
};
QTEST_MAIN(RadioMuteBridgeTest)
#include "test_radio_mute_bridge.moc"
