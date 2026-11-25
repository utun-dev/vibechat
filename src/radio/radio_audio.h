// radio_audio.h — сервисный аудио-движок рации (единый для desktop/Android/headless).
//
// Владеет Opus encoder (TX), per-source декодерами и микшером (RX). Приём:
// radio.c → radio_set_frame_cb → radio_audio_on_frame (uasync) сохраняет Opus-кадр
// в jitter источника; декодирование и time-stretch выполняются на pull; аудио-поток radio_audio_pull_pcm микширует
// PCM всех источников. Передача: аудио-поток radio_audio_feed_pcm кодирует PCM и
// постит radio_talk_send в uasync. Микшер избыточных источников — насыщающее
// суммирование (soft-clip). Полудуплекс: радио включено — слушаем, PTT — говорим.
//
// Потоки (как call_audio):
//   - radio_audio_start / capture_start / capture_stop / stop — GUI после остановки workers;
//   - radio_audio_feed_pcm / talk_begin / talk_end — один TX-writer, pull_pcm — отдельный RX-writer;
//   - radio_audio_on_frame                                 — uasync-поток (из radio.c).
// Общее состояние — под мьютексом. Синглтон на один активный group_id.

#ifndef RADIO_AUDIO_H
#define RADIO_AUDIO_H

#include <stdint.h>

struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_AUDIO_SAMPLE_RATE   48000
#define RADIO_AUDIO_FRAME_MS      20
#define RADIO_AUDIO_FRAME_SAMPLES (RADIO_AUDIO_SAMPLE_RATE * RADIO_AUDIO_FRAME_MS / 1000)  /* 960 */

/* Зарегистрировать аудио-движок в radio.c (идемпотентно). */
int  radio_audio_init(struct UTUN_INSTANCE* inst);
void radio_audio_destroy(struct UTUN_INSTANCE* inst);

/* GUI-поток: начать прослушивание канала (encoder + реестр источников).
 * capture_channels=0 — только RX; 1/2 сохраняют запуск RX+TX для desktop.
 * capture_channels — каналы захвата (1=моно, 2=стерео): задаёт GUI по фактической
 * возможности устройства (стерео недоступно → откат на моно). RX всегда стерео. */
int  radio_audio_start(uint64_t group_id, int capture_channels);

enum radio_auto_mode { RADIO_AUTO_OFF = 0, RADIO_AUTO_VAD = 1, RADIO_AUTO_LEVEL = 2, RADIO_AUTO_VAD_LEVEL = 3 };

struct radio_audio_input_status {
    float level_dbfs; /* RMS за 20мс после AEC, до компрессоров; тишина = -120dBFS. */
    int valid;       /* Есть PCM не старше 200мс. */
    int blocked;     /* Чужая речь / дренаж вывода / защитная пауза. */
    float vad_probability; /* Вероятность речи 0..1 из последнего успешного окна Silero. */
    int vad_valid;   /* VAD работает, эфир свободен, оценка не старше 200мс. */
    float vad_gain_db; /* Фактическое усиление отдельного компрессора VAD; 0 при выключенном. */
    int vad_compressor_enabled;
};
/* Любой поток: короткий снимок под mutex, без вызовов GUI из аудиопотока. */
struct radio_audio_input_status radio_audio_get_input_status(void);

/* Независимый TX: start(..., 0) включает только RX, захват готовится по фактическому формату микрофона.
 * capture_stop отправляет FIN активной передачи и освобождает TX, сохраняя RX. */
int  radio_audio_capture_start(uint64_t group_id, int capture_channels, int auto_mode);
void radio_audio_capture_stop(void);
/* Единственный TX-writer: завершить старый PCM-хвост, сбросить детектор после потери захвата. */
void radio_audio_capture_discontinuity(uint64_t group_id);

/* GUI-поток: прекратить прослушивание (освободить всё). */
void radio_audio_stop(void);
/* Playback остановлен: удалить устаревшие источники, сохранив TX и подписку. */
void radio_audio_reset_playback(uint64_t group_id);

