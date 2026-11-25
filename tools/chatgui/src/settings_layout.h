/* Общие секции настроек: заголовок и раскрываемые подробности без рамок.
 * Вызывать в GUI; layout/виджеты принадлежат Qt-дереву страницы, тексты передаются уже переведёнными. */
#pragma once

#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFormLayout>

// Добавляет заголовок секции в layout; возвращает дочернюю подпись.
inline QLabel* settingsHeading(QVBoxLayout* layout, const QString& title) {
    auto* label = new QLabel(title);
    label->setProperty("settingsRole", "heading");
    QFont font = label->font(); font.setBold(true); label->setFont(font);
    layout->addWidget(label);
    return label;
}

// Добавляет короткое пояснение с переносом строк под соответствующим параметром.
inline QLabel* settingsHint(QVBoxLayout* layout, const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true); label->setProperty("settingsRole", "hint");
    layout->addWidget(label);
    return label;
}

// Создаёт закрытый блок подробностей; возвращённый layout принимает поля, кнопка управляет только видимостью.
inline QVBoxLayout* settingsDetails(QVBoxLayout* layout, const QString& title) {
    auto* button = new QToolButton;
    button->setText(title); button->setCheckable(true); button->setArrowType(Qt::RightArrow);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setProperty("settingsRole", "details");
    auto* content = new QWidget;
    auto* fields = new QVBoxLayout(content);
    fields->setContentsMargins(12, 8, 0, 8); fields->setSpacing(10); content->hide();
    QObject::connect(button, &QToolButton::toggled, content, [button, content](bool open) {
        button->setArrowType(open ? Qt::DownArrow : Qt::RightArrow); content->setVisible(open);
    });
    layout->addWidget(button, 0, Qt::AlignLeft); layout->addWidget(content);
    return fields;
}
