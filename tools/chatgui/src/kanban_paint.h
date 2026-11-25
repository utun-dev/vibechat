/* Общая геометрия карточек: 2px-контур и замкнутый пунктир 8px с зазором около 6px.
 * Зазор qreal вычисляется из длины пути; стык лежит в середине зазора. */
#pragma once
#include <QtMath>
#include <QPainter>
#include <QPainterPath>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <cmath>
#include <QDateTime>
#include <QCoreApplication>

inline QRectF kanbanBubble(const QRectF& slot) { return slot.adjusted(6, 6, -6, -6); }
inline QPainterPath kanbanRounded(const QRectF& rect, qreal radius = 6) {
    QPainterPath path; path.addRoundedRect(rect, radius, radius); return path;
}
// Половины двух контуров по 1px плюс свободный зазор 3px между ними.
inline QPainterPath kanbanSelection(const QRectF& bubble) { return kanbanRounded(bubble.adjusted(-5, -5, 5, 5), 11); }
// Фон пузыря: 90% обычного фона темы и 10% цвета контура.
inline QColor kanbanFill(const QPalette& palette, const QColor& color) {
    QColor base = palette.alternateBase().color(), tint = color.isValid() ? color : palette.mid().color();
    return QColor::fromRgbF(base.redF() * 0.9 + tint.redF() * 0.1, base.greenF() * 0.9 + tint.greenF() * 0.1,
        base.blueF() * 0.9 + tint.blueF() * 0.1);
}
struct KanbanDash { int repeats; qreal gap; };
inline KanbanDash kanbanDash(const QPainterPath& path) {
    qreal length = path.length(); int repeats = qMax(1, qRound(length / 14.0));
    return {repeats, (length - repeats * 8.0) / repeats};
}
inline void kanbanDrawDash(QPainter& painter, const QPainterPath& path, const QColor& color) {
    QPen pen(color, 2); pen.setCapStyle(Qt::FlatCap);
    KanbanDash dash = kanbanDash(path);
    if (dash.gap > 0) {
        pen.setDashPattern({4.0, dash.gap / 2.0}); pen.setDashOffset(4.0 + dash.gap / 4.0);
    }
    painter.setPen(pen); painter.setBrush(Qt::NoBrush); painter.drawPath(path);
}
inline void kanbanTextDocument(QTextDocument& doc, const QString& text, const QFont& font, qreal width) {
    doc.setDefaultFont(font); doc.setDocumentMargin(0); doc.setPlainText(text);
    QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); doc.setDefaultTextOption(option);
    doc.setTextWidth(qMax(1.0, width));
}
inline int kanbanTextHeight(const QString& text, const QFont& font, qreal width) {
    QTextDocument doc; kanbanTextDocument(doc, text, font, width); return qCeil(doc.size().height());
}
inline void kanbanDrawText(QPainter& painter, const QString& text, const QFont& font, const QPalette& palette, const QRectF& rect) {
    QTextDocument doc; kanbanTextDocument(doc, text, font, rect.width());
    QAbstractTextDocumentLayout::PaintContext context; context.palette = palette;
    painter.save(); painter.translate(rect.topLeft()); doc.documentLayout()->draw(&painter, context); painter.restore();
}

// Срок хранится в микросекундах Unix; округление вверх не показывает «0 ч» до истечения срока.
inline QString kanbanDueText(qint64 due, qint64 now) {
    if (due <= 0) return {};
    const qint64 delta = due / 1000 - now, magnitude = qAbs(delta);
    const qint64 hours = (magnitude + 3600000 - 1) / 3600000;
    const qint64 days = hours / 24, remainder = hours % 24;
    QString text;
    if (days) text = QCoreApplication::translate("ChatGui", "%1 d").arg(days);
    if (remainder || !days) {
        if (!text.isEmpty()) text += QLatin1Char(' ');
        text += QCoreApplication::translate("ChatGui", "%1 h").arg(remainder);
    }
    return delta < 0 ? QStringLiteral("−") + text : text;
}
// Место под нижнюю строку и зазор до основного текста.
inline int kanbanFooterHeight(const QFont& font) { return qMax(22, QFontMetrics(font).height()) + 6; }
