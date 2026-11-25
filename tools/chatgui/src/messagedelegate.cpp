#include <QCoreApplication>
#include "messagedelegate.h"
#include <QTextBoundaryFinder>
#include "gui_theme.h"
#include "message_links.h"
#include "message_text.h"
#include "chatview.h"
#include "debug_ui.h"
#include "emoji.h"
#include "animtimer.h"
#include "lottieicon.h"
#include "voiceplayback.h"
#include "sound_manager.h"
#include <cmath>
#include <QPainter>
#include <QPainterPath>
#include <QApplication>
#include <QClipboard>
#include <QAbstractItemView>
#include <QFontMetrics>
#include <QTextLayout>
#include <QMouseEvent>
#include <QMap>
#include <QDateTime>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QDebug>
#include <QImageReader>
#include <QPersistentModelIndex>
#include <QPointer>

/* Полный plain-text снимок цитаты: одинаковые переносы при измерении и рисовании. */
static int layoutQuote(QTextLayout& layout, const QFont& font, QString text, int width) {
    text.replace('\n', QChar(0x2028)); layout.setText(text); layout.setFont(font);
    QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); layout.setTextOption(option);
    layout.beginLayout(); qreal height = 0;
    while (true) {
        auto line = layout.createLine(); if (!line.isValid()) break;
        line.setLineWidth(qMax(1, width)); line.setPosition(QPointF(0, height)); height += line.height();
    }
    layout.endLayout(); return qMax(QFontMetrics(font).height(), int(std::ceil(height)));
}

/* Один layout для размеров, отрисовки, выделения и hit-test ссылок. */
static qreal layoutMessageText(QTextLayout& layout, int width, const QString& compactLink = {}) {
    int linkStart = -1;
    if (!compactLink.isEmpty() && layout.text().endsWith(compactLink)) {
        linkStart = layout.text().length() - compactLink.length();
        const QString shortened = QFontMetrics(layout.font()).elidedText(compactLink, Qt::ElideRight, qMax(1, width - 1));
        layout.setText(layout.text().left(linkStart) + shortened);
    }
    QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    QVector<QTextLayout::FormatRange> formats;
    const auto links = linkStart >= 0 ? QVector<MessageLink>{{linkStart, (int)layout.text().length() - linkStart, compactLink}}
                                     : messageLinks(layout.text());
    for (const auto& link : links) {
        QTextLayout::FormatRange range;
        range.start = link.start; range.length = link.length;
        range.format.setForeground(QApplication::palette().link().color()); range.format.setFontUnderline(true);
        formats.append(range);
    }
    layout.setFormats(formats);
    layout.beginLayout();
    qreal y = 0;
    while (true) {
        auto line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(qMax(1, width)); line.setPosition(QPointF(0, y)); y += line.height();
    }
    layout.endLayout();
    return y;
}

QString MessageDelegate::linkAt(const QStyleOptionViewItem& option, const QModelIndex& index, const QPoint& point) const {
    const QString text = index.data(MsgTextRole).toString();
    const auto formatted = formattedMessageText(index);
    if (formatted ? formatted->links.isEmpty() : messageLinks(text).isEmpty()) return {};
    const Layout geometry = calcLayout(option, index);
    if (geometry.isVoice || geometry.isFile || geometry.isVideo || geometry.isImage || geometry.emojiOnly) return {};
    const QPoint local = point - option.rect.topLeft() - geometry.textRect.topLeft();
    if (!QRect(QPoint(0, 0), geometry.textRect.size()).contains(local)) return {};
    QString display = text; display.replace('\n', QChar(0x2028));
    QTextLayout layout(display, option.font);
    const QString compactLink = index.data(MsgInviteLinkRole).toString();
    if (formatted) layoutFormattedMessage(layout, *formatted, geometry.textRect.width(), option.palette.window().color().lightness() < 128);
    else layoutMessageText(layout, geometry.textRect.width(), compactLink);
    for (int i = 0; i < layout.lineCount(); ++i) {
        const auto line = layout.lineAt(i);
        if (local.y() < line.y() || local.y() >= line.y() + line.height()) continue;
        if (local.x() < line.x() || local.x() >= line.x() + line.naturalTextWidth()) return {};
        const int position = line.xToCursor(local.x(), QTextLine::CursorOnCharacter);
        if (formatted) {
            for (const auto& link : formatted->links) {
                if (position >= link.start && position < link.start + link.length) return link.url;
            }
            return {};
        }
        for (const auto& range : layout.formats()) {
            if (position >= range.start && position < range.start + range.length)
                return compactLink.isEmpty() ? text.mid(range.start, range.length) : compactLink;
        }
    }
    return {};
}

int MessageDelegate::codeCopyAt(const QStyleOptionViewItem& option, const QModelIndex& index, const QPoint& point) const {
    if (!index.isValid()) return -1;
    const auto message = formattedMessageText(index);
    if (!message || message->code.isEmpty()) return -1;
    const auto body = calcLayout(option, index);
    QTextLayout layout; layout.setFont(option.font);
    layoutFormattedMessage(layout, *message, body.textRect.width(), false);
    const QPoint local = point - option.rect.topLeft() - body.textRect.topLeft();
    for (int i = 0; i < message->code.size(); ++i)
        if (messageCodeCopyRect(layout, message->code[i], body.textRect.width()).contains(local)) return i;
    return -1;
}

MessageDelegate::MessageDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {
    GUI_DEBUG("MessageDelegate: avatar size=%dpx, light bubble outline=1px palette(mid)", kAvatar);
}

struct VoicePlayState {
    uint64_t playbackId = 0;
    bool loading = false;
    bool playing = false;
    bool paused = false;
    float durationSec = 0.0f;
    float pausedPosSec = 0.0f;
    float displayFrac = 0.0f;
    float displayElapsed = 0.0f;
};
static QMap<QString, VoicePlayState> s_voiceStates;
static QPointer<QTimer> s_voiceTimer;
static QPointer<QAbstractItemView> s_voiceView;

static void ensureVoiceTimer() {
    if (!s_voiceTimer) {
        s_voiceTimer = new QTimer(QCoreApplication::instance());
        s_voiceTimer->setObjectName("messageVoiceTimer");
        s_voiceTimer->setInterval(16);
        QObject::connect(s_voiceTimer, &QTimer::timeout, []() {
            bool anyActive = false;
            SoundManager* sm = SoundManager::instance();
            for (auto it = s_voiceStates.begin(); it != s_voiceStates.end(); ++it) {
                if (it->loading && !VoicePlayer::isPreparing()) it->loading = false;
                if (it->playing && !sm->isPcmPlaying(it->playbackId)) it->playing = false;
                float elapsed = it->paused ? it->pausedPosSec : 0;
                const int rate = sm->pcmSampleRate(it->playbackId);
                if (it->playing && rate > 0) elapsed = float(double(sm->pcmCursor(it->playbackId)) / rate);
                elapsed = qBound(0.0f, elapsed, it->durationSec);
                const float fraction = it->durationSec > 0 ? elapsed / it->durationSec : 0;
                const bool moving = std::fabs(fraction - it->displayFrac) > 0.001f || std::fabs(elapsed - it->displayElapsed) > 0.001f;
                it->displayFrac = moving ? it->displayFrac + (fraction - it->displayFrac) * 0.15f : fraction;
                it->displayElapsed = moving ? it->displayElapsed + (elapsed - it->displayElapsed) * 0.15f : elapsed;
                anyActive |= it->loading || it->playing || moving;
            }
            if (!anyActive) s_voiceTimer->stop();
            if (s_voiceView) s_voiceView->viewport()->update();
        });
    }
}

