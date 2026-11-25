#include "messagelist.h"
#include "messagedelegate.h"
#include "chatview.h"
#include "inputbar.h"
#include "message_text.h"
#include "debug_ui.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include "chat/chat_poll.h"
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <QWidgetAction>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QScopeGuard>
#include <QToolButton>
#include <QScrollBar>
#include <QWheelEvent>
#include <cstring>

extern "C" {
#include "../../../lib/mem.h"
}

static QString feedbackText(const char* source) { return QCoreApplication::translate("ChatGui", source); }

/* Колесо над строкой реакций двигает единственную, горизонтальную полосу. */
class ReactionScrollArea final : public QScrollArea {
public:
    explicit ReactionScrollArea(QWidget* parent) : QScrollArea(parent) {}
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Wheel) { wheelEvent(static_cast<QWheelEvent*>(event)); return true; }
        return QScrollArea::eventFilter(watched, event);
    }
    void wheelEvent(QWheelEvent* event) override {
        const QPoint pixels = event->pixelDelta(), angle = event->angleDelta();
        const int delta = !pixels.isNull() ? (pixels.x() ? pixels.x() : pixels.y())
            : (angle.x() ? angle.x() : angle.y()) * horizontalScrollBar()->singleStep() * 3 / 120;
        auto* bar = horizontalScrollBar(); const int before = bar->value();
        bar->setValue(before - delta); event->accept();
        GUI_DEBUG("reaction: wheel position=%d->%d range=%d", before, bar->value(), bar->maximum());
    }
};

QWidget* MessageList::createReactionStrip(QMenu* menu, const QString& channel, const QString& messageId) {
    QSet<int> mine;
    for (const auto& value : m_feedback.value(messageId).value("reactions").toArray())
        if (value.toObject().value("mine").toBool()) mine.insert(value.toObject().value("id").toInt());
    auto* scroll = new ReactionScrollArea(menu); auto* content = new QWidget;
    scroll->setObjectName("reactionStrip"); content->installEventFilter(scroll);
    auto* row = new QHBoxLayout(content); row->setContentsMargins(4, 4, 4, 4); row->setSpacing(2);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame); scroll->setFixedSize(qMin(560, qMax(240, m_view->viewport()->width() - 32)), 52);
    scroll->horizontalScrollBar()->setSingleStep(24);
    scroll->setStyleSheet("QScrollArea#reactionStrip { background: palette(window); border: 1px solid palette(mid); border-radius: 8px; }"
        "QScrollArea#reactionStrip QScrollBar:horizontal { height: 4px; background: transparent; margin: 0 8px; }"
        "QScrollArea#reactionStrip QScrollBar::handle:horizontal { background: palette(mid); border-radius: 2px; min-width: 24px; }"
        "QScrollArea#reactionStrip QScrollBar::handle:horizontal:hover { background: palette(highlight); }"
        "QScrollArea#reactionStrip QScrollBar::add-line:horizontal, QScrollArea#reactionStrip QScrollBar::sub-line:horizontal { width: 0; }"
        "QScrollArea#reactionStrip QScrollBar::add-page:horizontal, QScrollArea#reactionStrip QScrollBar::sub-page:horizontal { background: transparent; }");
    for (unsigned i = 0; i < CHAT_REACTION_COUNT; i++) {
        auto* button = new QToolButton(content); button->installEventFilter(scroll); button->setText(QString::fromUtf8(chat_reaction_emoji(i)));
        button->setCheckable(true); button->setChecked(mine.contains(i)); button->setFixedSize(36, 36);
        QFont font = button->font(); font.setPixelSize(22); button->setFont(font);
        connect(button, &QToolButton::clicked, this, [this, menu, channel, messageId, i]() {
            GUI_DEBUG("reaction: chosen ch=%s message=%s emoji=%u", qPrintable(channel), qPrintable(messageId), i);
            menu->close();
            if (channel != m_currentChannelId || !m_db
                || m_db->getMessageByKey(channel, QByteArray::fromHex(messageId.toLatin1())).messageId.isEmpty()) {
                GUI_WARN("reaction: target unavailable ch=%s message=%s", qPrintable(channel), qPrintable(messageId)); return;
            }
            sendFeedback({{"cmd", "react"}, {"ch", channel}, {"message_id", messageId}, {"emoji", (int)i}});
        });
        row->addWidget(button);
    }
    scroll->setWidget(content); return scroll;
}

