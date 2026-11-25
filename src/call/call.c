// call.c — P2P аудио-звонок: сигналинг + медиа (per-instance)
//
// Полный цикл: INVITE → RINGING → ACCEPT → SELECT/SELECTED → ACTIVE →
// медиа (CALL_MEDIA) → HANGUP/ENDED. Звонок из CHAT-группы или DM — модуль
// оперирует (group_id, peer_ed), маршрутизация через etcp_router.
//
// Сигналинг, аудио и видео требуют ROUTE_CRYPTO_SIGN|ROUTE_CRYPTO_ENCRYPT между конечными узлами.
// Реестр сессий: ll_queue с hash-индексом по call_id (8 байт).
//
// Thread-модель: всё в uasync-потоке (single-threaded). Освобождение сессии
// отложено через uasync_call_soon, чтобы не было UAF при завершении из
// коллбэков conn_mgr/таймеров.

#include "call.h"
#include "call_priv.h"
#include "call_video_transport.h"
#include "call_proto.h"
#include "../radio/radio_call.h"
#include "../dm/dm_core.h"
#include "../member_files/protocol.h"
#include "../utun_instance.h"
#include "../chat/chat_event.h"
#include "../chat/chat_core.h"
#include "../chat/chat_setting.h"
#include "../chat/group_policy.h"
#include "../ntp_time.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/route_crypto.h"
#include "../routing_layer/conn_mgr.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/etcp_tx_stats.h"
#include "call_audio_history.h"
#include "../../lib/mem.h"
#include "../../lib/ll_queue.h"
#include "../../lib/debug_config.h"
#include "../../lib/u_async.h"
#include "../../lib/db_async.h"
#include "../../lib/platform_compat.h"

#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define CALL_ID "call"

/* Локальный кадр до упаковки: время создания не зависит от сетевого wire timestamp пачки. */
struct call_audio_pending {
    uint64_t created_us;
    struct call_capture_time timing;
    uint16_t seq;
};
struct call_pm_read {
    struct call_ctx* owner; /* Только uasync; detach до free owner, callback владеет самим запросом. */
    struct UTUN_INSTANCE* inst;
    uint64_t id;
    uint8_t peer[32];
};
static void call_tx_flush(struct call_session* s);
static void call_audio_batch_tick(void* arg);
static void call_tx_flush(struct call_session* s);


/* ───────────────────────────────────────────────────────────────────────── */

static struct call_ctx* call_of(struct UTUN_INSTANCE* inst) {
    return inst ? inst->call : NULL;
}

static const char* call_state_name(uint8_t st) {
    switch (st) {
    case CALL_ST_IDLE:       return "IDLE";
    case CALL_ST_OUTGOING:   return "OUTGOING";
    case CALL_ST_RINGING:    return "RINGING";
    case CALL_ST_ACTIVE:     return "ACTIVE";
    case CALL_ST_ENDED:      return "ENDED";
    case CALL_ST_WAIT_DECISION: return "WAIT_DECISION";
    case CALL_ST_ANSWERING: return "ANSWERING";
    case CALL_ST_SELECTING: return "SELECTING";
    default:                 return "?";
    }
}

static const char* call_subcmd_name(uint8_t sc) {
    switch (sc) {
    case CALL_SUBCMD_INVITE:  return "INVITE";
    case CALL_SUBCMD_RINGING: return "RINGING";
    case CALL_SUBCMD_ACCEPT:  return "ACCEPT";
    case CALL_SUBCMD_SELECT: return "SELECT";
    case CALL_SUBCMD_SELECTED: return "SELECTED";
    case CALL_SUBCMD_DECLINE: return "DECLINE";
    case CALL_SUBCMD_BUSY:    return "BUSY";
    case CALL_SUBCMD_CANCEL:  return "CANCEL";
    case CALL_SUBCMD_HANGUP:  return "HANGUP";
    case CALL_SUBCMD_END_ACK: return "END_ACK";
    case CALL_SUBCMD_PHONE_BUSY: return "PHONE_BUSY";
    case CALL_SUBCMD_MEDIA:   return "MEDIA";
    default:                  return "?";
    }
}

/* Возврат 1 — tuple закрыт, 0 — нет, -1 — кэш заполнен. Без вытеснения свежих решений. */
static int call_closed(struct call_ctx* ctx, uint64_t id, uint64_t group, const uint8_t peer[32], int save) {
    uint64_t now = get_time_tb(); struct call_closed* empty = NULL;
    for (unsigned i = 0; i < 256; ++i) {
        struct call_closed* c = &ctx->closed[i];
        if (c->until <= now) { if (!empty) empty = c; continue; }
        if (c->id == id && c->group == group && !memcmp(c->peer_ed, peer, 32)) return 1;
    }
    if (!save) return 0;
    if (!empty) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call closed cache full; refusing end acknowledgement"); return -1; }
    *empty = (struct call_closed){.id = id, .group = group, .until = now + 700000};
    memcpy(empty->peer_ed, peer, 32);
    return 1;
}

/* ─────────────────── отправка ─────────────────── */

static void call_stats_timer_cb(void* arg);

/* data[0] = subcmd. etcp_route_send_key забирает владение entry. */
static int call_send(struct call_ctx* ctx, uint64_t group_id, const uint8_t dst[32],
                     const uint8_t* data, size_t len, int mode) {
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call send: entry allocation failed"); return -1; }
    e->dgram = u_malloc(1 + len);
    if (!e->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call send: packet allocation failed bytes=%zu", 1 + len);
        queue_entry_free(e); return -1;
    }
    e->dgram[0] = ETCP_RT_ID_CALL;
    if (len) memcpy(e->dgram + 1, data, len);
    e->len = (uint16_t)(1 + len);
    int rc = etcp_route_send_key(ctx->inst, group_id, dst, e, 1, mode);
    if (rc != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: send FAIL rc=%d subcmd=%s(0x%02x) dst=%s group=0x%016llx",
                    CALL_ID, rc, call_subcmd_name(len ? data[0] : 0), len ? data[0] : 0,
                    sc_public_key_hex(dst, (char[65]){0}), (unsigned long long)group_id);
    }
    return rc;
}

/* Собрать и отправить INVITE для сессии вызывающего. */
static int call_send_invite(struct call_ctx* ctx, struct call_session* s) {
    uint8_t buf[sizeof(struct call_pkt_invite)];
    struct call_pkt_invite* inv = (struct call_pkt_invite*)buf;
    inv->subcmd = CALL_SUBCMD_INVITE;
    inv->call_id = s->call_id;
    inv->num_tracks = 1;
    inv->force = s->force;
    inv->expires_us = ntp_time_get_us(ctx->inst) + 2000000;
    inv->tracks[0].type = CALL_TRACK_AUDIO;
    inv->tracks[0].codec = CALL_CODEC_OPUS;
    inv->tracks[0].sample_rate = CALL_OPUS_SAMPLE_RATE;
    inv->tracks[0].channels = 1;
    inv->tracks[0].bitrate = 32000;
    int result = -1;
    for (unsigned i = 0; i < s->target_count; ++i) {
        struct call_target* t = &s->targets[i];
        if (t->state == CALL_ST_ENDED) continue;
        inv->force = s->force;
        if (!call_send(ctx, s->group_id, t->ed, buf, sizeof(buf), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) result = 0;
    }
    return result;
}

struct call_targets_sql { uint8_t anchor[32]; uint8_t (*nodes)[32]; size_t count; };
struct call_targets_request {
    struct call_targets_sql sql; /* Worker получает только собственные SQL-значения. */
    struct UTUN_INSTANCE* inst;
    uint64_t group;
    unsigned capacity;
    call_targets_done_fn done;
    void* arg;
};

/* Один адресат требует актуальный канальный ключ и путь; SQL и повторная crypto-проверка не нужны. */
static int call_target_online(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]) {
    int self = !memcmp(peer, inst->my_ed25519_pubkey, 32);
    int admitted = topo_group_chat_key_allowed(inst, group, peer);
    int routed = admitted && !self && etcp_router_peer_reachable(inst, group, peer, ETCP_RT_ID_CALL);
    if (!routed) DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call target unavailable group=%llx peer=%s self=%d admitted=%d",
        (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), self, admitted);
    return routed;
}
static int call_targets_work(sqlite3* db, void* arg) {
    struct call_targets_sql* query = arg; return devgroup_nodes_read(db, query->anchor, &query->nodes, &query->count);
}
static void call_targets_cleanup(void* arg) {
    struct call_targets_request* request = arg; u_free(request->sql.nodes); u_free(request);
}
static void call_targets_ready(void* arg, const struct db_async_result* result) {
    struct call_targets_request* request = arg; struct UTUN_INSTANCE* inst = request->inst; int count = 0;
    if (result->status) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call targets READ failed group=%llx anchor=%s status=%d rc=%d: %s",
            (unsigned long long)request->group, sc_public_key_hex(request->sql.anchor, (char[65]){0}),
            result->status, result->sqlite_code, result->message); count = -1;
    }
    struct TOPO_GROUP* group = !inst->destroying && inst->topo_groups ? topo_groups_find(inst->topo_groups, request->group) : NULL;
    if (!group || !topo_group_chat_key_allowed(inst, request->group, request->sql.anchor)) count = -1;
    for (size_t i = 0; count >= 0 && i < request->sql.count; i++) {
        if (!call_target_online(inst, request->group, request->sql.nodes[i])) continue;
        if ((unsigned)count == request->capacity) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call targets exceed capacity group=%llx capacity=%u",
                (unsigned long long)request->group, request->capacity); count = -1; break;
        }
        if ((size_t)count != i) memcpy(request->sql.nodes[count], request->sql.nodes[i], 32);
        count++;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call targets ready group=%llx anchor=%s count=%d", (unsigned long long)request->group,
        sc_public_key_hex(request->sql.anchor, (char[65]){0}), count);
    request->done(request->arg, count, count > 0 ? request->sql.nodes : NULL);
}

void call_targets_read(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t peer[32], int devices, unsigned capacity,
    call_targets_done_fn done, void* arg) {
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call targets require completion callback"); return; }
    struct TOPO_GROUP* group = inst && !inst->destroying && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    if (!group || !peer || !capacity || capacity > 65535 || !topo_group_chat_key_allowed(inst, group_id, peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call targets invalid group/anchor/capacity"); done(arg, -1, NULL); return;
    }
    if (!devices) { int count = call_target_online(inst, group_id, peer); done(arg, count, count ? (const uint8_t (*)[32])peer : NULL); return; }
    struct call_targets_request* request = u_calloc(1, sizeof(*request));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call targets request allocation failed"); done(arg, -1, NULL); return; }
    memcpy(request->sql.anchor, peer, 32); request->inst = inst; request->group = group_id;
    request->capacity = capacity; request->done = done; request->arg = arg;
    if (inst->db_worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="call-targets", .mode=DB_ASYNC_READ, .work=call_targets_work,
            .data=&request->sql, .done=call_targets_ready, .cleanup=call_targets_cleanup}); return;
    }
    sqlite3* ram = inst->topo_sqlite_db; const char* file = ram ? sqlite3_db_filename(ram, "main") : NULL;
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_ABORT};
    snprintf(result.message, sizeof(result.message), "call targets have no asynchronous reader");
    if (file && !*file) {
        result.sqlite_code = call_targets_work(ram, &request->sql); result.status = result.sqlite_code ? DB_ASYNC_SQL : DB_ASYNC_OK;
        snprintf(result.message, sizeof(result.message), "RAM call targets: %s", sqlite3_errstr(result.sqlite_code));
    }
    call_targets_ready(request, &result); call_targets_cleanup(request);
}

static struct call_target* call_target_find(struct call_session* s, const uint8_t peer[32]) {
    for (unsigned i = 0; i < s->target_count; ++i) if (!memcmp(s->targets[i].ed, peer, 32)) return &s->targets[i];
    return NULL;
}

