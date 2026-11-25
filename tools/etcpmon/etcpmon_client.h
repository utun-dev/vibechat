/*
 * TCP-клиент control_server: init → connect → регулярный process → cleanup.
 * Все вызовы и callbacks выполняются в одном потоке; Winsock запускает/останавливает вызывающий код.
 * connect принимает числовой IPv4. connected включает CONNECTING, fully_connected означает готовый TCP.
 * process завершает connect, принимает кадры и сам запрашивает метрики выбранного пира раз в 100 мс.
 * Списки и debug config запрашивает вызывающая сторона. TX ограничен 64 КиБ/256 кадрами;
 * short send/WSAEWOULDBLOCK продолжаются в process с сохранением порядка и смещения.
 * Pending хранит до 255 запросов. Срок connect, отсутствия прогресса TX и ответа — по 5 секунд;
 * срок ответа начинается после полной отправки соответствующего кадра.
 */

#ifndef ETCPMON_CLIENT_H
#define ETCPMON_CLIENT_H

#include "etcpmon_protocol.h"
#include <winsock2.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Graph history size (samples) */
#define GRAPH_HISTORY_SIZE  860
#define ETCPMON_TX_MAX_BYTES (2u * ETCPMON_MAX_MSG_SIZE)
#define ETCPMON_TX_MAX_FRAMES 256

struct etcpmon_tx_frame;

/* Graph metrics types */
typedef enum {
    GRAPH_METRIC_INFLIGHT_BYTES = 0,
    GRAPH_METRIC_INFLIGHT_LIM,
    GRAPH_METRIC_MIN_RTT,
    GRAPH_METRIC_BW_HI,
    GRAPH_METRIC_PACING_RATE,
    GRAPH_METRIC_BW_LO,
    GRAPH_METRIC_INFLIGHT_HI,
    GRAPH_METRIC_BBR_MODE,
    GRAPH_METRIC_COUNT
} graph_metric_type_t;

/* Кольцевая история первого линка: объёмы в байтах, скорости в байтах/с, minRTT в мс, mode безразмерный. */
struct metrics_history {
    float values[GRAPH_METRIC_COUNT][GRAPH_HISTORY_SIZE];
    float min_val[GRAPH_METRIC_COUNT];
    float max_val[GRAPH_METRIC_COUNT];
    DWORD last_minmax_update;
    int head;                       /* Индекс следующей записи, 0..GRAPH_HISTORY_SIZE-1 */
    int count;                      /* Число заполненных сэмплов, не более GRAPH_HISTORY_SIZE */
    uint32_t last_retrans;
    uint32_t last_acks;
    uint64_t last_bytes_sent;
};

/* Клиент владеет сокетом, логом, кешами и pending. Не копировать активную структуру. */
struct etcpmon_client {
    SOCKET sock;                    /* INVALID_SOCKET после init/disconnect */
    int connected;                  /* 1 с начала неблокирующего connect */
    int fully_connected;            /* 1 после успешной проверки select/SO_ERROR в process */
    DWORD connect_started_at;        /* Монотонные мс начала connect; срок 5 секунд */
    struct etcpmon_tx_frame* tx_head; /* FIFO исходящих кадров; client владеет памятью */
    struct etcpmon_tx_frame* tx_tail;
    size_t tx_bytes;                 /* Сумма полных размеров кадров, включая частично отправленную голову */
    unsigned tx_count;               /* Не более ETCPMON_TX_MAX_FRAMES */
    DWORD tx_progress_at;            /* Начало ожидания или последний положительный send */
    int tx_blocked;                  /* 1 после WSAEWOULDBLOCK, для логирования переходов */
    
    /* Receive buffer */
    uint8_t recv_buffer[ETCPMON_MAX_MSG_SIZE];
    uint16_t recv_len;               /* Неразобранный хвост TCP-потока; границы recv не являются границами кадров */
    
    /* Server address */
    char server_addr[64];
    uint16_t server_port;
    
    /* Selected connection */
    uint64_t selected_peer_id;       /* 0 запрещает автоматический запрос метрик */
    
    /* Connection info cache */
    struct etcpmon_conn_info* conn_list;
    uint8_t conn_count;

    /* Socket info cache */
    struct etcpmon_socket_info* socket_list;
    uint8_t socket_count;

    /* Latest metrics */
    struct etcpmon_rsp_metrics* last_metrics;
    struct etcpmon_link_metrics* last_links;
    uint8_t last_links_count;
    
