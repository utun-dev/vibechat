/* db_sync — подписанная история SQLite, модель MESSAGES общего Merkle и адресные PUSH/ACK. */
#include "db_sync.h"
#include "../db_changes.h"
#include "../group/group_protocol.h"
#include "../db_schema.h"
#include "merkle_sync.h"
#include "member_sync.h"
#include "chat_feedback.h"
#include "chat_attachment.h"
#include "group_policy.h"
#include "chat_core_priv.h"
#include "chat_poll.h"
#include "chat_log.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/pkt_normalizer.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/platform_compat.h"
#include "../../lib/db_async.h"
#include "merkle_tree.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>

#define DB_BATCH_RECORDS 16
#define DB_WIRE_AUTHOR 40
#define DB_WIRE_NONCE (DB_WIRE_AUTHOR + 32)
#define DB_WIRE_VERSION (DB_WIRE_NONCE + 16)
#define DB_WIRE_EDITED (DB_WIRE_VERSION + 8)
#define DB_WIRE_LENGTH (DB_WIRE_EDITED + 8)
#define DB_WIRE_BODY (DB_WIRE_LENGTH + 4)
#define DB_WIRE_FIXED (DB_WIRE_BODY + DB_SIG_SIZE + 32)
#define DB_WIRE_MAX (DB_WIRE_FIXED + DB_SYNC_MESSAGE_MAX)
#define SI_DB(si) ((si)->db_sync->db)
#define SI_TBL(si) ((si)->table_name)

struct SI_PEER {
    uint8_t ed[32];
    uint64_t member_retry_at;
    uint8_t sync_state, sound_on, paused, needs_members; /* 0=готов,1=нужен MEMBERS,2=запрос выполняется */
};
struct DB_SYNC_INSTANCE {
    struct DB_SYNC* db_sync;
    uint64_t group_id, next_id, last_timestamp_ms, generation;
    char table_name[96], ns[65];
    uint8_t enabled, sync_gated;
    unsigned holds;
    void* ttl_timer;
    struct {
        uint8_t pending, restart, phase, after_valid, waiting_key, running;
        uint64_t revision;
        sqlite3_int64 after;
        void* timer;
    } index;
    struct SI_PEER* peers;
    int peer_count, peer_capacity;
    db_sync_insert_cb on_insert;
    void* on_insert_arg;
};
struct db_sync_done_cbk_entry {
    db_sync_done_fn fn;
    void* arg;
    uint8_t removed;
    struct db_sync_done_cbk_entry* next;
};
struct db_notice {
    struct db_notice* next;
    struct DB_SYNC* db;
    uint64_t group_id;
    uint8_t initial;
    unsigned count;
    void* work;
    /* Записи и тела лежат непосредственно после этой структуры. */
};
struct DB_SYNC {
    struct UTUN_INSTANCE* inst;
    sqlite3* db;
    uint8_t enabled, quiescing;
    uint64_t generation;
    struct DB_SYNC_INSTANCE** instances;
    int instance_count, instance_capacity;
    void* peer_check_timer;
#ifdef UTUN_HAVE_STANDBY
    void* peer_check_wait; /* Отменяемое ожидание активной фазы Android. */
#endif
    struct db_sync_done_cbk_entry* done_cbks;
    unsigned dispatch_depth;
    struct db_notice* notices;
};
struct db_record {
    sqlite3_int64 rowid;
    uint64_t id, timestamp, author;
    size_t data_len, sig_len;
    uint8_t signature[64], message_id[32], accepted_key[32];
    struct db_message_revision revision;
    const char* data;
    int cache_valid, updated, needs_members;
};
struct db_checked {
    struct db_record record;
    int check;
    char data[DB_SYNC_MESSAGE_MAX];
};
/* SQL-снимок одной истории: без runtime-указателей, используется только владельцем db. */
struct db_store {
    sqlite3* db;
    uint64_t group_id;
    char table_name[96], ns[65];
    int indexed, rebuild, error;
};

/* Снять runtime-флаги до отправки в worker; SQLite здесь не используется. */
static struct db_store db_store_for_instance(struct DB_SYNC_INSTANCE* si) {
    struct db_store store = {.db=SI_DB(si), .group_id=si->group_id,
        .indexed=!si->index.pending && merkle_sync_registered(si->db_sync->inst, MT_DATA_MESSAGES)};
    snprintf(store.table_name, sizeof(store.table_name), "%s", si->table_name);
    snprintf(store.ns, sizeof(store.ns), "%s", si->ns); return store;
}
static int db_store_error(struct db_store* store, const char* stage) {
    int rc = store->error ? store->error : sqlite3_extended_errcode(store->db);
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) rc = SQLITE_ERROR;
    store->error = rc;
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL tbl=%s stage=%s rc=%d: %s", store->table_name, stage, rc, sqlite3_errstr(rc));
    return -1;
}
static int db_store_prepare(struct db_store* store, sqlite3_stmt** st, const char* format) {
    char sql[512]; int n = snprintf(sql, sizeof(sql), format, store->table_name);
    if (n < 0 || (size_t)n >= sizeof(sql)) {
        *st = NULL; store->error = SQLITE_TOOBIG;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL statement too long tbl=%s", store->table_name); return store->error;
    }
    int rc = sqlite3_prepare_v2(store->db, sql, -1, st, NULL);
    if (rc) {
        store->error = rc;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL prepare tbl=%s rc=%d: %s", store->table_name, rc, sqlite3_errmsg(store->db));
    }
    return rc;
}

static void db_sync_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry);
static void db_sync_on_peer_ready(struct TOPO_GROUP* group, const uint8_t peer[32], int ready, void* arg);
static void db_sync_peer_check_cb(void* arg);
static void db_sync_resume_peer_check(struct DB_SYNC* db);
static void db_index_tick(void* arg);
static void db_index_request(struct DB_SYNC_INSTANCE* si);
static int db_start_peer(struct DB_SYNC_INSTANCE* si, const uint8_t peer[32]);
static void db_request_members(struct DB_SYNC_INSTANCE* si, const uint8_t peer[32]);
static int db_sync_peer_throttled(struct DB_SYNC* db, struct SI_PEER* peer);
static int db_message_hash(void*, const char*, uint8_t, uint64_t, EVP_MD_CTX*);
static int db_store_hash(void*, const char*, uint8_t, uint64_t, EVP_MD_CTX*);
static const struct merkle_sync_data_ops db_message_ops;
static const struct merkle_sync_sql_ops db_message_sql_ops;

static uint64_t db_read64(const uint8_t* p) { uint64_t n; memcpy(&n, p, 8); return be64toh(n); }
static void db_write64(uint8_t* p, uint64_t n) { n = htobe64(n); memcpy(p, &n, 8); }
static uint32_t db_read32(const uint8_t* p) { uint32_t n; memcpy(&n, p, 4); return ntohl(n); }
static void db_write32(uint8_t* p, uint32_t n) { n = htonl(n); memcpy(p, &n, 4); }

/* Канонические подписанные байты: домен, канал, постоянный ID, время, автор, nonce, версия, правка, длина и тело. */
static int db_signed_bytes(const uint8_t group_ed[32], const struct db_record* r, uint8_t* out) {
    static const char domain[] = "utun-chat-record-v5";
    if (!r->data || !r->data_len || r->data_len > DB_SYNC_MESSAGE_MAX || !r->timestamp || r->timestamp > INT64_MAX ||
        !memcmp(r->accepted_key, (uint8_t[32]){0}, 32) || !r->revision.ver || r->revision.ver > INT64_MAX || r->revision.edited_at > INT64_MAX ||
        (r->revision.ver == 1 ? r->revision.edited_at != 0 : r->revision.edited_at <= r->timestamp)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history invalid envelope group=%s ts=%llu ver=%llu edited=%llu bytes=%zu",
            sc_public_key_hex(group_ed, (char[65]){0}), (unsigned long long)r->timestamp, (unsigned long long)r->revision.ver,
            (unsigned long long)r->revision.edited_at, r->data_len); return -1;
    }
    size_t off = sizeof(domain) - 1; memcpy(out, domain, off);
    memcpy(out + off, group_ed, 32); off += 32;
    memcpy(out + off, r->message_id, 32); off += 32;
    db_write64(out + off, r->timestamp); off += 8; memcpy(out + off, r->accepted_key, 32); off += 32;
    memcpy(out + off, r->revision.nonce, 16); off += 16;
    db_write64(out + off, r->revision.ver); off += 8; db_write64(out + off, r->revision.edited_at); off += 8;
    memcpy(out + off, r->revision.generation, 32); off += 32;
    db_write32(out + off, (uint32_t)r->data_len); off += 4; memcpy(out + off, r->data, r->data_len);
    return (int)(off + r->data_len);
}

int db_sync_local_revision(struct DB_SYNC_INSTANCE* si, struct db_message_revision* revision) {
    if (!si || !revision) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history invalid local revision arguments"); return -1; }
    memset(revision, 0, sizeof(*revision)); revision->ver = 1;
    if (group_policy_key_generation(SI_DB(si), si->ns, si->db_sync->inst->my_ed25519_pubkey, revision->generation)
        || RAND_bytes(revision->nonce, 16) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history local generation unavailable ch=%s", si->ns); return -1;
    }
    return 0;
}

int db_sync_sign_message(sqlite3* db, const char* channel, uint64_t timestamp, const uint8_t author[32],
        const struct db_message_revision* revision, const char* data, size_t len,
        const uint8_t private_key[32], uint8_t signature[64]) {
    if (!author || !revision || !private_key || !signature) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history invalid signing arguments"); return -1;
    }
    struct db_record r = {.timestamp = timestamp, .revision = *revision, .data = data, .data_len = len};
    memcpy(r.accepted_key, author, 32);
    uint8_t body[DB_WIRE_MAX + 64], group_ed[32]; int found = 0;
    int rc = member_sync_channel_keys_read(db, channel, NULL, group_ed, &found);
    if (rc || !found) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history signing group key unavailable ch=%s rc=%d", channel, rc); return -1;
    }
    if (db_sync_message_id(timestamp, author, revision, r.message_id) < 0) return -1;
    int bytes = db_signed_bytes(group_ed, &r, body);
    if (bytes < 0 || sc_ed25519_sign(private_key, body, (size_t)bytes, signature) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history signing failed group=%s ver=%llu",
            channel, (unsigned long long)revision->ver); return -1;
    }
    return 0;
}

static int db_error(struct DB_SYNC_INSTANCE* si, const char* stage) {
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history tbl=%s stage=%s: %s", SI_TBL(si), stage, sqlite3_errmsg(SI_DB(si)));
    return -1;
}
static void db_rollback(struct DB_SYNC_INSTANCE* si, const char* stage) {
    db_error(si, stage);
    if (sqlite3_exec(SI_DB(si), "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) db_error(si, "rollback");
}
// Форматирует SQL-запрос, подставляя имя таблицы из инстанса (через %s)
static void si_sql(char* buf, size_t sz, struct DB_SYNC_INSTANCE* si, const char* fmt)
{
    snprintf(buf, sz, fmt, SI_TBL(si));
}

// Готовит SQLite statement с подстановкой имени таблицы — обёртка над si_sql + sqlite3_prepare_v2
static int si_prep(struct DB_SYNC_INSTANCE* si, sqlite3_stmt** stmt, const char* fmt)
{
    char sql[512];
    si_sql(sql, sizeof(sql), si, fmt);
    return sqlite3_prepare_v2(SI_DB(si), sql, -1, stmt, NULL);
}


static struct DB_SYNC_INSTANCE* db_instance_find(struct DB_SYNC* db, uint64_t group) {
    for (int i = 0; db && i < db->instance_count; i++)
        if (db->instances[i]->enabled && db->instances[i]->group_id == group) return db->instances[i];
    return NULL;
}
struct DB_SYNC_INSTANCE* db_sync_instance_find(struct UTUN_INSTANCE* inst, uint64_t group) {
    return db_instance_find(inst ? inst->db_sync : NULL, group);
}
struct DB_SYNC_INSTANCE* si_find(struct UTUN_INSTANCE* inst, const char* ch_id) {
    struct DB_SYNC* db = inst ? inst->db_sync : NULL;
    if (!db || !chat_channel_valid(ch_id)) return NULL;
    for (int i = 0; i < db->instance_count; i++) {
        struct DB_SYNC_INSTANCE* history = db->instances[i];
        if (history->enabled && !strcmp(history->ns, ch_id)) return history;
    }
    return NULL;
}

uint64_t db_sync_instance_group_id(struct DB_SYNC_INSTANCE* si) { return si ? si->group_id : 0; }
const char* db_sync_instance_channel(struct DB_SYNC_INSTANCE* si) { return si ? si->ns : ""; }
int db_sync_index_ready(struct DB_SYNC_INSTANCE* si) { return si && si->enabled && !si->index.pending; }
static void db_release(struct DB_SYNC_INSTANCE* si) {
    if (--si->holds == 0 && !si->enabled) u_free(si);
}
static int si_name_valid(const char* name) {
    if (!name || !*name || strlen(name) > 80) return 0;
    for (const char* p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    return 1;
}
// Ищет пира в инстансе по полному Ed25519.
static struct SI_PEER* si_peer_find(struct DB_SYNC_INSTANCE* si, const uint8_t ed[32])
{
    for (int i = 0; i < si->peer_count; i++) {
        if (ed && !memcmp(si->peers[i].ed, ed, 32)) return &si->peers[i];
    }
    return NULL;
}

// Добавляет пира в инстанс (если уже существует — возвращает существующего)
static struct SI_PEER* si_peer_add(struct DB_SYNC_INSTANCE* si, const uint8_t ed[32])
{
    if (!ed) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history missing peer Ed25519"); return NULL; }
    struct SI_PEER* p = si_peer_find(si, ed);
    if (p) return p;
    if (si->peer_count >= si->peer_capacity) {
        int nc = si->peer_capacity ? si->peer_capacity * 2 : 8;
        struct SI_PEER* np = u_realloc(si->peers, nc * sizeof(*si->peers));
        if (!np) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history peer allocation failed group=%016llx", si->group_id); return NULL; }
        si->peers = np;
        si->peer_capacity = nc;
    }
    p = &si->peers[si->peer_count++];
    memset(p, 0, sizeof(*p));
    memcpy(p->ed, ed, sizeof(p->ed));
    p->sound_on = 1;
    return p;
}

enum db_record_check { DB_CHECK_ERROR = -1, DB_CHECK_VALID, DB_CHECK_BAD_SIGNATURE, DB_CHECK_BAD_FORMAT, DB_CHECK_DEFERRED };

/* Проверяем конкретные байты. Только BAD_* доказывает их непригодность. */
static int db_chat_store(const struct db_store* store);
static enum db_record_check db_verify_author_sig(struct db_store* store, struct db_record* r)
{
    if (db_chat_store(store)) {
        struct db_sync_record record = {.timestamp=r->timestamp,.author=r->author,.revision=r->revision,.data=r->data,.size=r->data_len};
        memcpy(record.signature,r->signature,64);
        memcpy(record.author_ed,r->accepted_key,32);
        int valid=chat_log_message_check(store->db,store->ns,&record);
        if(valid<0) { store->error=-valid; return DB_CHECK_ERROR; }
        if(!valid) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC,"CHAT projection has no matching block ch=%s ver=%llu",store->ns,
            (unsigned long long)r->revision.ver); return DB_CHECK_BAD_SIGNATURE; }
        return DB_CHECK_VALID;
    }
    uint8_t msg[DB_WIRE_MAX + 64], group_ed[32]; int found = 0;
    int group_rc = member_sync_channel_keys_read(store->db, store->ns, NULL, group_ed, &found);
    if (group_rc || !found) {
        store->error = group_rc ? group_rc : SQLITE_NOTFOUND;
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history verification group key unavailable ch=%s rc=%d", store->ns, store->error);
        return DB_CHECK_ERROR;
    }
    int bytes = db_signed_bytes(group_ed, r, msg);
    if (r->sig_len != DB_SIG_SIZE || bytes < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history malformed record tbl=%s ts=%llu body=%zu signature=%zu",
            store->table_name, (unsigned long long)r->timestamp, r->data_len, r->sig_len);
        db_changes_warn(store->db, NULL, store->ns, "Message", "invalid signed envelope", r->accepted_key,
            r->sig_len == DB_SIG_SIZE ? r->signature : NULL, NULL);
        return DB_CHECK_BAD_FORMAT;
    }
    int key_error = SQLITE_OK;
    int key_rc = topo_node_sqlite_member_local_id(store->db, store->ns, r->accepted_key, 0, &r->author, &key_error);
    if (key_rc != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history verification unavailable tbl=%s author=%s key_status=%d; preserving data",
            store->table_name, sc_public_key_hex(r->accepted_key, (char[65]){0}), key_rc);
        if (key_rc == TOPO_SQLITE_LOOKUP_ERROR) store->error = key_error;
        return key_rc == TOPO_SQLITE_LOOKUP_ERROR ? DB_CHECK_ERROR : DB_CHECK_DEFERRED;
    }
    int rc = sc_ed25519_verify(r->accepted_key, msg, (size_t)bytes, r->signature);
    if (rc != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history signature check tbl=%s ts=%llu author=%s rc=%d result=%s",
            store->table_name, (unsigned long long)r->timestamp, sc_public_key_hex(r->accepted_key, (char[65]){0}), rc,
            rc == SC_ERR_AUTH_FAILED ? "mismatch" : "operational failure; preserving data");
        if (rc == SC_ERR_AUTH_FAILED) db_changes_warn(store->db, NULL, store->ns, "Message", "invalid signature", r->accepted_key, r->signature, NULL);
        return rc == SC_ERR_AUTH_FAILED ? DB_CHECK_BAD_SIGNATURE : DB_CHECK_ERROR;
    }
    return DB_CHECK_VALID;
}

