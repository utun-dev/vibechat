#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif
#include "etcp_api.h"
#include "etcp_connections.h"
#include "etcp.h"
#include "topo_node.h"
#include "topo_group.h"
#include "broadcast.h"

struct broadcast_msg {
    uint64_t local_time_tb;     // offset 0
    uint8_t  uuid[BROADCAST_UUID_SIZE]; // offset 8 — queue index key
    uint16_t data_len;          // offset 24
    uint8_t  data[];            // offset 26
} __attribute__((packed));

#define BMSG_UUID_OFFSET  8

struct broadcast_ctx {
    struct TOPO_GROUP*     group;
    struct ll_queue*       messages;
    struct broadcast_cbk_entry* cbks;
    void*                  cleanup_timer;
    void*                  cleanup_wait;   /* standby_wait handle (Android) */
    uint32_t               send_seq;
};

static void broadcast_cleanup_timer_cb(void* arg);

int broadcast_init(struct TOPO_GROUP* group) {
    if (!group) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "broadcast_init: null group"); return -1; }
    if (group->broadcast) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "broadcast_init: already initialized grp=%016llx", (unsigned long long)group->group_id); return -1; }

    struct broadcast_ctx* ctx = u_calloc(1, sizeof(struct broadcast_ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "broadcast_init: alloc failed"); return -1; }
    ctx->group = group;
    ctx->send_seq = 0;

    ctx->messages = queue_new(group->instance->ua, 64, BMSG_UUID_OFFSET, BROADCAST_UUID_SIZE, "broadcast_msg");
    if (!ctx->messages) { u_free(ctx); DEBUG_ERROR(DEBUG_CATEGORY_BGP, "broadcast_init: queue_new failed"); return -1; }

    ctx->cleanup_timer = uasync_set_timeout(group->instance->ua, BROADCAST_CLEANUP_INTERVAL_TB, ctx, broadcast_cleanup_timer_cb, "broadcast_cleanup_timer");
    if (!ctx->cleanup_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "broadcast_init: cleanup_timer failed grp=%016llx", (unsigned long long)group->group_id);
        queue_free(ctx->messages); u_free(ctx);
        return -1;
    }
    group->broadcast = ctx;

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "broadcast_init ok grp=%016llx", (unsigned long long)group->group_id);
    return 0;
}

void broadcast_destroy(struct TOPO_GROUP* group) {
    if (!group || !group->broadcast) return;
    struct broadcast_ctx* ctx = group->broadcast;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "broadcast_destroy grp=%016llx", (unsigned long long)group->group_id);

    if (ctx->cleanup_timer) { uasync_cancel_timeout(group->instance->ua, ctx->cleanup_timer); ctx->cleanup_timer = NULL; }
#ifdef UTUN_HAVE_STANDBY
    if (ctx->cleanup_wait) { standby_wait_cancel(ctx->cleanup_wait); ctx->cleanup_wait = NULL; }
#endif

    while (ctx->cbks) { struct broadcast_cbk_entry* c = ctx->cbks; ctx->cbks = c->next; u_free(c); }

    if (ctx->messages) {
        struct ll_entry* e;
        while ((e = queue_data_get(ctx->messages)) != NULL) queue_entry_free(e);
        queue_free(ctx->messages); ctx->messages = NULL;
    }

    u_free(ctx); group->broadcast = NULL;
}

/* Одна и та же кодограмма для первичной отправки и ретрансляции. */
static void broadcast_send_peer(struct ETCP_CONN* conn, const uint8_t key[32], const uint8_t* uuid,
        const uint8_t* data, uint16_t data_len) {
    uint16_t total = BROADCAST_HEADER_SIZE + data_len;
    struct ll_entry* entry = ll_alloc_lldgram(total);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "broadcast: send allocation bytes=%u", total); return; }
    uint8_t* packet = entry->dgram;
    packet[0] = ETCP_ID_BROADCAST;
    memcpy(packet + BROADCAST_GROUP_OFF, key, 32);
    memcpy(packet + BROADCAST_PAYLOAD_OFF, uuid, BROADCAST_UUID_SIZE);
    packet[BROADCAST_HEADER_SIZE - 2] = (uint8_t)data_len;
    packet[BROADCAST_HEADER_SIZE - 1] = (uint8_t)(data_len >> 8);
    memcpy(packet + BROADCAST_HEADER_SIZE, data, data_len);
    entry->len = total;
    if (etcp_send(conn, entry)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "broadcast: send rejected peer=%s bytes=%u", conn->log_name, total);
        queue_dgram_free(entry); queue_entry_free(entry);
    }
}

