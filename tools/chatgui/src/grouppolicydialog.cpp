#include "grouppolicydialog.h"
#include "../transport/gui_bridge.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <cstring>
extern "C" {
#include "chat/group_policy.h"
#include "chat/group_info.h"
#include "member_files/protocol.h"
#include "mem.h"
#include "debug_config.h"
}

static QString text(const char* s) { return QCoreApplication::translate("ChatGui", s); }
class GroupPolicyDialog;
static QHash<quint64, QPointer<GroupPolicyDialog>> requests;
static quint64 sequence = 0;

class GroupPolicyDialog : public QDialog {
public:
    GroupPolicyDialog(const QString& channel, QWidget* parent) : QDialog(parent), channel(channel) {
        setWindowTitle(text("Who may invite members")); resize(530, 440);
        auto* layout = new QVBoxLayout(this);
        mode = new QComboBox(this);
        mode->addItem(text("Owner and administrators only"), 0); mode->addItem(text("All group members"), 1);
        layout->addWidget(mode);
        auto* explanation = new QLabel(text("When permissions are restricted, members whose invitation chain no longer qualifies leave the group at the next synchronization. Select members below to re-sign with the group key and keep them."), this);
        explanation->setWordWrap(true); layout->addWidget(explanation);
        members = new QListWidget(this); layout->addWidget(members, 1);
        status = new QLabel(this); status->setWordWrap(true); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
        apply = buttons->button(QDialogButtonBox::Apply); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(apply, &QPushButton::clicked, this, [this] { request(true); });
        connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { if (loaded) request(false); });
        timer.setSingleShot(true); timer.setInterval(15000);
        connect(&timer, &QTimer::timeout, this, [this] {
            requests.remove(pending); pending = 0; token.clear(); setBusy(false);
            status->setText(text("The request timed out. Reopen settings to check the result."));
        });
        request(false);
    }
    ~GroupPolicyDialog() override { requests.remove(pending); }

    void response(quint64 id, const QByteArray& bytes) {
        if (id != pending) return;
        requests.remove(pending); pending = 0; timer.stop();
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error);
        const auto data = doc.object();
        if (error.error != QJsonParseError::NoError || !data.value("ok").toBool()) {
            token.clear(); setBusy(false);
            const auto reason = data.value("error").toString();
            status->setText(reason == "conflict" ? text("Membership changed. Review the refreshed list and apply again.") :
                reason == "sync_pending" ? text("Membership is synchronizing. Try again after synchronization.") : text("Could not change invitation permissions."));
            if (reason == "conflict") { conflict = true; request(false); }
            return;
        }
        canEdit = data.value("can_edit").toBool(); current = data.value("all_members").toBool();
        if (!loaded || data.value("applied").toBool()) {
            loaded = false; mode->setCurrentIndex(current ? 1 : 0); loaded = true;
        }
        token = data.value("token").toString(); members->clear();
        if (mode->currentIndex() != int(current) && !mode->currentIndex()) {
            for (const auto& value : data.value("affected").toArray()) {
                const auto member = value.toObject(); const auto node = member.value("node").toString();
                auto* item = new QListWidgetItem(member.value("name").toString() + " (" + node + ")", members);
                item->setData(Qt::UserRole, node); item->setCheckState(Qt::Unchecked);
                item->setToolTip(text("Invited by: %1").arg(member.value("inviter").toString()));
            }
        }
        status->setText(data.value("applied").toBool() ? text("Invitation permissions saved.") : conflict ?
            text("Membership changed. Review the refreshed list and apply again.") : canEdit ?
            text("Members affected: %1").arg(members->count()) : text("Only a superadministrator can change this setting."));
        conflict = false; setBusy(false);
    }
