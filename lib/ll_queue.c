// ll_queue.c - Упрощенная архитектура: разделение создания элементов и работы с очередью
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "ll_queue.h"
#include "u_async.h"
#include "debug_config.h"
#include "mem.h"

#ifdef _WIN32
    #include <windows.h>
#else
    #include <pthread.h>
#endif

// ==================== Thread safety check ====================
#ifdef QUEUE_THREAD_CHECK
static inline void queue_check_thread(struct ll_queue* q) {
    if (!q) return;
    
#ifdef _WIN32
    DWORD current = GetCurrentThreadId();
    if (q->owner_thread != current) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Queue '%s': thread mismatch! owner=%lu current=%lu",
                    q->name ? q->name : "unknown", 
                    (unsigned long)q->owner_thread, 
                    (unsigned long)current);
        printf("ERROR: Queue '%s' accessed from wrong thread!\n", q->name ? q->name : "unknown");
        abort();
    }
#else
    pthread_t current = pthread_self();
    if (!pthread_equal(q->owner_thread, current)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Queue '%s': thread mismatch! owner=%lu current=%lu",
                    q->name ? q->name : "unknown", 
                    (unsigned long)q->owner_thread, 
                    (unsigned long)current);
        printf("ERROR: Queue '%s' accessed from wrong thread!\n", q->name ? q->name : "unknown");
        abort();
    }
#endif
}
#endif

// Предварительные объявления внутренних функций
static void queue_resume_timeout_cb(void* arg);
static void check_waiters(struct ll_queue* q);
static void empty_trampoline(void* arg);
static void on_get_trampoline(void* arg);

static void empty_trampoline(void* arg) {
    struct ll_queue* q = (struct ll_queue*)arg;
    if (q->count != 0) return;
    queue_callback_fn cb = q->empty_callback;
    void* cb_arg = q->empty_callback_arg;
    q->empty_callback = NULL;
    q->empty_callback_arg = NULL;
    if (cb) cb(q, cb_arg);
}

static void on_get_trampoline(void* arg) {
    struct ll_queue* q = (struct ll_queue*)arg;
    q->on_get_call_soon_id = NULL;
    if (!q->on_get_pending || !q->on_get) return;
    q->on_get_pending--;
    if (q->on_get_pending) {
        q->on_get_call_soon_id = uasync_call_soon(q->ua, q, on_get_trampoline);
        if (!q->on_get_call_soon_id) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] on_get scheduling failed: pending=%zu", q->name, q->on_get_pending);
    }
    if (q->on_get) q->on_get(q, q->on_get_arg);
}

// ==================== Управление очередью ====================

struct ll_queue* queue_new(struct UASYNC* ua, size_t hash_size, uint16_t index_offset, uint16_t index_size, char* name) {
    if (!ua) return NULL;
    
    struct ll_queue* q = u_calloc(1, sizeof(struct ll_queue));
    if (!q) return NULL;
    
    q->name = name;
    q->ua = ua;
    q->size_limit = -1;  // Без ограничения по умолчанию
    q->threshold_max_packets = 0;  // По умолчанию: ждать пустой очереди
    q->threshold_max_bytes = 0;    // По умолчанию: не проверять байты
    q->waiter_defer = 0;
    q->hash_size = hash_size;
    q->index_offset = index_offset;
    q->index_size = index_size;
    
#ifdef QUEUE_THREAD_CHECK
#ifdef _WIN32
    q->owner_thread = GetCurrentThreadId();
#else
    q->owner_thread = pthread_self();
#endif
#endif
    
    // Создать хеш-таблицу если нужно
    if (hash_size > 0) {
        q->hash_table = u_calloc(hash_size, sizeof(struct ll_entry*));
        if (!q->hash_table) {
            u_free(q);
            return NULL;
        }
    }
    
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "queue_new: created queue %p, hash_size=%zu", q, hash_size);
    
    return q;
}

struct ll_entry* ll_alloc_lldgram(uint16_t len) {
    struct ll_entry* entry = queue_entry_new(0);
    if (!entry) return NULL;

    entry->len=0;
    entry->memlen=len;
    entry->dgram = u_malloc(len);
//    entry->dgram_pool = NULL; - уже null (memset)
//    entry->dgram_free_fn = NULL;
    if (!entry->dgram) {
        queue_entry_free(entry);
        return NULL;
    }

