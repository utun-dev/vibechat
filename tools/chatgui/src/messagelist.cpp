#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include "messagelist.h"
#include "message_links.h"
#include <QClipboard>
#include "media_preview_loader.h"
#include "../transport/stream_bridge.h"
#include "debug_ui.h"
#include "chatview.h"
#include "messagedelegate.h"
#include "group_attachment_roles.h"
#include "inputbar.h"
#include "media_async/attachment_send.h"
#include "emojipanel.h"
#include "formattingpanel.h"
#include "emoji.h"
#include "animtimer.h"
#include "lottieicon.h"
#include "audiorecorder.h"
#include "voiceplayback.h"
#include "../db/db_manager.h"
#include "../../lib/debug_config.h"
#include <QVBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QSignalBlocker>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QScrollBar>
#include <QApplication>
#include <QPointer>
#include <QDebug>
#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>
#include <QImageReader>
#include <QDir>
#include <QMenu>
#include <QFileDialog>
#include <QToolTip>
#include <QTreeWidget>
#include <QHeaderView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <vector>
#include <algorithm>
#include <memory>
#include <thread>

static std::vector<std::unique_ptr<LottieIcon>> s_icons;

extern "C" {
#include "../transport/gui_bridge.h"
#include "chat/chat_core.h"
#include "chat/chat_attachment.h"
#include "chat/chat_poll.h"
#include "dm/dm_core.h"
#include "dm/dm_media.h"
#include "video/video.h"
#include "radio/radio.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/audio_compressor.h"
#include "../../lib/debug_config.h"
}

#include "radio_audio_engine.h"
#include "radiopanel.h"
#include "flagpainter.h"
#include "outline_icon.h"
#include "avatars/avatarmanager.h"
#include "avatars/avatargallery.h"
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>

// Портативная рация: один и тот же значок во всех состояниях кнопки.
static QIcon makeRadioIcon(const QColor& color) {
    QPixmap pix(48, 48);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    FlagPainter::drawRadio(&p, 0, 0, 48, color);
    p.end();
    QIcon icon;
    // Явно задаём все режимы: Qt не перекрашивает значок при checked/disabled.
    for (auto mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected}) {
        icon.addPixmap(pix, mode, QIcon::Off);
        icon.addPixmap(pix, mode, QIcon::On);
    }
    return icon;
}

enum class HeaderActionIcon { Publish, Poll, OwnFiles };
static QIcon headerActionIcon(HeaderActionIcon type, const QColor& color) {
    QPainterPath path;
    if (type == HeaderActionIcon::Publish) {
        path.addRoundedRect(QRectF(2, 6, 14, 12), 2, 2);
        path.moveTo(16, 10); path.lineTo(22, 6); path.lineTo(22, 18); path.lineTo(16, 14);
    } else if (type == HeaderActionIcon::Poll) {
        path.addRoundedRect(QRectF(3, 2, 18, 20), 2, 2);
        for (int y : {7, 12, 17}) { path.addEllipse(QRectF(6, y - 1, 2, 2)); path.moveTo(11, y); path.lineTo(17, y); }
    } else {
        path.moveTo(2, 8); path.lineTo(2, 5); path.quadTo(2, 3, 4, 3); path.lineTo(9, 3); path.lineTo(12, 6);
        path.lineTo(20, 6); path.quadTo(22, 6, 22, 8); path.lineTo(22, 19); path.quadTo(22, 21, 20, 21);
        path.lineTo(4, 21); path.quadTo(2, 21, 2, 19); path.lineTo(2, 8); path.lineTo(22, 8);
        path.addEllipse(QRectF(10, 10, 4, 4)); path.moveTo(7, 19); path.quadTo(12, 13, 17, 19);
    }
    return outlineIcon(path, color, 24);
}

static QPixmap makeAvatar(const QColor &color, const QString &letter, int size = MessageDelegate::kAvatar) {
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    p.drawEllipse(0, 0, size, size);
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setBold(true);
    f.setPointSize(size / 3);
    p.setFont(f);
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, letter);
    p.end();
    return pix;
}


static void setMsg(QStandardItem *item,
                   const QString &author, const QString &text, const QString &time,
                   const QPixmap &avatar, bool edited,
                   bool hasQuote, const QString &qAuthor, const QString &qText,
                   const QVariantList &reactions) {
    item->setData(author,  MsgAuthorRole);
    item->setData(text,    MsgTextRole);
    item->setData(time,    MsgTimeRole);
    item->setData(edited,  MsgEditedRole);
    item->setData(QVariant::fromValue(avatar), MsgAvatarRole);
    item->setData(hasQuote, MsgHasQuoteRole);
    if (hasQuote) {
        item->setData(qAuthor, MsgQuoteAuthorRole);
        item->setData(qText,   MsgQuoteTextRole);
    }
    if (!reactions.isEmpty())
        item->setData(reactions, MsgReactionsRole);
}

