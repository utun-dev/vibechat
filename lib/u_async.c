// uasync.c

// CLOCK_BOOTTIME (Linux) требует _GNU_SOURCE в glibc.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "u_async.h"
#include "platform_compat.h"
#include "debug_config.h"
#include "mem.h"
#include "memory_pool.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>

#include "../lib/platform_compat.h"
//#ifdef _WIN32
//#include <windows.h>
//#else
//#include <sys/time.h>
//#endif

// Platform-specific includes
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#define HAS_EPOLL 1
#else
#define HAS_EPOLL 0
#endif



// Timeout node with safe cancellation
struct timeout_node {
    const char* name;
    void* arg;
    timeout_callback_t callback;
    uint64_t expiration_ms; // absolute expiration time in milliseconds
    struct UASYNC* ua;      // Pointer back to uasync instance for counter updates
    struct uasync_work work; // FIFO для однократного call_soon.
    int reusable;            // Запись остаётся в heap до явной отмены.
    size_t heap_index;       // Position in timeout_heap (SIZE_MAX if not in heap)
};

// Socket node with array-based storage
struct socket_node {
    int fd;                     // File descriptor (for pipe, file)
    socket_t sock;              // Socket (for cross-platform sockets)
    int type;                   // SOCKET_NODE_TYPE_FD or SOCKET_NODE_TYPE_SOCK
    socket_callback_t read_cbk;       // For FD type
    socket_callback_t write_cbk;      // For FD type
    socket_t_callback_t read_cbk_sock;  // For SOCK type
    socket_t_callback_t write_cbk_sock; // For SOCK type
    socket_callback_t except_cbk;
    void* user_data;
    const char* name; // Строковый идентификатор сокета для диагностики (литерал)
    int active;       // 1 if socket is active, 0 if freed (for reuse)
    int enable_read;  // 1 if read monitoring is enabled
    int enable_write; // 1 if write monitoring is enabled
    uint32_t gen;     // generation counter for epoll event validation
    uint32_t poll_gen; // Поколение на момент poll/select, до пользовательских callbacks
};

// Array-based socket management for O(1) operations
struct socket_array {
    struct socket_node* sockets;      // Dynamic array of socket nodes
    int* fd_to_index;                // FD to array index mapping
    int* index_to_fd;                // Array index to FD mapping
    int* active_indices;             // Array of indices of active sockets (for O(1) traversal)
    int capacity;                    // Total allocated capacity
    int count;                       // Number of active sockets
    int max_fd;                      // Maximum FD for bounds checking
    uint32_t gen_counter;            // incrementing generation for epoll stale-event detection
};

static struct socket_array* socket_array_create(int initial_capacity);
static void socket_array_destroy(struct socket_array* sa);
static int socket_array_add(struct socket_array* sa, int fd, socket_callback_t read_cbk, socket_callback_t write_cbk, socket_callback_t except_cbk, const char* name, void* user_data);
static int socket_array_remove(struct socket_array* sa, int fd);
static struct socket_node* socket_array_get(struct socket_array* sa, int fd);

// No global instance - each module must use its own struct UASYNC instance

// Array-based socket management implementation
static struct socket_array* socket_array_create(int initial_capacity) {
    if (initial_capacity < 4) initial_capacity = 4;  // Minimum capacity
    
    struct socket_array* sa = u_malloc(sizeof(struct socket_array));
    if (!sa) return NULL;
    sa->gen_counter = 0;
    
    sa->sockets = u_calloc(initial_capacity, sizeof(struct socket_node));
    sa->fd_to_index = u_calloc(initial_capacity, sizeof(int));
    sa->index_to_fd = u_calloc(initial_capacity, sizeof(int));
    sa->active_indices = u_calloc(initial_capacity, sizeof(int));
    
    if (!sa->sockets || !sa->fd_to_index || !sa->index_to_fd || !sa->active_indices) {
        u_free(sa->sockets);
        u_free(sa->fd_to_index);
        u_free(sa->index_to_fd);
        u_free(sa->active_indices);
        u_free(sa);
        return NULL;
    }
    
    // Initialize mapping arrays to -1 (invalid)
    for (int i = 0; i < initial_capacity; i++) {
        sa->fd_to_index[i] = -1;
        sa->index_to_fd[i] = -1;
        sa->active_indices[i] = -1;
        sa->sockets[i].fd = -1;
        sa->sockets[i].active = 0;
    }
    
    sa->capacity = initial_capacity;
    sa->count = 0;
    sa->max_fd = -1;
    
    return sa;
}

static void socket_array_destroy(struct socket_array* sa) {
    if (!sa) return;
    
    u_free(sa->sockets);
    u_free(sa->fd_to_index);
    u_free(sa->index_to_fd);
    u_free(sa->active_indices);
    u_free(sa);
}

static int socket_array_add_internal(struct socket_array* sa, int fd, socket_t sock, int type,
                                     socket_callback_t read_cbk_fd, socket_callback_t write_cbk_fd,
                                     socket_t_callback_t read_cbk_sock, socket_t_callback_t write_cbk_sock,
                                     socket_callback_t except_cbk, const char* name, void* user_data) {
    if (!sa || fd < 0) return -1;
    if (fd >= sa->capacity) {
        if (fd > INT_MAX - 16) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket array: fd=%d exceeds supported range", fd);
            return -1;
        }
        int new_capacity = sa->capacity <= INT_MAX / 2 ? sa->capacity * 2 : INT_MAX;
        if (fd >= new_capacity) new_capacity = fd + 16;

        /* Сначала выделяем новый набор целиком: отказ не затрагивает рабочие массивы. */
        struct socket_node* new_sockets = u_calloc(new_capacity, sizeof(struct socket_node));
        int* new_fd_to_index = u_calloc(new_capacity, sizeof(int));
        int* new_index_to_fd = u_calloc(new_capacity, sizeof(int));
        int* new_active_indices = u_calloc(new_capacity, sizeof(int));
        
        if (!new_sockets || !new_fd_to_index || !new_index_to_fd || !new_active_indices) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket array growth failed: fd=%d capacity=%d requested=%d", fd, sa->capacity, new_capacity);
            u_free(new_sockets);
            u_free(new_fd_to_index);
            u_free(new_index_to_fd);
            u_free(new_active_indices);
            return -1;
        }

        memcpy(new_sockets, sa->sockets, sa->capacity * sizeof(*new_sockets));
        memcpy(new_fd_to_index, sa->fd_to_index, sa->capacity * sizeof(*new_fd_to_index));
        memcpy(new_index_to_fd, sa->index_to_fd, sa->capacity * sizeof(*new_index_to_fd));
        memcpy(new_active_indices, sa->active_indices, sa->capacity * sizeof(*new_active_indices));
        
        // Initialize new elements
        for (int i = sa->capacity; i < new_capacity; i++) {
            new_fd_to_index[i] = -1;
            new_index_to_fd[i] = -1;
            new_active_indices[i] = -1;
            new_sockets[i].fd = -1;
            new_sockets[i].active = 0;
        }
        
        u_free(sa->sockets); u_free(sa->fd_to_index); u_free(sa->index_to_fd); u_free(sa->active_indices);
        DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "socket array grown: capacity=%d->%d fd=%d", sa->capacity, new_capacity, fd);
        sa->sockets = new_sockets;
        sa->fd_to_index = new_fd_to_index;
        sa->index_to_fd = new_index_to_fd;
        sa->active_indices = new_active_indices;
        sa->capacity = new_capacity;
    }
    
    // Check if FD already has a node — reuse inactive slot if present
    int index;
    int existing = sa->fd_to_index[fd];
    if (existing != -1) {
        if (sa->sockets[existing].active) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "socket already registered: fd=%d name=%s", fd, sa->sockets[existing].name);
            return -1;
        }
        index = existing;  // переиспользуем неактивный слот (после socket_array_remove)
    } else {
        // Find first free slot
        index = -1;
        for (int i = 0; i < sa->capacity; i++) {
            if (!sa->sockets[i].active) { index = i; break; }
        }
        if (index == -1) return -1;  // No free slots
        sa->fd_to_index[fd] = index;  // новая привязка fd→слот
    }
    
    // Add the socket
    sa->sockets[index].fd = fd;
    sa->sockets[index].sock = sock;
    sa->sockets[index].type = type;
    sa->sockets[index].read_cbk = read_cbk_fd;
    sa->sockets[index].write_cbk = write_cbk_fd;
    sa->sockets[index].read_cbk_sock = read_cbk_sock;
    sa->sockets[index].write_cbk_sock = write_cbk_sock;
    sa->sockets[index].except_cbk = except_cbk;
    sa->sockets[index].user_data = user_data;
    sa->sockets[index].name = name ? name : "?";
    sa->sockets[index].active = 1;
    sa->sockets[index].enable_read = (read_cbk_fd != NULL || read_cbk_sock != NULL) ? 1 : 0;
    sa->sockets[index].enable_write = (write_cbk_fd != NULL || write_cbk_sock != NULL) ? 1 : 0;
    sa->sockets[index].gen = ++sa->gen_counter;
    sa->sockets[index].poll_gen = 0;
    
    sa->index_to_fd[index] = fd;
    sa->active_indices[sa->count] = index;  // Add to active list
    sa->count++;
    
    if (fd > sa->max_fd) sa->max_fd = fd;
    
    return index;
}

// Wrapper for adding regular file descriptors (pipe, file)
static int socket_array_add(struct socket_array* sa, int fd, socket_callback_t read_cbk, 
                            socket_callback_t write_cbk, socket_callback_t except_cbk, const char* name, void* user_data) {
    return socket_array_add_internal(sa, fd, SOCKET_INVALID, SOCKET_NODE_TYPE_FD,
                                     read_cbk, write_cbk, NULL, NULL, except_cbk, name, user_data);
}

