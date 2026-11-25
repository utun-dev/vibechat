/* Просмотр изображения в GUI-потоке: вписывание, Ctrl+колесо (1..8 от вписанного размера),
 * перенос увеличенного изображения левой кнопкой, сброс двойным левым кликом, закрытие по Esc.
 * Подходит для отдельного окна и вложенного виджета; режимом окна и сохранением геометрии управляет владелец.
 * Закрытие не удаляет объект (WA_DeleteOnClose=false); владелец может повторно показать его. */
#pragma once

#include <QWidget>
#include <QPixmap>

class QPushButton;
class QLabel;
class QTimer;
class QPropertyAnimation;
class QGraphicsOpacityEffect;

class ImageViewerWindow : public QWidget {
    Q_OBJECT
public:
    /* parent владеет виджетом; без parent время жизни контролирует вызывающий код. */
    explicit ImageViewerWindow(QWidget *parent = nullptr);
    /* Синхронно читает path с EXIF-поворотом; успех сбрасывает масштаб/смещение, ошибка очищает изображение и логируется. */
    void loadFile(const QString& path);
    /* Заменяет изображение и сбрасывает вид; пустое изображение включает текст заглушки. */
    void loadImage(const QImage& image);
    /* Текст при пустом изображении; пустая строка выбирает стандартное сообщение об ошибке. */
    void setPlaceholder(const QString& text) { m_placeholder = text; update(); }

signals:
    void fileOpened(const QString& path); // Успешная загрузка файла явным просмотрщиком.
    /* Испускается из closeEvent до стандартного закрытия QWidget; для удаления используйте deleteLater. */
    void closed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    /* Возвращает вписанный размер, нулевое смещение и снимает состояние перетаскивания. */
    void resetView();
    /* Ограничивает смещение краями увеличенного изображения; по вписавшейся оси смещение равно нулю. */
    void clampOffset();
    /* Показывает кнопку/подсказку с анимацией и перезапускает таймер скрытия на 1 секунду. */
    void showOverlay();
    /* Запускает затухание кнопки/подсказки на 300 мс. */
    void hideOverlay();
    /* Перезапускает anim от текущей прозрачности eff к target за durMs миллисекунд. */
    void fadeTo(QPropertyAnimation* anim, QGraphicsOpacityEffect* eff,
                int durMs, double target);

    QPixmap m_image;        /* Текущее изображение; пустое — рисуется заглушка. */
    QString m_placeholder;  /* Необязательный текст заглушки. */
    double  m_scale = 1.0;  /* Множитель базового вписывания, 1..8. */
    QPointF m_offset;       /* смещение изображения в пикселях виджета */
    bool    m_dragging = false; /* Идёт перенос левой кнопкой. */
    QPointF m_dragStart;        /* Координаты мыши в виджете в начале переноса. */
    QPointF m_offsetStart;      /* Смещение изображения в начале переноса. */
    QPushButton* m_closeBtn = nullptr;             /* Дочерняя кнопка закрытия. */
    QGraphicsOpacityEffect* m_closeEffect = nullptr; /* Эффект принадлежит кнопке. */
    QPropertyAnimation*     m_closeFade = nullptr;   /* Дочерняя анимация кнопки. */
    QLabel*                 m_hintLabel = nullptr;  /* Дочерняя подсказка о Ctrl+колесе. */
    QGraphicsOpacityEffect* m_hintEffect = nullptr;  /* Эффект принадлежит подсказке. */
    QPropertyAnimation*     m_hintFade = nullptr;    /* Дочерняя анимация подсказки. */
    QTimer*                 m_hideTimer = nullptr;  /* Дочерний однократный таймер скрытия. */
};