static void call_send_simple(struct call_session* s, const uint8_t peer[32], uint8_t command) {
    struct call_pkt_accept packet = {command, s->call_id};
    call_send(s->ctx, s->group_id, peer, (const uint8_t*)&packet, sizeof(packet), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
}

/* ─────────────────── реестр сессий ─────────────────── */

struct call_session* call_session_find(struct call_ctx* ctx, uint64_t call_id) {
    if (!ctx || !ctx->sessions) return NULL;
    struct ll_entry* e = queue_find_data_by_index(ctx->sessions, &call_id);
    return e ? (struct call_session*)e->data : NULL;
}

static struct call_session* call_session_new(struct call_ctx* ctx, uint64_t call_id,
                                             uint64_t group_id, uint64_t peer, const uint8_t ed[32], uint8_t role) {
    if (!ctx || !ctx->sessions) return NULL;
    struct ll_entry* qe = queue_entry_new(sizeof(struct call_session));
    if (!qe) return NULL;
    struct call_session* s = (struct call_session*)qe->data;
    memset(s, 0, sizeof(*s));
    s->call_id = call_id;
    s->group_id = group_id;
    s->peer_node_id = peer;
    memcpy(s->peer_ed, ed, 32);
    s->role = role;
    s->state = CALL_ST_IDLE;
    s->started_tb = get_time_tb();
    s->ctx = ctx;
    call_audio_batch_init(&s->audio_batch, get_time_us());
    s->tx_q = queue_new(ctx->inst->ua, 0, 0, 0, "call_txq");
    if (!s->tx_q) { queue_entry_free(qe); return NULL; }
    // При провале queue_data_put_with_index сам освобождает qe (вместе с s), поэтому
    // сохраняем tx_q заранее: queue_entry_free(qe) повторно дал бы double-free.
    struct ll_queue* tx_q = s->tx_q;
    if (queue_data_put_with_index(ctx->sessions, qe) != 0) {
        queue_free(tx_q);
        return NULL;
    }
    return s;
}

static void call_session_free_cb(void* arg) {
    struct ll_entry* e = (struct ll_entry*)arg;
    queue_entry_free(e);
}

/* Освободить медиа-объекты сессии: отменить waiter на send_q роутера и
 * дренировать tx_q (без отправки). Безопасно вызывать из uasync-потока. */
static void call_session_cleanup(struct call_ctx* ctx, struct call_session* s) {
    call_video_transport_stop(s);
    if (s->audio_batch_timer) uasync_cancel_timeout(ctx->inst->ua, s->audio_batch_timer);
    s->audio_batch_timer = NULL;
    etcp_router_cancel_request(&s->audio_send);
    if (s->tx_q) {
        struct ll_entry* e;
        while ((e = queue_data_get(s->tx_q)) != NULL) {
            queue_dgram_free(e);
            queue_entry_free(e);
        }
        queue_free(s->tx_q);
        s->tx_q = NULL;
    }
}

/* Убрать сессию из реестра и освободить отложенно (не из коллбэков conn_mgr/таймеров). */
static void call_session_remove_and_free(struct call_ctx* ctx, struct call_session* s) {
    if (s->target_timer) uasync_cancel_timeout(ctx->inst->ua, s->target_timer);
    s->target_timer = NULL;
    call_session_cleanup(ctx, s);
    struct ll_entry* e = queue_find_data_by_index(ctx->sessions, &s->call_id);
    if (e) {
        queue_remove_data(ctx->sessions, e);
        uasync_call_soon(ctx->inst->ua, e, call_session_free_cb);
    }
}

/* ─────────────────── события в GUI ─────────────────── */

static void call_event_id(struct call_ctx* ctx, int type, uint64_t call_id, uint8_t reason) {
    uint8_t buf[16];
    size_t off = 0;
    memcpy(buf + off, &call_id, 8); off += 8;
    buf[off++] = reason;
    chat_event_post(ctx->inst, type, buf, (int)off);
}

static void call_event_incoming(struct call_ctx* ctx, uint64_t group_id, const uint8_t caller[32], uint64_t call_id) {
    const char* ch = topo_groups_channel(ctx->inst->topo_groups, group_id);
    size_t ch_len = strlen(ch);
    uint8_t buf[1 + 64 + 32 + 8];
    size_t off = 0;
    buf[off++] = (uint8_t)ch_len;
    memcpy(buf + off, ch, ch_len); off += ch_len;
    memcpy(buf + off, caller, 32); off += 32;
    memcpy(buf + off, &call_id, 8); off += 8;
    chat_event_post(ctx->inst, CHAT_EVT_CALL_INCOMING, buf, (int)off);
}

uint8_t call_phone_status(struct UTUN_INSTANCE* inst) {
    if (!inst || !(inst->incoming_call_policy & CALL_POLICY_PUBLISH_PHONE)) return 0;
    if (inst->phone_state == 2 || call_busy(inst)) return TOPO_FLAG_PHONE_KNOWN | TOPO_FLAG_PHONE_BUSY;
    return inst->phone_state == 1 || inst->client_type == CLIENT_TYPE_DESKTOP ? TOPO_FLAG_PHONE_KNOWN : 0;
}

void call_set_phone_state(struct UTUN_INSTANCE* inst, int state, unsigned policy) {
    if (!inst || state < 0 || state > 2 || policy > 31) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "invalid phone state=%d policy=%u", state, policy); return;
    }
    if (inst->phone_state == state && inst->incoming_call_policy == policy) {
        topo_node_update_chat_status(inst); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "phone state old=%u new=%d policy=%u->%u",
               inst->phone_state, state, inst->incoming_call_policy, policy);
    inst->phone_state = (uint8_t)state;
    inst->incoming_call_policy = (uint8_t)policy;
    topo_node_update_chat_status(inst);
}

/* ─────────────────── BGP-маршрут до пира ─────────────────── */

/* Формирует строку BGP-пути до peer_node_id и постит CHAT_EVT_CALL_PATH ([call_id:8][text:var]).
 * hop_list = [peer, m1, ..., mN] (mN — наш прямой сосед), поэтому идём от нас к пиру (в обратном порядке).
 * Формат hop'а: "[%04X] имя" (короткий адрес + имя, как в chat_status). */
static void call_post_bgp_path(struct call_ctx* ctx, struct call_session* s) {
    if (!ctx || !s || !ctx->inst || !ctx->inst->topo_groups) return;
    struct TOPO_GROUP* g = topo_groups_find(ctx->inst->topo_groups, s->group_id);
    if (!g) return;
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_ed(g, s->peer_ed);
    if (!nq) return;

    uint8_t hc = 0;
    const struct topo_hop* hops = topo_node_best_hop_list(nq, &hc, NULL);
    uint16_t cr = topo_get_chain_rtt(nq);

    char text[512];
    int off = 0;
    if (!hc) {
        off = snprintf(text, sizeof(text), "BGP: нет маршрута");
    } else if (hc == 1) {
        off = snprintf(text, sizeof(text), "BGP: прямой");
    } else {
        off = snprintf(text, sizeof(text), "BGP: через ");
        for (int i = hc - 1; i >= 1 && off < (int)sizeof(text) - 48; i--) {
            char hop[48];
            struct TOPO_GROUP_NODE* hop_node = topo_node_find_by_ed(g, hops[i].ed);
            struct TOPO_NODE* ni = hop_node ? hop_node->identity : NULL;
            if (ni && ni->node_name && ni->node_name[0])
                snprintf(hop, sizeof(hop), "[%04X] %s", (unsigned)(hops[i].id & 0xFFFF), ni->node_name);
            else
                snprintf(hop, sizeof(hop), "[%04X]", (unsigned)(hops[i].id & 0xFFFF));
            if (i != hc - 1) off += snprintf(text + off, sizeof(text) - (size_t)off, " → ");
            off += snprintf(text + off, sizeof(text) - (size_t)off, "%s", hop);
        }
    }
    if (cr != 0xFFFF)
        off += snprintf(text + off, sizeof(text) - (size_t)off, " (rtt %ums)", cr / 10);

    uint8_t buf[8 + 512];
    size_t boff = 0;
    memcpy(buf + boff, &s->call_id, 8); boff += 8;
    memcpy(buf + boff, text, (size_t)off); boff += (size_t)off;
    chat_event_post(ctx->inst, CHAT_EVT_CALL_PATH, buf, (int)boff);

    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: path id=%016llx hc=%u rtt=%u: %s",
               CALL_ID, (unsigned long long)s->call_id, hc, cr, text);
}

/* Тот же выбор next hop, что для CALL_MEDIA, включая BGP fallback до готовности CM. */
static void call_post_connection(struct call_session* s) {
    struct ETCP_ROUTER_PATH path = {0};
    if (s->state != CALL_ST_ENDED)
        path = etcp_router_get_path_key(s->ctx->inst, s->group_id, s->peer_ed,
                                    s->state == CALL_ST_ACTIVE ? ETCP_RT_ID_CALL_AUDIO : ETCP_RT_ID_CALL);
    if (s->connection_reported && s->connection_type == path.type && !memcmp(s->connection_next_hop, path.next_hop_ed, 32)) return;
    s->connection_reported = 1; s->connection_type = path.type; memcpy(s->connection_next_hop, path.next_hop_ed, 32);
    uint8_t buf[CHAT_CALL_CONNECTION_SIZE];
    memcpy(buf, &s->call_id, 8); buf[8] = path.type; memcpy(buf + 9, path.next_hop_ed, 32);
    static const char* names[] = { "NONE", "DIRECT", "REVERSE", "INDIRECT" };
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: connection id=%016llx peer=%s type=%s via=%s",
               CALL_ID, (unsigned long long)s->call_id, sc_public_key_hex(s->peer_ed, (char[65]){0}), names[path.type],
               sc_public_key_hex(path.next_hop_ed, (char[65]){0}));
    chat_event_post(s->ctx->inst, CHAT_EVT_CALL_CONNECTION, buf, sizeof(buf));
    if (s->state != CALL_ST_ENDED) call_post_bgp_path(s->ctx, s);
}

static void call_watch_connection(struct call_session* s) {
    call_post_connection(s);
    if (s->state == CALL_ST_ENDED || s->stats_timer) return;
    s->stats_timer = uasync_set_timeout(s->ctx->inst->ua, 10000, s, call_stats_timer_cb, "call_stats");
    if (!s->stats_timer) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: connection timer failed id=%016llx",
                                    CALL_ID, (unsigned long long)s->call_id);
}

/* ─────────────────── завершение ─────────────────── */

/* Финальная команда восстанавливается слоем звонка после смены эпохи роутера. */
static void call_target_tick(void* arg) {
    struct call_session* s = arg;
    s->target_timer = NULL;
    int pending = 0; uint64_t now = get_time_tb();
    for (unsigned i = 0; i < s->target_count; ++i) {
        struct call_target* t = &s->targets[i];
        if (!t->end_subcmd || t->ack) continue;
        if (!topo_group_chat_key_allowed(s->ctx->inst, s->group_id, t->ed)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call target retry cancelled after revoke id=%llx peer=%s",
                (unsigned long long)s->call_id, sc_public_key_hex(t->ed, (char[65]){0}));
            t->ack = 1; continue;
        }
        if (now - t->ended_tb >= CALL_END_TIMEOUT_TB) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call target end timeout id=%llx peer=%s",
                (unsigned long long)s->call_id, sc_public_key_hex(t->ed, (char[65]){0}));
            t->ack = 1; continue;
        }
        struct call_pkt_hangup packet = {t->end_subcmd, s->call_id, t->reason};
        size_t size = t->end_subcmd == CALL_SUBCMD_CANCEL ? sizeof(struct call_pkt_cancel) : sizeof(packet);
        call_send(s->ctx, s->group_id, t->ed, (const uint8_t*)&packet, size, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
        pending = 1;
    }
    if (pending) {
        s->target_timer = uasync_set_timeout(s->ctx->inst->ua, CALL_END_RETRY_TB, s, call_target_tick, "call_target_end");
        if (!s->target_timer) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call target end timer allocation failed");
    }
}

