#include "group_policy.h"
#include "chat_log.h"
#include "group_info.h"
#include "../group/group_protocol.h"
#include "../kanban/kanban.h"
#include "../kanban/kanban_sql.h"
#include "../db_schema.h"
#include "member_sync.h"
#include "chat_core.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "chat_sync.h"
#include "chat_feedback.h"
#include "db_sync.h"
#include "merkle_tree.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_group_connect.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/secure_channel.h"
#include "../member_files/protocol.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/json_flat.h"
#include "../../lib/strbuf.h"
#include "../../lib/db_async.h"
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

static void gp_write64(uint8_t* p, uint64_t n) { for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)n; n >>= 8; } }
static uint64_t gp_read64(const uint8_t* p) { uint64_t n = 0; for (int i = 0; i < 8; i++) n = n * 256 + p[i]; return n; }

int group_policy_schema(sqlite3* db) {
    return db_schema_table(db, "group_policy", NULL);
}

void group_policy_encode(const struct group_policy* p, uint8_t out[GROUP_POLICY_WIRE_SIZE]) {
    gp_write64(out, p->version); out[8] = p->all_members; memcpy(out + 9, p->signature, 64);
}

/* Проверка ключом группы привязывает подписанную политику к этой группе. */
static int gp_message(const char* channel, const struct group_policy* p, uint8_t body[40]) {
    if (!chat_channel_valid(channel) || p->version > INT64_MAX || p->all_members > 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy invalid channel/version/mode"); return -1;
    }
    memcpy(body, "utun.group.policy.v2", 20);
    gp_write64(body + 20, p->version); body[28] = p->all_members; return 29;
}

static int gp_key_read(sqlite3* db, const char* channel, int secret, uint8_t key[32]) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, secret ? "SELECT ed25519_privkey FROM channels WHERE channel_id=?" :
        "SELECT ed25519_pubkey FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        int type = sqlite3_column_type(st, 0), bytes = sqlite3_column_bytes(st, 0);
        if (type == SQLITE_NULL) rc = secret ? SQLITE_AUTH : SQLITE_NOTFOUND;
        else if (type != SQLITE_BLOB || bytes != 32) rc = SQLITE_CORRUPT;
        else { const void* blob = sqlite3_column_blob(st, 0); if (!blob) rc = SQLITE_NOMEM; else { memcpy(key, blob, 32); rc = SQLITE_OK; } }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy key read ch=%s secret=%d rc=%d finalize=%d", channel, secret, rc, final);
    return rc;
}

void group_policy_decode(const uint8_t wire[GROUP_POLICY_WIRE_SIZE], struct group_policy* out) {
    out->version = gp_read64(wire); out->all_members = wire[8]; memcpy(out->signature, wire + 9, 64);
}

int group_policy_check(sqlite3* db, const char* channel, const struct group_policy* p, int* valid) {
    if (!db || !chat_channel_valid(channel) || !p || !valid) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy check: missing database/channel/policy/result"); return SQLITE_MISUSE;
    }
    *valid = 0;
    uint8_t pub[32], body[40]; int bytes = gp_message(channel, p, body);
    if (bytes < 0) return SQLITE_OK;
    int rc = gp_key_read(db, channel, 0, pub); if (rc) return rc;
    int signature = !p->version ? (p->all_members || memcmp(p->signature, (uint8_t[64]){0}, 64) ? SC_ERR_AUTH_FAILED : SC_OK)
        : sc_ed25519_verify(pub, body, (size_t)bytes, p->signature);
    if (signature != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "group policy signature check ch=%s version=%llu sc=%d", channel, (unsigned long long)p->version, signature);
        return signature == SC_ERR_AUTH_FAILED ? SQLITE_OK : SQLITE_ERROR;
    }
    *valid = 1; return SQLITE_OK;
}

int group_policy_newer(const struct group_policy* candidate, const struct group_policy* current) {
    if (candidate->version != current->version) return candidate->version > current->version;
    if (candidate->all_members != current->all_members) return candidate->all_members < current->all_members;
    return memcmp(candidate->signature, current->signature, 64) < 0;
}

int group_policy_get(sqlite3* db, const char* channel, struct group_policy* out) {
    sqlite3_stmt* st = NULL; struct group_policy p = {0};
    int rc = sqlite3_prepare_v2(db, "SELECT version,all_members,signature FROM group_policy WHERE channel_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) <= 0 ||
            sqlite3_column_type(st, 1) != SQLITE_INTEGER || sqlite3_column_int64(st, 1) < 0 || sqlite3_column_int64(st, 1) > 1 ||
            sqlite3_column_type(st, 2) != SQLITE_BLOB || sqlite3_column_bytes(st, 2) != 64) rc = SQLITE_CORRUPT;
        else if (!sqlite3_column_blob(st, 2)) rc = SQLITE_NOMEM;
        else {
            p.version = (uint64_t)sqlite3_column_int64(st, 0); p.all_members = (uint8_t)sqlite3_column_int(st, 1);
            memcpy(p.signature, sqlite3_column_blob(st, 2), 64);
        }
    }
    int final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy read ch=%s rc=%d final=%d: %s", channel, rc, final, sqlite3_errmsg(db));
        return -(rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : final);
    }
    int valid; rc = group_policy_check(db, channel, &p, &valid);
    if (rc || !valid) return rc ? -rc : -SQLITE_AUTH;
    *out = p; return 0;
}

int group_policy_apply(sqlite3* db, const char* channel, const struct group_policy* p) {
    struct group_policy old; int valid;
    if (sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy apply requires transaction"); return -SQLITE_MISUSE; }
    int code = group_policy_check(db, channel, p, &valid);
    if (code || !valid) return code ? -code : -SQLITE_AUTH;
    code = group_policy_get(db, channel, &old); if (code) return code;
    int wins = group_policy_newer(p, &old);
    if (p->version != old.version || p->all_members != old.all_members || memcmp(p->signature, old.signature, 64))
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "group policy ch=%s selected=%s reason=%s", channel, wins ? "incoming" : "stored",
            p->version != old.version ? "version" : p->all_members != old.all_members ? "stricter mode" : "signature");
    if (!wins) return 0;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO group_policy VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?),?,?,?) ON CONFLICT(channel_id) DO UPDATE SET"
        " version=excluded.version,all_members=excluded.all_members,signature=excluded.signature", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)p->version);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 3, p->all_members);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, p->signature, 64, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK || sqlite3_changes(db) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy write ch=%s rc=%d final=%d: %s", channel, rc, final, sqlite3_errmsg(db));
        return -(rc != SQLITE_DONE ? rc : final ? final : SQLITE_CONSTRAINT);
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "group policy received ch=%s version=%llu all=%u", channel,
        (unsigned long long)p->version, p->all_members); return 1;
}

#define GP_INVITE_PREFIX 97 /* Домен (5), Ed25519 пригласившего, подпись. */
struct gp_invitation {
    struct gp_invitation *next, *sibling; /* Варианты адресата и исходящие рёбра пригласившего. */
    size_t subject, issuer; /* issuer=count для ключа группы. */
    int member_signer; /* Ненулевой для приглашения от участника. */
    uint64_t stamp;
    uint8_t body[GP_INVITE_PREFIX + MEMBER_SIGN_MSG_SIZE];
    size_t size;
};
struct gp_member {
    sqlite3_int64 row; /* Строка снимка для записи результатов в той же транзакции. */
    uint64_t node, join_ts, profile_version;
    uint8_t signer[32];
    uint8_t x[32], ed[32], join_id[32], valid, admin, previous, allowed, keep;
    uint8_t deleted, verified, previous_verified;
    struct gp_invitation *invitations, *children, *winner, *preferred; /* preferred — внешний вход при размыкании. */
    int has_admin;
    size_t depth; /* SIZE_MAX — путь к корню пока не найден. */
    uint8_t stored_signature[64];
    char name[128];
};

/* Только готовые результаты проверки: никаких SQLite/runtime указателей в снимке. */
struct gp_access_member {
    uint64_t node;
    uint8_t x[32], ed[32];
    uint8_t transport, admitted, verified, roles, deleted, pending; /* roles — биты group_member_role. */
    uint8_t can_invite; /* Подтверждённая capability из того же SQL-снимка owner/policy/owner tags. */
    uint8_t recovery; /* Полный проверенный JOIN допускает только локальный receive-only MEMBERS. */
    uint64_t profile_version;
    char* userinfo; /* Собственная копия подписанного профиля; NULL — профиль ещё не готов. */
};

struct group_policy_cache {
    struct group_policy_cache* next;
    char channel[65];
    size_t count;
    int clipboard_members;
    uint8_t group_ed[32]; /* Ed25519 из подтверждённого описания; нули до получения. */
    uint8_t admin_root[MT_HASH_SIZE]; /* Корень административных записей из той же транзакции. */
    struct gp_access_member members[]; /* Упорядочены по X25519. */
};

static int gp_invite_capabilities(sqlite3* db, struct group_policy_cache* cache, const uint8_t owner[32], int all);

static int gp_member_key_order(const void* left, const void* right) {
    const struct gp_access_member *a = left, *b = right;
    int order = memcmp(a->x, b->x, 32);
    return order ? order : memcmp(a->ed, b->ed, 32);
}

static struct group_policy_cache* gp_cache(struct UTUN_INSTANCE* inst, const char* channel) {
    for (struct group_policy_cache* c = inst ? inst->group_access : NULL; c; c = c->next)
        if (!strcmp(c->channel, channel)) return c;
    return NULL;
}

/* Прикладной допуск ищет полный адрес независимо от локальной проекции для маршрутизатора. */
static const struct gp_access_member* gp_access_lookup_key(const struct group_policy_cache* c, const uint8_t x[32], const uint8_t* ed) {
    size_t lo = 0, hi = c ? c->count : 0;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (memcmp(c->members[mid].x, x, 32) < 0) lo = mid + 1; else hi = mid; }
    const struct gp_access_member* found = NULL;
    for (; c && lo < c->count && !memcmp(c->members[lo].x, x, 32); lo++) {
        if (ed && memcmp(c->members[lo].ed, ed, 32)) continue;
        if (found) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "ambiguous member X25519 ch=%s key=%s", c->channel, sc_public_key_hex(x, (char[65]){0}));
            return NULL;
        }
        found = &c->members[lo];
    }
    return found;
}

/* Числовой индекс остаётся на границе маршрутизатора. Коллизия требует полного ключа. */
static const struct gp_access_member* gp_access_lookup_route(const struct group_policy_cache* c, uint64_t node) {
    const struct gp_access_member* found = NULL;
    for (size_t i = 0; c && i < c->count; i++) {
        if (c->members[i].node != node) continue;
        if (found) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "ambiguous route projection ch=%s id=%016llx", c->channel, (unsigned long long)node);
            return NULL;
        }
        found = &c->members[i];
    }
    return found;
}
/* RAM fixtures не выполняют файловый I/O и могут вызывать примитивы без запущенного сервиса. */
static sqlite3* gp_ram_database(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->topo_sqlite_db || inst->db_worker) return NULL;
    const char* path = sqlite3_db_filename(inst->topo_sqlite_db, "main");
    return path && !path[0] ? inst->topo_sqlite_db : NULL;
}

