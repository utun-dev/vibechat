#include "emojitabbar.h"
#include <QPainter>

EmojiTabBar::EmojiTabBar(QWidget *parent)
    : QTabBar(parent)
{
    setDrawBase(false);
    setExpanding(false);
    setUsesScrollButtons(false);
    setCursor(Qt::PointingHandCursor);
}

QSize EmojiTabBar::tabSizeHint(int index) const {
    Q_UNUSED(index);
    return QSize(48, 32);
}

void EmojiTabBar::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    const QColor bgColor = palette().window().color();
    const QColor activeTextColor = palette().link().color();
    const QColor inactiveTextColor = palette().text().color();
    const QColor activeBarColor = palette().highlight().color();
    const QColor separatorColor = palette().mid().color();

    p.fillRect(rect(), bgColor);

    QFont f = font();
    f.setPixelSize(18);
    p.setFont(f);

    int current = currentIndex();

    for (int i = 0; i < count(); ++i) {
        QRect r = tabRect(i);
        if (!r.isValid()) continue;

        bool active = (i == current);
        if (active) {
            QColor fill = activeBarColor;
            fill.setAlpha(32);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(r.adjusted(2, 4, -2, -2), 4, 4);
        }
        p.setPen(active ? activeTextColor : inactiveTextColor);
        p.drawText(r, Qt::AlignHCenter | Qt::AlignVCenter, tabText(i));

        if (active) {
            int barX = r.left() + 4;
            int barW = r.width() - 8;
            p.fillRect(QRect(barX, 1, barW, 3), activeBarColor);
        }
    }

    p.fillRect(QRect(0, 0, w, 1), separatorColor);
}
