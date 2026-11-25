/* Короткоживущие приглашения в рацию; один pending, ограниченный кэш результатов.
 * Повторы нужны также после смены router epoch. Один таймер обслуживает ближайший
 * deadline; callback не хранит указателей на освобождаемые сессии. */
#include <string.h>
#include "radio_call.h"
#include "radio.h"
#include "../utun_instance.h"
#include "../chat/chat_event.h"
#include "../chat/group_policy.h"
#include "../call/call.h"
#include "../ntp_time.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/route_crypto.h"
#include "../transport_layer/etcp.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/u_async.h"

#define RC_SLOTS 64
#define RC_TARGETS 64
#define RC_PACKET_SIZE 18
/* ACCEPT — решение пользователя, SELECT — необратимый выбор инициатора.
 * RESULT/ACK завершают отдельную ветку; ACCEPTED допустим только от выбранного адресата. */
enum { RC_INVITE = 1, RC_RINGING, RC_RESULT, RC_ACK, RC_PHONE_BUSY, RC_FORCE, RC_ACCEPT, RC_SELECT };
struct rc_target {
    uint64_t ended;
    uint8_t peer_ed[32];
    uint8_t state, reason, awaiting_ack, force;
};
struct radio_call_session {
    uint64_t id, group;
    uint8_t group_key[32], peer_ed[32];
    struct rc_target* selected; /* Заимствован из targets, живёт столько же, сколько сессия. */
    uint64_t started, last_peer, last_send, finished, response_started;
    uint64_t accept_until; /* Deadline ожидающего READ настроек после локального ACCEPT. */
    void* targets_read; /* Токен READ адресатов; callback владеет памятью и заново находит сессию. */
    uint8_t state, reason, incoming, ready, awaiting_ack, blocked, force;
    unsigned count;
    struct rc_target targets[RC_TARGETS];
};
struct radio_call_context {
    struct UTUN_INSTANCE* inst;
    void* timer;
    int stopping;
    struct radio_call_session sessions[RC_SLOTS];
};
static void rc_tick(void* arg);

static struct rc_target* rc_target(struct radio_call_session* s, const uint8_t peer[32]) {
    for (unsigned i = 0; i < s->count; ++i) if (!memcmp(s->targets[i].peer_ed, peer, 32)) return &s->targets[i];
    return NULL;
}

/* 300мс только пока есть приглашение/неподтверждённый отбой; idle не просыпается. */
static void rc_schedule(struct radio_call_context* ctx) {
    if (ctx->timer) { uasync_cancel_timeout(ctx->inst->ua, ctx->timer); ctx->timer = NULL; }
    if (ctx->stopping) return;
    uint64_t now = get_time_tb(), due = 0;
    for (int i = 0; i < RC_SLOTS; ++i) {
        struct radio_call_session* s = &ctx->sessions[i];
        if (!s->id) continue;
        int active = s->state != RADIO_CALL_FINISHED ||
            (s->awaiting_ack && now - s->finished < RADIO_CALL_RESPONSE_TB * 10);
        for (unsigned j = 0; j < s->count; ++j)
            active |= s->targets[j].awaiting_ack && now - s->targets[j].ended < RADIO_CALL_RESPONSE_TB * 10;
        if (!active) continue;
        uint64_t next = s->last_send + RADIO_CALL_RETRY_TB;
        if (s->state != RADIO_CALL_FINISHED) {
            uint64_t deadline = s->started + RADIO_CALL_RING_TB;
            if (s->incoming && s->last_peer + RADIO_CALL_RESPONSE_TB < deadline) deadline = s->last_peer + RADIO_CALL_RESPONSE_TB;
            if (!s->incoming && s->state == RADIO_CALL_OUTGOING && s->response_started + RADIO_CALL_RESPONSE_TB < deadline)
                deadline = s->response_started + RADIO_CALL_RESPONSE_TB;
            if (deadline < next) next = deadline;
        }
        if (!due || next < due) due = next;
    }
    if (!due) return;
    ctx->timer = uasync_set_timeout(ctx->inst->ua, due > now ? (int)(due - now) : 10, ctx, rc_tick, "radio_call");
    if (!ctx->timer) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call timer allocation failed");
}

