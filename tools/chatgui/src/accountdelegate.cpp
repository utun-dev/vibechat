#include "accountdelegate.h"
#include "gui_theme.h"
#include "flagpainter.h"
#include "memberlistmodel.h"
#include "debug_ui.h"
#include <QPainter>
#include <QFont>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QPainterPath>
#include <QCoreApplication>
#include <QAbstractItemView>

extern "C" {
#include "chat/chat_member.h"
}

static constexpr int NameOffset = GroupedMemberModel::DisclosureWidth + MemberListModel::kAvatarSize + 12;
static constexpr int MinimumNameWidth = 56; // Примерно семь символов при принятом размере шрифта 13px.
static constexpr int ActionStep = 32;

int AccountDelegate::assignment(const QModelIndex& index) const {
    if (!assignmentState || index.data(GroupedMemberModel::GroupHeaderRole).toBool() ||
        (index.data(Qt::UserRole + 3).toInt() & MEMBER_FLAG_DELETED)) return -1;
    return assignmentState(index.data(MemberListModel::Ed25519Role).toString());
}
int AccountDelegate::visibleActionCount(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const bool header = index.data(GroupedMemberModel::GroupHeaderRole).toBool();
    const bool calls = header ? index.data(GroupedMemberModel::CallAvailableRole).toBool() : !index.data(Qt::UserRole + 2).toBool();
    const int flags = header ? 0 : index.data(Qt::UserRole + 3).toInt() & (MEMBER_FLAG_ADMIN | MEMBER_FLAG_MODER);
    const int badges = __builtin_popcount(unsigned(flags));
    const int badgeWidth = badges * 13 + qMax(0, badges - 1) * 2;
    const int available = option.rect.width() - NameOffset - 5 - badgeWidth - MinimumNameWidth - (assignment(index) >= 0 ? ActionStep : 0);
    return qBound(0, available / ActionStep, calls ? 3 : 1);
}

QRect AccountDelegate::actionRect(const QRect& row, Action action) {
    const int offset = action == Action::RadioMute ? 31 : action == Action::RadioCall ? 95 : 63;
    return QRect(row.right() - offset, row.center().y() - 14, 28, 28);
}

/* Трубка в той же системе координат 24×24, что и портативная рация. */
static void drawCallButton(QPainter* painter, const QRect& button, const QColor& color) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(button.center().x() - 12, button.center().y() - 12);
    QPainterPath phone;
    phone.moveTo(6.6, 10.8); phone.cubicTo(8.1, 13.6, 10.4, 15.9, 13.2, 17.4);
    phone.lineTo(15.4, 15.2); phone.quadTo(15.8, 14.8, 16.3, 15);
    phone.quadTo(18.1, 15.6, 20, 15.6); phone.quadTo(21, 15.6, 21, 16.6);
    phone.lineTo(21, 20); phone.quadTo(21, 21, 20, 21);
    phone.cubicTo(10.6, 21, 3, 13.4, 3, 4); phone.quadTo(3, 3, 4, 3);
    phone.lineTo(7.4, 3); phone.quadTo(8.4, 3, 8.4, 4);
    phone.quadTo(8.4, 5.9, 9, 7.7); phone.quadTo(9.2, 8.2, 8.8, 8.6);
    phone.closeSubpath();
    painter->fillPath(phone, color);
    painter->restore();
}

// Волны показывают передачу независимо от локального mute; перечёркивание сохраняется при речи.
static void drawRadioMuteButton(QPainter* painter, const QRect& button, bool muted, bool speaking) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(button.center());
    QColor color(muted ? "#EF5350" : speaking ? "#4CAF50" : "#9E9E9E");
    QPainterPath speaker;
    speaker.moveTo(-8, -3); speaker.lineTo(-4, -3); speaker.lineTo(1, -7);
    speaker.lineTo(1, 7); speaker.lineTo(-4, 3); speaker.lineTo(-8, 3); speaker.closeSubpath();
    painter->fillPath(speaker, color);
    painter->setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap)); painter->setBrush(Qt::NoBrush);
    if (muted) painter->drawLine(QPointF(-10, 10), QPointF(10, -10));
    if (speaking) {
        painter->drawArc(QRectF(-3, -5, 10, 10), -60 * 16, 120 * 16);
        painter->drawArc(QRectF(-6, -8, 16, 16), -60 * 16, 120 * 16);
    }
    painter->restore();
}

