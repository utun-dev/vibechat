// etcp.c - ETCP Protocol Implementation (refactored and expanded based on etcp_protocol.txt)

#include "etcp_link_io.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_profile.h"
#include "etcp_session.h"
#include "node_conn_direct.h"
#include "etcp_debug.h"
#include "call_ring.h"
#include "etcp_loadbalancer.h"
#include "etcp_router.h"
#include "routing.h"
#include "topo_group.h"
#include "topo_node_rtt.h"
#include "nat_detection.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "crc32.h"  // For potential hashing, though not used yet.
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>  // For bandwidth calcs
#include <limits.h>  // For UINT16_MAX
#include "../lib/mem.h"
#include "../lib/strbuf.h"
#include "../lib/memory_pool.h"

// Enable comprehensive debug output for ETCP module
#define DEBUG_CATEGORY_ETCP_DETAILED 1

// Constants from spec (adjusted for completeness)
#define MAX_INFLIGHT_BYTES 65536  // Initial window
#define BURST_DELAY_FACTOR 4      // Delay before burst
#define BURST_SIZE 5              // Packets in burst (1 delayed + 4 burst)
#define RTT_HISTORY_SIZE 10       // For jitter calc
#define MAX_PENDING 32            // For ACKs/retrans (arbitrary; adjust)
#define SECTION_HEADER_SIZE 3     // type(1) + len(2)

// Container-of macro for getting struct from data pointer
//#define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

// Forward declarations
static void input_queue_cb(struct ll_queue* q, void* arg);
static void etcp_link_ready_callback(struct ETCP_CONN* etcp);
static void input_send_q_cb(struct ll_queue* q, void* arg);
static void etcp_conn_process_send_queue(struct ETCP_CONN* etcp);
static void etcp_send_timer_cb(void* arg);
static void etcp_connection_free_deferred(void* arg);
static void etcp_connection_close_deferred(void* arg);
static void ack_window_arm(struct ETCP_CONN* conn);
void etcp_fire_conn_status(struct ETCP_CONN* etcp, int status);
struct ETCP_DGRAM* etcp_request_pkt(struct ETCP_CONN* etcp);

/* Очереди и сетевые события только планируют проход: синхронные callbacks не входят в отправитель повторно. */
static void etcp_schedule_send(struct ETCP_CONN* conn, int delay_tb) {
    if (!conn || conn->close_requested || conn->state == 2 || conn->tx_token) return;
    if (!conn->initialized || conn->session_required || conn->reinit_pending) return;
    if (conn->tx_running && !delay_tb) return;
    int work = conn->input_send_q->count || conn->input_queue->count || conn->ack_till_deadline_us || conn->ack_request_pending;
    for (struct ETCP_LINK* link = conn->links; !work && link; link = link->next)
        work = link->burst_active || link->ack_reply_pending || (link->ack_q && link->ack_q->count);
    if (!work) return;
    conn->tx_delayed = delay_tb != 0;
    conn->tx_token = delay_tb ? uasync_set_timeout(conn->instance->ua, delay_tb, conn, etcp_send_timer_cb, "etcp_tx") :
        uasync_call_soon(conn->instance->ua, conn, etcp_send_timer_cb);
    if (!conn->tx_token) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot schedule sender delay=%d", conn->log_name, delay_tb);
        // Вызывающий может ещё собирать линк/менять очередь: ресурсы освобождаются после выхода из его стека.
        conn->close_requested = 1;
        conn->close_token = uasync_call_soon(conn->instance->ua, conn, etcp_connection_close_deferred);
        if (!conn->close_token) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot defer mandatory close after TX timer failure", conn->log_name);
            abort();
        }
    }
}

void etcp_conn_wake_sender(struct ETCP_CONN* conn) { etcp_schedule_send(conn, 0); }

/* Обычный запуск и OOM retry имеют разные операции отмены, но одновременно существует только один token. */
static void etcp_cancel_send(struct ETCP_CONN* conn) {
    if (!conn->tx_token) return;
    if (conn->tx_delayed) uasync_cancel_timeout(conn->instance->ua, conn->tx_token);
    else uasync_call_soon_cancel(conn->instance->ua, conn->tx_token);
    conn->tx_token = NULL;
}


static void drain_and_free_queue(struct ll_queue** q) {
    if (!*q) return;
    queue_clear(*q);
    queue_free(*q);
    *q = NULL;
}


uint16_t get_current_timestamp() {
    return (uint16_t)get_time_tb();
}

// Timestamp diff (with wrap-around)
static uint16_t timestamp_diff(uint16_t t1, uint16_t t2) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (t1 >= t2) {
        return t1 - t2;
    }
    return (UINT16_MAX - t2) + t1 + 1;
}

static const char EMPTY_NAME[] = "";

// Create new ETCP connection
// Таймаут DOWN удаляет только транспорт без локальных NCD владельцев.
static void etcp_down_timeout_cb(void* arg) {
    struct ETCP_CONN* conn = arg;
    conn->down_timer = NULL;
    if (conn->close_requested || conn->state == 2) return;
    if (conn->links_up) {
        DEBUG_WARN(DEBUG_CATEGORY_CONNECTION, "[%s] DOWN timeout ignored: transport is UP", conn->log_name);
        return;
    }
    int handles = node_conn_direct_handle_count(conn);
    if (handles) {
        DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] DOWN timeout: retaining transport peer=%016llx handles=%d refs=%d",
                   conn->log_name, (unsigned long long)conn->peer_node_id, handles, conn->ref_count);
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] DOWN timeout: deleting transport peer=%016llx refs=%d timeout_ms=%d",
               conn->log_name, (unsigned long long)conn->peer_node_id, conn->ref_count, ETCP_DOWN_TIMEOUT_TB / 10);
    etcp_connection_close(conn);
}

// Повторный DOWN не продлевает ожидание; первое подключение тоже имеет конечный срок.
int etcp_conn_schedule_down_close(struct ETCP_CONN* conn) {
    if (conn->down_timer || conn->links_up || conn->close_requested || conn->state == 2) return 0;
    uint64_t now = get_time_tb();
    if (!conn->down_deadline_tb) conn->down_deadline_tb = now + ETCP_DOWN_TIMEOUT_TB;
    if (conn->down_deadline_tb <= now && node_conn_direct_handle_count(conn)) return 0;
    int remaining = conn->down_deadline_tb > now ? (int)(conn->down_deadline_tb - now) : 1;
    conn->down_timer = uasync_set_timeout(conn->instance->ua, remaining, conn, etcp_down_timeout_cb, "etcp_down");
    if (!conn->down_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "[%s] cannot schedule DOWN timeout", conn->log_name);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "[%s] DOWN timer started remaining_tb=%d handles=%d refs=%d",
                conn->log_name, remaining, node_conn_direct_handle_count(conn), conn->ref_count);
    return 0;
}

// UP и окончательное закрытие отменяют отложенное удаление.
static void etcp_down_timer_cancel(struct ETCP_CONN* conn) {
    conn->down_deadline_tb = 0;
    if (!conn->down_timer) return;
    uasync_cancel_timeout(conn->instance->ua, conn->down_timer); conn->down_timer = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "[%s] DOWN timer cancelled up=%u closing=%u",
                conn->log_name, conn->links_up, conn->close_requested);
}

struct ETCP_CONN* etcp_connection_create(struct UTUN_INSTANCE* instance, char* name) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!instance) return NULL;


    struct ETCP_CONN* etcp = u_calloc(1, sizeof(struct ETCP_CONN));
    if (!etcp) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "creating connection failed for instance %p", instance);
        return NULL;
        }

    etcp->instance = instance;
    etcp->input_queue = queue_new(instance->ua, 0, 0, 0, "ETCP input");  // No hash for input_queue
    queue_set_threshold(etcp->input_queue, 0, 0);  // Backpressure: ждать полного освобождения
    etcp->output_queue = queue_new(instance->ua, 0, 0, 0, "ETCP output"); // No hash for output_queue
    etcp->transit_queues = queue_new(instance->ua, TRANSIT_QUEUE_HASH, 0, 16, "transit_q_reg"); // Hash for transit queues
    etcp->input_send_q = queue_new(instance->ua, INFLIGHT_INITIAL_HASH_SIZE, 0, 4, "input_send_q"); // Hash for send_q
    etcp->input_wait_ack = queue_new(instance->ua, INFLIGHT_INITIAL_HASH_SIZE, 0, 4, "input_wait_ack"); // Hash for wait_ack
    etcp->recv_q = queue_new(instance->ua, INFLIGHT_INITIAL_HASH_SIZE, 0, 4, "recv_q"); // Hash for recv_q
    etcp->inflight_pool = memory_pool_init(sizeof(struct INFLIGHT_PACKET), "inflight_pool");
    etcp->attempt_pool = memory_pool_init(sizeof(struct ETCP_TX_ATTEMPT), "attempt_pool");
    etcp->io_pool = memory_pool_init(sizeof(struct ETCP_FRAGMENT), "io_pool");
    etcp->max_inflight = (uint32_t)instance->config->global.bbr_max_cwnd;
    if (instance->config->global.bbr_max_cwnd < INFLIGHT_LIM_MIN) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "ETCP config cwnd=%d raised to minimum %u bytes",
            instance->config->global.bbr_max_cwnd, INFLIGHT_LIM_MIN);
        etcp->max_inflight = INFLIGHT_LIM_MIN;
    }
    etcp->optimal_inflight=100000;
    etcp->initialized=0;
    etcp->links_up=0;
    etcp->setup_start_tb = get_time_tb();
    etcp->reset_done=0;
    etcp->callbacks_running=0;
    etcp->ref_count=0;
    etcp->last_rr_link=NULL;
    etcp->name = u_strdup(name);
    // Initialize log_name with local node_id (peer will be updated later when known)
    snprintf(etcp->log_name, sizeof(etcp->log_name), "%04X->???? [%s]", (uint16_t)instance->node_id, etcp->name);

    if (!etcp->input_queue || !etcp->output_queue || !etcp->transit_queues ||
        !etcp->input_send_q || !etcp->recv_q || !etcp->attempt_pool ||
        !etcp->input_wait_ack || !etcp->inflight_pool || !etcp->io_pool) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "error - closing - input:%p output:%p send:%p wait:%x pool:%p",
               etcp->input_queue, etcp->output_queue, etcp->input_send_q, etcp->input_wait_ack, etcp->inflight_pool);
        etcp_connection_close(etcp);
        return NULL;
    }


    // etcp->window_size = MAX_INFLIGHT_BYTES;  // Not used
    etcp->mtu = ETCP_RFC791_MIN_MTU;  // Default MTU per RFC 791
    etcp->next_tx_id = 1;
    do { random_bytes((uint8_t*)&etcp->reset_id, 8); } while (!etcp->reset_id);
    etcp->diagnostic_id = etcp->reset_id; // ID объекта сохраняется при последующих сменах эпохи.
    etcp->rtt_avg_10 = 10;  // Initial guess (1ms)
    etcp->rtt_history_idx = 0;
    memset(etcp->rtt_history, 0, sizeof(etcp->rtt_history));


    etcp->normalizer = pn_init(etcp);
    if (!etcp->normalizer) {
        etcp_connection_close(etcp);
        return NULL;
    }
    etcp->send_input_q = etcp->normalizer->input;

    // Set input queue callback
    queue_set_callback(etcp->input_queue, input_queue_cb, etcp);
    queue_set_callback(etcp->input_send_q, input_send_q_cb, etcp);

    etcp->link_ready_for_send_fn = etcp_link_ready_callback;

    // Add to instance's connections queue (peer_node_id=0 for pending, reindexed later)
    {
        struct ll_entry* qe = queue_entry_new(sizeof(struct conn_queue_entry));
        if (!qe) { etcp_connection_close(etcp); return NULL; }
        struct conn_queue_entry* ce = (struct conn_queue_entry*)qe->data;
        ce->peer_node_id = 0;
        ce->conn = etcp;
        memset(ce->x25519, 0, sizeof(ce->x25519));
        if (queue_data_put_with_index(instance->connections, qe) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "cannot register pending transport"); etcp_connection_close(etcp); return NULL;
        }
        etcp->conn_queue_entry = qe;
        etcp->conn_queue = instance->connections;
    }

    if (etcp_padding_start(&etcp->padding, instance->ua, etcp->log_name) < 0) {
        etcp_connection_close(etcp);
        return NULL;
    }

    if (etcp_conn_schedule_down_close(etcp) < 0) { etcp_connection_close(etcp); return NULL; }

    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] connection initialized. ETCP=%p mtu=%d, next_tx_id=%u state=pending",
                etcp->log_name, etcp, etcp->mtu, etcp->next_tx_id);

    /* Конструктор удерживает ресурсы до завершения уведомлений; отмена не возвращает закрытый объект. */
    etcp->callbacks_running++;
    instance->etcp_callbacks_depth++;
    struct etcp_inst_cbk_entry* cbe = instance->new_conn_cbks;
    while (cbe) {
        struct etcp_inst_cbk_entry* next = cbe->next;
        if (cbe->fn) cbe->fn(etcp, cbe->arg);
        if (etcp->close_requested) break;
        cbe = next;
    }
    if (!etcp->close_requested) etcp_fire_conn_status(etcp, ETCP_CONN_STATUS_NEW);
    etcp->callbacks_running--;
    instance->etcp_callbacks_depth--;
    etcp_instance_callbacks_collect(instance);
    if (etcp->close_requested) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] connection creation cancelled by callback", etcp->log_name);
        etcp_connection_close(etcp);
        return NULL;
    }

    return etcp;
}


