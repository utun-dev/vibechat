#include "kanbanview.h"
#include "message_links.h"
#include <QClipboard>
#include "emojipanel.h"
#include "inputbar.h"
#include "formattingpanel.h"
#include "media_preview_loader.h"
#include <QMimeDatabase>
#include "kanban_card_list.h"
#include "kanban_inline_editor.h"
#include "memberlistmodel.h"
#include "chatview.h"
#include "messagedelegate.h"
#include "group_attachment_roles.h"
#include <QStandardItemModel>
#include <QGroupBox>
#include <QToolButton>
#include <QStyle>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QLocale>
#include <algorithm>
#include "../db/db_manager.h"
#include <QButtonGroup>
#include <QScopeGuard>
#include <QMouseEvent>
#include "../transport/gui_bridge.h"
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDrag>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextEdit>
#include <QTimer>
#include <QVariantAnimation>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QtEndian>
#include <functional>
extern "C" {
#include "kanban/kanban.h"
#include "attachments/download.h"
#include "attachments/manifest.h"
#include "media_async/attachment_send.h"
#include "chat/chat_event.h"
#include "chat/chat_core.h"
#include "mem.h"
#include "debug_config.h"
#include "db_async.h"
}
static QPointer<KanbanView> activeBoard;
static QString trb(const char* text) { return QCoreApplication::translate("ChatGui", text); }

struct CardDownload { struct UTUN_INSTANCE* inst; struct attachment_ref reference; };
static void cardDownloadDone(void* data, int error, const char*) {
    auto* request = static_cast<CardDownload*>(data);
    if (error) DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "card attachment download failed group=%s", request->reference.space);
    const QByteArray key = QByteArray::fromHex(request->reference.space);
    if (key.size() == 32) chat_event_post(request->inst, CHAT_EVT_KANBAN_CHANGED, reinterpret_cast<const uint8_t*>(key.constData()), key.size());
    u_free(request);
}
static void cardDownloadStart(void* data) {
    auto* request = static_cast<CardDownload*>(data);
    attachment_group_download(request->inst, &request->reference, cardDownloadDone, request);
}

static QString historyAction(const QString& action) {
    if (action == QStringLiteral("card_create")) return trb("Card created");
    if (action == QStringLiteral("card_edit")) return trb("Card edited");
    if (action == QStringLiteral("card_move")) return trb("Card moved");
    if (action == QStringLiteral("card_delete")) return trb("Card deleted");
    return {};
}


/* Линия ровно 1px; широкая прозрачная зона позволяет удобно тянуть границу мышью. */
class KanbanColumnGrip final : public QWidget {
public:
    std::function<void(int)> resized;
    std::function<void(bool)> interacting;
    explicit KanbanColumnGrip(QWidget* pane, QWidget* parent) : QWidget(parent), m_pane(pane) {
        setObjectName("kanbanColumnGrip"); setFixedWidth(7); setCursor(Qt::SplitHCursor);
    }
protected:
    void paintEvent(QPaintEvent*) override { QPainter p(this); p.fillRect(QRect(width() / 2, 0, 1, height()), palette().mid()); }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !m_pane) return;
        m_start = globalX(event); m_width = m_pane->width(); m_active = true;
        if (interacting) interacting(true); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_active || !m_pane) return;
        m_pane->setFixedWidth(qBound(220, m_width + globalX(event) - m_start, 1200)); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override { if (event->button() == Qt::LeftButton) finish(); }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::UngrabMouse) finish();
        return QWidget::event(event);
    }
private:
    static int globalX(QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return qRound(event->globalPosition().x());
#else
        return event->globalPos().x();
#endif
    }
    void finish() {
        if (!m_active) return;
        m_active = false;
        if (m_pane && resized) resized(m_pane->width());
        if (interacting) interacting(false);
    }
    QPointer<QWidget> m_pane;
    int m_start = 0, m_width = 0;
    bool m_active = false;
};

