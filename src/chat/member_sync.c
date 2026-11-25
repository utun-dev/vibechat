#include "../routing_layer/topo_group_connect.h"
#include "member_sync.h"
#include "../db_changes.h"
#include "chat_log.h"
#include "db_sync.h"
#include "chat_feedback.h"
#include "group_policy.h"
#include "group_info.h"
#include "../group/group_protocol.h"

#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/devgroup.h"
#include "../radio/radio.h"
#include "chat_core.h"
#include "chat_channel.h"
#include "chat_sync.h"
#include "chat_core_priv.h"
#include "../utun_instance.h"
#include "../db_schema.h"
#include "../config_updater.h"
#include "../transport_layer/etcp.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/db_async.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include "../../lib/json_flat.h"

#define MS_ID "member_sync"

/* ── DB access ── */

static sqlite3* _db(struct UTUN_INSTANCE* inst) {
    return inst ? inst->topo_sqlite_db : NULL;
}

static int member_registration_current(struct UTUN_INSTANCE* inst, const char* channel, uint64_t registration) {
    if (!inst->topo_groups) return !registration; /* SQL-fixture без транспорта. */
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel);
    return group && !group->stopping && group->generation == registration;
}

static void _peers_table(const char* ch_id, char* buf, size_t sz) {
    size_t i = 0;
    while (*ch_id && i < sz - 1) {
        char c = *ch_id++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            buf[i++] = c; else buf[i++] = '_';
    }
    buf[i] = '\0';
    char tbl[128]; snprintf(tbl, sizeof(tbl), "peers_%s", buf);
    snprintf(buf, sz, "%s", tbl);
}

/* Канонические целые для Merkle и wire; формат подписываемых сообщений задаётся отдельно. */
static void member_write64(uint8_t* p, uint64_t n) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)n; n >>= 8; }
}

static uint64_t member_read64(const uint8_t* p) {
    uint64_t n = 0;
    for (int i = 0; i < 8; i++) n = (n << 8) | p[i];
    return n;
}

static int member_owner_epoch_field(const char* key, const char* text, void* arg) {
    if (strcmp(key, GROUP_MESSAGE_EPOCH_TAG)) return 0;
    uint64_t value = 0;
    if (!text[0] || (text[0] == '0' && text[1])) goto invalid;
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9' || value > ((uint64_t)INT64_MAX - (*p - '0')) / 10) goto invalid;
        value = value * 10 + (*p - '0');
    }
    *(uint64_t*)arg = value; return 0;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member owner: invalid message epoch format/range"); return -1;
}

int member_sync_owner_epoch(const char* tags, uint64_t* epoch) {
    uint64_t value = 0;
    if (!epoch) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member owner: missing epoch output"); return -1; }
    if (tags && tags[0] && json_flat_parse(tags, member_owner_epoch_field, &value)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member owner: message epoch unavailable"); return -1;
    }
    *epoch = value; return 0;
}

/* Версия owner-блока — каноническая десятичная строка 1..INT_MAX. */
struct member_owner_fields { uint64_t epoch; int version, deleted, storage; };
static int member_owner_field(const char* key, const char* text, void* arg) {
    struct member_owner_fields* fields = arg;
    if (!strcmp(key, "deleted")) fields->deleted = !strcmp(text, "yes");
    if (!strcmp(key, "storage")) fields->storage = !strcmp(text, "yes");
    if (strcmp(key, "ver")) return member_owner_epoch_field(key, text, &fields->epoch);
    int value = 0;
    if (*text < '1' || *text > '9') goto invalid;
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9' || value > (INT_MAX - (*p - '0')) / 10) goto invalid;
        value = value * 10 + (*p - '0');
    }
    fields->version = value; return 0;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member owner: invalid version format/range"); return -1;
}

static int member_owner_read(sqlite3* db, const char* tags, struct member_owner_fields* fields) {
    if (!tags || !tags[0]) return SQLITE_OK;
    int code = chat_json_strings_read(db, tags, MS_ADM_TAGS_MAX, member_owner_field, fields);
    if (!code && !fields->version) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member owner: missing version"); return SQLITE_CONSTRAINT;
    }
    return code;
}

int member_sync_owner_version(sqlite3* db, const char* tags) {
    struct member_owner_fields fields = {0};
    return member_owner_read(db, tags, &fields) ? -1 : fields.version;
}

/* Порядок owner-блоков определяется только их независимо подписанными полями. */
static int member_owner_order(const char* left, const struct member_owner_fields* a, const uint8_t* a_sig,
                             const char* right, const struct member_owner_fields* b, const uint8_t* b_sig) {
    if (a->epoch != b->epoch) return a->epoch > b->epoch ? 1 : -1;
    if (a->version != b->version) return a->version > b->version ? 1 : -1;
    if (a->deleted != b->deleted) return a->deleted ? 1 : -1;
    int bytes = a->version ? memcmp(b_sig, a_sig, 64) : 0;
    if (!bytes) bytes = strcmp(right ? right : "", left ? left : ""); return bytes > 0 ? 1 : bytes < 0 ? -1 : 0;
}

int member_sync_owner_compare(sqlite3* db, const char* left, const uint8_t* a_sig, const char* right, const uint8_t* b_sig) {
    struct member_owner_fields a = {0}, b = {0};
    if (member_owner_read(db, left, &a) || member_owner_read(db, right, &b)) return -2;
    return member_owner_order(left, &a, a_sig, right, &b, b_sig);
}

int member_sync_build_owner_msg(const char* channel, const uint8_t ed[32], const char* tags, uint8_t* out, size_t capacity) {
    size_t len = tags ? strnlen(tags, MS_ADM_TAGS_MAX + 1) : 0;
    if (!chat_channel_valid(channel) || !ed || !out || !len || len > MS_ADM_TAGS_MAX || capacity < MS_OWNER_PREFIX_SIZE + len) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: invalid owner message group=%s bytes=%zu capacity=%zu",
            MS_ID, channel ? channel : "-", len, capacity);
        return -1;
    }
    size_t n = sizeof(MS_OWNER_DOMAIN) - 1;
    memcpy(out, MS_OWNER_DOMAIN, n);
    memcpy(out + n, ed, 32);
    memcpy(out + MS_OWNER_PREFIX_SIZE, tags, len);
    return (int)(MS_OWNER_PREFIX_SIZE + len);
}

const uint8_t* member_sync_signer(const uint8_t key[32]) {
    return key && memcmp(key, (const uint8_t[32]){0}, 32) ? key : NULL;
}

/* В обоих запросах профиля поля идут подряд: update_sig, update_ts, userinfo. */
static int member_profile_columns(sqlite3_stmt* st, int col) {
    int sig = sqlite3_column_type(st, col), ts = sqlite3_column_type(st, col + 1);
    return (sig == SQLITE_NULL || sig == SQLITE_BLOB) && (ts == SQLITE_NULL || ts == SQLITE_INTEGER)
        && sqlite3_column_type(st, col + 2) == SQLITE_TEXT;
}

int member_sync_build_update_msg(const uint8_t* join_id, uint64_t update_ts,
                                  const char* userinfo,
                                  uint8_t* out, int out_sz) {
    if (!join_id || !out || out_sz < 0) return -1;
    int off = 0;

    if (off + 32 > out_sz) return -1;
    memcpy(out + off, join_id, 32); off += 32;

    if (off + 8 > out_sz) return -1;
    member_write64(out + off, update_ts); off += 8;

    size_t ul = userinfo ? strlen(userinfo) : 0;
    if (off + (int)ul + 1 > out_sz) return -1;
    memcpy(out + off, userinfo ? userinfo : "", ul + 1); off += (int)ul + 1;

    return off;
}

int member_sync_build_join_msg(const uint8_t* ch_x25519, const uint8_t* ch_ed25519,
                               const uint8_t* node_ed25519, const uint8_t* node_x25519,
                               uint8_t* out, int out_sz) {
    if (!ch_x25519 || !ch_ed25519 || !node_ed25519 || !node_x25519 || !out || out_sz < MEMBER_JOIN_MSG_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invalid member identity message buffers"); return -1;
    }
    memcpy(out, "utunJOIN", 8);
    memcpy(out + 8, ch_x25519, 32);
    memcpy(out + 40, ch_ed25519, 32);
    memcpy(out + 72, node_ed25519, 32);
    memcpy(out + 104, node_x25519, 32);
    return MEMBER_JOIN_MSG_SIZE;
}

int member_sync_join_id(const uint8_t body[MEMBER_JOIN_MSG_SIZE], uint8_t id[32]) {
    if (body && id && EVP_Digest(body, MEMBER_JOIN_MSG_SIZE, id, NULL, EVP_sha256(), NULL)) return SQLITE_OK;
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN digest unavailable"); return SQLITE_ERROR;
}

int member_sync_join_id_read(sqlite3* db, const char* channel, const uint8_t node_ed[32], const uint8_t x[32], uint8_t id[32]) {
    uint8_t gx[32], ed[32], body[MEMBER_JOIN_MSG_SIZE]; int found;
    int rc = member_sync_channel_keys_read(db, channel, gx, ed, &found);
    if (rc) return rc;
    if (!found) { DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "JOIN digest missing group keys ch=%s", channel); return SQLITE_NOTFOUND; }
    if (member_sync_build_join_msg(gx, ed, node_ed, x, body, sizeof(body)) < 0) return SQLITE_MISMATCH;
    return member_sync_join_id(body, id);
}

/* ──  merkle_sync_data_ops implementation ── */

/* Строгие BLOB без преобразования типа: пустой optional означает нулевой блок подписи. */
static int member_record_blob(sqlite3_stmt* st, int col, int bytes, int optional, const uint8_t** out) {
    *out = NULL; int type = sqlite3_column_type(st, col);
    if (optional && type == SQLITE_NULL) return SQLITE_OK;
    if (type != SQLITE_BLOB) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS record column=%d type=%d expected=BLOB optional=%d", col, type, optional);
        return SQLITE_CORRUPT;
    }
    int n = sqlite3_column_bytes(st, col);
    if (optional && !n) return SQLITE_OK;
    if (n != bytes) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS record BLOB column=%d bytes=%d expected=%d", col, n, bytes); return SQLITE_CORRUPT;
    }
    *out = sqlite3_column_blob(st, col);
    if (!*out) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS record BLOB allocation failed column=%d bytes=%d", col, bytes);
    return *out ? SQLITE_OK : SQLITE_NOMEM;
}
/* Тип является первым байтом ключа. Ноль зарезервирован для метаданных группы. */
enum member_item_kind { MEMBER_ITEM_GROUP, MEMBER_ITEM_JOIN, MEMBER_ITEM_OWNER, MEMBER_ITEM_INVITE };
enum { MEMBER_PAGE_HEADER = 3, MEMBER_ITEM_HEADER = 34, MEMBER_PAGE_VERSION = 2 };
enum { MEMBER_JOIN_WIRE_FIXED = 32 + 32 + 64 + 8 + 64 + 2, MEMBER_OWNER_WIRE_FIXED = 32 + 64 + 2,
    MEMBER_INVITE_WIRE_SIZE = 32 + 32 + 8 + 64 };

static int _sig_is_zero64(const uint8_t* sig);

static int member_item_key(enum member_item_kind kind, const uint8_t* data, merkle_key_t* key) {
    uint8_t digest[MT_HASH_SIZE];
    if (!kind) { memset(key, 0, sizeof(*key)); return SQLITE_OK; }
    size_t bytes = kind == MEMBER_ITEM_INVITE ? 64 : 32;
    if (EVP_Digest(data, bytes, digest, NULL, EVP_sha256(), NULL) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS key digest failed kind=%d", kind); return SQLITE_ERROR;
    }
    key->bytes[0] = (uint8_t)kind;
    memcpy(key->bytes + 1, digest, sizeof(key->bytes) - 1);
    return SQLITE_OK;
}

uint64_t member_sync_leaf_prefix(const uint8_t ed[32]) {
    merkle_key_t key;
    return member_item_key(MEMBER_ITEM_JOIN, ed, &key) ? 0 : merkle_sync_level_prefix(member_read64(key.bytes), MT_MAX_LEVEL);
}

/* Хешируется тот же конверт, который получает сетевой читатель. */
static int member_hash_item(EVP_MD_CTX* hash, const uint8_t* key, const uint8_t* wire, size_t bytes) {
    uint8_t header[MEMBER_ITEM_HEADER];
    memcpy(header, key, 32);
    header[32] = (uint8_t)(bytes >> 8); header[33] = (uint8_t)bytes;
    return EVP_DigestUpdate(hash, header, sizeof(header)) == 1 && EVP_DigestUpdate(hash, wire, bytes) == 1;
}

static int member_items_select(sqlite3* db, const char* channel, uint64_t prefix, const merkle_key_t* after, sqlite3_stmt** out) {
    merkle_key_t low = {{0}}, high;
    memset(&high, 0xff, sizeof(high));
    member_write64(low.bytes, prefix);
    member_write64(high.bytes, prefix | ~merkle_sync_level_prefix(UINT64_MAX, MT_MAX_LEVEL));
    const char* sql = after
        ? "SELECT key,wire FROM member_sync_items WHERE channel=?1 AND key>?2 AND key<=?3 ORDER BY key"
        : "SELECT key,wire FROM member_sync_items WHERE channel=?1 AND key>=?2 AND key<=?3 ORDER BY key";
    int rc = sqlite3_prepare_v2(db, sql, -1, out, NULL);
    if (!rc) rc = sqlite3_bind_text(*out, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(*out, 2, after ? after->bytes : low.bytes, 32, SQLITE_TRANSIENT);
    if (!rc) rc = sqlite3_bind_blob(*out, 3, high.bytes, 32, SQLITE_TRANSIENT);
    return rc;
}

static int member_item_columns(sqlite3_stmt* st, const uint8_t** key, const uint8_t** wire, size_t* size) {
    int rc = member_record_blob(st, 0, 32, 0, key);
    int bytes = sqlite3_column_bytes(st, 1);
    if (rc) return rc;
    if (sqlite3_column_type(st, 1) != SQLITE_BLOB || bytes <= 0 || bytes > MT_PAGE_SIZE - MEMBER_ITEM_HEADER - MEMBER_PAGE_HEADER)
        return SQLITE_CORRUPT;
    *wire = sqlite3_column_blob(st, 1); *size = (size_t)bytes;
    return *wire ? SQLITE_OK : SQLITE_NOMEM;
}

/* Те же ключи и подписи, что в MEMBERS. В нулевом листе передаём только подписанную политику. */
static size_t member_admin_size(unsigned kind, const uint8_t* wire, size_t bytes) {
    if (kind == MEMBER_ITEM_GROUP && bytes >= GROUP_POLICY_WIRE_SIZE && member_read64(wire)) return GROUP_POLICY_WIRE_SIZE;
    if (kind == MEMBER_ITEM_OWNER) return bytes;
    if (kind == MEMBER_ITEM_INVITE && bytes == MEMBER_INVITE_WIRE_SIZE && !member_sync_signer(wire + 32)) return bytes;
    return 0;
}

static int member_view_hash(sqlite3* db, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash, int admin) {
    if (!db || !chat_channel_valid(ns) || !hash || level != MT_MAX_LEVEL || prefix != merkle_sync_level_prefix(prefix, level)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS invalid leaf request ch=%s level=%u", ns, level); return -SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    int rc = member_items_select(db, ns, prefix, NULL, &st), count = 0;
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t *key, *wire; size_t bytes;
        rc = member_item_columns(st, &key, &wire, &bytes);
        if (rc) break;
        if (admin && !(bytes = member_admin_size(key[0], wire, bytes))) continue;
        if (!member_hash_item(hash, key, wire, bytes)) { rc = SQLITE_ERROR; break; }
        count++;
    }
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS leaf read ch=%s prefix=%016llx rc=%d", ns, (unsigned long long)prefix, rc);
    return rc ? -rc : count;
}

int member_sync_leaf_hash(sqlite3* db, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    return member_view_hash(db, ns, level, prefix, hash, 0);
}

static int _member_update_bucket_hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    return member_sync_leaf_hash(_db(ctx), ns, level, prefix, hash);
}

static int member_admin_hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    return member_view_hash(_db(ctx), ns, level, prefix, hash, 1);
}

struct member_leaf_sql { sqlite3* db; const char* channel; int admin; };
static int member_sql_leaf(void* data, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    struct member_leaf_sql* leaf = data; (void)ns;
    return member_view_hash(leaf->db, leaf->channel, level, prefix, hash, leaf->admin);
}

/* Проекция wire обновляется в той же транзакции, что и подписанные исходные записи. */
static int member_item_write(sqlite3* db, const char* channel, const merkle_key_t* key, const uint8_t* wire, size_t bytes) {
    sqlite3_stmt* st = NULL;
    const char* sql = wire
        ? "INSERT INTO member_sync_items(channel,key,wire) VALUES(?,?,?)"
          " ON CONFLICT(channel,key) DO UPDATE SET wire=excluded.wire WHERE wire<>excluded.wire"
        : "DELETE FROM member_sync_items WHERE channel=? AND key=?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, key->bytes, 32, SQLITE_STATIC);
    if (!rc && wire) rc = sqlite3_bind_blob(st, 3, wire, (int)bytes, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS item write ch=%s key=%s rc=%d", channel,
            sc_public_key_hex(key->bytes, (char[65]){0}), rc); return -rc;
    }
    char ns[70]; snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_MEMBERS, channel);
    struct member_leaf_sql leaf = {.db=db, .channel=channel};
    uint64_t prefix = merkle_sync_level_prefix(member_read64(key->bytes), MT_MAX_LEVEL);
    int result = merkle_tree_update(db, ns, prefix, member_sql_leaf, &leaf);
    if (result < 0 || key->bytes[0] == MEMBER_ITEM_JOIN ||
        (key->bytes[0] == MEMBER_ITEM_INVITE && wire && !member_admin_size(key->bytes[0], wire, bytes))) return result;
    snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_MEMBER_ADMIN, channel);
    leaf.admin = 1;
    int indexed = merkle_tree_update(db, ns, prefix, member_sql_leaf, &leaf);
    return indexed < 0 ? indexed : result;
}

static int member_rebuild_index(sqlite3* db, const char* channel);

