/*
 * invite_build.c — сборка invite-ссылок utun:// (выбор узла + адреса)
 *
 * Общий модуль для всех GUI (desktop chatgui, Android, headless CLI).
 * Логика выбора «лучшего узла» перенесена из tools/chatgui-android/jni_bridge.
 * Работает в uasync-потоке, использует per-instance контекст.
 */
#include "invite_build.h"
#include "invite_link.h"
#include "../member_files/protocol.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "chat_join.h"
#include "group_policy.h"

#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../utun_instance.h"
#include "../../lib/mem.h"
#include "../../lib/platform_compat.h"
#include "../../lib/db_async.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

/* ── живость узла в канале (BGP/прямое/через посредника) ── */

static struct TOPO_GROUP* resolve_chat_group(struct UTUN_INSTANCE* inst, uint64_t channel_id) {
    if (!inst || !inst->topo_groups) return NULL;
    struct TOPO_GROUP* grp = topo_groups_find(inst->topo_groups, channel_id);
    if (!grp || grp->group_type != TOPO_GROUP_TYPE_CHAT) return NULL;
    return grp;
}

static int invite_ed_set(const uint8_t ed[32]) { return ed && !mf_zero(ed, 32); }

static int node_is_alive(struct UTUN_INSTANCE* inst, struct TOPO_GROUP* grp, const uint8_t ed[32]) {
    if (!memcmp(ed, inst->my_ed25519_pubkey, 32)) return 1; /* self всегда доступен */
    if (!grp) return 0;
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_ed(grp, ed);
    if (!nq) return 0;
    return topo_node_best_path(nq) != NULL;
}

/* есть ли у узла публичные адреса v4 и v6 (addr_type=DIRECT) */
static int node_has_both_public(sqlite3* db, const uint8_t ed[32]) {
    int has_v4 = 0, has_v6 = 0;
    sqlite3_stmt* st = NULL;
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT family FROM node_addresses WHERE node_x=(SELECT x25519_pubkey FROM nodes WHERE ed25519_pubkey=?) AND addr_type=%d AND port>0", ADDR_TYPE_DIRECT);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int family_type = sqlite3_column_type(st, 0);
        int fam = sqlite3_column_int(st, 0);
        if (family_type != SQLITE_INTEGER || (fam != 4 && fam != 6)) { rc = SQLITE_CORRUPT; break; }
        if (fam == 4) has_v4 = 1;
        else if (fam == 6) has_v6 = 1;
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: public families node=%s rc=%d finalize=%d: %s",
            sc_public_key_hex(ed, (char[65]){0}), rc, final, sqlite3_errmsg(db)); return -(rc != SQLITE_DONE ? rc : final);
    }
    return (has_v4 && has_v6) ? 1 : 0;
}

/* Один SQL-снимок всех eligible мемберов; BGP/topology используются только после READ в uasync. */
struct invite_node { uint8_t ed[32]; int type; };
struct invite_nodes { struct invite_node* rows; size_t count; };
struct invite_selection {
    uint8_t (*ids)[32];
    int capacity, count;
    const uint8_t* wanted;
    uint8_t best[32];
    int priority, found;
};

static int invite_nodes_read(sqlite3* db, const char* channel, struct invite_nodes* out) {
    char sql[256]; sqlite3_stmt* st = NULL; size_t capacity = 0; struct invite_nodes result = {0};
    snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey,node_type FROM peers_%s"
        " WHERE record_state=0 AND source=0 AND deleted=0 AND admitted=1 ORDER BY ed25519_pubkey", channel);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32 || sqlite3_column_type(st, 1) != SQLITE_INTEGER || sqlite3_column_int(st, 1) < 0 || sqlite3_column_int(st, 1) > 4) { rc = SQLITE_CORRUPT; break; }
        if (result.count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            if (next > UINT32_MAX / sizeof(*result.rows)) { rc = SQLITE_TOOBIG; break; }
            struct invite_node* grown = u_realloc(result.rows, next * sizeof(*result.rows));
            if (!grown) { rc = SQLITE_NOMEM; break; } result.rows = grown; capacity = next;
        }
        const void* ed = sqlite3_column_blob(st, 0);
        if (!ed) { rc = SQLITE_NOMEM; break; }
        struct invite_node* row = &result.rows[result.count++];
        memcpy(row->ed, ed, 32); row->type = sqlite3_column_int(st, 1);
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite candidates READ ch=%s rows=%zu rc=%d final=%d: %s",
            channel, result.count, rc, final, sqlite3_errstr(rc)); u_free(result.rows); return rc;
    }
    *out = result; return SQLITE_OK;
}

