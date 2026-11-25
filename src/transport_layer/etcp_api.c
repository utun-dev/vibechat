/**
 * @file etcp_api.c
 * @brief ETCP API — send/bind/recv dispatch
 */
#include "etcp_api.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "pkt_normalizer.h"
#include "etcp_profile.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <string.h>

#define DEBUG_CATEGORY_ETCP_API DEBUG_CATEGORY_ETCP

void etcp_instance_callbacks_collect(struct UTUN_INSTANCE* inst) {
    if (!inst || inst->etcp_callbacks_depth) return;
    unsigned removed = 0;
    struct etcp_inst_cbk_entry **created = &inst->new_conn_cbks;
    while (*created) {
        struct etcp_inst_cbk_entry *e = *created;
        if (!e->fn) { *created = e->next; u_free(e); removed++; } else created = &e->next;
    }
    struct etcp_status_cbk_entry **status = &inst->conn_status_cbks;
    while (*status) {
        struct etcp_status_cbk_entry *e = *status;
        if (!e->fn) { *status = e->next; u_free(e); removed++; } else status = &e->next;
    }
    struct etcp_link_status_cbk_entry **links = &inst->link_status_cbks;
    while (*links) {
        struct etcp_link_status_cbk_entry *e = *links;
        if (!e->fn) { *links = e->next; u_free(e); removed++; } else links = &e->next;
    }
    struct etcp_socket_cbk_entry **sockets = &inst->socket_cbks;
    while (*sockets) {
        struct etcp_socket_cbk_entry *e = *sockets;
        if (!e->fn) { *sockets = e->next; u_free(e); removed++; } else sockets = &e->next;
    }
    if (removed) DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "instance callback cleanup: removed=%u", removed);
}

// Per-connection unified callback chain helpers
static void etcp_cbk_add_to_chain(struct etcp_cbk_entry** head, etcp_cbk_fn fn, void* arg, int event_mask) {
    if (!head || !fn) return;
    struct etcp_cbk_entry* e = u_malloc(sizeof(struct etcp_cbk_entry));
    if (!e) return;
    e->fn = fn; e->arg = arg; e->event_mask = event_mask; e->next = *head;
    *head = e;
}
static void etcp_cbk_remove_from_chain(struct etcp_cbk_entry** head, etcp_cbk_fn fn, void* arg) {
    if (!head || !fn) return;
    struct etcp_cbk_entry** p = head;
    while (*p) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct etcp_cbk_entry* rm = *p;
            *p = rm->next; u_free(rm); return;
        }
        p = &(*p)->next;
    }
}

void etcp_conn_add_cbk(struct ETCP_CONN* conn, etcp_cbk_fn fn, void* arg, int event_mask) { if (conn) etcp_cbk_add_to_chain(&conn->cbks, fn, arg, event_mask); }
void etcp_conn_remove_cbk(struct ETCP_CONN* conn, etcp_cbk_fn fn, void* arg) {
    if (!conn) return;
    if (!conn->callbacks_running) { etcp_cbk_remove_from_chain(&conn->cbks, fn, arg); return; }
    for (struct etcp_cbk_entry* e = conn->cbks; e; e = e->next) {
        if (e->fn == fn && e->arg == arg) { e->fn = NULL; e->arg = NULL; e->event_mask = 0; return; }
    }
}
void etcp_conn_update_cbk_mask(struct ETCP_CONN* conn, etcp_cbk_fn fn, void* arg, int event_mask) {
    if (!conn || !fn) return;
    struct etcp_cbk_entry* p = conn->cbks;
    while (p) { if (p->fn == fn && p->arg == arg) { p->event_mask = event_mask; return; } p = p->next; }
}