    /* Log file */
    FILE* log_file;
    
    /* Metrics history for graph */
    struct metrics_history history;
    
    /* Принятые в TX запросы с ненулевым seq_id; disconnect отменяет их без вызова callbacks. */
    struct etcpmon_pending_request* pending_head;
    struct etcpmon_pending_request* pending_tail;
    uint8_t next_seq_id;
    
    /* Last received response seq_id (for matching) */
    uint8_t last_response_seq_id;
    
    /* Debug config cache */
    uint8_t debug_global_level;
    uint8_t debug_category_count;
    uint8_t debug_category_levels[ETCPMON_MAX_DEBUG_CATEGORIES];
    char    debug_category_names[512];
    uint8_t debug_config_dirty;

    /* Callbacks вызываются синхронно в потоке вызывающего API; аргументы заимствованы, владение не передаётся. */
    void (*on_connected)(void* user_data);
    void (*on_disconnected)(void* user_data);
    void (*on_conn_list)(struct etcpmon_conn_info* list, uint8_t count, void* user_data);
    void (*on_socket_list)(struct etcpmon_socket_info* list, uint8_t count, void* user_data);
    void (*on_metrics)(struct etcpmon_rsp_metrics* metrics,
                       struct etcpmon_link_metrics* links,
                       uint8_t links_count, void* user_data);
    void (*on_error)(const char* msg, void* user_data);
    void (*on_action_result)(const char* text, void* user_data);
    
    void* user_data;
    uint64_t last_metrics_request_ms; /* GetTickCount64, монотонные мс; 0 после выбора/отключения */
};

/* Pending request with callback */
struct etcpmon_pending_request {
    uint8_t seq_id;                              /* Sequence ID */
    uint8_t type;                                /* Тип команды для ограничения опроса */
    DWORD sent_at;                               /* Монотонное время полной отправки; значимо только при sent=1 */
    uint8_t sent;                                /* 1 после полной отправки; только тогда идёт таймаут ответа */
    void (*callback)(void* arg, uint8_t msg_type, const uint8_t* payload, uint16_t payload_size);
    void* arg;                                   /* User argument for callback */
    struct etcpmon_pending_request* next;
};

/* Обнулить неактивную caller-owned структуру client; сокет/лог не открывает. */
void etcpmon_client_init(struct etcpmon_client* client);

/* Отключить client и освободить его кеши, pending и лог; сам client и Winsock не освобождает. */
void etcpmon_client_cleanup(struct etcpmon_client* client);

/* Начать connect к числовому IPv4 addr:port. 0 = попытка начата, -1 = немедленная ошибка.
 * Завершение/таймаут 5 секунд проверяет process; on_connected означает успех.
 * Уже открытое соединение закрывается. Лог etcpmon.log создаётся при первой попытке в текущем каталоге. */
int etcpmon_client_connect(struct etcpmon_client* client, const char* addr, uint16_t port);

/* Закрыть сокет client, сбросить выбор/историю/кеши и отменить pending без callbacks.
 * Лог и кеш debug config сохраняются. Сам disconnect не вызывает on_disconnected. */
void etcpmon_client_disconnect(struct etcpmon_client* client);

/* Вернуть connected (также 1 во время CONNECTING), для NULL — 0. */
int etcpmon_client_is_connected(const struct etcpmon_client* client);

/* Поставить LIST_CONN с seq=0 в TX без ожидания ответа. 0 = принят в TX, -1 = ошибка.
 * Отказ выделения/лимита TX не меняет соединение; фатальная ошибка send отключает его с callbacks. */
int etcpmon_client_request_list(struct etcpmon_client* client);

/* Поставить LIST_SOCKETS с seq=0 в TX без ожидания ответа; возврат как у request_list. */
int etcpmon_client_request_socket_list(struct etcpmon_client* client);

/* Отправить SELECT_CONN с seq=0, без подтверждения; peer_node_id=0 снимает выбор.
 * После принятия в TX меняет локальный selected_peer_id и очищает историю.
 * 0 = принят в TX, -1 = ошибка; существование пира проверит сервер при запросе метрик. */
int etcpmon_client_select_connection(struct etcpmon_client* client, uint64_t peer_node_id);

/* Разовый GET_METRICS с seq=0, без ожидания ответа. Обычно достаточно автоопроса в process.
 * 0 = принят в TX, -1 = ошибка; этот вызов не участвует в ограничении автоопроса. */
