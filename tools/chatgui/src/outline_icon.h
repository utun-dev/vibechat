/* Общий контурный стиль кнопок: геометрия в координатах 24×24, круглые концы линий и размеры для HiDPI.
 * Вызывать в GUI-потоке; path задаёт контур, color — цвет, logicalSize > 0 — размер значка в логических пикселях.
 * Возвращает самостоятельный QIcon с растровыми вариантами 1x..4x; отсутствует зависимость от шрифтов ОС. */
#pragma once

#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

/* Общий контур микрофона для ввода и звонка; muted добавляет диагональное перечёркивание. */
inline QPainterPath microphoneOutline(bool muted = false) {
    QPainterPath path;
    path.addRoundedRect(QRectF(9, 2, 6, 12), 3, 3);
    path.moveTo(5, 10); path.lineTo(5, 12);
    path.cubicTo(5, 15.866, 8.134, 19, 12, 19); path.cubicTo(15.866, 19, 19, 15.866, 19, 12); path.lineTo(19, 10);
    path.moveTo(12, 19); path.lineTo(12, 22); path.moveTo(8, 22); path.lineTo(16, 22);
    if (muted) { path.moveTo(3, 3); path.lineTo(21, 21); }
    return path;
}

/* Рисует path в цвете color; logicalSize — размер кнопочного значка без учёта масштаба экрана. */
inline QIcon outlineIcon(const QPainterPath& path, const QColor& color, int logicalSize = 20) {
    QIcon icon;
    for (int scale : {1, 2, 3, 4}) {
        const int size = logicalSize * scale;
        QPixmap pixmap(size, size); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path); painter.end(); icon.addPixmap(pixmap);
    }
    return icon;
}

/* Наклонная скрепка без зависимости от системного emoji-шрифта. */
inline QIcon attachmentIcon(const QPalette& palette) {
    QPainterPath path;
    path.moveTo(16.5, 6.5); path.lineTo(16.5, 17.5);
    path.cubicTo(16.5, 20, 14.5, 22, 12, 22); path.cubicTo(9.5, 22, 7.5, 20, 7.5, 17.5);
    path.lineTo(7.5, 5); path.cubicTo(7.5, 3.35, 8.85, 2, 10.5, 2); path.cubicTo(12.15, 2, 13.5, 3.35, 13.5, 5);
    path.lineTo(13.5, 17); path.cubicTo(13.5, 17.83, 12.83, 18.5, 12, 18.5);
    path.cubicTo(11.17, 18.5, 10.5, 17.83, 10.5, 17); path.lineTo(10.5, 6.5);
    QTransform rotation; rotation.translate(12, 12); rotation.rotate(35); rotation.translate(-12, -12);
    return outlineIcon(rotation.map(path), palette.color(QPalette::ButtonText));
}

/* Смайлик с короткой улыбкой и свободным отступом от внешнего круга. */
inline QIcon smileIcon(const QPalette& palette) {
    QPainterPath path;
    path.addEllipse(QRectF(2, 2, 20, 20));
    path.moveTo(8.5, 9); path.lineTo(8.5, 9.15);
    path.moveTo(15.5, 9); path.lineTo(15.5, 9.15);
    path.moveTo(8, 14); path.cubicTo(10, 17.2, 14, 17.2, 16, 14);
    return outlineIcon(path, palette.color(QPalette::ButtonText));
}