/* Журнал относится к CHAT-истории; произвольные таблицы общего DB_SYNC сохраняют собственный протокол. */
static int db_chat_table(const char* name, const char* channel) {
    return !strncmp(name, "msg_", 4) && !strcmp(name + 4, channel);
}
static int db_chat_store(const struct db_store* store) {
    return db_chat_table(store->table_name, store->ns);
}
/* Сохранённая история проверяет подпись, новый сетевой приём дополнительно требует живого поколения автора. */
static enum db_record_check db_verify_incoming(struct db_store* store, struct db_record* r) {
    enum db_record_check signature = db_verify_author_sig(store, r);
    if (signature != DB_CHECK_VALID) { r->needs_members = signature == DB_CHECK_DEFERRED; return signature; }
    if (!db_chat_store(store)) {
        uint8_t generation[32]; int error=SQLITE_OK;
        int active=topo_node_sqlite_member_local_id(store->db,store->ns,r->accepted_key,1,&r->author,&error);
        if(active==TOPO_SQLITE_LOOKUP_ERROR) { store->error=error; return DB_CHECK_ERROR; }
        int epoch=active?1:group_policy_key_generation(store->db,store->ns,r->accepted_key,generation);
        if(epoch<0) return DB_CHECK_ERROR;
        if(active || epoch || memcmp(generation,r->revision.generation,32)) { r->needs_members=1; return DB_CHECK_DEFERRED; }
        return DB_CHECK_VALID;
    }
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC,"bare CHAT projection rejected ch=%s; signed member block required",store->ns);
    db_changes_warn(store->db, NULL, store->ns, "Chat message", "signed member block required", r->accepted_key, r->signature, NULL);
    return DB_CHECK_BAD_FORMAT;
}

/* Вызывается после завершения SELECT и только внутри собственной транзакции. */
static int db_delete_bad_row(struct db_store* store, sqlite3_int64 rowid) {
    sqlite3_stmt* stmt = NULL;
    int rc = db_store_prepare(store, &stmt, "DELETE FROM \"%s\" WHERE rowid=?");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, rowid);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(store->db), final_rc = sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && final_rc == SQLITE_OK && changed == 1) return 0;
    store->error = rc != SQLITE_DONE ? rc : final_rc != SQLITE_OK ? final_rc : SQLITE_CONSTRAINT;
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history delete tbl=%s rowid=%lld rc=%d finalize=%d changed=%d: %s",
        store->table_name, (long long)rowid, rc, final_rc, changed, sqlite3_errmsg(store->db)); return -1;
}

#define DB_COLUMNS "rowid,id,timestamp,data,author_signature,message_id,bucket,nonce,ver,edited_at,generation,accepted_key"

/* Private RAM fixtures создают историю без полного runtime bootstrap. */
static int si_table_init(struct DB_SYNC_INSTANCE* si) { return db_schema_history(SI_DB(si), SI_TBL(si)); }

/* Чтение конкретной строки. cache_valid — производные поля, их можно восстановить без удаления сообщения. */
static enum db_record_check db_read_record(struct db_store* store, sqlite3_stmt* st, struct db_record* r) {
    memset(r, 0, sizeof(*r));
    r->rowid = sqlite3_column_int64(st, 0); r->id = (uint64_t)sqlite3_column_int64(st, 1);
    r->timestamp = (uint64_t)sqlite3_column_int64(st, 2);
    r->data = sqlite3_column_blob(st, 3);
    if (!r->data && sqlite3_errcode(store->db) == SQLITE_NOMEM) return DB_CHECK_ERROR;
    r->data_len = (size_t)sqlite3_column_bytes(st, 3);
    const uint8_t* sig = sqlite3_column_blob(st, 4);
    if (!sig && sqlite3_errcode(store->db) == SQLITE_NOMEM) return DB_CHECK_ERROR;
    r->sig_len = (size_t)sqlite3_column_bytes(st, 4);
    if (!r->data || !r->data_len || r->data_len > DB_SYNC_MESSAGE_MAX || !sig || r->sig_len != 64) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history malformed stored row tbl=%s rowid=%lld body=%zu signature=%zu",
            store->table_name, (long long)r->rowid, r->data_len, r->sig_len); return DB_CHECK_BAD_FORMAT;
    }
    memcpy(r->signature, sig, 64);
    const void* nonce = sqlite3_column_blob(st, 7);
    if (!nonce && sqlite3_errcode(store->db) == SQLITE_NOMEM) return DB_CHECK_ERROR;
    if (!nonce || sqlite3_column_bytes(st, 7) != 16) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history malformed nonce tbl=%s rowid=%lld", store->table_name, (long long)r->rowid);
        return DB_CHECK_BAD_FORMAT;
    }
    memcpy(r->revision.nonce, nonce, 16);
    r->revision.ver = (uint64_t)sqlite3_column_int64(st, 8);
    r->revision.edited_at = (uint64_t)sqlite3_column_int64(st, 9);
    const void* generation = sqlite3_column_blob(st, 10);
    if (!generation && sqlite3_errcode(store->db) == SQLITE_NOMEM) return DB_CHECK_ERROR;
    if (!generation || sqlite3_column_bytes(st, 10) != 32) return DB_CHECK_BAD_FORMAT;
    memcpy(r->revision.generation, generation, 32);
    const void* accepted_key = sqlite3_column_blob(st, 11);
    if (!accepted_key && sqlite3_errcode(store->db) == SQLITE_NOMEM) return DB_CHECK_ERROR;
    if (!accepted_key || sqlite3_column_bytes(st, 11) != 32) return DB_CHECK_BAD_FORMAT;
    memcpy(r->accepted_key, accepted_key, 32);
    if (db_sync_message_id(r->timestamp, r->accepted_key, &r->revision, r->message_id) < 0) return DB_CHECK_ERROR;
    const uint8_t* key = sqlite3_column_blob(st, 5);
    if (!key && sqlite3_errcode(store->db) == SQLITE_NOMEM) return DB_CHECK_ERROR;
    r->cache_valid = key && sqlite3_column_bytes(st, 5) == 32 && !memcmp(key, r->message_id, 32) &&
        (uint64_t)sqlite3_column_int64(st, 6) == (db_read64(r->message_id) >> 39);
    return DB_CHECK_VALID;
}

static int db_encode_record(const struct db_record* r, uint8_t* buf) {
    memcpy(buf, r->message_id, 32); db_write64(buf + 32, r->timestamp); memcpy(buf + DB_WIRE_AUTHOR, r->accepted_key, 32);
    memcpy(buf + DB_WIRE_NONCE, r->revision.nonce, 16);
    db_write64(buf + DB_WIRE_VERSION, r->revision.ver); db_write64(buf + DB_WIRE_EDITED, r->revision.edited_at);
    db_write32(buf + DB_WIRE_LENGTH, (uint32_t)r->data_len); memcpy(buf + DB_WIRE_BODY, r->data, r->data_len);
    memcpy(buf + DB_WIRE_BODY + r->data_len, r->signature, 64);
    memcpy(buf + DB_WIRE_BODY + r->data_len + 64, r->revision.generation, 32);
    return (int)(DB_WIRE_FIXED + r->data_len);
}
static int db_parse_record_group(uint64_t group, const uint8_t** ptr, const uint8_t* end, struct db_record* r) {
    if ((size_t)(end - *ptr) < DB_WIRE_FIXED) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history truncated record"); return -1; }
    const uint8_t* p = *ptr;
    memset(r, 0, sizeof(*r));
    r->timestamp = db_read64(p + 32); memcpy(r->accepted_key, p + DB_WIRE_AUTHOR, 32); r->data_len = db_read32(p + DB_WIRE_LENGTH);
    memcpy(r->revision.nonce, p + DB_WIRE_NONCE, 16);
    r->revision.ver = db_read64(p + DB_WIRE_VERSION); r->revision.edited_at = db_read64(p + DB_WIRE_EDITED);
    if (!r->data_len || r->data_len > DB_SYNC_MESSAGE_MAX || r->data_len > (size_t)(end - p) - DB_WIRE_FIXED) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history invalid wire body=%zu", r->data_len); return -1;
    }
    r->data = (const char*)p + DB_WIRE_BODY; r->sig_len = 64; memcpy(r->signature, p + DB_WIRE_BODY + r->data_len, 64);
    memcpy(r->revision.generation, p + DB_WIRE_BODY + r->data_len + 64, 32);
    if (db_sync_message_id(r->timestamp, r->accepted_key, &r->revision, r->message_id) < 0) return -1;
    if (memcmp(r->message_id, p, 32)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history wire identity mismatch"); return -1; }
    *ptr = p + DB_WIRE_FIXED + r->data_len;
    return 0;
}
static int db_parse_record(struct DB_SYNC_INSTANCE* si, const uint8_t** ptr, const uint8_t* end, struct db_record* r) {
    return db_parse_record_group(si->group_id, ptr, end, r);
}

static int history_sql_error(sqlite3* db, const char* channel, const char* stage, int rc) {
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL failed ch=%s stage=%s rc=%d: %s", channel, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc)); return -rc;
}
static int db_index_clear(sqlite3* db, const char* channel) {
    char ns[70]; snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_MESSAGES, channel);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "DELETE FROM merkle_tree_hash WHERE namespace=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int end_rc = sqlite3_finalize(st);
    return rc == SQLITE_DONE && end_rc == SQLITE_OK ? 0
        : history_sql_error(db, channel, "clear message index", rc != SQLITE_DONE ? rc : end_rc);
}

/* Snapshot берётся при прежнем admitted; после переноса ни новое членство, ни поздняя реакция его не меняют. */
int db_sync_freeze_feedback(sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t author[32], const uint8_t generation[32]) {
    if (!db || !chat_channel_valid(channel) || !author || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history feedback freeze requires membership transaction and valid author"); return -SQLITE_MISUSE;
    }
    char sql[256]; sqlite3_stmt* st = NULL; const char* stage = "snapshot table";
    struct snapshot { uint8_t id[32]; char* json; } *rows = NULL; size_t count = 0, capacity = 0;
    snprintf(sql, sizeof(sql), "SELECT 1 FROM pragma_table_info('msg_%s') WHERE name='feedback_snapshot'", channel);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int exists = rc == SQLITE_ROW, final = sqlite3_finalize(st); st = NULL;
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) rc = final;
    if (rc != SQLITE_OK) goto fail;
    if (!exists) return 0;
    stage = "snapshot select";
    snprintf(sql, sizeof(sql), "SELECT message_id FROM msg_%s WHERE accepted_key=? AND (?=0 OR generation<>?) AND feedback_snapshot IS NULL", channel);
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, author, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 2, generation != NULL);
    if (rc == SQLITE_OK) rc = generation ? sqlite3_bind_blob(st, 3, generation, 32, SQLITE_STATIC) : sqlite3_bind_null(st, 3);
    if (rc != SQLITE_OK) goto fail;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            struct snapshot* grown = next <= UINT32_MAX / sizeof(*rows) ? u_realloc(rows, next * sizeof(*rows)) : NULL;
            if (!grown) { rc = SQLITE_NOMEM; break; } rows = grown; capacity = next;
        }
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) { rc = SQLITE_CORRUPT; break; }
        const void* message_id = sqlite3_column_blob(st, 0);
        if (!message_id) { rc = SQLITE_NOMEM; break; }
        memcpy(rows[count].id, message_id, 32); rows[count].json = NULL;
        int snapshot = chat_feedback_snapshot(db, self, channel, rows[count].id, 1, &rows[count].json);
        if (snapshot < 0) { count++; rc = -snapshot; break; }
        count++;
    }
    final = sqlite3_finalize(st); st = NULL;
    if (rc == SQLITE_DONE) rc = final;
    if (rc != SQLITE_OK) goto fail;
    stage = "snapshot update";
    for (size_t i = 0; i < count; i++) {
        snprintf(sql, sizeof(sql), "UPDATE msg_%s SET feedback_snapshot=? WHERE message_id=?", channel);
        rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, rows[i].json, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, rows[i].id, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int changed = sqlite3_changes(db); final = sqlite3_finalize(st); st = NULL;
        if (rc == SQLITE_DONE) rc = final != SQLITE_OK ? final : changed == 1 ? SQLITE_OK : SQLITE_CONSTRAINT;
        if (rc != SQLITE_OK) goto fail;
    }
    for (size_t i = 0; i < count; i++) u_free(rows[i].json);
    u_free(rows);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history feedback snapshots staged ch=%s author=%s records=%zu; commit pending",
        channel, sc_public_key_hex(author, (char[65]){0}), count); return 0;
fail:
    final = sqlite3_finalize(st);
    if (final != SQLITE_OK) history_sql_error(db, channel, "snapshot cleanup", final);
    for (size_t i = 0; i < count; i++) u_free(rows[i].json);
    u_free(rows); return history_sql_error(db, channel, stage, rc);
}

/* Архивирование и изменение допуска имеют одну границу commit, включая очистку старого Merkle-корня. */
int db_sync_archive_members(sqlite3* db, const uint8_t self[32], const char* channel) {
    if (!db || !chat_channel_valid(channel) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history archive requires membership transaction"); return -SQLITE_MISUSE;
    }
    char table[96], sql[512]; snprintf(table, sizeof(table), "msg_%s", channel);
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info(?) WHERE name='feedback_snapshot'", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW, final = sqlite3_finalize(st); st = NULL;
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK)
        return history_sql_error(db, channel, "archive table lookup", rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : final);
    if (!found) return 0;
    if (db_schema_history(db, table)) return -SQLITE_ERROR;
    uint8_t (*authors)[32] = NULL; size_t count = 0, capacity = 0; int moved = 0;
    snprintf(sql, sizeof(sql), "SELECT DISTINCT accepted_key FROM %s", table);
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL); if (rc != SQLITE_OK) goto fail;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            uint8_t (*grown)[32] = next <= UINT32_MAX / sizeof(*authors) ? u_realloc(authors, next * sizeof(*authors)) : NULL;
            if (!grown) { rc = SQLITE_NOMEM; break; } authors = grown; capacity = next;
        }
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) { rc = SQLITE_CORRUPT; break; }
        const void* key = sqlite3_column_blob(st, 0);
        if (!key) { rc = SQLITE_NOMEM; break; }
        memcpy(authors[count++], key, 32);
    }
    final = sqlite3_finalize(st); st = NULL;
    if (rc == SQLITE_DONE) rc = final;
    if (rc != SQLITE_OK) goto fail;
    for (size_t i = 0; i < count; i++) {
        uint8_t generation[32] = {0}; uint64_t local_id;
        int active = topo_node_sqlite_member_local_id(db, channel, authors[i], 1, &local_id, &rc);
        if (active == TOPO_SQLITE_LOOKUP_ERROR) goto fail;
        active = active == 0;
        if (active && group_policy_key_generation(db, channel, authors[i], generation)) { rc = SQLITE_ERROR; goto fail; }
        int freeze = db_sync_freeze_feedback(db, self, channel, authors[i], active ? generation : NULL);
        if (freeze < 0) { rc = -freeze; goto fail; }
        snprintf(sql, sizeof(sql), "INSERT INTO archive_%s SELECT * FROM %s WHERE accepted_key=?1 AND (?2=0 OR generation<>?3)",
            table, table);
        rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL); if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_bind_blob(st, 1, authors[i], 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 2, active);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, generation, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int archived = sqlite3_changes(db); final = sqlite3_finalize(st); st = NULL;
        if (rc == SQLITE_DONE) rc = final;
        if (rc != SQLITE_OK) goto fail;
        if (!archived) continue;
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE accepted_key=?1 AND (?2=0 OR generation<>?3)", table);
        rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL); if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_bind_blob(st, 1, authors[i], 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 2, active);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, generation, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int removed = sqlite3_changes(db); final = sqlite3_finalize(st); st = NULL;
        if (rc == SQLITE_DONE) rc = final != SQLITE_OK ? final : removed == archived ? SQLITE_OK : SQLITE_CONSTRAINT;
        if (rc != SQLITE_OK) goto fail;
        moved = 1;
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history archive staged ch=%s author=%s records=%d active=%d",
            channel, sc_public_key_hex(authors[i], (char[65]){0}), archived, active);
    }
    u_free(authors);
    if (moved) { int cleared = db_index_clear(db, channel); if (cleared < 0) return cleared; }
    return moved;