MessageList::MessageList(DbManager* db, AudioRecorder* recorder, QWidget *parent)
    : QWidget(parent)
    , m_view(new ChatView(this))
    , m_model(new QStandardItemModel(this))
    , m_inputBar(nullptr)
    , m_emojiPanel(nullptr)
    , m_db(db)
    , m_recorder(recorder)
{
    auto *layout = new QVBoxLayout(this);
    new MediaPreviewLoader(m_view, m_model, m_db ? m_db->dbPath() : QString());
    m_view->viewport()->installEventFilter(this);
    if (auto* manager = AvatarManager::instance()) connect(manager, &AvatarManager::changed, this,
        [this, manager](const QString& channel, const QByteArray& owner) {
            if (channel != m_currentChannelId && !isDmChannel(m_currentChannelId)) return;
            const QImage image = manager->image(m_currentChannelId, owner);
            for (int row = 0; row < m_model->rowCount(); row++) {
                auto* item = m_model->item(row);
                if (item->data(MsgAuthorAddressRole).toByteArray() != owner) continue;
                QPixmap avatar;
                if (!image.isNull()) avatar = QPixmap::fromImage(image);
                else {
                    const AccountRow account = m_db->getAccount(item->data(MsgAuthorAddressRole).toByteArray());
                    avatar = makeAvatar(QColor(account.avatarColor), account.avatarLetter.isEmpty() ?
                        item->data(MsgAuthorRole).toString().left(1) : account.avatarLetter);
                }
                item->setData(avatar, MsgAvatarRole);
            }
        });
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    connect(m_view, &ChatView::linkActivated, this, &MessageList::linkActivated);

    /* Шапка чата: название, трансляция, рация, опрос и свои файлы. */
    {
        auto* header = new QWidget(this);
        header->setObjectName("messageHeader");
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(10, 6, 10, 6);
        hl->setSpacing(8);

        m_peerStatus = new QLabel(header);
        m_peerStatus->setFixedSize(12, 12);
        m_peerStatus->hide();
        hl->addWidget(m_peerStatus);
        m_channelTitle = new QLabel(header);
        m_channelTitle->setTextFormat(Qt::PlainText);
        m_channelTitle->setStyleSheet("font-weight: bold; font-size: 14px;");
        m_channelTitle->setText(QCoreApplication::translate("ChatGui", "Group"));
        hl->addWidget(m_channelTitle, 1);
        m_keyRequestsButton = new QPushButton(header);
        m_keyRequestsButton->setObjectName("attachmentKeyRequestsButton");
        m_keyRequestsButton->hide();
        connect(m_keyRequestsButton, &QPushButton::clicked, this, &MessageList::showKeyRequests);
        hl->addWidget(m_keyRequestsButton);

        m_inviteBtn = new QPushButton(QStringLiteral("+"), header);
        m_inviteBtn->setAccessibleName(QCoreApplication::translate("ChatGui", "Invite to group"));
        m_inviteBtn->setToolTip(QCoreApplication::translate("ChatGui", "Invite to group"));
        m_inviteBtn->setFixedSize(34, 30);
        m_inviteBtn->setCursor(Qt::PointingHandCursor);
        m_inviteBtn->hide();
        hl->addWidget(m_inviteBtn);
        connect(m_inviteBtn, &QPushButton::clicked, this, [this]() {
            if (!m_db || !isDmChannel(m_currentChannelId)) return;
            for (const auto& conv : m_db->getDmConversations()) {
                if (conv.convId == dmConvId(m_currentChannelId)) {
                    emit inviteRequested(m_currentChannelId, conv.peerEd25519, m_channelTitle->text());
                    return;
                }
            }
            GUI_ERROR("invite PM: conversation not found target=%s", qPrintable(m_currentChannelId));
        });

        m_callBtn = new QPushButton(QStringLiteral("☎"), header);
        m_callBtn->setAccessibleName(QCoreApplication::translate("ChatGui", "Call peer"));
        m_callBtn->setFixedSize(34, 30);
        m_callBtn->setCursor(Qt::PointingHandCursor);
        m_callBtn->hide();
        clearDmCallRoute();
        hl->addWidget(m_callBtn);
        connect(m_callBtn, &QPushButton::clicked, this, [this]() {
            if (m_callBtn->isEnabled() && !m_callPeer.isEmpty() && isDmChannel(m_currentChannelId))
                emit callRequested(m_currentChannelId, m_callPeer, m_channelTitle->text());
        });

        auto actionButton = [this, header](const char* name, const char* title) {
            auto* button = new QPushButton(header); button->setObjectName(QLatin1String(name));
            button->setFixedSize(34, 30); button->setIconSize(QSize(24, 24)); button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(QCoreApplication::translate("ChatGui", title)); button->setAccessibleName(button->toolTip());
            button->setStyleSheet("QPushButton { border: none; border-radius: 6px; background: transparent; }"
                "QPushButton:hover { background: palette(light); }");
            button->setProperty("instantTooltip", true); button->installEventFilter(this); button->hide(); return button;
        };
        m_streamPublishBtn = actionButton("streamPublishButton", "Start stream");
        m_streamViewerBadge = new QLabel(m_streamPublishBtn); m_streamViewerBadge->setObjectName("streamViewerBadge");
        m_streamViewerBadge->setAttribute(Qt::WA_TransparentForMouseEvents); m_streamViewerBadge->setAlignment(Qt::AlignCenter);
        m_streamViewerBadge->setStyleSheet("background: #2e7d32; color: white; border-radius: 7px; padding: 0 3px;"
            "font-size: 10px; font-weight: bold;"); m_streamViewerBadge->hide();
        connect(m_streamPublishBtn, &QPushButton::clicked, this, [this] {
            const QString selected = m_currentChannelId;
            if (selected.size() != 64) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop publish action without a selected group"); return; }
            const QString group = m_streamPublisherGroup.isEmpty() ? selected : m_streamPublisherGroup;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop publish header clicked channel=%s publisher_group=%s target=%s",
                qPrintable(m_currentChannelId), qPrintable(m_streamPublisherGroup), qPrintable(group));
            emit streamPublishRequested(group);
        });
        hl->addWidget(m_streamPublishBtn);

        m_radioBtn = new QPushButton(header); m_radioBtn->setObjectName("radioButton");
        m_radioBtn->setIcon(makeRadioIcon(header->palette().color(QPalette::ButtonText)));
        m_radioBtn->setIconSize(QSize(24, 24));
        m_radioBtn->setAccessibleName(QCoreApplication::translate("ChatGui", "Radio"));
        m_radioBtn->setCheckable(true);
        m_radioBtn->setFixedSize(34, 30);
        m_radioBtn->setCursor(Qt::PointingHandCursor);
        m_radioBtn->setToolTip(QCoreApplication::translate("ChatGui", "Radio: toggle group listening"));
        m_radioBtn->setStyleSheet(
            "QPushButton { border: 1px solid palette(mid); border-radius: 6px; background: palette(base); font-size: 15px; }"
            "QPushButton:checked { background: #226d38; border-color: #226d38; }");
        connect(m_radioBtn, &QPushButton::toggled, this, &MessageList::onRadioToggled);
        hl->addWidget(m_radioBtn);

        m_pollBtn = actionButton("createPollButton", "Create poll");
        connect(m_pollBtn, &QPushButton::clicked, this, [this] {
            if (m_currentChannelId.size() != 64) { GUI_WARN("poll action without a selected group"); return; }
            GUI_INFO("create poll header clicked channel=%s", qPrintable(m_currentChannelId)); createPoll();
        });
        hl->addWidget(m_pollBtn);
        m_ownFilesBtn = actionButton("ownFilesButton", "My files");
        connect(m_ownFilesBtn, &QPushButton::clicked, this, [this] {
            const QString channel = m_currentChannelId;
            if (channel.size() != 64) { GUI_WARN("own files action without a selected group"); return; }
            GUI_INFO("own files header clicked channel=%s", qPrintable(channel)); emit ownFilesRequested(channel);
        });
        hl->addWidget(m_ownFilesBtn);

        m_streamBtn = new QPushButton(header); m_streamBtn->setObjectName("streamButton");
        QPixmap streamIcon(24, 24); streamIcon.fill(Qt::transparent);
        { QPainter painter(&streamIcon); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(QPen(QColor("#4caf50"), 2));
          painter.drawRoundedRect(QRectF(2, 6, 20, 14), 2, 2); painter.drawLine(8, 1, 12, 5); painter.drawLine(16, 1, 12, 5);
          painter.setBrush(QColor("#4caf50")); painter.drawEllipse(QPointF(12, 13), 3, 3); }
        m_streamBtn->setIcon(QIcon(streamIcon)); m_streamBtn->setIconSize(QSize(24, 24)); m_streamBtn->hide();
        m_streamBtn->setAccessibleName(QCoreApplication::translate("ChatGui", "Live streams"));
        m_streamBtn->setCursor(Qt::PointingHandCursor);
        m_streamMenu = new QMenu(this); m_streamMenu->setObjectName("streamSourceMenu");
        m_streamMenu->setAttribute(Qt::WA_NoMouseReplay); m_streamMenu->installEventFilter(this);
        m_streamBtn->installEventFilter(this);
        m_streamHold.setSingleShot(true); m_streamHold.setInterval(500);
        connect(&m_streamHold, &QTimer::timeout, this, [this] {
            if (!m_streamPressed || !m_streamBtn->isEnabled() || !m_streamBtn->isVisible()) return;
            m_streamLongPressed = true; m_streamBtn->setDown(false);
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop stream list opened by long press group=%s", qPrintable(m_currentChannelId));
            showStreamMenu();
        });
        connect(m_streamBtn, &QPushButton::clicked, this, [this] {
            m_streamMenu->hide();
            const QString group = m_currentChannelId;
            if (group.size() != 64 || m_streamBtn->isHidden()) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop watch-all action without active streams"); return; }
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop selected all streams group=%s action=watch-all", qPrintable(group));
            emit allStreamsRequested(group);
        });
        hl->addWidget(m_streamBtn);

        m_groupMenuButton = new QPushButton(QStringLiteral("…"), header);
        m_groupMenuButton->setObjectName("groupMenuButton");
        m_groupMenuButton->setFixedSize(34, 30);
        m_groupMenuButton->setCursor(Qt::PointingHandCursor);
        m_groupMenuButton->setToolTip(QCoreApplication::translate("ChatGui", "Options"));
        m_groupMenuButton->setAccessibleName(m_groupMenuButton->toolTip());
        m_groupMenuButton->hide();
        hl->addWidget(m_groupMenuButton);
        connect(m_groupMenuButton, &QPushButton::clicked, this, [this] {
            const QString channel = m_currentChannelId;
            if (channel.size() != 64) return;
            auto text = [](const char* s) { return QCoreApplication::translate("ChatGui", s); };
            QMenu menu; connect(this, &QObject::destroyed, &menu, &QMenu::close);
            auto* files = menu.addAction(text("Group files"));
            menu.addSeparator();
            auto* share = menu.addAction(text("Invite"));
            menu.addSeparator();
            auto* settings = menu.addAction(text("Group Settings"));
            QPointer<MessageList> guard(this);
            auto* action = menu.exec(m_groupMenuButton->mapToGlobal(QPoint(0, m_groupMenuButton->height())));
            if (!guard || !action) return;
            if (channel != m_currentChannelId) {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group menu: channel changed old=%s current=%s", qPrintable(channel), qPrintable(m_currentChannelId));
                return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "group menu: channel=%s action=%s", qPrintable(channel), qPrintable(action->text()));
            if (action == files) emit filesRequested(channel);
            else if (action == share) emit shareInviteRequested(channel);
            else if (action == settings) emit channelSettingsRequested(channel);
        });

        header->setStyleSheet("QWidget#messageHeader { background: palette(window); }");
        updateHeaderIcons();
        layout->addWidget(header);
    }

    m_radioPanel = new RadioPanel(this);
    m_radioPanel->hide();
    connect(RadioAudioEngine::instance(), &RadioAudioEngine::stateChanged, this, &MessageList::updateRadioUi);
    layout->addWidget(m_radioPanel);

    m_view->setModel(m_model);
    auto* msgDelegate = new MessageDelegate(this);
    m_view->setLinkHitTest([this, msgDelegate](const QModelIndex& index, const QPoint& point) {
        QStyleOptionViewItem option; option.initFrom(m_view); option.widget = m_view; option.rect = m_view->visualRect(index);
        return msgDelegate->linkAt(option, index, point);
    });
    m_view->setCodeCopyHitTest([this, msgDelegate](const QModelIndex& index, const QPoint& point) {
        QStyleOptionViewItem option; option.initFrom(m_view); option.widget = m_view; option.rect = m_view->visualRect(index);
        return msgDelegate->codeCopyAt(option, index, point) >= 0;
    });
    msgDelegate->setVideoOpenCallback([this](const QString& path) {
        emit openVideoRequested(path);
    });
    msgDelegate->setImageOpenCallback([this](const QString& path) {
        emit openImageRequested(path);
    });
    connect(msgDelegate, &MessageDelegate::avatarRequested, this, [](const QString& channel, const QByteArray& owner) {
        showAvatarGallery(channel, owner);
    });
    connect(msgDelegate, &MessageDelegate::messageMenuRequested, this, &MessageList::showMessageMenu);
    connect(msgDelegate, &MessageDelegate::reactionRequested, this, &MessageList::requestReaction);
    connect(msgDelegate, &MessageDelegate::pollRequested, this, &MessageList::showPoll);
    connect(msgDelegate, &MessageDelegate::replyRequested, this, &MessageList::jumpToMessage);
    connect(m_view, &ChatView::latestRequested, this, &MessageList::showLatestMessages);
    connect(msgDelegate, &MessageDelegate::attachmentOpening, this, [this](const QModelIndex& index) {
        const QString channel = index.data(MsgChannelIdRole).toString();
        if (!isDmChannel(channel) || channel != m_currentChannelId) return;
        const QString path = index.data(MsgVoiceFileRole).toString();
        const QByteArray author = index.data(MsgAuthorAddressRole).toByteArray();
        if (author.size() != 32) { GUI_WARN("PM opening: missing author key"); return; }
        m_openingDmFiles.insert(path, {dmConvId(channel), author,
            index.data(MsgIdRole).toULongLong(), m_currentDmGeneration});
    });
    connect(msgDelegate, &MessageDelegate::attachmentOpened, this, &MessageList::onAttachmentOpened);
    msgDelegate->setDownloadCallback([this](const QModelIndex& index) {
        QString target = index.data(MsgChannelIdRole).toString();
        qint64 id = index.data(MsgIdRole).toLongLong();
        if (target.startsWith("dm:")) return downloadDmFile(index);
        if (target.isEmpty() || id <= 0) return false;
        return gui_bridge_download_attachment(target.toUtf8().constData(), id) == 0;
    });
    m_view->setItemDelegate(msgDelegate);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::NoSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested,
            this, &MessageList::onMessageContextMenu);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setBackgroundImage(QStringLiteral(":/bg.webp"));
    layout->addWidget(m_view, 1);

    if (s_icons.empty()) {
        for (const auto &cat : builtinEmojiSet()) {
            for (const auto &e : cat.items) {
                if (e.type == EmojiType::Animation && !e.animPath.isEmpty()) {
                    QFile f(e.animPath);
                    if (f.open(QIODevice::ReadOnly)) {
                        QByteArray raw = f.readAll();
                        auto icon = std::make_unique<LottieIcon>(raw, e.animPath);
                        if (icon->isValid()) {
                            AnimTimer::instance()->registerIcon(e.animPath, icon.get());
                            s_icons.push_back(std::move(icon));
                        }
                    }
                }
            }
        }
    }

    m_emojiPanel = new EmojiPanel(m_view->viewport());
    m_formattingPanel = new FormattingPanel(m_view->viewport());

    m_inputBar = new InputBar(m_emojiPanel, m_formattingPanel, recorder, this);
    connect(m_inputBar, &InputBar::videoRecordingRequested, this, &MessageList::videoRecordingRequested);
    connect(m_inputBar, &InputBar::messageAccepted, this, &MessageList::showLatestMessages);
    setFocusProxy(m_inputBar);
    layout->addWidget(m_inputBar);
    m_inputBar->installFileDropTarget(m_view->viewport());
    m_inputBar->focusProxy()->installEventFilter(m_view);
    window()->installEventFilter(m_view);

    connect(m_inputBar, &InputBar::saveEdit, this,
        [this](const QString& channel, const QByteArray& id, quint64 version, const QString& text, quint64 request) {
        const auto bytes = text.toUtf8();
        if (!gui_bridge_get_inst() || !gui_bridge_is_uasync_ready() || channel != m_currentChannelId ||
            isDmChannel(channel) || id.size() != 32) {
            GUI_ERROR("edit: unavailable target=%s", qPrintable(channel)); m_inputBar->submissionResult(request, false); return;
        }
        auto* req = (struct chat_msg_submit*)u_calloc(1, sizeof(struct chat_msg_submit) + bytes.size());
        if (!req) { GUI_ERROR("edit: allocation failed"); m_inputBar->submissionResult(request, false); return; }
        req->request_id = request; req->expected_ver = version;
        snprintf(req->channel_id, sizeof(req->channel_id), "%s", channel.toUtf8().constData());
        memcpy(req->message_id, id.constData(), 32); req->data = (uint8_t*)(req + 1); req->data_len = bytes.size();
        memcpy(req->data, bytes.constData(), bytes.size());
        if (gui_bridge_submit_message(req) < 0) { u_free(req); m_inputBar->submissionResult(request, false); }
    });
    connect(m_inputBar, &InputBar::sendMessage, this, [this](const QString &text, const QByteArray& reply, quint64 request, const QByteArray& quote) {
        if (!gui_bridge_get_inst() || !gui_bridge_is_uasync_ready()) {
            GUI_ERROR("send: core unavailable"); m_inputBar->submissionResult(request, false); return;
        }
        if (m_currentChannelId.isEmpty()) {
            GUI_WARN("send: no channel selected, message not sent");
            m_inputBar->submissionResult(request, false);
            return;
        }

        if (isDmChannel(m_currentChannelId)) {
            if (!m_currentDmGeneration || m_dmDeleting.value(dmConvId(m_currentChannelId)) == m_currentDmGeneration) {
                GUI_WARN("PM text rejected: unavailable/deleting target=%s", qPrintable(m_currentChannelId));
                m_inputBar->submissionResult(request, false); return;
            }
            QByteArray textData = text.toUtf8();
            QString conv = dmConvId(m_currentChannelId);
            struct dm_send_req* req = (struct dm_send_req*)u_malloc(sizeof(struct dm_send_req) + textData.size());
            if (!req) { GUI_ERROR("PM text request allocation failed"); m_inputBar->submissionResult(request, false); return; }
            memset(req, 0, sizeof(*req));
            req->generation = m_currentDmGeneration;
            req->request_id = request;
            strncpy(req->conv_id, conv.toUtf8().constData(), sizeof(req->conv_id) - 1);
            req->data = (uint8_t*)(req + 1);
            req->data_len = (uint32_t)textData.size();
            memcpy(req->data, textData.constData(), textData.size());
            if (gui_bridge_submit_dm_message(req) < 0) {
                u_free(req); m_inputBar->submissionResult(request, false);
            }
            return;
        }

        QByteArray textData = text.toUtf8();
        auto ts = QDateTime::currentMSecsSinceEpoch();

        struct chat_msg_submit* req = (struct chat_msg_submit*)
            u_malloc(sizeof(struct chat_msg_submit) + textData.size());
        if (!req) { GUI_ERROR("send: text request allocation failed"); m_inputBar->submissionResult(request, false); return; }
        memset(req, 0, sizeof(*req));
        if (reply.size() == 32) memcpy(req->reply_to, reply.constData(), 32);
        memcpy(req->reply_quote, quote.constData(), size_t(quote.size()));
        req->request_id = request;
        snprintf(req->channel_id, sizeof(req->channel_id), "%s",
                 m_currentChannelId.toUtf8().constData());
        snprintf(req->content_type, sizeof(req->content_type), "text/plain");
        req->data = (uint8_t*)(req + 1);
        req->data_len = (uint32_t)textData.size();
        memcpy(req->data, textData.constData(), textData.size());
        req->timestamp = (uint64_t)ts;
        if (gui_bridge_submit_message(req) < 0) { u_free(req); m_inputBar->submissionResult(request, false); }
    });

    connect(m_inputBar, &InputBar::submitAttachment, this, [this](const QString& target, struct attachment_send_req* req) {
        const bool dm = isDmChannel(target);
        if (dm && (!req->dm_generation || m_dmDeleting.value(dmConvId(target)) == req->dm_generation)) {
            GUI_ERROR("send: attachment target/core unavailable target=%s generation=%llu", qPrintable(target),
                      (unsigned long long)req->dm_generation); m_inputBar->submissionResult(req->request_id, false);
            attachment_send_discard(req); return;
        }
        req->is_dm = dm;
        QByteArray id = (req->is_dm ? dmConvId(target) : target).toUtf8();
        snprintf(req->target, sizeof(req->target), "%s", id.constData());
        if (gui_bridge_submit_attachment(req) < 0) {
            m_inputBar->submissionResult(req->request_id, false); attachment_send_discard(req);
        }
    });

    connect(m_inputBar, &InputBar::sendFileAttachment, this, [this](const QString& target, const QString& mediaSrc, const QString& mediaDest,
                                                                      const QString& displayName, const QByteArray& reply, quint64 request, const QByteArray& quote) {
        if (target.isEmpty() || !gui_bridge_get_inst() || !gui_bridge_is_uasync_ready()) {
            GUI_ERROR("send: file target/core unavailable"); m_inputBar->submissionResult(request, false); return;
        }
        if (isDmChannel(target)) {
            if (target != m_currentChannelId || !m_currentDmGeneration ||
                m_dmDeleting.value(dmConvId(target)) == m_currentDmGeneration) {
                GUI_WARN("PM file rejected: unavailable/deleting target=%s", qPrintable(target));
                m_inputBar->submissionResult(request, false); return;
            }
            auto* req = (struct dm_file_req*)u_calloc(1, sizeof(struct dm_file_req));
            if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "PM file request allocation failed"); m_inputBar->submissionResult(request, false); return; }
            req->generation = m_currentDmGeneration;
            QByteArray conv = dmConvId(target).toUtf8(), path = mediaSrc.toUtf8(), name = displayName.toUtf8();
            if (path.size() >= (int)sizeof(req->path) || name.size() >= (int)sizeof(req->info.name)) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "PM attachment path or name is too long");
                u_free(req); m_inputBar->submissionResult(request, false); return;
            }
            snprintf(req->conv, sizeof(req->conv), "%s", conv.constData());
            snprintf(req->path, sizeof(req->path), "%s", path.constData());
            req->info.kind = ATTACHMENT_FILE;
            snprintf(req->info.name, sizeof(req->info.name), "%s", name.constData());
            if (gui_bridge_post_bound_fn(dm_send_file_trampoline, req, &req->inst) < 0) {
                u_free(req); m_inputBar->submissionResult(request, false);
            }
            return;
        }

        QByteArray textData = displayName.toUtf8();
        auto ts = QDateTime::currentMSecsSinceEpoch();

        struct chat_msg_submit* req = (struct chat_msg_submit*)
            u_malloc(sizeof(struct chat_msg_submit) + textData.size());
        if (!req) { GUI_ERROR("send: file request allocation failed"); m_inputBar->submissionResult(request, false); return; }
        memset(req, 0, sizeof(*req));
        if (reply.size() == 32) memcpy(req->reply_to, reply.constData(), 32);
        memcpy(req->reply_quote, quote.constData(), size_t(quote.size()));
        req->request_id = request;
        snprintf(req->channel_id, sizeof(req->channel_id), "%s",
                 target.toUtf8().constData());
        snprintf(req->content_type, sizeof(req->content_type), "application/octet-stream");
        snprintf(req->media_src, sizeof(req->media_src), "%s", mediaSrc.toUtf8().constData());
        snprintf(req->media_dest, sizeof(req->media_dest), "%s", mediaDest.toUtf8().constData());
        req->media_copy = (mediaSrc != mediaDest) ? 1 : 0;
        req->data = (uint8_t*)(req + 1);
        req->data_len = (uint32_t)textData.size();
        memcpy(req->data, textData.constData(), textData.size());
        req->timestamp = (uint64_t)ts;
        if (gui_bridge_submit_message(req) < 0) { u_free(req); m_inputBar->submissionResult(request, false); }
    });

    connect(m_inputBar, &InputBar::sendImageAttachment, this, [this](const QString& target, const QString& mediaSrc, const QString& mediaDest,
                                                                      const QString& displayName, const QByteArray& reply, quint64 request, const QByteArray& quote) {
        if (target.isEmpty() || !gui_bridge_get_inst() || !gui_bridge_is_uasync_ready()) {
            GUI_ERROR("send: image target/core unavailable"); m_inputBar->submissionResult(request, false); return;
        }

        QByteArray textData = displayName.toUtf8();
        auto ts = QDateTime::currentMSecsSinceEpoch();

        QFileInfo fi(displayName);
        QString suf = fi.suffix().toLower();
        QString ct = "image/jpeg";
        if (suf == "png") ct = "image/png";
        else if (suf == "gif") ct = "image/gif";
        else if (suf == "bmp") ct = "image/bmp";
        else if (suf == "webp") ct = "image/webp";

        int iw = 0, ih = 0;
        QImageReader reader(mediaSrc);
        reader.setAutoTransform(true);
        if (reader.canRead()) {
            QSize sz = reader.size();
            if (sz.width() > 0 && sz.height() > 0) { iw = sz.width(); ih = sz.height(); }
        }

        struct chat_msg_submit* req = (struct chat_msg_submit*)
            u_malloc(sizeof(struct chat_msg_submit) + textData.size());
        if (!req) { GUI_ERROR("send: image request allocation failed"); m_inputBar->submissionResult(request, false); return; }
        memset(req, 0, sizeof(*req));
        if (reply.size() == 32) memcpy(req->reply_to, reply.constData(), 32);
        memcpy(req->reply_quote, quote.constData(), size_t(quote.size()));
        req->request_id = request;
        snprintf(req->channel_id, sizeof(req->channel_id), "%s",
                 target.toUtf8().constData());
        snprintf(req->content_type, sizeof(req->content_type), "%s", ct.toUtf8().constData());
        snprintf(req->media_src, sizeof(req->media_src), "%s", mediaSrc.toUtf8().constData());
        snprintf(req->media_dest, sizeof(req->media_dest), "%s", mediaDest.toUtf8().constData());
        req->media_copy = (mediaSrc != mediaDest) ? 1 : 0;
        req->media_video = 0;
        req->width = iw;
        req->height = ih;
        req->data = (uint8_t*)(req + 1);
        req->data_len = (uint32_t)textData.size();
        memcpy(req->data, textData.constData(), textData.size());
        req->timestamp = (uint64_t)ts;
        if (gui_bridge_submit_message(req) < 0) { u_free(req); m_inputBar->submissionResult(request, false); }
    });

    connect(AnimTimer::instance(), &AnimTimer::ticked, this, [this]() {
        m_view->viewport()->update();
    });

    connect(m_view, &ChatView::animationTriggered, AnimTimer::instance(), &AnimTimer::play);

    m_readDebounce = new QTimer(this);
    m_readDebounce->setSingleShot(true);
    m_readDebounce->setInterval(300);
    connect(m_readDebounce, &QTimer::timeout, this, &MessageList::updateReadPosition);
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged,
            m_readDebounce, QOverload<>::of(&QTimer::start));
}

