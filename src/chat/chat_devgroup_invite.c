#include "chat_devgroup.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "member_sync.h"
#include "group_policy.h"
#include "../utun_instance.h"
#include "../routing_layer/route_crypto.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../transport_layer/etcp.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/strbuf.h"
#include "../../lib/db_async.h"
#include <openssl/rand.h>
#include <openssl/crypto.h>

#define DG_SLOTS 16
#define DG_TTL 1200000ULL
#define DG_RETRY 30000ULL
#define DG_OFFER 1
#define DG_PIN 2
#define DG_KEYS 3
#define DG_RESULT 4
#define DG_RETURN_PIN 5
#define DG_PHASE_FIRST 1
#define DG_PHASE_RETURN 2
#define DG_PHASE_IMPORT 3
struct dg_invite {
    uint8_t id[16], ed[32], group_ed[32];
    uint64_t group, expires, sent;
    unsigned state, outgoing, attempts, return_attempts, busy;
    uint8_t last_pin[4], last_result; /* Повторы одной попытки получают прежний результат. */
    uint8_t last_return_pin[4], return_result;
    char pin[5], entered_pin[5]; /* Свой PIN и введённый PIN пира; никогда не выводятся в журнал. */
};
struct chat_devgroup {
    struct UTUN_INSTANCE* inst;
    struct dg_invite slots[DG_SLOTS];
    void* timer;
    unsigned ticks;
};
static struct chat_devgroup* dg_ctx(struct UTUN_INSTANCE* inst) { return inst && CC(inst) ? CC(inst)->devgroup : NULL; }
static int dg_pending(unsigned state) { return state == DG_WAIT_PIN || state == DG_PIN_SENT || state == DG_KEYS_SENT || state >= DG_WAIT_RETURN; }
static void dg_clear_pins(struct dg_invite* p) {
    OPENSSL_cleanse(p->pin, sizeof(p->pin)); OPENSSL_cleanse(p->entered_pin, sizeof(p->entered_pin));
}
static int dg_random_pin(char pin[5], const char* different) {
    uint32_t random;
    do {
        do {
            if (RAND_bytes((uint8_t*)&random, sizeof(random)) != 1) {
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: PIN randomness failed"); return -1;
            }
        } while (random >= UINT32_MAX - UINT32_MAX % 10000);
        snprintf(pin, 5, "%04u", random % 10000);
    } while (different && !memcmp(pin, different, 4));
    return 0;
}
static void dg_hex(const uint8_t* in, size_t n, char* out) { for (size_t i = 0; i<n; i++) snprintf(out + 2*i, 3,"%02x", in[i]); }
static int dg_id(const char* text, uint8_t out[16]) {
    return !text || strlen(text) != 32 || strspn(text,"0123456789abcdef") != 32 || sc_hex_to_binary(text, out, 16) ? -1 : 0;
}
static int dg_peer(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32]) {
    if (!ed || !memcmp(ed, inst->my_ed25519_pubkey, 32) || !topo_group_chat_key_allowed(inst, group, ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup: peer unavailable group=%llu peer=%s",
            (unsigned long long)group, sc_public_key_hex(ed, (char[65]){0}));
        return -1;
    }
    return 0;
}
static struct dg_invite* dg_find(struct chat_devgroup* ctx, const uint8_t id[16]) {
    for (unsigned i = 0; i<DG_SLOTS; i++) if (ctx->slots[i].state && !memcmp(ctx->slots[i].id, id, 16)) return &ctx->slots[i];
    return NULL;
}
static struct dg_invite* dg_slot(struct chat_devgroup* ctx, const uint8_t ed[32]) {
    struct dg_invite* free_slot = NULL;
    for (unsigned i = 0; i<DG_SLOTS; i++) {
        struct dg_invite* p = &ctx->slots[i];
        if (dg_pending(p->state) && !memcmp(p->ed, ed, 32)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: peer already has pending invitation"); return NULL; }
        if (!p->state || (!dg_pending(p->state) && get_time_tb()>p->expires)) free_slot = p;
    }
    if (!free_slot) DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: invitation capacity reached");
    if (free_slot) OPENSSL_cleanse(free_slot, sizeof(*free_slot));
    return free_slot;
}
static int dg_send(struct chat_devgroup* ctx, struct dg_invite* p, uint8_t kind, const uint8_t* body, size_t n) {
    p->sent = get_time_tb();
    if (dg_peer(ctx->inst, p->group, p->ed)) return -1;
    struct ll_entry* entry = ll_alloc_lldgram(18 + n);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT,"devgroup: packet allocation failed"); return -1; }
    entry->dgram[0] = ETCP_RT_ID_DEVGROUP; entry->dgram[1] = kind; memcpy(entry->dgram + 2, p->id, 16);
    if (n) memcpy(entry->dgram + 18, body, n);
    entry->len = 18 + n;
    char id[33]; dg_hex(p->id, 16, id);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "devgroup TX: invite=%s peer=%s kind=%u state=%u bytes=%zu",
        id, sc_public_key_hex(p->ed, (char[65]){0}), kind, p->state, n);
    int rc = etcp_route_send_key(ctx->inst, p->group, p->ed, entry, 0, ROUTE_CRYPTO_SIGN|ROUTE_CRYPTO_ENCRYPT);
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: send failed peer=%s kind=%u", sc_public_key_hex(p->ed, (char[65]){0}), kind);
    return rc;
}
static void dg_changed(struct chat_devgroup* ctx) { chat_event_post(ctx->inst, CHAT_EVT_DEVGROUP_CHANGED, NULL, 0); }
void chat_devgroup_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct chat_devgroup* ctx = dg_ctx(inst); if (!ctx) return;
    int changed = 0;
    for (unsigned i = 0; i < DG_SLOTS; i++) {
        struct dg_invite* p = &ctx->slots[i];
        if (!dg_pending(p->state) || p->group != group) continue;
        if (!dg_peer(inst, group, p->ed)) continue;
        p->state = p->state == DG_KEYS_SENT ? DG_UNKNOWN : DG_REJECTED;
        dg_clear_pins(p); changed = 1;
        OPENSSL_cleanse(p->last_pin, sizeof(p->last_pin)); OPENSSL_cleanse(p->last_return_pin, sizeof(p->last_return_pin));
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup: invitation revoked group=%llu peer=%s state=%u",
            (unsigned long long)group, sc_public_key_hex(p->ed, (char[65]){0}), p->state);
    }
    if (changed) dg_changed(ctx);
}
static int dg_keys(struct chat_devgroup* ctx, struct dg_invite* p) {
    struct devgroup_record r; uint8_t ep[32], xp[32], data[64 + DEVGROUP_WIRE_MAX];
    int rc = devgroup_load_local(ctx->inst, &r, ep, xp);
    if (rc != 1 || memcmp(r.ed, p->group_ed, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: pending transfer cancelled after local membership changed"); p->state = 7; rc = -1;
    } else {
        int n = devgroup_encode(&r, data + 64, DEVGROUP_WIRE_MAX);
        if (n<0) rc = -1;
        else { memcpy(data, ep, 32); memcpy(data + 32, xp, 32); rc = dg_send(ctx, p, DG_KEYS, data, 64 + n); }
    }
    OPENSSL_cleanse(ep, 32); OPENSSL_cleanse(xp, 32); OPENSSL_cleanse(data, sizeof(data)); return rc;
}
static void dg_result(struct chat_devgroup* ctx, struct dg_invite* p, uint8_t phase, uint8_t code) {
    uint8_t result[3] = {phase, (uint8_t)(phase == DG_PHASE_RETURN ? p->return_attempts : p->attempts), code};
    dg_send(ctx, p, DG_RESULT, result, sizeof(result));
}
static void dg_send_pin(struct chat_devgroup* ctx, struct dg_invite* p) {
    uint8_t body[5] = {(uint8_t)(p->outgoing ? p->return_attempts : p->attempts)}; memcpy(body + 1, p->entered_pin, 4);
    dg_send(ctx, p, p->outgoing ? DG_RETURN_PIN : DG_PIN, body, sizeof(body)); OPENSSL_cleanse(body, sizeof(body));
}
/* Создание группы откладывается до двух ручных проверок. Пустая привязка допускает только создание новой группы. */
static int dg_prepare_keys(struct chat_devgroup* ctx, struct dg_invite* p) {
    struct devgroup_record own; uint8_t ep[32], xp[32], empty[32] = {0};
    int has = devgroup_load_local(ctx->inst, &own, ep, xp), pinned = memcmp(p->group_ed, empty, 32) != 0;
    OPENSSL_cleanse(ep, 32); OPENSSL_cleanse(xp, 32);
    if (!has && !pinned) {
        if (chat_devgroup_rename(ctx->inst, ctx->inst->name[0] ? ctx->inst->name : "My devices")) has = -1;
        else has = devgroup_load_local(ctx->inst, &own, ep, xp);
        OPENSSL_cleanse(ep, 32); OPENSSL_cleanse(xp, 32);
    }
    if (has != 1 || (pinned && memcmp(p->group_ed, own.ed, 32))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup: invitation cancelled after membership changed"); return -1;
    }
    memcpy(p->group_ed, own.ed, 32); return 0;
}
static int dg_job_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t id[16], int importing) {
    struct chat_devgroup* ctx = dg_ctx(inst); if (!ctx || !inst->group_started || !chat_core_is_initialized(inst)) return 0;
    struct dg_invite* p = dg_find(ctx, id);
    return p && p->group == group && peer && !memcmp(p->ed, peer, 32) && p->busy && get_time_tb() <= p->expires &&
        p->state == (importing ? DG_WAIT_KEYS : DG_RETURN_SENT) && p->outgoing == !importing &&
        !dg_peer(inst, group, p->ed);
}
int chat_devgroup_import_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t id[16]) {
    return id && dg_job_allowed(inst, group, peer, id, 1);
}
int chat_devgroup_ensure_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t id[16]) {
    return id && dg_job_allowed(inst, group, peer, id, 0);
}
enum dg_async_action { DG_AS_RECEIPT, DG_AS_IMPORT, DG_AS_ENSURE };
struct dg_async {
    struct UTUN_INSTANCE* inst; struct chat_devgroup* owner; uint64_t generation, common_generation;
    struct dg_invite request; enum dg_async_action action; /* Собственный идентификатор/ключи; указателя на slot нет. */
};
static void dg_async_done(void* data, const struct chat_devgroup_result* result) {
    struct dg_async* job = data; struct chat_devgroup* ctx = dg_ctx(job->inst);
    if (ctx != job->owner || job->inst->destroying || !job->inst->group_started ||
        chat_core_generation(job->inst) != job->generation || job->inst->group_generation != job->common_generation) goto out;
    if (job->action == DG_AS_RECEIPT) {
        if (!result->status && result->found) dg_result(ctx, &job->request, DG_PHASE_IMPORT, 0);
        else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "devgroup: repeated keys receipt unavailable peer=%s status=%d found=%d",
            sc_public_key_hex(job->request.ed, (char[65]){0}), result->status, result->found);
        goto out;
    }
    struct dg_invite* p = dg_find(ctx, job->request.id);
    if (!p || !p->busy || p->group != job->request.group || memcmp(p->ed, job->request.ed, 32) ||
        p->state != (job->action == DG_AS_IMPORT ? DG_WAIT_KEYS : DG_RETURN_SENT)) goto out;
    p->busy = 0;
    if (get_time_tb() > p->expires) { p->state = DG_EXPIRED; dg_clear_pins(p); dg_changed(ctx); goto out; }
    if (result->status || (job->action == DG_AS_ENSURE && dg_prepare_keys(ctx, p))) {
        p->state = DG_REJECTED; dg_clear_pins(p); dg_result(ctx, p, DG_PHASE_IMPORT, 2);
    } else if (job->action == DG_AS_IMPORT) {
        p->state = DG_JOINED; dg_clear_pins(p); dg_result(ctx, p, DG_PHASE_IMPORT, 0);
    } else { p->state = DG_KEYS_SENT; dg_keys(ctx, p); }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup: worker invitation continuation action=%d peer=%s state=%u status=%d committed=%d",
        job->action, sc_public_key_hex(p->ed, (char[65]){0}), p->state, result->status, result->committed);
    dg_changed(ctx);
