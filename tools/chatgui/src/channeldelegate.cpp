#include "flagpainter.h"
#include "channeldelegate.h"
#include "gui_theme.h"
#include "debug_ui.h"
#include <QPainter>
#include <QApplication>
#include <QMouseEvent>

// Правый индикатор рации, как в строке участника; узкая строка сохраняет место аватару.
QRect ChannelDelegate::radioRect(const QStyleOptionViewItem& option, const QModelIndex& index) {
    if (option.rect.width() < 90 || index.data(ChannelIsSectionRole).toBool() ||
        index.data(ChannelChannelIdRole).toString().startsWith(QStringLiteral("dm:")) ||
        !index.data(ChannelRadioActiveRole).toBool()) return {};
    return QRect(option.rect.right() - 29, option.rect.center().y() - 12, 24, 24);
}

ChannelDelegate::ChannelDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "ChannelDelegate created: row=56 avatar=32 bounded single-line preview");
}

void ChannelDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const {
    painter->save();
    painter->setClipRect(option.rect, Qt::IntersectClip);

    /* ── заголовок секции (например, «Личные») ── */
    if (index.data(ChannelIsSectionRole).toBool()) {
        painter->fillRect(option.rect, option.palette.window());
        painter->setPen(guiSecondaryText(option.palette));
        QFont f = option.font;
        f.setBold(true);
        painter->setFont(f);
        QRect tr = option.rect.adjusted(12, 0, -8, 0);
        painter->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(ChannelNameRole).toString());
        painter->restore();
        return;
    }

    const int barWidth = 4;
    const int margin = 8;
    const int iconSize = 32;

    bool isDm = index.data(ChannelChannelIdRole).toString().startsWith(QStringLiteral("dm:"));
    bool dmOnline = index.data(ChannelDmOnlineRole).toBool();
    int peersOnline = index.data(ChannelPeersOnlineRole).toInt();
    int autoActive  = index.data(ChannelAutoConnectActiveRole).toInt();

    /* ── background ── */
    if (option.state & QStyle::State_Selected) {
        painter->fillRect(option.rect, option.palette.highlight());
    } else if (option.state & QStyle::State_MouseOver) {
        painter->fillRect(option.rect, option.palette.light());
    } else {
        painter->fillRect(option.rect, option.palette.base());
    }

    /* ── vertical status bar (left edge, over background) ── */
    QColor barColor = Qt::transparent;
    if (peersOnline > 0)                      barColor = QColor("#4CAF50");
    else if (autoActive && peersOnline == 0)  barColor = QColor("#FFC107");
    if (barColor.alpha() > 0) {
        QRect barRect(option.rect.left(), option.rect.top(), barWidth, option.rect.height());
        painter->fillRect(barRect, barColor);
    }

    QRect iconRect(option.rect.left() + margin + barWidth,
                   option.rect.top() + (option.rect.height() - iconSize) / 2,
                   iconSize, iconSize);

    QIcon icon = qvariant_cast<QIcon>(index.data(ChannelIconRole));
    if (!icon.isNull()) {
        icon.paint(painter, iconRect);
    }

    /* ── online count badge (top-left of avatar, above it) ── */
    if (isDm || peersOnline > 0) {
        int badgeSize = 16;
        QRect badgeRect(iconRect.left() - 4, iconRect.top() - 6, badgeSize, badgeSize);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setBrush(QColor(isDm && !dmOnline ? "#b42332" : "#226d38"));
        painter->setPen(QPen(Qt::white, 1.5));
        painter->drawEllipse(badgeRect);
        QFont badgeFont = option.font;
        badgeFont.setBold(true);
        badgeFont.setPixelSize(10);
        painter->setFont(badgeFont);
        painter->setPen(Qt::white);
        QString num = QString::number(peersOnline);
        if (peersOnline > 9) num = "9+";
        if (!isDm) painter->drawText(badgeRect, Qt::AlignCenter, num);
    }

    QRect textRect(iconRect.right() + margin, option.rect.top() + margin,
                   qMax(0, option.rect.right() - iconRect.right() - 2 * margin),
                   qMax(0, option.rect.height() - 2 * margin));
    const QRect radio = radioRect(option, index);
    if (!radio.isEmpty()) {
        FlagPainter::drawRadio(painter, radio.left(), radio.top(), radio.width(), QColor(index.data(ChannelRadioTransmittingRole).toBool() ? "#4CAF50" : "#9E9E9E"));
        textRect.setRight(qMax(textRect.left() - 1, radio.left() - margin));
    }
    const QRect clipboard = clipboardRect(option, index);
    if (!clipboard.isEmpty()) {
        textRect.setRight(qMax(textRect.left() - 1, clipboard.left() - margin));
        const bool label = clipboard.width() >= 40;
        const QRect image(clipboard.center().x() - 12, label ? clipboard.top() : clipboard.center().y() - 12, 24, 24);
        qvariant_cast<QIcon>(index.data(ChannelClipboardIconRole)).paint(painter, image);
        if (label) {
            QFont font = option.font; font.setPixelSize(10); painter->setFont(font);
            painter->setPen(guiSecondaryText(option.palette, option.state & QStyle::State_Selected));
            painter->drawText(QRect(clipboard.left(), image.bottom() + 2, clipboard.width(), clipboard.height() - 26),
                Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
                QFontMetrics(font).elidedText(index.data(ChannelClipboardSenderRole).toString(), Qt::ElideRight, clipboard.width()));
        }
    }

    QString name = index.data(ChannelNameRole).toString();
    QFont nameFont = option.font;
    nameFont.setBold(true);
    if (nameFont.pointSizeF() > 0) nameFont.setPointSizeF(nameFont.pointSizeF() + 1);
    else nameFont.setPixelSize(nameFont.pixelSize() + 1);
    painter->setFont(nameFont);

    QColor textColor = (option.state & QStyle::State_Selected)
                       ? option.palette.highlightedText().color()
                       : option.palette.text().color();
    painter->setPen(textColor);

    QRect nameRect = textRect;
    nameRect.setHeight(QFontMetrics(nameFont).height());
    int unread = index.data(ChannelUnreadCountRole).toInt();
    QString unreadText = unread > 99 ? QStringLiteral("99+") : QString::number(unread);
    QFont unreadFont = option.font; unreadFont.setBold(true); unreadFont.setPixelSize(11);
    int unreadWidth = qMax(18, QFontMetrics(unreadFont).horizontalAdvance(unreadText) + 8);
    if (unread > 0) nameRect.setWidth(qMax(0, nameRect.width() - unreadWidth - 6));
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine,
                      painter->fontMetrics().elidedText(name, Qt::ElideRight, nameRect.width()));

    QString lastMsg = index.data(ChannelLastMessageRole).toString();
    if (!lastMsg.isEmpty() && textRect.width() > 0) {
        QFont msgFont = option.font;
        if (msgFont.pointSizeF() > 0) msgFont.setPointSizeF(qMax(1.0, msgFont.pointSizeF() - 1));
        else msgFont.setPixelSize(qMax(1, msgFont.pixelSize() - 1));
        QColor nickColor = option.palette.link().color();
        const bool selected = option.state & QStyle::State_Selected;
        const QColor msgColor = guiSecondaryText(option.palette, selected);
        if (selected) nickColor = option.palette.highlightedText().color();

        QRect msgRect(textRect.left(), nameRect.bottom() + 4,
                      textRect.width(),
                      qMax(0, option.rect.bottom() - nameRect.bottom() - margin));
        QFontMetrics msgFm(msgFont);

        int colon = lastMsg.indexOf(QString::fromUtf8(": "));
        if (colon >= 0) {
            QString nick = msgFm.elidedText(lastMsg.left(colon + 1), Qt::ElideRight, msgRect.width());
            QString body = lastMsg.mid(colon + 2);

            painter->setFont(msgFont);
            painter->setPen(nickColor);
            QRect nickRect = msgRect;
            nickRect.setWidth(qMin(msgRect.width(), msgFm.horizontalAdvance(nick)));
            painter->drawText(nickRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, nick);

            painter->setPen(msgColor);
            QRect bodyRect(msgRect.left() + nickRect.width(), msgRect.top(),
                           msgRect.width() - nickRect.width(), msgRect.height());
            QString elidedBody = msgFm.elidedText(body, Qt::ElideRight, bodyRect.width());
            painter->drawText(bodyRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, elidedBody);
        } else {
            painter->setFont(msgFont);
            painter->setPen(msgColor);
            QString elided = msgFm.elidedText(lastMsg, Qt::ElideRight, msgRect.width());
            painter->drawText(msgRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, elided);
        }
    }

    /* ── unread count badge (right of channel name) ── */
    if (unread > 0) {
        int badgeH = 18;
        QRect badgeRect(textRect.right() - unreadWidth, nameRect.top(), unreadWidth, badgeH);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setBrush(QColor("#E53935"));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(badgeRect, badgeH / 2, badgeH / 2);
        painter->setFont(unreadFont);
        painter->setPen(Qt::white);
        painter->drawText(badgeRect, Qt::AlignCenter, unreadText);
    }

    painter->restore();
}

QRect ChannelDelegate::clipboardRect(const QStyleOptionViewItem& option, const QModelIndex& index) {
    if (index.data(ChannelIsSectionRole).toBool() || qvariant_cast<QIcon>(index.data(ChannelClipboardIconRole)).isNull()) return {};
    QFont font = option.font; font.setPixelSize(10);
    const int width = option.rect.width() >= 240 ? qBound(40, QFontMetrics(font).horizontalAdvance(index.data(ChannelClipboardSenderRole).toString()) + 8, 96) : 28;
    const int radioWidth = radioRect(option, index).isEmpty() ? 0 : 30;
    return QRect(option.rect.right() - width - 5 - radioWidth, option.rect.top() + 6, width, option.rect.height() - 12);
}

bool ChannelDelegate::editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) {
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && clipboardRect(option, index).contains(mouse->pos())) {
            if (event->type() == QEvent::MouseButtonRelease) emit clipboardClicked(index.data(ChannelChannelIdRole).toString());
            return true;
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

QSize ChannelDelegate::sizeHint(const QStyleOptionViewItem &option,
                                const QModelIndex &index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    return QSize(200, 56);
}
