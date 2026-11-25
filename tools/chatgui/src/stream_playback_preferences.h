/* Общая настройка просмотра: GUI владеет переключателями, workers читают atomic. Сохранение — штатный ui_state. */
#pragma once
#include <QCoreApplication>
#include <QCheckBox>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QSignalBlocker>
#include <vector>
#include <atomic>
#include <functional>
#include "../transport/gui_bridge.h"
#include "../../../lib/debug_config.h"
inline std::function<bool(bool)> streamModeSave; // Устанавливается владельцем профиля, GUI-поток.
inline std::atomic<bool> streamLowLatency{false};
inline std::vector<QPointer<QCheckBox>> streamModeButtons;
inline void streamPlaybackModeChanged(bool low, bool save = true) {
    streamLowLatency = low;
    for (auto it = streamModeButtons.begin(); it != streamModeButtons.end();) {
        if (!*it) { it = streamModeButtons.erase(it); continue; }
        QSignalBlocker block(it->data()); (*it)->setChecked(low); ++it;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "viewer policy low_latency=%d shared=1", low);
    if (save && streamModeSave && !streamModeSave(low))
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer policy persistence request failed");
}

// Тумблер с молнией: положение бегунка и цвет дорожки показывают режим. Клавиатуру и доступность обслуживает QCheckBox.
class StreamLatencySwitch : public QCheckBox {
public:
    explicit StreamLatencySwitch(QWidget* parent) : QCheckBox(QCoreApplication::translate("ChatGui", "Low latency"), parent) {
        setFixedSize(50, 32); setCursor(Qt::PointingHandCursor);
        setObjectName(QStringLiteral("streamLowLatency")); setAccessibleName(text());
        setAccessibleDescription(QCoreApplication::translate("ChatGui", "Off: Low glitches (+0.5 s, 30 s history). Applies to all viewers."));
        setToolTip(text() + QLatin1Char('\n') + accessibleDescription());
    }
protected:
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const QRectF track(3, 5, width() - 6, height() - 10);
        const bool on = isChecked();
        painter.setPen(Qt::NoPen); painter.setBrush(on ? palette().highlight() : palette().mid());
        painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);
        const bool right = on != (layoutDirection() == Qt::RightToLeft);
        const QPointF center(right ? track.right() - track.height() / 2 : track.left() + track.height() / 2, track.center().y());
        painter.save(); painter.translate(center);
        painter.setBrush(on ? palette().highlightedText() : palette().window());
        painter.drawEllipse(QPointF(), 9, 9);
        QPolygonF bolt;
        bolt << QPointF(2, -6) << QPointF(-4, 1) << QPointF(-0.5, 1) << QPointF(-2, 6) << QPointF(4, -1) << QPointF(0.5, -1);
        painter.setBrush(on ? palette().highlight() : palette().windowText()); painter.drawPolygon(bolt);
        painter.restore();
        if (hasFocus()) {
            QStyleOptionFocusRect focus; focus.initFrom(this); focus.rect = rect().adjusted(1, 1, -1, -1);
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
        }
    }
};

// Создаёт у parent переключатель с текущим общим режимом и подключает сохранение по действию пользователя.
inline QCheckBox* streamPlaybackSwitch(QWidget* parent) {
    auto* button = new StreamLatencySwitch(parent); button->setChecked(streamLowLatency.load());
    streamModeButtons.push_back(button);
    QObject::connect(button, &QCheckBox::toggled, button, [](bool low) { streamPlaybackModeChanged(low); });
    return button;
}