void etcp_fire_conn_status(struct ETCP_CONN* etcp, int status) {
    if (!etcp || !etcp->instance) return;
    if (etcp->state == 2 && status != ETCP_CONN_STATUS_DELETE) return;  // при state==2 допустим только DELETE
    static const char* names[] = { "NEW", "UP", "DOWN", "DELETE", "REINIT" };
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "[%s] Connection status changed to %s", etcp->log_name,
               (status >= 0 && status < (int)(sizeof(names)/sizeof(names[0]))) ? names[status] : "?");
    etcp->callbacks_running++;
    struct UTUN_INSTANCE* inst = etcp->instance;
    inst->etcp_callbacks_depth++;
    struct etcp_status_cbk_entry* cbe = etcp->instance->conn_status_cbks;
    while (cbe) {
        struct etcp_status_cbk_entry* n = cbe->next;
        if (cbe->fn) cbe->fn(etcp, status, cbe->arg);
        if (etcp->close_requested && status != ETCP_CONN_STATUS_DOWN && status != ETCP_CONN_STATUS_DELETE) break;
        cbe = n;
    }
    etcp->callbacks_running--;
    inst->etcp_callbacks_depth--;
    etcp_instance_callbacks_collect(inst);
}

void etcp_cbk_fire(struct ETCP_CONN* conn, int event) {
    if (!conn) return;
    if (conn->state == 2 && event != ETCP_CBK_EVENT_DELETE) return;
    static const char* names[] = { "INIT", "REINIT", "UP", "DOWN", "NODE_CHANGED", "DELETE" };
    int idx = 0, e = event; while (e >>= 1) idx++;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] callback event: %s", conn->log_name, (idx >= 0 && idx < (int)(sizeof(names)/sizeof(names[0]))) ? names[idx] : "?");
    conn->callbacks_running++;
    struct etcp_cbk_entry* cbe = conn->cbks;
    while (cbe) {
        struct etcp_cbk_entry* n = cbe->next;
        if (cbe->fn && (cbe->event_mask & event)) cbe->fn(conn, event, cbe->arg);
        if (conn->close_requested && event != ETCP_CBK_EVENT_DOWN && event != ETCP_CBK_EVENT_DELETE) break;
        cbe = n;
    }
    conn->callbacks_running--;
    if (!conn->callbacks_running) {
        struct etcp_cbk_entry** pp = &conn->cbks;
        while (*pp) {
            struct etcp_cbk_entry* e = *pp;
            if (!e->fn) { *pp = e->next; u_free(e); } else pp = &e->next;
        }
    }
}

static void etcp_on_up(struct ETCP_CONN* etcp) {
    if (etcp->state == 2 || etcp->close_requested) return;
    etcp_down_timer_cancel(etcp);
    int total_links = 0; for (struct ETCP_LINK* l = etcp->links; l; l = l->next) total_links++;
    uint64_t elapsed_ms = (get_time_tb() - etcp->setup_start_tb) / 10;
    struct strbuf sb = strbuf_new(); int any = 0;
    for (struct ETCP_LINK* l = etcp->links; l; l = l->next) {
        if (l->link_status) {
            if (any) strbuf_adds(&sb, ", "); any = 1;
            strbuf_adds(&sb, sockaddr_storage_to_str(&l->remote_addr).str);
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] Connection UP (%d/%d links, mtu=%d, reinit=%u)%s%s",
               etcp->log_name, etcp->links_up, total_links, etcp->mtu, etcp->reinit_count,
               any > 0 ? " — up: " : "", strbuf_str(&sb));
    strbuf_free(&sb);
    (void)elapsed_ms;
    etcp_cbk_fire(etcp, ETCP_CBK_EVENT_UP);
    if (etcp->close_requested || !etcp->links_up) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] UP publication stopped after callback: closing=%u links_up=%u",
            etcp->log_name, etcp->close_requested, etcp->links_up);
        return;
    }
    etcp_fire_conn_status(etcp, ETCP_CONN_STATUS_UP);
}

static void etcp_on_down(struct ETCP_CONN* etcp, struct ETCP_LINK* down_link) {
    struct strbuf sb = strbuf_new(); int any = 0;
    for (struct ETCP_LINK* l = etcp->links; l; l = l->next) {
        if (l == down_link) continue;
        if (any) strbuf_adds(&sb, ", "); any = 1;
        strbuf_adds(&sb, sockaddr_storage_to_str(&l->remote_addr).str);
    }
    if (down_link)
        DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] Connection DOWN — link %s lost%s%s",
                   etcp->log_name, sockaddr_storage_to_str(&down_link->remote_addr).str,
                   any > 0 ? ", remaining: " : "", strbuf_str(&sb));
    else
        DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] Connection DOWN — closing, links: %s", etcp->log_name, any ? strbuf_str(&sb) : "(none)");
    strbuf_free(&sb);
    etcp_cbk_fire(etcp, ETCP_CBK_EVENT_DOWN);
    if (!etcp->close_requested) etcp_fire_conn_status(etcp, ETCP_CONN_STATUS_DOWN);
}


// Phase 2 resources cleanup (callable both sync and async via call_soon)
static void etcp_connection_free_resources(struct ETCP_CONN* etcp) {
    if (!etcp) return;
    callring_put(CR_CFREE_RES, (uintptr_t)etcp, 0);
    DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "[%s] freeing resources phase 2 conn=%p peer=%016llx",
                etcp->log_name, (void*)etcp, (unsigned long long)etcp->peer_node_id);

    // Close links
    if (etcp->links) {
        while (etcp->links) etcp_link_close(etcp->links);
    }

    // Drain and free all queues
    drain_and_free_queue(&etcp->input_queue);
    drain_and_free_queue(&etcp->output_queue);
    drain_and_free_queue(&etcp->input_send_q);
    drain_and_free_queue(&etcp->input_wait_ack);
    drain_and_free_queue(&etcp->recv_q);

    // Free callback chain
    { struct etcp_cbk_entry* cbe = etcp->cbks; while (cbe) { struct etcp_cbk_entry* n = cbe->next; u_free(cbe); cbe = n; } etcp->cbks = NULL; }

    if (etcp->inflight_pool) { memory_pool_destroy(etcp->inflight_pool); etcp->inflight_pool = NULL; }
    if (etcp->attempt_pool) { memory_pool_destroy(etcp->attempt_pool); etcp->attempt_pool = NULL; }
    if (etcp->io_pool)       { memory_pool_destroy(etcp->io_pool); etcp->io_pool = NULL; }

    u_free(etcp->name);
    u_free(etcp);
}

static void etcp_connection_free_deferred(void* arg) {
    etcp_connection_free_resources((struct ETCP_CONN*)arg);
}

/* Отложенное закрытие conn: вызывается из etcp_connection_close, когда закрытие
 * запрошено внутри цепочки коллбэков (callbacks_running) — синхронное закрытие
 * в этом случае освободило бы conn/линки, которые коллбэк-цепочка ещё использует. */
static void etcp_connection_close_deferred(void* arg) {
    struct ETCP_CONN* conn = arg;
    conn->close_token = NULL;
    etcp_connection_close(conn);
}

// Единственная точка планирования free: release внутри DELETE не опережает phase 1.
static void etcp_connection_maybe_free(struct ETCP_CONN* conn) {
    if (!conn->delete_complete || conn->ref_count || conn->callbacks_running || conn->free_scheduled) return;
    conn->free_scheduled = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "[%s] final free scheduled", conn->log_name);
    uasync_call_soon(conn->instance->ua, conn, etcp_connection_free_deferred);
}

// Close connection: phase 1 detach + deferred phase 2 cleanup
void etcp_connection_close(struct ETCP_CONN* etcp) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!etcp) return;
    if (etcp->state == 2 || etcp->close_running) return;
    callring_put(CR_CCLOSE_IN, (uintptr_t)etcp, 0);

    etcp->close_requested = 1;
    etcp_cancel_send(etcp);
    etcp_down_timer_cancel(etcp);
    etcp_padding_stop(&etcp->padding);
    if (etcp->callbacks_running) {
        if (etcp->close_token) return;
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] close requested inside callback chain — deferring", etcp->log_name);
        etcp->close_token = uasync_call_soon(etcp->instance->ua, etcp, etcp_connection_close_deferred);
        if (!etcp->close_token) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot defer mandatory close", etcp->log_name);
            abort();
        }
        return;
    }

    etcp->close_running = 1;
    if (etcp->close_token) { uasync_call_soon_cancel(etcp->instance->ua, etcp->close_token); etcp->close_token = NULL; }
    etcp_session_cancel(etcp);
    // === PHASE 1: detach from external world ===

    /* Финальный DOWN если соединение ещё UP (переход UP→DOWN до state=2):
       строгая последовательность ... → UP → DOWN → DELETE, без дублей. */
    if (etcp->links_up != 0) {
        etcp->links_up = 0;
        etcp_cbk_fire(etcp, ETCP_CBK_EVENT_DOWN);
    }

    etcp->state = 2; // deleted — blocks ref_take and etcp_send immediately

    if (etcp->peer_sleep_phase) {
        etcp->peer_sleep_phase = 0;
        utun_fire_peer_sleep_cbk(etcp, 0);
    }
    if (etcp->peer_phase_timer) { uasync_cancel_timeout(etcp->instance->ua, etcp->peer_phase_timer); etcp->peer_phase_timer = NULL; }

    etcp_cbk_fire(etcp, ETCP_CBK_EVENT_DELETE);
    etcp_fire_conn_status(etcp, ETCP_CONN_STATUS_DELETE);
    DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "[%s] close: state=2 conn=%p peer=%016llx (DELETE fired)", etcp->log_name, (void*)etcp, (unsigned long long)etcp->peer_node_id);

    // Cancel active timers
    if (etcp->ack_resp_timer) { uasync_cancel_timeout(etcp->instance->ua, etcp->ack_resp_timer); etcp->ack_resp_timer = NULL; }
    etcp->ack_resp_deadline_us = etcp->ack_till_deadline_us = 0;
    if (etcp->ack_window_timer) { uasync_cancel_timeout(etcp->instance->ua, etcp->ack_window_timer); etcp->ack_window_timer = NULL; }

    routing_del_conn(etcp);
    etcp_router_transit_queues_destroy(etcp);
    etcp_router_conn_destroyed(etcp);

    if (etcp->normalizer) { pn_deinit((struct PKTNORM*)etcp->normalizer); etcp->normalizer = NULL; etcp->send_input_q = NULL; }

    // Close links (detach from socket scan queues)
    if (etcp->links) {
        /* Callback закрытия одного пути может удалить соседний: следующий берём заново. */
        while (etcp->links) etcp_link_close(etcp->links);
    }

    // Remove from instance queue
    if (etcp->conn_queue && etcp->conn_queue_entry) {
        queue_remove_data(etcp->conn_queue, etcp->conn_queue_entry);
        queue_entry_free(etcp->conn_queue_entry);
        etcp->conn_queue_entry = NULL;
        etcp->conn_queue = NULL;
    }

    // === PHASE 2: deferred resource cleanup (only if no outstanding refs) ===
    etcp->delete_complete = 1;
    etcp->close_running = 0;
    etcp_connection_maybe_free(etcp);
}

// Take a reference on connection. Returns -1 if already deleted (state==2).
int etcp_conn_ref_take(struct ETCP_CONN* conn) {
    if (!conn) return -1;
    if (conn->state == 2 || conn->close_requested) { DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] ref_take on deleted conn", conn->log_name); return -1; }
    conn->ref_count++;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] ref_take ref_count=%d", conn->log_name, conn->ref_count);
    return 0;
}