// Wrapper for adding socket_t (cross-platform sockets)
static int socket_array_add_socket_t(struct socket_array* sa, socket_t sock, socket_t_callback_t read_cbk,
                                     socket_t_callback_t write_cbk, socket_callback_t except_cbk, const char* name, void* user_data) {
    // On Windows, SOCKET is UINT_PTR, so we need to handle indexing differently
#ifdef _WIN32
    int fd = (int)(intptr_t)sock;  // Use socket value as index on Windows (simplified)
    if (fd < 0) return -1;  // Windows sockets can have any value, only check negative
#else
    int fd = sock;  // On POSIX, socket_t is int
    if (fd < 0) return -1;
#endif
    return socket_array_add_internal(sa, fd, sock, SOCKET_NODE_TYPE_SOCK,
                                     NULL, NULL, read_cbk, write_cbk, except_cbk, name, user_data);
}

static int socket_array_remove(struct socket_array* sa, int fd) {
    if (!sa || fd < 0 || fd >= sa->capacity) return -1;
    
    int index = sa->fd_to_index[fd];
    if (index == -1 || !sa->sockets[index].active) return -1;  // FD not found
    
    // Mark as inactive and clear all pointers
    sa->sockets[index].active = 0;
    sa->sockets[index].fd = -1;
    sa->sockets[index].sock = SOCKET_INVALID;
    sa->sockets[index].type = SOCKET_NODE_TYPE_FD;
    sa->sockets[index].read_cbk = NULL;
    sa->sockets[index].write_cbk = NULL;
    sa->sockets[index].read_cbk_sock = NULL;
    sa->sockets[index].write_cbk_sock = NULL;
    sa->sockets[index].except_cbk = NULL;
    sa->sockets[index].user_data = NULL;
    sa->sockets[index].name = NULL;
    sa->sockets[index].enable_read = 0;
    sa->sockets[index].enable_write = 0;
    sa->fd_to_index[fd] = -1;
    sa->index_to_fd[index] = -1;
    
    // Remove from active_indices by swapping with last element
    // Find position in active_indices
    for (int i = 0; i < sa->count; i++) {
        if (sa->active_indices[i] == index) {
            // Swap with last element
            sa->active_indices[i] = sa->active_indices[sa->count - 1];
            sa->active_indices[sa->count - 1] = -1;
            break;
        }
    }
    sa->count--;
    
    return 0;
}

static struct socket_node* socket_array_get(struct socket_array* sa, int fd) {
    if (!sa || fd < 0 || fd >= sa->capacity) return NULL;
    
    int index = sa->fd_to_index[fd];
    if (index == -1 || !sa->sockets[index].active) return NULL;
    
    return &sa->sockets[index];
}

// Get socket_node by socket_t
static struct socket_node* socket_array_get_by_sock(struct socket_array* sa, socket_t sock) {
    if (!sa) return NULL;
#ifdef _WIN32
    int fd = (int)(intptr_t)sock;
#else
    int fd = sock;
#endif
    if (fd < 0 || fd >= sa->capacity) return NULL;
    
    int index = sa->fd_to_index[fd];
    if (index == -1 || !sa->sockets[index].active) return NULL;
    if (sa->sockets[index].type != SOCKET_NODE_TYPE_SOCK) return NULL;
    
    return &sa->sockets[index];
}

// Предохранитель: снять сокет с мониторинга по fd (SOCK или FD тип), если он
// оказался в HUP/ERR без except-обработчика. Не закрывает fd — только прекращает
// мониторинг, чтобы event loop не ушёл в busy-loop на вечно готовом сокете.
static void socket_force_unregister(struct UASYNC* ua, int fd) {
    if (!ua || fd < 0) return;
#if HAS_EPOLL
    if (ua->use_epoll && ua->epoll_fd >= 0) {
        epoll_ctl(ua->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    }
#endif
    if (socket_array_remove(ua->sockets, fd) == 0) {
        ua->socket_free_count++;
        ua->poll_fds_dirty = 1;
    }
}

// Callback to u_free timeout node and update counters
static void timeout_node_free_callback(void* user_data, void* data) {
    struct UASYNC* ua = (struct UASYNC*)user_data;
    struct timeout_node* node = (struct timeout_node*)data;
    (void)node; // Not used directly, but keep for consistency
    ua->timer_free_count++;
    memory_pool_free(ua->timeout_pool, data);
}

#ifdef _WIN32
uint64_t get_time_tb(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    double t = (double)count.QuadPart * 10000.0 / (double)freq.QuadPart;
    return (uint64_t)t;
}
//uint64_t get_time_tb(void) {
//    LARGE_INTEGER freq, count;
//    QueryPerformanceFrequency(&freq);  // Получаем частоту таймера
//    QueryPerformanceCounter(&count);   // Получаем текущее значение счётчика
//    return (uint64_t)(count.QuadPart * 10000ULL) / (uint64_t)freq.QuadPart;  // Преобразуем в требуемые единицы времени
//}
#else
uint64_t get_time_tb(void) {
    struct timespec ts;
#ifdef __linux__
    clock_gettime(CLOCK_BOOTTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 10000ULL + (uint64_t)ts.tv_nsec / 100000ULL;  // Преобразуем в требуемые единицы времени
}
#endif

#ifdef _WIN32
uint64_t get_time_us(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    uint64_t ticks = (uint64_t)count.QuadPart, hz = (uint64_t)freq.QuadPart;
    return ticks / hz * 1000000ULL + ticks % hz * 1000000ULL / hz;
}
#else
uint64_t get_time_us(void) {
    struct timespec ts;
#ifdef __linux__
    clock_gettime(CLOCK_BOOTTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif



// Drain wakeup pipe - read all available bytes
static void drain_wakeup_pipe(struct UASYNC* ua) {
    if (!ua || !ua->wakeup_initialized) return;
    
    uint64_t val;
    while (read(ua->wakeup_pipe[0], &val, sizeof(val)) > 0) {}
}

/* Стек пачек сохраняет доступ к ожидающим задачам при отмене из вложенного poll. */
struct uasync_post_batch {
    struct uasync_post_batch* previous;
    struct posted_task* head;
};

// Исполняем снимок очереди без блокировки; новые post остаются для следующей пачки.
static void process_posted_tasks(struct UASYNC* ua) {
    if (!ua) return;

    struct uasync_post_batch batch = { .previous = ua->posted_batches };
#ifdef _WIN32
    EnterCriticalSection(&ua->posted_lock);
#else
    pthread_mutex_lock(&ua->posted_lock);
#endif
    batch.head = ua->posted_tasks_head;
    ua->posted_tasks_head = ua->posted_tasks_tail = NULL;
#ifdef _WIN32
    LeaveCriticalSection(&ua->posted_lock);
#else
    pthread_mutex_unlock(&ua->posted_lock);
#endif

    ua->posted_batches = &batch;
    while (batch.head) {
        struct posted_task* t = batch.head;
        batch.head = t->next; // Исполняемый callback уже не подлежит отмене.
        DEBUG_TRACE(DEBUG_CATEGORY_SYS, "uasync: executing post task=%p arg=%p", (void*)t, t->arg);

        if (t->callback) {
            t->callback(t->arg);
        }
        u_free(t);
    }
    ua->posted_batches = batch.previous;
}

#ifdef _WIN32
// Unified wakeup handler (drain + execute posted callbacks)
static void handle_wakeup(struct UASYNC* ua) {
    if (!ua || !ua->wakeup_initialized) return;

    // Drain the wakeup pipe/socket
#ifdef _WIN32
    char buf[64];
    SOCKET s = (SOCKET)(intptr_t)ua->wakeup_pipe[0];
    while (recv(s, buf, sizeof(buf), 0) > 0) {}
#else
    uint64_t val;
    while (read(ua->wakeup_pipe[0], &val, sizeof(val)) > 0) {}
#endif

//    DEBUG_DEBUG(DEBUG_CATEGORY_TIMERS, "POST: wakeup process");
    // Execute all posted callbacks (in main thread)
    process_posted_tasks(ua);
}


#endif

struct uasync_work* uasync_schedule_work(struct UASYNC* ua, struct uasync_work* work, timeout_callback_t callback, void* arg) {
    if (work->ua) return work;
    work->ua = ua; work->callback = callback; work->arg = arg;
    work->sequence = ++ua->immediate_sequence;
    work->prev = ua->immediate_queue_tail;
    if (work->prev) work->prev->next = work;
    else ua->immediate_queue_head = work;
    ua->immediate_queue_tail = work;
    return work;
}

void uasync_cancel_work(struct uasync_work* work) {
    if (!work->ua) return;
    if (work->prev) work->prev->next = work->next;
    else work->ua->immediate_queue_head = work->next;
    if (work->next) work->next->prev = work->prev;
    else work->ua->immediate_queue_tail = work->prev;
    work->ua = NULL; work->next = NULL; work->prev = NULL;
}

static void run_soon_once(void* arg) {
    struct timeout_node* node = arg;
    if (node->callback) node->callback(node->arg);
    uasync_release_soon(node);
}

void process_immediate_queue(struct UASYNC* ua) {
    // Номер остаётся границей даже при отмене/перепостановке последнего элемента из callback.
    uint64_t last = ua->immediate_sequence;
    while (ua->immediate_queue_head) {
        struct uasync_work* work = ua->immediate_queue_head;
        if (last - work->sequence > UINT64_MAX / 2) break;
        uasync_cancel_work(work);
        work->callback(work->arg);
    }
}

void uasync_drain_immediate(struct UASYNC* ua) {
    if (ua) while (ua->immediate_queue_head) process_immediate_queue(ua);
}

// Process expired timeouts with safe cancellation
static void process_timeouts(struct UASYNC* ua) {
    if (!ua) return;

    process_immediate_queue(ua);
    
    if (!ua->timeout_heap) return;
    
    uint64_t now_ms = get_time_us() / 1000;
    
    while (1) {
        TimeoutEntry entry;
        if (timeout_heap_peek(ua->timeout_heap, &entry) != 0) break;
        if (entry.expiration > now_ms) break;
        
        struct timeout_node* node = entry.data;
        if (node->reusable) {
            uasync_timer_set(node, -1);
            node->callback(node->arg);
            continue;
        }
        // Pop the expired timeout
        if (timeout_heap_pop(ua->timeout_heap, &entry) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "expired timer disappeared between peek/pop ua=%p heap_size=%zu", ua, ua->timeout_heap->size);
            break;
        }
        if (node && memory_pool_is_freed(ua->timeout_pool, node)) {
            DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "expired timer already freed before callback: ua=%p node=%p expiration=%llu",
                        ua, node, (unsigned long long)entry.expiration);
            memory_pool_free(ua->timeout_pool, node);  /* Подробный лог пула и остановка на повреждении. */
        }
        const char* name = node ? node->name : "?";
        timeout_callback_t callback = node ? node->callback : NULL;
        void* arg = node ? node->arg : NULL;
        
        if (node && node->callback) {
            DEBUG_DEBUG(DEBUG_CATEGORY_TIMERS, "timer expired: name=%s node=%p callback=%p arg=%p expiration=%llu",
                        name, node, (void*)callback, arg, (unsigned long long)entry.expiration);
            node->callback(node->arg);
        }
        if (node && memory_pool_is_freed(ua->timeout_pool, node))
            DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "timer freed inside callback: ua=%p name=%s node=%p callback=%p arg=%p",
                        ua, name, node, (void*)callback, arg);
        
        // Always u_free the node after processing
        if (node && node->ua) {
            node->ua->timer_free_count++;
        }
        memory_pool_free(ua->timeout_pool, node);
        continue;  // Process next expired timeout
    }
}

// -1 позволяет poll спать до события, включая случай только неактивных резервов.
static int next_timeout_ms(struct UASYNC* ua) {
    TimeoutEntry entry;
    if (timeout_heap_peek(ua->timeout_heap, &entry) != 0 || entry.expiration == UINT64_MAX) return -1;
    uint64_t now_ms = get_time_us() / 1000;
    if (entry.expiration <= now_ms) return 0;
    uint64_t delta = entry.expiration - now_ms;
    return delta > INT_MAX ? INT_MAX : (int)delta;
}

static uint64_t timeout_expiration_ms(int timeout_tb) {
    uint64_t us = get_time_us();
    return timeout_tb ? (us + (uint64_t)timeout_tb * 100 + 999) / 1000 : us / 1000;
}

static struct timeout_node* create_timer(struct UASYNC* ua, int timeout_tb, void* arg, timeout_callback_t callback, const char* name) {
    if (!ua || !ua->timeout_heap || !callback) {
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "cannot create timer %s: invalid loop or callback", name ? name : "");
        return NULL;
    }
    struct timeout_node* node = memory_pool_alloc(ua->timeout_pool);
    if (!node) {
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "timer allocation failed name=%s", name ? name : "");
        return NULL;
    }
    ua->timer_alloc_count++;
    node->name = name ? name : "";
    node->arg = arg; node->callback = callback; node->ua = ua;
    node->reusable = timeout_tb < 0;
    node->expiration_ms = node->reusable ? UINT64_MAX : timeout_expiration_ms(timeout_tb);
    if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_TIMERS) && name && strncmp(name, "ncd_connect", 11) == 0) {
        DEBUG_DEBUG(DEBUG_CATEGORY_TIMERS, "set_timeout name=%s tb=%d expiration_ms=%llu",
                    name, timeout_tb, (unsigned long long)node->expiration_ms);
    }
    
    // Add to heap
    if (timeout_heap_push(ua->timeout_heap, node->expiration_ms, node, &node->heap_index) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "uasync_set_timeout: failed to push to heap");
        memory_pool_free(ua->timeout_pool, node);
        ua->timer_free_count++;  // Balance the alloc counter
        return NULL;
    }
    
    return node;
}

