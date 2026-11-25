// Общий автомат автопередачи VAD/уровень/VAD+уровень, без I/O.
#ifndef RADIO_AUTO_H
#define RADIO_AUTO_H
#include <stdint.h>

/* ── машина состояний авто-PTT ── */

struct radio_auto_fsm {
    int      talking;            /* идёт автоматическая передача */
    int      confirm_count;      /* подряд окон с value >= start_threshold */
    uint64_t silence_since_tb;   /* начало текущей тишины (0 — не в тишине) */
};

static inline void radio_auto_fsm_reset(struct radio_auto_fsm* f) {
    f->talking = 0;
    f->confirm_count = 0;
    f->silence_since_tb = 0;
}

/* Общее решение VAD/уровня. VAD передаёт одинаковые пороги вероятности.
 * Подтверждение старта — confirm_windows последовательных окон >= start_threshold.
 * Передача заканчивается после hangover_tb ниже stop_threshold; busy гасит её сразу.
 * now — монотонное время в 0.1мс; для уровня это позиция в захваченном PCM.
 * Возврат: 1 = START, -1 = STOP, 0 = без изменений. */
static inline int radio_auto_fsm_update(struct radio_auto_fsm* f, float value, float start_threshold, float stop_threshold,
                                       int confirm_windows, uint64_t hangover_tb,
                                       int busy, uint64_t now) {
    if (busy) {
        /* чужой разговор: старт запрещён, свою передачу гасим сразу */
        f->confirm_count = 0;
        f->silence_since_tb = 0;
        if (f->talking) { f->talking = 0; return -1; }
        return 0;
    }
    if (value >= (f->talking ? stop_threshold : start_threshold)) {
        f->silence_since_tb = 0;
        if (!f->talking) {
            f->confirm_count++;
            if (f->confirm_count >= confirm_windows) {
                f->confirm_count = 0;
                f->talking = 1;
                return 1;   /* старт: подтверждение набрано */
            }
        }
        return 0;
    }
    /* value ниже порога текущего состояния */
    f->confirm_count = 0;
    if (f->talking) {
        if (f->silence_since_tb == 0) f->silence_since_tb = now;
        else if (now - f->silence_since_tb >= hangover_tb) {
            f->talking = 0;
            f->silence_since_tb = 0;
            return -1;  /* стоп: hangover истёк */
        }
    }
    return 0;
}

/* Совместное решение на одном окне PCM: нужны и вероятность речи, и уровень до компрессоров.
 * Параметры подтверждения/отбоя и возврат — как у radio_auto_fsm_update; уровень использует свои on/off пороги.
 * Подтверждения разных условий на разных окнах не объединяются. */
static inline int radio_auto_fsm_update_vad_level(struct radio_auto_fsm* f, float probability, float vad_threshold,
                                                float level_dbfs, float level_on, float level_off, int confirm_windows,
                                                uint64_t hangover_tb, int busy, uint64_t now) {
    int accepted = probability >= vad_threshold && level_dbfs >= (f->talking ? level_off : level_on);
    return radio_auto_fsm_update(f, (float)accepted, 1, 1, confirm_windows, hangover_tb, busy, now);
}

#endif
