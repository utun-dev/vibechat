#include <QCoreApplication>
#include "channelsettingsdialog.h"
#include "grouppolicydialog.h"
#include "avatars/groupavatarmanager.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
extern "C" {
#include "chat/chat_core.h"
}
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QScopeGuard>
#include <cstring>
#include <limits>

ChannelSettingsDialog::ChannelSettingsDialog(const QString& channelId, DbManager* db, QWidget* parent)
    : QDialog(parent)
    , m_channelId(channelId)
    , m_db(db)
{
    m_validChannel = chat_channel_valid(channelId.toLatin1().constData());
    if (!m_validChannel) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "channel settings: invalid channel ID bytes=%d", int(channelId.toUtf8().size()));
    if (db) connect(db, &QObject::destroyed, this, &QDialog::reject);
    setWindowTitle(QCoreApplication::translate("ChatGui", "Group Settings"));
    setMinimumWidth(340);
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);

    auto* titleLabel = new QLabel(QCoreApplication::translate("ChatGui", "Settings for group"), this);
    titleLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
    layout->addWidget(titleLabel);

    auto* chIdLabel = new QLabel(channelId, this);
    chIdLabel->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
    chIdLabel->setWordWrap(true);
    layout->addWidget(chIdLabel);

    auto* rename = new QPushButton(QCoreApplication::translate("ChatGui", "Group name and clipboard permissions"), this);
    rename->setObjectName("renameGroup");
    rename->setEnabled(m_validChannel && m_db && m_db->isOpen() && gui_bridge_is_uasync_ready());
    connect(rename, &QPushButton::clicked, this, [this] { openGroupRenameDialog(m_channelId, this); });
    layout->addWidget(rename);

    if (QPointer<GroupAvatarManager> avatars = GroupAvatarManager::instance()) {
        auto* preview = new QLabel(this); preview->setFixedSize(64, 64); layout->addWidget(preview);
        auto* buttons = new QHBoxLayout;
        auto* upload = new QPushButton(QCoreApplication::translate("ChatGui", "Upload group avatar"), this);
        auto* remove = new QPushButton(QCoreApplication::translate("ChatGui", "Remove group avatar"), this);
        upload->setObjectName("uploadGroupAvatar"); remove->setObjectName("removeGroupAvatar");
        bool hasKey = false;
        if (db && db->isOpen()) for (const auto& row : db->getChannels())
            if (row.channelId == channelId) hasKey = row.ed25519_privkey.size() == 32;
        upload->setEnabled(m_validChannel && hasKey); remove->setEnabled(m_validChannel && hasKey);
        connect(avatars, &QObject::destroyed, upload, [upload, remove] { upload->setEnabled(false); remove->setEnabled(false); });
        auto* info = new QLabel(QCoreApplication::translate("ChatGui", "Group avatar: up to 16 KiB. Changes apply immediately and require the group key."), this);
        info->setWordWrap(true); auto* result = new QLabel(this); result->setWordWrap(true); result->setTextFormat(Qt::PlainText);
        buttons->addWidget(upload); buttons->addWidget(remove); layout->addLayout(buttons); layout->addWidget(info); layout->addWidget(result);
        auto refresh = [avatars, preview, channelId] {
            if (!avatars) return;
            const QImage image = avatars->image(channelId);
            if (image.isNull()) { preview->setPixmap(QPixmap()); preview->setText(QCoreApplication::translate("ChatGui", "No avatar")); }
            else preview->setPixmap(QPixmap::fromImage(image).scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        };
        connect(avatars, &GroupAvatarManager::changed, this, [channelId, refresh](const QString& channel) { if (channel == channelId) refresh(); });
        connect(avatars, &GroupAvatarManager::status, this, [channelId, result, refresh](const QString& channel, const QString& message) {
            if (channel == channelId) { result->setText(message); refresh(); }
        });
        connect(upload, &QPushButton::clicked, this, [this, avatars, channelId] {
            if (!avatars) return;
            QPointer<ChannelSettingsDialog> self(this);
            QPointer<QFileDialog> chooser = new QFileDialog(this, QCoreApplication::translate("ChatGui", "Choose avatar"));
            auto release = qScopeGuard([&] { delete chooser.data(); });
            chooser->setNameFilter(QCoreApplication::translate("ChatGui", "Images (*.png *.jpg *.jpeg *.webp)"));
            chooser->setFileMode(QFileDialog::ExistingFile);
            connect(avatars, &QObject::destroyed, chooser, &QDialog::reject);
            if (m_db) connect(m_db, &QObject::destroyed, chooser, &QDialog::reject);
            if (chooser->exec() != QDialog::Accepted || !self || !chooser || !avatars) return;
            const auto files = chooser->selectedFiles();
            if (!files.isEmpty()) avatars->upload(channelId, files.first());
        });
        connect(remove, &QPushButton::clicked, this, [avatars, channelId] { if (avatars) avatars->remove(channelId); }); refresh();
    }

    layout->addSpacing(8);

    auto* invitationPolicy = new QPushButton(QCoreApplication::translate("ChatGui", "Who may invite members"), this);
    invitationPolicy->setEnabled(m_validChannel && m_db && m_db->isOpen());
    connect(invitationPolicy, &QPushButton::clicked, this, [this] { openGroupPolicyDialog(m_channelId, this); });
    layout->addWidget(invitationPolicy);

    auto* invite = new QPushButton(QCoreApplication::translate("ChatGui", "Invite by node link"), this);
    invite->setObjectName("inviteByNodeLink"); invite->setAutoDefault(false);
    invite->setEnabled(m_validChannel && m_db && m_db->isOpen() && gui_bridge_is_uasync_ready());
    connect(invite, &QPushButton::clicked, this, [this] {
        if (!m_db || !m_db->isOpen() || !gui_bridge_is_uasync_ready()) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "channel settings: invite unavailable channel=%s", qPrintable(m_channelId));
            m_status->setText(QCoreApplication::translate("ChatGui", "Group is unavailable.")); return;
        }
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "channel settings: invite by node link channel=%s", qPrintable(m_channelId));
        emit inviteByLinkRequested(m_channelId);
    });
    layout->addWidget(invite);

    m_autoPlayVoice = new QCheckBox(QCoreApplication::translate("ChatGui", "Auto-play voice messages on receive"), this);
    m_autoPlayVoice->setToolTip(QCoreApplication::translate("ChatGui",
        "Voice messages will play automatically when received. Multiple messages queue up and play "
        "sequentially without interrupting current playback."));
    layout->addWidget(m_autoPlayVoice);

    m_soundOnMessage = new QCheckBox(QCoreApplication::translate("ChatGui", "Play sound on incoming message"), this);
    m_soundOnMessage->setChecked(true);
    m_soundOnMessage->setToolTip(QCoreApplication::translate("ChatGui", "Play a notification sound when a message arrives in this group."));
    layout->addWidget(m_soundOnMessage);
    m_status = new QLabel(this); m_status->setObjectName("channelSettingsStatus");
    m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); layout->addWidget(m_status);

    layout->addStretch();

    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(8);
    auto* deleteBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Delete group"), this);
    deleteBtn->setObjectName("deleteChannel");
    deleteBtn->setAutoDefault(false);
    deleteBtn->setStyleSheet("QPushButton { color: palette(bright-text); padding: 6px 12px; }");
    deleteBtn->setEnabled(m_validChannel && m_db && m_db->isOpen() && gui_bridge_is_uasync_ready());
    connect(deleteBtn, &QPushButton::clicked, this, &ChannelSettingsDialog::onDelete);
    btnLayout->addWidget(deleteBtn);
    btnLayout->addStretch();

    auto* cancelBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Cancel"), this);
    cancelBtn->setMinimumWidth(90);
    cancelBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; background: palette(button); }"
        "QPushButton:hover { background: palette(light); }");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(cancelBtn);

    auto* saveBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Save"), this);
    saveBtn->setEnabled(m_validChannel && m_db && m_db->isOpen());
    saveBtn->setMinimumWidth(90);
    saveBtn->setDefault(true);
    saveBtn->setStyleSheet(
        "QPushButton { border: none; border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; font-weight: bold;"
        " background: palette(highlight); color: palette(highlighted-text); }"
        "QPushButton:hover { background: #194e83; }");
    connect(saveBtn, &QPushButton::clicked, this, &ChannelSettingsDialog::onSave);
    btnLayout->addWidget(saveBtn);

    layout->addLayout(btnLayout);

    if (m_db && m_db->isOpen()) {
        QString autoPlayKey = QStringLiteral("ch_%1_auto_play_voice").arg(channelId);
        m_autoPlayVoice->setChecked(m_db->getUiStateInt(autoPlayKey, 0) != 0);
        QString soundKey = QStringLiteral("ch_%1_sound_on_message").arg(channelId);
        m_soundOnMessage->setChecked(m_db->getUiStateInt(soundKey, 1) != 0);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "channel settings opened: channel=%s valid=%d db=%d autoplay=%d sound=%d",
                qPrintable(channelId), m_validChannel, bool(m_db && m_db->isOpen()), m_autoPlayVoice->isChecked(), m_soundOnMessage->isChecked());
}