// Release a reference. If last ref and conn is deleted, schedule deferred free.
void etcp_conn_ref_free(struct ETCP_CONN* conn) {
    if (!conn) return;
    if (conn->ref_count <= 0) { DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] ref_free underflow ref_count=%d", conn->log_name, conn->ref_count); return; }
    conn->ref_count--;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] ref_free ref_count=%d state=%d", conn->log_name, conn->ref_count, conn->state);
    etcp_connection_maybe_free(conn);
}

// Reset connection
void etcp_conn_reset(struct ETCP_CONN* conn) {
    if (!conn || conn->state == 2) {
        if (conn) DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] conn_reset on deleted conn", conn->log_name);
        return;
    }
    DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] discard stream: unacked=%u send=%d wait=%d recv=%d output=%d",
        conn->log_name, conn->unacked_bytes, queue_entry_count(conn->input_send_q), queue_entry_count(conn->input_wait_ack),
        queue_entry_count(conn->recv_q), queue_entry_count(conn->output_queue));
    etcp_cancel_send(conn);
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) etcp_tx_link_reset(link, 0);
    etcp_cancel_send(conn);
    if (conn->ack_resp_timer) { uasync_cancel_timeout(conn->instance->ua, conn->ack_resp_timer); conn->ack_resp_timer = NULL; }
    conn->ack_resp_deadline_us = 0;
    if (conn->ack_window_timer) { uasync_cancel_timeout(conn->instance->ua, conn->ack_window_timer); conn->ack_window_timer = NULL; }
    queue_clear(conn->input_queue); queue_clear(conn->output_queue);
    queue_clear(conn->input_send_q); queue_clear(conn->input_wait_ack); queue_clear(conn->recv_q);
    if (conn->normalizer) pn_reset(conn->normalizer);
    conn->ack_till_deadline_us = conn->ack_request_pending = 0;
    conn->next_tx_id = 1; conn->last_rx_id = conn->last_delivered_id = conn->rx_ack_till = 0;
    conn->got_initial_pkt = 0;
    conn->unacked_bytes = conn->rtt_last = conn->rtt_avg_10 = conn->jitter = 0;
    conn->bytes_sent_total = conn->retransmissions_count = conn->ack_packets_count = 0;
    conn->diagnostic_ack_us = 0;
    conn->rx_dup_count = conn->tx_dup_count = 0;
    memset(conn->rtt_history, 0, sizeof(conn->rtt_history)); conn->rtt_history_idx = 0;
    conn->last_rr_link = NULL; conn->reset_count++;
    queue_resume_callback(conn->input_queue); queue_resume_callback(conn->output_queue);
    queue_resume_callback(conn->input_send_q); queue_resume_callback(conn->input_wait_ack); queue_resume_callback(conn->recv_q);
    conn->tx_state = ETCP_TX_STATE_DATA_WAIT;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] stream discarded without replay: reset_count=%u links_up=%u attempts=%u",
        conn->log_name, conn->reset_count, conn->links_up, conn->active_attempts);
}

void etcp_links_reset(struct ETCP_CONN* etcp) {// Если сбой в обмене или ребутнулась одна из сторон -> необходимо заново переинициализировать соединение
    if (!etcp || etcp->state == 2) return;
    // Сбрасываем initialized во всех линках
    struct ETCP_LINK* link = etcp->links;
    while (link) {
        link->initialized = 0;
        link = link->next;
    }
}

void etcp_conn_reinit(struct ETCP_CONN* etcp, const char* reason) {
    etcp_conn_fatal_reinit(etcp, reason);
}

// Фатальный ресет (normalizer desync / bad fragment size): меняем эпоху reset_id
void etcp_conn_fatal_reinit(struct ETCP_CONN* etcp, const char* reason) {
    if (!etcp || etcp->close_requested || etcp->state == 2) return;
    etcp->callbacks_running++;
    uint64_t new_id;
    do {
        if (random_bytes((uint8_t*)&new_id,8) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP,"[%s] reset epoch RNG failed; closing",etcp->log_name);
            etcp_connection_close(etcp); etcp->callbacks_running--; return;
        }
    } while (!new_id || new_id == etcp->reset_id);
    etcp_conn_reinit_id(etcp, reason, new_id);
    if (!etcp->close_requested) etcp_session_start(etcp);
    etcp->callbacks_running--;
}

void etcp_conn_reinit_id(struct ETCP_CONN* etcp, const char* reason, uint64_t reset_id) {// Если сбой в обмене или ребутнулась одна из сторон -> необходимо заново переинициализировать соединение
    if (!etcp) return;
    if (etcp->state == 2) { DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] conn_reinit on deleted conn", etcp->log_name); return; }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] REINIT: %s (init=%d links=%d reinit=%u tx=%d) rid=%016llx",
               etcp->log_name, reason, etcp->initialized, etcp->links_up, etcp->reinit_count, etcp->tx_state,
               (unsigned long long)reset_id);

    etcp->reset_id = reset_id;
    etcp->setup_start_tb = get_time_tb();
    etcp->reinit_count++;
    etcp->reinit_pending = 1;
    etcp->reset_done = 0;
    etcp->initialized = 0;// еще раз придёт conn_ready_callback
    etcp->callbacks_running++;

    // Отменяем висящие NAT-ping'и для этого соединения
    if (etcp->instance && etcp->instance->nat_det) {
        nat_detection_cancel_for_conn(etcp->instance->nat_det, etcp);
    }

    // Очистка не выделяет память; повторные передачи router остаются ответственностью его собственного inflight.
    etcp_conn_reset(etcp);
    if (!etcp->close_requested) etcp_router_pause_retrans_for_node(etcp->instance, etcp->peer_ed25519_pubkey);
    etcp->callbacks_running--;

    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "end");
}

// Применение чужого reset_id (эпохи ресета пира). Симметрично:
// если uid пира изменился — сбрасываем своё состояние (свой uid сохраняем).
void etcp_conn_apply_peer_reset_id(struct ETCP_CONN* conn, uint64_t peer_reset_id) {
    if (!conn || peer_reset_id == 0) return;  /* 0 = неизвестно (STCP-сервер ещё не создал conn) */
    if (peer_reset_id == conn->peer_reset_id) return;  // uid пира не изменился
    if (!conn->peer_reset_id) conn->peer_reset_id = peer_reset_id;
    else etcp_session_observe(conn, peer_reset_id); // INIT не доказывает свежесть новой эпохи

}

// внутренняя функция. Вызывается один раз когда первый линк готов.
void etcp_conn_ready(struct ETCP_CONN* conn) {
    if (!conn || conn->close_requested) return;
    if (conn->state == 2) { DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] conn_ready on deleted conn", conn->log_name); return; }
    if (conn->initialized || conn->session_required) return; // ready or awaiting authenticated confirmation

    conn->initialized = 1;
    conn->reset_done = 1;
    if (conn->tx_state == 0) { conn->tx_state = ETCP_TX_STATE_DATA_WAIT; }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] Connection ready", conn->log_name);

    // При асинхронном session handshake физический линк мог стать UP раньше готовности потока.
    for (struct ETCP_LINK* link = conn->links; link; link = link->next)
        if (link->initialized && link->link_status) conn->links_up = 1;
    etcp_conn_queue_set_ready(conn);
    if (conn->close_requested) return;

    // Реинит уже готового соединения (state!=0): etcp_conn_queue_set_ready не сработал
    // (ранний выход по state!=0), поэтому reinit_pending остаётся висеть и блокирует приём —
    // guard'ы в etcp_conn_input/etcp_output_try_assembly дропают все входящие пакеты.
    if (conn->reinit_pending) {
        conn->reinit_pending = 0;
        etcp_cbk_fire(conn, ETCP_CBK_EVENT_REINIT);
        if (conn->close_requested) return;
        etcp_fire_conn_status(conn, ETCP_CONN_STATUS_REINIT);
    }
    if (conn->normalizer && !conn->close_requested) queue_resume_callback(conn->normalizer->input);
    etcp_conn_wake_sender(conn);
}

// Все создатели транспорта публикуют peer до callbacks, сохраняя исходную запись реестра.
int etcp_connection_set_peer(struct ETCP_CONN* conn, const uint8_t* public_key) {
    if (!conn || !public_key || conn->links || conn->state || conn->close_requested || conn->peer_node_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "set_peer: expected an empty unconfigured connection"); return -1;
    }
    sc_context_t crypto;
    if (sc_init_ctx(&crypto, &conn->instance->my_keys) != SC_OK ||
        sc_set_peer_public_key(&crypto, public_key, SC_PEER_PUBKEY_BIN) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "set_peer: invalid peer key"); return -1;
    }
    uint64_t peer = sc_derive_node_id_from_pubkey(public_key);
    if (!peer || !memcmp(public_key, conn->instance->my_keys.public_key, SC_PUBKEY_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "set_peer: invalid/self identity"); return -1;
    }
    conn->crypto_ctx = crypto; conn->peer_node_id = peer;
    etcp_update_log_name(conn);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] peer key configured before attaching drivers", conn->log_name);
    return 0;
}

struct ETCP_CONN* instance_find_conn_by_key(struct UTUN_INSTANCE* inst, const uint8_t x[SC_PUBKEY_SIZE]) {
    if (!inst || !inst->connections || !x || !memcmp(x, (uint8_t[SC_PUBKEY_SIZE]){0}, SC_PUBKEY_SIZE)) return NULL;
    struct ll_entry* e = queue_find_data_by_index(inst->connections, x);
    return e ? ((struct conn_queue_entry*)e->data)->conn : NULL;
}

struct ETCP_CONN* instance_find_conn_by_ed(struct UTUN_INSTANCE* inst, const uint8_t ed[SC_PUBKEY_SIZE]) {
    uint8_t x[SC_PUBKEY_SIZE];
    if (!inst || !ed || sc_identity_public_x("ETCP peer lookup", ed, x) != SC_OK) return NULL;
    struct ETCP_CONN* conn = instance_find_conn_by_key(inst, x);
    return conn && !memcmp(conn->peer_ed25519_pubkey, ed, SC_PUBKEY_SIZE) ? conn : NULL;
}

int instance_lookup_conn(struct UTUN_INSTANCE* inst, uint64_t node_id, struct ETCP_CONN** result) {
    *result = NULL;
    if (!inst || !node_id) return 0;
    struct ETCP_CONN* found = NULL;
    for (struct ll_entry* e = inst->connections ? inst->connections->head : NULL; e; e = e->next) {
        const struct conn_queue_entry* peer = (const struct conn_queue_entry*)e->data;
        if (peer->peer_node_id != node_id) continue;
        if (!peer->conn) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "empty ETCP registry entry peer=%016llx", (unsigned long long)node_id); return -1;
        }
        if (found && found != peer->conn) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "ambiguous ETCP numeric peer=%016llx", (unsigned long long)node_id); return -2;
        }
        found = peer->conn;
    }
    *result = found;
    return found != NULL;
}

int etcp_conn_index_peer(struct ETCP_CONN* conn) {
    if (!conn || conn->close_requested || !conn->peer_node_id || !conn->conn_queue_entry ||
        conn->conn_queue != conn->instance->connections) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "cannot index peer: invalid/closing connection conn=%p", (void*)conn);
        return -1;
    }
    struct ll_entry *qe = conn->conn_queue_entry;
    struct conn_queue_entry *ce = (struct conn_queue_entry*)qe->data;
    const uint8_t* x = conn->crypto_ctx.peer_public_key;
    if (!memcmp(x, (uint8_t[SC_PUBKEY_SIZE]){0}, SC_PUBKEY_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] missing peer key for registry", conn->log_name); return -1;
    }
    struct ll_entry *existing = queue_find_data_by_index(conn->conn_queue, x);
    if (existing && existing != qe) {
        struct conn_queue_entry *other = (struct conn_queue_entry*)existing->data;
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] duplicate peer=%016llx conn=%p already indexed by conn=%p",
            conn->log_name, (unsigned long long)conn->peer_node_id, (void*)conn, (void*)other->conn);
        return -1;
    }
    if (ce->peer_node_id == conn->peer_node_id && !memcmp(ce->x25519, x, SC_PUBKEY_SIZE)) return 0;
    uint64_t old_id = ce->peer_node_id;
    if (queue_remove_data(conn->conn_queue, qe) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot remove registry entry=%p", conn->log_name, (void*)qe);
        return -1;
    }
    conn->conn_queue_entry = NULL; /* put забирает запись и при ошибке освобождает её. */
    ce->peer_node_id = conn->peer_node_id; memcpy(ce->x25519, x, SC_PUBKEY_SIZE);
    if (queue_data_put_with_index(conn->conn_queue, qe) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot index registry entry for peer=%016llx",
            conn->log_name, (unsigned long long)conn->peer_node_id);
        return -1;
    }
    conn->conn_queue_entry = qe;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] registry indexed: conn=%p entry=%p peer=%016llx->%016llx state=%u",
        conn->log_name, (void*)conn, (void*)qe, (unsigned long long)old_id, (unsigned long long)conn->peer_node_id, conn->state);
    return 0;
}