bool MessageDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                   const QStyleOptionViewItem &option,
                                   const QModelIndex &index) {
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease) return false;
    Layout L = calcLayout(option, index);

    const auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton) return false;
    const QPoint point = mouse->pos() - option.rect.topLeft();
    const int codeBlock = codeCopyAt(option, index, mouse->pos());
    const bool onQuote = L.quoteRect.contains(point);
    int reaction = -2;
    for (const auto& chip : L.reactionChips) if (chip.rect.contains(point)) { reaction = chip.id; break; }
    const bool onReaction = mouse->button() == Qt::LeftButton && reaction != -2;
    const bool onAvatar=mouse->button()==Qt::LeftButton && !L.narrow && L.avatarRect.contains(mouse->pos()-option.rect.topLeft());
    const bool onFooter = L.reactionChips.isEmpty() ? L.statusRect.contains(point)
        : point.y() >= L.reactionChips.first().rect.top();
    const bool onPoll = !onQuote && index.data(MsgContentTypeRole).toString() == "poll"
        && !index.data(MsgChannelIdRole).toString().startsWith("dm:")
        && (L.bubbleRect.contains(point) || L.emojiArea.contains(point))
        && !onFooter;
    const bool onMedia = (L.isVoice || L.isFile || L.isVideo || L.isImage) && L.bubbleRect.contains(point);
    const bool mediaAction = onMedia && !onFooter
        && (!L.isVoice || L.voicePlayBtnRect.contains(point) || L.voiceWaveformRect.contains(point));
    const bool onMenu = (L.bubbleRect.contains(point) || L.emojiArea.contains(point)) && !onQuote && !onAvatar
        && !onReaction && !onPoll && !mediaAction && codeBlock < 0;
    if (event->type()==QEvent::MouseButtonPress) {
        m_messagePress = onMenu ? index : QModelIndex(); m_messagePressPosition = mouse->pos();
        m_codePress = codeBlock >= 0 ? index : QModelIndex(); m_codePressPosition = mouse->pos(); m_codePressBlock = codeBlock;
        m_codePressSource = codeBlock >= 0 ? index.data(MsgTextRole).toString() : QString();
        m_mediaPress = onMedia ? index : QModelIndex(); m_mediaPressPosition = mouse->pos();
        m_quotePress = onQuote ? index : QModelIndex(); m_quotePressPosition = mouse->pos();
        m_reactionPress = onReaction ? index : QModelIndex(); m_pressedReaction = reaction; m_reactionPressPosition = mouse->pos();
        m_avatarPress=onAvatar ? index : QModelIndex(); m_avatarPressPosition=mouse->pos();
        m_pollPress = onPoll ? index : QModelIndex(); m_pollPressPosition = mouse->pos();
        return codeBlock >= 0 || onQuote || onAvatar || onReaction || onPoll;
    }
    const auto* view = qobject_cast<const ChatView*>(option.widget);
    const bool openMenu = onMenu && m_messagePress == index
        && (mouse->pos() - m_messagePressPosition).manhattanLength() < 5 && (!view || !view->textSelection().active);
    m_messagePress = QPersistentModelIndex();
    const bool copyCode = codeBlock >= 0 && m_codePress == index && m_codePressBlock == codeBlock
        && (mouse->pos() - m_codePressPosition).manhattanLength() < 5 && m_codePressSource == index.data(MsgTextRole).toString()
        && (!view || !view->textSelection().active);
    m_codePress = QPersistentModelIndex(); m_codePressBlock = -1; m_codePressSource.clear();
    if (copyCode) {
        const auto& block = L.formattedText->code[codeBlock];
        QApplication::clipboard()->setText(L.formattedText->text.mid(block.start, block.length), QClipboard::Clipboard);
        m_copiedCode = index; m_copiedBlock = codeBlock; m_copiedSource = index.data(MsgTextRole).toString();
        const quint64 feedback = ++m_copyFeedbackSequence;
        const auto* copyView = qobject_cast<const QAbstractItemView*>(option.widget);
        const QPointer<QWidget> repaint = copyView ? copyView->viewport() : const_cast<QWidget*>(option.widget);
        if (repaint) repaint->update();
        QTimer::singleShot(1500, this, [this, feedback, repaint] {
            if (feedback != m_copyFeedbackSequence) return;
            m_copiedCode = QPersistentModelIndex(); m_copiedBlock = -1; m_copiedSource.clear();
            if (repaint) repaint->update();
            GUI_DEBUG("message code: copy feedback cleared");
        });
        GUI_INFO("message code copied: row=%d block=%d chars=%d", index.row(), codeBlock, block.length);
        return true;
    }
    const bool activateMedia = onMedia && m_mediaPress == index && (mouse->pos() - m_mediaPressPosition).manhattanLength() < 5;
    m_mediaPress = QPersistentModelIndex();
    const bool hadQuotePress = m_quotePress.isValid();
    const bool activateQuote = onQuote && m_quotePress == index
        && (mouse->pos() - m_quotePressPosition).manhattanLength() < 5 && (!view || !view->hasSelection());
    m_quotePress = QPersistentModelIndex();
    const bool activatePoll = onPoll && m_pollPress == index && (mouse->pos() - m_pollPressPosition).manhattanLength() < 5
        && (!view || !view->textSelection().active);
    m_pollPress = QPersistentModelIndex();
    const bool hadReactionPress = m_reactionPress.isValid();
    const bool activateAvatar=onAvatar && m_avatarPress==index && (mouse->pos()-m_avatarPressPosition).manhattanLength()<5;
    m_avatarPress=QPersistentModelIndex();
    const bool activateReaction = onReaction && m_reactionPress == index && m_pressedReaction == reaction
        && (mouse->pos() - m_reactionPressPosition).manhattanLength() < 5;
    m_reactionPress = QPersistentModelIndex(); m_pressedReaction = -2;
    if (activateQuote) { emit replyRequested(index.data(MsgReplyToRole).toString()); return true; }
    if (hadQuotePress || onQuote) return true;
    if (activateReaction) { emit reactionRequested(index, reaction); return true; }
    if (hadReactionPress || onReaction) return true;
    if (activateAvatar) {
        emit avatarRequested(index.data(MsgChannelIdRole).toString(), index.data(MsgAuthorAddressRole).toByteArray());
        return true;
    }
    if (onAvatar) return true;
    if (openMenu && !index.data(MsgChannelIdRole).toString().isEmpty()
        && !index.data(MsgChannelIdRole).toString().startsWith("dm:")
        && index.data(MsgMessageKeyRole).toString().size() == 64) {
        emit messageMenuRequested(index, mouse->globalPos()); return true;
    }
    if (onFooter) return true;

    if (onPoll) {
        if (!activatePoll) return true;
        const QString messageId = index.data(MsgMessageKeyRole).toString();
        if (messageId.size() != 64) {
            GUI_WARN("poll: cannot open message with invalid key row=%d key_bytes=%d", index.row(), int(messageId.size()));
            return true;
        }
        GUI_INFO("poll: left click ch=%s message=%s", qPrintable(index.data(MsgChannelIdRole).toString()), qPrintable(messageId));
        emit pollRequested(messageId);
        return true;
    }

    if ((L.isVoice || L.isFile || L.isVideo || L.isImage) && !activateMedia) return false;

    if (L.isVideo) {
        int dlState = index.data(MsgFileDownloadStateRole).toInt();

        if (dlState == 2) {  /* скачано — открыть плеер */
            QString fp = index.data(MsgVoiceFileRole).toString();
            if (!fp.isEmpty() && QFile::exists(fp) && m_videoOpenCb) {
                emit attachmentOpening(index);
                m_videoOpenCb(fp);
                return true;
            }
            /* файл пропал (вычистка кеша/удаление) — перекачиваем ниже */
        }
        if (dlState == 1) return false; /* скачивается */

        return m_downloadCb && m_downloadCb(index);
    }

    if (L.isImage) {
        int dlState = index.data(MsgFileDownloadStateRole).toInt();

        if (dlState == 2) {  /* скачано — открыть просмотрщик */
            QString fp = index.data(MsgVoiceFileRole).toString();
            if (!fp.isEmpty() && QFile::exists(fp) && m_imageOpenCb) {
                emit attachmentOpening(index);
                m_imageOpenCb(fp);
                return true;
            }
            /* файл пропал (вычистка кеша/удаление) — перекачиваем ниже */
        }
        if (dlState == 1) return false; /* скачивается */

        return m_downloadCb && m_downloadCb(index);
    }

    if (L.isFile) {
        int dlState = index.data(MsgFileDownloadStateRole).toInt();
        qint64 msgId = index.data(MsgIdRole).toLongLong();
        QString chId = index.data(MsgChannelIdRole).toString();

        if (dlState == 2) {  /* downloaded: open file */
            QString fp = index.data(MsgVoiceFileRole).toString();
            if (!fp.isEmpty() && QFile::exists(fp)) {
                emit attachmentOpening(index);
                if (QDesktopServices::openUrl(QUrl::fromLocalFile(fp))) emit attachmentOpened(fp);
                else GUI_WARN("Attachment open failed path=%s", qPrintable(fp));
                return true;
            }
            /* файл пропал (вычистка кеша/удаление) — перекачиваем ниже */
        }
        if (dlState == 1) return false; /* downloading — no action */

        return m_downloadCb && m_downloadCb(index);
    }

    if (!L.isVoice) return false;

    QMouseEvent* me = static_cast<QMouseEvent*>(event);
    QPoint localPos = me->pos() - option.rect.topLeft();

    bool onPlayBtn = L.voicePlayBtnRect.contains(localPos);
    bool onWaveform = L.voiceWaveformRect.contains(localPos);
    if (!onPlayBtn && !onWaveform) return false;

    QString filePath = index.data(MsgVoiceFileRole).toString();
    int dlState = index.data(MsgFileDownloadStateRole).toInt();
    qint64 msgId = index.data(MsgIdRole).toLongLong();
    QString chId = index.data(MsgChannelIdRole).toString();

    GUI_DEBUG("editorEvent voice: playBtn=%d wave=%d file='%s' exists=%d dlState=%d msgId=%lld",
              (int)onPlayBtn, (int)onWaveform, qPrintable(filePath), QFile::exists(filePath),
              dlState, (long long)msgId);

    /* not downloaded (or file evicted): start download (play will start on completion) */
    bool fileMissing = filePath.isEmpty() || !QFile::exists(filePath);
    if (fileMissing && dlState != 1) {
        if (!m_downloadCb || !m_downloadCb(index)) return false;
        model->setData(index, true, MsgVoiceDownloadPendingRole);
        return true;
    }

    if (fileMissing) return false;
    GUI_DEBUG("editorEvent voice click: filePath=%s exists=%d wave=%d",
              qPrintable(filePath), QFile::exists(filePath), (int)onWaveform);
    if (filePath.isEmpty()) return false;

    ensureVoiceTimer();
    VoicePlayState& st = s_voiceStates[filePath];
    if (st.loading) return true;

    QString durStr = index.data(MsgVoiceDurationRole).toString();
    durStr.remove('s');
    float dur = durStr.toFloat();
    if (!std::isfinite(dur) || dur <= 0) dur = 1.0f;

    if (onWaveform) {
        float clickFrac = (float)(localPos.x() - L.voiceWaveformRect.x()) / L.voiceWaveformRect.width();
        if (clickFrac < 0) clickFrac = 0; if (clickFrac > 1) clickFrac = 1;
        float targetSec = clickFrac * dur;

        if (!st.playing && !st.paused) {
            /* Start playback then seek */
            st.loading = true;
            QPersistentModelIndex persistent(index);
            emit attachmentOpening(index);
            VoicePlayer::playOpusFile(filePath, this, [this, filePath, clickFrac, persistent](float actualDur, uint64_t playbackId) {
                s_voiceStates[filePath].loading = false;
                if (actualDur <= 0) return;
                MessageDelegate::startAutoPlay(filePath, actualDur, playbackId);
                emit attachmentOpened(filePath);
                float position = clickFrac * actualDur;
                auto& state = s_voiceStates[filePath];
                state.pausedPosSec = position;
                state.displayFrac = clickFrac; state.displayElapsed = position;
                int rate = SoundManager::instance()->pcmSampleRate(playbackId);
                if (rate > 0) SoundManager::instance()->seekPcm(playbackId, (unsigned long long)(position * rate));
                if (persistent.isValid()) const_cast<QAbstractItemModel*>(persistent.model())->setData(persistent, 1, MsgVoicePlayStateRole);
            });
            s_voiceTimer->start();
        } else {
            /* Currently playing or paused — seek */
            if (st.paused)
                st.pausedPosSec = targetSec;
            else if (!st.playing)
                { st.playing = true; s_voiceTimer->start(); }
            st.displayFrac = (targetSec > 0 && dur > 0) ? (targetSec / dur) : 0;
            st.displayElapsed = targetSec;
            int sr = SoundManager::instance()->pcmSampleRate(st.playbackId);
            if (sr > 0)
                SoundManager::instance()->seekPcm(st.playbackId, (unsigned long long)(targetSec * sr));
        }
        return true;
    }

    /* --- Play button click (original logic) --- */
    bool isOtherPlaying = false;
    for (auto it = s_voiceStates.begin(); it != s_voiceStates.end(); ++it) {
        if (it.key() != filePath && (it->playing || it->paused))
            isOtherPlaying = true;
    }

    if (st.playing) {
        /* Playing → Pause */
        const int rate = SoundManager::instance()->pcmSampleRate(st.playbackId);
        st.pausedPosSec = rate > 0 ? float(double(SoundManager::instance()->pcmCursor(st.playbackId)) / rate) : 0;
        st.playing = false;
        st.paused = true;
        SoundManager::instance()->pausePcm(st.playbackId);
        model->setData(index, 2, MsgVoicePlayStateRole);
        return true;
    }

    if (st.paused) {
        /* Paused → Resume */
        st.paused = false;
        st.playing = true;
        st.displayFrac = (dur > 0) ? (st.pausedPosSec / dur) : 0;
        st.displayElapsed = st.pausedPosSec;
        SoundManager::instance()->resumePcm(st.playbackId);
        s_voiceTimer->start();
        model->setData(index, 1, MsgVoicePlayStateRole);
        return true;
    }

    /* Stopped or different file → stop old, play new */
    if (isOtherPlaying) {
        for (auto it = s_voiceStates.begin(); it != s_voiceStates.end(); ++it) {
            it->playing = false; it->paused = false;
        }
        VoicePlayer::stop();
    }

    st.loading = true;
    QPersistentModelIndex persistent(index);
    emit attachmentOpening(index);
    VoicePlayer::playOpusFile(filePath, this, [this, filePath, persistent](float actualDur, uint64_t playbackId) {
        s_voiceStates[filePath].loading = false;
        if (actualDur <= 0) return;
        MessageDelegate::startAutoPlay(filePath, actualDur, playbackId);
        emit attachmentOpened(filePath);
        if (persistent.isValid()) const_cast<QAbstractItemModel*>(persistent.model())->setData(persistent, 1, MsgVoicePlayStateRole);
    });
    s_voiceTimer->start();
    return true;
}

QSize MessageDelegate::sizeHint(const QStyleOptionViewItem &option,
                                const QModelIndex &index) const {
    Layout L = calcLayout(option, index);
    return QSize(L.viewWidth, L.totalHeight);
}

