#include <QCoreApplication>
#include "chatview.h"
#include "messagedelegate.h"
#include "message_text.h"
#include "emoji.h"
#include "debug_ui.h"
#include <QPainter>
#include <QClipboard>
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStyleOptionSlider>

/* Перерисовываем светлее только ползунок; дорожку и стрелки рисует стандартный стиль. */
class HistoryScrollBar : public QScrollBar {
public:
    using QScrollBar::QScrollBar;
protected:
    void paintEvent(QPaintEvent* event) override {
        QScrollBar::paintEvent(event);
        QStyleOptionSlider option; initStyleOption(&option);
        option.subControls = QStyle::SC_ScrollBarSlider;
        option.palette.setColor(QPalette::Button, palette().window().color().lightness() < 128 ? QColor("#606060") : QColor("#a0a0a0"));
        QPainter painter(this);
        painter.setClipRect(style()->subControlRect(QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, this));
        style()->drawComplexControl(QStyle::CC_ScrollBar, &option, &painter, this);
    }
};

// Текст и реакции сообщения используют общий набор анимированных значков.
static QSet<QString> animatedEmojiPaths(const QModelIndex &index) {
    QSet<QString> paths;
    const auto formatted = formattedMessageText(index);
    const QString text = formatted ? formatted->text : index.data(MsgTextRole).toString();
    for (auto it = g_animatedEmojiMap.constBegin(); it != g_animatedEmojiMap.constEnd(); ++it) {
        for (int pos = text.indexOf(it.key()); pos >= 0; pos = text.indexOf(it.key(), pos + it.key().size())) {
            if (!formatted || !formatted->isCode(pos)) { paths.insert(it.value()); break; }
        }
    }
    for (const auto &reaction : index.data(MsgReactionsRole).toList()) {
        const QString path = g_animatedEmojiMap.value(reaction.toMap().value("emoji").toString());
        if (!path.isEmpty()) paths.insert(path);
    }
    return paths;
}

ChatView::ChatView(QWidget *parent)
    : QListView(parent)
{
    setMouseTracking(true);
    setVerticalScrollBar(new HistoryScrollBar(Qt::Vertical, this));
    GUI_DEBUG("history scrollbar: native style with theme-aware contrasting thumb");

    m_scrollDownBtn = new QPushButton(QStringLiteral("\u25BC"), this);
    m_scrollDownBtn->setFixedSize(36, 36);
    m_scrollDownBtn->setCursor(Qt::PointingHandCursor);
    m_scrollDownBtn->setStyleSheet(
        "QPushButton {"
        "  background: rgba(60,60,60,180);"
        "  color: #ffffff;"
        "  border: none;"
        "  border-radius: 18px;"
        "  font-size: 16px;"
        "}"
        "QPushButton:hover {"
        "  background: rgba(90,90,90,220);"
        "}"
    );
    m_scrollDownBtn->setToolTip(QCoreApplication::translate("ChatGui", "Scroll to bottom"));
    m_scrollDownBtn->setVisible(false);

    connect(m_scrollDownBtn, &QPushButton::clicked, this, [this]() {
        emit latestRequested();
        scrollToBottom();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &ChatView::updateScrollDownBtn);
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &ChatView::updateScrollDownBtn);

    m_scrollDownBtn->raise();
}

void ChatView::setBackgroundImage(const QString &path) {
    if (!m_background.load(path)) {
        GUI_WARN("ChatView: failed to load background path=%s", qPrintable(path));
        return;
    }
    { QPainter painter(&m_background); painter.fillRect(m_background.rect(), QColor(0, 0, 0, 64)); }
    GUI_DEBUG("ChatView: background loaded path=%s size=%dx%d black_overlay=64/255",
              qPrintable(path), m_background.width(), m_background.height());
    viewport()->update();
}

void ChatView::setHistoricalWindow(bool enabled) {
    m_historicalWindow = enabled;
    updateScrollDownBtn();
}

void ChatView::resizeEvent(QResizeEvent *event) {
    QListView::resizeEvent(event);
    scheduleDelayedItemsLayout();
    updateScrollDownBtn();
}

void ChatView::scrollContentsBy(int dx, int dy) {
    QListView::scrollContentsBy(dx, dy);
    viewport()->update();
}