// Готовность потока публикуется отдельно от наличия известного пира в реестре.
void etcp_conn_queue_set_ready(struct ETCP_CONN* conn) {
    if (!conn || conn->state != 0) return;
    if (etcp_conn_index_peer(conn) < 0) { etcp_connection_close(conn); return; }
    conn->state = 1;

    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] moved to ready queue, state=%d peer_node_id=0x%llx",
               conn->log_name, conn->state, (unsigned long long)conn->peer_node_id);

    etcp_cbk_fire(conn, ETCP_CBK_EVENT_INIT);
    if (conn->close_requested) return;
    if (conn->reinit_pending) {
        conn->reinit_pending = 0; etcp_cbk_fire(conn, ETCP_CBK_EVENT_REINIT);
        if (conn->close_requested) return;
        etcp_fire_conn_status(conn, ETCP_CONN_STATUS_REINIT);
    }
    if (conn->links_up) etcp_on_up(conn);
}




// Update log_name when peer_node_id becomes known
void etcp_update_log_name(struct ETCP_CONN* etcp) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!etcp || !etcp->instance) return;
    uint16_t local_id = etcp->instance->node_id;
    uint16_t peer_id = etcp->peer_node_id;
    const char* name = etcp->name ? etcp->name : EMPTY_NAME;
    if (etcp->instance->topo_groups && peer_id) {
        struct TOPO_NODE* ni = topo_node_registry_find(etcp->instance->topo_groups, etcp->peer_node_id);
        if (ni && ni->node_name && ni->node_name[0]) name = ni->node_name;
    }
    snprintf(etcp->log_name, sizeof(etcp->log_name), "%04X->%04X [%s]", local_id, peer_id, name);
}


// ====================================================================== Отправка данных в etcp после нормализации

// Send data through ETCP connection
// Allocates memory from data_pool and places in input queue
// Returns: 0 on success, -1 on failure
int etcp_int_send(struct ETCP_CONN* etcp, const void* data, uint16_t len) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");

    if (!etcp || !data || len == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] invalid parameters (etcp=%p, data=%p, len=%zu)", etcp->log_name, etcp, data, len);
        return -1;
    }

    if (etcp->state == 2) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] int_send on deleted conn", etcp->log_name);
        return -1;
    }

    if (!etcp->input_queue) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] input_queue is NULL for etcp=%p", etcp->log_name, etcp);
        return -1;
    }

    // Check length against maximum packet size
    if (len > ETCP_MAX_PAYLOAD_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] packet too large (len=%zu, max=%d)", etcp->log_name, len, ETCP_MAX_PAYLOAD_SIZE);
        return -1;
    }

    // Allocate packet data from data_pool (following ETCP reception pattern)
    uint8_t* packet_data = memory_pool_alloc(etcp->instance->data_pool);
    if (!packet_data) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] failed to allocate packet data from data_pool", etcp->log_name);
        return -1;
    }

    // Copy user data to packet buffer
    memcpy(packet_data, data, len);

    struct ETCP_FRAGMENT* pkt = (struct ETCP_FRAGMENT*)queue_entry_new_from_pool(etcp->io_pool);
    if (!pkt) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] failed to allocate queue entry", etcp->log_name);
        memory_pool_free(etcp->instance->data_pool, packet_data);
        return -1;
    }

    pkt->seq = 0;  // Will be assigned by input_queue_cb
    pkt->timestamp = 0;  // Will be set by input_queue_cb
    pkt->ll.dgram      = packet_data;  // Point to data_pool allocation
    pkt->ll.len        = len; // размер packet_data
    pkt->ll.dgram_pool = etcp->instance->data_pool;
    pkt->ll.memlen     = etcp->instance->data_pool->object_size;

    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] created PACKET %p with data %p (len=%zu)", etcp->log_name, pkt, packet_data, len);

    // Add to input queue - input_queue_cb will process it
    // При провале queue_data_put сам освобождает entry+dgram, повторный free дал бы double-free.
    if (queue_data_put(etcp->input_queue, (struct ll_entry*)pkt) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] failed to add to input queue", etcp->log_name);
        return -1;
    }

    return 0;
}





static int input_queue_try_push(struct ETCP_CONN* etcp) {// подгрузить не более одного нового фрагмента
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (etcp->unacked_bytes >= etcp->optimal_inflight) return 0;
    if (etcp->input_queue->count > 0) { input_queue_cb(etcp->input_queue, etcp); return 1; }
    queue_resume_callback(etcp->input_queue);
    return 0;
}

static void input_queue_try_resume(struct ETCP_CONN* etcp) {// при ACK
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");

    if (etcp->unacked_bytes < etcp->optimal_inflight) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] resume input queue: unacked_bytes=%u input_len=%zu", etcp->log_name,
            etcp->unacked_bytes, etcp->input_queue->total_bytes);
        queue_resume_callback(etcp->input_queue); // освобождённый общий бюджет допускает новые DATA и при непустой send_q
    }
}

// Called from link-level etcp_link_update_inflight_lim() after inflight_lim_bytes changes.
// Recalculates connection-level optimal_inflight and requests one sender pass without changing UP/DOWN.
void etcp_conn_on_inflight_lim_changed(struct ETCP_CONN* etcp) {
    if (!etcp || etcp->state == 2) return;
    uint64_t sum = 0;
    for (struct ETCP_LINK* tl = etcp->links; tl; tl = tl->next) sum += tl->inflight_lim_bytes;
    etcp->optimal_inflight = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
    etcp_conn_wake_sender(etcp);
}

void etcp_stats(struct ETCP_CONN* etcp) {
    if (!etcp || !debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_ETCP)) return;

    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] stats for conn=%p:", etcp->log_name, etcp);

    // Queue statistics
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]   Queues:", etcp->log_name);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     input_queue:     %zu pkts, %zu bytes", etcp->log_name,
                queue_entry_count(etcp->input_queue), queue_total_bytes(etcp->input_queue));
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     input_send_q:    %zu pkts, %zu bytes", etcp->log_name,
                queue_entry_count(etcp->input_send_q), queue_total_bytes(etcp->input_send_q));
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     input_wait_ack:  %zu pkts, %zu bytes", etcp->log_name,
                queue_entry_count(etcp->input_wait_ack), queue_total_bytes(etcp->input_wait_ack));
    for (struct ETCP_LINK* link = etcp->links; link; link = link->next)
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] path=%u attempts=%d ACK_queued=%d", etcp->log_name,
            link->local_link_id, link->attempts ? link->attempts->count : 0, link->ack_q ? link->ack_q->count : 0);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     recv_q:          %zu pkts", etcp->log_name,
                queue_entry_count(etcp->recv_q));
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     output_queue:    %zu pkts, %zu bytes", etcp->log_name,
                queue_entry_count(etcp->output_queue), queue_total_bytes(etcp->output_queue));

    // RTT metrics
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]   RTT metrics:", etcp->log_name);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     rtt_last:        %u (0.1ms)", etcp->log_name, etcp->rtt_last);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     rtt_avg_10:      %u (0.1ms)", etcp->log_name, etcp->rtt_avg_10);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     jitter:          %u (0.1ms)", etcp->log_name, etcp->jitter);

    // Counters
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]   Counters:", etcp->log_name);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     bytes_sent_total:        %u", etcp->log_name, etcp->bytes_sent_total);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     retransmissions_count:   %u", etcp->log_name, etcp->retransmissions_count);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     ack_packets_count:       %u", etcp->log_name, etcp->ack_packets_count);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     unacked_bytes:           %u", etcp->log_name, etcp->unacked_bytes);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     rx_dup_count:            %u", etcp->log_name, etcp->rx_dup_count);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     tx_dup_count:            %u", etcp->log_name, etcp->tx_dup_count);

    // IDs
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]   IDs:", etcp->log_name);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     next_tx_id:       %u", etcp->log_name, etcp->next_tx_id);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     last_rx_id:       %u", etcp->log_name, etcp->last_rx_id);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     last_delivered_id:%u", etcp->log_name, etcp->last_delivered_id);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s]     rx_ack_till:      %u", etcp->log_name, etcp->rx_ack_till);
}

// Input callback for input_queue (добавление новых кодограмм в стек)
// input_queue -> input_send_q
static void input_queue_cb(struct ll_queue* q, void* arg) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_TX_STACK);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    struct ETCP_CONN* etcp = (struct ETCP_CONN*)arg;
    if (!etcp->next_tx_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] sequence numbers exhausted; restarting stream before reuse", etcp->log_name);
        etcp_conn_fatal_reinit(etcp, "sequence numbers exhausted"); return;
    }
    /* Selective ACK освобождает inflight, но не место за дыркой в приёмном окне пира. */
    if ((uint32_t)(etcp->next_tx_id - etcp->rx_ack_till) > MAX_INFLIGHT_SIZE) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] peer receive window full: next=%u cumulative_ack=%u queued=%d",
                    etcp->log_name, etcp->next_tx_id, etcp->rx_ack_till, q->count);
        ack_window_arm(etcp); return;
    }
    if (q->head && (q->head->len > etcp->optimal_inflight ||
        etcp->unacked_bytes > etcp->optimal_inflight - q->head->len)) {
        // Фрагмент ещё до send_q: тот же selector отмечает заполненные окна, иначе BBR не может поднять inflight_hi.
        (void)etcp_loadbalancer_select_link(etcp, q->head->len, NULL);
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] input held by DATA budget: unacked=%u limit=%u next_bytes=%u send_q=%d",
            etcp->log_name, etcp->unacked_bytes, etcp->optimal_inflight, q->head->len, etcp->input_send_q->count);
        return;
    }
    uint32_t reset = etcp->reset_count;
    struct ETCP_FRAGMENT* in_pkt = (struct ETCP_FRAGMENT*)queue_data_get(q);

    if (!in_pkt) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot get element (pool=%p etcp=%p)", etcp->log_name, etcp->inflight_pool, etcp);
        queue_resume_callback(q);
        return;
        }
    if (etcp->close_requested || etcp->state == 2 || etcp->reset_count != reset || etcp->reinit_pending) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] input stopped after queue callback: closing=%u state=%d reset=%u->%u",
            etcp->log_name, etcp->close_requested, etcp->state, reset, etcp->reset_count);
        queue_dgram_free(&in_pkt->ll); queue_entry_free(&in_pkt->ll);
        return;
    }
    // Create INFLIGHT_PACKET

    struct INFLIGHT_PACKET* p = (struct INFLIGHT_PACKET*)queue_entry_new_from_pool(etcp->inflight_pool);
    if (!p) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot allocate INFLIGHT_PACKET (pool=%p etcp=%p)", etcp->log_name, etcp->inflight_pool, etcp);
        queue_dgram_free(&in_pkt->ll);
        queue_entry_free((struct ll_entry*)in_pkt);
        etcp_conn_fatal_reinit(etcp, "TX inflight OOM");
        return;
    }

    // Setup inflight packet (based on protocol.txt)
//    memset(p, 0, sizeof(*p));
    p->seq = etcp->next_tx_id++;  // Assign seq
    p->state = INFLIGHT_STATE_WAIT_SEND;
    p->last_timestamp = 0;
    p->ll.dgram = in_pkt->ll.dgram;
    p->ll.dgram_pool = in_pkt->ll.dgram_pool;
    p->ll.len = in_pkt->ll.len;

//    memory_pool_free(etcp->io_pool, in_pkt);// перемещаем из io_pool в inflight_pool
    queue_entry_free(&in_pkt->ll);

    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] TX input -> inflight seq=%u len=%u input=%d send=%d wait=%d",
                etcp->log_name, p->seq, p->ll.len, etcp->input_queue->count, etcp->input_send_q->count, etcp->input_wait_ack->count);
    int len=p->ll.len;// сохраним len

    // Add to send queue
    // При провале queue_data_put_with_index сам освобождает entry+dgram, повторный free дал бы double-free.
    if (queue_data_put_with_index(etcp->input_send_q, &p->ll) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] failed to add packet seq=%u to input_send_q", etcp->log_name, p->seq);
        etcp_conn_fatal_reinit(etcp, "TX inflight queue failure");
        return;
    }
    etcp->unacked_bytes += len;
    etcp_conn_wake_sender(etcp);