static int measureTextHeight(const QFont &font, const QString &text, int width, const QString& compactLink = {}) {
    if (text.isEmpty() || width <= 0) return 0;
    if (!messageLinks(text).isEmpty()) {
        QString display = text; display.replace('\n', QChar(0x2028));
        QTextLayout layout(display, font);
        return (int)std::ceil(layoutMessageText(layout, width, compactLink));
    }
    QRect r = QFontMetrics(font).boundingRect(QRect(0, 0, width, 100000),
                                               Qt::AlignLeft | Qt::TextWordWrap, text);
    return r.height();
}

static QString formatFileSize(qint64 bytes) {
    if (bytes < 1024) return QCoreApplication::translate("ChatGui", "%1B").arg(bytes);
    double kb = bytes / 1024.0;
    if (kb < 1024.0) return QCoreApplication::translate("ChatGui", "%1KB").arg(kb, 0, 'f', kb < 10.0 ? 1 : 0);
    double mb = kb / 1024.0;
    if (mb < 1024.0) return QCoreApplication::translate("ChatGui", "%1MB").arg(mb, 0, 'f', mb < 10.0 ? 1 : 0);
    double gb = mb / 1024.0;
    return QCoreApplication::translate("ChatGui", "%1GB").arg(gb, 0, 'f', 1);
}

static QString formatBitrate(qint64 bytes, qint64 durMs) {
    if (bytes <= 0 || durMs <= 0) return QString();
    qint64 kbps = bytes * 8 / durMs;
    if (kbps >= 1000) return QCoreApplication::translate("ChatGui", "%1Mbps").arg(kbps / 1000.0, 0, 'f', 1);
    return QCoreApplication::translate("ChatGui", "%1kbps").arg(kbps);
}

static int calcMaxLineWidth(const QFont &font, const QString &text, int availW) {
    int maxW = 0;
    int pos = 0;
    while (pos <= text.length()) {
        int end = text.indexOf('\n', pos);
        if (end < 0) end = text.length();
        QString para = text.mid(pos, end - pos);
        if (!para.isEmpty()) {
            QTextLayout layout(para, font);
            layout.beginLayout();
            while (true) {
                QTextLine line = layout.createLine();
                if (!line.isValid()) break;
                line.setLineWidth(availW);
                line.setPosition(QPointF(0, 0));
                maxW = qMax(maxW, (int)line.naturalTextWidth());
            }
            layout.endLayout();
        }
        pos = end + 1;
    }
    return maxW;
}

MessageDelegate::Layout MessageDelegate::calcLayout(
    const QStyleOptionViewItem& option, const QModelIndex& index) const {
    Layout layout = calcBodyLayout(option, index);
    // Медиа и крупные emoji получают ту же цитату над своим содержимым.
    if (index.data(MsgHasQuoteRole).toBool() && !layout.quoteRect.isValid()) {
        const int line = QFontMetrics(option.font).height();
        const QRect body = layout.emojiOnly ? layout.emojiArea : layout.bubbleRect;
        const int width = qMin(qMax(200, body.width()), qMax(40, layout.viewWidth - 2 * kMarginH));
        QTextLayout quote;
        const int textHeight = index.data(MsgReplyQuoteRole).toString().isEmpty() ? line
            : layoutQuote(quote, option.font, index.data(MsgQuoteTextRole).toString(), width - 2 * kQuotePadH - kQuoteBarW);
        const int height = line + textHeight + 2 * kQuotePadV + 2;
        const int x = layout.isOutgoing ? body.right() - width + 1 : body.left();
        layout.quoteRect = QRect(qMax(kMarginH, x), 0, width, height);
        layout.quoteBarRect = QRect(layout.quoteRect.left(), 0, kQuoteBarW, height);
        layout.quoteAuthorRect = QRect(layout.quoteRect.left() + kQuotePadH + kQuoteBarW, kQuotePadV,
                                      width - 2 * kQuotePadH - kQuoteBarW, line);
        layout.quoteTextRect = layout.quoteAuthorRect.translated(0, line + 2);
        layout.quoteTextRect.setHeight(textHeight);
        for (QRect* rect : {&layout.bubbleRect, &layout.avatarRect, &layout.authorRect, &layout.textRect, &layout.statusRect,
             &layout.deliveryRect, &layout.emojiArea, &layout.voicePlayBtnRect, &layout.voiceWaveformRect,
             &layout.voiceDurationRect, &layout.voiceTimeRect, &layout.mediaBarRect, &layout.progressBarRect,
             &layout.videoThumbRect, &layout.videoPlayBtnRect, &layout.videoDurBadgeRect, &layout.imageThumbRect,
             &layout.imageBadgeRect}) if (rect->isValid()) rect->translate(0, height + kQuoteGap);
        layout.totalHeight += height + kQuoteGap;
    }
    const QString channel = index.data(MsgChannelIdRole).toString();
    if (!m_reactionsEnabled || layout.emojiOnly || channel.isEmpty() || channel.startsWith("dm:")
        || index.data(MsgMessageKeyRole).toString().size() != 64 || layout.bubbleRect.isEmpty()) return layout;
    if (index.data(MsgReactionsRole).toList().isEmpty()) return layout;
    QFont font = option.font; font.setPointSize(qMax(1, font.pointSize() - 1));
    QFont timeFont = option.font; timeFont.setPointSize(qMax(1, timeFont.pointSize() - 2));
    const int height = qMax(24, QFontMetrics(font).height() + 6), left = layout.bubbleRect.left() + kPadH;
    layout.reactionTimeText = index.data(MsgTimeRole).toString();
    const int innerWidth = layout.bubbleRect.width() - 2 * kPadH;
    const int timeWidth = qMin(QFontMetrics(timeFont).horizontalAdvance(layout.reactionTimeText) + 2, qMax(0, innerWidth - height - 6));
    const int reactionsRight = layout.bubbleRect.right() - kPadH - timeWidth - 6;
    int top;
    if (layout.isVoice) top = layout.voiceDurationRect.bottom() + 4;
    else if (layout.isVideo) top = layout.statusRect.bottom() + 4;
    else if (layout.isImage) top = layout.bubbleRect.bottom() + 3;
    else if (layout.isFile) top = qMax(layout.statusRect.bottom() + 4, layout.bubbleRect.bottom() - kPadBot);
    else top = layout.statusRect.top();
    if (layout.deliveryRect.isValid()) top = qMax(top, layout.deliveryRect.bottom() + 4);
    int y = top, x = left;
    for (const auto& value : index.data(MsgReactionsRole).toList()) {
        const auto reaction = value.toMap(); font.setBold(reaction.value("mine").toBool());
        const int width = qMin(reactionsRight - left + 1, 30 + QFontMetrics(font).horizontalAdvance(reaction.value("count").toString()));
        if (x + width - 1 > reactionsRight) { x = left; y += height + 4; }
        layout.reactionChips.append({QRect(x, y, width, height), reaction.value("id").toInt(),
            reaction.value("emoji").toString(), reaction.value("count").toInt(), reaction.value("mine").toBool()});
        x += width + 4;
    }
    layout.reactionTime = QRect(layout.bubbleRect.right() - kPadH - timeWidth + 1, y, timeWidth, height);
    layout.bubbleRect.setBottom(y + height + kPadBot - 1);
    layout.totalHeight = qMax(layout.bubbleRect.bottom() + 1, kAvatar) + kItemGap;
    if (layout.avatarRect.isValid()) layout.avatarRect.moveTop(qMax(0, layout.bubbleRect.bottom() + 1 - kAvatar));
    return layout;
}