static void select_live(struct UTUN_INSTANCE* inst, struct TOPO_GROUP* group, const struct invite_nodes* nodes,
    struct invite_selection* result) {
    result->priority = 4;
    for (size_t i = 0; i < nodes->count; i++) {
        const struct invite_node* node = &nodes->rows[i];
        if (!node_is_alive(inst, group, node->ed)) continue;
        if (result->count < result->capacity) memcpy(result->ids[result->count++], node->ed, 32);
        if (result->wanted && !memcmp(node->ed, result->wanted, 32)) result->found = 1;
        int priority = node->type == 4 ? 1 : !memcmp(node->ed, inst->my_ed25519_pubkey, 32) ? 3 : 2;
        if (priority < result->priority) { memcpy(result->best, node->ed, 32); result->priority = priority; }
    }
}

static int select_nodes(struct UTUN_INSTANCE* inst, uint64_t channel, struct invite_selection* result) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    struct TOPO_GROUP* group = resolve_chat_group(inst, channel);
    if (!cc || !cc->initialized || !cc->db || cc->inst != inst || !channel || channel > INT64_MAX ||
        !group || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite candidates: invalid context/channel or RW requires async API ch=%llu",
            (unsigned long long)channel); return -1;
    }
    struct invite_nodes nodes = {0}; int rc = invite_nodes_read(cc->db, group->channel_id, &nodes); if (rc) return -1;
    select_live(inst, group, &nodes, result); u_free(nodes.rows);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "invite candidates ch=%llu returned=%d best=%s priority=%d wanted_found=%d",
        (unsigned long long)channel, result->count, sc_public_key_hex(result->best, (char[65]){0}), result->priority, result->found);
    return 0;
}

/* Адреса self/пира из общей проекции NODEINFO;port=0 и unspecified IP остаются только hints. */

