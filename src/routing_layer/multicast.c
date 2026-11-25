#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "multicast.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "route_crypto.h"
#include "topo_group.h"
#include "../chat/group_policy.h"
#include "utun_instance.h"

#define MC_CAT DEBUG_CATEGORY_ETCPROUTE
/* Каждый допустимый список обязан помещаться в один подписанный и зашифрованный отчёт. */
#define MC_MAX_RECIPIENTS ((ROUTER_SIGNED_ENCRYPTED_LIMIT - MULTICAST_REPORT_HEADER_SIZE) / MULTICAST_KEY_SIZE)

struct mc_packet {
    uint64_t group;
    const uint8_t* group_key;
    struct multicast_key source;
    uint16_t descriptor, count;
    const struct multicast_key* recipients;
    const uint8_t* payload;
    struct multicast_key visited[MULTICAST_MAX_VISITED];
    size_t payload_len;
    uint8_t hops;
};

struct mc_out {
    struct mc_out* next;
    struct multicast_ctx* ctx;
    struct ETCP_CONN* conn;       /* заимствован до DELETE; затем NULL до опустошения FIFO */
    struct ll_queue* q;
    struct queue_waiter_handle waiter;
};

struct mc_window {
    struct mc_window* next;
    uint64_t group, deadline;
    struct multicast_key source, dst;
    uint16_t descriptor;
    int dirty;                   /* есть потери после первого немедленного отчёта */
};

struct mc_report {
    struct mc_report* next;
    struct multicast_ctx* ctx;
    struct multicast_loss loss;
    uint64_t due, retry;          /* неизменный конец окна / следующая попытка передачи */
    uint8_t received;             /* входящий отчёт: только отложенный callback, без повторной отправки */
    struct queue_waiter_handle waiter;
    struct multicast_key ids[];
};

struct mc_descriptor {
    struct ll_entry ll;
    uint16_t id;                  /* хеш-ключ, первые два байта data[] */
    uint64_t group;
    multicast_loss_fn on_loss;
    void* arg;
};

struct multicast_subscription {
    struct ll_entry ll;
    uint8_t key[8 + MULTICAST_KEY_SIZE + 2];              /* group:8, source:32, descriptor:2; little-endian */
    struct multicast_ctx* ctx;    /* NULL после unsubscribe или удаления группы/ядра */
    multicast_recv_fn on_data;
    void* arg;
    unsigned running;            /* удерживает handle, если он закрыт из своего callback */
    int released;                /* вызывающий уже вызвал unsubscribe */
};

struct mc_delivery {
    struct ll_entry ll;
    struct multicast_subscription* subscription;
    struct multicast_key source, via;        /* source — объявленный автор; via — принявший пакет сосед, self для loopback */
    uint16_t descriptor;         /* нужен для отмены loopback при close исходящего дескриптора */
};

struct multicast_ctx {
    struct UTUN_INSTANCE* inst;
    struct mc_out* outputs;
    struct mc_window* windows;
    struct mc_report* reports;
    void *report_timer, *route_wake;
    uint64_t eviction_warn;
    struct multicast_stats stats;
    struct ll_queue *descriptors, *subscriptions, *deliveries;
    uint16_t last_descriptor;
};

struct mc_target { struct ETCP_CONN* conn; struct multicast_key id; };

static void mc_report_tick(void* arg);
static void mc_report_schedule(struct multicast_ctx* ctx);
static void mc_forward(struct multicast_ctx* ctx, struct ll_entry* e, struct mc_out* previous);
static void mc_out_drain(struct ll_queue* q, void* arg);
static void mc_loss(struct multicast_ctx* ctx, const struct mc_packet* p, struct multicast_key dst,
                    enum multicast_loss_reason reason, const struct mc_target* targets, unsigned count);