static void rc_event(struct radio_call_context* ctx, const struct radio_call_session* s) {
    uint8_t data[CHAT_RADIO_CALL_EVENT_SIZE];
    const uint8_t* peer = !s->incoming && s->selected ? s->selected->peer_ed : s->peer_ed;
    memcpy(data, &s->id, 8); memcpy(data + 8, s->group_key, 32); memcpy(data + CHAT_RADIO_CALL_PEER, peer, 32);
    data[CHAT_RADIO_CALL_STATE] = s->state; data[CHAT_RADIO_CALL_STATE + 1] = s->reason; data[CHAT_RADIO_CALL_STATE + 2] = s->incoming;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call id=%llx group=%llx peer=%s incoming=%u state=%u reason=%u targets=%u",
        (unsigned long long)s->id, (unsigned long long)s->group, sc_public_key_hex(peer, (char[65]){0}),
        s->incoming, s->state, s->reason, s->count);
    chat_event_post(ctx->inst, CHAT_EVT_RADIO_CALL, data, sizeof(data));
}

static int rc_send(struct radio_call_context* ctx, struct radio_call_session* s, const uint8_t peer[32], uint8_t command, uint8_t reason) {
    s->last_send = get_time_tb();
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get_key(ctx->inst, s->group, peer, ETCP_RT_ID_RADIO_CALL);
    if (!route) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call router allocation failed id=%016llx", (unsigned long long)s->id); return -1;
    }
    if (!etcp_router_conn_send_q_has_room(route)) {
        if (!s->blocked) DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio call waiting for send queue id=%016llx command=%u",
                                    (unsigned long long)s->id, command);
        s->blocked = 1; return 0;
    }
    if (s->blocked) DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio call send queue ready id=%016llx", (unsigned long long)s->id);
    s->blocked = 0;
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call packet allocation failed"); return -1; }
    e->dgram = u_malloc(1 + RC_PACKET_SIZE);
    if (!e->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call payload allocation failed"); queue_entry_free(e); return -1; }
    e->len = 1 + RC_PACKET_SIZE;
    DEBUG_TRACE(DEBUG_CATEGORY_RADIO, "radio call TX id=%016llx group=%016llx peer=%s command=%u reason=%u",
        (unsigned long long)s->id, (unsigned long long)s->group, sc_public_key_hex(peer, (char[65]){0}), command, reason);
    e->dgram[0] = ETCP_RT_ID_RADIO_CALL; e->dgram[1] = command;
    memcpy(e->dgram + 2, &s->id, 8);
    int64_t expires = ntp_time_get_us(ctx->inst) + 2000000;
    memcpy(e->dgram + 10, &expires, 8); e->dgram[18] = reason;
    int rc = etcp_route_send_key(ctx->inst, s->group, peer, e, 1, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call send failed id=%016llx command=%u rc=%d",
                       (unsigned long long)s->id, command, rc);
    return rc;
}

static struct radio_call_session* rc_slot(struct radio_call_context* ctx, uint64_t id, uint64_t group, const uint8_t ed[32], int create) {
    struct radio_call_session* empty = NULL;
    uint64_t now = get_time_tb();
    for (int i = 0; i < RC_SLOTS; ++i) {
        struct radio_call_session* s = &ctx->sessions[i];
        if (s->id && s->state == RADIO_CALL_FINISHED && now - s->finished >= RADIO_CALL_CACHE_TB) memset(s, 0, sizeof(*s));
        if (s->id == id && s->group == group && (!memcmp(s->peer_ed, ed, 32) || rc_target(s, ed))) return s;
        if (!s->id && !empty) empty = s;
    }
    if (!create || !empty) return NULL;
    const uint8_t* key = topo_group_wire_id(topo_groups_find(ctx->inst->topo_groups, group));
    if (!key) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call group key unavailable route=%llu", (unsigned long long)group); return NULL; }
    memcpy(empty->group_key, key, 32);
    empty->id = id; empty->group = group; memcpy(empty->peer_ed, ed, 32); empty->started = empty->last_peer = now;
    return empty;
}

int radio_call_busy(struct UTUN_INSTANCE* inst) {
    struct radio_call_context* ctx = inst ? inst->radio_call : NULL;
    for (int i = 0; ctx && i < RC_SLOTS; ++i)
        if (ctx->sessions[i].id && ctx->sessions[i].state != RADIO_CALL_FINISHED) return 1;
    return 0;
}

