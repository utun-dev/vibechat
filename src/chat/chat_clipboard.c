#include "chat_clipboard.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "group_info.h"
#include "group_policy.h"
#include "../utun_instance.h"
#include "../routing_layer/devgroup.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/route_crypto.h"
#include "../transport_layer/etcp.h"
#include "../dm/dm_core.h"
#include "../media_delivery/file_transfer.h"
#include "../../lib/mem.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>

#define CB_CHUNK 8192u
#define CB_HEADER 27u /* cmd1,id16,type1,size4,offset4,scope1 */
#define CB_DATA 1
#define CB_RESULT 2
#define CB_RX_SLOTS 4
#define CB_RX_BUDGET (64u * 1024u * 1024u)
#define CB_IDLE 300000ULL /* get_time_tb: 0.1 мс */
#define CB_LIFETIME 1200000ULL

struct cb_target {
    struct cb_target* next;
    struct chat_clipboard* owner;
    struct router_send_request ready;
    uint64_t group, progress;
    uint8_t peer_ed[32];
    uint32_t offset;
    unsigned state; /* 0=pending,1=applied,2=unavailable,3=failed */
};
struct cb_receive {
    uint64_t group, progress, started;
    uint8_t peer_ed[32];
    uint8_t id[16], *data;
    uint32_t size, offset;
    uint8_t type, posted, scope;
};
struct cb_source {
    struct chat_clipboard_source file;
    uint64_t group; /* Ненулевая — offer участнику, 0 — собственным устройствам. */
    uint8_t peer_ed[32];
    uint8_t devices[64]; /* Идентичность своей devgroup на момент публикации own offer. */
};
struct cb_device_job {
    uint8_t self[32]; /* Только frozen SQL-вход/выход до owner ниже. */
    uint8_t (*nodes)[32];
    size_t count;
    uint8_t ed[32], x[32];
    struct chat_clipboard* owner; /* Weak uasync owner; stop/timeout отсоединяет, SQL не читает. */
    int* acceptance; /* Только синхронный отказ submit; сразу после принятия обнуляется. */
};
struct chat_clipboard {
    struct UTUN_INSTANCE* inst;
    void* timer;
    struct cb_target* targets;
    uint8_t *data, id[16];
    uint64_t request, started;
    uint32_t size;
    uint8_t type, scope;
    struct cb_receive receive[CB_RX_SLOTS];
    struct file_transfer* files;
    struct cb_source* sources;
    unsigned source_count;
    struct cb_device_job* lookup;
};
static uint64_t cb_get(const uint8_t* p, unsigned n) {
    uint64_t v = 0; for (unsigned i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i); return v;
}
static void cb_put(uint8_t* p, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static struct chat_clipboard* cb_ctx(struct UTUN_INSTANCE* inst) { return inst && CC(inst) ? CC(inst)->clipboard : NULL; }
static void cb_free(uint8_t* p, size_t size) { if (p) { OPENSSL_cleanse(p, size); u_free(p); } }
static int cb_size(int type, size_t size) {
    return size && ((type == CHAT_CLIPBOARD_TEXT && size <= CHAT_CLIPBOARD_TEXT_MAX) ||
        (type == CHAT_CLIPBOARD_FILES && size <= CHAT_CLIPBOARD_TEXT_MAX) ||
        (type == CHAT_CLIPBOARD_PNG && size <= CHAT_CLIPBOARD_IMAGE_MAX));
}
/* Вызывается router до очередей, при каждом приёме/отправке и ретрансмите. */
static int cb_own(struct UTUN_INSTANCE* inst, const uint8_t peer[32]) {
    struct devgroup_record own, other;
    return devgroup_member_by_key(inst, inst->my_ed25519_pubkey, &own) == 1 &&
        devgroup_member_by_key(inst, peer, &other) == 1 && !memcmp(own.ed, other.ed, 32) && !memcmp(own.x, other.x, 32);
}
static int cb_member_permission(struct UTUN_INSTANCE* inst, uint64_t group) {
    const char* channel = topo_groups_channel(inst->topo_groups, group);
    return db_async_can_write(inst->db_worker) ? group_policy_clipboard_members(inst, channel)
        : group_info_clipboard_allowed(inst->topo_sqlite_db, channel);
}
static int cb_mode_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], int scope) {
    if (!inst || !chat_core_generation(inst) || !memcmp(peer, inst->my_ed25519_pubkey, 32) ||
        !topo_group_chat_key_allowed(inst, group, peer)) return 0;
    return scope == CHAT_CLIPBOARD_OWN ? cb_own(inst, peer) : scope == CHAT_CLIPBOARD_MEMBER && cb_member_permission(inst, group);
}
static int cb_key_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]) {
    if (!inst || !chat_core_generation(inst) || !peer || !memcmp(peer, inst->my_ed25519_pubkey, 32) ||
        !topo_group_chat_key_allowed(inst, group, peer)) return 0;
    return cb_member_permission(inst, group) || cb_own(inst, peer);
}
static int cb_source_allowed(struct chat_clipboard* ctx, const struct cb_source* s) {
    if (s->group) return cb_mode_allowed(ctx->inst, s->group, s->peer_ed, CHAT_CLIPBOARD_MEMBER);
    struct devgroup_record own;
    return devgroup_member_by_key(ctx->inst, ctx->inst->my_ed25519_pubkey, &own) == 1 &&
        !memcmp(own.ed, s->devices, 32) && !memcmp(own.x, s->devices + 32, 32);
}
void chat_clipboard_revalidate(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct chat_clipboard* ctx = cb_ctx(inst); if (!ctx) return;
    unsigned kept = 0, revoked = 0;
    struct devgroup_record own; int own_read = -1, member_allowed = 0;
    uint64_t checked_group = 0;
    uint8_t checked_peer[32] = {0};
    for (unsigned i = 0; i < ctx->source_count; i++) {
        struct cb_source* s = &ctx->sources[i];
        int allowed = 1;
        if (!s->group) {
            if (own_read < 0) own_read = devgroup_member_by_key(inst, inst->my_ed25519_pubkey, &own) == 1;
            allowed = own_read == 1 && !memcmp(own.ed, s->devices, 32) && !memcmp(own.x, s->devices + 32, 32);
        } else if (!group || s->group == group) {
            // Файлы одного offer идут подряд: членство/JSON читаем один раз на адресата, а не на каждый файл каталога.
            if (checked_group != s->group || memcmp(checked_peer, s->peer_ed, 32)) {
                checked_group = s->group; memcpy(checked_peer, s->peer_ed, 32);
                member_allowed = cb_mode_allowed(inst, s->group, s->peer_ed, CHAT_CLIPBOARD_MEMBER);
            }
            allowed = member_allowed;
        }
        if (!allowed) {
            file_transfer_cancel_object(ctx->files, s->file.object); revoked++;
        } else ctx->sources[kept++] = *s;
    }
    ctx->source_count = kept;
    if (revoked) DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard file offers revoked group=%llu files=%u remaining=%u",
        (unsigned long long)group, revoked, kept);
}
static int cb_file_lookup(void* arg, uint64_t group, const uint8_t ed[32], const uint8_t object[16], uint64_t size, char* path, size_t cap) {
    struct chat_clipboard* ctx = arg;
    for (unsigned i = 0; i < ctx->source_count; i++) {
        const struct cb_source* access = &ctx->sources[i]; const struct chat_clipboard_source* s = &access->file;
        if (s->size != size || memcmp(s->object, object, 16)) continue;
        if (!cb_source_allowed(ctx, access) || (access->group ? memcmp(access->peer_ed, ed, 32) || access->group != group : !cb_own(ctx->inst, ed))) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard file object access denied group=%llu peer=%s", (unsigned long long)group, sc_public_key_hex(ed, (char[65]){0}));
            return -1;
        }
        size_t n = strlen(s->path);
        if (n >= cap) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard file source path too long"); return -1; }
        memcpy(path, s->path, n + 1); return 0;
    }
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard requested unknown/expired file peer=%s bytes=%llu",
        sc_public_key_hex(ed, (char[65]){0}), (unsigned long long)size); return -1;
}
static void cb_report(struct chat_clipboard* ctx, uint64_t request, int state) {
    uint32_t total = 0, delivered = 0, unavailable = 0, failed = 0;
    if (state != CHAT_CLIPBOARD_REJECTED) for (struct cb_target* p = ctx->targets; p; p = p->next) {
        total++; delivered += p->state == 1; unavailable += p->state == 2; failed += p->state == 3;
    }
    uint8_t event[25]; cb_put(event, request, 8); event[8] = state;
    cb_put(event + 9, total, 4); cb_put(event + 13, delivered, 4);
    cb_put(event + 17, unavailable, 4); cb_put(event + 21, failed, 4);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard result: request=%llu state=%d total=%u delivered=%u unavailable=%u failed=%u",
        (unsigned long long)request, state, total, delivered, unavailable, failed);
    chat_event_post(ctx->inst, CHAT_EVT_CLIPBOARD_RESULT, event, sizeof(event));
}
static void cb_rx_clear(struct cb_receive* rx) { cb_free(rx->data, rx->size); memset(rx, 0, sizeof(*rx)); }
static void cb_tx_clear(struct chat_clipboard* ctx) {
    if (ctx->lookup) { ctx->lookup->owner = NULL; ctx->lookup = NULL; }
    while (ctx->targets) {
        struct cb_target* p = ctx->targets; ctx->targets = p->next;
        etcp_router_cancel_request(&p->ready); u_free(p);
    }
    cb_free(ctx->data, ctx->size); ctx->data = NULL; ctx->size = 0; ctx->request = 0;
}
/* Поиск не создаёт новый канал. При close/reset сервис уже снят с реестра router. */
static void cb_close(struct chat_clipboard* ctx, uint64_t group, const uint8_t peer[32]) {
    struct ETCP_ROUTER_CONN* conn = etcp_router_conn_lookup(ctx->inst, group, peer, ETCP_RT_ID_CLIPBOARD);
    if (conn) etcp_router_conn_close(conn);
}
static void cb_fail(struct cb_target* p, const char* reason) {
    if (p->state) return;
    p->state = 3; etcp_router_cancel_request(&p->ready);
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard TX failed: request=%llu peer=%s group=%llu offset=%u reason=%s",
        (unsigned long long)p->owner->request, sc_public_key_hex(p->peer_ed, (char[65]){0}), (unsigned long long)p->group, p->offset, reason);
    cb_close(p->owner, p->group, p->peer_ed);
}
static void cb_produce(struct ETCP_ROUTER_CONN* conn, void* arg) {
    struct cb_target* p = arg; struct chat_clipboard* ctx = p->owner;
    if (p->state || p->offset >= ctx->size) return;
    if (!cb_mode_allowed(ctx->inst, p->group, p->peer_ed, ctx->scope)) { cb_fail(p, "clipboard permission revoked"); return; }
    uint32_t n = ctx->size - p->offset; if (n > CB_CHUNK) n = CB_CHUNK;
    uint8_t packet[CB_HEADER + CB_CHUNK]; packet[0] = CB_DATA; memcpy(packet + 1, ctx->id, 16);
    packet[17] = ctx->type; cb_put(packet + 18, ctx->size, 4); cb_put(packet + 22, p->offset, 4);
    packet[26] = ctx->scope;
    memcpy(packet + CB_HEADER, ctx->data + p->offset, n);
    int rc = etcp_router_conn_send(conn, packet, CB_HEADER + n, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
    OPENSSL_cleanse(packet, CB_HEADER + n);
    if (rc) { cb_fail(p, "router send rejected"); return; }
    p->offset += n; p->progress = get_time_tb();
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT, "clipboard TX chunk: peer=%s offset=%u total=%u",
        sc_public_key_hex(p->peer_ed, (char[65]){0}), p->offset, ctx->size);
    if (p->offset < ctx->size && etcp_router_request_send(conn, &p->ready, cb_produce, p)) cb_fail(p, "producer registration");
    else if (p->offset == ctx->size) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "clipboard TX awaiting frontend ACK: peer=%s bytes=%u",
        sc_public_key_hex(p->peer_ed, (char[65]){0}), ctx->size);
}
static void cb_reply(struct chat_clipboard* ctx, uint64_t group, const uint8_t peer[32], const uint8_t id[16], int success) {
    struct ll_entry* e = ll_alloc_lldgram(19);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard ACK allocation failed"); return; }
    e->dgram[0] = ETCP_RT_ID_CLIPBOARD; e->dgram[1] = CB_RESULT;
    memcpy(e->dgram + 2, id, 16); e->dgram[18] = success != 0; e->len = 19;
    if (etcp_route_send_key(ctx->inst, group, peer, e, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT))
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard ACK send failed peer=%s", sc_public_key_hex(peer, (char[65]){0}));
}
void chat_clipboard_applied(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t id[16], int success) {
    struct chat_clipboard* ctx = cb_ctx(inst);
    if (ctx && id && peer) for (unsigned i = 0; i < CB_RX_SLOTS; i++) {
        struct cb_receive* rx = &ctx->receive[i];
        if (!rx->posted || rx->group != group || memcmp(rx->peer_ed, peer, 32) || memcmp(rx->id, id, 16)) continue;
        if (cb_mode_allowed(inst, group, peer, rx->scope)) cb_reply(ctx, group, peer, id, success);
        else DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard frontend result revoked peer=%s", sc_public_key_hex(peer, (char[65]){0}));
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard RX applied: peer=%s type=%u bytes=%u success=%d",
            sc_public_key_hex(peer, (char[65]){0}), rx->type, rx->size, success != 0);
        cb_rx_clear(rx); return;
    }
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard frontend result expired/unknown peer=%s", sc_public_key_hex(peer ? peer : (uint8_t[32]){0}, (char[65]){0}));
}
static void cb_receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct chat_clipboard* ctx = conn ? cb_ctx(conn->instance) : NULL;
    if (!ctx || !entry || !entry->dgram || entry->len < ROUTER_SVC_HDR_SIZE) goto done;
    uint64_t group; memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* ed = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    char peer[65]; sc_public_key_hex(ed, peer);
    size_t n = entry->len - ROUTER_SVC_HDR_SIZE;
    const uint8_t* packet = entry->dgram + ROUTER_SVC_HDR_SIZE;
    if (!n) {
        for (struct cb_target* p = ctx->targets; p; p = p->next)
            if (p->group == group && !memcmp(p->peer_ed, ed, 32)) cb_fail(p, "router reset/close");
        for (unsigned i = 0; i < CB_RX_SLOTS; i++) if (ctx->receive[i].group == group && !memcmp(ctx->receive[i].peer_ed, ed, 32)) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard RX aborted: router reset/close peer=%s", peer);
            cb_rx_clear(&ctx->receive[i]);
        }
        goto done;
    }
    if ((entry->dgram[ROUTER_SVC_FLAGS_OFF] & (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) !=
        (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) || !cb_key_allowed(ctx->inst, group, ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard RX denied: peer=%s group=%llu", peer, (unsigned long long)group);
        goto done;
    }
    if (!ctx->timer) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard RX rejected: watchdog unavailable peer=%s", peer);
        if (n >= 17 && packet[0] == CB_DATA) cb_reply(ctx, group, ed, packet + 1, 0);
        goto done;
    }
    if (n == 18 && packet[0] == CB_RESULT && packet[17] <= 1) {
        if (ctx->data && !memcmp(packet + 1, ctx->id, 16)) for (struct cb_target* p = ctx->targets; p; p = p->next) {
            if (p->state || p->group != group || memcmp(p->peer_ed, ed, 32)) continue;
            if (packet[17] && p->offset != ctx->size) { cb_fail(p, "premature applied ACK"); break; }
            p->state = packet[17] ? 1 : 3; etcp_router_cancel_request(&p->ready);
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard TX acknowledged: peer=%s applied=%u", peer, packet[17]);
        }
        goto done;
    }
    if (n <= CB_HEADER || n > CB_HEADER + CB_CHUNK || packet[0] != CB_DATA) goto bad;
    if (!cb_mode_allowed(ctx->inst, group, ed, packet[26])) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard RX scope denied peer=%s group=%llu scope=%u",
            peer, (unsigned long long)group, packet[26]); goto reject;
    }
    uint32_t size = (uint32_t)cb_get(packet + 18, 4), offset = (uint32_t)cb_get(packet + 22, 4);
    n -= CB_HEADER;
    if (!cb_size(packet[17], size) || offset > size || n > size - offset) goto bad;
    struct cb_receive *rx = NULL, *empty = NULL; uint32_t allocated = 0;
    for (unsigned i = 0; i < CB_RX_SLOTS; i++) {
        struct cb_receive* r = &ctx->receive[i]; if (!r->group) empty = r;
        allocated += r->group ? r->size : 0; /* Учитываем и полный снимок, ещё ожидающий применения frontend. */
        if (r->group == group && !memcmp(r->peer_ed, ed, 32)) rx = r;
    }
    if (!rx && !offset && empty && size <= CB_RX_BUDGET - allocated) {
        rx = empty; rx->data = u_malloc(size);
        if (!rx->data) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard RX allocation failed bytes=%u", size); goto reject; }
        rx->group = group; memcpy(rx->peer_ed, ed, 32); rx->type = packet[17]; rx->size = size;
        rx->scope = packet[26];
        rx->started = get_time_tb(); memcpy(rx->id, packet + 1, 16);
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard RX started: peer=%s type=%u bytes=%u", peer, rx->type, size);
    }
    if (!rx || rx->posted || memcmp(rx->id, packet + 1, 16) || rx->size != size || rx->type != packet[17] || rx->offset != offset || rx->scope != packet[26]) {
        const char* reason = !rx ? (!empty ? "receive slots exhausted" : size > CB_RX_BUDGET - allocated ? "receive budget exhausted" :
            "missing initial chunk") : rx->posted ? "awaiting frontend" : memcmp(rx->id, packet + 1, 16) ? "transfer ID mismatch" :
            rx->size != size ? "total size changed" : rx->type != packet[17] ? "content type changed" : "offset mismatch";
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard RX rejected: peer=%s offset=%u expected=%u bytes=%u allocated=%u reason=%s",
            peer, offset, rx ? rx->offset : 0, size, allocated, reason); goto reject;
    }
    memcpy(rx->data + offset, packet + CB_HEADER, n); rx->offset += (uint32_t)n; rx->progress = get_time_tb();
    DEBUG_TRACE(DEBUG_CATEGORY_CHAT, "clipboard RX chunk: peer=%s offset=%u total=%u", peer, rx->offset, size);
    if (rx->offset == size) {
        struct TOPO_GROUP* route = topo_groups_find(ctx->inst->topo_groups, group);
        const uint8_t* group_key = topo_group_wire_id(route);
        if (!group_key) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard receive group lost route=%llu", (unsigned long long)group); cb_rx_clear(rx); goto reject; }
        uint8_t* event = u_malloc(CHAT_CLIPBOARD_EVENT_HEADER + size);
        if (!event) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard RX event allocation failed"); cb_rx_clear(rx); goto reject; }
        memcpy(event, rx->id, 16); memcpy(event + CHAT_CLIPBOARD_EVENT_GROUP_OFF, group_key, 32);
        memcpy(event + CHAT_CLIPBOARD_EVENT_PEER_ED_OFF, rx->peer_ed, 32);
        event[CHAT_CLIPBOARD_EVENT_TYPE_OFF] = rx->type; event[CHAT_CLIPBOARD_EVENT_SCOPE_OFF] = rx->scope;
        memcpy(event + CHAT_CLIPBOARD_EVENT_HEADER, rx->data, size);
        cb_free(rx->data, size); rx->data = NULL; rx->posted = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "clipboard RX awaiting frontend apply: peer=%s bytes=%u", peer, size);
        chat_event_post(ctx->inst, CHAT_EVT_CLIPBOARD_RECEIVED, event, CHAT_CLIPBOARD_EVENT_HEADER + size);
        cb_free(event, CHAT_CLIPBOARD_EVENT_HEADER + size);
    }
    goto done;
