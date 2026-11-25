/* Упаковка Opus звонка и выбор размера пачки по Router enqueue->ACK. Views заимствуют входной буфер.
 * Владелец снимает статистику каждые 50мс. Отправка ждёт target кадров И готовности Router; deadline нет. */
#ifndef CALL_AUDIO_BATCH_H
#define CALL_AUDIO_BATCH_H
#include "call_proto.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct call_audio_frame_view {
    struct call_pkt_audio_frame header; /* Исходные seq/PTS/length; payload не копируется. */
    const uint8_t *opus;                /* Не менее length байт до конца encode/потребления view. */
};
struct call_audio_batch_view {
    struct call_pkt_media header;       /* Общие поля; count определяет число frames. */
    struct call_audio_frame_view frames[CALL_AUDIO_BATCH_MAX];
};
struct ETCP_ROUTER_TX_STATS;
/* Снимок владельца: локальная очередь до Router и идентичность выбранного транспорта. NULL в step — нулевой снимок. */
struct call_audio_batch_input {
    uint64_t path_node_id, path_epoch; /* Next hop и эпоха ETCP_CONN; смена сбрасывает проверку прежнего пути. */
    uint64_t oldest_us; /* Возраст первого кадра FIFO; намеренное накопление target кадров вычитается в регуляторе. */
    unsigned frames; /* Количество кадров FIFO: неполная пачка не доказывает backpressure. */
};
struct call_audio_batch_control {
    uint64_t local_epoch, peer_epoch, last_us, high_since_us, low_since_us, next_change_us;
    uint64_t path_node_id, path_epoch; /* Транспорт, для которого накоплены измерения и результаты проб. */
    uint64_t changed_ack_packets; /* После изменения ждём новых подтверждений, не реагируем повторно на старый снимок. */
    uint64_t delay_us; /* max полного RTT и возраста Router-очередей; локальное накопление не входит. */
    uint64_t reference_us; /* Доставка до пробы/последнего роста target; очередь не поднимает эту базу вслед за собой. */
    uint64_t probe_after_us, probe_backoff_us; /* Запрет следующей пробы и её backoff 30..120с. */
    unsigned target; /* Необходимое число кадров, 1..10; старт с 4. */
    unsigned probe_previous; /* Размер до проверяемого уменьшения; 0 — нет незавершённой пробы. */
    int feedback_received; /* Первый пригодный ACK в текущей Router epoch. */
    int high, low; /* Гистерезис роста/снижения target. */
};
/* Encode/parse: 0/-1. Проверяют весь пакет до изменения out/length/view; ошибки логируются.
 * Encode не допускает перекрытие output с Opus; capacity >= вычисленного размера, максимум 1024. */
int call_audio_batch_encode(const struct call_audio_batch_view *batch, uint8_t *out, size_t capacity, size_t *length);
int call_audio_batch_parse(const uint8_t *wire, size_t length, struct call_audio_batch_view *out);
/* Начальное target=4; новый звонок или смена Router epoch сбрасывает историю. */
void call_audio_batch_init(struct call_audio_batch_control *control, uint64_t now_us);
/* NULL net — канал отсутствует; без свежих ACK/при блокировке рост запрещён, незавершённая проба откатывается.
 * input — локальная FIFO/путь либо NULL; 0/-1 с логом. Вызывать каждые50мс в uasync. */
int call_audio_batch_step(struct call_audio_batch_control *control, uint64_t now_us,
                         const struct ETCP_ROUTER_TX_STATS *net, const struct call_audio_batch_input *input);
#ifdef __cplusplus
}
#endif
#endif