//    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "successfully moved from input_queue to input_send_q");

//    etcp_conn_process_send_queue(etcp);// сразу обработаем этот пакет
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] nextloop, input_queue size=%d ", etcp->log_name, q->count);
}


static void input_send_q_cb(struct ll_queue* q, void* arg) {// etcp->input_send_q data ready
    (void)q;
    etcp_conn_wake_sender(arg);
}
void etcp_conn_update_rtt(struct ETCP_CONN* conn) {
    if (!conn) return;
    uint16_t best = 0;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next)
        if (!link->closing && link->initialized && link->link_status && link->rtt_last &&
            (!best || link->rtt_last < best)) best = link->rtt_last;
    conn->rtt_last = best;
}

void etcp_on_link_down(struct ETCP_CONN* etcp, struct ETCP_LINK* down_link) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!etcp || etcp->state == 2) return;
    etcp_conn_update_rtt(etcp);
    int up=0;
    struct ETCP_LINK* link = etcp->links;
    while (link) {
        if (link->link_status) up=1;
        link = link->next;
    }
    int was_up = etcp->links_up;
    etcp->links_up = up;
    if (!up) {
        if (etcp_conn_schedule_down_close(etcp) < 0 && !node_conn_direct_handle_count(etcp)) {
            etcp_connection_close(etcp); return;
        }
    } else {
        etcp_down_timer_cancel(etcp);
        // После DOWN последней альтернативы ожидающий повтор может снова использовать прежний путь.
        etcp_conn_wake_sender(etcp);
    }
    if (up == 0 && was_up != 0) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "All links fall down");
        etcp_on_down(etcp, down_link);
    }
}

static void ack_response_timer_cb(void* arg) {
    struct ETCP_CONN* conn = arg;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] ACK timer fired due_us=%llu now_us=%llu", conn->log_name,
        (unsigned long long)conn->ack_resp_deadline_us, (unsigned long long)get_time_us());
    conn->ack_resp_timer = NULL; conn->ack_resp_deadline_us = 0;
    etcp_conn_schedule_ack(conn);
}

/* Один таймер обслуживает ближайший абсолютный срок: новый RX не отодвигает старые ACK. */
void etcp_conn_schedule_ack(struct ETCP_CONN* conn) {
    if (!conn) return;
    uint64_t now = get_time_us(), deadline = 0;
    if (!conn->close_requested && conn->state != 2 && conn->initialized && !conn->session_required && !conn->reinit_pending) {
        deadline = conn->ack_till_deadline_us;
        for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
            if (!link->initialized || !link->link_status) continue;
            if (link->ack_reply_pending) { deadline = now; break; }
            const struct ACK_PACKET* first = link->ack_q ? (struct ACK_PACKET*)link->ack_q->head : NULL;
            if (first && (!deadline || first->received_at_us + ETCP_ACK_DELAY_US < deadline))
                deadline = first->received_at_us + ETCP_ACK_DELAY_US;
        }
    }
    if (conn->ack_resp_timer && conn->ack_resp_deadline_us == deadline && deadline > now) return;
    if (conn->ack_resp_timer) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] ACK timer replace old_us=%llu next_us=%llu", conn->log_name,
            (unsigned long long)conn->ack_resp_deadline_us, (unsigned long long)deadline);
        uasync_cancel_timeout(conn->instance->ua, conn->ack_resp_timer); conn->ack_resp_timer = NULL;
    }
    conn->ack_resp_deadline_us = 0;
    if (!deadline) return;
    if (deadline <= now) { etcp_conn_wake_sender(conn); return; }
    int delay = (int)((deadline - now + 99) / 100);
    conn->ack_resp_timer = uasync_set_timeout(conn->instance->ua, delay, conn, ack_response_timer_cb, "etcp_ack_resp");
    if (!conn->ack_resp_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] ACK timer OOM due_us=%llu delay_tb=%d; forcing urgent ACK", conn->log_name,
            (unsigned long long)deadline, delay);
        for (struct ETCP_LINK* link = conn->links; link; link = link->next)
            if (link->initialized && link->link_status && link->ack_q && link->ack_q->count) link->ack_reply_pending = 1;
        if (conn->ack_till_deadline_us) conn->ack_till_deadline_us = now;
        etcp_conn_wake_sender(conn);
    } else {
        conn->ack_resp_deadline_us = deadline;
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] ACK timer arm due_us=%llu now_us=%llu delay_tb=%d", conn->log_name,
            (unsigned long long)deadline, (unsigned long long)now, delay);
    }
}

static void ack_window_cb(void* arg) {
    struct ETCP_CONN* conn = arg;
    conn->ack_window_timer = NULL;
    if (conn->close_requested || !conn->input_queue->count ||
        (uint32_t)(conn->next_tx_id - conn->rx_ack_till) <= MAX_INFLIGHT_SIZE) return;
    conn->ack_request_pending = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] requesting peer window: next_seq=%u till=%u",
        conn->log_name, conn->next_tx_id, conn->rx_ack_till);
    etcp_conn_wake_sender(conn);
    ack_window_arm(conn);
}

static void ack_window_arm(struct ETCP_CONN* conn) {
    if (conn->ack_window_timer || conn->close_requested) return;
    uint32_t delay = 1000000;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next)
        if (link->initialized && link->link_status && etcp_tx_rto_us(link) < delay) delay = etcp_tx_rto_us(link);
    conn->ack_window_timer = uasync_set_timeout(conn->instance->ua, (int)((delay + 99) / 100), conn,
        ack_window_cb, "etcp_peer_window");
    if (!conn->ack_window_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot schedule peer-window query", conn->log_name);
        conn->callbacks_running++; etcp_connection_close(conn); conn->callbacks_running--;
    }
}
static void etcp_link_ready_callback(struct ETCP_CONN* etcp) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!etcp) return;
    if (etcp->state == 2) return;   // удалённый conn не поднимается

    if (etcp->tx_state==0) {
       DEBUG_WARN(DEBUG_CATEGORY_ETCP, "ETCP not ready, skip link state change");
       return;// not initialized
    }

    int up = 0;
    for (struct ETCP_LINK* link = etcp->links; link; link = link->next)
        if (link->initialized && link->link_status) { up = 1; break; }
    if (etcp->links_up==0 && up) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] etcp_link_ready_callback: links_up 0→1, calling etcp_on_up (initialized=%d tx_state=%d)", etcp->log_name, etcp->initialized, etcp->tx_state);
        etcp->links_up=1;
        etcp_on_up(etcp);
    }


    etcp_conn_wake_sender(etcp);
}

/* Снимок после прохода отправителя: причины могут пересекаться, full относится к голове очереди.
 * Только BLE при etcp=trace; не вызывает selection/BBR и не меняет состояние очередей. */
static void trace_ble_tx_wait(struct ETCP_CONN* conn) {
    if (!debug_should_output(DEBUG_LEVEL_TRACE, DEBUG_CATEGORY_ETCP) || conn->close_requested || conn->state == 2) return;
    struct PKTNORM* pn = conn->normalizer;
    if (!conn->input_send_q->count && !conn->input_queue->count &&
        (!pn || (!queue_entry_count(pn->input) && !pn->pending && !pn->data_ptr))) return;
    struct INFLIGHT_PACKET* head = (struct INFLIGHT_PACKET*)conn->input_send_q->head;
    unsigned input_len = conn->input_queue->head ? conn->input_queue->head->len : 0;
    uint64_t now = get_time_tb();
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
        struct packet_link* channel = etcp_link_io_get(link);
        if (!channel || strcmp(packet_link_name(channel), "BLE")) continue;
        unsigned bytes = head ? head->ll.len : input_len;
        int cwnd_full = bytes && (bytes > link->inflight_lim_bytes || link->inflight_bytes > link->inflight_lim_bytes - bytes);
        int attempts_full = conn->active_attempts >= 2 * MAX_INFLIGHT_SIZE || queue_entry_count(link->attempts) >= MAX_INFLIGHT_SIZE;
        int input_full = input_len && (input_len > conn->optimal_inflight || conn->unacked_bytes > conn->optimal_inflight - input_len);
        uint64_t pacing_tb = link->shaper_load_time_tb >= now + SHAPER_BURST_DELAY_TB ?
            link->shaper_load_time_tb - now - SHAPER_BURST_DELAY_TB + 1 : 0;
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] TX gate path=%u head_seq=%u bytes=%u retry=%u io=%d up=%u init=%u "
                    "cwnd_full=%d attempts_full=%d pacing=%d pacing_us=%llu rate_Bps=%u flight=%u/%u "
                    "input_full=%d unacked=%u/%u peer_full=%d input=%d send=%d norm=%d pending=%u buffered=%u token=%d",
                    conn->log_name, link->local_link_id, head ? head->seq : 0, bytes, head && head->last_attempt_id != 0,
                    etcp_link_io_ready(link), link->link_status, link->initialized, cwnd_full, attempts_full,
                    link->shaper_timer != NULL, (unsigned long long)(pacing_tb * 100), link->bbr_pacing_rate,
                    link->inflight_bytes, link->inflight_lim_bytes, input_full, conn->unacked_bytes, conn->optimal_inflight,
                    (uint32_t)(conn->next_tx_id - conn->rx_ack_till) > MAX_INFLIGHT_SIZE,
                    conn->input_queue->count, conn->input_send_q->count, pn ? queue_entry_count(pn->input) : 0,
                    pn && pn->pending ? pn->pending->len - pn->pending_in_ptr : 0, pn ? pn->data_ptr : 0, conn->tx_token != NULL);
    }
}

enum tx_report_gate { TX_REPORT_IDLE, TX_REPORT_IO, TX_REPORT_CWND, TX_REPORT_PACING, TX_REPORT_READY };

/* Интегрируем состояния между проходами TX. Приоритет: окно DATA, готовность драйвера, pacing. */
static void report_tx_wait(struct ETCP_CONN* conn) {
    if (!debug_should_output(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_ETCP) || conn->close_requested || conn->state == 2) return;
    unsigned bytes = conn->input_send_q->head ? conn->input_send_q->head->len :
                     conn->input_queue->head ? conn->input_queue->head->len : 0;
    uint64_t now = get_time_us();
    if (!conn->tx_report_us) { conn->tx_report_us = now; conn->tx_report_acked = conn->diagnostic_acked_bytes; }
    uint64_t dt = now - conn->tx_report_us;
    int report = dt >= 1000000;
    uint64_t acked = conn->diagnostic_acked_bytes - conn->tx_report_acked;
    if (report && (acked || bytes)) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] TX flow window_us=%llu acked_bytes=%llu admission_bytes=%u/%u "
            "input_bytes=%zu send_bytes=%zu wait_ack_bytes=%zu peer_window_full=%u",
            conn->log_name, (unsigned long long)dt, (unsigned long long)acked, conn->unacked_bytes, conn->optimal_inflight,
            conn->input_queue->total_bytes, conn->input_send_q->total_bytes, conn->input_wait_ack->total_bytes,
            (uint32_t)(conn->next_tx_id - conn->rx_ack_till) > MAX_INFLIGHT_SIZE);
    }
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
        uint64_t then = link->tx_report.sample_us;
        uint64_t elapsed = now - then;
        switch (link->tx_report.gate) {
        case TX_REPORT_IO: link->tx_report.io_us += elapsed; break;
        case TX_REPORT_CWND: link->tx_report.cwnd_us += elapsed; break;
        case TX_REPORT_PACING: {
            uint64_t due = link->tx_report.pace_due_us;
            uint64_t until = due > now ? now : due > then ? due : then;
            link->tx_report.pacing_us += until - then;
            link->tx_report.late_us += now - until;
            break;
        }
        case TX_REPORT_READY: link->tx_report.ready_us += elapsed; break;
        default: break;
        }
        if (link->inflight_bytes > link->tx_report.flight_max) link->tx_report.flight_max = link->inflight_bytes;
        if (!then || report) {
            uint64_t rx = link->acked_bytes >= link->tx_report.acked_bytes ? link->acked_bytes - link->tx_report.acked_bytes : 0;
            uint64_t tx = link->diagnostic_wire_tx_bytes - link->tx_report.wire_bytes;
            if (then && (rx || bytes || link->tx_report.gate != TX_REPORT_IDLE)) {
                struct packet_link* channel = etcp_link_io_get(link);
                DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] TX path=%u driver=%s up=%u acked_bytes=%llu wire_bytes=%llu "
                    "flight_bytes=%u peak_sampled=%u cwnd_bytes=%u rtt_us=%u min_rtt_us=%u pacing_Bps=%u "
                    "wait_io_us=%llu wait_cwnd_us=%llu wait_pacing_us=%llu pacing_overdue_us=%llu ready_us=%llu",
                    conn->log_name, link->local_link_id, channel ? packet_link_name(channel) : "-", link->link_status,
                    (unsigned long long)rx, (unsigned long long)tx, link->inflight_bytes, link->tx_report.flight_max,
                    link->inflight_lim_bytes, link->data_rtt_us, link->bbr ? link->bbr->min_rtt_us : 0, link->bbr_pacing_rate,
                    (unsigned long long)link->tx_report.io_us, (unsigned long long)link->tx_report.cwnd_us,
                    (unsigned long long)link->tx_report.pacing_us, (unsigned long long)link->tx_report.late_us,
                    (unsigned long long)link->tx_report.ready_us);
            }
            memset(&link->tx_report, 0, sizeof(link->tx_report));
            link->tx_report.acked_bytes = link->acked_bytes;
            link->tx_report.wire_bytes = link->diagnostic_wire_tx_bytes;
        }
        link->tx_report.sample_us = now;
        link->tx_report.gate = TX_REPORT_IDLE;
        if (!bytes || !link->initialized || !link->link_status) continue;
        if (bytes > link->inflight_lim_bytes || link->inflight_bytes > link->inflight_lim_bytes - bytes ||
            conn->active_attempts >= 2 * MAX_INFLIGHT_SIZE || link->attempts->count >= MAX_INFLIGHT_SIZE)
            link->tx_report.gate = TX_REPORT_CWND;
        else if (!etcp_link_io_ready(link)) link->tx_report.gate = TX_REPORT_IO;
        else if (link->shaper_timer && !link->burst_active) {
            link->tx_report.gate = TX_REPORT_PACING;
            link->tx_report.pace_due_us = (link->shaper_load_time_tb + 1 - SHAPER_BURST_DELAY_TB) * 100;
        } else link->tx_report.gate = TX_REPORT_READY;
    }
    if (report) { conn->tx_report_us = now; conn->tx_report_acked = conn->diagnostic_acked_bytes; }
}