private:
    void setBusy(bool busy) {
        mode->setEnabled(!busy && canEdit); members->setEnabled(!busy && canEdit);
        apply->setEnabled(!busy && canEdit && !token.isEmpty() && mode->currentIndex() != int(current));
    }
    void request(bool save) {
        if (pending) return;
        QJsonArray keep;
        if (save) for (int i = 0; i < members->count(); i++)
            if (members->item(i)->checkState() == Qt::Checked) keep.append(members->item(i)->data(Qt::UserRole).toString());
        const QByteArray selection = QJsonDocument(keep).toJson(QJsonDocument::Compact);
        auto* q = static_cast<group_policy_request*>(u_calloc(1, sizeof(group_policy_request) + size_t(selection.size()) + 1));
        if (!q) { status->setText(text("Could not change invitation permissions.")); return; }
        q->id = ++sequence; qstrncpy(q->group, channel.toLatin1().constData(), sizeof(q->group)); q->all_members = mode->currentIndex();
        if (save) { const auto t = token.toLatin1(); snprintf(q->token, sizeof(q->token), "%s", t.constData()); }
        memcpy(q->keep, selection.constData(), size_t(selection.size()));
        pending = q->id; requests.insert(pending, this); setBusy(true);
        if (gui_bridge_post_bound_fn(group_policy_gui_command, q, &q->inst)) {
            requests.remove(pending); pending = 0; u_free(q); setBusy(false);
            status->setText(text("The core is not running.")); return;
        }
        timer.start();
    }
    QString channel, token;
    QComboBox* mode; QListWidget* members; QLabel* status; QPushButton* apply;
    QTimer timer; quint64 pending = 0; bool loaded = false, current = false, canEdit = false, conflict = false;
};

void openGroupPolicyDialog(const QString& channel, QWidget* parent) {
    gui_bridge_set_group_policy_result_cb([](const uint8_t* data, int size) {
        if (!data || size < 42) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group policy invalid UI result bytes=%d", size); return; }
        const quint64 id = mf_get64(data); const QByteArray json(reinterpret_cast<const char*>(data + 40), size - 40);
        QMetaObject::invokeMethod(qApp, [id, json] { if (auto dialog = requests.value(id)) dialog->response(id, json); }, Qt::QueuedConnection);
    });
    auto* dialog = new GroupPolicyDialog(channel, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowModality(Qt::WindowModal); dialog->show();
}

class GroupRenameDialog;
static QHash<quint64, QPointer<GroupRenameDialog>> renameRequests;
static quint64 renameSequence = 0;

/* Диалог ждёт конкретный commit; закрытие окна только снимает доставку UI-ответа. */
class GroupRenameDialog : public QDialog {
public:
    GroupRenameDialog(const QString& channel, QWidget* parent) : QDialog(parent), channel(channel) {
        setObjectName("groupRenameDialog"); setWindowTitle(text("Group Settings")); setMinimumWidth(360);
        auto* layout = new QVBoxLayout(this);
        currentName = new QLabel(this); currentName->setTextFormat(Qt::PlainText); currentName->setWordWrap(true); layout->addWidget(currentName);
        name = new QLineEdit(this); name->setObjectName("groupName"); name->setMaxLength(127); name->setReadOnly(true); layout->addWidget(name);
        status = new QLabel(this); status->setWordWrap(true); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
        clipboard = new QCheckBox(text("Allow sending clipboard to group members"), this);
        clipboard->setObjectName("allowMemberClipboard"); clipboard->setEnabled(false); layout->addWidget(clipboard);
        connect(clipboard, &QCheckBox::toggled, this, [this] { updateSave(); });
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, this);
        save = buttons->button(QDialogButtonBox::Save); save->setEnabled(false); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(save, &QPushButton::clicked, this, [this] { request(true); });
        connect(name, &QLineEdit::textChanged, this, [this] { updateSave(); });
        timer.setSingleShot(true); timer.setInterval(15000);
        connect(&timer, &QTimer::timeout, this, [this] {
            renameRequests.remove(pending); pending = 0; token.clear(); updateSave();
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group rename UI timeout channel=%s", qPrintable(this->channel));
            status->setText(text("The request timed out. Reopen settings to check the result."));
        });
        request(false);
    }
    ~GroupRenameDialog() override { renameRequests.remove(pending); }
    void response(quint64 id, const QString& group, const QByteArray& json) {
        if (pending != id || channel != group) return;
        renameRequests.remove(pending); pending = 0; timer.stop();
        QJsonParseError parse; const auto result = QJsonDocument::fromJson(json, &parse).object();
        if (parse.error != QJsonParseError::NoError || !result.value("ok").toBool()) {
            const bool conflict = result.value("error").toString() == "conflict";
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group rename UI refused channel=%s error=%s", qPrintable(channel),
                qPrintable(result.value("error").toString()));
            token.clear(); status->setText(conflict ? text("The group changed. Review the settings and try again.") : text("Could not save group settings"));
            updateSave(); if (conflict) request(false); return;
        }
        token = result.value("token").toString(); canEdit = result.value("can_edit").toBool();
        current = result.value("description").toObject().value("name").toString();
        currentClipboard = result.value("description").toObject().value("clipboard_members").toBool();
        currentName->setText(text("Current group name: %1").arg(current));
        if (!loaded) name->setText(current);
        if (!loaded || saving) clipboard->setChecked(currentClipboard);
        loaded = true; name->setReadOnly(!canEdit);
        clipboard->setEnabled(canEdit);
        if (saving) status->setText(text("Group settings saved"));
        else if (!canEdit) status->setText(text("The group key is required to change these settings."));
        updateSave();
    }