// Instance-level callback chain helpers
static void etcp_inst_cbk_add_chain(struct etcp_inst_cbk_entry** head, etcp_inst_cbk_fn fn, void* arg) {
    if (!head || !fn) return;
    struct etcp_inst_cbk_entry* e = u_malloc(sizeof(struct etcp_inst_cbk_entry));
    if (!e) return;
    e->fn = fn; e->arg = arg; e->next = *head;
    *head = e;
}
static void etcp_inst_cbk_remove_chain(struct etcp_inst_cbk_entry** head, etcp_inst_cbk_fn fn, void* arg, unsigned depth) {
    if (!head || !fn) return;
    struct etcp_inst_cbk_entry** p = head;
    while (*p) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct etcp_inst_cbk_entry* rm = *p;
            if (depth) { rm->fn = NULL; rm->arg = NULL; return; }
            *p = rm->next; u_free(rm); return;
        }
        p = &(*p)->next;
    }
}
void etcp_add_new_conn_cbk(struct UTUN_INSTANCE* inst, etcp_inst_cbk_fn fn, void* arg) { if (inst) etcp_inst_cbk_add_chain(&inst->new_conn_cbks, fn, arg); }
void etcp_remove_new_conn_cbk(struct UTUN_INSTANCE* inst, etcp_inst_cbk_fn fn, void* arg) {
    if (inst) etcp_inst_cbk_remove_chain(&inst->new_conn_cbks, fn, arg, inst->etcp_callbacks_depth);
}

// Link status callback chain
static void etcp_link_status_cbk_add_chain(struct etcp_link_status_cbk_entry** head, etcp_link_status_cbk_fn fn, void* arg) {
    if (!head || !fn) return;
    struct etcp_link_status_cbk_entry* e = u_malloc(sizeof(struct etcp_link_status_cbk_entry));
    if (!e) return;
    e->fn = fn; e->arg = arg; e->next = *head;
    *head = e;
}
static void etcp_link_status_cbk_remove_chain(struct etcp_link_status_cbk_entry** head, etcp_link_status_cbk_fn fn, void* arg, unsigned depth) {
    if (!head || !fn) return;
    struct etcp_link_status_cbk_entry** p = head;
    while (*p) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct etcp_link_status_cbk_entry* rm = *p;
            if (depth) { rm->fn = NULL; rm->arg = NULL; return; }
            *p = rm->next; u_free(rm); return;
        }
        p = &(*p)->next;
    }
}
void etcp_add_link_status_cbk(struct UTUN_INSTANCE* inst, etcp_link_status_cbk_fn fn, void* arg) { if (inst) etcp_link_status_cbk_add_chain(&inst->link_status_cbks, fn, arg); }
void etcp_remove_link_status_cbk(struct UTUN_INSTANCE* inst, etcp_link_status_cbk_fn fn, void* arg) {
    if (inst) etcp_link_status_cbk_remove_chain(&inst->link_status_cbks, fn, arg, inst->etcp_callbacks_depth);
}

void etcp_fire_link_status_cbk(struct ETCP_LINK* link, int old_state, int old_status) {
    if (!link || !link->etcp || !link->etcp->instance) return;
    struct ETCP_CONN* conn = link->etcp;
    struct UTUN_INSTANCE* inst = conn->instance;
    etcp_conn_update_rtt(conn);
    int closing_connection = conn->close_requested;
    int was_attached = 0;
    for (struct ETCP_LINK* current = conn->links; current; current = current->next) if (current == link) { was_attached = 1; break; }
    conn->callbacks_running++;
    link->active_users++;
    inst->etcp_callbacks_depth++;
    if (old_state != link->link_state || old_status != link->link_status) etcp_tx_conn_update_rto(conn);
    struct etcp_link_status_cbk_entry* cbe = conn->instance->link_status_cbks;
    while (cbe) {
        if (cbe->fn) cbe->fn(conn, link, old_state, old_status, cbe->arg);
        if (!closing_connection && conn->close_requested) break;
        if (was_attached) {
            int attached = 0;
            for (struct ETCP_LINK* current = conn->links; current; current = current->next)
                if (current == link) { attached = 1; break; }
            if (!attached) break;
        }
        cbe = cbe->next;
    }
    etcp_link_release(link); conn->callbacks_running--;
    inst->etcp_callbacks_depth--;
    etcp_instance_callbacks_collect(inst);
}