fail:
    final = sqlite3_finalize(st);
    if (final != SQLITE_OK) history_sql_error(db, channel, "archive cleanup", final);
    u_free(authors); return history_sql_error(db, channel, "archive records", rc);
}

void db_sync_archive_committed(struct UTUN_INSTANCE* inst, const char* channel) {
    struct DB_SYNC_INSTANCE* si = si_find(inst, channel);
    if (!si) return;
    db_index_request(si);
    merkle_sync_cancel_dataset_ns(inst, MT_DATA_MESSAGES, channel);
    for (int i = 0; i < si->peer_count; i++) si->peers[i].sync_state = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history archive committed ch=%s; exchanges cancelled, active index rebuild scheduled", channel);
}

int db_sync_message_archived(sqlite3* db, const char* channel, const uint8_t message[32]) {
    if (!db || !chat_channel_valid(channel) || !message) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history archive lookup requires database/channel/message ID"); return -SQLITE_MISUSE;
    }
    char table[96], sql[256]; snprintf(table, sizeof(table), "archive_msg_%s", channel);
    int exists = db_schema_exists(db, table); if (exists <= 0) return exists;
    sqlite3_stmt* st = NULL; snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE message_id=?", table);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, message, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW, final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK)
        return history_sql_error(db, channel, "archive identity lookup", rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : final);
    return found;
}

static int db_update_path_store(struct db_store* store, const uint8_t key[32]) {
    if (!store->indexed) return 0;
    char ns[70]; snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_MESSAGES, store->ns);
    int rc = merkle_tree_update(store->db, ns, db_read64(key), db_store_hash, store);
    if (rc < 0) { store->error = -rc; return db_store_error(store, "Merkle update"); }
    return 0;
}

static void db_committed(struct DB_SYNC_INSTANCE* si, int changed) {
    if (si->index.pending || (changed & 2)) db_index_request(si);
    else merkle_sync_changed(si->db_sync->inst, MT_DATA_MESSAGES, si->ns);
}

static int db_same_record(const struct db_record* a, const struct db_record* b) {
    return a->timestamp == b->timestamp && !memcmp(a->accepted_key, b->accepted_key, 32) && a->data_len == b->data_len &&
        a->revision.ver == b->revision.ver && a->revision.edited_at == b->revision.edited_at &&
        !memcmp(a->revision.nonce, b->revision.nonce, 16) &&
        !memcmp(a->revision.generation, b->revision.generation, 32) &&
        a->sig_len == b->sig_len && !memcmp(a->signature, b->signature, 64) && !memcmp(a->data, b->data, a->data_len);
}

/* Внешняя транзакция обязательна. Входящая подпись уже проверена до захвата write-lock. */
static int db_put_record(struct db_store* store, struct db_record* r, enum db_record_check incoming,
        const char* attrs, int* changed) {
    sqlite3_stmt* st = NULL;
    if (db_store_prepare(store, &st, "SELECT " DB_COLUMNS " FROM \"%s\" WHERE message_id=?") != SQLITE_OK)
        return db_store_error(store, "read duplicate");
    int rc = sqlite3_bind_blob(st, 1, r->message_id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    struct db_record stored;
    int exists = rc == SQLITE_ROW, remove = 0;
    if (exists) {
        enum db_record_check check = db_read_record(store, st, &stored);
        if (check == DB_CHECK_VALID && !(incoming == DB_CHECK_VALID && db_same_record(&stored, r)))
            check = db_verify_author_sig(store, &stored);
        if (check == DB_CHECK_ERROR || check == DB_CHECK_DEFERRED) { sqlite3_finalize(st); return -1; }
        remove = check == DB_CHECK_BAD_FORMAT || check == DB_CHECK_BAD_SIGNATURE;
        if (!remove && !stored.cache_valid) {
            sqlite3_finalize(st); store->rebuild = 1; store->error = SQLITE_CORRUPT;
            return db_store_error(store, "derived identity needs rebuild");
        }
    } else if (rc != SQLITE_DONE) { store->error = rc; sqlite3_finalize(st); return db_store_error(store, "duplicate step"); }
    int same = exists && !remove && db_same_record(&stored, r);
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) { store->error = rc; return db_store_error(store, "duplicate finalize"); }
    if (remove) {
        if (db_delete_bad_row(store, stored.rowid) < 0) return -1;
        if (store->indexed && db_index_clear(store->db, store->ns) < 0) return -1;
        *changed |= 3;
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history staged deletion of proven corrupt duplicate tbl=%s rowid=%lld",
            store->table_name, (long long)stored.rowid);
    }
    if (incoming != DB_CHECK_VALID) return -2;
    if (exists && !remove) {
        if (r->revision.ver < stored.revision.ver || same) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history duplicate/stale tbl=%s id=%02x%02x%02x%02x incoming=%llu stored=%llu",
                store->table_name, r->message_id[0], r->message_id[1], r->message_id[2], r->message_id[3],
                (unsigned long long)r->revision.ver, (unsigned long long)stored.revision.ver); return 1;
        }
        if (r->revision.ver == stored.revision.ver) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history version conflict tbl=%s id=%02x%02x%02x%02x ver=%llu",
                store->table_name, r->message_id[0], r->message_id[1], r->message_id[2], r->message_id[3],
                (unsigned long long)r->revision.ver); return -2;
        }
        r->updated = 1; r->id = stored.id;
        if (db_store_prepare(store, &st, "UPDATE \"%s\" SET data=?,author_signature=?,ver=?,edited_at=?,"
            "flags=flags & ~1,delivered_peers=0,delivery_chain='' WHERE message_id=?") != SQLITE_OK) return db_store_error(store, "update prepare");
        rc = sqlite3_bind_blob(st, 1, r->data, (int)r->data_len, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, r->signature, 64, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)r->revision.ver);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)r->revision.edited_at);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 5, r->message_id, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int end_rc = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || end_rc != SQLITE_OK) {
            store->error = rc != SQLITE_DONE ? rc : end_rc; return db_store_error(store, "update step");
        }
        if (db_update_path_store(store, r->message_id) < 0) return -1;
        *changed |= 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history updated tbl=%s id=%02x%02x%02x%02x ver=%llu->%llu edited=%llu",
            store->table_name, r->message_id[0], r->message_id[1], r->message_id[2], r->message_id[3],
            (unsigned long long)stored.revision.ver, (unsigned long long)r->revision.ver,
            (unsigned long long)r->revision.edited_at); return 0;
    }
    if (db_store_prepare(store, &st, "INSERT INTO \"%s\" (timestamp,id,flags,data,author_signature,message_id,bucket,local_attrs,nonce,ver,edited_at,"
        "generation,accepted_key) VALUES(?,?,0,?,?,?,?,?,?,?,?,?,?)") != SQLITE_OK) return db_store_error(store, "insert prepare");
    rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)r->timestamp);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)r->id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, r->data, (int)r->data_len, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, r->signature, 64, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 5, r->message_id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 6, (sqlite3_int64)(db_read64(r->message_id) >> 39));
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 7, attrs ? attrs : "", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 8, r->revision.nonce, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 9, (sqlite3_int64)r->revision.ver);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 10, (sqlite3_int64)r->revision.edited_at);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 11, r->revision.generation, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 12, r->accepted_key, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int final_rc = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final_rc != SQLITE_OK) {
        store->error = rc != SQLITE_DONE ? rc : final_rc; return db_store_error(store, "insert step");
    }
    if (!(*changed & 2) && db_update_path_store(store, r->message_id) < 0) return -1;
    *changed |= 1; return 0;
}

int db_sync_scope_snapshot(struct DB_SYNC_INSTANCE* si, struct db_sync_scope* out) {
    if (!si || !si->enabled || !out) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history scope snapshot requires live instance/output"); return -1;
    }
    struct db_store store = db_store_for_instance(si);
    memset(out, 0, sizeof(*out)); out->group_id = store.group_id; out->indexed = store.indexed;
    strcpy(out->channel, store.ns);
    out->instance_id = si->generation;
    snprintf(out->table, sizeof(out->table), "%s", store.table_name); return 0;
}
int db_sync_scope_list(struct UTUN_INSTANCE* inst, struct db_sync_scope** out, size_t* count) {
    if (out) *out = NULL; if (count) *count = 0;
    struct DB_SYNC* db = inst ? inst->db_sync : NULL;
    if (!out || !count || !db || !db->enabled || db->quiescing) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history scope list requires active owner/output"); return -1;
    }
    if (!db->instance_count) return 0;
    struct db_sync_scope* scopes = u_calloc((uint32_t)db->instance_count, sizeof(*scopes));
    if (!scopes) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history scope list allocation failed count=%d", db->instance_count); return -1; }
    for (int i = 0; i < db->instance_count; i++) db_sync_scope_snapshot(db->instances[i], &scopes[i]);
    *out = scopes; *count = (size_t)db->instance_count; return 0;
}

/* История допускает opaque payload; JSON envelope разбирает attachment helper в той же WRITE. */
static int db_record_attachment(sqlite3* db, const char* channel, const struct db_record* record) {
    size_t offset = 0;
    while (offset < record->data_len && strchr(" \r\n\t", record->data[offset])) offset++;
    if (offset == record->data_len || record->data[offset] != '{') return SQLITE_OK;
    return chat_attachment_observe_write(db, channel, record->message_id, record->accepted_key, record->data, record->data_len, NULL);
}

int db_sync_record_write(sqlite3* db, const struct db_sync_scope* scope, struct db_sync_record* record) {
    if (!db || sqlite3_get_autocommit(db) || !scope || !record || !chat_channel_valid(scope->channel) || !record->data || !record->size ||
        record->size > DB_SYNC_MESSAGE_MAX || !record->local_id ||
        !memchr(scope->table, 0, sizeof(scope->table)) || !scope->table[0] ||
        strspn(scope->table, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != strlen(scope->table)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL record invalid input"); return SQLITE_MISUSE;
    }
    /* Replay/worker может добавить историю раньше зарезервированного runtime номера. */
    char id_sql[512]; sqlite3_stmt* id_st=NULL;
    snprintf(id_sql,sizeof(id_sql),"SELECT CASE WHEN EXISTS(SELECT 1 FROM %s WHERE id=?1 AND message_id<>?2)"
        " THEN (SELECT COALESCE(max(id),0)+1 FROM %s) ELSE ?1 END",scope->table,scope->table);
    uint8_t identity[32];
    if(db_sync_message_id(record->timestamp,record->author_ed,&record->revision,identity)) return SQLITE_ERROR;
    int id_rc=sqlite3_prepare_v2(db,id_sql,-1,&id_st,NULL);
    if(!id_rc) id_rc=sqlite3_bind_int64(id_st,1,(sqlite3_int64)record->local_id);
    if(!id_rc) id_rc=sqlite3_bind_blob(id_st,2,identity,32,SQLITE_STATIC);
    if(!id_rc) id_rc=sqlite3_step(id_st);
    uint64_t available=id_rc==SQLITE_ROW?(uint64_t)sqlite3_column_int64(id_st,0):0; int id_final=sqlite3_finalize(id_st);
    if(id_rc!=SQLITE_ROW || id_final || !available || available>INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,"history local ID allocation failed table=%s rc=%d finalize=%d",scope->table,id_rc,id_final);
        return id_rc==SQLITE_ROW?(id_final?id_final:SQLITE_FULL):id_rc;
    }
    record->local_id=available;
    struct db_store store = {.db=db, .group_id=scope->group_id, .indexed=scope->indexed};
    snprintf(store.table_name, sizeof(store.table_name), "%s", scope->table);
    strcpy(store.ns, scope->channel);
    struct db_record r = {.timestamp=record->timestamp, .author=record->author, .id=record->local_id,
        .revision=record->revision, .data=record->data, .data_len=record->size, .sig_len=64};
    memcpy(r.signature, record->signature, 64);
    memcpy(r.accepted_key, record->author_ed, 32);
    memcpy(r.message_id, identity, 32);
    record->result = -1; record->changed = record->updated = record->needs_members = record->rebuild = 0;
    enum db_record_check check = db_verify_incoming(&store, &r);
    int rc;
    if (check == DB_CHECK_ERROR || check == DB_CHECK_DEFERRED) rc = db_store_error(&store, "incoming verification");
    else rc = db_put_record(&store, &r, check, record->local_attrs, &record->changed);
    record->result = rc; record->updated = r.updated; record->local_id = r.id;
    record->needs_members = r.needs_members; record->rebuild = store.rebuild || (!scope->indexed && record->changed);
    memcpy(record->message_id, r.message_id, 32);
    record->author = r.author;
    return rc == -1 ? (store.error ? store.error : SQLITE_ERROR) : rc < 0 ? SQLITE_OK : db_record_attachment(db, scope->channel, &r);
}

int db_sync_project_write(sqlite3* db,const struct db_sync_scope* scope,struct db_sync_record* record) {
    if(!db || sqlite3_get_autocommit(db) || !scope || !record || !record->local_id || !record->data || !record->size) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,"CHAT projection requires WRITE and complete record"); return SQLITE_MISUSE;
    }
    struct db_store store={.db=db,.group_id=scope->group_id};
    strcpy(store.ns, scope->channel);
    snprintf(store.table_name,sizeof(store.table_name),"msg_%s",store.ns);
    if(strcmp(store.table_name,scope->table) || chat_log_message_check(db,store.ns,record)!=1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,"CHAT projection reference invalid ch=%s",store.ns); return SQLITE_AUTH;
    }
    struct db_record r={.timestamp=record->timestamp,.author=record->author,.id=record->local_id,.revision=record->revision,
        .data=record->data,.data_len=record->size,.sig_len=64};
    memcpy(r.signature,record->signature,64); memcpy(r.message_id,record->message_id,32);
    memcpy(r.accepted_key, record->author_ed, 32);
    sqlite3_stmt* st=NULL;
    int rc=db_store_prepare(&store,&st,"DELETE FROM %s WHERE message_id=?");
    if(!rc) rc=sqlite3_bind_blob(st,1,r.message_id,32,SQLITE_STATIC);
    if(!rc) rc=sqlite3_step(st);
    int final=sqlite3_finalize(st);
    if(rc!=SQLITE_DONE || final) return store.error?store.error:rc==SQLITE_DONE?final:rc;
    int changed=0;
    if(db_put_record(&store,&r,DB_CHECK_VALID,record->local_attrs,&changed)<0) return store.error?store.error:SQLITE_ERROR;
    record->local_id=r.id; record->changed=changed; record->result=0; return SQLITE_OK;
}

int db_sync_record_reserve(struct DB_SYNC_INSTANCE* si, struct db_sync_scope* scope, struct db_sync_record* record) {
    if (!record || db_sync_scope_snapshot(si, scope) < 0 || si->next_id >= INT64_MAX || si->last_timestamp_ms >= INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history local reservation failed"); return -1;
    }
    record->local_id = si->next_id++; record->timestamp = db_sync_next_timestamp(si); return 0;
}