/* Переименование не должно перезагружать сообщения или сбрасывать выбранное окно истории. */
void MessageList::refreshChannelTitle() {
    QString title = m_currentChannelId;
    if (m_db && !isDmChannel(m_currentChannelId)) {
        for (const auto& row : m_db->getChannels()) if (row.channelId == m_currentChannelId) { title = row.name; break; }
    }
    if (m_channelTitle) m_channelTitle->setText(title);
}

void MessageList::loadChannel(const QString& channelId) {
    if (m_messageMenu) m_messageMenu->close();
    ++m_jumpRevision;
    QPointer<QWidget> prevFocus = QApplication::focusWidget();
    if (channelId.isEmpty()) m_openingDmFiles.clear();

    const auto conversations = m_db && isDmChannel(channelId) ? m_db->getDmConversations() : QList<DmConversation>();
    quint64 generation = 0;
    bool closed = false;
    for (const auto& conv : conversations) if (conv.convId == dmConvId(channelId)) {
        generation = conv.generation; closed = conv.closed; break;
    }
    bool sameChannel = m_currentChannelId == channelId && m_currentDmGeneration == generation;

    /* Save state of current channel before switching */
    if (!m_currentChannelId.isEmpty()) {
        if (!sameChannel) {
            saveChannelState();
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "Radio: navigation from=%s to=%s active=%s",
                qPrintable(m_currentChannelId), qPrintable(channelId), qPrintable(RadioAudioEngine::instance()->groupId()));
        } else {
            QModelIndex topIdx = m_view->indexAt(QPoint(0, 0));
            m_savedTopMsgId = topIdx.isValid() ? topIdx.data(MsgIdRole).toLongLong() : 0;
            QScrollBar* sb = m_view->verticalScrollBar();
            m_wasAtBottom = sb && (sb->value() >= sb->maximum());
        }
    }

    if (!sameChannel) { m_historyAnchor.clear(); m_lastReadMsgId = 0; }
    if (!sameChannel && m_pollDialog) m_pollDialog->reject();
    m_view->setHistoricalWindow(!m_historyAnchor.isEmpty());
    m_model->clear();
    m_avatarCache.clear();
    if (m_currentChannelId != channelId) { m_feedback.clear(); m_pendingPoll.clear(); }
    m_currentChannelId = channelId;
    m_currentDmGeneration = generation;
    updateRadioUi();
    if (!sameChannel && m_keyRequestsDialog) m_keyRequestsDialog->close();
    refreshKeyRequests();
    clearStreams(); watchStreams();
    setStreamPublisherGroup(m_streamPublisherGroup);
    for (auto* button : {m_groupMenuButton, m_pollBtn, m_ownFilesBtn}) button->setVisible(channelId.size() == 64);
    clearDmCallRoute();
    m_callBtn->setVisible(isDmChannel(channelId));
    m_inviteBtn->setVisible(isDmChannel(channelId));
    m_peerStatus->setVisible(isDmChannel(channelId));
    m_radioBtn->setVisible(!channelId.isEmpty() && !isDmChannel(channelId));
    const bool dmAvailable = generation && !closed && m_dmDeleting.value(dmConvId(channelId)) != generation;
    m_inputBar->setEnabled(!channelId.isEmpty() && (!isDmChannel(channelId) || dmAvailable));
    m_inviteBtn->setEnabled(dmAvailable);
    m_inputBar->setRecordingContext(m_db ? m_db->dbPath() + "/media" : QString(), channelId, generation);
    m_readDebounce->stop();

    refreshChannelTitle();

    m_inputBar->setDmMode(isDmChannel(channelId));
    if (channelId.isEmpty()) {
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "message list cleared: no channel selected");
        return;
    }
    /* История и состояния PM поступают из ядра в одном снимке. */
    if (isDmChannel(channelId)) {
        m_radioBtn->setEnabled(false);
        if (m_db) {
            for (const auto& conv : conversations) {
                if (conv.convId != dmConvId(channelId)) continue;
                QString name = conv.peerGroup ? conv.peerName : m_db->getDisplayName(conv.peerEd25519);
                if (name.isEmpty()) name = conv.peerName;
                if (name.isEmpty()) name = QString::fromLatin1(conv.peerEd25519.toHex());
                m_channelTitle->setText(name);
                break;
            }
        }
        if (!generation) { GUI_WARN("PM selection unavailable: target=%s", qPrintable(channelId)); return; }
        refreshDmCallRoute();
        requestDmMessages(channelId);
        if (prevFocus)
            prevFocus->setFocus();
        else
            m_inputBar->setFocus();
        return;
    }
    m_radioBtn->setEnabled(true);
    m_inputBar->setMediaEnabled(true);

    if (!m_db || !m_db->isOpen()) return;

    m_localNicks = m_db->getLocalNicknames(channelId);

    auto msgs = m_db->getMessages(channelId, 200, m_historyAnchor);

    for (const auto& m : msgs) {
        AccountRow acc = m_db->getAccount(m.authorEd);
        QString author  = authorName(m.authorEd);
        QString letter  = acc.avatarLetter.isEmpty() ? author.mid(0, 1) : acc.avatarLetter;
        QColor  color(acc.avatarColor);

        if (!m_avatarCache.contains(m.authorEd)) {
            m_avatarCache[m.authorEd] = color;
        }

        auto avatar = makeAvatar(color, letter);
        if (auto* manager = AvatarManager::instance()) {
            const QImage image = manager->image(m_currentChannelId, m.authorEd);
            if (!image.isNull()) avatar = QPixmap::fromImage(image);
        }
        auto time   = QDateTime::fromMSecsSinceEpoch(m.timestamp).toString("HH:mm");

        QVariantList reacts;
        auto *item = new QStandardItem();
        setMsg(item, author, QString::fromUtf8(m.data), time, avatar, false,
               false, QString(), QString(), reacts);
        item->setData(m.id, MsgIdRole);
        item->setData(m.authorEd, MsgAuthorAddressRole);
        item->setData(m.isOutgoing ? 1 : 0, MsgIsOutgoingRole);
        setGroupAttachmentRoles(item, m.data, m.contentType,
                              m_db ? m_db->dbPath() + "/media" : QString(), channelId, m.localAttrs, m_db, m.messageId);
        m_model->appendRow(item);
        setupFeedback(item, m, channelId);
        updateReply(item, m.replyTo, m.replyQuote);
    }

    if (sameChannel) {
        bool wasBottom = m_wasAtBottom;
        qint64 savedTop = m_savedTopMsgId;
        QTimer::singleShot(0, this, [this, wasBottom, savedTop, channelId, anchor = m_historyAnchor]() {
            if (channelId != m_currentChannelId || anchor != m_historyAnchor) return;
            if (wasBottom || savedTop == 0) {
                m_view->scrollToBottom();
            } else {
                for (int row = 0; row < m_model->rowCount(); row++) {
                    if (m_model->item(row)->data(MsgIdRole).toLongLong() == savedTop) {
                        m_view->scrollTo(m_model->index(row, 0), QAbstractItemView::PositionAtTop);
                        break;
                    }
                }
            }
            m_readDebounce->start();
        });
    } else {
        m_lastReadMsgId = m_db->getUiStateInt64(
            QStringLiteral("ch_%1_last_read").arg(channelId), 0);
        restoreScrollPosition(channelId);
        m_readDebounce->start();
    }

    if (prevFocus)
        prevFocus->setFocus();
    else
        m_inputBar->setFocus();
}

