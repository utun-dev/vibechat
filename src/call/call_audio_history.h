/* Снимок диагностики звонка за 30с. RX — запас Opus+DSP до выдачи PCM устройству;
 * TX — все логические Router-очереди ядра плюс ожидающее аудио этого звонка.
 * Нижний транспорт показан отдельно: его копии уже входят в Router inflight.
 * Все времена — get_time_us()/1000. UI только читает, скрытие графика не останавливает сбор.
 * Гистограмма описывает запас RX, не одностороннюю задержку сети. Старые интервалы
 * имеют вес exp2(-возраст/10с). Это наблюдение; алгоритм воспроизведения оно не меняет. */
#ifndef CALL_AUDIO_HISTORY_H
#define CALL_AUDIO_HISTORY_H
#include <stdint.h>
#define CALL_HISTORY_MS 30000
#define CALL_BUFFER_LOG_MS 200 /* Общий период сводок uasync; фактический dt выводится при задержке таймера. */
#define CALL_HISTORY_STEP_MS 50
#define CALL_HISTORY_POINTS (CALL_HISTORY_MS / CALL_HISTORY_STEP_MS + 1)
#define CALL_HISTORY_BIN_MS 50
#define CALL_HISTORY_BINS 61 /* 0..2999мс, последний столбец >=3000мс. */
/* Независимый от графиков интервал логирования: все pull, включая стартовое накопление.
 * samples/missing — длительности после старта; low/high действительны при pulls>0. */
struct call_history_interval {
    uint64_t pulls, samples, missing_samples, last_pull_ms;
    int low_ms, high_ms;
    int headroom_low_ms, headroom_high_ms; /* Экстремумы остатка после полной выдачи PCM-блока, мс; при паузе 0. */
};
struct call_history_rx {
    uint64_t at_ms; /* Монотонная отметка последнего измерения интервала. */
    int depth_ms, reserve_ms, tempo_x100;
    int low_ms, high_ms; /* Экстремумы внутри 50мс: короткий провал не исчезает при укрупнении графика. */
    int headroom_low_ms, headroom_high_ms; /* Остаток после полной выдачи PCM-блока; min/max внутри тех же 50мс, при паузе 0. */
    unsigned rendered_samples, missing_samples; /* После первого звука; начальное накопление сюда не входит. */
};
struct call_history_tx {
    uint64_t at_ms, queued_bytes, inflight_bytes, transport_bytes;
    uint64_t peer_id, tx_epoch, wire_tx_bytes; /* Текущий путь звонка: разность счётчиков только при одинаковых peer/epoch. */
    double wire_bytes_per_sec; /* Среднее за последние <=1с; -1 — нет пары измерений одного пути. */
};
struct call_audio_history {
    uint64_t now_ms;
    unsigned rx_count, tx_count;
    struct call_history_rx rx[CALL_HISTORY_POINTS]; /* По возрастанию времени, только последние 30с. */
    struct call_history_tx tx[CALL_HISTORY_POINTS];
    double histogram[CALL_HISTORY_BINS]; /* Доли времени 0..1, с учётом давности. */
    double observed_ms, missing_ms; /* Взвешенные длительности наблюдения и недостающего PCM. */
};
#endif