static int member_view_page(sqlite3* db, const char* ns, uint64_t prefix, int after_valid, const merkle_key_t* after,
    uint8_t* buf, size_t* len, merkle_key_t* next, int* more, int admin) {
    if (!db || !buf || !len || !after || !next || !more || !chat_channel_valid(ns) || *len < MEMBER_PAGE_HEADER) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS invalid page request ch=%s", ns); return -SQLITE_MISUSE;
    }
    merkle_key_t cursor = *after;
    memset(next, 0, sizeof(*next)); *more = 0;
    sqlite3_stmt* st = NULL;
    int rc = member_items_select(db, ns, prefix, after_valid ? &cursor : NULL, &st);
    size_t off = MEMBER_PAGE_HEADER; unsigned count = 0;
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t *key, *wire; size_t bytes;
        rc = member_item_columns(st, &key, &wire, &bytes);
        if (rc) break;
        if (admin && !(bytes = member_admin_size(key[0], wire, bytes))) continue;
        if (off + MEMBER_ITEM_HEADER + bytes > *len) {
            *more = count != 0; rc = count ? SQLITE_DONE : SQLITE_TOOBIG; break;
        }
        memcpy(buf + off, key, 32); memcpy(next->bytes, key, 32); off += 32;
        buf[off++] = (uint8_t)(bytes >> 8); buf[off++] = (uint8_t)bytes;
        memcpy(buf + off, wire, bytes); off += bytes; count++;
    }
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS page ch=%s prefix=%016llx rc=%d", ns, (unsigned long long)prefix, rc); return -rc;
    }
    buf[0] = MEMBER_PAGE_VERSION; buf[1] = (uint8_t)(count >> 8); buf[2] = (uint8_t)count; *len = off;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS page ch=%s admin=%d prefix=%016llx items=%u bytes=%zu more=%d",
        ns, admin, (unsigned long long)prefix, count, off, *more);
    return 0;
}

static int member_page_read(sqlite3* db, const char* ns, uint64_t prefix, int after_valid, const merkle_key_t* after,
    uint8_t* buf, size_t* len, merkle_key_t* next, int* more) {
    return member_view_page(db, ns, prefix, after_valid, after, buf, len, next, more, 0);
}

static int _member_get_page(void* ctx, const char* ns, uint64_t prefix, int after_valid, const merkle_key_t* after,
    uint8_t* buf, size_t* len, merkle_key_t* next, int* more) {
    return member_page_read(_db(ctx), ns, prefix, after_valid, after, buf, len, next, more);
}

static int member_admin_get_page(void* ctx, const char* ns, uint64_t prefix, int after_valid, const merkle_key_t* after,
    uint8_t* buf, size_t* len, merkle_key_t* next, int* more) {
    return member_view_page(_db(ctx), ns, prefix, after_valid, after, buf, len, next, more, 1);
}

struct ms_props_cbk { node_props_changed_fn fn; void* arg; struct ms_props_cbk* next; };

static int member_callback_channel(const char* channel, char out[65]) {
    size_t size = channel ? strnlen(channel, 65) : 0;
    if (size != 64) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member callback: invalid namespace"); return -1; }
    memcpy(out, channel, size + 1); return 0;
}

static void _fire_props_changed(struct UTUN_INSTANCE* inst, const uint8_t ed[32], const char* channel_id) {
    if (!inst || !CC(inst)) return;
    char channel[65]; if (member_callback_channel(channel_id, channel)) return;
    struct chat_core_ctx* cc = CC(inst);
    uint8_t key[32]; memcpy(key, ed, 32);
    cc->props_dispatch_depth++;
    for (struct ms_props_cbk* pc = cc->props_cbks; pc; pc = pc->next)
        if (pc->fn) pc->fn(key, channel, pc->arg);
    if (--cc->props_dispatch_depth == 0) {
        struct ms_props_cbk** p = &cc->props_cbks;
        while (*p) {
            if (!(*p)->fn) { struct ms_props_cbk* removed = *p; *p = removed->next; u_free(removed); }
            else p = &(*p)->next;
        }
    }
}

int member_sync_add_props_cbk(struct UTUN_INSTANCE* inst, node_props_changed_fn fn, void* arg) {
    if (!inst || !fn || !CC(inst)) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "props callback invalid arguments"); return -1; }
    struct ms_props_cbk* e = u_malloc(sizeof(*e));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "props callback allocation failed"); return -1; }
    e->fn = fn; e->arg = arg;
    e->next = CC(inst)->props_cbks;
    CC(inst)->props_cbks = e;
    return 0;
}

void member_sync_remove_props_cbk(struct UTUN_INSTANCE* inst, node_props_changed_fn fn, void* arg) {
    if (!inst || !fn || !CC(inst)) return;
    struct ms_props_cbk** p = &CC(inst)->props_cbks;
    while (*p) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct ms_props_cbk* r = *p;
            if (CC(inst)->props_dispatch_depth) { r->fn = NULL; r->arg = NULL; }
            else { *p = r->next; u_free(r); }
            return;
        }
        p = &(*p)->next;
    }
}

/* ── BGP node event callback: BGP сообщает member_sync об обновлении адресов/классификации.
   Классификацию node_type/storage пересчитывает сам BGP (nodeinfo_updated);
   member_sync только рассылает node_props_changed потребителям. ── */

void member_sync_notify_props(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32]) {
    if (!inst || !member_sync_signer(ed) || !chat_channel_valid(channel)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member properties notification: invalid instance/channel/node"); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "%s: properties ch=%s peer=%s", MS_ID, channel, sc_public_key_hex(ed, (char[65]){0}));
    _fire_props_changed(inst, ed, channel);
}

static void _ms_on_bgp_node(struct TOPO_GROUP* group, const uint8_t ed[32], int event, void* arg) {
    (void)event;
    if (group && !group->stopping) member_sync_notify_props(arg, group->channel_id, ed);
}

static int _sig_is_zero64(const uint8_t* sig) {
    if (!sig) return 1;
    for (int i = 0; i < 64; i++) if (sig[i]) return 0;
    return 1;
}

/* Нулевое время зарезервировано для пустой строки. Полное приглашение хранит Unix-время в SQLite INTEGER. */
static int member_invite_time_valid(const char* channel, const struct ms_member_rec* m) {
    if (m->join_ts && m->join_ts <= INT64_MAX) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member invitation timestamp rejected ch=%s node=%016llx join_ts=%llu",
        channel, (unsigned long long)m->node_id, (unsigned long long)m->join_ts);
    return 0;
}

/* Подпись JOIN после проверки публичной пары устройства. */
static int member_verify_join_signature(sqlite3* db, const char* ch_id, const struct ms_member_rec* m, int* error) {
    if (_sig_is_zero64(m->join_sig)) return 0;
    uint8_t ch_x[32], ch_ed[32]; int found = 0;
    int code = member_sync_channel_keys_read(db, ch_id, ch_x, ch_ed, &found);
    if (code || !found) {
        if (error) *error = code;
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: join verification unavailable ns=%s found=%d rc=%d", MS_ID, ch_id, found, code);
        return code ? MS_VERIFY_ERROR : MS_VERIFY_DEFERRED;
    }
    uint8_t vmsg[MEMBER_JOIN_MSG_SIZE];
    int vlen = member_sync_build_join_msg(ch_x, ch_ed, m->ed25519, m->x25519,
                                          vmsg, (int)sizeof(vmsg));
    if (vlen < 0) {
        if (error) *error = SQLITE_ERROR;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: join message build failed ns=%s", MS_ID, ch_id); return MS_VERIFY_ERROR;
    }
    int rc = sc_ed25519_verify(m->ed25519, vmsg, (size_t)vlen, m->join_sig);
    if (rc == SC_ERR_AUTH_FAILED) db_changes_warn(db, NULL, ch_id, "Member JOIN", "invalid signature", m->ed25519, m->join_sig, NULL);
    if (rc != SC_OK && rc != SC_ERR_AUTH_FAILED && error) *error = SQLITE_ERROR;
    if (rc != SC_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: join verification ns=%s node=%016llx rc=%d",
                                MS_ID, ch_id, (unsigned long long)m->node_id, rc);
    return rc == SC_OK ? 1 : rc == SC_ERR_AUTH_FAILED ? 0 : MS_VERIFY_ERROR;
}

static int member_verify_join_ex(sqlite3* db, const char* ch_id, const struct ms_member_rec* m, int* error) {
    int rc = sc_identity_check(ch_id, m->x25519, m->ed25519);
    if (rc == SC_OK) return member_verify_join_signature(db, ch_id, m, error);
    if (rc == SC_ERR_AUTH_FAILED) return 0;
    if (error) *error = SQLITE_ERROR;
    return MS_VERIFY_ERROR;
}

/* Подписант и получатель строят одни и те же байты из полей записи MEMBERS. */
int member_sync_build_sign_msg(const char* channel, const uint8_t signer_ed[32], const uint8_t x25519[32], const uint8_t ed25519[32],
    const uint8_t join_id[32], uint64_t join_ts, uint8_t* out, size_t capacity) {
    if (!chat_channel_valid(channel) || !x25519 || !ed25519 || !join_id || !join_ts || join_ts > INT64_MAX || !out) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invalid invitation fields group=%s timestamp=%llu",
            channel ? channel : "-", (unsigned long long)join_ts); return -1;
    }
    char x[65], ed[65], join[65], signer[65] = "group";
    if (signer_ed) bytes_to_hex(signer_ed, 32, signer, sizeof(signer));
    bytes_to_hex(x25519, 32, x, sizeof(x)); bytes_to_hex(ed25519, 32, ed, sizeof(ed));
    bytes_to_hex(join_id, 32, join, sizeof(join));
    int n = snprintf((char*)out, capacity, "{\"type\":\"utun.invite.v4\",\"signer\":\"%s\","
        "\"x25519\":\"%s\",\"ed25519\":\"%s\",\"join_id\":\"%s\",\"join_ts\":\"%llu\"}",
        signer, x, ed, join, (unsigned long long)join_ts);
    if (n < 0 || (size_t)n >= capacity) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invitation JSON encoding failed group=%s bytes=%d capacity=%zu", channel, n, capacity); return -1;
    }
    return n;
}

int member_sync_sign_join(const uint8_t signer_ed_priv[32], const char* channel, const uint8_t signer_ed[32],
    const uint8_t x25519[32], const uint8_t ed25519[32], const uint8_t join_id[32], uint64_t join_ts, uint8_t sig[64]) {
    uint8_t msg[MEMBER_SIGN_MSG_SIZE];
    if (!signer_ed_priv || !sig) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invitation certificate missing signing key or signature output");
        return -1;
    }
    int len = member_sync_build_sign_msg(channel, signer_ed, x25519, ed25519, join_id, join_ts, msg, sizeof(msg));
    if (len < 0) return -1;
    int rc = sc_ed25519_sign(signer_ed_priv, msg, (size_t)len, sig);
    if (rc != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invitation certificate signing failed ch=%s authority=%s rc=%d",
            channel, signer_ed ? "member" : "group", rc);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "signed member key pair and JOIN ch=%s authority=%s", channel, signer_ed ? "member" : "group");
    return 0;
}

/* Читает только требуемые ключи группы, сохраняя SQLite-код до finalize. Выходы меняются лишь при found=1. */
int member_sync_channel_keys_read(sqlite3* db, const char* channel, uint8_t* x, uint8_t* ed, int* found) {
    if (!db || !channel || !found) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member group-key read: missing database/channel/output"); return SQLITE_MISUSE;
    }
    *found = 0; uint8_t keys[2][32]; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT x25519_pubkey,ed25519_pubkey FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        rc = SQLITE_OK; *found = 1;
        for (int col = 0; col < 2; col++) {
            if (!(col ? ed : x)) continue;
            int bytes = sqlite3_column_bytes(st, col), type = sqlite3_column_type(st, col);
            if (!bytes || type == SQLITE_NULL) { *found = 0; break; }
            if (type != SQLITE_BLOB || bytes != 32) { rc = SQLITE_CORRUPT; break; }
            const void* blob = sqlite3_column_blob(st, col);
            if (!blob) { rc = SQLITE_NOMEM; break; }
            memcpy(keys[col], blob, 32);
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (!rc && *found) { if (x) memcpy(x, keys[0], 32); if (ed) memcpy(ed, keys[1], 32); }
    if (rc) { *found = 0; DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member group-key read ch=%s rc=%d final=%d", channel, rc, final); }
    return rc;
}

static int member_verify_invitation(sqlite3* db, const char* ch_id, const struct ms_member_rec* m, int* error,
    int require_issuer, uint8_t* envelope, size_t* bytes) {
    if (error) *error = SQLITE_OK;
    if (!db || !ch_id || !m || !m->x25519 || !m->ed25519) {
        if (error) *error = SQLITE_MISUSE;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: invalid invitation verification arguments", MS_ID); return MS_VERIFY_ERROR;
    }
    if (!m->signature || _sig_is_zero64(m->signature)) return 0;
    uint8_t msg[MEMBER_SIGN_MSG_SIZE];
    if (_sig_is_zero64(m->join_sig)) return 0;
    if (!member_invite_time_valid(ch_id, m)) return 0;
    uint8_t id[32]; int code = member_sync_join_id_read(db, ch_id, m->ed25519, m->x25519, id);
    if (code) { if (error) *error = code; return code == SQLITE_NOTFOUND ? MS_VERIFY_DEFERRED : MS_VERIFY_ERROR; }
    uint8_t signer_pub[32];
    if (m->signed_by) memcpy(signer_pub, m->signed_by, 32);
    else {
        int found;
        code = member_sync_channel_keys_read(db, ch_id, NULL, signer_pub, &found);
        if (code) { if (error) *error = code; return MS_VERIFY_ERROR; }
        if (!found) return MS_VERIFY_DEFERRED;
    }
    int len = member_sync_build_sign_msg(ch_id, m->signed_by ? signer_pub : NULL, m->x25519, m->ed25519, id, m->join_ts, msg, sizeof(msg));
    if (len < 0) {
        if (error) *error = SQLITE_ERROR;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: invitation message build failed", MS_ID); return MS_VERIFY_ERROR;
    }

    int rc = sc_ed25519_verify(signer_pub, msg, (size_t)len, m->signature);
    if (rc != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS invalid invitation ch=%s subject=%s signer=%s rc=%d",
            ch_id, sc_public_key_hex(m->ed25519, (char[65]){0}), sc_public_key_hex(signer_pub, (char[65]){0}), rc);
        if (rc == SC_ERR_AUTH_FAILED) {
            db_changes_warn(db, NULL, ch_id, "Invitation", "invalid issuer signature", m->ed25519, m->signature, NULL);
            return 0;
        }
        if (error) *error = SQLITE_ERROR;
        return MS_VERIFY_ERROR;
    }
    if (require_issuer && m->signed_by) {
        char sql[160]; sqlite3_stmt* st = NULL;
        snprintf(sql, sizeof(sql), "SELECT 1 FROM peers_%s WHERE ed25519_pubkey=? AND record_state=0 AND source=0", ch_id);
        code = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
        if (!code) code = sqlite3_bind_blob(st, 1, m->signed_by, 32, SQLITE_STATIC);
        if (!code) code = sqlite3_step(st);
        int found = code == SQLITE_ROW;
        int final = sqlite3_finalize(st);
        if (code == SQLITE_ROW || code == SQLITE_DONE) code = final;
        if (code) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS signer lookup ch=%s rc=%d", ch_id, code);
            if (error) *error = code;
            return MS_VERIFY_ERROR;
        }
        if (!found) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS waiting for inviter ch=%s signer=%s",
                ch_id, sc_public_key_hex(m->signed_by, (char[65]){0})); return MS_VERIFY_DEFERRED;
        }
    }
    if (envelope) {
        envelope[0] = MEMBER_LOG_INVITATION;
        memcpy(envelope + 1, signer_pub, 32); memcpy(envelope + 33, m->signature, 64);
        memcpy(envelope + MEMBER_LOG_INVITATION_PREFIX, msg, (size_t)len);
        *bytes = MEMBER_LOG_INVITATION_PREFIX + (size_t)len;
    }
    return 1;
}

static int member_verify_signature_ex(sqlite3* db, const char* ch_id, const struct ms_member_rec* m, int* error) {
    return member_verify_invitation(db, ch_id, m, error, 1, NULL, NULL);
}

int member_sync_verify_signature(sqlite3* db, const char* channel, const struct ms_member_rec* member) {
    return member_verify_signature_ex(db, channel, member, NULL);
}

int member_sync_validate_pubkey(struct UTUN_INSTANCE* inst, const char* ch_id,
                                const uint8_t* x25519_pubkey) {
    if (!inst || !ch_id || !x25519_pubkey) return 0;
    if (db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "legacy member presence lookup rejected in RW; use committed access snapshot"); return 0;
    }
    sqlite3* db = _db(inst); if (!db) return 0;
    if (!chat_channel_valid(ch_id)) return 0;
    char sql[128]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT 1 FROM peers_%s WHERE x25519_pubkey=? AND record_state=0", ch_id);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, x25519_pubkey, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    if ((rc != SQLITE_DONE && rc != SQLITE_ROW) || final)
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member lookup ch=%s rc=%d finalize=%d", ch_id, rc, final);
    return rc == SQLITE_ROW && !final;
}

/* JOIN не подписывает профиль: отсутствие update_sig допустимо только для пустого блока. */
/* 1 — допустимый профиль/JOIN-only, 0 — ошибка формата, -1 — отказ SQL. */
static int member_profile_shape(sqlite3* db, const char* channel, const struct ms_member_rec* m, int* error) {
    if (m->update_sig && !_sig_is_zero64(m->update_sig)) {
        size_t size = m->userinfo ? strnlen(m->userinfo, MS_USERINFO_MAX + 1) : 0;
        int code = size && size <= MS_USERINFO_MAX && m->update_ts && m->update_ts <= INT64_MAX
            ? chat_json_strings_read(db, m->userinfo, MS_USERINFO_MAX, NULL, NULL) : SQLITE_CONSTRAINT;
        if (!code) return 1;
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member profile rejected ch=%s node=%016llx version=%llu bytes=%zu rc=%d",
            channel, (unsigned long long)m->node_id, (unsigned long long)m->update_ts, size, code);
        if (code == SQLITE_CONSTRAINT) {
            db_changes_warn(db, NULL, channel, "Member profile", "invalid profile fields", m->ed25519, m->update_sig, NULL);
            return 0;
        }
        *error = code; return -1;
    }
    if (!m->update_ts && (!m->userinfo || !m->userinfo[0])) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member unsigned profile rejected ch=%s node=%016llx version=%llu bytes=%zu",
        channel, (unsigned long long)m->node_id, (unsigned long long)m->update_ts, m->userinfo ? strlen(m->userinfo) : 0);
    db_changes_warn(db, NULL, channel, "Member profile", "missing signature", m->ed25519, NULL, NULL);
    return 0;
}