out:
    OPENSSL_cleanse(job, sizeof(*job)); u_free(job);
}
static int dg_async_start(struct chat_devgroup* ctx, struct dg_invite* p, enum dg_async_action action,
    const struct devgroup_record* record, const uint8_t* keys) {
    struct dg_async* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup: continuation allocation failed action=%d", action); return -1; }
    job->inst = ctx->inst; job->owner = ctx; job->generation = chat_core_generation(ctx->inst);
    job->common_generation = ctx->inst->group_generation; job->request = *p; job->action = action;
    if (action == DG_AS_RECEIPT) chat_devgroup_receipt_submit(ctx->inst, p->id, p->ed, dg_async_done, job);
    else if (action == DG_AS_IMPORT) {
        p->busy = 1; chat_devgroup_import_submit(ctx->inst, p->group, p->ed, p->id, record, keys, keys + 32, dg_async_done, job);
    } else {
        p->busy = 1; chat_devgroup_ensure_submit(ctx->inst, p->group, p->ed, p->id,
            ctx->inst->name[0] ? ctx->inst->name : "My devices", dg_async_done, job);
    }
    return 0;
}
static void dg_receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct chat_devgroup* ctx = conn ? dg_ctx(conn->instance) : NULL;
    if (!ctx || !entry || !entry->dgram) goto out;
    if (entry->len == ROUTER_SVC_HDR_SIZE) goto out; /* reset маршрутизатора: retry остаётся у операции */
    if (entry->len<ROUTER_SVC_HDR_SIZE + 17 || (entry->dgram[ROUTER_SVC_FLAGS_OFF]&(ROUTER_FLAG_ENCRYPTED|ROUTER_FLAG_SIGNED)) != (ROUTER_FLAG_ENCRYPTED|ROUTER_FLAG_SIGNED)) goto bad;
    uint64_t group; memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* ed = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    if (dg_peer(ctx->inst, group, ed)) goto out;
    const uint8_t* data = entry->dgram + ROUTER_SVC_HDR_SIZE; uint8_t kind = *data++;
    const uint8_t* id = data; data+=16; size_t n = entry->len-ROUTER_SVC_HDR_SIZE-17;
    struct dg_invite* p = dg_find(ctx, id);
    char id_hex[33]; dg_hex(id, 16, id_hex);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "devgroup RX: invite=%s peer=%s kind=%u state=%u bytes=%zu",
        id_hex, sc_public_key_hex(ed, (char[65]){0}), kind, p ? p->state : 0, n);
    if (p && (p->group != group || memcmp(ed, p->ed, 32))) goto bad;
    if (kind == DG_KEYS && db_async_can_write(ctx->inst->db_worker)) {
        if (p && !p->outgoing && p->state == DG_WAIT_KEYS && get_time_tb() <= p->expires) {
            if (p->busy) goto out;
            struct devgroup_record record; if (n <= 64 || devgroup_decode(&record, data + 64, n - 64)) goto bad;
            if (dg_async_start(ctx, p, DG_AS_IMPORT, &record, data)) {
                p->state = DG_REJECTED; dg_clear_pins(p); dg_result(ctx, p, DG_PHASE_IMPORT, 2); dg_changed(ctx);
            }
        } else {
            struct dg_invite repeated = {.group=group};
            memcpy(repeated.id, id, 16); memcpy(repeated.ed, ed, 32);
            dg_async_start(ctx, &repeated, DG_AS_RECEIPT, NULL, NULL);
        }
        goto out;
    }
    if (p && p->busy) goto out;
    if (kind == DG_KEYS) {
        int receipt = chat_devgroup_receipt(ctx->inst, id, ed);
        if (receipt<0) goto out;
        if (receipt) {
            struct dg_invite reply = {.group = group}; memcpy(reply.id, id, 16); memcpy(reply.ed, ed, 32);
            dg_result(ctx, &reply, DG_PHASE_IMPORT, 0); goto out;
        }
    }
    if (kind == DG_OFFER) {
        if (n) goto bad;
        if (!p) {
            p = dg_slot(ctx, ed); if (!p) goto out;
            p->state = 1; p->group = group; p->expires = get_time_tb() + DG_TTL;
            memcpy(p->id, id, 16); memcpy(p->ed, ed, 32);
            DEBUG_INFO(DEBUG_CATEGORY_CHAT,"devgroup: invitation received peer=%s", sc_public_key_hex(ed, (char[65]){0}));
            dg_changed(ctx);
        }
        goto out;
    }
    if (!p || get_time_tb()>p->expires) { DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: expired/unknown invitation packet kind=%u", kind); goto out; }
    if ((kind == DG_PIN && p->outgoing) || (kind == DG_RETURN_PIN && !p->outgoing)) {
        if (n != 5 || data[0] < 1 || data[0] > 3) goto bad;
        const int first = kind == DG_PIN;
        const uint8_t phase = first ? DG_PHASE_FIRST : DG_PHASE_RETURN;
        unsigned* attempts = first ? &p->attempts : &p->return_attempts;
        uint8_t* last_pin = first ? p->last_pin : p->last_return_pin;
        uint8_t* result = first ? &p->last_result : &p->return_result;
        if (data[0] == *attempts) {
            if (CRYPTO_memcmp(data + 1, last_pin, 4)) goto bad;
            dg_result(ctx, p, phase, *result); goto out;
        }
        if (data[0] != *attempts + 1 || p->state != (first ? DG_WAIT_PIN : DG_WAIT_RETURN)) goto bad;
        *attempts = data[0]; memcpy(last_pin, data + 1, 4);
        if (CRYPTO_memcmp(data + 1, p->pin, 4)) {
            if (*attempts >= 3) { p->state = DG_REJECTED; dg_clear_pins(p); }
            *result = p->state == DG_REJECTED ? 2 : 1;
            DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: PIN rejected peer=%s phase=%u attempt=%u",
                sc_public_key_hex(ed, (char[65]){0}), phase, *attempts);
        } else {
            p->state = first ? DG_WAIT_RETURN : DG_WAIT_KEYS; *result = 0;
            dg_clear_pins(p);
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup: PIN verified peer=%s phase=%u state=%u",
                sc_public_key_hex(ed, (char[65]){0}), phase, p->state);
        }
        dg_result(ctx, p, phase, *result); dg_changed(ctx);
    } else if (kind == DG_KEYS && !p->outgoing && p->state == DG_WAIT_KEYS) {
        if (n <= 64) goto bad;
        struct devgroup_record r;
        if (devgroup_decode(&r, data + 64, n-64)) goto bad;
        int rc = chat_devgroup_import(ctx->inst, &r, data, data + 32, p->id, p->ed);
        p->state = rc ? DG_REJECTED : DG_JOINED; dg_clear_pins(p);
        dg_result(ctx, p, DG_PHASE_IMPORT, rc ? 2 : 0); dg_changed(ctx);
    } else if (kind == DG_RESULT) {
        if (n != 3 || data[0] < DG_PHASE_FIRST || data[0] > DG_PHASE_IMPORT || data[2] > 2) goto bad;
        if (data[0] == DG_PHASE_FIRST && !p->outgoing && p->state == DG_PIN_SENT && data[1] == p->attempts) {
            p->state = data[2] == 0 ? DG_WAIT_RETURN : data[2] == 1 ? DG_WAIT_PIN : DG_REJECTED;
            OPENSSL_cleanse(p->entered_pin, sizeof(p->entered_pin));
            if (data[2]) dg_clear_pins(p);
        } else if (data[0] == DG_PHASE_RETURN && p->outgoing && p->state == DG_RETURN_SENT && data[1] == p->return_attempts) {
            dg_clear_pins(p);
            if (data[2]) p->state = data[2] == 1 ? DG_WAIT_RETURN : DG_REJECTED;
            else if (db_async_can_write(ctx->inst->db_worker)) {
                if (dg_async_start(ctx, p, DG_AS_ENSURE, NULL, NULL)) { p->state = DG_REJECTED; dg_result(ctx, p, DG_PHASE_IMPORT, 2); }
            }
            else if (dg_prepare_keys(ctx, p)) { p->state = DG_REJECTED; dg_result(ctx, p, DG_PHASE_IMPORT, 2); }
            else { p->state = DG_KEYS_SENT; dg_keys(ctx, p); }
        } else if (data[0] == DG_PHASE_IMPORT && p->outgoing && p->state == DG_KEYS_SENT) {
            p->state = data[2] ? DG_UNKNOWN : DG_JOINED; dg_clear_pins(p);
        } else if (data[0] == DG_PHASE_IMPORT && !p->outgoing && p->state == DG_WAIT_KEYS && data[2] == 2) {
            p->state = DG_REJECTED; dg_clear_pins(p);
        } else goto out;
        DEBUG_INFO(DEBUG_CATEGORY_CHAT,"devgroup: invitation result peer=%s state=%u", sc_public_key_hex(ed, (char[65]){0}), p->state); dg_changed(ctx);
    } else goto bad;
    goto out;
