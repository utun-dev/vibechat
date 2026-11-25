#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QAtomicInt>
#include <QAtomicInteger>
#include <deque>
#include <vector>
#include <memory>
#include <utility>
#include <cstdint>

class QThread;

Q_DECLARE_METATYPE(std::shared_ptr<std::vector<int16_t>>)

/* Файловый FFmpeg-плеер: публичные методы/сигналы — GUI, декодирование — отдельный поток.
 * open запускает воспроизведение асинхронно; close останавливает и ждёт worker.
 * EOF декодера относится к эпохе seek и лишь завершает вход. Playing/Paused сохраняются до показа последнего
 * кадра на полную длительность и окончания PCM; после этого Finished/finished приходят один раз.
 * Аудио-курсор задаёт часы до конца PCM, далее работает монотонный таймер. Seek/open сбрасывают EOF. */
class VideoPlayerEngine : public QObject {
    Q_OBJECT
public:
    enum State { Stopped = 0, Playing = 1, Paused = 2, Finished = 3 }; // Загрузка также Stopped; Finished только после доигрывания.

    explicit VideoPlayerEngine(QObject* parent = nullptr);
    ~VideoPlayerEngine() override;

    void open(const QString& path); // Закрывает прежний файл, запускает worker; opened/error сообщают результат асинхронно.
    void play(); // Возобновляет Paused; в остальных состояниях ничего не делает.
    void pause(); // Приостанавливает Playing, включая доигрывание после EOF; сохраняет кадр и позицию.
    void togglePlayPause(); // Playing ↔ Paused; Finished запускает заново через seek(0).
    void seekMs(qint64 ms); // Асинхронный seek, мс от начала; ограничивается [0, duration], если длительность известна.
    void stop(); // Отменяет worker, удаляет очередь и PCM, сбрасывает позицию/длительность; поток освобождает close.
    void close(); // stop, quit и ожидание worker до 2с, затем освобождение worker/QThread; также вызывается деструктором.

    State state() const { return m_state; }
    qint64 durationMs() const { return m_durationMs; } // Длительность контейнера/видеопотока, 0 — неизвестна либо stop.
    qint64 positionMs() const { return m_positionMs; } // Последняя позиция часов/seek; при Finished — конец принятого медиа.
    bool hasAudio() const { return m_hasAudio; } // Найден аудиотрек; становится false при отказе декодирования/PCM или stop.
    int videoWidth() const { return m_videoWidth; }
    int videoHeight() const { return m_videoHeight; }
    QString filePath() const { return m_path; }
    QImage currentFrame() const { return m_renderFrame; } // Неизменяемый снимок показанного RGB-кадра, пустой до первого показа/после stop.

signals:
    void opened(int width, int height, qint64 durationMs, bool hasAudio); // Метаданные найдены; декодирование/PCM ещё могут завершиться ошибкой.
    void positionChanged(qint64 ms); // Текущая позиция при показе либо seek; при pause таймер остановлен.
    void stateChanged(int state); // Новое значение State уже записано; одинаковое состояние повторно не публикуется.
    void finished(); // Текущее воспроизведение завершено; все поля обновлены до сигнала, можно close/open/seek.
    void error(const QString& msg); // Ошибка текущего open; воспроизведение остановлено до сигнала.
    void framePresented(); // currentFrame уже обновлён; запоздалые промежуточные кадры могут пропускаться.

private slots:
    void onWorkerOpened(int width, int height, qint64 durationMs, bool hasAudio);
    void onAudioDecoded(std::shared_ptr<std::vector<int16_t>> pcm, int sampleRate, int channels, qint64 durationMs);
    void onFrameDecoded(const QImage& image, qint64 ptsMs, qint64 endMs, int epoch);
    void onWorkerError(const QString& msg);
    void onDecodeFinished(int epoch);
    void presentTick();

private:
    void setState(State s);
    qint64 clockMs(); // PCM, затем непрерывный монотонный таймер после окончания аудио.
    void clearPending();

    uint64_t m_openGeneration = 0; // Меняется при open/stop; отбрасывает queued-сигналы прежнего файла.
    QString m_path; // Последний запрошенный путь, сохраняется при stop/close.
    QThread* m_thread = nullptr; // Владеет engine; worker исполняет run до abort.
    class Worker* m_worker = nullptr; // Создаётся на open, удаляется на close.

    State m_state = Stopped; // Публичное состояние показа, независимо от опережающего декодирования.
    qint64 m_durationMs = 0; // Из метаданных, используется для ограничения seek/слайдера.
    qint64 m_positionMs = 0; // Мс; при pause удерживается, при stop сбрасывается.
    bool m_hasAudio = false; // Есть аудиотрек, для которого ещё не потребовался fallback на системные часы.
    int m_videoWidth = 0; // Исходные размеры видеопотока из opened, сохраняются при stop.
    int m_videoHeight = 0; // Высота в пикселях.

    /* управление worker'ом (атомики, доступны из обоих потоков) */
    QAtomicInt m_abort{0}; // 1 — прекратить run.
    QAtomicInt m_paused{0}; // 1 — удерживать декодирование видео; seek всё равно обрабатывается.
    QAtomicInteger<qint64> m_seekTargetMs{0}; // Цель последнего seek в мс, публикуется перед m_epoch.
    QAtomicInt m_buffered{0}; // Все отправленные, ещё не показанные/отброшенные кадры, включая queued-сигналы Qt.
    QAtomicInt m_epoch{0}; // GUI публикует новый target до release-инкремента; worker обрабатывает каждую последнюю эпоху.

    /* презентация (только GUI-поток) */
    std::deque<std::pair<QImage, qint64>> m_pending; // RGB-кадр + PTS в мс, принятые GUI и ожидающие своих часов.
    QImage m_renderFrame; // Последний представленный кадр, общий снимок для currentFrame.
    bool m_decodeEof = false; // EOF только текущей эпохи; не меняет состояние/возможность паузы.
    qint64 m_videoEndMs = 0; // Максимальный конец принятого кадра: PTS + его длительность.
    qint64 m_drainReportAt = 0; // Последний DEBUG-снимок доигрывания по m_elapsed, мс; не чаще раза в секунду.
    QTimer m_presentTimer; // GUI tick 16мс; работает только во время Playing.
    uint64_t m_audioPlayback = 0; // Принадлежащая engine PCM-сессия SoundManager, 0 — отсутствует.
    int m_audioSampleRate = 0; // Гц принятого PCM; 0 при отсутствии аудио/fallback.
    uint64_t m_audioFrames = 0; // Полная длина PCM на канал; сессия сохраняется после Finished для replay/seek.
    bool m_audioClock = false; // Часы ещё следуют PCM; после его конца продолжаются через m_elapsed.
    QElapsedTimer m_elapsed; // Монотонные часы без аудио либо после его конца; перезапускаются при play/seek.
    qint64 m_elapsedBaseMs = 0; // Медиапозиция, соответствующая нулю m_elapsed, мс.
};
