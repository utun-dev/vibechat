#include <QCoreApplication>
#include "devgroupdialog.h"
#include "../transport/gui_bridge.h"
#include "debug_ui.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QPointer>
#include <QHash>
#include <QMessageBox>
#include <QSet>
#include <QDialogButtonBox>
#include <QRegularExpressionValidator>
extern "C" {
#include "../../../src/chat/chat_devgroup.h"
#include "../../../lib/mem.h"
}
class DevgroupDialog;
static QPointer<DevgroupDialog> active;
static QHash<quint64, QPointer<QPushButton>> leaveRequests;
static QHash<quint64, QPointer<QWidget>> renameRequests;
static quint64 nextRequest = 0;
static QPointer<QWidget> invitationWindow;
static quint64 statusRequest = 0, lastAutomaticSnapshot = 0;
static QJsonObject invitationSnapshot;
static void refreshInvitations();
static void devgroupResult(const uint8_t* data, int len);
static bool pendingState(int state) { return state == 1 || state == 2 || state == 3 || state == 8 || state == 9 || state == 10; }
class DevgroupDialog : public QDialog {
public:
    static QString tr(const char* text) { return QCoreApplication::translate("DevgroupDialog", text); }
    void refresh() { send(CHAT_DEVGROUP_STATUS); }
    DevgroupDialog(QWidget* parent, const QString& channel, const QByteArray& peer, const QString& id = {})
        : QDialog(parent), channel(channel), outgoingId(id), peer(peer) {
        setAttribute(Qt::WA_DeleteOnClose); setObjectName("devgroupPairingDialog"); setModal(true);
        setWindowTitle(tr("Linking devices")); resize(480, 280);
        auto* layout = new QVBoxLayout(this);
        status = new QLabel(tr("Waiting for PIN"), this); status->setWordWrap(true);
        status->setTextFormat(Qt::PlainText);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(status);
        pin = new QLineEdit(this); pin->setObjectName("devgroupPinInput"); pin->setMaxLength(4);
        pin->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,4}"), pin)); layout->addWidget(pin);
        submit = new QPushButton(tr("Check PIN"), this); submit->setObjectName("devgroupCheckPin"); layout->addWidget(submit);
        connect(pin, &QLineEdit::textChanged, this, [this] { submit->setEnabled(input && !accepting && pin->text().size() == 4); });
        connect(submit, &QPushButton::clicked, this, [this] {
            if (!input || accepting || pin->text().size() != 4) return;
            send(CHAT_DEVGROUP_ACCEPT); pin->clear();
        });
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this); layout->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, this, &QDialog::close);
        auto* timer = new QTimer(this); timer->setInterval(1000);
        connect(timer, &QTimer::timeout, this, [this] {
            if (!gui_bridge_is_uasync_ready()) { pending.clear(); status->setText(tr("Core unavailable")); }
            else if (!pending.values().contains(CHAT_DEVGROUP_STATUS)) refresh();
        });
        timer->start();
        if (id.isEmpty()) { pin->hide(); submit->hide(); send(CHAT_DEVGROUP_INVITE); }
        else update(invitationSnapshot);
    }
    void update(const QJsonObject& snapshot) {
        for (const auto value : snapshot["invitations"].toArray()) {
            const auto invitation = value.toObject(); if (invitation["id"].toString() != outgoingId) continue;
            const int state = invitation["state"].toInt(), nextInput = invitation["input"].toInt();
            const bool outgoing = invitation["outgoing"].toInt() != 0;
            const QString shownPin = invitation["pin"].toString();
            const QStringList states = {{}, tr("Waiting for PIN"), tr("Checking PIN"), tr("Transferring keys"), tr("Joined"),
                tr("Rejected"), tr("Expired"), tr("Result unknown"), tr("Waiting for return PIN"), tr("Checking PIN"), tr("Transferring keys")};
            QString text = invitation["peer"].toString() + "\n\n";
            if (!outgoing) text += tr("Linking joins this device to your personal device subgroup and gives it the shared subgroup keys.") +
                "\n\n" + tr("If you are not linking your own personal devices, simply close this window.") + "\n\n";
            if (!shownPin.isEmpty()) text += (outgoing ? tr("PIN: %1 — enter it on the other device within 2 minutes.") :
                tr("Return PIN: %1 — enter it on the device that started linking." )).arg(shownPin) + "\n\n";
            if (nextInput) text += tr(nextInput == 1 ? "PIN shown on the inviting device" : "Enter the return PIN shown on the other device") + "\n";
            if (invitation["pin_error"].toInt()) text += tr("PIN was not accepted. Try again.") + "\n";
            status->setText(text + states.value(state));
            if (input != nextInput) pin->clear();
            input = nextInput; pin->setVisible(input); submit->setVisible(input);
            pin->setEnabled(!accepting); submit->setEnabled(input && !accepting && pin->text().size() == 4);
            return;
        }
        input = 0; pin->hide(); submit->hide(); status->setText(tr("Expired"));
    }
    void result(const uint8_t* data, int len) {
        if (len < 9) return;
        quint64 id = 0; for (int i = 0; i < 8; ++i) id |= quint64(data[i]) << (8*i);
        if (!pending.contains(id)) return;
        const int action = pending.take(id);
        if (action == CHAT_DEVGROUP_ACCEPT) accepting = false;
        if (!data[8]) { status->setText(tr("Operation failed. Check connection and invitation status.")); pin->setEnabled(true); return; }
        const auto json = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(data+9), len-9)).object();
        if (action == CHAT_DEVGROUP_INVITE) {
            outgoingId = json["id"].toString();
            status->setText(tr("PIN: %1 — enter it on the other device within 2 minutes.").arg(json["pin"].toString())); refresh(); return;
        }
        update(json);
    }