/* Предварительная проверка идёт без write-lock; внутри короткой транзакции сверяем прочитанные байты. */
static int db_index_batch(struct DB_SYNC_INSTANCE* si) {
    struct db_store store = db_store_for_instance(si);
    sqlite3* db = SI_DB(si);
    sqlite3_stmt* st = NULL;
    if (!merkle_sync_registered(si->db_sync->inst, MT_DATA_MESSAGES)) return 1;
    if (si->index.restart) {
        merkle_sync_cancel_dataset_ns(si->db_sync->inst, MT_DATA_MESSAGES, si->ns);
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return db_error(si, "index begin");
        if (db_index_clear(db, si->ns) < 0 || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            db_rollback(si, "index reset"); return -1;
        }
        si->index.restart = 0; si->index.phase = 0; si->index.after_valid = 0; si->index.after = 0;
    }
    if (si->index.phase == 0) {
        struct db_checked* rows = u_calloc(DB_BATCH_RECORDS, sizeof(*rows));
        if (!rows) return db_error(si, "preflight allocation");
        const char* query = si->index.after_valid
            ? "SELECT " DB_COLUMNS " FROM \"%s\" WHERE rowid>? ORDER BY rowid LIMIT 16"
            : "SELECT " DB_COLUMNS " FROM \"%s\" ORDER BY rowid LIMIT 16";
        int rc = si_prep(si, &st, query), count = 0, removed = 0;
        if (rc == SQLITE_OK && si->index.after_valid) rc = sqlite3_bind_int64(st, 1, si->index.after);
        if (rc != SQLITE_OK) goto verify_error;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            struct db_checked* item = &rows[count];
            item->check = db_read_record(&store, st, &item->record);
            if (item->check == DB_CHECK_VALID) {
                memcpy(item->data, item->record.data, item->record.data_len); item->record.data = item->data;
                item->check = db_verify_author_sig(&store, &item->record);
            }
            if (item->check == DB_CHECK_ERROR || item->check == DB_CHECK_DEFERRED) {
                si->index.waiting_key = item->check == DB_CHECK_DEFERRED;
                goto verify_error;
            }
            count++;
        }
        int final_rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_DONE || final_rc != SQLITE_OK) goto verify_error;
        st = NULL;
        if (!count) {
            si->index.phase = 1; si->index.after_valid = 0; si->index.after = -1;
            u_free(rows); return 0;
        }
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto verify_error;
        for (int i = 0; i < count; i++) {
            struct db_checked* item = &rows[i]; struct db_record current;
            if (si_prep(si, &st, "SELECT " DB_COLUMNS " FROM \"%s\" WHERE rowid=?") != SQLITE_OK) goto verify_rollback;
            if (sqlite3_bind_int64(st, 1, item->record.rowid) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) goto verify_rollback;
            enum db_record_check now = db_read_record(&store, st, &current);
            int matches = now == DB_CHECK_BAD_FORMAT && item->check == DB_CHECK_BAD_FORMAT;
            if (now == DB_CHECK_VALID && item->check != DB_CHECK_BAD_FORMAT) matches = db_same_record(&item->record, &current);
            if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto verify_rollback; }
            st = NULL;
            if (!matches) {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history preflight source changed tbl=%s; retry", SI_TBL(si)); goto verify_rollback;
            }
            if (item->check == DB_CHECK_BAD_FORMAT || item->check == DB_CHECK_BAD_SIGNATURE) {
                if (db_delete_bad_row(&store, item->record.rowid) < 0) goto verify_rollback;
                removed++;
            } else {
                if (si_prep(si, &st, "UPDATE \"%s\" SET message_id=?,bucket=? WHERE rowid=?") != SQLITE_OK) goto verify_rollback;
                if (sqlite3_bind_blob(st, 1, item->record.message_id, 32, SQLITE_STATIC) != SQLITE_OK ||
                    sqlite3_bind_int64(st, 2, (sqlite3_int64)(db_read64(item->record.message_id) >> 39)) != SQLITE_OK ||
                    sqlite3_bind_int64(st, 3, item->record.rowid) != SQLITE_OK || sqlite3_step(st) != SQLITE_DONE) goto verify_rollback;
                if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto verify_rollback; }
                st = NULL;
            }
        }
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto verify_rollback;
        si->index.after = rows[count - 1].record.rowid; si->index.after_valid = 1; si->index.waiting_key = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history preflight committed tbl=%s checked=%d removed=%d after=%lld",
            SI_TBL(si), count, removed, (long long)si->index.after);
        if (removed) DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history removed %d proven invalid stored records tbl=%s", removed, SI_TBL(si));
        u_free(rows); return 0;
verify_rollback:
        sqlite3_finalize(st); st = NULL; db_rollback(si, "preflight commit");
verify_error:
        sqlite3_finalize(st); u_free(rows); return db_error(si, si->index.waiting_key ? "preflight waiting for author key" : "preflight failed");
    }

    /* Все сохранённые подписи проверены; в write-транзакции остаётся только хеширование ограниченной пачки листьев. */
    sqlite3_int64 buckets[DB_BATCH_RECORDS]; int count = 0, rc;
    if (si_prep(si, &st, "SELECT DISTINCT bucket FROM \"%s\" WHERE bucket>? ORDER BY bucket LIMIT 16") != SQLITE_OK)
        return db_error(si, "index buckets");
    if (sqlite3_bind_int64(st, 1, si->index.after) != SQLITE_OK) { sqlite3_finalize(st); return db_error(si, "bucket bind"); }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) buckets[count++] = sqlite3_column_int64(st, 0);
    int end_rc = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || end_rc != SQLITE_OK) return db_error(si, "bucket step");
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return db_error(si, "hash begin");
    for (int i = 0; i < count; i++)
        if (merkle_sync_recompute_path(si->db_sync->inst, MT_DATA_MESSAGES, si->ns, (uint64_t)buckets[i] << 39) < 0) {
            db_rollback(si, "hash batch"); return -1;
        }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) { db_rollback(si, "hash commit"); return -1; }
    if (count) si->index.after = buckets[count - 1];
    else {
        si->index.pending = 0;
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history verified index ready tbl=%s group=%s", SI_TBL(si), si->ns);
        merkle_sync_changed(si->db_sync->inst, MT_DATA_MESSAGES, si->ns);
        for (int i = 0; i < si->peer_count; i++) db_start_peer(si, si->peers[i].ed);
    }
    return 0;
}

struct db_index_job {
    struct db_sync_scope scope;
    struct UTUN_INSTANCE* inst; /* Только guard/done. */
    uint64_t generation, revision;
    sqlite3_int64 after;
    int restart, phase, after_valid, pending, waiting_key;
};
static struct DB_SYNC_INSTANCE* db_task_current(struct UTUN_INSTANCE* inst, uint64_t generation, const struct db_sync_scope* scope) {
    if (!generation || inst->destroying || !inst->chat_content_started || !inst->db_sync || inst->db_sync->quiescing ||
        chat_core_generation(inst) != generation) return NULL;
    struct DB_SYNC_INSTANCE* si = db_sync_instance_find(inst, scope->group_id);
    return si && si->generation == scope->instance_id ? si : NULL;
}
static struct DB_SYNC_INSTANCE* db_index_current(struct db_index_job* job) {
    return db_task_current(job->inst, job->generation, &job->scope);
}
static int db_index_guard(void* data) {
    struct db_index_job* job = data; struct DB_SYNC_INSTANCE* si = db_index_current(job);
    return si && si->index.pending && si->index.revision == job->revision;
}
static int db_index_work(sqlite3* db, void* data) {
    struct db_index_job* job = data;
    struct db_store store = {.db=db, .group_id=job->scope.group_id};
    snprintf(store.table_name, sizeof(store.table_name), "%s", job->scope.table);
    strcpy(store.ns, job->scope.channel);
    int rc = SQLITE_OK;
    if (job->restart) {
        rc = db_index_clear(db, store.ns); if (rc < 0) return -rc;
        job->restart = job->phase = job->after_valid = 0; job->after = 0;
    }
    sqlite3_stmt* st = NULL; int count = 0;
    if (!job->phase) {
        struct db_checked* rows = u_calloc(DB_BATCH_RECORDS, sizeof(*rows));
        if (!rows) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history worker preflight allocation failed"); return SQLITE_NOMEM; }
        rc = db_store_prepare(&store, &st, job->after_valid
            ? "SELECT " DB_COLUMNS " FROM \"%s\" WHERE rowid>? ORDER BY rowid LIMIT 16"
            : "SELECT " DB_COLUMNS " FROM \"%s\" ORDER BY rowid LIMIT 16");
        if (rc == SQLITE_OK && job->after_valid) rc = sqlite3_bind_int64(st, 1, job->after);
        while (rc == SQLITE_OK && (rc = sqlite3_step(st)) == SQLITE_ROW) {
            struct db_checked* item = &rows[count]; item->check = db_read_record(&store, st, &item->record);
            if (item->check == DB_CHECK_VALID) {
                memcpy(item->data, item->record.data, item->record.data_len); item->record.data = item->data;
                item->check = db_verify_author_sig(&store, &item->record);
            }
            if (item->check == DB_CHECK_DEFERRED) { job->waiting_key = 1; rc = SQLITE_BUSY; break; }
            if (item->check == DB_CHECK_ERROR) { rc = sqlite3_errcode(db); if (rc == SQLITE_OK) rc = SQLITE_ERROR; break; }
            count++; rc = SQLITE_OK;
        }
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
        int final = sqlite3_finalize(st); st = NULL; if (rc == SQLITE_OK) rc = final;
        for (int i = 0; i < count && rc == SQLITE_OK; i++) {
            struct db_checked* item = &rows[i];
            if (item->check == DB_CHECK_BAD_SIGNATURE || item->check == DB_CHECK_BAD_FORMAT) {
                if (db_delete_bad_row(&store, item->record.rowid) < 0) rc = store.error;
            } else {
                rc = db_store_prepare(&store, &st, "UPDATE \"%s\" SET message_id=?,bucket=? WHERE rowid=?");
                if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, item->record.message_id, 32, SQLITE_STATIC);
                if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(db_read64(item->record.message_id) >> 39));
                if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, item->record.rowid);
                if (rc == SQLITE_OK) rc = sqlite3_step(st);
                final = sqlite3_finalize(st); st = NULL;
                rc = rc == SQLITE_DONE ? final : rc;
            }
        }
        if (rc == SQLITE_OK) {
            if (count) { job->after = rows[count - 1].record.rowid; job->after_valid = 1; }
            else { job->phase = 1; job->after = -1; job->after_valid = 0; }
        }
        u_free(rows);
    } else {
        sqlite3_int64 buckets[DB_BATCH_RECORDS];
        rc = db_store_prepare(&store, &st, "SELECT DISTINCT bucket FROM \"%s\" WHERE bucket>? ORDER BY bucket LIMIT 16");
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, job->after);
        while (rc == SQLITE_OK && (rc = sqlite3_step(st)) == SQLITE_ROW) { buckets[count++] = sqlite3_column_int64(st, 0); rc = SQLITE_OK; }
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
        int final = sqlite3_finalize(st); st = NULL; if (rc == SQLITE_OK) rc = final;
        char ns[70]; snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_MESSAGES, store.ns);
        for (int i = 0; i < count && rc == SQLITE_OK; i++) {
            int updated = merkle_tree_update(db, ns, (uint64_t)buckets[i] << 39, db_store_hash, &store);
            if (updated < 0) rc = -updated;
        }
        if (rc == SQLITE_OK) { if (count) job->after = buckets[count - 1]; else job->pending = 0; }
    }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history worker preflight failed tbl=%s phase=%d after=%lld waiting=%d rc=%d",
        store.table_name, job->phase, (long long)job->after, job->waiting_key, rc);
    else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history worker preflight staged tbl=%s phase=%d checked=%d after=%lld pending=%d",
        store.table_name, job->phase, count, (long long)job->after, job->pending);
    return rc;
}
static void db_index_schedule(struct DB_SYNC_INSTANCE* si, uint64_t delay) {
    if (si->index.timer || !si->index.pending || !merkle_sync_registered(si->db_sync->inst, MT_DATA_MESSAGES)) return;
    si->index.timer = uasync_set_timeout(si->db_sync->inst->ua, delay, si, db_index_tick, "history_preflight");
    if (!si->index.timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history preflight timer allocation failed tbl=%s", si->table_name);
        db_sync_resume_peer_check(si->db_sync);
    }
}
static void db_index_done(void* data, const struct db_async_result* result) {
    struct db_index_job* job = data; struct DB_SYNC_INSTANCE* si = db_index_current(job);
    if (!si) return;
    si->index.running = 0;
    if (!result->status && si->index.revision == job->revision) {
        si->index.after = job->after; si->index.phase = job->phase; si->index.after_valid = job->after_valid;
        si->index.restart = job->restart; si->index.pending = job->pending; si->index.waiting_key = 0;
        if (!job->pending) {
            DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history worker verified index ready tbl=%s", si->table_name);
            merkle_sync_changed(job->inst, MT_DATA_MESSAGES, si->ns);
            for (int i = 0; i < si->peer_count; i++) db_start_peer(si, si->peers[i].ed);
        }
    } else {
        si->index.waiting_key = job->waiting_key;
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history preflight retry tbl=%s status=%d rc=%d revision=%llu/%llu",
            si->table_name, result->status, result->sqlite_code, (unsigned long long)job->revision, (unsigned long long)si->index.revision);
    }
    db_index_schedule(si, result->status ? 50000 : 1);
}
static void db_index_cleanup(void* data) { u_free(data); }
static void db_index_tick(void* arg) {
    struct DB_SYNC_INSTANCE* si = arg; si->index.timer = NULL;
    if (!si->enabled || si->db_sync->quiescing || !si->index.pending) return;
    if (db_async_can_write(si->db_sync->inst->db_worker)) {
        if (si->index.running) return;
        struct db_index_job* job = u_calloc(1, sizeof(*job));
        if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history preflight request allocation failed tbl=%s", si->table_name);
            db_index_schedule(si, 50000); return; }
        db_sync_scope_snapshot(si, &job->scope); job->inst = si->db_sync->inst; job->generation = chat_core_generation(job->inst);
        job->revision = si->index.revision; job->after = si->index.after; job->phase = si->index.phase;
        job->after_valid = si->index.after_valid; job->restart = si->index.restart; job->pending = 1; si->index.running = 1;
        if (job->restart) merkle_sync_cancel_dataset_ns(job->inst, MT_DATA_MESSAGES, si->ns);
        struct db_async_task task = {.name="history-preflight", .mode=DB_ASYNC_WRITE, .data=job,
            .guard=db_index_guard, .work=db_index_work, .done=db_index_done, .cleanup=db_index_cleanup};
        db_async_run(job->inst->db_worker, &task); return;
    }
    int rc = db_index_batch(si);
    if (si->index.pending && merkle_sync_registered(si->db_sync->inst, MT_DATA_MESSAGES)) {
        si->index.timer = uasync_set_timeout(si->db_sync->inst->ua, rc ? 50000 : 1, si, db_index_tick, "history_preflight");
        if (!si->index.timer) { db_error(si, "preflight timer"); db_sync_resume_peer_check(si->db_sync); }
    }
}
static void db_index_request(struct DB_SYNC_INSTANCE* si) {
    si->index.revision++;
    si->index.pending = 1; si->index.restart = 1; si->index.waiting_key = 0;
    if (!si->index.timer && merkle_sync_registered(si->db_sync->inst, MT_DATA_MESSAGES))
        si->index.timer = uasync_set_timeout(si->db_sync->inst->ua, 0, si, db_index_tick, "history_preflight");
    db_sync_resume_peer_check(si->db_sync);
}

/* Чтение листа — одинаковый канонический отбор для корня и передачи. Проверка авторства уже выполнена preflight/put. */
static int db_store_hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    struct db_store* store = ctx; (void)ns;
    if (!store || level != MT_MAX_LEVEL) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL leaf invalid level=%u", level); return -SQLITE_MISUSE; }
    sqlite3_stmt* st = NULL;
    int prepared = db_store_prepare(store, &st, "SELECT " DB_COLUMNS " FROM \"%s\" WHERE bucket=? ORDER BY message_id");
    if (prepared) return -prepared;
    int rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)(prefix >> 39)), count = 0, bad = 0;
    if (rc != SQLITE_OK) { sqlite3_finalize(st); return -rc; }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct db_record r; uint8_t wire[DB_WIRE_MAX];
        enum db_record_check check = db_read_record(store, st, &r);
        if (check != DB_CHECK_VALID || !r.cache_valid) { bad = check != DB_CHECK_ERROR; rc = SQLITE_ERROR; break; }
        int bytes = db_encode_record(&r, wire);
        if (EVP_DigestUpdate(hash, wire, (size_t)bytes) != 1) { rc = SQLITE_ERROR; break; }
        count++;
    }
    int end_rc = sqlite3_finalize(st);
    if (bad) store->rebuild = 1;
    if (rc != SQLITE_DONE || end_rc != SQLITE_OK) {
        int error = rc != SQLITE_DONE ? rc : end_rc;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL leaf failed tbl=%s step=%d final=%d", store->table_name, rc, end_rc);
        return -error;
    }
    return count;
}
static int db_message_hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    struct DB_SYNC_INSTANCE* si = si_find(ctx, ns);
    if (!si) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history leaf missing channel=%s", ns); return -SQLITE_NOTFOUND; }
    struct db_store store = db_store_for_instance(si); int rc = db_store_hash(&store, ns, level, prefix, hash);
    if (store.rebuild) db_index_request(si);
    return rc;
}

