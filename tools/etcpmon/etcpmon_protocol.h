/*
 * Общий формат TCP control_server для etcpmon и других локальных клиентов управления.
 * Кадр: [size:uint16][type:uint8][seq_id:uint8][payload]; size включает 4-байтовый заголовок,
 * допустимо 4..32768 байт. Структуры packed(1), числа копируются в порядке байтов хоста
 * без htons/ntohs; взаимодействующие сборки должны иметь одинаковый layout и endianness.
 * VERSION — константа исходников, по сети не передаётся; согласования версии нет.
 * TCP не сохраняет границы кадров. Метрики выдаются по запросу; 100 мс — интервал клиента.
 * seq=0 не связывается с pending; ответ на запрос копирует его seq (кроме отдельных NODE_INFO).
 * SELECT_CONN/SET_DEBUG_CONFIG/DISCONNECT не подтверждаются; ACTION даёт ACTION_RESULT либо ERROR.
 * Подписка на узлы: NODE_INFO с seq=0, затем NODE_INFO_END с seq запроса; обновления также с seq=0.
 * Встроенного шифрования/аутентификации нет; сервер допускает IPv4 по [control] allow.
 */

#ifndef ETCPMON_PROTOCOL_H
#define ETCPMON_PROTOCOL_H

#include <stdint.h>
#include "../../src/routing_layer/address_profile.h"


#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Protocol Constants
 * ============================================================================ */

#define ETCPMON_PROTOCOL_VERSION    3

/* Server config types (mirrored from config_parser.h) */
#define CFG_SERVER_TYPE_UNKNOWN     0
#define CFG_SERVER_TYPE_PUBLIC      1
#define CFG_SERVER_TYPE_NAT         2
#define CFG_SERVER_TYPE_PRIVATE     3
#define CFG_SERVER_TYPE_LOCAL       4
#define ETCPMON_MAX_MSG_SIZE        32768
#define ETCPMON_MAX_CONN_NAME       32
#define ETCPMON_MAX_CONNECTIONS     250
#define ETCPMON_MAX_LINKS           16
#define ETCPMON_MAX_DEBUG_CATEGORIES 64

/* Update interval in milliseconds */
#define ETCPMON_UPDATE_INTERVAL_MS  100

/* ============================================================================
 * Message Types
 * ============================================================================ */

/* Client -> Server commands */
#define ETCPMON_CMD_LIST_CONN       0x01    /* Request connection list */
#define ETCPMON_CMD_SELECT_CONN     0x02    /* Select connection by peer_node_id */
#define ETCPMON_CMD_GET_METRICS     0x03    /* Request metrics for selected */
#define ETCPMON_CMD_DISCONNECT      0x04    /* Disconnect from monitoring */
#define ETCPMON_CMD_LIST_SOCKETS    0x05    /* Request local socket list */
#define ETCPMON_CMD_ACTION          0x06    /* Execute action (text command) */
#define ETCPMON_CMD_GET_DEBUG_CONFIG 0x07   /* Request current debug levels */
#define ETCPMON_CMD_SET_DEBUG_CONFIG 0x08   /* Set debug levels (byte array) */
#define ETCPMON_CMD_SUBSCRIBE_NODES   0x09   /* Subscribe to node changes */

/* Server -> Client responses */
#define ETCPMON_RSP_CONN_LIST       0x81    /* Connection list response */
#define ETCPMON_RSP_METRICS         0x82    /* Metrics response */
#define ETCPMON_RSP_SOCKET_LIST     0x83    /* Socket list response */
#define ETCPMON_RSP_DEBUG_CONFIG    0x84    /* Debug levels response */
#define ETCPMON_RSP_NODE_INFO       0x85    /* Single node info (payload: raw BGP_NODEINFO_PACKET) */
#define ETCPMON_RSP_NODE_INFO_END   0x86    /* End of node list (no payload) */
#define ETCPMON_RSP_NODE_REMOVED    0x88    /* Node removed (payload: uint64_t node_id) */
#define ETCPMON_RSP_ACTION_RESULT  0x89    /* Action result text response */
#define ETCPMON_RSP_ERROR           0xFF    /* Error response */

