/* GUI-модуль: VideoRenderWidget показывает готовые QImage; VideoPlayerWindow владеет файловым
 * VideoPlayerEngine и закрывается после полного проигрывания. Методы вызываются в GUI-потоке.
 * Родитель QObject владеет дочерними виджетами; closed сообщает о closeEvent, само окно не удаляет. */
#pragma once

#include <QWidget>
#include <QImage>
#include <QString>
#include <QPoint>
#include <QTimer>

class QSlider;
class QPushButton;
class QLabel;
class VideoPlayerEngine;

/* Кадр на чёрном фоне с сохранением пропорций. Без zoom заполняет доступную область, включая увеличение.
 * zoomEnabled ограничивает масштаб диапазоном от вписывания без увеличения до физического 1:1;
 * колесо сохраняет точку под курсором, drag двигает только выступающие части кадра. */
class VideoRenderWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoRenderWidget(QWidget* parent = nullptr, bool zoomEnabled = false);
    void setFrame(const QImage& img); // Shared-снимок кадра; в zoom смена размеров сбрасывает масштаб/drag, равный размер сохраняет вид.
    void clearFrame(); // Удаляет кадр, масштаб и drag, возвращает обычный курсор и чёрный фон.

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    qreal frameScale() const; // Логические пиксели Qt на пиксель изображения; 0 без кадра/доступной области.
    void constrainView(const char* reason); // Ограничивает масштаб/смещение после zoom, resize или смены экрана; reason — для TRACE.
    void finishDrag(); // Завершает pan без сброса выбранного участка; также при hide, потере окна/захвата мыши.

    QImage m_frame; // Последний shared-кадр, пустой рисуется чёрным.
    const bool m_zoomEnabled; // Выбирается при создании; файловое окно использует false, live-плеер — true.
    qreal m_scale = 0; // Физические пиксели на пиксель кадра; 0 — автоматическое вписывание.
    QPointF m_offset; // Смещение от центра в пикселях исходного кадра.
    QPointF m_lastMouse; // Предыдущая позиция мыши в логических координатах виджета во время pan.
    bool m_panning = false; // Активен drag левой кнопкой на увеличенном кадре.
};

/* Файловое окно без рамки: drag-перенос, resize краями с текущими пропорциями и минимумом 320×240.
 * Dblclick/Alt+Enter — fullscreen, Esc — выйти из fullscreen либо закрыть, Space — play/pause.
 * Геометрией/физическим монитором управляет вызывающий; открытие файла и размеры кадров её не меняют.
 * Fullscreen определяется состоянием QWidget, включая восстановление сохранённого режима.
 * В fullscreen панель/курсор скрываются через 2500мс, кроме Paused/Finished; движение мыши показывает панель. */
class VideoPlayerWindow : public QWidget {
    Q_OBJECT
public:
    explicit VideoPlayerWindow(QWidget* parent = nullptr);
    ~VideoPlayerWindow() override;

    void playFile(const QString& path); // Показывает окно с текущей геометрией/режимом, сбрасывает кадр/шкалу и открывает engine.

signals:
    void fileOpened(const QString& path); // Локальный файл успешно открыт движком после команды пользователя.
    void closed(); // Engine уже закрыт; владелец может запланировать deleteLater (MainWindow так и делает).

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onEngineOpened(int width, int height, qint64 durationMs, bool hasAudio);
    void onPositionChanged(qint64 ms);
    void onStateChanged(int state);
    void onError(const QString& msg);
    void onFramePresented();
    void onSliderPressed(); // Удерживает значение ползунка от обновлений engine; воспроизведение продолжается.
    void onSliderReleased(); // Seek к значению в мс, затем возобновление, включая ранее приостановленный файл.
    void onPlayPause();
    void onClose();
    void onFinished(); // Finished engine означает доигранный файл; закрывает engine и окно.
    void showControls();
    void hideControls();

private:
    enum ResizeMode {
        ResizeNone = 0,
        ResizeLeft, ResizeRight, ResizeTop, ResizeBottom,
        ResizeTopLeft, ResizeTopRight, ResizeBottomLeft, ResizeBottomRight
    };

    void toggleFullscreen(); // Переключает нормальное/полноэкранное состояние, настраивает видимость панели и курсора.
    void updateTimeLabels(qint64 posMs); // Отображает текущую позицию/длительность как минуты:секунды.
    void setControlsVisible(bool visible); // Панель и отдельная верхняя кнопка закрытия показываются/скрываются вместе.
    ResizeMode hitTestResize(const QPoint& localPos) const; // Край/угол в полосе 6 логических пикселей; иначе ResizeNone.
    void updateResizeCursor(const QPoint& localPos); // Курсор соответствует краю/углу окна.
    void applyResize(const QPoint& globalPos); // Пересчитывает геометрию от начала жеста, сохраняет исходные пропорции окна.
    void beginResize(ResizeMode m, const QPoint& globalPos); // Запоминает режим, геометрию и глобальную позицию начала жеста.

    VideoRenderWidget* m_render = nullptr; // Дочерний renderer без zoom, растянут на всё окно и прозрачен для мыши.
    QWidget* m_controls = nullptr; // Нижняя панель высотой 48; пустые поля маршрутизируют resize нижнего края.
    QSlider* m_slider = nullptr; // Позиция в мс, диапазон 0..duration; клик прыгает к выбранной точке.
    QPushButton* m_playBtn = nullptr; // Play/pause и соответствующая подсказка.
    QPushButton* m_closeBtn = nullptr; // Кнопка поверх кадра справа вверху, вне нижней панели.
    QLabel* m_timeLabel = nullptr; // Позиция/длительность либо текст ошибки engine.
    VideoPlayerEngine* m_engine = nullptr; // Дочерний файловый плеер; closeEvent освобождает его media-ресурсы.

    bool m_sliderDragging = false; // Не подменять значение слайдера текущей позицией engine.
    bool m_dragging = false; // Перенос окна за область кадра левой кнопкой.
    QPoint m_dragStart; // Глобальная позиция мыши в начале переноса.
    QPoint m_windowStart; // Верхний левый угол frameGeometry в начале переноса.
    ResizeMode m_resizeMode = ResizeNone; // Активный край/угол либо отсутствие жеста resize.
    QRect m_resizeStartGeom; // Геометрия до resize; её пропорции сохраняются на протяжении жеста.
    QPoint m_resizeStartGlobal; // Начальная глобальная позиция мыши для resize.
    QTimer* m_hideTimer = nullptr; // Дочерний одноразовый таймер 2500мс, скрытие разрешено только в fullscreen.
};