int broadcast_send(struct TOPO_GROUP* group, const uint8_t* data, uint16_t data_len) {
    if (!group || !group->broadcast || !data || data_len > BROADCAST_MAX_DATA) return -1;
    if (!topo_group_data_allowed(group, NULL)) return -1;
    const uint8_t* key = topo_group_wire_id(group);
    if (!key) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "broadcast: group key unavailable ch=%s", group->channel_id); return -1; }
    struct broadcast_ctx* ctx = group->broadcast;

    uint8_t uuid[BROADCAST_UUID_SIZE];
    {
        uint64_t tb = get_time_tb();
        uint32_t seq = ctx->send_seq++;
        uint64_t ptr = (uint64_t)(uintptr_t)ctx;
        memset(uuid, 0, BROADCAST_UUID_SIZE);
        memcpy(uuid, &tb, 8); memcpy(uuid + 8, &seq, 4); memcpy(uuid + 12, &ptr, 4);
    }

    {
        size_t sz = 26 + (size_t)data_len;  // 8(time) + 16(uuid) + 2(data_len) + data
        struct ll_entry* e = queue_entry_new(sz);
        if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "broadcast_send: alloc failed"); return -1; }
        struct broadcast_msg* msg = (struct broadcast_msg*)e->data;
        msg->local_time_tb = get_time_tb();
        memcpy(msg->uuid, uuid, BROADCAST_UUID_SIZE);
        msg->data_len = data_len;
        memcpy(msg->data, data, data_len);
        queue_data_put_with_index(ctx->messages, e);
    }

    if (group->senders_list) {
        struct ll_entry* se = group->senders_list->head;
        while (se) {
            struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)se->data;
            if (item && item->conn && topo_group_data_allowed(group, item->conn)) {
                broadcast_send_peer(item->conn, key, uuid, data, data_len);
            }
            se = se->next;
        }
    }
    return 0;
}

void broadcast_add_cbk(struct TOPO_GROUP* group, broadcast_recv_fn fn, void* arg) {
    if (!group || !group->broadcast || !fn) return;
    struct broadcast_ctx* ctx = group->broadcast;
    struct broadcast_cbk_entry* e = u_malloc(sizeof(*e));
    if (!e) return;
    e->fn = fn; e->arg = arg;
    e->next = ctx->cbks;
    ctx->cbks = e;
}

void broadcast_remove_cbk(struct TOPO_GROUP* group, broadcast_recv_fn fn, void* arg) {
    if (!group || !group->broadcast || !fn) return;
    struct broadcast_ctx* ctx = group->broadcast;
    struct broadcast_cbk_entry** p = &ctx->cbks;
    while (*p) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct broadcast_cbk_entry* r = *p;
            *p = r->next; u_free(r);
            return;
        }
        p = &(*p)->next;
    }
}

static void broadcast_fire_cbks(struct broadcast_ctx* ctx, struct ETCP_CONN* from_conn,
                                const uint8_t* uuid, const uint8_t* data, uint16_t data_len) {
    struct broadcast_cbk_entry* c = ctx->cbks;
    while (c) {
        if (!topo_group_data_allowed(ctx->group, from_conn)) return;
        c->fn(uuid, data, data_len, c->arg); c = c->next;
    }
}

static void broadcast_cleanup_timer_cb(void* arg) {
    struct broadcast_ctx* ctx = (struct broadcast_ctx*)arg;
    if (!ctx || !ctx->group) return;
    ctx->cleanup_timer = NULL;
#ifdef UTUN_HAVE_STANDBY
    if (standby_get_sleep_tb() > 0) {
        ctx->cleanup_wait = standby_wait(ctx, broadcast_cleanup_timer_cb);
        return;
    }
    ctx->cleanup_wait = NULL;
#endif
    uint64_t now_tb = get_time_tb();

    int removed = 0;
    while (ctx->messages->head) {
        struct ll_entry* e = ctx->messages->head;
        struct broadcast_msg* msg = (struct broadcast_msg*)e->data;
        if (now_tb - msg->local_time_tb <= BROADCAST_TTL_TB) break;
        queue_entry_free(queue_data_get(ctx->messages));
        removed++;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "broadcast_cleanup: grp=%016llx removed=%d cached=%d",
                (unsigned long long)ctx->group->group_id, removed, queue_entry_count(ctx->messages));

    ctx->cleanup_timer = uasync_set_timeout(ctx->group->instance->ua, BROADCAST_CLEANUP_INTERVAL_TB, ctx, broadcast_cleanup_timer_cb, "broadcast_cleanup_timer");
    if (!ctx->cleanup_timer)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "broadcast_cleanup: timer rearm failed grp=%016llx", (unsigned long long)ctx->group->group_id);
}