static void etcp_status_cbk_add_chain(struct etcp_status_cbk_entry** head, etcp_conn_status_fn fn, void* arg) {
    if (!head || !fn) return;
    struct etcp_status_cbk_entry* e = u_malloc(sizeof(struct etcp_status_cbk_entry));
    if (!e) return;
    e->fn = fn; e->arg = arg; e->next = *head;
    *head = e;
}
static void etcp_status_cbk_remove_chain(struct etcp_status_cbk_entry** head, etcp_conn_status_fn fn, void* arg, unsigned depth) {
    if (!head || !fn) return;
    struct etcp_status_cbk_entry** p = head;
    while (*p) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct etcp_status_cbk_entry* rm = *p;
            if (depth) { rm->fn = NULL; rm->arg = NULL; return; }
            *p = rm->next; u_free(rm); return;
        }
        p = &(*p)->next;
    }
}
void etcp_add_conn_status_cbk(struct UTUN_INSTANCE* inst, etcp_conn_status_fn fn, void* arg) { if (inst) etcp_status_cbk_add_chain(&inst->conn_status_cbks, fn, arg); }
void etcp_remove_conn_status_cbk(struct UTUN_INSTANCE* inst, etcp_conn_status_fn fn, void* arg) {
    if (inst) etcp_status_cbk_remove_chain(&inst->conn_status_cbks, fn, arg, inst->etcp_callbacks_depth);
}

static void etcp_socket_cbk_add_chain(struct etcp_socket_cbk_entry** head, etcp_socket_cbk_fn fn, void* arg, int event_mask) {
    if (!head || !fn) return;
    struct etcp_socket_cbk_entry* e = u_malloc(sizeof(struct etcp_socket_cbk_entry));
    if (!e) return;
    e->fn = fn; e->arg = arg; e->event_mask = event_mask; e->next = *head;
    *head = e;
}
static void etcp_socket_cbk_remove_chain(struct etcp_socket_cbk_entry** head, etcp_socket_cbk_fn fn, void* arg, unsigned depth) {
    if (!head || !fn) return;
    struct etcp_socket_cbk_entry** p = head;
    while (*p) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct etcp_socket_cbk_entry* rm = *p;
            if (depth) { rm->fn = NULL; rm->arg = NULL; return; }
            *p = rm->next; u_free(rm); return;
        }
        p = &(*p)->next;
    }
}
void etcp_add_socket_cbk(struct UTUN_INSTANCE* inst, etcp_socket_cbk_fn fn, void* arg, int event_mask) { if (inst) etcp_socket_cbk_add_chain(&inst->socket_cbks, fn, arg, event_mask); }
void etcp_remove_socket_cbk(struct UTUN_INSTANCE* inst, etcp_socket_cbk_fn fn, void* arg) {
    if (inst) etcp_socket_cbk_remove_chain(&inst->socket_cbks, fn, arg, inst->etcp_callbacks_depth);
}

void etcp_socket_cbk_fire(struct ETCP_SOCKET* sock, int event) {
    if (!sock || !sock->instance) return;
    struct UTUN_INSTANCE* inst = sock->instance;
    static const char* names[] = { "ADDR_CHANGED", "STATUS_CHANGED" };
    int idx = 0, e = event; while (e >>= 1) idx++;
    const char* name = (idx >= 0 && idx < (int)(sizeof(names)/sizeof(names[0]))) ? names[idx] : "?";
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "socket %s event: %s  if_addr=%s  local=%s  nat=%s  type=%d  netif=%u",
                sock->name, name,
                sockaddr_storage_to_str(&sock->interface_addr).str,
                sockaddr_storage_to_str(&sock->local_addr).str,
                sockaddr_storage_to_str(&sock->nat_addr).str,
                sock->type, sock->netif_index);
    inst->etcp_callbacks_depth++;
    struct etcp_socket_cbk_entry* cbe = inst->socket_cbks;
    while (cbe) {
        struct etcp_socket_cbk_entry* next = cbe->next;
        if (cbe->fn && (cbe->event_mask & event)) cbe->fn(sock, event, cbe->arg);
        cbe = next;
    }
    inst->etcp_callbacks_depth--;
    etcp_instance_callbacks_collect(inst);
}