/* Error codes for RSP_ERROR */
#define ETCPMON_ERR_INVALID_CMD     0x01    /* Unknown command */
#define ETCPMON_ERR_INVALID_CONN    0x02    /* Invalid connection ID */
#define ETCPMON_ERR_NO_CONN_SELECTED 0x03   /* No connection selected */
#define ETCPMON_ERR_SERVER_BUSY     0x04    /* Server too busy */

#pragma pack(push, 1)

/* ============================================================================
 * Message Header
 * ============================================================================
 * seq_id: 0 = broadcast/unsolicited, 1-255 = request/response pair */

struct etcpmon_msg_header {
    uint16_t size;          /* Total message size including header */
    uint8_t  type;          /* Message type (command or response) */
    uint8_t  seq_id;        /* Sequence ID: request pairs use same ID */
};

/* ============================================================================
 * Commands (Client -> Server)
 * ============================================================================ */

/* CMD_LIST_CONN: No payload required */

/* CMD_SELECT_CONN: Select connection to monitor */
struct etcpmon_cmd_select {
    uint64_t peer_node_id;  /* ID выбранного соединения; 0 снимает выбор, SELECT не подтверждается */
};

/* CMD_ACTION: Execute action command */
struct etcpmon_cmd_action {
    char action[32];        /* Ровно 32 байта с NUL; полное имя "nat"/"nodes", неизвестное имя даёт ERROR */
};

/* CMD_GET_METRICS: No payload required (uses previously selected connection) */

/* CMD_DISCONNECT: No payload required */

/* ============================================================================
 * Responses (Server -> Client)
 * ============================================================================ */

/* RSP_CONN_LIST: List of active connections */
struct etcpmon_conn_info {
    uint64_t peer_node_id;                  /* Connection identifier */
    char     name[ETCPMON_MAX_CONN_NAME];   /* Null-terminated name */
};

struct etcpmon_rsp_conn_list {
    uint8_t count;                          /* Number of connections */
    /* Followed by count * struct etcpmon_conn_info */
};

/* RSP_SOCKET_LIST: List of local sockets */
struct etcpmon_socket_info {
    char     name[ETCPMON_MAX_CONN_NAME];   /* Null-terminated socket name */
    char     ip[46];                        /* Null-terminated IP address string */
    uint16_t port;                          /* Port number (host byte order) */
    uint8_t  is_ipv6;                       /* 1 = IPv6, 0 = IPv4 */
    uint8_t  config_type;                   /* CFG_SERVER_TYPE_PUBLIC/NAT/PRIVATE */
    uint8_t  mapping, inbound, source;      /* ADDRESS_MAP_* / ADDRESS_IN_* / ADDRESS_SOURCE_* */
    uint8_t  is_tcp;
    uint64_t generation;
    char     nat_ip[46];                    /* Null-terminated NAT IP address string */
    uint16_t nat_port;                      /* NAT port (host byte order) */
};

struct etcpmon_rsp_socket_list {
    uint8_t count;                          /* Number of sockets */
    /* Followed by count * struct etcpmon_socket_info */
};

/* RSP_METRICS: Full metrics for ETCP connection and its links */

/* ETCP Connection metrics */
struct etcpmon_etcp_metrics {
    uint64_t peer_node_id;            /* Connection identifier */
    uint32_t rtt_last;              /* Last RTT (0.1ms units) */
    uint32_t rtt_avg_10;            /* Average RTT over 10 packets */
    uint32_t jitter;                /* Jitter (max-min RTT) */
    uint64_t bytes_sent_total;      /* Total bytes sent */
    uint32_t retrans_count;         /* Retransmission count */
    uint32_t ack_count;             /* ACK packets received */
    uint32_t unacked_bytes;         /* Current unacknowledged bytes */
    uint32_t optimal_inflight;      /* Target inflight window */

