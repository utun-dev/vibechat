/* Нижние вкладки EmojiPanel: кнопки 48x32 px со значками Unicode и выделением активной категории.
 * QTabBar управляет выбором и подсказками; рисование использует цвета текущей palette. */
#pragma once

#include <QTabBar>

class EmojiTabBar : public QTabBar {
    Q_OBJECT
public:
    /* parent владеет виджетом; отключает стандартную базовую линию, растяжение вкладок и кнопки прокрутки. */
    explicit EmojiTabBar(QWidget *parent = nullptr);

protected:
    QSize tabSizeHint(int index) const override; // Размер каждой кнопки категории — 48x32 логических px.
    /* Полностью рисует bar по tabRect/tabText/currentIndex: значок, выделение и верхний разделитель.
     * event не используется, базовый paintEvent не вызывается; иконки вкладок эта функция не рисует. */
    void paintEvent(QPaintEvent *event) override;
};