void MessageList::replyToMessage(const QString& channel, const QString& messageId) {
    if (channel != m_currentChannelId || !m_db) {
        GUI_WARN("reply: stale channel=%s", qPrintable(channel)); return;
    }
    const auto original = m_db->getMessageByKey(channel, QByteArray::fromHex(messageId.toLatin1()));
    if (original.messageId.isEmpty()) { GUI_WARN("reply: selected original disappeared ch=%s", qPrintable(channel)); return; }
    GUI_DEBUG("message menu: reply ch=%s message=%s", qPrintable(channel), qPrintable(messageId));
    m_inputBar->setReply(original.messageId, authorName(original.authorEd), DbManager::messagePreview(original));
}

void MessageList::showMessageMenu(const QModelIndex& index, const QPoint& globalPos) {
    const QString channel = index.data(MsgChannelIdRole).toString(), id = index.data(MsgMessageKeyRole).toString();
    if (!index.isValid() || channel != m_currentChannelId || isDmChannel(channel) || id.size() != 64) {
        GUI_WARN("message menu: invalid target ch=%s message=%s", qPrintable(channel), qPrintable(id)); return;
    }
    if (m_messageMenu) m_messageMenu->close();
    auto* menu = new QMenu(this); menu->setObjectName("messageClickMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
    m_messageMenu = menu;
    menu->setWindowFlag(Qt::NoDropShadowWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setStyleSheet("QMenu#messageClickMenu { background: transparent; border: none; padding: 0; }");
    auto* panel = new QWidget(menu); auto* blocks = new QVBoxLayout(panel);
    blocks->setContentsMargins(0, 0, 0, 0); blocks->setSpacing(6);
    auto* reactions = createReactionStrip(menu, channel, id); blocks->addWidget(reactions);
    auto* commands = new QMenu(panel); commands->setObjectName("messageCommands"); commands->setWindowFlags(Qt::Widget);
    commands->setStyleSheet("QMenu#messageCommands { background: palette(window); border: 1px solid palette(mid); border-radius: 8px; padding: 4px; }"
        "QMenu#messageCommands::item { padding: 7px 16px; }");
    blocks->addWidget(commands, 0, Qt::AlignLeft);
    auto* content = new QWidgetAction(menu); content->setDefaultWidget(panel); menu->addAction(content);
    connect(commands, &QMenu::triggered, menu, &QMenu::close);
    const QPersistentModelIndex target(index);
    connect(commands->addAction(feedbackText("Copy")), &QAction::triggered, this, [this, target, channel, id] {
        if (!target.isValid() || channel != m_currentChannelId || target.data(MsgMessageKeyRole).toString() != id) {
            GUI_WARN("message menu: copy target unavailable ch=%s message=%s", qPrintable(channel), qPrintable(id)); return;
        }
        const auto formatted = formattedMessageText(target);
        QApplication::clipboard()->setText(formatted ? formatted->text : target.data(MsgTextRole).toString());
        GUI_DEBUG("message menu: copied ch=%s message=%s", qPrintable(channel), qPrintable(id));
    });
    connect(commands->addAction(feedbackText("Reply")), &QAction::triggered, this, [this, channel, id] { replyToMessage(channel, id); });
    commands->setFixedWidth(qMin(reactions->width() - 24, qMax(180, commands->sizeHint().width())));
    commands->setFixedHeight(commands->sizeHint().height());
    GUI_DEBUG("message menu: opened ch=%s message=%s reactions_width=%d commands_width=%d",
        qPrintable(channel), qPrintable(id), reactions->width(), commands->width());
    menu->popup(globalPos);
}

void MessageList::requestReaction(const QModelIndex& index, int emoji) {
    const QString channel = index.data(MsgChannelIdRole).toString(), messageId = index.data(MsgMessageKeyRole).toString();
    if (!index.isValid() || channel.isEmpty() || isDmChannel(channel) || messageId.size() != 64
        || emoji < 0 || emoji >= CHAT_REACTION_COUNT) {
        GUI_WARN("reaction: invalid target ch=%s key_bytes=%d emoji=%d", qPrintable(channel), messageId.size(), emoji); return;
    }
    GUI_DEBUG("reaction: toggled ch=%s message=%s emoji=%d", qPrintable(channel), qPrintable(messageId), emoji);
    sendFeedback({{"cmd", "react"}, {"ch", channel}, {"message_id", messageId}, {"emoji", emoji}});
}

/* Итог для баббла и открытого диалога; строковые поля показываем как обычный текст. */
static QString pollSummary(const QJsonObject& poll) {
    QString text = poll.value("question").toString();
    for (const auto& value : poll.value("options").toArray()) {
        const auto option = value.toObject();
        text += QStringLiteral("\n%1 — %2 (%3%)").arg(option.value("text").toString())
            .arg(option.value("count").toInt()).arg(option.value("percent").toDouble(), 0, 'f', 1);
    }
    text += QStringLiteral("\n%1: %2").arg(feedbackText("Participants")).arg(poll.value("voters").toInt());
    return text;
}

/* Команды владеют только C-данными; QObject остаются в GUI-потоке. */
bool MessageList::sendFeedback(QJsonObject command, bool userAction) {
    if (!command.contains("ch")) command["ch"] = m_currentChannelId;
    if (command.value("ch").toString().isEmpty() || (userAction && command.value("ch").toString() != m_currentChannelId)) {
        GUI_WARN("feedback: stale/empty channel rejected"); return false;
    }
    QByteArray json = QJsonDocument(command).toJson(QJsonDocument::Compact);
    auto* req = static_cast<chat_feedback_request*>(u_malloc(sizeof(chat_feedback_request) + json.size() + 1));
    if (!req) { GUI_ERROR("feedback: command allocation failed bytes=%d", json.size()); return false; }
    req->request_id = userAction ? ++m_feedbackSequence : 0;
    memcpy(req + 1, json.constData(), json.size()); reinterpret_cast<char*>(req + 1)[json.size()] = 0;
    if (gui_bridge_post_bound_fn(chat_feedback_command_trampoline, req, &req->inst) < 0) { u_free(req); return false; }
    return true;
}

void MessageList::setupFeedback(QStandardItem* item, const MessageRow& message, const QString& channel) {
    item->setData(QString::fromLatin1(message.messageId.toHex()), MsgMessageKeyRole);
    item->setData(message.timestamp, MsgRawTimestampRole); item->setData(channel, MsgChannelIdRole);
    item->setData(QVariant::fromValue(message.version), MsgVersionRole);
    item->setData(message.editedAt > 0, MsgEditedRole);
    QString time = QDateTime::fromMSecsSinceEpoch(message.timestamp).toString("HH:mm");
    if (message.editedAt > 0) {
        const auto edited = QDateTime::fromMSecsSinceEpoch(message.editedAt);
        const QString format = edited.date() == QDate::currentDate() ? "HH:mm" : "dd.MM.yyyy HH:mm";
        time += " · " + QCoreApplication::translate("ChatGui", "edited %1").arg(edited.toString(format));
    }
    item->setData(time, MsgTimeRole);
    if (message.contentType == "poll") {
        struct chat_poll poll;
        QByteArray wire = QByteArray::fromBase64(message.data.mid(4), QByteArray::Base64UrlEncoding);
        if (chat_poll_decode(reinterpret_cast<const uint8_t*>(wire.constData()), wire.size(), &poll) == 0)
            item->setData(QString::fromUtf8(poll.question), MsgTextRole);
        else item->setData(feedbackText("Invalid poll"), MsgTextRole);
    }
    if (message.messageId.size() == 32) sendFeedback({{"cmd", "feedback"}, {"ch", channel},
        {"message_id", QString::fromLatin1(message.messageId.toHex())}}, false);
}

void MessageList::onFeedbackChanged(const uint8_t* data, int len) {
    if (!data || len < 34 || len != data[0] + 34) { GUI_ERROR("feedback: invalid change event bytes=%d", len); return; }
    QString channel = QString::fromUtf8(reinterpret_cast<const char*>(data + 1), data[0]);
    if (channel != m_currentChannelId) return;
    QString id = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(data + 1 + data[0]), 32).toHex());
    sendFeedback({{"cmd", "feedback"}, {"message_id", id}}, false);
}