    return entry;
}

static void queue_cancel_notifications(struct ll_queue* q) {
    while (q->deferred_waiters) queue_waiter_cancel(q, q->deferred_waiters);
    // Освободить список waiters (сбросить handle->internal, отменить отложенные вызовы)
    struct queue_waiter* w = q->waiter_head;
    while (w) {
        struct queue_waiter* next = w->next;
        if (w->handle) w->handle->internal = NULL;
        u_free(w);
        w = next;
    }
    q->waiter_head = NULL;
    q->waiter_tail = NULL;
    
    uasync_cancel_work(&q->resume_work);
    uasync_cancel_work(&q->empty_work);
    q->empty_callback = NULL;
    q->empty_callback_arg = NULL;
    q->on_get = NULL;
    q->on_get_arg = NULL;
    if (q->on_get_call_soon_id) uasync_call_soon_cancel(q->ua, q->on_get_call_soon_id);
    q->on_get_call_soon_id = NULL; q->on_get_pending = 0;
    q->callback_suspended = 1;
}

void queue_clear(struct ll_queue* q) {
    if (!q) return;
#ifdef QUEUE_THREAD_CHECK
    queue_check_thread(q);
#endif
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "[%s] discard queue: packets=%d bytes=%zu", q->name, q->count, q->total_bytes);
    queue_cancel_notifications(q);
    struct ll_entry* entry = q->head;
    q->head = q->tail = NULL; q->count = 0; q->total_bytes = 0;
    if (q->hash_table) memset(q->hash_table, 0, q->hash_size * sizeof(*q->hash_table));
    while (entry) {
        struct ll_entry* next = entry->next;
        entry->owner = NULL; entry->next = entry->prev = entry->hash_next = NULL;
        queue_dgram_free(entry); queue_entry_free(entry); entry = next;
    }
}

void queue_free(struct ll_queue* q) {
    if (!q) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "queue_free: freeing queue %p, head=%p, count=%d", (void*)q, (void*)q->head, q->count);
    queue_cancel_notifications(q);
    if (q->hash_table) u_free(q->hash_table);
    if (q->head) DEBUG_WARN(DEBUG_CATEGORY_SYS, "[%s] freeing nonempty queue: count=%d; entries remain caller-owned", q->name, q->count);
    for (struct ll_entry* e = q->head; e; e = e->next) e->owner = NULL;
    
    u_free(q);
}

// ==================== Конфигурация очереди ====================

void queue_set_callback(struct ll_queue* q, queue_callback_fn cbk_fn, void* arg) {
    if (!q) return;
    q->callback = cbk_fn;
    q->callback_arg = arg;
}

static void queue_resume_timeout_cb(void* arg) {
    struct ll_queue* q = (struct ll_queue*)arg;
    if (!q) return;
    
    // Вызвать коллбэк если есть элементы и коллбэки разрешены
    if (q->head && !q->callback_suspended && q->callback) {
        q->callback(q, q->callback_arg);
    }
}

void queue_resume_callback(struct ll_queue* q) {
    if (!q) return;
    
    q->callback_suspended = 0;
    
    // Если есть элементы, запланировать вызов коллбэка
    if (q->head && q->callback) {
        uasync_schedule_work(q->ua, &q->resume_work, queue_resume_timeout_cb, q);
    }
}

// Общий запуск коллбэка автозабора: immediate (default) или deferred через uasync_call_soon.
// Deferred-режим очищает стек производителя перед обработкой первого элемента.
static void queue_trigger_callback(struct ll_queue* q) {
    if (!q->head || q->callback_suspended || !q->callback) return;

    if (q->callback_defer) {
        uasync_schedule_work(q->ua, &q->resume_work, queue_resume_timeout_cb, q);
    } else {
        q->callback(q, q->callback_arg);
    }
}

void queue_set_size_limit(struct ll_queue* q, int lim) {
    if (!q) return;
    q->size_limit = lim;
}

// ==================== Управление элементами ====================

struct ll_entry* queue_entry_new(size_t data_size) {
    struct ll_entry* entry = u_malloc(sizeof(struct ll_entry) + data_size);
    if (!entry) return NULL;
    
    memset(entry, 0, sizeof(struct ll_entry) + data_size);
    entry->size = data_size;
    entry->len = 0;
    entry->pool = NULL;   // Выделено через u_malloc
    
//    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "queue_entry_new: created entry %p, size=%zu", entry, data_size);
    