KanbanView::KanbanView(QWidget* parent, DbManager* db) : QWidget(parent), m_db(db), m_layout(new QVBoxLayout(this)),
    m_scroll(new QScrollArea(this)), m_status(new QLabel(this)) {
    activeBoard = this;
    gui_bridge_set_kanban_cb([](const uint8_t* data, int size) { if (activeBoard) activeBoard->receive(data, size); });
    setAutoFillBackground(true); setBackgroundRole(QPalette::Base);
    auto* headerBar = new QWidget(this); headerBar->setAutoFillBackground(true); headerBar->setBackgroundRole(QPalette::Window);
    auto* bar = new QHBoxLayout(headerBar); bar->setContentsMargins(0, 0, 0, 0);
    auto* invite = new QPushButton(trb("Invite"), this); auto* settings = new QPushButton(trb("Group settings"), this);
    auto* add = new QPushButton(trb("Add column"), this);
    bar->addWidget(add); bar->addStretch(); bar->addWidget(invite); bar->addWidget(settings); m_layout->addWidget(headerBar);
    connect(invite, &QPushButton::clicked, this, [this] { emit inviteRequested(m_group); });
    connect(settings, &QPushButton::clicked, this, [this] { emit settingsRequested(m_group); });
    connect(add, &QPushButton::clicked, this, [this] {
        if (!m_board["can_manage"].toBool()) return;
        bool ok = false; QString name = QInputDialog::getText(this, trb("Add column"), trb("Column name"), QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty()) send({{"action", "column_create"}, {"text", name.trimmed()}});
    });
    add->setObjectName("kanbanAddColumn");
    m_scroll->setWidgetResizable(true); m_scroll->setFrameShape(QFrame::NoFrame);
    m_layout->addWidget(m_scroll, 1); m_layout->addWidget(m_status);
    setFocusPolicy(Qt::StrongFocus);
    auto* detailsScroll = new QScrollArea(this); detailsScroll->setWidgetResizable(true); detailsScroll->setFrameShape(QFrame::NoFrame);
    m_details = detailsScroll; m_details->setObjectName("kanbanDetails");
    auto* detailBody = new QWidget(detailsScroll); detailsScroll->setWidget(detailBody);
    auto* detail = new QVBoxLayout(detailBody); detail->setContentsMargins(16, 16, 16, 16); detail->setSpacing(10);
    detailBody->setObjectName("kanbanDetailBody");
    detailBody->setStyleSheet("QGroupBox { border: 1px solid palette(mid); border-radius: 8px; margin-top: 10px; padding: 12px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; }"
        "QPushButton { padding: 5px 8px; } QComboBox, QDateTimeEdit { padding: 4px; }");
    auto* header = new QHBoxLayout; auto* caption = new QLabel(trb("Card details"), detailBody);
    QFont captionFont = caption->font(); captionFont.setBold(true); caption->setFont(captionFont); header->addWidget(caption, 1);
    auto* close = new QToolButton(detailBody); close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    close->setObjectName("kanbanCloseCard"); close->setAutoRaise(true); close->setToolTip(trb("Close card")); close->setAccessibleName(trb("Close card")); header->addWidget(close);
    detail->addLayout(header); connect(close, &QToolButton::clicked, this, [this] { selectCard({}); });
    auto* copyLink = new QPushButton(trb("Copy link"), detailBody); header->insertWidget(1, copyLink);
    connect(copyLink, &QPushButton::clicked, this, [this] {
        ChannelRow group;
        if (m_db) for (const auto& row : m_db->getChannels()) if (row.channelId == m_group) { group = row; break; }
        const auto link = makeObjectLink(group.ed25519_pubkey, group.name, m_selectedCard, m_detailCard["text"].toString(), {}, {});
        if (!link.isEmpty()) QApplication::clipboard()->setText(link);
    });
    m_created = new QLabel(m_details); m_created->setWordWrap(true); detail->addWidget(m_created);
    auto* peopleBox = new QGroupBox(trb("People"), detailBody); auto* people = new QVBoxLayout(peopleBox); people->setSpacing(8);
    detail->addWidget(peopleBox);
    auto* authorRow = new QHBoxLayout; people->addLayout(authorRow);
    m_cardAuthor = new QComboBox(m_details); m_cardAuthor->setAccessibleName(trb("Author")); m_cardAuthor->setMinimumContentsLength(8);
    m_cardAuthor->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); authorRow->addWidget(m_cardAuthor, 1);
    auto* transfer = new QPushButton(trb("Transfer authorship"), m_details); transfer->setObjectName("kanbanTransferAuthor");
    authorRow->addWidget(transfer);
    connect(transfer, &QPushButton::clicked, this, [this] {
        send({{"action", "card_author"}, {"entity", m_selectedCard}, {"target", m_cardAuthor->currentData().toString()},
            {"grant", m_detailCard["grant"]}});
    });
    m_assigned = new QLabel(m_details); m_assigned->setWordWrap(true); people->addWidget(m_assigned);
    auto* dueBox = new QGroupBox(trb("Due date"), detailBody); auto* dueLayout = new QVBoxLayout(dueBox); detail->addWidget(dueBox);
    m_hasDue = new QCheckBox(trb("Due date"), m_details); dueLayout->addWidget(m_hasDue);
    auto* dueRow = new QHBoxLayout; dueLayout->addLayout(dueRow);
    m_due = new QDateTimeEdit(QDateTime::currentDateTime(), m_details); m_due->setCalendarPopup(true);
    m_due->setDisplayFormat("yyyy-MM-dd HH:mm"); dueRow->addWidget(m_due, 1);
    auto* due = new QPushButton(trb("Save due date"), m_details); due->setObjectName("kanbanSaveDue"); dueRow->addWidget(due);
    connect(due, &QPushButton::clicked, this, [this] {
        const auto value = m_hasDue->isChecked() ? QString::number(m_due->dateTime().toMSecsSinceEpoch() * 1000) : QString();
        const auto request = send({{"action", "card_due"}, {"entity", m_selectedCard}, {"grant", m_detailCard["grant"]}, {"text", value}});
        if (request) m_dueRequests[request] = {m_detailKey, value};
    });
    auto dueChanged = [this] {
        if (!m_fillingDetails && !m_detailKey.isEmpty())
            m_dueDrafts[m_detailKey] = m_hasDue->isChecked() ? QString::number(m_due->dateTime().toMSecsSinceEpoch() * 1000) : QString();
    };
    connect(m_hasDue, &QCheckBox::toggled, this, dueChanged);
    connect(m_due, &QDateTimeEdit::dateTimeChanged, this, dueChanged);
    auto* discussion = new QLabel(trb("Discussion"), detailBody); QFont heading = discussion->font(); heading.setBold(true);
    discussion->setFont(heading); detail->addWidget(discussion);
    auto* older = new QPushButton(trb("Earlier messages"), m_details); older->setObjectName("kanbanOlderMessages"); detail->addWidget(older);
    connect(older, &QPushButton::clicked, this, [this] { if (!m_messageBefore.isEmpty()) loadMessages(m_messageBefore); });
    m_messages = new ChatView(m_details); m_messages->setObjectName("kanbanMessages"); m_messages->setContextMenuPolicy(Qt::CustomContextMenu);
    m_messageModel = new QStandardItemModel(m_messages); m_messages->setModel(m_messageModel);
    new MediaPreviewLoader(m_messages, m_messageModel, m_db ? m_db->dbPath() : QString());
    auto* messageDelegate = new MessageDelegate(m_messages); messageDelegate->setReactionsEnabled(false);
    m_messages->setItemDelegate(messageDelegate);
    m_messages->setLinkHitTest([this, messageDelegate](const QModelIndex& index, const QPoint& point) {
        QStyleOptionViewItem option; option.initFrom(m_messages); option.rect = m_messages->visualRect(index); option.widget = m_messages;
        return messageDelegate->linkAt(option, index, point);
    });
    connect(m_messages, &ChatView::linkActivated, this, &KanbanView::linkActivated);
    connect(m_messages, &ChatView::latestRequested, this, [this] {
        ++m_targetRevision; m_historyAround.clear(); m_jumpMessage.clear(); m_chatRequest = 0; m_chatRows.clear();
        m_messages->setHistoricalWindow(false); loadMessages();
    });
    messageDelegate->setImageOpenCallback([this](const QString& path) { emit openImageRequested(path); });
    messageDelegate->setVideoOpenCallback([this](const QString& path) { emit openVideoRequested(path); });
    messageDelegate->setDownloadCallback([this](const QModelIndex& index) {
        const auto row = index.data(Qt::UserRole).toJsonObject(); const auto message = QByteArray::fromHex(row["id"].toString().toLatin1());
        const auto body = row["d"].toString().toUtf8(); struct media_index_result manifest = {};
        if (message.size() != 32 || attachment_manifest_parse(body.constData(), &manifest)) return false;
        auto* request = static_cast<CardDownload*>(u_calloc(1, sizeof(CardDownload)));
        if (!request) { media_index_result_free(&manifest); DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card download allocation failed"); return false; }
        request->reference.owner = ATTACHMENT_OWNER_KANBAN; qstrncpy(request->reference.space, m_group.toLatin1().constData(), sizeof(request->reference.space));
        memcpy(request->reference.message, message.constData(), 32); memcpy(request->reference.object, manifest.media_id, 16);
        media_index_result_free(&manifest);
        if (gui_bridge_post_bound_fn(cardDownloadStart, request, &request->inst) < 0) { u_free(request); return false; }
        return true;
    });
    m_messages->setMinimumHeight(160);
    detail->addWidget(m_messages, 2);
    connect(m_messages, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        const auto index = m_messages->indexAt(point); if (!index.isValid()) return;
        const auto message = index.data(Qt::UserRole).toJsonObject();
        const auto card = m_selectedCard, group = m_group;
        QMenu menu; auto* copy = menu.addAction(trb("Copy link"));
        bool editable = !message["deleted"].toBool() && message["author"] == m_board["self"] && m_board["can_write"].toBool();
        auto* edit = editable ? menu.addAction(trb("Edit message")) : nullptr;
        auto* remove = editable ? menu.addAction(trb("Delete message")) : nullptr;
        QPointer<KanbanView> guard(this); auto* action = menu.exec(m_messages->viewport()->mapToGlobal(point));
        if (!guard || card != m_selectedCard || group != m_group) return;
        if (action == copy) {
            ChannelRow channel;
            if (m_db) for (const auto& row : m_db->getChannels()) if (row.channelId == group) { channel = row; break; }
            MessageRow row; row.contentType = message["ct"].toString(); row.data = message["text"].toString().toUtf8();
            const auto preview = DbManager::messagePreview(row);
            const auto link = makeObjectLink(channel.ed25519_pubkey, channel.name, card, m_detailCard["text"].toString(), message["id"].toString(), preview);
            if (!link.isEmpty()) QApplication::clipboard()->setText(link);
        } else if (action && action == remove) send({{"action", "message_delete"}, {"entity", card}, {"before", message["id"]}});
        else if (action && action == edit) {
            bool ok = false; const auto text = QInputDialog::getMultiLineText(this, trb("Edit message"), trb("Text"), message["text"].toString(), &ok);
            if (guard && ok && !text.trimmed().isEmpty()) send({{"action", "message_edit"}, {"entity", card}, {"before", message["id"]}, {"text", text}}, group);
        }
    });
    m_emojiPanel = new EmojiPanel(m_messages->viewport());
    auto* formatting = new FormattingPanel(m_messages->viewport());
    m_composer = new InputBar(m_emojiPanel, formatting, nullptr, m_details); detail->addWidget(m_composer);
    m_composer->setObjectName("kanbanComposer");
    connect(m_composer, &InputBar::videoRecordingRequested, this, [this](const QString& group, quint64, const QByteArray&) {
        if (group != m_group || m_selectedCard.isEmpty() || !m_board["can_write"].toBool()) {
            DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "card video recording rejected target=%s group=%s card=%s can_write=%d",
                qPrintable(group), qPrintable(m_group), qPrintable(m_selectedCard), m_board["can_write"].toBool()); return;
        }
        DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "card video recording requested group=%s card=%s", qPrintable(group), qPrintable(m_selectedCard));
        emit videoRecordingRequested(group, m_selectedCard, m_detailCard["text"].toString().section('\n',0,0).left(80));
    });
    m_composer->installFileDropTarget(m_messages->viewport());
    m_chatInput = m_composer->findChild<QTextEdit*>(); m_chatInput->setObjectName("kanbanChatInput");
    connect(m_chatInput, &QTextEdit::textChanged, this, [this] {
        if (!m_fillingDetails && !m_detailKey.isEmpty()) m_chatDrafts[m_detailKey] = m_chatInput->toPlainText();
    });
    connect(m_composer, &InputBar::sendMessage, this, [this](const QString& text, const QByteArray&, quint64 composer) {
        const auto request = send({{"action", "message_create"}, {"entity", m_selectedCard}, {"text", text}});
        if (request) m_chatSubmissions[request] = {composer, m_detailKey, text};
        else m_composer->submissionResult(composer, false);
    });
    connect(m_composer, &InputBar::submitAttachment, this, &KanbanView::submitAttachment);
    auto file = [this](const QString& target, const QString& source, const QString&, const QString& name,
                       const QByteArray&, quint64 composer, bool image) {
        const auto path = source.toUtf8(), filename = name.toUtf8();
        auto* request = static_cast<attachment_send_req*>(u_calloc(1, sizeof(attachment_send_req)));
        if (!request || path.size() >= int(sizeof(request->source)) || filename.size() >= int(sizeof(request->info.name))) {
            DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card file allocation/path failed source=%s", qPrintable(source));
            u_free(request); m_composer->submissionResult(composer, false); return;
        }
        request->info.kind = ATTACHMENT_FILE; request->request_id = composer;
        memcpy(request->source, path.constData(), size_t(path.size())); memcpy(request->info.name, filename.constData(), size_t(filename.size()));
        if (image) snprintf(request->content_type, sizeof(request->content_type), "%s",
            qPrintable(QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension).name()));
        submitAttachment(target, request);
    };
    connect(m_composer, &InputBar::sendFileAttachment, this, [file](auto target, auto source, auto dest, auto name, auto reply,
                                                                quint64 request, const QByteArray&) {
        file(target, source, dest, name, reply, request, false);
    });
    connect(m_composer, &InputBar::sendImageAttachment, this, [file](auto target, auto source, auto dest, auto name, auto reply,
                                                                 quint64 request, const QByteArray&) {
        file(target, source, dest, name, reply, request, true);
    });
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "card composer ready: shared InputBar, clipboard images, file previews, reactions disabled");
    m_details->hide();
    if (m_db) connect(m_db, &DbManager::databaseChanged, this, [this] {
        m_widthGeneration++; m_widths.clear(); m_widthReads.clear();
        DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "column width cache reset database_generation=%llu", (unsigned long long)m_widthGeneration);
    });

}
KanbanView::~KanbanView() {
    m_destroying = true;
    if (m_inline && !m_editRequest) finishInline();
    closeInline();
    for (auto* widget : findChildren<QWidget*>()) if (auto* grip = dynamic_cast<KanbanColumnGrip*>(widget)) {
        grip->resized = {}; grip->interacting = {}; // UngrabMouse при уничтожении детей уже не обращается к полям владельца.
    }
    if (activeBoard == this) { activeBoard.clear(); gui_bridge_set_kanban_cb(nullptr); }
}
void KanbanView::setGroup(const QString& group) {
    if (m_group != group) {
        if (m_inline) {
            finishInline();
            if (m_inline) { m_draft.text = m_inline->text(); m_draft.color = m_inline->color(); m_drafts[m_editGroup] = m_draft; closeInline(); }
        }
        selectCard({}); m_group = group; m_nextColumn.clear(); m_nextCard.clear(); m_board = {}; m_status->clear(); render();
    }
    reload();
}
void KanbanView::reload() { if (!m_group.isEmpty()) send({{"action", "snapshot"}}); }
void KanbanView::openTarget(const QString& card, const QString& message) {
    ++m_targetRevision; m_targetCard = card; m_targetMessage = message; m_chatRequest = 0;
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "link target requested group=%s card=%s message=%s revision=%llu", qPrintable(m_group),
        qPrintable(card), qPrintable(message), (unsigned long long)m_targetRevision);
    m_targetBoardRequest = send({{"action", "snapshot"}});
    if (!m_targetBoardRequest) { m_targetCard.clear(); m_targetMessage.clear(); emit targetUnavailable(trb("Object is not available yet.")); }
}
void KanbanView::finishEditing() { finishInline(); }
quint64 KanbanView::send(QJsonObject command, const QString& target) {
    const QString channel = target.isEmpty() ? m_group : target;
    const QByteArray group = channel.toLatin1();
    if (!chat_channel_valid(group.constData())) return 0;
    QByteArray json = QJsonDocument(command).toJson(QJsonDocument::Compact);
    auto* req = static_cast<kanban_request*>(u_calloc(1, sizeof(kanban_request) + size_t(json.size()) + 1));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "desktop board request allocation failed"); m_status->setText(trb("Could not save changes.")); return 0; }
    qstrncpy(req->group, group.constData(), sizeof(req->group)); req->request = ++m_next; memcpy(req->json, json.constData(), size_t(json.size()));
    m_pending.insert(req->request, command["action"].toString());
    m_requestGroups.insert(req->request, channel);
    quint64 id = req->request;
    if (gui_bridge_post_bound_fn(kanban_gui_command, req, &req->inst) < 0) {
        m_pending.remove(id); m_requestGroups.remove(id); u_free(req); m_status->setText(trb("Connection is not ready.")); return 0;
    }
    return id;
}
void KanbanView::receive(const uint8_t* data, int size) {
    if (size == 32) { if (QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(data), 32).toHex()) == m_group) reload(); return; }
    if (size < 40) return;
    const QString group = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(data + 8), 32).toHex());
    quint64 id = qFromLittleEndian<quint64>(data);
    if (!m_pending.contains(id) || m_requestGroups.take(id) != group) return;
    QString action = m_pending.take(id); auto response = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(data + 40), size - 40)).object();
    if (m_dueRequests.contains(id)) {
        const auto saved = m_dueRequests.take(id);
        if (response["ok"].toBool() && m_dueDrafts.value(saved.first) == saved.second) m_dueDrafts.remove(saved.first);
    }
    if (m_chatSubmissions.contains(id)) {
        const auto submission = m_chatSubmissions.take(id);
        if (response["ok"].toBool() && m_chatDrafts.value(submission.key) == submission.text) m_chatDrafts.remove(submission.key);
        m_composer->submissionResult(submission.composer, response["ok"].toBool());
        if (m_emojiPanel && response["ok"].toBool()) m_emojiPanel->hide();
    }
    if (!response["ok"].toBool()) {
        if (group == m_group && ((id == m_targetBoardRequest && !m_targetCard.isEmpty()) || (id == m_chatRequest && !m_jumpMessage.isEmpty()))) {
            m_targetCard.clear(); m_targetMessage.clear(); m_jumpMessage.clear(); emit targetUnavailable(trb("Object is not available yet."));
        }
        if (id == m_chatRequest) { m_chatRequest = 0; m_chatReloadPending = false; }
        DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "desktop board action=%s error=%s", qPrintable(action), qPrintable(response["error"].toString()));
        m_status->setText(trb("Could not save changes. Check your permissions and try again."));
        if (id == m_editRequest) {
            m_editRequest = 0; m_drafts[m_editGroup] = m_draft;
            if (m_inline) m_inline->showError(m_status->text());
            else continueEditing();
        }
        refreshDetails();
        return;
    }
    bool savedEditor = id == m_editRequest;
    QString savedCard;
    if (savedEditor) {
        m_editRequest = 0; m_drafts.remove(m_editGroup);
        if (group == m_group && m_draft.creating && m_selectedCard.isEmpty()) savedCard = response["id"].toString();
        closeInline(); m_editGroup.clear();
    }
    if (response.contains("history")) {
        auto* dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(trb("Card history"));
        auto* layout = new QVBoxLayout(dialog); auto* text = new QTextEdit(dialog); text->setReadOnly(true);
        QStringList lines;
        for (const auto& value : response["history"].toArray()) {
            auto row = value.toObject(), body = row["body"].toObject();
            lines << QString("%1 · %2 · %3%4\n%5").arg(QDateTime::fromMSecsSinceEpoch(row["time"].toVariant().toLongLong() / 1000).toString(Qt::ISODate),
                row["author"].toString(), historyAction(body["action"].toString()), row["active"].toBool() ? QString() : trb(" (archived)"), body["text"].toString());
        }
        text->setPlainText(lines.join("\n\n")); layout->addWidget(text); dialog->resize(650, 450); dialog->show(); return;
    }
    if (response.contains("messages")) {
        if (group != m_group || response["entity"].toString() != m_selectedCard || id != m_chatRequest) return;
        m_chatRequest = 0;
        if (!m_jumpMessage.isEmpty() && !response["target_found"].toBool()) {
            m_jumpMessage.clear(); emit targetUnavailable(trb("Object is not available yet.")); return;
        }
        auto rows = response["messages"].toArray(); const bool more = rows.size() > 50;
        if (more) rows.removeLast();
        m_messageBefore = more && !rows.isEmpty() ? rows.last().toObject()["id"].toString() : QString();
        for (const auto& value : rows) { auto row = value.toObject(); m_chatRows[row["id"].toString()] = row; }
        m_details->findChild<QPushButton*>("kanbanOlderMessages")->setEnabled(more);
        if (!m_chatRefreshUntil.isEmpty() && !m_chatRows.contains(m_chatRefreshUntil) && more) {
            loadMessages(m_messageBefore); return;
        }
        m_chatRefreshUntil.clear(); renderMessages();
        if (m_chatReloadPending) { m_chatReloadPending = false; loadMessages(); }
        return;
    }
    if (group == m_group && response.contains("board")) {
        if (id >= m_displayed) { m_displayed = id; m_board = response["board"].toObject(); }
        m_status->clear(); readWidths(); render();
        if (!m_targetCard.isEmpty() && id == m_targetBoardRequest) {
            const auto card = m_targetCard, message = m_targetMessage; m_targetCard.clear(); m_targetMessage.clear();
            QJsonObject found;
            for (const auto& value : m_board["cards"].toArray()) if (value.toObject()["id"].toString() == card) found = value.toObject();
            if (found.isEmpty() || found["deleted"].toBool()) {
                DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "link card unavailable group=%s card=%s deleted=%d", qPrintable(m_group),
                    qPrintable(card), found["deleted"].toBool());
                selectCard({}); emit targetUnavailable(trb("Object is not available yet.")); return;
            }
            selectCard(card, false); m_historyAround = message; m_jumpMessage = message;
            m_chatRows.clear(); m_chatRequest = 0; m_chatRefreshUntil.clear();
            m_messages->setHistoricalWindow(!message.isEmpty()); loadMessages(); return;
        }
        if (!savedCard.isEmpty()) selectCard(savedCard);
        refreshDetails();
        if (!m_selectedCard.isEmpty()) loadMessages();
    } else if (savedEditor && m_renderPending) render();
    if (savedEditor) continueEditing();
}