bool MessageList::nearHistoryBottom() const {
    const auto* scrollbar = m_view->verticalScrollBar();
    const int gap = scrollbar->maximum() - scrollbar->value();
    const int threshold = 10 * m_view->fontMetrics().lineSpacing();
    GUI_DEBUG("history follow: ch=%s gap=%d threshold=%d near=%d", qPrintable(m_currentChannelId), gap, threshold, gap <= threshold);
    return gap <= threshold;
}

void MessageList::scrollHistoryToBottom() {
    m_view->doItemsLayout(); m_view->scrollToBottom();
    GUI_DEBUG("history bottom: ch=%s rows=%d position=%d maximum=%d", qPrintable(m_currentChannelId), m_model->rowCount(),
              m_view->verticalScrollBar()->value(), m_view->verticalScrollBar()->maximum());
    QTimer::singleShot(0, this, [this, channel = m_currentChannelId, generation = m_currentDmGeneration, anchor = m_historyAnchor] {
        if (channel != m_currentChannelId || generation != m_currentDmGeneration || anchor != m_historyAnchor) return;
        m_view->doItemsLayout(); m_view->scrollToBottom();
        GUI_DEBUG("history bottom after layout: ch=%s position=%d maximum=%d", qPrintable(channel),
                  m_view->verticalScrollBar()->value(), m_view->verticalScrollBar()->maximum());
    });
}

void MessageList::showLatestMessages() {
    if (!m_historyAnchor.isEmpty()) { m_historyAnchor.clear(); loadChannel(m_currentChannelId); }
    scrollHistoryToBottom();
}

void MessageList::refresh(bool outgoing) {
    if (m_currentChannelId.isEmpty()) return;
    if (outgoing) showLatestMessages();

    if (isDmChannel(m_currentChannelId)) {
        requestDmMessages(m_currentChannelId);
        return;
    }

    if (!m_db || !m_db->isOpen()) return;

    refreshReplies();
    if (!m_historyAnchor.isEmpty()) return;
    QSet<QString> present;
    for (int row = 0; row < m_model->rowCount(); row++) present.insert(m_model->item(row)->data(MsgMessageKeyRole).toString());
    auto newMsgs = m_db->getMessages(m_currentChannelId, 200);
    if (newMsgs.isEmpty()) return;

    m_localNicks = m_db->getLocalNicknames(m_currentChannelId);

    const bool wasAtBottom = nearHistoryBottom();
    bool added = false;

    for (const auto& m : newMsgs) {
        if (present.contains(QString::fromLatin1(m.messageId.toHex()))) continue;
        added = true;
        AccountRow acc = m_db->getAccount(m.authorEd);
        QString author  = authorName(m.authorEd);
        QString letter  = acc.avatarLetter.isEmpty() ? author.mid(0, 1) : acc.avatarLetter;
        QColor  color(acc.avatarColor);
        if (!m_avatarCache.contains(m.authorEd))
            m_avatarCache[m.authorEd] = color;
        auto avatar = makeAvatar(color, letter);
        if (auto* manager = AvatarManager::instance()) {
            const QImage image = manager->image(m_currentChannelId, m.authorEd);
            if (!image.isNull()) avatar = QPixmap::fromImage(image);
        }
        auto time   = QDateTime::fromMSecsSinceEpoch(m.timestamp).toString("HH:mm");
        auto *item = new QStandardItem();
        setMsg(item, author, QString::fromUtf8(m.data), time, avatar, false,
               false, QString(), QString(), QVariantList{});
        item->setData(m.id, MsgIdRole);
        item->setData(m.authorEd, MsgAuthorAddressRole);
        item->setData(m.isOutgoing ? 1 : 0, MsgIsOutgoingRole);
        setGroupAttachmentRoles(item, m.data, m.contentType,
                              m_db ? m_db->dbPath() + "/media" : QString(), m_currentChannelId, m.localAttrs, m_db, m.messageId);
        m_model->appendRow(item);
        setupFeedback(item, m, m_currentChannelId);
        updateReply(item, m.replyTo, m.replyQuote);
        while (m_model->rowCount() > 200)
            m_model->removeRow(0);
    }

    m_model->setSortRole(MsgRawTimestampRole); m_model->sort(0);
    if (added && wasAtBottom) scrollHistoryToBottom();
    m_readDebounce->start();
}

void MessageList::saveChannelState() {
    if (!m_db || !m_db->isOpen() || isDmChannel(m_currentChannelId)) return;
    if (!m_historyAnchor.isEmpty()) return; // Просмотр цитаты сохраняет прежнюю позицию чтения свежей истории.
    if (m_currentChannelId.isEmpty() || m_model->rowCount() == 0) return;

    QScrollBar* sb = m_view->verticalScrollBar();
    bool atBottom = sb && (sb->value() >= sb->maximum());

    qint64 lastPosMsgId  = 0;
    if (!atBottom) {
        QModelIndex topIdx = m_view->indexAt(QPoint(0, 0));
        if (topIdx.isValid())
            lastPosMsgId = topIdx.data(MsgIdRole).toLongLong();
    }

    qint64 lastReadMsgId = m_lastReadMsgId;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "MessageList::saveChannelState ch=%s bottom=%d read=%lld pos=%lld",
                qPrintable(m_currentChannelId), (int)atBottom, (long long)lastReadMsgId, (long long)lastPosMsgId);

    QByteArray cid = m_currentChannelId.toUtf8();
    auto postSaveKey = [&cid](const char* suffix, qint64 val) {
        QByteArray key = "ch_" + cid + suffix;
        QByteArray valStr = QByteArray::number(val);
        return gui_bridge_post_setting(GUI_SETTING_UI_STATE, key.constData(), valStr.constData()) == 0;
    };

    if (lastReadMsgId > 0)
        postSaveKey("_last_read", lastReadMsgId);
    postSaveKey("_last_pos", lastPosMsgId);
}