void* uasync_set_timeout(struct UASYNC* ua, int timeout_tb, void* arg, timeout_callback_t callback, const char* name) {
    if (timeout_tb < 0) { DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "invalid timeout=%d", timeout_tb); return NULL; }
    return create_timer(ua, timeout_tb, arg, callback, name);
}

struct timeout_node* uasync_reserve_timer(struct UASYNC* ua, void* arg, timeout_callback_t callback, const char* name) {
    return create_timer(ua, -1, arg, callback, name);
}

void uasync_timer_set(struct timeout_node* timer, int timeout_tb) {
    if (!timer || !timer->reusable || timeout_tb == 0 || timeout_tb < -1) {
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "invalid reserved timer=%p delay=%d", (void*)timer, timeout_tb);
        return;
    }
    uint64_t expiration = timeout_tb < 0 ? UINT64_MAX : timeout_expiration_ms(timeout_tb);
    if (expiration == timer->expiration_ms) return;
    timer->expiration_ms = expiration;
    timeout_heap_reschedule(timer->ua->timeout_heap, timer->heap_index, expiration);
    DEBUG_TRACE(DEBUG_CATEGORY_TIMERS, "reserved timer %s delay_tb=%d", timer->name, timeout_tb);
}

int uasync_timer_armed(const struct timeout_node* timer) {
    return timer && timer->expiration_ms != UINT64_MAX;
}

struct timeout_node* uasync_reserve_soon(struct UASYNC* ua, void* user_arg, timeout_callback_t callback) {
    if (!ua || !ua->timeout_pool || !callback) {
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "cannot reserve immediate callback: invalid loop or callback");
        return NULL;
    }
    
    struct timeout_node* node = memory_pool_alloc(ua->timeout_pool);
    if (!node) {
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "cannot reserve immediate callback: allocation failed");
        return NULL;
    }
    ua->timer_alloc_count++;
    
    node->name = "";
    node->arg = user_arg;
    node->callback = callback;
    node->ua = ua;
    node->expiration_ms = 0;
    node->heap_index = SIZE_MAX;
    return node;
}

void uasync_release_soon(struct timeout_node* node) {
    timeout_node_free_callback(node->ua, node);
}

void uasync_call_soon_reserved(struct timeout_node* node) {
    uasync_schedule_work(node->ua, &node->work, run_soon_once, node);
}

void* uasync_call_soon(struct UASYNC* ua, void* user_arg, timeout_callback_t callback) {
    struct timeout_node* node = uasync_reserve_soon(ua, user_arg, callback);
    if (node) uasync_call_soon_reserved(node);
    return node;
}

// Cancel immediate callback by setting callback to NULL - O(1)
err_t uasync_call_soon_cancel(struct UASYNC* ua, void* t_id) {
    if (!ua || !t_id) return ERR_FAIL;
    
    struct timeout_node* node = (struct timeout_node*)t_id;
    if (node->ua != ua) return ERR_FAIL;
    
    // Simply nullify callback - will be skipped in process_timeouts
    node->callback = NULL;
    
    return ERR_OK;
}



// Instance version
err_t uasync_cancel_timeout(struct UASYNC* ua, void* t_id) {
    if (!ua || !t_id || !ua->timeout_heap) {
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "uasync_cancel_timeout: invalid parameters ua=%p, t_id=%p, heap=%p", 
                    ua, t_id, ua ? ua->timeout_heap : NULL);
        return ERR_FAIL;
    }

    struct timeout_node* node = (struct timeout_node*)t_id;

    if (node->heap_index == SIZE_MAX || node->heap_index >= ua->timeout_heap->size ||
        ua->timeout_heap->heap[node->heap_index].data != node) {
        DEBUG_DEBUG(DEBUG_CATEGORY_TIMERS, "uasync_cancel_timeout: not found in heap: ua=%p, t_id=%p, node=%p, expires=%llu ms", 
                    ua, t_id, node, (unsigned long long)node->expiration_ms);
        return ERR_FAIL;
    }
    
    if (timeout_heap_cancel_at(ua->timeout_heap, node->heap_index, node) == 0) {
        node->heap_index = SIZE_MAX;
        node->callback = NULL;
        return ERR_OK;
    }

    return ERR_FAIL;
}


#ifndef _WIN32
// Память для poll резервируется до регистрации, когда API ещё может вернуть ошибку.
static int reserve_poll_fds(struct UASYNC* ua, int required) {
    if (ua->use_epoll) return 0;
    const int limit = INT_MAX / sizeof(struct pollfd);
    if (required < 0 || required > limit) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "poll array capacity overflow: required=%d limit=%d", required, limit);
        return -1;
    }
    if (required <= ua->poll_fds_capacity) return 0;
    int capacity = ua->poll_fds_capacity <= limit / 2 ? ua->poll_fds_capacity * 2 : limit;
    if (capacity < 16) capacity = 16;
    if (capacity < required) capacity = required;
    struct pollfd* fds = u_realloc(ua->poll_fds, (size_t)capacity * sizeof(*fds));
    if (!fds) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "poll array reserve failed: capacity=%d requested=%d required=%d",
                    ua->poll_fds_capacity, capacity, required);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "poll array reserved: capacity=%d->%d required=%d",
                ua->poll_fds_capacity, capacity, required);
    ua->poll_fds = fds;
    ua->poll_fds_capacity = capacity;
    return 0;
}
#endif