static void call_target_end(struct call_session* s, struct call_target* t, uint8_t reason) {
    if (t->state == CALL_ST_ENDED) return;
    t->state = CALL_ST_ENDED; t->reason = reason; t->end_subcmd = CALL_SUBCMD_CANCEL; t->ended_tb = get_time_tb();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call target cancelled id=%llx peer=%s selected=%u reason=%u",
        (unsigned long long)s->call_id, sc_public_key_hex(t->ed, (char[65]){0}), s->selected, reason);
    if (s->target_timer) uasync_cancel_timeout(s->ctx->inst->ua, s->target_timer);
    call_target_tick(s);
}

static void call_send_end(struct call_session* s) {
    struct call_pkt_hangup packet = {s->end_subcmd, s->call_id, s->reason};
    size_t len = s->end_subcmd == CALL_SUBCMD_CANCEL ? sizeof(struct call_pkt_cancel) : sizeof(packet);
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call end send id=%016llx command=%s elapsed_ms=%llu",
                (unsigned long long)s->call_id, call_subcmd_name(s->end_subcmd),
                (unsigned long long)((get_time_tb() - s->end_started_tb) / 10));
    call_send(s->ctx, s->group_id, s->peer_ed, (const uint8_t*)&packet, len, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
}

/* Медиа уже остановлены. CM держим до подтверждения команды либо ограниченного таймаута. */
static void call_teardown_timer_cb(void* arg) {
    struct call_session* s = (struct call_session*)arg;
    if (!s || !s->ctx) return;
    struct call_ctx* ctx = s->ctx;
    s->teardown_timer = NULL;
    if ((s->end_subcmd || s->target_timer) && get_time_tb() - s->end_started_tb < CALL_END_TIMEOUT_TB) {
        if (s->end_subcmd) call_send_end(s);
        s->teardown_timer = uasync_set_timeout(ctx->inst->ua, CALL_END_RETRY_TB, s, call_teardown_timer_cb, "call_end_retry");
        if (s->teardown_timer) return;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call end retry timer allocation failed id=%016llx", (unsigned long long)s->call_id);
    } else if (s->end_subcmd) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call end confirmation timeout id=%016llx command=%s elapsed_ms=%llu",
                   (unsigned long long)s->call_id, call_subcmd_name(s->end_subcmd),
                   (unsigned long long)((get_time_tb() - s->end_started_tb) / 10));
    }
    if (s->cm_handle) { conn_mgr_close(s->cm_handle); s->cm_handle = NULL; }
    call_session_remove_and_free(ctx, s);
}

static void call_end(struct call_ctx* ctx, struct call_session* s, uint8_t reason, uint8_t end_subcmd) {
    if (!ctx || !s || s->state == CALL_ST_ENDED) return;
    /* До READ ни одному устройству не отправлен INVITE; callback владеет токеном, здесь только отсоединяем. */
    if (s->targets_read) { s->targets_read = NULL; end_subcmd = 0; }

    if (s->role == CALL_ROLE_CALLEE) call_closed(ctx, s->call_id, s->group_id, s->peer_ed, 1);
    s->state = CALL_ST_ENDED;
    for (unsigned i = 0; i < s->target_count; ++i)
        if (!s->selected || memcmp(s->targets[i].ed, s->peer_ed, 32)) call_target_end(s, &s->targets[i], reason);
    topo_node_update_chat_status(ctx->inst);
    call_session_cleanup(ctx, s);
    s->reason = reason;
    s->end_subcmd = s->target_count && !s->selected ? 0 : end_subcmd;
    s->end_started_tb = get_time_tb();
    ctx->c_calls_ended++;

    if (s->ring_timer) { uasync_cancel_timeout(ctx->inst->ua, s->ring_timer); s->ring_timer = NULL; }
    if (s->invite_timer) { uasync_cancel_timeout(ctx->inst->ua, s->invite_timer); s->invite_timer = NULL; }
    if (s->traffic_timer) { uasync_cancel_timeout(ctx->inst->ua, s->traffic_timer); s->traffic_timer = NULL; }
    if (s->stats_timer) { uasync_cancel_timeout(ctx->inst->ua, s->stats_timer); s->stats_timer = NULL; }

    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: call ENDED id=%016llx reason=%d role=%s rx=%u tx=%u tx_dropped=%u",
               CALL_ID, (unsigned long long)s->call_id, reason,
               s->role == CALL_ROLE_CALLER ? "caller" : "callee",
               s->c_rx_frames, s->c_tx_frames, s->c_tx_dropped);

    call_post_connection(s);
    call_event_id(ctx, CHAT_EVT_CALL_ENDED, s->call_id, reason);

    if (s->end_subcmd) call_send_end(s);
    if (s->target_count || s->cm_handle || s->end_subcmd || s->role == CALL_ROLE_CALLEE) {
        unsigned delay = s->end_subcmd ? CALL_END_RETRY_TB : CALL_RING_DEAD_TB + 1000;
        s->teardown_timer = uasync_set_timeout(ctx->inst->ua, delay, s, call_teardown_timer_cb, "call_teardown");
        if (s->teardown_timer) return;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call teardown timer allocation failed id=%016llx", (unsigned long long)s->call_id);
    }
    if (s->cm_handle) { conn_mgr_close(s->cm_handle); s->cm_handle = NULL; }
    call_session_remove_and_free(ctx, s);
}

void call_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct call_ctx* ctx = call_of(inst);
    struct ll_entry* e = ctx && ctx->sessions ? ctx->sessions->head : NULL;
    while (e) {
        struct call_session* s = (struct call_session*)e->data;
        e = e->next;
        if (s->group_id != group) continue;
        int allowed = topo_group_chat_key_allowed(inst, group, s->peer_ed);
        for (unsigned i = 0; i < s->target_count; ++i) {
            struct call_target* t = &s->targets[i];
            if (topo_group_chat_key_allowed(inst, group, t->ed)) continue;
            t->ack = 1;
            if (!s->selected && t->state != CALL_ST_ENDED) allowed = 0;
        }
        if (allowed) continue;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call access revoked id=%llx group=%llx peer=%llx state=%s; stopping media and retries",
            (unsigned long long)s->call_id, (unsigned long long)group,
            (unsigned long long)s->peer_node_id, call_state_name(s->state));
        if (s->cm_handle) { conn_mgr_close(s->cm_handle); s->cm_handle = NULL; }
        s->end_subcmd = 0;
        call_end(ctx, s, CALL_REASON_CONNECT_FAIL, 0);
    }
}

/* ─────────────────── conn_mgr ─────────────────── */

static void call_traffic_timeout_cb(void* arg);

/* CM готовит оптимальный путь; звонок всегда отправляет через роутер с автоматическим fallback. */
static void call_conn_cb(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t group_id,
                         enum conn_mgr_event event, void* arg) {
    struct call_session* s = arg;
    if (!s || s->state == CALL_ST_ENDED) return;
    if (event == CONN_EVENT_UP) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: CM path ready id=%016llx peer=%s", CALL_ID,
                   (unsigned long long)s->call_id, sc_public_key_hex(ed, (char[65]){0}));
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: CM event=%d id=%016llx peer=%s; delivery continues via router",
                   CALL_ID, event, (unsigned long long)s->call_id, sc_public_key_hex(ed, (char[65]){0}));
    }
    call_watch_connection(s);
    (void)h; (void)group_id;
}

static void call_open_cm(struct call_session* s) {
    if (s->cm_handle) return;
    if (conn_mgr_open(s->ctx->inst, s->group_id, s->peer_ed, call_conn_cb, s, &s->cm_handle) != 0)
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: CM request failed id=%016llx; using existing routing", CALL_ID,
                   (unsigned long long)s->call_id);
}

static void call_activate(struct call_session* s) {
    if (s->state == CALL_ST_ACTIVE || s->state == CALL_ST_ENDED) return;
    struct call_ctx* ctx = s->ctx;
    s->state = CALL_ST_ACTIVE;
    call_audio_batch_tick(s);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio batch startup call=%llx mode=full-and-router-ready target=%u max_bytes=%u",
                (unsigned long long)s->call_id, s->audio_batch.target, CALL_MEDIA_MAX_BYTES);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: accepted id=%016llx -> ACTIVE (router delivery)", CALL_ID,
               (unsigned long long)s->call_id);
    uint8_t event[41] = {0};
    memcpy(event, &s->call_id, 8); memcpy(event + 9, s->peer_ed, 32);
    chat_event_post(ctx->inst, CHAT_EVT_CALL_ACCEPTED, event, sizeof(event));
    call_watch_connection(s);
    s->traffic_timer = uasync_set_timeout(ctx->inst->ua, CALL_NO_TRAFFIC_TB, s, call_traffic_timeout_cb, "call_traffic");
}

/* ─────────────────── таймеры ─────────────────── */

static void call_ring_timeout_cb(void* arg) {
    struct call_session* s = (struct call_session*)arg;
    if (!s || !s->ctx) return;
    struct call_ctx* ctx = s->ctx;
    s->ring_timer = NULL;
    if (s->state == CALL_ST_ENDED) return;

    DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: ring timeout id=%016llx -> CANCEL",
               CALL_ID, (unsigned long long)s->call_id);
    call_end(ctx, s, CALL_REASON_RING_TIMEOUT, CALL_SUBCMD_CANCEL);
}

/* Периодическая кодограмма INVITE (caller) каждые 300мс на всём протяжении вызова
 * (OUTGOING + RINGING) — «сердцебиение»: покрывает потерю INVITE/RINGING, держит
 * маршрут и кормит watchdog вызываемого (2с без кодограммы → вызов сорван).
 * При стойком отсутствии маршрута — fail-fast CONNECT_FAIL. */
static void call_invite_retry_cb(void* arg) {
    struct call_session* s = (struct call_session*)arg;
    if (!s || !s->ctx) return;
    struct call_ctx* ctx = s->ctx;
    s->invite_timer = NULL;
    if (s->state == CALL_ST_ENDED) return;
    if (s->state == CALL_ST_ANSWERING) call_send_simple(s, s->peer_ed, CALL_SUBCMD_ACCEPT);
    else if (s->state == CALL_ST_SELECTING) call_send_simple(s, s->peer_ed, CALL_SUBCMD_SELECT);
    else if (s->role == CALL_ROLE_CALLER && !s->selected) {
        unsigned live = 0;
        for (unsigned i = 0; i < s->target_count; ++i) {
            struct call_target* t = &s->targets[i];
            if (t->state == CALL_ST_ENDED) continue;
            if (etcp_router_peer_reachable(ctx->inst, s->group_id, t->ed, ETCP_RT_ID_CALL)) t->no_route = 0;
            else if (++t->no_route >= CALL_NO_ROUTE_MAX_STRIKES) {
                call_target_end(s, t, CALL_REASON_CONNECT_FAIL); continue;
            }
            live++;
        }
        if (!live) { call_end(ctx, s, CALL_REASON_CONNECT_FAIL, 0); return; }
        call_send_invite(ctx, s);
    } else return;

    s->invite_timer = uasync_set_timeout(ctx->inst->ua, CALL_INVITE_RETRY_TB, s, call_invite_retry_cb, "call_invite_retry");
}

/* Вызываемый: 2с без кодограммы INVITE → вызывающий пропал (отмена потерялась) — закрываем. */
static void call_ring_dead_cb(void* arg) {
    struct call_session* s = (struct call_session*)arg;
    if (!s || !s->ctx) return;
    struct call_ctx* ctx = s->ctx;
    s->ring_timer = NULL;
    if (s->state != CALL_ST_RINGING && s->state != CALL_ST_WAIT_DECISION && s->state != CALL_ST_ANSWERING) return;

    DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: ring dead (2с без кодограммы) id=%016llx -> end",
               CALL_ID, (unsigned long long)s->call_id);
    call_end(ctx, s, CALL_REASON_CANCEL, 0);
}

static void call_traffic_timeout_cb(void* arg) {
    struct call_session* s = (struct call_session*)arg;
    if (!s || !s->ctx) return;
    struct call_ctx* ctx = s->ctx;
    s->traffic_timer = NULL;
    if (s->state == CALL_ST_ENDED) return;

    DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: no traffic 20s id=%016llx -> HANGUP",
               CALL_ID, (unsigned long long)s->call_id);
    call_end(ctx, s, CALL_REASON_NO_TRAFFIC, CALL_SUBCMD_HANGUP);
}