// Process packets in send queue and transmit them
static void etcp_conn_process_send_queue(struct ETCP_CONN* etcp) {// вызываем когда есть элемент в send_q или надо отправить ack
    ETCP_PROFILE_SCOPE(ETCP_PROF_TX_STACK);
    if (etcp->tx_running || etcp->close_requested || etcp->state == 2) return;
    etcp->tx_running = 1;
    etcp->callbacks_running++;
    struct ETCP_DGRAM* dgram;
    while (!etcp->close_requested && (dgram = etcp_request_pkt(etcp))) {
        int sent = etcp_loadbalancer_send(dgram);
        if (sent < 0) { if (sent != -2) etcp_schedule_send(etcp, 100); break; }
    }
    etcp->callbacks_running--;
    etcp->tx_running = 0;
    report_tx_wait(etcp);
    trace_ble_tx_wait(etcp);
}

/* Один запуск обслуживает накопленные события; заблокированный DATA ждёт ACK/RTO/shaper, а не следующий poll. */
static void etcp_send_timer_cb(void* arg) {
    struct ETCP_CONN* conn = arg;
    conn->tx_token = NULL;
    etcp_conn_process_send_queue(conn);
}

// Подготовить и отправить кодограмму
// вызывается линком когда освобождается или очередью если появляются данные на передачу
static uint64_t read_le(const uint8_t* data, unsigned size) {
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++) value |= (uint64_t)data[i] << (i * 8);
    return value;
}

static void write_le(uint8_t* data, uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; i++) data[i] = (uint8_t)(value >> (i * 8));
}

/* Один просмотр без извлечения/резервов: первый готовый повтор, иначе первый готовый новый DATA.
 * Сохранённый кандидат живёт весь просмотр: здесь нет callbacks, poll или изменения очереди. */
static struct INFLIGHT_PACKET* send_queue_select(struct ETCP_CONN* conn, struct ETCP_LINK** chosen) {
    struct ETCP_LINK* burst = NULL;
    for (struct ETCP_LINK* l = conn->links; l; l = l->next)
        if (l->burst_active && l->initialized && l->link_status && etcp_link_io_ready(l)) { burst = l; break; }
    struct INFLIGHT_PACKET* fresh = NULL;
    struct ETCP_LINK* fresh_link = NULL;
    unsigned examined = 0, retry_elsewhere = 0;
    for (struct ll_entry* e = conn->input_send_q->head; e; e = e->next) {
        struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)e;
        examined++;
        if (!packet->last_attempt_id && fresh) continue;
        struct ETCP_LINK* link;
        if (burst && !packet->last_attempt_id) {
            if (conn->active_attempts >= 2 * MAX_INFLIGHT_SIZE || burst->attempts->count >= MAX_INFLIGHT_SIZE ||
                packet->ll.len > burst->inflight_lim_bytes || burst->inflight_bytes > burst->inflight_lim_bytes - packet->ll.len) {
                burst->cwnd_limited = 1; continue;
            }
            link = burst;
        } else link = etcp_loadbalancer_select_link(conn, packet->ll.len, packet->last_attempt_id ? packet : NULL);
        if (!link) continue;
        if (burst && link != burst) { if (packet->last_attempt_id) retry_elsewhere = 1; continue; }
        if (packet->last_attempt_id) {
            *chosen = link;
            DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] send scan examined=%u retry_seq=%u link=%u", conn->log_name,
                examined, packet->seq, link->local_link_id);
            return packet;
        }
        fresh = packet; fresh_link = link;
    }
    // Короткая burst-серия идёт FILLER, пока готовый повтор другого пути ждёт её завершения; новые DATA его не обгоняют.
    if (retry_elsewhere) fresh = NULL;
    *chosen = fresh ? fresh_link : burst;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] send scan examined=%u fresh_seq=%u link=%u", conn->log_name, examined,
        fresh ? fresh->seq : 0, *chosen ? (*chosen)->local_link_id : 0);
    return fresh;
}

struct ETCP_DGRAM* etcp_request_pkt(struct ETCP_CONN* conn) {
    if (!conn || conn->state == 2 || conn->close_requested) return NULL;
    if (!conn->initialized || conn->session_required || conn->reinit_pending) {
        if (conn->tx_state) conn->tx_state = ETCP_TX_STATE_LINK_WAIT;
        return NULL;
    }
    etcp_conn_schedule_ack(conn);
    if (conn->close_requested) return NULL;
    struct ETCP_LINK* link = NULL;
    struct INFLIGHT_PACKET* packet = send_queue_select(conn, &link);
    if (!packet) {
        uint32_t reset = conn->reset_count;
        int pulled = input_queue_try_push(conn);
        // Подгрузка может сменить эпоху/закрыть conn либо удалить burst-линк: прежние кандидаты больше не используются.
        if (conn->close_requested || conn->state == 2 || conn->reset_count != reset || !conn->initialized ||
            conn->session_required || conn->reinit_pending) return NULL;
        if (pulled) packet = send_queue_select(conn, &link);
    }
    if (!link || !link->burst_active) {
        uint64_t now = get_time_us();
        struct ETCP_LINK* ack_link = NULL;
        uint64_t first_due = UINT64_MAX;
        for (struct ETCP_LINK* candidate = conn->links; candidate; candidate = candidate->next) {
            if (!candidate->initialized || !candidate->link_status || !etcp_link_io_ready(candidate)) continue;
            const struct ACK_PACKET* first = candidate->ack_q ? (struct ACK_PACKET*)candidate->ack_q->head : NULL;
            if (!candidate->ack_reply_pending && !first) continue;
            uint64_t due = candidate->ack_reply_pending ? 0 : first->received_at_us + ETCP_ACK_DELAY_US;
            if (due <= now && (!ack_link || due < first_due)) { ack_link = candidate; first_due = due; }
        }
        if (ack_link) {
            DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] ACK due path=%u deadline_us=%llu now_us=%llu piggyback=%u", conn->log_name,
                ack_link->local_link_id, (unsigned long long)first_due, (unsigned long long)now, ack_link == link && packet != NULL);
            if (ack_link != link) packet = NULL;
            link = ack_link;
        }
    }
    if (!link && (conn->ack_request_pending ||
        (conn->ack_till_deadline_us && conn->ack_till_deadline_us <= get_time_us()))) {
        link = etcp_loadbalancer_select_link(conn, 0, NULL); packet = NULL;
    }
    if (!link) {
        conn->tx_state = conn->input_send_q->count || conn->input_queue->count ? ETCP_TX_STATE_LINK_WAIT : ETCP_TX_STATE_DATA_WAIT;
        return NULL;
    }
    conn->tx_state = ETCP_TX_STATE_DATA_WAIT;
    int max_enc = link->mtu - (etcp_link_network_overhead(link) + SC_ENCRYPT_OVERHEAD);
    if (max_enc > PACKET_DATA_SIZE) max_enc = PACKET_DATA_SIZE;
    unsigned payload_size = packet ? ETCP_PAYLOAD_HEADER_SIZE + packet->ll.len : 0;
    if (max_enc < (int)(ETCP_ACK_BASE_SIZE + payload_size + conn->ack_request_pending)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] DATA does not fit negotiated MTU path=%u max=%d payload=%u",
            conn->log_name, link->local_link_id, max_enc, payload_size);
        return NULL;
    }
    struct ETCP_DGRAM* dgram = memory_pool_alloc(conn->instance->pkt_pool);
    if (!dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot allocate TX packet; retry in 10ms", conn->log_name);
        etcp_schedule_send(conn, 100); return NULL;
    }
    dgram->link = link; dgram->attempt = NULL; dgram->noencrypt_len = 0;
    dgram->payload_len = packet ? packet->ll.len : 0;
    dgram->timestamp = get_current_timestamp(); dgram->burst_packet = link->burst_active;
    dgram->ack_request = conn->ack_request_pending;
    dgram->ack_count = dgram->commit_timestamp = dgram->commit_burst_response = 0;
    if (packet) {
        dgram->attempt = etcp_tx_prepare(link, packet);
        if (!dgram->attempt) {
            memory_pool_free(conn->instance->pkt_pool, dgram); etcp_schedule_send(conn, 100); return NULL;
        }
    }
    uint8_t* data = dgram->data;
    data[0] = ETCP_SECTION_ACK; data[1] = 0;
    write_le(data + 2, conn->last_delivered_id, 4); write_le(data + 6, conn->rx_dup_count, 2);
    int ptr = ETCP_ACK_BASE_SIZE, remaining = max_enc - ptr - payload_size - dgram->ack_request;
    int ack_space = remaining - ETCP_ACK_RX_POINT_SIZE;
    uint64_t ack_created_us = get_time_us();
    struct ll_entry* ack_entry = link->ack_q->head;
    while (ack_entry && ack_space >= ETCP_ACK_ENTRY_SIZE && data[1] != UINT8_MAX) {
        struct ACK_PACKET* ack = (struct ACK_PACKET*)ack_entry;
        ack_entry = ack_entry->next;
        write_le(data + ptr, ack->seq, 4); write_le(data + ptr + 4, (uint16_t)ack->attempt_id, 2);
        uint64_t delay = ack_created_us - ack->received_at_us;
        write_le(data + ptr + 6, delay >= UINT32_MAX ? UINT32_MAX : delay, 4);
        ptr += ETCP_ACK_ENTRY_SIZE; remaining -= ETCP_ACK_ENTRY_SIZE; ack_space -= ETCP_ACK_ENTRY_SIZE; data[1]++;
        dgram->ack_count++;
    }
    if (data[1]) {
        write_le(data + ptr, link->rx_data_bytes, 8); write_le(data + ptr + 8, link->rx_data_time_us, 8);
        write_le(data + ptr + 16, ack_created_us, 8);
        ptr += ETCP_ACK_RX_POINT_SIZE; remaining -= ETCP_ACK_RX_POINT_SIZE;
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] ACK point path=%u count=%u RX=%llu/%llu created_us=%llu", conn->log_name,
            link->local_link_id, data[1], (unsigned long long)link->rx_data_bytes, (unsigned long long)link->rx_data_time_us,
            (unsigned long long)ack_created_us);
    }
    if (dgram->ack_request) data[ptr++] = ETCP_SECTION_ACK_REQUEST;
    if (link->burst_resp_pending && !link->burst_active && remaining >= MEAS_RESP_SECTION_SIZE) {
        data[ptr++] = ETCP_SECTION_MEAS_RESP;
        write_le(data + ptr, link->burst_recv_id, 2); ptr += 2;
        data[ptr++] = link->burst_resp_valid;
        write_le(data + ptr, link->burst_resp_gap_avg, 4); ptr += 4;
        write_le(data + ptr, link->burst_resp_gap_min, 4); ptr += 4;
        data[ptr++] = link->burst_resp_pkt_count;
        remaining -= MEAS_RESP_SECTION_SIZE; dgram->commit_burst_response = 1;
    }
    int pkt_size_offset = 0;
    if (dgram->burst_packet && remaining >= MEAS_TS_SECTION_SIZE) {
        uint8_t flags = packet ? 0 : MEAS_FLAG_IS_FILLER;
        if (!link->burst_seq) flags |= MEAS_FLAG_IS_FIRST;
        if (link->burst_seq + 1 >= link->burst_count) flags |= MEAS_FLAG_IS_LAST;
        data[ptr++] = ETCP_SECTION_MEAS_TS;
        write_le(data + ptr, link->burst_id, 2); ptr += 2;
        data[ptr++] = flags; data[ptr++] = link->burst_seq;
        write_le(data + ptr, get_time_us(), 2); ptr += 2;
        pkt_size_offset = ptr; ptr += 2; remaining -= MEAS_TS_SECTION_SIZE;
    }
    if (link->last_recv_updated && remaining >= 5) {
        uint64_t dt = get_time_tb() - link->last_recv_local_time;
        dgram->commit_timestamp = 1;
        if (dt < 1000000) {
            data[ptr++] = ETCP_SECTION_TIMESTAMP;
            write_le(data + ptr, link->last_recv_timestamp + dt, 2); ptr += 2;
            write_le(data + ptr, link->last_recv_local_time - link->last_recv_timestamp, 2); ptr += 2;
            remaining -= 5;
        }
    }
    if (dgram->burst_packet && remaining > FILLER_HDR_SIZE) {
        unsigned filler = remaining - FILLER_HDR_SIZE;
        data[ptr++] = ETCP_SECTION_FILLER; write_le(data + ptr, filler, 2); ptr += 2;
        memset(data + ptr, 0, filler); ptr += filler;
    }
    if (packet) {
        data[ptr++] = ETCP_SECTION_PAYLOAD; write_le(data + ptr, packet->seq, 4); ptr += 4;
        write_le(data + ptr, (uint16_t)dgram->attempt->attempt_id, 2); ptr += 2;
        memcpy(data + ptr, packet->ll.dgram, packet->ll.len); ptr += packet->ll.len;
    }
    dgram->data_len = ptr;
    if (pkt_size_offset) {
        write_le(data + pkt_size_offset, ptr, 2); link->burst_pkt_size = ptr;
    }
    if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_DUMP)) etcp_dump_pkt_sections(dgram, link, 1);
    return dgram;
}