    return entry;
}

struct ll_entry* queue_entry_new_from_pool(struct memory_pool* pool) {
    if (!pool) return NULL;
    
    struct ll_entry* entry = memory_pool_alloc(pool);
    if (!entry) return NULL;
    
    memset(entry, 0, pool->object_size);
    entry->size = pool->object_size - sizeof(struct ll_entry);
    entry->len = 0;
    entry->pool = pool;   // Выделено из пула
    
//    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "queue_entry_new_from_pool: created entry %p from pool %p", entry, pool);
    
    return entry;
}

//void ll_u_free_dgram(struct ll_entry* entry) {
void queue_dgram_free(struct ll_entry* entry) {
    if (!entry) return;

    if (entry->dgram) {
        if (entry->dgram_free_fn) {
            entry->dgram_free_fn(entry->dgram);  // arg=NULL, если не задан
        } else if (entry->dgram_pool) {
            memory_pool_free(entry->dgram_pool, entry->dgram);
        } else {
            u_free(entry->dgram);
        }
        entry->dgram = NULL;
        entry->len = 0;  // Опционально сброс len
    }
}

void queue_entry_free(struct ll_entry* entry) {
    if (!entry) return;
    
    if (entry->pool) {
        memory_pool_free(entry->pool, entry);
    } else {
        u_free(entry);
    }
}

// ==================== Операции с очередью ====================

// Внутренняя функция добавления в хеш-таблицу
static void add_to_hash(struct ll_queue* q, struct ll_entry* entry) {
    if (!q || !entry) return;

    if (q->hash_size == 0 || q->index_size == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] Empty hash_size=%d or index_size=%d",q->name, q->hash_size, q->index_size);
        return;
    }

    uint32_t slot = entry->index_hash % q->hash_size;
    entry->hash_next = q->hash_table[slot];
    q->hash_table[slot] = entry;
}

static void remove_from_hash(struct ll_queue* q, struct ll_entry* entry) {
    if (!q || q->hash_size == 0 || !entry) return;

    uint32_t slot = entry->index_hash % q->hash_size;  // теперь используем index_hash
    struct ll_entry** ptr = &q->hash_table[slot];
    while (*ptr) {
        if (*ptr == entry) {
            *ptr = entry->hash_next;
            entry->hash_next = NULL;
            return;
        }
        ptr = &(*ptr)->hash_next;
    }
}

static uint32_t make_hash(const void* data, uint16_t len) {// алгоритм FNV-1a
    if (len == 0 || data == NULL) return 0;

    const uint8_t* p = (const uint8_t*)data;
    uint32_t hash = 0x811C9DC5u;           // FNV-1a offset basis (32-bit)

    for (uint16_t i = 0; i < len; ++i) {
        hash ^= (uint32_t)p[i];             // XOR с байтом
        hash *= 0x01000193u;                // FNV-1a prime
    }

    return hash;
}

static void unlink_deferred_waiter(struct ll_queue* q, struct queue_waiter_handle* h) {
    struct queue_waiter_handle** p = &q->deferred_waiters;
    while (*p && *p != h) p = &(*p)->defer_next;
    if (*p) *p = h->defer_next;
    else DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] deferred waiter not found: handle=%p", q->name, h);
    h->defer_next = NULL; h->defer_q = NULL;
}

static void waiter_defer_cb(void* arg) {
    struct queue_waiter_handle* h = (struct queue_waiter_handle*)arg;
    struct ll_queue* q = h->defer_q;
    queue_threshold_callback_fn cb = h->defer_cb;
    void* cb_arg = h->defer_arg;
    h->call_soon_id = NULL;
    unlink_deferred_waiter(q, h);
    h->defer_cb = NULL; h->defer_arg = NULL;
    if (cb) cb(q, cb_arg);
    else DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] deferred waiter has no callback", q->name);
}

static void queue_waiter_call(struct ll_queue* q, queue_threshold_callback_fn cb, void* arg,
                               struct queue_waiter_handle* h) {
    if (q->waiter_defer) {
        if (h->call_soon_id) queue_waiter_cancel(h->defer_q, h);
        h->defer_q = q; h->defer_cb = cb; h->defer_arg = arg;
        h->defer_next = q->deferred_waiters; q->deferred_waiters = h;
        h->call_soon_id = uasync_schedule_work(q->ua, &h->work, waiter_defer_cb, h);
    } else {
        cb(q, arg);
    }
}