// Instance version
void* uasync_add_socket(struct UASYNC* ua, int fd, socket_callback_t read_cbk, socket_callback_t write_cbk, socket_callback_t except_cbk, const char* name, void* user_data) {
    if (!ua || fd < 0) return NULL;
#ifndef _WIN32
    if (reserve_poll_fds(ua, ua->sockets->count + 2) < 0) return NULL; // Новый fd и wakeup.
#endif

    int index = socket_array_add(ua->sockets, fd, read_cbk, write_cbk, except_cbk, name, user_data);
    if (index < 0) return NULL;
    
    ua->socket_alloc_count++;
    ua->poll_fds_dirty = 1;  // Mark poll_fds as needing rebuild
    
#if HAS_EPOLL
    // Add to epoll if using epoll
    if (ua->use_epoll && ua->epoll_fd >= 0) {
        struct epoll_event ev;
        ev.events = 0;
        if (read_cbk) ev.events |= EPOLLIN;
        if (write_cbk) ev.events |= EPOLLOUT;
        if (except_cbk) ev.events |= EPOLLPRI;
        // Embed gen in upper 32 bits of data.u64 for stale-event detection
        ev.data.u64 = ((uint64_t)ua->sockets->sockets[index].gen << 32) | (uint32_t)fd;
        
        if (epoll_ctl(ua->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "epoll add failed: fd=%d name=%s error=%s", fd, name ? name : "?", strerror(errno));
            // Failed to add to epoll - remove from socket array and return error
            socket_array_remove(ua->sockets, fd);
            ua->socket_alloc_count--;
            return NULL;
        }
    }
#endif
    
    DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "socket_add fd=%d name='%s' type=FD r=%p w=%p e=%p ud=%p",
                fd, name ? name : "?", (void*)read_cbk, (void*)write_cbk, (void*)except_cbk, user_data);

    // Return index-based handle
    return (void*)(uintptr_t)(index + 1);
}

err_t uasync_remove_socket(struct UASYNC* ua, void* s_id) {
    if (!ua || !s_id) return ERR_FAIL;
    
    int i = (int)(uintptr_t)s_id - 1;
    if (i < 0 || i >= ua->sockets->capacity) return ERR_FAIL;
    struct socket_node* node = &ua->sockets->sockets[i];
    if (!node->active || node->fd < 0) return ERR_FAIL;
    
    int fd = node->fd;
    const char* rname = node->name ? node->name : "?";
    
#if HAS_EPOLL
    // Remove from epoll if using epoll
    if (ua->use_epoll && ua->epoll_fd >= 0) {
        epoll_ctl(ua->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    }
#endif
    
    int ret = socket_array_remove(ua->sockets, fd);
    if (ret == 0) {
        ua->socket_free_count++;
        ua->poll_fds_dirty = 1;  // Mark poll_fds as needing rebuild
        DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "socket_remove fd=%d name='%s'", fd, rname);
        return ERR_OK;
    }
    return ERR_FAIL;
}

// Add socket_t (cross-platform socket)
void* uasync_add_socket_t(struct UASYNC* ua, socket_t sock, socket_t_callback_t read_cbk, 
                          socket_t_callback_t write_cbk, socket_t_callback_t except_cbk, const char* name, void* user_data) {
    if (!ua || sock == SOCKET_INVALID) return NULL;
#ifndef _WIN32
    if (reserve_poll_fds(ua, ua->sockets->count + 2) < 0) return NULL;
#endif

    int index = socket_array_add_socket_t(ua->sockets, sock, read_cbk, write_cbk, 
                                          (socket_callback_t)except_cbk, name, user_data);
    if (index < 0) return NULL;
    
    ua->socket_alloc_count++;
    ua->poll_fds_dirty = 1;
    
#if HAS_EPOLL
    if (ua->use_epoll && ua->epoll_fd >= 0) {
        struct epoll_event ev;
        ev.events = 0;
        if (read_cbk) ev.events |= EPOLLIN;
        if (write_cbk) ev.events |= EPOLLOUT;
        if (except_cbk) ev.events |= EPOLLPRI;
        // On Windows, need to cast socket_t to int for epoll_ctl
#ifdef _WIN32
        int fd = (int)(intptr_t)sock;
#else
        int fd = sock;
#endif
        ev.data.u64 = ((uint64_t)ua->sockets->sockets[index].gen << 32) | (uint32_t)fd;
        if (epoll_ctl(ua->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "epoll add failed: fd=%d name=%s error=%s", fd, name ? name : "?", strerror(errno));
            socket_array_remove(ua->sockets, fd);
            ua->socket_alloc_count--;
            return NULL;
        }
    }
#endif
    
    DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "socket_add fd=%d name='%s' type=SOCK r=%p w=%p e=%p ud=%p",
                (int)sock, name ? name : "?", (void*)read_cbk, (void*)write_cbk, (void*)except_cbk, user_data);

    return (void*)(uintptr_t)(index + 1);   /* +1: index 0 ≠ NULL */
}

// Remove socket by socket_t
err_t uasync_remove_socket_t(struct UASYNC* ua, socket_t sock) {
    if (!ua || sock == SOCKET_INVALID) return ERR_FAIL;
    
    struct socket_node* node = socket_array_get_by_sock(ua->sockets, sock);
    if (!node || !node->active) return ERR_FAIL;
    const char* rname = node->name ? node->name : "?";
    
#if HAS_EPOLL
    if (ua->use_epoll && ua->epoll_fd >= 0) {
#ifdef _WIN32
        int fd = (int)(intptr_t)sock;
#else
        int fd = sock;
#endif
        epoll_ctl(ua->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    }
#endif
    
#ifdef _WIN32
    int fd = (int)(intptr_t)sock;
#else
    int fd = sock;
#endif
    int ret = socket_array_remove(ua->sockets, fd);
    if (ret == 0) {
        ua->socket_free_count++;
        ua->poll_fds_dirty = 1;
        DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "socket_remove fd=%d name='%s'", fd, rname);
        return ERR_OK;
    }
    return ERR_FAIL;
}

static err_t socket_set_monitoring(struct UASYNC* ua, void* s_id, int read_flag, int enable) {
    if (!ua || !s_id) return ERR_FAIL;
    uintptr_t handle = (uintptr_t)s_id;
    if (handle > (uintptr_t)ua->sockets->capacity) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "socket monitoring: invalid handle=%p", s_id);
        return ERR_FAIL;
    }
    struct socket_node* node = &ua->sockets->sockets[handle - 1];
    if (!node->active || node->fd < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "socket monitoring: inactive handle=%p", s_id);
        return ERR_FAIL;
    }
    int val = enable ? 1 : 0;
    int want_read = read_flag ? val : node->enable_read;
    int want_write = read_flag ? node->enable_write : val;
    if (want_read == node->enable_read && want_write == node->enable_write) return ERR_OK;
#if HAS_EPOLL
    if (ua->use_epoll && ua->epoll_fd >= 0) {
        struct epoll_event ev = {0};
        if ((node->read_cbk || node->read_cbk_sock) && want_read) ev.events |= EPOLLIN;
        if ((node->write_cbk || node->write_cbk_sock) && want_write) ev.events |= EPOLLOUT;
        if (node->except_cbk) ev.events |= EPOLLPRI;
        ev.data.u64 = ((uint64_t)node->gen << 32) | (uint32_t)node->fd;
        if (epoll_ctl(ua->epoll_fd, EPOLL_CTL_MOD, node->fd, &ev) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "epoll modify failed: fd=%d read=%d write=%d error=%s",
                        node->fd, want_read, want_write, strerror(errno));
            return ERR_FAIL;
        }
    }
#endif
    node->enable_read = want_read; node->enable_write = want_write;
    ua->poll_fds_dirty = 1;
    DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "socket monitoring: fd=%d name=%s read=%d write=%d",
                node->fd, node->name ? node->name : "?", want_read, want_write);
    return ERR_OK;
}

err_t uasync_set_socket_read(struct UASYNC* ua, void* s_id, int enable) {
    return socket_set_monitoring(ua, s_id, 1, enable);
}

err_t uasync_set_socket_write(struct UASYNC* ua, void* s_id, int enable) {
    return socket_set_monitoring(ua, s_id, 0, enable);
}

#ifndef _WIN32
// Заполняет уже зарезервированный массив, не выделяя память в event loop.
static int rebuild_poll_fds(struct UASYNC* ua) {
    int socket_count = ua->sockets->count;
    int wakeup_fd_present = ua->wakeup_initialized && ua->wakeup_pipe[0] != SOCKET_INVALID;
    int total_fds = socket_count + wakeup_fd_present;
    if (!ua->poll_fds || total_fds > ua->poll_fds_capacity) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "poll array invariant violated: fds=%p capacity=%d required=%d",
                    ua->poll_fds, ua->poll_fds_capacity, total_fds);
        return -1;
    }

    int idx = 0;
    
    // Add wakeup fd first if present
    if (wakeup_fd_present) {
        ua->poll_fds[idx].fd = ua->wakeup_pipe[0];
        ua->poll_fds[idx].events = POLLIN;
        ua->poll_fds[idx].revents = 0;
        idx++;
    }
    
    // Add socket fds using active_indices for O(1) traversal
    for (int i = 0; i < socket_count; i++) {
        int socket_array_idx = ua->sockets->active_indices[i];
        struct socket_node* cur = &ua->sockets->sockets[socket_array_idx];
        
        ua->poll_fds[idx].fd = cur->fd;
        ua->poll_fds[idx].events = 0;
        ua->poll_fds[idx].revents = 0;
        
        if (cur->type == SOCKET_NODE_TYPE_SOCK) {
            if (cur->read_cbk_sock && cur->enable_read) ua->poll_fds[idx].events |= POLLIN;
            if (cur->write_cbk_sock && cur->enable_write) ua->poll_fds[idx].events |= POLLOUT;
        } else {
            if (cur->read_cbk && cur->enable_read) ua->poll_fds[idx].events |= POLLIN;
            if (cur->write_cbk && cur->enable_write) ua->poll_fds[idx].events |= POLLOUT;
        }
        if (cur->except_cbk) ua->poll_fds[idx].events |= POLLPRI;
        
        idx++;
    }
    
    ua->poll_fds_count = total_fds;
    ua->poll_fds_dirty = 0;
    return 0;
}
#endif