private:
    void send(chat_devgroup_action action) {
        auto* req = static_cast<chat_devgroup_request*>(u_calloc(1, sizeof(chat_devgroup_request)));
        if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup UI: allocation failed"); return; }
        req->inst = gui_bridge_get_inst(); req->request_id = ++nextRequest; qstrncpy(req->group, channel.toLatin1().constData(), sizeof(req->group));
        if (action == CHAT_DEVGROUP_INVITE) memcpy(req->peer, peer.constData(), 32);
        req->action = action; const quint64 id = req->request_id;
        if (action == CHAT_DEVGROUP_ACCEPT) {
            const auto invitation = outgoingId.toLatin1(), entered = pin->text().toLatin1();
            memcpy(req->text, invitation.constData(), qMin(invitation.size(), qsizetype(32))); memcpy(req->pin, entered.constData(), 4);
        }
        if (gui_bridge_post_uasync_owned_fn(chat_devgroup_trampoline, req) < 0) {
            memset(req, 0, sizeof(*req)); u_free(req); status->setText(tr("Core unavailable"));
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: core unavailable"); return;
        }
        if (action == CHAT_DEVGROUP_ACCEPT) {
            accepting = true; pin->setEnabled(false); submit->setEnabled(false);
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup UI: PIN submitted invite=%s step=%d", qPrintable(outgoingId), input);
        }
        pending.insert(id, action);
    }
    QString channel, outgoingId; QByteArray peer;
    QLabel* status; QLineEdit* pin; QPushButton* submit;
    int input = 0; bool accepting = false;
    QHash<quint64, int> pending;
};
static void devgroupResult(const uint8_t* data, int len) {
    if (!len) {
        refreshInvitations();
        if (active) active->refresh();
    } else {
        if (len >= 9) {
            quint64 id = 0; for (int i = 0; i < 8; ++i) id |= quint64(data[i]) << (8*i);
            if (renameRequests.contains(id)) {
                const auto parent = renameRequests.take(id);
                if (!data[8]) {
                    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: rename refused");
                    if (parent) QMessageBox::warning(parent, QCoreApplication::translate("ChatGui", "Rename"),
                        QCoreApplication::translate("ChatGui", "Could not rename device group"));
                } else DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup UI: group renamed");
                return;
            }
            if (leaveRequests.contains(id)) {
                const auto button = leaveRequests.take(id);
                if (!data[8]) {
                    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: disconnect refused");
                    if (button) { button->setEnabled(true); QMessageBox::warning(button->window(),
                        QCoreApplication::translate("ChatGui", "Disconnect from my devices"),
                        DevgroupDialog::tr("Operation failed. Check connection and invitation status.")); }
                } else DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup UI: own device disconnected");
                return;
            }
            const bool automatic = id == statusRequest;
            if (id == statusRequest) statusRequest = 0;
            if (automatic) {
                if (data[8] && id >= lastAutomaticSnapshot) {
                    lastAutomaticSnapshot = id;
                    invitationSnapshot = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(data+9), len-9)).object();
                    if (active) active->update(invitationSnapshot);
                }
                else if (!data[8]) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: automatic PIN/status request refused");
            }
        }
        if (active) active->result(data, len);
    }
}
static void refreshInvitations() {
    if (!invitationWindow || statusRequest || !gui_bridge_is_uasync_ready()) return;
    auto* req = static_cast<chat_devgroup_request*>(u_calloc(1, sizeof(chat_devgroup_request)));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup UI: status allocation failed"); return; }
    req->inst = gui_bridge_get_inst(); req->action = CHAT_DEVGROUP_STATUS; req->request_id = ++nextRequest;
    const quint64 id = req->request_id;
    if (gui_bridge_post_uasync_owned_fn(chat_devgroup_trampoline, req) < 0) {
        u_free(req); DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: status enqueue failed"); return;
    }
    statusRequest = id;
}
bool hasPendingDevicePairing() {
    if (!gui_bridge_is_uasync_ready()) return false;
    for (const auto value : invitationSnapshot["invitations"].toArray()) if (pendingState(value.toObject()["state"].toInt())) return true;
    return false;
}
void finishDevicePairing(QWidget* parent) {
    QStringList labels, ids;
    for (const auto value : invitationSnapshot["invitations"].toArray()) {
        const auto entry = value.toObject(); if (!pendingState(entry["state"].toInt())) continue;
        ids << entry["id"].toString(); labels << entry["peer"].toString() + " / " + entry["id"].toString().left(8);
    }
    if (ids.isEmpty() || !gui_bridge_is_uasync_ready()) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: no pending pairing"); return; }
    QString id = ids.first();
    if (ids.size() > 1) {
        bool ok = false;
        const QString choice = QInputDialog::getItem(parent, DevgroupDialog::tr("Linking devices"),
            DevgroupDialog::tr("Select device"), labels, 0, false, &ok);
        if (!ok || !labels.contains(choice)) return;
        id = ids.at(labels.indexOf(choice));
    }
    if (active) active->close();
    active = new DevgroupDialog(parent, {}, {}, id); active->show(); active->refresh();
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup UI: pairing opened from self menu invite=%s", qPrintable(id));
}
void initDevgroupUi(QWidget* window) {
    if (invitationWindow) return;
    statusRequest = 0; lastAutomaticSnapshot = 0; invitationSnapshot = {};
    invitationWindow = window; gui_bridge_set_devgroup_result_cb(devgroupResult);
    auto* timer = new QTimer(window); timer->setInterval(1000);
    QObject::connect(timer, &QTimer::timeout, window, [] {
        if (!gui_bridge_is_uasync_ready()) {
            statusRequest = 0; lastAutomaticSnapshot = 0; invitationSnapshot = {};
            if (active) active->update(invitationSnapshot);
            return;
        }
        refreshInvitations();
    });
    timer->start(); refreshInvitations();
}
void inviteMyDevice(QWidget* parent, const QString& channel, const QByteArray& peer) {
    if (peer.size() != 32) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: missing peer Ed25519"); return; }
    if (active) { active->close(); active = nullptr; }
    gui_bridge_set_devgroup_result_cb(devgroupResult);
    active = new DevgroupDialog(parent, channel, peer); active->show();
}