reject:
    cb_reply(ctx, group, ed, packet + 1, 0); goto done;
bad:
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard RX malformed: peer=%s bytes=%zu", peer, n);
done:
    if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
}
static void cb_tick(void* arg) {
    struct chat_clipboard* ctx = arg; uint64_t now = get_time_tb();
    chat_clipboard_revalidate(ctx->inst, 0);
    unsigned pending = ctx->lookup != NULL;
    if (ctx->lookup && now - ctx->started >= CB_LIFETIME) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard device lookup timed out request=%llu", (unsigned long long)ctx->request);
        ctx->lookup->owner = NULL; ctx->lookup = NULL; pending = 0;
    }
    for (struct cb_target* p = ctx->targets; p; p = p->next) {
        if (p->state) continue;
        if (now - p->progress >= CB_IDLE || now - ctx->started >= CB_LIFETIME) cb_fail(p, "timeout");
        else if (!cb_mode_allowed(ctx->inst, p->group, p->peer_ed, ctx->scope)) cb_fail(p, "membership/permission revoked");
        else if (p->offset < ctx->size && !p->ready.conn) cb_fail(p, "router producer cancelled");
        pending += !p->state;
    }
    if (ctx->data && !pending) { cb_report(ctx, ctx->request, CHAT_CLIPBOARD_FINISHED); cb_tx_clear(ctx); }
    for (unsigned i = 0; i < CB_RX_SLOTS; i++) {
        struct cb_receive* rx = &ctx->receive[i];
        if (rx->group && (now - rx->progress >= CB_IDLE || now - rx->started >= CB_LIFETIME || !cb_mode_allowed(ctx->inst, rx->group, rx->peer_ed, rx->scope))) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard RX expired/revoked: peer=%s offset=%u total=%u posted=%u",
                sc_public_key_hex(rx->peer_ed, (char[65]){0}), rx->offset, rx->size, rx->posted); cb_rx_clear(rx);
        }
    }
    ctx->timer = uasync_set_timeout(ctx->inst->ua, 1000, ctx, cb_tick, "clipboard");
    if (!ctx->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard watchdog allocation failed; cancelling active exchanges");
        for (struct cb_target* p = ctx->targets; p; p = p->next) cb_fail(p, "watchdog allocation");
        if (ctx->data) { cb_report(ctx, ctx->request, CHAT_CLIPBOARD_FINISHED); cb_tx_clear(ctx); }
        for (unsigned i = 0; i < CB_RX_SLOTS; i++) cb_rx_clear(&ctx->receive[i]);
    }
}