static void rc_end_target(struct radio_call_context* ctx, struct radio_call_session* s, struct rc_target* t, uint8_t reason) {
    if (t->state == RADIO_CALL_FINISHED) return;
    t->state = RADIO_CALL_FINISHED; t->reason = reason; t->ended = get_time_tb(); t->awaiting_ack = 1;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call target cancel id=%llx peer=%s reason=%u",
        (unsigned long long)s->id, sc_public_key_hex(t->peer_ed, (char[65]){0}), reason);
    rc_send(ctx, s, t->peer_ed, RC_RESULT, reason);
}

static void rc_finish(struct radio_call_context* ctx, struct radio_call_session* s, uint8_t reason, int send) {
    if (s->state == RADIO_CALL_FINISHED) return;
    s->state = RADIO_CALL_FINISHED; s->reason = reason; s->finished = get_time_tb();
    if (s->incoming) {
        s->awaiting_ack = send;
        if (send) rc_send(ctx, s, s->peer_ed, RC_RESULT, reason);
    } else for (unsigned i = 0; i < s->count; ++i) {
        struct rc_target* t = &s->targets[i];
        if (reason == RADIO_CALL_ACCEPTED && t == s->selected) {
            t->state = RADIO_CALL_FINISHED; t->awaiting_ack = 0;
        } else rc_end_target(ctx, s, t, RADIO_CALL_CANCELLED);
    }
    rc_event(ctx, s); rc_schedule(ctx);
}

/* PHONE_BUSY не заглушает устройства, которые ещё могут ответить без force. */
static void rc_aggregate(struct radio_call_context* ctx, struct radio_call_session* s, uint8_t failure) {
    if (s->state == RADIO_CALL_FINISHED || s->selected) return;
    unsigned live = 0, waiting = 0, ringing = 0;
    for (unsigned i = 0; i < s->count; ++i) {
        live += s->targets[i].state != RADIO_CALL_FINISHED;
        waiting += s->targets[i].state == RADIO_CALL_WAIT_DECISION;
        ringing += s->targets[i].state == RADIO_CALL_RINGING;
    }
    if (!live) { rc_finish(ctx, s, failure, 0); return; }
    uint8_t state = waiting == live ? RADIO_CALL_WAIT_DECISION : ringing ? RADIO_CALL_RINGING : RADIO_CALL_OUTGOING;
    if (s->state != state) { s->state = state; rc_event(ctx, s); }
}

/* Отзыв одного устройства оставляет остальные адресаты; отзыв self завершает все ветки группы. */
void radio_call_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct radio_call_context* ctx = inst ? inst->radio_call : NULL;
    if (!ctx || ctx->stopping) return;
    int any_changed = 0;
    for (unsigned i = 0; i < RC_SLOTS; i++) {
        struct radio_call_session* s = &ctx->sessions[i];
        if (!s->id || (group && s->group != group)) continue;
        if (s->incoming) {
            if ((s->state != RADIO_CALL_FINISHED || s->awaiting_ack) && !topo_group_chat_key_allowed(inst, s->group, s->peer_ed)) {
                DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call revoke incoming group=%llx peer=%s id=%llx",
                    (unsigned long long)s->group, sc_public_key_hex(s->peer_ed, (char[65]){0}), (unsigned long long)s->id);
                any_changed = 1; s->awaiting_ack = 0; rc_finish(ctx, s, RADIO_CALL_UNAVAILABLE, 0);
            }
            continue;
        }
        if (s->state != RADIO_CALL_FINISHED && s->targets_read && !topo_group_chat_key_allowed(inst, s->group, s->peer_ed)) {
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call pending targets revoked id=%llx group=%llx",
                (unsigned long long)s->id, (unsigned long long)s->group);
            any_changed = 1; rc_finish(ctx, s, RADIO_CALL_UNAVAILABLE, 0); continue;
        }
        int selected_lost = 0, changed = 0;
        for (unsigned j = 0; j < s->count; j++) {
            struct rc_target* t = &s->targets[j];
            if ((t->state == RADIO_CALL_FINISHED && !t->awaiting_ack) || topo_group_chat_key_allowed(inst, s->group, t->peer_ed)) continue;
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call revoke target group=%llx peer=%s id=%llx selected=%d",
                (unsigned long long)s->group, sc_public_key_hex(t->peer_ed, (char[65]){0}), (unsigned long long)s->id, t == s->selected);
            t->state = RADIO_CALL_FINISHED; t->reason = RADIO_CALL_UNAVAILABLE; t->ended = get_time_tb(); t->awaiting_ack = 0;
            selected_lost |= t == s->selected; changed = 1;
        }
        any_changed |= changed;
        if (selected_lost) rc_finish(ctx, s, RADIO_CALL_UNAVAILABLE, 0);
        else if (changed) rc_aggregate(ctx, s, RADIO_CALL_UNAVAILABLE);
    }
    if (any_changed) rc_schedule(ctx);
}