/* Блок владельца подписан отдельно от профиля: его отзыв нельзя потерять при замене битого профиля. */
static int member_verify_owner_ex(sqlite3* db, const char* channel, const struct ms_member_rec* m, int* error) {
    if (!m->adm_tags || !m->adm_tags[0]) {
        if (_sig_is_zero64(m->adm_tags_sig)) return 1;
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member empty owner block has signature ch=%s ed=%s",
            channel, sc_public_key_hex(m->ed25519, (char[65]){0}));
        return 0;
    }
    if (!m->adm_tags_sig || _sig_is_zero64(m->adm_tags_sig)) {
        db_changes_warn(db, NULL, channel, "Member owner policy", "missing signature", m->ed25519, NULL, NULL);
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member owner signature missing ch=%s node=%016llx",
            channel, (unsigned long long)m->node_id); return 0;
    }
    uint8_t key[32], body[MS_OWNER_MSG_MAX];
    int found = 0, code = member_sync_channel_keys_read(db, channel, NULL, key, &found);
    if (code || !found) {
        if (error) *error = code;
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member owner key unavailable ch=%s found=%d rc=%d", channel, found, code);
        return code ? MS_VERIFY_ERROR : MS_VERIFY_DEFERRED;
    }
    int bytes = member_sync_build_owner_msg(channel, m->ed25519, m->adm_tags, body, sizeof(body));
    if (bytes < 0) return 0;
    int verified = sc_ed25519_verify(key, body, (size_t)bytes, m->adm_tags_sig);
    if (verified != SC_OK && verified != SC_ERR_AUTH_FAILED && error) *error = SQLITE_ERROR;
    if (verified == SC_OK) return 1;
    if (verified == SC_ERR_AUTH_FAILED)
        db_changes_warn(db, NULL, channel, "Member owner policy", "invalid group signature", m->ed25519, m->adm_tags_sig, NULL);
    DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member owner verification ch=%s node=%016llx rc=%d",
        channel, (unsigned long long)m->node_id, verified);
    return verified == SC_ERR_AUTH_FAILED ? 0 : MS_VERIFY_ERROR;
}
enum { MEMBER_BAD_IDENTITY = 1, MEMBER_BAD_PROFILE = 2, MEMBER_BAD_OWNER = 4, MEMBER_BAD_PROJECTION = 8 };
struct member_saved {
    struct ms_member_rec rec;
    uint8_t x[32], ed[32], signer[32], join[64], update[64], owner[64], tree[64];
    char info[MS_USERINFO_MAX + 1], tags[MS_ADM_TAGS_MAX + 1];
    sqlite3_int64 row;
    int source, identity_verified;
    unsigned malformed; /* Повреждённые части строки: удостоверение, профиль, блок владельца. */
};

/* Снимок строки не держит SQLite pointers через изменения таблицы и callbacks. */
static int member_read_saved_row(sqlite3* db, const char* ns, sqlite3_int64 row, const uint8_t* ed,
        int state, struct member_saved* out) {
    char sql[512]; sqlite3_stmt* st = NULL; memset(out, 0, sizeof(*out));
    snprintf(sql, sizeof(sql), "SELECT x25519_pubkey,ed25519_pubkey,join_sig,join_ts,update_sig,update_ts,userinfo,"
        "adm_tags,adm_tags_sig,signed_by,signature,source,identity_verified,node_id,id FROM peers_%s WHERE %s AND record_state=?",
        ns, ed ? "ed25519_pubkey=?" : "id=?");
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = ed ? sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC) : sqlite3_bind_int64(st, 1, row);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 2, state);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW;
    if (found) {
        uint64_t node = (uint64_t)sqlite3_column_int64(st, 13);
        out->row = sqlite3_column_int64(st, 14);
        if (!member_profile_columns(st, 4)) out->malformed |= MEMBER_BAD_PROFILE;
        const int cols[] = {0, 1, 2, 4, 8, 10}, sizes[] = {32, 32, 64, 64, 64, 64};
        const unsigned parts[] = {MEMBER_BAD_IDENTITY, MEMBER_BAD_IDENTITY, MEMBER_BAD_IDENTITY,
                                  MEMBER_BAD_PROFILE, MEMBER_BAD_OWNER, MEMBER_BAD_PROJECTION};
        uint8_t* blobs[] = {out->x, out->ed, out->join, out->update, out->owner, out->tree};
        for (size_t i = 0; i < 6; i++) {
            int type = sqlite3_column_type(st, cols[i]), bytes = sqlite3_column_bytes(st, cols[i]);
            const void* blob = sqlite3_column_blob(st, cols[i]);
            if (type == SQLITE_BLOB && bytes == sizes[i] && blob) memcpy(blobs[i], blob, (size_t)bytes);
            else if (bytes || i < 2 || (type != SQLITE_BLOB && type != SQLITE_NULL)) out->malformed |= parts[i];
        }
        if (sqlite3_column_type(st, 3) != SQLITE_INTEGER || sqlite3_column_int64(st, 3) < 0) out->malformed |= MEMBER_BAD_PROJECTION;
        for (int i = 0; i < 2; i++) {
            int col = i ? 7 : 6, type = sqlite3_column_type(st, col);
            int bytes = sqlite3_column_bytes(st, col), max = i ? MS_ADM_TAGS_MAX : MS_USERINFO_MAX;
            const char* text = (const char*)sqlite3_column_text(st, col);
            if ((type != SQLITE_TEXT && type != SQLITE_NULL) ||
                bytes > max || (text && strlen(text) != (size_t)bytes)) out->malformed |= i ? MEMBER_BAD_OWNER : MEMBER_BAD_PROFILE;
            else if (text) memcpy(i ? out->tags : out->info, text, (size_t)bytes + 1);
        }
        int signer_rc = topo_node_sqlite_read_optional_key(st, 9, out->signer);
        if (signer_rc == SQLITE_CORRUPT) out->malformed |= MEMBER_BAD_PROJECTION;
        else if (signer_rc) rc = signer_rc;
        out->rec = (struct ms_member_rec){.node_id=node,.x25519=out->x,.ed25519=out->ed,.join_sig=out->join,
            .join_ts=(uint64_t)sqlite3_column_int64(st,3),.update_sig=out->update,.update_ts=(uint64_t)sqlite3_column_int64(st,5),
            .userinfo=out->info,.adm_tags=out->tags,.adm_tags_sig=out->owner,
            .signed_by=member_sync_signer(out->signer),.signature=out->tree};
        out->source = sqlite3_column_int(st, 11);
        out->identity_verified = sqlite3_column_int(st, 12);
        if (sqlite3_errcode(db) == SQLITE_NOMEM) rc = SQLITE_NOMEM;
    }
    int final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        int error = rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member version read ch=%s ed=%s row=%lld state=%d rc=%d: %s",
            ns, sc_public_key_hex(ed, (char[65]){0}), (long long)row, state, error,
            (sqlite3_errcode(db) & 255) == (error & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(error)); return -error;
    }
    return found;
}

static int member_version_order(uint64_t incoming, uint64_t stored) {
    return (incoming > stored) - (incoming < stored);
}

/* Журнал пишется отдельной транзакцией после публикации MEMBERS. В задании нет указателей на Merkle-страницу. */
struct invitation_audit {
    struct UTUN_INSTANCE* inst;
    char channel[65];
    uint64_t self, now, generation, registration, seq;
    uint64_t peer_node;
    int history_changed;
    uint8_t key[32], secret[32];
    uint8_t peer[32];
    struct group_invitation_candidate candidate;
};

static int audit_current(void* arg) {
    struct invitation_audit* job = arg;
    /* При создании канала собственное приглашение уже сохранено, а регистрация группы ещё может отсутствовать. */
    return !job->inst->destroying && job->inst->group_started && job->generation == job->inst->group_generation &&
        (!job->registration || member_registration_current(job->inst, job->channel, job->registration));
}
static int audit_append(sqlite3* db, struct invitation_audit* job) {
    struct member_saved own;
    int found = member_read_saved_row(db, job->channel, 0, job->key, 0, &own);
    if (found < 0) return -found;
    if (!found) return SQLITE_OK;
    struct ms_member_rec m = own.rec;
    if (member_sync_signer(job->candidate.subject)) {
        m.signed_by = member_sync_signer(job->candidate.signer); m.join_ts = job->candidate.stamp; m.signature = job->candidate.signature;
    }
    if (_sig_is_zero64(m.signature)) return SQLITE_OK;
    uint8_t body[MEMBER_LOG_INVITATION_PREFIX + MEMBER_SIGN_MSG_SIZE] = {MEMBER_LOG_INVITATION};
    int code = SQLITE_OK;
    if (member_verify_join_signature(db, job->channel, &m, &code) != 1) return code ? code : SQLITE_AUTH;
    size_t bytes;
    if (member_verify_invitation(db, job->channel, &m, &code, 1, body, &bytes) != 1) return code ? code : SQLITE_AUTH;
    return chat_log_invitation(db, job->channel, job->now, body, bytes, job->key, job->secret, &job->seq, &job->history_changed);
}
static int audit_write(sqlite3* db, void* arg) {
    struct invitation_audit* job = arg;
    struct db_changes_origin source = db_changes_source(db, job->peer, job->peer_node, "invitation receipt");
    int rc = audit_append(db, job);
    db_changes_restore(db, source);
    return rc;
}
static void audit_done(void* arg, const struct db_async_result* result) {
    struct invitation_audit* job = arg;
    if (result->status) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "invitation audit ch=%s node=%016llx signer=%s rc=%d",
            job->channel, (unsigned long long)job->self, sc_public_key_hex(job->candidate.signer, (char[65]){0}), result->sqlite_code); return;
    }
    if (!job->seq || !audit_current(job)) return;
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "invitation recorded ch=%s node=%016llx seq=%llu",
        job->channel, (unsigned long long)job->self, (unsigned long long)job->seq);
    if (job->inst->chat_content_started && group_protocol_current(job->inst, job->channel) == GROUP_PROTOCOL_CHAT)
        chat_log_local_committed(job->inst, job->channel, job->seq, job->history_changed);
}
static void audit_free(void* arg) {
    struct invitation_audit* job = arg; OPENSSL_cleanse(job->secret, sizeof(job->secret)); u_free(job);
}
static void member_audit_invitation(struct UTUN_INSTANCE* inst, const char* channel,
    const struct group_invitation_candidate* candidate, const uint8_t* peer) {
    if (!inst || !inst->group_started || inst->destroying || !chat_channel_valid(channel) || (candidate && memcmp(candidate->subject, inst->my_ed25519_pubkey, 32))) return;
    sqlite3* db = _db(inst);
    int worker = db_async_can_write(inst->db_worker);
    if (!worker) {
        const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
        if (!file || !*file) return;
    }
    struct invitation_audit* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invitation audit allocation ch=%s", channel); return; }
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel);
    job->inst = inst; job->generation = inst->group_generation; job->self = inst->node_id; job->now = ntp_time_get_us(inst);
    job->registration = group ? group->generation : 0;
    strcpy(job->channel, channel); memcpy(job->key, inst->my_ed25519_pubkey, 32); memcpy(job->secret, inst->my_ed25519_privkey, 32);
    if (candidate) job->candidate = *candidate;
    if (peer) {
        memcpy(job->peer, peer, 32);
        struct ETCP_CONN* conn = instance_find_conn_by_ed(inst, peer);
        if (conn) job->peer_node = conn->peer_node_id;
    }
    if (worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="invitation-audit", .mode=DB_ASYNC_WRITE, .data=job,
            .guard=audit_current, .work=audit_write, .done=audit_done, .cleanup=audit_free});
        return;
    }
    int rc = audit_current(job) ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_ABORT;
    if (!rc) {
        rc = audit_write(db, job);
        if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        if (rc && !sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL))
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invitation audit rollback ch=%s: %s", channel, sqlite3_errmsg(db));
    }
    audit_done(job, &(struct db_async_result){.status=rc ? DB_ASYNC_SQL : 0, .sqlite_code=rc}); audit_free(job);
}

void member_sync_audit_invitation(struct UTUN_INSTANCE* inst, const char* channel, const struct group_invitation_candidate* candidate) {
    member_audit_invitation(inst, channel, candidate, NULL);
}

static struct group_invitation_candidate member_candidate(const struct ms_member_rec* m) {
    struct group_invitation_candidate c = {.stamp=m->join_ts};
    memcpy(c.subject, m->ed25519, 32);
    if (m->signed_by) memcpy(c.signer, m->signed_by, 32);
    memcpy(c.signature, m->signature, 64); return c;
}

static int member_invitation_index(sqlite3* db, const char* channel, const struct group_invitation_candidate* invite) {
    uint8_t wire[MEMBER_INVITE_WIRE_SIZE];
    memcpy(wire, invite->subject, 32); memcpy(wire + 32, invite->signer, 32);
    member_write64(wire + 64, invite->stamp); memcpy(wire + 72, invite->signature, 64);
    merkle_key_t key;
    int rc = member_item_key(MEMBER_ITEM_INVITE, wire, &key);
    return rc ? -rc : member_item_write(db, channel, &key, wire, sizeof(wire));
}

/* Подпись приглашения проверяется без JOIN-строк любого из её участников. */
static int member_invitation_check(sqlite3* db, const char* channel, const struct group_invitation_candidate* invite, int* rejected) {
    uint8_t x[32], id[32], group[32], body[MEMBER_SIGN_MSG_SIZE];
    *rejected = 0;
    int rc = sc_identity_public_x("MEMBERS invitation subject", invite->subject, x);
    const char* reason = "invalid subject or issuer key";
    if (rc) goto invalid;
    const uint8_t* signer = member_sync_signer(invite->signer);
    if (signer) {
        uint8_t signer_x[32];
        rc = sc_identity_public_x("MEMBERS invitation issuer", signer, signer_x);
        if (rc) goto invalid;
    } else {
        int found;
        rc = member_sync_channel_keys_read(db, channel, NULL, group, &found);
        if (rc || !found) return rc ? rc : SQLITE_NOTFOUND;
    }
    rc = member_sync_join_id_read(db, channel, invite->subject, x, id);
    if (rc) return rc;
    int bytes = member_sync_build_sign_msg(channel, signer, x, invite->subject, id, invite->stamp, body, sizeof(body));
    if (bytes <= 0) { rc = SC_ERR_AUTH_FAILED; reason = "invalid signed fields"; goto invalid; }
    rc = sc_ed25519_verify(signer ? signer : group, body, (size_t)bytes, invite->signature);
    if (rc == SC_OK) return SQLITE_OK;
    reason = "invalid issuer signature";
invalid:
    *rejected = rc == SC_ERR_AUTH_FAILED;
    if (*rejected) db_changes_warn(db, NULL, channel, "Invitation", reason, invite->subject, invite->signature, NULL);
    return *rejected ? SQLITE_AUTH : SQLITE_ERROR;
}

static int member_invitation_put(sqlite3* db, const char* channel, const struct group_invitation_candidate* invite) {
    int rejected;
    int rc = sqlite3_get_autocommit(db) ? SQLITE_MISUSE : member_invitation_check(db, channel, invite, &rejected);
    sqlite3_stmt* st = NULL;
    if (!rc) rc = sqlite3_prepare_v2(db,
        "INSERT INTO member_invitations(channel,subject,signer,stamp,signature) VALUES(?,?,?,?,?)"
        " ON CONFLICT(channel,subject,signer) DO UPDATE SET stamp=excluded.stamp,signature=excluded.signature"
        " WHERE (excluded.signature,excluded.stamp)<(signature,stamp)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, invite->subject, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, invite->signer, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)invite->stamp);
    if (!rc) rc = sqlite3_bind_blob(st, 5, invite->signature, 64, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int changed = rc == SQLITE_DONE && sqlite3_changes(db) == 1;
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS invitation ch=%s subject=%s issuer=%s rc=%d", channel,
            sc_public_key_hex(invite->subject, (char[65]){0}), sc_public_key_hex(invite->signer, (char[65]){0}), rc); return -rc;
    }
    if (!changed) return 0;
    rc = member_invitation_index(db, channel, invite);
    if (rc >= 0) DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS invitation stored ch=%s subject=%s issuer=%s", channel,
        sc_public_key_hex(invite->subject, (char[65]){0}), sc_public_key_hex(invite->signer, (char[65]){0}));
    return rc < 0 ? rc : 1;
}

int member_sync_reindex(sqlite3* db, const char* channel, const uint8_t ed[32]) {
    if (!db || !chat_channel_valid(channel) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS reindex requires database/canonical channel/caller transaction"); return -SQLITE_MISUSE;
    }
    merkle_key_t key = {{0}};
    if (!ed) {
        uint8_t wire[GROUP_POLICY_WIRE_SIZE + GROUP_INFO_WIRE_MAX];
        struct group_policy policy; struct group_info_record info;
        int rc = group_policy_get(db, channel, &policy);
        if (!rc) rc = group_info_get(db, channel, &info);
        if (rc < 0) return rc;
        group_policy_encode(&policy, wire);
        int bytes = group_info_encode(&info, wire + GROUP_POLICY_WIRE_SIZE, GROUP_INFO_WIRE_MAX);
        if (bytes < 0) return -SQLITE_CORRUPT;
        return member_item_write(db, channel, &key, wire, GROUP_POLICY_WIRE_SIZE + (size_t)bytes);
    }
    struct member_saved saved;
    int found = member_read_saved_row(db, channel, 0, ed, 0, &saved);
    if (found < 0) return found;
    if (found && (saved.malformed & ~MEMBER_BAD_PROJECTION)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS materialization ch=%s invalid stored fields=%u", channel, saved.malformed);
        return -SQLITE_CORRUPT;
    }
    int rc = member_item_key(MEMBER_ITEM_JOIN, ed, &key);
    if (rc) return -rc;
    uint8_t wire[MEMBER_OWNER_WIRE_FIXED + MS_ADM_TAGS_MAX]; size_t bytes = 0;
    if (found && !saved.source && !_sig_is_zero64(saved.join)) {
        memcpy(wire, ed, 32); memcpy(wire + 32, saved.x, 32); memcpy(wire + 64, saved.join, 64);
        member_write64(wire + 128, saved.rec.update_ts); memcpy(wire + 136, saved.update, 64);
        size_t size = strlen(saved.info);
        wire[200] = (uint8_t)(size >> 8); wire[201] = (uint8_t)size; memcpy(wire + 202, saved.info, size);
        bytes = MEMBER_JOIN_WIRE_FIXED + size;
    }
    int result = member_item_write(db, channel, &key, bytes ? wire : NULL, bytes);
    if (result < 0) return result;
    rc = member_item_key(MEMBER_ITEM_OWNER, ed, &key);
    if (rc) return -rc;
    bytes = 0;
    if (found && saved.tags[0]) {
        size_t size = strlen(saved.tags);
        memcpy(wire, ed, 32); memcpy(wire + 32, saved.owner, 64);
        wire[96] = (uint8_t)(size >> 8); wire[97] = (uint8_t)size; memcpy(wire + 98, saved.tags, size);
        bytes = MEMBER_OWNER_WIRE_FIXED + size;
    }
    rc = member_item_write(db, channel, &key, bytes ? wire : NULL, bytes);
    return rc < 0 ? rc : result || rc;
}

enum member_apply_mode { MEMBER_LOCAL, MEMBER_DIRECT, MEMBER_STAGE };

/* Сохраняет каталог и классификацию по полному транспортному ключу участника. */
static int member_project_node(sqlite3* db, const struct ms_member_rec* member, uint64_t now) {
    char name[128] = "";
    if (member->userinfo) json_flat_get(member->userinfo, "name", name, sizeof(name));
    int rc = topo_node_sqlite_node_write(db, member->node_id, name, member->x25519, member->ed25519,
        member->update_ts ? member->update_ts : member->join_ts, now / 1000000);
    return rc ? rc : topo_node_sqlite_nodeinfo_write(db, member->x25519);
}