static uint64_t mc_read(const uint8_t* p, unsigned bytes) {
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static void mc_write(uint8_t* p, uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static struct multicast_key mc_key(const uint8_t* ed) {
    struct multicast_key key;
    memcpy(key.ed, ed, sizeof(key.ed));
    return key;
}

static int mc_equal(struct multicast_key a, struct multicast_key b) {
    return !memcmp(a.ed, b.ed, MULTICAST_KEY_SIZE);
}

static const struct multicast_key mc_zero;

/* На границе с BGP полный адрес проверяется даже при коллизии short31. */
static struct TOPO_GROUP_NODE* mc_node(struct TOPO_GROUP* group, struct multicast_key key) {
    return topo_node_find_by_ed(group, key.ed);
}

static struct ETCP_CONN* mc_route(struct TOPO_GROUP* group, struct multicast_key key) {
    struct TOPO_NODEPATH* path = topo_node_best_path(mc_node(group, key));
    return path ? path->conn : NULL;
}

static int mc_allowed(struct multicast_ctx* ctx, uint64_t id, struct multicast_key peer) {
    struct TOPO_GROUP* g = topo_groups_find(ctx->inst->topo_groups, id);
    if (!g || g->stopping || mc_equal(peer, mc_zero)) return 0;
    if (g->group_type == TOPO_GROUP_TYPE_UTUN) return 1;
    uint8_t x[32];
    if (sc_identity_public_x("multicast", peer.ed, x) == SC_OK &&
        group_policy_member_key_allowed(ctx->inst, g->channel_id, ctx->inst->my_keys.public_key, ctx->inst->my_ed25519_pubkey, 0) &&
        group_policy_member_key_allowed(ctx->inst, g->channel_id, x, peer.ed, 0)) return 1;
    DEBUG_WARN(MC_CAT, "multicast access denied group=%016llx member=%s",
               (unsigned long long)id, sc_public_key_hex(peer.ed, (char[65]){0}));
    return 0;
}

static void mc_free(struct ll_entry* e) { queue_dgram_free(e); queue_entry_free(e); }

/* DATA: фиксированный заголовок и ключи адресатов, count переходов, адреса пути, payload. */
static int mc_parse(struct multicast_ctx* ctx, const struct ll_entry* e, struct mc_packet* p) {
    if (!e || !e->dgram || e->len <= MULTICAST_HEADER_SIZE || e->len > MULTICAST_MAX_DGRAM_SIZE) return -1;
    const uint8_t* w = e->dgram;
    p->group_key = w + MULTICAST_GROUP_OFF;
    struct TOPO_GROUP* g = topo_groups_find_wire(ctx->inst->topo_groups, p->group_key);
    if (!g) {
        DEBUG_WARN(MC_CAT, "multicast unknown group key=%s", sc_public_key_hex(p->group_key, (char[65]){0})); return -1;
    }
    p->group = g->group_id; p->descriptor = mc_read(w + MULTICAST_DESCRIPTOR_OFF, 2);
    p->count = mc_read(w + MULTICAST_COUNT_OFF, 2); p->source = mc_key(w + MULTICAST_SOURCE_OFF);
    size_t head = MULTICAST_HEADER_SIZE + (size_t)p->count * MULTICAST_KEY_SIZE;
    if (w[0] != ETCP_ID_MULTICAST || mc_equal(p->source, mc_zero) || !p->descriptor || !p->count ||
        p->count > MC_MAX_RECIPIENTS || head >= e->len) return -1;
    p->recipients = (const struct multicast_key*)(w + MULTICAST_HEADER_SIZE);
    p->hops = w[head];
    if (!p->hops || p->hops > MULTICAST_MAX_VISITED) return -1;
    const uint8_t* cursor = w + head + 1;
    for (unsigned i = 0; i < p->hops; i++) {
        struct topo_wire_address addr;
        int n = topo_address_read(cursor, (size_t)(w + e->len - cursor), TOPO_ADDRESS_COMPACT, &addr);
        if (n < 0) return -1;
        if (addr.ed) p->visited[i] = mc_key(addr.ed);
        else {
            struct TOPO_GROUP_NODE* node = topo_address_find(g, addr.short31, NULL);
            if (!node) {
                DEBUG_WARN(MC_CAT, "multicast unresolved hop group=%016llx short=%08x", (unsigned long long)p->group, addr.short31);
                return -1;
            }
            p->visited[i] = mc_key(node->ed25519);
        }
        cursor += n;
    }
    p->payload = cursor;
    p->payload_len = (size_t)(w + e->len - cursor);
    return head + p->payload_len + MULTICAST_PATH_RESERVE <= MULTICAST_MAX_DGRAM_SIZE ? 0 : -1;
}

static int mc_has_visited(const struct mc_packet* p, struct multicast_key key) {
    for (unsigned i = 0; i < p->hops; i++) if (mc_equal(p->visited[i], key)) return 1;
    return 0;
}

static int mc_ids_valid(const struct multicast_key* ids, uint16_t count) {
    if (!count || mc_equal(ids[0], mc_zero)) return 0;
    for (unsigned i = 1; i < count; i++)
        if (memcmp(ids[i-1].ed, ids[i].ed, MULTICAST_KEY_SIZE) >= 0) return 0;
    return 1;
}

/* В FIFO остаются полные ключи. Изменение индекса short31 не может изменить полученный путь. */
static struct ll_entry* mc_build(const struct mc_packet* p, const struct mc_target* targets, unsigned count) {
    size_t head = MULTICAST_HEADER_SIZE + count * MULTICAST_KEY_SIZE;
    size_t path_len = p->hops * (1 + MULTICAST_KEY_SIZE);
    size_t len = head + 1 + path_len + p->payload_len;
    if (len > MULTICAST_MAX_DGRAM_SIZE) { DEBUG_ERROR(MC_CAT, "multicast packet too large bytes=%zu", len); return NULL; }
    struct ll_entry* e = ll_alloc_lldgram((uint16_t)len);
    if (!e) { DEBUG_ERROR(MC_CAT, "multicast packet allocation failed bytes=%zu recipients=%u", len, count); return NULL; }
    e->len = len;
    uint8_t* w = e->dgram;
    w[0] = ETCP_ID_MULTICAST; memcpy(w + MULTICAST_GROUP_OFF, p->group_key, 32);
    mc_write(w + MULTICAST_DESCRIPTOR_OFF, p->descriptor, 2);
    mc_write(w + MULTICAST_COUNT_OFF, count, 2); memcpy(w + MULTICAST_SOURCE_OFF, p->source.ed, MULTICAST_KEY_SIZE);
    for (unsigned i = 0; i < count; i++) memcpy(w + MULTICAST_HEADER_SIZE + i * MULTICAST_KEY_SIZE, targets[i].id.ed, MULTICAST_KEY_SIZE);
    w[head] = p->hops;
    uint8_t* cursor = w + head + 1;
    for (unsigned i = 0; i < p->hops; i++) {
        *cursor++ = 0x80; memcpy(cursor, p->visited[i].ed, MULTICAST_KEY_SIZE); cursor += MULTICAST_KEY_SIZE;
    }
    if (p->payload_len) memcpy(cursor, p->payload, p->payload_len);
    return e;
}

/* Заимствованный дескриптор; callback может закрыть его, после callback не разыменовывать. */
static struct mc_descriptor* mc_descriptor_find(struct multicast_ctx* ctx, uint16_t id) {
    return (struct mc_descriptor*)queue_find_data_by_index(ctx->descriptors, &id);
}

/* Доставка извлечена до callback: unsubscribe может отменить остальные и освободить arg. */
static void mc_deliver(struct ll_queue* q, void* arg) {
    struct multicast_ctx* ctx = arg;
    struct mc_delivery* d = (struct mc_delivery*)queue_data_get(q);
    if (!d) { queue_resume_callback(q); return; }
    struct multicast_subscription* sub = d->subscription;
    ctx->stats.delivery_bytes -= sizeof(*d) + d->ll.len;
    uint64_t group = mc_read(sub->key, 8);
    if (!mc_allowed(ctx, group, d->source) || !mc_allowed(ctx, group, d->via)) {
        mc_free(&d->ll); queue_resume_callback(q); return;
    }
    sub->running++;
    ctx->stats.delivered++;
    sub->on_data(sub, d->ll.dgram, d->ll.len, sub->arg);
    sub->running--;
    if (sub->released) queue_entry_free(&sub->ll);
    mc_free(&d->ll); queue_resume_callback(q);
}

/* Подписка фиксируется в момент приёма: новая подписка не получит старые ожидающие DATA. */
static void mc_deliver_later(struct multicast_ctx* ctx, const struct mc_packet* p) {
    uint8_t key[8 + MULTICAST_KEY_SIZE + 2];
    mc_write(key, p->group, 8); memcpy(key + 8, p->source.ed, MULTICAST_KEY_SIZE); mc_write(key + 8 + MULTICAST_KEY_SIZE, p->descriptor, 2);
    struct multicast_subscription* sub = (struct multicast_subscription*)queue_find_data_by_index(ctx->subscriptions, key);
    if (!sub) { ctx->stats.unsubscribed++; return; }
    size_t bytes = sizeof(struct mc_delivery) + p->payload_len;
    if (ctx->stats.delivery_bytes + bytes > MULTICAST_QUEUE_BYTES) goto failed;
    struct mc_delivery* d = (struct mc_delivery*)queue_entry_new(sizeof(*d) - sizeof(struct ll_entry));
    if (!d) goto failed;
    if (p->payload_len) {
        d->ll.dgram = u_malloc(p->payload_len);
        if (!d->ll.dgram) { queue_entry_free(&d->ll); goto failed; }
        memcpy(d->ll.dgram, p->payload, p->payload_len);
    }
    d->ll.len = p->payload_len; d->subscription = sub; d->source = p->source; d->descriptor = p->descriptor;
    d->via = p->hops > 1 ? p->visited[p->hops - 2] : mc_key(ctx->inst->my_ed25519_pubkey);
    ctx->stats.delivery_bytes += bytes; queue_data_put(ctx->deliveries, &d->ll); return;
failed:
    DEBUG_ERROR(MC_CAT, "multicast local delivery unavailable pending=%zu need=%zu source=%016llx descriptor=%u",
                ctx->stats.delivery_bytes, bytes, (unsigned long long)mc_read(p->source.ed, 8), p->descriptor);
    struct mc_target target = { NULL, mc_key(ctx->inst->my_ed25519_pubkey) };
    mc_loss(ctx, p, mc_key(ctx->inst->my_ed25519_pubkey), MULTICAST_INTERNAL_ERROR, &target, 1);
}

/* Удаление по handle или локальному исходящему дескриптору; активный callback уже извлечён. */
static void mc_cancel_deliveries(struct multicast_ctx* ctx, struct multicast_subscription* sub, uint16_t descriptor) {
    for (struct ll_entry* e = ctx->deliveries->head; e;) {
        struct ll_entry* next = e->next;
        struct mc_delivery* d = (struct mc_delivery*)e;
        if ((sub && d->subscription == sub) || (descriptor && mc_equal(d->source, mc_key(ctx->inst->my_ed25519_pubkey)) && d->descriptor == descriptor)) {
            queue_remove_data(ctx->deliveries, e); ctx->stats.delivery_bytes -= sizeof(*d) + e->len; mc_free(e);
        }
        e = next;
    }
}

/* Отцепить принадлежащую приложению подписку, сохранив handle до unsubscribe. */
static void mc_subscription_detach(struct multicast_subscription* sub) {
    if (!sub->ctx) return;
    struct multicast_ctx* ctx = sub->ctx;
    mc_cancel_deliveries(ctx, sub, 0);
    queue_remove_data(ctx->subscriptions, &sub->ll); sub->ctx = NULL;
    DEBUG_DEBUG(MC_CAT, "multicast subscription detached source=%016llx descriptor=%u",
                (unsigned long long)mc_read(sub->key + 8, 8), (unsigned)mc_read(sub->key + 8 + MULTICAST_KEY_SIZE, 2));
}

/* Запись всегда удаляется вместе с waiter, в том числе уже запланированным. */
static void mc_report_remove(struct multicast_ctx* ctx, struct mc_report* r) {
    struct mc_report** pp = &ctx->reports;
    while (*pp != r) pp = &(*pp)->next;
    *pp = r->next;
    if (r->waiter.internal || r->waiter.call_soon_id) queue_waiter_cancel(r->waiter.defer_q, &r->waiter);
    ctx->stats.report_bytes -= sizeof(*r) + (size_t)r->loss.recipient_count * MULTICAST_KEY_SIZE;
    u_free(r);
}

static void mc_window_remove(struct multicast_ctx* ctx, struct mc_window** pp) {
    struct mc_window* w = *pp;
    *pp = w->next; ctx->stats.report_bytes -= sizeof(*w); u_free(w);
}

/* Общий жёсткий бюджет включает сами записи, их ID и состояние троттлинга. */
static int mc_report_room(struct multicast_ctx* ctx, size_t bytes) {
    if (bytes > MULTICAST_REPORT_BYTES) {
        DEBUG_ERROR(MC_CAT, "multicast report exceeds budget bytes=%zu limit=%u", bytes, MULTICAST_REPORT_BYTES); return -1;
    }
    while (ctx->stats.report_bytes + bytes > MULTICAST_REPORT_BYTES) {
        if (ctx->reports) {
            ctx->stats.reports_evicted++;
            mc_report_remove(ctx, ctx->reports);
        } else if (ctx->windows) mc_window_remove(ctx, &ctx->windows);
        else break;
        uint64_t now = get_time_tb();
        if (now >= ctx->eviction_warn) {
            DEBUG_WARN(MC_CAT, "multicast report budget exhausted bytes=%zu need=%zu evicted=%llu",
                       ctx->stats.report_bytes, bytes, (unsigned long long)ctx->stats.reports_evicted);
            ctx->eviction_warn = now + MULTICAST_REPORT_INTERVAL_TB;
        }
    }
    return 0;
}

/* Сохраняет потерю до освобождения пакета. Удалённому источнику сообщается только OVERLOAD. */
static void mc_loss(struct multicast_ctx* ctx, const struct mc_packet* p, struct multicast_key dst,
                    enum multicast_loss_reason reason, const struct mc_target* targets, unsigned count) {
    if (!mc_allowed(ctx, p->group, p->source)) return;
    if (reason == MULTICAST_OVERLOAD) ctx->stats.overload_packets++;
    else if (reason == MULTICAST_NO_ROUTE) ctx->stats.no_route_packets++;
    else if (reason == MULTICAST_PATH_LOOP) ctx->stats.loop_packets++;
    else ctx->stats.internal_errors++;
    if (reason != MULTICAST_OVERLOAD) {
        DEBUG_WARN(MC_CAT, "multicast drop group=%016llx source=%016llx descriptor=%u dst=%016llx recipients=%u reason=%d",
                   (unsigned long long)p->group, (unsigned long long)mc_read(p->source.ed, 8), p->descriptor,
                   (unsigned long long)mc_read(dst.ed, 8), count, reason);
        if (!mc_equal(p->source, mc_key(ctx->inst->my_ed25519_pubkey))) return;
    }
    uint64_t now = get_time_tb(), due = now;
    struct mc_window* window = NULL;
    if (reason == MULTICAST_OVERLOAD) {
        for (window = ctx->windows; window; window = window->next)
            if (window->group == p->group && mc_equal(window->source, p->source) &&
                window->descriptor == p->descriptor && mc_equal(window->dst, dst)) break;
        if (window) {
            /* Просроченное окно сначала завершит таймер; поздний пакет относится к новому окну. */
            if (window->deadline <= now) window->deadline = now + MULTICAST_REPORT_INTERVAL_TB;
            due = window->deadline; window->dirty = 1;
        }
    }
    for (struct mc_report* r = ctx->reports; r; r = r->next) {
        if (r->received || r->due != due || r->loss.group_id != p->group || !mc_equal(r->loss.source, p->source) ||
            !mc_equal(r->loss.dst, dst) || r->loss.descriptor != p->descriptor || r->loss.reason != reason ||
            r->loss.recipient_count != count || r->loss.packets == UINT32_MAX) continue;
        unsigned i = 0;
        while (i < count && mc_equal(r->ids[i], targets[i].id)) i++;
        if (i == count) { r->loss.packets++; mc_report_schedule(ctx); return; }
    }
    size_t bytes = sizeof(struct mc_report) + count * MULTICAST_KEY_SIZE;
    int new_window = reason == MULTICAST_OVERLOAD && !window;
    if (mc_report_room(ctx, bytes + (new_window ? sizeof(struct mc_window) : 0)) < 0) return;
    /* room может вытеснить окно; после него старый указатель больше не используется. */
    if (new_window) {
        window = u_calloc(1, sizeof(*window));
        if (!window) { DEBUG_ERROR(MC_CAT, "multicast throttle allocation failed"); return; }
        window->group = p->group; window->source = p->source; window->dst = dst; window->descriptor = p->descriptor;
        window->deadline = now + MULTICAST_REPORT_INTERVAL_TB;
        struct mc_window** tail = &ctx->windows;
        while (*tail) tail = &(*tail)->next;
        *tail = window; ctx->stats.report_bytes += sizeof(*window);
    }
    struct mc_report* r = u_calloc(1, bytes);
    if (!r) {
        DEBUG_ERROR(MC_CAT, "multicast loss allocation failed bytes=%zu", bytes);
        mc_report_schedule(ctx); /* Созданное окно должно истечь даже при отказе выделения отчёта. */
        return;
    }
    r->ctx = ctx; r->due = due; r->retry = due;
    r->loss = (struct multicast_loss){ .group_id = p->group, .source = p->source,
        .src = mc_key(ctx->inst->my_ed25519_pubkey), .dst = dst, .descriptor = p->descriptor, .recipient_count = count,
        .recipients = r->ids, .packets = 1, .reason = reason, .prefix_len = p->payload_len < 16 ? p->payload_len : 16 };
    if (r->loss.prefix_len) memcpy(r->loss.prefix, p->payload, r->loss.prefix_len);
    for (unsigned i = 0; i < count; i++) r->ids[i] = targets[i].id;
    struct mc_report** tail = &ctx->reports;
    while (*tail) tail = &(*tail)->next;
    *tail = r; ctx->stats.report_bytes += bytes;
    mc_report_schedule(ctx);
}

/* Освобождение router-очереди только будит общий таймер; нет рекурсивной отправки. */
static void mc_report_ready(struct ll_queue* q, void* arg) {
    (void)q;
    struct mc_report* r = arg;
    r->retry = get_time_tb(); mc_report_schedule(r->ctx);
}

/* Возвращает 1 после передачи отчёта, 0 при backpressure, -1 при ошибке отправки. */
static int mc_report_send(struct multicast_ctx* ctx, struct mc_report* r) {
    if (!mc_allowed(ctx, r->loss.group_id, r->loss.source) || !mc_allowed(ctx, r->loss.group_id, r->loss.src)) return 1;
    if (mc_equal(r->loss.source, mc_key(ctx->inst->my_ed25519_pubkey))) {
        struct mc_descriptor* d = mc_descriptor_find(ctx, r->loss.descriptor);
        if (!d || d->group != r->loss.group_id) return 1; /* дескриптор уже закрыт */
        DEBUG_WARN(MC_CAT, "multicast local loss descriptor=%u dst=%016llx reason=%d recipients=%u packets=%u",
                   r->loss.descriptor, (unsigned long long)mc_read(r->loss.dst.ed, 8), r->loss.reason, r->loss.recipient_count, r->loss.packets);
        if (!r->received) { ctx->stats.reports_sent++; ctx->stats.reports_received++; }
        d->on_loss(ctx->inst, &r->loss, d->arg);
        return 1;
    }
    struct TOPO_GROUP* group = topo_groups_find(ctx->inst->topo_groups, r->loss.group_id);
    struct TOPO_GROUP_NODE* node = mc_node(group, r->loss.source);
    if (!node) { DEBUG_WARN(MC_CAT, "multicast report source has no route descriptor=%u", r->loss.descriptor); return -1; }
    struct ETCP_ROUTER_CONN* channel = etcp_router_conn_get_key(ctx->inst, r->loss.group_id, r->loss.source.ed, ETCP_RT_ID_MULTICAST_REPORT);
    if (!channel) {
        DEBUG_ERROR(MC_CAT, "multicast report channel allocation failed source=%016llx",
                    (unsigned long long)mc_read(r->loss.source.ed, 8)); return -1;
    }
    int mode = group->group_type == TOPO_GROUP_TYPE_UTUN ? 0 : ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT;
    if (!etcp_router_conn_send_q_has_room(channel)) {
        etcp_router_conn_on_send_ready(channel, &r->waiter, mc_report_ready, r);
        return 0;
    }
    unsigned count = r->loss.recipient_count;
    size_t size = MULTICAST_REPORT_HEADER_SIZE + count * MULTICAST_KEY_SIZE;
    uint8_t* w = u_malloc(size);
    if (!w) { DEBUG_ERROR(MC_CAT, "multicast report packet allocation failed recipients=%u", count); return -1; }
    mc_write(w, r->loss.descriptor, 2); memcpy(w + 2, r->loss.src.ed, 32); memcpy(w + 34, r->loss.dst.ed, 32);
    mc_write(w + 66, r->loss.packets, 4); mc_write(w + 70, count, 2); w[72] = r->loss.prefix_len;
    memcpy(w + 73, r->loss.prefix, 16);
    memcpy(w + MULTICAST_REPORT_HEADER_SIZE, r->ids, count * MULTICAST_KEY_SIZE);
    int rc = etcp_router_conn_send(channel, w, size, mode);
    u_free(w);
    if (rc < 0) {
        DEBUG_WARN(MC_CAT, "multicast report send failed source=%016llx descriptor=%u",
                   (unsigned long long)mc_read(r->loss.source.ed, 8), r->loss.descriptor); return -1;
    }
    ctx->stats.reports_sent++;
    return 1;
}

/* Фиксированные окна заканчиваются независимо от нового трафика; blocked-отчёты не смешиваются с новыми. */
static void mc_report_tick(void* arg) {
    struct multicast_ctx* ctx = arg;
    ctx->report_timer = NULL;
    uint64_t now = get_time_tb();
    for (struct mc_window** pp = &ctx->windows; *pp;) {
        struct mc_window* w = *pp;
        if (w->deadline > now) { pp = &w->next; continue; }
        if (!w->dirty) { mc_window_remove(ctx, pp); continue; }
        w->dirty = 0; w->deadline = now + MULTICAST_REPORT_INTERVAL_TB; pp = &w->next;
    }
    unsigned processed = 0;
    for (;;) {
        struct mc_report* r = ctx->reports;
        while (r && r->retry > now) r = r->next;
        if (!r || processed++ == 16) break;
        /* Callback приложения может отправлять новые пакеты: не сохраняем курсор списка через него.
         * Временно снимаем запись с бюджета/списка, чтобы eviction не освободил её из callback. */
        if (mc_equal(r->loss.source, mc_key(ctx->inst->my_ed25519_pubkey))) {
            struct mc_report** pp = &ctx->reports;
            while (*pp != r) pp = &(*pp)->next;
            *pp = r->next; ctx->stats.report_bytes -= sizeof(*r) + r->loss.recipient_count * MULTICAST_KEY_SIZE;
            mc_report_send(ctx, r); u_free(r); continue;
        }
        r->retry = now + MULTICAST_REPORT_INTERVAL_TB;
        int result = mc_report_send(ctx, r);
        if (result == 1) {
            DEBUG_WARN(MC_CAT, "multicast overload report group=%016llx source=%016llx descriptor=%u link=%016llx->%016llx recipients=%u packets=%u",
                       (unsigned long long)r->loss.group_id, (unsigned long long)mc_read(r->loss.source.ed, 8), r->loss.descriptor,
                       (unsigned long long)mc_read(r->loss.src.ed, 8), (unsigned long long)mc_read(r->loss.dst.ed, 8), r->loss.recipient_count, r->loss.packets);
            mc_report_remove(ctx, r);
        }
    }
    mc_report_schedule(ctx);
}

/* Единственный таймер выбирает ближайшее окно или повтор, в покое таймера нет. */
static void mc_report_schedule(struct multicast_ctx* ctx) {
    uint64_t due = UINT64_MAX, now = get_time_tb();
    for (struct mc_window* w = ctx->windows; w; w = w->next) if (w->deadline < due) due = w->deadline;
    for (struct mc_report* r = ctx->reports; r; r = r->next) if (r->retry < due) due = r->retry;
    if (ctx->report_timer) { uasync_cancel_timeout(ctx->inst->ua, ctx->report_timer); ctx->report_timer = NULL; }
    if (due == UINT64_MAX) return;
    int delay = due <= now ? 0 : (due - now > INT_MAX ? INT_MAX : (int)(due - now));
    ctx->report_timer = uasync_set_timeout(ctx->inst->ua, delay, ctx, mc_report_tick, "multicast_reports");
    if (!ctx->report_timer) DEBUG_ERROR(MC_CAT, "multicast report timer allocation failed pending_bytes=%zu", ctx->stats.report_bytes);
}

/* Удаление FIFO только после отмены waiter; callbacks очереди отменяет queue_free. */
static void mc_out_destroy(struct multicast_ctx* ctx, struct mc_out* o) {
    if (o->conn) queue_waiter_cancel(o->conn->send_input_q, &o->waiter);
    struct mc_out** pp = &ctx->outputs;
    while (*pp != o) pp = &(*pp)->next;
    *pp = o->next;
    DEBUG_DEBUG(MC_CAT, "multicast queue destroy peer=%016llx packets=%d bytes=%zu",
                (unsigned long long)(o->conn ? o->conn->peer_node_id : 0), o->q->count, o->q->total_bytes);
    queue_set_callback(o->q, NULL, NULL);
    struct ll_entry* e;
    while ((e = queue_data_get(o->q))) mc_free(e);
    queue_free(o->q); u_free(o);
}

/* Единственная FIFO на транспорт, без групповых или descriptor-подочередей. */
static struct mc_out* mc_out_get(struct multicast_ctx* ctx, struct ETCP_CONN* conn) {
    for (struct mc_out* o = ctx->outputs; o; o = o->next) if (o->conn == conn) return o;
    struct mc_out* o = u_calloc(1, sizeof(*o));
    if (!o) { DEBUG_ERROR(MC_CAT, "multicast output allocation failed"); return NULL; }
    o->q = queue_new(ctx->inst->ua, 0, 0, 0, "multicast_out");
    if (!o->q) { DEBUG_ERROR(MC_CAT, "multicast FIFO allocation failed"); u_free(o); return NULL; }
    o->ctx = ctx; o->conn = conn; o->next = ctx->outputs; ctx->outputs = o;
    queue_set_callback_defer(o->q, 1); queue_set_callback(o->q, mc_out_drain, o);
    DEBUG_DEBUG(MC_CAT, "multicast queue created peer=%016llx limit=%u", (unsigned long long)conn->peer_node_id, MULTICAST_QUEUE_BYTES);
    return o;
}

static int mc_key_compare(const void* a, const void* b) {
    return memcmp(a, b, MULTICAST_KEY_SIZE);
}

/* Сортировка по next hop, внутри ветки — по полному ключу адресата. */
static int mc_target_compare(const void* a, const void* b) {
    const struct mc_target *x = a, *y = b;
    uintptr_t xc = (uintptr_t)x->conn, yc = (uintptr_t)y->conn;
    if (xc != yc) return xc < yc ? -1 : 1;
    return memcmp(x->id.ed, y->id.ed, MULTICAST_KEY_SIZE);
}

/* Выбирает маршруты один раз на обработку, изолирует потери каждой исходящей ветки. */
static void mc_forward_packet(struct multicast_ctx* ctx, const struct mc_packet* p, struct mc_out* previous) {
    struct TOPO_GROUP* group = topo_groups_find(ctx->inst->topo_groups, p->group);
    if (!group || group->stopping) {
        DEBUG_WARN(MC_CAT, "multicast group unavailable group=%016llx", (unsigned long long)p->group); return;
    }
    if (!mc_allowed(ctx, p->group, p->source) ||
        (p->hops > 1 && !mc_allowed(ctx, p->group, p->visited[p->hops - 2]))) return;
    struct mc_target targets[MC_MAX_RECIPIENTS];
    unsigned count = 0;
    for (unsigned i = 0; i < p->count; i++) {
        struct multicast_key id = p->recipients[i];
        if (!mc_allowed(ctx, p->group, id)) continue;
        if (mc_equal(id, mc_key(ctx->inst->my_ed25519_pubkey))) {
            mc_deliver_later(ctx, p);
            continue;
        }
        targets[count++] = (struct mc_target){ mc_route(group, id), id };
    }
    qsort(targets, count, sizeof(*targets), mc_target_compare);
    for (unsigned start = 0; start < count;) {
        unsigned end = start + 1;
        struct ETCP_CONN* conn = targets[start].conn;
        while (end < count && targets[end].conn == conn) end++;
        unsigned n = end - start;
        struct multicast_key peer = conn ? mc_key(conn->peer_ed25519_pubkey) : mc_zero;
        if (!conn) mc_loss(ctx, p, mc_zero, MULTICAST_NO_ROUTE, targets + start, n);
        else if (mc_equal(peer, mc_zero) || !topo_group_data_allowed(group, conn)) {
            DEBUG_WARN(MC_CAT, "multicast next hop denied group=%016llx peer=%016llx recipients=%u",
                       (unsigned long long)p->group, (unsigned long long)conn->peer_node_id, n);
        } else if (mc_has_visited(p, peer))
            mc_loss(ctx, p, peer, MULTICAST_PATH_LOOP, targets + start, n);
        else {
            struct mc_out* o = mc_out_get(ctx, conn);
            size_t size = MULTICAST_HEADER_SIZE + n * MULTICAST_KEY_SIZE + p->payload_len + p->hops * (1 + MULTICAST_KEY_SIZE) + 1;
            if (!o) mc_loss(ctx, p, peer, MULTICAST_INTERNAL_ERROR, targets + start, n);
            else if (o->q->total_bytes + size > MULTICAST_QUEUE_BYTES) {
                DEBUG_TRACE(MC_CAT, "multicast overflow source=%016llx descriptor=%u peer=%016llx bytes=%zu packet=%zu recipients=%u",
                            (unsigned long long)mc_read(p->source.ed, 8), p->descriptor, (unsigned long long)conn->peer_node_id,
                            o->q->total_bytes, size, n);
                mc_loss(ctx, p, peer, MULTICAST_OVERLOAD, targets + start, n);
            } else {
                struct ll_entry* branch = mc_build(p, targets + start, n);
                if (!branch) mc_loss(ctx, p, peer, MULTICAST_INTERNAL_ERROR, targets + start, n);
                else {
                    DEBUG_TRACE(MC_CAT, "multicast split source=%016llx descriptor=%u peer=%016llx recipients=%u bytes=%u",
                                (unsigned long long)mc_read(p->source.ed, 8), p->descriptor, (unsigned long long)conn->peer_node_id, n, branch->len);
                    if (o == previous) queue_data_put_first(o->q, branch);
                    else queue_data_put(o->q, branch);
                }
            }
        }
        start = end;
    }
}

/* При смене маршрута извлечённая ветвь с полными ключами снова разделяется по next hop. */
static void mc_forward(struct multicast_ctx* ctx, struct ll_entry* e, struct mc_out* previous) {
    struct mc_packet p;
    if (!mc_parse(ctx, e, &p)) mc_forward_packet(ctx, &p, previous);
    else { ctx->stats.invalid_packets++; DEBUG_ERROR(MC_CAT, "multicast invalid queued packet len=%u", e->len); }
    mc_free(e);
}

/* Применяем текущий индекс непосредственно перед передачей в ETCP. При ошибке e остаётся целой. */
static int mc_send_wire(struct mc_out* o, struct ll_entry* e) {
    struct mc_packet p;
    if (mc_parse(o->ctx, e, &p)) { DEBUG_ERROR(MC_CAT, "multicast invalid outgoing packet len=%u", e->len); return -1; }
    struct TOPO_GROUP* group = topo_groups_find(o->ctx->inst->topo_groups, p.group);
    uint8_t path[MULTICAST_PATH_RESERVE - 1];
    size_t used = 0;
    for (unsigned i = 0; i < p.hops; i++) {
        int n = topo_address_write(path + used, sizeof(path) - used, group, p.visited[i].ed, TOPO_ADDRESS_COMPACT);
        if (n < 0) return -1;
        used += (size_t)n;
    }
    uint8_t* start = e->dgram + MULTICAST_HEADER_SIZE + p.count * MULTICAST_KEY_SIZE + 1;
    memcpy(start, path, used);
    memmove(start + used, p.payload, p.payload_len);
    e->len = (size_t)(start - e->dgram) + used + p.payload_len;
    DEBUG_TRACE(MC_CAT, "multicast TX group=%016llx recipients=%u hops=%u path_bytes=%zu bytes=%u",
                (unsigned long long)p.group, p.count, p.hops, used, e->len);
    return etcp_send(o->conn, e);
}

/* Проверяет каждый адрес: изменение части назначений требует повторного разделения. */
static int mc_same_route(struct mc_out* o, const struct ll_entry* e) {
    struct mc_packet p;
    if (!o->conn || o->conn->state == 2 || o->conn->close_requested || !o->conn->links_up || mc_parse(o->ctx, e, &p) < 0) return 0;
    struct TOPO_GROUP* group = topo_groups_find(o->ctx->inst->topo_groups, p.group);
    if (!group || group->stopping) return 0;
    if (!mc_allowed(o->ctx, p.group, p.source) || !topo_group_data_allowed(group, o->conn) ||
        (p.hops > 1 && !mc_allowed(o->ctx, p.group, p.visited[p.hops - 2]))) return 0;
    for (unsigned i = 0; i < p.count; i++)
        if (!mc_allowed(o->ctx, p.group, p.recipients[i]) ||
            mc_route(group, p.recipients[i]) != o->conn) return 0;
    return 1;
}

static void mc_out_ready(struct ll_queue* q, void* arg) {
    (void)q;
    struct mc_out* o = arg;
    queue_resume_callback(o->q);
}

/* Ошибка передачи уже готовой ветки: локальный источник получает тот же loss callback. */
static void mc_out_failed(struct mc_out* o, struct ll_entry* e) {
    struct mc_packet p;
    if (mc_parse(o->ctx, e, &p) == 0) {
        struct mc_target targets[MC_MAX_RECIPIENTS];
        for (unsigned i = 0; i < p.count; i++) targets[i] = (struct mc_target){ NULL, p.recipients[i] };
        mc_loss(o->ctx, &p, mc_key(o->conn->peer_ed25519_pubkey), MULTICAST_INTERNAL_ERROR, targets, p.count);
    } else {
        DEBUG_ERROR(MC_CAT, "multicast invalid failed packet len=%u", e->len); o->ctx->stats.internal_errors++;
    }
    mc_free(e);
}

/* Автозабор FIFO: один пакет на свободную send_input_q, затем снова round-robin waiter. */
static void mc_out_drain(struct ll_queue* q, void* arg) {
    struct mc_out* o = arg;
    struct multicast_ctx* ctx = o->ctx;
    struct ll_entry* e = queue_data_get(q);
    if (!e) { queue_resume_callback(q); return; }
    if (!mc_same_route(o, e)) mc_forward(ctx, e, o);
    else if (o->conn->send_input_q->count) {
        queue_data_put_first(q, e);
        if (queue_waiter_wait(o->conn->send_input_q, &o->waiter, mc_out_ready, o) >= 0) return;
        DEBUG_ERROR(MC_CAT, "multicast send waiter allocation failed peer=%016llx", (unsigned long long)o->conn->peer_node_id);
        e = queue_data_get(q); mc_out_failed(o, e);
    } else if (mc_send_wire(o, e) < 0) {
        DEBUG_ERROR(MC_CAT, "multicast transport send failed peer=%016llx", (unsigned long long)o->conn->peer_node_id);
        mc_out_failed(o, e);
    } else ctx->stats.sent++;
    queue_resume_callback(q);
    if (!o->conn && !q->count) mc_out_destroy(ctx, o);
}

/* Отмена изменённых назначений идёт через remove, обычный забор FIFO остаётся в drain callback. */
static void mc_recheck(void* arg) {
    struct multicast_ctx* ctx = arg;
    ctx->route_wake = NULL;
    for (struct mc_out* o = ctx->outputs; o;) {
        struct mc_out* next = o->next;
        for (struct ll_entry* e = o->q->head; e;) {
            struct ll_entry* following = e->next;
            if (!mc_same_route(o, e)) {
                queue_remove_data(o->q, e); mc_forward(ctx, e, o);
            }
            e = following;
        }
        if (o->conn) queue_waiter_cancel(o->conn->send_input_q, &o->waiter);
        queue_resume_callback(o->q);
        if (!o->conn && !o->q->count) mc_out_destroy(ctx, o);
        o = next;
    }
}

void multicast_routes_changed(struct UTUN_INSTANCE* inst) {
    struct multicast_ctx* ctx = inst ? inst->multicast : NULL;
    if (!ctx || !ctx->outputs || ctx->route_wake) return;
    ctx->route_wake = uasync_call_soon(inst->ua, ctx, mc_recheck);
    if (!ctx->route_wake) DEBUG_ERROR(MC_CAT, "multicast route wake allocation failed");
}

/* DELETE приходит до уничтожения send_input_q: waiter отменяется, указатель conn забывается. */
static void mc_conn_status(struct ETCP_CONN* conn, int status, void* arg) {
    struct multicast_ctx* ctx = arg;
    if (status == ETCP_CONN_STATUS_NEW) return;
    if (status == ETCP_CONN_STATUS_DELETE) {
        for (struct mc_out* o = ctx->outputs; o; o = o->next) if (o->conn == conn) {
            queue_waiter_cancel(conn->send_input_q, &o->waiter); o->conn = NULL;
        }
    }
    multicast_routes_changed(ctx->inst);
}

/* Приём DATA: проверяет путь до локальной доставки и добавляет self один раз до очередей. */
static void mc_recv(struct ETCP_CONN* from, struct ll_entry* e) {
    struct multicast_ctx* ctx = from && from->instance ? from->instance->multicast : NULL;
    struct mc_packet p;
    if (!ctx || mc_parse(ctx, e, &p) < 0) goto invalid;
    struct TOPO_GROUP* group = topo_groups_find(ctx->inst->topo_groups, p.group);
    if (!group || group->stopping || !mc_ids_valid(p.recipients, p.count)) goto invalid;
    if (!topo_group_data_allowed(group, from) || !mc_allowed(ctx, p.group, p.source)) { mc_free(e); return; }
    int member = 0;
    for (struct ll_entry* se = group->senders_list ? group->senders_list->head : NULL; se; se = se->next) {
        struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)se->data;
        if (peer->conn == from && peer->accepted && !peer->transport_down) { member = 1; break; }
    }
    if (!member || !mc_equal(p.visited[0], p.source) || !mc_equal(p.visited[p.hops - 1], mc_key(from->peer_ed25519_pubkey)) ||
        sc_identity_check("multicast last hop", from->crypto_ctx.peer_public_key, p.visited[p.hops - 1].ed) != SC_OK) goto invalid;
    for (unsigned i = 0; i < p.hops; i++) {
        if (mc_equal(p.visited[i], mc_zero)) goto invalid;
        for (unsigned j = 0; j < i; j++) if (mc_equal(p.visited[j], p.visited[i])) goto invalid;
    }
    if (mc_has_visited(&p, mc_key(ctx->inst->my_ed25519_pubkey)) || p.hops == MULTICAST_MAX_VISITED) {
        ctx->stats.loop_packets++;
        DEBUG_WARN(MC_CAT, "multicast loop/path limit peer=%016llx hops=%u", (unsigned long long)from->peer_node_id, p.hops);
        mc_free(e); return;
    }
    p.visited[p.hops++] = mc_key(ctx->inst->my_ed25519_pubkey);
    mc_forward_packet(ctx, &p, NULL); mc_free(e); return;
invalid:
    if (ctx) ctx->stats.invalid_packets++;
    DEBUG_WARN(MC_CAT, "multicast invalid data peer=%016llx len=%u",
               (unsigned long long)(from ? from->peer_node_id : 0), e ? e->len : 0);
    if (e) mc_free(e);
}

/* Router уже доставил пакет адресу ядра; проверяем связность src и каноничность отчёта. */
static void mc_report_recv(struct ETCP_CONN* conn, struct ll_entry* e) {
    struct multicast_ctx* ctx = conn && conn->instance ? conn->instance->multicast : NULL;
    if (!ctx || !e || !e->dgram || e->len < ROUTER_SVC_HDR_SIZE) goto invalid;
    if (e->len == ROUTER_SVC_HDR_SIZE) {
        uint64_t group; memcpy(&group, e->dgram + ROUTER_SVC_GROUP_OFF, 8);
        mc_free(e); multicast_revalidate_members(ctx->inst, group); return;
    }
    if (e->len < ROUTER_SVC_HDR_SIZE + MULTICAST_REPORT_HEADER_SIZE) goto invalid;
    uint64_t group;
    memcpy(&group, e->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* w = e->dgram + ROUTER_SVC_HDR_SIZE;
    struct TOPO_GROUP* g = topo_groups_find(ctx->inst->topo_groups, group);
    struct multicast_key reporter = mc_key(w + 2), next_hop = mc_key(w + 34);
    unsigned count = mc_read(w + 70, 2);
    if (memcmp(e->dgram + ROUTER_SVC_DST_ED_OFF, ctx->inst->my_ed25519_pubkey, 32) ||
        !mc_equal(reporter, mc_key(e->dgram + ROUTER_SVC_SRC_ED_OFF)) || mc_equal(reporter, mc_zero) || mc_equal(next_hop, mc_zero) ||
        !mc_read(w, 2) || !mc_read(w + 66, 4) || w[72] > 16 || count > MC_MAX_RECIPIENTS ||
        e->len != ROUTER_SVC_HDR_SIZE + MULTICAST_REPORT_HEADER_SIZE + count * MULTICAST_KEY_SIZE ||
        !mc_ids_valid((const struct multicast_key*)(w + MULTICAST_REPORT_HEADER_SIZE), count)) goto invalid;
    for (unsigned i = w[72]; i < 16; i++) if (w[73 + i]) goto invalid;
    if (!g || g->stopping) goto invalid;
    struct mc_descriptor* descriptor = mc_descriptor_find(ctx, mc_read(w, 2));
    if (!descriptor || descriptor->group != group) { mc_free(e); return; }
    size_t bytes = sizeof(struct mc_report) + count * MULTICAST_KEY_SIZE;
    if (mc_report_room(ctx, bytes) < 0) { mc_free(e); return; }
    struct mc_report* r = u_calloc(1, bytes);
    if (!r) { DEBUG_ERROR(MC_CAT, "multicast incoming report allocation failed bytes=%zu", bytes); mc_free(e); return; }
    r->ctx = ctx; r->received = 1; r->due = r->retry = get_time_tb();
    memcpy(r->ids, w + MULTICAST_REPORT_HEADER_SIZE, count * MULTICAST_KEY_SIZE);
    r->loss = (struct multicast_loss){ .group_id = group, .source = mc_key(ctx->inst->my_ed25519_pubkey), .src = reporter,
        .dst = next_hop, .descriptor = mc_read(w, 2), .recipient_count = count,
        .recipients = r->ids, .packets = mc_read(w + 66, 4), .reason = MULTICAST_OVERLOAD, .prefix_len = w[72] };
    memcpy(r->loss.prefix, w + 73, 16); ctx->stats.reports_received++;
    struct mc_report** tail = &ctx->reports;
    while (*tail) tail = &(*tail)->next;
    *tail = r; ctx->stats.report_bytes += bytes;
    mc_free(e); mc_report_schedule(ctx); return;
invalid:
    if (ctx) ctx->stats.invalid_packets++;
    DEBUG_WARN(MC_CAT, "multicast malformed report len=%u", e ? e->len : 0);
    if (e) mc_free(e);
}

int multicast_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->ua || inst->multicast || inst->api_bindings.callbacks[ETCP_ID_MULTICAST] ||
        inst->router_bindings.callbacks[ETCP_RT_ID_MULTICAST_REPORT]) {
        DEBUG_ERROR(MC_CAT, "multicast init invalid instance or occupied binding"); return -1;
    }
    struct multicast_ctx* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) { DEBUG_ERROR(MC_CAT, "multicast context allocation failed"); return -1; }
    ctx->inst = inst; inst->multicast = ctx;
    ctx->descriptors = queue_new(inst->ua, 256, 0, 2, "multicast_descriptors");
    ctx->subscriptions = queue_new(inst->ua, 256, 0, 8 + MULTICAST_KEY_SIZE + 2, "multicast_subscriptions");
    ctx->deliveries = queue_new(inst->ua, 0, 0, 0, "multicast_deliveries");
    if (!ctx->descriptors || !ctx->subscriptions || !ctx->deliveries) {
        DEBUG_ERROR(MC_CAT, "multicast registry allocation failed"); multicast_destroy(inst); return -1;
    }
    queue_set_callback_defer(ctx->deliveries, 1); queue_set_callback(ctx->deliveries, mc_deliver, ctx);
    etcp_add_conn_status_cbk(inst, mc_conn_status, ctx);
    if (!inst->conn_status_cbks || inst->conn_status_cbks->fn != mc_conn_status || inst->conn_status_cbks->arg != ctx) {
        DEBUG_ERROR(MC_CAT, "multicast status subscription allocation failed"); multicast_destroy(inst); return -1;
    }
    if (etcp_bind(inst, ETCP_ID_MULTICAST, mc_recv) || etcp_router_bind(inst, ETCP_RT_ID_MULTICAST_REPORT, mc_report_recv) ||
        etcp_router_set_chat_policy(inst, ETCP_RT_ID_MULTICAST_REPORT,
                                   ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(MC_CAT, "multicast binding/CHAT policy initialization failed"); multicast_destroy(inst); return -1;
    }
    DEBUG_INFO(MC_CAT, "multicast initialized datagram_limit=%u max_recipients=%u queue_limit=%u reports_limit=%u",
               MULTICAST_MAX_DGRAM_SIZE, (unsigned)MC_MAX_RECIPIENTS, MULTICAST_QUEUE_BYTES, MULTICAST_REPORT_BYTES);
    return 0;
}

