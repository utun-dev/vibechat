/* Общие кнопки настроек захвата для звонка и трансляции: 48x48, значок 40x36, зазор 4.
 * MenuStyle назначается QMenuBar, родитель стиля должен пережить bar и его меню.
 * Меню вариантов должно содержать один QActionGroup; data хранит значение, checked — текущий выбор.
 * select меняет выбор без triggered; addOption отмечает первый вариант, clearOptions сохраняет группу и добавляет заголовок.
 * controlIcon рисует выбранное значение внутри силуэта, detail — дополнительную короткую подпись, rotation — угол маркера. */
#pragma once
#include <QCoreApplication>
#include <QMenuBar>
#include <QMenu>
#include <QActionGroup>
#include <QProxyStyle>
#include <QStyleOptionMenuItem>
#include <QPainter>
#include <QPainterPath>
#include "stream_video_modes.h"
#include "debug_config.h"

namespace CaptureControls {
class MenuStyle : public QProxyStyle {
public:
    explicit MenuStyle(int buttonSize = 48, QSize iconSize = QSize(40, 36)) : m_buttonSize(buttonSize), m_iconSize(iconSize) {}
    int pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override {
        if (qobject_cast<const QMenuBar*>(widget)) {
            if (metric == PM_SmallIconSize) return m_iconSize.width();
            if (metric == PM_MenuBarItemSpacing) return 4;
        }
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override {
        if (type == CT_MenuBarItem && qobject_cast<const QMenuBar*>(widget)) return QSize(m_buttonSize, m_buttonSize);
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    }
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override {
        auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (element != CE_MenuBarItem || !item || !qobject_cast<const QMenuBar*>(widget)) {
            QProxyStyle::drawControl(element, option, painter, widget); return;
        }
        painter->save(); painter->setRenderHint(QPainter::Antialiasing); painter->setPen(Qt::NoPen);
        QColor background = item->palette.button().color();
        if (item->state & (State_Selected | State_Sunken)) background = item->palette.highlight().color();
        background.setAlphaF(item->state & State_Enabled ? 0.3 : 0.12); painter->setBrush(background);
        painter->drawRoundedRect(item->rect, 10, 10);
        QRect iconRect = alignedRect(item->direction, Qt::AlignCenter, m_iconSize, item->rect);
        item->icon.paint(painter, iconRect, Qt::AlignCenter, item->state & State_Enabled ? QIcon::Normal : QIcon::Disabled);
        painter->restore();
    }
private:
    int m_buttonSize; // Сторона кнопки: 48 для захвата, 34 для компактной панели рации.
    QSize m_iconSize; // Прямоугольник значка внутри кнопки.
};

inline QAction* selected(QMenu* menu) { return menu->findChild<QActionGroup*>()->checkedAction(); }
inline QVariant value(QMenu* menu) { auto* action = selected(menu); return action ? action->data() : QVariant(); }
inline QAction* option(QMenu* menu, const QVariant& data) {
    for (auto* action : menu->actions()) if (action->isCheckable() && action->data() == data) return action;
    return nullptr;
}
inline bool select(QMenu* menu, const QVariant& data) {
    auto* action = option(menu, data);
    if (action) action->setChecked(true);
    return action;
}
inline QAction* addOption(QMenu* menu, const QString& label, const QVariant& data) {
    auto* group = menu->findChild<QActionGroup*>();
    auto* action = menu->addAction(label); action->setCheckable(true); action->setData(data); group->addAction(action);
    if (!group->checkedAction()) action->setChecked(true);
    return action;
}
inline void clearOptions(QMenu* menu, const char* heading) { menu->clear(); menu->addSection(QCoreApplication::translate("ChatGui", heading)); }

/* Все стандартные и аппаратные FPS остаются в меню; доступность зависит от точного size.
 * Сохраняет previous, иначе выбирает 30 либо первый доступный; без режимов снимает выбор. */
inline void populateFrameRates(QMenu* menu, const QList<StreamVideoMode>& modes, QSize size, int previous) {
    clearOptions(menu, "Frame rate"); QList<int> rates{15, 24, 25, 30, 50, 60};
    for (const auto& mode : modes) if (!rates.contains(mode.fps)) rates.append(mode.fps);
    std::sort(rates.begin(), rates.end()); int fallback = -1;
    for (int fps : rates) {
        auto* action = addOption(menu, QCoreApplication::translate("ChatGui", "%1 fps").arg(fps), fps);
        bool available = modes.contains({size, fps}); action->setEnabled(available);
        if (available && (fallback < 0 || fps == 30)) fallback = fps;
    }
    auto* requested = option(menu, previous);
    if (requested && requested->isEnabled()) select(menu, previous);
    else if (fallback >= 0) {
        select(menu, fallback);
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "desktop capture FPS adjusted resolution=%dx%d requested=%d selected=%d",
            size.width(), size.height(), previous, fallback);
    } else {
        auto* group = menu->findChild<QActionGroup*>(); group->setExclusive(false);
        if (auto* action = selected(menu)) action->setChecked(false);
        group->setExclusive(true);
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "desktop capture resolution has no available FPS width=%d height=%d", size.width(), size.height());
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "desktop capture FPS choices width=%d height=%d choices=%lld selected=%d",
        size.width(), size.height(), (long long)rates.size(), value(menu).toInt());
}