static void rc_transmit(struct radio_call_context* ctx, struct radio_call_session* s) {
    uint64_t now = get_time_tb();
    s->last_send = now;
    if (s->incoming) {
        if (s->state == RADIO_CALL_FINISHED) {
            if (s->awaiting_ack && now - s->finished < RADIO_CALL_RESPONSE_TB * 10) rc_send(ctx, s, s->peer_ed, RC_RESULT, s->reason);
        } else if (s->state == RADIO_CALL_ANSWERING) rc_send(ctx, s, s->peer_ed, RC_ACCEPT, 0);
        return;
    }
    for (unsigned i = 0; i < s->count; ++i) {
        struct rc_target* t = &s->targets[i];
        if (t->state == RADIO_CALL_FINISHED) {
            if (t->awaiting_ack && now - t->ended < RADIO_CALL_RESPONSE_TB * 10) rc_send(ctx, s, t->peer_ed, RC_RESULT, t->reason);
        } else if (s->selected == t) rc_send(ctx, s, t->peer_ed, RC_SELECT, 0);
        else rc_send(ctx, s, t->peer_ed, t->force ? RC_FORCE : RC_INVITE, 0);
    }
}

static void rc_tick(void* arg) {
    struct radio_call_context* ctx = arg; ctx->timer = NULL;
    radio_call_revalidate_members(ctx->inst, 0);
    uint64_t now = get_time_tb();
    for (int i = 0; i < RC_SLOTS; ++i) {
        struct radio_call_session* s = &ctx->sessions[i];
        if (!s->id) continue;
        if (s->state != RADIO_CALL_FINISHED) {
            if (!topo_groups_find(ctx->inst->topo_groups, s->group)) rc_finish(ctx, s, RADIO_CALL_UNAVAILABLE, 1);
            else if (now - s->started >= RADIO_CALL_RING_TB) rc_finish(ctx, s, RADIO_CALL_NO_ANSWER, 1);
            else if (s->incoming && now - s->last_peer >= RADIO_CALL_RESPONSE_TB) rc_finish(ctx, s, RADIO_CALL_CANCELLED, 1);
            else if (s->incoming && s->state == RADIO_CALL_SELECTED && s->accept_until) {
                if (radio_is_active(ctx->inst, s->group)) rc_finish(ctx, s, RADIO_CALL_ACCEPTED, 1);
                else if (!radio_start_pending(ctx->inst, s->group) || now >= s->accept_until) {
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio activation did not complete id=%llx group=%llx pending=%d deadline=%llu now=%llu",
                        (unsigned long long)s->id, (unsigned long long)s->group, radio_start_pending(ctx->inst, s->group),
                        (unsigned long long)s->accept_until, (unsigned long long)now);
                    rc_finish(ctx, s, RADIO_CALL_AUDIO_ERROR, 1);
                }
            }
            else if (!s->incoming && !s->selected && !s->targets_read) {
                for (unsigned j = 0; j < s->count; ++j)
                    if (s->targets[j].state == RADIO_CALL_OUTGOING && now - s->response_started >= RADIO_CALL_RESPONSE_TB)
                        rc_end_target(ctx, s, &s->targets[j], RADIO_CALL_UNAVAILABLE);
                rc_aggregate(ctx, s, RADIO_CALL_UNAVAILABLE);
            }
        }
        if (now - s->last_send >= RADIO_CALL_RETRY_TB) rc_transmit(ctx, s);
    }
    rc_schedule(ctx);
}

