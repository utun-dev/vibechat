/*
 * control_server.h - Control Socket Server for ETCP Monitoring
 * 
 * TCP-мониторинг ETCP: init → обработка команд в uasync-потоке → shutdown.
 * Метрики отправляются по запросу; доступ — по IPv4 [control] allow.
 * Один таймер 30 секунд: отсутствие команд в покое либо отсутствие прогресса send при активном TX.
 * Шифрования/аутентификации протокола нет. Общий wire-контракт — etcpmon_protocol.h.
 * Client владеет сокетом, таймером и одним TX-кадром до 32 КиБ. Остальные команды ждут в RX.
 * Начальный список узлов хранится как ID, сериализуется порционно; live-изменения объединяются по ID.
 */

#ifndef CONTROL_SERVER_H
#define CONTROL_SERVER_H

#include "../tools/etcpmon/etcpmon_protocol.h"
#include "../lib/socket_compat.h"
#include "../lib/ll_queue.h"
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct UTUN_INSTANCE;
struct UASYNC;
struct TOPO_GROUP_NODE;
struct ll_queue;

/* Forward declaration */
struct control_server;

#define CONTROL_NODE_DIRTY_MAX 256

/* Client connection state */
struct control_client {
    struct control_client* next;        /* Linked list */
    struct control_server* server;      /* Back pointer to server */
    socket_t fd;                        /* Client socket */
    void* socket_id;                    /* uasync socket ID */
    
    /* Receive buffer */
    uint8_t recv_buffer[ETCPMON_MAX_MSG_SIZE];
    uint16_t recv_len;                  /* Bytes received so far */
    
    /* Selected connection */
    uint64_t selected_peer_id;          /* 0 = none selected */

    struct ll_queue* output_queue;      /* Владеет единственным TX-кадром; следующий producer ждёт пустой очереди */
    uint16_t output_offset;             /* Отправленные байты головы очереди; dgram/len не меняются */
    void* idle_timer;                   /* Отсутствие команд либо прогресса TX, см. timer_is_tx */
    uint8_t subscribed_nodes;           /* 1 = subscribed to node updates */
    uint8_t write_registered;           /* 1 = write events registered in uasync */
    uint8_t read_paused;                /* Чтение/разбор ждут пустой TX-очереди */
    uint8_t failed;                     /* Ошибка producer; владелец callback закроет client после возврата */
    uint8_t timer_is_tx;                /* Таймер контролирует прогресс TX, иначе отсутствие команд */
    struct queue_waiter_handle output_waiter; /* Отложенное продолжение; отменить до освобождения client */
    uint64_t* snapshot_ids;             /* ID начального списка; никаких заимствованных TOPO_GROUP_NODE* */
    size_t snapshot_count;
    size_t snapshot_index;              /* Следующий ID для сериализации */
    uint8_t snapshot_active;            /* 1 до постановки NODE_INFO_END */
    uint8_t snapshot_seq;               /* seq запроса для NODE_INFO_END */
    uint64_t dirty_ids[CONTROL_NODE_DIRTY_MAX]; /* Последние изменения объединяются по ID */
    uint8_t dirty_removed[CONTROL_NODE_DIRTY_MAX]; /* Последнее событие: 1 = удаление, 0 = обновление */
    size_t dirty_count;                 /* При переполнении подписчик отключается, события не теряются молча */
};

/* Control server state */
struct control_server {
    struct UTUN_INSTANCE* instance;     /* Parent instance */
    struct UASYNC* ua;                  /* Async context */
    
    /* Listening socket */
    socket_t listen_fd;
    void* listen_socket_id;             /* uasync socket ID */
    struct sockaddr_storage bind_addr;  /* Bound address */
    
    /* Client connections */
    struct control_client* clients;     /* Linked list of clients */
    uint32_t client_count;              /* Number of connected clients */
    uint32_t max_clients;               /* Maximum allowed clients */
    
    /* Log file */
    FILE* log_file;
};

/* ============================================================================
 * Public API
 * ============================================================================ */

/* Initialize control server
 * 
 * Creates listening socket and registers with uasync
 * 
 * Parameters:
 *   server  - Server state structure to initialize
 *   ua      - uasync context for event handling
 *   instance - Parent utun instance
 *   bind_addr - Address to bind (from config control_sock)
 *   max_clients - Maximum concurrent client connections; 0 selects 8
 * 
 * Returns:
 *   0 on success, -1 on error. Server copies bind_addr; caller retains ua/instance/server.
 *   With valid arguments, shutdown also cleans resources left by a failed initialization.
 */
int control_server_init(struct control_server* server,
                        struct UASYNC* ua,
                        struct UTUN_INSTANCE* instance,
                        struct sockaddr_storage* bind_addr,
                        uint32_t max_clients);

/* Shutdown control server
 * 
 * Closes all client connections and listening socket
 * 
 * Parameters:
 *   server - Server state to shutdown
 */
void control_server_shutdown(struct control_server* server);

/* Разобрать накопленные команды всех клиентов в uasync-потоке.
 * Чтение сокета само запускает разбор; отдельный периодический вызов не требуется.
 * Функция лишь повторно разбирает уже принятые данные, не читает сокеты и не рассылает метрики по таймеру.
 * 
 * Parameters:
 *   server - Server state
 */
void control_server_process_updates(struct control_server* server);

/* Get number of connected clients
 * 
 * Parameters:
 *   server - Server state
 * 
 * Returns:
 *   Number of connected clients
 */
uint32_t control_server_get_client_count(const struct control_server* server);

/* В uasync-потоке отметить последнее изменение node у подписчиков; node заимствован на время вызова.
 * NODE_INFO содержит 01 04 + topo_node_serialize для UTUN с подсетями; seq=0.
 * Сериализация отложена до освобождения TX, запись заново ищется по ID. Вызывать только для UTUN-группы.
 * Более 256 различных ожидающих ID отключают подписчика с WARN; события не теряются молча. */
void control_server_notify_node_change(struct control_server* server, struct TOPO_GROUP_NODE* node);
/* В uasync-потоке отметить удаление UTUN node_id; NODE_REMOVED с seq=0 отправляется после освобождения TX. */
void control_server_notify_node_removed(struct control_server* server, uint64_t node_id);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_SERVER_H */
