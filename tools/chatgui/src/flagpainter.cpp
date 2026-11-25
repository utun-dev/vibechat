#include "flagpainter.h"
#include <QPainterPath>
#include <cmath>

void FlagPainter::draw(QPainter* p, uint8_t flags, int x, int y, int sz) {
    if (!flags) return;
    int cx = x;
    if (flags & MEMBER_FLAG_ADMIN)     { drawStar(p, cx, y, sz);      cx += sz + 2; }
    if (flags & MEMBER_FLAG_MODER)     { drawShield(p, cx, y, sz);    cx += sz + 2; }
}

// Три блока серверной стойки; масштабируется вместе с аватаром.
void FlagPainter::drawServer(QPainter* p, int x, int y, int sz) {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->translate(x, y);
    p->scale(sz / 24.0, sz / 24.0);
    for (int top : {2, 9, 16}) {
        p->setPen(QPen(QColor("#37474F"), 0.8)); p->setBrush(QColor("#607D8B"));
        p->drawRoundedRect(QRectF(2, top, 20, 5.5), 1, 1);
        p->setPen(QPen(QColor("#ECEFF1"), 1.2, Qt::SolidLine, Qt::RoundCap));
        p->drawLine(QPointF(5, top + 2.75), QPointF(11, top + 2.75));
        p->setPen(Qt::NoPen); p->setBrush(QColor("#B0BEC5"));
        p->drawEllipse(QPointF(18, top + 2.75), 1, 1);
    }
    p->restore();
}

void FlagPainter::drawStar(QPainter* p, int x, int y, int sz) {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    qreal cx = x + sz / 2.0, cy = y + sz / 2.0, r = sz / 2.0 - 1.0;
    for (int i = 0; i < 10; i++) {
        qreal a = (i * 36.0 - 90.0) * 3.14159265 / 180.0;
        qreal rr = (i & 1) ? r * 0.42 : r;
        qreal px = cx + rr * cos(a), py = cy + rr * sin(a);
        if (i == 0) path.moveTo(px, py);
        else path.lineTo(px, py);
    }
    path.closeSubpath();
    p->setPen(QPen(QColor("#E65100"), 0.5));
    p->setBrush(QColor("#FFD700"));
    p->drawPath(path);
    p->restore();
}

void FlagPainter::drawShield(QPainter* p, int x, int y, int sz) {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    qreal cx = x + sz / 2.0, t = y + 1.0, b = y + sz - 1.0;
    qreal w = sz * 0.38;
    qreal notch = sz * 0.22;
    path.moveTo(cx - w, t);
    path.lineTo(cx - w, t + notch);
    path.lineTo(cx - w * 0.3, t + notch + 1);
    path.lineTo(cx, b);
    path.lineTo(cx + w * 0.3, t + notch + 1);
    path.lineTo(cx + w, t + notch);
    path.lineTo(cx + w, t);
    path.closeSubpath();
    p->setPen(QPen(QColor("#1565C0"), 0.8));
    p->setBrush(QColor("#2196F3"));
    p->drawPath(path);
    p->restore();
}

// Цилиндр хранилища: верхний эллипс и дуги обручей целиком помещаются внутри значка.
void FlagPainter::drawStorage(QPainter* p, int x, int y, int sz) {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->translate(x, y);
    p->scale(sz / 24.0, sz / 24.0);
    QPainterPath body;
    body.moveTo(3, 5); body.lineTo(3, 19);
    body.cubicTo(3, 23, 21, 23, 21, 19);
    body.lineTo(21, 5); body.closeSubpath();
    p->setPen(QPen(QColor("#37474F"), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(QColor("#78909C"));
    p->drawPath(body);
    p->setBrush(Qt::NoBrush);
    for (int level : {10, 15}) {
        QPainterPath hoop;
        hoop.moveTo(3, level); hoop.cubicTo(3, level + 4, 21, level + 4, 21, level);
        p->drawPath(hoop);
    }
    p->setBrush(QColor("#CFD8DC"));
    p->drawEllipse(QRectF(3, 2, 18, 6));
    p->restore();
}