void MessageList::onFeedbackResult(const uint8_t* data, int len) {
    if (!data || len < 9 || data[8] > 1) { GUI_ERROR("feedback: invalid result event bytes=%d", len); return; }
    quint64 request; memcpy(&request, data, 8);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(data + 9), len - 9), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) { GUI_ERROR("feedback: invalid result JSON"); return; }
    QJsonObject state = document.object();
    if (!data[8]) {
        GUI_ERROR("feedback: command failed request=%llu reason=%s", (unsigned long long)request, qPrintable(state.value("error").toString()));
        if (request && gui_bridge_is_uasync_ready()) {
            m_pendingPoll.clear(); QByteArray reason = state.value("error").toString().toUtf8();
            QMessageBox::warning(this, feedbackText("Polls and reactions"), feedbackText(reason.constData()));
        }
        return;
    }
    if (state.value("ch").toString() != m_currentChannelId) return;
    if (state.value("snapshot_pending").toBool()) {
        const QString id = state.value("message_id").toString();
        GUI_WARN("feedback: command saved, requesting snapshot ch=%s message=%s", qPrintable(m_currentChannelId), qPrintable(id));
        sendFeedback({{"cmd", "feedback"}, {"message_id", id}}, false); return;
    }
    applyFeedback(state);
}

