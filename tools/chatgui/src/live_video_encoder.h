/* FFmpeg live encoder для звонков/трансляций: один worker последовательно вызывает все методы экземпляра.
 * open синхронно пробует реальный KEY и принудительный KEY (если backend умеет), затем открывает чистый контекст.
 * Профили звонка дополнительно проверяются по bitstream/декодированию, включая уровень и отсутствие B-кадров.
 * encode добавляет весь готовый выход в список; задержка допустима. Для профилей звонка сохраняет не более восьми
 * входных PTS и проверяет порядок выхода; QSV использует 90 kHz внутри codec, наружу возвращается capture PTS.
 * KEY data: config_length:4BE + decoder configuration + access unit; остальные — access unit (без заголовка uAV2-блока).
 * close/деструктор отбрасывают задержанный выход; для его получения сначала вызвать encode с пустым image и finish=true. */
#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QList>
#include <memory>
#include <cstdint>

/* Automatic — обычная трансляция; два явных профиля включают строгий bitstream-контракт звонка. */
enum class LiveVideoProfile { Automatic, H264ConstrainedBaseline, H265Main };
struct LiveVideoConfig {
    int width = 1280, height = 720,       /* Чётные размеры кодированного кадра, каждая сторона 16..4096. */
        fps = 30,                        /* Номинальная частота 1..60; encode сам не ограничивает подачу кадров. */
        bitrate = 2500000,               /* Целевой битрейт 64000..16000000 бит/с. */
        codec = 1,                       /* STREAM_MEDIA_H264/H265/VP9/AV1/H266, значения 1..5. */
        rotation = 0;                    /* Метаданные проверки профиля звонка; пиксели здесь не поворачиваются. */
    LiveVideoProfile profile = LiveVideoProfile::Automatic; /* Совместим с codec; явные профили только для H.264/H.265. */
    int level = 0, references = 0, vbvBits = 0; /* AVC/HEVC level_idc, число refs, VBV в битах; для звонка: level>0, refs=1, VBV>0.
                                               * В Automatic: level определяет encoder, refs=0 — его выбор, VBV=0 — bitrate/4. */
    int maxQp = -1;                       /* Потолок QP 0..51 либо -1 (выбор encoder); только H.264/H.265 Automatic. */
    QString backend;                      /* Пусто — перебор доступных; иначе точное имя FFmpeg encoder с учётом Backend. */
    /* Сравнивает все поля, включая выбранный backend. */
    bool operator==(const LiveVideoConfig&) const = default;
};
struct LiveEncodedFrame {
    QByteArray data;                      /* Владеет KEY payload или обычным access unit, максимум STREAM_MEDIA_MAX_FRAME. */
    uint64_t pts = 0;                     /* Исходный capture PTS в микросекундах, <=INT64_MAX. */
    bool key = false;                     /* Фактический KEY по выходному пакету encoder, не копия запроса key. */
};
class LiveVideoEncoder {
public:
    /* Ограничивает список имён encoder: Automatic — GPU/CPU, HardwareOnly — только GPU, SoftwareOnly — только CPU. */
    enum class Backend { Automatic, HardwareOnly, SoftwareOnly };
    LiveVideoEncoder();
    ~LiveVideoEncoder();
    /* Сначала close прежнего контекста, затем валидация config и пробы encoder. true — готовый чистый контекст.
     * false — смотреть error; продолжать encode после неудачного open нельзя. Без FFmpeg всегда false. */
    bool open(const LiveVideoConfig& config, Backend backend = Backend::Automatic);
    /* 64000..16000000 бит/с; true — обновлено для следующего кадра (только libx264 Automatic).
     * false — неверный битрейт либо требуется замена контекста; прежний контекст сохраняется. */
    bool setBitrate(int bitrate);
    /* Динамический QP доступен для H.264/H.265 Automatic: QSV либо AMF с опцией utun_qp_max. */
    bool canSetMaxQp() const;
    /* Запрашивает потолок 0..51 на следующей подаче кадра; false — ошибка/нет поддержки, причина записывается в error. */
    bool setMaxQp(int qp);
    /* Освобождает контекст без flush, очищает имя/ошибку/PTS; повтор безопасен. */
    void close();
    /* Масштабирует image с сохранением пропорций/чёрными полями под config, подаёт capture pts<=INT64_MAX и забирает выход.
     * key — запрос KEY; backend без принудительных KEY (AMF) после первого кадра использует периодический GOP.
     * Пустое image только забирает выход; finish=true требует пустое image и завершает подачу потока (после него close/open).
     * output дополняется, не очищается; true допускает пустой выход. При false вход/часть выхода уже могли быть приняты. */
    bool encode(const QImage& image, uint64_t pts, bool key, QList<LiveEncodedFrame>& output, bool finish = false);
    /* Число принятых, но ещё не выданных входов профиля звонка; Automatic не ведёт эту таблицу и возвращает 0. */
    unsigned pendingFrames() const;
    /* Последняя ошибка; при отказе open — причины всех попыток с именами encoder и сообщениями FFmpeg.
     * Успешный encode/setMaxQp не обязан очищать её. */
    QString error() const;
    /* Имя FFmpeg encoder текущей/последней пробы; после close пусто, само по себе не означает успешный open. */
    QString name() const;
    /* ID кодеков с зарегистрированным encoder указанного Backend; без открытия/проверки GPU, без FFmpeg — пусто. */
    static QList<int> available(Backend backend = Backend::Automatic);
    /* Имя протокольного codec 1..5; неизвестный ID возвращает пустую строку. */
    static QString codecName(int codec);
private:
    struct Impl;
    std::unique_ptr<Impl> m;              /* Единственный владелец codec, кадров, конвертера и таблицы ожидаемых PTS. */
};