// Process events from epoll (Linux only)
#if HAS_EPOLL
static void process_epoll_events(struct UASYNC* ua, struct epoll_event* events, int n_events) {
    for (int i = 0; i < n_events; i++) {
        if (events[i].data.fd < 0) {
            if (events[i].events & EPOLLIN) drain_wakeup_pipe(ua);
            process_posted_tasks(ua);
            continue;
        }
        int fd = (int)(events[i].data.u64 & 0xFFFFFFFF);
        uint32_t gen = (uint32_t)(events[i].data.u64 >> 32);
        uint32_t flags = events[i].events;
        struct socket_node* node = socket_array_get(ua->sockets, fd);
        if (!node || node->gen != gen) {
            DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "stale epoll event skipped: fd=%d generation=%u flags=0x%x", fd, gen, flags);
            continue;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "epoll event: fd=%d generation=%u flags=0x%x", fd, gen, flags);
        if ((flags & EPOLLIN) && node->enable_read) {
            if (node->type == SOCKET_NODE_TYPE_SOCK) {
                if (node->read_cbk_sock) node->read_cbk_sock(node->sock, node->user_data);
            } else if (node->read_cbk) node->read_cbk(node->fd, node->user_data);
        }
        /* Callback может удалить/заменить сокет, отключить write или переместить массив. */
        node = socket_array_get(ua->sockets, fd);
        if (!node || node->gen != gen) continue;
        if ((flags & EPOLLOUT) && node->enable_write) {
            if (node->type == SOCKET_NODE_TYPE_SOCK) {
                if (node->write_cbk_sock) node->write_cbk_sock(node->sock, node->user_data);
            } else if (node->write_cbk) node->write_cbk(node->fd, node->user_data);
        }
        node = socket_array_get(ua->sockets, fd);
        if (!node || node->gen != gen) continue;
        if (flags & (EPOLLERR | EPOLLHUP | EPOLLPRI)) {
            if (node->except_cbk) node->except_cbk(node->fd, node->user_data);
            else if (flags & (EPOLLERR | EPOLLHUP)) {
                DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "socket HUP/ERR without except handler: fd=%d name=%s flags=0x%x — unregister",
                           fd, node->name, flags);
                socket_force_unregister(ua, fd);
            }
        }
    }
}
#endif

// CPU-часы читаются только в потоке владельца, включая системное время этого потока.
static int uasync_thread_cpu_ns(uint64_t* value) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync CPU clock failed: error=%lu", (unsigned long)GetLastError());
        return -1;
    }
    uint64_t kernel_ticks = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    uint64_t user_ticks = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    *value = (kernel_ticks + user_ticks) * 100;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync CPU clock failed: %s", strerror(errno));
        return -1;
    }
    *value = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
    return 0;
}

#define UASYNC_LOAD_PERIOD_US 100000ULL
#define UASYNC_LOAD_WINDOW_US 1000000ULL

static void uasync_update_load(struct UASYNC* ua) {
    if (ua->load_state < 0) return;
    uint64_t now = get_time_us();
    if (ua->load_state && now < ua->load_next_us) return;
    uint64_t cpu_ns;
    if (uasync_thread_cpu_ns(&cpu_ns) < 0) {
        ua->load_state = -1; ua->load.valid = 0;
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "uasync CPU load disabled: ua=%p", (void*)ua);
        return;
    }
    if (!ua->load_state) {
        ua->load_state = 1; ua->load_previous_us = now; ua->load_previous_cpu_ns = cpu_ns;
        ua->load_next_us = now + UASYNC_LOAD_PERIOD_US; ua->load_log_us = now + UASYNC_LOAD_WINDOW_US;
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "uasync CPU load started: ua=%p period=100ms window=1000ms", (void*)ua);
        return;
    }
    if (cpu_ns < ua->load_previous_cpu_ns || now <= ua->load_previous_us) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync CPU load clock regression: ua=%p wall=%llu->%llu cpu=%llu->%llu",
                    (void*)ua, (unsigned long long)ua->load_previous_us, (unsigned long long)now,
                    (unsigned long long)ua->load_previous_cpu_ns, (unsigned long long)cpu_ns);
        ua->load_state = -1; ua->load.valid = 0;
        return;
    }
    uint64_t elapsed = now - ua->load_previous_us, delta_cpu = cpu_ns - ua->load_previous_cpu_ns;
    uint64_t late = now - ua->load_next_us;
    if (late >= UASYNC_LOAD_PERIOD_US && now - ua->last_poll_exit_us >= UASYNC_LOAD_PERIOD_US) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "uasync CPU sample delayed: ua=%p interval=%lluus late=%lluus missed=%llu",
                   (void*)ua, (unsigned long long)elapsed, (unsigned long long)late,
                   (unsigned long long)(late / UASYNC_LOAD_PERIOD_US));
    }
    unsigned int capacity = sizeof(ua->load_history) / sizeof(ua->load_history[0]);
    ua->load_history[ua->load_head] = (struct uasync_load_interval){ua->load_previous_us, now, delta_cpu};
    ua->load_head = (ua->load_head + 1) % capacity;
    if (ua->load_count < capacity) ua->load_count++;
    uint64_t cutoff = now > UASYNC_LOAD_WINDOW_US ? now - UASYNC_LOAD_WINDOW_US : 0;
    uint64_t window_us = 0;
    double window_cpu_ns = 0;
    for (unsigned int i = 0; i < ua->load_count; i++) {
        const struct uasync_load_interval* sample = &ua->load_history[i];
        if (sample->end_us <= cutoff) continue;
        uint64_t start = sample->start_us > cutoff ? sample->start_us : cutoff;
        uint64_t overlap = sample->end_us - start;
        window_us += overlap;
        window_cpu_ns += (double)sample->cpu_ns * overlap / (sample->end_us - sample->start_us);
    }
    ua->load = (struct uasync_load){
        .current_percent = (double)delta_cpu / elapsed / 10.0,
        .average_percent = window_cpu_ns / window_us / 10.0,
        .sample_time_us = now, .interval_us = elapsed, .window_us = window_us, .valid = 1
    };
    // Разница точности CPU/реальных часов может дать небольшое превышение 100%.
    if (ua->load.current_percent > 100.0) ua->load.current_percent = 100.0;
    if (ua->load.average_percent > 100.0) ua->load.average_percent = 100.0;
    ua->load_previous_us = now; ua->load_previous_cpu_ns = cpu_ns;
    ua->load_next_us += (late / UASYNC_LOAD_PERIOD_US + 1) * UASYNC_LOAD_PERIOD_US;
    if (now >= ua->load_log_us) {
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "uasync CPU load: ua=%p current=%.2f%% average=%.2f%% interval=%lluus window=%lluus late=%lluus",
                    (void*)ua, ua->load.current_percent, ua->load.average_percent, (unsigned long long)elapsed,
                    (unsigned long long)window_us, (unsigned long long)late);
        ua->load_log_us = now + UASYNC_LOAD_WINDOW_US;
    }
}

// Секунда непрерывного ожидания полностью вытесняет прежнюю нагрузку из окна.
static void uasync_load_after_wait(struct UASYNC* ua, uint64_t wait_start_us) {
    uint64_t now = get_time_us();
    ua->last_poll_exit_us = now;
    if (ua->load_state <= 0 || now - wait_start_us < UASYNC_LOAD_WINDOW_US) return;
    uint64_t cpu_ns;
    if (uasync_thread_cpu_ns(&cpu_ns) < 0) { ua->load_state = -1; ua->load.valid = 0; return; }
    ua->load_history[0] = (struct uasync_load_interval){now - UASYNC_LOAD_WINDOW_US, now, 0};
    ua->load_head = 1; ua->load_count = 1;
    ua->load_previous_us = now; ua->load_previous_cpu_ns = cpu_ns;
    ua->load_next_us = now + UASYNC_LOAD_PERIOD_US;
    ua->load = (struct uasync_load){.sample_time_us = now, .interval_us = now - wait_start_us,
                                   .window_us = UASYNC_LOAD_WINDOW_US, .valid = 1};
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "uasync CPU load: ua=%p current=0%% average=0%% idle=%lluus",
                (void*)ua, (unsigned long long)(now - wait_start_us));
    ua->load_log_us = now + UASYNC_LOAD_WINDOW_US;
}

err_t uasync_get_load(struct UASYNC* ua, struct uasync_load* load) {
    if (!ua || !load) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync_get_load: invalid argument ua=%p load=%p", (void*)ua, (void*)load);
        return ERR_FAIL;
    }
    uasync_update_load(ua);
    *load = ua->load;
    return ERR_OK;
}

static void uasync_poll_once(struct UASYNC* ua, int timeout_tb) {
    process_immediate_queue(ua);

    uint64_t now_us_poll_entry = get_time_us();
    if (ua->last_poll_exit_us && now_us_poll_entry - ua->last_poll_exit_us > 10000) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "Event loop stall: %lluus since end of last poll processing",
                   (unsigned long long)(now_us_poll_entry - ua->last_poll_exit_us));
    }

    // После stop ещё разрешён неблокирующий проход для teardown и удаления отменённых таймеров.
    if (__atomic_load_n(&ua->stop, __ATOMIC_ACQUIRE)) timeout_tb = 0;
    int timeout_ms = next_timeout_ms(ua);
    if (timeout_tb >= 0) {
        int requested_ms = timeout_tb / 10 + (timeout_tb % 10 != 0);
        if (timeout_ms < 0 || requested_ms < timeout_ms) timeout_ms = requested_ms;
    }
    if (ua->immediate_queue_head) timeout_ms = 0;
    int socket_count = ua->sockets->count;

    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "poll(%d sockets, %zu timers, timeout=%dms)",
                socket_count, ua->timeout_heap->size, timeout_ms);
    