/* SQL-owner: выдача не меняет runtime, rebuild возвращается в frozen scope. */
static int db_store_page(struct db_store* store, uint64_t prefix, int av, const merkle_key_t* after,
        uint8_t* buf, size_t* len, merkle_key_t* next, int* more) {
    if (!after || !buf || !len || !next || !more) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL page invalid arguments"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    const char* query = av
        ? "SELECT " DB_COLUMNS " FROM \"%s\" WHERE bucket=? AND message_id>? ORDER BY message_id LIMIT 17"
        : "SELECT " DB_COLUMNS " FROM \"%s\" WHERE bucket=? ORDER BY message_id LIMIT 17";
    int rc = db_store_prepare(store, &st, query), count = 0;
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)(prefix >> 39));
    if (rc == SQLITE_OK && av) rc = sqlite3_bind_blob(st, 2, after->bytes, 32, SQLITE_STATIC);
    size_t used = 0; *more = 0; memset(next, 0, sizeof(*next));
    while (rc == SQLITE_OK && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct db_record record;
        enum db_record_check check = db_read_record(store, st, &record);
        if (check == DB_CHECK_VALID) check = db_verify_author_sig(store, &record);
        if (check != DB_CHECK_VALID || !record.cache_valid) {
            store->rebuild = check == DB_CHECK_BAD_SIGNATURE || check == DB_CHECK_BAD_FORMAT ||
                (check == DB_CHECK_VALID && !record.cache_valid);
            rc = store->error ? store->error : check == DB_CHECK_ERROR ? SQLITE_ERROR
                : check == DB_CHECK_DEFERRED ? SQLITE_BUSY : SQLITE_CONSTRAINT; break;
        }
        size_t bytes = DB_WIRE_FIXED + record.data_len;
        if (count == DB_BATCH_RECORDS || used + bytes > *len) {
            if (!count) rc = SQLITE_TOOBIG;
            else { *more = 1; rc = SQLITE_OK; }
            break;
        }
        used += (size_t)db_encode_record(&record, buf + used); memcpy(next->bytes, record.message_id, 32); count++; rc = SQLITE_OK;
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (rc == SQLITE_OK) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL page failed tbl=%s rc=%d rebuild=%d", store->table_name, rc, store->rebuild);
    else *len = used;
    return rc;
}
static int db_message_page(void* ctx, const char* ns, uint64_t prefix, int av, const merkle_key_t* after,
        uint8_t* buf, size_t* len, merkle_key_t* next, int* more) {
    struct DB_SYNC_INSTANCE* si = si_find(ctx, ns);
    if (!si || si->index.pending) return -1;
    struct db_store store = db_store_for_instance(si);
    int rc = db_store_page(&store, prefix, av, after, buf, len, next, more);
    if (store.rebuild) db_index_request(si);
    return rc == SQLITE_OK ? 0 : -1;
}

static void db_done_sweep(struct DB_SYNC* db) {
    if (db->dispatch_depth) return;
    struct db_sync_done_cbk_entry** link = &db->done_cbks;
    while (*link) {
        struct db_sync_done_cbk_entry* e = *link;
        if (e->removed) { *link = e->next; u_free(e); } else link = &e->next;
    }
}
int db_sync_add_done_cbk(struct UTUN_INSTANCE* inst, db_sync_done_fn fn, void* arg) {
    struct DB_SYNC* db = inst ? inst->db_sync : NULL;
    if (!db || !fn) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history done subscription requires context/callback"); return -1; }
    for (struct db_sync_done_cbk_entry* current = db->done_cbks; current; current = current->next)
        if (!current->removed && current->fn == fn && current->arg == arg) return 0;
    struct db_sync_done_cbk_entry* e = u_calloc(1, sizeof(*e));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history done subscription allocation"); return -1; }
    e->fn = fn; e->arg = arg; e->next = db->done_cbks; db->done_cbks = e;
    return 0;
}
void db_sync_remove_done_cbk(struct UTUN_INSTANCE* inst, db_sync_done_fn fn, void* arg) {
    struct DB_SYNC* db = inst ? inst->db_sync : NULL;
    if (!db) return;
    for (struct db_sync_done_cbk_entry* e = db->done_cbks; e; e = e->next)
        if (!e->removed && e->fn == fn && e->arg == arg) { e->removed = 1; break; }
    db_done_sweep(db);
}
static void db_message_result(void* ctx, const char* ns, const uint8_t peer[32], int result) {
    struct DB_SYNC_INSTANCE* si = si_find(ctx, ns);
    if (!si || si->db_sync->quiescing) return;
    struct SI_PEER* p = si_peer_add(si, peer);
    if (!p) return;
    p->sync_state = result == MT_OK ? 2 : 0;
    if (result != MT_OK) {
        db_sync_resume_peer_check(si->db_sync);
        if (result == MT_ERR_CONFLICT) db_request_members(si, peer);
        else if (result == MT_ERR_IO) db_index_request(si);
        return;
    }
    struct DB_SYNC* db = si->db_sync;
    si->holds++; db->dispatch_depth++;
    for (struct db_sync_done_cbk_entry* e = db->done_cbks; e && si->enabled; e = e->next)
        if (!e->removed) e->fn(si, peer, e->arg);
    db->dispatch_depth--; db_done_sweep(db); db_release(si);
}

void db_sync_journal_synchronized(struct UTUN_INSTANCE* inst,const char* channel,const uint8_t peer[32]) {
    db_message_result(inst,channel,peer,MT_OK);
}

/* Текущий допуск CHAT и точный ключ ETCP обязательны даже при старом READY.
 * Настройки разрешены до готовности истории, но не через исключение MEMBERS/JOIN. */
static int db_peer_allowed(struct DB_SYNC_INSTANCE* si, struct ETCP_CONN* conn) {
    if (!si || !si->enabled || si->db_sync->quiescing) return 0;
    struct UTUN_INSTANCE* inst = si->db_sync->inst;
    return conn && conn->links_up && !conn->close_requested && topo_group_chat_connection_allowed(topo_groups_find(inst->topo_groups, si->group_id), conn);
}

static int db_message_peer(void* ctx, const char* ns, struct ETCP_CONN* conn) {
    struct UTUN_INSTANCE* inst = ctx;
    struct DB_SYNC_INSTANCE* si = si_find(inst, ns);
    struct TOPO_GROUP* group = si ? topo_groups_find(inst->topo_groups, si->group_id) : NULL;
    struct db_store store=si?db_store_for_instance(si):(struct db_store){0};
    return si && !db_chat_store(&store) && !si->sync_gated && !si->index.pending && topo_group_connection_ready(group, conn) && db_peer_allowed(si, conn);
}
static int db_sync_peer_throttled(struct DB_SYNC* db, struct SI_PEER* p) {
    struct ETCP_CONN* conn = p && !p->sound_on ? instance_find_conn_by_ed(db->inst, p->ed) : NULL;
    return conn && conn->peer_sleep_phase;
}
static int db_message_can_run(void* ctx, const char* ns, struct ETCP_CONN* conn) {
    struct DB_SYNC_INSTANCE* si = si_find(ctx, ns);
    if (!si || !db_message_peer(ctx, ns, conn)) return 0;
    struct SI_PEER* p = si_peer_find(si, conn->peer_ed25519_pubkey);
    return !p || (!p->paused && !p->needs_members && (p->sound_on || !conn->peer_sleep_phase));
}

/* Журнал независим от готовности производного MESSAGES: он сам восстанавливает этот индекс. */
int db_sync_journal_can_run(void* ctx, const char* ns, struct ETCP_CONN* conn) {
    struct DB_SYNC_INSTANCE* si = si_find(ctx, ns);
    if (!si || !db_peer_allowed(si, conn)) return 0;
    struct SI_PEER* p = si_peer_find(si, conn->peer_ed25519_pubkey);
    return !p || (!p->paused && (p->sound_on || !conn->peer_sleep_phase));
}

/* Успех MEMBERS открывает историю заново; context — ядро, а не экземпляр таблицы, который UI мог уже удалить. */
static void db_members_done(const uint8_t peer[32], const char* channel, int result, void* ctx) {
    struct DB_SYNC_INSTANCE* si = si_find(ctx, channel);
    if (si && si->db_sync->quiescing) return;
    struct SI_PEER* p = si ? si_peer_find(si, peer) : NULL;
    if (!p) return;
    p->needs_members = result == MT_OK ? 0 : 1; p->sync_state = 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history dependency MEMBERS ch=%s peer=%s result=%d", channel,
        sc_public_key_hex(peer, (char[65]){0}), result);
    if (result == MT_OK) db_start_peer(si, peer);
    db_sync_resume_peer_check(si->db_sync);
}

/* Старый состав у соседа не должен создавать бесконечный обмен несовпадающими корнями MESSAGES. */
static void db_request_members(struct DB_SYNC_INSTANCE* si, const uint8_t peer[32]) {
    struct SI_PEER* p = si_peer_add(si, peer);
    if (!p) return;
    if (!p->needs_members) p->needs_members = 1;
    db_sync_resume_peer_check(si->db_sync);
    uint64_t now = get_time_tb();
    struct ETCP_CONN* conn = instance_find_conn_by_ed(si->db_sync->inst, peer);
    if (p->needs_members == 2 || p->paused || now < p->member_retry_at || !db_message_peer(si->db_sync->inst, si->ns, conn)) return;
    p->member_retry_at = now + DB_SYNC_PEER_CHECK_INTERVAL * 10000ULL;
    if (member_sync_start(si->db_sync->inst, conn, si->ns, db_members_done, si->db_sync->inst) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history dependency MEMBERS start failed ch=%s peer=%s", si->ns,
            sc_public_key_hex(peer, (char[65]){0})); return;
    }
    p->needs_members = 2;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history waits for MEMBERS ch=%s peer=%s", si->ns, sc_public_key_hex(peer, (char[65]){0}));
}
static int db_start_peer(struct DB_SYNC_INSTANCE* si, const uint8_t peer[32]) {
    struct db_store store=db_store_for_instance(si);
    if(db_chat_store(&store)) { chat_log_peer_ready(si->db_sync->inst,si->ns,peer); return 0; }
    struct ETCP_CONN* conn = instance_find_conn_by_ed(si->db_sync->inst, peer);
    if (!merkle_sync_registered(si->db_sync->inst, MT_DATA_MESSAGES) || !db_message_can_run(si->db_sync->inst, si->ns, conn)) return 0;
    struct SI_PEER* p = si_peer_add(si, peer);
    if (!p) return -1;
    if (merkle_sync_start(si->db_sync->inst, MT_DATA_MESSAGES, conn, si->ns, NULL, NULL) < 0) {
        p->sync_state = 0; db_sync_resume_peer_check(si->db_sync); return -1;
    }
    p->sync_state = 1; db_sync_resume_peer_check(si->db_sync); return 0;
}

static void db_notice_unlink(struct db_notice* n) {
    struct db_notice** link = &n->db->notices;
    while (*link && *link != n) link = &(*link)->next;
    if (*link) *link = n->next;
}
static void db_notice_run(void* arg) {
    struct db_notice* n = arg; n->work = NULL; db_notice_unlink(n);
    struct DB_SYNC_INSTANCE* si = db_instance_find(n->db, n->group_id);
    if (si && !si->db_sync->quiescing) {
        si->holds++;
        struct db_record* records = (struct db_record*)(n + 1);
        for (unsigned i = 0; i < n->count && si->enabled; i++)
            if (si->on_insert) si->on_insert(si, records[i].message_id, records[i].timestamp, records[i].data,
                records[i].data_len, records[i].author, records[i].accepted_key, n->initial, records[i].updated, si->on_insert_arg);
        db_release(si);
    }
    u_free(n);
}
static void db_notice_abort(struct db_notice* n) {
    if (!n) return;
    uasync_call_soon_cancel(n->db->inst->ua, n->work); db_notice_unlink(n); u_free(n);
}
static int db_notice_stage(struct DB_SYNC_INSTANCE* si, struct db_record* records, int count, const uint8_t* added, struct db_notice** out) {
    size_t bodies = 0; unsigned total = 0; *out = NULL;
    for (int i = 0; i < count; i++) if (added[i]) { total++; bodies += records[i].data_len; }
    if (!total || !si->on_insert) return 0;
    struct db_notice* n = u_calloc(1, sizeof(*n) + total * sizeof(struct db_record) + bodies);
    if (!n) return db_error(si, "notification allocation");
    n->db = si->db_sync; n->group_id = si->group_id; n->initial = 1; n->count = total;
    struct db_record* saved = (struct db_record*)(n + 1); char* data = (char*)(saved + total); unsigned index = 0;
    for (int i = 0; i < count; i++) if (added[i]) {
        saved[index] = records[i]; saved[index].data = data; memcpy(data, records[i].data, records[i].data_len);
        data += records[i].data_len; index++;
    }
    n->work = uasync_call_soon(si->db_sync->inst->ua, n, db_notice_run);
    if (!n->work) { u_free(n); return db_error(si, "notification scheduling"); }
    n->next = si->db_sync->notices; si->db_sync->notices = n; *out = n; return 0;
}

/* Полная проверка входящей страницы — до BEGIN; write-lock охватывает только небольшой commit данных и индекса. */
static int db_message_apply(void* ctx, const char* ns, const uint8_t peer[32], uint64_t prefix, int av,
        const merkle_key_t* after, const merkle_key_t* next_key, int more, const uint8_t* data, size_t len) {
    struct DB_SYNC_INSTANCE* si = si_find(ctx, ns);
    if (!si || si->index.pending) return -1;
    struct db_store store = db_store_for_instance(si);
    struct db_record records[DB_BATCH_RECORDS]; uint8_t added[DB_BATCH_RECORDS] = {0};
    const uint8_t* p = data; int count = 0;
    while (p < data + len) {
        if (count == DB_BATCH_RECORDS || db_parse_record(si, &p, data + len, &records[count]) < 0) return -1;
        struct db_record* r = &records[count];
        if (merkle_sync_level_prefix(db_read64(r->message_id), MT_MAX_LEVEL) != prefix ||
            (av && memcmp(r->message_id, after->bytes, 32) <= 0)) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history page key outside requested range ns=%s", ns); return -1;
        }
        if (db_verify_incoming(&store, r) != DB_CHECK_VALID) {
            if (r->needs_members) db_request_members(si, peer);
            return -1;
        }
        if (count && memcmp(records[count - 1].message_id, r->message_id, 32) >= 0) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history incoming page not strictly ordered ns=%s", ns); return -1;
        }
        count++;
    }
    static const uint8_t empty[32];
    if ((!count && (more || memcmp(next_key->bytes, empty, 32))) ||
        (count && memcmp(next_key->bytes, records[count - 1].message_id, 32))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history page cursor does not match records ns=%s", ns); return -1;
    }
    if (!count) return 0;
    sqlite3* db = SI_DB(si);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return db_error(si, "page begin");
    uint64_t next = si->next_id; int changed = 0; struct db_notice* notice = NULL;
    for (int i = 0; i < count; i++) {
        records[i].id = next;
        int result = db_put_record(&store, &records[i], DB_CHECK_VALID, NULL, &changed);
        if (result < 0) goto rollback;
        if (!result) { added[i] = 1; if (!records[i].updated) next++; }
    }
    if (db_notice_stage(si, records, count, added, &notice) < 0) goto rollback;
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto rollback;
    si->next_id = next;
    if (changed) db_committed(si, changed);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history page committed ns=%s records=%d changed=%d", ns, count, changed);
    return 0;
rollback:
    db_notice_abort(notice); db_rollback(si, "page apply");
    if (store.rebuild) db_index_request(si);
    return -1;
}