// Проверить и запустить первый ожидающий коллбэк (FIFO / round-robin)
static void check_waiters(struct ll_queue* q) {
    if (!q || !q->waiter_head) return;
    
    const int cond = q->count <= q->threshold_max_packets
                     && (q->threshold_max_bytes == 0 || q->total_bytes <= q->threshold_max_bytes);
    if (!cond) return;
    
    struct queue_waiter* waiter = q->waiter_head;
    q->waiter_head = waiter->next;
    if (!q->waiter_head) q->waiter_tail = NULL;
    
    struct queue_waiter_handle* h = waiter->handle;
    if (h) h->internal = NULL;

    queue_threshold_callback_fn cb = waiter->callback;
    void* arg = waiter->callback_arg;
    if (cb) {
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "check_waiters: waking head waiter, count=%d<=%d, bytes=%zu<=%zu",
                    q->count, q->threshold_max_packets, q->total_bytes, q->threshold_max_bytes);
        queue_waiter_call(q, cb, arg, h);
    }
    u_free(waiter);
}

int queue_append_tail(struct ll_queue* q, const uint8_t* data, uint16_t len, size_t capacity) {
#ifdef QUEUE_THREAD_CHECK
    queue_check_thread(q);
#endif
    struct ll_entry* tail = q->tail;
    if (!tail || !tail->len) return 0;
    size_t combined = (size_t)tail->len + len;
    if (combined > capacity || combined > UINT16_MAX) return 0;
    memcpy(tail->dgram + tail->len, data, len);
    tail->len += len;
    tail->int_len += len;
    q->total_bytes += len;
    return 1;
}

int queue_data_put(struct ll_queue* q, struct ll_entry* entry) {
    if (!q || !entry) return -1;
#ifdef QUEUE_THREAD_CHECK
    queue_check_thread(q);
#endif

    if (entry->owner || entry->prev || entry->next || q->head == entry || q->tail == entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] double put detected! entry=%p prev=%p next=%p len=%u",
                    q->name, (void*)entry, (void*)entry->prev, (void*)entry->next, entry->len);
        abort();
    }

    if (q->hash_size > 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] Put no-hash data to hash queue",q->name);
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return -1;
    }

    if (q->size_limit >= 0 && q->count >= q->size_limit) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "[%s] queue full: count=%d limit=%d", q->name, q->count, q->size_limit);
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return -1;
    }

    // Добавляем в конец
    entry->next = NULL;
    entry->prev = q->tail;
    if (q->tail) q->tail->next = entry; else q->head = entry;
    q->tail = entry;

    entry->owner = q;
    q->count++;
    entry->int_len = entry->len;
    q->total_bytes += entry->int_len;

    // НЕ вызываем add_to_hash

#ifdef QUEUE_DEBUG
    queue_check_consistency(q);
#endif
    // Пользовательский callback может освободить очередь.
    if (q->count == 1) queue_trigger_callback(q);
    return 0;
}

int queue_data_put_with_index(struct ll_queue* q, struct ll_entry* entry) {
    if (!q || !entry) return -1;
#ifdef QUEUE_THREAD_CHECK
    queue_check_thread(q);
#endif

    if (entry->owner || entry->prev || entry->next || q->head == entry || q->tail == entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] double put_with_index detected! entry=%p prev=%p next=%p",
                    q->name, (void*)entry, (void*)entry->prev, (void*)entry->next);
        abort();
    }

    if (q->index_size == 0 || (size_t)q->index_offset + q->index_size > (size_t)entry->size) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] invalid index: offset=%u size=%u (data_size=%u)",q->name, q->index_offset, q->index_size, entry->size);
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return -1;
    }

    entry->index_hash = make_hash(entry->data + q->index_offset, q->index_size);

    if (q->size_limit >= 0 && q->count >= q->size_limit) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "[%s] queue full: count=%d limit=%d", q->name, q->count, q->size_limit);
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return -1;
    }

    // Добавляем в конец
    entry->next = NULL;
    entry->prev = q->tail;
    if (q->tail) q->tail->next = entry; else q->head = entry;
    q->tail = entry;

    entry->owner = q;
    q->count++;
    entry->int_len = entry->len;
    q->total_bytes += entry->int_len;

    add_to_hash(q, entry);