#if HAS_EPOLL
    // Use epoll on Linux if available
    if (ua->use_epoll && ua->epoll_fd >= 0) {
        struct epoll_event events[64];  // Stack-allocated array for events
        int max_events = 64;
        
        uint64_t wait_start_us = get_time_us();
        int ret = epoll_wait(ua->epoll_fd, events, max_events, timeout_ms);
        int saved_errno = errno;
        uasync_load_after_wait(ua, wait_start_us);
        if (ret < 0) {
            if (saved_errno == EINTR) {
                DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "epoll_wait→EINTR");
                return;
            }
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "epoll_wait→error errno=%d", saved_errno);
            return;
        }
        
        /* Process socket events */
        if (ret > 0) {
            process_epoll_events(ua, events, ret);
        }
        
        /* Process timeouts that may have expired during poll or socket processing */
        process_timeouts(ua);
        return;
    }
#endif
    
    // Снимок поколений защищает также следующие сокеты в общей порции событий.
    for (int i = 0; i < socket_count; i++) {
        struct socket_node* node = &ua->sockets->sockets[ua->sockets->active_indices[i]];
        node->poll_gen = node->gen;
    }

    // Fallback to poll() for non-Linux or if epoll failed
    
    // Include wakeup pipe if initialized
    int wakeup_fd_present = ua->wakeup_initialized && ua->wakeup_pipe[0] != SOCKET_INVALID;
    int total_fds = socket_count + wakeup_fd_present;

    // If no sockets to poll, just wait and process timeouts
    if (total_fds == 0) {
        uint64_t wait_start_us = get_time_us();
        if (timeout_ms > 0) {
#ifdef _WIN32
            Sleep(timeout_ms);
#else
            struct timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000 };
            nanosleep(&ts, NULL);
#endif
        }
        uasync_load_after_wait(ua, wait_start_us);
        process_timeouts(ua);
        return;
    }

#ifdef _WIN32
    // On Windows, use select() instead of WSAPoll to avoid issues with accepted sockets
    fd_set read_fds, write_fds, except_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);
    
    SOCKET max_fd = 0;
    
    // Add all active sockets to fd_sets
    for (int i = 0; i < ua->sockets->count; i++) {
        int idx = ua->sockets->active_indices[i];
        struct socket_node* node = &ua->sockets->sockets[idx];
        if (!node->active) continue;
        
        SOCKET s;
        if (node->type == SOCKET_NODE_TYPE_SOCK) {
            s = node->sock;
        } else {
            s = (SOCKET)node->fd;
        }
        
        if (node->type == SOCKET_NODE_TYPE_SOCK) {
            if (node->read_cbk_sock && node->enable_read) FD_SET(s, &read_fds);
            if (node->write_cbk_sock && node->enable_write) FD_SET(s, &write_fds);
        } else {
            if (node->read_cbk && node->enable_read) FD_SET(s, &read_fds);
            if (node->write_cbk && node->enable_write) FD_SET(s, &write_fds);
        }
        if (node->except_cbk) FD_SET(s, &except_fds);
        
        if (s > max_fd) max_fd = s;
    }
    
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    uint64_t wait_start_us = get_time_us();
    int ret = select((int)max_fd + 1, &read_fds, &write_fds, &except_fds, timeout_ms < 0 ? NULL : &tv);
    int saved_error = ret < 0 ? WSAGetLastError() : 0;
    uasync_load_after_wait(ua, wait_start_us);
    
    if (ret < 0) {
        int err = saved_error;
        if (err != WSAEINTR) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "select failed: %d", err);
        }
        return;
    }
    
    if (ret > 0) {
        for (int i = 0; i < ua->sockets->count; i++) {
            int idx = ua->sockets->active_indices[i];
            struct socket_node* node = &ua->sockets->sockets[idx];
            if (!node->active) continue;

            SOCKET s;
            if (node->type == SOCKET_NODE_TYPE_SOCK) {
                s = node->sock;
            } else {
                s = (SOCKET)node->fd;
            }

            int has_read = FD_ISSET(s, &read_fds);
            int has_write = FD_ISSET(s, &write_fds);
            int has_except = FD_ISSET(s, &except_fds);

            if (!has_read && !has_write && !has_except) continue;
            DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "select→fd=%d r=%d w=%d e=%d", (int)s, has_read, has_write, has_except);

            /* Коллбэки могут удалить/добавить сокет (realloc массива) — перед
             * каждым последующим коллбэком пере-валидируем node по индексу. */
            if (node->gen != node->poll_gen) continue;
            uint32_t generation = node->gen;
            int cb_called = 0;

            if (has_except) {
                if (node->except_cbk) {
                    node->except_cbk(node->fd, node->user_data);
                }
                cb_called++;
            }

            if (has_read) {
                if (cb_called) {
                    node = &ua->sockets->sockets[idx];
                    if (!node->active || node->gen != generation) continue;
                }
                if (node->type == SOCKET_NODE_TYPE_SOCK) {
                    if (node->read_cbk_sock && node->enable_read) {
                        node->read_cbk_sock(node->sock, node->user_data);
                    }
                } else {
                    if (node->read_cbk && node->enable_read) {
                        node->read_cbk(node->fd, node->user_data);
                    }
                }
                cb_called++;
            }

            if (has_write) {
                if (cb_called) {
                    node = &ua->sockets->sockets[idx];
                    if (!node->active || node->gen != generation) continue;
                }
                if (node->type == SOCKET_NODE_TYPE_SOCK) {
                    if (node->write_cbk_sock && node->enable_write) {
                        node->write_cbk_sock(node->sock, node->user_data);
                    }
                } else {
                    if (node->write_cbk && node->enable_write) {
                        node->write_cbk(node->fd, node->user_data);
                    }
                }
                cb_called++;
            }
        }
    }
#else
    // On non-Windows, use poll()
    
    if (ua->poll_fds_dirty || ua->poll_fds_count != total_fds || !ua->poll_fds) {
        if (rebuild_poll_fds(ua) < 0) {
            // Нарушение внутреннего инварианта не должно оставлять поток без wakeup.
            process_posted_tasks(ua);
            process_timeouts(ua);
            return;
        }
    }

    uint64_t wait_start_us = get_time_us();
    int ret = poll(ua->poll_fds, ua->poll_fds_count, timeout_ms);
    int saved_errno = errno;
    uasync_load_after_wait(ua, wait_start_us);
    if (ret < 0) {
        if (saved_errno == EINTR) {
            return;
        }
        perror("poll");
        return;
    }
    
    /* Process socket events first to give sockets higher priority */
    if (ret > 0) {
        for (int i = 0; i < ua->poll_fds_count; i++) {
            if (ua->poll_fds[i].revents == 0) continue;
            DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "poll→fd=%d rev=0x%x", ua->poll_fds[i].fd, ua->poll_fds[i].revents);
            
            /* Handle wakeup fd separately */
            if (wakeup_fd_present && i == 0) {
                if (ua->poll_fds[i].revents & POLLIN) {
                    drain_wakeup_pipe(ua);
                }
                process_posted_tasks(ua);
                continue;
            }
            
            /* Socket event - lookup by fd */
            int fd = ua->poll_fds[i].fd;
            struct socket_node* node = socket_array_get(ua->sockets, fd);
            if (!node) {  // Try by socket_t (in case this is a socket)
                node = socket_array_get_by_sock(ua->sockets, fd);
            }
            if (!node || node->gen != node->poll_gen) continue;  // Регистрация изменилась после poll.

            /* Коллбэки могут удалить/добавить сокет (realloc массива) — перед
             * каждым последующим коллбэком пере-валидируем node по fd. */
            uint32_t generation = node->gen;
            int cb_called = 0;

            /* Read readiness BEFORE error — avoid losing data on combined IN+ERR/HUP events */
            if (ua->poll_fds[i].revents & POLLIN) {
                if (node->type == SOCKET_NODE_TYPE_SOCK) {
                    if (node->read_cbk_sock && node->enable_read) {
                        node->read_cbk_sock(node->sock, node->user_data);
                    }
                } else {
                    if (node->read_cbk && node->enable_read) {
                        node->read_cbk(node->fd, node->user_data);
                    }
                }
                cb_called++;
            }

            /* Write readiness BEFORE error — flush pending writes before handling HUP */
            if (ua->poll_fds[i].revents & POLLOUT) {
                if (cb_called) {
                    node = socket_array_get(ua->sockets, fd);
                    if (!node) node = socket_array_get_by_sock(ua->sockets, fd);
                    if (!node || node->gen != generation) continue;
                }
                if (node->type == SOCKET_NODE_TYPE_SOCK) {
                    if (node->write_cbk_sock && node->enable_write) {
                        node->write_cbk_sock(node->sock, node->user_data);
                    }
                } else {
                    if (node->write_cbk && node->enable_write) {
                        node->write_cbk(node->fd, node->user_data);
                    }
                }
                cb_called++;
            }

            /* Check for error conditions LAST — I/O handlers drain/process data first */
            if (ua->poll_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                if (cb_called) {
                    node = socket_array_get(ua->sockets, fd);
                    if (!node) node = socket_array_get_by_sock(ua->sockets, fd);
                    if (!node || node->gen != generation) continue;
                }
                /* Treat as exceptional condition */
                if (node->except_cbk) {
                    node->except_cbk(node->fd, node->user_data);
                } else {
                    DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "socket HUP/ERR/NVAL without except handler: fd=%d rev=0x%x name='%s' type=%s r=%p w=%p — unregister",
                               fd, ua->poll_fds[i].revents, node->name ? node->name : "?",
                               node->type == SOCKET_NODE_TYPE_SOCK ? "SOCK" : "FD",
                               (void*)(node->type == SOCKET_NODE_TYPE_SOCK ? (void*)node->read_cbk_sock : (void*)node->read_cbk),
                               (void*)(node->type == SOCKET_NODE_TYPE_SOCK ? (void*)node->write_cbk_sock : (void*)node->write_cbk));
                    socket_force_unregister(ua, fd);
                }
                cb_called++;
            }

            /* Exceptional data (out-of-band) */
            if (ua->poll_fds[i].revents & POLLPRI) {
                if (cb_called) {
                    node = socket_array_get(ua->sockets, fd);
                    if (!node) node = socket_array_get_by_sock(ua->sockets, fd);
                    if (!node || node->gen != generation) continue;
                }
                if (node->except_cbk) {
                    node->except_cbk(node->fd, node->user_data);
                }
                cb_called++;
            }
        }
    }