/* Периодическое событие CALL_STATS (раз в 1с): rtt_ms, buffer_ms, tempo_x100. */
static void call_post_stats(struct call_session* s) {
    struct call_ctx* ctx = s->ctx;

    int buffer_ms = 0, tempo_x100 = 100;
    struct call_jitter_range range = {0, 0, 60};
    uint32_t dropped = 0, underruns = 0;
    const struct call_audio_ops* audio = ctx->inst->call_audio_ops;
    if (audio && audio->get_stats) audio->get_stats(s->call_id, &buffer_ms, &tempo_x100, &dropped, &underruns, &range);

    uint16_t rtt_tb = 0;
    struct ETCP_ROUTER_CONN* rconn =
        etcp_router_conn_get_key(ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_AUDIO);
    if (rconn && rconn->recv_conn) rtt_tb = rconn->recv_conn->rtt_last;
    uint16_t rtt = rtt_tb / 10; /* ETCP хранит RTT в 0.1мс; CALL_STATS передаёт миллисекунды. */
    uint32_t oldest_ms = 0;
    if (s->tx_q && s->tx_q->head) {
        struct call_audio_pending *oldest = (struct call_audio_pending*)s->tx_q->head->data;
        oldest_ms = (uint32_t)((get_time_us() - oldest->created_us) / 1000);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call audio TX queue id=%016llx drained=%u queued=%zu oldest_ms=%u wait_avg_max_ms=%llu/%u",
                (unsigned long long)s->call_id, s->tx_wait_frames, s->tx_q ? (size_t)queue_entry_count(s->tx_q) : 0, oldest_ms,
                (unsigned long long)(s->tx_wait_frames ? s->tx_wait_total_ms / s->tx_wait_frames : 0), s->tx_wait_max_ms);
    s->tx_wait_frames = s->tx_wait_max_ms = 0; s->tx_wait_total_ms = 0;
    call_tx_flush(s); /* Повтор регистрации после reset/ошибки выделения; обычный drain работает по событиям. */
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "%s: stats id=%016llx recv_peer=%016llx rtt_tb=%u rtt_ms=%u buffer=%dms tempo=%d dropped=%u underruns=%u",
                CALL_ID, (unsigned long long)s->call_id,
                (unsigned long long)(rconn && rconn->recv_conn ? rconn->recv_conn->peer_node_id : 0),
                rtt_tb, rtt, buffer_ms, tempo_x100, dropped, underruns);

    uint8_t buf[24];
    size_t off = 0;
    memcpy(buf + off, &s->call_id, 8); off += 8;
    buf[off++] = (uint8_t)(rtt >> 8); buf[off++] = (uint8_t)(rtt & 0xFF);
    uint16_t bms = (uint16_t)(buffer_ms < 0 ? 0 : (buffer_ms > 0xFFFF ? 0xFFFF : buffer_ms));
    buf[off++] = (uint8_t)(bms >> 8); buf[off++] = (uint8_t)(bms & 0xFF);
    uint16_t t = (uint16_t)(tempo_x100 < 0 ? 0 : (tempo_x100 > 0xFFFF ? 0xFFFF : tempo_x100));
    buf[off++] = (uint8_t)(t >> 8); buf[off++] = (uint8_t)(t & 0xFF);
    uint16_t dr = (uint16_t)(dropped > 0xFFFF ? 0xFFFF : dropped);
    buf[off++] = (uint8_t)(dr >> 8); buf[off++] = (uint8_t)(dr & 0xFF);
    uint16_t un = (uint16_t)(underruns > 0xFFFF ? 0xFFFF : underruns);
    buf[off++] = (uint8_t)(un >> 8); buf[off++] = (uint8_t)(un & 0xFF);
    int values[] = {range.min_ms, range.max_ms, range.reserve_ms};
    for (size_t i = 0; i < 3; i++) {
        uint16_t value = (uint16_t)values[i];
        buf[off++] = (uint8_t)(value >> 8); buf[off++] = (uint8_t)value;
    }
    chat_event_post(ctx->inst, CHAT_EVT_CALL_STATS, buf, (int)off);
}

static void call_stats_timer_cb(void* arg) {
    struct call_session* s = arg;
    if (!s || !s->ctx) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: invalid stats timer session", CALL_ID); return; }
    s->stats_timer = NULL;
    if (s->state == CALL_ST_ENDED) return;
    if (s->state == CALL_ST_ACTIVE) call_post_stats(s);
    call_watch_connection(s);
}

/* ─────────────────── обработчики приёма ─────────────────── */

static void call_handle_invite(struct call_ctx* ctx, uint64_t from, const uint8_t ed[32], uint64_t group,
                               const uint8_t* b, size_t n) {
    if (n < sizeof(struct call_pkt_invite)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: INVITE too short n=%zu", CALL_ID, n);
        return;
    }
    struct call_pkt_invite invite;
    memcpy(&invite, b, sizeof(invite));
    uint64_t call_id = invite.call_id;
    int64_t now_us = ntp_time_get_us(ctx->inst);
    if (!call_id || invite.force > 1 || invite.num_tracks != 1 ||
        n != sizeof(invite) || invite.expires_us <= now_us || invite.expires_us > now_us + 4000000) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "invalid/stale call invite id=%016llx force=%u bytes=%zu",
                   (unsigned long long)call_id, invite.force, n); return;
    }

    if (call_closed(ctx, call_id, group, ed, 0)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call invite ignored after terminal id=%llx peer=%llx",
            (unsigned long long)call_id, (unsigned long long)from); return;
    }

    /* кодограмма-дубликат (тот же call_id): пока звоним — пере-взводим watchdog и
     * идемпотентно отвечаем RINGING; в разговоре (ACTIVE) — игнорируем. */
    struct call_session* s = call_session_find(ctx, call_id);
    if (s) {
        if (memcmp(s->peer_ed, ed, 32) || s->group_id != group || s->role != CALL_ROLE_CALLEE) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call invite tuple/role mismatch id=%016llx", (unsigned long long)call_id); return;
        }
        if (s->state == CALL_ST_RINGING || s->state == CALL_ST_WAIT_DECISION || s->state == CALL_ST_ANSWERING) {
            if (s->state == CALL_ST_WAIT_DECISION && ((ctx->inst->incoming_call_policy & CALL_POLICY_BLOCK_CALLS) ||
                (ctx->inst->phone_state == 2 && (ctx->inst->incoming_call_policy & CALL_POLICY_REJECT_PHONE)))) {
                call_end(ctx, s, (ctx->inst->incoming_call_policy & CALL_POLICY_BLOCK_CALLS) ? CALL_REASON_POLICY : CALL_REASON_PHONE_BUSY,
                         CALL_SUBCMD_DECLINE); return;
            }
            if (get_time_tb() - s->started_tb >= CALL_RING_TIMEOUT_TB) {
                call_end(ctx, s, CALL_REASON_RING_TIMEOUT, CALL_SUBCMD_CANCEL); return;
            }
            if (s->state == CALL_ST_WAIT_DECISION && invite.force) {
                s->force = 1; s->state = CALL_ST_RINGING;
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "phone busy overridden id=%016llx", (unsigned long long)call_id);
                call_event_incoming(ctx, group, ed, call_id);
                call_watch_connection(s);
            }
            if (s->ring_timer) { uasync_cancel_timeout(ctx->inst->ua, s->ring_timer); s->ring_timer = NULL; }
            uint64_t remaining = CALL_RING_TIMEOUT_TB - (get_time_tb() - s->started_tb);
            s->ring_timer = uasync_set_timeout(ctx->inst->ua, remaining < CALL_RING_DEAD_TB ? remaining : CALL_RING_DEAD_TB,
                                              s, call_ring_dead_cb, "call_ring_dead");
            uint8_t pkt[sizeof(struct call_pkt_ringing)];
            struct call_pkt_ringing* r = (struct call_pkt_ringing*)pkt;
            r->subcmd = s->state == CALL_ST_ANSWERING ? CALL_SUBCMD_ACCEPT :
                s->state == CALL_ST_WAIT_DECISION ? CALL_SUBCMD_PHONE_BUSY : CALL_SUBCMD_RINGING;
            r->call_id = call_id;
            call_send(ctx, s->group_id, ed, pkt, sizeof(pkt), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
        } else {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "%s: INVITE codegram id=%016llx ignored (state=%s)",
                        CALL_ID, (unsigned long long)call_id, call_state_name(s->state));
        }
        return;
    }

    if (invite.force) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call continuation without pending attempt id=%016llx", (unsigned long long)call_id); return;
    }

    if (ctx->pm_read || radio_call_busy(ctx->inst)) {
        struct call_pkt_busy busy = {CALL_SUBCMD_BUSY, call_id};
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call busy: pending radio invitation id=%016llx", (unsigned long long)call_id);
        call_send(ctx, group, ed, (const uint8_t*)&busy, sizeof(busy), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
        return;
    }
    /* занятость: есть другая активная сессия */
    struct ll_entry* e = ctx->sessions ? ctx->sessions->head : NULL;
    while (e) {
        struct call_session* other = (struct call_session*)e->data;
        if (other->state != CALL_ST_ENDED) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: busy — active call id=%016llx, reply BUSY to id=%016llx",
                       CALL_ID, (unsigned long long)other->call_id, (unsigned long long)call_id);
            uint8_t pkt[sizeof(struct call_pkt_busy)];
            struct call_pkt_busy* bz = (struct call_pkt_busy*)pkt;
            bz->subcmd = CALL_SUBCMD_BUSY;
            bz->call_id = call_id;
            call_send(ctx, group, ed, pkt, sizeof(pkt), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
            return;
        }
        e = e->next;
    }

    s = call_session_new(ctx, call_id, group, from, ed, CALL_ROLE_CALLEE);
    if (!s) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: session alloc failed id=%016llx", CALL_ID, (unsigned long long)call_id);
        return;
    }
    if (ctx->inst->incoming_call_policy & CALL_POLICY_BLOCK_CALLS) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "incoming call blocked by policy id=%016llx", (unsigned long long)call_id);
        call_end(ctx, s, CALL_REASON_POLICY, CALL_SUBCMD_DECLINE); return;
    }
    if (ctx->inst->phone_state == 2 && (ctx->inst->incoming_call_policy & CALL_POLICY_REJECT_PHONE)) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "phone busy; incoming call rejected id=%016llx", (unsigned long long)call_id);
        call_end(ctx, s, CALL_REASON_PHONE_BUSY, CALL_SUBCMD_DECLINE); return;
    }
    s->state = (ctx->inst->incoming_call_policy & CALL_POLICY_WARN_PHONE) && ctx->inst->phone_state == 2
        ? CALL_ST_WAIT_DECISION : CALL_ST_RINGING;
    topo_node_update_chat_status(ctx->inst);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: INVITE id=%016llx from=0x%016llx group=0x%016llx",
               CALL_ID, (unsigned long long)call_id, (unsigned long long)from,
               (unsigned long long)group);

    if (s->state == CALL_ST_RINGING) {
        call_event_incoming(ctx, group, ed, call_id);
        call_watch_connection(s);
    } else DEBUG_INFO(DEBUG_CATEGORY_CALL, "phone busy; incoming call suppressed id=%016llx", (unsigned long long)call_id);

    /* watchdog: вызывающий шлёт кодограммы INVITE каждые 300мс; 2с тишины → вызов сорван */
    s->ring_timer = uasync_set_timeout(ctx->inst->ua, CALL_RING_DEAD_TB, s, call_ring_dead_cb, "call_ring_dead");

    uint8_t pkt[sizeof(struct call_pkt_ringing)];
    struct call_pkt_ringing* r = (struct call_pkt_ringing*)pkt;
    r->subcmd = s->state == CALL_ST_WAIT_DECISION ? CALL_SUBCMD_PHONE_BUSY : CALL_SUBCMD_RINGING;
    r->call_id = call_id;
    call_send(ctx, group, ed, pkt, sizeof(pkt), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
}

