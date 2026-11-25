/* Настоящие Qt-карточка и модель; очередь ядра задерживает ответы для проверки смены выбора. */
#include "../src/accountlist.h"
#include "../src/accountdelegate.h"
#include "../src/devgroupdialog.h"
#include <QScopeGuard>
#include <QInputDialog>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include "../src/memberlistmodel.h"
#include "../transport/gui_bridge.h"
#include "../transport/member_edit.h"
#include "../db/db_manager.h"
#include <QtTest>
#include <QListView>
#include <QTreeWidget>
#include <QAbstractItemModelTester>
#include <QMenu>
#include <QMessageBox>
#include <QDir>
#include <QPainter>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QStandardItemModel>
#include <QScopeGuard>
#include <memory>
#include <functional>
#include <vector>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_member.h"
#include "chat/chat_devgroup.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/topo_node.h"
#include "member_files/protocol.h"
#include "debug_config.h"
#include "mem.h"
}

static constexpr char testChannel[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static constexpr char otherChannel[] = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
static UTUN_INSTANCE instance{};
static std::vector<chat_devgroup_request> devgroupRequests;
static gui_dm_messages_fn devgroupCallback;
static std::vector<chat_member_detail_req> requests;
static gui_member_detail_fn detailCallback;
static bool ready = true;
static bool rejectOwned = false;
static int ownedPosts = 0;
static std::vector<chat_member_list_req> memberRequests;
static std::vector<chat_connect_node_req> connectRequests;
static std::vector<QByteArray> muteRequests;

extern "C" UTUN_INSTANCE* gui_bridge_get_inst() { return &instance; }
extern "C" int gui_bridge_self_ed(uint8_t ed[32]) { memcpy(ed, instance.my_ed25519_pubkey, 32); return 0; }
extern "C" int gui_bridge_is_uasync_ready() { return ready; }
extern "C" void gui_bridge_set_member_detail_cb(gui_member_detail_fn cb) { detailCallback = cb; }
extern "C" void gui_bridge_set_radio_mute_result_cb(gui_dm_messages_fn) {}
extern "C" int gui_bridge_toggle_member_radio_mute(const char*, const uint8_t ed[32]) {
    muteRequests.emplace_back(reinterpret_cast<const char*>(ed), 32); return 0;
}
extern "C" void gui_bridge_post_uasync_fn(void (*fn)(void*), void* arg) {
    if (fn == chat_core_collect_member_detail_trampoline) requests.push_back(*static_cast<chat_member_detail_req*>(arg));
    if (fn == chat_core_request_member_list_trampoline || fn == chat_core_request_member_rtt_trampoline)
        memberRequests.push_back(*static_cast<chat_member_list_req*>(arg));
    if (fn == chat_core_connect_node_trampoline) connectRequests.push_back(*static_cast<chat_connect_node_req*>(arg));
    if (fn == chat_devgroup_trampoline) devgroupRequests.push_back(*static_cast<chat_devgroup_request*>(arg));
    u_free(arg);
}
extern "C" int gui_bridge_post_uasync_owned_fn(void (*fn)(void*), void* arg) {
    ++ownedPosts;
    if (rejectOwned) return -1;
    gui_bridge_post_uasync_fn(fn, arg);
    return 0;
}

// Сохранение диалогов проверяет test_member_dialogs; здесь такие команды не ожидаются.
bool gui_bridge_save_member(sqlite3*, const QString&, const QByteArray&, const QString&, const QByteArray&, QObject*, std::function<void(int)>) {
    QTest::qFail("Unexpected member save in member detail test", __FILE__, __LINE__);
    return false;
}

bool gui_bridge_transfer_admin(sqlite3*, const QString&, const QByteArray&) {
    QTest::qFail("Unexpected admin transfer in member detail test", __FILE__, __LINE__);
    return false;
}

static QByteArray testEd(quint64 node) {
    return QByteArray::fromHex(QByteArray::number(node, 16).rightJustified(64, '0'));
}

static QByteArray row(quint64 node, bool online = true, int up = 5, int rtt = 30, int nat = 255, int intermediaries = 0, bool self = false) {
    chat_member_display member{};
    memcpy(member.ed25519, testEd(node).constData(), 32);
    member.online = online; member.conn_presence = 5; member.conn_up = up;
    member.rtt = rtt; member.nat_type = nat; member.intermediaries = intermediaries; member.is_self = self;
    member.flags = CHAT_MEMBER_FLAG_PHONE_KNOWN;
    snprintf(member.name, sizeof(member.name), "peer%llu", (unsigned long long)node);
    QByteArray bytes(CHAT_MEMBER_DISPLAY_SIZE, 0);
    chat_core_serialize_member(&member, reinterpret_cast<uint8_t*>(bytes.data()));
    return bytes;
}

static QByteArray reply(const chat_member_detail_req& req, const QList<quint64>& hops = {}, int result = 0) {
    QByteArray bytes(CHAT_MEMBER_DETAIL_HEADER_SIZE + hops.size() * 32, 0);
    auto* data = reinterpret_cast<uint8_t*>(bytes.data());
    mf_put64(data, req.request_id); memcpy(data + 8, req.group, 32); memcpy(data + 40, req.ed, sizeof(req.ed));
    data[CHAT_MEMBER_DETAIL_RESULT_OFF] = result; data[CHAT_MEMBER_DETAIL_RESULT_OFF + 1] = result ? 0 : 7; data[CHAT_MEMBER_DETAIL_HOP_COUNT_OFF] = hops.size();
    for (int i = 0; i < hops.size(); i++) memcpy(data + CHAT_MEMBER_DETAIL_HEADER_SIZE + i * 32, testEd(hops[i]).constData(), 32);
    return bytes;
}

static void deliver(const QByteArray& bytes) {
    if (detailCallback) detailCallback(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
}

static void fill(AccountList& panel, const QString& channel, const QByteArray& bytes) {
    panel.setChannel(channel);
    panel.onMemberList(channel, bytes.size() / CHAT_MEMBER_DISPLAY_SIZE,
        reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
}

class MemberDetailTest : public QObject {
    Q_OBJECT
    static QString body(AccountList& p) { return p.findChild<QTextEdit*>("memberDetailText")->toPlainText(); }
    static QString header(AccountList& p) { return p.findChild<QLabel*>("memberDetailHeader")->text(); }
    static bool select(AccountList& p, int rowNumber) {
        return QMetaObject::invokeMethod(&p, "onMemberClicked", Qt::DirectConnection, Q_ARG(QModelIndex, p.model()->index(rowNumber)));
    }
    static bool context(AccountList& panel, const QString& actionText, const std::function<void()>& change = {}, int row = 0) {
        auto* view = panel.findChild<QListView*>();
        const QPoint point = view->visualRect(view->model()->index(row, 0)).center();
        bool clicked = false;
        QTimer choose, deadline; choose.setSingleShot(true); deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &deadline, [] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) menu->close();
        });
        QObject::connect(&choose, &QTimer::timeout, &choose, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) { QTest::qFail("member context menu was not shown", __FILE__, __LINE__); return; }
            if (change) change();
            if (!menu->isVisible()) return;
            for (auto* action : menu->actions()) if (action->text() == actionText) {
                if (!action->isEnabled()) { menu->close(); return; }
                clicked = true;
                QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
                return;
            }
            menu->close(); QTest::qFail("member context action was not found", __FILE__, __LINE__);
        });
        choose.start(0); deadline.start(1000);
        const bool invoked = QMetaObject::invokeMethod(&panel, "onMemberContextMenu", Qt::DirectConnection, Q_ARG(QPoint, point));
        return invoked && clicked;
    }