#endif
    
    /* Process posted tasks and timeouts that may have expired during poll */
    process_posted_tasks(ua);
    process_timeouts(ua);
}


void uasync_poll(struct UASYNC* ua, int timeout_tb) {
    if (!ua || !ua->sockets || !ua->timeout_heap) return;
    uasync_update_load(ua);
    uasync_poll_once(ua, timeout_tb);
    uasync_update_load(ua);
}


#ifdef _WIN32
static void wakeup_read_callback_win(socket_t sock, void* arg) {
    (void)sock;                     // не нужен
    handle_wakeup((struct UASYNC*)arg);
}

#endif


// ========== Instance management functions ==========

// Modified function in u_async.c: uasync_create
// Changes: Use self-connected UDP socket for wakeup on Windows instead of pipe.
// This ensures the wakeup is a selectable SOCKET.

struct UASYNC* uasync_create(void) {
    if (socket_platform_init() != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync socket platform initialization failed");
        return NULL;
    }
    struct UASYNC* ua = u_calloc(1, sizeof(*ua));
    if (!ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync instance allocation failed");
        socket_platform_cleanup(); return NULL;
    }
    ua->epoll_fd = -1; ua->wakeup_pipe[0] = ua->wakeup_pipe[1] = -1; ua->poll_fds_dirty = 1;
    ua->sockets = socket_array_create(16);
    if (!ua->sockets) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync socket array allocation failed"); goto fail; }
    ua->timeout_heap = timeout_heap_create(16);
    if (!ua->timeout_heap) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync timeout heap allocation failed"); goto fail; }
    ua->timeout_pool = memory_pool_init(sizeof(struct timeout_node), "timeout_pool");
    if (!ua->timeout_pool) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync timeout pool allocation failed"); goto fail; }
    timeout_heap_set_free_callback(ua->timeout_heap, ua, timeout_node_free_callback);
#if HAS_EPOLL
    ua->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ua->epoll_fd >= 0) {
        ua->use_epoll = 1;
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "Using epoll for socket monitoring");
    } else DEBUG_WARN(DEBUG_CATEGORY_SYS, "Failed to create epoll, falling back to poll: %s", strerror(errno));
#endif
#ifdef _WIN32
    SOCKET r = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET w = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ua->wakeup_pipe[0] = r; ua->wakeup_pipe[1] = w;
    if (r == INVALID_SOCKET || w == INVALID_SOCKET) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to create wakeup sockets: %d", WSAGetLastError()); goto fail;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int addr_len = sizeof(addr);
    if (bind(r, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        getsockname(r, (struct sockaddr*)&addr, &addr_len) == SOCKET_ERROR ||
        connect(w, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Wakeup socket setup failed: %d", WSAGetLastError()); goto fail;
    }
    u_long mode = 1;
    if (ioctlsocket(r, FIONBIO, &mode) || ioctlsocket(w, FIONBIO, &mode)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Wakeup socket nonblocking setup failed: %d", WSAGetLastError()); goto fail;
    }
    if (!uasync_add_socket_t(ua, r, wakeup_read_callback_win, NULL, NULL, "wakeup", ua)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Wakeup socket registration failed"); goto fail;
    }
    InitializeCriticalSection(&ua->posted_lock);
#else
#ifdef __linux__
    ua->wakeup_pipe[0] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ua->wakeup_pipe[0] < 0) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "eventfd failed: %s", strerror(errno)); goto fail; }
    ua->wakeup_pipe[1] = ua->wakeup_pipe[0];
#else
    if (pipe(ua->wakeup_pipe) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "wakeup pipe failed: %s", strerror(errno)); goto fail; }
    if (fcntl(ua->wakeup_pipe[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(ua->wakeup_pipe[1], F_SETFL, O_NONBLOCK) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "wakeup pipe nonblocking setup failed: %s", strerror(errno)); goto fail;
    }
#endif
#if HAS_EPOLL
    if (ua->use_epoll) {
        struct epoll_event ev = {0}; ev.events = EPOLLIN; ev.data.fd = -1;
        if (epoll_ctl(ua->epoll_fd, EPOLL_CTL_ADD, ua->wakeup_pipe[0], &ev) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Wakeup epoll registration failed: %s", strerror(errno)); goto fail;
        }
    }
#endif
    if (reserve_poll_fds(ua, 1) < 0) goto fail;
    int lock_error = pthread_mutex_init(&ua->posted_lock, NULL);
    if (lock_error) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync mutex initialization failed: %s", strerror(lock_error)); goto fail; }
#endif
    ua->wakeup_initialized = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "uasync created: ua=%p epoll=%d wakeup=%llu", ua, ua->use_epoll,
                (unsigned long long)ua->wakeup_pipe[0]);
    return ua;
fail:
    u_free(ua->poll_fds);
    timeout_heap_destroy(ua->timeout_heap); memory_pool_destroy(ua->timeout_pool); socket_array_destroy(ua->sockets);
    if (ua->epoll_fd >= 0) close(ua->epoll_fd);
    if (ua->wakeup_pipe[0] != SOCKET_INVALID) {
#ifdef _WIN32
        closesocket((SOCKET)(intptr_t)ua->wakeup_pipe[0]);
#else
        close(ua->wakeup_pipe[0]);
#endif
    }
    if (ua->wakeup_pipe[1] != SOCKET_INVALID && ua->wakeup_pipe[1] != ua->wakeup_pipe[0]) {
#ifdef _WIN32
        closesocket((SOCKET)(intptr_t)ua->wakeup_pipe[1]);
#else
        close(ua->wakeup_pipe[1]);
#endif
    }
    u_free(ua); socket_platform_cleanup(); return NULL;
}

// Print all resources for debugging
void uasync_print_resources(struct UASYNC* ua, const char* prefix) {
    if (!ua) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "%s: NULL uasync instance", prefix);
        return;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "%s: UASYNC Resource Report for %p", prefix, ua);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "   Timer Statistics: allocated=%zu, u_freed=%zu, active=%zd",
           ua->timer_alloc_count, ua->timer_free_count,
           (ssize_t)(ua->timer_alloc_count - ua->timer_free_count));
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "   Socket Statistics: allocated=%zu, u_freed=%zu, active=%zd",
           ua->socket_alloc_count, ua->socket_free_count,
           (ssize_t)(ua->socket_alloc_count - ua->socket_free_count));
    
    // Показать активные таймеры
    if (ua->timeout_heap) {
        uint64_t now_ms = get_time_us() / 1000;
        size_t active = 0, reserved = 0, deleted = 0;
        for (size_t i = 0; i < ua->timeout_heap->size; i++) {
            const TimeoutEntry* entry = &ua->timeout_heap->heap[i];
            struct timeout_node* node = entry->data;
            if (entry->deleted) {
                deleted++;
                DEBUG_INFO(DEBUG_CATEGORY_SYS, "   Timer DELETED: node=%p name='%s'", (void*)node, node->name);
            } else if (entry->expiration == UINT64_MAX) {
                reserved++;
                DEBUG_INFO(DEBUG_CATEGORY_SYS, "   Timer RESERVED: node=%p name='%s'", (void*)node, node->name);
            } else {
                active++;
                DEBUG_INFO(DEBUG_CATEGORY_SYS, "   Timer ACTIVE: node=%p name='%s' exp=%llu remain=%lldms",
                           (void*)node, node->name, (unsigned long long)entry->expiration, (long long)entry->expiration - (long long)now_ms);
            }
        }
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "   Timers in heap: active=%zu reserved=%zu deleted=%zu total=%zu",
                   active, reserved, deleted, ua->timeout_heap->size);
    }
    
    // Показать активные сокеты
    if (ua->sockets) {
        int active_sockets = 0;
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "   Socket array capacity: %d, active: %d",
               ua->sockets->capacity, ua->sockets->count);
        for (int i = 0; i < ua->sockets->capacity; i++) {
            if (ua->sockets->sockets[i].active) {
                struct socket_node* sn = &ua->sockets->sockets[i];
                active_sockets++;
                DEBUG_INFO(DEBUG_CATEGORY_SYS, "   Socket: fd=%d name='%s' type=%s r=%p w=%p e=%p ud=%p",
                       sn->fd, sn->name ? sn->name : "?",
                       sn->type == SOCKET_NODE_TYPE_SOCK ? "SOCK" : "FD",
                       (void*)(sn->type == SOCKET_NODE_TYPE_SOCK ? (void*)sn->read_cbk_sock : (void*)sn->read_cbk),
                       (void*)(sn->type == SOCKET_NODE_TYPE_SOCK ? (void*)sn->write_cbk_sock : (void*)sn->write_cbk),
                       (void*)sn->except_cbk, sn->user_data);
            }
        }
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "   Total active sockets: %d", active_sockets);
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "%s: End of resource report", prefix);
}

// Modified function in u_async.c: uasync_destroy
// Changes: Close wakeup sockets properly on Windows.

void uasync_mark_running(struct UASYNC* ua) {
    if (!ua) return;
    __atomic_store_n(&ua->running, 1, __ATOMIC_RELEASE);
}

void uasync_mark_stopped(struct UASYNC* ua) {
    if (!ua) return;
    __atomic_store_n(&ua->running, 0, __ATOMIC_RELEASE);
}