MessageDelegate::Layout MessageDelegate::calcBodyLayout(
    const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Layout L{};
    L.viewWidth = 300;
    if (option.widget) {
        auto *v = qobject_cast<const QAbstractItemView *>(option.widget);
        if (v) L.viewWidth = v->viewport()->width();
    }
    if (L.viewWidth <= 0) L.viewWidth = 300;
    L.narrow = L.viewWidth - 2 * kMarginH - kAvatar - kGap < 400;

    bool hasQuote = index.data(MsgHasQuoteRole).toBool();
    QString text  = index.data(MsgTextRole).toString();
    L.isOutgoing = index.data(MsgIsOutgoingRole).toBool();

    /* Voice message detection */
    QString contentType = index.data(MsgContentTypeRole).toString();
    L.isVoice = (contentType == "audio/opus");
    if (L.isVoice) {
        L.voiceDuration = index.data(MsgVoiceDurationRole).toString();
        int pbs = 16;     // play button size
        int wh = 25;      // waveform height
        int th = 14;      // text line height
        L.bubbleWidth = 220;
        int barW = 3, barPad = 4;
        int bubbleH = kPadTop + wh + 4 + th + kPadBot;
        L.totalHeight = qMax(bubbleH, kAvatar) + kItemGap;

        int contentEdge = L.isOutgoing
            ? L.viewWidth - (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap))
            : (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap));
        int bx = L.isOutgoing ? (contentEdge - L.bubbleWidth - barW - barPad) : contentEdge;
        L.bubbleRect = QRect(bx, 0, L.bubbleWidth, bubbleH);
        L.mediaBarRect = QRect(L.isOutgoing ? bx + L.bubbleWidth + 1 : bx - barW - 1, 0, barW, bubbleH);

        int dlState = index.data(MsgFileDownloadStateRole).toInt();
        int bd = index.data(MsgMediaBlocksDoneRole).toInt();
        int nb = index.data(MsgMediaNumBlocksRole).toInt();
        L.mediaBarColor = (dlState == 2) ? 2 : (dlState == 1 || (bd > 0 && nb > 0)) ? 1 : 0;
        if (dlState == 1 && nb > 0) L.progressFrac = (float)bd / nb;
        if (dlState == 1) { L.progressBarRect = QRect(bx, bubbleH - 3, L.bubbleWidth, 3); }

        L.voicePlayBtnRect = QRect(bx + kPadH, kPadTop + 4, pbs, pbs);
        int wx = bx + kPadH + pbs + 4;
        int ww = L.bubbleWidth - 2*kPadH - pbs - 4;
        L.voiceWaveformRect = QRect(wx, kPadTop + 1, ww, wh);
        L.voiceDurationRect = QRect(wx, kPadTop + wh + 4, ww - 40, th);
        L.voiceTimeRect = QRect(wx + ww - 40, kPadTop + wh + 4, 40, th);
        if (!index.data(MsgDeliveryStateRole).toString().isEmpty()) {
            L.deliveryRect = QRect(bx, bubbleH + 2, L.bubbleWidth, th);
            L.totalHeight += th + 2;
        }

        if (!L.narrow) {
            int avatarY = qMax(0, L.bubbleRect.bottom() + 1 - kAvatar);
            if (!L.isOutgoing)
                L.avatarRect = QRect(kMarginH, avatarY, kAvatar, kAvatar);
            else
                L.avatarRect = QRect(L.viewWidth - kMarginH - kAvatar, avatarY, kAvatar, kAvatar);
        }
        return L;
    }

    /* Video message detection */
    L.isVideo = (contentType.startsWith("video/"));
    if (L.isVideo) {
        int barW = 3, barPad = 4;
        int thW = 200, thH = 112;   /* 16:9 превью */
        int infoH = 16;             /* строка под пузырём: res·size·bitrate + время */
        L.bubbleWidth = thW;
        L.totalHeight = qMax(thH, kAvatar) + infoH + kItemGap;

        int contentEdge = L.isOutgoing
            ? L.viewWidth - (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap))
            : (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap));
        int bx = L.isOutgoing ? (contentEdge - thW - barW - barPad) : contentEdge;
        L.bubbleRect = QRect(bx, 0, thW, thH);
        L.videoThumbRect = L.bubbleRect;
        L.mediaBarRect = QRect(L.isOutgoing ? bx + thW + 1 : bx - barW - 1, 0, barW, thH);
        L.statusRect = QRect(bx, thH + 2, thW, infoH - 2);
        if (!index.data(MsgDeliveryStateRole).toString().isEmpty()) {
            L.deliveryRect = QRect(bx, thH + infoH + 2, thW, infoH - 2);
            L.totalHeight += infoH;
        }

        int dlState = index.data(MsgFileDownloadStateRole).toInt();
        int bd = index.data(MsgMediaBlocksDoneRole).toInt();
        int nb = index.data(MsgMediaNumBlocksRole).toInt();
        L.mediaBarColor = (dlState == 2) ? 2 : (dlState == 1 || (bd > 0 && nb > 0)) ? 1 : 0;
        if (dlState == 1 && nb > 0) L.progressFrac = (float)bd / nb;
        if (dlState == 1) { L.progressBarRect = QRect(bx, thH - 3, thW, 3); }

        L.videoPlayBtnRect = QRect(bx + thW / 2 - 24, thH / 2 - 24, 48, 48);
        L.videoDurBadgeRect = QRect(bx + thW - 66, thH - 26, 58, 18);

        if (!L.narrow) {
            int avatarY = qMax(0, L.bubbleRect.bottom() + 1 - kAvatar);
            if (!L.isOutgoing)
                L.avatarRect = QRect(kMarginH, avatarY, kAvatar, kAvatar);
            else
                L.avatarRect = QRect(L.viewWidth - kMarginH - kAvatar, avatarY, kAvatar, kAvatar);
        }
        return L;
    }

    /* Image message detection */
    L.isImage = contentType.startsWith("image/");
    if (L.isImage) {
        int barW = 3, barPad = 4;
        int thW = 220, thH = 160;
        const int iw = index.data(MsgImageWidthRole).toInt(), ih = index.data(MsgImageHeightRole).toInt();
        if (iw > 0 && ih > 0) thH = qBound(80, qRound((double)thW * ih / iw), 300);
        L.bubbleWidth = thW;
        L.totalHeight = qMax(thH, kAvatar) + kItemGap;

        int contentEdge = L.isOutgoing
            ? L.viewWidth - (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap))
            : (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap));
        int bx = L.isOutgoing ? (contentEdge - thW - barW - barPad) : contentEdge;
        L.bubbleRect = QRect(bx, 0, thW, thH);
        L.imageThumbRect = L.bubbleRect;
        L.mediaBarRect = QRect(L.isOutgoing ? bx + thW + 1 : bx - barW - 1, 0, barW, thH);

        int dlState = index.data(MsgFileDownloadStateRole).toInt();
        int bd = index.data(MsgMediaBlocksDoneRole).toInt();
        int nb = index.data(MsgMediaNumBlocksRole).toInt();
        L.mediaBarColor = (dlState == 2) ? 2 : (dlState == 1 || (bd > 0 && nb > 0)) ? 1 : 0;
        if (dlState == 1 && nb > 0) L.progressFrac = (float)bd / nb;
        if (dlState == 1) { L.progressBarRect = QRect(bx, thH - 3, thW, 3); }

        L.imageBadgeRect = QRect(bx + thW - 150, thH - 26, 142, 18);

        if (!L.narrow) {
            int avatarY = qMax(0, L.bubbleRect.bottom() + 1 - kAvatar);
            if (!L.isOutgoing)
                L.avatarRect = QRect(kMarginH, avatarY, kAvatar, kAvatar);
            else
                L.avatarRect = QRect(L.viewWidth - kMarginH - kAvatar, avatarY, kAvatar, kAvatar);
        }
        return L;
    }

    /* File attachment detection */
    L.isFile = (contentType == "application/octet-stream");
    if (L.isFile) {
        QFontMetrics fm(option.font);
        int th = fm.height();
        int ih = 24;      /* icon height */
        L.bubbleWidth = 230;
        int barW = 3, barPad = 4;
        int bubbleH = kPadTop + ih + 2 + th + kPadBot;
        L.totalHeight = qMax(bubbleH, kAvatar) + kItemGap;

        int contentEdge = L.isOutgoing
            ? L.viewWidth - (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap))
            : (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap));
        int bx = L.isOutgoing ? (contentEdge - L.bubbleWidth - barW - barPad) : contentEdge;
        L.bubbleRect = QRect(bx, 0, L.bubbleWidth, bubbleH);
        L.mediaBarRect = QRect(L.isOutgoing ? bx + L.bubbleWidth + 1 : bx - barW - 1, 0, barW, bubbleH);

        int dlState = index.data(MsgFileDownloadStateRole).toInt();
        int bd = index.data(MsgMediaBlocksDoneRole).toInt();
        int nb = index.data(MsgMediaNumBlocksRole).toInt();
        L.mediaBarColor = (dlState == 2) ? 2 : (dlState == 1 || (bd > 0 && nb > 0)) ? 1 : 0;
        if (dlState == 1 && nb > 0) L.progressFrac = (float)bd / nb;
        if (dlState == 1) { L.progressBarRect = QRect(bx, bubbleH - 3, L.bubbleWidth, 3); }

        L.textRect = QRect(bx + kPadH + ih + 8, kPadTop, L.bubbleWidth - 2*kPadH - ih - 8, th);
        L.statusRect = QRect(bx + kPadH + ih + 8 - 48, kPadTop + th + 2, L.bubbleWidth - 2*kPadH - ih - 8 + 48 - 40, th);

        if (!L.narrow) {
            int avatarY = qMax(0, L.bubbleRect.bottom() + 1 - kAvatar);
            if (!L.isOutgoing)
                L.avatarRect = QRect(kMarginH, avatarY, kAvatar, kAvatar);
            else
                L.avatarRect = QRect(L.viewWidth - kMarginH - kAvatar, avatarY, kAvatar, kAvatar);
        }
        return L;
    }

    L.formattedText = formattedMessageText(index);
    L.emojiCount = L.formattedText ? 0 : detectEmojiOnly(text);
    L.emojiOnly = L.emojiCount >= 1 && !index.data(MsgEditedRole).toBool();

    if (L.emojiOnly) {
        QFontMetrics fm(option.font);
        int lineH = fm.lineSpacing();

        int availW = L.viewWidth - kMarginH - kAvatar - kGap - kMarginH;
        bool narrow = (availW < 400);
        int contentEdge = !L.isOutgoing
            ? (narrow ? kMarginH : (kMarginH + kAvatar + kGap))
            : L.viewWidth - (narrow ? kMarginH : (kMarginH + kAvatar + kGap));

        int eSize = (L.emojiCount == 1) ? lineH * 4 : lineH * 2;
        int emojiGap = lineH / 2;
        int totalW = L.emojiCount * eSize + (L.emojiCount - 1) * emojiGap;
        int x = !L.isOutgoing
            ? (contentEdge + kPadH)
            : (contentEdge - kPadH - totalW);

        L.emojiArea = QRect(x, kGap * 2, totalW, eSize);
        L.totalHeight = eSize + kGap * 2 + kItemGap;
        L.bubbleRect = QRect();
        return L;
    }
    QString qText = hasQuote ? index.data(MsgQuoteTextRole).toString() : QString();
    if (L.formattedText) text = L.formattedText->text;

    int availW = L.viewWidth - kMarginH - kAvatar - kGap - kMarginH;
    L.narrow = (availW < 400);
    if (L.narrow) availW = L.viewWidth - 2 * kMarginH;
    int textLimitW = availW - 2 * kPadH;
    int limitW = text.contains(' ') ? qMin(textLimitW, 700)
                                    : qMin(textLimitW, 1200);

    QFontMetrics fm(option.font);
    int contentW = 0;
    if (L.formattedText) {
        QTextLayout layout; layout.setFont(option.font);
        layoutFormattedMessage(layout, *L.formattedText, limitW, false);
        for (int i = 0; i < layout.lineCount(); ++i) contentW = qMax(contentW, int(std::ceil(layout.lineAt(i).naturalTextWidth())));
    } else contentW = calcMaxLineWidth(option.font, text, limitW);
    QFont statusFont = option.font; statusFont.setPointSize(qMax(1, statusFont.pointSize() - 2));
    contentW = qMax(contentW, QFontMetrics(statusFont).horizontalAdvance(index.data(MsgTimeRole).toString()) + 4);

    if (!L.isOutgoing) {
        QString author = index.data(MsgAuthorRole).toString();
        QFont boldFm = option.font;
        boldFm.setBold(true);
        int authorW = QFontMetrics(boldFm).horizontalAdvance(author);
        contentW = qMax(contentW, authorW);
    }

    if (hasQuote) {
        QString qAuthor = index.data(MsgQuoteAuthorRole).toString();
        QFont boldFm = option.font;
        boldFm.setBold(true);
        int qAuthorW = QFontMetrics(boldFm).horizontalAdvance(qAuthor);
        contentW = qMax(contentW, qAuthorW + kQuoteBarW + kQuotePadH + 4);
        int qTextW = index.data(MsgReplyQuoteRole).toString().isEmpty() ? qMin(contentW + 8, limitW)
            : qMax(1, limitW - kQuoteBarW - kQuotePadH - 4);
        int qWidth = calcMaxLineWidth(option.font, qText, qTextW);
        contentW = qMax(contentW, qWidth + kQuoteBarW + kQuotePadH + 4);
    }

    int textW = qMax(200 - 2 * kPadH, contentW);
    textW = qMin(textW, textLimitW);
    int bubbleW = textW + 2 * kPadH;
    L.bubbleWidth = bubbleW;

    int qH = 0;
    if (hasQuote) {
        int qTextH = fm.height();
        if (!index.data(MsgReplyQuoteRole).toString().isEmpty()) {
            QTextLayout quote; qTextH = layoutQuote(quote, option.font, qText, textW - kQuoteBarW - kQuotePadH);
        }
        int qOneLineH = fm.height();
        qH = kQuotePadV + qOneLineH + (qText.isEmpty() ? 0 : 2 + qTextH) + kQuotePadV;
    }

    int textH;
    if (L.formattedText) {
        QTextLayout layout; layout.setFont(option.font);
        textH = std::ceil(layoutFormattedMessage(layout, *L.formattedText, textW, false));
    } else textH = measureTextHeight(option.font, text, textW, index.data(MsgInviteLinkRole).toString());
    int authorH = fm.height();
    int bubbleH = L.isOutgoing
        ? kPadTop + (hasQuote ? qH + kQuoteGap : 0) + textH + kStatusH + kPadBot
        : kPadTop + authorH + 2 + (hasQuote ? qH + kQuoteGap : 0) + textH + kStatusH + kPadBot;
    L.totalHeight = qMax(bubbleH, kAvatar) + kItemGap;

    int contentEdge = L.isOutgoing
        ? L.viewWidth - (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap))
        : (L.narrow ? kMarginH : (kMarginH + kAvatar + kGap));

    if (!L.isOutgoing)
        L.bubbleRect = QRect(contentEdge, 0, bubbleW, bubbleH);
    else
        L.bubbleRect = QRect(contentEdge - bubbleW, 0, bubbleW, bubbleH);

    int contentTop = kPadTop;

    if (!L.isOutgoing) {
        L.authorRect = QRect(contentEdge + kPadH, contentTop, textW, authorH);
        contentTop += authorH + 2;
    }

    if (hasQuote) {
        int qX = !L.isOutgoing
            ? (contentEdge + kPadH)
            : (contentEdge - kPadH - (bubbleW - 2 * kPadH));
        L.quoteRect = QRect(qX, contentTop, bubbleW - 2 * kPadH, qH);
        if (!L.isOutgoing)
            L.quoteBarRect = QRect(L.quoteRect.left(), L.quoteRect.top(), kQuoteBarW, qH);
        else
            L.quoteBarRect = QRect(L.quoteRect.right() - kQuoteBarW, L.quoteRect.top(), kQuoteBarW, qH);
        int tx = !L.isOutgoing
            ? (L.quoteBarRect.right() + kQuotePadH)
            : (L.quoteRect.left() + kQuotePadH);
        L.quoteAuthorRect = QRect(tx, L.quoteRect.top() + kQuotePadV,
                                  L.quoteRect.width() - kQuoteBarW - kQuotePadH,
                                  QFontMetrics(option.font).height());
        L.quoteTextRect = QRect(tx,
                                L.quoteAuthorRect.bottom() + 2,
                                L.quoteAuthorRect.width(),
                                qH - kQuotePadV - QFontMetrics(option.font).height() - 2 - kQuotePadV);
        contentTop += qH + kQuoteGap;
    }

    int txRect = !L.isOutgoing
        ? (contentEdge + kPadH)
        : (contentEdge - kPadH - textW);
    L.textRect   = QRect(txRect, contentTop, textW, textH);
    L.statusRect = QRect(txRect, contentTop + textH + 2, textW, kStatusH);

    if (!L.narrow) {
        int avatarY = qMax(0, L.bubbleRect.bottom() + 1 - kAvatar);
        if (!L.isOutgoing)
            L.avatarRect = QRect(kMarginH, avatarY, kAvatar, kAvatar);
        else
            L.avatarRect = QRect(L.viewWidth - kMarginH - kAvatar, avatarY, kAvatar, kAvatar);
    }

    return L;
}