static int member_repair_invalid(sqlite3* db, const char* channel, const struct member_saved* saved, int* retained);

static int member_apply_record(sqlite3* db, struct member_sync_write_context* context, const char* channel,
    const struct ms_member_rec* input, const struct member_saved* local, enum member_apply_mode mode) {
    struct ms_member_rec record = *input;
    int changed = 0, stale = 0, code = SQLITE_OK;
    const char* block = "identity";
    if (!record.ed25519 || !record.x25519 || !record.node_id) { code = SQLITE_MISUSE; goto fail; }
    int binding = sc_identity_check(channel, record.x25519, record.ed25519);
    if (binding) { code = binding == SC_ERR_AUTH_FAILED ? SQLITE_AUTH : SQLITE_ERROR; goto fail; }
    if (local && !local->source && memcmp(local->x, record.x25519, 32)) { code = SQLITE_AUTH; goto fail; }

    block = "invitation";
    int incoming_invite = !_sig_is_zero64(record.signature);
    if (incoming_invite) {
        struct group_invitation_candidate invite = member_candidate(&record);
        if (!invite.stamp && local) {
            /* Смена подписанта по команде владельца использует время сохранённого приглашения. */
            invite.stamp = local->rec.join_ts; record.join_ts = invite.stamp;
        }
        int stored = member_invitation_put(db, channel, &invite);
        if (stored < 0) { code = -stored; goto fail; }
        changed |= stored != 0;
    }

    int incoming_join = !_sig_is_zero64(record.join_sig);
    int incoming_profile = !_sig_is_zero64(record.update_sig) || record.update_ts || (record.userinfo && *record.userinfo);
    int local_join = local && !local->source && !_sig_is_zero64(local->join);
    if (incoming_join || incoming_profile) {
        block = "JOIN/profile";
        if (!incoming_join) {
            if (!local_join || record.join_sig) { code = SQLITE_AUTH; goto fail; }
            record.join_sig = local->join;
        }
        struct ms_member_rec check = record;
        check.signature = NULL; check.adm_tags = NULL; check.adm_tags_sig = NULL;
        int verified = member_sync_verify_local_record_ex(db, channel, &check, &code);
        if (verified != 1) { if (!code) code = SQLITE_AUTH; goto fail; }

        int join_order = !local_join ? 1 : memcmp(local->join, record.join_sig, 64);
        int profile_order = !local_join ? 1 : member_version_order(record.update_ts, local->rec.update_ts);
        if (!profile_order && record.update_ts) {
            profile_order = memcmp(local->update, record.update_sig, 64);
            if (!profile_order) profile_order = strcmp(local->info, record.userinfo);
        }
        if (join_order > 0 || profile_order > 0) {
            const uint8_t* signer = local_join ? local->rec.signed_by : record.signed_by;
            const uint8_t* cert = local_join ? local->rec.signature : record.signature;
            uint64_t stamp = local_join ? local->rec.join_ts : record.join_ts;
            if (topo_node_sqlite_member_block_put(db, channel, record.node_id, join_order > 0 ? record.join_sig : local->join, stamp,
                    profile_order > 0 ? record.update_sig : local->update, profile_order > 0 ? record.update_ts : local->rec.update_ts,
                    record.x25519, record.ed25519, profile_order > 0 ? record.userinfo : local->info, signer, cert)) {
                code = sqlite3_extended_errcode(db); if (!code) code = SQLITE_ERROR; goto fail;
            }
            changed = 1;
            if (mode != MEMBER_STAGE) {
                struct ms_member_rec selected = record;
                if (profile_order <= 0) { selected.userinfo = local->info; selected.update_ts = local->rec.update_ts; }
                code = member_project_node(db, &selected, context->now);
                if (code) goto fail;
            }
        }
        stale |= profile_order < 0 || join_order < 0;
    } else if (record.join_sig && mode != MEMBER_LOCAL) { code = SQLITE_AUTH; goto fail; }

    block = "owner";
    int owner_check = member_verify_owner_ex(db, channel, &record, &code);
    if (owner_check != 1) { if (!code) code = SQLITE_AUTH; goto fail; }
    if (record.adm_tags && record.adm_tags[0]) {
        struct member_owner_fields owner = {0}, previous = {0};
        code = member_owner_read(db, record.adm_tags, &owner);
        if (!code && local) code = member_owner_read(db, local->tags, &previous);
        if (code) goto fail;
        int order = member_owner_order(record.adm_tags, &owner, record.adm_tags_sig, local ? local->tags : "", &previous, local ? local->owner : NULL);
        if (order > 0) {
            if (!local && !incoming_join) {
                code = topo_node_sqlite_member_placeholder_write(db, channel, record.node_id, record.x25519, record.ed25519);
                if (code) goto fail;
            }
            if (topo_node_sqlite_member_owner_put(db, channel, record.ed25519, record.adm_tags, record.adm_tags_sig, owner.storage)) {
                code = sqlite3_extended_errcode(db); if (!code) code = SQLITE_ERROR; goto fail;
            }
            changed = 1;
        }
        stale |= order < 0;
    }
    block = "index";
    if (changed && (incoming_join || incoming_profile || (record.adm_tags && *record.adm_tags))) {
        int indexed = member_sync_reindex(db, channel, record.ed25519);
        if (indexed < 0) { code = -indexed; goto fail; }
    }
    return (changed ? MS_APPLY_CHANGED : 0) | (stale ? MS_APPLY_STALE : 0);
fail:
    context->sqlite_code = code;
    DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS merge ch=%s subject=%s block=%s rc=%d (%s)", channel,
        sc_public_key_hex(record.ed25519, (char[65]){0}), block, code, sqlite3_errstr(code));
    return code == SQLITE_AUTH || code == SQLITE_CONSTRAINT ? MT_ERR_CONFLICT : MT_ERR_IO;
}

int member_sync_record_write(sqlite3* db, struct member_sync_write_context* context,
    const char* channel, const uint8_t source_ed[32], const struct ms_member_rec* record) {
    if (!db || !context || !record || !record->ed25519 || !chat_channel_valid(channel) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member WRITE requires SQL context and caller transaction");
        if (context) context->sqlite_code = SQLITE_MISUSE;
        return MT_ERR_IO;
    }
    context->sqlite_code = SQLITE_OK;
    struct member_saved previous;
    int found = member_read_saved_row(db, channel, 0, record->ed25519, 0, &previous);
    if (found < 0) { context->sqlite_code = -found; return MT_ERR_IO; }
    if (found && previous.malformed) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member write: malformed stored record ch=%s node=%016llx fields=%u",
            channel, (unsigned long long)record->node_id, previous.malformed);
        context->sqlite_code = SQLITE_CORRUPT; return MT_ERR_IO;
    }
    return member_apply_record(db, context, channel, record, found ? &previous : NULL, source_ed ? MEMBER_DIRECT : MEMBER_LOCAL);
}

struct member_device_job {
    struct TOPO_NODE node; /* Только node_id, оба public key и owned devgroup; остальные поля нулевые. */
    struct UTUN_INSTANCE* inst; uint64_t generation; /* Только guard в uasync. */
};
static int member_device_guard(void* data) {
    struct member_device_job* job = data;
    return !job->inst->destroying && job->inst->group_started &&
        chat_core_generation(job->inst) == job->generation && chat_core_is_initialized(job->inst);
}
static int member_device_write(sqlite3* db, void* data) {
    struct member_device_job* job = data; int result = devgroup_project(db, &job->node); return result < 0 ? -result : SQLITE_OK;
}
static void member_device_done(void* data, const struct db_async_result* result) {
    struct member_device_job* job = data;
    if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member device projection failed node=%016llx status=%d rc=%d: %s",
        (unsigned long long)job->node.node_id, result->status, result->sqlite_code, result->message);
}
static void member_device_free(void* data) { u_free(data); }
static void member_device_project(struct UTUN_INSTANCE* inst, const struct TOPO_NODE* node) {
    if (!db_async_can_write(inst->db_worker)) {
        if (devgroup_project(_db(inst), node)) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member device projection failed node=%016llx",
            (unsigned long long)node->node_id);
        return;
    }
    struct member_device_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member device projection allocation node=%016llx", (unsigned long long)node->node_id); return; }
    job->inst = inst; job->generation = chat_core_generation(inst); job->node.node_id = node->node_id; job->node.devgroup = node->devgroup;
    memcpy(job->node.public_key, node->public_key, 32); memcpy(job->node.ed25519_public_key, node->ed25519_public_key, 32);
    db_async_run(inst->db_worker, &(struct db_async_task){.name="member-devgroup", .mode=DB_ASYNC_WRITE,
        .guard=member_device_guard, .work=member_device_write, .done=member_device_done, .cleanup=member_device_free, .data=job});
}

static void member_publish_record(struct UTUN_INSTANCE* inst, const char* ns, const uint8_t ed[32], int notify_sync) {
    char channel[65]; if (!inst || member_callback_channel(ns, channel)) return;
    uint8_t key[32], x[32];
    if (!member_sync_signer(ed)) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member commit missing Ed25519 ch=%s", channel); return; }
    memcpy(key, ed, 32); ed = key;
    if (inst->topo_groups && sc_identity_public_x("member commit", ed, x) == SC_OK) {
        struct TOPO_NODE* snapshot = topo_node_registry_lookup(inst->topo_groups, sc_derive_node_id_from_pubkey(x), x);
        if (snapshot && snapshot->timestamp && !memcmp(snapshot->ed25519_public_key, ed, 32)) member_device_project(inst, snapshot);
    }
    if (notify_sync) {
        merkle_sync_changed(inst, MT_DATA_MEMBERS, channel);
        group_protocol_members_changed(inst, channel);
    }
    uint64_t group_id = topo_groups_channel_slot(inst->topo_groups, channel);
    struct TOPO_GROUP* g = inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    if (g && g->group_type == TOPO_GROUP_TYPE_CHAT) {
        member_sync_notify_props(inst, channel, ed);
        /* Получатель свойств вправе удалить группу/соединение;после callback берём только актуальные handles. */
        g = inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
        if (!g || g->group_type != TOPO_GROUP_TYPE_CHAT) return;
        struct ETCP_CONN* conn = ed ? instance_find_conn_by_ed(inst, ed) : NULL;
        /* Существующий UP сохраняем; новый транспорт выбирает только автопланировщик. */
        if (conn && conn->links_up && !conn->close_requested && topo_group_new_conn(g, conn) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: cannot attach UP member channel=%s peer=%s", MS_ID, channel,
                       sc_public_key_hex(ed, (char[65]){0}));
        g = inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
        if (g && g->group_type == TOPO_GROUP_TYPE_CHAT) topo_group_connect_changed(g);
    }
}

void member_sync_record_committed(struct UTUN_INSTANCE* inst, const char* ns, const uint8_t ed[32], unsigned changes) {
    (void)changes;
    member_publish_record(inst, ns, ed, 1);
}

static void member_writer_init(struct member_sync_write_context* writer, struct UTUN_INSTANCE* inst) {
    writer->self = inst->node_id;
    writer->now = ntp_time_get_us(inst);
    memcpy(writer->public_key, inst->my_ed25519_pubkey, sizeof(writer->public_key));
}

int member_sync_apply_record(struct UTUN_INSTANCE* inst, const char* ch_id,
                             const uint8_t source_ed[32], const struct ms_member_rec* m) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "legacy member mutation rejected in RW; use record_write in caller transaction"); return -1;
    }
    sqlite3* db = _db(inst);
    if (!db || !ch_id || !m) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: invalid apply args", MS_ID); return -1; }
    int own = sqlite3_get_autocommit(db);
    struct group_policy_update* admission = NULL;
    if (sqlite3_exec(db, "SAVEPOINT member_record", NULL, NULL, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: record begin: %s", MS_ID, sqlite3_errmsg(db)); return -1;
    }
    struct member_sync_write_context context = {0};
    member_writer_init(&context, inst);
    struct ETCP_CONN* peer = source_ed ? instance_find_conn_by_ed(inst, source_ed) : NULL;
    struct db_changes_origin source = {0};
    if (source_ed) source = db_changes_source(db, source_ed, peer ? peer->peer_node_id : 0, "member update");
    int rc = member_sync_record_write(db, &context, ch_id, source_ed, m);
    if (rc < 0) goto error;
    if (own && (rc & (MS_APPLY_CHANGED)) &&
        group_policy_project(db, inst->node_id, ch_id, &admission)) goto error;
    int sql_rc = sqlite3_exec(db, "RELEASE member_record", NULL, NULL, NULL);
    if (sql_rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: record commit ch=%s rc=%d: %s", MS_ID, ch_id, sql_rc, sqlite3_errmsg(db)); goto error;
    }
    if (source_ed) db_changes_restore(db, source);
    if (own && !memcmp(m->ed25519, inst->my_ed25519_pubkey, 32) && m->join_ts && !_sig_is_zero64(m->signature)) {
        struct group_invitation_candidate received = member_candidate(m);
        member_audit_invitation(inst, ch_id, &received, source_ed);
    }
    if (admission) group_policy_publish(inst, admission);
    if (own && rc >= 0 && (rc & (MS_APPLY_CHANGED)))
        member_sync_record_committed(inst, ch_id, m->ed25519, (unsigned)rc);
    return rc;
error:
    if (source_ed) db_changes_restore(db, source);
    group_policy_discard(admission);
    if (!sqlite3_get_autocommit(db)) {
        int rollback = sqlite3_exec(db, "ROLLBACK TO member_record", NULL, NULL, NULL);
        if (rollback == SQLITE_OK) rollback = sqlite3_exec(db, "RELEASE member_record", NULL, NULL, NULL);
        if (rollback != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC,
            "%s: record rollback ch=%s rc=%d own=%d;partial savepoint not released: %s", MS_ID, ch_id, rollback, own, sqlite3_errmsg(db));
        if (own && !sqlite3_get_autocommit(db)) {
            rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: record full rollback ch=%s rc=%d: %s",
                MS_ID, ch_id, rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: record transaction remains open;requesting owner stop/DB close", MS_ID);
                utun_instance_stop(inst);
            }
        }
    }
    return -1;
}

/* Проверка сохранённых подписей, привязки ключа и представления профиля.
 * Неверная запись передаётся на восстановление, операционная ошибка откатывает транзакцию. */

int member_sync_verify_local_record_ex(struct sqlite3* db, const char* ch_id, const struct ms_member_rec* m, int* error) {
    if (error) *error = SQLITE_OK;
    if (!db || !ch_id || !m || !error) {
        if (error) *error = SQLITE_MISUSE;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: invalid local verification arguments", MS_ID); return -1;
    }
    if (!m->x25519 || !m->ed25519) return 1; /* неполная запись — не доказательство порчи */
    int shape = member_profile_shape(db, ch_id, m, error);
    if (shape != 1) return shape;

    int binding = sc_identity_check(ch_id, m->x25519, m->ed25519);
    if (binding == SC_ERR_AUTH_FAILED) {
        db_changes_warn(db, NULL, ch_id, "Member identity", "X25519 and Ed25519 keys do not match", m->ed25519, m->join_sig, NULL);
        return 0;
    }
    if (binding) { *error = SQLITE_ERROR; return -1; }
    /* жёсткая привязка идентичности */
    uint64_t derived = sc_derive_node_id_from_pubkey(m->x25519);
    if (!derived) {
        *error = SQLITE_ERROR;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: local identity verification unavailable ns=%s", MS_ID, ch_id); return -1;
    }
    if (derived != m->node_id) {
        db_changes_warn(db, NULL, ch_id, "Member identity", "node ID does not match X25519 key", m->ed25519, m->join_sig, NULL);
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: verify_local — node_id/x25519 mismatch nid=0x%016llx ns=%s",
                    MS_ID, (unsigned long long)m->node_id, ch_id);
        return 0;
    }
    /* JOIN проверяется и при наличии профиля: профиль сам по себе не связывает запись с группой. */
    if (m->join_sig && !_sig_is_zero64(m->join_sig)) {
        int verified = member_verify_join_signature(db, ch_id, m, error);
        if (verified == MS_VERIFY_DEFERRED) { *error = SQLITE_NOTFOUND; return -1; }
        if (verified != 1) return verified == MS_VERIFY_ERROR ? -1 : 0;
    }
    /* update_sig присутствует — верифицируем */
    if (m->update_sig && !_sig_is_zero64(m->update_sig)) {
        uint8_t vmsg[8192];
        uint8_t id[32];
        *error = member_sync_join_id_read(db, ch_id, m->ed25519, m->x25519, id);
        if (*error) return -1;
        int vlen = member_sync_build_update_msg(id, m->update_ts,
            m->userinfo ? m->userinfo : "", vmsg, (int)sizeof(vmsg));
        if (vlen < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: invalid local update message ns=%s node=%016llx",
                        MS_ID, ch_id, (unsigned long long)m->node_id); return 0;
        }
        int verified = sc_ed25519_verify(m->ed25519, vmsg, (size_t)vlen, m->update_sig);
        if (verified != SC_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: local update verification ns=%s node=%016llx rc=%d",
                        MS_ID, ch_id, (unsigned long long)m->node_id, verified);
            if (verified != SC_ERR_AUTH_FAILED) *error = SQLITE_ERROR;
            else db_changes_warn(db, NULL, ch_id, "Member profile", "invalid signature", m->ed25519, m->update_sig, NULL);
            return verified == SC_ERR_AUTH_FAILED ? 0 : -1;
        }
    }

    int owner = member_verify_owner_ex(db, ch_id, m, error);
    if (owner == MS_VERIFY_DEFERRED) { *error = SQLITE_NOTFOUND; return -1; }
    if (owner == MS_VERIFY_ERROR) return -1;
    if (!owner) return 0;

    /* дерево приглашений: подпись есть и невалидна (подписант резолвится) — битая запись.
       -1 = родитель ещё не пришёл — не трогаем (не битая). */
    if (m->signature && !_sig_is_zero64(m->signature)) {
        int sv = member_verify_signature_ex(db, ch_id, m, error);
        if (sv == MS_VERIFY_ERROR) return -1;
        if (sv == 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: verify_local — invalid tree-signature nid=0x%016llx signed_by=%s ns=%s",
                        MS_ID, (unsigned long long)m->node_id, sc_public_key_hex(m->signed_by, (char[65]){0}), ch_id);
            return 0;
        }
    }
    return 1;
}
int member_sync_verify_local_record(sqlite3* db, const char* channel, const struct ms_member_rec* record) {
    int error; return member_sync_verify_local_record_ex(db, channel, record, &error);
}