void multicast_destroy(struct UTUN_INSTANCE* inst) {
    struct multicast_ctx* ctx = inst ? inst->multicast : NULL;
    if (!ctx) return;
    DEBUG_INFO(MC_CAT, "multicast destroy sent=%llu delivered=%llu overload=%llu reports_evicted=%llu report_bytes=%zu",
               (unsigned long long)ctx->stats.sent, (unsigned long long)ctx->stats.delivered,
               (unsigned long long)ctx->stats.overload_packets, (unsigned long long)ctx->stats.reports_evicted, ctx->stats.report_bytes);
    etcp_unbind(inst, ETCP_ID_MULTICAST); etcp_router_unbind(inst, ETCP_RT_ID_MULTICAST_REPORT);
    etcp_remove_conn_status_cbk(inst, mc_conn_status, ctx);
    if (ctx->route_wake) uasync_call_soon_cancel(inst->ua, ctx->route_wake);
    if (ctx->report_timer) uasync_cancel_timeout(inst->ua, ctx->report_timer);
    while (ctx->outputs) mc_out_destroy(ctx, ctx->outputs);
    while (ctx->reports) mc_report_remove(ctx, ctx->reports);
    while (ctx->windows) mc_window_remove(ctx, &ctx->windows);
    while (ctx->subscriptions && ctx->subscriptions->head)
        mc_subscription_detach((struct multicast_subscription*)ctx->subscriptions->head);
    struct ll_entry* e;
    if (ctx->descriptors) while ((e = queue_data_get(ctx->descriptors))) queue_entry_free(e);
    queue_free(ctx->deliveries); queue_free(ctx->subscriptions); queue_free(ctx->descriptors);
    inst->multicast = NULL; u_free(ctx);
}

