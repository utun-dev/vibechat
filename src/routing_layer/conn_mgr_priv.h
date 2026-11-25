/* conn_mgr_priv — внутренние состояния, wire-команды и общие операции трёх частей conn_mgr.
 * core управляет handles и попытками, indirect — согласованием посредников, monitor — оценкой/обновлением путей.
 * Все структуры принадлежат менеджеру topo-группы и используются в его uasync-потоке.
 * Сервисы подключаются через conn_mgr.h; поля ENTRY/CANDIDATE не являются внешним API. */
#ifndef CONN_MGR_PRIV_H
#define CONN_MGR_PRIV_H

#include "conn_mgr.h"
#include <stdint.h>
#include "../lib/ll_queue.h"
#include "etcp_api.h"
#include "secure_channel.h"
#include "../transport_layer/node_conn_direct.h"
#include "topo_node.h"

struct UTUN_INSTANCE;
struct ETCP_CONN;
struct ETCP_LINK;
struct ETCP_SOCKET;
struct ETCP_ROUTER_CONN;
struct TOPO_GROUP_NODE;
struct TOPO_GROUP;
struct stcp_link;
struct stcp_client;
struct topo_node_load;

/* ═══════ константы ═══════ */

#define CONN_MGR_MAX_CANDIDATES        3
#define CONN_MGR_CANDIDATE_STALE_TB 300000
#define CONN_MGR_CANDIDATE_PING_TB  20000
#define CONN_MGR_BG_PING_INTERVAL_TB 1000
#define CONN_MGR_BG_PING_CYCLE_MIN_TB 600000
#define CONN_MGR_REVERSE_DELAY_MS       500
#define CONN_MGR_INDIRECT_DELAY_MS     1000
#define CONN_MGR_CONNECT_REVERSE_TIMEOUT_MS 3000
#define CONN_MGR_INTERM_EXCHANGE_TIMEOUT_MS 15000
#define CONN_MGR_REVERSE_RETRANSMIT_MS      1000
#define CONN_MGR_EXCHANGE_RETRANSMIT_MS     1000
#define CONN_MGR_RETRY_MIN_MS              1000
#define CONN_MGR_RETRY_MAX_MS             15000
#define CACHE_MS 20000  /* свежесть RTT-пробы 20с; поля *_tb и RTT используют 0.1мс */
#define CONN_MGR_MAX_INTERMEDIARIES 3

/* ═══════ состояния ═══════ */

enum CONN_MGR_STATE {
    CONN_MGR_STATE_DISCONNECTED = 0,
    CONN_MGR_STATE_CONNECTING   = 1,
    CONN_MGR_STATE_CONNECTED    = 2,
};

enum CM_TRY_STATE {
    CM_TRY_NONE    = 0,
    CM_TRY_PENDING = 1,
    CM_TRY_OK      = 2,
    CM_TRY_FAILED  = 3,
};

/* ═══════ протокол CONN_MGR (ETCP_RT_ID_CONN_MGR) ═══════ */

enum {
    CM_SUBCMD_DIRECT_REQ          = 0x01,
    CM_SUBCMD_INTERM_EXCHANGE_REQ = 0x03,
    CM_SUBCMD_INTERM_EXCHANGE_RESP= 0x04,
    CM_SUBCMD_INTERM_SELECTED     = 0x05,
    CM_SUBCMD_INTERM_ACK          = 0x07,
};

/* RTT — 0.1мс; 0/FFFF не участвуют в согласовании. */
struct CONN_MGR_CANDIDATE { uint8_t ed[SC_PUBKEY_SIZE]; uint16_t rtt; } __attribute__((packed));

/* Полные фиксированные структуры, включая неиспользованные слоты массивов.
 * Native endian, cmd=ETCP_RT_ID_CONN_MGR; request_id ненулевой.
 * DIRECT_REQ=6B, EXCHANGE_REQ=109B, EXCHANGE_RESP=212B, SELECTED/ACK=109B. */
#pragma pack(push, 1)
struct CM_DIRECT_REQ    { uint8_t cmd, subcmd; uint32_t request_id; };
struct CM_EXCHANGE_REQ  { uint8_t cmd, subcmd; uint32_t request_id; uint8_t candidate_count; struct CONN_MGR_CANDIDATE candidates[CONN_MGR_MAX_CANDIDATES]; };
struct CM_EXCHANGE_RESP { uint8_t cmd, subcmd; uint32_t request_id; uint8_t my_count, your_count; struct CONN_MGR_CANDIDATE my_candidates[CONN_MGR_MAX_CANDIDATES], your_candidates[CONN_MGR_MAX_CANDIDATES]; };
struct CM_INTERM_SEL    { uint8_t cmd, subcmd; uint32_t request_id; uint8_t count; struct CONN_MGR_CANDIDATE selected[CONN_MGR_MAX_INTERMEDIARIES]; };
#pragma pack(pop)