/* Компактные векторные значки с текущим значением, читаемые в обеих темах. */
inline QIcon controlIcon(const QPalette& palette, const QString& shape, const QString& label = {}, const QString& detail = {}, int rotation = 0) {
    QPixmap pixmap(80, 72); pixmap.fill(Qt::transparent); QPainter painter(&pixmap); painter.scale(2, 2);
    painter.setRenderHint(QPainter::Antialiasing); painter.setPen(QPen(palette.windowText(), 1.6));
    QRectF labelRect(3, 5, 34, 26); int labelSize = 11;
    if (shape == "camera") {
        QPainterPath body; body.moveTo(4, 9); body.lineTo(11, 9); body.lineTo(14, 3); body.lineTo(26, 3);
        body.lineTo(29, 9); body.lineTo(36, 9); body.quadTo(38, 9, 38, 12); body.lineTo(38, 31);
        body.quadTo(38, 34, 35, 34); body.lineTo(5, 34); body.quadTo(2, 34, 2, 31); body.lineTo(2, 12); body.quadTo(2, 9, 4, 9);
        painter.drawPath(body); labelRect.moveTop(10); labelRect.setHeight(22);
    } else if (shape == "screen") {
        painter.drawRoundedRect(QRectF(1.5, 2, 37, 27), 3, 3); painter.drawLine(20, 29, 20, 34); painter.drawLine(12, 34, 28, 34);
        labelRect = QRectF(3, 3, 34, 24);
    } else if (shape == "chip") {
        painter.drawRoundedRect(QRectF(5.5, 5, 29, 26), 3, 3);
        painter.setPen(QPen(palette.highlight(), 1.5));
        for (double position : {0.28, 0.5, 0.72}) {
            painter.drawLine(QPointF(40 * position, 1), QPointF(40 * position, 5));
            painter.drawLine(QPointF(40 * position, 31), QPointF(40 * position, 35));
            painter.drawLine(QPointF(1, 36 * position), QPointF(5.5, 36 * position));
            painter.drawLine(QPointF(34.5, 36 * position), QPointF(39, 36 * position));
        }
        labelRect = QRectF(6, 5, 28, 26);
    } else if (shape == "gauge") {
        painter.setPen(QPen(palette.highlight(), 1.5)); painter.drawArc(QRectF(2, 2, 36, 32), 0, 180 * 16);
        painter.drawLine(2, 18, 6, 18); painter.drawLine(20, 2, 20, 6); painter.drawLine(34, 18, 38, 18);
        painter.drawLine(6, 34, 34, 34); labelRect = QRectF(3, 17, 34, 15); labelSize = 12;
    } else if (shape == "rotation") {
        painter.save(); painter.translate(20, 18); painter.rotate(rotation); painter.translate(-20, -18);
        QColor frame = palette.windowText().color(); frame.setAlphaF(0.6); painter.setPen(QPen(frame, 1.5));
        painter.drawRoundedRect(QRectF(8, 1.5, 24, 33), 4, 4);
        painter.setPen(Qt::NoPen); painter.setBrush(palette.highlight()); painter.drawRoundedRect(QRectF(8, 1.5, 24, 4), 2, 2);
        QPolygonF triangle; triangle << QPointF(15, 4.5) << QPointF(25, 4.5) << QPointF(20, 8.5);
        painter.drawPolygon(triangle); painter.restore(); labelSize = 10;
        labelRect.translate(rotation == 90 ? -2 : rotation == 270 ? 2 : 0, rotation == 0 ? 2 : 0);
    } else if (shape == "statistics") {
        painter.drawLine(5, 4, 5, 31); painter.drawLine(5, 31, 36, 31);
        painter.setPen(QPen(palette.highlight(), 2));
        QPainterPath graph; graph.moveTo(9, 25); graph.lineTo(17, 17); graph.lineTo(24, 21); graph.lineTo(34, 8);
        painter.drawPath(graph);
    } else if (shape == "settings") {
        painter.translate(20, 18);
        for (int i = 0; i < 8; i++) { painter.drawLine(0, -10, 0, -15); painter.rotate(45); }
        painter.drawEllipse(QPointF(), 10, 10); painter.drawEllipse(QPointF(), 4, 4);
    } else if (shape == "microphone" || shape == "microphone-muted") {
        painter.save(); painter.translate(7, 0); painter.scale(0.65, 0.65);
        painter.drawRoundedRect(QRectF(15, 3, 10, 19), 5, 5); painter.drawArc(QRectF(10, 12, 20, 16), 180 * 16, 180 * 16);
        painter.drawLine(20, 28, 20, 33); painter.drawLine(14, 33, 26, 33);
        if (shape == "microphone-muted") painter.drawLine(9, 3, 32, 28);
        painter.restore(); labelRect = QRectF(1, 23, 38, 13); labelSize = 9;
    } else if (shape == "sound" || shape == "silent" || shape == "output") {
        if (shape == "output") { painter.save(); painter.translate(7, 0); painter.scale(0.65, 0.65); }
        QPolygon speaker; speaker << QPoint(6, 14) << QPoint(13, 14) << QPoint(22, 7) << QPoint(22, 29)
            << QPoint(13, 22) << QPoint(6, 22); painter.drawPolygon(speaker);
        if (shape != "silent") painter.drawArc(QRectF(18, 6, 17, 24), -70 * 16, 140 * 16);
        else { painter.drawLine(27, 13, 35, 23); painter.drawLine(27, 23, 35, 13); }
        if (shape == "output") { painter.restore(); labelRect = QRectF(1, 23, 38, 13); labelSize = 9; }
    } else {
        painter.drawRoundedRect(QRectF(3, 5, 34, 26), 4, 4);
        if (!detail.isEmpty()) labelRect = QRectF(3, 16, 34, 15);
    }
    painter.setPen(palette.windowText().color());
    if (!label.isEmpty()) {
        QFont font = painter.font(); font.setPixelSize(labelSize); font.setBold(true); painter.setFont(font);
        painter.drawText(labelRect, Qt::AlignCenter, painter.fontMetrics().elidedText(label, Qt::ElideRight, int(labelRect.width())));
    }
    if (!detail.isEmpty()) {
        QFont font = painter.font(); font.setPixelSize(8); font.setBold(false); painter.setFont(font);
        painter.drawText(QRectF(3, 7, 34, 10), Qt::AlignCenter, detail);
    }
    return QIcon(pixmap);
}
}