    /* Queue metrics */
    uint32_t input_queue_bytes;
    uint32_t input_queue_packets;
    uint32_t input_send_q_bytes;
    uint32_t input_send_q_packets;
    uint32_t input_wait_ack_bytes;
    uint32_t input_wait_ack_packets;
    uint32_t ack_q_bytes;
    uint32_t ack_q_packets;
    uint32_t recv_q_bytes;
    uint32_t recv_q_packets;
    uint32_t output_queue_bytes;
    uint32_t output_queue_packets;

    /* Error counters */
    uint32_t reinit_count;
    uint32_t reset_count;
    uint32_t pkt_format_errors;

    uint8_t  links_count;           /* Number of active links */

    /* Timer flags (1 = active, 0 = NULL) */
    uint8_t  retrans_timer_active;
    uint8_t  ack_resp_timer_active;
    
    /* input_wait_ack queue state */
    uint8_t wait_ack_cb_suspended;
    uint8_t wait_ack_cb_set;
    uint8_t wait_ack_resume_timeout;
    
    /* Connection IDs */
    uint32_t next_tx_id;
    uint32_t last_rx_id;
    uint32_t last_delivered_id;
    uint32_t rx_ack_till;
    
    /* Normalizer */
    uint32_t norm_input_pkts;
    uint32_t norm_input_bytes;
    uint32_t norm_output_pkts;
    uint32_t norm_output_bytes;
    uint32_t norm_alloc_errors;
    uint32_t norm_logic_errors;
    uint16_t norm_frag_size;
    uint16_t norm_data_ptr;
    uint16_t norm_data_size;
    uint64_t norm_in_total_pkts;
    uint64_t norm_in_total_bytes;
    uint64_t norm_out_total_pkts;
    uint64_t norm_out_total_bytes;

    /* ACK debug counters */
    uint32_t cnt_ack_hit_inf;
    uint32_t cnt_ack_hit_sndq;
    uint32_t cnt_ack_miss;
    uint32_t cnt_link_wait;
    uint32_t rx_dup_count;
    uint32_t tx_dup_count;
    uint32_t tx_state;
    uint32_t debug[8];

    /* System resources */
    uint32_t active_timeouts;
    uint32_t busy_memory_blocks;
};

/* Link metrics */
struct etcpmon_link_metrics {
    uint8_t  local_link_id;         /* Local link identifier */
    uint8_t  status;                /* 1 = up, 0 = down */
    uint32_t encrypt_errors;        /* Encryption errors */
    uint32_t decrypt_errors;        /* Decryption errors */
    uint32_t send_errors;           /* Send errors */
    uint32_t recv_errors;           /* Receive errors */
    uint64_t total_encrypted;       /* Total bytes encrypted/sent */
    uint64_t total_decrypted;      /* Total bytes decrypted/received */
    uint32_t bandwidth;             /* Configured bandwidth (Kbps) */
    uint32_t nat_changes_count;     /* NAT address changes */
    uint32_t rtt_last;              /* Last RTT (0.1ms units) */
    uint32_t rtt_avg10;             /* Сейчас BBR min_rtt_us / 100 (0.1 мс), 0 без BBR; не среднее 10 RTT */
    uint32_t tt_last;               /* Last transmit time (0.1ms units) */

    /* Timer flags (1 = active, 0 = NULL) */
    uint8_t  init_timer_active;
    uint8_t  keepalive_timer_active;
    uint8_t  shaper_timer_active;
    
    /* Keepalive counters */
    uint32_t keepalive_sent;
    uint32_t keepalive_recv;
    
    /* Inflight counters */
    uint32_t inflight_bytes;
    uint32_t inflight_packets;
    uint32_t inflight_lim_bytes;

