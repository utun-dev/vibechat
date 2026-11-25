/* GUI получает готовую ссылку: выбор LAN-устройства не пересобирает её и ACK не означает JOIN. */
#include "../src/invitedialog.h"
#include "../transport/gui_bridge.h"
#include "../db/db_manager.h"
#include "../../../src/chat/chat_local.h"
#include "../../../src/chat/invite_build.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include <QtTest>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QDataStream>
#include <QtEndian>

static gui_local_event_fn devices_cb, delivery_cb;
static gui_invite_link_ready_fn link_cb;
static gui_invite_candidates_fn candidates_cb;
static uint64_t link_request, revoked_request;
static struct chat_local_send_request sent;
static int sends;

QByteArray DbReadView::localEd25519() const { return {}; }
QString DbReadView::getDisplayName(const QByteArray& ed, const QString&) const { return QString::fromLatin1(ed.toHex()); }
struct UTUN_INSTANCE* gui_bridge_get_inst(void) { return reinterpret_cast<UTUN_INSTANCE*>(1); }
int gui_bridge_is_uasync_ready(void) { return 1; }
void gui_bridge_set_local_devices_cb(gui_local_event_fn cb) { devices_cb = cb; }
void gui_bridge_set_local_delivery_cb(gui_local_event_fn cb) { delivery_cb = cb; }
void gui_bridge_set_invite_link_ready_cb(gui_invite_link_ready_fn cb) { link_cb = cb; }
void gui_bridge_set_invite_candidates_cb(gui_invite_candidates_fn cb) { candidates_cb = cb; }
int gui_bridge_post_bound_fn(void (*fn)(void*), void* arg, struct UTUN_INSTANCE**) {
    if (fn == chat_invite_build_link_trampoline) link_request = static_cast<chat_invite_build_req*>(arg)->request_id;
    if (fn == chat_invite_cancel_window_trampoline) revoked_request = static_cast<chat_invite_cancel_req*>(arg)->request_id;
    u_free(arg);
    return 0;
}
void gui_bridge_post_uasync_fn(void (*fn)(void*), void* arg) {
    if (fn == chat_local_peers_trampoline) return;
    if (fn == chat_local_send_trampoline) { sent = *static_cast<chat_local_send_request*>(arg); sends++; }
    u_free(arg);
}

static QByteArray devices(std::initializer_list<quint64> ids) {
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream << quint16(ids.size());
    for (quint64 id : ids) {
        QByteArray address(CHAT_LOCAL_ADDRESS_SIZE, '\0'); address[31] = char(id);
        stream.writeRawData(address.constData(), address.size());
        stream << quint8(4); stream.writeRawData("Nick", 4);
    }
    return result;
}

class LocalInviteUiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_config_init(); }

    void deliveryAndDeviceChanges() {
        {
            ShareInviteDialog dialog("42", nullptr);
            auto* combo = dialog.findChild<QComboBox*>("localDevices");
            auto* send = dialog.findChild<QPushButton*>("sendLocalInvite");
            QVERIFY(combo && send && devices_cb && delivery_cb && link_cb && candidates_cb);
            QVERIFY(!send->isEnabled());
            auto list = devices({7, 8}); devices_cb((const uint8_t*)list.constData(), list.size());
            QCOMPARE(combo->count(), 2); QVERIFY(!send->isEnabled());
            uint8_t candidates[34] = {1};
            candidates_cb("42", 2, candidates, sizeof(candidates));
            QVERIFY(link_request);
            link_cb("42", 2, link_request, "utun://registered-link");
            QVERIFY(send->isEnabled()); combo->setCurrentIndex(1);
            list = devices({8, 9, 7}); devices_cb((const uint8_t*)list.constData(), list.size());
            QCOMPARE(combo->currentData().toByteArray().at(31), char(8));
            devices_cb((const uint8_t*)list.constData(), list.size() - 1);
            QCOMPARE(combo->count(), 3);
            QTest::mouseClick(send, Qt::LeftButton);
            QCOMPARE(sends, 1); QCOMPARE(sent.address.ed[31], uint8_t(8));
            QCOMPARE(QByteArray(sent.link), QByteArray("utun://registered-link"));
            QVERIFY(!send->isEnabled());
            uint8_t response[8 + CHAT_LOCAL_ADDRESS_SIZE + 1] = {0};
            memcpy(response + 8, sent.address.ed, 32);
            qToBigEndian<quint64>(sent.request_id + 1, response); response[sizeof(response) - 1] = 1;
            delivery_cb(response, sizeof(response)); QVERIFY(!send->isEnabled());
            qToBigEndian<quint64>(sent.request_id, response);
            delivery_cb(response, sizeof(response)); QVERIFY(send->isEnabled());
            QTest::mouseClick(send, Qt::LeftButton); QCOMPARE(sends, 2);
            qToBigEndian<quint64>(sent.request_id, response); response[sizeof(response) - 1] = LOCAL_DELIVERY_FORBIDDEN;
            delivery_cb(response, sizeof(response)); QVERIFY(send->isEnabled());
            bool forbiddenMessage = false;
            for (auto* label : dialog.findChildren<QLabel*>())
                if (label->text() == "Receiving invitations is forbidden. Enable it in the recipient device's settings.") forbiddenMessage = true;
            QVERIFY(forbiddenMessage);
            link_cb("42", 2, link_request, ""); QVERIFY(!send->isEnabled());
            list = devices({}); devices_cb((const uint8_t*)list.constData(), list.size());
            QCOMPARE(combo->count(), 0); QVERIFY(!send->isEnabled());
            dialog.reject();
            QCOMPARE(revoked_request, link_request);
        }
        QVERIFY(!devices_cb && !delivery_cb && !link_cb);
    }
};

QTEST_MAIN(LocalInviteUiTest)
#include "test_local_invite_ui.moc"
