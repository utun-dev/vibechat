/* Компактная GUI-панель встроенных emoji: вкладки со значками снизу, названия категорий — в подсказках.
 * Смайлики 27 px в 9 колонках кнопок 32x32 px с зазором 1 px; высота зависит от числа рядов выбранной категории.
 * Анимированные смайлики увеличены до 41 px (кнопки 46x46 px); число колонок подстраивается под ту же ширину панели.
 * Передаёт точную строку через emojiSelected.
 * Создаётся скрытой поверх parent без добавления в layout; держится в его правом нижнем углу при resize.
 * Показывать/скрывать должен владелец. Выбор не скрывает панель и сам не меняет composer.
 * До конструктора зарегистрировать анимационные значки в AnimTimer: кнопки кешируют заимствованные указатели,
 * владельцы значков должны сохранять их до удаления панели. Без значка кнопка показывает обычный Unicode. */
#pragma once

#include <QWidget>
#include <QVector>

class QPushButton;
class QTabWidget;
struct EmojiCategory;

class EmojiPanel : public QWidget {
    Q_OBJECT
public:
    /* Строит каталог поверх parent (viewport истории); parent владеет панелью, панель — вкладками, страницами и кнопками. */
    explicit EmojiPanel(QWidget *parent = nullptr);

signals:
    void emojiSelected(const QString &unicode); // Click передаёт Unicode записи, не shortcode/путь; подписчик решает, куда вставить.

protected:
    void showEvent(QShowEvent *event) override; // Разместить поверх истории и поднять над соседями, не меняя layout чата.
    bool eventFilter(QObject *object, QEvent *event) override; // Resize родителя перемещает открытую панель вместе с нижним правым углом.

private slots:
    void onEmojiClicked(); // Читает Unicode из анимационной записи либо текст обычной QPushButton; чужой sender игнорируется.

private:
    QTabWidget *m_tabs = nullptr; // Дочерние вкладки; текущая страница определяет нужную высоту панели.
    void updateOverlayGeometry(); // Высота по содержимому, с parent — максимум 320 px и половина viewport; нижний правый угол закреплён.
    void buildPanel(const QVector<EmojiCategory> &categories); // Однократная сборка вкладок из переданных категорий в конструкторе.
    /* Новая scroll-страница с плотной сеткой: 9 колонок кнопок 32x32 px либо 6 колонок 46x46 px для Animation.
     * Владение передаётся QTabWidget; крупные кнопки не расширяют панель.
     * Animation использует локальный таймер hover: минимум AnimTimer::PlaybackMs, затем остановка на кадре 0.
     * Выход курсора не прерывает цикл; Unicode/Image отображаются текстом, imagePath не используется. */
    QWidget *buildCategoryPage(const EmojiCategory &cat);
};