static QRect avatarRect(const QRect& row) {
    const int size = MemberListModel::kAvatarSize;
    return QRect(row.left() + GroupedMemberModel::DisclosureWidth + 4, row.top() + (row.height() - size) / 2, size, size);
}

AccountDelegate::Action AccountDelegate::actionAt(const QPoint& position, const QRect& row, int actionCount, bool assignment) {
    if (assignment && actionRect(row, Action::RadioMute).contains(position)) return Action::Assign;
    const QRect controls = assignment ? row.adjusted(0, 0, -ActionStep, 0) : row;
    if (avatarRect(row).contains(position)) return Action::Avatar;
    if (actionCount >= 1 && actionRect(controls, Action::RadioMute).contains(position)) return Action::RadioMute;
    if (actionCount >= 2 && actionRect(controls, Action::Call).contains(position)) return Action::Call;
    if (actionCount >= 3 && actionRect(controls, Action::RadioCall).contains(position)) return Action::RadioCall;
    return Action::None;
}

AccountDelegate::AccountDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {
    GUI_DEBUG("AccountDelegate: row height=%dpx avatar=%dpx gap=%dpx", MemberListModel::kRowHeight,
        MemberListModel::kAvatarSize, MemberListModel::kRowHeight - MemberListModel::kAvatarSize);
    GUI_DEBUG("AccountDelegate: adaptive actions name_reserve=%dpx order=radio-call/call/speaker", MinimumNameWidth);
    if (const auto* view = qobject_cast<QWidget*>(parent))
        GUI_DEBUG("AccountDelegate: tree text=%s background=%s alpha=160 width=1.5px",
            qPrintable(view->palette().color(QPalette::Text).name()), qPrintable(view->palette().color(QPalette::AlternateBase).name()));
}

void AccountDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                             const QModelIndex &index) const {
    painter->save();

    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, option.palette.highlight());
    else if (option.state & QStyle::State_MouseOver)
        painter->fillRect(option.rect, option.palette.light());
    else
        painter->fillRect(option.rect, option.palette.alternateBase());

    const bool header = index.data(GroupedMemberModel::GroupHeaderRole).toBool();
    const bool expanded = index.data(GroupedMemberModel::GroupExpandedRole).toBool();
    const int branch = index.data(GroupedMemberModel::GroupBranchRole).toInt();
    const qreal branchX = option.rect.left() + 12, branchY = option.rect.center().y();
    QColor branchColor = option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text);
    branchColor.setAlpha(160);
    painter->setRenderHint(QPainter::Antialiasing); painter->setPen(QPen(branchColor, 1.5));
    if (header && expanded) painter->drawLine(QPointF(branchX, branchY + 7), QPointF(branchX, option.rect.bottom() + 1));
    if (branch) {
        painter->drawLine(QPointF(branchX, option.rect.top()), QPointF(branchX, branch == 2 ? branchY : option.rect.bottom() + 1));
        painter->drawLine(QPointF(branchX, branchY), QPointF(avatarRect(option.rect).left() - 4, branchY));
    }
    if (header) {
        const QRectF box(option.rect.left() + 5, option.rect.center().y() - 7, 14, 14);
        painter->setRenderHint(QPainter::Antialiasing); painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(option.palette.color(QPalette::Text), 1.3)); painter->drawRoundedRect(box, 2, 2);
        painter->drawLine(QPointF(box.left() + 3, box.center().y()), QPointF(box.right() - 3, box.center().y()));
        if (!expanded)
            painter->drawLine(QPointF(box.center().x(), box.top() + 3), QPointF(box.center().x(), box.bottom() - 3));
    }
    if (index.data(GroupedMemberModel::DeletedGroupRole).toBool()) {
        QFont font = painter->font(); font.setBold(true); painter->setFont(font);
        painter->setPen(option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(option.rect.adjusted(GroupedMemberModel::DisclosureWidth + 4, 0, -4, 0),
            Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());
        painter->restore(); return;
    }
    const int margin = 4;

    QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    if (!icon.isNull())
        icon.paint(painter, avatarRect(option.rect));
    if (header) {
        const QRect avatar = avatarRect(option.rect); const QColor color(index.data(MemberListModel::OnlineRole).toBool() ? "#4CAF50" : "#9E9E9E");
        painter->setPen(QPen(color, 1)); painter->setBrush(color); painter->drawEllipse(QRect(avatar.right() - 8, avatar.bottom() - 8, 8, 8));
    }

    const int assigned = assignment(index);
    const QRect controls = assigned >= 0 ? option.rect.adjusted(0, 0, -ActionStep, 0) : option.rect;
    const int x = option.rect.left() + NameOffset, actions = visibleActionCount(option, index);
    const bool online = index.data(MemberListModel::OnlineRole).toBool();
    const int w = qMax(0, option.rect.right() - x - margin - actions * ActionStep - (assigned >= 0 ? ActionStep : 0));
    const int yMid = option.rect.top() + option.rect.height() / 2;

    uint8_t flags = (uint8_t)(index.data(Qt::UserRole + 3).toInt() & 0xFF);

    QFont nameFont = painter->font();
    nameFont.setBold(true);
    nameFont.setPixelSize(13);
    if (!header && (flags & MEMBER_FLAG_DELETED)) nameFont.setStrikeOut(true);
    painter->setFont(nameFont);

    QColor textColor = option.palette.color(QPalette::Text);
    if (option.state & QStyle::State_Selected) textColor = option.palette.color(QPalette::HighlightedText);
    if (index.data(Qt::UserRole + 2).toBool() && !(option.state & QStyle::State_Selected))
        textColor = option.palette.window().color().lightness() < 128 ? QColor("#80d995") : QColor("#226d38");
    painter->setPen(textColor);

    painter->save();
    painter->setClipRect(QRect(x, option.rect.top(), w, option.rect.height()), Qt::IntersectClip);
    const uint8_t visualFlags = header ? 0 : flags & (MEMBER_FLAG_ADMIN | MEMBER_FLAG_MODER);
    int flagX = x;
    if (visualFlags) {
        FlagPainter::draw(painter, flags, flagX, option.rect.top() + (option.rect.height() - nameFont.pixelSize()) / 2, nameFont.pixelSize());
        int n = __builtin_popcount(visualFlags);
        flagX += nameFont.pixelSize() * n + (n - 1) * 2;
    }
    if (!header && !index.data(MemberListModel::ProtocolWarningRole).toString().isEmpty()) {
        painter->save();
        const int y = option.rect.top() + 3;
        QPolygon triangle; triangle << QPoint(flagX + 7, y) << QPoint(flagX, y + 13) << QPoint(flagX + 14, y + 13);
        painter->setPen(QColor("#8A6500")); painter->setBrush(QColor("#FFC107")); painter->drawPolygon(triangle);
        painter->setPen(Qt::black); painter->drawText(QRect(flagX, y + 2, 14, 12), Qt::AlignCenter, "!");
        painter->restore(); flagX += 18;
    }
    QString name = index.data(Qt::DisplayRole).toString();
    painter->drawText(QRect(flagX, option.rect.top() + 2, w - (flagX - x), yMid - option.rect.top() - 2),
                      Qt::AlignLeft | Qt::AlignBottom, name);

    QFont subFont = painter->font();
    subFont.setBold(false);
    subFont.setPixelSize(10);
    painter->setFont(subFont);
    painter->setPen(guiSecondaryText(option.palette, option.state & QStyle::State_Selected));

    QString sub = index.data(Qt::UserRole + 1).toString();
    if (!sub.isEmpty())
        painter->drawText(QRect(flagX, yMid + 2, w - (flagX - x), option.rect.bottom() - yMid - 2),
                          Qt::AlignLeft | Qt::AlignTop, sub);

    painter->restore();
    if (assigned >= 0) {
        const QRect button = actionRect(option.rect, Action::RadioMute);
        const QColor color = assigned ? QColor("#4CAF50") : option.palette.color(QPalette::Text);
        painter->setPen(QPen(color, 1.8)); painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(button.adjusted(5, 5, -5, -5), 3, 3);
        if (assigned) {
            QPainterPath check; check.moveTo(button.left() + 8, button.center().y());
            check.lineTo(button.left() + 12, button.center().y() + 4); check.lineTo(button.right() - 6, button.top() + 8);
            painter->drawPath(check);
        } else {
            painter->drawLine(button.center() + QPoint(-4, 0), button.center() + QPoint(4, 0));
            painter->drawLine(button.center() + QPoint(0, -4), button.center() + QPoint(0, 4));
        }
    }
    const bool available = !header || index.data(MemberListModel::OnlineRole).toBool();
    painter->save(); if (!available) painter->setOpacity(0.35);
    if (actions >= 1) drawRadioMuteButton(painter, actionRect(controls, Action::RadioMute), flags & CHAT_MEMBER_FLAG_RADIO_MUTED,
                        index.data(MemberListModel::RadioSpeakingRole).toBool());
    if (actions >= 2) {
        const QColor color = online ? option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text) :
            option.palette.color(QPalette::Disabled, QPalette::Text);
        if (actions >= 3) {
            const QRect radio = actionRect(controls, Action::RadioCall);
            const QColor radioColor = flags & CHAT_MEMBER_FLAG_RADIO ? QColor("#4CAF50") : color;
            FlagPainter::drawRadio(painter, radio.center().x() - 12, radio.center().y() - 12, 24, radioColor);
        }
        const bool known = index.data(MemberListModel::OnlineRole).toBool() && (flags & CHAT_MEMBER_FLAG_PHONE_KNOWN);
        const QColor phoneColor = known ? QColor(flags & CHAT_MEMBER_FLAG_PHONE_BUSY ? "#E53935" : "#4CAF50") : color;
        drawCallButton(painter, actionRect(controls, Action::Call), phoneColor);
    }
    painter->restore();
    painter->restore();
}