/* Ширины — локальное состояние: один READ для новых ключей, живое изменение мышью всегда новее его результата. */
void KanbanView::readWidths() {
    if (!m_db || !m_db->isOpen() || m_group.isEmpty()) return;
    QStringList keys;
    for (const auto& value : m_board["columns"].toArray()) {
        const auto column = value.toObject(); if (column["deleted"].toBool()) continue;
        const QString key = QStringLiteral("kanban_%1_%2_width").arg(m_group, column["id"].toString());
        if (!m_widths.contains(key) && !m_widthReads.contains(key)) { keys.append(key); m_widthReads.insert(key); }
    }
    if (keys.isEmpty()) return;
    const auto generation = m_widthGeneration; const auto group = m_group;
    auto work = [keys](DbReadView& view, QVariant* value) {
        QVariantMap widths;
        for (const auto& key : keys) widths.insert(key, view.getUiStateInt(key, 290));
        *value = std::move(widths); return SQLITE_OK;
    };
    auto complete = [this, generation, group, keys](const db_async_result& status, const QVariant& value) {
        if (generation != m_widthGeneration) return;
        for (const auto& key : keys) m_widthReads.remove(key);
        if (status.status != DB_ASYNC_OK || !value.canConvert<QVariantMap>()) {
            DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "column width READ failed group=%s status=%d rc=%d: %s",
                qPrintable(group), status.status, status.sqlite_code, status.message); return;
        }
        const auto widths = value.toMap();
        for (const auto& key : keys) if (!m_widths.contains(key)) m_widths.insert(key, qBound(220, widths.value(key, 290).toInt(), 1200));
        DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "column width READ complete group=%s columns=%d", qPrintable(group), int(keys.size()));
        if (group != m_group || !m_scroll->widget()) return;
        if (m_dragging || m_pointerDown || m_inline) { m_renderPending = true; return; }
        for (const auto& value : m_board["columns"].toArray()) {
            const auto column = value.toObject(); const auto id = column["id"].toString();
            const QString key = QStringLiteral("kanban_%1_%2_width").arg(group, id);
            if (auto* pane = m_scroll->widget()->findChild<QWidget*>("kanbanColumn_" + id)) pane->setFixedWidth(m_widths.value(key, 290));
        }
    };
    if (!m_db->hasAsyncReader()) {
        const char* path = sqlite3_db_filename(m_db->m_db, "main"); QVariant value; int rc = SQLITE_MISUSE;
        if (!path || !*path) { DbReadView view(m_db->m_db); work(view, &value); rc = view.sqlError(); }
        else DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "column width READ requires asynchronous file database backend");
        const db_async_result status{rc == SQLITE_OK ? DB_ASYNC_OK : DB_ASYNC_SQL, rc, "RAM column width read failed"};
        complete(status, value); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "column width READ queued group=%s columns=%d", qPrintable(group), int(keys.size()));
    if (!m_db->readAsync(this, work, complete)) {
        const db_async_result status{DB_ASYNC_CLOSED, SQLITE_OK, "Column width read scheduling rejected"}; complete(status, {});
    }
}