/* Строка без JOIN может содержать подписанный блок владельца. */
static int member_saved_verify(sqlite3* db, const char* channel, const struct member_saved* saved, int projection, int* code) {
    *code = SQLITE_OK;
    if (saved->malformed & (projection ? ~0u : ~MEMBER_BAD_PROJECTION)) return 0;
    if (!saved->source) {
        struct ms_member_rec record = saved->rec;
        if (!projection) record.signature = NULL;
        return member_sync_verify_local_record_ex(db, channel, &record, code);
    }
    int valid = member_verify_owner_ex(db, channel, &saved->rec, code);
    if (valid < 0 && !*code) *code = SQLITE_NOTFOUND;
    return valid;
}

struct member_repair_item { sqlite3_int64 row; uint8_t ed[32]; int retained; };

/* Владеет транзакцией канала; публикует удаления только после commit данных и индекса. */
int member_sync_verify_and_purge(struct UTUN_INSTANCE* inst, const char* ch_id) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "legacy member purge rejected in RW; use worker bootstrap prepare"); return -1;
    }
    sqlite3* db = _db(inst);
    size_t ns_len = ch_id ? strlen(ch_id) : 0;
    if (!db || !inst->msync || !chat_channel_valid(ch_id) || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge rejected ns=%s db=%p external_transaction=%d",
                    MS_ID, ch_id ? ch_id : "-", (void*)db, db ? !sqlite3_get_autocommit(db) : 0); return -1;
    }
    uint64_t group_id = topo_groups_channel_slot(inst->topo_groups, ch_id);
    char ns[70]; memcpy(ns, ch_id, ns_len + 1);
    char peers_tbl[128]; _peers_table(ns, peers_tbl, sizeof(peers_tbl));
    struct member_repair_item* items = NULL;
    uint32_t capacity = 0;
    int count = 0, rc = SQLITE_OK;
    const char* stage = "begin";
    sqlite3_stmt* st = NULL;
    struct group_policy_update* admission = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge begin failed ns=%s: %s", MS_ID, ns, sqlite3_errmsg(db)); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge begin ns=%s", MS_ID, ns);
    char sql[384]; snprintf(sql, sizeof(sql),
        "SELECT id FROM \"%s\" WHERE record_state=0 AND (source=0 OR adm_tags IS NOT NULL OR adm_tags_sig IS NOT NULL)"
        " AND length(x25519_pubkey)>0 AND length(ed25519_pubkey)>0 ORDER BY id", peers_tbl);
    stage = "read";
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct member_saved saved;
        sqlite3_int64 row = sqlite3_column_int64(st, 0);
        int found = member_read_saved_row(db, ns, row, NULL, 0, &saved);
        if (found != 1) { rc = found < 0 ? -found : SQLITE_CORRUPT; goto fail; }
        int verified = member_saved_verify(db, ns, &saved, 1, &rc);
        if (verified < 0) { stage = "verify"; goto fail; }
        if (verified == 0) {
            if (count == INT_MAX || (uint32_t)count == capacity) {
                if (count == INT_MAX || capacity > UINT32_MAX / sizeof(*items) / 2) {
                    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge list too large ns=%s count=%d", MS_ID, ns, count);
                    stage = "allocate"; rc = SQLITE_TOOBIG; goto fail;
                }
                uint32_t next = capacity ? capacity * 2 : 8;
                struct member_repair_item* ids = u_realloc(items, next * sizeof(*items));
                if (!ids) {
                    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge allocation failed ns=%s count=%d", MS_ID, ns, count);
                    stage = "allocate"; rc = SQLITE_NOMEM; goto fail;
                }
                items = ids; capacity = next;
            }
            items[count++] = (struct member_repair_item){.row=row};
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge selected ns=%s node=%016llx malformed=%d",
                       MS_ID, ns, (unsigned long long)saved.rec.node_id, saved.malformed);
        }
    }
    if (rc != SQLITE_DONE) goto fail;
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    stage = "repair";
    int deleted = 0;
    for (int i = 0; i < count; i++) {
        struct member_saved saved;
        int found = member_read_saved_row(db, ns, items[i].row, NULL, 0, &saved);
        if (found != 1) { rc = found < 0 ? -found : SQLITE_CORRUPT; goto fail; }
        memcpy(items[i].ed, saved.ed, 32);
        rc = member_repair_invalid(db, ns, &saved, &items[i].retained);
        if (rc) goto fail;
        if (!items[i].retained) deleted++;
    }
    stage = "index";
    if (count && (rc = member_rebuild_index(db, ns))) goto fail;
    stage = "admission snapshot";
    if (count) {
        int projected = group_policy_project(db, inst->node_id, ns, &admission);
        if (projected < 0) { rc = -projected; goto fail; }
    }
    stage = "commit";
    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (admission) { group_policy_publish(inst, admission); admission = NULL; }
    if (count) {
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge committed ns=%s repaired=%d removed=%d", MS_ID, ns, count - deleted, deleted);
        merkle_sync_changed(inst, MT_DATA_MEMBERS, ns);
    }
    for (int i = 0; i < count; i++) {
        if (items[i].retained) { _fire_props_changed(inst, items[i].ed, ns); continue; }
        radio_forget_member_mute(inst, group_id, items[i].ed);
        struct TOPO_GROUP* g = inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
        struct ETCP_CONN* conn = member_sync_signer(items[i].ed) ? instance_find_conn_by_ed(inst, items[i].ed) : NULL;
        if (g && g->group_type == TOPO_GROUP_TYPE_CHAT && conn) topo_group_remove_conn(g, conn, TOPO_REMOVE_MEMBER_INVALID);
        _fire_props_changed(inst, items[i].ed, ns);
    }
    u_free(items);
    return deleted;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge aborted ns=%s stage=%s selected=%d rc=%d: %s",
                MS_ID, ns, stage, count, rc, sqlite3_errmsg(db));
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge finalize ns=%s rc=%d", MS_ID, ns, rc);
    group_policy_discard(admission);
    if (!sqlite3_get_autocommit(db)) {
        rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge rollback failed ns=%s rc=%d: %s", MS_ID, ns, rc, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) {
                inst->running = 0;
                DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: database transaction unrecovered; application restart required", MS_ID);
            }
        }
    }
    u_free(items);
    return -1;
}

/* Обходит каналы без живого SQLite cursor во время публикации очистки. */
int member_sync_verify_and_purge_all(struct UTUN_INSTANCE* inst) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "legacy member purge-all rejected in RW; use worker bootstrap prepare"); return -1;
    }
    sqlite3* db = _db(inst);
    if (!db || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: invalid purge-all context", MS_ID); return -1;
    }
    char last[128] = "";
    sqlite3_stmt* st = NULL;
    for (;;) {
        int rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name GLOB 'peers_*'"
            " AND name>? ORDER BY name LIMIT 1", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, last, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const char* table = (const char*)sqlite3_column_text(st, 0);
            int len = sqlite3_column_bytes(st, 0);
            if (!table || len < 7 || len >= (int)sizeof(last)) rc = SQLITE_CORRUPT;
            else memcpy(last, table, (size_t)len + 1);
        }
        int final_rc = sqlite3_finalize(st); st = NULL;
        if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final_rc != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: purge-all read failed rc=%d finalize=%d: %s",
                        MS_ID, rc, final_rc, sqlite3_errmsg(db)); return -1;
        }
        if (rc == SQLITE_DONE) return 0;
        if (member_sync_verify_and_purge(inst, last + 6) < 0) return -1;
    }
}

struct member_wire {
    struct ms_member_rec rec;
    struct group_invitation_candidate invite;
    uint8_t x[32];
    char text[MS_ADM_TAGS_MAX + 1];
};

struct member_pull {
    char channel[65];
    sqlite3_int64 id;
    uint64_t round, generation, registration;
    uint8_t peer[32];
    size_t records, count, capacity, audit_count, audit_capacity;
    merkle_key_t* changed;
    struct group_invitation_candidate* audit;
    struct group_policy_update* admission;
    struct group_policy_cache* profiles;
    EVP_MD_CTX* source_leaf;
    int admin, source_nonempty, committed, graph_changed, info_changed, policy_changed;
};

/* Кодировка одного факта; подписи проверяются отдельно от сборки графа. */
static int member_decode_item(const merkle_key_t* key, const uint8_t* p, size_t len, struct member_wire* w) {
    unsigned kind = key->bytes[0];
    if (kind < MEMBER_ITEM_JOIN || kind > MEMBER_ITEM_INVITE) return SQLITE_MISMATCH;
    size_t fixed = kind == MEMBER_ITEM_JOIN ? MEMBER_JOIN_WIRE_FIXED : kind == MEMBER_ITEM_OWNER ? MEMBER_OWNER_WIRE_FIXED : MEMBER_INVITE_WIRE_SIZE;
    if (len < fixed) return SQLITE_MISMATCH;
    merkle_key_t expected;
    int rc = member_item_key(kind, p, &expected);
    if (rc || memcmp(key->bytes, expected.bytes, 32)) return rc ? rc : SQLITE_MISMATCH;
    memset(w, 0, sizeof(*w));
    if (kind == MEMBER_ITEM_INVITE) {
        if (len != fixed) return SQLITE_MISMATCH;
        memcpy(w->invite.subject, p, 32); memcpy(w->invite.signer, p + 32, 32);
        w->invite.stamp = member_read64(p + 64); memcpy(w->invite.signature, p + 72, 64);
        return SQLITE_OK;
    }
    size_t text = (size_t)p[fixed - 2] * 256 + p[fixed - 1];
    size_t limit = kind == MEMBER_ITEM_JOIN ? MS_USERINFO_MAX : MS_ADM_TAGS_MAX;
    if (text != len - fixed || text > limit || memchr(p + fixed, 0, text)) return SQLITE_MISMATCH;
    memcpy(w->text, p + fixed, text);
    w->rec.ed25519 = p;
    if (kind == MEMBER_ITEM_JOIN) {
        if (_sig_is_zero64(p + 64)) return SQLITE_AUTH;
        w->rec.x25519 = p + 32; w->rec.join_sig = p + 64;
        w->rec.update_ts = member_read64(p + 128); w->rec.update_sig = p + 136; w->rec.userinfo = w->text;
    } else {
        if (!text || _sig_is_zero64(p + 32)) return SQLITE_AUTH;
        rc = sc_identity_public_x("MEMBERS owner subject", p, w->x);
        if (rc) return rc == SC_ERR_AUTH_FAILED ? SQLITE_AUTH : SQLITE_ERROR;
        w->rec.x25519 = w->x; w->rec.adm_tags_sig = p + 32; w->rec.adm_tags = w->text;
    }
    w->rec.node_id = sc_derive_node_id_from_pubkey(w->rec.x25519);
    return w->rec.node_id ? SQLITE_OK : SQLITE_ERROR;
}

/* Каждая подписанная часть переживает повреждение соседних полей. */
static int member_repair_invalid(sqlite3* db, const char* channel, const struct member_saved* saved, int* retained) {
    int code = SQLITE_OK, keep_join = 0, keep_profile = 0, keep_owner = 0;
    struct ms_member_rec check = saved->rec;
    check.signature = NULL; check.adm_tags = NULL; check.adm_tags_sig = NULL;
    if (!saved->source && !(saved->malformed & MEMBER_BAD_IDENTITY) && !_sig_is_zero64(saved->join) &&
        sc_derive_node_id_from_pubkey(saved->x) == saved->rec.node_id) {
        keep_join = member_verify_join_ex(db, channel, &check, &code);
        if (keep_join < 0) return code ? code : SQLITE_NOTFOUND;
        if (keep_join && !(saved->malformed & MEMBER_BAD_PROFILE)) {
            keep_profile = member_sync_verify_local_record_ex(db, channel, &check, &code);
            if (keep_profile < 0) return code;
        }
    }
    struct member_owner_fields owner = {0};
    if (!(saved->malformed & MEMBER_BAD_OWNER) && saved->tags[0]) {
        keep_owner = member_verify_owner_ex(db, channel, &saved->rec, &code);
        if (keep_owner < 0) return code ? code : SQLITE_NOTFOUND;
        if (keep_owner) {
            code = member_owner_read(db, saved->tags, &owner);
            if (code) return code;
        }
    }
    *retained = saved->source || keep_join || keep_owner;
    char sql[640];
    if (*retained) {
        snprintf(sql, sizeof(sql), "UPDATE peers_%s SET %s%s%s"
            "signed_by=NULL,signature=NULL,join_ts=0,deleted=%d,storage=%d WHERE id=? AND record_state=0",
            channel, keep_profile ? "" : "update_ts=0,update_sig=NULL,userinfo='',",
            keep_owner ? "" : "adm_tags=NULL,adm_tags_sig=NULL,",
            keep_join ? "" : "source=1,admitted=0,identity_verified=0,x25519_pubkey=zeroblob(32),join_sig=zeroblob(64),",
            owner.deleted, keep_join && owner.storage);
    } else snprintf(sql, sizeof(sql), "DELETE FROM peers_%s WHERE id=? AND record_state=0", channel);
    sqlite3_stmt* st = NULL;
    code = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!code) code = sqlite3_bind_int64(st, 1, saved->row);
    if (!code) code = sqlite3_step(st);
    if (code == SQLITE_DONE) code = sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT;
    int final = sqlite3_finalize(st); if (!code) code = final;
    DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS repair ch=%s ed=%s row=%lld keep_join=%d keep_profile=%d keep_owner=%d rc=%d",
        channel, sc_public_key_hex(saved->ed, (char[65]){0}), (long long)saved->row, keep_join, keep_profile, keep_owner, code);
    return code;
}

int member_sync_recovery_source_read(sqlite3* db, const char* channel, const uint8_t ed[32], const uint8_t x[32], int* valid) {
    if (valid) *valid = 0;
    if (!db || !chat_channel_valid(channel) || !ed || !x || !valid) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member recovery proof invalid database/channel/key/output"); return SQLITE_MISUSE;
    }
    struct member_saved saved; int found = member_read_saved_row(db, channel, 0, ed, 0, &saved);
    if (found < 0) return -found;
    if (!found || saved.source || (saved.malformed & MEMBER_BAD_IDENTITY) || memcmp(saved.x, x, 32) || sc_derive_node_id_from_pubkey(saved.x) != saved.rec.node_id) return SQLITE_OK;
    int code = SQLITE_OK;
    int verified = member_verify_join_ex(db, channel, &saved.rec, &code);
    if (verified != 1) return code;
    sqlite3_stmt* st = NULL;
    code = sqlite3_prepare_v2(db, "SELECT signer,stamp,signature FROM member_invitations WHERE channel=? AND subject=?", -1, &st, NULL);
    if (!code) code = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!code) code = sqlite3_bind_blob(st, 2, ed, 32, SQLITE_STATIC);
    if (!code) while ((code = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t *signer, *sig;
        code = member_record_blob(st, 0, 32, 0, &signer);
        if (!code) code = member_record_blob(st, 2, 64, 0, &sig);
        if (code) break;
        struct group_invitation_candidate invite = {.stamp=(uint64_t)sqlite3_column_int64(st, 1)};
        memcpy(invite.subject, ed, 32); memcpy(invite.signer, signer, 32); memcpy(invite.signature, sig, 64);
        int rejected;
        code = member_invitation_check(db, channel, &invite, &rejected);
        if (!code) { *valid = 1; break; }
        if (!rejected) break;
    }
    int final = sqlite3_finalize(st);
    return code && code != SQLITE_DONE ? code : final;
}

/* Явно удаляем строки и для SQLite-соединений без foreign_keys. */
static int member_pull_delete(sqlite3* db, sqlite3_int64 id) {
    char sql[220];
    snprintf(sql, sizeof(sql), "DELETE FROM member_sync_records WHERE receive_id=%lld;"
        "DELETE FROM member_sync_receives WHERE id=%lld", (long long)id, (long long)id);
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void member_pull_free(void* state) {
    struct member_pull* pull = state;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "member receive released ch=%s peer=%s sync=%lld round=%llu records=%llu committed=%d",
        pull->channel, sc_public_key_hex(pull->peer, (char[65]){0}), (long long)pull->id, (unsigned long long)pull->round,
        (unsigned long long)pull->records, pull->committed);
    EVP_MD_CTX_free(pull->source_leaf);
    group_policy_discard(pull->admission);
    group_policy_snapshot_free(pull->profiles);
    u_free(pull->changed);
    u_free(pull->audit);
    u_free(pull);
}

static int member_pull_rollback(struct UTUN_INSTANCE* inst, const char* channel, int result, int code) {
    sqlite3* db = _db(inst);
    if (code) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member receive transaction ch=%s rc=%d (%s)", channel, code, sqlite3_errstr(code));
    if (!sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member receive rollback failed ch=%s: %s", channel, sqlite3_errmsg(db));
        utun_instance_stop(inst);
    }
    return result < 0 ? result : MT_ERR_IO;
}

static void member_abort_pull(void* ctx, void* state) {
    struct UTUN_INSTANCE* inst = ctx;
    struct member_pull* pull = state;
    if (!pull->committed) {
        sqlite3* db = _db(inst);
        int code = db && sqlite3_get_autocommit(db) ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
        if (!code) {
            code = member_pull_delete(db, pull->id);
            if (!code) code = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
            if (code) member_pull_rollback(inst, pull->channel, MT_ERR_IO, code);
        } else DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member receive cleanup ch=%s sync=%lld rc=%d",
            pull->channel, (long long)pull->id, code);
    }
    member_pull_free(pull);
}

struct member_sql_scope {
    struct member_sync_write_context writer;
    uint64_t generation, registration;
    int admin;
};

/* Снимок поколения берётся в uasync, SQL worker видит только собственные поля. */
static void member_scope_init(struct member_sql_scope* scope, struct UTUN_INSTANCE* inst, const char* channel) {
    member_writer_init(&scope->writer, inst);
    scope->generation = inst->group_generation;
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel);
    scope->registration = group ? group->generation : 0;
}

static int member_begin_write(sqlite3* db, const struct member_sql_scope* scope, const char* channel,
    const uint8_t peer[32], uint64_t round, void** out) {
    *out = NULL;
    if (!chat_channel_valid(channel) || sqlite3_get_autocommit(db)) return SQLITE_MISUSE;
    struct member_pull* pull = u_calloc(1, sizeof(*pull));
    if (!pull) return SQLITE_NOMEM;
    int rc = sqlite3_exec(db, "INSERT INTO member_sync_receives DEFAULT VALUES", NULL, NULL, NULL);
    if (rc) { u_free(pull); return rc; }
    pull->id = sqlite3_last_insert_rowid(db);
    pull->round = round;
    pull->generation = scope->generation;
    pull->registration = scope->registration;
    pull->admin = scope->admin;
    strcpy(pull->channel, channel);
    memcpy(pull->peer, peer, sizeof(pull->peer));
    DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "member receive begin ch=%s peer=%s sync=%lld round=%llu",
        channel, sc_public_key_hex(peer, (char[65]){0}), (long long)pull->id, (unsigned long long)round);
    *out = pull;
    return SQLITE_OK;
}