static void call_handle_ringing(struct call_ctx* ctx, const uint8_t* b, size_t n) {
    if (n < 9) return;
    uint64_t call_id; memcpy(&call_id, b + 1, 8);
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->role != CALL_ROLE_CALLER || s->state == CALL_ST_ENDED) return;
    if (s->state == CALL_ST_OUTGOING) {
        s->state = CALL_ST_RINGING;
        /* invite_timer не трогаем — кодограмма INVITE продолжается и в RINGING */
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: RINGING id=%016llx", CALL_ID, (unsigned long long)call_id);
        call_event_id(ctx, CHAT_EVT_CALL_RINGING, call_id, 0);
    }
}

static void call_handle_phone_busy(struct call_ctx* ctx, const uint8_t* b, size_t n) {
    if (n != sizeof(struct call_pkt_ringing)) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "invalid PHONE_BUSY bytes=%zu", n); return; }
    uint64_t id; memcpy(&id, b + 1, 8);
    struct call_session* s = call_session_find(ctx, id);
    if (!s || s->role != CALL_ROLE_CALLER || s->state != CALL_ST_OUTGOING || s->force) return;
    s->state = CALL_ST_WAIT_DECISION;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "phone busy; waiting for caller decision id=%016llx", (unsigned long long)id);
    call_event_id(ctx, CHAT_EVT_CALL_PHONE_BUSY, id, 0);
}

static void call_handle_accept(struct call_ctx* ctx, uint64_t from, const uint8_t ed[32], const uint8_t* b, size_t n) {
    if (n < 9) return;
    uint64_t call_id; memcpy(&call_id, b + 1, 8);
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->role != CALL_ROLE_CALLER || (s->state != CALL_ST_OUTGOING && s->state != CALL_ST_RINGING && s->state != CALL_ST_WAIT_DECISION)) return;

    struct call_target* t = call_target_find(s, ed);
    if (!t || t->state == CALL_ST_ENDED || s->selected) return;
    if (get_time_tb() - s->started_tb >= CALL_RING_TIMEOUT_TB) {
        call_end(ctx, s, CALL_REASON_RING_TIMEOUT, CALL_SUBCMD_CANCEL); return;
    }
    s->selected = 1; s->peer_node_id = from; memcpy(s->peer_ed, ed, 32); s->state = CALL_ST_SELECTING;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call selected id=%llx peer=%llx targets=%u",
        (unsigned long long)call_id, (unsigned long long)from, s->target_count);
    for (unsigned i = 0; i < s->target_count; ++i)
        if (memcmp(s->targets[i].ed, ed, 32)) call_target_end(s, &s->targets[i], CALL_REASON_CANCEL);
    call_send_simple(s, ed, CALL_SUBCMD_SELECT);
}

static void call_handle_decline(struct call_ctx* ctx, const uint8_t* b, size_t n) {
    if (n < 10) return;
    uint64_t call_id; memcpy(&call_id, b + 1, 8);
    uint8_t reason = b[9];
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->state == CALL_ST_ENDED) return;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: DECLINE id=%016llx reason=%d", CALL_ID, (unsigned long long)call_id, reason);
    call_event_id(ctx, CHAT_EVT_CALL_DECLINED, call_id, reason);
    call_end(ctx, s, reason == CALL_REASON_PHONE_BUSY ? CALL_REASON_PHONE_BUSY : CALL_REASON_DECLINE, 0);
}

static void call_handle_busy(struct call_ctx* ctx, const uint8_t* b, size_t n) {
    if (n < 9) return;
    uint64_t call_id; memcpy(&call_id, b + 1, 8);
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->state == CALL_ST_ENDED) return;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: BUSY id=%016llx", CALL_ID, (unsigned long long)call_id);
    call_end(ctx, s, CALL_REASON_BUSY, 0);
}

static void call_handle_cancel(struct call_ctx* ctx, const uint8_t* b, size_t n) {
    if (n < 9) return;
    uint64_t call_id; memcpy(&call_id, b + 1, 8);
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->state == CALL_ST_ENDED) return;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: CANCEL id=%016llx", CALL_ID, (unsigned long long)call_id);
    call_end(ctx, s, CALL_REASON_CANCEL, 0);
}

static void call_handle_hangup(struct call_ctx* ctx, const uint8_t* b, size_t n) {
    if (n < 10) return;
    uint64_t call_id; memcpy(&call_id, b + 1, 8);
    uint8_t reason = b[9];
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->state == CALL_ST_ENDED) return;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: HANGUP id=%016llx reason=%d", CALL_ID, (unsigned long long)call_id, reason);
    call_end(ctx, s, CALL_REASON_REMOTE_HANGUP, 0);
}

static void call_handle_media(struct call_ctx* ctx, const uint8_t ed[32], const uint8_t* b, size_t n) {
    struct call_audio_batch_view batch;
    if (call_audio_batch_parse(b, n, &batch)) return;
    struct call_pkt_media *h = &batch.header;
    struct call_session* s = call_session_find(ctx, h->call_id);
    if (!s || s->state != CALL_ST_ACTIVE) return;
    if (memcmp(ed, s->peer_ed, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio batch wrong peer=%s expected=%s",
                    sc_public_key_hex(ed, (char[65]){0}), sc_public_key_hex(s->peer_ed, (char[65]){0})); return;
    }
    ++s->rx_batches;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "audio batch RX call=%llx frames=%u bytes=%zu first_seq=%u sent_ms=%u",
                (unsigned long long)s->call_id, h->count, n, batch.frames[0].header.seq, h->sent_ms);
    s->last_media_tb = get_time_tb();
    for (unsigned i = 0; i < h->count && s->state == CALL_ST_ACTIVE; i++) {
        const struct call_audio_frame_view *f = &batch.frames[i];
        struct call_capture_time timing = {f->header.capture_pts_us, h->capture_epoch, h->codec_delay_samples};
        s->c_rx_frames++;
        const struct call_audio_ops* audio = ctx->inst->call_audio_ops;
        if (audio && audio->on_media)
            audio->on_media(ctx->inst, s->call_id, f->header.seq, h->sent_ms, f->opus, f->header.length, &timing, i == 0);
        if (ctx->media_recv_fn && s->state == CALL_ST_ACTIVE)
            ctx->media_recv_fn(ctx->inst, s->call_id, f->opus, f->header.length, ctx->media_recv_arg);
    }
    if (s->state != CALL_ST_ACTIVE) return;
    if (s->traffic_timer) uasync_cancel_timeout(ctx->inst->ua, s->traffic_timer);
    s->traffic_timer = uasync_set_timeout(ctx->inst->ua, CALL_NO_TRAFFIC_TB, s, call_traffic_timeout_cb, "call_traffic");
    if (!s->traffic_timer) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio RX watchdog allocation failed");
}

/* ─────────────────── приём (etcp_router) ─────────────────── */

