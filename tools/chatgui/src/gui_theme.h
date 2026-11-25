/* Две палитры GUI: applyGuiTheme вызывается в Qt-потоке при запуске и после Save.
 * Виджеты наследуют палитру приложения; растровые значки обновляются по PaletteChange. */
#pragma once

#include <QApplication>
#include <QPalette>
#include <QString>
#include "../../../lib/debug_config.h"

/* Возвращает самостоятельную палитру light/dark; неизвестное имя трактуется как dark. */
inline QPalette guiThemePalette(const QString& theme) {
    const bool light = theme == QStringLiteral("light");
    QPalette p;
    p.setColor(QPalette::Window, light ? QColor("#ffffff") : QColor("#202020"));
    p.setColor(QPalette::WindowText, light ? QColor("#000000") : QColor("#dcdcdc"));
    p.setColor(QPalette::Base, light ? QColor("#ffffff") : QColor("#181818"));
    p.setColor(QPalette::AlternateBase, light ? QColor("#ffffff") : QColor("#262626"));
    p.setColor(QPalette::ToolTipBase, light ? QColor("#ffffff") : QColor("#121212"));
    p.setColor(QPalette::ToolTipText, p.color(QPalette::WindowText));
    p.setColor(QPalette::Text, p.color(QPalette::WindowText));
    p.setColor(QPalette::Button, light ? QColor("#f4f4f4") : QColor("#242424"));
    p.setColor(QPalette::ButtonText, p.color(QPalette::WindowText));
    p.setColor(QPalette::Light, light ? QColor("#ededed") : QColor("#353535"));
    p.setColor(QPalette::Midlight, light ? QColor("#f1f1f1") : QColor("#2e2e2e"));
    p.setColor(QPalette::Mid, light ? QColor("#dddddd") : QColor("#606060"));
    p.setColor(QPalette::Dark, light ? QColor("#d0d0d0") : QColor("#121212"));
    p.setColor(QPalette::Shadow, light ? QColor("#c8c8c8") : QColor("#080808"));
    p.setColor(QPalette::BrightText, light ? QColor("#b42332") : QColor("#ff8e91"));
    p.setColor(QPalette::Link, light ? QColor("#155da8") : QColor("#79baff"));
    p.setColor(QPalette::LinkVisited, light ? QColor("#6941a5") : QColor("#c9a3ff"));
    p.setColor(QPalette::Highlight, QColor("#2060a0"));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, light ? QColor("#526174") : QColor("#b0b0b0"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, light ? QColor("#707070") : QColor("#929292"));
    return p;
}

/* Второстепенный текст, включая подписи на выделенных строках; palette — палитра рисуемого виджета. */
inline QColor guiSecondaryText(const QPalette& palette, bool selected = false) {
    return palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText);
}

/* Устанавливает палитру всему приложению; неизвестная тема логируется и заменяется на dark. */
inline void applyGuiTheme(const QString& theme) {
    const bool valid = theme == QStringLiteral("dark") || theme == QStringLiteral("light");
    if (!valid) DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI theme: invalid preference=%s; using dark", qPrintable(theme));
    const QPalette palette = guiThemePalette(theme);
    QApplication::setPalette(palette);
    // Пересчитываем palette(...) в локальных QSS: без repolish уже открытые поля сохраняют прежний фон.
    // Цвета подсказок задаём явно: tooltip-роли доступны не во всех версиях QSS Qt.
    qApp->setStyleSheet(QStringLiteral(
        "QToolTip { color: %1; background: %2; border: 1px solid %3; }"
        "QFrame[frameShape=\"4\"] { border: none; background: palette(mid); min-height: 1px; max-height: 1px; }"
        "QFrame[frameShape=\"5\"] { border: none; background: palette(mid); min-width: 1px; max-width: 1px; }")
        .arg(palette.color(QPalette::ToolTipText).name(), palette.color(QPalette::ToolTipBase).name(), palette.mid().color().name()));
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI theme applied: theme=%s window=%s base=%s text=%s secondary=%s highlight=%s tooltip=%s/%s",
        theme == QStringLiteral("light") ? "light" : "dark", qPrintable(palette.window().color().name()),
        qPrintable(palette.base().color().name()), qPrintable(palette.text().color().name()),
        qPrintable(guiSecondaryText(palette).name()), qPrintable(palette.highlight().color().name()),
        qPrintable(palette.color(QPalette::ToolTipText).name()), qPrintable(palette.color(QPalette::ToolTipBase).name()));
}
