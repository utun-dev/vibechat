/* timeout_heap — min-heap таймеров для uasync; сам не измеряет время и не вызывает таймеры.
 * Владелец задаёт абсолютные expiration в одной шкале и извлекает готовые записи через peek/pop.
 * cancel помечает запись; её data передаётся free_callback при последующей очистке или destroy.
 * Без free_callback data не освобождается. Успешный pop передаёт data вызывающему.
 * index_ptr, если задан, должен жить до удаления записи; heap обновляет индекс при перестановках.
 * Все операции одного heap выполняются в одном потоке, внутренней синхронизации нет.
 * Внешние индексы 0-based; после удаления их значение больше не определяет запись и не сбрасывается автоматически.
 * push/cancel_at — O(log n), cancel с поиском — O(n). Отмена ставит deleted=1, expiration=0 и поднимает запись.
 * peek/pop очищают k отменённых корней за O(k log n), вызывая free_callback; успешный pop его не вызывает.
 * Равные expiration не гарантируют FIFO. Размер включает ещё не очищенные отменённые записи.
 * Пример: create → set_free_callback → push(expiration, data, &живой_индекс) → peek/pop или cancel_at → destroy. */

#ifndef TIMEOUT_HEAP_H
#define TIMEOUT_HEAP_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>  // For uint64_t
#include <stddef.h>  // For size_t

typedef uint64_t TimeoutTime;  // Абсолютное время в шкале владельца (uasync хранит здесь миллисекунды).

typedef struct {
    TimeoutTime expiration;  // Sort key (smaller = earlier)
    void *data;              // User data (e.g., callback or ID)
    size_t *index_ptr;       // Pointer to node's heap_index (NULL = no tracking)
    int deleted;             // 0 = active, 1 = deleted
} TimeoutEntry;

typedef struct TimeoutHeap TimeoutHeap;

struct TimeoutHeap {
    TimeoutEntry *heap;      // Dynamic array
    size_t size;             // Current number of elements
    size_t capacity;         // Allocated size
    void* user_data;         // User data for free callback
    void (*free_callback)(void* user_data, void* data); // Callback to free data
};

/**
 * Create a new timeout heap with initial capacity.
 * @param initial_capacity Starting capacity (will grow as needed).
 * @return Pointer to the heap, or NULL on failure.
 */
TimeoutHeap *timeout_heap_create(size_t initial_capacity);

/**
 * Destroy the timeout heap and free resources.
 * @param h The heap to destroy.
 */
void timeout_heap_destroy(TimeoutHeap *h);

/**
 * Set a callback function to free data when deleted nodes are removed.
 * @param h The heap.
 * @param user_data User data passed to callback.
 * @param callback Освобождает data отменённых записей и всех записей при destroy; NULL оставляет data владельцу.
 */
void timeout_heap_set_free_callback(TimeoutHeap *h, void* user_data, void (*callback)(void* user_data, void* data));

/**
 * Insert a new timeout into the heap.
 * @param h The heap.
 * @param expiration The expiration time.
 * @param data User data associated with the timeout.
 * @param index_ptr Optional pointer to a live size_t updated with the entry's zero-based heap index.
 * @return 0 on success, -1 on allocation failure.
 */
int timeout_heap_push(TimeoutHeap *h, TimeoutTime expiration, void *data, size_t *index_ptr);

/**
 * Peek at the earliest non-deleted timeout without removing it.
 * @param h The heap.
 * @param out Where to store the entry.
 * @return 0 on success, -1 if empty.
 */
int timeout_heap_peek(TimeoutHeap *h, TimeoutEntry *out);

/**
 * Pop the earliest non-deleted timeout from the heap.
 * @param h The heap.
 * @param out Where to store the entry.
 * @return 0 on success, -1 if empty.
 */
int timeout_heap_pop(TimeoutHeap *h, TimeoutEntry *out);

/**
 * Cancel a timeout by matching expiration and data.
 * Scans the heap linearly, so O(n) time.
 * Assumes combinations are unique; cancels the first match.
 * @param h The heap.
 * @param expiration The expiration time to match.
 * @param data The data to match.
 * @return 0 if found and canceled, -1 if not found.
 */
int timeout_heap_cancel(TimeoutHeap *h, TimeoutTime expiration, void *data);

/* Изменяет срок живой записи по её актуальному index, без выделения памяти. Обновляет индексы переставленных записей. */
void timeout_heap_reschedule(TimeoutHeap *h, size_t index, TimeoutTime expiration);

/* Отмена по сохранённому индексу; data защищает от отмены чужой записи. 0 / -1, освобождение отложено. */
int timeout_heap_cancel_at(TimeoutHeap *h, size_t index, void *data);

/* Число записей, включая ещё не извлечённые отменённые; NULL → 0. */
size_t timeout_heap_get_size(TimeoutHeap *h);


#ifdef __cplusplus
}
#endif
#endif // TIMEOUT_HEAP_H