static void call_etcp_recv(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!entry || !conn || !entry->dgram || entry->len < ROUTER_SVC_PAYLOAD_OFF + 1) {
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return;
    }
    struct call_ctx* ctx = call_of(conn->instance);
    if (!ctx || !ctx->initialized) { queue_dgram_free(entry); queue_entry_free(entry); return; }

    const uint8_t* d = entry->dgram;
    const uint8_t* ed = d + ROUTER_SVC_SRC_ED_OFF;
    uint64_t group; memcpy(&group, d + ROUTER_SVC_GROUP_OFF, 8);
    uint8_t subcmd = d[ROUTER_SVC_PAYLOAD_OFF];
    const uint8_t* body = d + ROUTER_SVC_PAYLOAD_OFF;
    size_t blen = entry->len - ROUTER_SVC_PAYLOAD_OFF;

    if ((d[ROUTER_SVC_FLAGS_OFF] & (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) != (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "unprotected call packet command=%u peer=%s flags=%02x",
                   subcmd, sc_public_key_hex(ed, (char[65]){0}), d[ROUTER_SVC_FLAGS_OFF]);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }

    if ((subcmd == CALL_SUBCMD_MEDIA) != (d[0] == ETCP_RT_ID_CALL_AUDIO)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call packet in wrong channel svc=%u subcmd=%u", d[0], subcmd);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    if (subcmd != CALL_SUBCMD_INVITE && blen >= 9) {
        if (subcmd != CALL_SUBCMD_MEDIA) {
            size_t expected = subcmd == CALL_SUBCMD_DECLINE || subcmd == CALL_SUBCMD_HANGUP ? 10 : 9;
            if (blen != expected) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "call signal size invalid command=%u bytes=%zu", subcmd, blen); goto packet_done; }
        }
        uint64_t id; memcpy(&id, body + 1, 8);
        struct call_session* s = call_session_find(ctx, id);
        struct call_target* t = s ? call_target_find(s, ed) : NULL;
        if (s && (s->group_id != group || (memcmp(s->peer_ed, ed, 32) && !t))) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call packet peer/group mismatch id=%llx", (unsigned long long)id);
            goto packet_done;
        }
        if (s && t) {
            if (subcmd == CALL_SUBCMD_END_ACK && blen == sizeof(struct call_pkt_end_ack)) {
                t->ack = 1;
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call target end acknowledged id=%llx peer=%s",
                    (unsigned long long)id, sc_public_key_hex(ed, (char[65]){0}));
                if (!s->selected || memcmp(s->peer_ed, ed, 32)) goto packet_done;
            }
            if (s->selected && memcmp(s->peer_ed, ed, 32)) {
                if (subcmd == CALL_SUBCMD_DECLINE || subcmd == CALL_SUBCMD_CANCEL || subcmd == CALL_SUBCMD_HANGUP)
                    call_send_simple(s, ed, CALL_SUBCMD_END_ACK);
                else if (subcmd != CALL_SUBCMD_END_ACK) call_send_simple(s, ed, CALL_SUBCMD_CANCEL);
                goto packet_done;
            }
            if (!s->selected && t->state == CALL_ST_ENDED && subcmd != CALL_SUBCMD_END_ACK) {
                if (subcmd == CALL_SUBCMD_DECLINE || subcmd == CALL_SUBCMD_CANCEL || subcmd == CALL_SUBCMD_HANGUP)
                    call_send_simple(s, ed, CALL_SUBCMD_END_ACK);
                call_send_simple(s, ed, CALL_SUBCMD_CANCEL); goto packet_done;
            }
            if (!s->selected && (subcmd == CALL_SUBCMD_BUSY || subcmd == CALL_SUBCMD_PHONE_BUSY ||
                subcmd == CALL_SUBCMD_RINGING || subcmd == CALL_SUBCMD_DECLINE || subcmd == CALL_SUBCMD_CANCEL)) {
                if (subcmd == CALL_SUBCMD_DECLINE && blen != sizeof(struct call_pkt_decline)) goto packet_done;
                if (subcmd == CALL_SUBCMD_DECLINE || subcmd == CALL_SUBCMD_CANCEL) call_send_simple(s, ed, CALL_SUBCMD_END_ACK);
                if (subcmd == CALL_SUBCMD_DECLINE && body[9] == CALL_REASON_DECLINE) {
                    call_event_id(ctx, CHAT_EVT_CALL_DECLINED, id, CALL_REASON_DECLINE);
                    call_end(ctx, s, CALL_REASON_DECLINE, 0); goto packet_done;
                }
                if (subcmd == CALL_SUBCMD_PHONE_BUSY && s->force) goto packet_done;
                if (subcmd == CALL_SUBCMD_RINGING) t->state = CALL_ST_RINGING;
                else if (subcmd == CALL_SUBCMD_PHONE_BUSY) t->state = CALL_ST_WAIT_DECISION;
                else call_target_end(s, t, subcmd == CALL_SUBCMD_BUSY ? CALL_REASON_BUSY : CALL_REASON_CONNECT_FAIL);
                unsigned live = 0, waiting = 0, ringing = 0;
                for (unsigned i = 0; i < s->target_count; ++i) {
                    live += s->targets[i].state != CALL_ST_ENDED;
                    waiting += s->targets[i].state == CALL_ST_WAIT_DECISION;
                    ringing += s->targets[i].state == CALL_ST_RINGING;
                }
                if (!live) call_end(ctx, s, subcmd == CALL_SUBCMD_BUSY ? CALL_REASON_BUSY :
                    subcmd == CALL_SUBCMD_DECLINE ? body[9] : CALL_REASON_CONNECT_FAIL, 0);
                else if (waiting == live && s->state != CALL_ST_WAIT_DECISION) {
                    s->state = CALL_ST_WAIT_DECISION; call_event_id(ctx, CHAT_EVT_CALL_PHONE_BUSY, id, 0);
                } else if (ringing && s->state != CALL_ST_RINGING) {
                    s->state = CALL_ST_RINGING; call_event_id(ctx, CHAT_EVT_CALL_RINGING, id, 0);
                }
                goto packet_done;
            }
        }
        if (s && blen == sizeof(struct call_pkt_accept) &&
            ((subcmd == CALL_SUBCMD_SELECT && s->role == CALL_ROLE_CALLEE &&
                (s->state == CALL_ST_ANSWERING || s->state == CALL_ST_ACTIVE)) ||
             (subcmd == CALL_SUBCMD_SELECTED && s->role == CALL_ROLE_CALLER && s->state == CALL_ST_SELECTING))) {
            if (s->state != CALL_ST_ACTIVE && get_time_tb() - s->started_tb >= CALL_RING_TIMEOUT_TB) {
                call_end(ctx, s, CALL_REASON_RING_TIMEOUT, CALL_SUBCMD_CANCEL); goto packet_done;
            }
            if (subcmd == CALL_SUBCMD_SELECT) call_send_simple(s, ed, CALL_SUBCMD_SELECTED);
            if (s->ring_timer) uasync_cancel_timeout(ctx->inst->ua, s->ring_timer);
            if (s->invite_timer) uasync_cancel_timeout(ctx->inst->ua, s->invite_timer);
            s->ring_timer = s->invite_timer = NULL;
            call_open_cm(s); call_activate(s); goto packet_done;
        }
    }

    int terminal = subcmd == CALL_SUBCMD_HANGUP || subcmd == CALL_SUBCMD_CANCEL || subcmd == CALL_SUBCMD_DECLINE;
    if (terminal || subcmd == CALL_SUBCMD_END_ACK) {
        uint8_t required = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
        if ((d[ROUTER_SVC_FLAGS_OFF] & required) != required) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call end unauthenticated command=%s peer=%s flags=%02x",
                       call_subcmd_name(subcmd), sc_public_key_hex(ed, (char[65]){0}), d[ROUTER_SVC_FLAGS_OFF]);
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        size_t expected = subcmd == CALL_SUBCMD_CANCEL || subcmd == CALL_SUBCMD_END_ACK ? 9 : 10;
        if (blen != expected) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call end invalid packet command=%s bytes=%zu expected=%zu",
                       call_subcmd_name(subcmd), blen, expected);
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        uint64_t id; memcpy(&id, body + 1, 8);
        struct call_session* s = call_session_find(ctx, id);
        if (terminal) {
            if (call_closed(ctx, id, group, ed, 1) < 0) goto packet_done;
            // Подтверждаем также повторы после удаления сессии; повторного ENDED обработчики не создают.
            struct call_pkt_end_ack ack = {CALL_SUBCMD_END_ACK, id};
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call end ack id=%016llx command=%s state=%s",
                        (unsigned long long)id, call_subcmd_name(subcmd), s ? call_state_name(s->state) : "absent");
            call_send(ctx, group, ed, (const uint8_t*)&ack, sizeof(ack), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
        } else {
            if (s && s->state == CALL_ST_ENDED && s->end_subcmd) {
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "call end confirmed id=%016llx command=%s elapsed_ms=%llu",
                           (unsigned long long)id, call_subcmd_name(s->end_subcmd),
                           (unsigned long long)((get_time_tb() - s->end_started_tb) / 10));
                s->end_subcmd = 0; // Ближайший teardown tick освобождает CM и сессию.
            } else {
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call end late ack ignored id=%016llx state=%s",
                            (unsigned long long)id, s ? call_state_name(s->state) : "absent");
            }
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
    }

    uint64_t from = 0;
    if (subcmd == CALL_SUBCMD_INVITE || subcmd == CALL_SUBCMD_ACCEPT) {
        uint8_t x[SC_PUBKEY_SIZE];
        if (sc_identity_public_x("call sender", ed, x) != SC_OK) goto packet_done;
        from = sc_derive_node_id_from_pubkey(x);
    }
    switch (subcmd) {
    case CALL_SUBCMD_INVITE:  call_handle_invite(ctx, from, ed, group, body, blen); break;
    case CALL_SUBCMD_RINGING: call_handle_ringing(ctx, body, blen); break;
    case CALL_SUBCMD_PHONE_BUSY: call_handle_phone_busy(ctx, body, blen); break;
    case CALL_SUBCMD_ACCEPT:  call_handle_accept(ctx, from, ed, body, blen); break;
    case CALL_SUBCMD_DECLINE: call_handle_decline(ctx, body, blen); break;
    case CALL_SUBCMD_BUSY:    call_handle_busy(ctx, body, blen); break;
    case CALL_SUBCMD_CANCEL:  call_handle_cancel(ctx, body, blen); break;
    case CALL_SUBCMD_HANGUP:  call_handle_hangup(ctx, body, blen); break;
    case CALL_SUBCMD_MEDIA:   call_handle_media(ctx, ed, body, blen); break;
    default:
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: unknown subcmd=0x%02x from=%s len=%zu",
                   CALL_ID, subcmd, sc_public_key_hex(ed, (char[65]){0}), blen);
        break;
    }
packet_done:
    queue_dgram_free(entry);
    queue_entry_free(entry);
}

/* ─────────────────── публичный API ─────────────────── */

int call_init(struct UTUN_INSTANCE* inst) {
    if (!inst) return -1;
    if (inst->call && inst->call->initialized) return 0;

    struct call_ctx* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->inst = inst;
    inst->call = ctx;
    inst->incoming_call_policy = (uint8_t)chat_setting_get_int(inst, "incoming_call_policy", CALL_POLICY_PUBLISH_PHONE);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "incoming call policy=%u", inst->incoming_call_policy);

    ctx->sessions = queue_new(inst->ua, 256, offsetof(struct call_session, call_id), 8, "call_sessions");
    if (!ctx->sessions) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: queue_new(sessions) failed", CALL_ID);
        u_free(ctx); inst->call = NULL;
        return -1;
    }
    if (etcp_router_bind(inst, ETCP_RT_ID_CALL, call_etcp_recv) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: etcp_router_bind(0x%02x) failed", CALL_ID, ETCP_RT_ID_CALL);
        queue_free(ctx->sessions);
        u_free(ctx); inst->call = NULL;
        return -1;
    }

    ctx->initialized = 1;
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_CALL, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)
        || etcp_router_set_authorize(inst, ETCP_RT_ID_CALL, topo_group_chat_key_allowed)
        || etcp_router_bind(inst, ETCP_RT_ID_CALL_AUDIO, call_etcp_recv)
        || etcp_router_require_crypto(inst, ETCP_RT_ID_CALL_AUDIO, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)
        || etcp_router_set_authorize(inst, ETCP_RT_ID_CALL_AUDIO, topo_group_chat_key_allowed)
        || call_video_transport_bind(inst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call media channel bind failed");
        call_destroy(inst); return -1;
    }
    topo_node_update_chat_status(inst);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: initialized (svc=0x%02x audio/video E2E=AES-128-CCM+Ed25519 required)", CALL_ID, ETCP_RT_ID_CALL);
    return 0;
}

void call_destroy(struct UTUN_INSTANCE* inst) {
    struct call_ctx* ctx = call_of(inst);
    if (!ctx || !ctx->initialized) return;
    if (ctx->pm_read) { ctx->pm_read->owner = NULL; ctx->pm_read = NULL; }

    /* завершить все активные сессии (без отправки — мы выключаемся) */
    while (ctx->sessions && ctx->sessions->head) {
        struct ll_entry* e = ctx->sessions->head;
        struct call_session* s = (struct call_session*)e->data;
        s->state = CALL_ST_ENDED;
        if (s->ring_timer) { uasync_cancel_timeout(inst->ua, s->ring_timer); s->ring_timer = NULL; }
        if (s->invite_timer) { uasync_cancel_timeout(inst->ua, s->invite_timer); s->invite_timer = NULL; }
        if (s->traffic_timer) { uasync_cancel_timeout(inst->ua, s->traffic_timer); s->traffic_timer = NULL; }
        if (s->stats_timer) { uasync_cancel_timeout(inst->ua, s->stats_timer); s->stats_timer = NULL; }
        if (s->target_timer) { uasync_cancel_timeout(inst->ua, s->target_timer); s->target_timer = NULL; }
        if (s->teardown_timer) { uasync_cancel_timeout(inst->ua, s->teardown_timer); s->teardown_timer = NULL; }
        if (s->cm_handle) { conn_mgr_close(s->cm_handle); s->cm_handle = NULL; }
        call_session_cleanup(ctx, s);
        queue_remove_data(ctx->sessions, e);
        queue_entry_free(e);
    }
    queue_free(ctx->sessions);
    call_video_transport_unbind(inst);
    etcp_router_unbind(inst, ETCP_RT_ID_CALL_AUDIO);
    etcp_router_unbind(inst, ETCP_RT_ID_CALL);
    inst->call = NULL;
    uint32_t started = ctx->c_calls_started, ended = ctx->c_calls_ended;
    u_free(ctx);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: destroyed (started=%u ended=%u)", CALL_ID, started, ended);
}

int call_busy(struct UTUN_INSTANCE* inst) {
    struct call_ctx* ctx = call_of(inst);
    if (ctx && ctx->pm_read) return 1;
    for (struct ll_entry* e = ctx && ctx->sessions ? ctx->sessions->head : NULL; e; e = e->next)
        if (((struct call_session*)e->data)->state != CALL_ST_ENDED) return 1;
    return 0;
}

/* Только uasync: проверка доступности перед созданием сессии; очереди групп не межпоточные. */
static int call_is_online(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t peer[32]) {
    if (!topo_group_chat_key_allowed(inst, group_id, peer)) return 0;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    return group && topo_node_find_by_ed(group, peer) != NULL;
}

void call_set_media_recv_cb(struct UTUN_INSTANCE* inst, call_media_recv_fn fn, void* arg) {
    struct call_ctx* ctx = call_of(inst);
    if (!ctx) return;
    ctx->media_recv_fn = fn;
    ctx->media_recv_arg = arg;
}

void call_set_audio_ops(struct UTUN_INSTANCE* inst, const struct call_audio_ops* ops) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call audio registration without instance"); return; }
    if (inst->call_audio_ops == ops) return;
    inst->call_audio_ops = ops;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call audio frontend %s inst=%p chat=%d",
               ops ? "attached" : "detached", (void*)inst, inst->chat_started);
}

/* ─────────────────── TX медиа: tx_q + backpressure ─────────────────── */