QString MessageList::authorName(const QByteArray& key) const {
    auto it = m_localNicks.find(key);
    if (it != m_localNicks.end() && !it->isEmpty()) return *it;
    return m_db->getDisplayName(key);
}

void MessageList::reloadLocalNicks() {
    if (m_currentChannelId.isEmpty() || !m_db) return;
    m_localNicks = m_db->getLocalNicknames(m_currentChannelId);
    for (int r = 0; r < m_model->rowCount(); r++) {
        auto* item = m_model->item(r);
        if (!item) continue;
        const QByteArray key = item->data(MsgAuthorAddressRole).toByteArray();
        if (key.size() == 32) item->setData(authorName(key), MsgAuthorRole);
    }
    if (!isDmChannel(m_currentChannelId)) refreshReplies();
    m_view->viewport()->update();
}

void MessageList::restoreScrollPosition(const QString& channelId) {
    if (!m_db || m_model->rowCount() == 0) {
        return;
    }

    qint64 lastPosMsgId = m_db->getUiStateInt64(
        QStringLiteral("ch_%1_last_pos").arg(channelId), 0);

    if (lastPosMsgId > 0) {
        for (int row = 0; row < m_model->rowCount(); row++) {
            QStandardItem* item = m_model->item(row);
            if (item && item->data(MsgIdRole).toLongLong() == lastPosMsgId) {
                m_view->scrollTo(m_model->index(row, 0), QAbstractItemView::PositionAtTop);
                return;
            }
        }
    }

    m_view->scrollToBottom();
}

void MessageList::updateReadPosition() {
    if (!m_db || !m_db->isOpen() || isDmChannel(m_currentChannelId)) return;
    if (!m_historyAnchor.isEmpty()) return;
    if (m_currentChannelId.isEmpty() || m_model->rowCount() == 0) return;

    int vpHeight = m_view->viewport()->height();
    int lastFullyVisibleRow = -1;
    qint64 newReadMsgId = m_lastReadMsgId;

    for (int row = m_model->rowCount() - 1; row >= 0; --row) {
        QModelIndex idx = m_model->index(row, 0);
        QRect vr = m_view->visualRect(idx);
        if (vr.isValid() && vr.top() >= 0 && vr.bottom() < vpHeight) {
            if (lastFullyVisibleRow < 0) lastFullyVisibleRow = row;
            newReadMsgId = qMax(newReadMsgId, m_model->item(row)->data(MsgIdRole).toLongLong());
        }
    }

    if (lastFullyVisibleRow >= 0) {
        GUI_DEBUG("read watermark: maximum visible row id=%lld", (long long)newReadMsgId);
    }

    QModelIndex topIdx = m_view->indexAt(QPoint(0, 0));
    QScrollBar* sb = m_view->verticalScrollBar();
    bool atBottom = sb && (sb->value() >= sb->maximum());
    qint64 lastPosMsgId = (!atBottom && topIdx.isValid()) ? topIdx.data(MsgIdRole).toLongLong() : 0;

    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT,
        "MessageList::updateReadPosition ch=%s oldRead=%lld newRead=%lld pos=%lld bottom=%d lastVisRow=%d/%d",
        qPrintable(m_currentChannelId), (long long)m_lastReadMsgId,
        (long long)newReadMsgId, (long long)lastPosMsgId, (int)atBottom,
        lastFullyVisibleRow, m_model->rowCount());

    QByteArray cid = m_currentChannelId.toUtf8();
    auto postSaveKey = [&cid](const char* suffix, qint64 val) {
        QByteArray key = "ch_" + cid + suffix;
        QByteArray valStr = QByteArray::number(val);
        return gui_bridge_post_setting(GUI_SETTING_UI_STATE, key.constData(), valStr.constData()) == 0;
    };

    if (newReadMsgId > m_lastReadMsgId) {
        if (postSaveKey("_last_read", newReadMsgId)) {
            m_lastReadMsgId = newReadMsgId;
            emit readPositionChanged(m_currentChannelId, m_lastReadMsgId);
        }
    }

    postSaveKey("_last_pos", lastPosMsgId);
}

void MessageList::cleanupAnimations() {
    size_t n = s_icons.size();
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "MessageList: cleanupAnimations clearing %zu icons", n);
    s_icons.clear();
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "MessageList: cleanupAnimations done");
}

void MessageList::updateMessageProgress(const QString& chId, int64_t msgId,
                                         int blocksDone, int numBlocks) {
    if (chId != m_currentChannelId) return;
    for (int r = 0; r < m_model->rowCount(); r++) {
        QStandardItem* item = m_model->item(r);
        if (!item) continue;
        if (item->data(MsgIdRole).toLongLong() == msgId) {
            item->setData(blocksDone, MsgMediaBlocksDoneRole);
            item->setData(numBlocks, MsgMediaNumBlocksRole);
            item->setData(1, MsgFileDownloadStateRole);
            m_view->viewport()->update();
            return;
        }
    }
}

void MessageList::onAttachmentDownloaded(const QString& chId, int64_t msgId) {
    if (chId != m_currentChannelId || !m_db || !m_db->isOpen()) return;

    MessageRow m = m_db->getMessageById(chId, msgId);
    if (m.id == 0) return;

    int row = -1;
    bool wasPending = false;
    for (int r = 0; r < m_model->rowCount(); r++) {
        QStandardItem* it = m_model->item(r);
        if (it && it->data(MsgIdRole).toLongLong() == msgId) {
            row = r;
            wasPending = it->data(MsgVoiceDownloadPendingRole).toBool();
            break;
        }
    }
    if (row < 0) return;

    QStandardItem* item = m_model->item(row);
    setGroupAttachmentRoles(item, m.data, m.contentType,
                         m_db->dbPath() + "/media", chId, m.localAttrs, m_db, m.messageId);
    item->setData(0, MsgMediaBlocksDoneRole);
    item->setData(false, MsgVoiceDownloadPendingRole);

    if (m.contentType == "audio/opus" && wasPending) {
        QString fp = item->data(MsgVoiceFileRole).toString();
        if (!fp.isEmpty() && QFile::exists(fp)) {
            QTimer::singleShot(500, this, [this, fp, chId]() {
                if (m_currentChannelId != chId) return;
                MessageDelegate::stopAllAutoPlay();
                VoicePlayer::playOpusFile(fp, this, [fp](float actualDur, uint64_t playbackId) { MessageDelegate::startAutoPlay(fp, actualDur, playbackId); });
            });
        }
    }

}

bool MessageList::eventFilter(QObject* object, QEvent* event) {
    if (object == m_streamBtn) {
        if (event->type() == QEvent::Hide) { m_streamHold.stop(); m_streamPressed = m_streamLongPressed = false; m_streamMenu->hide(); }
        else if (event->type() == QEvent::Leave) m_streamHold.stop();
        else if (event->type() == QEvent::ContextMenu) { showStreamMenu(); return true; }
        else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::RightButton) { showStreamMenu(); return true; }
            if (mouse->button() == Qt::LeftButton) { m_streamPressed = true; m_streamLongPressed = false; m_streamHold.start(); }
        } else if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            m_streamHold.stop(); m_streamPressed = false;
            if (m_streamLongPressed) { m_streamLongPressed = false; m_streamBtn->setDown(false); return true; }
        } else if (event->type() == QEvent::KeyPress && (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Down ||
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Menu)) {
            showStreamMenu(); return true;
        }
    }
    if (object == m_streamMenu) {
        if (event->type() == QEvent::Hide) {
            m_streamHold.stop(); m_streamPressed = m_streamLongPressed = false; m_streamBtn->setDown(false);
        } else if (event->type() == QEvent::ContextMenu) return true;
        else if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const bool overButton = m_streamBtn->rect().contains(m_streamBtn->mapFromGlobal(m_streamMenu->mapToGlobal(mouse->pos())));
            if (event->type() == QEvent::MouseButtonPress && overButton) {
                if (mouse->button() == Qt::RightButton) return true;
                if (mouse->button() == Qt::LeftButton) {
                    m_streamPressed = true; m_streamLongPressed = false; m_streamHold.start(); return true;
                }
            } else if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::RightButton && overButton) return true;
            else if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton && (m_streamPressed || m_streamLongPressed)) {
                const bool watchAll = overButton && !m_streamLongPressed;
                m_streamHold.stop(); m_streamPressed = m_streamLongPressed = false; m_streamBtn->setDown(false);
                if (watchAll) { m_streamMenu->hide(); m_streamBtn->click(); }
                return true;
            } else if (event->type() == QEvent::MouseMove && m_streamPressed && !overButton) {
                m_streamHold.stop(); m_streamPressed = false;
            }
        }
    }
    if (object->property("instantTooltip").toBool()) {
        auto* widget = static_cast<QWidget*>(object);
        if (event->type() == QEvent::Enter && widget->isEnabled())
            QToolTip::showText(widget->mapToGlobal(QPoint(widget->width() / 2, widget->height())), widget->toolTip(), widget);
        else if (event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Hide)
            QToolTip::hideText();
    }
    return QWidget::eventFilter(object, event);
}

/* Цитаты разрешаются независимо от окна истории; поздняя синхронизация обновляет уже видимые ответы. */
void MessageList::updateReply(QStandardItem* item, const QByteArray& original, const QString& quote) {
    item->setData(quote, MsgReplyQuoteRole);
    item->setData(QString::fromLatin1(original.toHex()), MsgReplyToRole);
    item->setData(!original.isEmpty(), MsgHasQuoteRole);
    if (original.isEmpty() || !m_db) return;
    const auto message = m_db->getMessageByKey(m_currentChannelId, original);
    const QString author = message.messageId.isEmpty() ? QCoreApplication::translate("ChatGui", "Reply") : authorName(message.authorEd);
    const QString preview = !quote.isEmpty() ? quote : message.messageId.isEmpty()
        ? QCoreApplication::translate("ChatGui", "Original message is not available yet") : DbManager::messagePreview(message);
    if (item->data(MsgQuoteAuthorRole).toString() == author && item->data(MsgQuoteTextRole).toString() == preview) return;
    item->setData(author, MsgQuoteAuthorRole); item->setData(preview, MsgQuoteTextRole);
    GUI_DEBUG("reply: preview ch=%s original=%s resolved=%d", qPrintable(m_currentChannelId), original.toHex().constData(),
              !message.messageId.isEmpty());
}

void MessageList::refreshReplies() {
    for (int row = 0; row < m_model->rowCount(); ++row) {
        auto* item = m_model->item(row);
        const QByteArray key = QByteArray::fromHex(item->data(MsgReplyToRole).toString().toLatin1());
        if (!key.isEmpty()) updateReply(item, key, item->data(MsgReplyQuoteRole).toString());
    }
}