static int collect_node_addrs_from_db(sqlite3* db, const uint8_t ed[32], struct InviteAddr* out, int max_cnt, int dir_only,
                                      struct InviteReality* reality) {
    char sql[384];
    snprintf(sql, sizeof(sql), "SELECT family,socket_id,protocol,address,port,options FROM node_addresses"
        " WHERE node_x=(SELECT x25519_pubkey FROM nodes WHERE ed25519_pubkey=?) AND (%d=0 OR addr_type=%d) ORDER BY family,protocol,socket_id,address_kind,address,port",
        dir_only, ADDR_TYPE_DIRECT);
    sqlite3_stmt* st = NULL; int count = 0; struct InviteReality parameters = {0};
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int family_type = sqlite3_column_type(st, 0), socket_type = sqlite3_column_type(st, 1), proto_type = sqlite3_column_type(st, 2);
        int port_type = sqlite3_column_type(st, 4), address_type = sqlite3_column_type(st, 3);
        int family = sqlite3_column_int(st, 0), socket = sqlite3_column_int(st, 1), proto = sqlite3_column_int(st, 2);
        int port = sqlite3_column_int(st, 4), bytes = sqlite3_column_bytes(st, 3);
        const uint8_t* address = sqlite3_column_blob(st, 3);
        if (family_type != SQLITE_INTEGER || socket_type != SQLITE_INTEGER || proto_type != SQLITE_INTEGER ||
            port_type != SQLITE_INTEGER || address_type != SQLITE_BLOB || (family != 4 && family != 6) || socket < 0 || socket > 255 ||
            (proto != INVITE_PROTO_UDP && proto != INVITE_PROTO_TCP && proto != (INVITE_PROTO_TCP | INVITE_PROTO_REALITY)) ||
            !address || bytes != (family == 4 ? 4 : 16) || port < 0 || port > 65535) { rc = SQLITE_CORRUPT; break; }
        int nonzero = 0; for (int i = 0; i < bytes; i++) nonzero |= address[i];
        if (!port || !nonzero) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: skip address hint node=%s family=%d socket=%d port=%d",
                sc_public_key_hex(ed, (char[65]){0}), family, socket, port); continue;
        }
        if (proto & INVITE_PROTO_REALITY) {
            struct TOPO_ADDR_REALITY_OPTS options;
            int options_type = sqlite3_column_type(st, 5);
            const void* blob = sqlite3_column_blob(st, 5);
            if (options_type != SQLITE_BLOB || !blob || sqlite3_column_bytes(st, 5) != sizeof(options)) {
                rc = SQLITE_CORRUPT; break;
            }
            memcpy(&options, blob, sizeof(options));
            size_t name_length = strnlen(options.server_name, sizeof(options.server_name));
            if (!name_length || name_length == sizeof(options.server_name) || name_length >= INVITE_SNAME_MAX) { rc = SQLITE_CORRUPT; break; }
            struct InviteReality current = {.has = 1};
            memcpy(current.short_id, options.short_id, 8); memcpy(current.server_pubkey, options.server_pubkey, 32);
            memcpy(current.version, options.version, 3); strcpy(current.server_name, options.server_name);
            if (parameters.has && memcmp(&parameters, &current, sizeof(current))) { rc = SQLITE_CORRUPT; break; }
            parameters = current;
        }
        if (count < max_cnt) {
            out[count] = (struct InviteAddr){.family = family, .socketId = (uint8_t)socket, .proto = (uint8_t)proto, .port = (uint16_t)port};
            memcpy(out[count++].address, address, (size_t)bytes);
        }
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: addresses node=%s count=%d rc=%d finalize=%d: %s",
            sc_public_key_hex(ed, (char[65]){0}), count, rc, final, sqlite3_errmsg(db)); return -(rc != SQLITE_DONE ? rc : final);
    }
    if (reality) *reality = parameters;
    return count;
}

/* ── публичный API ── */

int chat_server_link_build(struct UTUN_INSTANCE* inst, char* link, size_t capacity) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !inst->config->global.chatserver_enabled || !inst->config->global.chatserver_has_add_key ||
        db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "server link requires active chatserver and configured add_key"); return -1;
    }
    struct InviteData data = {.kind = INVITE_KIND_SERVER};
    memcpy(data.pubkey, inst->my_keys.public_key, sizeof(data.pubkey));
    memcpy(data.add_key, inst->config->global.chatserver_add_key, sizeof(data.add_key));
    int count = collect_node_addrs_from_db(cc->db, inst->my_ed25519_pubkey, data.addrs, INVITE_ADDR_MAX, 0, &data.reality);
    if (count <= 0) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "server link has no published endpoints count=%d", count); return -1; }
    data.addrCount = (uint8_t)count;
    int result = invite_link_encode(&data, link, capacity);
    OPENSSL_cleanse(data.add_key, sizeof(data.add_key));
    if (result < 0) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "server link built node=%016llx endpoints=%d", (unsigned long long)inst->node_id, count);
    return 0;
}

