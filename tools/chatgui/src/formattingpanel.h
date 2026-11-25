/* Подсказка форматирования сообщений поверх истории чата, по аналогии с EmojiPanel.
 * Создаётся скрытой поверх parent без добавления в layout; при показе и resize закрепляется в правом нижнем углу.
 * Высота автоматически подгоняется под весь текст без прокрутки; текст можно выделять и копировать.
 * Показывать/скрывать панель должен владелец, parent владеет её жизненным циклом. */
#pragma once

#include <QWidget>

class QShowEvent;

class FormattingPanel : public QWidget {
    Q_OBJECT
public:
    /* Создать скрытую панель с краткой справкой по inline-тегам и блокам кода. */
    explicit FormattingPanel(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override; // Разместить поверх истории и поднять над соседями.
    bool eventFilter(QObject *object, QEvent *event) override; // Удерживать открытую панель в углу при resize parent.

private:
    void updateOverlayGeometry(); // Ограничить ширину parent, подобрать высоту содержимого и закрепить в правом нижнем углу.
};