static int member_view_begin(void* ctx, const char* channel, const uint8_t peer[32], uint64_t round, void** out, int admin) {
    struct UTUN_INSTANCE* inst = ctx;
    sqlite3* db = _db(inst);
    *out = NULL;
    if (!db || !sqlite3_get_autocommit(db) || !chat_channel_valid(channel)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member receive begin requires channel and own WRITE");
        return MT_ERR_IO;
    }
    struct member_sql_scope scope = {.admin=admin};
    member_scope_init(&scope, inst, channel);
    int code = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (!code) code = member_begin_write(db, &scope, channel, peer, round, out);
    if (!code) code = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (!code) return MT_OK;
    if (*out) { member_pull_free(*out); *out = NULL; }
    return member_pull_rollback(inst, channel, MT_ERR_IO, code);
}

static int member_begin_pull(void* ctx, const char* channel, const uint8_t peer[32], uint64_t round, void** out) {
    return member_view_begin(ctx, channel, peer, round, out, 0);
}

static int member_admin_begin(void* ctx, const char* channel, const uint8_t peer[32], uint64_t round, void** out) {
    return member_view_begin(ctx, channel, peer, round, out, 1);
}

static int member_group_decode(const merkle_key_t* key, const uint8_t* wire, size_t len,
    struct group_policy* policy, struct group_info_record* info) {
    if (memcmp(key->bytes, (const uint8_t[32]){0}, 32) || len <= GROUP_POLICY_WIRE_SIZE ||
        group_info_decode(info, wire + GROUP_POLICY_WIRE_SIZE, len - GROUP_POLICY_WIRE_SIZE)) return SQLITE_MISMATCH;
    group_policy_decode(wire, policy);
    return SQLITE_OK;
}

static int member_view_group(const merkle_key_t* key, const uint8_t* wire, size_t bytes, int admin,
    struct group_policy* policy, struct group_info_record* info) {
    if (!admin) return member_group_decode(key, wire, bytes, policy, info);
    if (memcmp(key->bytes, (const uint8_t[32]){0}, 32) || bytes != GROUP_POLICY_WIRE_SIZE) return SQLITE_MISMATCH;
    group_policy_decode(wire, policy);
    return SQLITE_OK;
}

static int member_item_verify(sqlite3* db, const struct member_pull* pull, const merkle_key_t* key, const uint8_t* wire, size_t bytes,
    int* rejected) {
    const char* channel = pull->channel;
    if (pull->admin && member_admin_size(key->bytes[0], wire, bytes) != bytes) {
        *rejected = 1; return SQLITE_AUTH;
    }
    int rc, valid;
    if (key->bytes[0] == MEMBER_ITEM_GROUP) {
        struct group_policy policy; struct group_info_record info; enum group_info_decision decision;
        rc = member_view_group(key, wire, bytes, pull->admin, &policy, &info);
        if (!rc) rc = group_policy_check(db, channel, &policy, &valid);
        if (!rc && !valid) {
            db_changes_warn(db, NULL, channel, "Group policy", "invalid group signature", NULL, policy.signature, NULL);
            *rejected = 1; return SQLITE_AUTH;
        }
        if (!rc && !pull->admin) {
            rc = group_info_check(db, channel, &info, &decision, NULL);
            if (!rc && decision == GROUP_INFO_REJECTED) {
                db_changes_warn(db, NULL, channel, "Group description", "invalid signed description", NULL, info.signature, NULL);
                *rejected = 1; return SQLITE_AUTH;
            }
        }
        return rc;
    }
    struct member_wire decoded;
    rc = member_decode_item(key, wire, bytes, &decoded);
    if (rc) { *rejected = rc == SQLITE_AUTH; return rc; }
    if (key->bytes[0] == MEMBER_ITEM_INVITE) {
        return member_invitation_check(db, channel, &decoded.invite, rejected);
    }
    valid = member_sync_verify_local_record_ex(db, channel, &decoded.rec, &rc);
    *rejected = valid == 0;
    return valid == 1 ? SQLITE_OK : rc ? rc : SQLITE_AUTH;
}

static int member_stage_write(sqlite3* db, struct merkle_sync_sql_request* request) {
    struct member_pull* pull = request->pull;
    const uint8_t* data = request->data;
    size_t len = request->length, off = MEMBER_PAGE_HEADER;
    const char* stage = "page header"; int rc = SQLITE_MISMATCH, rejected = 0;
    merkle_key_t last = {{0}}, previous = request->after;
    if (len < MEMBER_PAGE_HEADER || len > MT_PAGE_SIZE || data[0] != MEMBER_PAGE_VERSION) goto fail;
    unsigned count = (unsigned)data[1] * 256 + data[2];
    if (count > (len - off) / MEMBER_ITEM_HEADER) goto fail;
    if (request->verify_source && !request->after_valid) {
        stage = "leaf digest"; rc = SQLITE_ERROR;
        if (!pull->source_leaf) pull->source_leaf = EVP_MD_CTX_new();
        if (!pull->source_leaf || !merkle_tree_digest_begin(pull->source_leaf, MT_MAX_LEVEL)) goto fail;
        pull->source_nonempty = 0;
    }
    for (unsigned i = 0; i < count; i++) {
        stage = "item framing"; rc = SQLITE_MISMATCH;
        if (len - off < MEMBER_ITEM_HEADER) goto fail;
        memcpy(last.bytes, data + off, 32); off += 32;
        size_t bytes = (size_t)data[off] * 256 + data[off + 1]; off += 2;
        if (!bytes || bytes > len - off || merkle_sync_level_prefix(member_read64(last.bytes), MT_MAX_LEVEL) != request->prefix ||
            ((i || request->after_valid) && memcmp(last.bytes, previous.bytes, 32) <= 0)) goto fail;
        previous = last;
        stage = "item verification";
        rc = member_item_verify(db, pull, &last, data + off, bytes, &rejected);
        if (rc) goto fail;
        stage = "item staging";
        sqlite3_stmt* st = NULL;
        rc = sqlite3_prepare_v2(db, "INSERT INTO member_sync_records(receive_id,key,wire)"
            " SELECT ?1,?2,?3 WHERE EXISTS(SELECT 1 FROM member_sync_receives WHERE id=?1)", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_int64(st, 1, pull->id);
        if (!rc) rc = sqlite3_bind_blob(st, 2, last.bytes, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 3, data + off, (int)bytes, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_DONE && sqlite3_changes(db) != 1) rc = SQLITE_ABORT;
        int final = sqlite3_finalize(st);
        if (rc == SQLITE_DONE) rc = final;
        if (rc) goto fail;
        if (request->verify_source) {
            stage = "item digest"; rc = SQLITE_ERROR;
            if (!pull->source_leaf || !member_hash_item(pull->source_leaf, last.bytes, data + off, bytes)) goto fail;
            pull->source_nonempty = 1;
        }
        off += bytes;
    }
    stage = "page cursor"; rc = SQLITE_MISMATCH;
    if (off != len || memcmp(request->next.bytes, last.bytes, 32) || (!count && request->more)) goto fail;
    if (request->verify_source && !request->more) {
        uint8_t hash[MT_HASH_SIZE];
        stage = "leaf digest"; rc = SQLITE_ERROR;
        if (!pull->source_leaf || EVP_DigestFinal_ex(pull->source_leaf, hash, NULL) != 1) goto fail;
        if (!pull->source_nonempty) memset(hash, 0, sizeof(hash));
        stage = "leaf proof"; rc = SQLITE_AUTH;
        if (memcmp(hash, request->source_hash, sizeof(hash))) {
            db_changes_warn_hash(db, NULL, pull->channel, "Members Merkle page", "leaf hash mismatch", NULL, NULL, request->source_hash, hash);
            rejected = 1; goto fail;
        }
    }
    pull->records += count;
    request->result = MT_OK;
    return SQLITE_OK;
fail:
    if (rc == SQLITE_MISMATCH)
        db_changes_warn(db, NULL, pull->channel, "Members Merkle page", stage, last.bytes, NULL, NULL);
    DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS stage refused ch=%s peer=%s round=%llu stage=%s prefix=%016llx key=%s rc=%d",
        pull->channel, sc_public_key_hex(pull->peer, (char[65]){0}), (unsigned long long)pull->round, stage,
        (unsigned long long)request->prefix, sc_public_key_hex(last.bytes, (char[65]){0}), rc);
    request->result = rejected ? MT_ERR_CONFLICT : rc == SQLITE_MISMATCH ? MT_ERR_DATA : MT_ERR_IO;
    return rc;
}

static int member_stage_request(void* ctx, struct merkle_sync_sql_request* request) {
    struct UTUN_INSTANCE* inst = ctx;
    sqlite3* db = _db(inst);
    struct member_pull* pull = request->pull;
    if (!db || !pull || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member stage requires pull and own WRITE");
        return MT_ERR_IO;
    }
    if (pull->generation != inst->group_generation || !member_registration_current(inst, pull->channel, pull->registration)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS stage discarded after group replacement ch=%s round=%llu",
            pull->channel, (unsigned long long)pull->round);
        return MT_ERR_DISCONNECTED;
    }
    int code = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (!code) code = member_stage_write(db, request);
    if (!code) code = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    return code ? member_pull_rollback(inst, pull->channel, request->result, code) : request->result;
}

/* Список ID копируется до изменения строк; NULL при пустом результате. */
static int member_receive_ids(sqlite3* db, const char* sql, uint64_t** out, size_t* count) {
    sqlite3_stmt* st = NULL; uint64_t* ids = NULL; size_t n = 0, capacity = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == capacity) {
            size_t next = capacity ? capacity * 2 : 32;
            if (next > UINT32_MAX / sizeof(*ids)) { rc = SQLITE_TOOBIG; break; }
            uint64_t* resized = u_realloc(ids, next * sizeof(*ids));
            if (!resized) { rc = SQLITE_NOMEM; break; } ids = resized; capacity = next;
        }
        ids[n++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        int code = rc == SQLITE_DONE ? final : rc;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member receive ID snapshot failed rc=%d finalize=%d (%s)", code, final, sqlite3_errstr(code));
        u_free(ids); return code;
    }
    *out = ids; *count = n; return 0;
}

static int member_delete_rows(sqlite3* db, const char* sql, const char* key) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    return rc == SQLITE_DONE ? final : rc;
}

static int member_rebuild_index(sqlite3* db, const char* channel) {
    char tree[70], sql[256]; sqlite3_stmt* st = NULL;
    snprintf(tree, sizeof(tree), "%u:%s", MT_DATA_MEMBERS, channel);
    int rc = member_delete_rows(db, "DELETE FROM merkle_tree_hash WHERE namespace=?", tree);
    snprintf(tree, sizeof(tree), "%u:%s", MT_DATA_MEMBER_ADMIN, channel);
    if (!rc) rc = member_delete_rows(db, "DELETE FROM merkle_tree_hash WHERE namespace=?", tree);
    if (!rc) rc = member_delete_rows(db, "DELETE FROM member_sync_items WHERE channel=?", channel);
    if (!rc) {
        int indexed = member_sync_reindex(db, channel, NULL);
        if (indexed < 0) rc = -indexed;
    }
    snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey FROM peers_%s WHERE record_state=0 AND (source=0 OR adm_tags IS NOT NULL)", channel);
    if (!rc) rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t* ed;
        rc = member_record_blob(st, 0, 32, 0, &ed);
        if (rc) break;
        int indexed = member_sync_reindex(db, channel, ed);
        if (indexed < 0) { rc = -indexed; break; }
    }
    int final = sqlite3_finalize(st); st = NULL;
    if (rc == SQLITE_DONE) rc = final;
    /* Фиксируем результат SELECT до удаления повреждённых строк из этой же таблицы. */
    if (!rc) rc = sqlite3_prepare_v2(db, "WITH stored AS MATERIALIZED ("
        "SELECT subject,signer,stamp,signature FROM member_invitations WHERE channel=?) SELECT * FROM stored", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t *subject, *signer, *signature;
        rc = member_record_blob(st, 0, 32, 0, &subject);
        if (!rc) rc = member_record_blob(st, 1, 32, 0, &signer);
        if (!rc) rc = member_record_blob(st, 3, 64, 0, &signature);
        if (rc) break;
        struct group_invitation_candidate invite = {.stamp=(uint64_t)sqlite3_column_int64(st, 2)};
        memcpy(invite.subject, subject, 32); memcpy(invite.signer, signer, 32); memcpy(invite.signature, signature, 64);
        int rejected;
        rc = member_invitation_check(db, channel, &invite, &rejected);
        if (rejected) {
            DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS removing corrupt invitation ch=%s subject=%s signer=%s", channel,
                sc_public_key_hex(subject, (char[65]){0}), sc_public_key_hex(signer, (char[65]){0}));
            sqlite3_stmt* remove = NULL;
            rc = sqlite3_prepare_v2(db, "DELETE FROM member_invitations WHERE channel=? AND subject=? AND signer=?", -1, &remove, NULL);
            if (!rc) rc = sqlite3_bind_text(remove, 1, channel, -1, SQLITE_STATIC);
            if (!rc) rc = sqlite3_bind_blob(remove, 2, invite.subject, 32, SQLITE_STATIC);
            if (!rc) rc = sqlite3_bind_blob(remove, 3, invite.signer, 32, SQLITE_STATIC);
            if (!rc) rc = sqlite3_step(remove);
            int closed = sqlite3_finalize(remove);
            rc = rc == SQLITE_DONE ? closed : rc;
        } else if (!rc) {
            int indexed = member_invitation_index(db, channel, &invite);
            if (indexed < 0) rc = -indexed;
        }
        if (rc) break;
    }
    final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS rebuild ch=%s rc=%d (%s)", channel, rc, sqlite3_errstr(rc));
    return rc;
}



/* Идентичности для проверки дерева и вычисления изменений читаются отдельно от локальных rowid. */
static int member_collect_change(struct member_pull* pull, const uint8_t ed[32]) {
    if (pull->count == pull->capacity) {
        size_t next = pull->capacity ? pull->capacity * 2 : 16;
        if (next > UINT32_MAX / sizeof(*pull->changed)) return SQLITE_TOOBIG;
        merkle_key_t* grown = u_realloc(pull->changed, next * sizeof(*grown));
        if (!grown) return SQLITE_NOMEM;
        pull->changed = grown; pull->capacity = next;
    }
    memcpy(pull->changed[pull->count++].bytes, ed, 32);
    return SQLITE_OK;
}

static int member_collect_audit(struct member_pull* pull, const struct group_invitation_candidate* invite) {
    if (pull->audit_count == pull->audit_capacity) {
        size_t next = pull->audit_capacity ? pull->audit_capacity * 2 : 4;
        if (next > UINT32_MAX / sizeof(*pull->audit)) return SQLITE_TOOBIG;
        struct group_invitation_candidate* grown = u_realloc(pull->audit, next * sizeof(*grown));
        if (!grown) return SQLITE_NOMEM;
        pull->audit = grown; pull->audit_capacity = next;
    }
    pull->audit[pull->audit_count++] = *invite;
    return SQLITE_OK;
}

static int member_merge_item(sqlite3* db, struct member_sync_write_context* context, struct member_pull* pull,
    const merkle_key_t* key, const uint8_t* wire, size_t bytes, int* rejected) {
    const char* channel = pull->channel;
    if (key->bytes[0] == MEMBER_ITEM_GROUP) {
        struct group_policy policy; struct group_info_record info; enum group_info_decision decision;
        int rc = member_view_group(key, wire, bytes, pull->admin, &policy, &info);
        if (rc) return rc;
        if (!pull->admin) {
            rc = group_info_apply_checked(db, channel, &info, &decision);
            if (rc) return rc;
            if (decision == GROUP_INFO_REJECTED) { *rejected = 1; return SQLITE_AUTH; }
            pull->info_changed = decision == GROUP_INFO_NEWER;
        }
        int applied = group_policy_apply(db, channel, &policy);
        if (applied < 0) return -applied;
        pull->policy_changed = applied;
        if (!applied) return SQLITE_OK;
        int indexed = member_sync_reindex(db, channel, NULL);
        return indexed < 0 ? -indexed : SQLITE_OK;
    }
    struct member_wire decoded;
    int rc = member_decode_item(key, wire, bytes, &decoded);
    if (rc) { *rejected = rc == SQLITE_AUTH; return rc; }
    if (key->bytes[0] == MEMBER_ITEM_INVITE) {
        int stored = member_invitation_put(db, channel, &decoded.invite);
        if (stored < 0) return -stored;
        if (!memcmp(context->public_key, decoded.invite.subject, 32) && (rc = member_collect_audit(pull, &decoded.invite))) return rc;
        pull->graph_changed |= stored != 0;
        return stored ? member_collect_change(pull, decoded.invite.subject) : SQLITE_OK;
    }
    struct member_saved previous;
    int found = member_read_saved_row(db, channel, 0, decoded.rec.ed25519, 0, &previous);
    if (found < 0) return -found;
    if (found) {
        int valid = member_saved_verify(db, channel, &previous, 0, &rc);
        if (valid < 0) return rc ? rc : SQLITE_NOTFOUND;
        if (!valid) {
            int retained;
            rc = member_repair_invalid(db, channel, &previous, &retained);
            if (rc) return rc;
            found = member_read_saved_row(db, channel, 0, decoded.rec.ed25519, 0, &previous);
            if (found < 0) return -found;
            rc = member_sync_reindex(db, channel, decoded.rec.ed25519);
            if (rc < 0) return -rc;
            rc = member_collect_change(pull, decoded.rec.ed25519);
            if (rc) return rc;
            pull->graph_changed = 1;
        }
    }
    int applied = member_apply_record(db, context, channel, &decoded.rec, found ? &previous : NULL, MEMBER_STAGE);
    if (applied < 0) return context->sqlite_code ? context->sqlite_code : SQLITE_ERROR;
    if (!(applied & MS_APPLY_CHANGED)) return SQLITE_OK;
    pull->graph_changed |= key->bytes[0] == MEMBER_ITEM_OWNER || !found || previous.source;
    return member_collect_change(pull, decoded.rec.ed25519);
}

static int member_key_order(const void* a, const void* b) { return memcmp(a, b, MT_KEY_SIZE); }