static int gp_access_channel(const char* channel) {
    return chat_channel_valid(channel);
}

int group_policy_member_access(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, int transport) {
    if (!inst || !gp_access_channel(channel) || !node || (transport != 0 && transport != 1)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member access: invalid instance/channel/node/mode"); return 0;
    }
    struct group_policy_cache* c = gp_cache(inst, channel);
    if (!c) {
        sqlite3* ram = gp_ram_database(inst);
        if (ram) return transport ? topo_node_sqlite_member_transport_allowed(ram, channel, node)
            : topo_node_sqlite_member_allowed(ram, channel, node);
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "member access unavailable ch=%s node=%016llx transport=%d: no committed snapshot",
            channel, (unsigned long long)node, transport); return 0;
    }
    const struct gp_access_member* member = gp_access_lookup_route(c, node);
    return member && (transport ? member->transport : member->admitted);
}

int group_policy_member_keys(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, int active,
        uint8_t x[32], uint8_t ed[32]) {
    if (!inst || !gp_access_channel(channel) || !node || (!x && !ed) || (active != 0 && active != 1)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member keys snapshot: invalid arguments"); return TOPO_SQLITE_LOOKUP_ERROR;
    }
    struct group_policy_cache* c = gp_cache(inst, channel);
    if (!c) {
        sqlite3* ram = gp_ram_database(inst);
        if (ram) return topo_node_sqlite_member_keys(ram, channel, node, active, x, ed);
    }
    const struct gp_access_member* member = gp_access_lookup_route(c, node);
    if (!member || !member->verified || (active && !member->admitted)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "member keys unavailable ch=%s node=%016llx active=%d snapshot=%d",
            channel, (unsigned long long)node, active, !!c); return TOPO_SQLITE_LOOKUP_MISSING;
    }
    if (x) memcpy(x, member->x, 32);
    if (ed) memcpy(ed, member->ed, 32);
    return 0;
}

int group_policy_member_key_allowed(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t key[32], const uint8_t ed[32], int transport) {
    if (!key || (transport && !ed)) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member key snapshot: missing peer key"); return 0; }
    if (!gp_access_channel(channel)) return 0;
    struct group_policy_cache* cache = gp_cache(inst, channel);
    if (!cache) {
        sqlite3* db = gp_ram_database(inst);
        return db && topo_node_sqlite_member_key_allowed(db, channel, key, ed, transport);
    }
    const struct gp_access_member* member = gp_access_lookup_key(cache, key, ed);
    return member && (transport ? member->transport : member->verified && member->admitted);
}

int group_policy_member_ed_by_x(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t x[32], uint8_t ed[32]) {
    if (!inst || !gp_access_channel(channel) || !x || !ed) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member Ed25519 lookup: invalid arguments"); return TOPO_SQLITE_LOOKUP_ERROR;
    }
    const struct group_policy_cache* cache = gp_cache(inst, channel);
    if (!cache) {
        sqlite3* db = gp_ram_database(inst);
        return db ? topo_node_sqlite_member_ed_by_x(db, channel, x, ed) : TOPO_SQLITE_LOOKUP_MISSING;
    }
    const struct gp_access_member* member = gp_access_lookup_key(cache, x, NULL);
    if (!member || !member->verified || !member->admitted) return TOPO_SQLITE_LOOKUP_MISSING;
    memcpy(ed, member->ed, 32);
    return 0;
}

int group_policy_member_verified(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32]) {
    if (!inst || !gp_access_channel(channel) || !ed) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member verification snapshot: invalid channel/key"); return 0;
    }
    const struct group_policy_cache* cache = gp_cache(inst, channel);
    if (!cache) {
        sqlite3* ram = gp_ram_database(inst); uint64_t id;
        return ram && !topo_node_sqlite_member_local_id(ram, channel, ed, 0, &id, NULL);
    }
    uint8_t x[32];
    if (sc_identity_public_x("member verification snapshot", ed, x) != SC_OK) return 0;
    const struct gp_access_member* member = gp_access_lookup_key(cache, x, ed);
    return member && member->verified;
}

int group_policy_member_verified_key(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t x[32], const uint8_t ed[32]) {
    if (!inst || !gp_access_channel(channel) || !x || !ed) return 0;
    const struct gp_access_member* member = gp_access_lookup_key(gp_cache(inst, channel), x, ed);
    return member && member->verified;
}

int group_policy_recovery_source(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32], const uint8_t x[32]) {
    if (!inst || !gp_access_channel(channel) || !ed || !x) return 0;
    const struct gp_access_member* member = gp_access_lookup_key(gp_cache(inst, channel), x, ed);
    return member && member->recovery;
}
const uint8_t* group_policy_public_key(struct UTUN_INSTANCE* inst, const char* channel) {
    const struct group_policy_cache* c = gp_cache(inst, channel);
    return c && memcmp(c->group_ed, (uint8_t[32]){0}, 32) ? c->group_ed : NULL;
}

int group_policy_clipboard_members(struct UTUN_INSTANCE* inst, const char* channel) {
    struct group_policy_cache* cache = inst && gp_access_channel(channel) ? gp_cache(inst, channel) : NULL;
    return cache && cache->clipboard_members;
}

struct gp_roles { int flags, admin; };

static int gp_role_field(const char* key, const char* value, void* data) {
    struct gp_roles* roles = data;
    if (!strcmp(key, "admin")) {
        for (const char* p = value; *p; p++)
            if (*p == 'e' || *p == 'd') roles->admin = *p == 'e';
    } else if (!strcmp(value, "yes")) {
        if (!strcmp(key, "supernode")) roles->flags |= GROUP_MEMBER_SUPERNODE;
        if (!strcmp(key, "storage")) roles->flags |= GROUP_MEMBER_STORAGE;
    }
    return 0;
}

/* Отсутствующие поля оставляют роли нулевыми. Ошибка разбора прерывает построение снимка. */
static int gp_read_roles(sqlite3_stmt* st, int column, struct gp_roles* out) {
    int type = sqlite3_column_type(st, column); *out = (struct gp_roles){0};
    if (type == SQLITE_NULL) return SQLITE_OK;
    int bytes = sqlite3_column_bytes(st, column);
    if (type != SQLITE_TEXT || bytes > MS_ADM_TAGS_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "owner role tags invalid type=%d bytes=%d", type, bytes); return SQLITE_CORRUPT;
    }
    const char* tags = (const char*)sqlite3_column_text(st, column);
    if (!tags) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "owner role tags conversion allocation failed"); return SQLITE_NOMEM; }
    if (memchr(tags, 0, (size_t)bytes)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "owner role tags contain embedded NUL bytes=%d", bytes); return SQLITE_CORRUPT;
    }
    if (bytes && json_flat_parse(tags, gp_role_field, out)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "owner role tags parse failed bytes=%d", bytes); return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

static int gp_member_role(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, const uint8_t* ed, enum group_member_role role) {
    uint8_t x[32];
    if (ed && sc_identity_public_x("group member role", ed, x) != SC_OK) return -1;
    if (!inst || !gp_access_channel(channel) || (!node && !ed) || (role != GROUP_MEMBER_SUPERNODE && role != GROUP_MEMBER_STORAGE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "owner role snapshot: invalid instance/channel/node/role=%d", role); return -1;
    }
    struct group_policy_cache* cache = gp_cache(inst, channel);
    if (cache) {
        const struct gp_access_member* member = ed ? gp_access_lookup_key(cache, x, ed) : gp_access_lookup_route(cache, node);
        return member && member->admitted && member->verified && (member->roles & role) != 0;
    }
    sqlite3* ram = gp_ram_database(inst);
    if (!ram) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "owner role snapshot unavailable ch=%s node=%016llx role=%d",
            channel, (unsigned long long)node, role);
        return 0;
    }
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT adm_tags FROM peers_%s WHERE %s=?"
        " AND record_state=0 AND source=0 AND deleted=0 AND admitted=1 AND identity_verified=1",
        channel, ed ? "ed25519_pubkey" : "node_id");
    sqlite3_stmt* st = NULL; struct gp_roles roles = {0};
    int rc = sqlite3_prepare_v2(ram, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = ed ? sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC) : sqlite3_bind_int64(st, 1, (sqlite3_int64)node);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) rc = gp_read_roles(st, 0, &roles);
    else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_OK || final != SQLITE_OK) {
        int error = rc != SQLITE_OK ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "owner role RAM read failed ch=%s node=%016llx role=%d rc=%d finalize=%d: %s",
            channel, (unsigned long long)node, role, rc, final,
            (sqlite3_errcode(ram) & 255) == (error & 255) ? sqlite3_errmsg(ram) : sqlite3_errstr(error)); return -1;
    }
    return (roles.flags & role) != 0;
}

int group_policy_member_role(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, enum group_member_role role) {
    return gp_member_role(inst, channel, node, NULL, role);
}

int group_policy_member_role_key(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32], enum group_member_role role) {
    return gp_member_role(inst, channel, 0, ed, role);
}

/* Курсор по ID не удерживает указатель в снимок и не требует отдельного массива получателей. */
uint64_t group_policy_role_next(struct UTUN_INSTANCE* inst, const char* channel, enum group_member_role role, uint64_t after) {
    if (!inst || !gp_access_channel(channel) || (role != GROUP_MEMBER_SUPERNODE && role != GROUP_MEMBER_STORAGE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "owner role cursor: invalid instance/channel/role=%d", role); return 0;
    }
    const struct group_policy_cache* cache = gp_cache(inst, channel);
    uint64_t next = 0;
    for (size_t i = 0; cache && i < cache->count; i++) {
        const struct gp_access_member* member = &cache->members[i];
        if (member->node > after && (!next || member->node < next) && member->admitted && member->verified && (member->roles & role))
            next = member->node;
    }
    return next;
}

int group_policy_role_key_next(struct UTUN_INSTANCE* inst, const char* channel, enum group_member_role role,
                               size_t* cursor, uint8_t ed[32]) {
    if (!inst || !gp_access_channel(channel) || !cursor || !ed || (role != GROUP_MEMBER_SUPERNODE && role != GROUP_MEMBER_STORAGE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "owner role key cursor: invalid arguments role=%d", role); return 0;
    }
    const struct group_policy_cache* cache = gp_cache(inst, channel);
    while (cache && *cursor < cache->count) {
        const struct gp_access_member* member = &cache->members[(*cursor)++];
        if (!member->admitted || !member->verified || !(member->roles & role)) continue;
        memcpy(ed, member->ed, 32); return 1;
    }
    return 0;
}