int etcp_bind(struct UTUN_INSTANCE* inst, uint8_t id, etcp_recv_fn callback) {
    if (!inst || !callback || (unsigned)id >= ETCP_MAX_BINDINGS) return -1;
    inst->api_bindings.callbacks[id] = callback;
    return 0;
}

int etcp_unbind(struct UTUN_INSTANCE* inst, uint8_t id) {
    if (!inst || (unsigned)id >= ETCP_MAX_BINDINGS) return -1;
    inst->api_bindings.callbacks[id] = NULL;
    return 0;
}

int etcp_send(struct ETCP_CONN* conn, struct ll_entry* entry) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_TX_STACK);
    ETCP_PROFILE_MARK(ETCP_PROF_SUBMIT, conn);
    /* Владение entry: при успехе entry уходит в очередь (может быть освобождён
     * синхронно); при ошибке (-1) entry НЕ освобождается — вызывающий владеет. */
    if (!conn || !entry || !conn->send_input_q || conn->state == 2 || conn->close_requested) return -1;
    if (!entry->dgram || !entry->len || entry->len > PKTNORM_MAX_DGRAM_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "etcp_send: invalid datagram conn=%s len=%u max=%u", conn->log_name, entry->len, PKTNORM_MAX_DGRAM_SIZE);
        return -1;
    }
    /* queue_data_put освобождает entry при отказе. Проверяем до передачи владения,
     * чтобы сохранить контракт etcp_send: при -1 entry остаётся у вызывающего. */
    struct ll_queue* q = conn->send_input_q;
    if (q->hash_size || (q->size_limit >= 0 && q->count >= q->size_limit)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "etcp_send: invalid/full input queue conn=%p count=%d limit=%d hash_size=%zu",
            (void*)conn, q->count, q->size_limit, q->hash_size);
        return -1;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_TRAFFIC, "enqueue id=0x%02x conn=%s len=%u pending=%d",
                entry->dgram[0], conn->log_name, entry->len, q->count);
    return queue_data_put(conn->send_input_q, entry);
}

void etcp_int_recv(struct ll_queue* queue, void* arg) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_RX_STACK);
    struct ETCP_CONN* conn = (struct ETCP_CONN*)arg;
    if (!queue || !conn) return;
    struct ll_entry* e = queue_data_get(queue);
    if (!e) { queue_resume_callback(queue); return; }
    if (!e->dgram || e->len == 0) { queue_dgram_free(e); queue_entry_free(e); queue_resume_callback(queue); return; }
    uint8_t id = e->dgram[0];
    struct UTUN_INSTANCE* inst = conn->instance;
    if (!inst) { queue_dgram_free(e); queue_entry_free(e); queue_resume_callback(queue); return; }
    conn->callbacks_running++;
    if (inst->api_bindings.callbacks[id]) {
        DEBUG_TRACE(DEBUG_CATEGORY_TRAFFIC, "dispatch id=0x%02x conn=%s len=%zu", id, conn->log_name, e->len);
        inst->api_bindings.callbacks[id](conn, e);
    } else if (inst->api_bindings.callbacks[ETCP_ID_DATA]) {
        DEBUG_TRACE(DEBUG_CATEGORY_TRAFFIC, "dispatch id=0x%02x (fallback) conn=%s len=%zu", id, conn->log_name, e->len);
        inst->api_bindings.callbacks[ETCP_ID_DATA](conn, e);
    } else { queue_dgram_free(e); queue_entry_free(e); }
    if (!conn->close_requested) queue_resume_callback(queue);
    conn->callbacks_running--;
}