#define CM_DIRECT_REQ_H_SIZE     (sizeof(struct CM_DIRECT_REQ))
#define CM_EXCHANGE_REQ_SIZE     (sizeof(struct CM_EXCHANGE_REQ))
#define CM_EXCHANGE_RESP_SIZE    (sizeof(struct CM_EXCHANGE_RESP))
#define CM_INTERM_SEL_SIZE       (sizeof(struct CM_INTERM_SEL))

/* ═══════ внутренние структуры ═══════ */

struct CONN_MGR_HANDLE {
    uint8_t ed[32]; uint64_t group_id; conn_mgr_cb_t cb; void* cb_arg; /* исходная цель/группа, заимствованный callback */
    struct CONN_MGR_ENTRY* entry; struct CONN_MGR_HANDLE* next;
    struct CONN_MGR* mgr; /* владелец памяти до destroy; NULL у отсоединённого после destroy handle */
    struct CONN_MGR_HANDLE* owner_next; /* список всех handles mgr либо список закрытых до безопасного GC */
    struct UASYNC* ua;
    uint8_t closed; /* close отменяет callbacks; менеджер освобождает память вне текущего стека */
    void* deliver_up_token;  /* токен отложенного cm_deliver_up_cb (uasync_call_soon), NULL если не постился/уже сработал */
};

struct CONN_MGR_ENTRY {
    struct ll_entry ll;              /* ll_queue entry — первый элемент */
    uint8_t ed[32];                  /* Хеш-ключ записи, полный Ed25519. */
    uint8_t x[32];                   /* Производный транспортный ключ. */
    uint64_t node_id;                /* Производный номер для загрузчика снимков и диагностики. */
    uint8_t state, conn_type;        /* CONN_MGR_STATE / CONN_TYPE; рабочая доставка независимо от проб */
    uint64_t last_traffic_tb;        /* последняя принятая CM-команда при CONNECTED; не idle-дедлайн */
    struct CONN_MGR_HANDLE* handles; struct NODE_CONN_DIRECT* ncd_handle; /* владельцы запроса; отдельный общий прямой транспорт CM */
    struct topo_node_load* node_load; /* Чтение отсутствующего узла; до done транспортные попытки/таймеры не запускаются. */
    uint8_t intermediaries[CONN_MGR_MAX_INTERMEDIARIES][SC_PUBKEY_SIZE]; uint8_t intermediariy_count; /* подтверждённый резерв, актуальность проверяется перед выбором */
    uint8_t local_scan_state, main_connect_state, db_loaded; /* CM_TRY / CM_TRY / временный узел из SQLite */
    uint8_t reverse_started, indirect_started, indirect_confirmed, direct_up, timeout_notified, ever_up;
    uint8_t attempt_active;          /* исходящий цикл; не признак CONNECTED */
    uint16_t retry_delay_ms;         /* пауза между циклами, 1000..15000мс */
    void* timer; void* refresh_token; /* ближайший шаг/срок; отложенный пересчёт после маршрутов */
    uint64_t start_tb, reverse_start_tb, indirect_start_tb, deadline_tb;
    uint64_t initial_deadline_tb, retry_tb, node_timestamp, peer_exchange_deadline_tb;
    uint64_t reverse_next_tb, reverse_deadline_tb, indirect_next_tb;
    uint32_t reverse_request_id, exchange_request_id, peer_exchange_request_id, peer_reverse_request_id; /* независимые исходящие/входящие id */
    struct CM_INTERM_SEL selection; /* Исходящий выбор до подтверждения пира. */
    struct CONN_MGR* mgr;
    struct CONN_MGR_ENTRY* retired_next; /* уже снятая с entries запись до безопасного GC */
};

