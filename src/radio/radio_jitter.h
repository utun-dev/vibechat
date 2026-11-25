#ifndef RADIO_JITTER_H
#define RADIO_JITTER_H

// radio_jitter.h — адаптивный джиттер-буфер голоса с time-stretch (SoundTouch), C API.
//
// Хранит КОДИРОВАННЫЕ (Opus) кадры: uasync-поток кладёт кодированные кадры,
// аудио-поток на pull декодирует их (через коллбэк decode) и пропускает через
// SoundTouch с плавно меняющимся темпом. Излишек голоса держится до декодирования —
// декодируется только то, что готово к воспроизведению (экономия памяти ~48x и
// разгрузка uasync-потока).
//
// Потоки:
//   - rj_push      — uasync-поток (producer);
//   - rj_pull      — аудио-поток (consumer; единственный владелец decode/SoundTouch);
//   - rj_get_stats — любой поток.
// Кольцо кодированных кадров защищено мьютексом (SPSC, дёшево: ~50 кадров/с).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rj;

/* decode: закодированный кадр → PCM. Возвращает число декодированных сэмплов. */
typedef int (*rj_decode_fn)(void* arg, const uint8_t* enc, int len, int16_t* pcm, int max_samples);

/* Полная конфигурация адаптивного джиттер-буфера. */
struct rj_cfg {
    int    sample_rate;
    int    channels;
    int    frame_samples;   /* сэмплов в кадре (960 @20мс/48k) */
    int    frame_ms;        /* длительность кадра в мс */
    int    max_frames;      /* ёмкость кодированных кадров (50 = 1000мс) */
    double target_ms;       /* целевая глубина буфера */
    double min_tempo;       /* темп при пустом буфере (<1.0 = замедление, накопление) */
    double max_tempo;       /* темп при глубоком буфере (>1.0 = догон live) */
    int    pre_roll;        /* 1 = не отдавать звук, пока глубина < target_ms */
    rj_decode_fn decode;
    void* decode_arg;
};

/* Создать буфер с полной конфигурацией (pre-roll, симметричный темп). */
struct rj* rj_create_cfg(const struct rj_cfg* cfg);

/* Создать буфер с конфигурацией по умолчанию (без pre-roll,
 * темп 1.0..1.6, target 60мс). frame_samples — сэмплов в кадре (960 @20мс/48k). */
struct rj* rj_create(int sample_rate, int channels, int frame_samples, int frame_ms,
                     int max_frames, rj_decode_fn decode, void* decode_arg);

void rj_destroy(struct rj* j);

/* producer: положить кодированный кадр (копируется). При переполнении дропает старейший. */
void rj_push(struct rj* j, const uint8_t* enc, int len);

/* consumer: выдать до max_samples декодированного+растянутого PCM. Может вернуть
 * < max_samples при нехватке данных (андерфлоу). */
int rj_pull(struct rj* j, int16_t* out, int max_samples);

/* audio-поток: вход закончен (FIN) — принудительно снять pre-roll, чтобы дренировать
 * накопленный остаток до конца. Безопасно вызывать многократно. */
void rj_end(struct rj* j);

/* Диагностика: глубина буфера (мс), темп (×100, 100 = 1.0x), дропнуто кадров
 * при переполнении, число underrun-ов (pull вернул меньше запрошенного). */
void rj_get_stats(struct rj* j, int* depth_ms, int* tempo_x100, uint32_t* dropped, uint32_t* underruns);

/* Число дропнутых кадров при переполнении (для диагностики/тестов). */
uint32_t rj_dropped(struct rj* j);

/* Число хард-ресинков к live (после сталла аудио-девайса) — диагностика. */
uint32_t rj_resyncs(struct rj* j);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_JITTER_H */