static void rc_receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct radio_call_context* ctx = conn && conn->instance ? conn->instance->radio_call : NULL;
    if (ctx && entry && entry->dgram && entry->len == ROUTER_SVC_HDR_SIZE) {
        uint64_t group; memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
        radio_call_revalidate_members(ctx->inst, group); goto done;
    }
    if (!ctx || !entry || !entry->dgram || entry->len != ROUTER_SVC_PAYLOAD_OFF + RC_PACKET_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call invalid packet bytes=%u", entry ? entry->len : 0); goto done;
    }
    const uint8_t* d = entry->dgram; const uint8_t* b = d + ROUTER_SVC_PAYLOAD_OFF;
    if ((d[ROUTER_SVC_FLAGS_OFF] & (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) != (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call unauthenticated packet"); goto done;
    }
    const uint8_t* ed = d + ROUTER_SVC_SRC_ED_OFF;
    uint64_t group, id; int64_t expires;
    memcpy(&group, d + ROUTER_SVC_GROUP_OFF, 8);
    memcpy(&id, b + 1, 8); memcpy(&expires, b + 9, 8);
    uint8_t command = b[0], reason = b[17];
    if (!id || command < RC_INVITE || command > RC_SELECT || reason > RADIO_CALL_POLICY ||
        ((command == RC_RESULT || command == RC_ACK) ? !reason : reason != 0)) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call invalid command=%u reason=%u", command, reason); goto done;
    }
    if (!memcmp(ed, ctx->inst->my_ed25519_pubkey, 32) || !topo_group_chat_key_allowed(ctx->inst, group, ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call peer denied group=%llx peer=%s",
            (unsigned long long)group, sc_public_key_hex(ed, (char[65]){0})); goto done;
    }
    if (command == RC_INVITE || command == RC_FORCE || command == RC_SELECT) {
        int64_t now = ntp_time_get_us(ctx->inst);
        if (expires <= now || expires > now + 4000000) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call stale/invalid command=%u id=%llx", command, (unsigned long long)id); goto done;
        }
    }
    struct radio_call_session* s = rc_slot(ctx, id, group, ed, 0);
    if (!s && command == RC_INVITE) {
        int busy = call_busy(ctx->inst) || radio_call_busy(ctx->inst);
        s = rc_slot(ctx, id, group, ed, 1);
        if (!s) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call cache full; reply BUSY");
            struct radio_call_session rejected = {.id = id, .group = group};
            rc_send(ctx, &rejected, ed, RC_RESULT, RADIO_CALL_BUSY); goto done;
        }
        s->incoming = 1; s->state = RADIO_CALL_INCOMING;
        if (ctx->inst->incoming_call_policy & CALL_POLICY_BLOCK_RADIO) rc_finish(ctx, s, RADIO_CALL_POLICY, 1);
        else if (busy) rc_finish(ctx, s, RADIO_CALL_BUSY, 1);
        else if (ctx->inst->phone_state == 2 && (ctx->inst->incoming_call_policy & CALL_POLICY_REJECT_PHONE))
            rc_finish(ctx, s, RADIO_CALL_PHONE_BUSY, 1);
        else if (ctx->inst->phone_state == 2 && (ctx->inst->incoming_call_policy & CALL_POLICY_WARN_PHONE)) {
            s->state = RADIO_CALL_WAIT_DECISION; rc_send(ctx, s, ed, RC_PHONE_BUSY, 0);
        } else rc_event(ctx, s);
        goto done;
    }
    if (!s) {
        if (command == RC_RESULT && reason != RADIO_CALL_ACCEPTED) {
            s = rc_slot(ctx, id, group, ed, 1);
            if (s) { s->incoming = 1; s->state = RADIO_CALL_FINISHED; s->reason = reason; s->finished = get_time_tb();
                rc_send(ctx, s, ed, RC_ACK, reason); }
        }
        goto done;
    }
    struct rc_target* t = rc_target(s, ed);
    if (command == RC_ACK) {
        if (t && t->state == RADIO_CALL_FINISHED && t->reason == reason) t->awaiting_ack = 0;
        else if (s->incoming && s->state == RADIO_CALL_FINISHED && s->reason == reason) s->awaiting_ack = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio call end acknowledged id=%llx peer=%s reason=%u",
            (unsigned long long)id, sc_public_key_hex(ed, (char[65]){0}), reason); goto done;
    }
    if (s->state != RADIO_CALL_FINISHED && get_time_tb() - s->started >= RADIO_CALL_RING_TB)
        rc_finish(ctx, s, RADIO_CALL_NO_ANSWER, 1);
    if (command == RC_RESULT) {
        if (s->incoming) {
            if (reason != RADIO_CALL_CANCELLED && reason != RADIO_CALL_UNAVAILABLE && reason != RADIO_CALL_NO_ANSWER &&
                reason != RADIO_CALL_AUDIO_ERROR) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call invalid caller result=%u", reason); goto done; }
            rc_send(ctx, s, ed, RC_ACK, reason); rc_finish(ctx, s, reason, 0);
        } else if (t) {
            rc_send(ctx, s, ed, RC_ACK, reason);
            if (s->state == RADIO_CALL_FINISHED || (s->selected && s->selected != t) || t->state == RADIO_CALL_FINISHED) goto done;
            if (reason == RADIO_CALL_ACCEPTED && s->selected != t) {
                DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call acceptance before selection id=%llx", (unsigned long long)id); goto done;
            }
            if (s->selected || reason == RADIO_CALL_DECLINED) rc_finish(ctx, s, reason, 0);
            else { rc_end_target(ctx, s, t, RADIO_CALL_CANCELLED); rc_aggregate(ctx, s, reason); }
        }
        goto done;
    }
    if (s->state == RADIO_CALL_FINISHED || (t && t->state == RADIO_CALL_FINISHED)) {
        rc_send(ctx, s, ed, RC_RESULT, t ? t->reason : s->reason); goto done;
    }
    if (!s->incoming && t) {
        if (s->selected) { if (t == s->selected) rc_send(ctx, s, ed, RC_SELECT, 0); goto done; }
        if (t->state == RADIO_CALL_OUTGOING && get_time_tb() - s->response_started >= RADIO_CALL_RESPONSE_TB) {
            rc_end_target(ctx, s, t, RADIO_CALL_UNAVAILABLE); rc_aggregate(ctx, s, RADIO_CALL_UNAVAILABLE); goto done;
        }
        if (command == RC_ACCEPT) {
            s->selected = t;
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call selected id=%llx peer=%s", (unsigned long long)id, sc_public_key_hex(ed, (char[65]){0}));
            for (unsigned i = 0; i < s->count; ++i) if (&s->targets[i] != t)
                rc_end_target(ctx, s, &s->targets[i], RADIO_CALL_CANCELLED);
            rc_send(ctx, s, ed, RC_SELECT, 0);
        } else if (command == RC_RINGING) { t->state = RADIO_CALL_RINGING; rc_aggregate(ctx, s, RADIO_CALL_UNAVAILABLE); }
        else if (command == RC_PHONE_BUSY && !t->force) { t->state = RADIO_CALL_WAIT_DECISION; rc_aggregate(ctx, s, RADIO_CALL_PHONE_BUSY); }
    } else if (s->incoming && (command == RC_INVITE || command == RC_FORCE || command == RC_SELECT)) {
        s->last_peer = get_time_tb();
        if (command == RC_SELECT) {
            if (s->state == RADIO_CALL_ANSWERING) { s->state = RADIO_CALL_SELECTED; rc_event(ctx, s); }
        } else {
            if (s->state == RADIO_CALL_WAIT_DECISION && ((ctx->inst->incoming_call_policy & CALL_POLICY_BLOCK_RADIO) ||
                (ctx->inst->phone_state == 2 && (ctx->inst->incoming_call_policy & CALL_POLICY_REJECT_PHONE)))) {
                rc_finish(ctx, s, (ctx->inst->incoming_call_policy & CALL_POLICY_BLOCK_RADIO) ? RADIO_CALL_POLICY : RADIO_CALL_PHONE_BUSY, 1);
                goto done;
            }
            if (s->state == RADIO_CALL_WAIT_DECISION && command == RC_FORCE) {
                s->state = RADIO_CALL_INCOMING; s->force = 1; rc_event(ctx, s);
            }
            if (s->state == RADIO_CALL_WAIT_DECISION) rc_send(ctx, s, ed, RC_PHONE_BUSY, 0);
            else if (s->state == RADIO_CALL_ANSWERING) rc_send(ctx, s, ed, RC_ACCEPT, 0);
            else if (s->ready) rc_send(ctx, s, ed, RC_RINGING, 0);
        }
    }
