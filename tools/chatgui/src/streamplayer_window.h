/* Просмотр одной live-сессии (group, source, epoch): конструктор запускает подключение и worker.
 * GUI владеет окном; audio worker получает батчи/готовит PCM, video worker единолично владеет FFmpeg.
 * Общий SoundManager смешивает output; отдельные часы этого player учитывают PCM, задержку устройства и адаптацию tempo.
 * GUI показывает готовые кадры/телеметрию; zoom/drag ограничены физическим 1:1. Пауза/seek не предусмотрены.
 * Публичные методы вызываются в GUI-потоке; stop/close/деструктор отменяют свой reader, отсоединяют output и join обоих worker.
 * После конца/ошибки окно остаётся открытым с последним кадром; родительский grid решает, когда удалить или пересоздать player. */
#pragma once

#include <QWidget>
#include <thread>
#include <atomic>
#include <QTimer>
#include <memory>
#include <cstdint>

class VideoRenderWidget;
class QLabel;
class QCloseEvent;
class QKeyEvent;
class QPushButton;

class StreamPlayerWindow : public QWidget {
    Q_OBJECT
public:
    void stopAsync(); // GUI: скрыть/отсоединить output; join выполняется в отдельном worker.
    bool released() const { return m_joinDone.load(); } // После stopAsync оба worker завершены, объект ещё жив.
    // group/source/epoch фиксируют media-сессию; name — подпись. embedded передаёт управление окном grid и уплотняет layout.
    explicit StreamPlayerWindow(const QString& group, const QByteArray& source, uint64_t epoch, const QString& name,
                                QWidget* parent = nullptr, bool embedded = false);
    ~StreamPlayerWindow() override;
    // GUI-поток: обновить подпись, tooltip и заголовок окна; decoder, reader, аудиочасы и громкость сохраняются.
    void setSourceName(const QString& name);
    void setPlaybackSwitchVisible(bool visible); // Общий переключатель на строке задержек нижней плитки либо focused player.
    void setWindowControlsVisible(bool visible); // Grid показывает fullscreen/close у развёрнутой плитки без пересоздания playback.
    void stop(); // Идемпотентная окончательная остановка; повторного старта нет, сама finished/failed не устанавливает.
    bool finished() const; // Полное естественное доигрывание либо терминальное закрытие/ошибка; не просто stop().
    bool failed() const; // Ошибка, для которой grid должен переподключиться; штатные SOURCE_CHANGED/MEMBER/GROUP/SERVICE дают false.
    int drainTimeoutMs() const; // finished → 0; иначе запас ready-clock в мс + 2000, ограниченный [2000, 30000], для grid.
signals:
    void statusChanged(const QString& status); // Статус/ошибка из worker; GUI показывает сообщение через queued-соединение.
    void framePresented(); // GUI передал новый кадр renderer; промежуточные опоздавшие кадры могут быть пропущены.
    void fullscreenRequested(); // Embedded: владелец переключает fullscreen общего окна grid.
    void closeRequested(); // Embedded: владелец возвращает сетку вместо закрытия playback плитки.
protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
private:
    struct Playback;
    std::unique_ptr<Playback> m_playback; // Владелец reader-id, двух worker, очередей/часов и DSP; разделяемые данные под lock/атомиками.
    std::thread m_joinThread; // Join обоих decoder workers; QWidget удаляется после released.
    std::atomic<bool> m_joinDone{false};
    bool m_audioAttached = false; // GUI зарегистрировал callback общего output; detach завершается до удаления Playback.
    QTimer m_timer; // GUI tick 15мс: показ кадров, телеметрия и завершение output.
    VideoRenderWidget* m_render = nullptr; // Дочерний renderer с zoom, сохраняющий вид при следующих кадрах того же размера.
    QLabel* m_title = nullptr; // Подпись источника; принадлежит QObject-дереву окна, меняется без перезапуска playback.
    QLabel* m_status = nullptr; // Статус подключения, ожидания, завершения либо ошибки; скрыт во время показа.
    QLabel* m_deliveryWarning = nullptr; // Отставание без доступного свежего KEY; показ продолжается.
    QLabel* m_telemetry = nullptr; // Rich text: цепочка имён/буферов и tempo; полные имена доступны в tooltip.
    QPushButton *m_fullscreen = nullptr, *m_close = nullptr; // Кнопки справа в нижней панели; embedded делегирует их действия grid.
    int64_t m_telemetryAt = 0; // Монотонное время последней телеметрии, микросекунды steady_clock.
    void showStatus(const QString& status); // Показать строку состояния с сообщением в GUI-потоке.
    void attachAudio(); // Регистрирует callback 48кГц stereo; он забирает ближайший PCM и учитывает часы DAC/generation output.
    void tick(); // GUI берёт кадры, готовые по часам, и снимает output при полном доигрывании/ошибке.
    void decode(); // Audio worker: открывает/закрывает свой reader, получает события/батчи, готовит PCM, управляет resync/tempo.
    void decodeVideo(); // Video worker: ограниченная очередь кадров, revision/generation, FFmpeg drain и отбрасывание старых результатов.
};