/* Один frozen запрос модели. Тела records заимствуют request.data движка до publish/free. */
struct db_message_sql_scope {
    struct db_sync_scope history;
    uint64_t generation, next_id;
    unsigned count;
    int changed, rebuild;
    struct db_record records[DB_BATCH_RECORDS];
    uint8_t added[DB_BATCH_RECORDS];
};
static int db_message_sql_snapshot(void* ctx, const char* ns, void** out) {
    struct UTUN_INSTANCE* inst = ctx; *out = NULL;
    struct DB_SYNC_INSTANCE* si = inst ? si_find(inst, ns) : NULL;
    if (!si || si->index.pending || inst->destroying || !inst->chat_content_started || si->db_sync->quiescing) return -1;
    struct db_message_sql_scope* scope = u_calloc(1, sizeof(*scope));
    if (!scope) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history Merkle snapshot allocation failed ch=%s", ns); return -1; }
    db_sync_scope_snapshot(si, &scope->history); scope->generation = chat_core_generation(inst); scope->next_id = si->next_id;
    *out = scope; return 0;
}
static int db_message_sql_apply(struct db_store* store, struct db_message_sql_scope* scope,
    struct merkle_sync_sql_request* request) {
    const uint8_t* cursor = request->data; const uint8_t* end = cursor + request->length;
    while (cursor < end) {
        if (scope->count == DB_BATCH_RECORDS || db_parse_record_group(store->group_id, &cursor, end, &scope->records[scope->count]) < 0) {
            request->result = MT_ERR_DATA; return SQLITE_CONSTRAINT;
        }
        struct db_record* r = &scope->records[scope->count];
        if (merkle_sync_level_prefix(db_read64(r->message_id), MT_MAX_LEVEL) != request->prefix ||
            (request->after_valid && memcmp(r->message_id, request->after.bytes, 32) <= 0) ||
            (scope->count && memcmp(scope->records[scope->count - 1].message_id, r->message_id, 32) >= 0)) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history SQL page key outside cursor/range ch=%s", request->ns);
            request->result = MT_ERR_DATA; return SQLITE_CONSTRAINT;
        }
        enum db_record_check check = db_verify_incoming(store, r);
        if (check != DB_CHECK_VALID) {
            request->result = r->needs_members ? MT_ERR_CONFLICT : check == DB_CHECK_ERROR ? MT_ERR_IO : MT_ERR_DATA;
            return store->error ? store->error : check == DB_CHECK_ERROR ? SQLITE_ERROR : SQLITE_AUTH;
        }
        scope->count++;
    }
    static const uint8_t empty[32];
    if ((!scope->count && (request->more || memcmp(request->next.bytes, empty, 32))) ||
        (scope->count && memcmp(request->next.bytes, scope->records[scope->count - 1].message_id, 32))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history SQL page cursor does not match records ch=%s", request->ns);
        request->result = MT_ERR_DATA; return SQLITE_CONSTRAINT;
    }
    for (unsigned i = 0; i < scope->count; i++) {
        struct db_record* r = &scope->records[i]; r->id = scope->next_id;
        if (scope->next_id >= INT64_MAX) { request->result = MT_ERR_IO; return SQLITE_FULL; }
        int result = db_put_record(store, r, DB_CHECK_VALID, NULL, &scope->changed);
        if (result < 0) {
            request->result = result == -1 ? MT_ERR_IO : MT_ERR_DATA;
            return store->error ? store->error : result == -1 ? SQLITE_ERROR : SQLITE_CONSTRAINT;
        }
        scope->added[i] = !result;
        if (!result && !r->updated) scope->next_id++;
        int attachment = db_record_attachment(store->db, scope->history.channel, r);
        if (attachment) { request->result = MT_ERR_IO; return attachment; }
    }
    scope->rebuild = store->rebuild || (!scope->history.indexed && scope->changed);
    return SQLITE_OK;
}
static int db_message_sql_work(sqlite3* db, void* data, struct merkle_sync_sql_request* request) {
    struct db_message_sql_scope* scope = data;
    if (!db || !scope || !request || strcmp(request->ns, scope->history.channel) ||
        request->length > MT_PAGE_SIZE || request->capacity > MT_PAGE_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history Merkle SQL invalid frozen request"); return SQLITE_MISUSE;
    }
    request->result = MT_OK;
    struct db_store store = {.db=db, .group_id=scope->history.group_id, .indexed=scope->history.indexed};
    snprintf(store.table_name, sizeof(store.table_name), "%s", scope->history.table);
    memcpy(store.ns, scope->history.channel, sizeof(store.ns));
    if (request->action == MT_SQL_FINISH) return SQLITE_OK;
    if (request->action == MT_SQL_APPLY_PAGE) return db_message_sql_apply(&store, scope, request);
    if (request->action != MT_SQL_PAGE) { request->result = MT_ERR_PROTOCOL; return SQLITE_MISUSE; }
    size_t length = request->capacity;
    int rc = db_store_page(&store, request->prefix, request->after_valid, &request->after,
        request->data, &length, &request->next, &request->more);
    if (rc) request->result = MT_ERR_IO;
    else request->length = length;
    return rc;
}
static void db_message_sql_publish(void* ctx, void* data, struct merkle_sync_sql_request* request) {
    struct UTUN_INSTANCE* inst = ctx; struct db_message_sql_scope* scope = data;
    if (request->action != MT_SQL_APPLY_PAGE || !scope->changed || inst->destroying || !inst->chat_content_started ||
        chat_core_generation(inst) != scope->generation) return;
    struct DB_SYNC_INSTANCE* si = db_sync_instance_find(inst, scope->history.group_id);
    if (!si || si->db_sync->quiescing || si->generation != scope->history.instance_id) return;
    si->holds++;
    if (si->next_id < scope->next_id) si->next_id = scope->next_id;
    for (unsigned i = 0; i < scope->count; i++) {
        if (si->last_timestamp_ms < scope->records[i].timestamp) si->last_timestamp_ms = scope->records[i].timestamp;
        if (si->last_timestamp_ms < scope->records[i].revision.edited_at) si->last_timestamp_ms = scope->records[i].revision.edited_at;
    }
    if (scope->rebuild) db_index_request(si); else db_committed(si, scope->changed);
    for (unsigned i = 0; i < scope->count && si->enabled && !si->db_sync->quiescing && inst->chat_content_started; i++) {
        struct db_record* r = &scope->records[i];
        if (scope->added[i] && si->on_insert) si->on_insert(si, r->message_id, r->timestamp, r->data, r->data_len,
            r->author, r->accepted_key, 1, r->updated, si->on_insert_arg);
    }
    db_release(si);
}
static void db_message_sql_free(void* scope) { u_free(scope); }
static const struct merkle_sync_sql_ops db_message_sql_ops = {
    .snapshot=db_message_sql_snapshot, .work=db_message_sql_work, .publish=db_message_sql_publish, .free=db_message_sql_free
};
static const struct merkle_sync_data_ops db_message_ops = {
    .group_scoped = 1,
    .update_bucket_hash = db_message_hash, .get_page = db_message_page, .apply_page = db_message_apply,
    .validate_peer = db_message_peer, .can_run = db_message_can_run, .on_result = db_message_result, .sql=&db_message_sql_ops
};

int db_sync_register_model(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->db_sync || !inst->db_sync->enabled) return -1;
    if (merkle_sync_registered(inst, MT_DATA_MESSAGES)) return 0;
    if (merkle_sync_register(inst, MT_DATA_MESSAGES, &db_message_ops, inst) < 0) return -1;
    for (int i = 0; i < inst->db_sync->instance_count; i++) db_index_request(inst->db_sync->instances[i]);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history registered shared Merkle model");
    return 0;
}

static int db_send(struct DB_SYNC_INSTANCE* si, const uint8_t peer[32], uint8_t type, const uint8_t* data, size_t len) {
    struct ETCP_CONN* conn = instance_find_conn_by_ed(si->db_sync->inst, peer);
    if (!(type == CHAT_LOG_PUSH || type == CHAT_LOG_ACK ? db_sync_journal_can_run(si->db_sync->inst, si->ns, conn)
        : db_message_can_run(si->db_sync->inst, si->ns, conn))) return -1;
    if (!conn || !conn->links_up) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history transport unavailable peer=%s", sc_public_key_hex(peer, (char[65]){0})); return -1; }
    const uint8_t* key = topo_group_wire_id(topo_groups_find(conn->instance->topo_groups, si->group_id));
    if (!key || len > PKTNORM_MAX_DGRAM_SIZE - DB_SYNC_HEADER_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history send invalid group key or packet size ch=%s bytes=%zu", si->ns, len); return -1;
    }
    struct ll_entry* e = ll_alloc_lldgram((uint16_t)(DB_SYNC_HEADER_SIZE + len));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history packet allocation"); return -1; }
    e->dgram[0] = ETCP_ID_DB_SYNC; memcpy(e->dgram + DB_SYNC_SCOPE_OFF, key, 32); e->dgram[DB_SYNC_TYPE_OFF] = type;
    if (len) memcpy(e->dgram + DB_SYNC_HEADER_SIZE, data, len);
    e->len = (uint16_t)(DB_SYNC_HEADER_SIZE + len);
    int rc = etcp_send(conn, e);
    if (rc != 0) {
        queue_dgram_free(e); queue_entry_free(e);
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history send rejected peer=%s rc=%d", sc_public_key_hex(peer, (char[65]){0}), rc);
    }
    return rc;
}

int db_sync_journal_send(struct UTUN_INSTANCE* inst,const char* channel,const uint8_t peer[32],uint8_t type,const uint8_t* data,size_t len) {
    struct DB_SYNC_INSTANCE* si=si_find(inst, channel);
    if(!si || (type != CHAT_LOG_PUSH && type != CHAT_LOG_ACK) || len>PKTNORM_MAX_DGRAM_SIZE-DB_SYNC_HEADER_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC,"CHAT journal send invalid context"); return -1;
    }
    return db_send(si,peer,type,data,len);
}
void db_sync_project_committed(struct UTUN_INSTANCE* inst,const char* channel,const struct db_sync_record* r,int initial,int updated) {
    struct DB_SYNC_INSTANCE* si=si_find(inst, channel);
    if(!si || si->db_sync->quiescing) return;
    si->holds++;
    if (!memcmp(r->author_ed, inst->my_ed25519_pubkey, 32)) {
        if (si->last_timestamp_ms < r->timestamp) si->last_timestamp_ms = r->timestamp;
        if (si->last_timestamp_ms < r->revision.edited_at) si->last_timestamp_ms = r->revision.edited_at;
    }
    if(si->next_id<=r->local_id) si->next_id=r->local_id+1;
    if (si->on_insert && si->on_insert != on_msg_inserted)
        si->on_insert(si, r->message_id, r->timestamp, r->data, r->size, r->author, r->author_ed, initial, updated, si->on_insert_arg);
    db_release(si);
}

static void db_emit_push(struct DB_SYNC_INSTANCE* si, const struct db_record* r, const uint8_t source[32]) {
    si->holds++;
    if (si->on_insert)
        si->on_insert(si, r->message_id, r->timestamp, r->data, r->data_len, r->author, r->accepted_key, 0, r->updated, si->on_insert_arg);
    if (si->enabled) {
        uint8_t wire[DB_WIRE_MAX]; size_t bytes = (size_t)db_encode_record(r, wire);
        for (int i = 0; i < si->peer_count; i++)
            if ((!source || memcmp(si->peers[i].ed, source, 32)) && si->peers[i].sync_state && !si->peers[i].paused &&
                !db_sync_peer_throttled(si->db_sync, &si->peers[i]))
                db_send(si, si->peers[i].ed, DB_MSG_PUSH, wire, bytes);
    }
    db_release(si);
}
void db_sync_record_committed(struct DB_SYNC_INSTANCE* si, const struct db_sync_record* record, const uint8_t source[32]) {
    if (!si || !si->enabled || si->db_sync->quiescing || !record) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history commit publication skipped: no live history"); return;
    }
    si->holds++;
    if (record->rebuild) db_index_request(si);
    else if (record->changed) db_committed(si, record->changed);
    if (!si->enabled || record->result != 0) { db_release(si); return; }
    struct db_record r = {.timestamp=record->timestamp, .author=record->author, .id=record->local_id,
        .revision=record->revision, .data=record->data, .data_len=record->size, .sig_len=64, .updated=record->updated};
    memcpy(r.signature, record->signature, 64); memcpy(r.message_id, record->message_id, 32);
    memcpy(r.accepted_key, record->author_ed, 32);
    if (si->last_timestamp_ms < r.timestamp) si->last_timestamp_ms = r.timestamp;
    if (si->last_timestamp_ms < r.revision.edited_at) si->last_timestamp_ms = r.revision.edited_at;
    db_emit_push(si, &r, source);
    db_release(si);
}
static int db_insert_one_impl(struct DB_SYNC_INSTANCE* si, struct db_record* r, const char* attrs, int local) {
    struct db_store store = db_store_for_instance(si);
    if (db_sync_message_id(r->timestamp, r->accepted_key, &r->revision, r->message_id) < 0) return -1;
    sqlite3* db = SI_DB(si);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return db_error(si, "insert begin");
    if (db_chat_store(&store)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC,"bare CHAT insertion rejected ch=%s; use message command or member block",si->ns);
        db_rollback(si,"CHAT journal required"); return -1;
    }
    enum db_record_check incoming = db_verify_incoming(&store, r);
    if (incoming == DB_CHECK_ERROR || incoming == DB_CHECK_DEFERRED) { db_rollback(si, "journal witness"); return -1; }
    int changed = 0; r->id = si->next_id;
    int result = db_put_record(&store, r, incoming, attrs, &changed);
    if (store.rebuild) db_index_request(si);
    if (result == -1 || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) { db_rollback(si, "insert"); return -1; }
    if (!result && !r->updated) si->next_id++;
    if (changed) db_committed(si, changed);
    return result;
}
static int db_insert_one(struct DB_SYNC_INSTANCE* si, struct db_record* r, const char* attrs) {
    return db_insert_one_impl(si, r, attrs, 0);
}
/* ACK конкретного сообщения: повтор не увеличивает счётчик, очередь отправки не считается доставкой. */
static int db_delivery_write(struct db_store* store, const uint8_t self[32], const uint8_t message_id[32], uint64_t ver, const uint8_t peer_id[32]) {
    if (!peer_id) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history ACK missing peer Ed25519"); return SQLITE_MISUSE; }
    char hex[65]; sc_public_key_hex(peer_id, hex);
    sqlite3_stmt* stmt = NULL;
    int rc = db_store_prepare(store, &stmt,
        "UPDATE \"%s\" SET flags=flags | CASE WHEN accepted_key=?3 THEN 1 ELSE 0 END,"
        " delivered_peers=delivered_peers + (instr(',' || delivery_chain || ',', ',' || ?1 || ',')=0),"
        " delivery_chain=CASE WHEN instr(',' || delivery_chain || ',', ',' || ?1 || ',')>0 THEN delivery_chain"
        " WHEN delivery_chain='' THEN ?1 ELSE delivery_chain || ',' || ?1 END WHERE message_id=?2 AND ver=?4");
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, hex, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(stmt, 2, message_id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(stmt, 3, self, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, (sqlite3_int64)ver);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(store->db), final_rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || final_rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history ACK update tbl=%s peer=%s rc=%d finalize=%d: %s",
            store->table_name, hex, rc, final_rc, sqlite3_errmsg(store->db)); return rc != SQLITE_DONE ? rc : final_rc;
    }
    if (!changed) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history ACK stale/unknown tbl=%s peer=%s ver=%llu",
        store->table_name, hex, (unsigned long long)ver);
    else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history ACK recorded tbl=%s peer=%s", store->table_name, hex);
    return SQLITE_OK;
}
int db_sync_journal_ack_write(sqlite3* db,const char* channel,const uint8_t self[32],const uint8_t message[32],uint64_t ver,const uint8_t peer[32]) {
    struct db_store store={.db=db};
    snprintf(store.table_name,sizeof(store.table_name),"msg_%s",channel); snprintf(store.ns,sizeof(store.ns),"%s",channel);
    return db_delivery_write(&store,self,message,ver,peer);
}
static int si_delivery_update(struct DB_SYNC_INSTANCE* si, const uint8_t message_id[32], uint64_t ver, const uint8_t peer_id[32]) {
    struct db_store store = db_store_for_instance(si);
    return db_delivery_write(&store, si->db_sync->inst->my_ed25519_pubkey, message_id, ver, peer_id) == SQLITE_OK ? 0 : -1;
}