/* Единственный TX-writer: PTT упорядочен с PCM; сетевые BEGIN/FIN исполняются в uasync. */
void radio_audio_talk_begin(uint64_t group_id);
void radio_audio_talk_end(uint64_t group_id);

/* Единственный TX-writer: очищенные interleaved отсчёты произвольной длины (кратно числу каналов).
 * VAD использует свою копию PCM и отдельный AGC с целью 0.25 и пределом radio_vad_compressor_max_gain_db;
 * общие цель и ограничение роста слышимого TX на калибровку детектора не влияют.
 * Включённый AGC VAD накапливает 20мс; выключенный пропускает PCM сразу. TX не использует обработанную VAD-копию.
 * При включённой автоматике до BEGIN хранится до 600мс непереданного PCM и отправляется перед live.
 * Один автомат manual/auto; хвост каждой серии дополняется тишиной до FIN. */
int  radio_audio_feed_pcm(uint64_t group_id, const int16_t* pcm, int count);
/* Захват без платформенных AEC/NS: ровно 960*channels int16, AEC → NS → уровень/VAD/TX.
 * radio_aec_enabled читается при capture_start; отдельный AEC получает stereo reference из pull_pcm.
 * Его линия задержки 4x20 мс, эхо-хвост 300 мс; подготовленный feed_pcm AEC/NS повторно не применяет.
 * Вход заимствован и не меняется. Возвращает 0/-1 с логом; PTT и feed выполняет один TX-writer. */
int radio_audio_feed_capture_pcm(uint64_t group_id, const int16_t* pcm, int count);
/* Только raw capture: сила -40..-1 dB, 0 полностью обходит NS. Под TX mutex между кадрами.
 * Для очищенного feed_pcm фильтр не создаётся. Возвращает 0/-1 с логом. */
int radio_audio_set_noise_suppression(int suppress_db);

/* аудио-поток: выдать до max_samples микшированного PCM (источники микшируются,
 * источники без данных дают тишину). Возвращает число сэмплов (обычно max). */
int  radio_audio_pull_pcm(uint64_t group_id, int16_t* out, int max_samples);

/* RX-writer: включить учёт вывода вызовом (group, 0, 0) до первого pull.
 * position — число стерео PCM-фреймов из pull, дошедших до устройства (не число int16).
 * delay_tb — оставшееся время до их физического воспроизведения, 0 при подтверждении playback head.
 * Позиции начинаются с нуля после start/reset_playback; устаревшие callbacks отсекает владелец I/O.
 * Автопередача ждёт вывода последнего голоса и ещё 200мс. Без отчётов учитывается только выход pull. */
void radio_audio_playback_progress(uint64_t group_id, uint64_t position, uint64_t delay_tb);

/* Любой поток: 1, если не muted RX требует аппаратный output (pre-roll, речь, FIN-drain/timeout).
 * Также удаляет завершённые/просроченные источники без audio, которые больше не будут обслужены pull.
 * Амплитуда PCM не используется; локальный TX сам по себе output не требует. */
int radio_audio_output_needed(void);

/* 1 = аудио-движок активен (радио канала включено). */
int  radio_audio_active(void);

/* Фактический RADIO_AUTO_OFF/VAD/LEVEL/VAD_LEVEL; ручной PTT имеет приоритет.
 * VAD_LEVEL сопоставляет вероятность VAD с уровнем исходного 32мс окна до компрессоров, с гистерезисом уровня.
 * Все режимы требуют непрерывный capture, общий prefetch 600мс и RX-защиту. */
int  radio_audio_auto_mode(void);

/* Текущее состояние передачи: ручной PTT либо сработавший детектор. */
int radio_audio_transmitting(void);

/* uasync-поток: копируем Opus-кадр в jitter источника; декодирование/растяжение — на pull в RX-потоке. */
void radio_audio_on_frame(struct UTUN_INSTANCE* inst, uint64_t group_id,
                          const uint8_t src_ed[32], uint16_t stream_id, uint16_t seq,
                          uint8_t fin, const uint8_t* opus, int len, void* arg);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_AUDIO_H */