done:
    if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
    if (ctx) rc_schedule(ctx);
}

struct rc_targets_read { struct UTUN_INSTANCE* inst; uint64_t id; };

/* Поздний READ не оживляет отменённое приглашение/новый сервис, даже при повторном ID. */
static void rc_targets_ready(void* arg, int count, const uint8_t (*peers)[32]) {
    struct rc_targets_read* read = arg; struct radio_call_context* ctx = read->inst->radio_call;
    struct radio_call_session* s = NULL;
    for (unsigned i = 0; ctx && !ctx->stopping && i < RC_SLOTS; i++)
        if (ctx->sessions[i].id == read->id && ctx->sessions[i].targets_read == read) { s = &ctx->sessions[i]; break; }
    if (s) s->targets_read = NULL;
    u_free(read);
    if (!s || s->state == RADIO_CALL_FINISHED) return;
    if (count <= 0) { rc_finish(ctx, s, RADIO_CALL_UNAVAILABLE, 0); rc_schedule(ctx); return; }
    for (int i = 0; i < count; i++) {
        struct rc_target* target = &s->targets[s->count++];
        memcpy(target->peer_ed, peers[i], 32);
        target->state = RADIO_CALL_OUTGOING;
    }
    if (!s->count) { rc_finish(ctx, s, RADIO_CALL_UNAVAILABLE, 0); return; }
    s->response_started = get_time_tb();
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio call targets ready id=%llx count=%d", (unsigned long long)s->id, count);
    rc_event(ctx, s);
    if (s->state != RADIO_CALL_FINISHED) rc_transmit(ctx, s);
    rc_schedule(ctx);
}