void uasync_destroy(struct UASYNC* ua, int close_fds) {
    if (!ua) return;
    ua->running = 0;

    DEBUG_DEBUG(DEBUG_CATEGORY_TIMERS, "uasync_destroy: starting cleanup for ua=%p", ua);

    // Диагностика ресурсов перед очисткой
    uasync_print_resources(ua, "BEFORE_DESTROY");

    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "Cleanup pending resources: timers=%zu sockets=%zu",
                ua->timer_alloc_count - ua->timer_free_count, ua->socket_alloc_count - ua->socket_free_count);

    // Free all remaining timeouts
    
    // Очистить immediate_queue
    while (ua->immediate_queue_head) {
        struct uasync_work* work = ua->immediate_queue_head;
        uasync_cancel_work(work);
        if (work->callback == run_soon_once) uasync_release_soon(work->arg);
    }

    // Очистить heap
    if (ua->timeout_heap) {
        while (1) {
            TimeoutEntry entry;
            if (timeout_heap_pop(ua->timeout_heap, &entry) != 0) break;
            struct timeout_node* node = (struct timeout_node*)entry.data;

            // Free all timer nodes (avoid double-u_free bug)
            if (node) {
                ua->timer_free_count++;
                memory_pool_free(ua->timeout_pool, node);
            }
        }
        timeout_heap_destroy(ua->timeout_heap);
        ua->timeout_heap = NULL;
    }

    // Destroy timeout pool
    if (ua->timeout_pool) {
        memory_pool_destroy(ua->timeout_pool);
        ua->timeout_pool = NULL;
    }

    // Free all socket nodes using array approach
    if (ua->sockets) {
        // Count and u_free all active sockets
        int u_freed_count = 0;
        for (int i = 0; i < ua->sockets->capacity; i++) {
            if (ua->sockets->sockets[i].active) {
                if (close_fds && ua->sockets->sockets[i].fd >= 0) {
#ifdef _WIN32
                    if (ua->sockets->sockets[i].type == SOCKET_NODE_TYPE_SOCK) {
                        closesocket(ua->sockets->sockets[i].sock);
                    } else {
                        close(ua->sockets->sockets[i].fd);  // For pipes/FDs
                    }
#else
                    close(ua->sockets->sockets[i].fd);
#endif
                }
                ua->socket_free_count++;
                u_freed_count++;
            }
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "Freed %d socket nodes in destroy", u_freed_count);
        socket_array_destroy(ua->sockets);
    }

    // Close wakeup pipe/sockets
    if (ua->wakeup_initialized) {
#ifdef _WIN32
        closesocket((SOCKET)(intptr_t)ua->wakeup_pipe[0]);
        if (ua->wakeup_pipe[1] != ua->wakeup_pipe[0])
            closesocket((SOCKET)(intptr_t)ua->wakeup_pipe[1]);
#else
        close(ua->wakeup_pipe[0]);
        // eventfd на Linux: wakeup_pipe[0] == wakeup_pipe[1] — избегаем двойного close
        if (ua->wakeup_pipe[1] != ua->wakeup_pipe[0])
            close(ua->wakeup_pipe[1]);
#endif
    }

    // Free cached poll_fds
    u_free(ua->poll_fds);

    // Close epoll fd on Linux
#if HAS_EPOLL
    if (ua->epoll_fd >= 0) {
        close(ua->epoll_fd);
    }
#endif

    // Final leak check
    if (ua->timer_alloc_count != ua->timer_free_count || ua->socket_alloc_count != ua->socket_free_count) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Memory leaks detected after cleanup: timers %zu/%zu, sockets %zu/%zu",
                   ua->timer_alloc_count, ua->timer_free_count, ua->socket_alloc_count, ua->socket_free_count);
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "FINAL Timer leak: allocated=%zu, u_freed=%zu, diff=%zd",
                   ua->timer_alloc_count, ua->timer_free_count,
                   (ssize_t)(ua->timer_alloc_count - ua->timer_free_count));
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "FINAL Socket leak: allocated=%zu, u_freed=%zu, diff=%zd",
                   ua->socket_alloc_count, ua->socket_free_count,
                   (ssize_t)(ua->socket_alloc_count - ua->socket_free_count));
        abort();
    }

    DEBUG_DEBUG(DEBUG_CATEGORY_TIMERS, "uasync_destroy: completed successfully for ua=%p", ua);

    while (ua->posted_tasks_head) {
        struct posted_task* t = ua->posted_tasks_head;
        ua->posted_tasks_head = t->next;
        u_free(t);
    }
#ifdef _WIN32
    DeleteCriticalSection(&ua->posted_lock);
#else
    pthread_mutex_destroy(&ua->posted_lock);
#endif

    u_free(ua);

    // Cleanup socket platform (WSACleanup on Windows)
    socket_platform_cleanup();
}

// Debug statistics
void uasync_get_stats(struct UASYNC* ua, size_t* timer_alloc, size_t* timer_u_free, size_t* socket_alloc, size_t* socket_u_free) {
    if (!ua) return;
    if (timer_alloc) *timer_alloc = ua->timer_alloc_count;
    if (timer_u_free) *timer_u_free = ua->timer_free_count;
    if (socket_alloc) *socket_alloc = ua->socket_alloc_count;
    if (socket_u_free) *socket_u_free = ua->socket_free_count;
}

void uasync_memsync(struct UASYNC* ua) {
#ifdef _WIN32
    EnterCriticalSection(&ua->posted_lock);
#else
    pthread_mutex_lock(&ua->posted_lock);
#endif
#ifdef _WIN32
    LeaveCriticalSection(&ua->posted_lock);
#else
    pthread_mutex_unlock(&ua->posted_lock);
#endif
}

void uasync_post(struct UASYNC* ua, uasync_post_callback_t callback, void* arg) {
    if (!ua || !callback) return;

    struct posted_task* task = u_malloc(sizeof(struct posted_task));
    if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync_post: task allocation failed"); return; }

    task->callback = callback;
    task->arg = arg;
    uasync_post_reserved(ua, task);
}

/* Worker резервирует completion до запуска, поэтому завершение не требует памяти. */
void uasync_post_reserved(struct UASYNC* ua, struct posted_task* task) {
    task->next = NULL;

#ifdef _WIN32
    EnterCriticalSection(&ua->posted_lock);
#else
    pthread_mutex_lock(&ua->posted_lock);
#endif

    int wake = !ua->posted_tasks_head;
    if (ua->posted_tasks_tail) {
        // есть конец списка — добавляем туда
        ua->posted_tasks_tail->next = task;
        ua->posted_tasks_tail = task;
    } else {
        // список пустой — это первая задача
        ua->posted_tasks_head = ua->posted_tasks_tail = task;
    }

#ifdef _WIN32
    LeaveCriticalSection(&ua->posted_lock);
#else
    pthread_mutex_unlock(&ua->posted_lock);
#endif

    // Одного пробуждения достаточно на всю непустую очередь; снимок снимается под тем же lock.
    if (wake) uasync_wakeup(ua);
}

/* В потоке loop удаляем ожидающую задачу из общей очереди или ещё не исполненной части любой пачки. */
err_t uasync_cancel_post(struct UASYNC* ua, struct posted_task* task) {
#ifdef _WIN32
    EnterCriticalSection(&ua->posted_lock);
#else
    pthread_mutex_lock(&ua->posted_lock);
#endif
    struct posted_task** p = &ua->posted_tasks_head;
    struct posted_task* previous = NULL;
    while (*p && *p != task) { previous = *p; p = &(*p)->next; }
    int found = *p != NULL;
    if (found) {
        *p = task->next;
        if (ua->posted_tasks_tail == task) ua->posted_tasks_tail = previous;
    }
#ifdef _WIN32
    LeaveCriticalSection(&ua->posted_lock);
#else
    pthread_mutex_unlock(&ua->posted_lock);
#endif
    const char* source = "queue";
    for (struct uasync_post_batch* batch = ua->posted_batches; !found && batch; batch = batch->previous) {
        p = &batch->head;
        while (*p && *p != task) p = &(*p)->next;
        if (*p) {
            *p = task->next;
            found = 1;
            source = "batch";
        }
    }
    if (found) {
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "uasync_cancel_post: cancelled task=%p source=%s", (void*)task, source);
        u_free(task);
    } else DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "uasync_cancel_post: task=%p running or absent", (void*)task);
    return found ? ERR_OK : ERR_FAIL;
}

// Wakeup mechanism
int uasync_wakeup(struct UASYNC* ua) {
    if (!ua || !ua->wakeup_initialized) return -1;

#ifdef _WIN32
    char byte = 0;
    int ret = send((SOCKET)(intptr_t)ua->wakeup_pipe[1], &byte, 1, 0);
    if (ret != 1) {
        return -1;
    }
#else
    uint64_t val = 1;
    ssize_t ret = write(ua->wakeup_pipe[1], &val, sizeof(val));
    if (ret != sizeof(val)) {
        return -1;
    }
#endif
    return 0;
}

socket_t uasync_get_wakeup_fd(struct UASYNC* ua) {
    if (!ua || !ua->wakeup_initialized) return SOCKET_INVALID;
    return ua->wakeup_pipe[1];
}

/* Поиск кодированного handle по fd, устойчивого к расширению массива. */
int uasync_lookup_socket(struct UASYNC* ua, int fd, void** socket_id) {
    if (!ua || !ua->sockets || !socket_id || fd < 0 || fd >= ua->sockets->capacity) {
        return -1;
    }
    
    struct socket_node* node = socket_array_get(ua->sockets, fd);
    *socket_id = node ? (void*)(uintptr_t)(ua->sockets->fd_to_index[fd] + 1) : NULL;
    return node ? 0 : -1;
}

void uasync_stop(struct UASYNC* ua) {
    if (!ua) return;
    __atomic_store_n(&ua->stop, 1, __ATOMIC_RELEASE);
    if (uasync_wakeup(ua) < 0 && errno != EAGAIN)
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "uasync stop wakeup failed: ua=%p", ua);
}

void uasync_mainloop(struct UASYNC* ua) {
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "uasync_mainloop: null instance"); return; }
    while (!__atomic_load_n(&ua->stop, __ATOMIC_ACQUIRE)) {
        uasync_poll(ua, -1);
    }
}