static int member_commit_pull_write(sqlite3* db, struct member_sync_write_context* context, struct member_pull* pull) {
    sqlite3_stmt* staged = NULL; const char* stage = "staged items";
    int rejected = 0;
    int rc = sqlite3_get_autocommit(db) ? SQLITE_MISUSE : sqlite3_prepare_v2(db,
        "SELECT key,wire FROM member_sync_records WHERE receive_id=? ORDER BY key", -1, &staged, NULL);
    if (!rc) rc = sqlite3_bind_int64(staged, 1, pull->id);
    if (!rc) while ((rc = sqlite3_step(staged)) == SQLITE_ROW) {
        const uint8_t *data, *wire; size_t bytes; merkle_key_t key;
        rc = member_item_columns(staged, &data, &wire, &bytes);
        if (!rc) { memcpy(key.bytes, data, sizeof(key.bytes)); rc = member_merge_item(db, context, pull, &key, wire, bytes, &rejected); }
        if (rc) break;
    }
    int final = sqlite3_finalize(staged);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) goto fail;
    stage = "admission";
    if (pull->graph_changed || pull->policy_changed || pull->info_changed) {
        rc = -group_policy_project(db, context->self, pull->channel, &pull->admission);
        if (rc) goto fail;
    }
    stage = "node directory";
    if (pull->count > 1) qsort(pull->changed, pull->count, sizeof(*pull->changed), member_key_order);
    size_t unique = 0;
    for (size_t i = 0; i < pull->count; i++) {
        const uint8_t* ed = pull->changed[i].bytes;
        if (unique && !memcmp(ed, pull->changed[unique - 1].bytes, 32)) continue;
        pull->changed[unique++] = pull->changed[i];
        struct member_saved saved;
        int found = member_read_saved_row(db, pull->channel, 0, ed, 0, &saved);
        if (found < 0) { rc = -found; goto fail; }
        if (found && !saved.source && topo_node_sqlite_member_key_allowed(db, pull->channel, saved.x, ed, 0)) {
            rc = member_project_node(db, &saved.rec, context->now);
            if (rc) goto fail;
        }
    }
    pull->count = unique;
    if (pull->count && !pull->admission) {
        stage = "profile snapshot";
        rc = group_policy_profiles_read(db, pull->channel, pull->changed, pull->count, &pull->profiles);
        if (rc) goto fail;
    }
    stage = "JOIN recovery completion";
    struct member_saved self;
    int have_self = member_read_saved_row(db, pull->channel, 0, context->public_key, 0, &self);
    if (have_self < 0) { rc = -have_self; goto fail; }
    if (have_self && topo_node_sqlite_member_key_allowed(db, pull->channel, self.x, self.ed, 0)) {
        rc = member_delete_rows(db, "DELETE FROM chat_join_recovery WHERE channel=?", pull->channel);
        if (rc) goto fail;
    }
    stage = "staging cleanup";
    rc = member_pull_delete(db, pull->id);
    if (rc) goto fail;
    context->sqlite_code = SQLITE_OK;
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS receive staged ch=%s admin=%d peer=%s round=%llu items=%zu subjects=%zu policy=%d",
        pull->channel, pull->admin, sc_public_key_hex(pull->peer, (char[65]){0}), (unsigned long long)pull->round, pull->records, pull->count,
        pull->policy_changed);
    return MT_OK;
fail:
    context->sqlite_code = rc;
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS merge refused ch=%s peer=%s round=%llu stage=%s rc=%d (%s)", pull->channel,
        sc_public_key_hex(pull->peer, (char[65]){0}), (unsigned long long)pull->round, stage, rc, sqlite3_errstr(rc));
    return rejected ? MT_ERR_CONFLICT : rc == SQLITE_MISMATCH ? MT_ERR_DATA : MT_ERR_IO;
}

static int member_commit_pull(void* ctx, void* state) {
    struct UTUN_INSTANCE* inst = ctx; sqlite3* db = _db(inst); struct member_pull* pull = state;
    if (!db || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member receive legacy commit requires own transaction"); return MT_ERR_IO;
    }
    if (pull->generation != inst->group_generation || !member_registration_current(inst, pull->channel, pull->registration)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBERS commit discarded after group replacement ch=%s round=%llu",
            pull->channel, (unsigned long long)pull->round);
        return MT_ERR_DISCONNECTED;
    }
    int code = sqlite3_exec(db, "BEGIN IMMEDIATE /* member receive */", NULL, NULL, NULL), result = MT_ERR_IO;
    struct member_sync_write_context context = {0};
    member_writer_init(&context, inst);
    if (!code) result = member_commit_pull_write(db, &context, pull);
    if (!result) code = sqlite3_exec(db, "COMMIT /* member receive */", NULL, NULL, NULL);
    if (!result && !code) { pull->committed = 1; return 0; }
    return member_pull_rollback(inst, pull->channel, result, code);
}

/* Контекст уже отсоединён от Merkle; отзыв допуска может свободно закрывать его сессию. */
static int member_pull_current(struct UTUN_INSTANCE* inst, const struct member_pull* pull) {
    if (inst->destroying || inst->group_generation != pull->generation ||
        (db_async_can_write(inst->db_worker) && !inst->group_started)) return 0;
    return member_registration_current(inst, pull->channel, pull->registration);
}

static void member_publish_pull(void* ctx, void* state) {
    struct UTUN_INSTANCE* inst = ctx;
    struct member_pull* pull = state;
    if (!pull->committed || !member_pull_current(inst, pull)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "member receive publication skipped ch=%s round=%llu committed=%d registration=%llu",
            pull->channel, (unsigned long long)pull->round, pull->committed, (unsigned long long)pull->registration);
        return;
    }
    const char* channel = pull->channel;
    for (size_t i = 0; i < pull->audit_count; i++) member_audit_invitation(inst, channel, &pull->audit[i], pull->peer);
    struct group_policy_update* admission = pull->admission;
    pull->admission = NULL;
    group_policy_publish_changes(inst, admission, 0);
    if (!member_pull_current(inst, pull)) return;
    if (pull->profiles) {
        group_policy_profiles_publish(inst, pull->profiles);
        pull->profiles = NULL;
    }
    if (pull->count || pull->policy_changed || pull->info_changed) merkle_sync_changed(inst, MT_DATA_MEMBERS, channel);
    if (!member_pull_current(inst, pull)) return;
    if (pull->count || pull->policy_changed) group_protocol_members_changed(inst, channel);
    if (pull->info_changed && member_pull_current(inst, pull)) group_info_publish_changes(inst, channel, 0);
    for (size_t i = 0; i < pull->count && member_pull_current(inst, pull); i++) {
        const uint8_t* ed = pull->changed[i].bytes;
        if (member_pull_current(inst, pull)) member_publish_record(inst, channel, ed, 0);
    }
    if (member_pull_current(inst, pull)) chat_sync_members_published(inst, channel);
}

static int member_view_snapshot(void* ctx, const char* channel, void** out, int admin) {
    struct UTUN_INSTANCE* inst = ctx; *out = NULL;
    if (!inst || inst->destroying || !inst->msync || !chat_channel_valid(channel)) return -1;
    struct member_sql_scope* scope = u_calloc(1, sizeof(*scope));
    if (!scope) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member SQL snapshot allocation ch=%s", channel); return -1; }
    member_scope_init(scope, inst, channel);
    scope->admin = admin;
    *out = scope; return 0;
}

static int member_sql_snapshot(void* ctx, const char* channel, void** out) {
    return member_view_snapshot(ctx, channel, out, 0);
}

static int member_admin_snapshot(void* ctx, const char* channel, void** out) {
    return member_view_snapshot(ctx, channel, out, 1);
}

static int member_sql_work(sqlite3* db, void* data, struct merkle_sync_sql_request* request) {
    struct member_sql_scope* scope = data; int code = SQLITE_OK, result = MT_OK;
    switch (request->action) {
    case MT_SQL_PAGE:
        request->length = request->capacity;
        result = member_view_page(db, request->ns, request->prefix, request->after_valid, &request->after,
            request->data, &request->length, &request->next, &request->more, scope->admin);
        if (result < 0) { code = -result; result = MT_ERR_IO; }
        break;
    case MT_SQL_BEGIN_PULL:
        code = member_begin_write(db, scope, request->ns, request->peer, request->round, &request->pull);
        break;
    case MT_SQL_STAGE_PAGE: return member_stage_write(db, request);
    case MT_SQL_COMMIT_PULL:
        result = member_commit_pull_write(db, &scope->writer, request->pull);
        code = scope->writer.sqlite_code;
        break;
    case MT_SQL_ABORT_PULL: {
        struct member_pull* pull = request->pull;
        code = pull->committed ? SQLITE_OK : member_pull_delete(db, pull->id);
        break;
    }
    case MT_SQL_FINISH: break;
    default: code = SQLITE_MISUSE; result = MT_ERR_DATA; break;
    }
    if (code && result == MT_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member SQL stage failed ch=%s action=%d rc=%d (%s)",
            request->ns, request->action, code, sqlite3_errstr(code));
        result = MT_ERR_IO;
    }
    request->result = result;
    if (result < 0 && !code) code = result == MT_ERR_IO ? SQLITE_ERROR : SQLITE_AUTH;
    return code;
}
static void member_sql_publish(void* ctx, void* scope, struct merkle_sync_sql_request* request) {
    (void)scope;
    if (request->action == MT_SQL_COMMIT_PULL && request->result == MT_OK) {
        struct member_pull* pull = request->pull; pull->committed = 1; member_publish_pull(ctx, pull);
    }
}
static void member_sql_free(void* scope) { u_free(scope); }
static const struct merkle_sync_sql_ops member_sql_ops = {.snapshot=member_sql_snapshot, .work=member_sql_work,
    .publish=member_sql_publish, .free=member_sql_free, .pull_free=member_pull_free};
static const struct merkle_sync_sql_ops member_admin_sql_ops = {.snapshot=member_admin_snapshot, .work=member_sql_work,
    .publish=member_sql_publish, .free=member_sql_free, .pull_free=member_pull_free};


static int _member_validate_peer(void* ctx, const char* ns, struct ETCP_CONN* conn) {
    struct UTUN_INSTANCE* inst = ctx;
    struct TOPO_GROUP* group = inst && ns ? topo_groups_find_channel(inst->topo_groups, ns) : NULL;
    return conn && conn->instance == inst && conn->initialized && conn->links_up && conn->state != 2 && !conn->close_requested &&
        topo_group_chat_connection_allowed(group, conn);
}

int member_sync_admin_allowed(struct UTUN_INSTANCE* inst, const char* channel, struct ETCP_CONN* conn) {
    struct TOPO_GROUP* group = inst ? topo_groups_find_channel(inst->topo_groups, channel) : NULL;
    return group && group->group_type == TOPO_GROUP_TYPE_CHAT && !group->stopping &&
        conn && conn->instance == inst && conn->initialized && conn->links_up && conn->state == 1 && !conn->close_requested &&
        group_policy_member_verified_key(inst, channel, inst->my_keys.public_key, inst->my_ed25519_pubkey) &&
        group_policy_member_verified_key(inst, channel, conn->crypto_ctx.peer_public_key, conn->peer_ed25519_pubkey);
}

static int member_admin_validate(void* ctx, const char* ns, struct ETCP_CONN* conn) {
    return member_sync_admin_allowed(ctx, ns, conn);
}

static void member_round_result(void* ctx, const char* channel, const uint8_t peer[32], int result) {
    struct UTUN_INSTANCE* inst = ctx;
    if (result == MT_OK && _member_validate_peer(inst, channel, instance_find_conn_by_ed(inst, peer)))
        group_protocol_peer_ready(inst, channel, peer);
}

/* Разрешение только спросить: канал выбран локально, ключ источника уже связан с его JOIN.
 * Никаких страниц/хешей в ответ этот режим не выдаёт; источник сам проверяет наш допуск. */
static int member_validate_request(void* ctx, const char* ns, struct ETCP_CONN* conn) {
    struct UTUN_INSTANCE* inst = ctx;
    if (!ns) return 0;
    struct TOPO_GROUP* group = inst && inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, ns) : NULL;
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT || group->stopping || strcmp(group->channel_id, ns) ||
        !conn || conn->instance != inst || !conn->initialized || !conn->links_up || conn->state == 2 || conn->close_requested) return 0;
    return group_policy_member_key_allowed(inst, ns, conn->crypto_ctx.peer_public_key, conn->peer_ed25519_pubkey, 0);
}


static const struct merkle_sync_data_ops g_member_ops = {
    .update_bucket_hash = _member_update_bucket_hash,
    .get_page           = _member_get_page,
    .validate_peer      = _member_validate_peer,
    .validate_request   = member_validate_request,
    .on_result          = member_round_result,
    .begin_pull         = member_begin_pull,
    .stage_page_request = member_stage_request,
    .group_scoped       = 1,
    .keep_transport     = 1,
    .commit_pull        = member_commit_pull,
    .publish_pull       = member_publish_pull,
    .abort_pull         = member_abort_pull,
    .sql                = &member_sql_ops,
};

static const struct merkle_sync_data_ops g_member_admin_ops = {
    .update_bucket_hash = member_admin_hash,
    .get_page           = member_admin_get_page,
    .validate_peer      = member_admin_validate,
    .begin_pull         = member_admin_begin,
    .stage_page_request = member_stage_request,
    .group_scoped       = 1,
    .keep_transport     = 1,
    .commit_pull        = member_commit_pull,
    .publish_pull       = member_publish_pull,
    .abort_pull         = member_abort_pull,
    .sql                = &member_admin_sql_ops,
};

static int member_admin_register(struct UTUN_INSTANCE* inst) {
    if (!merkle_sync_register(inst, MT_DATA_MEMBER_ADMIN, &g_member_admin_ops, inst)) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "MEMBER_ADMIN registration failed");
    merkle_sync_destroy(inst);
    return -1;
}

struct member_prepared_channel {
    struct member_prepared_channel* next;
    struct group_policy_cache* cache;
};
struct member_sync_prepared {
    uint64_t self;
    struct member_prepared_channel* channels;
    int activated;
};

void member_sync_prepared_free(struct member_sync_prepared* prepared) {
    if (!prepared) return;
    while (prepared->channels) {
        struct member_prepared_channel* channel = prepared->channels; prepared->channels = channel->next;
        group_policy_snapshot_free(channel->cache); u_free(channel);
    }
    u_free(prepared);
}

/* Старые helpers с0/-1 сохраняют SQLite connection error; семантическая ошибка становится ERROR. */
static int member_prepare_code(sqlite3* db) {
    int rc = sqlite3_extended_errcode(db);
    return rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_ERROR : rc;
}

static int member_prepare_channel(sqlite3* db, uint64_t self, const char* channel, struct group_policy_cache** out) {
    char sql[320]; sqlite3_stmt* st = NULL; uint64_t* ids = NULL; size_t count = 0;
    struct group_policy_update* admission = NULL; int code = SQLITE_OK; const char* stage = "member schema";
    if (topo_node_sqlite_member_local_init(db, channel)) { code = member_prepare_code(db); goto done; }
    stage = "abandoned versions";
    snprintf(sql, sizeof(sql), "DELETE FROM peers_%s WHERE record_state<>0", channel);
    code = sqlite3_exec(db, sql, NULL, NULL, NULL); if (code) goto done;
    if (sqlite3_changes(db)) DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "member startup removed abandoned versions ch=%s rows=%d",
        channel, sqlite3_changes(db));
    stage = "stored member list";
    snprintf(sql, sizeof(sql), "SELECT id FROM peers_%s WHERE record_state=0"
        " AND (source=0 OR adm_tags IS NOT NULL OR adm_tags_sig IS NOT NULL)"
        " AND length(x25519_pubkey)>0 AND length(ed25519_pubkey)>0 ORDER BY id", channel);
    code = member_receive_ids(db, sql, &ids, &count); if (code) goto done;
    stage = "stored signatures";
    for (size_t i = 0; i < count; i++) {
        struct member_saved saved;
        int found = member_read_saved_row(db, channel, ids[i], NULL, 0, &saved);
        if (found < 0) { code = -found; goto done; }
        if (!found) { code = SQLITE_CORRUPT; goto done; }
        int valid = member_saved_verify(db, channel, &saved, 1, &code);
        if (valid < 0) goto done;
        if (valid) continue;
        int retained;
        code = member_repair_invalid(db, channel, &saved, &retained); if (code) goto done;
    }
    stage = "member index";
    code = member_rebuild_index(db, channel);
    if (code) goto done;
    stage = "admission";
    int projected = group_policy_project(db, self, channel, &admission);
    if (projected < 0) code = -projected;
done:
    { int final = sqlite3_finalize(st); if (!code) code = final;
      else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member startup cleanup ch=%s rc=%d", channel, final); }
    if (!code) *out = group_policy_update_take_snapshot(admission);
    else DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member startup ch=%s stage=%s rc=%d: %s", channel, stage, code,
        (sqlite3_errcode(db) & 255) == (code & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(code));
    group_policy_discard(admission); u_free(ids); return code;
}

int member_sync_prepare(sqlite3* db, uint64_t self, struct member_sync_prepared** out) {
    if (out) *out = NULL;
    if (!db || !self || !out || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member startup requires database/self/output and caller WRITE"); return SQLITE_MISUSE;
    }
    struct member_sync_prepared* prepared = u_calloc(1, sizeof(*prepared));
    if (!prepared) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member startup snapshot allocation failed"); return SQLITE_NOMEM; }
    prepared->self = self;
    int code = SQLITE_OK; sqlite3_stmt* st = NULL; char last[sizeof("peers_") + 64] = "";
    if (group_policy_schema(db) || db_schema_table(db, "merkle_tree_hash", NULL)) {
        code = member_prepare_code(db); goto fail;
    }
    code = sqlite3_exec(db, "DELETE FROM member_sync_records; DELETE FROM member_sync_receives", NULL, NULL, NULL);
    if (code) goto fail;
    code = chat_feedback_prepare(db); if (code) goto fail;
    char indices[128];
    snprintf(indices, sizeof(indices), "DELETE FROM merkle_tree_hash WHERE namespace GLOB '%u:*' OR namespace GLOB '%u:*'",
        MT_DATA_MEMBERS, MT_DATA_MEMBER_ADMIN);
    code = sqlite3_exec(db, indices, NULL, NULL, NULL);
    if (code) goto fail;
    for (;;) {
        code = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name GLOB 'peers_*'"
            " AND name>? ORDER BY name LIMIT 1", -1, &st, NULL);
        if (!code) code = sqlite3_bind_text(st, 1, last, -1, SQLITE_STATIC);
        if (!code) code = sqlite3_step(st);
        int found = code == SQLITE_ROW;
        if (found) {
            const char* name = (const char*)sqlite3_column_text(st, 0); int size = sqlite3_column_bytes(st, 0);
            if (!name) code = SQLITE_NOMEM;
            else if (size != (int)sizeof(last) - 1 || !chat_channel_valid(name + 6)) code = SQLITE_CORRUPT;
            else { memcpy(last, name, (size_t)size + 1); code = SQLITE_OK; }
        } else if (code == SQLITE_DONE) code = SQLITE_OK;
        int final = sqlite3_finalize(st); st = NULL; if (!code) code = final;
        if (code) goto fail;
        if (!found) break;
        struct member_prepared_channel* channel = u_calloc(1, sizeof(*channel));
        if (!channel) { code = SQLITE_NOMEM; goto fail; }
        channel->next = prepared->channels; prepared->channels = channel;
        code = member_prepare_channel(db, self, last + 6, &channel->cache); if (code) goto fail;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "member startup prepared signed records, access snapshots and MEMBERS index");
    *out = prepared; return SQLITE_OK;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member startup failed table=%s rc=%d: %s", last, code,
        (sqlite3_errcode(db) & 255) == (code & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(code));
    sqlite3_finalize(st); member_sync_prepared_free(prepared); return code;
}

