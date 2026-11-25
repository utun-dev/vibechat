/* swm_min — минимум последних window_size целых значений (окно по числу добавлений, не по времени).
 * create → add/get_min → destroy; память O(window_size) выделяется при create, внутренней синхронизации нет.
 * Монотонный deque: add амортизированно O(1), get_min O(1); значения int, включая отрицательные. */
#ifndef SLIDING_WINDOW_MIN_H
#define SLIDING_WINDOW_MIN_H

#include <stddef.h>   /* для size_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SlidingWindowMin SlidingWindowMin;

/**
 * Создаёт новую структуру скользящего минимума.
 * @param window_size  размер окна (должен быть > 0)
 * @return указатель на структуру или NULL при window_size <= 0 / ошибке выделения памяти
 */
SlidingWindowMin* swm_create(int window_size);

/**
 * Уничтожает структуру и освобождает всю динамически выделенную память.
 * @param swm окно; NULL допустим
 */
void swm_destroy(SlidingWindowMin* swm);

/**
 * Добавляет новое значение в окно.
 * @param swm окно; при NULL ничего не делает
 * @param val новое целое значение
 * После вызова swm_get_min() сразу вернёт минимум последних min(window_size, добавленных) значений.
 */
void swm_add(SlidingWindowMin* swm, int val);

/**
 * Возвращает текущий минимум в окне.
 * @param swm окно
 * @return минимум; если окно пустое или swm == NULL — INT_MAX
 */
int swm_get_min(const SlidingWindowMin* swm);

#ifdef __cplusplus
}
#endif

#endif /* SLIDING_WINDOW_MIN_H */