void MessageList::applyFeedback(const QJsonObject& state) {
    QString id = state.value("message_id").toString(); m_feedback[id] = state;
    QVariantList reactions;
    for (const auto& value : state.value("reactions").toArray()) reactions.append(value.toObject().toVariantMap());
    auto poll = state.value("poll").toObject();
    for (int row = 0; row < m_model->rowCount(); row++) {
        auto* item = m_model->item(row);
        if (item->data(MsgMessageKeyRole).toString() != id) continue;
        item->setData(reactions, MsgReactionsRole);
        if (!poll.isEmpty()) item->setData(pollSummary(poll), MsgTextRole);
    }
    if (m_pollDialog && m_pollDialogMessage == id) {
        auto* results = m_pollDialog->findChild<QPlainTextEdit*>("pollResults");
        if (results) {
            QString text = pollSummary(poll);
            const auto options = poll.value("options").toArray();
            for (const auto& value : poll.value("answers").toArray()) {
                auto row = value.toObject(), answer = row.value("answer").toObject(); QStringList parts;
                for (const auto& choice : answer.value("choices").toArray())
                    if (choice.toInt() >= 0 && choice.toInt() < options.size()) parts << options[choice.toInt()].toObject().value("text").toString();
                if (!answer.value("custom").toString().isEmpty()) parts << answer.value("custom").toString();
                const QByteArray author = QByteArray::fromHex(row.value("author_id").toString().toLatin1());
                if (author.size() != 32) { GUI_WARN("poll feedback: invalid author key"); continue; }
                const QString name = m_db ? m_db->getDisplayName(author, m_currentChannelId) : QString::fromLatin1(author.toHex());
                text += QStringLiteral("\n%1: %2").arg(name, parts.join(", "));
            }
            results->setPlainText(text);
        }
    }
    if (m_pendingPoll == id && !poll.isEmpty()) { m_pendingPoll.clear(); showPoll(id); }
}

void MessageList::createPoll() {
    if (m_currentChannelId.isEmpty() || isDmChannel(m_currentChannelId)) return;
    const QString channel = m_currentChannelId;
    QPointer<QDialog> holder = new QDialog(this); auto release = qScopeGuard([&] { delete holder.data(); });
    QDialog& dialog = *holder;
    dialog.setWindowTitle(feedbackText("Create poll")); auto* layout = new QVBoxLayout(&dialog);
    auto* question = new QPlainTextEdit(&dialog); question->setPlaceholderText(feedbackText("Question")); question->setMaximumHeight(70);
    auto* options = new QPlainTextEdit(&dialog); options->setPlaceholderText(feedbackText("One option per line"));
    auto* mode = new QComboBox(&dialog); mode->addItem(feedbackText("Single choice")); mode->addItem(feedbackText("Multiple choices"));
    auto* custom = new QCheckBox(feedbackText("Allow a custom answer"), &dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(question); layout->addWidget(options); layout->addWidget(mode); layout->addWidget(custom); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this, channel]() {
        const QString title = question->toPlainText().trimmed(); QJsonArray values; QSet<QString> unique;
        int bytes = 6 + title.toUtf8().size(); bool valid = !title.isEmpty() && title.toUtf8().size() <= CHAT_POLL_QUESTION_MAX;
        for (const auto& line : options->toPlainText().split('\n')) {
            QString text = line.trimmed(); if (text.isEmpty()) continue;
            if (unique.contains(text) || text.toUtf8().size() > CHAT_POLL_OPTION_MAX) valid = false;
            unique.insert(text); values.append(text); bytes += 2 + text.toUtf8().size();
        }
        if (!valid || values.size() < 2 || values.size() > CHAT_POLL_OPTIONS_MAX || bytes > CHAT_POLL_WIRE_MAX) {
            QMessageBox::warning(&dialog, feedbackText("Create poll"),
                feedbackText("Enter a question and distinct options within the size limit.")); return;
        }
        if (sendFeedback({{"cmd", "poll_create"}, {"ch", channel}, {"question", title}, {"options", values},
            {"mode", mode->currentIndex() ? "multiple" : "single"}, {"allow_custom_answer", custom->isChecked()}})) dialog.accept();
    });
    dialog.resize(420, 360); dialog.exec();
}