// ====================================================================== Прием данных


void etcp_output_try_assembly(struct ETCP_CONN* etcp) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    // пробуем собрать выходную очередь из фрагментов
//    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] etcp=%p, last_delivered_id=%u, recv_q_count=%d",
//                etcp->log_name, etcp, etcp->last_delivered_id, queue_entry_count(etcp->recv_q));

    uint32_t next_expected_id = etcp->last_delivered_id + 1;
    uint32_t reset = etcp->reset_count;
    int delivered_count = 0;
    uint32_t delivered_bytes = 0;

    // Look for contiguous packets starting from next_expected_id
    while (!etcp->close_requested && !etcp->reinit_pending && etcp->reset_count == reset) {
        struct ETCP_FRAGMENT* rx_pkt = (struct ETCP_FRAGMENT*)queue_find_data_by_index(etcp->recv_q, &next_expected_id);
        if (!rx_pkt) {
            // No more contiguous packets found
            DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] no packet found for id=%u, stopping", etcp->log_name, next_expected_id);
            break;
        }

//        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] assembling packet id=%u (len=%u)", etcp->log_name,
//                    rx_pkt->seq, rx_pkt->ll.len);

        // Simply move ETCP_FRAGMENT from recv_q to output_queue - no data copying needed
        // Remove from recv_q first
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "Remove from assembly queue");
        queue_remove_data(etcp->recv_q, (struct ll_entry*)rx_pkt);
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "move: ETCP -> PN");

        // Add to output_queue using the same ETCP_FRAGMENT structure
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] moving packet id=%u to output_queue (qlen=%d)", etcp->log_name,
                   next_expected_id, etcp->output_queue->count);
        uint16_t pkt_len = rx_pkt->ll.len;
        // На провале queue_data_put сам освобождает rx_pkt — re-put дал бы double-free.
        if (queue_data_put(etcp->output_queue, (struct ll_entry*)rx_pkt) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] failed to add packet id=%u to output_queue", etcp->log_name,
                       next_expected_id);
            break;
        }
        // pn_unpacker_cb (вызывается из put синхронно) мог ре-энтрантно сделать reinit/close:
        // при reinit last_delivered_id уже сброшен в 0 и recv_q очищен, при close state==2.
        if (etcp->close_requested || etcp->state == 2 || etcp->reinit_pending || etcp->reset_count != reset) break;

        delivered_bytes += pkt_len;
        delivered_count++;

        // Update state for next iteration
        etcp->last_delivered_id = next_expected_id;
        next_expected_id++;
    }

    if (delivered_count > 0 && !etcp->close_requested && !etcp->reinit_pending) {
        if (!etcp->ack_till_deadline_us) etcp->ack_till_deadline_us = get_time_us() + ETCP_ACK_DELAY_US;
        etcp_conn_schedule_ack(etcp);
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] delivered %u fragments (%u bytes), till=%u recv_q=%d",
            etcp->log_name, delivered_count, delivered_bytes, etcp->last_delivered_id, etcp->recv_q->count);
    }
}

