/* Общие viewers desktop/Android: независимый media cursor и ограниченный батч на session ID.
 * create/destroy — uasync вне callbacks; внешний bridge защищает destroy от параллельных callers.
 * Остальные API потокобезопасны. Один manager занимает stream event callback ядра, заменяя прежний callback;
 * другие владельцы не должны менять его до destroy manager. Instance/uasync живут до destroy.
 * stop снимает только своего reader; медленные/остановленные viewers не задерживают остальных.
 * pull передаёт владение цепочкой records вызывающему: освобождать records_free(). Кольца RS остаются общими. */
#ifndef STREAM_VIEWER_H
#define STREAM_VIEWER_H
#include "stream_media_buffer.h"
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct stream_viewers;
struct stream_viewer_telemetry {
    struct stream_telemetry chain;          /* Худшая входящая цепочка без локального playback-буфера. */
    struct stream_address self; /* Локальный ключ. */
    uint64_t sample_ms;               /* Локальный узел и монотонное время снимка. */
    uint64_t live_lag_ms;                  /* Отставание полного локального видео от PTS автора; нижняя граница отставания показа. */
    int live_lag_valid, live_waiting_key;   /* Позиции известны; предел превышен и свежий общий KEY пока недоступен. */
    char names[STREAM_PATH_MAX + 1][256];   /* Имена строк chain, затем имя локального узла; UTF-8. */
};
#define STREAM_VIEWER_BATCH_RECORDS 16u
enum stream_viewer_kind {
    STREAM_VIEWER_VIDEO = 1, STREAM_VIEWER_AUDIO, STREAM_VIEWER_GAP,
    STREAM_VIEWER_OPENED, STREAM_VIEWER_END, STREAM_VIEWER_CLOSED,
    STREAM_VIEWER_VIDEO_END, STREAM_VIEWER_AUDIO_END
};
struct stream_viewer_record {
    struct stream_viewer_record *next; /* Следующая запись батча; NULL завершает цепочку. */
    enum stream_viewer_kind kind;       /* Тип данных/управляющего события. */
    struct stream_media_format format; /* Формат только VIDEO/AUDIO; у событий нули. */
    uint64_t pts;                /* PTS только этого источника, микросекунды. */
    size_t length;               /* Длина owned payload после структуры. */
    int flags, reason;           /* VIDEO: KEY; CLOSED: stream_close_reason. */
    uint8_t data[];              /* Полный access unit либо один Opus; у событий length=0. */
};
/* 0 — успех, -1 — ошибка; out заранее NULL. */
int stream_viewers_create(struct stream_viewers **out, struct UTUN_INSTANCE *inst);
/* После остановки внешних callers отменить actor, снять callback, закрыть все readers; NULL допустим.
 * Выполняется в uasync вне callbacks; notify здесь вызывается без lock при исключительном владении manager. */
void stream_viewers_destroy(struct stream_viewers *viewers);
/* Уникальный ненулевой ID занят до обработки stop actor, включая завершённый/ошибочный сеанс.
 * group/source/epoch ненулевые; tracks — VIDEO либо VIDEO|AUDIO. 0 — открытие поставлено, -1 — отказ.
 * Открытие асинхронно: OPENED/CLOSED возвращается через pull без предварительного request. */
int stream_viewer_open(struct stream_viewers *viewers, uint64_t id, struct stream_address group, struct stream_address source, uint64_t epoch, unsigned tracks);
/* Асинхронно освободить reader/батч; синхронно разбудить и снять notify. Старый ID/NULL допустимы.
 * После возврата ID ещё может быть занят до исполнения actor; уже полученные records остаются у caller. */
void stream_viewer_stop(struct stream_viewers *viewers, uint64_t id);
/* Owned батч или NULL. Выдача батча погашает demand, включая повторный request до pull.
 * NULL сохраняет ожидающий запрос. Следующие данные требуют нового request; idle не копирует кольцо.
 * VIDEO_END/AUDIO_END следуют за последней записью своего трека; другой трек их не задерживает.
 * END завершает оба входа. GAP/CLOSED отменяют старый батч и требуют немедленного сброса frontend. */
struct stream_viewer_record *stream_viewer_pull(struct stream_viewers *viewers, uint64_t id);
/* Освобождает всю owned цепочку и payload каждой записи; NULL допустим. */
void stream_viewer_records_free(struct stream_viewer_record *records);
/* Позиция и audio_ahead относятся только к этому reader; повторный demand заменяет прежний.
 * position<0 прогревает звук перед KEY; audio_ahead=-1 отключает выдачу звука, иначе 0..60000000 мкс.
 * video — максимум видеокадров, 0..records; records — максимум всех записей, 1..STREAM_VIEWER_BATCH_RECORDS.
 * Батч отдаётся сразу при WAIT/MORE или после бюджета 2ms/256KiB, без ожидания заполнения.
 * Последняя запись может пересечь порог байтов, но не превышает STREAM_MEDIA_MAX_FRAME.
 * 0 — принято, -1 — неизвестный/закрытый ID или ошибка scheduling. */
int stream_viewer_request(struct stream_viewers *viewers, uint64_t id, int64_t position, unsigned video,
                          int64_t audio_ahead, unsigned records);
/* Уведомление о готовом батче/закрытии, под lock manager (кроме destroy): только разбудить worker,
 * не обращаться повторно к manager или API ядра.
 * arg живёт до stop/destroy или снятия callback (notify=NULL); после возврата старый callback уже не выполняется.
 * Уже готовый батч уведомляет сразу. 0 — установлен, -1 — ID отсутствует/остановлен. */
int stream_viewer_set_notify(struct stream_viewers *viewers, uint64_t id, void (*notify)(void *), void *arg);
struct stream_audio_meter;
/* Потокобезопасно получить ссылку на счётчик аудио открытого viewer; NULL до открытия/после закрытия. */
struct stream_audio_meter *stream_viewer_audio_meter(struct stream_viewers *viewers, uint64_t id);
/* Последний снимок сохраняется после END/CLOSED до stop; до первого обновления actor и после resync он нулевой.
 * 0 — получен, -1 — аргументы/ID отсутствует/остановлен, выход не меняется. */
int stream_viewer_status(struct stream_viewers *viewers, uint64_t id, struct stream_media_status *status);
/* Последний снимок цепочки с актуализированным возрастом, до первого снимка нули; 0/-1, ошибка не меняет выход.
 * Чтение раз в 200ms запрашивает обновление в uasync даже при остановке DATA; текущий вызов его не ждёт. */
int stream_viewer_get_telemetry(struct stream_viewers *viewers, uint64_t id, struct stream_viewer_telemetry *telemetry);
/* Очистить ответ и искать ближайший KEY только этого reader. 0 — принято, -1 — отказ. */
int stream_viewer_resync(struct stream_viewers *viewers, uint64_t id);
#ifdef __cplusplus
}
#endif
#endif
