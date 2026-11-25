// tcp_io.h — управление TCP-соединением через uasync + ll_queue
//
// Один tcp_conn = одно TCP-соединение. Два пула:
//   entry_pool — struct ll_entry (без inline data)
//   data_pool  — буферы (max(entry_data_size, write_chunk_size))
//
// Чтение: read_cb → recv() в data_pool → entry в read_queue → callback внешнего кода.
//   high_water/low_water управляют backpressure (EPOLLIN on/off).
//   FIN (recv==0) → on_fin (сразу или через empty_callback после дренажа очереди).
//
// Запись: внешний код кладёт entry в write_queue через queue_data_put().
//   Первый write_queue_fetch_cb может быть синхронным; продолжение — через uasync_call_soon.
//   send(), при EAGAIN → write_buf + EPOLLOUT.
//   Всё отправлено → on_flushed.
//
// FIN/Close: tcp_conn_push_fin/close ставят сентинел в write_queue.
//   FIN  → shutdown(SHUT_WR) → on_fin_sent
//   CLOSE → close сокета → on_closed
//
// Ошибка: любая ошибка сокета → tcp_conn_handle_error → close + null коллбэков → on_error.
//   Реализация on_error обязана вызвать tcp_conn_destroy. После возврата tc недействителен.
//
// Все операции и callbacks — в потоке ua. После create установить consumer на read_queue:
//   queue_set_callback(tc->read_queue, on_read, arg).
// Consumer извлекает entry, обрабатывает e->len байт, возвращает dgram в tc->data_pool,
// освобождает entry и вызывает queue_resume_callback(q) для продолжения чтения.
// Для записи выделить entry из tc->entry_pool и dgram из tc->data_pool, заполнить len и dgram_pool;
// затем queue_data_put(tc->write_queue, e). Длина должна помещаться в буфер и uint16_t.
// После принятого FIN новые данные не добавлять; закрывать сокет через push_close/destroy.
// HUP не снимает backpressure/явную pause_read: регистрация без доступной работы временно удаляется,
// low_water восстанавливает её с новым поколением uasync. FD/очереди сохраняются до решения владельца.
// Ошибка восстановления/ожидания очереди вызывает on_error, как остальные ошибки TCP.

#ifndef TCP_IO_H
#define TCP_IO_H