static void call_audio_batch_tick(void* arg) {
    struct call_session *s = arg; s->audio_batch_timer = NULL;
    if (s->state != CALL_ST_ACTIVE) return;
    struct ETCP_ROUTER_TX_STATS net = {0};
    uint64_t now = get_time_us();
    int found = etcp_router_tx_stats_key(s->ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_AUDIO, now, &net) == 0;
    struct ETCP_ROUTER_PATH path = etcp_router_get_path_key(s->ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_AUDIO);
    struct call_audio_batch_input input = {.path_node_id = path.next_hop_node_id, .path_epoch = path.tx_epoch,
        .frames = s->tx_q ? (unsigned)s->tx_q->count : 0,
        .oldest_us = s->tx_q && s->tx_q->head ? now - ((struct call_audio_pending*)s->tx_q->head->data)->created_us : 0};
    if (now - s->buffer_log_us >= CALL_BUFFER_LOG_MS * 1000) {
        uint64_t elapsed = s->buffer_log_us ? now - s->buffer_log_us : 0; s->buffer_log_us = now;
        if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CALL)) {
            struct ETCP_ROUTER_CONN* r = etcp_router_conn_lookup(s->ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_AUDIO);
            uint64_t oldest_us = s->tx_q && s->tx_q->head
                ? now - ((struct call_audio_pending*)s->tx_q->head->data)->created_us : 0;
            const char* wait = !s->tx_q || !s->tx_q->count ? "empty" : s->tx_q->count < (int)s->audio_batch.target ? "frames"
                : !found ? "no-channel" : net.no_route ? "no-route" : !net.ready ? "handshake"
                : net.receive_busy ? "peer-busy" : r && net.inflight_packets >= r->inflight_limit ? "window"
                : !s->audio_send.conn ? "unregistered" : "router-or-transport";
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL,
                "audio transport call=%016llx mono_ms=%llu dt_ms=%llu batch=%u feedback=%d delay_ms=%.1f wait=%s "
                "local_frames=%u local_B=%llu local_age_ms=%.1f router_found=%d ready=%d no_route=%d busy=%d probing=%d "
                "queued_pkts_B=%u/%llu queued_age_ms=%.1f inflight_pkts_B=%u/%llu inflight_age_ms=%.1f "
                "enqueue_ack_ms=%.1f submit_ack_ms=%.1f baseline_ms=%.1f ack_age_ms=%.1f ack_B_500ms=%llu "
                "epochs=%llu/%llu tx_frames_batches=%u/%u rx_frames_batches=%u/%u tx_dropped_total=%u rx_idle_ms=%lld "
                "router_retrans_dup_total=%u/%u router_window_pkts=%u",
                (unsigned long long)s->call_id, (unsigned long long)(now / 1000), (unsigned long long)(elapsed / 1000),
                s->audio_batch.target, s->audio_batch.feedback_received, s->audio_batch.delay_us / 1000.0, wait,
                s->tx_q ? (unsigned)s->tx_q->count : 0, (unsigned long long)(s->tx_q ? s->tx_q->total_bytes : 0), oldest_us / 1000.0,
                found, net.ready, net.no_route, net.receive_busy, net.probing,
                net.queued_packets, (unsigned long long)net.queued_bytes, net.queued_age_us / 1000.0,
                net.inflight_packets, (unsigned long long)net.inflight_bytes, net.inflight_age_us / 1000.0,
                net.delivery_rtt_us / 1000.0, net.rtt_us / 1000.0, net.baseline_rtt_us / 1000.0,
                net.last_ack_us ? (now - net.last_ack_us) / 1000.0 : -1.0, (unsigned long long)net.window.bytes,
                (unsigned long long)net.local_epoch, (unsigned long long)net.peer_epoch,
                s->c_tx_frames, s->tx_batches, s->c_rx_frames, s->rx_batches, s->c_tx_dropped,
                s->last_media_tb ? (long long)((get_time_tb() - s->last_media_tb) / 10) : -1ll,
                r ? r->c_retrans_done : 0, r ? r->c_dup_dropped : 0, r ? r->inflight_limit : 0);
        }
    }
    const struct call_audio_ops* audio = s->ctx->inst->call_audio_ops;
    if (audio && audio->record_tx) {
        struct call_history_tx snapshot = {.at_ms = now / 1000, .queued_bytes = s->tx_q ? s->tx_q->total_bytes : 0};
        struct ll_queue* queues = s->ctx->inst->router_conns;
        for (struct ll_entry* e = queues ? queues->head : NULL; e; e = e->next) {
            struct ETCP_ROUTER_CONN* r = (struct ETCP_ROUTER_CONN*)e;
            if (r->closed) continue;
            if (r->send_q) snapshot.queued_bytes += r->send_q->total_bytes;
            if (r->inflight_q) snapshot.inflight_bytes += r->inflight_q->total_bytes;
        }
        struct etcp_tx_snapshot transport;
        if (etcp_tx_get_snapshot(s->ctx->inst, now, &transport) == 0)
            snapshot.transport_bytes = transport.queued_bytes + transport.inflight_bytes;
        snapshot.peer_id = path.next_hop_node_id; snapshot.tx_epoch = path.tx_epoch; snapshot.wire_tx_bytes = path.wire_tx_bytes;
        audio->record_tx(s->call_id, &snapshot);
    }
    if (call_audio_batch_step(&s->audio_batch, now, found ? &net : NULL, &input))
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio batch control failed call=%llx; retaining target=%u",
                    (unsigned long long)s->call_id, s->audio_batch.target);
    call_tx_flush(s);
    s->audio_batch_timer = uasync_set_timeout(s->ctx->inst->ua, 500, s, call_audio_batch_tick, "audio_batch");
    if (!s->audio_batch_timer) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio batch control timer allocation failed");
}

/* Пачка формируется по готовности Router; до принятия все кадры остаются в FIFO сервиса. */
static void call_tx_ready(struct ETCP_ROUTER_CONN* rconn, void* arg) {
    struct call_session *s = arg;
    if (s->state != CALL_ST_ACTIVE || !s->tx_q || !s->tx_q->head) return;
    if (s->tx_q->count < (int)s->audio_batch.target) return; /* target мог вырасти после регистрации. */
    struct call_audio_pending *first = (struct call_audio_pending*)s->tx_q->head->data;
    struct call_audio_batch_view batch = {.header = {.subcmd = CALL_SUBCMD_MEDIA, .call_id = s->call_id,
        .track_id = CALL_TRACK_AUDIO, .capture_epoch = first->timing.epoch,
        .codec_delay_samples = first->timing.delay_samples, .sent_ms = (uint32_t)(get_time_tb() / 10)}};
    size_t size = CALL_MEDIA_HDR_SIZE;
    struct ll_entry *next = s->tx_q->head;
    const char *reason = "available";
    while (next && batch.header.count < s->audio_batch.target) {
        struct call_audio_pending *p = (struct call_audio_pending*)next->data;
        if (p->timing.epoch != first->timing.epoch || p->timing.delay_samples != first->timing.delay_samples) {
            reason = "capture-change"; break;
        }
        if (size + CALL_AUDIO_FRAME_HDR_SIZE + next->len > CALL_MEDIA_MAX_BYTES) { reason = "bytes"; break; }
        batch.frames[batch.header.count++] = (struct call_audio_frame_view){
            .header = {p->seq, p->timing.pts_us, next->len}, .opus = next->dgram};
        size += CALL_AUDIO_FRAME_HDR_SIZE + next->len; next = next->next;
    }
    if (batch.header.count == s->audio_batch.target) reason = "target";
    uint8_t wire[CALL_MEDIA_MAX_BYTES];
    size_t encoded = 0;
    if (call_audio_batch_encode(&batch, wire, sizeof(wire), &encoded)) return;
    uint64_t now = get_time_us(), age = now - first->created_us;
    unsigned count = batch.header.count;
    /* Внутри ready Router только копирует пакет; сервисные callbacks до возврата не вызываются. */
    if (etcp_router_conn_send(rconn, wire, encoded, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio batch enqueue failed call=%llx frames=%u; retaining Opus",
                   (unsigned long long)s->call_id, count);
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        struct ll_entry *e = queue_data_get(s->tx_q);
        uint32_t wait_ms = (uint32_t)((now - ((struct call_audio_pending*)e->data)->created_us) / 1000);
        s->tx_wait_frames++; s->tx_wait_total_ms += wait_ms;
        if (wait_ms > s->tx_wait_max_ms) s->tx_wait_max_ms = wait_ms;
        queue_dgram_free(e); queue_entry_free(e);
    }
    s->c_tx_frames += count; s->tx_batches++;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "audio batch TX call=%llx frames=%u target=%u bytes=%zu oldest_us=%llu first_seq=%u reason=%s remaining=%d",
                (unsigned long long)s->call_id, count, s->audio_batch.target, encoded, (unsigned long long)age,
                batch.frames[0].header.seq, reason, queue_entry_count(s->tx_q));
    call_tx_flush(s);
}

static void call_tx_flush(struct call_session* s) {
    if (s->state != CALL_ST_ACTIVE || !s->tx_q || !s->tx_q->head || s->audio_send.conn) return;
    if (s->tx_q->count < (int)s->audio_batch.target) return;
    struct ETCP_ROUTER_CONN *rconn = etcp_router_conn_get_key(s->ctx->inst, s->group_id, s->peer_ed, ETCP_RT_ID_CALL_AUDIO);
    if (etcp_router_request_send(rconn, &s->audio_send, call_tx_ready, s))
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio router request failed call=%llx; retaining Opus", (unsigned long long)s->call_id);
}

int call_send_media(struct UTUN_INSTANCE* inst, uint64_t call_id, const uint8_t* opus, int len,
                    const struct call_capture_time* timing) {
    struct call_ctx* ctx = call_of(inst);
    if (!ctx || !ctx->initialized) return -1;
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->state != CALL_ST_ACTIVE) return -1;
    if (!opus || len <= 0 || (size_t)len > CALL_AUDIO_MAX_OPUS || (timing && (timing->delay_samples > CALL_OPUS_SAMPLE_RATE
        || (timing->pts_us != INT64_MIN && (timing->pts_us < 0 || timing->pts_us > INT64_MAX / 2 || !timing->epoch))))) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio frame invalid length/timing len=%d", len); return -1;
    }
    struct ll_entry* e = queue_entry_new(sizeof(struct call_audio_pending));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio frame metadata allocation failed"); return -1; }
    e->dgram = u_malloc(len);
    if (!e->dgram) { queue_entry_free(e); DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio frame allocation failed len=%d", len); return -1; }
    *(struct call_audio_pending*)e->data = (struct call_audio_pending){get_time_us(),
        timing ? *timing : (struct call_capture_time){INT64_MIN, 0, 0}, s->tx_media_seq++};
    memcpy(e->dgram, opus, len); e->len = len;
    if (queue_entry_count(s->tx_q) >= CALL_MAX_TX_FRAMES) {
        struct ll_entry* old = queue_data_get(s->tx_q);
        queue_dgram_free(old); queue_entry_free(old); s->c_tx_dropped++;
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio TX overflow call=%llx dropped=%u", (unsigned long long)call_id, s->c_tx_dropped);
    }
    queue_data_put(s->tx_q, e);
    call_tx_flush(s);
    return 0;
}

struct call_start_read { struct UTUN_INSTANCE* inst; uint64_t id; int devices; };
static void call_start_target(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t peer_ed[32], uint64_t out_call_id, int devices);

static void call_pm_route_ready(void* arg, const struct dm_call_route_result* result) {
    struct call_pm_read* read = arg; struct call_ctx* ctx = read->owner;
    if (ctx) {
        if (ctx->pm_read != read) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM call READ lost owner id=%llx", (unsigned long long)read->id);
        else ctx->pm_read = NULL;
    }
    if (!ctx || !ctx->initialized || read->inst->destroying || result->cancelled || result->sqlite_code == SQLITE_ABORT ||
        (db_async_can_write(read->inst->db_worker) && !read->inst->chat_content_started)) { u_free(read); return; }
    if (result->status || !result->available || (!result->peer_group && !mf_zero(read->peer, 32) && memcmp(result->peer_ed, read->peer, 32))) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "PM call route unavailable id=%llx requested=%s peer=%s available=%d status=%d rc=%d",
            (unsigned long long)read->id, sc_public_key_hex(read->peer, (char[65]){0}), sc_public_key_hex(result->peer_ed, (char[65]){0}),
            result->available, result->status, result->sqlite_code);
        uint64_t id = read->id; u_free(read); call_event_id(ctx, CHAT_EVT_CALL_ERROR, id, 1); return;
    }
    char channel[65]; strcpy(channel, topo_groups_channel(read->inst->topo_groups, result->group));
    struct UTUN_INSTANCE* inst = read->inst; uint64_t id = read->id; u_free(read);
    call_start_target(inst, channel, result->peer_ed, id, result->peer_group);
}