private slots:
    void cardAssignmentButtonUsesItsOwnTargetAndPreservesCalls() {
        QStandardItemModel model(1, 1); const auto index = model.index(0, 0);
        model.setData(index, testEd(42), MemberListModel::AvatarEdRole);
        model.setData(index, true, MemberListModel::OnlineRole);
        const QString key(64, QLatin1Char('a')); model.setData(index, key, MemberListModel::Ed25519Role);
        AccountDelegate delegate; int assigned = 0;
        delegate.assignmentState = [&](const QString& ed) { return ed == key ? assigned : -1; };
        QSignalSpy assignments(&delegate, &AccountDelegate::assignmentRequested), calls(&delegate, &AccountDelegate::callRequested);
        QStyleOptionViewItem option; option.rect = QRect(0, 0, 340, MemberListModel::kRowHeight);
        auto click = [&](QPoint point) {
            QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            delegate.editorEvent(&press, &model, option, index); delegate.editorEvent(&release, &model, option, index);
        };
        click(QPoint(option.rect.right() - 17, option.rect.center().y()));
        QCOMPARE(assignments.size(), 1); QCOMPARE(assignments.first().first().toString(), key);
        assigned = 1; click(QPoint(option.rect.right() - 17, option.rect.center().y())); QCOMPARE(assignments.size(), 2);
        click(QPoint(option.rect.right() - 81, option.rect.center().y())); QCOMPARE(calls.size(), 1);
        // Самого себя можно назначать, но удалённые записи и заголовки устройств не являются исполнителями.
        model.setData(index, true, Qt::UserRole + 2); click(QPoint(option.rect.right() - 17, option.rect.center().y()));
        QCOMPARE(assignments.size(), 3);
        model.setData(index, CHAT_MEMBER_FLAG_DELETED, Qt::UserRole + 3);
        click(QPoint(option.rect.right() - 17, option.rect.center().y())); QCOMPARE(assignments.size(), 3);
        assigned = -1; model.setData(index, 0, Qt::UserRole + 3);
        click(QPoint(option.rect.right() - 17, option.rect.center().y())); QCOMPARE(assignments.size(), 3);
    }
    void connectionOrderBeforeAddresses_data() {
        QTest::addColumn<QByteArray>("order"); QTest::addColumn<QString>("expected");
        QTest::newRow("sequential") << QByteArray("RTU2-2") << QString("REA->TCP->UDP seq 2s");
        QTest::newRow("first") << QByteArray("UTR1") << QString("UDP->TCP->REA first");
        QTest::newRow("parallel") << QByteArray("TUR3") << QString("TCP->UDP->REA par");
        QTest::newRow("unknown") << QByteArray() << QString("unknown");
    }

    void connectionOrderBeforeAddresses() {
        QFETCH(QByteArray, order); QFETCH(QString, expected);
        sqlite3* database = nullptr; QCOMPARE(sqlite3_open(":memory:", &database), SQLITE_OK);
        const auto close = qScopeGuard([&] { sqlite3_close(database); });
        TOPOMSG_NODE h{};
        const uint8_t seed[32] = {7}; uint8_t privateX[32];
        QCOMPARE(sc_identity_from_seed(seed, h.public_key, privateX, h.ed25519_public_key), SC_OK);
        const auto node = sc_derive_node_id_from_pubkey(h.public_key);
        const auto xHex = QByteArray(reinterpret_cast<const char*>(h.public_key), 32).toHex();
        char* schema = sqlite3_mprintf("CREATE TABLE nodes(node_id,last_seen_at,created_at,ed25519_pubkey,x25519_pubkey); INSERT INTO nodes VALUES(%llu,0,0,x'%s',x'%s');"
            "CREATE TABLE node_snapshots(x25519,record,format);"
            "CREATE TABLE node_addresses(node_x,family,protocol,address,port,socket_id,addr_type,nat_mapping,nat_inbound,nat_source);"
            "INSERT INTO node_addresses VALUES(x'%s',4,1,x'c0000201',1333,1,0,0,0,0);",
            (unsigned long long)node, testEd(node).toHex().constData(), xHex.constData(), xHex.constData());
        const auto freeSql = qScopeGuard([&] { sqlite3_free(schema); });
        QVERIFY(schema);
        QCOMPARE(sqlite3_exec(database, schema, nullptr, nullptr, nullptr), SQLITE_OK);
        if (!order.isEmpty()) {
            h.node_name_len = 4; h.connection_order_len = order.size();
            QByteArray record(reinterpret_cast<const char*>(&h), sizeof(h)); record += "Peer"; record += order;
            sqlite3_stmt* st = nullptr;
            QCOMPARE(sqlite3_prepare_v2(database, "INSERT INTO node_snapshots VALUES(?,?,3)", -1, &st, nullptr), SQLITE_OK);
            QCOMPARE(sqlite3_bind_blob(st, 1, h.public_key, 32, SQLITE_STATIC), SQLITE_OK);
            QCOMPARE(sqlite3_bind_blob(st, 2, record.constData(), record.size(), SQLITE_TRANSIENT), SQLITE_OK);
            QCOMPARE(sqlite3_step(st), SQLITE_DONE); QCOMPARE(sqlite3_finalize(st), SQLITE_OK);
        }
        DbManager manager; manager.setDb(database); AccountList panel(&manager); panel.show();
        fill(panel, testChannel, row(node)); QVERIFY(select(panel, 0));
        deliver(reply(requests.back()));
        const auto text = body(panel);
        QVERIFY2(text.contains(expected + "\n── Addresses ──"), qPrintable(text));
        manager.setDb(nullptr);
    }

    void protocolWarningsFollowMemberStateThroughGroupedModel() {
        MemberListModel source; source.setChannel(testChannel, true);
        GroupedMemberModel grouped(&source);
        auto unknown = row(7), compatible = row(8), incompatible = row(9, false);
        compatible[CHAT_MEMBER_PROTOCOL_STATE_OFF] = 1; compatible[CHAT_MEMBER_PROTOCOL_STATE_OFF + 1] = 1; compatible[CHAT_MEMBER_PROTOCOL_STATE_OFF + 3] = 1;
        incompatible[CHAT_MEMBER_PROTOCOL_STATE_OFF] = 2; incompatible[CHAT_MEMBER_PROTOCOL_STATE_OFF + 1] = 1; incompatible[CHAT_MEMBER_PROTOCOL_STATE_OFF + 3] = 2;
        const auto bytes = unknown + compatible + incompatible;
        source.onMemberList(testChannel, 3, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QCOMPARE(grouped.rowCount(), 3);
        for (int i = 0; i < 3; ++i) {
            const auto expected = source.index(i).data(MemberListModel::ProtocolWarningRole).toString();
            QCOMPARE(expected.isEmpty(), i != 2);
            QCOMPARE(grouped.index(i).data(MemberListModel::ProtocolWarningRole).toString(), expected);
            QVERIFY(!grouped.index(i).data(GroupedMemberModel::DeletedGroupRole).toBool());
        }
        const auto warning = grouped.index(2).data(MemberListModel::ProtocolWarningRole).toString();
        QVERIFY(warning.contains("1") && warning.contains("2"));
        incompatible[CHAT_MEMBER_PROTOCOL_STATE_OFF] = 1; incompatible[CHAT_MEMBER_PROTOCOL_STATE_OFF + 3] = 1;
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(incompatible.constData()));
        QVERIFY(grouped.index(2).data(MemberListModel::ProtocolWarningRole).toString().isEmpty());
        compatible[CHAT_MEMBER_FLAGS_OFF] = CHAT_MEMBER_FLAG_DELETED;
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(compatible.constData()));
        const auto deleted = grouped.index(grouped.rowCount() - 1);
        QVERIFY(deleted.data(GroupedMemberModel::DeletedGroupRole).toBool());
        QVERIFY(deleted.data(MemberListModel::ProtocolWarningRole).toString().isEmpty());
    }

    void clipboardContextChecksPermissionAndCurrentTarget_data() {
        QTest::addColumn<bool>("allowed"); QTest::addColumn<bool>("online"); QTest::addColumn<bool>("self"); QTest::addColumn<bool>("revoked");
        QTest::newRow("off") << false << true << false << false;
        QTest::newRow("on") << true << true << false << false;
        QTest::newRow("offline") << true << false << false << false;
        QTest::newRow("self") << true << true << true << false;
        QTest::newRow("revoked-in-menu") << true << true << false << true;
    }
    void clipboardContextChecksPermissionAndCurrentTarget() {
        QFETCH(bool, allowed); QFETCH(bool, online); QFETCH(bool, self); QFETCH(bool, revoked);
        sqlite3* database = nullptr; QCOMPARE(sqlite3_open(":memory:", &database), SQLITE_OK);
        const auto close = qScopeGuard([&] { sqlite3_close(database); }); QCOMPARE(topo_node_sqlite_init(database), 0);
        sqlite3_stmt* insert = nullptr;
        QCOMPARE(sqlite3_prepare_v2(database, "INSERT INTO channels(channel_id,description_json,x25519_pubkey,ed25519_pubkey,signature) "
            "VALUES(?,'{\"name\":\"Group\",\"clipboard_members\":false}',zeroblob(32),?,zeroblob(64))", -1, &insert, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_bind_text(insert, 1, testChannel, -1, SQLITE_STATIC), SQLITE_OK);
        const QByteArray groupKey = QByteArray::fromHex(testChannel);
        QCOMPARE(sqlite3_bind_blob(insert, 2, groupKey.constData(), groupKey.size(), SQLITE_TRANSIENT), SQLITE_OK);
        QCOMPARE(sqlite3_step(insert), SQLITE_DONE);
        QCOMPARE(sqlite3_finalize(insert), SQLITE_OK);
        if (allowed) QVERIFY2(sqlite3_exec(database, "UPDATE channels SET description_json=json_set(description_json,'$.clipboard_members',json('true'))",
            nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(database));
        DbManager manager; manager.setDb(database); AccountList panel(&manager); panel.resize(420, 600); panel.show();
        fill(panel, testChannel, row(7, online, 5, 30, 255, 0, self)); QSignalSpy sent(&panel, &AccountList::clipboardRequested);
        bool inspected = false;
        QTimer::singleShot(0, &panel, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); QVERIFY(menu);
            const auto dismiss = qScopeGuard([menu] { if (menu->isVisible()) menu->close(); });
            QAction* action = nullptr; for (auto* candidate : menu->actions()) if (candidate->text() == "Send clipboard") action = candidate;
            QCOMPARE(action != nullptr, allowed); inspected = true; if (!action) return;
            QCOMPARE(action->isEnabled(), online && !self); if (!action->isEnabled()) return;
            if (revoked) QVERIFY2(sqlite3_exec(database, "UPDATE channels SET description_json=json_set(description_json,'$.clipboard_members',json('false'))",
                nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(database));
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
        });
        auto* view = panel.findChild<QListView*>(); const auto point = view->visualRect(view->model()->index(0, 0)).center();
        QVERIFY(QMetaObject::invokeMethod(&panel, "onMemberContextMenu", Qt::DirectConnection, Q_ARG(QPoint, point))); QVERIFY(inspected);
        QCOMPARE(sent.count(), allowed && online && !self && !revoked ? 1 : 0);
        if (!sent.isEmpty()) {
            QByteArray address(32, '\0'); address[31] = char(7);
            QCOMPARE(sent[0][0].toString(), QString::fromLatin1(testChannel)); QCOMPARE(sent[0][1].toByteArray(), address);
        }
        manager.setDb(nullptr);
    }
    void narrowRowsHideActionsInPaintClicksAndTooltips() {
        const struct { int width, actions, badges; } cases[] = {
            {300, 3, 0}, {210, 2, 0}, {180, 1, 0}, {150, 0, 0},
            {240, 2, CHAT_MEMBER_FLAG_ADMIN | CHAT_MEMBER_FLAG_MODER}
        };
        for (const auto& test : cases) {
            QStandardItemModel model(1, 1); const auto index = model.index(0, 0);
            model.setData(index, "Nickname"); model.setData(index, true, MemberListModel::OnlineRole);
            model.setData(index, CHAT_MEMBER_FLAG_RADIO | CHAT_MEMBER_FLAG_PHONE_KNOWN | CHAT_MEMBER_FLAG_PHONE_BUSY | test.badges, Qt::UserRole + 3);
            QPixmap avatar(MemberListModel::kAvatarSize, MemberListModel::kAvatarSize); avatar.fill(Qt::blue);
            model.setData(index, QIcon(avatar), Qt::DecorationRole);
            QListView view; view.setModel(&model); AccountDelegate delegate;
            QStyleOptionViewItem option; option.rect = QRect(0, 0, test.width, MemberListModel::kRowHeight);
            option.palette.setColor(QPalette::AlternateBase, Qt::white); option.palette.setColor(QPalette::Text, QColor(20, 50, 100));
            QImage image(option.rect.size(), QImage::Format_RGB32); image.fill(Qt::white);
            { QPainter painter(&image); delegate.paint(&painter, option, index); }
            QSignalSpy radios(&delegate, &AccountDelegate::radioCallRequested), calls(&delegate, &AccountDelegate::callRequested);
            QSignalSpy mutes(&delegate, &AccountDelegate::radioMuteRequested);
            const struct { int offset, minimum; const char* color; QSignalSpy* spy; } buttons[] = {
                {95, 3, "#4CAF50", &radios}, {63, 2, "#E53935", &calls}, {31, 1, "#9E9E9E", &mutes}
            };
            for (const auto& button : buttons) {
                const bool visible = test.actions >= button.minimum;
                if (!test.badges) {
                    int pixels = 0;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = option.rect.right() - button.offset; x < option.rect.right() - button.offset + 28; ++x)
                            pixels += image.pixelColor(x, y) == QColor(button.color);
                    QCOMPARE(pixels > 0, visible);
                }
                const QPoint point(option.rect.right() - button.offset + 14, option.rect.center().y());
                QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                delegate.editorEvent(&press, &model, option, index); delegate.editorEvent(&release, &model, option, index);
                QCOMPARE(button.spy->size(), visible ? 1 : 0);
                QHelpEvent help(QEvent::ToolTip, point, view.viewport()->mapToGlobal(point));
                QCOMPARE(delegate.helpEvent(&help, &view, option, index), visible); QToolTip::hideText();
            }
        }
    }
    void contextKeepsClickedMemberSelected_data() {
        QTest::addColumn<bool>("previousGroup");
        QTest::newRow("previous-member") << false;
        QTest::newRow("previous-subgroup") << true;
    }
    void contextKeepsClickedMemberSelected() {
        QFETCH(bool, previousGroup);
        AccountList panel; panel.resize(420, 600); panel.show();
        auto first = row(7), target = row(8), extra = row(9);
        if (previousGroup) {
            memset(first.data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(first.data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "Devices", 8);
        }
        const auto snapshot = first + target + extra; fill(panel, testChannel, snapshot);
        auto* view = panel.findChild<QListView*>(); QVERIFY(view);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                          view->visualRect(view->model()->index(0, 0)).topLeft() + QPoint(100, 16));
        const QPoint point = view->visualRect(view->model()->index(1, 0)).topLeft() + QPoint(100, 16);
        // Правый press меняет Qt selection, но не обязан посылать clicked для обновления карточки.
        QTest::mousePress(view->viewport(), Qt::RightButton, Qt::NoModifier, point);
        bool inspected = false;
        QTimer::singleShot(0, &panel, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) { QTest::qFail("member context menu missing", __FILE__, __LINE__); return; }
            const auto close = qScopeGuard([menu] { menu->close(); });
            // Полный снимок во вложенном event loop меню должен восстановить именно выбранный правой кнопкой узел.
            panel.onMemberList(testChannel, 3, reinterpret_cast<const uint8_t*>(snapshot.constData()), snapshot.size());
            DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "context selection after snapshot: previous_group=%d peer=%s header=%s",
                previousGroup, view->currentIndex().data(MemberListModel::AvatarEdRole).toByteArray().toHex().constData(), qPrintable(header(panel)));
            QCOMPARE(view->currentIndex().data(MemberListModel::AvatarEdRole).toByteArray(), testEd(8));
            QVERIFY(header(panel).contains("peer8")); inspected = true;
        });
        QVERIFY(QMetaObject::invokeMethod(view, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point)));
        QTest::mouseRelease(view->viewport(), Qt::RightButton, Qt::NoModifier, point);
        QVERIFY(inspected);
        QCOMPARE(view->currentIndex().data(MemberListModel::AvatarEdRole).toByteArray(), testEd(8));
    }

    void init() {
        requests.clear(); memberRequests.clear(); connectRequests.clear(); muteRequests.clear(); ownedPosts = 0; rejectOwned = false; ready = true;
        devgroupRequests.clear();
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    }

    void deviceInvitationWaitsForSelfMenuAndSupportsBothPins_data() {
        QTest::addColumn<bool>("grouped");
        QTest::newRow("self") << false; QTest::newRow("collapsed-own-group") << true;
    }
    void deviceInvitationWaitsForSelfMenuAndSupportsBothPins() {
        QFETCH(bool, grouped);
        AccountList window; window.resize(420, 600); window.show();
        auto self = row(7, true, 5, 30, 255, 0, true);
        if (grouped) { memset(self.data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(self.data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "Devices", 8); }
        fill(window, testChannel, self); devgroupRequests.clear(); initDevgroupUi(&window);
        QVERIFY(devgroupCallback && !devgroupRequests.empty());
        auto respond = [](quint64 request, const QByteArray& json) {
            QByteArray bytes(9, 0); mf_put64(reinterpret_cast<uint8_t*>(bytes.data()), request); bytes[8] = 1; bytes += json;
            devgroupCallback(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        };
        const QByteArray invite = "0123456789abcdef0123456789abcdef";
        const QByteArray snapshot = "{\"group\":\"\",\"name\":\"\",\"invitations\":[{\"id\":\"" + invite +
            "\",\"peer\":\"0000000000000000000000000000000000000000000000000000000000000007\",\"channel\":\"" + testChannel + "\",\"outgoing\":0,\"state\":1,\"input\":1}]}";
        respond(devgroupRequests.back().request_id, snapshot);
        QVERIFY(hasPendingDevicePairing()); QVERIFY(!window.findChild<QDialog*>("devgroupPairingDialog"));
        QVERIFY(context(window, "Finish pairing"));
        auto* dialog = window.findChild<QDialog*>("devgroupPairingDialog"); QVERIFY(dialog && dialog->isVisible());
        auto* label = dialog->findChild<QLabel*>(); QVERIFY(label->text().contains("personal device subgroup"));
        QVERIFY(label->text().contains("simply close this window"));
        QVERIFY(dialog->grab().save(QDir::tempPath() + "/device-link-pin-help.png"));
        auto* edit = dialog->findChild<QLineEdit*>("devgroupPinInput"); QVERIFY(edit);
        auto* check = dialog->findChild<QPushButton*>("devgroupCheckPin"); QVERIFY(check && !check->isEnabled());
        edit->setText("1234"); QVERIFY(check->isEnabled()); check->click();
        QCOMPARE(devgroupRequests.back().action, CHAT_DEVGROUP_ACCEPT);
        QCOMPARE(QByteArray(devgroupRequests.back().text), invite); QCOMPARE(QByteArray(devgroupRequests.back().pin), QByteArray("1234"));
        // Неверный PIN разрешает повтор в том же окне, а закрытие не открывает его автоматически.
        respond(devgroupRequests.back().request_id, snapshot);
        QVERIFY(edit->isEnabled()); QVERIFY(!check->isEnabled()); dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        devgroupCallback(nullptr, 0); respond(devgroupRequests.back().request_id, snapshot);
        QVERIFY(!window.findChild<QDialog*>("devgroupPairingDialog"));
        QVERIFY(context(window, "Finish pairing"));
        dialog = window.findChild<QDialog*>("devgroupPairingDialog"); QVERIFY(dialog && dialog->isVisible());
        const QByteArray second = "{\"group\":\"\",\"name\":\"\",\"invitations\":[{\"id\":\"" + invite +
            "\",\"peer\":\"0000000000000000000000000000000000000000000000000000000000000007\",\"outgoing\":0,\"state\":8,\"pin\":\"5678\",\"input\":0}]}";
        respond(devgroupRequests.back().request_id, second);
        QVERIFY(dialog->findChild<QLabel*>()->text().contains("Return PIN: 5678"));
        QVERIFY(!dialog->findChild<QLineEdit*>()->isVisible());
        const QByteArray outgoing = "{\"group\":\"\",\"name\":\"\",\"invitations\":[{\"id\":\"" + invite +
            "\",\"peer\":\"0000000000000000000000000000000000000000000000000000000000000007\",\"outgoing\":1,\"state\":8,\"input\":2}]}";
        devgroupCallback(nullptr, 0); respond(devgroupRequests.back().request_id, outgoing);
        edit = dialog->findChild<QLineEdit*>(); QVERIFY(edit->isVisible()); edit->setText("5678");
        dialog->findChild<QPushButton*>("devgroupCheckPin")->click();
        QCOMPARE(devgroupRequests.back().action, CHAT_DEVGROUP_ACCEPT);
        QCOMPARE(QByteArray(devgroupRequests.back().pin), QByteArray("5678"));
    }

    void addMyDeviceImmediatelyRequestsInvitationAndShowsPin() {
        QWidget parent; devgroupRequests.clear(); const QByteArray peer = QByteArray(31, 0) + char(7); inviteMyDevice(&parent, testChannel, peer);
        QVERIFY(!devgroupRequests.empty()); const auto request = devgroupRequests.back();
        QCOMPARE(request.action, CHAT_DEVGROUP_INVITE); QCOMPARE(QByteArray(request.group), QByteArray(testChannel)); QCOMPARE(QByteArray(reinterpret_cast<const char*>(request.peer), 32), peer);
        QByteArray bytes(9, 0); mf_put64(reinterpret_cast<uint8_t*>(bytes.data()), request.request_id); bytes[8] = 1;
        bytes += "{\"id\":\"0123456789abcdef0123456789abcdef\",\"pin\":\"1234\"}";
        devgroupCallback(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        bool shown = false; for (auto* label : parent.findChildren<QLabel*>()) shown |= label->text().contains("1234");
        QVERIFY(shown);
    }

    void myDeviceMenuEligibility_data() {
        QTest::addColumn<QString>("scenario");
        for (const char* scenario : {"eligible", "offline", "direct-only", "known-bgp", "self", "deleted", "grouped", "unnamed-group"})
            QTest::newRow(scenario) << QString(scenario);
    }
    void myDeviceMenuEligibility() {
        QFETCH(QString, scenario);
        AccountList panel; panel.resize(420, 600); panel.show();
        auto member = row(7, scenario != "offline", scenario == "direct-only" ? 1 : scenario == "known-bgp" ? 0 : 5,
                          30, 255, 0, scenario == "self");
        if (scenario == "deleted") member[CHAT_MEMBER_FLAGS_OFF] = CHAT_MEMBER_FLAG_DELETED;
        if (scenario == "grouped" || scenario == "unnamed-group") member[CHAT_MEMBER_DEVGROUP_OFF] = 42;
        if (scenario == "grouped") memcpy(member.data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "Devices", 8);
        fill(panel, testChannel, member);
        if (scenario == "deleted") { // Владелец канала видит удалённую строку, но связывать её также нельзя.
            panel.model()->setChannel(testChannel, true);
            panel.onMemberList(testChannel, 1, reinterpret_cast<const uint8_t*>(member.constData()), member.size());
        }
        auto* view = panel.findChild<QListView*>();
        auto* grouped = qobject_cast<GroupedMemberModel*>(view->model()); QVERIFY(grouped);
        if (grouped->index(0, 0).data(GroupedMemberModel::GroupHeaderRole).toBool()) QVERIFY(grouped->toggle(0));
        view->doItemsLayout(); const int targetRow = grouped->rowCount() - 1;
        bool inspected = false;
        QTimer::singleShot(0, &panel, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); QVERIFY(menu);
            const auto close = qScopeGuard([menu] { menu->close(); });
            bool found = false;
            for (auto* action : menu->actions()) found |= action->text() == "This is my device";
            QCOMPARE(found, scenario == "eligible"); inspected = true;
        });
        const QPoint point = view->visualRect(grouped->index(targetRow, 0)).center();
        QVERIFY(QMetaObject::invokeMethod(&panel, "onMemberContextMenu", Qt::DirectConnection, Q_ARG(QPoint, point)));
        QVERIFY(inspected); QVERIFY(devgroupRequests.empty());
    }

    void cardLayoutDisablesMemberDetailsAndLateReplies() {
        AccountList panel; panel.resize(420, 600); panel.show(); fill(panel, testChannel, row(7));
        QVERIFY(select(panel, 0)); QVERIFY(!requests.empty()); const auto pending = requests.back();
        panel.setDetailsEnabled(false);
        auto* text = panel.findChild<QTextEdit*>("memberDetailText"); QVERIFY(text && !text->isVisible());
        const auto count = requests.size();
        QVERIFY(select(panel, 0)); deliver(reply(pending)); QApplication::processEvents();
        QCOMPARE(requests.size(), count); QVERIFY(body(panel).isEmpty()); QVERIFY(!text->isVisible());
        panel.setDetailsEnabled(true); QVERIFY(text->isVisible());
        QVERIFY(select(panel, 0)); QVERIFY(requests.size() > count);
    }

    void myDeviceConfirmation_data() {
        QTest::addColumn<QString>("scenario");
        for (const char* scenario : {"cancel", "link", "offline", "grouped", "channel-changed", "deleted"})
            QTest::newRow(scenario) << QString(scenario);
    }
    void myDeviceConfirmation() {
        QFETCH(QString, scenario);
        AccountList panel; panel.resize(420, 600); panel.show(); fill(panel, testChannel, row(7));
        bool inspected = false;
        QVERIFY(context(panel, "This is my device", [&] {
            QTimer::singleShot(0, &panel, [&] {
                auto* dialog = panel.findChild<QMessageBox*>("linkMyDeviceConfirmation"); QVERIFY(dialog);
                const auto close = qScopeGuard([dialog] { dialog->reject(); });
                QVERIFY(devgroupRequests.empty()); QVERIFY(dialog->text().contains("shared keys"));
                if (scenario == "cancel") QVERIFY(dialog->grab().save(QDir::tempPath() + "/device-link-confirmation.png"));
                QPushButton* link = nullptr;
                for (auto* button : dialog->buttons()) if (button->text() == "Start linking") link = qobject_cast<QPushButton*>(button);
                QVERIFY(link && link->isEnabled());
                auto changed = row(7);
                if (scenario == "offline") changed = row(7, false, 0);
                if (scenario == "grouped") changed[CHAT_MEMBER_DEVGROUP_OFF] = 42;
                if (scenario == "deleted") changed[CHAT_MEMBER_FLAGS_OFF] = CHAT_MEMBER_FLAG_DELETED;
                if (scenario == "channel-changed") fill(panel, otherChannel, row(7));
                else if (scenario != "link" && scenario != "cancel")
                    panel.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(changed.constData()));
                if (scenario == "link") link->click();
                else if (scenario != "cancel") QVERIFY(!link->isEnabled());
                inspected = true;
            });
        }));
        QVERIFY(inspected); QCOMPARE(devgroupRequests.size(), size_t(scenario == "link" ? 1 : 0));
        if (scenario == "link") {
            QCOMPARE(devgroupRequests.front().action, CHAT_DEVGROUP_INVITE);
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(devgroupRequests.front().peer), 32), QByteArray(31, 0) + char(7)); QCOMPARE(QByteArray(devgroupRequests.front().group), QByteArray(testChannel));
        }
    }

    void disconnectSelfUsesLeaveCommandAndDisablesButton() {
        QWidget parent; QPushButton button("Disconnect from my devices", &parent); devgroupRequests.clear();
        disconnectMyDevice(&button);
        QVERIFY(!button.isEnabled()); QVERIFY(!devgroupRequests.empty());
        QCOMPARE(devgroupRequests.back().action, CHAT_DEVGROUP_LEAVE);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(devgroupRequests.back().peer), 32), QByteArray(32, 0));
        QByteArray bytes(9, 0); mf_put64(reinterpret_cast<uint8_t*>(bytes.data()), devgroupRequests.back().request_id); bytes[8] = 1;
        bytes += "{}"; devgroupCallback(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QVERIFY(!button.isEnabled());
    }

    void actualRouteText_data() {
        QTest::addColumn<int>("up"); QTest::addColumn<int>("hops"); QTest::addColumn<int>("localNat"); QTest::addColumn<int>("peerNat");
        QTest::addColumn<int>("rtt"); QTest::addColumn<bool>("online"); QTest::addColumn<QString>("compact"); QTest::addColumn<QString>("detail");
        QTest::newRow("bgp-one-public-address") << 4 << 1 << 2 << 1 << 50 << true << "B1, RTT=5ms" << "BGP(1 hop) RTT=5ms";
        QTest::newRow("bgp-two") << 4 << 2 << 2 << 1 << 50 << true << "B2, RTT=5ms" << "BGP(2 hop) RTT=5ms";
        QTest::newRow("direct-both-nats") << 5 << 0 << 2 << 3 << 50 << true << "DIR, RTT=5ms" << "DIR(EIM-STRICT) RTT=5ms";
        QTest::newRow("direct-reverse-order") << 5 << 0 << 3 << 2 << 93 << true << "DIR, RTT=9.3ms" << "DIR(STRICT-EIM) RTT=9.3ms";
        QTest::newRow("direct-unknown-nat-rtt") << 5 << 0 << 1 << 255 << 65535 << true << "DIR" << "DIR(NONE-?)";
        QTest::newRow("bgp-unknown-rtt") << 4 << 2 << 1 << 1 << 65535 << true << "B2" << "BGP(2 hop)";
        QTest::newRow("offline-public-address") << 5 << 0 << 1 << 1 << 50 << false << "offline" << "offline";
        QTest::newRow("indirect-session") << 2 << 1 << 2 << 3 << 50 << true << "IND, RTT=5ms" << "IND RTT=5ms";
    }

    void actualRouteText() {
        QFETCH(int, up); QFETCH(int, hops); QFETCH(int, localNat); QFETCH(int, peerNat); QFETCH(int, rtt); QFETCH(bool, online);
        QFETCH(QString, compact); QFETCH(QString, detail);
        AccountList panel; panel.show(); fill(panel, testChannel, row(21, true, 0, 65535, localNat, 0, true) + row(7, online, up, rtt, peerNat, hops));
        QCOMPARE(panel.model()->data(panel.model()->index(1), Qt::UserRole + 1).toString(), compact);
        QVERIFY(select(panel, 1)); QCOMPARE(body(panel).section('\n', 0, 0), detail);
    }

    void localNatChangeUpdatesSelectedPeer() {
        AccountList panel; panel.show(); fill(panel, testChannel, row(21, true, 0, 65535, 2, 0, true) + row(7, true, 5, 50, 3));
        QVERIFY(select(panel, 1)); QCOMPARE(body(panel).section('\n', 0, 0), QString("DIR(EIM-STRICT) RTT=5ms"));
        size_t previousRequests = requests.size(); const auto changed = row(21, true, 0, 65535, 1, 0, true);
        panel.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(changed.constData()));
        QCOMPARE(body(panel).section('\n', 0, 0), QString("DIR(NONE-STRICT) RTT=5ms")); QCOMPARE(requests.size(), previousRequests);
    }

    void devgroupHeadersAreExpandableAndNeverRepresentADevice() {
        MemberListModel source; source.setChannel(testChannel, false);
        GroupedMemberModel grouped(&source);
        QAbstractItemModelTester tester(&grouped, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto first = row(7, true), second = row(8, false), other = row(9, false);
        for (auto* bytes : {&first, &second}) {
            memset(bytes->data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(bytes->data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "My devices", 11);
        }
        auto data = first + second + other;
        source.onMemberList(testChannel, 3, reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(grouped.rowCount(), 2); QCOMPARE(grouped.sourceRow(0), -1);
        QVERIFY(grouped.index(0).data(GroupedMemberModel::GroupHeaderRole).toBool());
        QVERIFY(grouped.index(0).data(MemberListModel::OnlineRole).toBool());
        QCOMPARE(grouped.index(0).data(GroupedMemberModel::GroupBranchRole).toInt(), 0);
        QCOMPARE(grouped.index(1).data(GroupedMemberModel::GroupBranchRole).toInt(), 0);
        QVERIFY(grouped.toggle(0)); QCOMPARE(grouped.rowCount(), 4);
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(1)), testEd(7));
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(2)), testEd(8));
        QCOMPARE(grouped.index(1).data(GroupedMemberModel::GroupBranchRole).toInt(), 1);
        QCOMPARE(grouped.index(2).data(GroupedMemberModel::GroupBranchRole).toInt(), 2);
        QCOMPARE(grouped.index(3).data(GroupedMemberModel::GroupBranchRole).toInt(), 0);
        source.onMemberRemoved(testChannel, QByteArray(31, '\0') + char(7));
        QVERIFY(!grouped.index(0).data(MemberListModel::OnlineRole).toBool());
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(1)), testEd(8));
        QCOMPARE(grouped.index(1).data(GroupedMemberModel::GroupBranchRole).toInt(), 2);
        QVERIFY(grouped.toggle(0)); QCOMPARE(grouped.rowCount(), 2);
        QCOMPARE(grouped.index(1).data(GroupedMemberModel::GroupBranchRole).toInt(), 0);
    }

    void dmDevicesStaySeparatedAndRejectStaleSnapshots() {
        AccountList panel; panel.resize(420, 600); panel.show(); panel.setChannel("dm:42", 7);
        auto* tree = panel.findChild<QTreeWidget*>("dmDevices"); QVERIFY(tree && tree->isVisible());
        QVERIFY(!panel.findChild<QListView*>()->isVisible());
        auto snapshot = [](quint64 generation, const QByteArray& entries, int count) {
            QByteArray bytes(13, 0); bytes[0] = 2; bytes[1] = '4'; bytes[2] = '2';
            mf_put64(reinterpret_cast<uint8_t*>(bytes.data()) + 3, generation); bytes[11] = count;
            return bytes + entries;
        };
        const auto own = QByteArray(1, 0) + row(21, true, 0, 0xffff, 255, 0, true) + QByteArray(1, 0) + row(7, false);
        const auto peer = QByteArray(1, 1) + row(9, false);
        auto apply = [&](const QByteArray& bytes) { panel.onDmMembers(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size()); };
        apply(snapshot(7, own + peer, 3)); QCOMPARE(tree->topLevelItemCount(), 2);
        QCoreApplication::processEvents(); QVERIFY(tree->height() > panel.height() * 3 / 4);
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("My devices"));
        QCOMPARE(tree->topLevelItem(1)->text(0), QString("Peer devices"));
        QCOMPARE(tree->topLevelItem(0)->childCount(), 2); QCOMPARE(tree->topLevelItem(1)->childCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->child(0)->data(0, Qt::UserRole).toByteArray(), testEd(21));
        QCOMPARE(tree->topLevelItem(0)->child(1)->text(1), QString("offline"));
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_DM_MEMBERS_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(panel.grab().save(screenshot));
        apply(snapshot(6, {}, 0)); QCOMPARE(tree->topLevelItem(0)->childCount(), 2);
        apply(snapshot(7, peer + peer, 2)); QCOMPARE(tree->topLevelItem(0)->childCount(), 2);
        apply(snapshot(7, peer, 1)); QCOMPARE(tree->topLevelItem(0)->childCount(), 0);
        QCOMPARE(tree->topLevelItem(1)->childCount(), 1);
        apply(snapshot(7, {}, 0)); QCOMPARE(tree->topLevelItem(1)->childCount(), 0);
        panel.setChannel(testChannel); QVERIFY(!tree->isVisible()); QVERIFY(panel.findChild<QListView*>()->isVisible());
        apply(snapshot(7, own + peer, 3)); QCOMPARE(tree->topLevelItemCount(), 0);
    }

    void deletedMembersHaveTheirOwnLastGroupAndFollowUpdates() {
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        MemberListModel source; source.setChannel(testChannel, true);
        GroupedMemberModel grouped(&source);
        QAbstractItemModelTester tester(&grouped, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto removed = row(7), active = row(8), ungrouped = row(9);
        for (auto* bytes : {&removed, &active}) {
            memset(bytes->data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(bytes->data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "Devices", 8);
        }
        removed[CHAT_MEMBER_FLAGS_OFF] = CHAT_MEMBER_FLAG_DELETED; ungrouped[CHAT_MEMBER_FLAGS_OFF] = CHAT_MEMBER_FLAG_DELETED;
        const auto bytes = removed + ungrouped + active;
        source.onMemberList(testChannel, 3, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QCOMPARE(grouped.rowCount(), 2);
        QCOMPARE(grouped.index(0).data(Qt::DisplayRole).toString(), QString("Devices"));
        QVERIFY(grouped.index(1).data(GroupedMemberModel::DeletedGroupRole).toBool());
        QVERIFY(!grouped.index(1).data(GroupedMemberModel::GroupExpandedRole).toBool());
        QCOMPARE(grouped.avatarSourceRow(1), -1); QCOMPARE(grouped.actionSourceRow(1), -1);
        QCOMPARE(grouped.index(1).data(MemberListModel::AvatarEdRole).toByteArray(), QByteArray());
        QVERIFY(grouped.index(1).data(MemberListModel::DevgroupRole).toString().isEmpty());
        QVERIFY(!grouped.index(1).data(GroupedMemberModel::CallAvailableRole).toBool());
        QVERIFY(grouped.toggle(1)); QCOMPARE(grouped.rowCount(), 4);
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(2)), testEd(7));
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(3)), testEd(9));
        QCOMPARE(grouped.index(2).data(GroupedMemberModel::GroupBranchRole).toInt(), 1);
        QCOMPARE(grouped.index(3).data(GroupedMemberModel::GroupBranchRole).toInt(), 2);
        QVERIFY(grouped.toggle(0)); QCOMPARE(grouped.rowCount(), 5);
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(1)), testEd(8)); // Удалённый участник не дублируется в devgroup.
        removed[CHAT_MEMBER_FLAGS_OFF] = 0;
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(removed.constData()));
        QCOMPARE(grouped.rowCount(), 5);
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(1)), testEd(7));
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(2)), testEd(8));
        QVERIFY(grouped.index(3).data(GroupedMemberModel::DeletedGroupRole).toBool());
        source.onMemberRemoved(testChannel, QByteArray(31, '\0') + char(9)); QCOMPARE(grouped.rowCount(), 3);
        active[CHAT_MEMBER_FLAGS_OFF] = CHAT_MEMBER_FLAG_DELETED;
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(active.constData()));
        QVERIFY(grouped.index(2).data(GroupedMemberModel::DeletedGroupRole).toBool());
        ready = false; source.setChannel(testChannel, false); QCOMPARE(grouped.rowCount(), 2);
        for (int i = 0; i < grouped.rowCount(); ++i) QVERIFY(!grouped.index(i).data(GroupedMemberModel::DeletedGroupRole).toBool());
    }

    void groupSelectionShowsOnlyLiveMembers() {
        AccountList panel; panel.resize(420, 600); panel.show();
        auto self = row(21, true, 0, 0xffff, 255, 0, true), peer = row(7), foreign = row(9);
        for (auto* bytes : {&self, &peer}) {
            memset(bytes->data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(bytes->data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "My devices", 11);
        }
        memset(foreign.data() + CHAT_MEMBER_DEVGROUP_OFF, 43, 32); memcpy(foreign.data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "Other devices", 14);
        fill(panel, testChannel, peer + foreign + row(8) + self);
        auto* view = panel.findChild<QListView*>(); auto* devices = panel.findChild<QListView*>("devgroupDevices");
        auto* detailHeader = panel.findChild<QLabel*>("memberDetailHeader");
        QVERIFY(devices && detailHeader); QVERIFY(!panel.findChild<QPushButton*>("devgroupRename"));
        QCOMPARE(view->model()->rowCount(), 3);
        auto clickGroup = [&](int row) {
            const QRect rect = view->visualRect(view->model()->index(row, 0));
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(rect.left() + 100, rect.center().y()));
        };
        clickGroup(0);
        QCOMPARE(view->model()->rowCount(), 3); QVERIFY(devices->isVisible()); QVERIFY(!detailHeader->isVisible());
        QCOMPARE(devices->model()->rowCount(), 2);
        QCOMPARE(devices->model()->index(0, 0).data(MemberListModel::AvatarEdRole).toByteArray(), testEd(21));
        QCOMPARE(devices->model()->index(1, 0).data(MemberListModel::AvatarEdRole).toByteArray(), testEd(7));
        QVERIFY(requests.empty()); QVERIFY(header(panel).isEmpty());
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_GROUP_DETAILS_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(panel.grab().save(screenshot));
        QSignalSpy calls(&panel, &AccountList::callRequested);
        const QRect peerRect = devices->visualRect(devices->model()->index(1, 0));
        QTest::mouseClick(devices->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(peerRect.right() - 49, peerRect.center().y()));
        QCOMPARE(calls.size(), 1); QCOMPARE(calls.last()[1].toByteArray(), QByteArray(31, '\0') + char(7)); QVERIFY(!calls.last()[3].toBool());
        clickGroup(0);
        const QRect groupRect = view->visualRect(view->model()->index(0, 0));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(groupRect.left() + 12, groupRect.center().y()));
        QCOMPARE(view->model()->rowCount(), 5); QVERIFY(devices->isVisible()); QVERIFY(!detailHeader->isVisible());
        QCOMPARE(devices->model()->rowCount(), 2);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(groupRect.left() + 12, groupRect.center().y()));
        QCOMPARE(view->model()->rowCount(), 3); QVERIFY(devices->isVisible());
        panel.onMemberRemoved(testChannel, QByteArray(31, '\0') + char(7)); QCOMPARE(devices->model()->rowCount(), 1);
        clickGroup(1); QVERIFY(header(panel).isEmpty()); QVERIFY(!detailHeader->isVisible());
        QCOMPARE(devices->model()->rowCount(), 1);
        QSignalSpy messages(&panel, &AccountList::pmRequested);
        QVERIFY(context(panel, "Send message", {}, 1));
        QCOMPARE(messages.size(), 1); QCOMPARE(messages.last()[0].toString(), QString(testChannel));
        QCOMPARE(messages.last()[1].toByteArray(), QByteArray(31, '\0') + char(9)); QVERIFY(messages.last()[3].toBool());
        QVERIFY(!QApplication::activePopupWidget()); QVERIFY(devgroupRequests.empty());
        panel.onMemberRemoved(testChannel, QByteArray(31, '\0') + char(9)); QVERIFY(!devices->isVisible());
        QVERIFY(detailHeader->isVisible());
        clickGroup(0); QVERIFY(devices->isVisible());
        const QRect memberRect = devices->visualRect(devices->model()->index(0, 0));
        QTest::mouseClick(devices->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(memberRect.left() + 100, memberRect.center().y()));
        QVERIFY(!devices->isVisible()); QVERIFY(detailHeader->isVisible()); QVERIFY(!header(panel).isEmpty());
    }

    void ownGroupContextRenameUsesCoreAndRejectsMembershipChange() {
        AccountList panel; panel.resize(420, 600); panel.show();
        auto self = row(21, true, 0, 0xffff, 255, 0, true), peer = row(7);
        for (auto* bytes : {&self, &peer}) {
            memset(bytes->data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(bytes->data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "My devices", 11);
        }
        fill(panel, testChannel, peer + self);
        QVERIFY(context(panel, "Rename", [&] {
            QTimer::singleShot(0, &panel, [&] {
                auto* dialog = panel.findChild<QInputDialog*>("devgroupRenameDialog"); QVERIFY(dialog);
                QCOMPARE(dialog->textValue(), QString("My devices"));
                dialog->setTextValue(QString(32, QChar(0x044f)));
                QVERIFY(!dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled());
                dialog->setTextValue(QString::fromUtf8("Мои устройства")); dialog->accept();
            });
        }));
        QCOMPARE(devgroupRequests.size(), size_t(1)); QCOMPARE(devgroupRequests.back().action, CHAT_DEVGROUP_RENAME);
        QCOMPARE(QString::fromUtf8(devgroupRequests.back().text), QString::fromUtf8("Мои устройства"));
        QByteArray response(9, 0); mf_put64(reinterpret_cast<uint8_t*>(response.data()), devgroupRequests.back().request_id); response[8] = 1;
        response += "{}"; devgroupCallback(reinterpret_cast<const uint8_t*>(response.constData()), response.size());
        auto* view = panel.findChild<QListView*>();
        const QRect rect = view->visualRect(view->model()->index(0, 0));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(rect.left() + 100, rect.center().y()));
        QVERIFY(!panel.findChild<QPushButton*>("devgroupRename"));
        QVERIFY(context(panel, "Rename", [&] {
            QTimer::singleShot(0, &panel, [&] {
                auto* dialog = panel.findChild<QInputDialog*>("devgroupRenameDialog"); QVERIFY(dialog);
                auto left = row(21, true, 0, 0xffff, 255, 0, true);
                panel.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(left.constData()));
                dialog->setTextValue("Wrong group"); dialog->accept();
            });
        }));
        QCOMPARE(devgroupRequests.size(), size_t(1));
    }

    void selfIsFirstIncludingLateArrivalAndExpandedDeviceGroup() {
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        MemberListModel source; source.setChannel(testChannel, false);
        GroupedMemberModel grouped(&source);
        QAbstractItemModelTester tester(&grouped, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const auto initial = row(7) + row(8);
        source.onMemberList(testChannel, 2, reinterpret_cast<const uint8_t*>(initial.constData()), initial.size());
        auto self = row(21, true, 0, 0xffff, 255, 0, true);
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(self.constData()));
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(0)), testEd(21));
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(1)), testEd(7));
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(2)), testEd(8));
        auto peer = row(7);
        for (auto* bytes : {&self, &peer}) {
            memset(bytes->data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(bytes->data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "My devices", 11);
        }
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(peer.constData()));
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(self.constData()));
        QCOMPARE(grouped.rowCount(), 2);
        QVERIFY(grouped.index(0).data(GroupedMemberModel::GroupHeaderRole).toBool());
        QCOMPARE(source.memberKeyAt(grouped.avatarSourceRow(0)), testEd(21));
        QCOMPARE(grouped.index(0).data(GroupedMemberModel::ActionPeerRole).toByteArray(), testEd(21));
        QVERIFY(grouped.toggle(0)); QCOMPARE(grouped.rowCount(), 4);
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(1)), testEd(21));
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(2)), testEd(7));
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(3)), testEd(8));
        QCOMPARE(grouped.index(1).data(GroupedMemberModel::GroupBranchRole).toInt(), 1);
        QCOMPARE(grouped.index(2).data(GroupedMemberModel::GroupBranchRole).toInt(), 2);
        // Полный refresh с self в середине сохраняет раскрытие и порядок остальных.
        const auto refresh = row(8) + self + peer;
        source.onMemberList(testChannel, 3, reinterpret_cast<const uint8_t*>(refresh.constData()), refresh.size());
        QCOMPARE(grouped.rowCount(), 4);
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(1)), testEd(21));
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(2)), testEd(7));
        source.onMemberRemoved(testChannel, QByteArray(31, '\0') + char(21));
        QCOMPARE(grouped.rowCount(), 3);
        QCOMPARE(source.memberKeyAt(grouped.sourceRow(0)), testEd(8));
        QCOMPARE(grouped.index(1).data(GroupedMemberModel::ActionPeerRole).toByteArray(), testEd(7));
    }

    void singleDeviceBranchIsVisible_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("actual-dark-palette") << true;
    }

    void singleDeviceBranchIsVisible() {
        QFETCH(bool, dark);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        const QPalette previous = QApplication::palette();
        const auto restorePalette = qScopeGuard([&] { QApplication::setPalette(previous); });
        QPalette palette = previous;
        palette.setColor(QPalette::Window, dark ? QColor(32, 32, 32) : QColor(240, 240, 240));
        palette.setColor(QPalette::WindowText, dark ? QColor(220, 220, 220) : Qt::black);
        palette.setColor(QPalette::Base, dark ? QColor(24, 24, 24) : Qt::white);
        palette.setColor(QPalette::AlternateBase, dark ? QColor(38, 38, 38) : QColor(247, 247, 247));
        palette.setColor(QPalette::Text, dark ? QColor(220, 220, 220) : Qt::black);
        palette.setColor(QPalette::Mid, dark ? QColor(30, 30, 30) : QColor(160, 160, 160));
        QApplication::setPalette(palette);
        AccountList panel; panel.resize(380, 240); panel.show();
        auto* view = panel.findChild<QListView*>();
        auto device = row(7);
        memset(device.data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(device.data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "My devices", 11);
        fill(panel, testChannel, device + row(8));
        QTest::qWait(10);
        const QRect header = view->visualRect(view->model()->index(0, 0));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(header.left() + 12, header.center().y()));
        QCOMPARE(view->model()->rowCount(), 3);
        QCOMPARE(view->model()->index(1, 0).data(GroupedMemberModel::GroupBranchRole).toInt(), 2);
        const QRect child = view->visualRect(view->model()->index(1, 0));
        const QImage image = view->viewport()->grab().toImage();
        const QColor background = palette.color(QPalette::AlternateBase);
        auto contrast = [&](int x, int y) { return qAbs(image.pixelColor(x, y).lightness() - background.lightness()); };
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "single device branch: dark=%d background=%s mid=%s text=%s vertical_contrast=%d corner_contrast=%d",
            dark, qPrintable(background.name()), qPrintable(palette.color(QPalette::Mid).name()),
            qPrintable(palette.color(QPalette::Text).name()), contrast(child.left() + 12, child.top() + 4),
            contrast(child.left() + 18, child.center().y()));
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_SINGLE_GROUP_SCREENSHOT");
        if (!screenshot.isEmpty() && dark) QVERIFY(panel.grab().save(screenshot));
        QVERIFY2(contrast(child.left() + 12, child.top() + 4) >= 40, "Vertical branch must be visible against the row background");
        QVERIFY2(contrast(child.left() + 18, child.center().y()) >= 40, "Final corner must be visible against the row background");
        QCOMPARE(contrast(child.left() + 12, child.bottom() - 4), 0); // Линия заканчивается на единственном устройстве.
    }

    void devgroupAvatarUsesFirstPublishedPhotoAndKeepsHeaderIdentity() {
        MemberListModel source; source.setChannel(testChannel, true); GroupedMemberModel grouped(&source);
        auto first = row(7), second = row(8, false), third = row(9);
        for (auto* bytes : {&first, &second, &third}) {
            memset(bytes->data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(bytes->data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "My devices", 11);
        }
        auto setPhoto = [](QByteArray& bytes, bool present) {
            chat_member_display member{};
            QCOMPARE(chat_core_deserialize_member(reinterpret_cast<const uint8_t*>(bytes.constData()), &member), 0);
            member.avatar_server[0] = present ? 99 : 0;
            snprintf(member.avatar_file, sizeof(member.avatar_file), "%s", present ? "photo.json" : "");
            chat_core_serialize_member(&member, reinterpret_cast<uint8_t*>(bytes.data()));
        };
        setPhoto(second, true); setPhoto(third, true);
        memcpy(second.data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "Other name", 11);
        const auto bytes = first + second + third;
        source.onMemberList(testChannel, 3, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        const auto header = grouped.index(0);
        QCOMPARE(header.data(MemberListModel::AvatarEdRole).toByteArray(), QByteArray(31, '\0') + char(8));
        QCOMPARE(header.data(GroupedMemberModel::ActionPeerRole).toByteArray(), testEd(7));
        QCOMPARE(header.data(Qt::DisplayRole).toString(), QString("My devices"));
        second[CHAT_MEMBER_FLAGS_OFF] = CHAT_MEMBER_FLAG_DELETED;
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(second.constData()));
        QCOMPARE(grouped.index(0).data(MemberListModel::AvatarEdRole).toByteArray(), QByteArray(31, '\0') + char(9));
        setPhoto(third, false); source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(third.constData()));
        QCOMPARE(grouped.index(0).data(MemberListModel::AvatarEdRole).toByteArray(), QByteArray(31, '\0') + char(7));
        setPhoto(first, true); source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(first.constData()));
        QCOMPARE(grouped.index(0).data(MemberListModel::AvatarEdRole).toByteArray(), QByteArray(31, '\0') + char(7));
    }

    void devgroupControlsFollowFirstOnlineDeviceAndKeepFirstAvatar() {
        AccountList panel; panel.resize(380, 350); panel.show();
        auto first = row(7, false), second = row(8), third = row(9);
        for (auto* bytes : {&first, &second, &third}) {
            memset(bytes->data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(bytes->data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "My devices", 11);
        }
        fill(panel, testChannel, first + second + third + row(10));
        auto* view = panel.findChild<QListView*>();
        auto* grouped = qobject_cast<GroupedMemberModel*>(view->model()); QVERIFY(grouped);
        QSignalSpy calls(&panel, &AccountList::callRequested), radioCalls(&panel, &AccountList::radioCallRequested);
        const auto group = grouped->index(0);
        QCOMPARE(group.data(Qt::DisplayRole).toString(), QString("My devices"));
        QCOMPARE(group.data(GroupedMemberModel::ActionPeerRole).toByteArray(), testEd(8));
        QCOMPARE(grouped->avatarSourceRow(0), 0); QCOMPARE(grouped->sourceRow(0), -1);
        const auto avatar = qvariant_cast<QIcon>(group.data(Qt::DecorationRole)).pixmap(34, 34).toImage();
        QCOMPARE(avatar, qvariant_cast<QIcon>(panel.model()->index(0).data(Qt::DecorationRole)).pixmap(34, 34).toImage());
        QTest::qWait(10);
        const QRect rect = view->visualRect(group);
        const QPoint call(rect.right() - 49, rect.center().y()), radio(rect.right() - 81, rect.center().y());
        const QPoint mute(rect.right() - 17, rect.center().y());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, call);
        QCOMPARE(calls.size(), 1); QCOMPARE(calls.last()[1].toByteArray(), QByteArray(31, '\0') + char(8));
        QCOMPARE(calls.last()[2].toString(), QString("My devices"));
        QVERIFY(calls.last()[3].toBool());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, radio);
        QCOMPARE(radioCalls.size(), 1); QCOMPARE(radioCalls.last()[1].toByteArray(), QByteArray(31, '\0') + char(8));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, mute);
        QCOMPARE(muteRequests.size(), size_t(1)); QCOMPARE(muteRequests.back(), QByteArray(31, '\0') + char(8));
        QCOMPARE(grouped->rowCount(), 2); // Кнопки не раскрывают группу.
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, call);
        second[CHAT_MEMBER_ONLINE_OFF] = 0; // Устройство уходит offline между press и release.
        panel.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(second.constData()));
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, call);
        QCOMPARE(calls.size(), 1);
        QCOMPARE(group.data(GroupedMemberModel::ActionPeerRole).toByteArray(), testEd(9));
        QCOMPARE(qvariant_cast<QIcon>(group.data(Qt::DecorationRole)).pixmap(34, 34).toImage(), avatar);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, call);
        QCOMPARE(calls.size(), 2); QCOMPARE(calls.last()[1].toByteArray(), QByteArray(31, '\0') + char(9));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(rect.left() + 12, rect.center().y()));
        QCOMPARE(grouped->rowCount(), 5); QVERIFY(grouped->index(0).data(GroupedMemberModel::GroupExpandedRole).toBool());
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_GROUP_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(panel.grab().save(screenshot));
        third[CHAT_MEMBER_ONLINE_OFF] = 0; panel.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(third.constData()));
        QCOMPARE(grouped->index(0).data(GroupedMemberModel::ActionPeerRole).toByteArray(), QByteArray());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, call);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, mute);
        QCOMPARE(calls.size(), 2); QCOMPARE(muteRequests.size(), size_t(1));
    }

    void devgroupIndicatorsCombineDevicesEvenWhenFirstOnlineIsSelf() {
        MemberListModel source; source.setChannel(testChannel, true);
        GroupedMemberModel grouped(&source);
        auto self = row(7, true, 0, 0xffff, 255, 0, true), busy = row(8), removed = row(9);
        for (auto* bytes : {&self, &busy, &removed}) {
            memset(bytes->data() + CHAT_MEMBER_DEVGROUP_OFF, 42, 32); memcpy(bytes->data() + CHAT_MEMBER_DEVGROUP_NAME_OFF, "My devices", 11);
        }
        const int active = CHAT_MEMBER_FLAG_RADIO | CHAT_MEMBER_FLAG_PHONE_KNOWN | CHAT_MEMBER_FLAG_PHONE_BUSY;
        busy[CHAT_MEMBER_FLAGS_OFF] = char(active | CHAT_MEMBER_FLAG_RADIO_MUTED);
        removed[CHAT_MEMBER_FLAGS_OFF] = char(active | CHAT_MEMBER_FLAG_DELETED);
        const auto bytes = self + busy + removed;
        source.onMemberList(testChannel, 3, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        const auto header = grouped.index(0);
        QVERIFY(header.data(Qt::UserRole + 2).toBool());
        QCOMPARE(header.data(Qt::UserRole + 3).toInt(), active); // mute относится только к цели кнопок.
        AccountDelegate delegate;
        QStyleOptionViewItem option; option.rect = QRect(0, 0, 340, MemberListModel::kRowHeight);
        const QColor background("#FFFFFF");
        option.palette.setColor(QPalette::AlternateBase, background);
        auto image = [&] {
            QImage result(option.rect.size(), QImage::Format_RGB32); result.fill(background);
            QPainter painter(&result); delegate.paint(&painter, option, header); painter.end(); return result;
        };
        auto colors = [](const QImage& image, int rightOffset, const QColor& color) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = image.width() - rightOffset - 1; x < image.width() - rightOffset + 27; ++x)
                    count += image.pixelColor(x, y) == color;
            return count;
        };
        const auto activeImage = image();
        QVERIFY(colors(activeImage, 95, QColor("#4CAF50")) > 0);
        QVERIFY(colors(activeImage, 63, QColor("#E53935")) > 0);
        QVERIFY(colors(activeImage, 31, QColor("#9E9E9E")) > 0);
        QSignalSpy calls(&delegate, &AccountDelegate::callRequested), toggles(&delegate, &AccountDelegate::groupToggleRequested);
        for (int offset : {81, 49, 17}) {
            const QPoint point(option.rect.right() - offset, option.rect.center().y());
            QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            delegate.editorEvent(&press, &grouped, option, header); delegate.editorEvent(&release, &grouped, option, header);
        }
        QCOMPARE(calls.size(), 1); QCOMPARE(toggles.size(), 0); // Заголовок вызывает другие свои устройства; mute self не действует.
        busy[CHAT_MEMBER_FLAGS_OFF] = CHAT_MEMBER_FLAG_PHONE_KNOWN;
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(busy.constData()));
        QCOMPARE(header.data(Qt::UserRole + 3).toInt(), int(CHAT_MEMBER_FLAG_PHONE_KNOWN));
        QCOMPARE(colors(image(), 63, QColor("#E53935")), 0); // Удалённый busy-аккаунт не влияет на группу.
        QCOMPARE(colors(image(), 95, QColor("#4CAF50")), 0); // self не окрашивает выключенную рацию в зелёный.
        self[CHAT_MEMBER_ONLINE_OFF] = 0; busy[CHAT_MEMBER_ONLINE_OFF] = 0;
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(self.constData()));
        source.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(busy.constData()));
        QVERIFY(!header.data(MemberListModel::OnlineRole).toBool());
        const auto offlineImage = image();
        for (int offset : {95, 63}) QCOMPARE(colors(offlineImage, offset, background), 28 * offlineImage.height());
    }

    void flatModelPreservesRowsSnapshotsAndPersistentIndexes() {
        MemberListModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setChannel(testChannel, false);
        auto bytes = row(7) + row(8) + row(9);
        model.onMemberList(testChannel, 3, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QCOMPARE(model.rowCount(model.index(0)), 0);
        const QPersistentModelIndex last = model.index(2);
        const auto snapshot = model.member(testEd(9)); QVERIFY(snapshot);
        model.onMemberRemoved(testChannel, QByteArray(31, '\0') + char(7));
        QCOMPARE(last.row(), 1); QCOMPARE(model.memberKeyAt(last.row()), testEd(9));
        const auto changed = row(9, false, 0, 0xffff);
        model.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(changed.constData()));
        QVERIFY(snapshot->online); QVERIFY(!model.member(testEd(9))->online);
        const auto added = row(10);
        model.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(added.constData()));
        QCOMPARE(model.rowCount(), 3); QCOMPARE(model.memberKeyAt(2), testEd(10));
        QVERIFY(!model.member(testEd(7))); QCOMPARE(model.memberKeyAt(-1), QByteArray());
    }

    void snapshotChangedSeesNewChannel() {
        MemberListModel model; model.setChannel(testChannel, false);
        memberRequests.clear();
        connect(&model, &MemberListModel::snapshotChanged, &model, [&] { model.refresh(); });
        model.setChannel(otherChannel, false);
        QCOMPARE(memberRequests.size(), size_t(2));
        for (const auto& request : memberRequests) QCOMPARE(QString::fromUtf8(request.ch_id), QString(otherChannel));
    }

    void losingOwnerPolicyHidesDeletedRowsWithoutCore() {
        MemberListModel model; model.setChannel(testChannel, true);
        auto bytes = row(7) + row(8);
        bytes[CHAT_MEMBER_DISPLAY_SIZE + CHAT_MEMBER_FLAGS_OFF] = CHAT_MEMBER_FLAG_DELETED;
        model.onMemberList(testChannel, 2, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QCOMPARE(model.rowCount(), 2);
        ready = false; model.setChannel(testChannel, false);
        QCOMPARE(model.rowCount(), 1); QVERIFY(!model.member(testEd(8)));
    }

    void malformedUpdatesKeepExistingSnapshot() {
        MemberListModel model; model.setChannel(testChannel, false);
        const auto bytes = row(7);
        model.onMemberList(testChannel, 1, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        model.onMemberList(testChannel, 2, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        model.onMemberList(testChannel, -1, nullptr, -1);
        model.onMemberUpdated(testChannel, nullptr);
        QCOMPARE(model.rowCount(), 1); QCOMPARE(model.memberKeyAt(0), testEd(7));
    }

    void rejectedMemberRequestsReleaseArguments() {
        const auto before = u_get_allocated_count();
        rejectOwned = true;
        MemberListModel model; model.setChannel(testChannel, false);
        model.refresh(); model.refreshRtt();
        QCOMPARE(ownedPosts, 3);
        QCOMPARE(u_get_allocated_count(), before);
    }

    void invalidChannelDoesNotReplaceVisibleSnapshotOrSendRequests() {
        MemberListModel model; model.setChannel(testChannel, false);
        const auto bytes = row(7);
        model.onMemberList(testChannel, 1, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        memberRequests.clear();
        model.setChannel(QString(65, '1'), false);
        model.setChannel(QString::fromLatin1(testChannel) + QChar(u'\0') + QStringLiteral("suffix"), false);
        QCOMPARE(model.rowCount(), 1); QCOMPARE(model.memberKeyAt(0), testEd(7));
        QVERIFY(memberRequests.empty());
        model.refresh(); QCOMPARE(memberRequests.size(), size_t(1));
        QCOMPARE(QString::fromUtf8(memberRequests.back().ch_id), QString(testChannel));
    }

    void twoPanelsReceiveTheirRepliesAndSurviveOtherPanelDestruction() {
        AccountList first; first.show(); fill(first, testChannel, row(7)); QVERIFY(select(first, 0));
        const auto firstRequest = requests.back();
        auto second = std::make_unique<AccountList>(); second->show(); fill(*second, otherChannel, row(8)); QVERIFY(select(*second, 0));
        const auto secondRequest = requests.back();
        deliver(reply(firstRequest, {7, 111})); QVERIFY(body(first).contains("000000000000006f"));
        deliver(reply(secondRequest, {8, 222})); QVERIFY(body(*second).contains("00000000000000de"));
        second.reset();
        QVERIFY(QMetaObject::invokeMethod(&first, "onDetailTimerTick"));
        deliver(reply(requests.back(), {7, 333})); QVERIFY(body(first).contains("000000000000014d"));
    }

    void rejectedDetailRequestCanRetryImmediately() {
        AccountList panel; panel.show(); fill(panel, testChannel, row(7));
        rejectOwned = true; QVERIFY(select(panel, 0)); QVERIFY(requests.empty());
        rejectOwned = false; QVERIFY(QMetaObject::invokeMethod(&panel, "onDetailTimerTick"));
        QCOMPARE(requests.size(), size_t(1));
        deliver(reply(requests.back(), {7, 8})); QVERIFY(body(panel).contains("BGP:"));
    }

    void detailEscapesSocketNamesAndFormatsIpv6Ports() {
        AccountList panel; panel.show(); fill(panel, testChannel, row(7)); QVERIFY(select(panel, 0));
        auto bytes = reply(requests.back()); bytes.resize(CHAT_MEMBER_DETAIL_HEADER_SIZE + CHAT_MEMBER_DETAIL_LINK_SIZE);
        auto* data = reinterpret_cast<uint8_t*>(bytes.data()); data[CHAT_MEMBER_DETAIL_LINK_COUNT_OFF] = 1;
        auto* link = data + CHAT_MEMBER_DETAIL_HEADER_SIZE; memset(link, 0, CHAT_MEMBER_DETAIL_LINK_SIZE);
        link[0] = 1; link[1] = 3; link[2] = 1; link[3] = 1;
        link[4] = 6; link[20] = 1; link[21] = 0x35; link[22] = 5;
        link[23] = 6; link[39] = 2; link[40] = 0xbb; link[41] = 1;
        strcpy(reinterpret_cast<char*>(link + 42), "<sock&>");
        strcpy(reinterpret_cast<char*>(link + 62), "TCP+REALITY");
        deliver(bytes);
        QVERIFY(body(panel).contains("[::1]:1333")); QVERIFY(body(panel).contains("[::2]:443"));
        QVERIFY(body(panel).contains("<sock&>"));
    }

    void bleHasNoIpPortsAndUsesPhysicalDirection() {
        for (int direction = 0; direction < 3; direction++) {
            AccountList panel; panel.show(); fill(panel, testChannel, row(7)); QVERIFY(select(panel, 0));
            auto bytes = reply(requests.back()); bytes.resize(CHAT_MEMBER_DETAIL_HEADER_SIZE + CHAT_MEMBER_DETAIL_LINK_SIZE);
            auto* data = reinterpret_cast<uint8_t*>(bytes.data()); data[CHAT_MEMBER_DETAIL_LINK_COUNT_OFF] = 1;
            auto* link = data + CHAT_MEMBER_DETAIL_HEADER_SIZE; memset(link, 0, CHAT_MEMBER_DETAIL_LINK_SIZE);
            link[0] = direction; link[1] = 3; link[2] = 1; strcpy(reinterpret_cast<char*>(link + 62), "BLE");
            deliver(bytes);
            const QString text = body(panel);
            QVERIFY(text.contains("BLE")); QVERIFY(!text.contains("UDP")); QVERIFY(!text.contains(":0"));
            QCOMPARE(text.contains("▶"), direction == 1); QCOMPARE(text.contains("◀"), direction == 2);
            // Неверное направление или имя в пакете не заменяет предыдущую карточку.
            QVERIFY(QMetaObject::invokeMethod(&panel, "onDetailTimerTick"));
            auto invalid = bytes;
            auto* header = reinterpret_cast<uint8_t*>(invalid.data()); mf_put64(header, requests.back().request_id);
            header[CHAT_MEMBER_DETAIL_HEADER_SIZE] = 3; deliver(invalid); QCOMPARE(body(panel), text);
        }
    }

    void contextWithoutDatabaseUsesSnapshotAndOwnsConnectRequest() {
        AccountList panel; panel.resize(300, 600); panel.show(); fill(panel, testChannel, row(7));
        QSignalSpy calls(&panel, &AccountList::callRequested);
        QVERIFY(context(panel, "Call peer")); QCOMPARE(calls.size(), 1);
        QCOMPARE(calls.first()[0].toString(), QString(testChannel)); QCOMPARE(calls.first()[1].toByteArray(), QByteArray(31, '\0') + char(7));
        QVERIFY(!context(panel, "Rename")); QVERIFY(!context(panel, "Edit"));
        const auto before = u_get_allocated_count();
        rejectOwned = true; QVERIFY(context(panel, "Connect")); QCOMPARE(u_get_allocated_count(), before);
        QVERIFY(connectRequests.empty());
        rejectOwned = false; QVERIFY(context(panel, "Connect")); QCOMPARE(connectRequests.size(), size_t(1));
        QCOMPARE(QString::fromUtf8(connectRequests.back().ch_id), QString(testChannel)); QCOMPARE(QByteArray(reinterpret_cast<const char*>(connectRequests.back().ed), 32), QByteArray(31, '\0') + char(7));
    }

    void contextRevalidatesChannelMemberAndOnlineState() {
        AccountList panel; panel.resize(300, 600); panel.show(); fill(panel, testChannel, row(7));
        QSignalSpy calls(&panel, &AccountList::callRequested);
        QVERIFY(context(panel, "Call peer", [&] {
            const auto offline = row(7, false);
            panel.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(offline.constData()));
        }));
        QCOMPARE(calls.size(), 0);
        fill(panel, testChannel, row(7));
        QVERIFY(context(panel, "Call peer", [&] { fill(panel, otherChannel, row(8)); }));
        QCOMPARE(calls.size(), 0);
        fill(panel, testChannel, row(7));
        QVERIFY(context(panel, "Call peer", [&] { panel.onMemberRemoved(testChannel, QByteArray(31, '\0') + char(7)); }));
        QCOMPARE(calls.size(), 0);
        fill(panel, testChannel, row(7) + row(8));
        QVERIFY(context(panel, "Call peer", [&] { fill(panel, testChannel, row(8) + row(7)); }));
        QCOMPARE(calls.size(), 1); QCOMPARE(calls.first()[1].toByteArray(), QByteArray(31, '\0') + char(7));
    }

    void destroyingPanelDuringContextMenuIsSafe() {
        auto panel = std::make_unique<AccountList>(); panel->show(); fill(*panel, testChannel, row(7));
        QPointer<AccountList> weak(panel.get());
        QVERIFY(!context(*panel, "Call peer", [&] { panel.reset(); }));
        QVERIFY(weak.isNull());
    }

    void destroyingBorrowedDatabaseDisablesEditing() {
        auto database = std::make_unique<DbManager>();
        AccountList panel(database.get()); panel.show(); fill(panel, testChannel, row(7));
        database.reset();
        QVERIFY(QMetaObject::invokeMethod(&panel, "onMemberDoubleClicked", Qt::DirectConnection,
                                        Q_ARG(QModelIndex, panel.model()->index(0))));
        QVERIFY(!context(panel, "Edit"));
    }

    void unavailableCoreDiscardsPendingTokenBeforeRetry() {
        AccountList panel; panel.show(); fill(panel, testChannel, row(7)); QVERIFY(select(panel, 0));
        const auto old = requests.back();
        ready = false; QVERIFY(QMetaObject::invokeMethod(&panel, "onDetailTimerTick"));
        ready = true; QVERIFY(QMetaObject::invokeMethod(&panel, "onDetailTimerTick"));
        QCOMPARE(requests.size(), size_t(2));
        deliver(reply(old, {111})); QVERIFY(!body(panel).contains("BGP:"));
        deliver(reply(requests.back(), {7, 222})); QVERIFY(body(panel).contains("00000000000000de"));
    }

    void interfaceNamesBelongOnlyToOwnAddresses() {
        const auto previous = g_debug_config.category_levels[DEBUG_CATEGORY_CONNECTION];
        const auto restore = qScopeGuard([&] { debug_set_category_level(DEBUG_CATEGORY_CONNECTION, previous); });
        debug_set_category_level(DEBUG_CATEGORY_CONNECTION, DEBUG_LEVEL_TRACE);
        sqlite3* database = nullptr; QCOMPARE(sqlite3_open(":memory:", &database), SQLITE_OK);
        const auto close = qScopeGuard([&] { sqlite3_close(database); });
        const char* schema = "CREATE TABLE nodes(node_id,last_seen_at,created_at,ed25519_pubkey,x25519_pubkey);"
            "INSERT INTO nodes VALUES(7,0,0,x'0000000000000000000000000000000000000000000000000000000000000007', x'0000000000000000000000000000000000000000000000000000000000000007'),(8,0,0,x'0000000000000000000000000000000000000000000000000000000000000008', x'0000000000000000000000000000000000000000000000000000000000000008');"
            "CREATE TABLE node_addresses(node_x,family,protocol,address,port,socket_id,addr_type,nat_mapping,nat_inbound,nat_source);"
            "INSERT INTO node_addresses VALUES(x'0000000000000000000000000000000000000000000000000000000000000007',4,1,x'c0000201',1333,1,0,0,0,0);"
            "INSERT INTO node_addresses VALUES(x'0000000000000000000000000000000000000000000000000000000000000008',4,1,x'c0000202',2444,1,0,0,0,0);";
        QCOMPARE(sqlite3_exec(database, schema, nullptr, nullptr, nullptr), SQLITE_OK);
        DbManager manager; manager.setDb(database);
        AccountList panel(&manager); panel.show();
        fill(panel, testChannel, row(7, true, 5, 30, 255, 0, true) + row(8));
        // Одинаковый socket_id на двух узлах: снимок содержит только локальный интерфейс.
        for (int selected : {0, 1, 0}) {
            QVERIFY(select(panel, selected));
            auto bytes = reply(requests.back());
            bytes.append(QByteArray(CHAT_MEMBER_DETAIL_SOCK_SIZE, 0));
            auto* data = reinterpret_cast<uint8_t*>(bytes.data()); data[CHAT_MEMBER_DETAIL_SOCK_COUNT_OFF] = 1;
            auto* socket = data + CHAT_MEMBER_DETAIL_HEADER_SIZE;
            socket[0] = 1; socket[4] = 0x35; socket[5] = 5;
            strcpy(reinterpret_cast<char*>(socket + 10), "ethernet_32778");
            deliver(bytes);
            const auto text = body(panel);
            DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "member interface ownership: selected=%d card=%s", selected, qPrintable(text));
            QCOMPARE(text.contains("ethernet_32778"), selected == 0);
            QVERIFY(text.contains("[sock1] UDP"));
            QVERIFY(text.contains(selected == 0 ? "192.0.2.1:1333" : "192.0.2.2:2444"));
        }
    }

    void malformedStoredAddressDoesNotReadOutsideBlob() {
        sqlite3* database = nullptr; QCOMPARE(sqlite3_open(":memory:", &database), SQLITE_OK);
        const auto close = qScopeGuard([&] { sqlite3_close(database); });
        DbManager manager;
        AccountList panel(&manager); panel.show(); fill(panel, testChannel, row(7));
        const char* schema = "CREATE TABLE nodes(node_id,last_seen_at,created_at,ed25519_pubkey,x25519_pubkey);"
            "INSERT INTO nodes VALUES(7,0,0,x'0000000000000000000000000000000000000000000000000000000000000007', x'0000000000000000000000000000000000000000000000000000000000000007');"
            "CREATE TABLE node_addresses(node_x,family,protocol,address,port,socket_id,addr_type,nat_mapping,nat_inbound,nat_source);"
            "INSERT INTO node_addresses VALUES(x'0000000000000000000000000000000000000000000000000000000000000007',6,2,x'01',555,1,1,0,0,0);"
            "INSERT INTO node_addresses VALUES(x'0000000000000000000000000000000000000000000000000000000000000007',4,2,x'7f000001',444,2,1,0,0,0);";
        QCOMPARE(sqlite3_exec(database, schema, nullptr, nullptr, nullptr), SQLITE_OK);
        manager.setDb(database);
        QVERIFY(select(panel, 0));
        QVERIFY(body(panel).contains("127.0.0.1:444"));
        QVERIFY(!body(panel).contains(":555"));
    }

    void modelOwnsPresenceAndRttAndRefreshPreservesSelection() {
        AccountList panel; panel.resize(600, 800); panel.show();
        fill(panel, testChannel, row(7, false, 0, 0xffff) + row(8));
        QVERIFY(select(panel, 0)); QCOMPARE(requests.size(), size_t(1));
        deliver(reply(requests.back())); // Физический транспорт UP не делает участника онлайн.
        QVERIFY(header(panel).contains("#9E9E9E")); QVERIFY(body(panel).contains("offline"));
        const auto live = row(7, true, 4, 93);
        panel.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(live.constData()));
        QVERIFY(header(panel).contains("#4CAF50")); QVERIFY(body(panel).contains("RTT=9.3ms"));
        deliver(reply(requests.back(), {7, 9})); QVERIFY(body(panel).contains("0000000000000009"));
        const auto other = row(7, false, 0, 0xffff);
        panel.onMemberUpdated(otherChannel, reinterpret_cast<const uint8_t*>(other.constData()));
        QVERIFY(header(panel).contains("#4CAF50"));
        panel.refresh(); QCOMPARE(panel.model()->rowCount(), 2); QVERIFY(body(panel).contains("RTT=9.3ms"));
        const auto reordered = row(8) + live;
        panel.onMemberList(testChannel, 2, reinterpret_cast<const uint8_t*>(reordered.constData()), reordered.size());
        QVERIFY(header(panel).contains("peer7"));
        QCOMPARE(panel.findChild<QListView*>()->currentIndex().row(), 1);
    }

    void aToBToADiscardsOldResponsesWithoutClearingNewRequest() {
        AccountList panel; panel.show(); fill(panel, testChannel, row(7) + row(8));
        QVERIFY(select(panel, 0)); const auto first = requests.back();
        QVERIFY(select(panel, 1)); const auto second = requests.back();
        QVERIFY(select(panel, 0)); const auto third = requests.back();
        QVERIFY(first.request_id < second.request_id && second.request_id < third.request_id);
        deliver(reply(first, {111})); deliver(reply(second, {222}));
        QVERIFY(!body(panel).contains("BGP:"));
        auto wrongGroup = third; wrongGroup.group[31] ^= 1;
        deliver(reply(wrongGroup, {333})); QVERIFY(!body(panel).contains("BGP:"));
        auto wrongKey = third; memcpy(wrongKey.ed, second.ed, 32);
        deliver(reply(wrongKey, {333})); QVERIFY(!body(panel).contains("BGP:"));
        deliver(reply(third, {7, 444})); QVERIFY(body(panel).contains("00000000000001bc"));
        const auto pending = requests.size();
        QVERIFY(QMetaObject::invokeMethod(&panel, "onDetailTimerTick")); QCOMPARE(requests.size(), pending + 1);
        const auto oldChannel = requests.back();
        fill(panel, otherChannel, row(7)); QVERIFY(select(panel, 0)); const auto current = requests.back();
        deliver(reply(oldChannel, {555})); QVERIFY(!body(panel).contains("BGP:"));
        deliver(reply(current, {7, 666})); QVERIFY(body(panel).contains("000000000000029a"));
    }

    void newMemberEventInvalidatesPendingPathEvenWhenFieldsAreEqual() {
        AccountList panel; panel.show(); const auto initial = row(7);
        fill(panel, testChannel, initial); QVERIFY(select(panel, 0)); const auto first = requests.back();
        panel.onMemberUpdated(testChannel, reinterpret_cast<const uint8_t*>(initial.constData()));
        QCOMPARE(requests.size(), size_t(1));
        deliver(reply(first, {111})); QCOMPARE(requests.size(), size_t(2)); QVERIFY(!body(panel).contains("BGP:"));
        const auto correct = reply(requests.back(), {7, 8});
        deliver(correct.left(correct.size() - 1)); QVERIFY(!body(panel).contains("BGP:"));
        deliver(correct); QVERIFY(body(panel).contains("0000000000000008"));
        panel.onMemberRemoved(testChannel, QByteArray(31, '\0') + char(7)); QVERIFY(!header(panel).contains("peer7"));
        deliver(correct); QVERIFY(!body(panel).contains("BGP:"));
    }

    void errorTimeoutAndHiddenPanelReleaseOnlyTheirOwnRequests() {
        AccountList panel; panel.show(); fill(panel, testChannel, row(7)); QVERIFY(select(panel, 0));
        deliver(reply(requests.back(), {}, CHAT_MEMBER_DETAIL_NOT_MEMBER));
        QVERIFY(header(panel).contains("#4CAF50")); // Диагностическая ошибка не меняет live-снимок.
        QVERIFY(QMetaObject::invokeMethod(&panel, "onDetailTimerTick")); const auto expired = requests.back();
        QTRY_VERIFY_WITH_TIMEOUT(requests.back().request_id != expired.request_id, 6500);
        const auto current = requests.back(); deliver(reply(expired, {111}));
        deliver(reply(current, {7, 8})); QVERIFY(body(panel).contains("BGP:"));
        QVERIFY(QMetaObject::invokeMethod(&panel, "onDetailTimerTick")); const auto hidden = requests.back();
        panel.hide(); deliver(reply(hidden, {222})); panel.show();
        QVERIFY(requests.back().request_id > hidden.request_id);
        deliver(reply(hidden, {222})); QVERIFY(!body(panel).contains("00000000000000de"));
        ready = false; panel.setChannel(QString()); deliver(reply(requests.back(), {333}));
        QCOMPARE(panel.model()->rowCount(), 0); QVERIFY(!body(panel).contains("BGP:"));
    }
};

QTEST_MAIN(MemberDetailTest)
#include "test_member_detail.moc"

void gui_bridge_set_devgroup_result_cb(gui_dm_messages_fn cb) { devgroupCallback = cb; }