static void cb_send_start(struct chat_clipboard* ctx) {
    ctx->started = get_time_tb();
    for (struct cb_target* p = ctx->targets; p; p = p->next) {
        if (p->state) continue;
        p->progress = ctx->started;
        struct ETCP_ROUTER_CONN* conn = etcp_router_conn_get_key(ctx->inst, p->group, p->peer_ed, ETCP_RT_ID_CLIPBOARD);
        if (!conn || etcp_router_request_send(conn, &p->ready, cb_produce, p)) cb_fail(p, "router registration");
    }
    cb_report(ctx, ctx->request, CHAT_CLIPBOARD_STARTED);
}
static int cb_devices_guard(void* data) {
    struct cb_device_job* job = data; struct chat_clipboard* ctx = job->owner; struct devgroup_record current;
    return ctx && ctx->lookup == job && ctx->data && !ctx->inst->destroying && chat_core_generation(ctx->inst) &&
        devgroup_member_by_key(ctx->inst, job->self, &current) == 1 && !memcmp(current.ed, job->ed, 32) && !memcmp(current.x, job->x, 32);
}
static int cb_devices_work(sqlite3* db, void* data) {
    struct cb_device_job* job = data; return devgroup_nodes_read(db, job->self, &job->nodes, &job->count);
}
static void cb_devices_done(void* data, const struct db_async_result* result) {
    struct cb_device_job* job = data; struct chat_clipboard* ctx = job->owner;
    int success = !result->status && cb_devices_guard(job);
    if (job->acceptance) *job->acceptance = success ? 0 : -1;
    if (ctx) ctx->lookup = NULL;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard device READ failed status=%d rc=%d: %s",
        result->status, result->sqlite_code, result->message);
    for (size_t i = 0; success && i < job->count; i++) {
        const uint8_t* peer = job->nodes[i];
        if (!memcmp(peer, job->self, 32)) continue;
        struct cb_target* target = u_calloc(1, sizeof(*target));
        if (!target) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard target allocation failed peer=%s", sc_public_key_hex(peer, (char[65]){0}));
            success = 0; break;
        }
        target->owner = ctx; memcpy(target->peer_ed, peer, 32); target->next = ctx->targets; ctx->targets = target;
        if (!dm_route_group_key(ctx->inst, peer, 0, &target->group, ETCP_RT_ID_CLIPBOARD)) {
            target->state = 2;
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard target unavailable peer=%s: no live common CHAT route", sc_public_key_hex(peer, (char[65]){0}));
        }
    }
    u_free(job->nodes); u_free(job);
    if (!ctx) return;
    if (success) cb_send_start(ctx);
    else {
        uint64_t request = ctx->request; cb_tx_clear(ctx);
        if (!ctx->inst->destroying && chat_core_generation(ctx->inst)) cb_report(ctx, request, CHAT_CLIPBOARD_REJECTED);
    }
}
int chat_clipboard_send(struct UTUN_INSTANCE* inst, uint64_t request, uint64_t group, const uint8_t peer[32], int type, const uint8_t* data, size_t size) {
    struct chat_clipboard* ctx = cb_ctx(inst); struct devgroup_record own;
    const char* reason = !ctx || inst->destroying || !chat_core_generation(inst) ? "service unavailable" :
        !ctx->timer ? "watchdog unavailable" : !request || !data || !cb_size(type, size)
        ? "invalid arguments or payload limit" : ctx->data ? "another exchange is active" : NULL;
    if (!reason && (group || peer) && (!group || !peer || !cb_mode_allowed(inst, group, peer, CHAT_CLIPBOARD_MEMBER)))
        reason = "member clipboard forbidden";
    if (!reason && !peer && devgroup_member_by_key(inst, inst->my_ed25519_pubkey, &own) != 1) reason = "local devgroup unavailable";
    if (reason) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard send rejected: request=%llu type=%d bytes=%zu reason=%s",
            (unsigned long long)request, type, size, reason);
        struct chat_clipboard fallback = {.inst = inst};
        if (inst && request) cb_report(ctx ? ctx : &fallback, request, CHAT_CLIPBOARD_REJECTED);
        return -1;
    }
    ctx->data = u_malloc(size); ctx->size = (uint32_t)size; ctx->type = (uint8_t)type; ctx->request = request;
    ctx->scope = peer ? CHAT_CLIPBOARD_MEMBER : CHAT_CLIPBOARD_OWN;
    if (!ctx->data || RAND_bytes(ctx->id, 16) != 1) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard send allocation/random failed"); goto fail; }
    memcpy(ctx->data, data, size);
    if (peer) {
        struct cb_target* target = u_calloc(1, sizeof(*target));
        if (!target) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard target allocation failed"); goto fail; }
        target->owner = ctx; target->group = group; memcpy(target->peer_ed, peer, 32); ctx->targets = target;
        uint64_t route = 0;
        if (!dm_route_group_key(inst, peer, group, &route, ETCP_RT_ID_CLIPBOARD) || route != group) target->state = 2;
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard member send group=%llu peer=%s available=%d",
            (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), !target->state);
        cb_send_start(ctx); return 0;
    }
    struct cb_device_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard device READ allocation failed"); goto fail; }
    memcpy(job->self, inst->my_ed25519_pubkey, 32); job->owner = ctx; memcpy(job->ed, own.ed, 32); memcpy(job->x, own.x, 32);
    ctx->lookup = job; ctx->started = get_time_tb();
    if (inst->db_worker) {
        int accepted = 0; job->acceptance = &accepted;
        db_async_submit_guarded(inst->db_worker, "clipboard-devices", DB_ASYNC_READ, cb_devices_guard, cb_devices_work, job, cb_devices_done);
        if (!accepted) job->acceptance = NULL;
        return accepted;
    }
    sqlite3* ram = inst->topo_sqlite_db; const char* file = ram ? sqlite3_db_filename(ram, "main") : NULL;
    struct db_async_result result = {.status=DB_ASYNC_CLOSED,.sqlite_code=SQLITE_MISUSE};
    if (ram && (!file || !*file)) {
        result.sqlite_code = cb_devices_work(ram, job); result.status = result.sqlite_code ? DB_ASYNC_SQL : DB_ASYNC_OK;
    }
    snprintf(result.message, sizeof(result.message), "clipboard device READ: %s", sqlite3_errstr(result.sqlite_code));
    int accepted = result.status == DB_ASYNC_OK; cb_devices_done(job, &result); return accepted ? 0 : -1;