void KanbanView::render() {
    if (m_destroying) return;
    if (m_dragging || m_pointerDown || m_inline) { m_renderPending = true; return; }
    m_renderPending = false;
    if (m_board["pending_history"].toBool()) m_status->setText(trb("Waiting for board history…"));
    int scroll = m_scroll->horizontalScrollBar()->value();
    QHash<QString, QPair<int, bool>> columnScroll;
    for (auto* widget : m_scroll->findChildren<QListWidget*>()) if (auto* list = dynamic_cast<CardList*>(widget); list && list->group == m_group) {
        auto* bar = list->verticalScrollBar(); columnScroll[list->column] = {bar->value(), bar->value() == bar->maximum()};
    }
    delete m_scroll->takeWidget(); auto* content = new QWidget(m_scroll); auto* columns = new QHBoxLayout(content);
    QPalette background = palette(); background.setColor(QPalette::Window, background.base().color());
    content->setPalette(background); content->setAutoFillBackground(true);
    m_scroll->viewport()->setPalette(background); m_scroll->viewport()->setAutoFillBackground(true);
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "board background group=%s surface=%s header=%s", qPrintable(m_group),
        qPrintable(background.window().color().name()), qPrintable(palette().window().color().name()));
    columns->setSpacing(0); columns->setContentsMargins(0, 0, 0, 0);
    if (auto* add = findChild<QPushButton*>("kanbanAddColumn")) add->setEnabled(m_board["can_manage"].toBool());
    QJsonArray live;
    for (const auto& value : m_board["columns"].toArray()) if (!value.toObject()["deleted"].toBool()) live.append(value);
    for (int i = 0; i < live.size(); i++) {
        auto column = live[i].toObject(); auto* pane = new QWidget(content);
        pane->setObjectName("kanbanColumn_" + column["id"].toString());
        QString widthKey = QStringLiteral("kanban_%1_%2_width").arg(m_group, column["id"].toString());
        int width = m_widths.value(widthKey, 290);
        pane->setFixedWidth(qBound(220, width, 1200));
        auto saveWidth = [this, widthKey](int width) {
            m_widths[widthKey] = width;
            if (gui_bridge_post_setting(GUI_SETTING_UI_STATE, qPrintable(widthKey), qPrintable(QString::number(width))) < 0)
                DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "column width save unavailable key=%s width=%d", qPrintable(widthKey), width);
            else DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "local column width queued key=%s width=%d", qPrintable(widthKey), width);
        };
        auto* layout = new QVBoxLayout(pane); layout->setContentsMargins(0, 6, 0, 0); auto* title = new QPushButton(column["name"].toString(), pane);
        int builtin = column["builtin"].toInt();
        if (builtin) title->setText(trb(builtin == 1 ? "New" : builtin == 2 ? "In progress" : "Done"));
        layout->addWidget(title);
        connect(title, &QPushButton::clicked, this, [this, column, live, i, title, pane, saveWidth] {
            QPointer<KanbanView> self(this); m_dragging = true;
            auto interaction = qScopeGuard([self] {
                if (!self) return; self->m_dragging = false;
                if (self->m_renderPending) QTimer::singleShot(0, self, [self] { if (self) self->render(); });
            });
            QMenu menu; auto* rename = menu.addAction(trb("Rename column")); auto* left = menu.addAction(trb("Move left"));
            auto* right = menu.addAction(trb("Move right")); auto* remove = menu.addAction(trb("Delete column"));
            menu.addSeparator(); auto* width = menu.addAction(trb("Column width…"));
            bool manage = m_board["can_manage"].toBool();
            rename->setEnabled(manage); remove->setEnabled(manage); left->setEnabled(manage && i > 0); right->setEnabled(manage && i + 1 < live.size());
            QPointer<QWidget> paneGuard(pane);
            auto* action = menu.exec(title->mapToGlobal(QPoint(0, title->height())));
            if (!self) return;
            if (action == width) {
                bool ok = false;
                int value = QInputDialog::getInt(this, trb("Column width…"), trb("Width (pixels)"), paneGuard ? paneGuard->width() : 290, 220, 1200, 10, &ok);
                if (ok && paneGuard) { paneGuard->setFixedWidth(value); saveWidth(value); }
                return;
            }
            QJsonObject command{{"entity", column["id"]}};
            if (action == rename) {
                bool ok = false; auto name = QInputDialog::getText(this, trb("Rename column"), trb("Column name"), QLineEdit::Normal,
                    column["name"].toString(), &ok); if (!ok || name.trimmed().isEmpty()) return;
                command["action"] = "column_rename"; command["text"] = name.trimmed();
            } else if (action == left || action == right) {
                command["action"] = "column_move";
                command["before"] = action == left ? live[i - 1].toObject()["id"] : i + 2 < live.size() ? live[i + 2].toObject()["id"] : QJsonValue("");
            } else if (action == remove) command["action"] = "column_delete"; else return;
            send(command);
        });
        auto* list = new CardList(pane); list->group = m_group; list->column = column["id"].toString();
        list->avatar = [this](const QString& id) {
            if (m_members) for (int row = 0; row < m_members->rowCount(); row++) {
                const auto index = m_members->index(row, 0);
                if (index.data(MemberListModel::Ed25519Role).toString() == id) return qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            }
            return QIcon();
        };
        list->writable = m_board["can_write"].toBool();
        list->setDragEnabled(list->writable);
        list->activate = [this, list](QListWidgetItem* item, QPoint point) {
            const auto id = item->data(Qt::UserRole).toString();
            if (list->selectedAtPress() && id == m_selectedCard) beginInline(list, item, point);
            else selectCard(id);
        };
        list->create = [this, list] { selectCard({}); beginInline(list, nullptr, QPoint(16, 13)); };
        list->pointerDown = [this](bool down) {
            m_pointerDown = down;
            if (!down && !m_dragging) {
                if (m_renderPending) QTimer::singleShot(0, this, [this] { render(); });
                else continueEditing();
            }
        };
        list->setAcceptDrops(m_board["can_write"].toBool());
        list->submit = [this](QJsonObject command) { send(command); };
        list->dragging = [this, list](bool active) {
            m_dragging = active;
            auto* item = list->currentItem();
            QString id = active && item ? item->data(Qt::UserRole).toString() : QString();
            int height = active && item ? list->visualItemRect(item).height() : 0;
            for (auto* widget : m_scroll->findChildren<QListWidget*>())
                if (auto* cards = dynamic_cast<CardList*>(widget)) cards->previewDrag(id, height);
            if (!active && m_renderPending) { m_renderPending = false; QTimer::singleShot(0, this, [this] { render(); }); }
        };
        list->hover = [this](CardList* target) {
            for (auto* widget : m_scroll->findChildren<QListWidget*>())
                if (auto* cards = dynamic_cast<CardList*>(widget); cards && cards != target) cards->leavePreview();
        };
        list->edge = [this](QPoint global) {
            int x = m_scroll->viewport()->mapFromGlobal(global).x(); auto* bar = m_scroll->horizontalScrollBar();
            if (x < 35) bar->setValue(bar->value() - 15); if (x > m_scroll->viewport()->width() - 35) bar->setValue(bar->value() + 15);
        };
        for (const auto& value : m_board["cards"].toArray()) {
            auto card = value.toObject(); if (card["deleted"].toBool() || card["column"] != column["id"]) continue;
            auto* item = new QListWidgetItem((card["conflict"].toBool() ? trb("Text conflict") + "\n" : QString()) + card["text"].toString(), list);
            int height = kanbanTextHeight(item->text(), list->font(), pane->width() - 52) + 26 + kanbanFooterHeight(list->font());
            item->setSizeHint(QSize(pane->width() - 20, qMax(56, height)));
            item->setData(Qt::UserRole, card["id"].toString()); item->setData(Qt::UserRole + 1, card);
            DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "card footer group=%s card=%s creator=%s assignees=%d created=%lld due=%s remaining=%s",
                qPrintable(m_group), qPrintable(card["id"].toString()), qPrintable(card["creator"].toString()),
                int(card["assignees"].toArray().size()), card["created_at"].toVariant().toLongLong(), qPrintable(card["due"].toString()),
                qPrintable(kanbanDueText(card["due"].toString().toLongLong(), QDateTime::currentMSecsSinceEpoch())));
            if (card["id"].toString() == m_selectedCard) list->setCurrentItem(item);
        }
        connect(list, &QListWidget::itemSelectionChanged, this, [this, list] {
            auto selected = list->selectedItems();
            if (selected.isEmpty()) return;
            if (selected.front()->data(Qt::UserRole).toString() == "local-draft") return;
            selectCard(selected.front()->data(Qt::UserRole).toString());
            for (auto* other : m_scroll->findChildren<QListWidget*>()) if (other != list) other->clearSelection();
        });
        connect(list, &QWidget::customContextMenuRequested, this, [this, list](QPoint point) {
            auto* item = list->itemAt(point); if (!item) return; auto card = item->data(Qt::UserRole + 1).toJsonObject();
            QPointer<KanbanView> self(this); QMenu menu; auto* change = menu.addAction(trb("Edit card")); auto* history = menu.addAction(trb("Card history"));
            auto* remove = menu.addAction(trb("Delete card"));
            change->setEnabled(m_board["can_write"].toBool());
            remove->setEnabled(m_board["can_write"].toBool() && (m_board["can_moderate"].toBool() || card["author"] == m_board["self"]));
            auto* action = menu.exec(list->viewport()->mapToGlobal(point));
            if (!self) return;
            if (action == change) edit(card);
            else if (action == history) send({{"action", "history"}, {"entity", card["id"]}});
            else if (action == remove) send({{"action", "card_delete"}, {"entity", card["id"]}});
        });
        layout->addWidget(list, 1); columns->addWidget(pane);
        if (i + 1 < live.size()) {
            auto* grip = new KanbanColumnGrip(pane, content); grip->resized = saveWidth;
            grip->interacting = [this](bool active) {
                m_dragging = active;
                if (!active && m_renderPending) { m_renderPending = false; QTimer::singleShot(0, this, [this] { render(); }); }
            };
            columns->addWidget(grip);
        }
    }
    columns->addStretch(); m_scroll->setWidget(content); content->layout()->activate();
    m_scroll->horizontalScrollBar()->setValue(scroll);
    for (auto* widget : content->findChildren<QListWidget*>()) if (auto* list = dynamic_cast<CardList*>(widget); list && columnScroll.contains(list->column)) {
        list->doItemsLayout(); auto* bar = list->verticalScrollBar(); const auto position = columnScroll.value(list->column);
        bar->setValue(position.second ? bar->maximum() : position.first);
        DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "column scroll restored ch=%s column=%s value=%d maximum=%d bottom=%d",
            qPrintable(m_group), qPrintable(list->column), bar->value(), bar->maximum(), position.second);
    }
    continueEditing();
}
void KanbanView::edit(const QJsonObject& card) {
    selectCard(card["id"].toString());
    m_nextColumn = card["column"].toString(); m_nextCard = card["id"].toString(); m_nextPoint = QPoint(16, 13);
    if (m_inline) finishInline();
    else continueEditing();
}
void KanbanView::beginInline(CardList* list, QListWidgetItem* item, QPoint point) {
    if (!list || !list->writable) return;
    if (m_inline && list == m_editList && item == m_editItem) { m_inline->focusAt(point); return; }
    if (m_inline || m_editRequest || m_pointerDown) {
        m_nextColumn = list->column; m_nextCard = item ? item->data(Qt::UserRole).toString() : QString(); m_nextPoint = point;
        if (m_inline) finishInline(); return;
    }
    m_editGroup = m_group; m_draft = {}; m_draft.column = list->column; m_draft.creating = !item;
    if (item) m_draft.card = item->data(Qt::UserRole + 1).toJsonObject();
    else {
        QJsonArray colors = m_board["colors"].toArray(); m_draft.card["color"] = colors.isEmpty() ? QJsonValue() : colors[0];
        item = new QListWidgetItem(list); item->setData(Qt::UserRole, "local-draft");
    }
    m_editItem = item; m_editList = list; list->inlineItem = item; list->setCurrentItem(item);
    m_inline = new KanbanInlineEditor(m_draft.card, m_board["colors"].toArray(), list->viewport());
    m_inline->heightChanged = [this, list, item](int height) {
        if (m_editList != list || m_editItem != item) return;
        item->setSizeHint(QSize(qMax(1, list->viewport()->width() - list->spacing() * 2), height));
    };
    m_inline->finished = [this] { finishInline(); };
    m_inline->cancelled = [this] {
        if (m_editRequest) return;
        m_drafts.remove(m_editGroup); closeInline(); m_editGroup.clear(); render(); continueEditing();
    };
    item->setSizeHint(QSize(qMax(1, list->viewport()->width() - list->spacing() * 2), 72));
    list->setItemWidget(item, m_inline); list->scrollToItem(item, QAbstractItemView::EnsureVisible);
    QPointer<KanbanInlineEditor> editor = m_inline;
    QTimer::singleShot(0, this, [editor, point] { if (editor) editor->focusAt(point); });
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "inline card editor opened ch=%s column=%s create=%d", qPrintable(m_group), qPrintable(list->column), m_draft.creating);
}
void KanbanView::finishInline() {
    if (!m_inline || m_editRequest) return;
    m_draft.text = m_inline->text(); m_draft.color = m_inline->color();
    if ((m_draft.creating && m_draft.text.trimmed().isEmpty()) ||
        (!m_draft.creating && m_draft.text == m_draft.card["text"].toString() && m_draft.color == m_draft.card["color"].toString())) {
        m_drafts.remove(m_editGroup); closeInline(); m_editGroup.clear(); render(); continueEditing(); return;
    }
    if (m_draft.text.trimmed().isEmpty()) {
        m_inline->showError(trb("Card text cannot be empty."));
        DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "inline card save refused: empty text ch=%s", qPrintable(m_editGroup)); return;
    }
    QJsonObject command{{"action", m_draft.creating ? "card_create" : "card_edit"}, {"text", m_draft.text}};
    if (m_draft.creating) { command["column"] = m_draft.column; command["color"] = m_draft.color; }
    else {
        command["entity"] = m_draft.card["id"]; command["parents"] = m_draft.card["heads"];
        if (m_draft.color != m_draft.card["color"].toString()) command["color"] = m_draft.color;
    }
    m_drafts[m_editGroup] = m_draft; m_editRequest = send(command, m_editGroup);
    if (m_editRequest) m_inline->setBusy(true);
    else m_inline->showError(trb("Connection is not ready."));
}
void KanbanView::closeInline() {
    if (m_inline) {
        m_inline->finished = {}; m_inline->cancelled = {}; m_inline->heightChanged = {}; m_inline->hide();
        if (m_editList && m_editItem) {
            m_editList->inlineItem = nullptr; m_editList->removeItemWidget(m_editItem);
            if (m_draft.creating) delete m_editList->takeItem(m_editList->row(m_editItem));
        }
        m_inline->deleteLater(); m_inline.clear();
    }
    m_editList.clear(); m_editItem = nullptr;
}
void KanbanView::continueEditing() {
    if (m_destroying || m_inline || m_editRequest || m_dragging || m_pointerDown || !m_board["can_write"].toBool()) return;
    bool restore = m_drafts.contains(m_group);
    Draft draft = restore ? m_drafts.value(m_group) : Draft{};
    QString column = restore ? draft.column : m_nextColumn, id = restore ? draft.card["id"].toString() : m_nextCard;
    if (column.isEmpty()) return;
    QPoint point = restore ? QPoint(16, 13) : m_nextPoint;
    if (!restore) { m_nextColumn.clear(); m_nextCard.clear(); }
    for (auto* widget : m_scroll->findChildren<QListWidget*>()) {
        auto* list = dynamic_cast<CardList*>(widget); if (!list || list->column != column) continue;
        QListWidgetItem* item = nullptr;
        for (int i = 0; i < list->count(); i++) if (list->item(i)->data(Qt::UserRole).toString() == id) { item = list->item(i); break; }
        if (!id.isEmpty() && !item && !restore) return;
        if (restore && !draft.creating) {
            if (!item) item = new QListWidgetItem(list);
            item->setData(Qt::UserRole, id); item->setData(Qt::UserRole + 1, draft.card);
        }
        beginInline(list, item, point);
        if (restore && m_inline) { m_draft = draft; m_inline->restore(draft.text, draft.color); }
        return;
    }
}