static int gp_member_profile(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, const uint8_t* exact,
        const char** userinfo, uint64_t* version, uint8_t ed[32]) {
    if (!inst || !gp_access_channel(channel) || (!node && !exact) || !userinfo || !version) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member profile snapshot: invalid arguments"); return MS_AUTHOR_ERROR;
    }
    struct group_policy_cache* cache = gp_cache(inst, channel);
    if (!cache) return MS_AUTHOR_PENDING;
    uint8_t x[32];
    if (exact && sc_identity_public_x("member profile", exact, x) != SC_OK) return MS_AUTHOR_INVALID;
    const struct gp_access_member* member = exact ? gp_access_lookup_key(cache, x, exact) : gp_access_lookup_route(cache, node);
    if (!member || member->deleted) return MS_AUTHOR_INACTIVE;
    if (member->pending) return MS_AUTHOR_PENDING;
    if (!member->admitted || !member->verified) return MS_AUTHOR_INACTIVE;
    if (!member->userinfo) return MS_AUTHOR_PENDING;
    *userinfo = member->userinfo; *version = member->profile_version;
    if (ed) memcpy(ed, member->ed, 32);
    return MS_AUTHOR_ACTIVE;
}

int group_policy_member_profile(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node,
        const char** userinfo, uint64_t* version, uint8_t ed[32]) {
    return gp_member_profile(inst, channel, node, NULL, userinfo, version, ed);
}

int group_policy_member_profile_key(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32],
        const char** userinfo, uint64_t* version) {
    return gp_member_profile(inst, channel, 0, ed, userinfo, version, NULL);
}

void group_policy_forget(struct UTUN_INSTANCE* inst, const char* channel) {
    if (!inst) return;
    struct group_policy_cache** link = &inst->group_access;
    while (*link) {
        struct group_policy_cache* c = *link;
        if (channel && strcmp(c->channel, channel)) { link = &c->next; continue; }
        *link = c->next;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot released ch=%s members=%zu", c->channel, c->count);
        group_policy_snapshot_free(c);
        if (channel) break;
    }
}

/* У проверенной активной записи профиль может ещё отсутствовать (JOIN-only). */
static int gp_copy_profile(sqlite3_stmt* st, int column, struct gp_access_member* member) {
    if (!member->admitted || !member->verified) return SQLITE_OK;
    int type = sqlite3_column_type(st, column);
    if (type == SQLITE_NULL) return SQLITE_OK;
    sqlite3_int64 version = sqlite3_column_int64(st, column);
    if (type != SQLITE_INTEGER || version < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot profile version type=%d value=%lld", type, (long long)version);
        return SQLITE_CORRUPT;
    }
    if (!version || sqlite3_column_type(st, column + 2) == SQLITE_NULL) return SQLITE_OK;
    if (sqlite3_column_type(st, column + 2) != SQLITE_BLOB || sqlite3_column_bytes(st, column + 2) != 64) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot profile signature type=%d bytes=%d",
            sqlite3_column_type(st, column + 2), sqlite3_column_bytes(st, column + 2)); return SQLITE_CORRUPT;
    }
    const void* signature = sqlite3_column_blob(st, column + 2);
    if (!signature) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot profile signature unavailable"); return SQLITE_NOMEM; }
    if (!memcmp(signature, (uint8_t[64]){0}, 64) || sqlite3_column_type(st, column + 1) == SQLITE_NULL) return SQLITE_OK;
    type = sqlite3_column_type(st, column + 1); int bytes = sqlite3_column_bytes(st, column + 1);
    if (type != SQLITE_TEXT || bytes > MS_USERINFO_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot profile text type=%d bytes=%d", type, bytes); return SQLITE_CORRUPT;
    }
    const char* text = (const char*)sqlite3_column_text(st, column + 1);
    if (!text) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot profile conversion failed"); return SQLITE_NOMEM; }
    if (memchr(text, 0, (size_t)bytes)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot profile contains NUL bytes=%d", bytes); return SQLITE_CORRUPT;
    }
    member->userinfo = u_calloc((size_t)bytes + 1, 1);
    if (!member->userinfo) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot profile copy allocation failed bytes=%d", bytes); return SQLITE_NOMEM;
    }
    memcpy(member->userinfo, text, (size_t)bytes); member->profile_version = (uint64_t)version; return SQLITE_OK;
}

/* Транспортные снимки без CHAT-метаданных не получают права приглашать. Ошибки SQL не дают capability. */
static int gp_invite_settings_read(sqlite3* db, const char* channel, uint8_t owner[32], int* all) {
    memset(owner, 0, 32); *all = 0;
    if (!chat_channel_valid(channel)) return SQLITE_OK;
    sqlite3_stmt* st = NULL; const char* stage = "schema";
    int rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN('channels','group_policy')",
        -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    int complete = rc == SQLITE_ROW && sqlite3_column_int(st, 0) == 2;
    if (rc == SQLITE_ROW) rc = SQLITE_OK;
    { int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final; }
    if (rc || !complete) goto done;
    stage = "owner";
    rc = sqlite3_prepare_v2(db, "SELECT owner_node_id FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    else if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) == SQLITE_NULL) rc = SQLITE_OK;
        else rc = topo_node_sqlite_read_optional_key(st, 0, owner);
    }
    { int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final; }
    if (rc || !member_sync_signer(owner)) goto done;
    struct group_policy policy;
    stage = "signed policy"; rc = group_policy_get(db, channel, &policy);
    if (rc < 0) rc = rc == -1 ? SQLITE_AUTH : -rc;
    if (!rc) *all = policy.all_members;
done:
    if (rc) {
        memset(owner, 0, 32); *all = 0;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invitation capability read ch=%s stage=%s rc=%d: %s", channel, stage, rc, sqlite3_errstr(rc));
    }
    return rc;
}

int group_policy_snapshot_read(sqlite3* db, const char* channel, struct group_policy_cache** out) {
    if (out) *out = NULL;
    if (!db || !gp_access_channel(channel) || !out) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot read: invalid database/channel/output"); return SQLITE_MISUSE;
    }
    char sql[320]; sqlite3_stmt* st = NULL; struct group_policy_cache* c = NULL;
    const char* stage = "prepare";
    uint8_t invite_owner[32]; int invite_all;
    int rc = gp_invite_settings_read(db, channel, invite_owner, &invite_all); if (rc) return rc;
    snprintf(sql, sizeof(sql), "SELECT node_id,x25519_pubkey,ed25519_pubkey,deleted,admitted,identity_verified,adm_tags,"
        "source,update_ts,userinfo,update_sig FROM peers_%s WHERE record_state=0", channel);
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL); size_t capacity = 0, count = 0;
    if (rc != SQLITE_OK) goto done;
    stage = "allocate header";
    c = u_calloc(1, sizeof(*c));
    if (!c) { rc = SQLITE_NOMEM; goto done; }
    strcpy(c->channel, channel); stage = "step";
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count == capacity) {
            stage = "grow rows";
            size_t next = capacity ? capacity * 2 : 16;
            if (next > (UINT32_MAX - sizeof(*c)) / sizeof(c->members[0])) { rc = SQLITE_TOOBIG; goto done; }
            struct group_policy_cache* resized = u_realloc(c, sizeof(*c) + next * sizeof(c->members[0]));
            if (!resized) { rc = SQLITE_NOMEM; goto done; }
            c = resized; capacity = next;
        }
        struct gp_access_member* row = &c->members[count]; memset(row, 0, sizeof(*row));
        stage = "integer ID/trust column types";
        if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_type(st, 3) != SQLITE_INTEGER ||
            sqlite3_column_type(st, 4) != SQLITE_INTEGER || sqlite3_column_type(st, 5) != SQLITE_INTEGER ||
            sqlite3_column_type(st, 7) != SQLITE_INTEGER) {
            rc = SQLITE_CORRUPT; goto done;
        }
        row->node = (uint64_t)sqlite3_column_int64(st, 0);
        stage = "route projection/trust flags";
        int deleted = sqlite3_column_int(st, 3), admitted = sqlite3_column_int(st, 4), verified = sqlite3_column_int(st, 5);
        if (!row->node || deleted < 0 || deleted > 1 ||
            admitted < 0 || admitted > 1 || verified < 0 || verified > 1) { rc = SQLITE_CORRUPT; goto done; }
        row->deleted = (uint8_t)deleted; row->pending = sqlite3_column_int64(st, 7) != 0;
        row->transport = !deleted && !row->pending; row->admitted = admitted && row->transport;
        row->verified = verified && !row->pending;
        stage = "verified key sizes";
        const void* x = sqlite3_column_blob(st, 1); const void* ed = sqlite3_column_blob(st, 2);
        if (x && ed && sqlite3_column_bytes(st, 1) == 32 && sqlite3_column_bytes(st, 2) == 32) {
            memcpy(row->x, x, 32); memcpy(row->ed, ed, 32);
        } else if (row->verified) { rc = SQLITE_CORRUPT; goto done; }
        stage = "owner role tags"; struct gp_roles roles; rc = gp_read_roles(st, 6, &roles);
        if (rc != SQLITE_OK) goto done;
        row->roles = (uint8_t)roles.flags; stage = "signed profile"; rc = gp_copy_profile(st, 8, row);
        if (rc != SQLITE_OK) goto done;
        count++; c->count = count; stage = "step";
    }
done:
    { int final = sqlite3_finalize(st); if (rc == SQLITE_DONE) { rc = final; stage = "finalize"; } }
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot read failed ch=%s stage=%s rows=%zu rc=%d (%s): %s",
            channel, stage, count, rc, sqlite3_errstr(rc),
            (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        group_policy_snapshot_free(c); return rc;
    }
    c->count = count;
    if (count > 1) qsort(c->members, count, sizeof(c->members[0]), gp_member_key_order);
    if (member_sync_signer(invite_owner)) {
        rc = group_info_access_read(db, channel, &c->clipboard_members, c->group_ed);
        if (!rc) {
            char tree[70]; snprintf(tree, sizeof(tree), "%u:%s", MT_DATA_MEMBER_ADMIN, channel);
            rc = -merkle_tree_get(db, tree, 0, 0, c->admin_root);
        }
        if (rc) { group_policy_snapshot_free(c); return rc; }
    }
    for (size_t i = 0; i < count; i++) if (member_sync_signer(invite_owner) && c->members[i].transport) {
        int valid = 0; rc = member_sync_recovery_source_read(db, channel, c->members[i].ed, c->members[i].x, &valid);
        if (rc) { group_policy_snapshot_free(c); return rc; }
        c->members[i].recovery = (uint8_t)valid;
    }
    rc = member_sync_signer(invite_owner) ? gp_invite_capabilities(db, c, invite_owner, invite_all) : SQLITE_OK;
    if (rc) { group_policy_snapshot_free(c); return rc; }
    *out = c; return SQLITE_OK;
}

void group_policy_snapshot_publish(struct UTUN_INSTANCE* inst, struct group_policy_cache* cache) {
    if (!inst || !cache) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot publish: missing instance/snapshot"); group_policy_snapshot_free(cache); return;
    }
    group_policy_forget(inst, cache->channel);
    cache->next = inst->group_access; inst->group_access = cache;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot committed ch=%s members=%zu", cache->channel, cache->count);
}