/* Как в Android: удаляются только локальные данные; ядро уведомит GUI по завершении. */
void ChannelSettingsDialog::onDelete() {
    if (!m_validChannel || !m_db || !m_db->isOpen()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "channel delete: unavailable channel=%s", qPrintable(m_channelId));
        m_status->setText(QCoreApplication::translate("ChatGui", "Group is unavailable.")); return;
    }
    QString name = m_channelId;
    bool found = false;
    for (const auto& channel : m_db->getChannels())
        if (channel.channelId == m_channelId) { name = channel.name; found = true; break; }
    if (!found) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "channel delete: channel removed before confirmation channel=%s", qPrintable(m_channelId));
        m_status->setText(QCoreApplication::translate("ChatGui", "Group is unavailable.")); return;
    }
    QPointer<ChannelSettingsDialog> self(this);
    QMessageBox confirmation(QMessageBox::Warning, QCoreApplication::translate("ChatGui", "Delete group"),
        QCoreApplication::translate("ChatGui", "Delete “%1” and all its local messages and files? Other members keep the group.")
            .arg(name), QMessageBox::NoButton);
    connect(this, &QObject::destroyed, &confirmation, &QDialog::reject);
    connect(m_db, &QObject::destroyed, &confirmation, &QDialog::reject);
    confirmation.setTextFormat(Qt::PlainText);
    auto* remove = confirmation.addButton(QCoreApplication::translate("ChatGui", "Delete"), QMessageBox::DestructiveRole);
    remove->setObjectName("confirmDeleteChannel");
    auto* cancel = confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(cancel);
    confirmation.setEscapeButton(cancel);
    confirmation.exec();
    if (!self || confirmation.clickedButton() != remove) return;
    found = false;
    if (m_db && m_db->isOpen()) for (const auto& channel : m_db->getChannels())
        if (channel.channelId == m_channelId) { found = true; break; }
    if (!found) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "channel delete: channel removed during confirmation channel=%s", qPrintable(m_channelId));
        m_status->setText(QCoreApplication::translate("ChatGui", "Group is unavailable.")); return;
    }
    auto* inst = gui_bridge_get_inst();
    if (!inst || !gui_bridge_is_uasync_ready()) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel delete: core unavailable channel=%s", qPrintable(m_channelId));
        m_status->setText(QCoreApplication::translate("ChatGui", "The core is not running."));
        return;
    }
    const QByteArray channel = m_channelId.toUtf8();
    auto* request = static_cast<chat_delete_channel_req*>(u_calloc(1, sizeof(chat_delete_channel_req)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel delete: request allocation failed channel=%s", channel.constData());
        m_status->setText(QCoreApplication::translate("ChatGui", "Failed to delete group."));
        return;
    }
    request->inst = inst;
    snprintf(request->ch_id, sizeof(request->ch_id), "%s", channel.constData());
    if (gui_bridge_post_uasync_owned_fn(chat_core_delete_channel_trampoline, request) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel delete: request rejected channel=%s", channel.constData());
        u_free(request); m_status->setText(QCoreApplication::translate("ChatGui", "Failed to delete group.")); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "channel delete queued: channel=%s scope=local", channel.constData());
    accept();
}