int member_sync_activate(struct UTUN_INSTANCE* inst, struct member_sync_prepared* prepared) {
    if (!inst || !prepared || prepared->activated || prepared->self != inst->node_id || inst->msync) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member startup activation: invalid owner/snapshot/state"); return -1;
    }
    if (merkle_sync_activate(inst, ETCP_ID_MEMBER_SYNC, &g_member_ops, inst)) return -1;
    if (member_admin_register(inst)) return -1;
    if (inst->topo_groups && inst->topo_groups->group_list) {
        for (struct ll_entry* entry = inst->topo_groups->group_list->head; entry; entry = entry->next) {
            struct TOPO_GROUP* group = (struct TOPO_GROUP*)entry;
            if (group->group_type == TOPO_GROUP_TYPE_CHAT && member_sync_subscribe_group(inst, group)) {
                member_sync_destroy(inst); return -1;
            }
        }
    }
    for (struct member_prepared_channel* channel = prepared->channels; channel; channel = channel->next) {
        group_policy_snapshot_publish(inst, channel->cache); channel->cache = NULL;
    }
    prepared->activated = 1;
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "member startup activated committed access and Merkle runtime without SQL"); return 0;
}

/* ──  Public API ── */

static int _rebuild_all_trees(struct UTUN_INSTANCE* inst) {
    sqlite3* db = _db(inst);
    sqlite3_stmt* tables = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'peers_%'", -1, &tables, NULL);
    if (rc != SQLITE_OK) goto fail;
    if (sqlite3_exec(db, "SAVEPOINT member_rebuild", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db, "DELETE FROM member_sync_records; DELETE FROM member_sync_receives", NULL, NULL, NULL);
    while (rc == SQLITE_OK && (rc = sqlite3_step(tables)) == SQLITE_ROW) {
        rc = SQLITE_OK;
        const char* table = (const char*)sqlite3_column_text(tables, 0);
        if (!table || strncmp(table, "peers_", 6)) continue;
        const char* ns = table + 6;
        if (topo_node_sqlite_member_local_init(db, ns)) {
            rc = SQLITE_ERROR; break;
        }
        char sql[256];
        snprintf(sql, sizeof(sql), "DELETE FROM \"%s\" WHERE record_state<>0", table);
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) { rc = SQLITE_ERROR; break; }
        if (sqlite3_changes(db)) DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "member receive abandoned versions removed ch=%s rows=%d",
            ns, sqlite3_changes(db));
        int indexed = member_rebuild_index(db, ns);
        if (indexed) { rc = indexed; break; }
    }
    if (rc != SQLITE_DONE) {
        if (sqlite3_exec(db, "ROLLBACK TO member_rebuild", NULL, NULL, NULL) != SQLITE_OK)
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: rebuild rollback failed: %s", MS_ID, sqlite3_errmsg(db));
    }
    int release = sqlite3_exec(db, "RELEASE member_rebuild", NULL, NULL, NULL);
    sqlite3_finalize(tables); tables = NULL;
    if (rc != SQLITE_DONE || release != SQLITE_OK) goto fail;
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: trees rebuilt from distinct leaves", MS_ID);
    return 0;
fail:
    sqlite3_finalize(tables);
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: tree rebuild failed: %s", MS_ID, db ? sqlite3_errmsg(db) : "no DB");
    return -1;
}

/* Подписаться на BGP node-события chat-группы (BGP → member_sync → node_props_changed). */
int member_sync_subscribe_group(struct UTUN_INSTANCE* inst, struct TOPO_GROUP* group) {
    if (!inst || !group || group->group_type != TOPO_GROUP_TYPE_CHAT) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "invalid group subscription"); return -1;
    }
    for (struct topo_node_cbk_entry* cb = group->node_cbks; cb; cb = cb->next)
        if (cb->fn == _ms_on_bgp_node && cb->arg == inst) return 0;
    topo_group_add_node_cbk(group, _ms_on_bgp_node, inst);
    for (struct topo_node_cbk_entry* cb = group->node_cbks; cb; cb = cb->next)
        if (cb->fn == _ms_on_bgp_node && cb->arg == inst) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "subscribed to BGP member changes ch=%s", group->channel_id);
            return 0;
        }
    DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "group subscription allocation failed ch=%s", group->channel_id); return -1;
}

int member_sync_init(struct UTUN_INSTANCE* inst) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "legacy member init rejected in RW; use prepare/activate"); return -1;
    }
    sqlite3* db = _db(inst);
    if (!db || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: init requires database outside transaction", MS_ID); return -1;
    }
    if (group_policy_schema(db)) return -1;
    int rc = merkle_sync_init(inst, ETCP_ID_MEMBER_SYNC, &g_member_ops, inst);
    if (rc != 0) return rc;
    if (member_admin_register(inst)) return -1;
    /* очистка битых записей во всех каналах перед построением дерева */
    if (member_sync_verify_and_purge_all(inst) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: initialization stopped after purge failure", MS_ID);
        merkle_sync_destroy(inst); return -1;
    }
    if (_rebuild_all_trees(inst) < 0) { merkle_sync_destroy(inst); return -1; }
    char last[128] = "";
    for (;;) {
        sqlite3_stmt* st = NULL;
        int sr = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name GLOB 'peers_*'"
            " AND name>? ORDER BY name LIMIT 1", -1, &st, NULL);
        if (sr == SQLITE_OK) sr = sqlite3_bind_text(st, 1, last, -1, SQLITE_STATIC);
        if (sr == SQLITE_OK) sr = sqlite3_step(st);
        if (sr == SQLITE_ROW) {
            const char* name = (const char*)sqlite3_column_text(st, 0);
            if (!name || strlen(name) >= sizeof(last)) sr = SQLITE_CORRUPT;
            else strcpy(last, name);
        }
        int final = sqlite3_finalize(st);
        if ((sr != SQLITE_ROW && sr != SQLITE_DONE) || final != SQLITE_OK ||
            (sr == SQLITE_ROW && group_policy_refresh(inst, last + 6))) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "admission initialization failed rc=%d table=%s", sr, last);
            merkle_sync_destroy(inst); return -1;
        }
        if (sr == SQLITE_DONE) break;
    }
    /* подписка на BGP-события существующих chat-групп */
    if (inst->topo_groups && inst->topo_groups->group_list) {
        struct ll_entry* ge = inst->topo_groups->group_list->head;
        while (ge) {
            struct TOPO_GROUP* g = (struct TOPO_GROUP*)ge;
            if (g->group_type == TOPO_GROUP_TYPE_CHAT && g->channel_id[0])
                if (member_sync_subscribe_group(inst, g) < 0) { member_sync_destroy(inst); return -1; }
            ge = ge->next;
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "%s: initialized, merkle_rc=%d", MS_ID, rc);
    return 0;
}

void member_sync_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    if (inst->topo_groups && inst->topo_groups->group_list) {
        struct ll_entry* ge = inst->topo_groups->group_list->head;
        while (ge) {
            struct TOPO_GROUP* g = (struct TOPO_GROUP*)ge;
            if (g->group_type == TOPO_GROUP_TYPE_CHAT && g->channel_id[0])
                topo_group_remove_node_cbk(g, _ms_on_bgp_node, inst);
            ge = ge->next;
        }
    }
    /* освободить callback-списки (иначе утекут при chat_core_destroy) */
    if (CC(inst)) {
        struct ms_props_cbk* pc = CC(inst)->props_cbks;
        while (pc) { struct ms_props_cbk* n = pc->next; u_free(pc); pc = n; }
        CC(inst)->props_cbks = NULL;
    }
    merkle_sync_destroy(inst);
    group_policy_forget(inst, NULL);
}

int member_sync_start(struct UTUN_INSTANCE* inst, struct ETCP_CONN* conn,
                       const char* ch_id, merkle_sync_done_cb done_cb, void* arg) {
    return merkle_sync_start(inst, MT_DATA_MEMBERS, conn, ch_id, done_cb, arg);
}

void member_sync_cancel(struct UTUN_INSTANCE* inst, const uint8_t peer[32], const char* ch_id) {
    merkle_sync_cancel(inst, MT_DATA_MEMBERS, peer, ch_id);
}

void member_sync_cancel_channel(struct UTUN_INSTANCE* inst, const char* ch_id) {
    merkle_sync_cancel_ns(inst, ch_id);
}


int member_sync_put(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t* x25519,
                     const uint8_t* ed25519,
                     const uint8_t* join_sig, uint64_t join_ts,
                     const uint8_t* update_sig, uint64_t update_ts,
                     const char* userinfo,
                     const char* adm_tags, const uint8_t* adm_tags_sig, int storage,
                     const uint8_t* signed_by, const uint8_t* signature) {
    if (!inst || !ch_id) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "%s: put — inst=%p ch_id=%s", MS_ID, (void*)inst, ch_id ? ch_id : "(null)"); return -1; }
    uint64_t member_id = sc_derive_node_id_from_pubkey(x25519);
    if (!member_id) { DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "member put: missing transport identity ch=%s", ch_id); return -1; }
    DEBUG_TRACE(DEBUG_CATEGORY_MEMBER_SYNC, "%s: put ch=%s nid=%016llx tags_bytes=%zu storage=%d signed_by=%s",
        MS_ID, ch_id, (unsigned long long)member_id, adm_tags ? strlen(adm_tags) : 0, storage, sc_public_key_hex(signed_by, (char[65]){0}));

    struct ms_member_rec m;
    memset(&m, 0, sizeof(m));
    m.node_id = member_id;
    m.x25519 = x25519; m.ed25519 = ed25519;
    m.join_sig = join_sig; m.join_ts = join_ts;
    m.update_sig = update_sig; m.update_ts = update_ts;
    m.userinfo = userinfo;
    m.adm_tags = adm_tags; m.adm_tags_sig = adm_tags_sig; m.storage = storage;
    m.signed_by = signed_by; m.signature = signature;

    return member_sync_apply_record(inst, ch_id, NULL, &m);
}

int member_sync_count(struct UTUN_INSTANCE* inst, const char* ch_id) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "legacy member count rejected in RW; read count in worker snapshot"); return 0;
    }
    sqlite3* db = _db(inst);
    if (!db || !ch_id) return 0;
    char peers_tbl[128]; _peers_table(ch_id, peers_tbl, sizeof(peers_tbl));
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\" WHERE record_state=0", peers_tbl);
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int c = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) c = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return c;
}

/* Проверяет удостоверение целиком; ожидание зависимости сохраняет возможность позднего догоняния. */
static enum ms_author_status member_verify_author(sqlite3* db, const char* channel,
        uint64_t author, const uint8_t* ed_key, uint8_t* key, int* error) {
    if (error) *error = SQLITE_OK;
    if (!db || (!ed_key && (!key || !author)) || !chat_channel_valid(channel)) {
        if (error) *error = SQLITE_MISUSE;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "author verification: invalid arguments"); return MS_AUTHOR_ERROR;
    }
    char sql[512], unique[160] = "";
    if (!ed_key) snprintf(unique, sizeof(unique), " AND 1=(SELECT count(*) FROM peers_%s WHERE node_id=?1 AND record_state=0)", channel);
    snprintf(sql, sizeof(sql), "SELECT x25519_pubkey,ed25519_pubkey,signed_by,signature,join_sig,join_ts,identity_verified,admitted"
        " FROM peers_%s WHERE %s=?1 AND record_state=0 AND source=0 AND deleted=0%s",
        channel, ed_key ? "ed25519_pubkey" : "node_id", unique);
    sqlite3_stmt* st = NULL;
    enum ms_author_status status = MS_AUTHOR_ERROR;
    const char* reason = "member lookup";
    uint8_t verified_key[32]; int final, code = SQLITE_OK;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = ed_key ? sqlite3_bind_blob(st, 1, ed_key, 32, SQLITE_STATIC) : sqlite3_bind_int64(st, 1, (sqlite3_int64)author);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { status = MS_AUTHOR_INACTIVE; reason = "member inactive"; goto done; }
    if (rc != SQLITE_ROW) goto done;
    if (!sqlite3_column_int(st, 6)) { status = MS_AUTHOR_PENDING; reason = "invitation chain pending"; goto done; }
    if (!sqlite3_column_int(st, 7)) { status = MS_AUTHOR_INACTIVE; reason = "group policy denied"; goto done; }
    int x_bytes = sqlite3_column_bytes(st, 0), ed_bytes = sqlite3_column_bytes(st, 1);
    int tree_bytes = sqlite3_column_bytes(st, 3), join_bytes = sqlite3_column_bytes(st, 4);
    if (sqlite3_errcode(db) == SQLITE_NOMEM) { rc = SQLITE_NOMEM; goto done; }
    status = MS_AUTHOR_INVALID;
    reason = "invalid member fields";
    uint8_t signer[32];
    int signer_rc = topo_node_sqlite_read_optional_key(st, 2, signer);
    if (signer_rc && signer_rc != SQLITE_CORRUPT) { status = MS_AUTHOR_ERROR; code = signer_rc; reason = "read inviter"; goto done; }
    if (signer_rc || x_bytes != 32 || ed_bytes != 32 || (tree_bytes != 0 && tree_bytes != 64)
        || (join_bytes != 0 && join_bytes != 64)) goto done;
    struct ms_member_rec member = { .node_id = author, .x25519 = sqlite3_column_blob(st, 0),
        .ed25519 = sqlite3_column_blob(st, 1), .signed_by = member_sync_signer(signer),
        .signature = sqlite3_column_blob(st, 3), .join_sig = sqlite3_column_blob(st, 4),
        .join_ts = (uint64_t)sqlite3_column_int64(st, 5) };
    if (sqlite3_errcode(db) == SQLITE_NOMEM || !member.x25519 || !member.ed25519
        || (tree_bytes && !member.signature) || (join_bytes && !member.join_sig)) {
        status = MS_AUTHOR_ERROR; reason = "member column allocation"; rc = SQLITE_NOMEM; goto done;
    }
    if (!tree_bytes || !join_bytes || sqlite3_column_int64(st, 5) <= 0
        || _sig_is_zero64(member.signature) || _sig_is_zero64(member.join_sig)) {
        status = MS_AUTHOR_PENDING; reason = "join or invitation pending"; goto done;
    }
    reason = "member X25519/Ed25519 mismatch";
    rc = sc_identity_check(channel, member.x25519, member.ed25519);
    if (rc) { status = rc == SC_ERR_AUTH_FAILED ? MS_AUTHOR_INVALID : MS_AUTHOR_ERROR; goto done; }
    if (!ed_key) {
        reason = "route projection/X25519 mismatch";
        uint64_t derived = sc_derive_node_id_from_pubkey(member.x25519);
        if (!derived) { status = MS_AUTHOR_ERROR; reason = "identity digest"; rc = SC_ERR_CRYPTO; goto done; }
        if (derived != author) goto done;
    }
    uint8_t x[32], ed[32], body[MEMBER_JOIN_MSG_SIZE];
    reason = "channel keys unavailable";
    int found = 0;
    code = member_sync_channel_keys_read(db, channel, x, ed, &found);
    if (code || !found) {
        status = code ? MS_AUTHOR_ERROR : MS_AUTHOR_PENDING; goto done;
    }
    reason = "invalid join signature";
    int bytes = member_sync_build_join_msg(x, ed, member.ed25519, member.x25519, body, sizeof(body));
    if (bytes != MEMBER_JOIN_MSG_SIZE) { status = MS_AUTHOR_ERROR; reason = "join message build"; goto done; }
    int signature = sc_ed25519_verify(member.ed25519, body, bytes, member.join_sig);
    if (signature != SC_OK) {
        status = signature == SC_ERR_AUTH_FAILED ? MS_AUTHOR_INVALID : MS_AUTHOR_ERROR; rc = signature; goto done;
    }
    reason = "invitation verification";
    signature = member_verify_signature_ex(db, channel, &member, &code);
    if (signature != 1) {
        status = signature == MS_VERIFY_DEFERRED ? MS_AUTHOR_PENDING
            : signature == MS_VERIFY_ERROR ? MS_AUTHOR_ERROR : MS_AUTHOR_INVALID;
        rc = signature;
        goto done;
    }
    memcpy(verified_key, member.ed25519, 32); status = MS_AUTHOR_ACTIVE; reason = "verified";
done:
    final = sqlite3_finalize(st);
    if (final != SQLITE_OK) { status = MS_AUTHOR_ERROR; reason = "member finalize"; rc = final; }
    if (status == MS_AUTHOR_ERROR) {
        if (final != SQLITE_OK) code = final;
        else if (!code) code = rc > 0 && rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : SQLITE_ERROR;
        if (error) *error = code;
    }
    if (status == MS_AUTHOR_ACTIVE && key) memcpy(key, verified_key, 32);
    if (status == MS_AUTHOR_ERROR) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC,
        "author verification failed ch=%s node=%016llx reason=%s result=%d sqlite=%d",
        channel, (unsigned long long)author, reason, rc, code);
    else if (status == MS_AUTHOR_INVALID) DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC,
        "author invalid ch=%s node=%016llx reason=%s", channel, (unsigned long long)author, reason);
    else if (status != MS_AUTHOR_ACTIVE) DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC,
        "author unavailable ch=%s node=%016llx status=%d reason=%s", channel, (unsigned long long)author, status, reason);
    return status;
}

enum ms_author_status member_sync_author_key_ex(sqlite3* db, const char* channel, uint64_t node, uint8_t key[32], int* error) {
    return member_verify_author(db, channel, node, NULL, key, error);
}

enum ms_author_status member_sync_verify_author(sqlite3* db, const char* channel, const uint8_t ed[32], int* error) {
    return member_verify_author(db, channel, 0, ed, NULL, error);
}

enum ms_author_status member_sync_author_key(sqlite3* db, const char* channel, uint64_t author, uint8_t key[32]) {
    return member_sync_author_key_ex(db, channel, author, key, NULL);
}

/* Строгий допуск radio/attachments: ни ожидание, ни исключение не разрешают действие. */
int member_sync_verified_ed25519(struct UTUN_INSTANCE* inst, const char* channel, uint64_t author, uint8_t key[32]) {
    if (inst && db_async_can_write(inst->db_worker)) return group_policy_member_keys(inst, channel, author, 1, NULL, key) ? -1 : 0;
    if (!inst || !topo_node_sqlite_member_allowed(inst->topo_sqlite_db, channel, author)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member admission denied ch=%s node=%016llx",
            channel ? channel : "-", (unsigned long long)author); return -1;
    }
    enum ms_author_status status = member_sync_author_key(inst->topo_sqlite_db, channel, author, key);
    if (status == MS_AUTHOR_ACTIVE) return 0;
    DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "member verification denied ch=%s node=%016llx status=%d",
        channel ? channel : "-", (unsigned long long)author, status); return -1;
}