    /* BBR congestion control metrics */
    uint8_t  bbr_mode;            /* 0=STARTUP, 1=DRAIN, 2=PROBE_BW, 3=PROBE_RTT */
    uint8_t  bbr_cycle_idx;       /* 0=UP, 1=DOWN, 2=CRUISE, 3=REFILL */
    uint8_t  bbr_full_bw_reached; /* startup completed? 0/1 */
    uint8_t  bbr_loss_in_round;   /* loss in current round? 0/1 */
    uint32_t bbr_pacing_rate;     /* bytes/sec */
    uint32_t bbr_min_rtt_us;      /* min RTT (microseconds) */
    uint32_t bbr_bw_hi;           /* max bandwidth estimate (bytes/sec, converted from BW_UNIT scale) */
    uint32_t bbr_bw_lo;           /* conservative bandwidth estimate (bytes/sec) */
    uint32_t bbr_inflight_hi;     /* upper inflight bound (bytes) */
    uint32_t bbr_inflight_lo;     /* lower inflight bound (bytes) */
    uint16_t bbr_pacing_gain;     /* pacing gain (BBR_UNIT fixed-point: 710=2.77x, 192=0.75x) */
    uint16_t pad_bbr;             /* padding for 4-byte alignment */
};

/* TUN interface metrics */
struct etcpmon_tun_metrics {
    uint64_t bytes_read;            /* Bytes read from TUN */
    uint64_t bytes_written;         /* Bytes written to TUN */
    uint32_t packets_read;          /* Packets read from TUN */
    uint32_t packets_written;       /* Packets written to TUN */
    uint32_t read_errors;           /* Read error count */
    uint32_t write_errors;          /* Write error count */
    
    /* Routing statistics */
    uint64_t routed_packets;       /* Successfully routed packets */
    uint64_t dropped_packets;       /* Dropped packets */
    
    /* TUN queues */
    uint32_t tun_in_q_packets;     /* Packets in TUN input queue */
    uint32_t tun_in_q_bytes;       /* Bytes in TUN input queue */
    uint32_t tun_out_q_packets;    /* Packets in TUN output queue */
    uint32_t tun_out_q_bytes;      /* Bytes in TUN output queue */
    
    /* Routing table */
    uint32_t rt_count;             /* Total routes in table */
    uint32_t rt_local;             /* Local routes */
    uint32_t rt_learned;           /* Learned routes (BGP) */
    uint32_t rt_bgp_senders;       /* BGP senders_list count (TOPO_BGP) */
    uint32_t rt_bgp_nodes;         /* BGP nodes count (TOPO_BGP) */
};

/* Router congestion metrics — aggregated over all router_conns */
struct etcpmon_router_metrics {
    uint32_t total_conns;
    uint32_t total_inflight; /* Сумма числа записей inflight_q, не байтов */
    uint32_t total_send_q;   /* Сумма числа записей send_q */
    uint32_t total_recv_q;   /* Сумма числа записей recv_q */
    uint32_t pkts_sent;
    uint32_t pkts_send_err;
    uint32_t pkts_rcvd;
    uint32_t ack_sent;
    uint32_t ack_recv;
    uint32_t dup_dropped;
    uint32_t oob_dropped;
    uint32_t stale_ack;
    uint32_t sign_fail;
    uint32_t rtt_last;       /* max RTT across all rconns (0.1ms) */
    uint32_t rtt_avg;        /* avg RTT across all rconns with rtt>0 (0.1ms) */
    uint32_t jitter;         /* max jitter across all rconns (0.1ms) */
    uint32_t minrtt;         /* min minRTT across all rconns (0.1ms) */
};

struct etcpmon_rsp_metrics {
    struct etcpmon_etcp_metrics   etcp;           /* ETCP connection metrics */
    struct etcpmon_tun_metrics    tun;            /* TUN interface metrics */
    struct etcpmon_router_metrics router;         /* Router congestion metrics (aggregated) */
    /* Followed by etcp.links_count * struct etcpmon_link_metrics */
};