void radio_call_execute(const struct radio_call_request* req) {
    struct radio_call_context* ctx = req && req->inst ? req->inst->radio_call : NULL;
    if (!ctx || ctx->stopping || !req->id) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call command without context/id"); return; }
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio call command id=%llx group=%s peer=%s action=%d devices=%d",
        (unsigned long long)req->id, sc_public_key_hex(req->group, (char[65]){0}), sc_public_key_hex(req->peer, (char[65]){0}), req->action, req->devices);
    radio_call_revalidate_members(req->inst, 0);
    if (req->action == RADIO_CALL_START) {
        const uint8_t* peer = req->peer;
        struct TOPO_GROUP* route = topo_groups_find_wire(req->inst->topo_groups, req->group);
        uint64_t group = route && route->group_type == TOPO_GROUP_TYPE_CHAT ? route->group_id : 0;
        if (!group || !topo_group_chat_key_allowed(req->inst, group, peer)) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call start denied group=%s peer=%s id=%llx",
                sc_public_key_hex(req->group, (char[65]){0}), sc_public_key_hex(peer, (char[65]){0}), (unsigned long long)req->id);
            struct radio_call_session failed = {.id = req->id, .group = group,
                .state = RADIO_CALL_FINISHED, .reason = RADIO_CALL_UNAVAILABLE};
            memcpy(failed.group_key, req->group, 32); memcpy(failed.peer_ed, peer, 32); rc_event(ctx, &failed); return;
        }
        if (rc_slot(ctx, req->id, group, peer, 0)) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call duplicate start id=%llx", (unsigned long long)req->id); return;
        }
        int busy = radio_call_busy(req->inst) || call_busy(req->inst);
        struct radio_call_session* s = rc_slot(ctx, req->id, group, peer, 1);
        if (!s) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call cache full at start");
            struct radio_call_session failed = {.id = req->id, .group = group,
                .state = RADIO_CALL_FINISHED, .reason = RADIO_CALL_BUSY};
            memcpy(failed.group_key, req->group, 32); memcpy(failed.peer_ed, peer, 32); rc_event(ctx, &failed); return;
        }
        s->state = RADIO_CALL_OUTGOING; s->response_started = s->started;
        if (busy) { rc_finish(ctx, s, RADIO_CALL_BUSY, 0); return; }
        struct rc_targets_read* read = u_calloc(1, sizeof(*read));
        if (!read) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call target READ token allocation failed"); rc_finish(ctx, s, RADIO_CALL_UNAVAILABLE, 0); return;
        }
        read->inst = req->inst; read->id = req->id; s->targets_read = read;
        rc_schedule(ctx);
        call_targets_read(req->inst, group, peer, req->devices, RC_TARGETS, rc_targets_ready, read); return;
    }
    struct radio_call_session* s = NULL;
    for (int i = 0; i < RC_SLOTS; ++i) if (ctx->sessions[i].id == req->id && ctx->sessions[i].state != RADIO_CALL_FINISHED)
        s = &ctx->sessions[i];
    if (!s) { DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio call stale action id=%llx action=%d", (unsigned long long)req->id, req->action); return; }
    if (get_time_tb() - s->started >= RADIO_CALL_RING_TB) { rc_finish(ctx, s, RADIO_CALL_NO_ANSWER, 1); return; }
    if (req->action == RADIO_CALL_CONTINUE && !s->incoming && !s->selected && s->state == RADIO_CALL_WAIT_DECISION) {
        for (unsigned i = 0; i < s->count; ++i) if (s->targets[i].state == RADIO_CALL_WAIT_DECISION) {
            s->targets[i].force = 1; s->targets[i].state = RADIO_CALL_OUTGOING;
        }
        s->state = RADIO_CALL_OUTGOING; s->response_started = get_time_tb(); rc_event(ctx, s); rc_transmit(ctx, s);
    } else if (req->action == RADIO_CALL_READY && s->incoming && s->state == RADIO_CALL_INCOMING) {
        s->ready = 1; rc_send(ctx, s, s->peer_ed, RC_RINGING, 0);
    } else if (req->action == RADIO_CALL_ANSWER && s->incoming && s->state == RADIO_CALL_INCOMING) {
        s->state = RADIO_CALL_ANSWERING; rc_event(ctx, s); rc_send(ctx, s, s->peer_ed, RC_ACCEPT, 0);
    } else if (req->action == RADIO_CALL_ACCEPT && s->incoming && s->state == RADIO_CALL_SELECTED) {
        int ready = radio_is_active(req->inst, s->group);
        if (!ready && radio_start_pending(req->inst, s->group)) {
            if (!s->accept_until) {
                s->accept_until = get_time_tb() + RADIO_CALL_RESPONSE_TB;
                DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio ACCEPT waits for settings READ id=%llx group=%llx",
                    (unsigned long long)s->id, (unsigned long long)s->group);
            }
        } else {
            if (!ready) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio selected device failed to activate id=%llx", (unsigned long long)s->id);
            rc_finish(ctx, s, ready ? RADIO_CALL_ACCEPTED : RADIO_CALL_AUDIO_ERROR, 1);
        }
    } else if (req->action == RADIO_CALL_DECLINE && s->incoming) rc_finish(ctx, s, RADIO_CALL_DECLINED, 1);
    else if (req->action == RADIO_CALL_CANCEL) rc_finish(ctx, s, RADIO_CALL_CANCELLED, 1);
    else if (req->action == RADIO_CALL_FAIL) rc_finish(ctx, s, RADIO_CALL_AUDIO_ERROR, 1);
    else if (req->action == RADIO_CALL_REJECT_BUSY) rc_finish(ctx, s, RADIO_CALL_BUSY, 1);
    else DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call invalid action=%d state=%u incoming=%u", req->action, s->state, s->incoming);
    rc_schedule(ctx);
}