static int prepare_link(struct UTUN_INSTANCE* inst, uint64_t channel_id, const uint8_t wanted[32],
                        char* out, size_t out_size, uint8_t target[32], uint64_t* key) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!out || out_size == 0) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: missing link output"); return -1; }
    out[0] = '\0';
    if (!cc || !cc->initialized || !inst || !cc->db || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: not initialized (inst=%p db=%p)",
                    (void*)inst, (void*)(cc ? cc->db : NULL));
        return -1;
    }
    if (!channel_id || channel_id > INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: invalid channel_id");
        return -1;
    }

    struct invite_selection selection = {.wanted = wanted};
    int automatic = !invite_ed_set(wanted);
    if (select_nodes(inst, channel_id, &selection) || (automatic ? !invite_ed_set(selection.best) : !selection.found)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: no eligible target ch=%llu wanted=%s",
            (unsigned long long)channel_id, sc_public_key_hex(wanted, (char[65]){0})); return -1;
    }
    const uint8_t* ed = automatic ? selection.best : wanted;
    int has_both_public = node_has_both_public(cc->db, ed);
    if (has_both_public < 0) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: selected ch=%llu node=%s auto=%d both=%d",
        (unsigned long long)channel_id, sc_public_key_hex(ed, (char[65]){0}), automatic, has_both_public);

    /* ── pubkey ── */
    struct InviteData data;
    memset(&data, 0, sizeof(data));
    memcpy(data.inviter_pubkey, inst->my_ed25519_pubkey, 32);
    /* Ключ сохраняет пригласивший после успешной сборки ссылки. */
    if (RAND_bytes((uint8_t*)&data.join_key, sizeof(data.join_key)) != 1 || data.join_key == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: join_key generation failed");
        return -1;
    }
    if (sc_identity_public_x("invite target", ed, data.pubkey) != SC_OK) return -1;

    /* Общая проекция NODEINFO уже исключает private/probe сокеты и содержит NAT/REALITY. */
    int count = collect_node_addrs_from_db(cc->db, ed, data.addrs, INVITE_ADDR_MAX, has_both_public, &data.reality);
    if (count < 0) return -1;
    data.addrCount = (uint8_t)count;
    if (data.addrCount == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: no addrs for node=%s",
                    sc_public_key_hex(ed, (char[65]){0}));
        return -1;
    }

    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: node=%s addrs=%d dir_only=%d",
               sc_public_key_hex(ed, (char[65]){0}), data.addrCount, has_both_public);

    /* ── encode ── */
    if (invite_link_encode(&data, out, out_size) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: encode failed ch=%llu",
                    (unsigned long long)channel_id);
        return -1;
    }

    memcpy(target, ed, 32);
    *key = data.join_key;

    return 0;
}