void disconnectMyDevice(QPushButton* button) {
    auto* req = static_cast<chat_devgroup_request*>(u_calloc(1, sizeof(chat_devgroup_request)));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup UI: disconnect allocation failed"); return; }
    req->inst = gui_bridge_get_inst(); req->action = CHAT_DEVGROUP_LEAVE; req->request_id = ++nextRequest;
    const quint64 id = req->request_id; gui_bridge_set_devgroup_result_cb(devgroupResult);
    if (gui_bridge_post_uasync_owned_fn(chat_devgroup_trampoline, req) < 0) {
        u_free(req); DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: disconnect enqueue failed");
        QMessageBox::warning(button->window(), QCoreApplication::translate("ChatGui", "Disconnect from my devices"),
            DevgroupDialog::tr("Core unavailable")); return;
    }
    leaveRequests.insert(id, button); button->setEnabled(false);
}

void renameMyDeviceGroup(QWidget* parent, const QString& name) {
    const auto bytes = name.toUtf8();
    if (bytes.isEmpty() || bytes.size() > 63 || bytes.contains('\0')) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: rename invalid name bytes=%lld", (long long)bytes.size()); return;
    }
    auto* req = static_cast<chat_devgroup_request*>(u_calloc(1, sizeof(chat_devgroup_request)));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup UI: rename allocation failed"); return; }
    req->inst = gui_bridge_get_inst(); req->action = CHAT_DEVGROUP_RENAME; req->request_id = ++nextRequest;
    memcpy(req->text, bytes.constData(), bytes.size());
    const quint64 id = req->request_id; gui_bridge_set_devgroup_result_cb(devgroupResult);
    if (gui_bridge_post_uasync_owned_fn(chat_devgroup_trampoline, req) < 0) {
        u_free(req); DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup UI: rename enqueue failed");
        QMessageBox::warning(parent, QCoreApplication::translate("ChatGui", "Rename"), DevgroupDialog::tr("Core unavailable")); return;
    }
    renameRequests.insert(id, parent);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup UI: rename submitted name_bytes=%lld", (long long)bytes.size());
}
