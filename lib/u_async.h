/* Цикл событий одного потока: сокеты (epoll/poll/select), таймеры и FIFO call_soon.
 * create → регистрация callbacks → poll/mainloop → destroy вне callbacks после остановки владельцев.
 * Таймауты задаются в timebase 0.1мс, положительный абсолютный срок округляется вверх до миллисекунды.
 * arg принадлежит вызывающему, освобождает его callback/владелец; отмена и destroy arg не освобождают.
 * API сокетов/таймеров используется в потоке loop. Из другого потока — post/post_reserved, wakeup и stop;
 * публикация не продлевает время жизни ua/arg, их освобождение нужно согласовать с остановкой producers. */

#ifndef UASYNC_H
#define UASYNC_H

#ifdef __cplusplus
extern "C" {
#endif


#include "platform_compat.h"
#include <stddef.h>
#include <signal.h>
#include "timeout_heap.h"
#include "socket_compat.h"

typedef void (*timeout_callback_t)(void* user_arg);// передаёт user_arg из uasync_set_timeout
typedef void (*socket_callback_t)(int fd, void* user_arg);// передаёт user_arg из uasync_add_socket (for pipe/file)
typedef void (*socket_t_callback_t)(socket_t sock, void* user_arg);// передаёт user_arg из uasync_add_socket_t (for sockets)
// user_arg полезен если нужно передать управляющую структуру. Ее можно выделить в памяти и в ней хранить всё что надо. т.е. при set_timeout передаём и получаем ее в callback-е


// Error type
typedef int err_t;
#define ERR_OK 0
#define ERR_FAIL -1

// Socket node types
#define SOCKET_NODE_TYPE_FD     0  // Regular file descriptor (pipe, file)
#define SOCKET_NODE_TYPE_SOCK   1  // Socket (socket_t)

typedef void (*uasync_post_callback_t)(void* user_arg);

#include "memory_pool.h"

struct timeout_node;  // Forward declaration
struct uasync_post_batch;
struct UASYNC;

// Встроенный отложенный вызов. Обнулить при создании, отменить до освобождения владельца.
struct uasync_work {
    struct UASYNC* ua;                  // Очередь владельца, NULL между постановками.
    struct uasync_work *prev, *next;    // Соседи в общем FIFO.
    uint64_t sequence;                 // Номер постановки для границы текущей порции.
    timeout_callback_t callback;
    void* arg;
};

/* CPU одного потока: 100% = одно ядро. При работе цикла/запросе снимок обновляется раз в 100 мс, без пробуждений.
 * До первого замера valid=0; window_us растёт до секунды. При задержке замера
 * вклад граничного интервала в среднее оценивается пропорционально времени. */
struct uasync_load {
    double current_percent;      // CPU потока за последний фактический интервал, 0..100%
    double average_percent;      // Среднее за накопленное окно до секунды, 0..100%
    uint64_t sample_time_us;     // Время снимка в шкале get_time_us
    uint64_t interval_us;        // Длительность последнего интервала
    uint64_t window_us;          // Длительность учтённого окна среднего
    int valid;                  // 1 = снимок готов; 0 = ещё нет замера или ошибка часов
};

struct uasync_load_interval {
    uint64_t start_us, end_us, cpu_ns;
};

struct posted_task {
        uasync_post_callback_t callback;
        void* arg;
        struct posted_task* next;
    };

// Uasync instance structure
struct UASYNC {
    struct memory_pool* timeout_pool;  // Pool for timeout_node allocation
    struct uasync_work* immediate_queue_head;  // FIFO queue for immediate execution
    struct uasync_work* immediate_queue_tail;
    uint64_t immediate_sequence;       // Номер последнего вызова в FIFO.
    TimeoutHeap* timeout_heap;  // Heap for timeout management
    struct socket_array* sockets; // Array-based socket management
    // Debug counters for memory allocation tracking
    size_t timer_alloc_count;
    size_t timer_free_count;
    size_t socket_alloc_count;
    size_t socket_free_count;
    // Wakeup: eventfd на Linux, pipe на остальных POSIX, UDP-сокеты на Windows
    socket_t wakeup_pipe[2]; // [0] чтение, [1] запись; для eventfd значения совпадают, Windows — полный SOCKET.
    int wakeup_initialized;
    // Кеш pollfd для POSIX fallback poll, включая Linux без epoll
    struct pollfd* poll_fds;      // Pre-allocated pollfd array
    int poll_fds_capacity;        // Current capacity of poll_fds
    int poll_fds_count;           // Number of valid entries in poll_fds
    int poll_fds_dirty;           // 1 if poll_fds needs rebuild
    // Epoll support (Linux only)
    int epoll_fd;                 // epoll file descriptor (-1 if not using epoll)
    int use_epoll;                // 1 = epoll; 0 = poll на POSIX или select на Windows
    struct posted_task* posted_tasks_head;
    struct posted_task* posted_tasks_tail;
    struct uasync_post_batch* posted_batches; // Ожидающие задачи вложенных dispatch; только поток loop

#ifdef _WIN32
    CRITICAL_SECTION posted_lock;
#else
    pthread_mutex_t posted_lock;
#endif
    volatile sig_atomic_t stop;      // Флаг выхода из mainloop; stop устанавливает, автоматически не сбрасывается
    volatile sig_atomic_t running;   // Статус, выставляемый владельцем через mark_*; API его не проверяет
    uint64_t last_poll_exit_us;
    struct uasync_load load;
    struct uasync_load_interval load_history[12]; // 10 периодов + граничные интервалы при задержках
    uint64_t load_previous_us, load_previous_cpu_ns, load_next_us, load_log_us;
    unsigned int load_head, load_count;
    int load_state;              // 0=не начат, 1=работает, -1=ошибка часов
};

// Type definitions
typedef struct UASYNC uasync_t;
typedef struct UASYNC UASYNC_t;

// Атомарно читает статус владельца (NULL → 0); состояние stop и работу poll не определяет.
static inline int uasync_is_running(struct UASYNC* ua) {
    if (!ua) return 0;
    return __atomic_load_n(&ua->running, __ATOMIC_ACQUIRE);
}

// Создаёт готовый к регистрации и poll экземпляр с running=0, stop=0; NULL при ошибке инициализации.
struct UASYNC* uasync_create(void);
// Уничтожение вне callbacks после возврата из poll/mainloop и завершения всех producers; NULL допустим.
// close_fds != 0 закрывает оставшиеся зарегистрированные fd; внутренние wakeup/epoll закрываются всегда.
// Ожидающие таймеры и задачи удаляются без вызова callbacks; user_arg остаётся владельцу.
void uasync_destroy(struct UASYNC* ua, int close_fds);
// Атомарно выставляют running=1/0. Не меняют stop, не запускают/останавливают цикл и не запрещают API.
void uasync_mark_running(struct UASYNC* ua);
void uasync_mark_stopped(struct UASYNC* ua);

// Время в единицах 0.1мс: Linux CLOCK_BOOTTIME (включает suspend), прочие POSIX CLOCK_MONOTONIC, Windows QPC.
uint64_t get_time_tb(void);
// Время в микросекундах в той же шкале; календарные часы ОС на неё не влияют.
uint64_t get_time_us(void);

// Timeouts, timebase = 0.1 mS
// Как работать с таймаутами если таймаут может быть использован несколько раз:
// 1. заводим дескриптор таймаута и обнуляем
// 2. перед активацией таймаута проверяем дескриптор (t_id) на null
// 3. после успешного cancel или в начале callback дескриптор обнуляем
// это обеспечит отсутствие утечек и задвоений таймаутов
// Однократный таймер: timeout_tb >= 0, callback не NULL, user_arg передаётся без копирования.
// При timeout_tb > 0 срок = ceil((now_us + timeout_tb*100)/1000) мс; при 0 готов к обработке сразу.
// Callback вызывается при обработке heap, а не синхронно из set_timeout; задержка исполнения возможна.
// name сохраняется по указателю без ограничения в 15 символов; строка должна жить до освобождения узла.
// Возвращает handle или NULL при неверных аргументах/ошибке выделения ресурсов.
void* uasync_set_timeout(struct UASYNC* ua, int timeout_tb, void* user_arg, timeout_callback_t callback, const char* name);
// Только для ещё живого таймера этого ua: ERR_OK при отмене, ERR_FAIL при неверных аргументах/отсутствии в heap.
// Освобождение узла отложено до очистки heap/destroy. После освобождения старый handle использовать нельзя.
err_t uasync_cancel_timeout(struct UASYNC* ua, void* t_id);

// Зарезервировать неактивный таймер вместе с местом в heap. Возвращает NULL при отказе.
// Перед callback таймер снова неактивен. Окончательное освобождение через uasync_cancel_timeout.
struct timeout_node* uasync_reserve_timer(struct UASYNC* ua, void* arg, timeout_callback_t callback, const char* name);
// Изменить срок живого резерва без аллокаций. Положительное время в 0.1 мс, -1 снимает до следующей постановки.
void uasync_timer_set(struct timeout_node* timer, int timeout_tb);
// Есть ли назначенный срок у резерва. NULL допустим, возвращает 0.
int uasync_timer_armed(const struct timeout_node* timer);

// Запланировать callback(arg) в потоке ua без аллокаций. Повторная постановка ожидающего work объединяется с первой.
// Возвращает work. К моменту вызова он уже снят, callback может перепоставить его или освободить владельца.
struct uasync_work* uasync_schedule_work(struct UASYNC* ua, struct uasync_work* work, timeout_callback_t callback, void* arg);
// Снять встроенный вызов из FIFO. Для неактивного work ничего не делает.
void uasync_cancel_work(struct uasync_work* work);

// Добавляет callback(user_arg) в FIFO; handle или NULL при неверных аргументах/ошибке выделения.
// Один проход берёт исходную порцию; poll обрабатывает очередь в начале и после ожидания, перед heap.
// Поэтому callback может выполниться в текущем poll. Синхронно из call_soon он не вызывается.
void* uasync_call_soon(struct UASYNC* ua, void* user_arg, timeout_callback_t callback);
// Зарезервировать вызов callback(user_arg) в потоке ua. До постановки callback не исполняется.
// NULL при ошибке. Каждый успешный резерв владелец обязан поставить в очередь или вернуть через uasync_release_soon до уничтожения ua.
struct timeout_node* uasync_reserve_soon(struct UASYNC* ua, void* user_arg, timeout_callback_t callback);
// Передать ненулевой резерв в FIFO его ua ровно один раз. Постановка не требует памяти.
void uasync_call_soon_reserved(struct timeout_node* node);
// Возвращает неиспользованный резерв в пул. Допустимо только до первой постановки node в очередь.
void uasync_release_soon(struct timeout_node* node);
// Только для ещё живого узла call_soon этого ua: зануляет callback, освобождение — при обработке/destroy.
// ERR_OK при отмене (включая повторную до обработки узла), ERR_FAIL при NULL/чужом ua.
// После успешной отмены обнулить handle; после исполнения/освобождения обращаться к нему нельзя.
err_t uasync_call_soon_cancel(struct UASYNC* ua, void* t_id);

// Регистрирует fd с callbacks чтения/записи/исключения (каждый может быть NULL) и user_arg без копирования.
// name — заимствованная строка для логов, должна жить до удаления регистрации; NULL допустим.
// Возвращает handle или NULL при неверном fd, дублирующей регистрации или ошибке ресурсов/backend.
// Handle кодирует индекс без поколения, устойчив к расширению массива, но недействителен после удаления.
void* uasync_add_socket(struct UASYNC* ua, int fd, socket_callback_t read_cbk, socket_callback_t write_cbk, socket_callback_t except_cbk, const char* name, void* user_arg);
// То же для socket_t; callbacks получают значение sock. Возврат и владение как у uasync_add_socket.
void* uasync_add_socket_t(struct UASYNC* ua, socket_t sock, socket_t_callback_t read_cbk, socket_t_callback_t write_cbk, socket_t_callback_t except_cbk, const char* name, void* user_arg);
// Удаляет регистрацию по актуальному handle, не закрывает fd и не освобождает user_arg; ERR_OK / ERR_FAIL.
// Слот и значение handle могут быть переиспользованы новой регистрацией; старый handle нужно обнулить.
err_t uasync_remove_socket(struct UASYNC* ua, void* s_id);
// Удаляет регистрацию типа SOCK по значению sock; fd остаётся открыт. ERR_OK / ERR_FAIL.
err_t uasync_remove_socket_t(struct UASYNC* ua, socket_t sock);

// Меняет мониторинг чтения/записи по актуальному handle: enable != 0 включает, 0 отключает.
// Callback должен быть задан при регистрации. ERR_OK при успехе, ERR_FAIL при неверном handle/ошибке backend.
err_t uasync_set_socket_read(struct UASYNC* ua, void* s_id, int enable);
err_t uasync_set_socket_write(struct UASYNC* ua, void* s_id, int enable);

// Один проход в потоке владельца: call_soon, ожидание/I/O/posted tasks, call_soon и готовые таймеры.
// timeout_tb < 0 — без пользовательского предела ожидания; 0 — без блокирующего ожидания;
// > 0 — предел системного ожидания в 0.1мс с округлением вверх до миллисекунд, сокращаемый таймерами.
// После stop ожидание неблокирующее. Время callbacks не ограничено timeout_tb; ошибка ожидания может прервать проход.
void uasync_poll(struct UASYNC* ua, int timeout_tb);
// Синхронно опустошает только call_soon, включая новые порции из callbacks; постоянный самоперезапуск мешает возврату.
void uasync_drain_immediate(struct UASYNC* ua);

// Повторяет poll(ua, -1), пока stop == 0; уже установленный stop не сбрасывает и сразу возвращается.
void uasync_mainloop(struct UASYNC* ua);
// Атомарно устанавливает stop=1 и пытается разбудить ожидание; допустим из другого потока при живом ua.
// Не прерывает текущий poll/callback и оставшуюся обработку событий. running не изменяет.
void uasync_stop(struct UASYNC* ua);

// Копирует накопительные счётчики выделения/освобождения; любой выходной указатель может быть NULL.
void uasync_get_stats(struct UASYNC* ua, size_t* timer_alloc, size_t* timer_free, size_t* socket_alloc, size_t* socket_free);

// Актуализирует снимок по сроку 100 мс. Только в потоке uasync; наружу передавать копию через события.
// ERR_OK при копировании (в том числе с valid=0), ERR_FAIL при NULL ua/load.
err_t uasync_get_load(struct UASYNC* ua, struct uasync_load* load);

// Находит регистрацию по fd: 0 и актуальный handle, либо -1. socket_id обязателен.
// При допустимых аргументах и отсутствии регистрации пишет NULL; при неверных аргументах выход не меняет.
int uasync_lookup_socket(struct UASYNC* ua, int fd, void** socket_id);

// Выводит счётчики, записи heap таймеров (включая отменённые) и активные сокеты с заданным prefix.
void uasync_print_resources(struct UASYNC* ua, const char* prefix);

// Пробуждение: POSIX пишет uint64_t(1) в eventfd/pipe, Windows посылает байт в UDP-сокет; 0 / -1.
// Допустим из другого потока; ua и внутренний endpoint должны оставаться живы.
int uasync_wakeup(struct UASYNC* ua);
// Заимствованный endpoint записи или -1 при NULL/неинициализированном wakeup. Не закрывать самостоятельно.
// Linux: eventfd (тот же fd для чтения/записи; писать uint64_t), прочие POSIX: pipe, Windows: UDP-сокет.
socket_t uasync_get_wakeup_fd(struct UASYNC* ua);

// Потокобезопасно публикует callback(user_arg) для потока, обслуживающего ua. ua/callback должны быть не NULL.
// При OOM задача не публикуется, ошибка только логируется; статуса возврата нет. user_arg не копируется.
// При destroy ожидающая задача удаляется без callback; user_arg во всех случаях остаётся владельцу.
void uasync_post(struct UASYNC* ua, uasync_post_callback_t callback, void* user_arg);

/* Потокобезопасно передаёт заранее выделенный через u_malloc task без новых аллокаций; ua/task не NULL.
 * До передачи задать callback и arg. Производитель после передачи к task не обращается.
 * uasync освобождает task после callback, успешной отмены или при destroy без callback; arg не освобождает.
 * Публикация не проверяет running/stop. Ошибка wakeup не удаляет уже поставленную задачу и не возвращается. */
void uasync_post_reserved(struct UASYNC* ua, struct posted_task* task);
// Отмена в потоке loop после завершения производителя; допустима из callback, ua/task не NULL.
// ERR_OK — ожидающий task удалён и освобождён, в том числе из исполняемой пачки; arg остаётся владельцу.
// ERR_FAIL — task отсутствует или его callback уже начат; исполняющийся task отменить нельзя.
err_t uasync_cancel_post(struct UASYNC* ua, struct posted_task* task);

// Lock/unlock posted_lock у живого ua; не ждёт исполнения задач. post уже публикует данные под этим мьютексом.
void uasync_memsync(struct UASYNC* ua);


#ifdef __cplusplus
}
#endif
#endif // UASYNC_H