/* RSP_DEBUG_CONFIG: Server -> Client debug levels */
struct etcpmon_rsp_debug_config {
    uint8_t global_level;           /* Global debug level (0=NONE..5=TRACE) */
    uint8_t category_count;         /* Number of categories */
    /* Followed by: char names_csv[]; NUL-terminated CSV категорий 1..N из debug_config */
    /* Followed by: uint8_t levels[category_count];  per-category levels */
};

/* CMD_SET_DEBUG_CONFIG: Client -> Server set debug levels */
struct etcpmon_cmd_set_debug {
    uint8_t global_level;           /* Global debug level (0=NONE..5=TRACE) */
    uint8_t category_count;         /* Number of categories */
    uint8_t levels[ETCPMON_MAX_DEBUG_CATEGORIES]; /* Actual length = category_count */
};

/* RSP_ACTION_RESULT: Text result of an action command */
struct etcpmon_rsp_action_result {
    uint8_t code;               /* 0=success, 1=error */
    /* Followed by null-terminated UTF-8 text string */
};

/* RSP_ERROR: Error response */
struct etcpmon_rsp_error {
    uint8_t error_code;             /* Error code */
    /* Followed by null-terminated error message string */
};

#pragma pack(pop)

/* ============================================================================
 * Helper Macros
 * ============================================================================ */

/* Calculate size of connection list response */
#define ETCPMON_CONN_LIST_SIZE(count) \
    (sizeof(struct etcpmon_msg_header) + sizeof(struct etcpmon_rsp_conn_list) + \
     (count) * sizeof(struct etcpmon_conn_info))

/* Calculate size of socket list response */
#define ETCPMON_SOCKET_LIST_SIZE(count) \
    (sizeof(struct etcpmon_msg_header) + sizeof(struct etcpmon_rsp_socket_list) + \
     (count) * sizeof(struct etcpmon_socket_info))

/* Calculate size of metrics response */
#define ETCPMON_METRICS_SIZE(links_count) \
    (sizeof(struct etcpmon_msg_header) + sizeof(struct etcpmon_rsp_metrics) + \
     (links_count) * sizeof(struct etcpmon_link_metrics))

/* Calculate size of error response */
#define ETCPMON_ERROR_SIZE(msg_len) \
    (sizeof(struct etcpmon_msg_header) + sizeof(struct etcpmon_rsp_error) + (msg_len) + 1)

/* Calculate size of action result response */
#define ETCPMON_ACTION_RESULT_SIZE(text_len) \
    (sizeof(struct etcpmon_msg_header) + sizeof(struct etcpmon_rsp_action_result) + (text_len) + 1)

/* ============================================================================
 * Utility Functions (optional, for both client and server)
 * ============================================================================ */

/* Заполнить hdr из размера payload, типа и seq. Границы не проверяет:
 * вызывающий код обязан обеспечить payload_size <= ETCPMON_MAX_MSG_SIZE - sizeof(*hdr). */
static inline void etcpmon_build_header(struct etcpmon_msg_header* hdr, 
                                         uint16_t payload_size, 
                                         uint8_t msg_type,
                                         uint8_t seq_id) {
    hdr->size = sizeof(struct etcpmon_msg_header) + payload_size;
    hdr->type = msg_type;
    hdr->seq_id = seq_id;
}

/* Проверить только диапазон hdr->size; 0 = допустим, -1 = ошибка.
 * hdr должен указывать на полный заголовок; наличие payload/тип/seq эта функция не проверяет. */
static inline int etcpmon_validate_header(const struct etcpmon_msg_header* hdr) {
    if (hdr->size < sizeof(struct etcpmon_msg_header) || 
        hdr->size > ETCPMON_MAX_MSG_SIZE) {
        return -1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* ETCPMON_PROTOCOL_H */