// Process ACK receipt - remove acknowledged packet from inflight queues
void etcp_ack_recv(struct ETCP_CONN* conn, uint32_t seq) {
    if (!conn || conn->state == 2 || conn->close_requested) return;
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)queue_find_data_by_index(conn->input_wait_ack, &seq);
    struct ll_queue* queue = conn->input_wait_ack;
    if (!packet) { packet = (struct INFLIGHT_PACKET*)queue_find_data_by_index(conn->input_send_q, &seq); queue = conn->input_send_q; }
    if (!packet) { conn->cnt_ack_miss++; return; }
    if (queue == conn->input_wait_ack) conn->cnt_ack_hit_inf++; else conn->cnt_ack_hit_sndq++;
    queue_remove_data(queue, &packet->ll);
    if (conn->unacked_bytes < packet->ll.len) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] logical ACK accounting mismatch seq=%u unacked=%u len=%u",
            conn->log_name, seq, conn->unacked_bytes, packet->ll.len);
        abort();
    }
    conn->unacked_bytes -= packet->ll.len; conn->bytes_sent_total += packet->ll.len; conn->ack_packets_count++;
    conn->diagnostic_acked_bytes += packet->ll.len; conn->diagnostic_ack_us = get_time_us();
    etcp_tx_record_ack(&conn->instance->tx_history, get_time_us(), packet->ll.len);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] logical delivery seq=%u bytes=%u active_attempts=%u unacked=%u",
        conn->log_name, seq, packet->ll.len, packet->active_attempts, conn->unacked_bytes);
    if (packet->ll.dgram) memory_pool_free(conn->instance->data_pool, packet->ll.dgram);
    queue_entry_free(&packet->ll);
    input_queue_try_resume(conn); etcp_conn_wake_sender(conn);
}
// Process incoming decrypted packet
void etcp_conn_input(struct ETCP_DGRAM* pkt) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!pkt) return;
    if (memory_pool_is_freed(pkt->link->etcp->instance->pkt_pool, pkt)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "pkt=%p ALREADY FREED in pkt_pool — HALTING", (void*)pkt);
        volatile int _halt = 1; while (_halt) {}
    }
    if (!pkt->data_len) {
        memory_pool_free(pkt->link->etcp->instance->pkt_pool, pkt);
        return;
    }

    if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_DUMP)) etcp_dump_pkt_sections(pkt, pkt->link, 0);

    /* Окно освобождает ядро: RTT включает очереди transport/crypto. Часы сокета нужны только для RX-полосы. */
    uint64_t processed_us = get_time_us(), received_us = pkt->received_us ? pkt->received_us : processed_us;
    pkt->link->last_recv_local_time = processed_us / 100;

    struct ETCP_CONN* etcp = pkt->link->etcp;
    uint8_t* data = pkt->data;
    uint16_t len = pkt->data_len;
    uint16_t ts = pkt->timestamp;  // Received timestamp

    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] RX pkt dlen=%d dispatch_us=%llu", etcp->log_name, len,
        (unsigned long long)(processed_us >= received_us ? processed_us - received_us : 0));

    while (len >= 1) {
        // Ре-энтрантный reinit/close (из pn_unpacker_cb/etcp_int_recv) — дальше разбирать пакет нельзя.
        if (etcp->close_requested || etcp->state == 2 || etcp->reinit_pending) break;
        uint8_t type = data[0];

        // Process sections as per protocol.txt
        switch (type) {
            case ETCP_SECTION_ACK: {
                if (len < ETCP_ACK_BASE_SIZE) { DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] short ACK len=%u", etcp->log_name, len); len = 0; break; }
                unsigned count = data[1], section_len = etcp_ack_section_size(data[1]);
                if (section_len > len) {
                    DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] ACK exceeds packet count=%u len=%u", etcp->log_name, count, len);
                    len = 0; break;
                }
                uint32_t till = (uint32_t)read_le(data + 2, 4), last_tx = etcp->next_tx_id - 1;
                if ((int32_t)(till - last_tx) > 0) till = last_tx;
                uint16_t duplicates = (uint16_t)read_le(data + 6, 2);
                unsigned point_offset = ETCP_ACK_BASE_SIZE + count * ETCP_ACK_ENTRY_SIZE;
                uint64_t rx_bytes = count ? read_le(data + point_offset, 8) : 0;
                uint64_t rx_time = count ? read_le(data + point_offset + 8, 8) : 0;
                uint64_t ack_created_us = count ? read_le(data + point_offset + 16, 8) : 0;
                etcp->tx_dup_count += (int16_t)(duplicates - (uint16_t)etcp->tx_dup_count);
                for (unsigned i = 0; i < count; i++) {
                    const uint8_t* entry = data + ETCP_ACK_BASE_SIZE + i * ETCP_ACK_ENTRY_SIZE;
                    uint32_t seq = (uint32_t)read_le(entry, 4);
                    uint16_t number = (uint16_t)read_le(entry + 4, 2);
                    etcp_tx_ack_at(pkt->link, seq, etcp_tx_attempt_id(seq, number),
                                (uint32_t)read_le(entry + 6, 4), rx_bytes, rx_time, ack_created_us, processed_us);
                    if (etcp->close_requested || etcp->reinit_pending) break;
                }
                uint32_t advanced = 0;
                while (!etcp->close_requested && (int32_t)(till - etcp->rx_ack_till) > 0 && advanced < ETCP_ACK_GAP_MAX_PACKETS) {
                    etcp->rx_ack_till++; etcp_ack_recv(etcp, etcp->rx_ack_till); advanced++;
                }
                if (advanced) {
                    if (etcp->ack_window_timer && (uint32_t)(etcp->next_tx_id - etcp->rx_ack_till) <= MAX_INFLIGHT_SIZE) {
                        uasync_cancel_timeout(etcp->instance->ua, etcp->ack_window_timer); etcp->ack_window_timer = NULL;
                    }
                    input_queue_try_resume(etcp);
                }
                data += section_len; len -= section_len;
                break;
            }
            case ETCP_SECTION_ACK_REQUEST:
                pkt->link->ack_reply_pending = 1; etcp_conn_wake_sender(etcp);
                data++; len--;
                break;
            case ETCP_SECTION_TIMESTAMP: {
                if (len < 5) { len = 0; break; }
                uint16_t cur_ts = (uint16_t)(processed_us / 100);
                uint16_t ret_ts=data[1] | (data[2]<<8);// cur_ts=ret_ts = RTT
                uint16_t new_rtt=cur_ts-ret_ts;
                if (!new_rtt) new_rtt = 1; /* Нулём обозначено отсутствие измерения. */
                int prev_rtt = pkt->link->rtt_last;
                pkt->link->rtt_last=new_rtt;
                DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] KA-RTT: rtt=%u cur=%u ret=%u dlen=%u",
                            etcp->log_name, new_rtt, cur_ts, ret_ts, pkt->data_len);

                int recv_dt_tx1=data[3] | (data[4]<<8);// localtime удаленной стороны момента принятия пакета - timestamp этого пакета (на стороне отправителя, т.е. у нас)

                int recv_dt_rx=cur_ts - pkt->link->rtt_last/2 - 1000 - ts;
                int recv_dt_tx=recv_dt_tx1 - pkt->link->rtt_last/2 - 1000;
                /* Timestamp циклический uint16: Q16 также вычисляем по модулю 2^32, без signed overflow. */
                uint32_t sample_rx = (uint32_t)recv_dt_rx * 65536u;
                uint32_t sample_tx = (uint32_t)recv_dt_tx * 65536u;
                if (pkt->link->recv_dt_avg_rx==0) pkt->link->recv_dt_avg_rx=sample_rx;
                if (pkt->link->recv_dt_avg_tx==0) pkt->link->recv_dt_avg_tx=sample_tx;
                pkt->link->recv_dt_avg_rx +=((int32_t)(sample_rx - pkt->link->recv_dt_avg_rx))/32;
                pkt->link->recv_dt_avg_tx +=((int32_t)(sample_tx - pkt->link->recv_dt_avg_tx))/32;
                pkt->link->rt_last = cur_ts - ts - pkt->link->recv_dt_avg_rx/65536;
                pkt->link->tt_last = recv_dt_tx1 - pkt->link->recv_dt_avg_tx/65536;
                //tts_correction += ((NOW - RTT/2 - TTS) - tts_correction)/16 (инициализируем сразу по 1 пакету)
                data+=5; len-=5;

                int d_rtt = new_rtt - prev_rtt;
                if (d_rtt<0) d_rtt=-d_rtt;
                pkt->link->jitter += ((int64_t)d_rtt * 65536 - pkt->link->jitter) / 32;

                struct ETCP_LINK* c=etcp->links;
                int rtt_max=0;
                int tt_sum=0;
                int j_sum=0;
                int cnt=0;
                while (c) {
                    if (rtt_max < c->rtt_last) rtt_max = c->rtt_last;
                    tt_sum  += c->tt_last;
                    j_sum  += c->jitter/32768;
                    cnt++;
                    c=c->next;
                }
                etcp_conn_update_rtt(etcp);
                etcp->rtt_avg_10=rtt_max;
                etcp->tt_last=tt_sum/cnt;
                etcp->jitter=j_sum/cnt;
                if (new_rtt != prev_rtt) etcp_tx_update_rto(pkt->link);
                topo_node_rtt_link_sample(pkt->link);
                break;
            }
            case ETCP_SECTION_PAYLOAD: {
                if (len < ETCP_PAYLOAD_HEADER_SIZE) {
                    DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] short DATA len=%u", etcp->log_name, len); len = 0; break;
                }
                uint32_t seq = (uint32_t)read_le(data + 1, 4);
                uint16_t attempt_no = (uint16_t)read_le(data + 5, 2);
                uint64_t attempt_id = etcp_tx_attempt_id(seq, attempt_no);
                uint32_t payload_len = len - ETCP_PAYLOAD_HEADER_SIZE;
                if (!attempt_no || (!etcp->got_initial_pkt && seq != 1)) {
                    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] DATA rejected before initial fragment seq=%u attempt=%llu",
                        etcp->log_name, seq, (unsigned long long)attempt_no);
                    len = 0; break;
                }
                int32_t distance = (int32_t)(seq - etcp->last_delivered_id);
                if (distance > MAX_INFLIGHT_SIZE || distance < -MAX_INFLIGHT_SIZE || payload_len > PACKET_DATA_SIZE) {
                    DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] DATA out of bounds seq=%u till=%u len=%u",
                        etcp->log_name, seq, etcp->last_delivered_id, payload_len);
                    len = 0; break;
                }
                if (pkt->link->rx_data_bytes > UINT64_MAX - payload_len) {
                    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] RX byte counter exhausted path=%u seq=%u", etcp->log_name,
                        pkt->link->local_link_id, seq);
                    etcp->callbacks_running++; etcp_connection_close(etcp); etcp->callbacks_running--;
                    len = 0; break;
                }
                int duplicate = distance <= 0 || queue_find_data_by_index(etcp->recv_q, &seq) != NULL;
                if (!duplicate) {
                    struct ETCP_FRAGMENT* fragment = (struct ETCP_FRAGMENT*)queue_entry_new_from_pool(etcp->io_pool);
                    uint8_t* payload = memory_pool_alloc(etcp->instance->data_pool);
                    if (!fragment || !payload) {
                        if (fragment) queue_entry_free(&fragment->ll);
                        if (payload) memory_pool_free(etcp->instance->data_pool, payload);
                        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] DATA admission OOM seq=%u attempt=%llu; no ACK",
                            etcp->log_name, seq, (unsigned long long)attempt_no);
                        len = 0; break;
                    }
                    fragment->seq = seq; fragment->timestamp = pkt->timestamp;
                    fragment->ll.dgram = payload; fragment->ll.dgram_pool = etcp->instance->data_pool; fragment->ll.len = payload_len;
                    memcpy(payload, data + ETCP_PAYLOAD_HEADER_SIZE, payload_len);
                    if (queue_data_put_with_index(etcp->recv_q, &fragment->ll) < 0) {
                        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot retain DATA seq=%u; no ACK", etcp->log_name, seq);
                        len = 0; break;
                    }
                } else etcp->rx_dup_count++;
                if (payload_len) {
                    pkt->link->rx_data_bytes += payload_len; pkt->link->rx_data_time_us = received_us;
                }
                etcp->got_initial_pkt = 1;
                if (etcp_tx_queue_ack(pkt->link, seq, attempt_id) == 0) etcp_conn_schedule_ack(etcp);
                DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] accepted DATA seq=%u attempt=%llu path=%u bytes=%u duplicate=%d",
                    etcp->log_name, seq, (unsigned long long)attempt_no, pkt->link->local_link_id, payload_len, duplicate);
                if (distance == 1) etcp_output_try_assembly(etcp);
                len = 0;
                break;
            }

            case ETCP_SECTION_MEAS_TS: {
                if (len < MEAS_TS_SECTION_SIZE) { len = 0; break; }
                uint16_t b_id   = data[1] | (data[2] << 8);
                uint8_t  flags  = data[3];
                uint8_t  seq    = data[4];
                uint16_t pkt_sz = data[7] | (data[8] << 8);
                struct ETCP_LINK* link = pkt->link;
                if (b_id != link->burst_recv_id) {
                    link->burst_recv_id = b_id;
                    link->burst_recv_count = BURST_PACKET_COUNT;
                    link->burst_recv_next_seq = 0;
                    link->burst_recv_valid = 1;
                    link->burst_recv_received = 0;
                }
                if (link->burst_recv_valid) {
                    if (seq != link->burst_recv_next_seq) {
                        link->burst_recv_valid = 0;
                    }
                    if (seq < 16) {
                        link->burst_recv_times[seq] = received_us;
                        link->burst_recv_next_seq = seq + 1;
                        link->burst_recv_received++;
                        link->burst_recv_pkt_size = pkt_sz;
                    }
                }
                if ((flags & MEAS_FLAG_IS_LAST) || link->burst_recv_received >= link->burst_recv_count) {
                    uint8_t total = (flags & MEAS_FLAG_IS_LAST) ? link->burst_recv_received : link->burst_recv_count;
                    if (link->burst_recv_valid && total >= BURST_SKIP_COUNT + 2) {
                        uint32_t sum_gap = 0, min_gap = UINT32_MAX;
                        uint8_t measured = 0;
                        for (uint8_t i = BURST_SKIP_COUNT; i + 1 < total; i++) {
                            uint64_t dt = link->burst_recv_times[i + 1] - link->burst_recv_times[i];
                            uint32_t gap = (uint32_t)(dt > 0 ? dt : 1);
                            sum_gap += gap; measured++;
                            if (gap < min_gap) min_gap = gap;
                        }
                        if (measured > 0) {
                            link->burst_resp_gap_avg = sum_gap / measured;
                            link->burst_resp_gap_min = min_gap;
                            link->burst_resp_pkt_count = measured;
                            link->burst_resp_valid = 1;
                        }
                    }
                    link->burst_resp_pending = 1;
                }
                data += MEAS_TS_SECTION_SIZE; len -= MEAS_TS_SECTION_SIZE;
                break;
            }

            case ETCP_SECTION_MEAS_RESP: {
                if (len < MEAS_RESP_SECTION_SIZE) { len = 0; break; }
                uint16_t b_id = data[1] | (data[2] << 8);
                uint8_t  valid = data[3];
                uint32_t gap_avg = data[4] | (data[5] << 8) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
                uint32_t gap_min = data[8] | (data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
                uint8_t  pkt_cnt = data[12];
                struct ETCP_LINK* link = pkt->link;
                if (b_id == link->burst_id - 1 || b_id == link->burst_id) {
                    if (link->burst_resp_timer) { uasync_cancel_timeout(link->etcp->instance->ua, link->burst_resp_timer); link->burst_resp_timer = NULL; }
                    if (valid == MEAS_RESP_VALID && gap_min > 0 && link->burst_pkt_size > 0) {
                        uint64_t bw_kbps = (uint64_t)link->burst_pkt_size * 8000ULL / gap_min;
                        if (bw_kbps > 10000000) bw_kbps = 10000000;
                        // Wire-замер серии — диагностика; DATA pacing/окно принадлежат модели BBR.
                        uint64_t bdp = link->bbr->min_rtt_us == UINT32_MAX ? 0 : bw_kbps * link->bbr->min_rtt_us / 8000;
                        link->burst_target_bdp = bdp > UINT32_MAX ? UINT32_MAX : (uint32_t)bdp;
                        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] burst resp: BW=%u Kbps, BDP=%u gap_min=%u us pkt_cnt=%u",
                                   link->etcp->log_name, (uint32_t)bw_kbps, link->burst_target_bdp, gap_min, pkt_cnt);
                    }
                }
                data += MEAS_RESP_SECTION_SIZE; len -= MEAS_RESP_SECTION_SIZE;
                break;
            }

            case ETCP_SECTION_FILLER: {
                if (len < FILLER_HDR_SIZE) { len = 0; break; }
                uint16_t fill_len = data[1] | (data[2] << 8);
                if (fill_len > len - FILLER_HDR_SIZE) { len = 0; break; }
                data += FILLER_HDR_SIZE + fill_len; len -= FILLER_HDR_SIZE + fill_len;
                break;
            }

            default:
                DEBUG_WARN(DEBUG_CATEGORY_ETCP, "unknown section type=0x%02x", type);
                len=0;
                break;
        }

    }

    // Используем локальный etcp (валиден до phase 2 close): pkt->link мог быть освобождён ре-энтрантным close.
    if (memory_pool_is_freed(etcp->instance->pkt_pool, pkt)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "pkt=%p ALREADY FREED in pkt_pool — HALTING", (void*)pkt);
        volatile int _halt = 1; while (_halt) {}
    }


    memory_pool_free(etcp->instance->pkt_pool, pkt);  // Free the incoming dgram
}

void etcp_update_mtu(struct ETCP_CONN* etcp) {
    if (!etcp || etcp->state == 2) return;
    int new_mtu = PACKET_DATA_MAX_MTU;
    int has_links = 0;
    struct ETCP_LINK* link = etcp->links;
    while (link) {
        has_links = 1;
        if (link->mtu > 0 && link->mtu < new_mtu) new_mtu = link->mtu;
        link = link->next;
    }
    if (!has_links) new_mtu = ETCP_RFC791_MIN_MTU;
    etcp->mtu = new_mtu;
    if (etcp->normalizer) {
        int max_headers = has_links ? 0 : IP_UDP_OVERHEAD_V4;
        for (struct ETCP_LINK* l = etcp->links; l; l = l->next)
            if (etcp_link_network_overhead(l) > max_headers) max_headers = etcp_link_network_overhead(l);
        etcp->normalizer->frag_size = new_mtu - max_headers - SC_ENCRYPT_OVERHEAD - ETCP_PAYLOAD_HEADER_SIZE - ACK_REZERV;
    }
}