void KanbanView::setMembers(QAbstractItemModel* members) {
    m_members = members;
    auto refresh = [this] {
        refreshDetails();
        for (auto* list : m_scroll->findChildren<QListWidget*>()) list->viewport()->update();
    };
    if (members) {
        connect(members, &QAbstractItemModel::modelReset, this, refresh);
        connect(members, &QAbstractItemModel::dataChanged, this, refresh);
    }
}

int KanbanView::assignmentState(const QString& ed25519) const {
    if (ed25519.size() != 64 || m_selectedCard.isEmpty() || !m_board["can_write"].toBool()) return -1;
    const bool own = m_detailCard["author"] == m_board["self"] ||
        (!m_detailCard["author_active"].toBool() && m_board["can_manage"].toBool());
    if (!own && ed25519 != m_board["self"].toString()) return -1;
    return m_detailCard["assignees"].toArray().contains(ed25519) ? 1 : 0;
}
void KanbanView::toggleAssignee(const QString& ed25519) {
    const int state = assignmentState(ed25519);
    if (state < 0) { DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "member assignment unavailable ed=%s", qPrintable(ed25519)); return; }
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "member assignment group=%s card=%s ed=%s assigned=%d", qPrintable(m_group),
        qPrintable(m_selectedCard), qPrintable(ed25519), state);
    send({{"action", state ? "unassign" : "assign"}, {"entity", m_selectedCard}, {"target", ed25519},
        {"grant", m_detailCard["grant"]}});
}