void group_policy_snapshot_free(struct group_policy_cache* cache) {
    if (!cache) return;
    for (size_t i = 0; i < cache->count; i++) u_free(cache->members[i].userinfo);
    u_free(cache);
}

int group_policy_snapshot_present(struct UTUN_INSTANCE* inst, const char* channel) {
    return gp_cache(inst, channel) != NULL;
}

int group_policy_profiles_read(sqlite3* db, const char* channel, const merkle_key_t* keys, size_t count,
    struct group_policy_cache** out) {
    *out = NULL;
    char sql[320]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT x25519_pubkey,admitted AND NOT deleted,identity_verified,update_ts,userinfo,update_sig"
        " FROM peers_%s WHERE ed25519_pubkey=? AND record_state=0 AND source=0", channel);
    struct group_policy_cache* delta = count <= (UINT32_MAX - sizeof(*delta)) / sizeof(delta->members[0])
        ? u_calloc(1, sizeof(*delta) + count * sizeof(delta->members[0])) : NULL;
    int rc = delta ? sqlite3_prepare_v2(db, sql, -1, &st, NULL) : SQLITE_NOMEM;
    if (delta) strcpy(delta->channel, channel);
    for (size_t i = 0; !rc && i < count; i++) {
        rc = sqlite3_bind_blob(st, 1, keys[i].bytes, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_CORRUPT; break; }
        const void* x = sqlite3_column_blob(st, 0);
        if (!x || sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) { rc = SQLITE_CORRUPT; break; }
        struct gp_access_member* row = &delta->members[delta->count++];
        memcpy(row->x, x, 32);
        memcpy(row->ed, keys[i].bytes, 32);
        row->admitted = sqlite3_column_int(st, 1) == 1;
        row->verified = sqlite3_column_int(st, 2) == 1;
        rc = gp_copy_profile(st, 3, row);
        if (!rc) rc = sqlite3_reset(st);
    }
    int final = sqlite3_finalize(st);
    if (!rc) rc = final;
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member profile snapshot failed ch=%s rc=%d (%s)", channel, rc, sqlite3_errstr(rc));
        group_policy_snapshot_free(delta); return rc;
    }
    *out = delta; return SQLITE_OK;
}

void group_policy_profiles_publish(struct UTUN_INSTANCE* inst, struct group_policy_cache* profiles) {
    struct group_policy_cache* cache = gp_cache(inst, profiles->channel);
    for (size_t i = 0; i < profiles->count; i++) {
        struct gp_access_member* profile = &profiles->members[i];
        struct gp_access_member* member = (struct gp_access_member*)gp_access_lookup_key(cache, profile->x, profile->ed);
        if (!member) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member profile snapshot target missing ch=%s ed=%s",
                profiles->channel, sc_public_key_hex(profile->ed, (char[65]){0}));
            continue;
        }
        u_free(member->userinfo);
        member->userinfo = profile->userinfo; profile->userinfo = NULL;
        member->profile_version = profile->profile_version;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "member profiles committed ch=%s records=%zu", profiles->channel, profiles->count);
    group_policy_snapshot_free(profiles);
}

static size_t gp_find_ed(const struct gp_member* m, size_t count, const uint8_t ed[32]) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (memcmp(m[mid].ed, ed, 32) < 0) lo = mid + 1; else hi = mid;
    }
    return lo < count && !memcmp(m[lo].ed, ed, 32) ? lo : count;
}

static void gp_members_free(struct gp_member* members, size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct gp_invitation* edge = members[i].invitations;
        while (edge) { struct gp_invitation* next = edge->next; u_free(edge); edge = next; }
    }
    u_free(members);
}

/* sig проверяется по удостоверению группы даже при отсутствии пути к корню. */
static int gp_add_edge(const char* ch, const uint8_t group_key[32], struct gp_member* m, size_t count, size_t n,
    const uint8_t* signer, uint64_t stamp, const uint8_t* sig) {
    size_t issuer = signer ? gp_find_ed(m, count, signer) : count;
    if (n == count || !m[n].valid || (signer && issuer == count)) return SQLITE_AUTH;
    struct gp_invitation* edge = u_calloc(1, sizeof(*edge));
    if (!edge) return SQLITE_NOMEM;
    edge->subject = n; edge->issuer = issuer; edge->member_signer = signer != NULL; edge->stamp = stamp;
    edge->body[0] = 5; /* Домен канонического хеша административного приглашения. */
    memcpy(edge->body + 1, signer ? m[issuer].ed : group_key, 32); memcpy(edge->body + 33, sig, 64);
    uint8_t* json = edge->body + GP_INVITE_PREFIX;
    int bytes = member_sync_build_sign_msg(ch, signer ? m[issuer].ed : NULL, m[n].x, m[n].ed, m[n].join_id, stamp, json, MEMBER_SIGN_MSG_SIZE);
    int verified = bytes > 0 ? sc_ed25519_verify(edge->body + 1, json, (size_t)bytes, sig) : SC_ERR_AUTH_FAILED;
    if (verified != SC_OK) { u_free(edge); return verified == SC_ERR_AUTH_FAILED ? SQLITE_AUTH : SQLITE_ERROR; }
    edge->size = GP_INVITE_PREFIX + (size_t)bytes;
    edge->next = m[n].invitations; m[n].invitations = edge;
    if (signer) { edge->sibling = m[issuer].children; m[issuer].children = edge; }
    return SQLITE_OK;
}

/* Загружает удостоверения целиком. Ошибка проверки не превращается в массовое исключение. */
static int gp_load(sqlite3* db, const char* ch, struct gp_member** out, size_t* count, int proof_only) {
    char sql[512]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,update_sig,update_ts,"
        "userinfo,adm_tags,adm_tags_sig,signed_by,signature,deleted,admitted,identity_verified,id FROM peers_%s"
        " WHERE record_state=0 AND source=0 ORDER BY ed25519_pubkey", ch);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    struct gp_member* list = NULL; size_t n = 0, capacity = 0;
    if (rc != SQLITE_OK) goto done;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            if (next > UINT32_MAX / sizeof(*list)) { rc = SQLITE_TOOBIG; goto done; }
            struct gp_member* p = u_realloc(list, next * sizeof(*p));
            if (!p) { rc = SQLITE_NOMEM; goto done; } list = p; capacity = next;
        }
        struct gp_member* p = &list[n++]; memset(p, 0, sizeof(*p));
        p->row = sqlite3_column_int64(st, 15); p->node = (uint64_t)sqlite3_column_int64(st, 0);
        rc = topo_node_sqlite_read_optional_key(st, 10, p->signer); if (rc) goto done;
        p->previous = sqlite3_column_int(st, 13) != 0;
        p->deleted = sqlite3_column_int(st, 12) != 0;
        p->previous_verified = sqlite3_column_int(st, 14) != 0;
        const char* info = (const char*)sqlite3_column_text(st, 7);
        if (info) json_flat_get(info, "name", p->name, sizeof(p->name));
        if (sqlite3_column_bytes(st, 2) == 32 && sqlite3_column_blob(st, 2)) memcpy(p->ed, sqlite3_column_blob(st, 2), 32);
        if (sqlite3_column_bytes(st, 1) != 32 || sqlite3_column_bytes(st, 2) != 32 || sqlite3_column_bytes(st, 3) != 64) continue;
        struct ms_member_rec m = {.node_id = p->node, .x25519 = sqlite3_column_blob(st, 1), .ed25519 = sqlite3_column_blob(st, 2),
            .join_sig = sqlite3_column_blob(st, 3), .join_ts = (uint64_t)sqlite3_column_int64(st, 4),
            .update_sig = sqlite3_column_bytes(st, 5) == 64 ? sqlite3_column_blob(st, 5) : NULL,
            .update_ts = (uint64_t)sqlite3_column_int64(st, 6), .userinfo = info, .adm_tags = (const char*)sqlite3_column_text(st, 8),
            .adm_tags_sig = sqlite3_column_bytes(st, 9) == 64 ? sqlite3_column_blob(st, 9) : NULL,
            .signed_by = member_sync_signer(p->signer), .signature = sqlite3_column_blob(st, 11)};
        if (!m.x25519 || !m.ed25519 || !m.join_sig) { rc = SQLITE_NOMEM; goto done; }
        memcpy(p->x, m.x25519, 32);
        rc = member_sync_join_id_read(db, ch, m.ed25519, m.x25519, p->join_id);
        if (rc) goto done;
        p->join_ts = m.join_ts; p->profile_version = m.update_ts;
        if (sqlite3_column_bytes(st, 11) == 64 && m.signature) memcpy(p->stored_signature, m.signature, 64);
        m.signature = NULL; /* Каждую подпись приглашения проверяет gp_add_edge. */
        int verified = member_sync_verify_local_record(db, ch, &m);
        if (verified < 0) {
            rc = sqlite3_extended_errcode(db); if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) rc = SQLITE_ERROR;
            goto done;
        }
        p->valid = verified == 1 && (proof_only || !p->deleted) && memcmp(m.join_sig, (uint8_t[64]){0}, 64);
        if (p->valid && m.adm_tags_sig && memcmp(m.adm_tags_sig, (uint8_t[64]){0}, 64)) {
            struct gp_roles roles; rc = gp_read_roles(st, 8, &roles);
            if (rc) goto done;
            p->admin = (uint8_t)roles.admin;
        }
    }
done:
    { int final = sqlite3_finalize(st); if (rc == SQLITE_DONE) rc = final; st = NULL; }
    uint8_t key[32]; int found = 0;
    if (!rc && n) rc = member_sync_channel_keys_read(db, ch, NULL, key, &found);
    if (!rc && n && !found) rc = SQLITE_NOTFOUND;
    if (!rc) rc = sqlite3_prepare_v2(db, "SELECT subject,signer,stamp,signature FROM member_invitations WHERE channel=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, ch, -1, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t* subject = sqlite3_column_blob(st, 0);
        const uint8_t* signer = sqlite3_column_blob(st, 1);
        const uint8_t* signature = sqlite3_column_blob(st, 3);
        if (!subject || !signer || !signature || sqlite3_column_bytes(st, 0) != 32 || sqlite3_column_bytes(st, 1) != 32 ||
            sqlite3_column_bytes(st, 3) != 64) { rc = SQLITE_CORRUPT; break; }
        size_t i = gp_find_ed(list, n, subject);
        signer = member_sync_signer(signer);
        if (i == n || !list[i].valid || (signer && gp_find_ed(list, n, signer) == n)) continue;
        rc = gp_add_edge(ch, key, list, n, i, signer, (uint64_t)sqlite3_column_int64(st, 2), signature);
        if (rc == SQLITE_AUTH) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "unverified invitation ch=%s member=%016llx signer=%s",
                ch, (unsigned long long)list[i].node, sc_public_key_hex(signer, (char[65]){0}));
            continue;
        }
        if (rc) break;
    }
    { int final = sqlite3_finalize(st); if (rc == SQLITE_DONE) rc = final; }
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group admission load ch=%s rows=%zu rc=%d: %s", ch, n, rc, sqlite3_errmsg(db));
        gp_members_free(list, n);
    } else { *out = list; *count = n; }
    return -rc;
}

