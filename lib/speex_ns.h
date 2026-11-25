/* SpeexDSP-шумоподавление PCM s16/48кГц, mono/stereo; блок — 960 фреймов на канал.
 * Один владелец выполняет все вызовы, аппаратный callback только передаёт PCM worker-у.
 * Поток: create -> process_frame -> destroy. Запись: create -> push -> finish -> destroy;
 * эти два способа обработки одного объекта не смешивать. Внутри Speex остаётся задержка 20мс.
 * NULL вместо объекта означает полный обход без обработки, накопления и задержки.
 * AGC/VAD/dereverb и подавление остаточного эха выключены; AEC выполняется перед этим модулем. */
#ifndef SPEEX_NS_H
#define SPEEX_NS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPEEX_NS_FRAME_SAMPLES 960
#define SPEEX_NS_DEFAULT_DB (-20)
#define SPEEX_NS_MIN_DB (-40)
#define SPEEX_NS_MAX_DB (-1)

struct speex_ns;

/* Получатель заимствованного interleaved PCM: count — int16, вернуть 0 при успехе, -1 при отказе.
 * Данные действительны только внутри вызова, получатель копирует/обрабатывает их синхронно. */
typedef int (*speex_ns_output_fn)(void* arg, const int16_t* pcm, int count);

/* Создать отдельное состояние каждого канала (channels=1/2), suppress_db=-40..-1.
 * Возвращает объект либо NULL с ERROR-логом. Нулевое подавление задаётся обходом, а не create. */
struct speex_ns* speex_ns_create(int channels, int suppress_db);
/* Освободить все состояния; NULL допустим, накопленные PCM здесь не выдаются. */
void speex_ns_destroy(struct speex_ns* ns);
/* Изменить силу без потери адаптации; 0 — успех, -1 — неверный объект/диапазон/ошибка ctl. */
int speex_ns_set_level(struct speex_ns* ns, int suppress_db);
/* Обработать ровно 960*channels interleaved отсчётов на месте без выделений памяти.
 * При ns=NULL ненулевой PCM любого положительного count остаётся нетронутым. Возвращает 0/-1. */
int speex_ns_process_frame(struct speex_ns* ns, int16_t* pcm, int count);
/* Запись: принять произвольное число interleaved отсчётов (кратно channels), накопить полный кадр и вызвать output.
 * ns=NULL сразу передаёт исходный буфер output. Возвращает 0/-1; после отказа запись нужно отклонить. */
int speex_ns_push(struct speex_ns* ns, const int16_t* pcm, int count, speex_ns_output_fn output, void* arg);
/* Завершить запись: дополнить частичный кадр и выдать внутренний хвост Speex.
 * У непустой записи выход длиннее входа на 960 фреймов (начальная задержка сохраняется, padding не сохраняется).
 * Повтор/пустой объект безопасны, возвращает 0/-1; после finish новые PCM требуют нового объекта. */
int speex_ns_finish(struct speex_ns* ns, speex_ns_output_fn output, void* arg);

#ifdef __cplusplus
}
#endif
#endif