#ifdef QUEUE_DEBUG
    queue_check_consistency(q);
#endif
    // Пользовательский callback может освободить очередь.
    if (q->count == 1) queue_trigger_callback(q);
    return 0;
}

int queue_data_put_first(struct ll_queue* q, struct ll_entry* entry) {
    if (!q || !entry) return -1;
#ifdef QUEUE_THREAD_CHECK
    queue_check_thread(q);
#endif

    if (entry->owner || entry->prev || entry->next || q->head == entry || q->tail == entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] double put_first detected! entry=%p prev=%p next=%p",
                    q->name, (void*)entry, (void*)entry->prev, (void*)entry->next);
        abort();
    }

    if (q->hash_size > 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] Put no-hash data to hash queue",q->name);
        queue_dgram_free(entry);
        queue_entry_free(entry);   // как было в оригинале
        return -1;
    }

    if (q->size_limit >= 0 && q->count >= q->size_limit) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "[%s] queue full: count=%d limit=%d", q->name, q->count, q->size_limit);
        queue_dgram_free(entry); queue_entry_free(entry); return -1;
    }
    entry->next = q->head;
    entry->prev = NULL;
    if (q->head) q->head->prev = entry; else q->tail = entry;
    q->head = entry;

    entry->owner = q;
    q->count++;
    entry->int_len = entry->len;
    q->total_bytes += entry->int_len;

#ifdef QUEUE_DEBUG
    queue_check_consistency(q);
#endif
    // Пользовательский callback может освободить очередь.
    if (q->count == 1) queue_trigger_callback(q);
    return 0;
}

// С хешем для put_first
int queue_data_put_first_with_index(struct ll_queue* q, struct ll_entry* entry) {
    if (!q || !entry) return -1;
#ifdef QUEUE_THREAD_CHECK
    queue_check_thread(q);
#endif

    if (entry->owner || entry->prev || entry->next || q->head == entry || q->tail == entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] double put_first_with_index detected! entry=%p prev=%p next=%p",
                    q->name, (void*)entry, (void*)entry->prev, (void*)entry->next);
        abort();
    }

    if (q->index_size == 0 || (size_t)q->index_offset + q->index_size > (size_t)entry->size) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] invalid index: offset=%u size=%u (data_size=%u)",q->name, q->index_offset, q->index_size, entry->size);
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return -1;
    }

    entry->index_hash = make_hash(entry->data + q->index_offset, q->index_size);

    if (q->size_limit >= 0 && q->count >= q->size_limit) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "[%s] queue full: count=%d limit=%d", q->name, q->count, q->size_limit);
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return -1;
    }

    entry->next = q->head;
    entry->prev = NULL;
    if (q->head) q->head->prev = entry; else q->tail = entry;
    q->head = entry;

    entry->owner = q;
    q->count++;
    entry->int_len = entry->len;
    q->total_bytes += entry->int_len;

    add_to_hash(q, entry);

#ifdef QUEUE_DEBUG
    queue_check_consistency(q);
#endif
    // Пользовательский callback может освободить очередь.
    if (q->count == 1) queue_trigger_callback(q);
    return 0;
}


struct ll_entry* queue_data_get(struct ll_queue* q) {
    if (!q || !q->head) return NULL;

#ifdef QUEUE_THREAD_CHECK
    queue_check_thread(q);
#endif

    struct ll_entry* entry = q->head;
    
    q->head = entry->next;
    if (q->head) q->head->prev = NULL;
    if (!q->head) q->tail = NULL;
    
    q->count--;
    q->total_bytes -= entry->int_len;
    
    entry->next = NULL;
    entry->prev = NULL;
    entry->owner = NULL;
    
    remove_from_hash(q, entry);
    
//    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "queue_data_get: got entry %p (id=%u), count=%d", entry, entry->id, q->count);
    
    // Приостановить коллбэки для предотвращения рекурсии
    q->callback_suspended = 1;
    
    if (q->count == 0 && q->empty_callback)
        uasync_schedule_work(q->ua, &q->empty_work, empty_trampoline, q);
    
    if (q->on_get) {
        q->on_get_pending++;
        if (!q->on_get_call_soon_id) {
            q->on_get_call_soon_id = uasync_call_soon(q->ua, q, on_get_trampoline);
            if (!q->on_get_call_soon_id) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] on_get scheduling failed: pending=%zu", q->name, q->on_get_pending);
        }
    }
    