/* Перезагружаем только ограниченное окно вокруг цели; обычный refresh сохраняет открытый участок истории. */
bool MessageList::jumpToMessage(const QString& messageId) {
    if (!m_db || isDmChannel(m_currentChannelId)) return false;
    const QByteArray key = QByteArray::fromHex(messageId.toLatin1());
    if (key.size() != 32) { GUI_ERROR("reply: invalid navigation key=%s", qPrintable(messageId)); return false; }
    bool present = false;
    for (int row = 0; row < m_model->rowCount(); ++row)
        if (m_model->item(row)->data(MsgMessageKeyRole).toString() == messageId) { present = true; break; }
    if (!present) {
        if (m_db->getMessageByKey(m_currentChannelId, key).messageId.isEmpty()) {
            GUI_WARN("reply: navigation original unavailable ch=%s id=%s", qPrintable(m_currentChannelId), qPrintable(messageId));
            QToolTip::showText(QCursor::pos(), QCoreApplication::translate("ChatGui", "Original message is not available yet"), m_view);
            return false;
        }
        m_historyAnchor = key;
        loadChannel(m_currentChannelId);
    }
    // Даже уже загруженный оригинал не должен вытесняться новыми сообщениями во время просмотра.
    m_historyAnchor = key;
    m_view->setHistoricalWindow(true);
    GUI_DEBUG("reply: navigate ch=%s id=%s loaded_window=%d", qPrintable(m_currentChannelId), qPrintable(messageId), !present);
    const QString channel = m_currentChannelId;
    const auto revision = ++m_jumpRevision;
    QTimer::singleShot(0, this, [this, channel, messageId, revision]() {
        if (channel != m_currentChannelId || revision != m_jumpRevision) return;
        for (int row = 0; row < m_model->rowCount(); ++row) {
            const QModelIndex index = m_model->index(row, 0);
            if (index.data(MsgMessageKeyRole).toString() != messageId) continue;
            m_view->scrollTo(index, QAbstractItemView::PositionAtCenter);
            m_model->setData(index, true, MsgHighlightRole);
            QTimer::singleShot(1500, this, [this, index = QPersistentModelIndex(index)]() {
                if (index.isValid()) m_model->setData(index, false, MsgHighlightRole);
            });
            return;
        }
        GUI_WARN("reply: navigation target disappeared ch=%s id=%s", qPrintable(channel), qPrintable(messageId));
    });
    return true;
}

void MessageList::onMessageSubmitted(const uint8_t* data, int len) {
    if (!data || len != 9 || data[8] > 2) { GUI_ERROR("send: invalid submission result bytes=%d", len); return; }
    quint64 request = 0;
    for (unsigned i = 0; i < 8; ++i) request |= quint64(data[i]) << (8 * i);
    m_inputBar->submissionResult(request, data[8] == 1, data[8] == 2);
    emit videoRecordingSubmitted(request, data[8] == 1, data[8] == 2);
}

/* Меняем существующий item даже в окне старой цитаты, сохраняя позицию прокрутки. */
void MessageList::onMessageUpdated(const uint8_t* data, int len) {
    if (!data || len < 34 || len != 1 + data[0] + 32) { GUI_ERROR("edit: invalid event bytes=%d", len); return; }
    const QString channel = QString::fromUtf8((const char*)data + 1, data[0]);
    if (channel != m_currentChannelId || !m_db) return;
    const QByteArray id((const char*)data + 1 + data[0], 32);
    const auto message = m_db->getMessageByKey(channel, id);
    if (message.messageId.isEmpty()) { GUI_WARN("edit: updated message disappeared ch=%s", qPrintable(channel)); return; }
    auto* sb = m_view->verticalScrollBar(); const bool atBottom = sb->value() == sb->maximum();
    const QPersistentModelIndex top(m_view->indexAt(QPoint(1, 1))); const int offset = m_view->visualRect(top).top();
    for (int row = 0; row < m_model->rowCount(); ++row) {
        auto* item = m_model->item(row);
        if (item->data(MsgMessageKeyRole).toString() != QString::fromLatin1(id.toHex())) continue;
        if (item->data(MsgVersionRole).toULongLong() >= message.version) break;
        item->setData(QString::fromUtf8(message.data), MsgTextRole);
        setupFeedback(item, message, channel); updateReply(item, message.replyTo, message.replyQuote); break;
    }
    refreshReplies(); m_view->doItemsLayout();
    if (atBottom) m_view->scrollToBottom();
    else if (top.isValid()) { m_view->scrollTo(top, QAbstractItemView::PositionAtTop); sb->setValue(sb->value() - offset); }
    GUI_DEBUG("edit: row refreshed ch=%s id=%s ver=%llu", qPrintable(channel), id.toHex().constData(),
        (unsigned long long)message.version);
}

void MessageList::onMessageContextMenu(const QPoint& pos) {
    QPersistentModelIndex idx(m_view->indexAt(pos));
    if (!idx.isValid()) return;

    QString ct = idx.data(MsgContentTypeRole).toString();
    bool isVideo = (ct.startsWith("video/"));
    bool isImage = ct.startsWith("image/");
    bool isDmFile = (isDmChannel(idx.data(MsgChannelIdRole).toString()) || idx.data(MsgEncryptedRole).toBool()) && ct == "application/octet-stream";
    bool isDm = isDmChannel(idx.data(MsgChannelIdRole).toString());

    QString filePath = idx.data(MsgVoiceFileRole).toString();
    QString displayName = idx.data(MsgFileDisplayNameRole).toString();
    bool downloaded = !filePath.isEmpty() && QFile::exists(filePath);

    QMenu menu;
    connect(this, &QObject::destroyed, &menu, &QMenu::close);
    QPointer<MessageList> guard(this);
    QString messageId = idx.data(MsgMessageKeyRole).toString();
    if (!isDm && messageId.size() == 64) {
        const QString channel = idx.data(MsgChannelIdRole).toString();
        connect(menu.addAction(QCoreApplication::translate("ChatGui", "Copy link")), &QAction::triggered, this,
            [this, channel, messageId]() {
                if (!m_db) return;
                ChannelRow group;
                for (const auto& row : m_db->getChannels()) if (row.channelId == channel) { group = row; break; }
                const auto message = m_db->getMessageByKey(channel, QByteArray::fromHex(messageId.toLatin1()));
                const auto link = makeObjectLink(group.ed25519_pubkey, group.name, {}, {}, messageId, DbManager::messagePreview(message));
                if (!link.isEmpty()) { QApplication::clipboard()->setText(link); GUI_INFO("object link copied ch=%s message=%s", qPrintable(channel), qPrintable(messageId)); }
            });
        if (idx.data(MsgIsOutgoingRole).toBool() && (ct == "text" || ct == "text/plain")) {
            connect(menu.addAction(QCoreApplication::translate("ChatGui", "Edit message")), &QAction::triggered, this,
                [this, channel, messageId] {
                if (channel != m_currentChannelId || !m_db) return;
                const auto message = m_db->getMessageByKey(channel, QByteArray::fromHex(messageId.toLatin1()));
                if (message.isOutgoing && message.messageId.size() == 32)
                    m_inputBar->editMessage(message.messageId, message.version, QString::fromUtf8(message.data));
            });
        }
        connect(menu.addAction(QCoreApplication::translate("ChatGui", "Reply")), &QAction::triggered, this, [this, channel, messageId]() {
            replyToMessage(channel, messageId);
        });
        const QString selected = m_view->selectedText(idx);
        if ((ct == "text" || ct == "text/plain") && !selected.trimmed().isEmpty()) {
            connect(menu.addAction(QCoreApplication::translate("ChatGui", "Quote selection")), &QAction::triggered, this,
                [this, idx, channel, messageId, selected] {
                    if (channel != m_currentChannelId || !idx.isValid() || idx.data(MsgMessageKeyRole).toString() != messageId) {
                        GUI_WARN("quote: selection target changed ch=%s", qPrintable(channel)); return;
                    }
                    m_inputBar->setReply(QByteArray::fromHex(messageId.toLatin1()), idx.data(MsgAuthorRole).toString(), {}, selected);
                    GUI_INFO("quote selected ch=%s original=%s chars=%d", qPrintable(channel), qPrintable(messageId), int(selected.size()));
                });
        }
        if (ct == "poll") connect(menu.addAction(QCoreApplication::translate("ChatGui", "Vote / results")), &QAction::triggered,
            this, [this, messageId]() { showPoll(messageId); });
        connect(menu.addAction(QCoreApplication::translate("ChatGui", "Clear reactions")), &QAction::triggered,
            this, [this, messageId]() { sendFeedback({{"cmd", "react"}, {"message_id", messageId}, {"clear", true}}); });
    }
    QAction* saveAct = nullptr;
    if (isVideo || isImage || isDmFile) {
        saveAct = menu.addAction(QCoreApplication::translate("ChatGui", "Save to file")); saveAct->setEnabled(downloaded);
    }
    QAction* dlAct = nullptr;
    if ((isVideo || isImage || isDmFile || (isDm && ct == "audio/opus")) && !downloaded)
        dlAct = menu.addAction(QCoreApplication::translate("ChatGui", "Download"));

    QAction* chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
    if (!guard) return;
    if (!idx.isValid()) { GUI_WARN("message menu cancelled: row was replaced while menu was open"); return; }

    if (saveAct && chosen == saveAct) {
        QString base = displayName;
        if (base.isEmpty()) base = QFileInfo(filePath).fileName();
        QString dest = QFileDialog::getSaveFileName(this, QCoreApplication::translate("ChatGui", "Save file"), base);
        if (dest.isEmpty()) return;
        std::thread([filePath, dest]() {
            if (!QFile::copy(filePath, dest))
                DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "MessageList: save failed src=%s dst=%s", qPrintable(filePath), qPrintable(dest));
            else DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "MessageList: saved %s -> %s", qPrintable(filePath), qPrintable(dest));
        }).detach();
        return;
    }

    if (dlAct && chosen == dlAct) {
        if (isDm) { downloadDmFile(idx); return; }
        qint64 msgId = idx.data(MsgIdRole).toLongLong();
        QString chId = idx.data(MsgChannelIdRole).toString();
        if (chId.isEmpty() || msgId <= 0) return;
        gui_bridge_download_attachment(chId.toUtf8().constData(), msgId);
    }
}

/* ── DM (p2p чат) ── */

bool MessageList::downloadDmFile(const QModelIndex& index) {
    if (index.data(MsgChannelIdRole).toString() != m_currentChannelId || !isDmChannel(m_currentChannelId) ||
        !m_currentDmGeneration) return false;
    const QByteArray author = index.data(MsgAuthorAddressRole).toByteArray();
    if (author.size() != 32) { GUI_WARN("PM download: missing author key"); return false; }
    auto* req = (struct dm_media_action_req*)u_calloc(1, sizeof(struct dm_media_action_req));
    if (!req) { GUI_WARN("PM download: request allocation failed"); return false; }
    strncpy(req->conv, dmConvId(m_currentChannelId).toUtf8().constData(), sizeof(req->conv) - 1);
    memcpy(req->author, author.constData(), sizeof(req->author)); req->seq = index.data(MsgIdRole).toULongLong();
    req->generation = m_currentDmGeneration;
    if (gui_bridge_post_bound_fn(dm_media_action_trampoline, req, &req->inst) == 0) return true;
    u_free(req); GUI_WARN("PM download: core unavailable"); return false;
}

