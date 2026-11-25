/* Stereo Opus/SoundTouch и ограниченный SPSC-запас готового PCM.
 * feed/prepare/set_tempo/finish — один producer; pull/silence — один consumer.
 * position/pending_gain сериализуют с consumer короткой внешней блокировкой; pending_gain вызывает producer.
 * Producer не держит эту блокировку при feed/prepare/finish: callback не ждёт Opus/SoundTouch.
 * feed прогревает Opus перед start_pts, затем подаёт PCM с темпом 1.0..1.6 без изменения высоты звука.
 * pull записывает соответствие кадров устройства исходному PTS; silence продлевает hardware time, но не media time.
 * position/pending_gain используют реально проигранный frame counter, поэтому уже поставленный PCM сохраняет свой темп.
 * При старте/нехватке consumer ждёт buffer_ms готового PCM, сохраняя PTS и decoder; backend вставляет silence.
 * Последний Opus-пакет/EOF не теряется: finish выталкивает остаток SoundTouch и снимает ожидание порога. */
#ifndef STREAM_PLAYBACK_AUDIO_H
#define STREAM_PLAYBACK_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stream_playback_audio;
/* buffer_ms (1..1000) — порог пополнения, старта и возобновления готового PCM.
 * Считается только готовый PCM: аппаратная очередь и закодированные пакеты в него не входят.
 * start_pts_us >= 0; возвращает объект либо NULL при ошибке. Создание/уничтожение — без параллельных обращений. */
struct stream_playback_audio *stream_playback_audio_create(int64_t start_pts_us, unsigned buffer_ms);
/* Producer под внешней блокировкой consumer: изменить порог 1..1000 мс без сброса PCM/PTS.
 * Повышение включает накопление, понижение освобождает готовый запас; ошибка -1, успех 0. */
int stream_playback_audio_set_buffer_ms(struct stream_playback_audio *audio, unsigned buffer_ms);
struct stream_audio_meter;
/* До запуска consumer передать плееру свою ссылку на счётчик reader. NULL допустим. Destroy освобождает ссылку. */
void stream_playback_audio_set_meter(struct stream_playback_audio *audio, struct stream_audio_meter *meter);
/* После остановки producer/consumer освободить DSP, Opus и PCM; NULL допустим. */
void stream_playback_audio_destroy(struct stream_playback_audio *audio);
/* Producer: один 20-мс Opus mono/stereo, 1..1275 байтов, PTS >= 0; выход всегда stereo 48 кГц.
 * PTS нормализуется к шагу 20 мс с допуском 10 мс; пропуски до 100 мс заполняются PLC.
 * Возврат 0 — принят, 1 — старый пакет пропущен, -2 — разрыв >=110 мс, нужен новый playback,
 * -1 — аргументы/декодирование/DSP. После finish ввод запрещён; перед новым вводом проверяют needs_input. */
int stream_playback_audio_feed(struct stream_playback_audio *audio, int64_t pts_us, const uint8_t *packet, size_t length);
/* Producer: перенести готовый выход DSP в SPSC; вызывать после освобождения PCM, в том числе после finish.
 * 0 — успех (также при полном SPSC), -1 — ошибка состояния/DSP. */
int stream_playback_audio_prepare(struct stream_playback_audio *audio);
/* Потокобезопасный снимок готового PCM, в stereo frames (48000 frames/s). */
unsigned stream_playback_audio_buffered(const struct stream_playback_audio *audio);
/* Потокобезопасно: consumer накапливает PCM перед стартом/возобновлением; после finish всегда 0.
 * При этом уже поставленный устройству звук продолжает идти по прежней шкале PTS. */
int stream_playback_audio_buffering(const struct stream_playback_audio *audio);
/* Producer: внутренний lookahead DSP при текущем темпе; добавляется к guard устройства, без PCM target. */
int64_t stream_playback_audio_latency_us(struct stream_playback_audio *audio);
/* Producer: установить темп 1000..1600, сохранив PTS уже подготовленного PCM; неверный аргумент только логируется. */
void stream_playback_audio_set_tempo(struct stream_playback_audio *audio, int tempo_milli);
/* 1 — ввод ещё открыт и готового PCM меньше buffer_ms; 0 — порог достигнут, finish/failed либо audio=NULL.
 * Producer ограничивает этим ввод, иначе скрытый выход DSP может накапливаться сверх SPSC. */
int stream_playback_audio_needs_input(const struct stream_playback_audio *audio);
/* frames <=8192; limit_pts_us ограничивает исходный PTS, INT64_MAX разрешает весь подготовленный звук.
 * Возвращает число stereo frames, -1 при ошибке; при накоплении порога — 0. После finish порог не применяется.
 * Оставшиеся места backend заполняет тишиной и вызывает silence; ограничение PTS не запускает новое накопление. */
int stream_playback_audio_pull(struct stream_playback_audio *audio, int16_t *pcm, unsigned frames, int64_t limit_pts_us);
/* Consumer: учесть frames тишины, реально поставленные после PCM; PTS остаётся последним, written растёт. */
void stream_playback_audio_silence(struct stream_playback_audio *audio, unsigned frames);
/* Consumer либо под его внешней блокировкой: PTS по аппаратному счётчику played_frames с той же нулевой точкой,
 * что written. Значение выше written ограничивается; до PCM возвращает start_pts, для NULL -1. */
int64_t stream_playback_audio_position(struct stream_playback_audio *audio, uint64_t played_frames);
/* Producer под блокировкой consumer: ещё не проигранное сокращение длительности в DSP/SPSC/устройстве, мкс.
 * Учитывает прежние темпы поставленного PCM; NULL/полностью проигранный EOF дают 0. */
int64_t stream_playback_audio_pending_gain(struct stream_playback_audio *audio, uint64_t played_frames);
/* Consumer либо под его блокировкой: 1 — аппаратная позиция попала в тишину/за written, 0 — PCM/finish/NULL. */
int stream_playback_audio_starved(const struct stream_playback_audio *audio, uint64_t played_frames);
/* Consumer либо под его блокировкой: всего поставлено устройству stereo frames, включая silence; NULL даёт 0. */
uint64_t stream_playback_audio_written(const struct stream_playback_audio *audio);
/* Producer: однократно flush DSP и запретить feed; остаток далее переносится prepare. NULL/повтор допустимы. */
void stream_playback_audio_finish(struct stream_playback_audio *audio);
/* Потокобезопасно: DSP полностью подготовлен и SPSC разобран consumer; аппаратная очередь ещё может играть.
 * Возвращает 1 также для NULL, 0 — пока есть остаток или ввод не завершён. */
int stream_playback_audio_empty(const struct stream_playback_audio *audio);

#ifdef __cplusplus
}
#endif
#endif