bad:
    DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: malformed/unexpected invitation packet bytes=%u", entry ? entry->len : 0);
out:
    if (entry) { if (entry->dgram) OPENSSL_cleanse(entry->dgram, entry->len); queue_dgram_free(entry); queue_entry_free(entry); }
}
static void dg_tick(void* arg) {
    struct chat_devgroup* ctx = arg; ctx->timer = NULL; uint64_t now = get_time_tb();
    for (unsigned i = 0; i<DG_SLOTS; i++) {
        struct dg_invite* p = &ctx->slots[i]; if (!dg_pending(p->state)) continue;
        if (now>p->expires) {
            p->state = p->state == DG_KEYS_SENT ? DG_UNKNOWN : DG_EXPIRED; dg_clear_pins(p);
            DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: invitation timeout peer=%s state=%u", sc_public_key_hex(p->ed, (char[65]){0}), p->state);
            dg_changed(ctx); continue;
        }
        if (p->busy) continue;
        if (now-p->sent<DG_RETRY) continue;
        if (p->outgoing && p->state == 1) dg_send(ctx, p, DG_OFFER, NULL, 0);
        else if (p->outgoing && p->state == 3) dg_keys(ctx, p);
        else if (!p->outgoing && p->state == 2) dg_send_pin(ctx, p);
        else if (p->outgoing && p->state == DG_RETURN_SENT) dg_send_pin(ctx, p);
        else if (!p->outgoing && p->state == DG_WAIT_KEYS) dg_result(ctx, p, DG_PHASE_RETURN, 0);
    }
    if (++ctx->ticks%5 == 0) chat_devgroup_refresh(ctx->inst);
    ctx->timer = uasync_set_timeout(ctx->inst->ua, 10000, ctx, dg_tick,"devgroup");
    if (!ctx->timer) DEBUG_ERROR(DEBUG_CATEGORY_CHAT,"devgroup: timer allocation failed");
}
int chat_devgroup_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst) || CC(inst)->devgroup) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT,"devgroup: invalid init"); return -1; }
    struct chat_devgroup* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT,"devgroup: context allocation failed"); return -1; }
    ctx->inst = inst; CC(inst)->devgroup = ctx;
    if (etcp_router_bind(inst, ETCP_RT_ID_DEVGROUP, dg_receive)
        || etcp_router_require_crypto(inst, ETCP_RT_ID_DEVGROUP, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)
        || etcp_router_set_authorize(inst, ETCP_RT_ID_DEVGROUP, topo_group_chat_key_allowed)) goto fail;
    ctx->timer = uasync_set_timeout(inst->ua, 10000, ctx, dg_tick,"devgroup"); if (!ctx->timer) goto fail;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT,"devgroup: invitation service started"); return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT,"devgroup: service initialization failed"); chat_devgroup_destroy(inst); return -1;
}
void chat_devgroup_cancel_pending(struct UTUN_INSTANCE* inst) {
    struct chat_devgroup* ctx = dg_ctx(inst); if (!ctx) return;
    for (unsigned i = 0; i < DG_SLOTS; i++) {
        struct dg_invite* p = &ctx->slots[i]; if (!dg_pending(p->state)) continue;
        p->state = p->state == 3 ? 7 : 5;
        dg_clear_pins(p);
        OPENSSL_cleanse(p->last_pin, sizeof(p->last_pin)); OPENSSL_cleanse(p->last_return_pin, sizeof(p->last_return_pin));
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup: invitation cancelled peer=%s state=%u", sc_public_key_hex(p->ed, (char[65]){0}), p->state);
    }
    dg_changed(ctx);
}
void chat_devgroup_destroy(struct UTUN_INSTANCE* inst) {
    struct chat_devgroup* ctx = dg_ctx(inst); if (!ctx) return;
    chat_devgroup_cancel_pending(inst);
    if (ctx->timer) uasync_cancel_timeout(inst->ua, ctx->timer);
    etcp_router_unbind(inst, ETCP_RT_ID_DEVGROUP); CC(inst)->devgroup = NULL;
    OPENSSL_cleanse(ctx, sizeof(*ctx)); u_free(ctx); DEBUG_INFO(DEBUG_CATEGORY_CHAT,"devgroup: invitation service stopped");
}
int chat_devgroup_invite(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], char out_id[33], char out_pin[5]) {
    struct chat_devgroup* ctx = dg_ctx(inst);
    if (!ctx || !out_id || !out_pin || dg_peer(inst, group, ed)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: invite rejected"); return -1; }
    struct dg_invite* p = dg_slot(ctx, ed); if (!p) return -1;
    struct devgroup_record own; uint8_t ep[32], xp[32];
    int has = devgroup_load_local(inst, &own, ep, xp);
    OPENSSL_cleanse(ep, 32); OPENSSL_cleanse(xp, 32);
    if (has < 0) return -1;
    if (has) memcpy(p->group_ed, own.ed, 32); // Нулевая привязка: группа создаётся после проверки PIN.

    if (dg_random_pin(p->pin, NULL)) return -1;
    if (RAND_bytes(p->id, 16) != 1) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT,"devgroup: invitation randomness failed"); return -1; }
    p->group = group; p->outgoing = 1; p->state = DG_WAIT_PIN; p->expires = get_time_tb() + DG_TTL;
    memcpy(p->ed, ed, 32); dg_hex(p->id, 16, out_id); memcpy(out_pin, p->pin, 5);
    dg_send(ctx, p, DG_OFFER, NULL, 0); DEBUG_INFO(DEBUG_CATEGORY_CHAT,"devgroup: invitation created peer=%s", sc_public_key_hex(ed, (char[65]){0}));
    dg_changed(ctx); return 0;
}
int chat_devgroup_accept(struct UTUN_INSTANCE* inst, const char* id, const char* pin) {
    struct chat_devgroup* ctx = dg_ctx(inst); uint8_t binary[16];
    if (!ctx || dg_id(id, binary) || !pin || strlen(pin) != 4 || strspn(pin,"0123456789") != 4) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: invalid acceptance arguments"); return -1;
    }
    struct dg_invite* p = dg_find(ctx, binary);
    if (p) chat_devgroup_revalidate_members(inst, p->group);
    if (!p || get_time_tb()>p->expires || p->state != (p->outgoing ? DG_WAIT_RETURN : DG_WAIT_PIN)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: acceptance unavailable"); return -1;
    }
    unsigned* attempts = p->outgoing ? &p->return_attempts : &p->attempts;
    if (*attempts >= 3) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup: PIN attempts exhausted"); return -1; }
    if (!p->outgoing && dg_random_pin(p->pin, pin)) return -1;
    memcpy(p->entered_pin, pin, 5); (*attempts)++; p->state = p->outgoing ? DG_RETURN_SENT : DG_PIN_SENT;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup: PIN submitted peer=%s phase=%u attempt=%u",
        sc_public_key_hex(p->ed, (char[65]){0}), p->outgoing ? DG_PHASE_RETURN : DG_PHASE_FIRST, *attempts);
    dg_send_pin(ctx, p); dg_changed(ctx); return 0;
}
char* chat_devgroup_status(struct UTUN_INSTANCE* inst) {
    struct chat_devgroup* ctx = dg_ctx(inst); if (!ctx) { DEBUG_WARN(DEBUG_CATEGORY_CHAT,"devgroup: status unavailable"); return NULL; }
    struct devgroup_record r; struct devgroup_profile profile; uint8_t ep[32], xp[32]; char id[65] = "";
    int found = devgroup_load_local(inst, &r, ep, xp); OPENSSL_cleanse(ep, 32); OPENSSL_cleanse(xp, 32); if (found<0) return NULL;
    struct strbuf json = {0};
    if (found) { if (devgroup_profile_read(&r, &profile)) return NULL; dg_hex(r.ed, 32, id); }
    if (strbuf_addf(&json,"{\"group\":\"%s\",\"name\":", id)<0 || strbuf_add_json_string(&json, found ? profile.name : "") || strbuf_addf(&json,",\"invitations\":[")<0) goto fail;
    unsigned count = 0;
    for (unsigned i = 0; i<DG_SLOTS; i++) {
        struct dg_invite* p = &ctx->slots[i]; if (!p->state) continue; dg_hex(p->id, 16, id);
        int input = !p->outgoing && p->state == DG_WAIT_PIN ? 1 : p->outgoing && p->state == DG_WAIT_RETURN ? 2 : 0;
        int show_pin = p->outgoing ? p->state == DG_WAIT_PIN : p->state == DG_WAIT_RETURN || p->state == DG_PIN_SENT;
        int error = input && (p->outgoing ? p->return_attempts : p->attempts);
        if (strbuf_addf(&json,"%s{\"id\":\"%s\",\"peer\":\"%s\",\"channel\":\"%s\",\"outgoing\":%u,\"state\":%u,"
            "\"pin\":\"%s\",\"input\":%d,\"pin_error\":%d}", count++ ? "," : "", id, sc_public_key_hex(p->ed, (char[65]){0}),
            topo_groups_channel(inst->topo_groups, p->group), p->outgoing, p->state, show_pin ? p->pin : "", input, error)<0) goto fail;
    }
    if (strbuf_addf(&json,"]}")<0) goto fail;
    return strbuf_detach(&json);
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT,"devgroup: status allocation failed"); strbuf_free(&json); return NULL;
}