static int gp_administrative(const struct gp_member* m, const struct gp_invitation* e, const uint8_t owner[32]) {
    return !e->member_signer || (owner && !memcmp(m[e->issuer].ed, owner, 32)) || m[e->issuer].admin;
}

/* Полный порядок при равенстве расстояний. Содержимое конверта обеспечивает последний tie-break. */
static int gp_edge_before(const struct gp_invitation* a, const struct gp_invitation* b) {
    if (!b) return 1;
    int order = memcmp(a->body + 33, b->body + 33, 64);
    if (!order) order = memcmp(a->body, b->body, a->size < b->size ? a->size : b->size);
    return order ? order < 0 : a->size < b->size;
}

/* Кратчайшие пути по предпочтительным рёбрам. У победителя глубина пригласившего строго меньше. */
static int gp_path_edge(const struct gp_member* m, const struct gp_invitation* e, const uint8_t owner[32]) {
    const struct gp_member* p = &m[e->subject];
    return p->preferred ? p->preferred == e : !p->has_admin || gp_administrative(m, e, owner);
}

static void gp_paths(struct gp_member* m, size_t count, int all, const uint8_t owner[32], size_t* queue) {
    size_t head = 0, tail = 0;
    for (size_t i = 0; i < count; i++) {
        m[i].depth = SIZE_MAX; m[i].winner = NULL;
        if (!m[i].valid) continue;
        for (struct gp_invitation* e = m[i].invitations; e; e = e->next) {
            if (gp_path_edge(m, e, owner) && (!e->member_signer || m[i].keep)) {
                if (!m[i].winner || gp_edge_before(e, m[i].winner)) m[i].winner = e;
            }
        }
        if (m[i].winner) { m[i].depth = 1; queue[tail++] = i; }
    }
    while (head < tail) {
        struct gp_member* parent = &m[queue[head++]];
        if (!all && !parent->admin && (!owner || memcmp(parent->ed, owner, 32))) continue;
        size_t depth = parent->depth + 1;
        for (struct gp_invitation* e = parent->children; e; e = e->sibling) {
            struct gp_member* p = &m[e->subject];
            if (!p->valid || !gp_path_edge(m, e, owner) || p->depth < depth) continue;
            if (p->depth == SIZE_MAX) queue[tail++] = e->subject;
            if (p->depth > depth || gp_edge_before(e, p->winner)) { p->depth = depth; p->winner = e; }
        }
    }
}

static int gp_evaluate(struct gp_member* m, size_t count, int all, const uint8_t owner[32]) {
    size_t* queue = count ? u_malloc(count * sizeof(*queue)) : NULL;
    if (count && !queue) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group path allocation count=%zu", count); return -1; }
    for (size_t i = 0; i < count; i++) {
        m[i].preferred = NULL; m[i].has_admin = 0;
        for (struct gp_invitation* e = m[i].invitations; e; e = e->next) {
            if (gp_administrative(m, e, owner) && (!e->member_signer || m[e->issuer].valid))
                m[i].has_admin = 1;
        }
    }
    int used_fallback = 0;
    for (;;) {
        gp_paths(m, count, all, owner, queue);
        struct gp_invitation* exit = NULL; size_t best = SIZE_MAX;
        for (size_t i = 0; i < count; i++) {
            if (!m[i].valid || m[i].depth != SIZE_MAX || !m[i].has_admin) continue;
            for (struct gp_invitation* e = m[i].invitations; e; e = e->next) {
                if (e->member_signer && (!all && !gp_administrative(m, e, owner))) continue;
                size_t depth = e->member_signer ? m[e->issuer].depth : 0;
                if (depth != SIZE_MAX && (depth < best || (depth == best && gp_edge_before(e, exit)))) { best = depth; exit = e; }
            }
        }
        if (!exit) break;
        m[exit->subject].preferred = exit;
        used_fallback = 1;
    }
    /* Внешний вход мог открыть административный путь также для потомков кольца.
     * Возвращаем приоритет там, где замена оставляет путь участника к корню. */
    while (used_fallback) {
        used_fallback = 0;
        for (size_t i = 0; i < count; i++) {
            if (!m[i].preferred || m[i].depth == SIZE_MAX) continue;
            struct gp_invitation* previous = m[i].preferred;
            m[i].preferred = NULL;
            gp_paths(m, count, all, owner, queue);
            if (m[i].depth != SIZE_MAX) { used_fallback = 1; continue; }
            m[i].preferred = previous;
            gp_paths(m, count, all, owner, queue);
        }
    }
    for (size_t i = 0; i < count; i++) {
        m[i].allowed = m[i].depth != SIZE_MAX;
        if (m[i].allowed) continue;
        for (struct gp_invitation* e = m[i].invitations; e; e = e->next) {
            if (!gp_path_edge(m, e, owner)) continue;
            if (gp_edge_before(e, m[i].winner)) m[i].winner = e;
        }
    }
    u_free(queue); return 0;
}

int group_policy_choose_invitations(sqlite3* db, const char* ch, struct group_invitation_candidate* c, size_t nc) {
    struct gp_member* m = NULL; size_t count = 0; struct group_policy policy; uint8_t owner[32];
    uint8_t key[32]; int found = 0;
    int rc = member_sync_channel_keys_read(db, ch, NULL, key, &found);
    if (!rc && (!found || group_policy_get(db, ch, &policy) || topo_node_sqlite_channel_get(db, ch, NULL, 0, owner, NULL, NULL, NULL))) rc = SQLITE_ERROR;
    if (!rc) rc = -gp_load(db, ch, &m, &count, 1);
    for (size_t i = 0; !rc && i < nc; i++) {
        size_t n = gp_find_ed(m, count, c[i].subject);
        rc = gp_add_edge(ch, key, m, count, n, member_sync_signer(c[i].signer), c[i].stamp, c[i].signature);
        if (rc) DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "invitation verification ch=%s member=%s signer=%s rc=%d",
            ch, sc_public_key_hex(c[i].subject, (char[65]){0}), sc_public_key_hex(c[i].signer, (char[65]){0}), rc);
    }
    for (size_t i = 0; i < count; i++) if (m[i].deleted) m[i].valid = 0;
    if (!rc && gp_evaluate(m, count, policy.all_members, owner)) rc = SQLITE_NOMEM;
    for (size_t i = 0; !rc && i < nc; i++) {
        struct gp_member* member = &m[gp_find_ed(m, count, c[i].subject)];
        struct gp_invitation* e = member->winner;
        c[i].wins = e && e->stamp == c[i].stamp && !memcmp(e->body + 33, c[i].signature, 64) &&
            (e->member_signer ? !memcmp(e->body + 1, c[i].signer, 32) : !member_sync_signer(c[i].signer));
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS choose ch=%s member=%s signer=%s selected=%d rooted=%d winner=%s admin=%d depth=%zu",
            ch, sc_public_key_hex(c[i].subject, (char[65]){0}), sc_public_key_hex(c[i].signer, (char[65]){0}), c[i].wins, member->allowed,
            sc_public_key_hex(e ? e->body + 1 : NULL, (char[65]){0}), e && gp_administrative(m, e, owner), member->depth);
    }
    gp_members_free(m, count); return rc;
}

/* Profile-only refresh не наследует доверие: подписи owner/JOIN и цепочки проверяются по текущему ключу группы. */
static int gp_invite_capabilities(sqlite3* db, struct group_policy_cache* cache, const uint8_t owner[32], int all) {
    struct gp_member* members = NULL; size_t count = 0;
    int rc = gp_load(db, cache->channel, &members, &count, 0);
    if (rc < 0) return -rc;
    if (gp_evaluate(members, count, all, owner)) { gp_members_free(members, count); return SQLITE_NOMEM; }
    for (size_t i = 0; i < cache->count; i++) {
        struct gp_access_member* row = &cache->members[i]; size_t j = gp_find_ed(members, count, row->ed);
        row->can_invite = row->admitted && row->verified && j < count && members[j].allowed &&
            (all || !memcmp(row->ed, owner, 32) || members[j].admin);
    }
    gp_members_free(members, count); return SQLITE_OK;
}

/* Поколение не зависит от профиля/сетевых адресов. Метка владельца переживает восстановление допуска. */
static int gp_message_generation(sqlite3* db, const char* channel, uint64_t node, const uint8_t* ed, uint8_t out[32]) {
    if (!db || !chat_channel_valid(channel) || (!node && !ed) || !out) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "message generation: invalid arguments"); return -1;
    }
    char sql[256]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT x25519_pubkey,adm_tags,ed25519_pubkey FROM peers_%s WHERE %s=? AND record_state=0 AND source=0",
        channel, ed ? "ed25519_pubkey" : "node_id");
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = ed ? sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC) : sqlite3_bind_int64(st, 1, (sqlite3_int64)node);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
        int final = sqlite3_finalize(st); st = NULL;
        if (final != SQLITE_OK) goto fail;
        return 1;
    }
    uint8_t body[64] = "chat-message-epoch-v4", generation[32];
    if (rc != SQLITE_ROW || sqlite3_column_bytes(st, 0) != 32 || sqlite3_column_bytes(st, 2) != 32) goto fail;
    const void* x = sqlite3_column_blob(st, 0); const void* subject = sqlite3_column_blob(st, 2);
    if (!x || !subject || member_sync_join_id_read(db, channel, subject, x, body + 24)) goto fail;
    const char* tags = (const char*)sqlite3_column_text(st, 1); uint64_t epoch;
    if (sqlite3_errcode(db) == SQLITE_NOMEM || member_sync_owner_epoch(tags, &epoch)) goto fail;
    gp_write64(body + 56, epoch);
    if (EVP_Digest(body, sizeof(body), generation, NULL, EVP_sha256(), NULL) != 1) goto fail;
    int final = sqlite3_finalize(st); st = NULL;
    if (final != SQLITE_OK) goto fail;
    memcpy(out, generation, 32); return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "message generation failed ch=%s node=%016llx ed=%s rc=%d: %s",
        channel, (unsigned long long)node, sc_public_key_hex(ed, (char[65]){0}), rc, sqlite3_errmsg(db)); sqlite3_finalize(st); return -1;
}

int group_policy_message_generation(sqlite3* db, const char* channel, uint64_t node, uint8_t out[32]) {
    return gp_message_generation(db, channel, node, NULL, out);
}

int group_policy_key_generation(sqlite3* db, const char* channel, const uint8_t ed[32], uint8_t out[32]) {
    return gp_message_generation(db, channel, 0, ed, out);
}