int etcpmon_client_request_metrics(struct etcpmon_client* client);

/* Поставить type/payload с уникальным seq=1..255 в ограниченный TX; payload копируется до возврата.
 * 0 = кадр и pending приняты, -1 = ошибка. Фатальная ошибка send закрывает соединение с callbacks.
 * Обязательный callback(arg, type, payload, size) вызывается по совпадению seq до общих callbacks;
 * тип ответа и полный payload проверяются до снятия pending/callback. RSP_ERROR завершает любой запрос.
 * Payload действителен только во время callback. Неизвестный ненулевой seq игнорируется с WARN.
 * callback может отправлять запросы или отключить клиент; рекурсивно вызывать process нельзя.
 * Таймаут 5 с проверяется в process; при отмене/таймауте callback не вызывается. */
int etcpmon_client_send_request(struct etcpmon_client* client,
                                 uint8_t type,
                                 const void* payload, uint16_t payload_size,
                                 void (*callback)(void* arg, uint8_t msg_type, const uint8_t* payload, uint16_t payload_size),
                                 void* arg);

/* Неблокирующий шаг в потоке владельца: connect, досылка TX, одно recv, разбор, таймауты и автоопрос.
 * Возвращает число разобранных кадров (может быть >1), 0 = полных кадров нет, -1 = ошибка/отключение.
 * На автоопрос метрик одновременно допускается один pending. Вызывать регулярно, чаще 100 мс.
 * Фатальная ошибка TCP/протокола или таймаут закрывают соединение и вызывают on_disconnected/on_error. */
int etcpmon_client_process(struct etcpmon_client* client);

/* Установить необязательные общие callbacks и caller-owned user_data.
 * Списки/метрики принадлежат client до следующей замены кеша/отключения;
 * строки ошибок/Action действительны только во время callback. */
void etcpmon_client_set_callbacks(struct etcpmon_client* client,
                                   void (*on_connected)(void*),
                                   void (*on_disconnected)(void*),
                                   void (*on_conn_list)(struct etcpmon_conn_info*, uint8_t, void*),
                                   void (*on_socket_list)(struct etcpmon_socket_info*, uint8_t, void*),
                                   void (*on_metrics)(struct etcpmon_rsp_metrics*,
                                                      struct etcpmon_link_metrics*,
                                                      uint8_t, void*),
                                   void (*on_error)(const char*, void*),
                                   void (*on_action_result)(const char*, void*),
                                   void* user_data);

/* Заимствованное имя peer_id в кеше client до его замены/отключения; NULL, если не найдено. */
const char* etcpmon_client_get_conn_name(struct etcpmon_client* client, uint64_t peer_id);

/* Заимствованная встроенная история client; NULL для NULL client, отдельно не освобождать. */
struct metrics_history* etcpmon_client_get_history(struct etcpmon_client* client);

/* Добавить сэмпл из client->last_links[0]; metrics должен быть ненулевым.
 * Вызывается парсером метрик ровно один раз на принятый снимок; GUI повторно не добавляет. */
void etcpmon_client_add_to_history(struct etcpmon_client* client, struct etcpmon_rsp_metrics* metrics);

/* Отправить action (до 31 байта + NUL) как запрос с pending; возврат как у send_request.
 * Успех передаётся on_action_result, ошибка команды — on_error без отключения TCP.
 * "nat" подтверждает запуск проверки, "nodes" возвращает текстовый дамп. */
int etcpmon_client_send_action(struct etcpmon_client* client, const char* action);

/* Запрос debug config с pending; 0 = принят в TX, -1 = ошибка. Кеш обновляет process. */
int etcpmon_client_request_debug_config(struct etcpmon_client* client);
/* Отправить SET_DEBUG_CONFIG с seq=0 без подтверждения: count=1..64, levels[count].
 * 0 = принят в TX и локальные уровни обновлены; -1 = ошибка.
 * Категории следуют порядку имён из ответа GET_DEBUG_CONFIG; GUI поддерживает уровни 0..5. */
int etcpmon_client_send_debug_config(struct etcpmon_client* client, uint8_t global_level, uint8_t count, const uint8_t* levels);

/* Wall-clock мс Unix epoch для лога; для таймаутов не используется. */
uint64_t get_timestamp_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* ETCPMON_CLIENT_H */
