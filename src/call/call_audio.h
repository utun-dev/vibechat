#ifndef CALL_AUDIO_H
#define CALL_AUDIO_H

// call_audio.h — общий аудио-движок P2P-звонка для frontend с захватом/воспроизведением (desktop/Android).
// TCP-сервис готового PCM имеет собственные кодеки и описан отдельно в call_headless.h.
//
// Владеет Opus codec, адаптивным джиттер-буфером (call_jitter: кодированные
// кадры + SoundTouch time-stretch на pull) и тонами (глитч при сталле / завершение).
// Излишек голоса держится до декодирования — декодируется только то, что готово
// к воспроизведению. Потребитель (desktop miniaudio / Android AudioTrack) получает
// финальный PCM через call_audio_pull_pcm и отдаёт захваченный PCM через
// call_audio_feed_pcm.
//
// Потоки:
//   - call_audio_init / destroy               — uasync-поток, lifecycle ядра;
//   - call_audio_start / begin_end / release — GUI-поток (по ACCEPTED/ENDED);
//   - call_audio_feed_pcm / pull_pcm            — по одному аудио-потоку на направление;
//   - call_audio_on_media / get_stats           — uasync-поток (из call.c).
// Mutex защищает привязку, TX и снимки общих указателей. Encoder используется capture worker,
// decoder — playback worker (из call_jitter_pull через callback), call_jitter_push — uasync.
// Pull использует снимок указателей вне mutex: перед release/сменой ID владелец останавливает оба I/O-направления.

#include <stdint.h>
#include "call_jitter.h"

struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

#define CALL_AUDIO_SAMPLE_RATE   48000
#define CALL_AUDIO_FRAME_MS      20
#define CALL_AUDIO_FRAME_SAMPLES (CALL_AUDIO_SAMPLE_RATE * CALL_AUDIO_FRAME_MS / 1000)  /* 960 */

/* uasync-поток: привязать общий для процесса движок к inst, допустимо до запуска чата.
 * Регистрация принадлежит ядру и сохраняется при stop/start чата: повторный init не требуется.
 * Повтор для того же inst идемпотентен: не меняет эпоху владельца и занятые TX-слоты.
 * Перед освобождением inst/ua остановить I/O, снять привязку через destroy в uasync.
 * Лимит8 TX-задач относится к текущей привязке, возраст до отправки в call.c ограничен200мс.
 * Задачи прежних привязок не отправляют медиа и не меняют
 * её счётчик/позднюю статистику; uasync освобождает их после callback либо при destroy. 0 = успех, -1 = нет inst. */
int  call_audio_init(struct UTUN_INSTANCE* inst);
/* uasync-поток: снять регистрацию inst; текущий владелец также инвалидирует TX-привязку.
 * Поздний detach прежнего inst не затрагивает нового владельца. Codec/jitter освобождаются отдельно через stop/release. */
void call_audio_destroy(struct UTUN_INSTANCE* inst);

/* GUI-поток: создать codec + jitter + tones. Битрейт берётся из opus_codec_preset; runtime-set обновляет его без restart.
 * Старый API принимает raw PCM и использует core AEC.
 * Нужен привязанный через init живой inst; без него -1. Для desktop нужен start_prepared:
 * AEC и stereo reference принадлежат VoiceAudioIo. Повтор активного ID сохраняет DSP;
 * смена raw/prepared для него отклоняется. До смены ID остановить оба I/O-направления. 0 = успех, -1 = ошибка. */
int  call_audio_start(uint64_t call_id);
/* Контракт start, но AEC принадлежит платформенному захвату; pull не подаёт собственный референс. */
int call_audio_start_prepared(uint64_t call_id);

/* GUI-поток: по CALL_ENDED прекратить TX, pull_pcm выдаст450мс тона, затем тишину.
 * Сам движок не освобождается: frontend отдельно останавливает I/O и вызывает release. */
void call_audio_begin_end(uint64_t call_id);

/* GUI-поток (после остановки аудиоустройства): освободить все объекты. */
void call_audio_release(uint64_t call_id);

/* GUI-поток: остановить текущий активный звонок (удобная обёртка release). */
void call_audio_stop(void);

/* Capture worker: ровно960 raw-сэмплов; capture_pts_us — монотонная метка первого либо INT64_MIN.
 * Encoder работает синхронно, PCM не сохраняется; отличие известного PTS от ожидаемого более2мс меняет capture_epoch.
 * 0 = Opus поставлен в uasync, -1 = ошибка/отказ очереди; постановка не подтверждает доставку пиру.
 * Для mute использовать feed_pcm_muted с настоящим микрофоном, не занулять raw-вход до AEC/NS/AGC. */
int call_audio_feed_pcm(uint64_t call_id, const int16_t* pcm, int count, int64_t capture_pts_us);
/* Подготовленный PCM и обязательная маска mute (960 элементов); AEC/NS уже выполнены до этого API.
 * Маска применяется после core AGC, результат/PTS — как у feed_pcm. */
int call_audio_feed_prepared_pcm(uint64_t call_id, const int16_t* pcm, const uint8_t* mute_mask, int64_t capture_pts_us);
/* Raw capture: AEC -> NS -> AGC получают настоящий PCM; muted!=0 зануляет только вход Opus после обработки.
 * Android передаёт исходный кадр и snapshot mute через JNI; результат/PTS — как у feed_pcm. */