/* Сессия могла завершиться/исчезнуть за время READ; токен не допускает ABA при новом запуске с тем же ID. */
static void call_start_ready(void* arg, int count, const uint8_t (*nodes)[32]) {
    struct call_start_read* read = arg; struct call_ctx* ctx = call_of(read->inst);
    struct call_session* s = call_session_find(ctx, read->id); int devices = read->devices;
    int current = s && s->targets_read == read;
    if (current) s->targets_read = NULL;
    u_free(read);
    if (!current || s->state == CALL_ST_ENDED) return;
    if (count <= 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call start has no available targets id=%llx result=%d", (unsigned long long)s->call_id, count);
        call_event_id(ctx, CHAT_EVT_CALL_ERROR, s->call_id, 1); call_end(ctx, s, CALL_REASON_CONNECT_FAIL, 0); return;
    }
    s->target_count = (unsigned)count;
    for (int i = 0; i < count; i++) {
        memcpy(s->targets[i].ed, nodes[i], 32);
        s->targets[i].state = CALL_ST_OUTGOING;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call targets accepted id=%llx count=%d", (unsigned long long)s->call_id, count);
    if (!devices) call_open_cm(s);
    if (s->state == CALL_ST_ENDED) return;
    if (call_send_invite(ctx, s) != 0) { call_end(ctx, s, CALL_REASON_CONNECT_FAIL, 0); return; }
    if (s->state == CALL_ST_ENDED) return;
    call_watch_connection(s);
    if (s->state == CALL_ST_ENDED) return;
    s->invite_timer = uasync_set_timeout(ctx->inst->ua, CALL_INVITE_RETRY_TB, s, call_invite_retry_cb, "call_invite_retry");
    if (!s->invite_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call invite timer allocation failed id=%llx", (unsigned long long)s->call_id);
        call_end(ctx, s, CALL_REASON_CONNECT_FAIL, 0);
    }
}

static void call_start_target(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t peer_ed[32], uint64_t out_call_id, int devices) {
    struct call_ctx* ctx = call_of(inst);
    int direct = ch_id && !strncmp(ch_id, "dm:", 3);
    if (!ctx || !ctx->initialized || !ch_id || !ch_id[0] || (!direct && (!peer_ed || mf_zero(peer_ed, 32))) || !out_call_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: call_start bad args ctx=%p ch=%s peer=%s id=%016llx", CALL_ID,
                    (void*)ctx, ch_id ? ch_id : "(null)", sc_public_key_hex(peer_ed, (char[65]){0}), (unsigned long long)out_call_id);
        return;
    }
    struct call_session* existing = call_session_find(ctx, out_call_id);
    if (existing) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call start ID already registered id=%016llx state=%s peer=%s requested_peer=%s ch=%s",
                   (unsigned long long)out_call_id, call_state_name(existing->state), sc_public_key_hex(existing->peer_ed, (char[65]){0}),
                   sc_public_key_hex(peer_ed, (char[65]){0}), ch_id);
        return;
    }
    if (ctx->pm_read) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call start rejected: PM route READ pending id=%llx", (unsigned long long)ctx->pm_read->id);
        call_event_id(ctx, CHAT_EVT_CALL_ENDED, out_call_id, CALL_REASON_BUSY); return;
    }
    if (peer_ed && !memcmp(peer_ed, inst->my_ed25519_pubkey, 32) && !devices) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: call_start self-call rejected", CALL_ID);
        return;
    }
    /* Как для входящего INVITE, ENDED остаётся только для подтверждения завершения и не занимает линию. */
    for (struct ll_entry* e = ctx->sessions->head; e; e = e->next) {
        struct call_session* busy = (struct call_session*)e->data;
        if (busy->state == CALL_ST_ENDED) continue;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call start rejected id=%016llx ch=%s peer=%s busy_id=%016llx state=%s busy_peer=%s",
                   (unsigned long long)out_call_id, ch_id, sc_public_key_hex(peer_ed, (char[65]){0}),
                   (unsigned long long)busy->call_id, call_state_name(busy->state), sc_public_key_hex(busy->peer_ed, (char[65]){0}));
        call_event_id(ctx, CHAT_EVT_CALL_ENDED, out_call_id, CALL_REASON_BUSY);
        return;
    }
    if (radio_call_busy(inst)) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call start rejected: pending radio invitation");
        call_event_id(ctx, CHAT_EVT_CALL_ENDED, out_call_id, CALL_REASON_BUSY);
        return;
    }

    if (direct) {
        struct call_pm_read* read = u_calloc(1, sizeof(*read));
        if (!read) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "PM call READ allocation failed"); call_event_id(ctx, CHAT_EVT_CALL_ERROR, out_call_id, 1); return; }
        read->inst = inst; read->owner = ctx; read->id = out_call_id; ctx->pm_read = read;
        if (peer_ed) memcpy(read->peer, peer_ed, 32);
        dm_call_route_submit(inst, ch_id + 3, 0, call_pm_route_ready, read); return;
    }
    uint64_t group_id = topo_groups_channel_slot(inst->topo_groups, ch_id);
    uint8_t x25519[32];
    if (!group_id || sc_identity_public_x("call target", peer_ed, x25519) != SC_OK ||
        (!devices && !call_is_online(inst, group_id, peer_ed))) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call target unavailable group=%s peer=%s", ch_id, sc_public_key_hex(peer_ed, (char[65]){0}));
        call_event_id(ctx, CHAT_EVT_CALL_ERROR, out_call_id, 1); return;
    }
    uint64_t peer_node_id = sc_derive_node_id_from_pubkey(x25519);

    struct call_start_read* read = u_calloc(1, sizeof(*read));
    if (!read) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call start READ token allocation failed"); call_event_id(ctx, CHAT_EVT_CALL_ERROR, out_call_id, 1); return;
    }
    struct call_session* s = call_session_new(ctx, out_call_id, group_id, peer_node_id, peer_ed, CALL_ROLE_CALLER);
    if (!s) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: session alloc failed id=%016llx", CALL_ID, (unsigned long long)out_call_id);
        u_free(read); call_event_id(ctx, CHAT_EVT_CALL_ERROR, out_call_id, 1); return;
    }
    read->inst = inst; read->id = out_call_id; read->devices = devices; s->targets_read = read;
    s->state = CALL_ST_OUTGOING;
    ctx->c_calls_started++;
    topo_node_update_chat_status(inst);
    if (s->state == CALL_ST_ENDED) { s->targets_read = NULL; u_free(read); return; }

    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: call_start id=%016llx peer=%s group=0x%016llx",
               CALL_ID, (unsigned long long)out_call_id, sc_public_key_hex(peer_ed, (char[65]){0}), (unsigned long long)group_id);

    uint64_t elapsed = get_time_tb() - s->started_tb;
    if (elapsed < CALL_RING_TIMEOUT_TB)
        s->ring_timer = uasync_set_timeout(inst->ua, CALL_RING_TIMEOUT_TB - elapsed, s, call_ring_timeout_cb, "call_ring");
    if (!s->ring_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call start deadline/timer unavailable id=%llx elapsed=%llu",
            (unsigned long long)out_call_id, (unsigned long long)elapsed);
        s->targets_read = NULL; u_free(read); call_end(ctx, s, CALL_REASON_CONNECT_FAIL, 0); return;
    }
    call_targets_read(inst, group_id, s->peer_ed, devices, CALL_MAX_TARGETS, call_start_ready, read);
}

void call_start(struct UTUN_INSTANCE* inst, const char* ch, const uint8_t peer[32], uint64_t id) {
    call_start_target(inst, ch, peer, id, 0);
}
void call_start_devices(struct UTUN_INSTANCE* inst, const char* ch, const uint8_t peer[32], uint64_t id) {
    call_start_target(inst, ch, peer, id, 1);
}

void call_accept(struct UTUN_INSTANCE* inst, uint64_t call_id) {
    struct call_ctx* ctx = call_of(inst);
    if (!ctx || !ctx->initialized) return;
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->role != CALL_ROLE_CALLEE || s->state != CALL_ST_RINGING) return;

    if (get_time_tb() - s->started_tb >= CALL_RING_TIMEOUT_TB) {
        call_end(ctx, s, CALL_REASON_RING_TIMEOUT, CALL_SUBCMD_CANCEL); return;
    }
    s->state = CALL_ST_ANSWERING;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call answer waiting for selection id=%llx", (unsigned long long)call_id);
    call_send_simple(s, s->peer_ed, CALL_SUBCMD_ACCEPT);
    s->invite_timer = uasync_set_timeout(inst->ua, CALL_INVITE_RETRY_TB, s, call_invite_retry_cb, "call_answer_retry");
    if (!s->invite_timer) call_end(ctx, s, CALL_REASON_CONNECT_FAIL, CALL_SUBCMD_CANCEL);
}

void call_decline(struct UTUN_INSTANCE* inst, uint64_t call_id) {
    struct call_ctx* ctx = call_of(inst);
    if (!ctx || !ctx->initialized) return;
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->role != CALL_ROLE_CALLEE || s->state == CALL_ST_ENDED) return;

    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: call_decline id=%016llx", CALL_ID, (unsigned long long)call_id);
    call_event_id(ctx, CHAT_EVT_CALL_DECLINED, call_id, CALL_REASON_DECLINE);
    call_end(ctx, s, CALL_REASON_DECLINE, CALL_SUBCMD_DECLINE);
}

void call_hangup(struct UTUN_INSTANCE* inst, uint64_t call_id) {
    struct call_ctx* ctx = call_of(inst);
    if (!ctx || !ctx->initialized) return;
    if (ctx->pm_read && ctx->pm_read->id == call_id) {
        ctx->pm_read->owner = NULL; ctx->pm_read = NULL;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "PM call cancelled during route READ id=%llx", (unsigned long long)call_id);
        call_event_id(ctx, CHAT_EVT_CALL_ENDED, call_id, CALL_REASON_LOCAL_HANGUP); return;
    }
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->state == CALL_ST_ENDED) return;

    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: call_hangup id=%016llx", CALL_ID, (unsigned long long)call_id);
    call_end(ctx, s, CALL_REASON_LOCAL_HANGUP, CALL_SUBCMD_HANGUP);
}

/* ─────────────────── trampoline-функции (GUI → uasync) ─────────────────── */

void call_continue(struct UTUN_INSTANCE* inst, uint64_t call_id) {
    struct call_ctx* ctx = call_of(inst);
    struct call_session* s = call_session_find(ctx, call_id);
    if (!s || s->role != CALL_ROLE_CALLER || s->state != CALL_ST_WAIT_DECISION) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "stale call continuation id=%016llx", (unsigned long long)call_id); return;
    }
    if (get_time_tb() - s->started_tb >= CALL_RING_TIMEOUT_TB) {
        call_end(ctx, s, CALL_REASON_RING_TIMEOUT, CALL_SUBCMD_CANCEL); return;
    }
    s->force = 1; s->state = CALL_ST_OUTGOING;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "caller confirmed phone busy override id=%016llx", (unsigned long long)call_id);
    call_send_invite(ctx, s);
}

void call_continue_trampoline(void* arg) {
    struct call_id_arg* a = arg;
    call_continue(a->inst, a->call_id); u_free(a);
}

void call_start_trampoline(void* arg) {
    struct call_start_arg* a = (struct call_start_arg*)arg;
    call_start_target(a->inst, a->ch_id, a->peer, a->call_id, a->devices);
    u_free(a);
}

void call_accept_trampoline(void* arg) {
    struct call_id_arg* a = (struct call_id_arg*)arg;
    call_accept(a->inst, a->call_id);
    u_free(a);
}

void call_decline_trampoline(void* arg) {
    struct call_id_arg* a = (struct call_id_arg*)arg;
    call_decline(a->inst, a->call_id);
    u_free(a);
}

void call_hangup_trampoline(void* arg) {
    struct call_id_arg* a = (struct call_id_arg*)arg;
    call_hangup(a->inst, a->call_id);
    u_free(a);
}

void call_send_media_trampoline(void* arg) {
    struct call_media_arg* a = (struct call_media_arg*)arg;
    call_send_media(a->inst, a->call_id, a->opus, a->len, &a->timing);
    u_free(a);
}