private:
    void updateSave() {
        const auto bytes = name->text().toUtf8();
        save->setEnabled(!pending && canEdit && token.size() == 64 && !bytes.isEmpty() && bytes.size() <= 127 &&
            !bytes.contains('\0') && (name->text() != current || clipboard->isChecked() != currentClipboard));
    }
    void request(bool apply) {
        auto* q = static_cast<group_info_request*>(u_calloc(1, sizeof(group_info_request)));
        if (!q) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group settings UI allocation failed"); status->setText(text("Could not save group settings")); return;
        }
        q->id = ++renameSequence; qstrncpy(q->group, channel.toLatin1().constData(), sizeof(q->group));
        if (apply) {
            const auto bytes = name->text().toUtf8(), expected = token.toLatin1();
            if (bytes.isEmpty() || bytes.size() > 127 || bytes.contains('\0') || expected.size() != 64) {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group rename UI invalid input"); u_free(q); return;
            }
            memcpy(q->name, bytes.constData(), size_t(bytes.size())); memcpy(q->expected, expected.constData(), 64);
            q->clipboard_members = clipboard->isChecked() ? 2 : 1;
        }
        const quint64 id = q->id;
        if (gui_bridge_post_bound_fn(group_info_gui_command, q, &q->inst)) {
            u_free(q); DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group rename UI enqueue refused");
            status->setText(text("Could not save group settings")); return;
        }
        saving = apply; pending = id; renameRequests.insert(id, this); timer.start(); updateSave();
    }
    QString channel, token, current;
    QLineEdit* name;
    QCheckBox* clipboard;
    bool currentClipboard = false;
    QLabel* status;
    QLabel* currentName;
    QPushButton* save;
    QTimer timer;
    quint64 pending = 0;
    bool loaded = false, canEdit = false, saving = false;
};

void openGroupRenameDialog(const QString& channel, QWidget* parent) {
    gui_bridge_set_group_info_result_cb([](const uint8_t* data, int size) {
        if (!data || size < 42) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group rename UI invalid result bytes=%d", size); return; }
        const quint64 id = mf_get64(data);
        const QString group = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(data + 8), 32).toHex());
        const QByteArray json(reinterpret_cast<const char*>(data + 40), size - 40);
        if (auto dialog = renameRequests.value(id)) dialog->response(id, group, json);
    });
    auto* dialog = new GroupRenameDialog(channel, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowModality(Qt::WindowModal); dialog->show();
}