fail:
    cb_tx_clear(ctx); cb_report(ctx, request, CHAT_CLIPBOARD_REJECTED); return -1;
}
int chat_clipboard_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst) || CC(inst)->clipboard) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard invalid init"); return -1; }
    struct chat_clipboard* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard context allocation failed"); return -1; }
    ctx->inst = inst; CC(inst)->clipboard = ctx;
    if (etcp_router_bind(inst, ETCP_RT_ID_CLIPBOARD, cb_receive) ||
        etcp_router_require_crypto(inst, ETCP_RT_ID_CLIPBOARD, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_CLIPBOARD, cb_key_allowed)) goto fail;
    ctx->files = file_transfer_create_service(inst, ETCP_RT_ID_CLIPBOARD_FILES, cb_file_lookup, cb_key_allowed, ctx);
    if (!ctx->files) goto fail;
    ctx->timer = uasync_set_timeout(inst->ua, 1000, ctx, cb_tick, "clipboard");
    if (!ctx->timer) goto fail;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard service started: E2E router, own devices and permitted group members"); return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard service initialization failed"); chat_clipboard_destroy(inst); return -1;
}
void chat_clipboard_destroy(struct UTUN_INSTANCE* inst) {
    struct chat_clipboard* ctx = cb_ctx(inst); if (!ctx) return;
    if (ctx->timer) uasync_cancel_timeout(inst->ua, ctx->timer);
    file_transfer_destroy(ctx->files); ctx->files = NULL;
    u_free(ctx->sources); ctx->sources = NULL;
    if (ctx->data) {
        for (struct cb_target* p = ctx->targets; p; p = p->next) cb_fail(p, "service stopped");
        cb_report(ctx, ctx->request, CHAT_CLIPBOARD_FINISHED);
    }
    cb_tx_clear(ctx);
    for (unsigned i = 0; i < CB_RX_SLOTS; i++) cb_rx_clear(&ctx->receive[i]);
    etcp_router_unbind(inst, ETCP_RT_ID_CLIPBOARD); CC(inst)->clipboard = NULL;
    for (;;) {
        struct ETCP_ROUTER_CONN* found = NULL;
        for (struct ll_entry* e = inst->router_conns->head; e; e = e->next) {
            struct ETCP_ROUTER_CONN* conn = (struct ETCP_ROUTER_CONN*)e;
            if (conn->svc_id == ETCP_RT_ID_CLIPBOARD) { found = conn; break; }
        }
        if (!found) break;
        etcp_router_conn_close(found);
    }
    OPENSSL_cleanse(ctx, sizeof(*ctx)); u_free(ctx); DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard service stopped");
}
void chat_clipboard_trampoline(void* arg) {
    struct chat_clipboard_request* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard missing command"); return; }
    const uint8_t* peer = memcmp(req->peer_ed, (uint8_t[32]){0}, 32) ? req->peer_ed : NULL;
    int has_group = memcmp(req->group, (uint8_t[32]){0}, 32) != 0;
    struct TOPO_GROUP* route = has_group && req->inst ? topo_groups_find_wire(req->inst->topo_groups, req->group) : NULL;
    uint64_t group = route ? route->group_id : 0;
    if (has_group && !route) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard group unavailable group=%s action=%d", sc_public_key_hex(req->group, (char[65]){0}), req->action);
        struct chat_clipboard fallback = {.inst = req->inst};
        if (req->inst && req->action != 1) cb_report(&fallback, req->request, CHAT_CLIPBOARD_REJECTED);
        goto cleanup;
    }
    if (!req->action) chat_clipboard_send(req->inst, req->request, group, peer, req->type, (const uint8_t*)(req + 1), req->size);
    else if (req->action == 1) chat_clipboard_applied(req->inst, group, req->peer_ed, req->id, req->success);
    else if (req->action == 2) {
        struct chat_clipboard* ctx = cb_ctx(req->inst); struct devgroup_record own;
        struct chat_clipboard_source* sources = (struct chat_clipboard_source*)(req + 1);
        size_t bytes = sizeof(*sources) * req->source_count;
        const char* reason = !ctx ? "service unavailable" : !ctx->timer ? "watchdog unavailable" : ctx->data ? "exchange active" :
            req->source_count > CHAT_CLIPBOARD_SOURCES_MAX ? "source count exceeds limit" : NULL;
        if (!reason && (group || peer) && (!group || !peer ||
            !cb_mode_allowed(req->inst, group, peer, CHAT_CLIPBOARD_MEMBER))) reason = "member clipboard forbidden";
        if (!reason && !peer && devgroup_member_by_key(req->inst, req->inst->my_ed25519_pubkey, &own) != 1) reason = "local devgroup unavailable";
        for (unsigned i = 0; !reason && i < req->source_count; i++)
            if (!memchr(sources[i].path, 0, sizeof(sources[i].path))) reason = "unterminated source path";
        int valid = !reason;
        if (valid) chat_clipboard_revalidate(req->inst, 0);
        unsigned previous = valid ? ctx->source_count : 0;
        if (previous + req->source_count > CHAT_CLIPBOARD_SOURCES_MAX) { valid = 0; reason = "published source budget exhausted"; }
        size_t total = sizeof(struct cb_source) * (previous + req->source_count);
        struct cb_source* copy = valid && total ? u_calloc(1, total) : NULL;
        if (valid && (!total || copy)) {
            if (previous) memcpy(copy, ctx->sources, sizeof(*copy) * previous);
            for (unsigned i = 0; i < req->source_count; i++) {
                copy[previous + i].file = sources[i]; copy[previous + i].group = group;
                if (peer) memcpy(copy[previous + i].peer_ed, peer, 32);
                if (!peer) { memcpy(copy[previous + i].devices, own.ed, 32); memcpy(copy[previous + i].devices + 32, own.x, 32); }
            }
            u_free(ctx->sources); ctx->sources = copy; ctx->source_count = previous + req->source_count;
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard file offer published: files=%u manifest_bytes=%u", ctx->source_count, req->size);
            if (chat_clipboard_send(req->inst, req->request, group, peer, CHAT_CLIPBOARD_FILES, (const uint8_t*)sources + bytes, req->size)) {
                ctx->source_count = previous;
                if (!previous) { u_free(ctx->sources); ctx->sources = NULL; }
                DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard unpublished file sources rolled back count=%u", req->source_count);
            }
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard file offer rejected: new=%u previous=%u reason=%s",
                req->source_count, previous, reason ? reason : "source allocation failed");
            struct chat_clipboard fallback = {.inst = req->inst};
            if (req->inst) cb_report(ctx ? ctx : &fallback, req->request, CHAT_CLIPBOARD_REJECTED);
        }
    }
    else DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard unknown command=%d", req->action);