int call_audio_feed_pcm_muted(uint64_t call_id, const int16_t* pcm, int count, int muted, int64_t capture_pts_us);

/* аудио-поток: выдать max_samples финального PCM (jitter + stretch + тоны + тишина).
 * Активный движок всегда выдаёт полный блок; 0 означает отсутствие движка/ошибку аргументов. */
int  call_audio_pull_pcm(uint64_t call_id, int16_t* out, int max_samples);

/* Очередной адаптер использует render_pcm: до первого речевого PCM возвращается 0,
 * без продвижения PCM-шкалы. Адаптер готовит свою очередь перед разрешением вывода.
 * После старта блок полный, с маскирующей тишиной/тонами при паузе; speech_samples
 * отличает речь от заполнения. pull_pcm выше — непрерывный вывод (Android AudioTrack):
 * начальная тишина занимает реальные позиции устройства и учитывается в шкале.
 * Оба API принадлежат одному playback worker; смешивать их в одной сессии нельзя. */
enum call_audio_render_phase { CALL_AUDIO_WAITING, CALL_AUDIO_SPEECH, CALL_AUDIO_RECOVERING, CALL_AUDIO_ENDING };
struct call_audio_render { enum call_audio_render_phase phase; int speech_samples; };
int call_audio_render_pcm(uint64_t call_id, int16_t* out, int max_samples, struct call_audio_render* state);

/* uasync-поток (из call.c): кадр текущего inst/активного DSP передаётся в call_jitter_push;
 * данные копируются, декодирование на pull. ts_ms — время пачки; packet_start=1 только на первом кадре. Чужой inst/ID или отсутствие DSP — no-op. */
void call_audio_on_media(struct UTUN_INSTANCE* inst, uint64_t call_id,
                         uint16_t seq, uint32_t ts_ms, const uint8_t* opus, int len, const struct call_capture_time* timing, int packet_start);

/* Playback worker: номер PCM-фрейма и время его вывода, epoch берётся из get_clock после start/reset.
 * UI/video: get_clock возвращает 0 для существующего звонка; valid внутри снимка может быть 0.
 * now_us/at_us — локальные монотонные часы, та же база, что и callback аудиоустройства. */
void call_audio_playback_progress(uint64_t call_id, uint64_t epoch, uint64_t position,
                                  int64_t at_us, int64_t now_us, int measured);
int call_audio_get_clock(uint64_t call_id, int64_t now_us, struct call_clock_snapshot* out);
/* Планировщик: 1 — снимок получен (valid может быть 0), 0 — одна из блокировок занята,
 * -1 — звонка нет. out обязателен. BUSY не означает invalid и не сдвигает видеорасписание. */
int call_audio_try_clock(uint64_t call_id, int64_t now_us, struct call_clock_snapshot* out);
/* Playback worker: старый аппаратный якорь недействителен до progress нового выхода. PCM/JB сохраняются. */
void call_audio_suspend_device(uint64_t call_id, uint64_t epoch);
/* Любой поток: диагностический снимок без потребления событий CALL_STATS.
 * out обязателен; 0 = снимок получен, -1 = нет DSP/выхода. При отсутствии DSP out обнуляется. */
int call_audio_get_diagnostics(uint64_t call_id, struct call_jitter_diagnostics* out);
/* UI, любой поток: снимок последних 30с и взвешенной гистограммы; не зависит от уровня логов.
 * 0 — получен, -1 — звонок уже завершён; out обязателен и обнуляется при отсутствии звонка. */
int call_audio_get_history(uint64_t call_id, struct call_audio_history* out);

/* uasync-поток: метрики для CALL_STATS; все выходы необязательны, 0 = успех, -1 = нет DSP (выходы не меняются).
 * dropped/underruns накопительные; диагностические события и интервальные TX-измерения потребляются/логируются.
 * Для чтения без потребления событий использовать get_diagnostics. */
int  call_audio_get_stats(uint64_t call_id, int* buffer_ms, int* tempo_x100,
                          uint32_t* dropped, uint32_t* underruns, struct call_jitter_range* range);

/* После остановки указанного направления: сбросить AEC; capture также сбрасывает AGC и увеличивает capture_epoch.
 * Playback сбрасывает устаревший jitter/PCM по call_jitter_reset (последний запас Opus сохраняется) и playback clock epoch.
 * Capture и playback могут работать в разных потоках, по одному writer на направление.
 * Release допустим только после остановки обоих. */
void call_audio_reset_io(uint64_t call_id, int playback);

/* Core AEC для raw capture: Android — по спикерфону (route == SPEAKER).
 * Prepared capture не создаёт core AEC; desktop передаёт настройку в VoiceAudioIo. */
void call_audio_set_aec_enabled(int enabled);
/* Raw capture: сила NS -40..-1 dB, 0 уничтожает фильтр и убирает его задержку.
 * Выполняется под DSP mutex между кадрами; prepared capture уже очищен платформой. Возвращает 0/-1 с логом. */
int call_audio_set_noise_suppression(int suppress_db);

/* Задать задержку рендер→захват AEC в кадрах (20 мс). Android измеряет через
 * AudioTrack/AudioRecord getTimestamp. Prepared desktop выравнивает референс по времени. Если core AEC уже
 * активен — пересоздаётся с новой задержкой. */
void call_audio_set_aec_delay_frames(int frames);

#ifdef __cplusplus
}
#endif

#endif /* CALL_AUDIO_H */
