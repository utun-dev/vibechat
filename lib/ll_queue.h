#ifndef LL_QUEUE_H
#define LL_QUEUE_H

/* Очередь одного uasync-потока; наличие pthread-заголовков не делает её межпоточной.
 * put может синхронно вызвать consumer/waiter. Держать владельцев живыми до завершения callbacks,
 * waiter handle обнулять целиком и отменять до освобождения; entry и dgram освобождаются отдельно. */

#ifdef __cplusplus
extern "C" {
#endif


#include <stddef.h> // для size_t
#include <stdint.h>  // для uint64_t
#include "u_async.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#else
    #include <pthread.h>
#endif

//#define QUEUE_DEBUG 1       // раскомментировать для отладки очереди
//#define QUEUE_THREAD_CHECK 1  // 0 to disable

/**
 * @file ll_queue.h
 * @brief Упрощённая двусвязная очередь с поддержкой автозабора элементов через callback,
 *        порогового ожидания освобождения места, поиска по ID и гибкого управления памятью.
 *
 * Основные особенности:
 * - FIFO-очередь (с возможностью добавления в начало для приоритета)
 * - Автоматический вызов callback при появлении элементов (один элемент за раз)
 * - Обязательный вызов queue_resume_callback после обработки элемента
 * - Поддержка пулов памяти для entry и отдельно для dgram
 * - Хеш-таблица для быстрого поиска по ID (опционально)
 * - Связный список waiters для backpressure c общим порогом (round-robin)
 * - Использует uasync для отложенного возобновления callback'ов (без рекурсии в стеке)
 *
 * @note Важные правила использования автозабора (queue_set_callback):
 *       1. Коллбэк получает управление только если в очереди есть элементы И !callback_suspended
 *       2. Внутри коллбэка ОБЯЗАТЕЛЬНО: queue_data_get() → обработать элемент → queue_resume_callback()
 *       3. Без queue_resume_callback автозабор остаётся приостановленным
 *       4. Ручное извлечение допустимо; для продолжения автозабора после него тоже нужен resume
 *       5. queue_data_get() автоматически suspend'ит коллбэки для предотвращения рекурсии
 *
 * @note Память:
 *       - queue_free() освобождает структуру очереди, хеш-таблицу и список waiters
 *         (handle->internal каждого ожидающего сбрасывается в NULL)
 *       - Все элементы должны быть извлечены через queue_data_get() и освобождены через queue_entry_free()
 *       - dgram освобождается отдельно через queue_dgram_free() или кастомную dgram_free_fn
 */

// Forward declarations
struct ll_queue;
struct queue_waiter_handle;

/**
 * @struct ll_entry
 * @brief Элемент очереди (переменного размера).
 *
 * Память выделяется одним блоком: [struct ll_entry + data[size]].
 * Поле dgram — отдельный блок (u_malloc / пул / внешняя память со своим dgram_free_fn).
 */
struct ll_entry {
    char* name;
    struct ll_queue* owner;               // Владелец элемента; NULL после извлечения
    struct ll_entry* next;                // Следующий элемент в очереди
    struct ll_entry* prev;                // Предыдущий элемент в очереди
    uint16_t size;                        // Размер пользовательского буфера data[] (байт)
    uint16_t len;                         // Актуальная длина данных в dgram
    uint16_t memlen;                      // Выделенный размер под dgram
    uint16_t int_len;                     // Внутреннее (не использовать)
    uint8_t* dgram;                       // Указатель на данные пакета
    void (*dgram_free_fn)(uint8_t* data); // Кастомная функция освобождения dgram
    struct memory_pool* dgram_pool;       // Пул для dgram (если используется)
    uint32_t index_hash;                  // FNV-1a (32 бита) по всем index_size байтам data[] от index_offset
    struct ll_entry* hash_next;           // Следующий в хеш-цепочке
    struct memory_pool* pool;             // Пул, из которого выделен сам entry (NULL = u_malloc)
    uint8_t data[0];                      // Гибкий массив пользовательских данных (размер = size)
};

/**
 * @typedef queue_callback_fn
 * @brief Коллбэк автозабора элементов из очереди.
 * @param q указатель на очередь
 * @param arg пользовательский аргумент (из queue_set_callback)
 *
 * @note Внутри функции:
 *       - Вызвать queue_data_get(q) для получения элемента
 *       - Обработать элемент (можно асинхронно)
 *       - После завершения обработки вызвать queue_resume_callback(q)
 */
typedef void (*queue_callback_fn)(struct ll_queue* q, void* arg);

/**
 * @typedef queue_threshold_callback_fn
 * @brief Одноразовый коллбэк при достижении порога очереди.
 * @param q указатель на очередь
 * @param arg пользовательский аргумент
 */
typedef void (*queue_threshold_callback_fn)(struct ll_queue* q, void* arg);

/**
 * @struct queue_waiter
 * @brief Внутренний узел в связном списке ожидающих (очередь владеет, аллоцирует/освобождает сама).
 */
struct queue_waiter {
    struct queue_waiter* next;               ///< Следующий в списке ожидающих
    struct queue_waiter_handle* handle;      ///< Обратная ссылка на публичную структуру
    queue_threshold_callback_fn callback;
    void* callback_arg;
};

/**
 * @struct queue_waiter_handle
 * @brief Публичная управляющая структура (встраивается в структуру вызывающей стороны).
 *
 * Вызывающая сторона:
 *   1. Объявляет поле struct queue_waiter_handle в своей структуре (= {0} или u_calloc)
 *   3. Вызывает queue_waiter_wait() когда хочет дождаться освобождения очереди
 *   4. Вызывает queue_waiter_cancel() при деинициализации для отмены ожидания
 */
struct queue_waiter_handle {
    struct queue_waiter* internal;           ///< NULL = не ждём; иначе — внутренний узел
    struct ll_queue*     defer_q;
    queue_threshold_callback_fn defer_cb;
    void*                defer_arg;
    struct uasync_work   work;               ///< Отложенный вызов живёт вместе с handle.
    struct uasync_work*  call_soon_id;       ///< Ненулевой при отложенном вызове, отмена через queue_waiter_cancel.
    struct queue_waiter_handle* defer_next;   ///< список отложенных уведомлений очереди
};

/**
 * @struct ll_queue
 * @brief Структура очереди.
 */
struct ll_queue {
    char* name;
    struct ll_entry* head;          // Голова очереди (отсюда извлекаем)
    struct ll_entry* tail;          // Хвост очереди (сюда добавляем)
    int count;                      // Текущее количество элементов
    size_t total_bytes;             // Суммарный объём данных (сумма int_len)
    int size_limit;                 // Максимальное количество элементов (-1 = без лимита)

    queue_callback_fn callback;     // Коллбэк автозабора
    void* callback_arg;             // Аргумент коллбэка
    int callback_suspended;         // 1 = коллбэки временно приостановлены

    struct uasync_work resume_work; // Запланированный автозабор; пригоден для повторных постановок.
    struct UASYNC* ua;              // Экземпляр uasync (обязателен для таймеров)

    struct queue_waiter* waiter_head; // Голова списка ожидающих
    struct queue_waiter* waiter_tail; // Хвост списка ожидающих
    struct queue_waiter_handle* deferred_waiters; // Уже запланированные уведомления
    int threshold_max_packets;        // Общий порог: макс. кол-во элементов (по умолчанию 0)
    size_t threshold_max_bytes;       // Общий порог: макс. объём данных (0 = не проверять)
    int waiter_defer;                 // 0=immediate callback, 1=defer via uasync_call_soon
    int callback_defer;               // 0=immediate callback из put (default), 1=defer via uasync_call_soon (очистка стека)

    queue_callback_fn empty_callback;     // одноразовый коллбэк при опустошении очереди
    void*             empty_callback_arg;
    struct uasync_work empty_work; // Однократное уведомление о пустой очереди.

    queue_callback_fn on_get;             // коллбэк после каждого queue_data_get (deferred)
    void*             on_get_arg;
    void*             on_get_call_soon_id;
    size_t            on_get_pending;

    struct ll_entry** hash_table;   // Хеш-таблица для поиска по id (если hash_size > 0)
    size_t hash_size;               // Размер хеш-таблицы
    uint16_t index_offset;          // Смещение индекса в data[] (для всех entry очереди)
    uint16_t index_size;            // Размер индекса (0 = без индекса)

#ifdef QUEUE_THREAD_CHECK
#ifdef _WIN32
    DWORD owner_thread;
#else
    pthread_t owner_thread;
#endif
#endif
};

// ==================== Создание / уничтожение ====================

/**
 * @brief Создаёт новую пустую очередь.
 * @param ua экземпляр uasync (обязателен для отложенного resume коллбэков)
 * @param hash_size размер хеш-таблицы (0 = поиск по id отключён)
 * @param index_offset смещение ключа индекса в data[] каждого entry (0 если hash_size=0)
 * @param index_size размер ключа индекса (0 если hash_size=0)
 * @param name заимствованное имя для отладки, должно жить до queue_free
 * @return указатель на очередь или NULL при NULL ua / ошибке выделения памяти
 */
struct ll_queue* queue_new(struct UASYNC* ua, size_t hash_size, uint16_t index_offset, uint16_t index_size, char* name);

/**
 * @brief Освобождает структуру очереди.
 * @param q очередь
 *
 * @warning НЕ ОСВОБОЖДАЕТ элементы в очереди!
 *          Их нужно извлечь через queue_data_get() и освободить через queue_entry_free().
 * Перед дренажем снять callback и отменить ожидания владельцев, затем для каждого entry освободить dgram и entry.
 * queue_free отменяет свои отложенные уведомления и waiters; user_arg не освобождает. NULL допустим.
 */
void queue_free(struct ll_queue* q);

/* Аварийный сброс: освободить все entry/dgram, отменить waiters и отложенные уведомления без их вызова.
 * Не выделяет память (кроме действий пользовательского dgram_free_fn). Очередь/индекс/обычный callback/пороги сохраняются.
 * empty/on_get обработчики снимаются, callback приостановлен; владельцы заново ставят ожидания после восстановления.
 * Вызывать в потоке ua; NULL допустим. Для продолжения автозабора нужен queue_resume_callback(). */
void queue_clear(struct ll_queue* q);

// ==================== Конфигурация ====================

/**
 * @brief Устанавливает максимальное количество элементов в очереди.
 * @param q очередь
 * @param lim лимит (любое отрицательное значение = без ограничения)
 *
 * При превышении лимита новые элементы автоматически освобождаются.
 */
void queue_set_size_limit(struct ll_queue* q, int lim);

// ==================== Автозабор элементов ====================

/**
 * @brief Устанавливает коллбэк для автоматического извлечения элементов.
 * @param q очередь
 * @param cbk_fn функция-коллбэк (NULL отключает автозабор)
 * @param arg пользовательский аргумент
 *
 * Только заменяет функцию и аргумент: не запускает обработку, не снимает callback_suspended
 * и не отменяет уже запланированный resume. Отложенный resume использует новые функцию и аргумент.
 * Для запуска автозабора из уже заполненной очереди вызовите queue_resume_callback().
 */
void queue_set_callback(struct ll_queue* q, queue_callback_fn cbk_fn, void* arg);

/**
 * @brief Разрешает автозабор и планирует отложенный вызов коллбэка.
 * @param q очередь
 *
 * Сразу снимает callback_suspended. Если есть элементы и callback, планирует uasync_call_soon,
 * если resume ещё не запланирован. При исполнении повторно проверяет наличие элементов,
 * callback и отсутствие приостановки. Последующий put в пустую очередь может вызвать callback
 * синхронно, если callback_defer == 0.
 * @note Вызывать после завершения обработки извлечённого элемента для продолжения автозабора.
 */
void queue_resume_callback(struct ll_queue* q);

// ==================== Пороговое ожидание (backpressure) ====================

/**
 * @brief Устанавливает общий порог освобождения очереди.
 * @param q очередь
 * @param max_packets максимальное количество элементов (условие: count <= max_packets)
 * @param max_bytes максимальный объём данных (0 = не проверять)
 *
 * Порог — общий на всю очередь. Все waiters в связном списке используют этот порог.
 * После успешного get/remove при выполнении порога вызывается один следующий waiter из FIFO-списка.
 * Сам setter старые ожидания не проверяет. Байт-порог использует int_len, зафиксированный при put.
 * По умолчанию: max_packets=0, max_bytes=0 (пустая очередь).
 */
void queue_set_threshold(struct ll_queue* q, int max_packets, size_t max_bytes);

// enable=0 — синхронный waiter (по умолчанию), !=0 — через uasync_call_soon.
// Deferred-уведомление не резервирует место и повторно порог не проверяет: очередь к исполнению может заполниться.
// Повторную регистрацию producer из его callback при свободной очереди откладывать, чтобы избежать рекурсии.
void queue_set_waiter_defer(struct ll_queue* q, int enable);

/**
 * @brief Управляет способом вызова коллбэка автозабора при добавлении элемента.
 * @param q очередь
 * @param enable 0 — первый запуск коллбэка из queue_data_put* вызывается синхронно (по умолчанию);
 *               1 — первый запуск откладывается через uasync_call_soon (очищает стек вызывающего).
 *
 * @note Влияет только на первый запуск из put. Повторные запуски (queue_resume_callback)
 *       всегда идут через uasync_call_soon.
 */
void queue_set_callback_defer(struct ll_queue* q, int enable);

/**
 * @brief Устанавливает одноразовый коллбэк при опустошении очереди (count==0).
 * @param q очередь
 * @param cbk_fn коллбэк (NULL для отмены)
 * @param arg пользовательский аргумент
 *
 * Заменяет регистрацию и отменяет её прежний отложенный вызов. При count == 0 сразу планирует
 * uasync_call_soon; иначе вызов планируется при опустошении через queue_data_get/queue_remove_data.
 * При исполнении снова проверяет count: если очередь заполнилась, регистрация сохраняется
 * до следующего опустошения. Функция и аргумент сбрасываются ДО вызова callback,
 * поэтому внутри него можно зарегистрировать новое уведомление.
 */
void queue_set_empty_callback(struct ll_queue* q, queue_callback_fn cbk_fn, void* arg);

/**
 * @brief Устанавливает коллбэк после каждого успешного queue_data_get (deferred через call_soon).
 * @param q очередь
 * @param cbk_fn коллбэк (NULL для отмены)
 * @param arg пользовательский аргумент
 *
 * Уведомления подсчитываются, не сливаются в одно; remove их не создаёт.
 * NULL отменяет отложенный вызов и обнуляет накопленные уведомления. Замена использует новые функцию/arg.
 */
void queue_set_on_get(struct ll_queue* q, queue_callback_fn cbk_fn, void* arg);

/**
 * @brief Регистрирует ожидание освобождения очереди до общего порога.
 * @param q очередь
 * @param h handle, полностью обнулённый перед первым использованием: `struct queue_waiter_handle h = {0}`
 * @param callback функция, вызываемая при достижении порога
 * @param arg аргумент коллбэка
 * @return 1 — условие выполнено (callback вызван или отложен), 0 — ожидает порог, -1 — ошибка
 *
 * При корректных аргументах повторная регистрация в той же очереди отменяет прежнее ожидание.
 * Если h->internal != NULL, перед переносом в другую очередь обязательно вызовите
 * queue_waiter_cancel(old_q, h): очередь старого ожидания в handle не хранится.
 * Уже отложенный callback (h->call_soon_id != NULL) отменяется по сохранённому h->defer_q,
 * поэтому в этом состоянии handle можно передать другой очереди без отдельной отмены.
 * Уведомление зарегистрированного waiter ставится в FIFO без выделения памяти.
 * При queue_free отменяются также уже запланированные уведомления.
 */
int queue_waiter_wait(struct ll_queue* q, struct queue_waiter_handle* h,
                      queue_threshold_callback_fn callback, void* arg);

/**
 * @brief Отменяет ожидание (удаляет из списка, если зарегистрирован).
 * @param q очередь исходного ожидания (для h->internal); не NULL
 * @param h указатель на handle
 *
 * Отложенный callback отменяется через сохранённую h->defer_q. Если ожидания и отложенного
 * callback нет — ничего не делает. При h->internal != NULL передавать другую очередь нельзя.
 */
void queue_waiter_cancel(struct ll_queue* q, struct queue_waiter_handle* h);

// ==================== Работа с данными ====================

/**
 * Общие правила владения для всех четырёх queue_data_put*:
 * - При q == NULL или entry == NULL возвращается -1 без освобождения памяти.
 * - Элемент должен быть извлечён из прежней очереди: owner, prev и next должны быть NULL.
 *   Повторная вставка связанного элемента приводит к abort().
 * - При остальных отказах (лимит, неверный вид put или границы индекса) освобождаются
 *   сначала dgram через queue_dgram_free(), затем entry через queue_entry_free().
 * - При успехе владение передаётся очереди/потребителю. Put может синхронно вызвать callback,
 *   который обработает и освободит entry и даже очередь до возврата из put.
 *   Использовать эти указатели после put можно только при гарантированном времени жизни.
 */

/**
 * @brief Добавляет элемент в конец очереди (FIFO).
 * @param q очередь без хеш-таблицы (hash_size == 0)
 * @param entry элемент; правила передачи владения см. выше
 * @return 0 — добавлен, -1 — NULL-аргумент, переполнение или hash_size > 0
 */
int queue_data_put(struct ll_queue* q, struct ll_entry* entry);

// Дописывает len байт data в непустой хвост с буфером dgram ёмкостью capacity, обновляет счётчик байт.
// Для очередей с изменяемыми буферами известного размера. Возвращает 1 при записи, 0 если нужен новый элемент.
int queue_append_tail(struct ll_queue* q, const uint8_t* data, uint16_t len, size_t capacity);

/**
 * @brief Добавляет элемент в начало очереди (LIFO, высокий приоритет).
 * @param q очередь без хеш-таблицы (hash_size == 0)
 * @param entry элемент; правила передачи владения см. выше
 * @return 0 — добавлен, -1 — NULL-аргумент, переполнение или hash_size > 0
 */
int queue_data_put_first(struct ll_queue* q, struct ll_entry* entry);

/**
 * @brief Добавляет элемент в конец очереди с хеш-индексом (использует index_offset/index_size из очереди).
 * @param q очередь с настроенной хеш-таблицей (hash_size > 0, index_size > 0)
 * @param entry элемент; ключ занимает index_size байт в data[] от index_offset и должен помещаться в entry->size
 * @return 0 — добавлен, -1 — NULL-аргумент, переполнение или неверные границы/нулевой размер индекса
 * @note Владение см. выше. При hash_size == 0 реализация может вернуть 0, добавив элемент без индексации;
 *       наличие хеш-таблицы — предусловие, а не гарантированная проверка с возвратом -1.
 */
int queue_data_put_with_index(struct ll_queue* q, struct ll_entry* entry);

/**
 * @brief Добавляет элемент в начало очереди с хеш-индексом (использует index_offset/index_size из очереди).
 * @param q очередь с настроенной хеш-таблицей (hash_size > 0, index_size > 0)
 * @param entry элемент; ключ занимает index_size байт в data[] от index_offset и должен помещаться в entry->size
 * @return 0 — добавлен, -1 — NULL-аргумент, переполнение или неверные границы/нулевой размер индекса
 * @note Владение см. выше. При hash_size == 0 реализация может вернуть 0, добавив элемент без индексации;
 *       наличие хеш-таблицы — предусловие, а не гарантированная проверка с возвратом -1.
 */
int queue_data_put_first_with_index(struct ll_queue* q, struct ll_entry* entry);


/**
 * @brief Извлекает элемент из начала очереди.
 * @param q очередь
 * @return элемент или NULL, если очередь пуста
 *
 * @note Автоматически приостанавливает коллбэки (callback_suspended = 1)
 *       После обработки элемента необходимо вызвать queue_resume_callback().
 */
struct ll_entry* queue_data_get(struct ll_queue* q);

/**
 * @brief Возвращает текущее количество элементов в очереди.
 * @param q очередь
 * @return количество элементов
 */
int queue_entry_count(struct ll_queue* q);

// ==================== Управление памятью элементов ====================

// проверить правильность очереди (корректность цепочки, счетчики элементов и байтов)
int queue_check_consistency(struct ll_queue* q);

/**
 * @brief Выделяет элемент с отдельным буфером dgram через u_malloc (entry->len=0, memlen=len).
 * @param len требуемый размер dgram
 * @return элемент или NULL при ошибке
 */
struct ll_entry* ll_alloc_lldgram(uint16_t len);

/**
 * @brief Создаёт обнулённый элемент с пользовательским буфером data[] (u_malloc одним блоком).
 * @param data_size размер data[], должен помещаться в uint16_t
 * @return элемент или NULL при ошибке
 */
struct ll_entry* queue_entry_new(size_t data_size);

/**
 * @brief Создаёт элемент из пула памяти.
 * @param pool пул с object_size >= sizeof(struct ll_entry); разность размеров должна помещаться в uint16_t
 * @return элемент или NULL при ошибке выделения; пустой кеш пула пополняется автоматически
 */
struct ll_entry* queue_entry_new_from_pool(struct memory_pool* pool);

/**
 * @brief Освобождает структуру элемента (возвращает в пул или u_free).
 * @param entry извлечённый из очереди элемент; NULL допустим
 *
 * @note НЕ освобождает dgram! Используйте queue_dgram_free() отдельно.
 */
void queue_entry_free(struct ll_entry* entry);

/**
 * @brief Освобождает буфер dgram элемента.
 * @param entry элемент
 *
 * При dgram != NULL вызывает dgram_free_fn, если она задана; иначе возвращает буфер в dgram_pool,
 * если задан пул; иначе вызывает u_free. Затем сбрасывает dgram и len. Сам entry не освобождает.
 * Внешнему буферу нужен подходящий dgram_free_fn.
 */
void queue_dgram_free(struct ll_entry* entry);

// ==================== Поиск и удаление ====================

/**
 * @brief Находит элемент по ключу индекса (требуется hash_size > 0 при создании).
 * @param q очередь (размер ключа берётся из q->index_size)
 * @param index_key искомый ключ
 * @return элемент или NULL
 */
struct ll_entry* queue_find_data_by_index(struct ll_queue* q,
                                          const void* index_key);

/**
 * @brief Находит следующий элемент с тем же ключом (продолжение поиска).
 * @param q очередь (размер ключа берётся из q->index_size)
 * @param index_key искомый ключ
 * @param prev_entry предыдущий найденный элемент (от queue_find_data_by_index или предыдущего queue_find_next_by_index)
 * @return следующий элемент с тем же ключом или NULL
 */
struct ll_entry* queue_find_next_by_index(struct ll_queue* q,
                                           const void* index_key,
                                           struct ll_entry* prev_entry);

/**
 * @brief Удаляет элемент из очереди (не освобождает память).
 * @param q очередь
 * @param entry элемент
 * @return 0 — успех, -1 — элемент не найден в очереди
 */
int queue_remove_data(struct ll_queue* q, struct ll_entry* entry);

// ==================== Утилиты ====================

/**
 * @brief Возвращает суммарный объём данных в очереди.
 * @param q очередь
 * @return сумма int_len (снимки len при put); изменение len связанного entry счётчик не пересчитывает
 */
static inline size_t queue_total_bytes(struct ll_queue* q) {
    return q->total_bytes;
}


#ifdef __cplusplus
}
#endif
#endif // LL_QUEUE_H