void ChannelSettingsDialog::onSave() {
    if (!m_validChannel || !m_db || !m_db->isOpen()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "channel settings save: unavailable channel=%s", qPrintable(m_channelId));
        m_status->setText(QCoreApplication::translate("ChatGui", "Group is unavailable.")); return;
    }
    auto* inst = gui_bridge_get_inst();
    if (!inst || !gui_bridge_is_uasync_ready()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "channel settings save: core unavailable channel=%s", qPrintable(m_channelId));
        m_status->setText(QCoreApplication::translate("ChatGui", "The core is not running.")); return;
    }
    auto* voice = static_cast<save_ui_state_arg*>(u_calloc(1, sizeof(save_ui_state_arg)));
    auto* sound = static_cast<chat_sound_arg*>(u_calloc(1, sizeof(chat_sound_arg)));
    auto release = qScopeGuard([&] { u_free(voice); u_free(sound); });
    if (!voice || !sound) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel settings save: allocation failed channel=%s voice=%d sound=%d",
                    qPrintable(m_channelId), bool(voice), bool(sound));
        m_status->setText(QCoreApplication::translate("ChatGui", "Failed to save group settings. Please try again.")); return;
    }
    voice->inst = sound->inst = inst;
    const QByteArray key = QStringLiteral("ch_%1_auto_play_voice").arg(m_channelId).toUtf8();
    memcpy(voice->data, key.constData(), size_t(key.size()));
    voice->data[key.size() + 1] = m_autoPlayVoice->isChecked() ? '1' : '0';
    const QByteArray channel = m_channelId.toUtf8();
    memcpy(sound->ch_id, channel.constData(), size_t(channel.size())); sound->on = m_soundOnMessage->isChecked();
    if (gui_bridge_post_uasync_owned_fn(chat_core_save_ui_state_trampoline, voice) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel settings save: autoplay request rejected channel=%s", channel.constData());
        m_status->setText(QCoreApplication::translate("ChatGui", "Failed to save group settings. Please try again.")); return;
    }
    voice = nullptr;
    if (gui_bridge_post_uasync_owned_fn(chat_core_set_sound_on_message_trampoline, sound) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel settings save: sound request rejected after autoplay accepted channel=%s", channel.constData());
        m_status->setText(QCoreApplication::translate("ChatGui", "Failed to save group settings. Please try again.")); return;
    }
    sound = nullptr;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "channel settings queued: channel=%s autoplay=%d sound=%d", channel.constData(),
                m_autoPlayVoice->isChecked(), m_soundOnMessage->isChecked());
    accept();
}