struct db_network_job {
    struct db_sync_scope scope;
    struct db_sync_record record;
    uint64_t generation;
    uint64_t peer_node;
    uint8_t self[32], peer[32];
    size_t length;
    uint8_t type, wire[DB_WIRE_MAX];
    struct UTUN_INSTANCE* inst; /* Только guard/done, SQL использует frozen поля выше. */
};
static int db_network_guard(void* data) {
    struct db_network_job* job = data; struct DB_SYNC_INSTANCE* si = db_task_current(job->inst, job->generation, &job->scope);
    struct ETCP_CONN* conn = instance_find_conn_by_ed(job->inst, job->peer);
    if (!si || !conn || !db_message_peer(job->inst, si->ns, conn)) return 0;
    db_sync_scope_snapshot(si, &job->scope);
    if (job->type == DB_MSG_PUSH) {
        if (si->next_id >= INT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history incoming local id exhausted ch=%s", si->ns); return 0; }
        job->record.local_id = si->next_id++;
    }
    return 1;
}
static int db_network_apply(sqlite3* db, struct db_network_job* job) {
    if (job->type == DB_MSG_ACK_PUSH) {
        struct db_store store = {.db=db, .group_id=job->scope.group_id};
        snprintf(store.table_name, sizeof(store.table_name), "%s", job->scope.table);
        return db_delivery_write(&store, job->self, job->wire, db_read64(job->wire + 32), job->peer);
    }
    struct db_record decoded; const uint8_t* cursor = job->wire;
    if (db_parse_record_group(job->scope.group_id, &cursor, job->wire + job->length, &decoded) < 0 || cursor != job->wire + job->length) {
        db_changes_warn(db, NULL, job->scope.channel, "Chat message", "invalid envelope or message ID", NULL, NULL, NULL);
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history incoming PUSH malformed group=%llu bytes=%zu",
            (unsigned long long)job->scope.group_id, job->length); return SQLITE_CONSTRAINT;
    }
    struct db_sync_record* record = &job->record;
    record->timestamp = decoded.timestamp; record->author = decoded.author; record->revision = decoded.revision;
    memcpy(record->author_ed, decoded.accepted_key, 32);
    record->data = decoded.data; record->size = decoded.data_len; memcpy(record->signature, decoded.signature, 64);
    return db_sync_record_write(db, &job->scope, record);
}
static int db_network_work(sqlite3* db, void* data) {
    struct db_network_job* job = data;
    struct db_changes_origin source = db_changes_source(db, job->peer, job->peer_node,
        job->type == DB_MSG_ACK_PUSH ? "history delivery ACK" : "history push");
    int rc = db_network_apply(db, job);
    db_changes_restore(db, source);
    return rc;
}
static void db_network_done(void* data, const struct db_async_result* result) {
    struct db_network_job* job = data; struct DB_SYNC_INSTANCE* si = db_task_current(job->inst, job->generation, &job->scope);
    if (result->status) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history network WRITE failed group=%llu peer=%s type=%u status=%d rc=%d: %s",
            (unsigned long long)job->scope.group_id, sc_public_key_hex(job->peer, (char[65]){0}), job->type, result->status, result->sqlite_code, result->message);
        if (si && job->record.needs_members) db_request_members(si, job->peer);
        return;
    }
    if (!si || job->type != DB_MSG_PUSH) return;
    si->holds++;
    db_sync_record_committed(si, &job->record, job->peer);
    if (si->enabled && !si->db_sync->quiescing && job->record.result >= 0 && db_message_peer(job->inst, si->ns, instance_find_conn_by_ed(job->inst, job->peer))) {
        uint8_t acknowledgement[40]; memcpy(acknowledgement, job->record.message_id, 32);
        db_write64(acknowledgement + 32, job->record.revision.ver);
        db_send(si, job->peer, DB_MSG_ACK_PUSH, acknowledgement, sizeof(acknowledgement));
    } else if (job->record.result < 0) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history incoming PUSH rejected group=%llu result=%d",
        (unsigned long long)job->scope.group_id, job->record.result);
    db_release(si);
}
static void db_network_cleanup(void* data) { u_free(data); }
static void db_network_submit(struct DB_SYNC_INSTANCE* si, struct ETCP_CONN* conn, uint8_t type, const uint8_t* data, size_t length) {
    if ((type == DB_MSG_ACK_PUSH && (length != 40 || !db_read64(data + 32))) ||
        (type == DB_MSG_PUSH && (length < DB_WIRE_FIXED || length > DB_WIRE_MAX))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history network packet invalid type=%u bytes=%zu", type, length); return;
    }
    struct db_network_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history network WRITE allocation failed ch=%s type=%u", si->ns, type); return; }
    job->inst = si->db_sync->inst; job->generation = chat_core_generation(job->inst);
    memcpy(job->self, job->inst->my_ed25519_pubkey, 32);
    memcpy(job->peer, conn->peer_ed25519_pubkey, 32); job->type = type; job->length = length; db_sync_scope_snapshot(si, &job->scope);
    job->peer_node = conn->peer_node_id;
    memcpy(job->wire, data, length);
    struct db_async_task task = {.name=type == DB_MSG_PUSH ? "history-push" : "history-ack", .mode=DB_ASYNC_WRITE,
        .data=job, .guard=db_network_guard, .work=db_network_work, .done=db_network_done, .cleanup=db_network_cleanup};
    db_async_run(job->inst->db_worker, &task);
}

static void db_sync_recv_cb(struct ETCP_CONN* conn, struct ll_entry* e) {
    if (!e) return;
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    if (!inst || !e->dgram || e->len < DB_SYNC_HEADER_SIZE || e->len > PKTNORM_MAX_DGRAM_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history invalid frame bytes=%u", e->len); goto done;
    }
    const uint8_t* key = e->dgram + DB_SYNC_SCOPE_OFF;
    struct TOPO_GROUP* group = topo_groups_find_wire(inst->topo_groups, key);
    struct DB_SYNC_INSTANCE* si = group && group->group_type == TOPO_GROUP_TYPE_CHAT ? db_sync_instance_find(inst, group->group_id) : NULL;
    uint8_t type = e->dgram[DB_SYNC_TYPE_OFF];
    const uint8_t* data = e->dgram + DB_SYNC_HEADER_SIZE; size_t len = e->len - DB_SYNC_HEADER_SIZE;
    if (si && (type == CHAT_LOG_PUSH || type == CHAT_LOG_ACK)) { chat_log_receive(conn, si->ns, type, data, len); goto done; }
    if (!si || !db_message_peer(inst, si->ns, conn)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history receive denied key=%s peer=%s type=%u bytes=%u: unknown group or no current admission",
            sc_public_key_hex(key, (char[65]){0}), sc_public_key_hex(conn->peer_ed25519_pubkey, (char[65]){0}), type, e->len);
        goto done;
    }
    if (db_async_can_write(inst->db_worker) && (type == DB_MSG_ACK_PUSH || type == DB_MSG_PUSH)) {
        db_network_submit(si, conn, type, data, len); goto done;
    }
    si->holds++;
    if (type == DB_MSG_ACK_PUSH) {
        if (len == 40) {
            sqlite3* db = SI_DB(si);
            struct db_changes_origin source = db_changes_source(db, conn->peer_ed25519_pubkey, conn->peer_node_id, "history delivery ACK");
            si_delivery_update(si, data, db_read64(data + 32), conn->peer_ed25519_pubkey);
            db_changes_restore(db, source);
        }
        else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history invalid ACK bytes=%zu", len);
    } else if (type == DB_MSG_PUSH) {
        const uint8_t* p = data; struct db_record r;
        if (db_parse_record(si, &p, data + len, &r) == 0 && p == data + len) {
            sqlite3* db = SI_DB(si);
            struct db_changes_origin source = db_changes_source(db, conn->peer_ed25519_pubkey, conn->peer_node_id, "history push");
            int result = db_insert_one(si, &r, NULL);
            db_changes_restore(db, source);
            if (result < 0 && r.needs_members) db_request_members(si, conn->peer_ed25519_pubkey);
            if (!result) db_emit_push(si, &r, conn->peer_ed25519_pubkey);
            if (result >= 0 && si->enabled) {
                uint8_t ack[40]; memcpy(ack, r.message_id, 32); db_write64(ack + 32, r.revision.ver);
                db_send(si, conn->peer_ed25519_pubkey, DB_MSG_ACK_PUSH, ack, sizeof(ack));
            }
        }
    } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history unknown message type=%u", type);
    db_release(si);
done:
    queue_dgram_free(e); queue_entry_free(e);
}

static uint64_t db_now_ms(struct DB_SYNC_INSTANCE* si) {
    int64_t us;
    if (si->db_sync->inst->ntp.synced) us = ntp_time_get_us(si->db_sync->inst);
    else { struct timeval tv; utun_gettimeofday(&tv, NULL); us = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec; }
    if (us < 0) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history wall clock before epoch"); return 0; }
    return (uint64_t)us / 1000;
}
static int db_ttl_write(sqlite3* db, const struct db_sync_scope* scope, const uint8_t self[32], uint64_t cutoff, int indexed, int* removed) {
    if (sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history TTL requires caller WRITE"); return SQLITE_MISUSE; }
    char sql[512]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "DELETE FROM \"%s\" WHERE rowid IN (SELECT rowid FROM \"%s\""
        " WHERE accepted_key=? AND (flags & 1)=0 AND CASE WHEN edited_at>0 THEN edited_at ELSE timestamp END<? LIMIT 16)", scope->table, scope->table);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, self, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)cutoff);
    struct db_changes_origin source = db_changes_source(db, NULL, 0, "history TTL");
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    db_changes_restore(db, source);
    *removed = rc == SQLITE_DONE ? sqlite3_changes(db) : 0;
    int final = sqlite3_finalize(st); rc = rc == SQLITE_DONE ? final : rc;
    if (!rc && *removed && indexed) {
        const char* channel = scope->channel;
        int clear = db_index_clear(db, channel); if (clear < 0) rc = -clear;
    }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history TTL WRITE failed tbl=%s rc=%d: %s", scope->table, rc, sqlite3_errstr(rc));
    return rc;
}
static void db_ttl_tick(void* arg);
static void db_ttl_schedule(struct DB_SYNC_INSTANCE* si, uint64_t delay) {
    if (!si->enabled || si->db_sync->quiescing || si->ttl_timer) return;
    si->ttl_timer = uasync_set_timeout(si->db_sync->inst->ua, delay, si, db_ttl_tick, "history_ttl");
    if (!si->ttl_timer) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history TTL timer allocation failed tbl=%s", si->table_name);
}
struct db_ttl_job {
    struct db_sync_scope scope;
    uint64_t generation, cutoff;
    uint8_t self[32];
    int indexed, removed;
    struct UTUN_INSTANCE* inst; /* Только guard/done. */
};
static int db_ttl_guard(void* data) {
    struct db_ttl_job* job = data; struct DB_SYNC_INSTANCE* si = db_task_current(job->inst, job->generation, &job->scope);
    if (!si) return 0;
    db_sync_scope_snapshot(si, &job->scope); job->indexed = merkle_sync_registered(job->inst, MT_DATA_MESSAGES);
    uint64_t now = db_now_ms(si), ttl = (uint64_t)job->inst->config->global.db_sync_ttl * 1000;
    job->cutoff = now > ttl ? now - ttl : 0; return 1;
}
static int db_ttl_work(sqlite3* db, void* data) {
    struct db_ttl_job* job = data; return db_ttl_write(db, &job->scope, job->self, job->cutoff, job->indexed, &job->removed);
}
static void db_ttl_done(void* data, const struct db_async_result* result) {
    struct db_ttl_job* job = data; struct DB_SYNC_INSTANCE* si = db_task_current(job->inst, job->generation, &job->scope);
    if (!si) return;
    si->holds++;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history TTL failed tbl=%s status=%d rc=%d: %s",
        job->scope.table, result->status, result->sqlite_code, result->message);
    else if (job->removed) {
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history TTL committed tbl=%s removed=%d cutoff_ms=%llu",
            job->scope.table, job->removed, (unsigned long long)job->cutoff);
        db_index_request(si);
    }
    if (db_task_current(job->inst, job->generation, &job->scope) == si)
        db_ttl_schedule(si, !result->status && job->removed == DB_BATCH_RECORDS ? 1 : DB_SYNC_TTL_INTERVAL * 10000ULL);
    db_release(si);
}
static void db_ttl_cleanup(void* data) { u_free(data); }
static void db_ttl_tick(void* arg) {
    struct DB_SYNC_INSTANCE* si = arg; si->ttl_timer = NULL;
    if (!si->enabled || si->db_sync->quiescing) return;
    if (db_async_can_write(si->db_sync->inst->db_worker)) {
        struct db_ttl_job* job = u_calloc(1, sizeof(*job));
        if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history TTL request allocation failed tbl=%s", si->table_name);
            db_ttl_schedule(si, 50000); return; }
        job->inst = si->db_sync->inst; job->generation = chat_core_generation(job->inst);
        memcpy(job->self, job->inst->my_ed25519_pubkey, 32);
        db_sync_scope_snapshot(si, &job->scope);
        struct db_async_task task = {.name="history-ttl", .mode=DB_ASYNC_WRITE, .data=job, .guard=db_ttl_guard,
            .work=db_ttl_work, .done=db_ttl_done, .cleanup=db_ttl_cleanup};
        db_async_run(job->inst->db_worker, &task); return;
    }
    sqlite3* db = SI_DB(si);
    uint64_t now = db_now_ms(si);
    uint64_t ttl = (uint64_t)si->db_sync->inst->config->global.db_sync_ttl * 1000;
    uint64_t cut = now > ttl ? now - ttl : 0;
    int removed = 0, committed = 0;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) { db_error(si, "TTL begin"); goto schedule; }
    struct db_sync_scope scope; db_sync_scope_snapshot(si, &scope);
    if (db_ttl_write(db, &scope, si->db_sync->inst->my_ed25519_pubkey, cut,
        merkle_sync_registered(si->db_sync->inst, MT_DATA_MESSAGES), &removed)) { db_rollback(si, "TTL delete/index"); goto schedule; }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) { db_rollback(si, "TTL commit"); goto schedule; }
    committed = 1;
    if (removed) {
        db_index_request(si);
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history TTL committed tbl=%s removed=%d cutoff_ms=%llu",
            SI_TBL(si), removed, (unsigned long long)cut);
    }
schedule:
    uint64_t delay = committed && removed == DB_BATCH_RECORDS ? 1 : DB_SYNC_TTL_INTERVAL * 10000ULL;
    db_ttl_schedule(si, delay);
}

static void db_sync_resume_peer_check(struct DB_SYNC* db) {
    if (!db || !db->enabled || db->quiescing || db->peer_check_timer) return;
#ifdef UTUN_HAVE_STANDBY
    if (db->peer_check_wait) return;
#endif
    db->peer_check_timer = uasync_set_timeout(db->inst->ua, DB_SYNC_PEER_CHECK_INTERVAL * 10000ULL, db, db_sync_peer_check_cb, "history_retry");
    if (!db->peer_check_timer) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history retry timer allocation");
}
#ifdef UTUN_HAVE_STANDBY
/* standby_wait может вызвать callback синхронно при отказе памяти: освобождаем handle и ставим таймер без рекурсии. */
static void db_sync_peer_check_wake(void* arg) {
    struct DB_SYNC* db = arg; db->peer_check_wait = NULL;
    db_sync_resume_peer_check(db);
}
#endif
static void db_sync_peer_check_cb(void* arg) {
    struct DB_SYNC* db = arg; db->peer_check_timer = NULL; int pending = 0;
    if (!db->enabled || db->quiescing) return;
#ifdef UTUN_HAVE_STANDBY
    db->peer_check_wait = NULL;
    if (standby_get_sleep_tb() > 0) {
        db->peer_check_wait = standby_wait(db, db_sync_peer_check_wake);
        if (!db->peer_check_wait) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history standby wait allocation; retry later");
            db_sync_resume_peer_check(db);
        }
        return;
    }
#endif
    for (int i = 0; i < db->instance_count; i++) {
        struct DB_SYNC_INSTANCE* si = db->instances[i];
        if (si->index.pending) {
            pending = 1;
            if (!si->index.timer && !si->index.running) db_index_schedule(si, 0);
        }
        for (int j = 0; j < si->peer_count; j++) {
            struct SI_PEER* p = &si->peers[j];
            if (p->needs_members) { pending = 1; db_request_members(si, p->ed); continue; }
            if (p->sync_state != 2 && db_message_can_run(db->inst, si->ns, instance_find_conn_by_ed(db->inst, p->ed))) {
                pending = 1;
                if (!p->sync_state) db_start_peer(si, p->ed);
            }
        }
    }
    if (pending) db_sync_resume_peer_check(db);
}
static void db_sync_on_peer_ready(struct TOPO_GROUP* group, const uint8_t peer[32], int ready, void* arg) {
    (void)group; struct DB_SYNC_INSTANCE* si = arg;
    if (!si->enabled || si->db_sync->quiescing) return;
    struct SI_PEER* p = ready ? si_peer_add(si, peer) : si_peer_find(si, peer);
    if (!p) return;
    if (!ready) {
        p->sync_state = 0;
        if (p->needs_members == 2) p->needs_members = 1;
        merkle_sync_cancel(si->db_sync->inst, MT_DATA_MESSAGES, peer, si->ns);
    } else db_start_peer(si, peer);
}