void KanbanView::selectCard(const QString& id, bool load) {
    ++m_targetRevision; m_targetCard.clear(); m_targetMessage.clear(); m_historyAround.clear(); m_jumpMessage.clear();
    m_messages->setHistoricalWindow(false);
    if (id == m_selectedCard) return;
    if (m_emojiPanel) m_emojiPanel->hide();
    if (m_composer) m_composer->setRecordingContext(m_db ? m_db->dbPath() + "/media" : QString(),
        id.isEmpty() ? QString() : m_group, 0, id);
    m_selectedCard = id; m_detailKey.clear(); m_chatRows.clear(); m_messageModel->clear(); m_chatRequest = 0;
    m_messageBefore.clear(); m_chatRefreshUntil.clear(); m_chatReloadPending = false;
    if (id.isEmpty()) for (auto* list : m_scroll->findChildren<QListWidget*>()) list->clearSelection();
    refreshDetails(); emit cardSelected(!m_selectedCard.isEmpty()); emit assignmentsChanged();
    if (load && !m_selectedCard.isEmpty()) loadMessages();
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "desktop card selected group=%s card=%s", qPrintable(m_group), qPrintable(m_selectedCard));
}

void KanbanView::refreshDetails() {
    QJsonObject card;
    for (const auto& value : m_board["cards"].toArray()) {
        auto candidate = value.toObject();
        if (candidate["id"].toString() == m_selectedCard && !candidate["deleted"].toBool()) { card = candidate; break; }
    }
    if (card.isEmpty()) {
        if (!m_selectedCard.isEmpty()) { m_selectedCard.clear(); m_detailKey.clear(); emit cardSelected(false); emit assignmentsChanged(); }
        return;
    }
    m_fillingDetails = true; m_detailCard = card; emit assignmentsChanged();
    const auto key = m_group + "/" + m_selectedCard; bool changed = key != m_detailKey; m_detailKey = key;
    const auto authorChoice = changed ? card["author"].toString() : m_cardAuthor->currentData().toString();
    auto name = [this](const QString& id) {
        if (m_members) for (int i = 0; i < m_members->rowCount(); i++) if (m_members->index(i, 0).data(MemberListModel::Ed25519Role).toString() == id)
            return m_members->index(i, 0).data(Qt::DisplayRole).toString();
        return id;
    };
    m_created->setText(trb("Created") + ": " + QLocale().toString(
        QDateTime::fromMSecsSinceEpoch(card["created_at"].toVariant().toLongLong() / 1000), QLocale::ShortFormat));
    m_cardAuthor->clear();
    if (m_members) for (int i = 0; i < m_members->rowCount(); i++) {
        if (m_members->index(i, 0).data(Qt::UserRole + 3).toInt() & CHAT_MEMBER_FLAG_DELETED) continue;
        const auto id = m_members->index(i, 0).data(MemberListModel::Ed25519Role).toString();
        m_cardAuthor->addItem(name(id), id);
    }
    int author = m_cardAuthor->findData(card["author"].toString());
    if (author < 0) { m_cardAuthor->addItem(name(card["author"].toString()), card["author"].toString()); author = m_cardAuthor->count() - 1; }
    m_cardAuthor->setCurrentIndex(author);
    if (!changed && m_cardAuthor->findData(authorChoice) >= 0) m_cardAuthor->setCurrentIndex(m_cardAuthor->findData(authorChoice));
    QStringList assigned; for (const auto& value : card["assignees"].toArray()) assigned << name(value.toString());
    m_assigned->setText(trb("Assignees") + ": " + assigned.join(", "));
    bool own = card["author"] == m_board["self"] || (!card["author_active"].toBool() && m_board["can_manage"].toBool());
    bool write = m_board["can_write"].toBool();
    for (const auto& button : {"kanbanTransferAuthor", "kanbanSaveDue"})
        m_details->findChild<QPushButton*>(button)->setEnabled(own && write);
    m_cardAuthor->setEnabled(own && write);
    m_hasDue->setEnabled(own && write); m_due->setEnabled(own && write);
    const auto due = m_dueDrafts.value(key, card["due"].toString()); m_hasDue->setChecked(!due.isEmpty());
    if (!due.isEmpty()) m_due->setDateTime(QDateTime::fromMSecsSinceEpoch(due.toLongLong() / 1000));
    m_hasDue->setText(!due.isEmpty() && due.toLongLong() / 1000 < QDateTime::currentMSecsSinceEpoch() ? trb("Due date (overdue)") : trb("Due date"));
    m_composer->setRecordingContext(m_db ? m_db->dbPath() + "/media" : QString(), m_group, 0, m_selectedCard);
    if (changed) m_chatInput->setPlainText(m_chatDrafts.value(key));
    m_composer->setEnabled(write); m_composer->setMediaEnabled(write);
    if (!write && m_emojiPanel) m_emojiPanel->hide();
    m_fillingDetails = false;
}