/* Только автор административного изменения подписывает закрытие поколений; получатели распространяют готовые owner-блоки. */
int group_policy_seal_revocations_write(sqlite3* db, struct member_sync_write_context* context, const char* channel) {
    if (!db || !context || !chat_channel_valid(channel) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "message epoch seal requires caller transaction"); return -1;
    }
    struct gp_member* members = NULL; size_t count = 0; struct group_policy policy; uint8_t owner[32];
    uint8_t key[32] = {0}; int result = -1; sqlite3_stmt* st = NULL;
    if (group_policy_get(db, channel, &policy) || topo_node_sqlite_channel_get(db, channel, NULL, 0, owner, NULL, NULL, NULL)
        || gp_load(db, channel, &members, &count, 0) || gp_evaluate(members, count, policy.all_members, owner)) goto done;
    int have_key = 0;
    for (size_t i = 0; i < count; i++) {
        struct gp_member* m = &members[i];
        if (!m->previous || m->allowed) continue;
        if (!have_key && topo_node_sqlite_channel_get_priv(db, channel, key)) goto done;
        have_key = 1;
        uint8_t signature[64], body[MS_OWNER_MSG_MAX]; char token[32], sql[384], tags[MS_ADM_TAGS_MAX + 1];
        snprintf(sql, sizeof(sql), "SELECT adm_tags FROM peers_%s WHERE ed25519_pubkey=? AND record_state=0", channel);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK || sqlite3_bind_blob(st, 1, m->ed, 32, SQLITE_STATIC) != SQLITE_OK
            || sqlite3_step(st) != SQLITE_ROW) goto done;
        const char* previous = (const char*)sqlite3_column_text(st, 0);
        uint64_t epoch;
        if (member_sync_owner_epoch(previous, &epoch) || epoch == INT64_MAX) goto done;
        snprintf(token, sizeof(token), "%llu", (unsigned long long)(epoch + 1));
        int version = previous && previous[0] && strcmp(previous, "{}") ? member_sync_owner_version(db, previous) : 0;
        if (version < 0 || version == INT_MAX) goto done;
        snprintf(tags, sizeof(tags), "%s", previous && previous[0] ? previous : "{}");
        if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto done; } st = NULL;
        char seal[65] = {0}, chat_seal[65] = {0};
        if (group_protocol_type(db, channel) == GROUP_PROTOCOL_CHAT && chat_log_seal(db, channel, m->ed, chat_seal)) goto done;
        if (group_protocol_type(db, channel) == 2) {
            uint8_t generation[32];
            if (group_policy_key_generation(db, channel, m->ed, generation)) goto done;
            struct kanban_identity writer = {.now=(int64_t)context->now};
            memcpy(writer.public_key, context->public_key, 32); memcpy(writer.private_key, context->private_key, 32);
            int sealed = kanban_seal_write(db, &writer, channel, m->ed, generation, seal);
            OPENSSL_cleanse(&writer, sizeof(writer));
            if (sealed) goto done;
        }
        if (sqlite3_prepare_v2(db, "SELECT CASE WHEN ?5<>'' THEN json_set(?1,'$.ver',?2,'$._message_epoch',?3,'$._chat_seal',?5)"
            " WHEN ?4='' THEN json_set(?1,'$.ver',?2,'$._message_epoch',?3)"
            " ELSE json_set(?1,'$.ver',?2,'$._message_epoch',?3,'$._kanban_seal',?4) END", -1, &st, NULL) != SQLITE_OK
            || sqlite3_bind_text(st, 1, tags, -1, SQLITE_TRANSIENT) != SQLITE_OK) goto done;
        char version_text[32]; snprintf(version_text, sizeof(version_text), "%d", version + 1);
        if (sqlite3_bind_text(st, 2, version_text, -1, SQLITE_STATIC) != SQLITE_OK
            || sqlite3_bind_text(st, 4, seal, -1, SQLITE_STATIC) != SQLITE_OK
            || sqlite3_bind_text(st, 5, chat_seal, -1, SQLITE_STATIC) != SQLITE_OK
            || sqlite3_bind_text(st, 3, token, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) goto done;
        const char* encoded = (const char*)sqlite3_column_text(st, 0); int bytes = sqlite3_column_bytes(st, 0);
        if (!encoded || bytes <= 0 || bytes > MS_ADM_TAGS_MAX) goto done;
        memcpy(tags, encoded, bytes + 1);
        if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto done; } st = NULL;
        bytes = member_sync_build_owner_msg(channel, m->ed, tags, body, sizeof(body));
        struct ms_member_rec record = {.node_id=m->node, .x25519=m->x, .ed25519=m->ed, .adm_tags=tags, .adm_tags_sig=signature};
        if (bytes < 0 || sc_ed25519_sign(key, body, bytes, signature) != SC_OK ||
            member_sync_record_write(db, context, channel, NULL, &record) < 0) goto done;
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "message epoch closed ch=%s node=%016llx owner_ver=%d", channel,
            (unsigned long long)m->node, version + 1);
    }
    result = 0;
done:
    if (result) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "message epoch sealing failed ch=%s: %s", channel, sqlite3_errmsg(db));
    sqlite3_finalize(st); OPENSSL_cleanse(key, sizeof(key)); gp_members_free(members, count); return result;
}

int group_policy_seal_revocations(struct UTUN_INSTANCE* inst, const char* channel) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "message epoch seal missing instance"); return -1; }
    struct member_sync_write_context context = {.self=inst->node_id, .now=ntp_time_get_us(inst)};
    memcpy(context.public_key, inst->my_ed25519_pubkey, 32); memcpy(context.private_key, inst->my_ed25519_privkey, 32);
    int rc = group_policy_seal_revocations_write(inst->topo_sqlite_db, &context, channel);
    OPENSSL_cleanse(&context, sizeof(context)); return rc;
}

struct group_policy_update {
    char channel[65];
    struct group_policy policy;
    struct gp_member* members;
    struct group_policy_cache* cache;
    size_t count;
    int changes;
    int archived;
};

void group_policy_discard(struct group_policy_update* update) {
    if (update) { gp_members_free(update->members, update->count); group_policy_snapshot_free(update->cache); u_free(update); }
}

struct group_policy_cache* group_policy_update_take_snapshot(struct group_policy_update* update) {
    struct group_policy_cache* cache = update ? update->cache : NULL;
    if (update) update->cache = NULL;
    return cache;
}

/* Проекция допуска сохраняется той же транзакцией, что и породившие её удостоверения. */
int group_policy_project(sqlite3* db, uint64_t self, const char* channel, struct group_policy_update** out) {
    if (out) *out = NULL;
    if (!db || !out || !chat_channel_valid(channel) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group admission project requires channel and caller transaction"); return -SQLITE_MISUSE;
    }
    struct group_policy_update* update = u_calloc(1, sizeof(*update));
    if (!update) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group admission allocation ch=%s", channel); return -SQLITE_NOMEM; }
    strcpy(update->channel, channel);
    sqlite3_stmt* st = NULL; char sql[384]; uint8_t owner[32]; int code = SQLITE_ERROR;
    const char* stage = "policy/member evaluation";
    if (group_policy_get(db, channel, &update->policy) ||
        topo_node_sqlite_channel_get(db, channel, NULL, 0, owner, NULL, NULL, NULL) ||
        gp_load(db, channel, &update->members, &update->count, 1) ||
        gp_evaluate(update->members, update->count, 1, NULL)) goto error;
    struct gp_member* m = update->members;
    for (size_t i = 0; i < update->count; i++) {
        m[i].verified = m[i].allowed;
        if (m[i].deleted) m[i].valid = 0;
    }
    if (gp_evaluate(m, update->count, update->policy.all_members, owner)) goto error;
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=?1,identity_verified=?3,signed_by=?4,signature=?5,join_ts=?6"
        " WHERE id=?2 AND record_state=0", channel);
    stage = "prepare trust update"; code = sqlite3_prepare_v2(db, sql, -1, &st, NULL); if (code != SQLITE_OK) goto error;
    time_t created = time(NULL);
    for (size_t i = 0; i < update->count; i++) {
        const struct gp_invitation* edge = m[i].winner;
        const uint8_t empty[64] = {0};
        const uint8_t* signature = edge ? edge->body + 33 : empty;
        const uint8_t* issuer = edge && edge->member_signer ? edge->body + 1 : empty;
        uint64_t stamp = edge ? edge->stamp : 0;
        if (m[i].previous == m[i].allowed && m[i].previous_verified == m[i].verified && m[i].join_ts == stamp &&
            !memcmp(m[i].stored_signature, signature, 64) && !memcmp(m[i].signer, issuer, 32)) continue;
        stage = "bind trust allowed"; code = sqlite3_bind_int(st, 1, m[i].allowed); if (code != SQLITE_OK) goto error;
        stage = "bind trust member"; code = sqlite3_bind_int64(st, 2, m[i].row); if (code != SQLITE_OK) goto error;
        stage = "bind trust verified"; code = sqlite3_bind_int(st, 3, m[i].verified); if (code != SQLITE_OK) goto error;
        stage = "bind invitation projection";
        code = sqlite3_bind_blob(st, 4, member_sync_signer(issuer), 32, SQLITE_STATIC); if (code != SQLITE_OK) goto error;
        code = sqlite3_bind_blob(st, 5, edge ? signature : NULL, 64, SQLITE_STATIC); if (code != SQLITE_OK) goto error;
        code = sqlite3_bind_int64(st, 6, (sqlite3_int64)stamp); if (code != SQLITE_OK) goto error;
        stage = "step trust update"; code = sqlite3_step(st); if (code != SQLITE_DONE) goto error;
        stage = "trust update count"; if (sqlite3_changes(db) != 1) { code = SQLITE_CONSTRAINT; goto error; }
        stage = "reset trust update"; code = sqlite3_reset(st); if (code != SQLITE_OK) goto error;
        if (m[i].allowed && !m[i].previous) {
            stage = "admitted node directory";
            code = topo_node_sqlite_node_write(db, m[i].node, m[i].name, m[i].x, m[i].ed,
                m[i].profile_version ? m[i].profile_version : stamp, created);
            if (!code) code = topo_node_sqlite_nodeinfo_write(db, m[i].x);
            if (code) goto error;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "member trust ch=%s node=%016llx verified=%u admitted=%u",
            channel, (unsigned long long)m[i].node, m[i].verified, m[i].allowed);
        update->changes++;
    }
    stage = "finalize trust update"; code = sqlite3_finalize(st); st = NULL; if (code != SQLITE_OK) goto error;
    update->archived = group_protocol_archive(db, self, channel);
    if (update->archived < 0) { code = -update->archived; stage = "history archive"; goto error; }
    if (update->archived && !update->changes) update->changes = 1;
    /* Один конструктор для READ и WRITE: снимок отражает эту транзакцию и готов до commit. */
    stage = "prepare access snapshot"; code = group_policy_snapshot_read(db, channel, &update->cache);
    if (code != SQLITE_OK) goto error;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "group admission staged ch=%s members=%zu changes=%d archived=%d; commit pending",
        channel, update->count, update->changes, update->archived);
    *out = update; return 0;
