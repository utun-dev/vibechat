/* Визуальный drag-preview карточек: модель остаётся неизменной до drop.
 * previewDrag задаёт переносимую карточку всем колонкам; hover убирает чужие placeholders.
 * QVariantAnimation плавно сдвигает нарисованные карточки, submit вызывается только при drop. */
#pragma once
#include "kanban_paint.h"
#include <QApplication>
#include <QListWidget>
#include <QMouseEvent>
#include <QVariantAnimation>
#include <QTimer>
#include <QScrollBar>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QDrag>
#include <QDropEvent>
#include <QMimeData>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <functional>
#include <QJsonArray>
#include <QIcon>
static constexpr const char* cardMime = "application/x-utun-kanban-card";
class CardList final : public QListWidget {
public:
    QString group, column;
    bool writable = false;
    std::function<QIcon(const QString&)> avatar; // Готовый аватар из GUI-модели участников; пустой заменяется инициалом ID.
    QListWidgetItem* inlineItem = nullptr; // Заимствованная строка текущего редактора, модель не переставляется.
    std::function<void(QListWidgetItem*, QPoint)> activate;
    bool selectedAtPress() const { return m_pressedSelected; } // Выделение до press отличает выбор карточки от повторного клика.
    std::function<void()> create;
    std::function<void(bool)> pointerDown;
    QRect newCardRect() const {
        int top = count() ? visualItemRect(item(count() - 1)).bottom() + spacing() + 1 : spacing();
        return QRect(spacing(), top, qMax(0, viewport()->width() - spacing() * 2), 64);
    }
    std::function<void(QJsonObject)> submit;
    std::function<void(bool)> dragging;
    std::function<void(CardList*)> hover;
    std::function<void(QPoint)> edge;
    explicit CardList(QWidget* parent) : QListWidget(parent), m_animation(new QVariantAnimation(this)) {
        setDragEnabled(true); setAcceptDrops(true); setDropIndicatorShown(false); setDragDropMode(DragDrop);
        setWordWrap(true); setSpacing(5); setMinimumWidth(200); setContextMenuPolicy(Qt::CustomContextMenu);
        setFrameShape(QFrame::NoFrame); setMouseTracking(true); viewport()->setMouseTracking(true);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setEditTriggers(QAbstractItemView::NoEditTriggers); setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_animation->setObjectName("kanbanPlacement");
        m_animation->setDuration(180); m_animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            qreal progress = value.toReal();
            for (auto it = m_targets.cbegin(); it != m_targets.cend(); ++it)
                m_offsets[it.key()] = m_from.value(it.key()) + (it.value() - m_from.value(it.key())) * progress;
            viewport()->update();
        });
        auto* clock = new QTimer(this); clock->setInterval(60000);
        connect(clock, &QTimer::timeout, viewport(), QOverload<>::of(&QWidget::update)); clock->start();
        m_timer = new QTimer(this); m_timer->setInterval(40);
        connect(m_timer, &QTimer::timeout, this, [this] {
            QPoint point = viewport()->mapFromGlobal(QCursor::pos());
            if (!viewport()->rect().adjusted(-25, -25, 25, 25).contains(point)) return;
            if (point.y() < 30) verticalScrollBar()->setValue(verticalScrollBar()->value() - 12);
            if (point.y() > viewport()->height() - 30) verticalScrollBar()->setValue(verticalScrollBar()->value() + 12);
            if (edge) edge(QCursor::pos());
            previewAt(point);
        });
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { if (!m_updating) targets(); });
    }
    void previewDrag(const QString& id, int height) {
        bool starting = m_dragged.isEmpty() && !id.isEmpty();
        m_restoreMaximum = !m_dragged.isEmpty() && id.isEmpty();
        if (starting) m_originalMaximum = verticalScrollBar()->maximum();
        m_newHover = false; m_dragged = id; m_gapHeight = height + spacing() * 2; m_hover = false; m_before.clear();
        if (starting) for (int i = 0; i < count(); i++) if (item(i)->data(Qt::UserRole).toString() == id) {
            m_hover = true; if (i + 1 < count()) m_before = item(i + 1)->data(Qt::UserRole).toString(); break;
        }
        if (id.isEmpty()) m_timer->stop();
        targets();
    }
    void leavePreview() { m_hover = false; m_before.clear(); targets(); }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QListWidget::resizeEvent(event);
        int width = qMax(80, viewport()->width() - 2 * spacing() - 30);
        for (int i = 0; i < count(); i++) {
            auto* card = item(i);
            if (card == inlineItem) continue;
            int height = kanbanTextHeight(card->text(), font(), width) + 26 + kanbanFooterHeight(font());
            card->setSizeHint(QSize(width + 32, qMax(56, height)));
        }
    }
    void updateGeometries() override {
        if (m_geometries) return;
        m_geometries = true; QListWidget::updateGeometries();
        if (writable && !inlineItem && m_dragged.isEmpty()) {
            int bottom = newCardRect().bottom() + verticalScrollBar()->value() + spacing() + 1;
            verticalScrollBar()->setMaximum(qMax(verticalScrollBar()->maximum(), bottom - viewport()->height()));
        }
        m_geometries = false;
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        m_newHover = writable && !inlineItem && m_dragged.isEmpty() && event->pos().y() >= newCardRect().top();
        viewport()->setCursor(m_newHover ? Qt::PointingHandCursor : Qt::ArrowCursor); viewport()->update();
        QListWidget::mouseMoveEvent(event);
    }
    void leaveEvent(QEvent* event) override { m_newHover = false; viewport()->update(); QListWidget::leaveEvent(event); }
    void mousePressEvent(QMouseEvent* event) override {
        m_pressed = event->pos(); m_dragHappened = false;
        auto* card = itemAt(event->pos()); m_pressedSelected = card && card->isSelected();
        if (pointerDown) pointerDown(true);
        QListWidget::mousePressEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        m_pressed = event->pos(); m_dragHappened = false;
        auto* card = itemAt(event->pos()); m_pressedSelected = card && card->isSelected();
        if (pointerDown) pointerDown(true);
        QListWidget::mouseDoubleClickEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QPointer<CardList> self(this); QListWidget::mouseReleaseEvent(event); if (!self) return;
        if (event->button() == Qt::LeftButton && !m_dragHappened &&
            (event->pos() - m_pressed).manhattanLength() < QApplication::startDragDistance()) {
            auto* card = itemAt(event->pos());
            if (card && activate && writable) activate(card, event->pos() - visualItemRect(card).topLeft());
            else if (!card && create && writable && newCardRect().contains(event->pos())) create();
        }
        if (self && pointerDown) pointerDown(false);
    }
    void startDrag(Qt::DropActions) override {
        auto* item = currentItem(); if (!item || inlineItem) return;
        m_dragHappened = true;
        QRect rect = visualItemRect(item);
        QJsonObject payload{{"group", group}, {"entity", item->data(Qt::UserRole).toString()}, {"height", rect.height()}};
        auto* mime = new QMimeData; mime->setData(cardMime, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        QPointer<QDrag> drag = new QDrag(this); drag->setMimeData(mime); drag->setPixmap(viewport()->grab(rect));
        drag->setHotSpot(QPoint(rect.width() / 2, qMin(25, rect.height() / 2)));
        QPointer<CardList> self(this); if (dragging) dragging(true);
        drag->exec(Qt::MoveAction); if (drag) delete drag.data();
        if (self && pointerDown) pointerDown(false);
        if (self && dragging) dragging(false);
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        auto payload = QJsonDocument::fromJson(event->mimeData()->data(cardMime)).object();
        if (payload["group"].toString() == group && !m_dragged.isEmpty()) {
            m_hover = true; if (hover) hover(this); targets(); event->acceptProposedAction(); m_timer->start();
        } else event->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (m_dragged.isEmpty()) { event->ignore(); return; }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        previewAt(event->position().toPoint());
#else
        previewAt(event->pos());
#endif
        event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent* event) override { m_timer->stop(); leavePreview(); event->accept(); }
    void dropEvent(QDropEvent* event) override {
        m_timer->stop(); auto payload = QJsonDocument::fromJson(event->mimeData()->data(cardMime)).object();
        if (payload["group"].toString() != group || m_dragged.isEmpty()) { event->ignore(); return; }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        previewAt(event->position().toPoint());
#else
        previewAt(event->pos());
#endif
        if (submit) submit({{"action", "card_move"}, {"entity", payload["entity"]}, {"column", column}, {"before", m_before}});
        event->acceptProposedAction();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(viewport()); painter.fillRect(viewport()->rect(), palette().window()); painter.setRenderHint(QPainter::Antialiasing);
        if (m_newHover && writable && !inlineItem && m_dragged.isEmpty())
            kanbanDrawDash(painter, kanbanRounded(kanbanBubble(newCardRect())), palette().mid().color());
        if (m_hover && !m_dragged.isEmpty())
            kanbanDrawDash(painter, kanbanRounded(kanbanBubble(m_gap)), palette().highlight().color());
        for (int i = 0; i < count(); i++) {
            auto* card = item(i); QString id = card->data(Qt::UserRole).toString(); if (id == m_dragged || card == inlineItem) continue;
            QRect rect = visualItemRect(card).translated(0, qRound(m_offsets.value(id)));
            if (!viewport()->rect().intersects(rect)) continue;
            const QRectF bubble = kanbanBubble(rect);
            QColor color(card->data(Qt::UserRole + 1).toJsonObject()["color"].toString());
            painter.setPen(QPen(color.isValid() ? color : palette().mid().color(), 2)); painter.setBrush(kanbanFill(palette(), color));
            painter.drawPath(kanbanRounded(bubble));
            if (card->isSelected()) kanbanDrawDash(painter, kanbanSelection(bubble), palette().highlight().color());
            QRectF content = bubble.adjusted(10, 7, -10, -7);
            kanbanDrawText(painter, card->text(), font(), palette(), content.adjusted(0, 0, 0, -kanbanFooterHeight(font())));
            drawFooter(painter, card->data(Qt::UserRole + 1).toJsonObject(), content);
        }
    }
private:
    void drawFooter(QPainter& painter, const QJsonObject& card, const QRectF& content) {
        const int size = 22, gap = 4;
        QFont small = font(); small.setPointSizeF(qMax(7.0, small.pointSizeF() - 1));
        QFontMetrics metrics(small); painter.save(); painter.setFont(small);
        QRectF row(content.left(), content.bottom() - qMax(size, metrics.height()), content.width(), qMax(size, metrics.height()));
        const qint64 now = QDateTime::currentMSecsSinceEpoch(), due = card["due"].toString().toLongLong();
        QString date;
        const qint64 created = card["created_at"].toVariant().toLongLong();
        if (created > 0) date = QDateTime::fromMSecsSinceEpoch(created / 1000).toString("dd.MM");
        const QString remaining = kanbanDueText(due, now);
        const int timeWidth = metrics.horizontalAdvance(date) + (remaining.isEmpty() ? 0 : gap * 2 + metrics.horizontalAdvance(remaining));
        const qreal timeLeft = row.right() - timeWidth;
        auto drawAvatar = [&](const QString& id, qreal x) {
            QRect rect(qRound(x), qRound(row.center().y() - size / 2.0), size, size);
            QIcon icon = avatar ? avatar(id) : QIcon();
            painter.save(); painter.setClipPath(kanbanRounded(rect, size / 2.0));
            if (!icon.isNull()) icon.paint(&painter, rect);
            else {
                painter.setPen(Qt::NoPen); painter.setBrush(palette().mid()); painter.drawEllipse(rect);
                painter.setPen(palette().text().color()); painter.drawText(rect, Qt::AlignCenter, id.left(1).toUpper());
            }
            painter.restore();
        };
        qreal x = row.left();
        if (!card["creator"].toString().isEmpty()) { drawAvatar(card["creator"].toString(), x); x += size + gap * 2; }
        const auto assignees = card["assignees"].toArray();
        for (int i = 0; i < assignees.size(); i++) {
            const QString overflow = QStringLiteral("+%1").arg(assignees.size() - i);
            const int reserve = i + 1 < assignees.size() ? metrics.horizontalAdvance(overflow) + gap : 0;
            if (x + size + gap + reserve > timeLeft) {
                painter.setPen(palette().text().color());
                painter.drawText(QRectF(x, row.top(), qMax(0.0, timeLeft - x - gap), row.height()), Qt::AlignVCenter, overflow); break;
            }
            drawAvatar(assignees[i].toString(), x); x += size + gap;
        }
        painter.setPen(palette().text().color());
        painter.drawText(QRectF(timeLeft, row.top(), metrics.horizontalAdvance(date), row.height()), Qt::AlignVCenter, date);
        if (!remaining.isEmpty()) {
            painter.setPen(due / 1000 < now ? QColor("#e45b5b") : palette().text().color());
            painter.drawText(QRectF(timeLeft + metrics.horizontalAdvance(date) + gap * 2, row.top(),
                metrics.horizontalAdvance(remaining), row.height()), Qt::AlignVCenter, remaining);
        }
        painter.restore();
    }
    void previewAt(QPoint point) {
        if (m_dragged.isEmpty()) return;
        if (m_hover && m_gap.contains(point)) return;
        m_hover = true; QString before;
        for (int i = 0; i < count(); i++) {
            auto* card = item(i); QString id = card->data(Qt::UserRole).toString(); if (id == m_dragged || card == inlineItem) continue;
            if (point.y() < visualItemRect(card).translated(0, qRound(m_offsets.value(id))).center().y()) { before = id; break; }
        }
        if (before != m_before) { m_before = before; targets(); } else if (m_targets.isEmpty()) targets();
    }
    void targets() {
        if (m_updating) return;
        if (m_dragged.isEmpty() && !m_restoreMaximum) { viewport()->update(); return; }
        m_updating = true;
        QHash<QString, qreal> target; int y = count() ? visualItemRect(item(0)).top() : spacing(); bool gap = false, source = false;
        for (int i = 0; i < count(); i++) {
            auto* card = item(i); QString id = card->data(Qt::UserRole).toString(); QRect rect = visualItemRect(card);
            if (!m_dragged.isEmpty() && id == m_dragged) { source = true; continue; }
            if (m_hover && !m_dragged.isEmpty() && !gap && id == m_before) {
                m_gap = QRect(0, y, viewport()->width(), m_gapHeight); y += m_gapHeight; gap = true;
            }
            target[id] = m_dragged.isEmpty() ? 0 : y - rect.top(); y += rect.height() + spacing() * 2;
        }
        if (m_hover && !m_dragged.isEmpty() && !gap) m_gap = QRect(0, y, viewport()->width(), m_gapHeight);
        if (!m_dragged.isEmpty()) verticalScrollBar()->setMaximum(qMax(0, m_originalMaximum + (m_hover ? m_gapHeight : 0) - (source ? m_gapHeight : 0)));
        else verticalScrollBar()->setMaximum(m_originalMaximum);
        m_restoreMaximum = false;
        if (target != m_targets) {
            m_animation->stop(); m_from = m_offsets; m_targets = target;
            m_animation->setStartValue(0.0); m_animation->setEndValue(1.0); m_animation->start();
        }
        viewport()->update(); m_updating = false;
    }
    QPoint m_pressed;
    bool m_newHover = false, m_dragHappened = false, m_geometries = false, m_pressedSelected = false;
    QTimer* m_timer;
    QVariantAnimation* m_animation;
    QHash<QString, qreal> m_offsets, m_from, m_targets;
    QString m_dragged, m_before;
    QRect m_gap;
    int m_gapHeight = 0, m_originalMaximum = 0;
    bool m_hover = false, m_updating = false, m_restoreMaximum = false;
};