static int paraLayoutHeight(const QFont &font, const QString &text,
                            int textWidth, int globalChar, qreal globalY,
                            int yTop, int yBot, int xTop, int xBot,
                            bool &foundStart, bool &foundEnd,
                            int &startChar, int &endChar) {
    if (text.isEmpty()) return QFontMetrics(font).lineSpacing();

    QTextLayout layout(text, font);
    layout.beginLayout();
    qreal localY = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(textWidth);
        line.setPosition(QPointF(0, globalY + localY));
        localY += line.height();

        if (!foundStart && globalY + localY > yTop) {
            startChar = globalChar + line.xToCursor(xTop);
            foundStart = true;
        }
        if (!foundEnd && globalY >= yBot) break;
        if (!foundEnd && globalY + localY >= yBot) {
            endChar = globalChar + line.xToCursor(xBot);
            foundEnd = true;
        }
    }
    layout.endLayout();
    return (int)ceil(localY);
}

static void calcTextSelection(const QFont &font, const QString &text,
                               int textWidth, const QPoint &p1, const QPoint &p2,
                               int &startChar, int &endChar) {
    startChar = 0;
    endChar = 0;
    if (text.isEmpty() || textWidth <= 0) return;

    int yTop = qMin(p1.y(), p2.y());
    int yBot = qMax(p1.y(), p2.y());
    int xTop = (p1.y() < p2.y()) ? p1.x() : p2.x();
    int xBot = (p1.y() < p2.y()) ? p2.x() : p1.x();

    int globalChar = 0;
    qreal globalY = 0;
    bool foundStart = false, foundEnd = false;
    int paraStart = 0;

    while (paraStart <= text.length()) {
        int paraEnd = text.indexOf('\n', paraStart);
        if (paraEnd < 0) paraEnd = text.length();
        QString para = text.mid(paraStart, paraEnd - paraStart);

        int paraH = paraLayoutHeight(font, para, textWidth,
                                     globalChar, globalY,
                                     yTop, yBot, xTop, xBot,
                                     foundStart, foundEnd,
                                     startChar, endChar);
        globalChar += para.length();
        globalY += paraH;

        if (paraEnd < text.length())
            globalChar++;

        paraStart = paraEnd + 1;
        if (foundStart && foundEnd) break;
    }

    if (!foundStart) startChar = 0;
    if (!foundEnd) endChar = text.length();
    if (startChar == endChar && endChar < text.length()) endChar = startChar + 1;
    if (startChar > endChar) qSwap(startChar, endChar);
}

QPair<int, int> MessageDelegate::selectionRange(const QStyleOptionViewItem& option, const QModelIndex& index,
                                               const QPoint& first, const QPoint& last) const {
    const QString type = index.data(MsgContentTypeRole).toString();
    if (!type.isEmpty() && type != "text" && type != "text/plain") return {-1, -1};
    const Layout body = calcLayout(option, index);
    if (!body.textRect.isValid()) return {-1, -1};
    const QString source = index.data(MsgTextRole).toString();
    const QString text = body.formattedText ? body.formattedText->text : source;
    const QPoint a = first - body.textRect.topLeft(), b = last - body.textRect.topLeft();
    int start = 0, end = 0;
    if (body.formattedText || !messageLinks(source).isEmpty()) {
        QTextLayout layout; layout.setFont(option.font);
        if (body.formattedText) layoutFormattedMessage(layout, *body.formattedText, body.textRect.width(), false);
        else {
            QString display = text; display.replace('\n', QChar(0x2028)); layout.setText(display);
            layoutMessageText(layout, body.textRect.width(), index.data(MsgInviteLinkRole).toString());
        }
        auto cursor = [&](const QPoint& point) {
            for (int i = 0; i < layout.lineCount(); ++i) {
                const auto line = layout.lineAt(i);
                if (point.y() < line.y() + line.height()) return line.xToCursor(point.x());
            }
            return int(layout.text().size());
        };
        start = cursor(a); end = cursor(b);
        if (start > end) qSwap(start, end);
        const QString link = index.data(MsgInviteLinkRole).toString();
        if (!link.isEmpty() && end > source.size() - link.size()) {
            if (start >= source.size() - link.size()) start = source.size() - link.size();
            end = source.size();
        }
    } else calcTextSelection(option.font, text, body.textRect.width(), a, b, start, end);
    start = qBound(0, start, int(text.size())); end = qBound(start, end, int(text.size()));
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
    boundaries.setPosition(start); if (!boundaries.isAtBoundary()) start = boundaries.toPreviousBoundary();
    boundaries.setPosition(end); if (!boundaries.isAtBoundary()) end = boundaries.toNextBoundary();
    return {start, end};
}

static void drawTextSelectionHighlight(QPainter *p, const QFont &font,
                                        const QString &text, int textWidth,
                                        const QRect &textRect,
                                        const QColor &selColor,
                                        int startChar, int endChar) {
    if (startChar >= endChar) return;

    int globalChar = 0;
    qreal globalY = 0;
    int paraStart = 0;

    while (paraStart <= text.length()) {
        int paraEnd = text.indexOf('\n', paraStart);
        if (paraEnd < 0) paraEnd = text.length();
        QString para = text.mid(paraStart, paraEnd - paraStart);

        if (para.isEmpty()) {
            globalY += QFontMetrics(font).lineSpacing();
        } else {
            QTextLayout layout(para, font);
            layout.beginLayout();
            qreal localY = 0;
            while (true) {
                QTextLine line = layout.createLine();
                if (!line.isValid()) break;
                line.setLineWidth(textWidth);
                line.setPosition(QPointF(0, globalY + localY));

                int ls = globalChar + line.textStart();
                int le = ls + line.textLength();
                localY += line.height();

                if (ls >= endChar || le <= startChar) continue;

                int cs = qMax(startChar, ls);
                int ce = qMin(endChar, le);

                qreal x1 = line.cursorToX(cs - globalChar);
                qreal x2 = line.cursorToX(ce - globalChar);

                QRectF hr(textRect.left() + x1,
                          textRect.top() + line.y(), // line.y() уже включает смещение абзаца globalY.
                          x2 - x1, line.height());
                p->fillRect(hr, selColor);
            }
            layout.endLayout();
            globalY += localY;
        }

        globalChar += para.length();
        if (paraEnd < text.length())
            globalChar++;

        paraStart = paraEnd + 1;
    }
}

static void drawAnimatedEmojiOverlay(QPainter *p, const QFont &font,
                                      const QString &text, int textWidth,
                                      const QRect &textRect, const QTextLayout* prepared = nullptr, const MessageText* formatted = nullptr) {
    if (g_animatedEmojiMap.isEmpty()) return;

    if (prepared && formatted) {
        for (int i = 0; i < prepared->lineCount(); ++i) {
            const auto line = prepared->lineAt(i);
            if (formatted->isCode(line.textStart())) continue;
            for (auto it = g_animatedEmojiMap.cbegin(); it != g_animatedEmojiMap.cend(); ++it) {
                int pos = text.indexOf(it.key(), line.textStart());
                while (pos >= 0 && pos + it.key().size() <= line.textStart() + line.textLength()) {
                    bool inLink = false;
                    for (const auto& link : formatted->links) {
                        if (pos >= link.start && pos < link.start + link.length) { inLink = true; break; }
                    }
                    if (!inLink) {
                        auto* icon = AnimTimer::instance()->icon(it.value());
                        if (icon && icon->isValid()) {
                            const qreal x1 = line.cursorToX(pos), x2 = line.cursorToX(pos + it.key().size());
                            const QSize size(qMax(1, int((x2 - x1) * 1.3)), qMax(1, int(line.height() * 1.3)));
                            p->drawImage(QPointF(textRect.x() + x1 + (x2 - x1 - size.width()) / 2,
                                               textRect.y() + line.y() + (line.height() - size.height()) / 2),
                                         icon->renderFrame(icon->currentFrame(), size));
                        }
                    }
                    pos = text.indexOf(it.key(), pos + it.key().size());
                }
            }
        }
        return;
    }

    int globalChar = 0;
    qreal globalY = 0;
    int paraStart = 0;

    while (paraStart <= text.length()) {
        int paraEnd = text.indexOf('\n', paraStart);
        if (paraEnd < 0) paraEnd = text.length();
        QString para = text.mid(paraStart, paraEnd - paraStart);

        if (!para.isEmpty()) {
            QTextLayout layout(para, font);
            if (!messageLinks(text).isEmpty()) {
                QTextOption wrap; wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); layout.setTextOption(wrap);
            }
            layout.beginLayout();
            qreal localY = 0;
            while (true) {
                QTextLine line = layout.createLine();
                if (!line.isValid()) break;
                line.setLineWidth((qreal)textWidth);
                line.setPosition(QPointF(0, globalY + localY));
                localY += line.height();

                int ls = globalChar + line.textStart();
                int le = ls + line.textLength();

                for (auto it = g_animatedEmojiMap.begin(); it != g_animatedEmojiMap.end(); ++it) {
                    const QString &unicode = it.key();
                    int pos = text.indexOf(unicode, ls);
                    while (pos >= 0 && pos + unicode.length() <= le) {
                        int linePos = pos - globalChar;
                        int cp1 = linePos, cp2 = linePos + unicode.length();
                        qreal x1 = line.cursorToX(&cp1);
                        qreal x2 = line.cursorToX(&cp2);

                        LottieIcon *icon = AnimTimer::instance()->icon(it.value());
                        if (icon && icon->isValid()) {
                            int eW = qMax(1, (int)((x2 - x1) * 1.3));
                            int eH = qMax(1, (int)(line.height() * 1.3));
                            QImage frame = icon->renderFrame(icon->currentFrame(), QSize(eW, eH));
                            qreal cx = x1 + (x2 - x1 - eW) * 0.5;
                            qreal cy = (line.height() - eH) * 0.5;
                            p->drawImage(QPointF(textRect.left() + cx, textRect.top() + line.y() + cy), frame);
                        }
                        pos = text.indexOf(unicode, pos + unicode.length());
                    }
                }
            }
            layout.endLayout();
            globalY += localY;
        } else {
            globalY += QFontMetrics(font).lineSpacing();
        }

        globalChar += para.length();
        if (paraEnd < text.length()) globalChar++;
        paraStart = paraEnd + 1;
    }
}

void MessageDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const {
    const Layout layout = calcLayout(option, index);
    paintBody(painter, option, index, layout);
    painter->save(); painter->translate(option.rect.topLeft());
    painter->setClipRect(QRect(QPoint(0, 0), option.rect.size()));
    if (layout.quoteRect.isValid()) drawQuote(painter, layout, index, option);
    if (layout.reactionTime.isValid()) drawReactions(painter, layout, option);
    if (index.data(MsgHighlightRole).toBool()) {
        QColor color = option.palette.highlight().color(); color.setAlpha(55);
        painter->fillRect(QRect(QPoint(0, 0), option.rect.size()), color);
    }
    painter->restore();
}

void MessageDelegate::paintBody(QPainter* painter, const QStyleOptionViewItem& option,
    const QModelIndex& index, const Layout& L) const {
    painter->save();
    painter->translate(option.rect.topLeft());
    painter->setClipRect(QRect(QPoint(0, 0), option.rect.size()));

    if (L.deliveryRect.isValid()) {
        QFont font = option.font;
        font.setPointSize(qMax(1, font.pointSize() - 2));
        painter->setFont(font);
        painter->setPen(guiSecondaryText(option.palette));
        const auto text = QFontMetrics(font).elidedText(index.data(MsgDeliveryStateRole).toString(), Qt::ElideRight, L.deliveryRect.width());
        painter->drawText(L.deliveryRect, Qt::AlignRight | Qt::AlignVCenter, text);
    }

    if (L.emojiOnly) {
        QString msgText = index.data(MsgTextRole).toString();
        QString trimmed = msgText.trimmed();
        QFontMetrics fm(option.font);
        int lineH = fm.lineSpacing();
        int eSize = (L.emojiCount == 1) ? lineH * 4 : lineH * 2;

        int x = L.emojiArea.left();
        int y = L.emojiArea.top();
        int pos = 0;
        while (pos < trimmed.length()) {
            if (trimmed.at(pos).isSpace()) { ++pos; continue; }
            QString emojiStr;
            bool found = false;
            for (const auto &cat : builtinEmojiSet()) {
                for (const auto &e : cat.items) {
                    if (e.unicode.length() > trimmed.length() - pos) continue;
                    if (QStringView(trimmed).mid(pos, e.unicode.length()) == e.unicode) {
                        emojiStr = e.unicode;
                        pos += e.unicode.length();
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            if (emojiStr.isEmpty()) { ++pos; continue; }

            bool animated = g_animatedEmojiMap.contains(emojiStr);
            if (animated) {
                LottieIcon *icon = AnimTimer::instance()->icon(g_animatedEmojiMap[emojiStr]);
                if (icon && icon->isValid()) {
                    QImage frame = icon->renderFrame(icon->currentFrame(), QSize(eSize, eSize));
                    painter->drawImage(x, y, frame);
                } else {
                    QFont big = option.font;
                    big.setPointSize((int)(big.pointSize() * (L.emojiCount == 1 ? 4.0 : 2.0)));
                    painter->setFont(big);
                    painter->setPen(option.palette.text().color());
                    painter->drawText(QRect(x, y, eSize, eSize), Qt::AlignCenter, emojiStr);
                }
            } else {
                QFont big = option.font;
                big.setPointSize((int)(big.pointSize() * (L.emojiCount == 1 ? 4.0 : 2.0)));
                painter->setFont(big);
                painter->setPen(option.palette.text().color());
                painter->drawText(QRect(x, y, eSize, eSize), Qt::AlignCenter, emojiStr);
            }
            x += eSize + lineH / 2;
        }
        painter->restore();
        return;
    }

    if (L.isVoice) {
        auto* view = qobject_cast<QAbstractItemView*>(const_cast<QWidget*>(option.widget));
        if (view && s_voiceView != view) {
            s_voiceView = view;
            GUI_DEBUG("voice delegate: tracking view=%p", static_cast<void*>(view));
            QObject::connect(view, &QObject::destroyed, QCoreApplication::instance(), [view] {
                if (s_voiceView && s_voiceView.data() != view) return; // Другой текущий view продолжает воспроизведение.
                s_voiceView = nullptr;
                MessageDelegate::stopAllAutoPlay();
                GUI_DEBUG("voice delegate: view=%p destroyed, playback state released", static_cast<void*>(view));
            });
        }
        drawBubble(painter, L, option);
        if (!L.narrow) drawAvatar(painter, L, index);

        QString filePath = index.data(MsgVoiceFileRole).toString();
        VoicePlayState copySt;
        VoicePlayState* st = &copySt;
        if (!filePath.isEmpty() && s_voiceStates.contains(filePath)) {
            st = &s_voiceStates[filePath];
        } else {
            int stateVal = index.data(MsgVoicePlayStateRole).toInt();
            copySt.playing = (stateVal == 1);
            st = &copySt;
        }

        bool isPlaying = st->playing;
        bool isPaused = st->paused;

        /* compute elapsed/frac for progress */
        float durSec = st->durationSec;
        if (durSec <= 0) {
            QString ds = index.data(MsgVoiceDurationRole).toString();
            ds.remove('s'); durSec = ds.toFloat();
        }
        float frac = st->displayFrac;

        QRectF playR = L.voicePlayBtnRect;

        /* Play/Pause icon (small — 10px triangle) */
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x33, 0x90, 0xEC));
        if (isPlaying || isPaused) {
            qreal bw = playR.width() / 5.0;
            qreal gap = playR.width() / 8.0;
            qreal top = playR.center().y() - playR.height() / 3.0;
            qreal bot = playR.center().y() + playR.height() / 3.0;
            painter->drawRoundedRect(QRectF(playR.center().x() - gap - bw, top, bw, bot - top), 1, 1);
            painter->drawRoundedRect(QRectF(playR.center().x() + gap, top, bw, bot - top), 1, 1);
        } else {
            QPainterPath path;
            qreal cx = playR.center().x() - playR.width() / 6.0;
            qreal cy = playR.center().y();
            qreal hs = playR.height() / 2.5;
            path.moveTo(cx, cy - hs);
            path.lineTo(cx + hs * 1.5, cy);
            path.lineTo(cx, cy + hs);
            path.closeSubpath();
            painter->drawPath(path);
        }

        /* Waveform (25px) */
        QList<QVariant> wf = index.data(MsgVoiceWaveformRole).toList();
        int wfN = wf.size();
        QRect wfR = L.voiceWaveformRect;
        if (wfN > 0) {
            float barW = (float)wfR.width() / wfN;
            int ph = wfR.height() - 3;
            for (int i = 0; i < wfN; i++) {
                float logV = wf[i].toFloat();  /* already 0..1 normalized */
                int barH = 2 + (int)(logV * (ph - 2));
                int x = wfR.left() + (int)(i * barW);
                int y = wfR.top() + ph - barH;
                int bw = qMax(2, (int)(barW * 0.8f));
                bool before = wfN > 0 && (float)i / wfN <= frac;
                QColor played(0x33, 0x90, 0xEC);
                QColor unplayed(0x33, 0x90, 0xEC, 80);
                QColor idle(0x70, 0x70, 0x78, 130);
                painter->setBrush((isPlaying || isPaused) ? (before ? played : unplayed) : idle);
                painter->drawRect(QRect(x, y, bw, barH));
            }
        }

        /* Progress bar (3px, at bottom of waveform area) */
        int ph = 3;
        int pbY = wfR.bottom() - ph;
        painter->setBrush(QColor(0x44, 0x44, 0x44));
        painter->drawRoundedRect(QRect(wfR.left(), pbY, wfR.width(), ph), 2, 2);
        if (frac > 0) {
            painter->setBrush(QColor(0x33, 0x90, 0xEC));
            painter->drawRoundedRect(QRect(wfR.left(), pbY, (int)(wfR.width() * frac), ph), 2, 2);
        }

        /* Duration + elapsed on bottom line */
        QFont timeFont = option.font;
        timeFont.setPointSize(timeFont.pointSize() - 2);
        painter->setFont(timeFont);
        painter->setPen(guiSecondaryText(option.palette));
        int eSec = (int)st->displayElapsed, dSec = (int)durSec;
        QString durText = QString("%1:%2 / %3:%4")
            .arg(eSec / 60).arg(eSec % 60, 2, 10, QChar('0'))
            .arg(dSec / 60).arg(dSec % 60, 2, 10, QChar('0'));
        painter->drawText(L.voiceDurationRect, Qt::AlignLeft | Qt::AlignVCenter, durText);

        /* Timestamp */
        QFont tsFont = option.font;
        tsFont.setPointSize(tsFont.pointSize() - 1);
        painter->setFont(tsFont);
        if (!L.reactionTime.isValid()) painter->drawText(L.voiceTimeRect, Qt::AlignRight | Qt::AlignVCenter,
            index.data(MsgTimeRole).toString());

        /* media bar (left edge, 3px) */
        { QColor barC = L.mediaBarColor == 2 ? QColor(0x40, 0xC0, 0x40) : L.mediaBarColor == 1 ? QColor(0xF0, 0xC0, 0x40) : QColor(0x80, 0x80, 0x80);
          painter->setPen(Qt::NoPen); painter->setBrush(barC); painter->drawRect(L.mediaBarRect); }
        /* progress bar (bottom edge, 3px) */
        if (L.progressBarRect.isValid()) {
            painter->setBrush(QColor(0x44, 0x44, 0x44));
            painter->drawRoundedRect(L.progressBarRect, 2, 2);
            int pw = (int)(L.progressBarRect.width() * L.progressFrac);
            if (pw > 0) { painter->setBrush(QColor(0x33, 0x90, 0xEC)); painter->drawRoundedRect(QRect(L.progressBarRect.left(), L.progressBarRect.top(), pw, 3), 2, 2); }
        }

        painter->restore();
        return;
    }

    if (L.isVideo) {
        drawBubble(painter, L, option);
        if (!L.narrow) drawAvatar(painter, L, index);

        int dlState = index.data(MsgFileDownloadStateRole).toInt();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.addRoundedRect(L.videoThumbRect, kRadius, kRadius);
        painter->setClipPath(clip);

        const QImage thumb = index.data(MsgImagePreviewRole).value<QImage>();

        if (!thumb.isNull()) {
            QSize source = L.videoThumbRect.size().scaled(thumb.size(), Qt::KeepAspectRatio);
            QRect crop(QPoint((thumb.width() - source.width()) / 2, (thumb.height() - source.height()) / 2), source);
            painter->drawImage(L.videoThumbRect, thumb, crop);
        } else {
            painter->fillRect(L.videoThumbRect, QColor(30, 30, 35));
            QFont iconFont = option.font;
            iconFont.setPointSize(iconFont.pointSize() + 10);
            painter->setFont(iconFont);
            painter->setPen(QColor(150, 150, 150));
            painter->drawText(L.videoThumbRect, Qt::AlignCenter, QString::fromUtf8("\xF0\x9F\x8E\xAC"));
        }

        /* play-оверлей */
        if (dlState == 2) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0, 0, 0, 110));
            painter->drawEllipse(L.videoPlayBtnRect);
            QPainterPath tri;
            int cx = L.videoPlayBtnRect.center().x() - L.videoPlayBtnRect.width() / 12;
            int cy = L.videoPlayBtnRect.center().y();
            int hs = L.videoPlayBtnRect.height() / 4;
            tri.moveTo(cx, cy - hs);
            tri.lineTo(cx + hs * 2, cy);
            tri.lineTo(cx, cy + hs);
            tri.closeSubpath();
            painter->setBrush(Qt::white);
            painter->drawPath(tri);
        }

        /* бейдж: длительность (m:ss) */
        QString durStr = index.data(MsgVoiceDurationRole).toString();
        if (!durStr.isEmpty()) {
            QFont durFont = option.font;
            durFont.setPointSize(durFont.pointSize() - 1);
            painter->setFont(durFont);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0, 0, 0, 160));
            painter->drawRoundedRect(L.videoDurBadgeRect, 5, 5);
            painter->setPen(Qt::white);
            painter->drawText(L.videoDurBadgeRect, Qt::AlignCenter, durStr);
        }
        painter->restore();

        /* progress overlay (скачивание) */
        if (dlState == 1) {
            painter->setBrush(QColor(0, 0, 0, 140));
            painter->drawRect(L.videoThumbRect);
            QFont pfont = option.font;
            pfont.setPointSize(pfont.pointSize() + 2);
            painter->setFont(pfont);
            painter->setPen(Qt::white);
            painter->drawText(L.videoThumbRect, Qt::AlignCenter,
                              QStringLiteral("%1%").arg((int)(L.progressFrac * 100)));
        }

        /* media bar (левая кромка) */
        { QColor barC = L.mediaBarColor == 2 ? QColor(0x40, 0xC0, 0x40) : L.mediaBarColor == 1 ? QColor(0xF0, 0xC0, 0x40) : QColor(0x80, 0x80, 0x80);
          painter->setPen(Qt::NoPen); painter->setBrush(barC); painter->drawRect(L.mediaBarRect); }
        if (L.progressBarRect.isValid()) {
            painter->setBrush(QColor(0x44, 0x44, 0x44));
            painter->drawRoundedRect(L.progressBarRect, 2, 2);
            int pw = (int)(L.progressBarRect.width() * L.progressFrac);
            if (pw > 0) { painter->setBrush(QColor(0x33, 0x90, 0xEC)); painter->drawRoundedRect(QRect(L.progressBarRect.left(), L.progressBarRect.top(), pw, 3), 2, 2); }
        }

        /* строка под пузырём: res · size · bitrate + время */
        {
            qint64 fsz = index.data(MsgFileSizeRole).toLongLong();
            qint64 durMs = index.data(MsgVideoDurationMsRole).toLongLong();
            int vw = index.data(MsgVideoWidthRole).toInt();
            int vh = index.data(MsgVideoHeightRole).toInt();
            QStringList info;
            if (vw > 0 && vh > 0) info << QString("%1×%2").arg(vw).arg(vh);
            QString sz = formatFileSize(fsz);
            if (!sz.isEmpty()) info << sz;
            QString br = formatBitrate(fsz, durMs);
            if (!br.isEmpty()) info << br;

            QFont smallFont = option.font;
            smallFont.setPointSize(smallFont.pointSize() - 2);
            painter->setFont(smallFont);
            painter->setPen(guiSecondaryText(option.palette));

            if (!info.isEmpty())
                painter->drawText(L.statusRect, Qt::AlignLeft | Qt::AlignVCenter, info.join(" \xC2\xB7 "));
            if (!L.reactionTime.isValid()) painter->drawText(L.statusRect, Qt::AlignRight | Qt::AlignVCenter,
                index.data(MsgTimeRole).toString());
        }

        painter->restore();
        return;
    }

    if (L.isImage) {
        drawBubble(painter, L, option);
        if (!L.narrow) drawAvatar(painter, L, index);

        int dlState = index.data(MsgFileDownloadStateRole).toInt();
        QString filePath = index.data(MsgVoiceFileRole).toString();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(L.imageThumbRect, kRadius, kRadius);
        painter->setClipPath(clip);

        const QImage image = index.data(MsgImagePreviewRole).value<QImage>();
        if (!image.isNull()) {
            painter->fillRect(L.imageThumbRect, QColor(20, 20, 24));
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            QSize fitted = image.size().scaled(L.imageThumbRect.size(), Qt::KeepAspectRatio);
            QPoint origin(L.imageThumbRect.left() + (L.imageThumbRect.width() - fitted.width()) / 2,
                          L.imageThumbRect.top() + (L.imageThumbRect.height() - fitted.height()) / 2);
            painter->drawImage(QRect(origin, fitted), image);
        } else {
            painter->fillRect(L.imageThumbRect, QColor(30, 30, 35));
            QFont iconFont = option.font;
            iconFont.setPointSize(iconFont.pointSize() + 10);
            painter->setFont(iconFont);
            painter->setPen(QColor(150, 150, 150));
            painter->drawText(L.imageThumbRect, Qt::AlignCenter, index.data(MsgImageErrorRole).toBool()
                ? QCoreApplication::translate("ChatGui", "Failed to load photo") : QString::fromUtf8("\xF0\x9F\x93\xB7"));
        }

        /* бейдж: разрешение · размер */
        qint64 fsz = index.data(MsgFileSizeRole).toLongLong();
        int iw = index.data(MsgImageWidthRole).toInt();
        int ih = index.data(MsgImageHeightRole).toInt();
        QString res = (iw > 0 && ih > 0) ? QString("%1×%2").arg(iw).arg(ih) : QString();
        QString sizeStr = formatFileSize(fsz);
        QString badge = res.isEmpty() ? sizeStr : res + " \xC2\xB7 " + sizeStr;
        if (!badge.isEmpty()) {
            QFont durFont = option.font;
            durFont.setPointSize(durFont.pointSize() - 1);
            painter->setFont(durFont);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0, 0, 0, 160));
            painter->drawRoundedRect(L.imageBadgeRect, 5, 5);
            painter->setPen(Qt::white);
            painter->drawText(L.imageBadgeRect, Qt::AlignCenter, badge);
        }
        painter->restore();

        /* progress overlay (скачивание) */
        if (dlState == 1) {
            painter->setBrush(QColor(0, 0, 0, 140));
            painter->drawRect(L.imageThumbRect);
            QFont pfont = option.font;
            pfont.setPointSize(pfont.pointSize() + 2);
            painter->setFont(pfont);
            painter->setPen(Qt::white);
            painter->drawText(L.imageThumbRect, Qt::AlignCenter,
                              QStringLiteral("%1%").arg((int)(L.progressFrac * 100)));
        }

        /* media bar (левая кромка) */
        { QColor barC = L.mediaBarColor == 2 ? QColor(0x40, 0xC0, 0x40) : L.mediaBarColor == 1 ? QColor(0xF0, 0xC0, 0x40) : QColor(0x80, 0x80, 0x80);
          painter->setPen(Qt::NoPen); painter->setBrush(barC); painter->drawRect(L.mediaBarRect); }
        if (L.progressBarRect.isValid()) {
            painter->setBrush(QColor(0x44, 0x44, 0x44));
            painter->drawRoundedRect(L.progressBarRect, 2, 2);
            int pw = (int)(L.progressBarRect.width() * L.progressFrac);
            if (pw > 0) { painter->setBrush(QColor(0x33, 0x90, 0xEC)); painter->drawRoundedRect(QRect(L.progressBarRect.left(), L.progressBarRect.top(), pw, 3), 2, 2); }
        }

        painter->restore();
        return;
    }

        if (L.isFile) {
        drawBubble(painter, L, option);
        if (!L.narrow) drawAvatar(painter, L, index);

        int dlState = index.data(MsgFileDownloadStateRole).toInt();

        /* File icon based on download state: 0=not_started, 1=downloading, 2=downloaded */
        QFont iconFont = option.font;
        iconFont.setPointSize(iconFont.pointSize() + 6);
        painter->setFont(iconFont);
        painter->setPen(option.palette.text().color());
        QRect iconRect(L.bubbleRect.left() + kPadH, L.bubbleRect.top() + kPadTop, 24, 24);
        if (dlState == 2)
            painter->drawText(iconRect, Qt::AlignCenter, QString::fromUtf8("\xE2\x9C\x85"));
        else
            painter->drawText(iconRect, Qt::AlignCenter, QString::fromUtf8("\xE2\xAC\x87\xEF\xB8\x8F"));

        QFont fileFont = option.font;
        fileFont.setBold(true);
        painter->setFont(fileFont);
        painter->setPen(option.palette.text().color());
        QString displayName = index.data(MsgFileDisplayNameRole).toString();
        painter->drawText(L.textRect, Qt::AlignLeft | Qt::AlignVCenter, displayName);

        QFont sizeFont = option.font;
        sizeFont.setPointSize(sizeFont.pointSize() - 2);
        painter->setFont(sizeFont);
        painter->setPen(guiSecondaryText(option.palette));
        qint64 fsz = index.data(MsgFileSizeRole).toLongLong();
        QString sizeStr = formatFileSize(fsz);
        QString stateStr = (dlState == 2) ? QString::fromUtf8("\xE2\x9C\x85") : (dlState == 1) ? QCoreApplication::translate("ChatGui",
            "Downloading...") : QCoreApplication::translate("ChatGui",
            "Download");
        QString deliveryState = index.data(MsgDeliveryStateRole).toString();
        if (!deliveryState.isEmpty()) stateStr = deliveryState;
        QString info = sizeStr + " \xC2\xB7 " + stateStr;
        int nameH = QFontMetrics(fileFont).height();
        QRect sizeRect(L.textRect.left(), L.textRect.top() + nameH, L.textRect.width(), QFontMetrics(sizeFont).height());
        painter->drawText(sizeRect, Qt::AlignLeft | Qt::AlignVCenter, info);

        /* media bar (left edge, 3px) */
        { QColor barC = L.mediaBarColor == 2 ? QColor(0x40, 0xC0, 0x40) : L.mediaBarColor == 1 ? QColor(0xF0, 0xC0, 0x40) : QColor(0x80, 0x80, 0x80);
          painter->setPen(Qt::NoPen); painter->setBrush(barC); painter->drawRect(L.mediaBarRect); }
        /* progress bar (bottom edge, 3px) */
        if (L.progressBarRect.isValid()) {
            painter->setBrush(QColor(0x44, 0x44, 0x44));
            painter->drawRoundedRect(L.progressBarRect, 2, 2);
            int pw = (int)(L.progressBarRect.width() * L.progressFrac);
            if (pw > 0) { painter->setBrush(QColor(0x33, 0x90, 0xEC)); painter->drawRoundedRect(QRect(L.progressBarRect.left(), L.progressBarRect.top(), pw, 3), 2, 2); }
        }

        painter->restore();
        return;


    }

    auto *cv = qobject_cast<const ChatView *>(option.widget);
    bool singleTextSel = false;
    bool multiRowSel   = false;
    if (cv && cv->hasSelection()) {
        if (cv->isRowSelected(index)) {
            if (cv->selStart() == cv->selEnd()) {
                singleTextSel = true;
            } else {
                multiRowSel = true;
            }
        }
    }

    drawBubble(painter, L, option);

    if (multiRowSel) {
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x33, 0x90, 0xEC, 40));
        painter->drawRect(L.bubbleRect);
        painter->restore();
    }

    if (!L.narrow)
        drawAvatar(painter, L, index);

    if (!L.isOutgoing) {
        QFont authorFont = option.font;
        authorFont.setBold(true);
        painter->setFont(authorFont);
        painter->setPen(option.palette.link().color());
        painter->drawText(L.authorRect, Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(MsgAuthorRole).toString());
    }

    painter->setPen(option.palette.text().color());
    QFont textFont = option.font;
    textFont.setPointSize(textFont.pointSize());
    painter->setFont(textFont);

    QString msgText = index.data(MsgTextRole).toString();
    QString overlayText = msgText;
    const bool hasLinks = !messageLinks(msgText).isEmpty();
    if (hasLinks || L.formattedText) {
        QString display = msgText; display.replace('\n', QChar(0x2028));
        QTextLayout layout(display, textFont);
        const QString compactLink = index.data(MsgInviteLinkRole).toString();
        if (L.formattedText) {
            const bool dark = option.palette.window().color().lightness() < 128;
            layoutFormattedMessage(layout, *L.formattedText, L.textRect.width(), dark);
            const QColor background = dark ? QColor(0, 0, 0, 65) : QColor(0, 0, 0, 18);
            for (int i = 0; i < layout.lineCount(); ++i) {
                const auto line = layout.lineAt(i);
                if (L.formattedText->isCode(line.textStart()))
                    painter->fillRect(QRectF(L.textRect.x(), L.textRect.y() + line.y(), L.textRect.width(), line.height()), background);
            }
        } else layoutMessageText(layout, L.textRect.width(), compactLink);
        overlayText = layout.text(); overlayText.replace(QChar(0x2028), '\n');
        QVector<QTextLayout::FormatRange> selections;
        if (singleTextSel && cv) {
            const auto& selection = cv->textSelection();
            const QPair<int, int> bounds{selection.startChar, selection.endChar};
            QTextLayout::FormatRange range;
            range.start = qMax(0, bounds.first); range.length = qMax(0, qMin(bounds.second, int(layout.text().size())) - range.start);
            range.format.setBackground(QColor(0x33, 0x90, 0xEC, 60));
            selections.append(range);
        }
        layout.draw(painter, L.textRect.topLeft(), selections);
        if (L.formattedText) {
            painter->save();
            for (int number = 0; number < L.formattedText->code.size(); ++number) {
                const auto& block = L.formattedText->code[number];
                const bool copied = m_copiedCode == index && m_copiedBlock == number && m_copiedSource == msgText;
                const auto button = messageCodeCopyRect(layout, block, L.textRect.width()).translated(L.textRect.topLeft());
                painter->fillRect(QRectF(L.textRect.x(), button.top() - 2, L.textRect.width(), button.height() + 4),
                                  option.palette.alternateBase());
                painter->setPen(option.palette.mid().color());
                painter->setBrush(copied ? option.palette.highlight() : option.palette.button());
                painter->drawRoundedRect(button.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
                painter->setPen(copied ? option.palette.highlightedText().color() : option.palette.buttonText().color());
                painter->drawText(button, Qt::AlignCenter, QCoreApplication::translate("ChatGui", copied ? "Copied" : "Copy"));
                const QRectF label(L.textRect.x() + 8, button.top(), qMax(0.0, button.left() - L.textRect.x() - 16), button.height());
                painter->setPen(option.palette.text().color());
                const QString language = block.languageName.isEmpty() ? QStringLiteral("text") : block.languageName;
                painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                                  painter->fontMetrics().elidedText(language, Qt::ElideRight, int(label.width())));
            }
            painter->restore();
        }
        if (L.formattedText)
            drawAnimatedEmojiOverlay(painter, textFont, L.formattedText->text, L.textRect.width(), L.textRect, &layout, L.formattedText.data());
    } else painter->drawText(L.textRect, Qt::AlignLeft | Qt::TextWordWrap, msgText);

    if (singleTextSel && cv && !hasLinks && !L.formattedText) {
        const auto &ts = cv->textSelection();
        const QPair<int, int> bounds{ts.startChar, ts.endChar};
        drawTextSelectionHighlight(painter, textFont, msgText,
                                    L.textRect.width(), L.textRect,
                                    QColor(0x33, 0x90, 0xEC, 60),
                                    bounds.first, bounds.second);
    }

    if (!L.formattedText) drawAnimatedEmojiOverlay(painter, textFont, overlayText, L.textRect.width(), L.textRect);

    drawStatusBar(painter, L, index, option);

    painter->restore();
}