/* Номера выдаются циклически после последнего успешного open; закрытие не меняет порядок. */
int multicast_open(struct TOPO_GROUP* group, multicast_loss_fn on_loss, void* arg) {
    if (!group || group->stopping || !group->instance || !group->instance->multicast || !on_loss) {
        DEBUG_ERROR(MC_CAT, "multicast open invalid group or callback"); return -1;
    }
    struct multicast_ctx* ctx = group->instance->multicast;
    if (!mc_allowed(ctx, group->group_id, mc_key(group->instance->my_ed25519_pubkey))) return -1;
    if (ctx->descriptors->count == UINT16_MAX) {
        DEBUG_ERROR(MC_CAT, "multicast descriptors exhausted count=%d", ctx->descriptors->count); return -1;
    }
    uint16_t id = ctx->last_descriptor;
    do { id = id == UINT16_MAX ? 1 : id + 1; } while (mc_descriptor_find(ctx, id));
    struct mc_descriptor* d = (struct mc_descriptor*)queue_entry_new(sizeof(*d) - sizeof(struct ll_entry));
    if (!d) { DEBUG_ERROR(MC_CAT, "multicast descriptor allocation failed"); return -1; }
    d->id = id; d->group = group->group_id; d->on_loss = on_loss; d->arg = arg;
    queue_data_put_with_index(ctx->descriptors, &d->ll); ctx->last_descriptor = id;
    DEBUG_DEBUG(MC_CAT, "multicast open group=%016llx descriptor=%u", (unsigned long long)d->group, id);
    return id;
}