error:
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) code = SQLITE_ERROR;
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group admission projection failed ch=%s stage=%s rc=%d: %s", channel, stage, code,
        (sqlite3_errcode(db) & 255) == (code & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(code));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group admission cleanup ch=%s rc=%d", channel, final);
    group_policy_discard(update); return -code;
}

static int gp_publication_current(struct UTUN_INSTANCE* inst, const char* channel, uint64_t generation, uint64_t registration) {
    if (inst->destroying || generation != inst->group_generation) return 0;
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel);
    return group ? !group->stopping && group->generation == registration : !registration;
}

void group_policy_publish_changes(struct UTUN_INSTANCE* inst, struct group_policy_update* update, int notify_sync) {
    if (!update) return;
    if (!inst || !update->cache) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member snapshot publish: missing instance/prepared snapshot");
        group_policy_discard(update); return;
    }
    const char* channel = update->channel;
    struct TOPO_GROUP* initial = topo_groups_find_channel(inst->topo_groups, channel);
    uint64_t generation = inst->group_generation, registration = initial ? initial->generation : 0;
    const struct group_policy_cache* previous = gp_cache(inst, channel);
    int admin_changed = memcmp(previous ? previous->admin_root : (const uint8_t[MT_HASH_SIZE]){0},
        update->cache->admin_root, MT_HASH_SIZE) != 0;
    group_policy_snapshot_publish(inst, update->cache); update->cache = NULL;
    struct gp_member* m = update->members; size_t count = update->count;
    int proof_changed = 0;
    for (size_t i = 0; i < count; i++) proof_changed |= m[i].previous_verified != m[i].verified;
    if (admin_changed || proof_changed) chat_sync_admission_changed(inst, channel, admin_changed);
    struct group_policy policy = update->policy;
    if (update->archived) group_protocol_archived(inst, channel);
    if (!gp_publication_current(inst, channel, generation, registration)) goto done;
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel);
    if (group) topo_group_revalidate_members(group);
    if (!gp_publication_current(inst, channel, generation, registration)) goto done;
    merkle_sync_revalidate_peers(inst, MT_DATA_MEMBERS, channel);
    merkle_sync_revalidate_peers(inst, MT_DATA_MEMBER_ADMIN, channel);
    size_t self = gp_find_ed(m, count, inst->my_ed25519_pubkey);
    int restored = self < count && !m[self].previous && m[self].allowed;
    for (size_t i = 0; i < count; i++) if (restored || m[i].previous != m[i].allowed) {
        if (!gp_publication_current(inst, channel, generation, registration)) goto done;
        if (m[i].previous != m[i].allowed) {
            DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "group admission ch=%s node=%016llx signer=%s allowed=%u all=%u version=%llu",
                channel, (unsigned long long)m[i].node, sc_public_key_hex(m[i].signer, (char[65]){0}), m[i].allowed, policy.all_members,
                (unsigned long long)policy.version);
            member_sync_notify_props(inst, channel, m[i].ed);
        }
        if (!gp_publication_current(inst, channel, generation, registration)) goto done;
        struct TOPO_GROUP* g = topo_groups_find_channel(inst->topo_groups, channel);
        struct ETCP_CONN* conn = instance_find_conn_by_key(inst, m[i].x);
        if (g && conn && m[i].allowed && conn->links_up && !conn->close_requested) {
            topo_group_connect_admitted(g, conn);
            if (topo_group_new_conn(g, conn) < 0)
                DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "group JOIN after admission failed ch=%s peer=%016llx restored=%d",
                    channel, (unsigned long long)m[i].node, restored);
            if (!gp_publication_current(inst, channel, generation, registration)) goto done;
            /* Recovery завершён: отдельный обычный start подтверждает состав и ожидающему соседу. */
            if (topo_group_chat_connection_allowed(g, conn) &&
                merkle_sync_start(inst, MT_DATA_MEMBERS, conn, channel, NULL, NULL) < 0)
                DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member exchange after admission unavailable ch=%s peer=%016llx",
                    channel, (unsigned long long)m[i].node);
        }
    }
    if (!gp_publication_current(inst, channel, generation, registration)) goto done;
    if (update->changes) {
        if (notify_sync) {
            merkle_sync_changed(inst, MT_DATA_MEMBERS, channel);
            if (!gp_publication_current(inst, channel, generation, registration)) goto done;
            group_protocol_members_changed(inst, channel);
        }
        if (!gp_publication_current(inst, channel, generation, registration)) goto done;
        uint8_t evt[65]; evt[0] = (uint8_t)strlen(channel); memcpy(evt + 1, channel, evt[0]);
        chat_event_post(inst, CHAT_EVT_MEMBERS_CHANGED, evt, evt[0] + 1);
    }
    if (self < count && m[self].allowed && !m[self].previous && gp_publication_current(inst, channel, generation, registration))
        member_sync_audit_invitation(inst, channel, NULL);
done:
    group_policy_discard(update);
}

void group_policy_publish(struct UTUN_INSTANCE* inst, struct group_policy_update* update) {
    group_policy_publish_changes(inst, update, 1);
}

int group_policy_refresh(struct UTUN_INSTANCE* inst, const char* channel) {
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    if (!db || db_async_can_write(inst->db_worker) || !chat_channel_valid(channel) || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group admission refresh requires database outside transaction"); return -1;
    }
    struct group_policy_update* update = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE /* admission */", NULL, NULL, NULL) != SQLITE_OK ||
        group_policy_project(db, inst->node_id, channel, &update) || sqlite3_exec(db, "COMMIT /* admission */", NULL, NULL, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group admission transaction ch=%s: %s", channel, sqlite3_errmsg(db));
        if (!sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group admission rollback failed ch=%s", channel); utun_instance_stop(inst);
        }
        group_policy_discard(update); return -1;
    }
    group_policy_publish(inst, update); return 0;
}

static int gp_can_invite_ram(sqlite3* db, const char* channel, const uint8_t inviter[32]) {
    struct group_policy policy; struct gp_member* m = NULL; size_t count = 0; uint8_t owner[32];
    /* Проверяем подписи и всю цепочку заново: обмен профилями не блокирует действующего приглашающего. */
    if (!db || !chat_channel_valid(channel) || group_policy_get(db, channel, &policy) ||
        topo_node_sqlite_channel_get(db, channel, NULL, 0, owner, NULL, NULL, NULL) || gp_load(db, channel, &m, &count, 0)) return -1;
    int rc = gp_evaluate(m, count, policy.all_members, owner);
    size_t i = gp_find_ed(m, count, inviter);
    int allowed = !rc && i < count && m[i].allowed && (policy.all_members || !memcmp(m[i].ed, owner, 32) || m[i].admin);
    if (!allowed) DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "invitation denied ch=%s inviter=%s mode=%u version=%llu",
        channel, sc_public_key_hex(inviter, (char[65]){0}), policy.all_members, (unsigned long long)policy.version);
    gp_members_free(m, count); return rc ? -1 : allowed;
}

static int gp_invitation_allowed(const struct group_policy_cache* cache, const uint8_t ed[32]) {
    uint8_t x[32]; if (sc_identity_public_x("invitation rights", ed, x) != SC_OK) return 0;
    const struct gp_access_member* member = gp_access_lookup_key(cache, x, ed);
    return member && member->admitted && member->verified && member->can_invite;
}

int group_policy_can_invite(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t inviter[32]) {
    if (!inst || !chat_channel_valid(channel) || !inviter) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invitation capability: invalid instance/channel/member"); return -1;
    }
    const struct group_policy_cache* cache = gp_cache(inst, channel);
    if (cache) return gp_invitation_allowed(cache, inviter);
    sqlite3* ram = gp_ram_database(inst);
    if (ram) return gp_can_invite_ram(ram, channel, inviter);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "invitation capability unavailable ch=%s inviter=%s: no committed snapshot", channel,
        sc_public_key_hex(inviter, (char[65]){0}));
    return 0;
}

int group_policy_can_invite_read(sqlite3* db, const char* channel, const uint8_t inviter[32]) {
    if (!db || !chat_channel_valid(channel) || !inviter) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invitation SQL capability: invalid database/channel/member"); return -SQLITE_MISUSE;
    }
    struct group_policy_cache* cache = NULL; int rc = group_policy_snapshot_read(db, channel, &cache);
    if (rc) return -rc;
    int allowed = gp_invitation_allowed(cache, inviter);
    group_policy_snapshot_free(cache); return allowed;
}

/* Выбор передаётся полными публичными Ed25519 в HEX; поиск и защита от повторов по 32 байтам. */
static int gp_select(sqlite3* db, struct gp_member* m, size_t count, const char* json) {
    sqlite3_stmt* st = NULL;
    const char* stage = "selection type prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT CASE WHEN json_valid(?1) THEN json_type(?1) END", -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "selection type bind"; rc = sqlite3_bind_text(st, 1, json ? json : "[]", -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "selection type step"; rc = sqlite3_step(st); }
    if (rc == SQLITE_DONE) rc = SQLITE_ERROR;
    if (rc != SQLITE_ROW) goto done;
    stage = "selection must be array";
    if (!sqlite3_column_text(st, 0) || strcmp((const char*)sqlite3_column_text(st, 0), "array")) {
        rc = SQLITE_MISMATCH; goto done;
    }
    stage = "selection type finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto done;
    stage = "selection rows";
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(db, "SELECT type,value FROM json_each(?1)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, json ? json : "[]", -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto done;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* type = (const char*)sqlite3_column_text(st, 0); const char* text = (const char*)sqlite3_column_text(st, 1);
        int bytes = sqlite3_column_bytes(st, 1);
        uint8_t ed[32];
        if (!type || strcmp(type, "text") || !text || bytes != 64 || sc_hex_to_binary(text, ed, sizeof(ed))) {
            rc = SQLITE_MISMATCH; break;
        }
        size_t i = gp_find_ed(m, count, ed);
        if (i == count || !m[i].valid || !m[i].previous || m[i].keep) { rc = SQLITE_CONSTRAINT; break; }
        m[i].keep = 1;
    }
done:
    { int final = sqlite3_finalize(st); if (rc == SQLITE_DONE) { rc = final; stage = "selection rows finalize"; } }
    if (rc != SQLITE_OK) DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "group policy preservation failed stage=%s rc=%d (%s): %s",
        stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    return -rc;
}

/* Расчёт не содержит runtime/SQLite указателей; members принадлежат вызывающему до u_free. */
struct gp_preview {
    struct group_policy policy;
    struct gp_member* members;
    size_t count;
    int can_edit;
    char token[65];
};