#ifdef QUEUE_DEBUG
    queue_check_consistency(q);// !!!! for debug
#endif
    check_waiters(q);
    return entry;
}

int queue_entry_count(struct ll_queue* q) {
    return q ? q->count : 0;
}

// Функция проверки консистентности count и total_bytes
// Возвращает 0 если ok, -1 если есть несоответствия
int queue_check_consistency(struct ll_queue* q) {
    if (!q) return -1;
    
    if (q->count > 0 && !q->head) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Queue '%s': count=%d but head is NULL!",
                    q->name ? q->name : "unknown", q->count);
        return -1;
    }

    // Floyd cycle detection — за O(N) без риска зависания
    {
        struct ll_entry *slow = q->head, *fast = q->head;
        while (fast && fast->next) {
            slow = slow->next;
            fast = fast->next->next;
            if (slow == fast) {
                DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Queue '%s': CYCLE detected! entry=%p count=%d",
                            q->name ? q->name : "unknown", (void*)slow, q->count);
                abort();
            }
        }
    }
    
    int actual_count = 0;
    size_t actual_bytes = 0;
    struct ll_entry* current = q->head;
    
    while (current) {
        actual_count++;
        actual_bytes += current->int_len;
        if (current->next) {
            if (current->next->prev != current) {
                DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Queue '%s': prev/next error at entry %p != %p entries: %d!=%d bytes: %zu!=%zu",
                          q->name ? q->name : "unknown", (void*)current, (void*)current->next->prev, actual_count, q->count, actual_bytes, q->total_bytes);
                return -1;
            }
        }
        current = current->next;
    }
    
    // Проверить хвост
    if (q->tail && q->tail->next != NULL) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Queue '%s': tail error", q->name ? q->name : "unknown");
        return -1; // Хвост должен иметь next == NULL
    }
    
    // Сравнить с сохранёнными значениями
    if (actual_count != q->count || actual_bytes != q->total_bytes) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Queue '%s': count error entries: %d!=%d or bytes: %zu!=%zu",
                    q->name ? q->name : "unknown", actual_count, q->count, actual_bytes, q->total_bytes);
        return -1;
    }
    
    return 0;
}

// ==================== Пороговое ожидание (backpressure) ====================

void queue_set_threshold(struct ll_queue* q, int max_packets, size_t max_bytes) {
    if (!q) return;
    q->threshold_max_packets = max_packets;
    q->threshold_max_bytes = max_bytes;
}

void queue_set_waiter_defer(struct ll_queue* q, int enable) {
    if (!q) return;
    q->waiter_defer = enable;
}

void queue_set_callback_defer(struct ll_queue* q, int enable) {
    if (!q) return;
    q->callback_defer = enable;
}

void queue_set_empty_callback(struct ll_queue* q, queue_callback_fn cbk_fn, void* arg) {
    if (!q) return;
    uasync_cancel_work(&q->empty_work);
    q->empty_callback = cbk_fn;
    q->empty_callback_arg = arg;
    if (cbk_fn && q->count == 0)
        uasync_schedule_work(q->ua, &q->empty_work, empty_trampoline, q);
}

void queue_set_on_get(struct ll_queue* q, queue_callback_fn cbk_fn, void* arg) {
    if (!q) return;
    q->on_get = cbk_fn;
    q->on_get_arg = arg;
    if (!cbk_fn) {
        if (q->on_get_call_soon_id) uasync_call_soon_cancel(q->ua, q->on_get_call_soon_id);
        q->on_get_call_soon_id = NULL; q->on_get_pending = 0;
    }
}

int queue_waiter_wait(struct ll_queue* q, struct queue_waiter_handle* h,
                      queue_threshold_callback_fn callback, void* arg) {
    if (!q || !h || !callback) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "queue_waiter_wait: invalid arguments q=%p handle=%p callback=%p", q, h, (void*)callback);
        return -1;
    }

    if (h->internal || h->call_soon_id) queue_waiter_cancel(h->call_soon_id ? h->defer_q : q, h);

    if (q->count <= q->threshold_max_packets
        && (q->threshold_max_bytes == 0 || q->total_bytes <= q->threshold_max_bytes)) {
        queue_waiter_call(q, callback, arg, h);
        return 1;
    }

    struct queue_waiter* waiter = u_malloc(sizeof(struct queue_waiter));
    if (!waiter) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] queue_waiter_wait: u_malloc failed", q->name);
        return -1;
    }
    waiter->next = NULL;
    waiter->handle = h;
    waiter->callback = callback;
    waiter->callback_arg = arg;
    h->internal = waiter;

    if (q->waiter_tail) q->waiter_tail->next = waiter; else q->waiter_head = waiter;
    q->waiter_tail = waiter;

    return 0;
}