void MessageList::onAttachmentOpened(const QString& path) {
    auto found = m_openingDmFiles.find(path);
    if (found == m_openingDmFiles.end()) return;
    const auto action = found.value(); m_openingDmFiles.erase(found);
    auto* req = (struct dm_media_action_req*)u_calloc(1, sizeof(struct dm_media_action_req));
    if (!req) { GUI_WARN("PM opened: request allocation failed"); return; }
    strncpy(req->conv, action.conv.toUtf8().constData(), sizeof(req->conv) - 1);
    memcpy(req->author, action.author.constData(), sizeof(req->author)); req->seq = action.seq; req->generation = action.generation; req->opened = 1;
    if (gui_bridge_post_bound_fn(dm_media_action_trampoline, req, &req->inst) < 0) {
        u_free(req); GUI_WARN("PM opened: core unavailable");
    }
}

void MessageList::clearDmCallRoute() {
    m_callPeer.clear(); m_callGroup.clear();
    m_callBtn->setEnabled(false);
    m_callBtn->setToolTip(QCoreApplication::translate("ChatGui", "Peer unavailable"));
    m_peerStatus->setStyleSheet("background: #E53935; border-radius: 6px;");
    m_peerStatus->setToolTip(QCoreApplication::translate("ChatGui", "Offline"));
}

void MessageList::refreshDmCallRoute() {
    if (!isDmChannel(m_currentChannelId)) return;
    auto* req = (struct dm_messages_req*)u_calloc(1, sizeof(struct dm_messages_req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM route request: allocation failed"); clearDmCallRoute(); return; }
    QByteArray conv = dmConvId(m_currentChannelId).toUtf8();
    snprintf(req->conv_id, sizeof(req->conv_id), "%s", conv.constData());
    if (gui_bridge_post_bound_fn(dm_call_route_trampoline, req, &req->inst) < 0) { u_free(req); clearDmCallRoute(); }
}

void MessageList::onDmCallRoute(const uint8_t* data, int len) {
    if (!data || len < 66 || !data[0] || len != 65 + data[0]) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "PM route snapshot: invalid size=%d", len); return;
    }
    QString conv = QString::fromUtf8((const char*)data + 1, data[0]);
    if (m_currentChannelId != QStringLiteral("dm:") + conv) return;
    const QByteArray peer(reinterpret_cast<const char*>(data + 1 + data[0]), 32);
    const QByteArray group(reinterpret_cast<const char*>(data + 33 + data[0]), 32);
    const QByteArray empty(32, '\0');
    const bool available = group != empty && peer != empty;
    if (m_callPeer != peer || m_callGroup != group)
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "PM button: conv=%s peer=%s group=%s enabled=%d",
                   conv.toUtf8().constData(), peer.toHex().constData(), group.toHex().constData(), available);
    m_callPeer = available ? peer : QByteArray(); m_callGroup = group;
    m_callBtn->setEnabled(available);
    m_peerStatus->setStyleSheet(available ? "background: #4CAF50; border-radius: 6px;" : "background: #E53935; border-radius: 6px;");
    m_peerStatus->setToolTip(available ? QCoreApplication::translate("ChatGui", "Online") : QCoreApplication::translate("ChatGui", "Offline"));
    m_callBtn->setToolTip(available ? QCoreApplication::translate("ChatGui", "Call peer") : QCoreApplication::translate("ChatGui", "Peer unavailable"));
}

bool MessageList::isDmChannel(const QString& channelId) {
    return channelId.startsWith(QStringLiteral("dm:"));
}

/* Ожидающее удаление блокирует только старое поколение, сохраняя просмотр другой беседы. */
void MessageList::setDmDeleting(const QString& convId, quint64 generation, bool deleting) {
    if (deleting) m_dmDeleting.insert(convId, generation);
    else if (m_dmDeleting.value(convId) == generation) m_dmDeleting.remove(convId);
    if (m_currentChannelId != QStringLiteral("dm:") + convId || m_currentDmGeneration != generation) return;
    m_inputBar->setEnabled(!deleting); m_inviteBtn->setEnabled(!deleting);
    if (!deleting) requestDmMessages(m_currentChannelId);
}

/* После commit удаляем только UI старого поколения; повторно открытый PM остаётся выбранным. */
bool MessageList::forgetDmHistory(const QString& convId, quint64 generation) {
    const QString target = QStringLiteral("dm:") + convId;
    m_inputBar->forgetDmHistory(target, generation);
    emit videoRecordingInvalidated(target, generation);
    if (m_currentChannelId != target || m_currentDmGeneration != generation) return false;
    loadChannel(QString());
    return true;
}

QString MessageList::dmConvId(const QString& channelId) {
    return channelId.mid(3);
}

void MessageList::requestDmMessages(const QString& channelId) {
    if (!m_currentDmGeneration || m_dmDeleting.value(dmConvId(channelId)) == m_currentDmGeneration) return;
    if (!gui_bridge_get_inst() || !gui_bridge_is_uasync_ready()) {
        GUI_WARN("PM history request skipped: core unavailable target=%s", qPrintable(channelId)); return;
    }
    QString conv = dmConvId(channelId);
    struct dm_messages_req* req = (struct dm_messages_req*)u_malloc(sizeof(*req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "PM history request allocation failed"); return; }
    memset(req, 0, sizeof(*req));
    req->generation = m_currentDmGeneration;
    strncpy(req->conv_id, conv.toUtf8().constData(), sizeof(req->conv_id) - 1);
    if (gui_bridge_post_bound_fn(dm_messages_trampoline, req, &req->inst) < 0) u_free(req);
}

void MessageList::onAttachmentPreparation(const uint8_t* data, int len) {
    if (!data || len < 18 || len != 18 + data[0] || data[len - 1] > 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Invalid attachment preparation event bytes=%d", len); return;
    }
    int length = data[0];
    m_inputBar->updatePreparation(QString::fromUtf8((const char*)data + 1, length),
        QByteArray((const char*)data + 1 + length, 16), data[len - 1]);
}

void MessageList::onDmMessages(const uint8_t* data, int len) {
    if (!data || len < 10 || !data[0] || len <= 9 + data[0]) {
        GUI_ERROR("PM history: invalid snapshot bytes=%d", len); return;
    }
    uint8_t cl = data[0];
    QString convId = QString::fromUtf8((const char*)data + 1, cl);
    quint64 generation; memcpy(&generation, data + 1 + cl, 8);
    if (m_currentChannelId != QStringLiteral("dm:") + convId || !generation || generation != m_currentDmGeneration ||
        m_dmDeleting.value(convId) == generation) {
        GUI_DEBUG("PM history: stale snapshot conv=%s generation=%llu", qPrintable(convId), (unsigned long long)generation); return;
    }

    QJsonParseError error;
    auto document = QJsonDocument::fromJson(QByteArray((const char*)data + 9 + cl, len - 9 - cl), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "Invalid PM history JSON: %s", qPrintable(error.errorString())); return;
    }
    auto* scrollbar = m_view->verticalScrollBar();
    bool atBottom = m_model->rowCount() == 0 || scrollbar->value() >= scrollbar->maximum();
    const bool nearBottom = nearHistoryBottom();
    QSet<QString> previousMessages;
    for (int row = 0; row < m_model->rowCount(); ++row) previousMessages.insert(m_model->item(row)->data(MsgMessageKeyRole).toString());
    bool newMessage = false, outgoingMessage = false;
    int previousScroll = scrollbar->value();
    m_model->clear();
    m_avatarCache.clear();

    for (const auto& entry : document.array()) {
        auto message = entry.toObject();
        int dir = message.value("dir").toInt();
        qint64 seq = (qint64)message.value("seq").toDouble();
        qint64 ts = (qint64)message.value("ts").toDouble();
        const QString address = message.value("author_ed25519").toString();
        const QByteArray authorEd = QByteArray::fromHex(address.toLatin1());
        const QString messageKey = address + ':' + QString::number(seq);
        if (!previousMessages.contains(messageKey)) { newMessage = true; if (dir == 1) outgoingMessage = true; }
        QString text = message.value("data").toString();
        const bool invite = message.value("ct").toString() == QStringLiteral("invite");
        const QString inviteLink = invite ? text : QString();
        if (invite) {
            const QString groupName = message.value("tags").toObject().value("group_name").toString();
            text = QCoreApplication::translate("ChatGui", "Invitation to group “%1”").arg(groupName) + '\n' + inviteLink;
            GUI_DEBUG("invite PM: rendered conv=%s seq=%lld group_name_bytes=%d link_bytes=%d", qPrintable(m_currentChannelId),
                      (long long)seq, (int)groupName.toUtf8().size(), (int)inviteLink.size());
        }
        bool file = message.value("ct").toString() == "file";
        QString state = message.value("media_status").toString();
        QString status;
        if (file) {
            if (state == "delivered") status = QCoreApplication::translate("ChatGui", "Delivered");
            else if (state == "expired") status = QCoreApplication::translate("ChatGui", "Storage period expired");
            else if (state == "ready") status = QCoreApplication::translate("ChatGui", "Received");
            else if (state == "downloading") status = QCoreApplication::translate("ChatGui", "Downloading…");
            else if (state == "unavailable") status = QCoreApplication::translate("ChatGui", "File unavailable");
            else status = QCoreApplication::translate("ChatGui", "Download");
        } else if (message.value("source").toBool()) status = message.value("pending").toBool() ? QCoreApplication::translate("ChatGui",
            "Awaiting delivery") : QCoreApplication::translate("ChatGui",
            "Delivered");

        AccountRow acc = m_db ? m_db->getAccount(authorEd) : AccountRow();
        QString name = m_db ? m_db->getDisplayName(authorEd) : QString();
        if (name.isEmpty()) name = address;
        QString letter = acc.avatarLetter.isEmpty() ? name.mid(0, 1) : acc.avatarLetter;
        QColor color(acc.avatarColor);
        if (!m_avatarCache.contains(authorEd))
            m_avatarCache[authorEd] = color;
        auto avatar = makeAvatar(color, letter);
        if (auto* manager = AvatarManager::instance()) {
            const QImage image = manager->image(m_currentChannelId, authorEd);
            if (!image.isNull()) avatar = QPixmap::fromImage(image);
        }
        auto time = QDateTime::fromMSecsSinceEpoch((qint64)ts).toString("HH:mm");
        if (!file && !status.isEmpty()) time += QStringLiteral(" · ") + status;

        auto *item = new QStandardItem();
        setMsg(item, name, text, time, avatar, false,
               false, QString(), QString(), QVariantList{});
        item->setData((qint64)seq, MsgIdRole);
        item->setData(messageKey, MsgMessageKeyRole);
        item->setData(authorEd, MsgAuthorAddressRole);
        item->setData(dir == 1 ? 1 : 0, MsgIsOutgoingRole);
        item->setData(m_currentChannelId, MsgChannelIdRole);
        QString type = message.value("content_type").toString(QStringLiteral("text/plain"));
        item->setData(type, MsgContentTypeRole);
        if (invite) item->setData(inviteLink, MsgInviteLinkRole);
        if (file) {
            QString path = message.value("media_path").toString();
            item->setData(path, MsgVoiceFileRole);
            item->setData(text, MsgFileDisplayNameRole);
            item->setData((qint64)message.value("media_size").toDouble(), MsgFileSizeRole);
            item->setData(path.isEmpty() ? (state == "downloading" ? 1 : 0) : 2, MsgFileDownloadStateRole);
            item->setData(status, MsgDeliveryStateRole);
            int duration = message.value("duration_ms").toInt();
            if (type == "audio/opus") {
                item->setData(QString::number(duration / 1000.0, 'f', 1) + "s", MsgVoiceDurationRole);
                item->setData((qint64)message.value("media_size").toDouble(), MsgVoiceFileSizeRole);
                QVariantList waveform;
                const QString levels = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
                for (QChar level : message.value("waveform").toString()) waveform.append(qMax(0, levels.indexOf(level)) / 63.0);
                item->setData(waveform, MsgVoiceWaveformRole);
            } else if (type.startsWith("video/")) {
                item->setData(duration, MsgVideoDurationMsRole);
                item->setData(message.value("width").toInt(), MsgVideoWidthRole);
                item->setData(message.value("height").toInt(), MsgVideoHeightRole);
            } else if (type.startsWith("image/")) {
                item->setData(message.value("width").toInt(), MsgImageWidthRole);
                item->setData(message.value("height").toInt(), MsgImageHeightRole);
            }
        }
        m_model->appendRow(item);
    }

    if (atBottom || outgoingMessage || (newMessage && nearBottom)) scrollHistoryToBottom();
    else { m_view->doItemsLayout(); scrollbar->setValue(previousScroll); }
}

