/* Относительное отставание одного субпотока от одного поставщика, без NTP и сокетов.
 * now_ms — монотонные часы получателя; timestamp — неизменяемые 16-битные часы источника.
 * Первый блок/разрыв устанавливает новую базу, история проблем остаётся за последние 60 секунд.
 * Контекст без динамических ресурсов: init → observe/poll → end, один uasync-поток.
 * DUPLICATE не меняет ни часы, ни статистику. Для другой эпохи требуется новый init. */
#ifndef STREAM_METRICS_H
#define STREAM_METRICS_H

#include "stream_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_METRIC_SECONDS 60

struct stream_metric_bucket {
    uint64_t second; // Абсолютная локальная секунда, которой принадлежит слот кольца.
    uint64_t blocks; // Новые наблюдённые seq, включая установки базы.
    uint64_t late_blocks; // Последовательные блоки с вычисленным lag>500мс.
    uint64_t stops; // Эпизоды остановки, отнесённые к секунде превышения порога.
    uint64_t bytes; // Байты только последовательных пар без rebase, для расчёта rates.
    uint64_t source_ms; // Сумма16-битных временных дельт этих пар.
    uint64_t receive_ms; // Сумма локальных дельт этих пар.
    uint64_t intervals; // Число последовательных пар, включая пары с source_delta0.
    int valid; // Слот заполнен и относится именно к second.
};

struct stream_metrics {
    struct stream_metric_bucket buckets[STREAM_METRIC_SECONDS]; // Секундная история, без allocation.
    uint64_t last_seq; // Последний принятый seq; повторы/меньшие seq игнорируются.
    uint64_t last_receive_ms; // Время последнего нового seq в локальных монотонных мс.
    uint64_t lag_ms; // Накопленное относительное отставание на момент последнего блока.
    uint64_t expected_interval_ms; // Изначально100мс; при ненулевой source_ms — среднее текущей секунды, минимум1мс.
    uint16_t last_timestamp_ms; // Последние часы источника по модулю65536мс.
    int initialized; // Было хотя бы одно успешное observe.
    int stopped; // Один текущий эпизод: пауза>expected_interval+500мс.
    int ended; // END принят: observe запрещён, ожидание новых блоков остановлено.
};

struct stream_metric_snapshot {
    uint64_t lag_ms; // Текущий lag с учётом незавершённого ожидания; после END не растёт от времени.
    uint64_t blocks; // Число новых seq за живую часть60-секундной истории.
    uint64_t late_blocks; // С lag>500мс в той же истории.
    uint64_t stops; // Число эпизодов остановки в той же истории.
    uint64_t source_rate_bytes_sec; // Общие bytes / общая длительность источника последовательных пар.
    uint64_t receive_rate_bytes_sec; // Те же bytes / общая длительность приёма последовательных пар.
    uint64_t expected_interval_ms; // Последняя оценка периода источника.
    uint64_t progress_age_ms; /* Монотонный возраст последнего реально принятого блока; известен при успешном poll. */
    int stopped; // Сейчас отсутствуют новые блоки сверх порога; END сбрасывает.
    int source_rate_valid; // Есть ненулевая суммарная длительность source_ms.
    int receive_rate_valid; // Есть ненулевая суммарная длительность receive_ms.
};

/* Обнулить историю/состояние и установить период source flush100мс. metrics обязателен. */
void stream_metrics_init(struct stream_metrics *metrics);
/* seq!=UINT64_MAX, bytes>0, flags только TIME_RESET; время не раньше последнего принятого блока.
 * OK — новый блок, DUPLICATE — seq<=последнего, ERR_ARGUMENT/STATE — неверный вызов/ended/регресс времени.
 * Первый блок, разрыв seq, TIME_RESET или пауза>=65536мс меняют базу lag, не стирая историю проблем. */
int stream_metrics_observe(struct stream_metrics *metrics, uint64_t seq, uint16_t timestamp_ms, uint8_t flags,
                           uint32_t bytes, uint64_t now_ms);
/* Также обнаруживает полную остановку; episode учитывается в момент превышения порога, не на каждом poll.
 * До первого блока возвращает WAIT и обнулённый snapshot; при ошибке snapshot остаётся прежним. */
int stream_metrics_poll(struct stream_metrics *metrics, uint64_t now_ms, struct stream_metric_snapshot *snapshot);
/* Запретить observe и сбросить stopped; история продолжает стареть, poll остаётся доступен. NULL — ошибка с логом. */
void stream_metrics_end(struct stream_metrics *metrics);

#ifdef __cplusplus
}
#endif
#endif