void ChatView::paintEvent(QPaintEvent *event) {
    {
        QPainter p(viewport());
        if (!m_background.isNull() && palette().window().color().lightness() < 128)
            p.drawTiledPixmap(viewport()->rect(), m_background);
        else p.fillRect(viewport()->rect(), palette().base());
    }
    QListView::paintEvent(event);
    QSet<QString> visible;
    if (model()) {
        const QRect area = viewport()->rect();
        for (int row = 0; row < model()->rowCount(); ++row) {
            const QModelIndex index = model()->index(row, 0);
            if (!visualRect(index).intersects(area)) continue;
            const QString message = QStringLiteral("%1/%2/%3/")
                .arg(index.data(MsgChannelIdRole).toString()).arg(index.data(MsgIdRole).toLongLong())
                .arg(index.data(MsgIsOutgoingRole).toInt());
            for (const auto &path : animatedEmojiPaths(index)) visible.insert(message + path);
        }
    }
    const int appeared = (visible - m_visibleAnimations).size();
    m_visibleAnimations = visible;
    if (appeared > 0) {
        GUI_DEBUG("ChatView: visible animation trigger appeared=%d visible=%d", appeared, (int)visible.size());
        emit animationTriggered();
    }
}

bool ChatView::isRowSelected(const QModelIndex &idx) const {
    if (!m_selStart.isValid() || !m_selEnd.isValid() || !idx.isValid())
        return false;
    int r = idx.row();
    int a = m_selStart.row();
    int b = m_selEnd.row();
    if (a > b) qSwap(a, b);
    return r >= a && r <= b;
}

void ChatView::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QListView::mousePressEvent(event);
        return;
    }
    if (hasSelection()) {
        m_selStart = QPersistentModelIndex();
        m_selEnd   = QPersistentModelIndex();
        m_textSel  = ChatTextSelection{};
        m_selecting = false;
        viewport()->update();
    }
    QModelIndex idx = indexAt(viewport()->mapFrom(this, event->pos()));
    m_pressIndex = QPersistentModelIndex(idx);
    m_pressPos = event->pos();
    QListView::mousePressEvent(event);
}

bool ChatView::viewportEvent(QEvent *event) {
    if (event->type() == QEvent::Leave && m_lastHovered.isValid()) {
        m_lastHovered = QPersistentModelIndex();
    }
    return QListView::viewportEvent(event);
}

void ChatView::mouseMoveEvent(QMouseEvent *event) {
    if (!m_selecting && m_pressIndex.isValid()) {
        int dx = event->pos().x() - m_pressPos.x();
        int dy = event->pos().y() - m_pressPos.y();
        if (dx * dx + dy * dy >= 25) {
            m_selStart = m_pressIndex;
            m_selEnd   = m_pressIndex;
            m_selecting = true;
            QRect r = visualRect(QModelIndex(m_pressIndex));
            m_textSel.index    = m_pressIndex;
            m_textSel.source   = m_pressIndex.data(MsgTextRole).toString();
            m_textSel.clickPos = m_pressPos - r.topLeft();
            m_textSel.dragPos  = m_textSel.clickPos;
            m_textSel.active   = true;
            viewport()->update();
        }
    }
    if (!m_selecting) {
        QModelIndex idx = indexAt(viewport()->mapFrom(this, event->pos()));
        const bool link = m_linkHitTest && !m_linkHitTest(idx, event->pos()).isEmpty();
        const bool copyCode = m_codeCopyHitTest && m_codeCopyHitTest(idx, event->pos());
        viewport()->setCursor(link || copyCode ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (idx != QModelIndex(m_lastHovered)) {
            m_lastHovered = idx;
            const auto paths = animatedEmojiPaths(idx);
            if (!paths.isEmpty()) {
                GUI_DEBUG("ChatView: hover animation trigger row=%d icons=%d", idx.row(), (int)paths.size());
                emit animationTriggered();
            }
        }
    }
    if (!m_selecting) {
        QListView::mouseMoveEvent(event);
        return;
    }
    QModelIndex idx = indexAt(viewport()->mapFrom(this, event->pos()));
    if (idx.isValid()) {
        m_selEnd = idx;
    }
    if (m_textSel.active) {
        QRect r = visualRect(QModelIndex(m_textSel.index));
        m_textSel.dragPos = event->pos() - r.topLeft();
    }
    viewport()->update();
    event->accept();
}

void ChatView::mouseReleaseEvent(QMouseEvent *event) {
    const QModelIndex pressed(m_pressIndex);
    if (event->button() == Qt::LeftButton && !m_selecting && pressed.isValid() &&
        (event->pos() - m_pressPos).manhattanLength() < 5 && indexAt(event->pos()) == pressed) {
        const QString link = m_linkHitTest ? m_linkHitTest(pressed, event->pos()) : QString();
        if (!link.isEmpty()) {
            m_pressIndex = QPersistentModelIndex();
            GUI_INFO("message link: activated row=%d bytes=%d", pressed.row(), link.size());
            emit linkActivated(link);
            event->accept(); return;
        }
    }
    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        viewport()->update();
    }
    m_pressIndex = QPersistentModelIndex();
    QListView::mouseReleaseEvent(event);
}