cleanup:
    cb_free((uint8_t*)req, sizeof(*req) + req->size + (req->action == 2 ? sizeof(struct chat_clipboard_source) * req->source_count : 0));
}
static void cb_pull_done(void* arg, int error) {
    struct chat_clipboard_pull_request* req = arg;
    if (!error && !cb_key_allowed(req->inst, req->route_index, req->peer_ed)) error = -2;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard file pull complete: peer=%s bytes=%llu error=%d",
        sc_public_key_hex(req->peer_ed, (char[65]){0}), (unsigned long long)req->size, error);
    req->done(req->arg, error); u_free(req);
}
void chat_clipboard_pull_trampoline(void* arg) {
    struct chat_clipboard_pull_request* req = arg; struct chat_clipboard* ctx = req ? cb_ctx(req->inst) : NULL;
    if (!req || !req->done) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard invalid file pull command"); u_free(req); return; }
    struct TOPO_GROUP* route = ctx ? topo_groups_find_wire(req->inst->topo_groups, req->group) : NULL;
    uint64_t group = route ? route->group_id : 0;
    if (!ctx || !memchr(req->destination, 0, sizeof(req->destination)) ||
        !dm_route_group_key(req->inst, req->peer_ed, group, &group, ETCP_RT_ID_CLIPBOARD_FILES) ||
        !cb_key_allowed(req->inst, group, req->peer_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard file pull unavailable/revoked"); cb_pull_done(req, -1); return;
    }
    req->route_index = group;
    if (!file_transfer_pull(ctx->files, group, req->peer_ed, req->object, req->size, req->destination, cb_pull_done, req)) cb_pull_done(req, -1);
}