void radio_call_trampoline(void* arg) { radio_call_execute(arg); u_free(arg); }

int radio_call_init(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call init without instance"); return -1; }
    if (inst->radio_call) return 0;
    struct radio_call_context* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call context allocation failed"); return -1; }
    ctx->inst = inst; inst->radio_call = ctx;
    if (etcp_router_bind(inst, ETCP_RT_ID_RADIO_CALL, rc_receive)) { radio_call_destroy(inst); return -1; }
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_RADIO_CALL, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_RADIO_CALL, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call router access policy initialization failed");
        radio_call_destroy(inst); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call initialized first_response_ms=2000 ring_ms=60000 cache=%d", RC_SLOTS);
    return 0;
}

void radio_call_destroy(struct UTUN_INSTANCE* inst) {
    struct radio_call_context* ctx = inst ? inst->radio_call : NULL;
    if (!ctx) return;
    ctx->stopping = 1;
    if (ctx->timer) { uasync_cancel_timeout(inst->ua, ctx->timer); ctx->timer = NULL; }
    for (int i = 0; i < RC_SLOTS; ++i) if (ctx->sessions[i].id && ctx->sessions[i].state != RADIO_CALL_FINISHED)
        rc_finish(ctx, &ctx->sessions[i], RADIO_CALL_CANCELLED, 0);
    etcp_router_unbind(inst, ETCP_RT_ID_RADIO_CALL);
    inst->radio_call = NULL; u_free(ctx);
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio call destroyed; timer and pending invitations released");
}