#ifdef __cplusplus
extern "C" {
#endif


#include "u_async.h"
#include "socket_compat.h"
#include "ll_queue.h"
#include "memory_pool.h"

struct tcp_conn {
    socket_t sock;
    struct UASYNC* ua;
    void*    socket_id;
    struct timeout_node* deferred_free; // Резерв для освобождения tc и пулов после возврата из callbacks.

    struct ll_queue* read_queue;      // сокет → данные (блоки до entry_data_size)
    struct ll_queue* write_queue;     // данные → сокет (блоки до write_chunk_size)

    int      read_high_water;         // Число входных entry для приостановки чтения
    int      read_low_water;          // Порог очереди для возобновления чтения
    int      rcvbuf_size;             // Запрошенный SO_RCVBUF; <=0 — не настраивать буферы
    uint8_t  read_paused;             // Чтение приостановлено; low_water снимает только автоматическую паузу
    uint8_t  read_stopped;            // Владелец запретил чтение; low_water/HUP не снимает запрет
    uint8_t  hup_received;            // Получен HUP с SO_ERROR=0; idle registration снимается до готовности очереди

    uint8_t  write_monitor;           // 1 = EPOLLOUT активен
    uint8_t  connected;
    uint8_t  error;
    uint8_t  fin_remote;              // FIN получен от удалённой стороны (recv == 0)
    uint8_t  fin_pending;             // FIN поставлен в write_queue, shutdown ещё не выполнен
    uint8_t  fin_local;               // FIN отправлен удалённой стороне (shutdown SHUT_WR)
    uint8_t  closed;                  // сокет полностью закрыт (close)
    uint8_t  destroyed;               // 1 = tcp_conn_destroy вызван, tc ожидает отложенного free

    int      connect_timeout_ms;      // таймаут установления соединения, 0 = без таймаута
    void*    connect_timer;           // handle uasync таймера connect

    // Частичная отправка (из data_pool, не в очереди — досылается первой)
    uint8_t* write_buf;
    size_t   write_len;
    size_t   write_offset;
    size_t   write_chunk_size;

    // Пулы памяти
    struct memory_pool* entry_pool;   // sizeof(struct ll_entry)
    struct memory_pool* data_pool;    // max(entry_data_size, write_chunk_size)
    size_t   entry_data_size;

    struct queue_waiter_handle read_waiter;

    // Коллбэки
    void (*on_connected)(struct tcp_conn* tc, void* arg); // исходящий connect завершён
    void (*on_fin)(struct tcp_conn* tc, void* arg);       // FIN получен; вызов сразу при пустой read_queue, иначе после её дренажа
    void (*on_fin_sent)(struct tcp_conn* tc, void* arg);   // FIN отправлен удалённой стороне (fin_local=1)
    void (*on_error)(struct tcp_conn* tc, int err, void* arg); // фатальная ошибка (вызывается после close сокета; обязан вызвать tcp_conn_destroy)
    void (*on_flushed)(struct tcp_conn* tc, void* arg);    // все данные записи отправлены (write_queue + write_buf пусты)
    void (*on_closed)(struct tcp_conn* tc, void* arg);     // сокет закрыт через очередь (после close)
    void* arg;                       // Заимствованный контекст всех callbacks, модуль его не освобождает
};

// Регистрирует подключённый/подключающийся неблокирующий sock в ua; сам connect не вызывает.
// entry_data_size — ёмкость recv-блока, write_chunk_size — размер буфера записи; оба 1..UINT16_MAX,
// max этих размеров должен вмещать void* (требование пула). Пороги: 0 <= low < high, проверяет вызывающий.
// rcvbuf_size > 0 пробует SO_SNDBUF=0/SO_RCVBUF=rcvbuf_size; ошибка опции логируется, create продолжается.
// Создаёт две очереди и два пула; on_fin/on_error и arg сохраняются без передачи владения arg.
// Результат — tc или NULL при неверных ua/sock, OOM/ошибке регистрации; при NULL sock остаётся вызывающему.
// При успехе сокетом владеет tc. Пулы и выданные из них блоки живут до отложенного освобождения tc.
struct tcp_conn* tcp_conn_create(
    struct UASYNC* ua,
    socket_t sock,
    size_t entry_data_size,
    size_t write_chunk_size,
    int read_high_water,
    int read_low_water,
    int rcvbuf_size,
    void (*on_fin)(struct tcp_conn* tc, void* arg),
    void (*on_error)(struct tcp_conn* tc, int err, void* arg),
    void* arg);

// Сразу отменяет callbacks/таймер, дренирует очереди и закрывает сокет.
// Пулы и tc освобождаются через uasync_call_soon в том же uasync-потоке.
// NULL безопасен; повторный destroy допустим только до отложенного free.
// Владелец после destroy больше не использует tc; event loop должен выполнить отложенную очистку.
// Ранее извлечённые entry/dgram вернуть до освобождения пулов; перед destroy ua дренировать call_soon вне callbacks.
void tcp_conn_destroy(struct tcp_conn* tc);

// Запись: queue_data_put(tc->write_queue, entry) напрямую.
// Первый автозабор может быть синхронным; queue_resume_callback откладывает продолжение.
// Порог backpressure: count <= 32, не жёсткий лимит ёмкости; производитель ждёт через queue_waiter_wait.
// Обычные буферы записи должны принадлежать data_pool: отправка возвращает их непосредственно в этот пул.

// Поставить сигналы в очередь отправки. Все данные в очереди перед сигналом
// будут переданы ОС до его обработки. Сигналы имеют len=0: dgram=NULL → CLOSE,
// dgram=&tcp_fin_sentinel → FIN (внутренний указатель, сигналы ставить только через API).
//   tcp_conn_push_fin(tc)    — FIN: после отправки предшествующих данных вызывает
//                              shutdown(SHUT_WR) и on_fin_sent.
//                              Повторный вызов возвращает -1 при fin_pending или fin_local.
//   tcp_conn_push_close(tc)  — CLOSE: после отправки предшествующих данных закрывает
//                              сокет (close), вызывает on_closed.
//                              После close сокет удалён из uasync, tc->closed=1.
//                              После фактического закрытия повторный вызов возвращает -1.
// Оба API: 0 = сигнал принят; -1 = ошибка. Callback может выполниться до возврата из push.
int tcp_conn_push_fin(struct tcp_conn* tc);
int tcp_conn_push_close(struct tcp_conn* tc);

// Одноразовый коллбэк: вызывается когда write_queue + write_buf полностью опустели.
// Сбрасывается перед вызовом; callback может установить новое уведомление.
// Setter только заменяет функцию (NULL снимает); для уже пустой очереди сам вызов не планирует.
void tcp_conn_set_flushed(struct tcp_conn* tc, void (*on_flushed)(struct tcp_conn* tc, void* arg));

// Принудительная остановка чтения: убирает EPOLLIN и отменяет read_waiter.
// 0 — чтение остановлено, -1 — ошибка. Callback ошибки уже мог уничтожить tc.
int tcp_conn_pause_read(struct tcp_conn* tc);

// Заменить connect-таймаут: мс от текущего вызова, <=0 отменяет таймер.
// Только для живого tc в его uasync-потоке. Для connected=1 таймер не создаётся.
// Допустимый положительный диапазон: <= INT_MAX/10 (uasync принимает 0.1 мс).
// Возврат: 0 = применено; -EINVAL = аргументы/состояние, -ENOMEM = создание таймера,
// -EIO = нарушение состояния отменяемого таймера. При ошибке прежняя настройка сохраняется.
// Функция не вызывает callbacks. Истечение → on_error(ETIMEDOUT); отмена при connect/error/destroy.
int tcp_conn_set_connect_timeout(struct tcp_conn* tc, int timeout_ms);


#ifdef __cplusplus
}
#endif
#endif
