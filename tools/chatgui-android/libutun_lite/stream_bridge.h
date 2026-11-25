/* Android-очереди live media: attach/detach только в uasync, вне callbacks.
 * UI/кодировщики используют только session_id, никогда не сохраняют inst/ua.
 * Один publisher и независимые viewers, TX <=2MiB/128 records/250мс, RX — ограниченный батч на viewer.
 * Data принимает полные access units пяти кодеков uAV2 (KEY: config_length:4/config/frame) и Opus 48k mono/stereo/20ms.
 * pull передаёт u_malloc-буфер последовательных записей:
 * kind:1,flags:1,width:2,height:2,rotation:2,PTS_us:8,length:4,revision:4,codec:1,reserved:3,data.
 * kinds: video=1,audio=2,gap=3,opened=4,ended=5,error=6,video_end=7,audio_end=8; error.flags = stream_close_reason.
 * Конец трека следует за его media; GAP/error отменяют старые записи. Числа big-endian.
 * Медиаданные передаются через pull, не через общий чатовый event bus; полученный буфер освобождает caller через u_free.
 * Команды/снимки защищены bridge mutex, signal имеет свой mutex; callers используют API из frontend/media workers.
 * После detach команды получают отказ/пустой результат, read_request с допустимыми параметрами возвращает 0. */
#ifndef ANDROID_STREAM_BRIDGE_H
#define ANDROID_STREAM_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include "../../../src/stream/stream_buffer.h"

struct UTUN_INSTANCE;
struct stream_media_format;
struct stream_media_status;
struct stream_viewer_telemetry;
struct android_stream_signal;
#define ANDROID_STREAM_HEADER_SIZE 28
struct android_stream_tx_stats {
    /* ID сеанса, время снимка в мкс; video/audio/stream — очереди publisher, transport/inflight/acked_1s — всего instance.
     * total_bytes — сумма video/audio/stream/transport/inflight; байты ACK за 1с в неё не входят. */
    uint64_t id, sample_us, video_bytes, audio_bytes, stream_bytes, transport_bytes, inflight_bytes, acked_1s, total_bytes;
    /* Число соединений в общем ETCP-снимке. */
    unsigned connections;
    int source_overloaded, video_bitrate; /* Перегрузка собственной публикации и рекомендуемый bitrate, бит/с. */
    /* Позиции получателей в кольце, обновляются раз в 500мс. */
    struct stream_buffer_positions buffer;
};
/* Снимок publisher обновляется в uasync каждые 100мс; getter копирует под mutex. 0/-1, ошибка не меняет stats. */
int android_stream_tx_snapshot(uint64_t id, struct android_stream_tx_stats *stats);
/* 0 attached, -1 invalid/busy: нельзя передать singleton другому ядру до detach старого. */
int android_stream_attach(struct UTUN_INSTANCE *inst);
/* Тот же inst: отменить watch, закрыть managers/сеансы и снять привязку; повтор допустим. */
void android_stream_detach(struct UTUN_INSTANCE *inst);
/* id/group ненулевые. source=0 — publisher: width/height=1..4096, rotation=0/90/180/270, codec=H264..H266.
 * source!=0 — viewer с обязательным expected_epoch; размеры/rotation игнорируются. codec проверяется в обоих режимах.
 * audio включает Opus stereo у publisher либо получение аудио viewer. 0 — команда принята, -1 — отказ; результат через pull.
 * Publisher ID занят до получения ended/error через pull; viewer ID — до обработки stop в uasync. */
int android_stream_open(uint64_t id, struct stream_address group, struct stream_address source, unsigned width, unsigned height,
                        unsigned rotation, int audio, uint64_t expected_epoch, unsigned codec);
/* Наблюдение канала через CHAT_EVT_STREAM_SOURCES; нулевой ключ снимает его. 0 — поставлено, -1 — отказ;
 * несколько команд до исполнения сливаются в последнюю. */
int android_stream_watch(struct stream_address group);
/* Publisher: 0 — команда mute/unmute принята, -1 — нет сеанса/аудиотрека; codec/эпоха не меняются.
 * Producer сам заглушает PCM; эта команда только публикует состояние. */
int android_stream_set_audio_enabled(uint64_t id, int enabled);
/* Режим и исходный bitrate publisher; возвращает текущий target, бит/с, либо -1. 0<minimum<=nominal. */
int android_stream_video_rate(uint64_t id, int automatic, int nominal, int minimum);
/* Publisher: pts — мкс <=INT64_MAX, строго растёт отдельно по трекам; data копируется на вызове.
 * Видео требует format с revision/размером/поворотом; новая revision принимается атомарно с KEY.
 * Для аудио format/key игнорируются. 1 — принято в очередь, 0 — drop (video: запросить KEY), -1 — отказ. */
int android_stream_feed(uint64_t id, int video, uint64_t pts, int key, const uint8_t *data, size_t length,
                        const struct stream_media_format *format);
/* Асинхронный stop: publisher дренирует очередь и держит FIN 2с; viewer снимает reader и notify.
 * Уже выданный pull-буфер остаётся у caller. Старый/нулевой ID допустим. */
void android_stream_stop(uint64_t id);
/* Передать owned батч; length обязателен, NULL/length=0 — сейчас нет данных либо ошибка.
 * Publisher выдаёт opened/ended/error без demand; viewer — также media по read_request. */
uint8_t *android_stream_pull(uint64_t id, size_t *length);
/* Один батч на запрос: до video кадров и records записей, звук до position_us+audio_ahead_us.
 * video=0..records, records=1..16; audio_ahead_us=-1 выключает звук, иначе 0..60000000; position_us<0 — прогрев до KEY.
 * Повторный demand заменяет прежний. 0 — параметры допустимы (даже для неизвестного ID/после detach), -1 — неверные параметры.
 * CLOSED/END доступны pull после закрытия cursor; указатели кольца остаются в uasync. */
int android_stream_read_request(uint64_t id, int64_t position_us, unsigned video, int64_t audio_ahead_us, unsigned records);
/* Своя ссылка на счётчик аудио viewer, NULL без открытого аудиокурсора. */
struct stream_audio_meter *android_stream_audio_meter(uint64_t id);
/* Viewer-снимки: 0 — скопировано, -1 — неверные аргументы/ID; ошибка не меняет выход. */
int android_stream_get_status(uint64_t id, struct stream_media_status *status);
int android_stream_get_telemetry(uint64_t id, struct stream_viewer_telemetry *telemetry);
/* Viewer: очистить старый батч и искать ближайший KEY; 0 — принято, -1 — отказ. */
int android_stream_resync(uint64_t id);
/* Worker владеет signal; create привязывает его к открытому viewer. Готовность/stop/detach будят wait.
 * signal также будит worker после расходования PCM. Уведомления сливаются, но не теряются до wait.
 * create возвращает NULL при ошибке. На viewer допускается один signal/notify.
 * destroy — только после stream_stop или detach и остановки всех внешних signal/wait callers. */
struct android_stream_signal *android_stream_signal_create(uint64_t id);
void android_stream_signal_notify(struct android_stream_signal *signal);
/* 1 — уведомление, 0 — deadline, -1 — ошибка; timeout_ms 0..50, ожидание не держит mutex ядра/bridge. */
int android_stream_signal_wait(struct android_stream_signal *signal, unsigned timeout_ms);
void android_stream_signal_destroy(struct android_stream_signal *signal);

#endif
