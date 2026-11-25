/* Компактные кнопки нижней панели сетки и плеера. Подпись доступна в подсказке и экранном дикторе. */
#pragma once

#include <QPushButton>
#include <QStyle>

// Создаёт кнопку у parent. themeIcon при наличии заменяет стандартный значок Qt, label описывает действие.
inline QPushButton* streamViewButton(QWidget* parent, const QString& label, QStyle::StandardPixmap fallback, const QString& themeIcon = {}) {
    auto* button = new QPushButton(parent);
    const QIcon icon = button->style()->standardIcon(fallback);
    button->setIcon(themeIcon.isEmpty() ? icon : QIcon::fromTheme(themeIcon, icon));
    button->setFixedSize(32, 32);
    button->setToolTip(label); button->setAccessibleName(label);
    return button;
}