static int gp_preview_read(sqlite3* db, const char* channel, int all, const char* keep, struct gp_preview* preview) {
    if (!db || !chat_channel_valid(channel) || (all != 0 && all != 1)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy preview: invalid database/channel/mode"); return SQLITE_MISUSE;
    }
    int rc = group_policy_get(db, channel, &preview->policy);
    if (rc) return -rc;
    sqlite3_stmt* st = NULL; uint8_t owner[32] = {0}; const char* stage = "channel metadata";
    rc = sqlite3_prepare_v2(db, "SELECT owner_node_id,CASE WHEN ed25519_privkey IS NULL THEN 0"
        " WHEN typeof(ed25519_privkey)='blob' AND length(ed25519_privkey)=32 THEN 1 ELSE -1 END"
        " FROM channels WHERE channel_id=?1", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    else if (rc == SQLITE_ROW) {
        rc = topo_node_sqlite_read_optional_key(st, 0, owner);
        if (!rc) {
            int key = sqlite3_column_int(st, 1);
            if (key < 0) DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "group policy malformed private key disables editing ch=%s", channel);
            preview->can_edit = key == 1; rc = SQLITE_OK;
        }
    }
    { int final = sqlite3_finalize(st); if (rc == SQLITE_OK) { rc = final; stage = "channel metadata finalize"; } }
    if (rc != SQLITE_OK) goto done;
    uint8_t hash[32]; char ns[70]; snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_MEMBERS, channel);
    stage = "Merkle root"; rc = -merkle_tree_get(db, ns, 0, 0, hash); if (rc != SQLITE_OK) goto done;
    for (int i = 0; i < 32; i++) snprintf(preview->token + i * 2, 3, "%02x", hash[i]);
    stage = "signed members"; rc = -gp_load(db, channel, &preview->members, &preview->count, 0); if (rc != SQLITE_OK) goto done;
    stage = "selection"; rc = -gp_select(db, preview->members, preview->count, keep); if (rc != SQLITE_OK) goto done;
    stage = "admission traversal";
    if (gp_evaluate(preview->members, preview->count, all, owner)) rc = SQLITE_NOMEM;
done:
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy preview failed ch=%s stage=%s count=%zu rc=%d (%s): %s",
        channel, stage, preview->count, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    return rc;
}

/* Один формат ответа для предварительного READ и legacy-транзакции; память резервируется до commit. */
static int gp_response(const struct gp_preview* preview, int applied, int all, struct strbuf* json) {
    if (strbuf_addf(json, "{\"ok\":true,\"applied\":%s,\"all_members\":%s,\"can_edit\":%s,\"token\":\"%s\",\"affected\":[",
        applied ? "true" : "false", (applied ? all : preview->policy.all_members) ? "true" : "false",
        preview->can_edit ? "true" : "false", preview->token) < 0) goto fail;
    int separator = 0;
    for (size_t i = 0; i < preview->count; i++) {
        const struct gp_member* member = &preview->members[i];
        if (!member->previous || member->allowed) continue;
        if (strbuf_addf(json, "%s{\"node\":\"%s\",\"inviter\":\"%s\",\"name\":", separator++ ? "," : "",
            sc_public_key_hex(member->ed, (char[65]){0}), sc_public_key_hex(member->signer, (char[65]){0})) < 0 ||
            strbuf_add_json_string(json, member->name) < 0 || strbuf_addf(json, "}") < 0) goto fail;
    }
    if (strbuf_addf(json, "]}") >= 0) return SQLITE_OK;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy response allocation failed count=%zu applied=%d", preview->count, applied);
    return SQLITE_NOMEM;
}

int group_policy_preview_read(sqlite3* db, const char* channel, int all, const char* keep, char** output) {
    if (!output) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy preview: missing output"); return SQLITE_MISUSE; }
    *output = NULL; struct gp_preview preview = {0}; struct strbuf json = strbuf_new();
    int rc = gp_preview_read(db, channel, all, keep, &preview);
    if (rc == SQLITE_OK) rc = gp_response(&preview, 0, all, &json);
    if (rc == SQLITE_OK) {
        *output = strbuf_detach(&json);
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "group policy preview complete ch=%s all=%d count=%zu bytes=%zu",
            channel, all, preview.count, strlen(*output));
    }
    gp_members_free(preview.members, preview.count); strbuf_free(&json); return rc;
}

int group_policy_command_write(sqlite3* db, struct group_policy_mutation* query) {
    if (!db || !query || !query->context || !query->token || strlen(query->token) != 64 ||
        !chat_channel_valid(query->channel) || (query->all_members != 0 && query->all_members != 1) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy WRITE requires frozen inputs and caller transaction"); return SQLITE_MISUSE;
    }
    const char* channel = query->channel; uint8_t key[32] = {0}; struct gp_preview preview = {0};
    struct strbuf json = strbuf_new(); const char* error = "unavailable"; query->status = -1;
    int code = gp_preview_read(db, channel, query->all_members, query->keep, &preview);
    if (code == SQLITE_MISMATCH || code == SQLITE_CONSTRAINT) { error = "invalid_selection"; code = SQLITE_OK; goto done; }
    if (code) goto done;
    if (!preview.can_edit) { error = "forbidden"; goto done; }
    if (strcmp(query->token, preview.token)) { error = "conflict"; query->status = -2; goto done; }
    if (preview.policy.version == INT64_MAX) { error = "version_exhausted"; goto done; }
    code = gp_response(&preview, 1, query->all_members, &json); if (code) goto done;
    code = gp_key_read(db, channel, 1, key); if (code) goto done;
    for (size_t i = 0; i < preview.count; i++) {
        struct gp_member* member = &preview.members[i]; if (!member->keep || !member_sync_signer(member->signer)) continue;
        uint8_t signature[64];
        if (member_sync_sign_join(key, channel, NULL, member->x, member->ed, member->join_id, member->join_ts, signature)) { code = SQLITE_ERROR; goto done; }
        struct ms_member_rec record = {.node_id=member->node, .x25519=member->x, .ed25519=member->ed, .signature=signature};
        int changed = member_sync_record_write(db, query->context, channel, NULL, &record);
        if (changed < 0) { code = query->context->sqlite_code ? query->context->sqlite_code : SQLITE_ERROR; goto done; }
    }
    struct group_policy policy = preview.policy;
    uint64_t version = query->context->now && query->context->now <= INT64_MAX ? query->context->now : 1;
    policy.version = version > policy.version ? version : policy.version + 1; policy.all_members = query->all_members;
    uint8_t body[40]; int bytes = gp_message(channel, &policy, body);
    if (bytes < 0 || sc_ed25519_sign(key, body, (size_t)bytes, policy.signature) != SC_OK) { code = SQLITE_ERROR; goto done; }
    int applied = group_policy_apply(db, channel, &policy); if (applied < 0) { code = -applied; goto done; }
    if (group_policy_seal_revocations_write(db, query->context, channel)) {
        code = query->context->sqlite_code ? query->context->sqlite_code : SQLITE_ERROR; goto done;
    }
    int indexed = member_sync_reindex(db, channel, 0); if (indexed < 0) { code = -indexed; goto done; }
    int projected = group_policy_project(db, query->context->self, channel, &query->admission);
    if (projected) { code = -projected; goto done; }
    query->version = policy.version; query->status = 0; query->json = strbuf_detach(&json);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "group policy staged ch=%s all=%d version=%llu; commit pending", channel,
        query->all_members, (unsigned long long)policy.version);
done:
    if (query->status && !code) {
        char response[96]; snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", error);
        query->json = u_strdup(response); if (!query->json) code = SQLITE_NOMEM;
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "group policy command denied ch=%s reason=%s", channel, error);
    }
    if (code) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy WRITE failed ch=%s rc=%d: %s", channel, code, sqlite3_errstr(code));
    OPENSSL_cleanse(key, sizeof(key)); gp_members_free(preview.members, preview.count); strbuf_free(&json); return code;
}

int group_policy_command(struct UTUN_INSTANCE* inst, const char* channel, int all, const char* token, const char* keep, char** output) {
    if (output) *output = NULL;
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL; uint8_t key[32] = {0}; struct gp_preview preview = {0};
    struct strbuf json = strbuf_new(); int result = -1, transaction = 0;
    struct group_policy_update* admission = NULL;
    const char* error = "unavailable";
    if (!output || !db || db_async_can_write(inst->db_worker) || !chat_channel_valid(channel) ||
        (all != 0 && all != 1) || !sqlite3_get_autocommit(db)) goto done;
    int rc = gp_preview_read(db, channel, all, keep, &preview);
    if (rc != SQLITE_OK) {
        if (rc == SQLITE_MISMATCH || rc == SQLITE_CONSTRAINT) error = "invalid_selection";
        goto done;
    }
    struct gp_member* m = preview.members; size_t count = preview.count; struct group_policy policy = preview.policy;
    /* Ответ выделяем до записи: ошибка памяти не должна выдавать отказ уже выполненной операции. */
    if (gp_response(&preview, token != NULL, all, &json) != SQLITE_OK) goto done;
    if (token) {
        if (!preview.can_edit) { error = "forbidden"; goto done; }
        if (strcmp(token, preview.token)) { error = "conflict"; result = -2; goto done; }
        if (topo_node_sqlite_channel_get_priv(db, channel, key)) goto done;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE /* group policy */", NULL, NULL, NULL) != SQLITE_OK) goto done;
        transaction = 1;
        for (size_t i = 0; i < count; i++) if (m[i].keep && member_sync_signer(m[i].signer)) {
            uint8_t signature[64];
            if (member_sync_sign_join(key, channel, NULL, m[i].x, m[i].ed, m[i].join_id, m[i].join_ts, signature) ||
                member_sync_put(inst, channel, m[i].x, m[i].ed, NULL, 0, NULL, 0, NULL, NULL, NULL, 0, 0, signature) < 0) goto done;
        }
        if (policy.version == INT64_MAX) { error = "version_exhausted"; goto done; }
        int64_t now = ntp_time_get_us(inst); uint64_t version = now > 0 ? (uint64_t)now : 1;
        policy.version = version > policy.version ? version : policy.version + 1; policy.all_members = all;
        uint8_t body[40]; int bytes = gp_message(channel, &policy, body);
        if (bytes < 0 || sc_ed25519_sign(key, body, bytes, policy.signature) != SC_OK ||
            group_policy_apply(db, channel, &policy) < 0 || group_policy_seal_revocations(inst, channel)
            || member_sync_reindex(db, channel, NULL) < 0 || group_policy_project(db, inst->node_id, channel, &admission) ||
            sqlite3_exec(db, "COMMIT /* group policy */", NULL, NULL, NULL) != SQLITE_OK) goto done;
        transaction = 0;
        merkle_sync_changed(inst, MT_DATA_MEMBERS, channel);
        group_policy_publish(inst, admission); admission = NULL;
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "group policy committed ch=%s all=%d version=%llu", channel, all, (unsigned long long)policy.version);
    }
    *output = strbuf_detach(&json); result = 0;
done:
    group_policy_discard(admission);
    OPENSSL_cleanse(key, sizeof(key)); gp_members_free(preview.members, preview.count); strbuf_free(&json);
    if (transaction && sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group policy rollback failed ch=%s", channel); utun_instance_stop(inst);
    }
    if (result < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "group policy command failed ch=%s reason=%s", channel ? channel : "-", error);
        if (output) { char text[96]; snprintf(text, sizeof(text), "{\"ok\":false,\"error\":\"%s\"}", error); *output = u_strdup(text); }
    }
    return result;
}