bool AccountDelegate::editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                                  const QModelIndex& index) {
    Q_UNUSED(model);
    if (event->type() == QEvent::MouseMove) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (m_pressedIndex.isValid() && (!(mouse->buttons() & Qt::LeftButton) ||
            (mouse->pos() - m_pressedPosition).manhattanLength() >= ClickDistance)) {
            GUI_DEBUG("AccountDelegate: action cancelled row=%d action=%d distance=%d left_down=%d", m_pressedIndex.row(),
                int(m_pressedAction), (mouse->pos() - m_pressedPosition).manhattanLength(), bool(mouse->buttons() & Qt::LeftButton));
            m_pressedIndex = QPersistentModelIndex(); m_pressedAction = Action::None;
        }
        return false;
    }
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease &&
        event->type() != QEvent::MouseButtonDblClick) return false;
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton) return false;
    const bool header = index.data(GroupedMemberModel::GroupHeaderRole).toBool();
    const bool self = index.data(Qt::UserRole + 2).toBool(), online = index.data(MemberListModel::OnlineRole).toBool();
    const int actions = visibleActionCount(option, index);
    Action action = actionAt(mouse->pos(), option.rect, actions, assignment(index) >= 0);
    if (index.data(GroupedMemberModel::DeletedGroupRole).toBool()) action = Action::ToggleGroup;
    if (header && action == Action::None && mouse->pos().x() < option.rect.left() + GroupedMemberModel::DisclosureWidth)
        action = Action::ToggleGroup;
    const bool isCall = action == Action::Call || action == Action::RadioCall;
    const bool enabled = action == Action::Assign || action == Action::Avatar || action == Action::ToggleGroup ||
        (isCall ? (header ? index.data(GroupedMemberModel::CallAvailableRole).toBool() : !self && online) : !self && (!header || online));
    if (event->type() == QEvent::MouseButtonPress) {
        GUI_DEBUG("AccountDelegate: press row=%d width=%d visible_actions=%d action=%d", index.row(), option.rect.width(), actions, int(action));
        m_pressedIndex = action != Action::None && enabled ? index : QModelIndex();
        m_pressedPeer = index.data(GroupedMemberModel::ActionPeerRole).toByteArray();
        m_pressedAction = action; m_pressedPosition = mouse->pos();
        return action != Action::None;
    }
    if (event->type() == QEvent::MouseButtonDblClick) return action != Action::None;
    const bool handled = action != Action::None || m_pressedIndex.isValid();
    const bool activate = enabled && action != Action::None && action == m_pressedAction && m_pressedIndex == index &&
        m_pressedPeer == index.data(GroupedMemberModel::ActionPeerRole).toByteArray() &&
        (mouse->pos() - m_pressedPosition).manhattanLength() < ClickDistance;
    m_pressedIndex = QPersistentModelIndex(); m_pressedAction = Action::None;
    if (activate) {
        GUI_DEBUG("AccountDelegate: action activated row=%d group=%d action=%d peer=%s online=%d flags=%02x",
            index.row(), header, int(action), m_pressedPeer.toHex().constData(), online, index.data(Qt::UserRole + 3).toInt());
        if (action == Action::Assign) emit assignmentRequested(index.data(MemberListModel::Ed25519Role).toString());
        else if (action == Action::Avatar) emit avatarRequested(index);
        else if (action == Action::RadioMute) emit radioMuteRequested(index);
        else if (action == Action::RadioCall) emit radioCallRequested(index);
        else if (action == Action::Call) emit callRequested(index);
        else if (action == Action::ToggleGroup) emit groupToggleRequested(index);
    }
    return handled;
}

