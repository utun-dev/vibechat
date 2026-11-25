/* Чистая отрисовка значков участника/storage/рации, без владения painter/paint device.
 * p — ненулевой активный QPainter; x/y — верхний левый угол, sz>0 — размер в координатах painter.
 * Каждый рисунок сохраняет/восстанавливает состояние painter, включая transform/pen/brush.
 * Состояние участников не меняется; синхронизацию совместного paint device обеспечивает вызывающий. */
#ifndef FLAGPAINTER_H
#define FLAGPAINTER_H

#include <QPainter>
#include <QColor>
#include <stdint.h>

// Значения совпадают с CHAT_MEMBER_FLAG_* (src/chat/chat_member.h).
#define MEMBER_FLAG_SUPERNODE 0x01 // сервер вместо аватара; draw игнорирует
#define MEMBER_FLAG_ADMIN     0x02 // звезда
#define MEMBER_FLAG_MODER     0x04 // щит
#define MEMBER_FLAG_DELETED   0x08 // draw игнорирует; делегат отдельно зачёркивает имя
#define MEMBER_FLAG_RADIO     0x10 // draw игнорирует; делегат отдельно выбирает цвет рации

class FlagPainter {
public:
    // ADMIN -> MODER, только установленные биты, с промежутком 2 между квадратами sz×sz.
    // Общая ширина при n>0: n*sz+(n-1)*2; flags без этих битов не рисует ничего.
    static void draw(QPainter* p, uint8_t flags, int x, int y, int sz);
    static void drawServer(QPainter* p, int x, int y, int sz); // серверная стойка в квадрате sz×sz
    static void drawStorage(QPainter* p, int x, int y, int sz); // цилиндр с обручами, целиком внутри квадрата sz×sz
    // Контур рации заданного цвета, без заливки; общий для участников и списка каналов.
    static void drawRadio(QPainter* p, int x, int y, int sz, const QColor& color) {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->translate(x, y);
        p->scale(sz / 24.0, sz / 24.0);
        p->setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        p->drawLine(QPointF(8.5, 1), QPointF(8.5, 9));
        p->drawLine(QPointF(14.5, 7), QPointF(14.5, 9));
        p->drawRoundedRect(QRectF(7, 9, 10, 13), 2, 2);
        p->drawRoundedRect(QRectF(9.5, 11.5, 5, 3), 0.5, 0.5);
        for (int yLine : {16, 18, 20}) p->drawLine(QPointF(9.5, yLine), QPointF(14.5, yLine));
        p->restore();
    }

private:
    static void drawStar(QPainter* p, int x, int y, int sz);
    static void drawShield(QPainter* p, int x, int y, int sz);
};

#endif