/* ── рация ── */

void MessageList::setRadioMembers(MemberListModel* members) { m_radioPanel->setMembers(members); }

/* Каталог выбранного канала не удерживает media owner; DM/пустой target снимает наблюдение. */
void MessageList::watchStreams() {
    gui_stream_watch(m_currentChannelId.size() == 64 ? m_currentChannelId : QString());
}

void MessageList::clearStreams() {
    m_streamMenu->hide();
    m_streamSources.clear(); m_streamBtn->setText(QString());
    m_streamBtn->hide();
    m_streamBtn->setToolTip(QCoreApplication::translate("ChatGui", "Live streams"));
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop stream header cleared channel=%s visible=0", qPrintable(m_currentChannelId));
}

void MessageList::showStreamMenu() {
    if (!m_streamBtn->isVisible() || !m_streamBtn->isEnabled() || m_streamMenu->isVisible()) return;
    m_streamMenu->clear();
    const QString channel = m_currentChannelId;
    for (const auto& source : m_streamSources) {
        if (source.self) continue;
        auto* action = m_streamMenu->addAction(QStringLiteral("%1 · %2").arg(source.name).arg(QString::fromLatin1(source.id.toHex())));
        connect(action, &QAction::triggered, this, [this, channel, source] {
            const bool available = channel == m_currentChannelId && std::any_of(m_streamSources.cbegin(), m_streamSources.cend(),
                [&source](const LiveStreamSource& current) { return !current.self && current.id == source.id && current.epoch == source.epoch; });
            if (!available) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop stale stream selection group=%s current=%s source=%s epoch=%llx",
                    qPrintable(channel), qPrintable(m_currentChannelId), source.id.toHex().constData(), (unsigned long long)source.epoch);
                return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop selected stream group=%s source=%s epoch=%llx action=watch",
                qPrintable(channel), source.id.toHex().constData(), (unsigned long long)source.epoch);
            emit streamRequested(channel, source.id, source.epoch, source.name);
        });
    }
    if (m_streamMenu->isEmpty()) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop stream source menu group=%s sources=%d", qPrintable(channel), int(m_streamMenu->actions().size()));
    m_streamMenu->popup(m_streamBtn->mapToGlobal(QPoint(0, m_streamBtn->height())));
}

/* Состояние своей трансляции берётся у publisher, а не из сетевого каталога. */
void MessageList::setStreamPublisherGroup(const QString& group) {
    const bool visible = m_currentChannelId.size() == 64;
    const bool changed = group != m_streamPublisherGroup || visible != !m_streamPublishBtn->isHidden();
    if (group != m_streamPublisherGroup) m_streamPublisherViewers = 0;
    m_streamPublisherGroup = group;
    m_streamPublishBtn->setVisible(visible);
    m_streamPublishBtn->setToolTip(QCoreApplication::translate("ChatGui", !group.isEmpty() ? "Manage stream" : "Start stream"));
    m_streamPublishBtn->setAccessibleName(m_streamPublishBtn->toolTip()); updateHeaderIcons();
    if (changed) DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop publish header channel=%s publisher_group=%s visible=%d active=%d",
        qPrintable(m_currentChannelId), qPrintable(group), visible, !group.isEmpty());
}

/* Снимок обновляется независимо от окна управления; повторное число не перерисовывает шапку. */
void MessageList::setStreamPublisherViewers(const QString& group, unsigned viewers) {
    if (group.isEmpty() || group != m_streamPublisherGroup || viewers == m_streamPublisherViewers) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop publisher viewer badge group=%s count=%u previous=%u",
        qPrintable(group), viewers, m_streamPublisherViewers);
    m_streamPublisherViewers = viewers; updateHeaderIcons();
}

/* Полный снимок применяется атомарно; ответы предыдущего канала не меняют текущую шапку. */
void MessageList::onStreamSources(const uint8_t* data, int len) {
    StreamSourceSnapshot snapshot;
    if (!parseStreamSources(data, len, snapshot)) return;
    const QString& group = snapshot.group;
    if (isDmChannel(m_currentChannelId) || group != m_currentChannelId) return;
    if (!snapshot.valid) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop source catalogue unavailable group=%s", qPrintable(group)); return;
    }
    const bool changed = m_streamSources.size() != snapshot.sources.size() || !std::equal(m_streamSources.cbegin(), m_streamSources.cend(),
        snapshot.sources.cbegin(), [](const LiveStreamSource& a, const LiveStreamSource& b) {
            return a.id == b.id && a.epoch == b.epoch && a.name == b.name && a.self == b.self;
        });
    if (changed) m_streamMenu->hide();
    m_streamSources = std::move(snapshot.sources);
    const auto remote = std::count_if(m_streamSources.begin(), m_streamSources.end(), [](const LiveStreamSource& source) { return !source.self; });
    m_streamBtn->setVisible(remote > 0);
    m_streamBtn->setText(remote > 1 ? QString::number(remote) : QString());
    m_streamBtn->setToolTip(QCoreApplication::translate("ChatGui", "Live streams: %1").arg(remote));
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop stream header group=%s sources=%d remote=%d visible=%d",
               qPrintable(group), (int)m_streamSources.size(), (int)remote, remote > 0);
}

bool MessageList::enableRadioForCall(quint64 id) {
    const bool wasActive = RadioAudioEngine::instance()->isActive()
        && RadioAudioEngine::instance()->groupId() == m_currentChannelId;
    bool ready = enableRadio();
    if (ready && !wasActive) { m_radioCallOwner = id; m_radioCallGroup = RadioAudioEngine::instance()->groupId(); }
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call activation id=%llx ready=%d owned=%d", (unsigned long long)id, ready, !wasActive);
    return ready;
}

void MessageList::finishRadioCall(quint64 id, bool accepted) {
    if (m_radioCallOwner != id) return;
    m_radioCallOwner = 0;
    if (!accepted) {
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call activation cancelled id=%llx", (unsigned long long)id);
        if (RadioAudioEngine::instance()->groupId() == m_radioCallGroup) RadioAudioEngine::instance()->stop();
    }
}

bool MessageList::enableRadio() {
    if (m_currentChannelId.isEmpty() || isDmChannel(m_currentChannelId)) return false;
    if (RadioAudioEngine::instance()->groupId() != m_currentChannelId) onRadioToggled(true);
    updateRadioUi();
    return m_radioBtn->isChecked();
}

void MessageList::disableRadioForPhone() {
    m_radioCallOwner = 0;
    RadioAudioEngine::instance()->stop();
    updateRadioUi();
}

void MessageList::onRadioToggled(bool on) {
    m_radioCallOwner = 0;
    const QString group = m_currentChannelId;
    if (group.size() != 64) { updateRadioUi(); return; }
    auto* engine = RadioAudioEngine::instance();
    if (on) {
        const QString previous = engine->groupId();
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "Radio: activate previous=%s requested=%s",
            qPrintable(previous), qPrintable(group));
        if (engine->start(group) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Radio: activation failed group=%s", qPrintable(group));
            auto* notice = new QMessageBox(QMessageBox::Warning, QCoreApplication::translate("ChatGui", "Radio"),
                QCoreApplication::translate("ChatGui", "Could not start group radio"), QMessageBox::Ok, this);
            notice->setAttribute(Qt::WA_DeleteOnClose); notice->open();
        }
    } else if (engine->groupId() == group) engine->stop();
    updateRadioUi();
}

void MessageList::updateRadioUi() {
    auto* engine = RadioAudioEngine::instance();
    const bool show = engine->isActive() && !engine->groupId().isEmpty() && engine->groupId() == m_currentChannelId;
    QSignalBlocker blocker(m_radioBtn);
    m_radioBtn->setChecked(show);
    m_radioPanel->setVisible(show);
}

void MessageList::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() != QEvent::PaletteChange) return;
    updateHeaderIcons();
    GUI_DEBUG("Message header: palette changed, icon color=%s", qPrintable(palette().buttonText().color().name()));
}

void MessageList::updateHeaderIcons() {
    const QColor color = palette().color(QPalette::ButtonText);
    if (m_radioBtn) m_radioBtn->setIcon(makeRadioIcon(color));
    if (m_streamPublishBtn) m_streamPublishBtn->setIcon(headerActionIcon(HeaderActionIcon::Publish,
        !m_streamPublisherGroup.isEmpty() ? QColor("#e53935") : color));
    if (m_streamViewerBadge) {
        m_streamViewerBadge->setText(QString::number(m_streamPublisherViewers));
        m_streamViewerBadge->setFixedSize(std::max(18, m_streamViewerBadge->sizeHint().width()), 14);
        m_streamPublishBtn->setFixedWidth(std::max(34, m_streamViewerBadge->width() + 4));
        m_streamViewerBadge->move(m_streamPublishBtn->width() - m_streamViewerBadge->width(), m_streamPublishBtn->height() - 14);
        m_streamViewerBadge->setVisible(!m_streamPublisherGroup.isEmpty() && m_streamPublisherViewers > 0);
        QString tooltip = QCoreApplication::translate("ChatGui", !m_streamPublisherGroup.isEmpty() ? "Manage stream" : "Start stream");
        if (!m_streamPublisherGroup.isEmpty()) tooltip += '\n' + QCoreApplication::translate("ChatGui", "Viewers: %1").arg(m_streamPublisherViewers);
        m_streamPublishBtn->setToolTip(tooltip); m_streamPublishBtn->setAccessibleName(tooltip);
    }
    if (m_pollBtn) m_pollBtn->setIcon(headerActionIcon(HeaderActionIcon::Poll, color));
    if (m_ownFilesBtn) m_ownFilesBtn->setIcon(headerActionIcon(HeaderActionIcon::OwnFiles, color));
}

void MessageList::setVideoRecordingEnabled(bool enabled) { m_inputBar->setVideoRecordingEnabled(enabled); }
