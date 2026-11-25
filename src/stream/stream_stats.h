/* Статистика одной трансляции: отчёт каждые 3с, история автора 5 минут.
 * API ядра — только uasync. read возвращает owned массив узлов, освобождать u_free().
 * Значения sample.v индексируются enum; байты/мс/байты в секунду, CPU в сотых процента.
 * 100% CPU = одно ядро. Недоступные задержка, максимум паузы и CPU обозначаются UINT64_MAX. */
#ifndef STREAM_STATS_H
#define STREAM_STATS_H
#include "stream_proto.h"
#ifdef __cplusplus
extern "C" {
#endif
#define STREAM_STATS_PERIOD_MS 3000u
#define STREAM_STATS_HISTORY_MS 300000u
#define STREAM_STATS_STALE_MS 9000u
#define STREAM_STATS_HISTORY 128u
enum stream_stat_field {
    ST_DT, /* Длительность отчётного интервала. */
    ST_RX, ST_TX, ST_USEFUL, /* Байты/с: входящие части, исходящие части, проверенное содержимое. */
    ST_PAUSES, ST_PAUSE_MS, /* Новые сетевые недостачи звука и суммарная длительность подставленной тишины, мс. */
    ST_PAUSE_MAX, /* Длительность самого длинного наблюдавшегося аудиоглитча, включая продолжающийся; мс либо UINT64_MAX. */
    ST_PAUSED, ST_AGE, /* Аудиоглитч на конец интервала и время с последней полной пачки. */
    ST_LAG, /* Отставание полного видео от известной live-головы, мс либо UINT64_MAX. */
    ST_PROVIDERS, ST_RECEIVERS, ST_IN, ST_OUT, ST_PARTS, ST_REQUIRED, /* Уникальные узлы/части и DATA-выдачи. */
    ST_QUEUE, ST_QUEUE_PARTS, ST_QUEUE_AGE, ST_REMOVED, ST_QUEUE_DELTA, /* Очередь, удалённые байты; DELTA — int64_t изменения байтов в uint64_t wire. */
    ST_GAPS, ST_REJECTED, ST_DUPLICATES, ST_MIGRATIONS, ST_JUMPS, /* События за интервал. */
    ST_CPU, ST_UASYNC, /* CPU процесса за интервал / uasync за секунду; сотые процента. */
    ST_NAT, /* 0 неизвестно, 1 прямой IP, 2 EIM, 3 strict; лучший подтверждённый тип. */
    ST_RELAY, ST_PHASE, ST_FINISHED, ST_OBSERVED, /* Роль, фаза, конец, наблюдаемое время после первой пачки. */
    ST_AUDIO_OBSERVED, /* Время измеренного аудиовыхода, мс, суммарно для активных плееров узла. */
    STREAM_STAT_FIELDS
};
struct stream_stat_sample {
    uint64_t at_ms, sequence, generation; /* Часы автора при приёме, номер отчёта и поколение отправителя. */
    uint64_t v[STREAM_STAT_FIELDS]; /* Поля enum stream_stat_field. */
};
struct stream_stat_node {
    uint64_t now_ms; /* Часы снимка; после закрытия публикации заморожены на последнем отчёте. */
    struct stream_address node; /* Ed25519 отправителя. */
    char name[256]; /* Owned UTF-8 имя либо ID. */
    unsigned count; /* История oldest-first, не более STREAM_STATS_HISTORY. */
    struct stream_stat_sample samples[STREAM_STATS_HISTORY];
};
struct UTUN_INSTANCE;
/* Скопировать историю точной эпохи автора; out заранее NULL, count обнуляется; OK/ERR_*. */
int stream_stats_read(struct UTUN_INSTANCE *inst, uint64_t group, uint64_t epoch,
                      struct stream_stat_node **out, size_t *count);
/* Число зрителей точной эпохи автора по отчётам моложе 9с; relay и завершённые исключены.
 * Только uasync; при неверных inst/group/epoch пишет ошибку и возвращает 0. Историю не копирует. */
unsigned stream_stats_viewer_count(struct UTUN_INSTANCE *inst, uint64_t group, uint64_t epoch);
/* Внутренний учёт: номера ST_RX/ST_TX/ST_REMOVED задают байты; прочие события — счётчик. */
void stream_stats_add(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, unsigned field, uint64_t value);
#ifdef __cplusplus
}
#endif
#endif