int multicast_send(struct UTUN_INSTANCE* inst, uint16_t descriptor, const struct multicast_key* recipients,
                   uint16_t count, const void* payload, size_t len) {
    struct multicast_ctx* ctx = inst ? inst->multicast : NULL;
    struct mc_descriptor* d = ctx ? mc_descriptor_find(ctx, descriptor) : NULL;
    struct TOPO_GROUP* group = d ? topo_groups_find(inst->topo_groups, d->group) : NULL;
    if (!group || group->stopping || !recipients ||
        !count || count > MC_MAX_RECIPIENTS || (!payload && len) ||
        len > MULTICAST_MAX_DGRAM_SIZE - MULTICAST_HEADER_SIZE - MULTICAST_PATH_RESERVE - (size_t)count * MULTICAST_KEY_SIZE) {
        DEBUG_ERROR(MC_CAT, "multicast send rejected descriptor=%u count=%u payload=%zu max_count=%u datagram_limit=%u",
                    descriptor, count, len, (unsigned)MC_MAX_RECIPIENTS, MULTICAST_MAX_DGRAM_SIZE); return -1;
    }
    if (!mc_allowed(ctx, group->group_id, mc_key(inst->my_ed25519_pubkey))) return -1;
    const uint8_t* key = topo_group_wire_id(group);
    if (!key) { DEBUG_WARN(MC_CAT, "multicast send without group key ch=%s", group->channel_id); return -1; }
    struct multicast_key sorted[MC_MAX_RECIPIENTS];
    memcpy(sorted, recipients, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), mc_key_compare);
    if (mc_equal(sorted[0], mc_zero)) { DEBUG_ERROR(MC_CAT, "multicast zero recipient"); return -1; }
    unsigned unique = 1;
    for (unsigned i = 1; i < count; i++) if (!mc_equal(sorted[i], sorted[unique - 1])) sorted[unique++] = sorted[i];
    struct mc_packet p = { .group = group->group_id, .group_key = key, .source = mc_key(inst->my_ed25519_pubkey), .descriptor = descriptor,
        .recipients = sorted, .count = unique, .payload = payload, .payload_len = len, .hops = 1 };
    p.visited[0] = p.source;
    mc_forward_packet(ctx, &p, NULL); return 0;
}