void KanbanView::loadMessages(const QString& before) {
    if (m_selectedCard.isEmpty()) return;
    if (m_chatRequest) { if (before.isEmpty()) m_chatReloadPending = true; return; }
    if (before.isEmpty() && !m_chatRows.isEmpty()) {
        const auto items = m_chatRows.values();
        const auto oldest = std::min_element(items.cbegin(), items.cend(), [](const QJsonObject& a, const QJsonObject& b) {
            const auto at = a["created_at"].toVariant().toLongLong(), bt = b["created_at"].toVariant().toLongLong();
            return at == bt ? a["id"].toString() < b["id"].toString() : at < bt;
        });
        m_chatRefreshUntil = (*oldest)["id"].toString(); m_chatRows.clear();
    }
    QJsonObject query{{"action", "messages"}, {"entity", m_selectedCard}, {"before", before}};
    if (before.isEmpty() && !m_historyAround.isEmpty()) query["around"] = m_historyAround;
    m_chatRequest = send(query);
    if (m_chatRequest) m_details->findChild<QPushButton*>("kanbanOlderMessages")->setEnabled(false);
}

void KanbanView::renderMessages() {
    const bool bottom = m_messages->verticalScrollBar()->value() == m_messages->verticalScrollBar()->maximum();
    const auto top = m_messages->indexAt(QPoint(1, 1));
    const auto topMessage = top.data(MsgMessageKeyRole).toString();
    const auto topOffset = m_messages->visualRect(top).top();
    auto rows = m_chatRows.values();
    std::sort(rows.begin(), rows.end(), [](const QJsonObject& a, const QJsonObject& b) {
        auto at = a["created_at"].toVariant().toLongLong(), bt = b["created_at"].toVariant().toLongLong();
        return at == bt ? a["id"].toString() < b["id"].toString() : at < bt;
    });
    m_messageModel->clear();
    for (const auto& row : rows) {
        auto* item = new QStandardItem;
        const auto author = row["author"].toString(); QString name = author;
        if (m_members) for (int i = 0; i < m_members->rowCount(); i++) {
            const auto index = m_members->index(i, 0);
            if (index.data(MemberListModel::Ed25519Role).toString() == author) {
                name = index.data(Qt::DisplayRole).toString(); break;
            }
        }
        item->setEditable(false); item->setData(row, Qt::UserRole);
        item->setData(name, MsgAuthorRole);
        item->setData(QByteArray::fromHex(author.toLatin1()), MsgAuthorAddressRole);
        item->setData(row["id"].toString(), MsgMessageKeyRole);
        item->setData(row["created_at"].toVariant().toLongLong(), MsgRawTimestampRole);
        item->setData(author == m_board["self"].toString(), MsgIsOutgoingRole);
        item->setData(row["deleted"].toBool() ? trb("Message deleted") : row["text"].toString(), MsgTextRole);
        item->setData(QDateTime::fromMSecsSinceEpoch(row["created_at"].toVariant().toLongLong() / 1000).toString("dd.MM HH:mm"), MsgTimeRole);
        item->setData(row["updated_at"] != row["created_at"] && !row["deleted"].toBool(), MsgEditedRole);
        QJsonObject attributes;
        if (!row["file_path"].toString().isEmpty()) attributes = {{"st", "fl"}, {"fp", QFileInfo(row["file_path"].toString()).fileName()}};
        setGroupAttachmentRoles(item, row["d"].toString().toUtf8(), row["ct"].toString(),
            m_db ? m_db->dbPath() + "/media" : QString(), m_group, QJsonDocument(attributes).toJson(QJsonDocument::Compact),
            m_db, QByteArray::fromHex(row["id"].toString().toLatin1()));
        // media_files.location хранится относительно каталога профиля.
        const QString location = row["file_path"].toString();
        const QString path = location.isEmpty() ? QString() : QDir(m_db ? m_db->dbPath() : QStringLiteral(".")).filePath(location);
        if (!path.isEmpty() && !row["deleted"].toBool()) {
            item->setData(path, MsgVoiceFileRole);
            DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "card media local path group=%s message=%s path=%s", qPrintable(m_group),
                qPrintable(row["id"].toString()), qPrintable(path));
        }
        m_messageModel->appendRow(item);
    }
    if (!m_jumpMessage.isEmpty()) {
        const auto message = m_jumpMessage, card = m_selectedCard, group = m_group;
        const auto revision = m_targetRevision; m_jumpMessage.clear();
        QTimer::singleShot(0, this, [this, message, card, group, revision] {
            if (revision != m_targetRevision || group != m_group || card != m_selectedCard) return;
            for (int row = 0; row < m_messageModel->rowCount(); row++) {
                auto index = m_messageModel->index(row, 0); if (index.data(MsgMessageKeyRole).toString() != message) continue;
                m_messages->scrollTo(index, QAbstractItemView::PositionAtCenter); m_messageModel->setData(index, true, MsgHighlightRole);
                QTimer::singleShot(1500, this, [this, index = QPersistentModelIndex(index)] {
                    if (index.isValid()) m_messageModel->setData(index, false, MsgHighlightRole);
                });
                DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "link message opened group=%s card=%s message=%s", qPrintable(group), qPrintable(card), qPrintable(message));
                return;
            }
        });
    } else if (bottom && m_historyAround.isEmpty()) m_messages->scrollToBottom();
    else if (!m_historyAround.isEmpty() && !topMessage.isEmpty()) {
        for (int row = 0; row < m_messageModel->rowCount(); row++) {
            const auto index = m_messageModel->index(row, 0);
            if (index.data(MsgMessageKeyRole).toString() != topMessage) continue;
            m_messages->scrollTo(index, QAbstractItemView::PositionAtTop);
            auto* bar = m_messages->verticalScrollBar(); bar->setValue(bar->value() - topOffset); break;
        }
    }
}

