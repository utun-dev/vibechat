#ifndef CALL_TONES_H
#define CALL_TONES_H

// call_tones.h — генератор тонов звонка (48kHz mono int16): глитч при сталле
// и тон завершения. Используется только аудио-потоком (single-writer).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct call_tones;

/* Создать выключенный генератор; NULL при нехватке памяти. Управление и render сериализует владелец. */
struct call_tones* call_tones_create(void);
void call_tones_destroy(struct call_tones* t); // Освободить после остановки render; NULL допустим.
void call_tones_reset(struct call_tones* t);         /* в тишину */

void call_tones_glitch_start(struct call_tones* t);  /* запустить/продолжить цикл глитча */
void call_tones_glitch_stop(struct call_tones* t);   /* прекратить глитч (тишина) */

void call_tones_ended_start(struct call_tones* t);   /* 3 нисходящих тона 700/500/400Гц по 150мс; повторный start начинает заново */
int  call_tones_ended_pending(struct call_tones* t); /* 1 = тон завершения ещё звучит */

/* При валидных аргументах заполняет ровно max_samples (тон/паузы/тишина после завершения).
 * Возвращает max_samples; 0 при NULL t/out или max_samples<=0. Остальные операции с NULL t безопасны. */
int call_tones_render(struct call_tones* t, int16_t* out, int max_samples);

#ifdef __cplusplus
}
#endif

#endif /* CALL_TONES_H */