struct CONN_MGR {
    struct UTUN_INSTANCE* instance; struct TOPO_GROUP* group;
    struct ll_queue* entries;
    struct CONN_MGR_HANDLE *handles, *retired_handles;
    struct CONN_MGR_ENTRY* retired_entries; /* сборка в фоновых callbacks/destroy не требует выделений памяти */
    void *bg_ping_timer, *candidate_ping_timer;
    void *bg_ping_wait, *candidate_ping_wait;  /* standby_wait handle (Android) */
    size_t bg_ping_cursor; uint64_t bg_ping_cycle_start_tb; /* один узел за тик; полный цикл не чаще 60с */
    struct CONN_MGR_CANDIDATE best_candidates[CONN_MGR_MAX_CANDIDATES]; uint8_t best_candidate_count;
    struct stcp_client* cand_tcp_pings[CONN_MGR_MAX_CANDIDATES]; /* влётные candidate TCP-пинги (для отмены при destroy) */
    uint32_t next_request_id; uint8_t initialized;
};

/* ═══════ декларации внутренних функций ═══════ */

/* core — публичные (в priv.h, не в conn_mgr.h) */
int  cm_open(struct CONN_MGR* mgr, const uint8_t ed[32],
             conn_mgr_cb_t cb, void* cb_arg, struct CONN_MGR_HANDLE** out_handle);

/* core */
struct CONN_MGR_ENTRY*  cm_find_entry(struct CONN_MGR* mgr, const uint8_t ed[32]);
struct CONN_MGR_ENTRY*  cm_ensure_entry(struct CONN_MGR* mgr, const uint8_t ed[32]);
void cm_entry_cleanup(struct CONN_MGR_ENTRY* entry);
void cm_collect(struct CONN_MGR* mgr); /* только вне вложенных CM-вызовов: начало фонового callback либо destroy */
void cm_cleanup_db_node(struct CONN_MGR_ENTRY* entry);
void cm_deliver_event(struct CONN_MGR_ENTRY* entry, enum conn_mgr_event event);
void cm_update_nodeinfo(struct CONN_MGR_ENTRY* entry);
void cm_ncd_callback(struct NODE_CONN_DIRECT* ncd_h, enum ncd_event ncd_ev, void* arg);
void cm_start_phase_direct(struct CONN_MGR_ENTRY* entry);
void cm_start_phase_reverse(struct CONN_MGR_ENTRY* entry);
void cm_start_phase_indirect(struct CONN_MGR_ENTRY* entry);
void cm_start_local_scan(struct CONN_MGR_ENTRY* entry);
void cm_handle_direct_req(const uint8_t src[32], struct CONN_MGR* mgr, const uint8_t* data, size_t len);
void cm_finalize_indirect(struct CONN_MGR_ENTRY* entry);
void cm_refresh_path(struct CONN_MGR_ENTRY* entry);
void cm_arm_timer(struct CONN_MGR_ENTRY* entry);
int cm_send_control(struct CONN_MGR* mgr, const uint8_t ed[32], const void* packet, size_t len);
struct TOPO_NODE* cm_peer_snapshot(struct CONN_MGR* mgr, const uint8_t ed[32]);
uint16_t cm_candidate_rtt(struct CONN_MGR* mgr, const uint8_t ed[SC_PUBKEY_SIZE]);
struct ETCP_CONN* cm_direct_conn_ed(struct CONN_MGR* mgr, const uint8_t ed[SC_PUBKEY_SIZE]);
int  cm_has_direct_ip(struct CONN_MGR* mgr, struct TOPO_GROUP_NODE* nq);
uint8_t cm_classify_v6_addr(const uint8_t addr[16]);

#define CM_V6_LL  0
#define CM_V6_LOC 1
#define CM_V6_DIR 2
#define CM_V6_OTH 3

/* indirect */
void cm_handle_interm_exchange_req(const uint8_t src[32], struct CONN_MGR* mgr, struct CM_EXCHANGE_REQ* req);
void cm_handle_interm_exchange_resp(struct CONN_MGR* mgr, const uint8_t src[32], const uint8_t* data, size_t len);
void cm_handle_interm_selected(struct CONN_MGR* mgr, const uint8_t src[32], const uint8_t* data, size_t len);
void cm_handle_interm_ack(struct CONN_MGR* mgr, const uint8_t src[32], const uint8_t* data, size_t len);

/* monitor */
void cm_bg_ping_timer_cb(void* arg);
void cm_candidate_ping_timer_cb(void* arg);
uint64_t cm_get_node_max_probe_time(struct TOPO_GROUP_NODE* nq);
int  cm_is_rtt_fresh(struct TOPO_GROUP_NODE* nq, uint64_t now_tb);

#endif
