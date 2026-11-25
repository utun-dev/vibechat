/* Общий publisher Android/desktop: принимает полные access units и Opus, публикует uAV2/RS.
 * create/destroy выполняются в uasync вне callbacks, внешний bridge сериализует destroy с producers.
 * open/feed/stop/snapshot потокобезопасны; вызывающие используют session ID, не pointers ядра.
 * Один publisher на контекст. Snapshot содержит состояние и 100ms телеметрию, callbacks не устанавливаются.
 * TX ограничен 2MiB/128 records/250ms; потеря видео требует KEY. Stop завершает очередь и держит FIN 2s.
 * Opus не делится: без видео используется вся ёмкость пачки. После отдельной аудиопачки ожидающее видео
 * получает следующий слот actor; при наличии обоих треков обычная пачка сохраняет место под фрагмент видео.
 * Actor удаляет Opus, не помещающийся даже в отдельную пачку, с WARN; feed подтверждает постановку в очередь. */
#ifndef STREAM_PUBLISHER_H
#define STREAM_PUBLISHER_H
#include "stream_media.h"
#include "stream_buffer.h"
#include "stream_proto.h"
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct stream_publisher;
enum stream_publisher_state { STREAM_PUBLISHER_OPENING = 1, STREAM_PUBLISHER_LIVE, STREAM_PUBLISHER_STOPPING,
    STREAM_PUBLISHER_DRAINING, STREAM_PUBLISHER_ENDED, STREAM_PUBLISHER_FAILED };
struct stream_publisher_status {
    uint64_t id, epoch,       // Session ID caller и эпоха публикации; epoch=0 до успешного открытия
        sample_us,           // Монотонное время последнего 100-мс снимка очередей
        video_bytes,         // Непереданный остаток очереди access units этого publisher
        audio_bytes,         // Байты целых Opus-пакетов его очереди
        stream_bytes,        // Логические RS-байты ожидающих выдач этого запуска
        transport_bytes,     // ETCP-очереди всех соединений instance, включая другие сервисы
        inflight_bytes,      // Их же неподтверждённые байты
        acked_1s,            // Их же подтверждённые за секунду байты
        total_bytes;         // Сумма video/audio/stream/transport/inflight для диагностики
    unsigned connections;    // Число соединений в общем ETCP-снимке instance
    unsigned viewers;        // Зрители по свежим отчётам источнику; без relay и завершённых сессий
    int state, reason, need_key; // stream_publisher_state, stream_close_reason либо 0, запрос нового KEY
    int compressor_enabled, compressor_max_gain_db, compressor_rise_rate; // Снимок общих настроек компрессора
    int audio_enabled;       // Последнее применённое actor состояние; codec эпохи от mute не меняется
    int source_overloaded;   // CPU >80% не менее2с и повторные просрочки собственной очереди; сброс после10с восстановления
    int video_bitrate;       // Рекомендуемый target в бит/с, 0 до первого video_rate; применение подтверждает encoder
    struct stream_buffer_positions buffer; // Позиции получателей в кольце; обновление раз в 500 мс
};
/* out=NULL, inst с работающим stream-контекстом; 0/-1. Inst/uasync живут до destroy. */
int stream_publisher_create(struct stream_publisher **out, struct UTUN_INSTANCE *inst);
/* Снять producers, вызвать в uasync вне stream callbacks: отменить actor/таймеры и освободить publisher; NULL допустим. */
void stream_publisher_destroy(struct stream_publisher *publisher);
/* Ненулевые id/group и корректный format; 0 — поставлен OPENING, -1 — аргументы/busy/scheduling.
 * FEC берётся из настроек actor при фактическом open, результат читается snapshot.
 * После ENDED/FAILED разрешён новый open; снимок предыдущего ID заменяется. */
int stream_publisher_open(struct stream_publisher *publisher, uint64_t id, struct stream_address group, const struct stream_media_format *format);
/* enabled=0/1; producer заглушает свою копию PCM, actor публикует состояние без смены аудиотрека/эпохи.
 * 0 — команда принята в OPENING/LIVE; -1 — старый/закрытый ID, включение без audio codec или scheduling. */
int stream_publisher_set_audio_enabled(struct stream_publisher *publisher, uint64_t id, int enabled);
/* Потокобезопасно задать режим/исходный bitrate и получить текущий target в бит/с, -1 при неверных аргументах/ID.
 * automatic=0/1, 0<minimum<=nominal; повтор не сбрасывает адаптацию. Auto: -20% не чаще5с при перегрузке,
 * после10с CPU<=70% без просрочек +5% nominal каждые5с; ручной режим всегда возвращает nominal.
 * При изменении nominal в Auto сохраняется относительное снижение. Не меняет формат/эпоху и не управляет encoder. */
int stream_publisher_video_rate(struct stream_publisher *publisher, uint64_t id, int automatic, int nominal, int minimum);
/* 1 скопировано в очередь, 0 требуется KEY/перегрузка, -1 ошибка или старый/закрытый session ID.
 * pts — микросекунды, <=INT64_MAX, строго растёт отдельно для video/audio; data заимствован только на вызов.
 * video!=0 — полный access unit до MAX_FRAME, format обязателен; иначе Opus до MAX_OPUS, format/key игнорируются.
 * Новый video revision допускается лишь с KEY, атомарно заменяя очередь видео; audio codec эпохи неизменен.
 * Actor определяет ёмкость при open; слишком крупный Opus удаляет с WARN. Принятые записи также ограничены TTL очереди. */
int stream_publisher_feed(struct stream_publisher *publisher, uint64_t id, int video, uint64_t pts, int key,
                          const uint8_t *data, size_t length, const struct stream_media_format *format);
/* Асинхронно запретить feed, дренировать очередь с её TTL, подписать END и держать owner ещё 2 с.
 * В OPENING отменяет публикацию; повтор/старый ID допустим. После закрытия publisher кольцо живёт при других owners. */
void stream_publisher_stop(struct stream_publisher *publisher, uint64_t id);
/* Скопировать последний снимок, включая ENDED/FAILED: 0/-1. Ошибка не меняет status. */
int stream_publisher_snapshot(struct stream_publisher *publisher, uint64_t id, struct stream_publisher_status *status);
#ifdef __cplusplus
}
#endif
#endif