void MessageDelegate::drawQuote(QPainter *p, const Layout &L,
                                const QModelIndex &index,
                                const QStyleOptionViewItem &opt) const {
    p->save();

    p->setPen(Qt::NoPen);
    QColor background = opt.palette.highlight().color(); background.setAlpha(28);
    p->setBrush(background);
    p->drawRoundedRect(L.quoteRect, 5, 5);

    p->setPen(Qt::NoPen);
    p->setBrush(opt.palette.highlight());
    p->drawRect(L.quoteBarRect);

    QFont boldFont = opt.font;
    boldFont.setBold(true);
    p->setFont(boldFont);
    p->setPen(opt.palette.text().color());
    p->drawText(L.quoteAuthorRect, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(boldFont).elidedText(index.data(MsgQuoteAuthorRole).toString(), Qt::ElideRight, L.quoteAuthorRect.width()));

    p->setFont(opt.font);
    p->setPen(opt.palette.text().color());
    if (!index.data(MsgReplyQuoteRole).toString().isEmpty()) {
        QTextLayout quote; layoutQuote(quote, opt.font, index.data(MsgQuoteTextRole).toString(), L.quoteTextRect.width());
        quote.draw(p, L.quoteTextRect.topLeft());
    } else p->drawText(L.quoteTextRect, Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetrics(opt.font).elidedText(index.data(MsgQuoteTextRole).toString(), Qt::ElideRight, L.quoteTextRect.width()));

    p->restore();
}