/* Удаляет только пакеты локального источника этого дескриптора; транзит других источников сохраняется. */
void multicast_close(struct UTUN_INSTANCE* inst, uint16_t descriptor) {
    struct multicast_ctx* ctx = inst ? inst->multicast : NULL;
    struct mc_descriptor* d = ctx ? mc_descriptor_find(ctx, descriptor) : NULL;
    if (!d) { DEBUG_ERROR(MC_CAT, "multicast close unknown descriptor=%u", descriptor); return; }
    uint64_t group = d->group;
    queue_remove_data(ctx->descriptors, &d->ll); queue_entry_free(&d->ll);
    unsigned discarded = 0;
    for (struct mc_out* o = ctx->outputs; o; o = o->next) {
        for (struct ll_entry* e = o->q->head; e;) {
            struct ll_entry* next = e->next;
            if (!memcmp(e->dgram + MULTICAST_SOURCE_OFF, inst->my_ed25519_pubkey, MULTICAST_KEY_SIZE) && mc_read(e->dgram + MULTICAST_DESCRIPTOR_OFF, 2) == descriptor) {
                queue_remove_data(o->q, e); mc_free(e); discarded++;
            }
            e = next;
        }
        if (o->conn) queue_waiter_cancel(o->conn->send_input_q, &o->waiter);
        queue_resume_callback(o->q);
    }
    mc_cancel_deliveries(ctx, NULL, descriptor);
    for (struct mc_report* r = ctx->reports; r;) {
        struct mc_report* next = r->next;
        if (mc_equal(r->loss.source, mc_key(inst->my_ed25519_pubkey)) && r->loss.descriptor == descriptor) mc_report_remove(ctx, r);
        r = next;
    }
    for (struct mc_window** pp = &ctx->windows; *pp;) {
        if (mc_equal((*pp)->source, mc_key(inst->my_ed25519_pubkey)) && (*pp)->descriptor == descriptor) mc_window_remove(ctx, pp);
        else pp = &(*pp)->next;
    }
    DEBUG_DEBUG(MC_CAT, "multicast close group=%016llx descriptor=%u discarded=%u", (unsigned long long)group, descriptor, discarded);
    mc_report_schedule(ctx);
}