bool ChatView::scrollWithKey(QKeyEvent *event) {
    if (event->modifiers() != Qt::NoModifier) return false;
    QAbstractSlider::SliderAction action;
    switch (event->key()) {
        case Qt::Key_Up: action = QAbstractSlider::SliderSingleStepSub; break;
        case Qt::Key_Down: action = QAbstractSlider::SliderSingleStepAdd; break;
        case Qt::Key_PageUp: action = QAbstractSlider::SliderPageStepSub; break;
        case Qt::Key_PageDown: action = QAbstractSlider::SliderPageStepAdd; break;
        case Qt::Key_End: action = QAbstractSlider::SliderToMaximum; break;
        default: return false;
    }
    if (event->key() == Qt::Key_End) emit latestRequested();
    auto* bar = verticalScrollBar();
    const int before = bar->value();
    bar->triggerAction(action);
    GUI_DEBUG("history keyboard scroll: key=%d action=%d position=%d->%d range=%d..%d",
              event->key(), int(action), before, bar->value(), bar->minimum(), bar->maximum());
    event->accept();
    return true;
}

bool ChatView::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::KeyPress && isVisible() && isEnabled() && scrollWithKey(static_cast<QKeyEvent*>(event))) return true;
    return QListView::eventFilter(object, event);
}

void ChatView::keyPressEvent(QKeyEvent *event) {
    if (scrollWithKey(event)) return;
    if (event == QKeySequence::Copy) {
        copySelection();
        return;
    }
    QListView::keyPressEvent(event);
}

const ChatTextSelection& ChatView::textSelection() const {
    m_textSel.startChar = m_textSel.endChar = -1;
    const auto* delegate = dynamic_cast<const ChatSelectionDelegate*>(itemDelegate());
    if (delegate && hasSelection() && selStart() == selEnd() && m_textSel.active &&
        m_textSel.source == m_textSel.index.data(MsgTextRole).toString()) {
        QStyleOptionViewItem option; option.initFrom(this); option.font = font(); option.widget = this;
        option.rect = visualRect(m_textSel.index);
        const auto range = delegate->selectionRange(option, m_textSel.index, m_textSel.clickPos, m_textSel.dragPos);
        m_textSel.startChar = range.first; m_textSel.endChar = range.second;
    }
    return m_textSel;
}

QString ChatView::selectedText(const QModelIndex& index) const {
    if (!index.isValid() || selStart() != index || selStart() != selEnd()) return {};
    const auto& selection = textSelection();
    if (selection.startChar < 0 || selection.endChar <= selection.startChar) return {};
    const auto formatted = formattedMessageText(index);
    const QString text = formatted ? formatted->text : index.data(MsgTextRole).toString();
    return text.mid(selection.startChar, selection.endChar - selection.startChar);
}

void ChatView::copySelection() {
    if (!hasSelection()) return;
    auto *m = model();
    if (!m) return;

    if (selStart() == selEnd()) {
        QModelIndex idx(selStart());
        const auto formatted = formattedMessageText(idx);
        QString text = formatted ? formatted->text : idx.data(MsgTextRole).toString();
        const QString fragment = selectedText(idx);
        if (!fragment.isEmpty()) text = fragment;
        QApplication::clipboard()->clear(QClipboard::Clipboard);
        QApplication::clipboard()->setText(text);
    } else {
        QString r;
        int a = selStart().row(), b = selEnd().row();
        if (a > b) qSwap(a, b);
        for (int i = a; i <= b; ++i) {
            QModelIndex idx = m->index(i, 0);
            if (!r.isEmpty()) r += '\n';
            r += '[' + idx.data(MsgTimeRole).toString() + "] ";
            r += idx.data(MsgAuthorRole).toString() + ": ";
            const auto formatted = formattedMessageText(idx);
            r += formatted ? formatted->text : idx.data(MsgTextRole).toString();
        }
        QApplication::clipboard()->clear(QClipboard::Clipboard);
        QApplication::clipboard()->setText(r);
    }
}

void ChatView::updateScrollDownBtn() {
    QScrollBar* sb = verticalScrollBar();
    bool atBottom = sb && (sb->value() >= sb->maximum());
    m_scrollDownBtn->setVisible(m_historicalWindow || !atBottom);
    if (!atBottom) {
        QRect vr = viewport()->geometry();
        m_scrollDownBtn->move(vr.right() - 44, vr.bottom() - 44);
        m_scrollDownBtn->raise();
    }
}