void MessageList::showPoll(const QString& messageId) {
    if (m_pollDialog) {
        GUI_DEBUG("poll: raising open dialog message=%s", qPrintable(m_pollDialogMessage));
        m_pollDialog->raise(); return;
    }
    auto state = m_feedback.value(messageId), poll = state.value("poll").toObject();
    if (poll.isEmpty()) {
        GUI_DEBUG("poll: requesting state ch=%s message=%s", qPrintable(m_currentChannelId), qPrintable(messageId));
        m_pendingPoll = messageId;
        if (!sendFeedback({{"cmd", "feedback"}, {"message_id", messageId}})) m_pendingPoll.clear();
        return;
    }
    const QString channel = m_currentChannelId; QPointer<MessageList> self(this);
    QPointer<QDialog> holder = new QDialog(this); auto release = qScopeGuard([&] { delete holder.data(); });
    QDialog& dialog = *holder; dialog.setWindowTitle(feedbackText("Vote / results"));
    m_pollDialog = &dialog; m_pollDialogMessage = messageId; auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(poll.value("question").toString(), &dialog); title->setTextFormat(Qt::PlainText); title->setWordWrap(true);
    layout->addWidget(title);
    const bool multiple = poll.value("mode").toString() == "multiple";
    auto* group = new QButtonGroup(&dialog); group->setExclusive(!multiple);
    QList<QAbstractButton*> choices; QSet<int> selected;
    for (const auto& value : poll.value("mine").toObject().value("choices").toArray()) selected.insert(value.toInt());
    auto values = poll.value("options").toArray();
    for (int i = 0; i < values.size(); i++) {
        QString text = values[i].toObject().value("text").toString();
        QAbstractButton* choice = multiple ? static_cast<QAbstractButton*>(new QCheckBox(text, &dialog))
            : static_cast<QAbstractButton*>(new QRadioButton(text, &dialog));
        group->addButton(choice, i); choice->setChecked(selected.contains(i)); layout->addWidget(choice); choices.append(choice);
    }
    auto* own = new QPlainTextEdit(&dialog); own->setMaximumHeight(80); own->setPlaceholderText(feedbackText("Your answer"));
    own->setPlainText(poll.value("mine").toObject().value("custom").toString());
    QRadioButton* other = nullptr;
    if (poll.value("allow_custom_answer").toBool()) {
        if (!multiple) {
            other = new QRadioButton(feedbackText("Your answer"), &dialog); group->addButton(other);
            layout->addWidget(other); connect(other, &QRadioButton::toggled, own, &QWidget::setEnabled);
            other->setChecked(!own->toPlainText().isEmpty()); own->setEnabled(other->isChecked());
        }
        layout->addWidget(own);
    } else own->hide();
    auto* results = new QPlainTextEdit(&dialog); results->setObjectName("pollResults"); results->setReadOnly(true);
    layout->addWidget(results);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto* withdraw = buttons->addButton(feedbackText("Withdraw vote"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(withdraw, &QPushButton::clicked, &dialog, [&, this, channel]() {
        if (sendFeedback({{"cmd", "vote"}, {"ch", channel}, {"message_id", messageId}, {"choices", QJsonArray{}}, {"custom", ""}})) dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this, channel]() {
        QJsonArray ids; for (int i = 0; i < choices.size(); i++) if (choices[i]->isChecked()) ids.append(i);
        QString text = poll.value("allow_custom_answer").toBool() && (multiple || (other && other->isChecked()))
            ? own->toPlainText().trimmed() : QString();
        if (text.toUtf8().size() > CHAT_POLL_CUSTOM_MAX || (other && other->isChecked() && text.isEmpty())) {
            QMessageBox::warning(&dialog, feedbackText("Your answer"), feedbackText("Enter a shorter nonempty answer.")); return;
        }
        if (sendFeedback({{"cmd", "vote"}, {"ch", channel}, {"message_id", messageId}, {"choices", ids}, {"custom", text}})) dialog.accept();
    });
    GUI_INFO("poll: opening dialog ch=%s message=%s options=%d", qPrintable(m_currentChannelId), qPrintable(messageId), int(values.size()));
    applyFeedback(state); dialog.resize(450, 560); dialog.exec();
    if (!self) return;
    m_pollDialog = nullptr; m_pollDialogMessage.clear();
    GUI_DEBUG("poll: dialog closed message=%s", qPrintable(messageId));
}