/* Ключ принадлежит группе приёма, номер — источнику; исходящий дескриптор здесь не создаётся. */
struct multicast_subscription* multicast_subscribe(struct TOPO_GROUP* group, const uint8_t src_node[MULTICAST_KEY_SIZE], uint16_t src_descriptor,
                                                    multicast_recv_fn on_data, void* arg) {
    if (!group || group->stopping || !group->instance || !group->instance->multicast || !src_node || !src_descriptor || !on_data) {
        DEBUG_ERROR(MC_CAT, "multicast subscribe invalid arguments"); return NULL;
    }
    struct multicast_ctx* ctx = group->instance->multicast;
    if (!mc_allowed(ctx, group->group_id, mc_key(src_node))) return NULL;
    uint8_t key[8 + MULTICAST_KEY_SIZE + 2];
    mc_write(key, group->group_id, 8); memcpy(key + 8, src_node, MULTICAST_KEY_SIZE); mc_write(key + 8 + MULTICAST_KEY_SIZE, src_descriptor, 2);
    if (queue_find_data_by_index(ctx->subscriptions, key)) {
        DEBUG_ERROR(MC_CAT, "multicast duplicate subscription group=%016llx source=%016llx descriptor=%u",
                    (unsigned long long)group->group_id, (unsigned long long)mc_read(src_node, 8), src_descriptor); return NULL;
    }
    struct multicast_subscription* sub = (struct multicast_subscription*)queue_entry_new(sizeof(*sub) - sizeof(struct ll_entry));
    if (!sub) { DEBUG_ERROR(MC_CAT, "multicast subscription allocation failed"); return NULL; }
    memcpy(sub->key, key, sizeof(key)); sub->ctx = ctx; sub->on_data = on_data; sub->arg = arg;
    queue_data_put_with_index(ctx->subscriptions, &sub->ll);
    DEBUG_DEBUG(MC_CAT, "multicast subscribe group=%016llx source=%016llx descriptor=%u",
                (unsigned long long)group->group_id, (unsigned long long)mc_read(src_node, 8), src_descriptor);
    return sub;
}