int chat_invite_best_node(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint8_t out[32]) {
    if (!out) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: missing best-node output"); return -1; }
    struct invite_selection selection = {0};
    if (select_nodes(inst, channel_id, &selection)) return -1;
    if (!invite_ed_set(selection.best)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: no reachable full member"); return -1; }
    memcpy(out, selection.best, 32); return 0;
}

enum invite_kind { INVITE_BUILD_LINK, INVITE_BUILD_CANDIDATES, INVITE_BUILD_SERVER };
struct chat_invite_build {
    char channel_key[65];
    struct chat_invite_build* next;
    struct chat_core_ctx* owner; /* Слабый owner; stop отсоединяет до освобождения cc. */
    struct UTUN_INSTANCE* inst; /* Живёт до DB drain. */
    void* timer;
    chat_invite_ready_fn callback;
    chat_invite_candidates_fn candidates_callback;
    void* arg;
    enum invite_kind kind;
    int completed, sql_active, addresses;
    uint64_t channel;
    uint64_t window_request; /* Ненулевой для ссылки, которой владеет окно. */
    uint8_t self_key[32], wanted[32], selected[32], best[32];
    struct invite_nodes nodes; /* Собственный SQL-результат первого READ. */
    uint8_t (*ids)[32]; unsigned capacity; size_t count;
    struct InviteData data; /* Frozen public keys/add_key и собственные endpoints второго READ. */
    char link[INVITE_LINK_MAX];
};

static void invite_unlink(struct chat_invite_build* req) {
    if (!req->owner) return;
    struct chat_invite_build** p = &req->owner->invite_builds;
    while (*p && *p != req) p = &(*p)->next;
    if (*p) *p = req->next;
    else DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite cleanup lost request channel=%llu", (unsigned long long)req->channel);
    req->owner = NULL; req->next = NULL;
}
static void invite_free(struct chat_invite_build* req) {
    invite_unlink(req); u_free(req->nodes.rows); u_free(req->ids);
    OPENSSL_cleanse(req->link, sizeof(req->link));
    OPENSSL_cleanse(&req->data, sizeof(req->data)); u_free(req);
}
static int invite_current(void* arg) {
    struct chat_invite_build* req = arg;
    if (req->completed || !req->owner || CC(req->inst) != req->owner || !req->owner->initialized ||
        req->inst->destroying || req->inst->join_stopping) return 0;
    if (req->kind == INVITE_BUILD_SERVER) return 1;
    struct TOPO_GROUP* group = resolve_chat_group(req->inst, req->channel);
    if (!group || group->stopping || req->inst->join_stopping) return 0;
    if (req->kind == INVITE_BUILD_LINK) {
        const char* channel = group->channel_id;
        if (group_policy_can_invite(req->inst, channel, req->inst->my_ed25519_pubkey) != 1) return 0;
        if (invite_ed_set(req->selected) && (!topo_group_chat_key_allowed(req->inst, req->channel, req->selected) ||
            !node_is_alive(req->inst, group, req->selected))) return 0;
    }
    return 1;
}

/* Callback ровно один раз; pending SQL держит собственный req до done, без доступа к старому owner. */
static void invite_finish(struct chat_invite_build* req, int result) {
    if (req->completed) return;
    req->completed = 1;
    if (req->timer) { uasync_cancel_timeout(req->inst->ua, req->timer); req->timer = NULL; }
    invite_unlink(req);
    chat_invite_ready_fn callback = req->callback; chat_invite_candidates_fn candidates = req->candidates_callback;
    req->callback = NULL; req->candidates_callback = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "invite complete kind=%d channel=%llu selected=%s result=%d sql_pending=%d",
        req->kind, (unsigned long long)req->channel, sc_public_key_hex(req->selected, (char[65]){0}), result, req->sql_active);
    if (candidates) candidates(req->arg, result, result == CHAT_JOIN_OK ? req->best : NULL,
        result == CHAT_JOIN_OK ? req->ids : NULL, result == CHAT_JOIN_OK ? req->count : 0);
    else if (callback) callback(req->arg, result, result == CHAT_JOIN_OK ? req->link : NULL);
    if (!req->sql_active) invite_free(req);
}
static void invite_publish(struct chat_invite_build* req) {
    if (!invite_current(req)) { invite_finish(req, CHAT_JOIN_CANCELLED); return; }
    int rc = req->window_request ? chat_join_store_window_key(req->inst, req->channel, req->data.join_key, req->window_request)
        : chat_join_store_local_key(req->inst, req->channel, req->data.join_key);
    invite_finish(req, rc ? CHAT_JOIN_ERROR : CHAT_JOIN_OK);
}
static int invite_read_work(sqlite3* db, void* arg) {
    struct chat_invite_build* req = arg;
    if (!req->addresses) return invite_nodes_read(db, req->channel_key, &req->nodes);
    int direct = req->kind == INVITE_BUILD_SERVER ? 0 : node_has_both_public(db, req->selected);
    if (direct < 0) return -direct;
    if (sc_identity_public_x("invite target", req->selected, req->data.pubkey) != SC_OK) return SQLITE_AUTH;
    int count = collect_node_addrs_from_db(db, req->selected, req->data.addrs, INVITE_ADDR_MAX, direct, &req->data.reality);
    if (count < 0) return -count;
    if (!count) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "invite endpoints absent node=%s", sc_public_key_hex(req->selected, (char[65]){0})); return SQLITE_NOTFOUND; }
    req->data.addrCount = (uint8_t)count; return SQLITE_OK;
}
static void invite_read_done(void* arg, const struct db_async_result* result);
static void invite_dispatch(struct chat_invite_build* req) {
    req->sql_active = 1;
    if (req->inst->db_worker) {
        db_async_submit_guarded(req->inst->db_worker, "invite-read", DB_ASYNC_READ, invite_current, invite_read_work, req, invite_read_done);
        return;
    }
    sqlite3* db = req->owner ? req->owner->db : NULL; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int rc = !invite_current(req) ? SQLITE_ABORT : !file || *file ? SQLITE_MISUSE : invite_read_work(db, req);
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite RAM read channel=%llu rc=%d", (unsigned long long)req->channel, rc);
    invite_read_done(req, &result);
}
static void invite_read_done(void* arg, const struct db_async_result* result) {
    struct chat_invite_build* req = arg; req->sql_active = 0;
    if (req->completed) { invite_free(req); return; }
    if (result->status || !invite_current(req)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "invite READ failed/cancelled channel=%llu stage=%s status=%d sqlite=%d",
            (unsigned long long)req->channel, req->addresses ? "endpoints" : "members", result->status, result->sqlite_code);
        invite_finish(req, invite_current(req) ? CHAT_JOIN_ERROR : CHAT_JOIN_CANCELLED); return;
    }
    if (!req->addresses) {
        struct invite_selection selection = {.ids=req->ids, .capacity=(int)req->capacity, .wanted=req->wanted};
        select_live(req->inst, resolve_chat_group(req->inst, req->channel), &req->nodes, &selection);
        u_free(req->nodes.rows); req->nodes = (struct invite_nodes){0};
        memcpy(req->best, selection.best, 32); req->count = (size_t)selection.count;
        if (req->kind == INVITE_BUILD_CANDIDATES) { invite_finish(req, CHAT_JOIN_OK); return; }
        int automatic = !invite_ed_set(req->wanted);
        memcpy(req->selected, automatic ? selection.best : req->wanted, 32);
        if (!invite_ed_set(req->selected) || (!automatic && !selection.found)) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "invite no reachable target channel=%llu wanted=%s",
                (unsigned long long)req->channel, sc_public_key_hex(req->wanted, (char[65]){0})); invite_finish(req, CHAT_JOIN_ERROR); return;
        }
        memcpy(req->data.inviter_pubkey, req->self_key, sizeof(req->self_key));
        if (RAND_bytes((uint8_t*)&req->data.join_key, sizeof(req->data.join_key)) != 1 || !req->data.join_key) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite join-key generation failed"); invite_finish(req, CHAT_JOIN_ERROR); return;
        }
        req->addresses = 1; invite_dispatch(req); return;
    }
    if (invite_link_encode(&req->data, req->link, sizeof(req->link)) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite encoding failed kind=%d channel=%llu", req->kind,
            (unsigned long long)req->channel); invite_finish(req, CHAT_JOIN_ERROR); return;
    }
    if (req->kind == INVITE_BUILD_SERVER) { invite_finish(req, CHAT_JOIN_OK); return; }
    invite_publish(req);
}
static void invite_kick(void* arg) {
    struct chat_invite_build* req = arg; req->timer = NULL;
    if (!invite_current(req)) { invite_finish(req, CHAT_JOIN_CANCELLED); return; }
    if (req->kind == INVITE_BUILD_LINK && req->link[0]) invite_publish(req);
    else invite_dispatch(req);
}
static struct chat_invite_build* invite_alloc(struct UTUN_INSTANCE* inst, uint64_t channel, enum invite_kind kind,
    chat_invite_ready_fn callback, chat_invite_candidates_fn candidates, void* arg) {
    struct chat_core_ctx* owner = inst ? CC(inst) : NULL;
    if (!owner || !owner->initialized || !inst->ua || inst->destroying || inst->join_stopping || owner->inst != inst ||
        (!callback && !candidates) || (kind != INVITE_BUILD_SERVER && (!channel || channel > INT64_MAX || !resolve_chat_group(inst, channel)))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite start invalid owner/channel/callback"); return NULL;
    }
    struct chat_invite_build* req = u_calloc(1, sizeof(*req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite request allocation failed"); return NULL; }
    req->owner = owner; req->inst = inst; req->channel = channel; strcpy(req->channel_key, topo_groups_channel(inst->topo_groups, channel)); req->kind = kind;
    req->callback = callback; req->candidates_callback = candidates; req->arg = arg;
    memcpy(req->self_key, inst->my_ed25519_pubkey, sizeof(req->self_key));
    req->next = owner->invite_builds; owner->invite_builds = req;
    return req;
}
static struct chat_invite_build* invite_start_read(struct chat_invite_build* req) {
    req->timer = uasync_set_timeout(req->inst->ua, 0, req, invite_kick, "invite-read");
    if (!req->timer) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite deferred start allocation failed"); invite_free(req); return NULL; }
    return req;
}

struct chat_invite_build* chat_invite_build_link(struct UTUN_INSTANCE* inst, uint64_t channel_id, const uint8_t target[32],
    chat_invite_ready_fn callback, void* arg) {
    struct chat_invite_build* req = invite_alloc(inst, channel_id, INVITE_BUILD_LINK, callback, NULL, arg);
    if (!req) return NULL;
    if (target) memcpy(req->wanted, target, 32);
    if (inst->db_worker) return invite_start_read(req);
    if (prepare_link(inst, channel_id, req->wanted, req->link, sizeof(req->link), req->selected, &req->data.join_key)) {
        invite_free(req); return NULL;
    }
    return invite_start_read(req);
}
struct chat_invite_build* chat_invite_candidates_start(struct UTUN_INSTANCE* inst, uint64_t channel, unsigned max_ids,
    chat_invite_candidates_fn callback, void* arg) {
    if (!max_ids || max_ids > UINT16_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite candidate capacity invalid count=%u", max_ids); return NULL; }
    struct chat_invite_build* req = invite_alloc(inst, channel, INVITE_BUILD_CANDIDATES, NULL, callback, arg);
    if (!req) return NULL;
    req->ids = u_malloc(max_ids * sizeof(*req->ids)); req->capacity = max_ids;
    if (!req->ids) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite candidate output allocation failed"); invite_free(req); return NULL; }
    return invite_start_read(req);
}
struct chat_invite_build* chat_server_link_start(struct UTUN_INSTANCE* inst, chat_invite_ready_fn callback, void* arg) {
    if (!inst || !inst->config || !inst->config->global.chatserver_enabled || !inst->config->global.chatserver_has_add_key) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "server link requires configured chatserver/add_key"); return NULL;
    }
    struct chat_invite_build* req = invite_alloc(inst, 0, INVITE_BUILD_SERVER, callback, NULL, arg);
    if (!req) return NULL;
    req->addresses = 1; memcpy(req->selected, inst->my_ed25519_pubkey, 32); req->data.kind = INVITE_KIND_SERVER;
    memcpy(req->data.add_key, inst->config->global.chatserver_add_key, sizeof(req->data.add_key));
    return invite_start_read(req);
}
void chat_invite_build_cancel(struct chat_invite_build* req) {
    if (!req || req->completed) return;
    invite_finish(req, CHAT_JOIN_CANCELLED);
}
void chat_invite_build_stop(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* owner = inst ? CC(inst) : NULL;
    if (inst) chat_invite_cancel_window(inst, 0);
    while (owner && owner->invite_builds) chat_invite_build_cancel(owner->invite_builds);
}

int chat_invite_candidate_nodes(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint8_t (*out_ids)[32], int max_ids) {
    if (!out_ids || max_ids <= 0 || max_ids > UINT16_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: invalid candidate output capacity=%d", max_ids); return -1;
    }
    uint8_t (*ids)[32] = u_malloc((uint32_t)max_ids * sizeof(*ids));
    if (!ids) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: candidate allocation failed count=%d", max_ids); return -1; }
    struct invite_selection selection = {.ids = ids, .capacity = max_ids};
    int rc = select_nodes(inst, channel_id, &selection);
    if (!rc && selection.count) memcpy(out_ids, ids, (size_t)selection.count * sizeof(*ids));
    u_free(ids); return rc ? -1 : selection.count;
}

/* ── трамплины для gui_bridge ── */

static void gui_candidates_ready(void* arg, int result, const uint8_t best[32], const uint8_t (*ids)[32], size_t n) {
    struct chat_invite_cand_req* req = arg;
    if (result == CHAT_JOIN_OK) {
        size_t length = strlen(req->ch_id); uint8_t event[1 + 64 + 32 + 2 + INVITE_ADDR_MAX * 32];
        uint16_t count = (uint16_t)n;
        event[0] = (uint8_t)length; memcpy(event + 1, req->ch_id, length); memcpy(event + 1 + length, best, 32);
        event[33 + length] = (uint8_t)count; event[34 + length] = (uint8_t)(count >> 8);
        if (count) memcpy(event + 35 + length, ids, (size_t)count * 32);
        chat_event_post(req->inst, CHAT_EVT_INVITE_CANDIDATES, event, (int)(35 + length + (size_t)count * 32));
    } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "invite candidates GUI channel=%s result=%d", req->ch_id, result);
    u_free(req);
}
void chat_invite_candidates_trampoline(void* arg) {
    struct chat_invite_cand_req* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: null candidates request"); return; }
    if (!memchr(req->ch_id, 0, sizeof(req->ch_id)) || !chat_channel_valid(req->ch_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: invalid candidates channel"); u_free(req); return;
    }
    if (!chat_invite_candidates_start(req->inst, topo_groups_channel_slot(req->inst->topo_groups, req->ch_id), INVITE_ADDR_MAX, gui_candidates_ready, req)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite candidates GUI start failed channel=%s", req->ch_id); u_free(req);
    }
}

static void gui_invite_ready(void* arg, int result, const char* link) {
    struct chat_invite_build_req* req = arg;
    if (req->inst->invite_gui_request != req) { u_free(req); return; }
    req->inst->invite_gui_request = NULL;
    size_t ch_len = strlen(req->ch_id);
    size_t link_len = result == CHAT_JOIN_OK && link ? strlen(link) : 0;
    size_t evt_sz = 1 + ch_len + 8 + 2 + link_len;
    uint8_t evt[1 + 64 + 8 + 2 + INVITE_LINK_MAX];
    evt[0] = (uint8_t)ch_len;
    memcpy(evt + 1, req->ch_id, ch_len);
    uint16_t u16 = (uint16_t)link_len;
    memcpy(evt + 1 + ch_len, &req->request_id, 8);
    memcpy(evt + 1 + ch_len + 8, &u16, 2);
    if (link_len) memcpy(evt + 1 + ch_len + 10, link, link_len);
    chat_event_post(req->inst, CHAT_EVT_INVITE_LINK_READY, evt, (int)evt_sz);
    OPENSSL_cleanse(evt, evt_sz);
    u_free(req);
}

/* Ключ и незавершённая сборка принадлежат одному запросу окна. */
void chat_invite_cancel_window(struct UTUN_INSTANCE* inst, uint64_t request) {
    struct chat_invite_build_req* pending = inst->invite_gui_request;
    if (pending && (!request || pending->request_id == request)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "cancel window link build group=%s request=%llu", pending->ch_id,
            (unsigned long long)pending->request_id);
        inst->invite_gui_request = NULL;
        chat_invite_build_cancel(pending->pending);
    }
    chat_join_revoke_window_key(inst, request);
}

void chat_invite_cancel_window_trampoline(void* arg) {
    struct chat_invite_cancel_req* req = arg;
    if (req && req->inst && req->request_id) chat_invite_cancel_window(req->inst, req->request_id);
    else DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invalid GUI invitation revocation request");
    u_free(req);
}

void chat_invite_build_link_trampoline(void* arg) {
    struct chat_invite_build_req* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: null GUI request"); return; }
    if (!req->inst || !req->request_id || !memchr(req->ch_id, 0, sizeof(req->ch_id)) || !chat_channel_valid(req->ch_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "invite_build: invalid GUI request");
        u_free(req); return;
    }
    chat_invite_cancel_window(req->inst, 0);
    req->inst->invite_gui_request = req;
    req->pending = chat_invite_build_link(req->inst, topo_groups_channel_slot(req->inst->topo_groups, req->ch_id), req->target,
                                         gui_invite_ready, req);
    if (!req->pending) gui_invite_ready(req, CHAT_JOIN_ERROR, NULL);
    else req->pending->window_request = req->request_id;
}