void MessageDelegate::drawBubble(QPainter *p, const Layout &L, const QStyleOptionViewItem &opt) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    const bool dark = opt.palette.window().color().lightness() < 128;
    QRectF b = L.bubbleRect;
    if (!dark) b.adjust(0.5, 0.5, -0.5, -0.5); // Контур 1px внутри bubbleRect, прямые участки совпадают с сеткой пикселей.
    qreal r = kRadius;
    qreal t = kTail;

    QPainterPath body;
    body.addRoundedRect(b, r, r);

    QPainterPath tail;
    if (!L.isOutgoing) {
        qreal tx = b.left();
        qreal ty = b.bottom();
        tail.moveTo(tx, ty - r);
        tail.lineTo(tx - t, ty - t / 2.0);
        tail.lineTo(tx + r, ty);
    } else {
        qreal tx = b.right();
        qreal ty = b.bottom();
        tail.moveTo(tx, ty - r);
        tail.lineTo(tx + t, ty - t / 2.0);
        tail.lineTo(tx - r, ty);
    }
    tail.closeSubpath();

    QPainterPath combined = body.united(tail);

    QColor bubbleFill = dark ? opt.palette.window().color().lighter(135) : opt.palette.base().color();
    if (L.isOutgoing && dark) {
        bubbleFill.setRed(qMax(0, bubbleFill.red() - 10));
        bubbleFill.setGreen(qMin(255, bubbleFill.green() + 5));
        bubbleFill.setBlue(qMax(0, bubbleFill.blue() - 10));
    }
    p->setPen(dark ? QPen(Qt::NoPen) : QPen(opt.palette.mid().color(), 1));
    p->setBrush(bubbleFill);
    p->drawPath(combined);

    p->restore();
}

void MessageDelegate::drawReactions(QPainter* p, const Layout& layout, const QStyleOptionViewItem& option) const {
    p->save(); p->setRenderHint(QPainter::Antialiasing);
    for (const auto& chip : layout.reactionChips) {
        p->setPen(Qt::NoPen); p->setBrush(chip.mine ? QColor(51, 144, 236, 100) : QColor(0, 0, 0, 40));
        p->drawRoundedRect(chip.rect, 9, 9);
        const QRect iconRect(chip.rect.left() + 5, chip.rect.top() + (chip.rect.height() - 18) / 2, 18, 18);
        LottieIcon* icon = g_animatedEmojiMap.contains(chip.emoji) ? AnimTimer::instance()->icon(g_animatedEmojiMap[chip.emoji]) : nullptr;
        QFont font = option.font; font.setPointSize(qMax(1, font.pointSize() - 1)); p->setFont(font);
        if (icon && icon->isValid()) p->drawImage(iconRect, icon->renderFrame(icon->currentFrame(), QSize(18, 18)));
        else { p->setPen(option.palette.text().color()); p->drawText(iconRect, Qt::AlignCenter, chip.emoji); }
        font.setBold(chip.mine); p->setFont(font); p->setPen(option.palette.text().color());
        p->drawText(chip.rect.adjusted(25, 0, -5, 0), Qt::AlignLeft | Qt::AlignVCenter, QString::number(chip.count));
    }
    QFont timeFont = option.font; timeFont.setPointSize(qMax(1, timeFont.pointSize() - 2)); p->setFont(timeFont);
    p->setPen(guiSecondaryText(option.palette));
    p->drawText(layout.reactionTime, Qt::AlignRight | Qt::AlignVCenter,
        QFontMetrics(timeFont).elidedText(layout.reactionTimeText, Qt::ElideLeft, layout.reactionTime.width()));
    p->restore();
}

void MessageDelegate::drawStatusBar(QPainter* p, const Layout& layout, const QModelIndex& index,
    const QStyleOptionViewItem& option) const {
    if (layout.reactionTime.isValid()) return;
    p->save();
    QFont font = option.font; font.setPointSize(qMax(1, font.pointSize() - 2)); p->setFont(font);
    p->setPen(guiSecondaryText(option.palette));
    QString text = index.data(MsgTimeRole).toString();
    p->drawText(layout.statusRect, Qt::AlignRight | Qt::AlignVCenter, text);
    p->restore();
}

void MessageDelegate::drawAvatar(QPainter *p, const Layout &L,
                                 const QModelIndex &index) const {
    QPixmap av = index.data(MsgAvatarRole).value<QPixmap>();
    if (av.isNull()) return;

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(L.avatarRect);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    p->setClipPath(clip);
    p->drawPixmap(L.avatarRect, av);
    p->restore();
}

void MessageDelegate::startAutoPlay(const QString& filePath, float actualDurationSec, uint64_t playbackId) {
    if (!std::isfinite(actualDurationSec) || actualDurationSec <= 0 || !playbackId) {
        GUI_WARN("voice delegate: invalid playback duration/id"); return;
    }
    ensureVoiceTimer();
    s_voiceStates.clear();
    VoicePlayState& st = s_voiceStates[filePath];
    st.playbackId = playbackId;
    st.playing = true;
    st.paused = false;
    st.durationSec = (actualDurationSec > 0) ? actualDurationSec : 1.0f;
    st.pausedPosSec = 0.0f;
    st.displayFrac = 0.0f;
    st.displayElapsed = 0.0f;
    s_voiceTimer->start();
    if (s_voiceView) s_voiceView->viewport()->update();
}

void MessageDelegate::stopAllAutoPlay() {
    VoicePlayer::stop();
    s_voiceStates.clear();
    if (s_voiceTimer) s_voiceTimer->stop();
}