void multicast_unsubscribe(struct multicast_subscription* sub) {
    if (!sub) return;
    mc_subscription_detach(sub); sub->released = 1;
    if (!sub->running) queue_entry_free(&sub->ll);
}

int multicast_get_stats(struct UTUN_INSTANCE* inst, struct multicast_stats* out) {
    if (!inst || !inst->multicast || !out) { DEBUG_ERROR(MC_CAT, "multicast stats invalid arguments"); return -1; }
    *out = inst->multicast->stats;
    out->descriptors = inst->multicast->descriptors->count; out->subscriptions = inst->multicast->subscriptions->count;
    for (struct mc_out* o = inst->multicast->outputs; o; o = o->next) { out->queues++; out->queued_bytes += o->q->total_bytes; }
    return 0;
}

void multicast_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group_id) {
    struct multicast_ctx* ctx = inst ? inst->multicast : NULL;
    if (!ctx) return;
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    if (group && group->group_type == TOPO_GROUP_TYPE_UTUN) return;
    if (!mc_allowed(ctx, group_id, mc_key(inst->my_ed25519_pubkey))) { multicast_close_group(group); return; }
    unsigned subscriptions = 0, deliveries = 0, reports = 0;
    for (struct ll_entry* e = ctx->subscriptions->head; e;) {
        struct ll_entry* next = e->next;
        struct multicast_subscription* sub = (struct multicast_subscription*)e;
        if (mc_read(sub->key, 8) == group_id && !mc_allowed(ctx, group_id, mc_key(sub->key + 8))) {
            mc_subscription_detach(sub); subscriptions++;
        }
        e = next;
    }
    for (struct ll_entry* e = ctx->deliveries->head; e;) {
        struct ll_entry* next = e->next;
        struct mc_delivery* d = (struct mc_delivery*)e;
        if (mc_read(d->subscription->key, 8) == group_id && !mc_allowed(ctx, group_id, d->via)) {
            queue_remove_data(ctx->deliveries, e); ctx->stats.delivery_bytes -= sizeof(*d) + e->len;
            mc_free(e); deliveries++;
        }
        e = next;
    }
    for (struct mc_report* r = ctx->reports; r;) {
        struct mc_report* next = r->next;
        if (r->loss.group_id == group_id &&
            (!mc_allowed(ctx, group_id, r->loss.source) || !mc_allowed(ctx, group_id, r->loss.src))) {
            mc_report_remove(ctx, r); reports++;
        }
        r = next;
    }
    for (struct mc_window** pp = &ctx->windows; *pp;) {
        if ((*pp)->group == group_id && !mc_allowed(ctx, group_id, (*pp)->source)) mc_window_remove(ctx, pp);
        else pp = &(*pp)->next;
    }
    /* Пересобрать только изменённые ветки общих FIFO; чужие группы и допущенные адресаты сохраняются. */
    if (ctx->route_wake) uasync_call_soon_cancel(inst->ua, ctx->route_wake);
    mc_recheck(ctx);
    mc_report_schedule(ctx);
    if (subscriptions || deliveries || reports)
        DEBUG_INFO(MC_CAT, "multicast revoked group=%016llx subscriptions=%u deliveries=%u reports=%u",
                   (unsigned long long)group_id, subscriptions, deliveries, reports);
}

void multicast_close_group(struct TOPO_GROUP* group) {
    struct multicast_ctx* ctx = group ? group->instance->multicast : NULL;
    if (!ctx) return;
    struct UTUN_INSTANCE* inst = group->instance;
    uint64_t group_id = group->group_id;
    const uint8_t* key = topo_group_wire_id(group);
    for (struct ll_entry* e = ctx->descriptors->head; e;) {
        struct ll_entry* next = e->next;
        struct mc_descriptor* d = (struct mc_descriptor*)e;
        if (d->group == group_id) multicast_close(inst, d->id);
        e = next;
    }
    for (struct ll_entry* e = ctx->subscriptions->head; e;) {
        struct ll_entry* next = e->next;
        struct multicast_subscription* sub = (struct multicast_subscription*)e;
        if (mc_read(sub->key, 8) == group_id) mc_subscription_detach(sub);
        e = next;
    }
    unsigned dropped = 0;
    for (struct mc_out* o = ctx->outputs; o; o = o->next) {
        for (struct ll_entry* e = o->q->head; e;) {
            struct ll_entry* next = e->next;
            if (key && !memcmp(e->dgram + MULTICAST_GROUP_OFF, key, 32)) { queue_remove_data(o->q, e); mc_free(e); dropped++; }
            e = next;
        }
        if (o->conn) queue_waiter_cancel(o->conn->send_input_q, &o->waiter);
        queue_resume_callback(o->q);
    }
    for (struct mc_report* r = ctx->reports; r;) {
        struct mc_report* next = r->next;
        if (r->loss.group_id == group_id) mc_report_remove(ctx, r);
        r = next;
    }
    for (struct mc_window** pp = &ctx->windows; *pp;) {
        if ((*pp)->group == group_id) mc_window_remove(ctx, pp);
        else pp = &(*pp)->next;
    }
    DEBUG_INFO(MC_CAT, "multicast group closed group=%016llx discarded=%u reports_bytes=%zu",
               (unsigned long long)group_id, dropped, ctx->stats.report_bytes);
    mc_report_schedule(ctx);
}
