/* Захват камеры/экрана/окна в отдельном процессе. GUI управляет только IPC и собственными CPU-кадрами.
 * Все методы вызываются в GUI при работающем event loop. stop и деструктор не ждут аппаратное закрытие;
 * независимая сессия контролирует выход worker и завершает его при таймауте. main обрабатывает
 * --video-capture-worker до конфига, БД и single-instance. Поколение отсекает старые кадры/ошибки. */
#pragma once
#include <QObject>
#include <QImage>
#include <QList>
#include "stream_video_modes.h"

enum class VideoCaptureKind : quint8 { Camera, Screen, Window };
/* Только значения: QScreen/QCapturableWindow и аппаратные handles остаются в worker. */
struct VideoCaptureDevice {
    QString name; // Отображаемое имя устройства.
    QByteArray id; // Непрозрачный идентификатор Qt backend.
    QList<StreamVideoMode> modes; // Режимы камеры; Call ограничен Full HD, Stream — 4096 по каждой стороне.
    VideoCaptureKind kind = VideoCaptureKind::Camera; // Тип источника; id окна действует в текущем процессе worker.
    QSize size; // Исходные размеры экрана, пусто до выбора portal; камера использует modes.
};
using VideoCaptureDevices = QList<VideoCaptureDevice>;
constexpr qint64 VideoCaptureMaxFrameAgeUs = 250000; // Предел возраста кадра в микросекундах при конвертации и приёме GUI.

struct VideoCaptureConfig {
    bool enabled = false; // false останавливает источник, сохраняя процесс для опроса устройств.
    QByteArray device; // Точный id из devicesChanged; автоматическая подмена источника запрещена.
    int width = 640, height = 480, fps = 30; // Камера: аппаратный режим; экран: предел размеров IPC и частоты кадров.
    VideoCaptureKind kind = VideoCaptureKind::Camera;
    bool cursor = true; // Курсор screen; portal применяет значение только при открытии сессии.
    bool operator==(const VideoCaptureConfig&) const = default;
};

class VideoCapture : public QObject {
    Q_OBJECT
public:
    enum class Usage { Call, Stream }; // Call перечисляет камеры Full HD; Stream также экраны/окна и размеры до 4096.
    /* Создаёт сессию и откладывает запуск worker до event loop; без configure только опрашивает устройства. */
    explicit VideoCapture(QObject* parent = nullptr, Usage usage = Usage::Call);
    /* Отвязывает GUI-владельца и запрашивает асинхронный Quit; сессия сама дождётся выхода/убьёт зависший worker. */
    ~VideoCapture() override;
    /* Call принимает камеры Full HD, Stream — камеры/экраны/окна до 4096. FPS 1..60 ограничивает выдачу.
     * Параметры проверяются и при enabled=false; повтор конфигурации без ошибки ничего не меняет.
     * После failed enabled=true допускает перезапуск; во время stop конфигурация отклоняется с WARN. */
    void configure(const VideoCaptureConfig& config);
    void stop(); // Запросить выход процесса; deadline 1.5 с не продлевается сообщениями worker. Повтор безопасен.
    bool closing() const; // true до подтверждённого выхода процесса после stop; GUI никогда не ждёт его синхронно.
    void refresh(); // Асинхронный опрос источников в worker; без capture-процесса запускает его заново.
signals:
    /* Полный список устройств/режимов после опроса и при изменениях backend, независимо от enabled. */
    void devicesChanged(const VideoCaptureDevices& devices);
    /* Текущий свежий RGB32-кадр в собственной CPU-памяти GUI; image можно сохранить копированием QImage.
     * pts — монотонное время захвата в микросекундах (шкала call_video_playout_now_us), correction — накопленное
     * уменьшение опережения исходных PTS. Ack worker отправлен до сигнала, mapping больше не удерживается. */
    void frameReady(const QImage& image, qint64 pts, qint64 correction);
    /* Ошибка запуска/IPC/драйвера/параметров или watchdog: процесс останавливается, захват отключается.
     * Watchdog: отсутствие прогресса или кадров камеры около 5 с; неподвижному экрану достаточно heartbeat.
     * stop контролирует фактический выход процесса с deadline 1.5 с, проверка каждые 250 мс. */
    void failed(const QString& error);
    void stopped(); // Процесс фактически вышел, включая уничтожение Qt/TLS; перед этим повторный start запрещён.
private:
    struct Session;
    Session* m; // Независимый QObject с QPointer на владельца; после shutdown живёт до завершения worker/event loop.
};

/* Entry point дочернего процесса: argc=4, argv[2] — endpoint, argv[3] — call/stream; создаёт QGuiApplication.
 * Вызывается до обычной инициализации приложения, конфига, БД и single-instance lock.
 * Возвращает код event loop; без HAVE_STREAM_CAPTURE или с неверным argc — 2. */
int video_capture_worker_main(int argc, char** argv);