void queue_waiter_cancel(struct ll_queue* q, struct queue_waiter_handle* h) {
    if (!q || !h) return;
    /* Отложенный вызов (waiter_defer) может висеть и при internal==NULL:
     * queue_waiter_wait по ветке «очередь свободна» планирует call_soon_id,
     * не регистрируя waiter. Ранний выход по !internal оставлял бы висячий
     * callback с arg=handle, который после teardown разыменовывал бы freed-память. */
    if (h->call_soon_id) {
        struct ll_queue* deferred_q = h->defer_q;
        uasync_cancel_work(&h->work); h->call_soon_id = NULL;
        unlink_deferred_waiter(deferred_q, h); h->defer_cb = NULL; h->defer_arg = NULL;
    }
    if (!h->internal) return;

    struct queue_waiter* waiter = h->internal;
    struct queue_waiter* prev = NULL;
    struct queue_waiter* curr = q->waiter_head;
    while (curr && curr != waiter) {
        prev = curr;
        curr = curr->next;
    }
    if (!curr) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[%s] queue_waiter_cancel: waiter not in list", q->name);
        h->internal = NULL;
        return;
    }
    
    if (prev) prev->next = waiter->next; else q->waiter_head = waiter->next;
    if (waiter == q->waiter_tail) q->waiter_tail = prev;
    
    h->internal = NULL;
    u_free(waiter);
}

// ==================== Поиск и удаление по ID ====================

struct ll_entry* queue_find_data_by_index(struct ll_queue* q, const void* index_key) {
    if (!q || q->hash_size == 0 || !q->hash_table || q->index_size == 0 || !index_key) return NULL;

    uint32_t hash_val = make_hash(index_key, q->index_size);

    uint32_t slot = hash_val % q->hash_size;
    struct ll_entry* entry = q->hash_table[slot];

    while (entry) {
        if ((size_t)q->index_offset + q->index_size <= (size_t)entry->size &&
            memcmp(entry->data + q->index_offset, index_key, q->index_size) == 0) {
            return entry;
        }
        entry = entry->hash_next;
    }
    return NULL;
}

struct ll_entry* queue_find_next_by_index(struct ll_queue* q, const void* index_key,
                                           struct ll_entry* prev_entry) {
    if (!q || q->hash_size == 0 || !index_key || q->index_size == 0 || !prev_entry) return NULL;

    struct ll_entry* entry = prev_entry->hash_next;
    while (entry) {
        if ((size_t)q->index_offset + q->index_size <= (size_t)entry->size &&
            memcmp(entry->data + q->index_offset, index_key, q->index_size) == 0) {
            return entry;
        }
        entry = entry->hash_next;
    }
    return NULL;
}

int queue_remove_data(struct ll_queue* q, struct ll_entry* entry) {
    if (!q || !entry) return -1;
    if (entry->owner != q) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "[%s] cannot remove foreign entry=%p owner=%p", q->name, entry, entry->owner);
        return -1;
    }
    
#ifdef QUEUE_THREAD_CHECK
    queue_check_thread(q);
#endif
    
    // Удалить из двусвязного списка
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        q->head = entry->next;
    }
    
    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        q->tail = entry->prev;
    }
    
    q->count--;
    q->total_bytes -= entry->int_len;
    
    entry->next = NULL;
    entry->prev = NULL;
    entry->owner = NULL;
    
    remove_from_hash(q, entry);
    
//    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "queue_remove_data: removed entry %p (id=%u), count=%d", entry, entry->id, q->count);
#ifdef QUEUE_DEBUG
    queue_check_consistency(q);// !!!! for debug
#endif    
    if (q->count == 0 && q->empty_callback)
        uasync_schedule_work(q->ua, &q->empty_work, empty_trampoline, q);
    check_waiters(q);
    return 0;
}