bool AccountDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
                                const QModelIndex& index) {
    if (index.data(GroupedMemberModel::DeletedGroupRole).toBool() && event) {
        QToolTip::showText(event->globalPos(), QCoreApplication::translate("ChatGui",
            index.data(GroupedMemberModel::GroupExpandedRole).toBool() ? "Collapse deleted accounts" : "Expand deleted accounts"), view);
        return true;
    }
    if (index.data(GroupedMemberModel::GroupHeaderRole).toBool() && event &&
        event->pos().x() < option.rect.left() + GroupedMemberModel::DisclosureWidth) {
        QToolTip::showText(event->globalPos(), QCoreApplication::translate("ChatGui",
            index.data(GroupedMemberModel::GroupExpandedRole).toBool() ? "Collapse devices" : "Expand devices"), view); return true;
    }
    const bool self = index.data(Qt::UserRole + 2).toBool();
    const bool header = index.data(GroupedMemberModel::GroupHeaderRole).toBool(), online = index.data(MemberListModel::OnlineRole).toBool();
    const Action action = event ? actionAt(event->pos(), option.rect, visibleActionCount(option, index), assignment(index) >= 0) : Action::None;
    const QString warning = index.data(MemberListModel::ProtocolWarningRole).toString();
    if (!header && !warning.isEmpty() && action == Action::None && event) {
        QToolTip::showText(event->globalPos(), warning, view); return true;
    }
    if (action == Action::Assign) {
        QToolTip::showText(event->globalPos(), QCoreApplication::translate("ChatGui",
            assignment(index) ? "Remove assignee" : "Assign to card"), view); return true;
    }
    if (action == Action::RadioMute) {
        const bool muted = index.data(Qt::UserRole + 3).toInt() & CHAT_MEMBER_FLAG_RADIO_MUTED;
        const bool speaking = index.data(MemberListModel::RadioSpeakingRole).toBool();
        QString text = QCoreApplication::translate("ChatGui", speaking ? "Radio: speaking" : "Radio: silent");
        if (muted) text += " · " + QCoreApplication::translate("ChatGui", "Radio: muted");
        if (!self && (!header || online)) text += '\n' + QCoreApplication::translate("ChatGui",
            muted ? "Listen to this member in radio" : "Mute this member in radio");
        QToolTip::showText(event->globalPos(), text, view);
        return true;
    }
    if (event && (header ? index.data(GroupedMemberModel::CallAvailableRole).toBool() : !self)) {
        if (action == Action::RadioCall || action == Action::Call) {
            QString text = QCoreApplication::translate("ChatGui", action == Action::RadioCall ? "Call by radio" : "Call peer");
            if (action == Action::RadioCall && (index.data(Qt::UserRole + 3).toInt() & CHAT_MEMBER_FLAG_RADIO))
                text += QCoreApplication::translate("ChatGui", " · Radio enabled");
            if (action == Action::Call && index.data(MemberListModel::OnlineRole).toBool()) {
                const int flags = index.data(Qt::UserRole + 3).toInt();
                text += " · " + QCoreApplication::translate("ChatGui", !(flags & CHAT_MEMBER_FLAG_PHONE_KNOWN) ? "Phone status unknown" :
                    (flags & CHAT_MEMBER_FLAG_PHONE_BUSY) ? "Busy on a call" : "Phone is free");
            }
            if (!index.data(MemberListModel::OnlineRole).toBool()) text += " · " + QCoreApplication::translate("ChatGui", "offline");
            QToolTip::showText(event->globalPos(), text, view);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

QSize AccountDelegate::sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const {
    (void)option; (void)index;
    return QSize(160, MemberListModel::kRowHeight);
}