void broadcast_recv(struct TOPO_GROUP* group, struct ETCP_CONN* from_conn, const uint8_t* payload, size_t payload_len) {
    if (!group || !group->broadcast || !payload || payload_len < BROADCAST_UUID_SIZE + 2) return;
    if (!topo_group_data_allowed(group, from_conn)) return;
    const uint8_t* key = topo_group_wire_id(group);
    if (!key) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "broadcast: received for group without key ch=%s", group->channel_id); return; }
    struct broadcast_ctx* ctx = group->broadcast;

    const uint8_t* uuid = payload;
    uint16_t data_len = (uint16_t)payload[BROADCAST_UUID_SIZE] | ((uint16_t)payload[BROADCAST_UUID_SIZE + 1] << 8);
    const uint8_t* data = payload + BROADCAST_UUID_SIZE + 2;
    if (data_len > BROADCAST_MAX_DATA || BROADCAST_UUID_SIZE + 2 + (size_t)data_len != payload_len) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "broadcast_recv: invalid size data_len=%u payload_len=%zu", data_len, payload_len);
        return;
    }

    if (queue_find_data_by_index(ctx->messages, uuid) != NULL) return;

    {
        size_t sz = 26 + (size_t)data_len;
        struct ll_entry* e = queue_entry_new(sz);
        if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "broadcast_recv: alloc failed"); return; }
        struct broadcast_msg* msg = (struct broadcast_msg*)e->data;
        msg->local_time_tb = get_time_tb();
        memcpy(msg->uuid, uuid, BROADCAST_UUID_SIZE);
        msg->data_len = data_len;
        memcpy(msg->data, data, data_len);
        queue_data_put_with_index(ctx->messages, e);
    }

    if (group->senders_list) {
        struct ll_entry* se = group->senders_list->head;
        while (se) {
            struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)se->data;
            if (item && item->conn && item->conn != from_conn && topo_group_data_allowed(group, item->conn)) {
                broadcast_send_peer(item->conn, key, uuid, data, data_len);
            }
            se = se->next;
        }
    }

    broadcast_fire_cbks(ctx, from_conn, uuid, data, data_len);
}

static void broadcast_recv_dispatcher(struct ETCP_CONN* from_conn, struct ll_entry* entry) {
    if (!entry) return;
    if (!from_conn || !from_conn->instance || !entry->dgram || entry->len < BROADCAST_HEADER_SIZE ||
        entry->dgram[0] != ETCP_ID_BROADCAST) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "broadcast: invalid frame bytes=%u", entry->len);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    const uint8_t* key = entry->dgram + BROADCAST_GROUP_OFF;
    struct TOPO_GROUP* group = topo_groups_find_wire(from_conn->instance->topo_groups, key);
    if (group) broadcast_recv(group, from_conn, entry->dgram + BROADCAST_PAYLOAD_OFF, entry->len - BROADCAST_PAYLOAD_OFF);
    else DEBUG_WARN(DEBUG_CATEGORY_BGP, "broadcast: unknown group key=%s peer=%s", sc_public_key_hex(key, (char[65]){0}), from_conn->log_name);
    queue_dgram_free(entry); queue_entry_free(entry);
}

int broadcast_init_instance(struct UTUN_INSTANCE* instance) {
    if (!instance) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "broadcast_init_instance: binding ETCP_ID_BROADCAST");
    return etcp_bind(instance, ETCP_ID_BROADCAST, broadcast_recv_dispatcher);
}

void broadcast_destroy_instance(struct UTUN_INSTANCE* instance) {
    if (!instance) return;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "broadcast_destroy_instance: unbinding ETCP_ID_BROADCAST");
    etcp_unbind(instance, ETCP_ID_BROADCAST);
}