int db_sync_init(struct UTUN_INSTANCE* inst) {
    if (!inst || inst->db_sync) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history invalid init"); return -1; }
    struct DB_SYNC* db = u_calloc(1, sizeof(*db));
    if (!db) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history allocation"); return -1; }
    db->inst = inst; db->db = inst->topo_sqlite_db; inst->db_sync = db;
    return inst->config->global.db_sync_enabled ? db_sync_enable(inst) : 0;
}
int db_sync_enable(struct UTUN_INSTANCE* inst) {
    struct DB_SYNC* db = inst ? inst->db_sync : NULL;
    if (!db || (!inst->topo_sqlite_db && !db_async_can_write(inst->db_worker))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history requires core SQLite owner"); return -1;
    }
    db->quiescing = 0;
    if (db->enabled) return 0;
    db->db = inst->topo_sqlite_db; db->enabled = 1;
    etcp_bind(inst, ETCP_ID_DB_SYNC, db_sync_recv_cb);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history enabled on shared core SQLite");
    return 0;
}
int db_sync_instance_prepare(sqlite3* db, const char* name, const char* channel, struct db_sync_bootstrap* out) {
    if (!db || !si_name_valid(name) || !chat_channel_valid(channel) || !out) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL bootstrap invalid arguments"); return SQLITE_MISUSE;
    }
    int type = group_protocol_type(db, channel);
    if (type != 1) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history SQL bootstrap requires CHAT ch=%s type=%d", channel, type);
        return type < 0 ? SQLITE_ERROR : SQLITE_MISMATCH;
    }
    if (db_schema_history(db, name) < 0) return sqlite3_errcode(db) == SQLITE_OK ? SQLITE_ERROR : sqlite3_extended_errcode(db);
    struct db_sync_bootstrap result = {0}; strcpy(result.channel, channel); snprintf(result.table, sizeof(result.table), "%s", name);
    char sql[256]; sqlite3_stmt* st = NULL;
    /* CHAT восстанавливает время автора из его цепочки в транзакции записи сообщения. */
    const char* clock = db_chat_table(name, channel) ? "0" : "COALESCE(MAX(max(timestamp,edited_at)),0)";
    snprintf(sql, sizeof(sql), "SELECT COALESCE(MAX(id),0),%s FROM \"history_%s\"", clock, name);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(st, 0), timestamp = sqlite3_column_int64(st, 1);
        if (id < 0 || id == INT64_MAX || timestamp < 0 || timestamp == INT64_MAX) rc = SQLITE_TOOBIG;
        else { result.next_id = (uint64_t)id + 1; result.last_timestamp = (uint64_t)timestamp; rc = SQLITE_OK; }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (rc == SQLITE_OK) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history SQL bootstrap failed ch=%s rc=%d: %s", channel, rc, sqlite3_errstr(rc));
    else *out = result;
    return rc;
}
struct DB_SYNC_INSTANCE* db_sync_instance_activate(struct UTUN_INSTANCE* inst, const struct db_sync_bootstrap* prepared) {
    struct DB_SYNC* db = inst ? inst->db_sync : NULL;
    uint64_t group_id = db && prepared ? topo_groups_channel_slot(inst->topo_groups, prepared->channel) : 0;
    if (!db || !db->enabled || !prepared || !memchr(prepared->table, 0, sizeof(prepared->table)) ||
        !si_name_valid(prepared->table) || !prepared->next_id || !group_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history activation requires committed snapshot/topology"); return NULL;
    }
    struct DB_SYNC_INSTANCE* si = db_instance_find(db, group_id);
    if (si) return si;
    if (db->instance_count == db->instance_capacity) {
        int cap = db->instance_capacity ? db->instance_capacity * 2 : 4;
        void* entries = u_realloc(db->instances, (size_t)cap * sizeof(*db->instances));
        if (!entries) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history registry allocation"); return NULL; }
        db->instances = entries; db->instance_capacity = cap;
    }
    si = u_calloc(1, sizeof(*si));
    if (!si) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history table allocation"); return NULL; }
    if (db->generation == UINT64_MAX) { u_free(si); DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history generation exhausted"); return NULL; }
    si->db_sync = db; si->group_id = group_id; si->generation = ++db->generation; si->enabled = 1; si->sync_gated = 1;
    si->next_id = prepared->next_id; si->last_timestamp_ms = prepared->last_timestamp;
    snprintf(si->table_name, sizeof(si->table_name), "%s", prepared->table);
    strcpy(si->ns, prepared->channel);
    db->instances[db->instance_count++] = si;
    if (topo_group_add_peer_ready_cbk(topo_groups_find(inst->topo_groups, si->group_id), db_sync_on_peer_ready, si) < 0) goto error;
    db_index_request(si);
    struct db_store ttl_store=db_store_for_instance(si);
    if(!db_chat_store(&ttl_store)) si->ttl_timer = uasync_set_timeout(inst->ua, DB_SYNC_TTL_INTERVAL * 10000ULL, si, db_ttl_tick, "history_ttl");
    if (!db_chat_store(&ttl_store) && !si->ttl_timer) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history initial TTL timer allocation failed"); goto error; }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history activated tbl=%s group=%s generation=%llu; preflight pending",
        si->table_name, si->ns, (unsigned long long)si->generation);
    return si;
error:
    db_sync_instance_remove(si); return NULL;
}
struct DB_SYNC_INSTANCE* db_sync_instance_add(struct UTUN_INSTANCE* inst, const char* name, uint64_t group_id) {
    struct DB_SYNC* db = inst ? inst->db_sync : NULL; struct db_sync_bootstrap prepared;
    if (!db || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history legacy registration requires synchronous owner"); return NULL;
    }
    if (db_sync_instance_prepare(db->db, name, topo_groups_channel(inst->topo_groups, group_id), &prepared) != SQLITE_OK) return NULL;
    return db_sync_instance_activate(inst, &prepared);
}
void db_sync_instance_remove(struct DB_SYNC_INSTANCE* si) {
    if (!si || !si->enabled) return;
    si->enabled = 0; struct DB_SYNC* db = si->db_sync;
    topo_group_remove_peer_ready_cbk(topo_groups_find(db->inst->topo_groups, si->group_id), db_sync_on_peer_ready, si);
    merkle_sync_cancel_dataset_ns(db->inst, MT_DATA_MESSAGES, si->ns);
    if (si->index.timer) uasync_cancel_timeout(db->inst->ua, si->index.timer);
    if (si->ttl_timer) uasync_cancel_timeout(db->inst->ua, si->ttl_timer);
    for (struct db_notice* n = db->notices; n;) {
        struct db_notice* next = n->next; if (n->group_id == si->group_id) db_notice_abort(n); n = next;
    }
    for (int i = 0; i < db->instance_count; i++) if (db->instances[i] == si) {
        memmove(db->instances + i, db->instances + i + 1, (size_t)(db->instance_count - i - 1) * sizeof(*db->instances));
        db->instance_count--; break;
    }
    u_free(si->peers); si->peers = NULL; si->peer_count = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history table detached tbl=%s; signed records preserved", SI_TBL(si));
    if (!si->holds) u_free(si);
}
void db_sync_destroy(struct UTUN_INSTANCE* inst) {
    struct DB_SYNC* db = inst ? inst->db_sync : NULL;
    if (!db) return;
    db->enabled = 0;
    if (db->peer_check_timer) uasync_cancel_timeout(inst->ua, db->peer_check_timer);
#ifdef UTUN_HAVE_STANDBY
    if (db->peer_check_wait) standby_wait_cancel(db->peer_check_wait);
#endif
    while (db->instance_count) db_sync_instance_remove(db->instances[db->instance_count - 1]);
    while (db->notices) db_notice_abort(db->notices);
    while (db->done_cbks) {
        struct db_sync_done_cbk_entry* e = db->done_cbks; db->done_cbks = e->next; u_free(e);
    }
    etcp_unbind(inst, ETCP_ID_DB_SYNC);
    inst->db_sync = NULL; u_free(db->instances); u_free(db);
}
void db_sync_quiesce(struct UTUN_INSTANCE* inst) {
    struct DB_SYNC* db = inst ? inst->db_sync : NULL;
    if (!db || db->quiescing) return;
    db->quiescing = 1;
    if (db->peer_check_timer) { uasync_cancel_timeout(inst->ua, db->peer_check_timer); db->peer_check_timer = NULL; }
#ifdef UTUN_HAVE_STANDBY
    if (db->peer_check_wait) { standby_wait_cancel(db->peer_check_wait); db->peer_check_wait = NULL; }
#endif
    for (int i = 0; i < db->instance_count; i++) {
        struct DB_SYNC_INSTANCE* si = db->instances[i];
        if (si->index.timer) { uasync_cancel_timeout(inst->ua, si->index.timer); si->index.timer = NULL; }
        if (si->ttl_timer) { uasync_cancel_timeout(inst->ua, si->ttl_timer); si->ttl_timer = NULL; }
    }
    while (db->notices) db_notice_abort(db->notices);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history quiesced tables=%d; active SQL will finish without publication", db->instance_count);
}

int db_sync_insert_signed(struct DB_SYNC_INSTANCE* si, const char* data, size_t len, const uint8_t* sig, size_t sig_len,
        uint64_t timestamp, const char* attrs, const struct db_message_revision* revision) {
    if (!si || !si->enabled || !sig || sig_len != 64 || !revision) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "history invalid insertion"); return -1;
    }
    struct db_record r = {.timestamp = timestamp, .author = si->db_sync->inst->node_id, .data = data, .data_len = len, .sig_len = sig_len};
    memcpy(r.accepted_key, si->db_sync->inst->my_ed25519_pubkey, 32);
    r.revision = *revision;
    memcpy(r.signature, sig, 64);
    int result = db_insert_one_impl(si, &r, attrs, 1);
    struct db_store store=db_store_for_instance(si);
    if (!result && !db_chat_store(&store)) db_emit_push(si, &r, NULL);
    return result;
}
uint32_t db_sync_count(struct DB_SYNC_INSTANCE* si) {
    if (!si || !si->enabled) return 0;
    sqlite3_stmt* st = NULL; int rc = si_prep(si, &st, "SELECT COUNT(*) FROM \"%s\"");
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    uint32_t count = rc == SQLITE_ROW ? (uint32_t)sqlite3_column_int64(st, 0) : 0;
    if (rc != SQLITE_ROW) db_error(si, "count"); sqlite3_finalize(st); return count;
}
uint64_t db_sync_get_last_timestamp(struct DB_SYNC_INSTANCE* si) { return si ? si->last_timestamp_ms : 0; }
uint64_t db_sync_next_timestamp(struct DB_SYNC_INSTANCE* si) {
    if (!si) return 0; uint64_t now = db_now_ms(si);
    if (now <= si->last_timestamp_ms) now = si->last_timestamp_ms + 1;
    return si->last_timestamp_ms = now;
}
void db_sync_set_insert_cb(struct DB_SYNC_INSTANCE* si, db_sync_insert_cb fn, void* arg) {
    if (si) { si->on_insert = fn; si->on_insert_arg = arg; }
}
int db_sync_select(struct DB_SYNC_INSTANCE* si, uint32_t offset, uint32_t limit, db_sync_select_cb cb, void* arg) {
    if (!si || !si->enabled || !cb) return -1;
    sqlite3_stmt* st = NULL; si->holds++;
    int rc = si_prep(si, &st, "SELECT id,timestamp,accepted_key,data,author_signature,delivered_peers,delivery_chain"
        " FROM \"history_%s\" ORDER BY timestamp,message_id LIMIT ? OFFSET ?");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, limit ? limit : -1LL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, offset);
    int count = 0;
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW && si->enabled) {
        const char* data = sqlite3_column_blob(st, 3); int bytes = sqlite3_column_bytes(st, 3);
        const uint8_t* signature = sqlite3_column_blob(st, 4); int sig_len = sqlite3_column_bytes(st, 4);
        const char* chain = (const char*)sqlite3_column_text(st, 6);
        const uint8_t* author = sqlite3_column_blob(st, 2);
        if (sqlite3_errcode(SI_DB(si)) == SQLITE_NOMEM) { rc = SQLITE_NOMEM; break; }
        if (!author || sqlite3_column_bytes(st, 2) != 32) { rc = SQLITE_CORRUPT; break; }
        cb(arg, (uint64_t)sqlite3_column_int64(st, 0), (uint64_t)sqlite3_column_int64(st, 1), data, (size_t)bytes,
            author, signature, (size_t)sig_len, sqlite3_column_int(st, 5), chain ? chain : "");
        count++;
    }
    int end_rc = sqlite3_finalize(st);
    if ((rc != SQLITE_DONE && si->enabled) || end_rc != SQLITE_OK) { db_error(si, "select"); count = -1; }
    db_release(si); return count;
}

/* Диагностический синхронный обход: в рабочем обмене preflight идёт порциями по таймеру. */
int db_sync_verify(struct DB_SYNC_INSTANCE* si) {
    if (!si || !si->enabled || !merkle_sync_registered(si->db_sync->inst, MT_DATA_MESSAGES)) return -1;
    if (!si->index.pending) db_index_request(si);
    while (si->index.pending) if (db_index_batch(si) != 0) return si->index.waiting_key ? 1 : -1;
    if (si->index.timer) { uasync_cancel_timeout(si->db_sync->inst->ua, si->index.timer); si->index.timer = NULL; }
    uint8_t root[32]; return merkle_sync_read_hash(si->db_sync->inst, MT_DATA_MESSAGES, si->ns, 0, 0, root);
}
int db_sync_read_root(struct DB_SYNC_INSTANCE* si, uint8_t root[32]) {
    if (!si || !si->enabled || !root) return -1;
    if (si->index.pending) return 1;
    return merkle_sync_read_hash(si->db_sync->inst, MT_DATA_MESSAGES, si->ns, 0, 0, root);
}
void db_sync_peer_set_paused(struct DB_SYNC_INSTANCE* si, const uint8_t peer[32], int paused) {
    struct SI_PEER* p = si ? si_peer_add(si, peer) : NULL;
    if (!p) return;
    p->paused = !!paused; p->sync_state = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history peer pause tbl=%s peer=%s paused=%u",
        SI_TBL(si), sc_public_key_hex(peer, (char[65]){0}), p->paused);
    struct db_store store=db_store_for_instance(si);
    if (paused) merkle_sync_cancel(si->db_sync->inst,db_chat_store(&store)?MT_DATA_CHAT_LOG:MT_DATA_MESSAGES,peer,si->ns);
    else db_start_peer(si, peer);
}
void db_sync_reinitiate(struct DB_SYNC_INSTANCE* si, const uint8_t peer[32]) {
    if (!si) return;
    struct SI_PEER* p = si_peer_add(si, peer); if (!p) return;
    p->paused = 0; p->sync_state = 0; db_start_peer(si, peer);
}
void db_sync_instance_set_gated(struct DB_SYNC_INSTANCE* si, int gated) {
    if (!si || !si->enabled) return;
    si->sync_gated = !!gated;
    struct db_store store=db_store_for_instance(si);
    if (gated) merkle_sync_cancel_dataset_ns(si->db_sync->inst,db_chat_store(&store)?MT_DATA_CHAT_LOG:MT_DATA_MESSAGES,si->ns);
    struct TOPO_GROUP* group = topo_groups_find(si->db_sync->inst->topo_groups, si->group_id);
    for (struct ll_entry* e = group && group->senders_list ? group->senders_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        if (!item->conn) continue;
        struct SI_PEER* p = si_peer_add(si, item->conn->peer_ed25519_pubkey); if (p) p->sync_state = 0;
        if (!gated) db_start_peer(si, item->conn->peer_ed25519_pubkey);
    }
    db_sync_resume_peer_check(si->db_sync);
}
void db_sync_peer_policy_changed(struct ETCP_CONN* conn) {
    if (!conn) return;
    struct UTUN_INSTANCE* inst = conn->instance; const uint8_t* peer = conn->peer_ed25519_pubkey;
    struct DB_SYNC* db = inst ? inst->db_sync : NULL; if (!db) return;
    merkle_sync_recheck_peer(conn);
    for (int i = 0; i < db->instance_count; i++) {
        struct DB_SYNC_INSTANCE* si = db->instances[i]; struct SI_PEER* p = si_peer_find(si, peer);
        if (p && !p->sync_state) db_start_peer(si, peer);
    }
}
void db_sync_set_peer_sound(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], int sound) {
    struct DB_SYNC_INSTANCE* si = db_sync_instance_find(inst, group);
    if (!db_peer_allowed(si, instance_find_conn_by_ed(inst, peer))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "history SOUND_SETTING denied group=%llu peer=%s: no current CHAT admission",
            (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0})); return;
    }
    struct SI_PEER* p = si_peer_add(si, peer);
    if (!p) return;
    p->sound_on = !!sound;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "history SOUND_SETTING accepted group=%llu peer=%s sound=%u",
        (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), p->sound_on);
    db_sync_peer_policy_changed(instance_find_conn_by_ed(inst, peer));
}
void db_sync_members_changed(struct UTUN_INSTANCE* inst, const char* ns) {
    struct DB_SYNC* db = inst ? inst->db_sync : NULL; if (!db) return;
    merkle_sync_revalidate_peers(inst, MT_DATA_MESSAGES, ns);
    struct DB_SYNC_INSTANCE* channel = si_find(inst, ns);
    if (channel) {
        for (int i = 0; i < channel->peer_count; i++)
            if (!channel->peers[i].sync_state) db_start_peer(channel, channel->peers[i].ed);
    }
    for (int i = 0; i < db->instance_count; i++) {
        struct DB_SYNC_INSTANCE* si = db->instances[i];
        if (si->index.pending && si->index.waiting_key) {
            if (si->index.timer) uasync_cancel_timeout(inst->ua, si->index.timer);
            si->index.timer = uasync_set_timeout(inst->ua, 0, si, db_index_tick, "history_key_ready");
            if (!si->index.timer) db_sync_resume_peer_check(db);
        }
    }
}
