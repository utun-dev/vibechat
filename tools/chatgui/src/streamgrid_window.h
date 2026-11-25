/* Окно всех активных трансляций одного канала. Собственный catalogue owner не зависит от выбранного чата.
 * Плитка сохраняет player/decoder/часы при перекладке; новая эпоха заменяет только соответствующий player.
 * Своя плитка получает setLocalPreview до кодирования, без reader/audio; клик запрашивает управление.
 * Чужая плитка разворачивается без пересоздания players; Esc/повторный клик возвращает сетку.
 * Исчезновение из каталога сохраняет открытый viewer в ожидании; FIN/терминальное закрытие разрешают удалить плитку.
 * Все методы — GUI-поток; диспетчер передаёт снимки через onSources, publisher — кадры через setLocalPreview.
 * Закрытие терминально: останавливает таймер/плитки и снимает свой watch. WA_DeleteOnClose задаёт владелец окна. */
#pragma once
#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QPointer>
#include <QVariant>
#include <vector>
#include <cstdint>

class StreamPlayerWindow;
class QGridLayout;
class QLabel;
class QCloseEvent;

class StreamGridWindow : public QWidget {
    Q_OBJECT
public:
    void stopForPhone(); // Остановить watch/плитки без ожидания GUI; окно сохраняет владельцев до release.
    bool released() const; // Все decoder workers остановлены после stopForPhone.
    // group — ненулевая CHAT-группа; parent владеет окном по правилам QWidget, конструктор начинает свой watch.
    explicit StreamGridWindow(const QString& group, QWidget* parent = nullptr);
    // Идемпотентно остановить viewers/watch; уничтожение плиток завершает их workers.
    ~StreamGridWindow() override;
    // Неизменная группа этого окна.
    QString group() const { return m_group; }
    // Заимствованный CHAT_EVT_STREAM_SOURCES: ошибка/другая группа игнорируются; valid=false сохраняет все плитки.
    // Новая эпоха или смена self пересоздаёт соответствующую плитку; новое имя обновляет подписи без сброса playback.
    void onSources(const uint8_t* data, int length);
    // Последний QVideoFrame локального publisher в QVariant; пустой кадр сбрасывает preview, другая группа игнорируется.
    void setLocalPreview(const QString& group, const QVariant& frame);
    // Число плиток, включая источники в ожидании восстановления/дочитывания FIN.
    int tileCount() const { return static_cast<int>(m_tiles.size()); }
    // Число столбцов ceil(sqrt(tileCount)); 0 для пустого окна, не меняется при разворачивании одной плитки.
    int gridSide() const { return m_side; }
    // ID развёрнутой чужой плитки; 0 означает общий вид.
    QByteArray focusedSource() const { return m_focused; }
signals:
    void publishRequested(const QString& group); // Открыть управление своей публикацией, сохранив это окно.
protected:
    // stop до штатного закрытия QWidget; повторное show не возобновляет просмотр.
    void closeEvent(QCloseEvent* event) override;
    // Отложенная активация по короткому клику; drag/колесо остаются у видео для pan/zoom.
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Tile {
        QByteArray source;
        uint64_t epoch; // Узел источника и эпоха публикации из каталога.
        QString name; // Имя заголовка/повторно открываемого player.
        bool self = false, previewPresented = false; // Прямой локальный preview; последний переданный ему кадр был валиден.
        QWidget* widget = nullptr; // Владеет player либо прямым preview, удаляется целиком.
        QWidget* video = nullptr; // Поверхность изображения; сюда направлены клики.
        StreamPlayerWindow* player = nullptr; // Только remote: владелец decoder, аудиочасов и viewer.
        QLabel* title = nullptr; // Подпись self-preview; подпись remote принадлежит StreamPlayerWindow.
        QLabel* status = nullptr; // Статус прямого preview; у remote им владеет player.
        bool listed = true; // Есть в каталоге; отсутствие не закрывает рабочий/ожидающий viewer.
        qint64 retryAt = 0; // Дедлайн повтора decoder по m_clock в мс; 0 — не назначен.
        unsigned failures = 0; // Backoff decoder: 500мс, 1/2/4/8с, далее максимум 10с.
    };
    QString m_group;
    uint64_t m_owner; // Группа и независимый process-local owner каталога.
    std::vector<Tile> m_tiles; // Каждый widget принадлежит окну; удаляется целиком при смене идентичности/stop.
    // Элементы интерфейса принадлежат QObject-дереву этого окна.
    QGridLayout* m_grid = nullptr;
    QWidget* m_controls = nullptr; // Общая нижняя панель сетки; при focus её заменяет нижняя панель выбранного player.
    QLabel* m_status = nullptr;
    QVariant m_preview; // Только последний кадр издателя; сеть/декодер для self не открываются.
    QTimer m_timer; // 250мс: повтор watch, backoff decoder и завершение исчезнувших источников.
    QElapsedTimer m_clock; // Общие монотонные часы дедлайнов плиток.
    int m_side = 0; // Число столбцов в общем виде.
    bool m_watching = false, m_closed = false; // Watch принят bridge; окно терминально остановлено.
    QByteArray m_focused; // Source развёрнутой чужой плитки; 0 — сетка.
    Qt::WindowStates m_gridWindowState; // Состояние окна до разворачивания remote-плитки.
    QPointer<QObject> m_pressed; // Поверхность нажатия; удаление плитки безопасно обнуляет указатель.
    QPoint m_pressPosition; // Глобальная позиция нажатия для отличия клика от drag.
    bool m_dragged = false; // Пройден QApplication::startDragDistance.
    void createTile(Tile& tile); // Создать self-preview либо embedded player, установить фильтры кликов.
    void updatePreview(Tile& tile); // Передать m_preview поверхности self и обновить её статус.
    void activateTile(const QByteArray& source); // Self запрашивает управление; remote разворачивается/возвращается в сетку.
    void restoreGrid(); // Восстановить состояние окна и раскладку, сохранив playback всех плиток.
    void arrange(); // Переложить/скрыть widgets, сохранив их decoder, reader и часы.
    void tick(); // Повторить watch/decoder либо удалить отсутствующую в каталоге терминально завершённую плитку.
    void stop(); // Терминально остановить таймер/watch и уничтожить плитки; повтор безопасен.
};