void KanbanView::submitAttachment(const QString& target, attachment_send_req* request) {
    if (target != m_group || m_selectedCard.isEmpty() || !m_board["can_write"].toBool()) {
        DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "card attachment rejected: changed target/card or read-only");
        m_composer->submissionResult(request->request_id, false); attachment_send_discard(request); return;
    }
    snprintf(request->target, sizeof(request->target), "%s", qPrintable(target));
    snprintf(request->card, sizeof(request->card), "%s", qPrintable(m_selectedCard));
    const quint64 composer = request->request_id;
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "card attachment submitted group=%s card=%s kind=%d request=%llu", qPrintable(target),
        qPrintable(m_selectedCard), request->info.kind, (unsigned long long)composer);
    if (gui_bridge_submit_attachment(request) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "card attachment queue rejected request=%llu", (unsigned long long)composer);
        m_composer->submissionResult(composer, false); attachment_send_discard(request);
    }
}
void KanbanView::onMessageSubmitted(const uint8_t* data, int size) {
    if (!data || size != 9 || data[8] > 2) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "invalid submission result bytes=%d", size); return; }
    m_composer->submissionResult(qFromLittleEndian<quint64>(data), data[8] == 1, data[8] == 2);
}
void KanbanView::onAttachmentPreparation(const uint8_t* data, int size) {
    if (!data || size < 18 || size != 18 + data[0] || data[size - 1] > 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "invalid preparation event bytes=%d", size); return;
    }
    m_composer->updatePreparation(QString::fromUtf8(reinterpret_cast<const char*>(data + 1), data[0]),
        QByteArray(reinterpret_cast<const char*>(data + 1 + data[0]), 16), data[size - 1]);
}

void KanbanView::setVideoRecordingEnabled(bool enabled) { m_composer->setVideoRecordingEnabled(enabled); }
