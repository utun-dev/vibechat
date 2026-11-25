#define GROUP_A "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"
#define GROUP_B "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16"

/* Собственная SQLite и настоящие подписи: сохранность истории, очистка порчи,
 * транзакционные отказы, восстановление Merkle и точная идентичность ACK. */
#include "../src/chat/db_sync.c"
#include <assert.h>

static int fail_verify, fail_commit, fail_timer, fail_blob, blob_nomem;
static unsigned fault_hits;
static sqlite3* verification_db;
static int assert_unlocked;
static int concentrate_bucket;
extern int __real_db_sync_message_id(uint64_t, const uint8_t[32], const struct db_message_revision*, uint8_t[32]);
int __wrap_db_sync_message_id(uint64_t ts, const uint8_t author[32], const struct db_message_revision* revision, uint8_t key[32]) {
    int rc = __real_db_sync_message_id(ts, author, revision, key);
    /* Только для плотного тестового листа: подписи настоящие, оставшиеся223 бита ключа сохраняются. */
    if (rc == 0 && concentrate_bucket) { key[0] = key[1] = key[2] = 0; key[3] &= 0x7f; }
    return rc;
}
extern int __real_EVP_DigestVerifyInit(EVP_MD_CTX*, EVP_PKEY_CTX**, const EVP_MD*, ENGINE*, EVP_PKEY*);
int __wrap_EVP_DigestVerifyInit(EVP_MD_CTX* ctx, EVP_PKEY_CTX** pctx, const EVP_MD* md, ENGINE* engine, EVP_PKEY* key) {
    if (assert_unlocked) assert(sqlite3_get_autocommit(verification_db));
    if (fail_verify && !--fail_verify) { fault_hits++; return 0; }
    return __real_EVP_DigestVerifyInit(ctx, pctx, md, engine, key);
}

extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** err) {
    if (fail_commit && !strcmp(sql, "COMMIT")) { fail_commit = 0; fault_hits++; return SQLITE_BUSY; }
    return __real_sqlite3_exec(db, sql, cb, arg, err);
}

extern const void* __real_sqlite3_column_blob(sqlite3_stmt*, int);
const void* __wrap_sqlite3_column_blob(sqlite3_stmt* stmt, int col) {
    if (fail_blob) { fail_blob = 0; blob_nomem = 1; fault_hits++; return NULL; }
    return __real_sqlite3_column_blob(stmt, col);
}
extern int __real_sqlite3_errcode(sqlite3*);
int __wrap_sqlite3_errcode(sqlite3* db) {
    if (blob_nomem) { blob_nomem = 0; return SQLITE_NOMEM; }
    return __real_sqlite3_errcode(db);
}

extern void* __real_uasync_set_timeout(struct UASYNC*, uint64_t, void*, void (*)(void*), const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, uint64_t delay, void* arg, void (*cb)(void*), const char* name) {
    if (fail_timer && !strcmp(name, "history_preflight")) { fail_timer = 0; fault_hits++; return NULL; }
    return __real_uasync_set_timeout(ua, delay, arg, cb, name);
}

/* Friend обеих моделей: настоящий Merkle failure callback удаляет историю внутри commit publication. */
static int fail_merkle_dispatch;
static void* integrity_call_soon(struct UASYNC* ua, void* arg, timeout_callback_t callback) {
    if (fail_merkle_dispatch) { fail_merkle_dispatch = 0; return NULL; }
    return uasync_call_soon(ua, arg, callback);
}
#define uasync_call_soon integrity_call_soon
#include "../src/chat/merkle_sync.c"
#undef uasync_call_soon

struct fixture {
    struct TOPO_GROUPS groups;
    struct UTUN_INSTANCE inst;
    struct DB_SYNC db;
    struct DB_SYNC_INSTANCE* si;
    struct DB_SYNC_INSTANCE* other;
    uint8_t seed[32];
    struct db_message_revision revision;
};
static int empty_hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    (void)ctx; (void)ns; (void)level; (void)prefix; (void)hash; return 0;
}
static int empty_page(void* ctx, const char* ns, uint64_t prefix, int av, const merkle_key_t* after,
        uint8_t* buf, size_t* len, merkle_key_t* next, int* more) {
    (void)ctx; (void)ns; (void)prefix; (void)av; (void)after; (void)buf;
    *len = 0; *more = 0; memset(next, 0, sizeof(*next)); return 0;
}
static int empty_apply(void* ctx, const char* ns, const uint8_t peer[32], const uint8_t* p, size_t len) {
    (void)ctx; (void)ns; (void)peer; (void)p; return len ? -1 : 0;
}
static const struct merkle_sync_data_ops empty_ops = {
    .update_bucket_hash = empty_hash, .get_page = empty_page, .apply_items = empty_apply, .group_scoped = 1
};
static void sql(struct fixture* f, const char* query) {
    int rc = sqlite3_exec(f->db.db, query, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "integrity SQL %s: %s", query, sqlite3_errmsg(f->db.db));
    assert(rc == SQLITE_OK);
}
/* Этот unit проверяет историю; результат проверки MEMBERS задаётся явно, по отдельности для каждой группы. */
static void member_table(struct fixture* f, unsigned group) {
    char query[512]; snprintf(query, sizeof(query), "CREATE TABLE peers_%s(node_id INTEGER PRIMARY KEY,"
        "x25519_pubkey BLOB DEFAULT(zeroblob(32)),ed25519_pubkey BLOB,record_state INTEGER DEFAULT 0,"
        "source INTEGER DEFAULT 0,deleted INTEGER DEFAULT 0,admitted INTEGER DEFAULT 1,identity_verified INTEGER DEFAULT 1,"
        "join_sig BLOB DEFAULT(zeroblob(64)),adm_tags TEXT)", group == 10 ? GROUP_A : GROUP_B);
    sql(f, query);
}
static void member_key(struct fixture* f, unsigned group, uint64_t node, int deleted) {
    char query[192]; snprintf(query, sizeof(query), "INSERT INTO peers_%s(node_id,ed25519_pubkey,deleted) VALUES(?,?,?)", group == 10 ? GROUP_A : GROUP_B);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->db.db, query, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, (sqlite3_int64)node) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->inst.my_ed25519_pubkey, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_int(st, 3, deleted) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}
static struct DB_SYNC_INSTANCE* new_table(struct fixture* f, const char* name, uint64_t group) {
    struct DB_SYNC_INSTANCE* si = u_calloc(1, sizeof(*si)); assert(si);
    si->db_sync = &f->db; si->enabled = 1; si->group_id = group; si->next_id = 1; si->sync_gated = 1;
    snprintf(si->table_name, sizeof(si->table_name), "%s", name);
    strcpy(si->ns, group == 10 ? GROUP_A : GROUP_B);
    assert(db_schema_history(f->db.db, name) == 0); return si;
}
static void init(struct fixture* f) {
    memset(f, 0, sizeof(*f)); f->inst.node_id = 1; f->inst.ua = uasync_create(); assert(f->inst.ua);
    f->inst.connections = queue_new(f->inst.ua, 8, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "history test"); assert(f->inst.connections);
    memset(f->seed, 9, sizeof(f->seed));
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, f->seed, 32); assert(key);
    size_t len = 32;
    assert(EVP_PKEY_get_raw_public_key(key, f->inst.my_ed25519_pubkey, &len) == 1); EVP_PKEY_free(key);
    assert(sqlite3_open(":memory:", &f->db.db) == SQLITE_OK);
    f->db.inst = &f->inst; f->db.enabled = 1; f->inst.db_sync = &f->db; f->inst.topo_sqlite_db = f->db.db;
    verification_db = f->db.db;
    f->groups.instance = &f->inst;
    f->groups.group_list = queue_new(f->inst.ua, 16, 0, sizeof(uint64_t), "history test routes"); assert(f->groups.group_list);
    f->inst.topo_groups = &f->groups;
    for (uint64_t slot = 10; slot <= 11; slot++) {
        struct TOPO_GROUP* group = (void*)queue_entry_new(sizeof(*group) - sizeof(group->ll)); assert(group);
        group->group_id = slot; group->group_type = TOPO_GROUP_TYPE_CHAT;
        strcpy(group->channel_id, slot == 10 ? GROUP_A : GROUP_B);
        assert(!queue_data_put_with_index(f->groups.group_list, &group->ll));
    }
    sql(f, "CREATE TABLE nodes(node_id INTEGER PRIMARY KEY,ed25519_pubkey BLOB,deleted INTEGER DEFAULT 0)");
    sql(f, "CREATE TABLE channels(channel_id TEXT PRIMARY KEY,x25519_pubkey BLOB,ed25519_pubkey BLOB);"
        "INSERT INTO channels VALUES('" GROUP_A "',zeroblob(32),x'" GROUP_A "'),('" GROUP_B "',zeroblob(32),x'" GROUP_B "')");
    member_table(f, 10); member_table(f, 11); member_key(f, 10, 1, 0); member_key(f, 11, 1, 0);
    f->si = new_table(f, "history", 10); f->other = new_table(f, "other_history", 11);
    f->db.instances = u_calloc(2, sizeof(*f->db.instances)); assert(f->db.instances);
    f->db.instances[0] = f->si; f->db.instances[1] = f->other; f->db.instance_count = f->db.instance_capacity = 2;
    assert(merkle_sync_init(&f->inst, ETCP_ID_MEMBER_SYNC, &empty_ops, f) == 0 && db_sync_register_model(&f->inst) == 0);
}
static void destroy(struct fixture* f) {
    f->db.enabled = 0;
    if (f->db.peer_check_timer) uasync_cancel_timeout(f->inst.ua, f->db.peer_check_timer);
    merkle_sync_destroy(&f->inst);
    while (f->db.instance_count) db_sync_instance_remove(f->db.instances[f->db.instance_count - 1]);
    while (f->db.done_cbks) { struct db_sync_done_cbk_entry* cb = f->db.done_cbks; f->db.done_cbks = cb->next; u_free(cb); }
    u_free(f->db.instances); queue_free(f->inst.connections);
    queue_clear(f->groups.group_list); queue_free(f->groups.group_list);
    assert(sqlite3_get_autocommit(f->db.db) && sqlite3_close(f->db.db) == SQLITE_OK);
    uasync_destroy(f->inst.ua, 0); verification_db = NULL;
}
static void sign(struct fixture* f, uint64_t ts, const char* text, uint8_t sig[64]) {
    uint8_t hash[32]; size_t len = strlen(text); assert(len <= DB_SYNC_MESSAGE_MAX);
    assert(EVP_Digest(text, len, hash, NULL, EVP_sha256(), NULL) == 1);
    f->revision = (struct db_message_revision){.ver = 1}; memcpy(f->revision.nonce, hash, 16);
    assert(!group_policy_message_generation(f->db.db, GROUP_A, f->inst.node_id, f->revision.generation));
    assert(db_sync_sign_message(f->db.db, GROUP_A, ts, f->inst.my_ed25519_pubkey, &f->revision, text, len, f->seed, sig) == 0);
}
static int insert(struct fixture* f, uint64_t ts, const char* text, const uint8_t sig[64]) {
    return db_sync_insert_signed(f->si, text, strlen(text), sig, 64, ts, NULL, &f->revision);
}
static void intact(struct fixture* f, const char* text) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->db.db, "SELECT data FROM history", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_bytes(st, 0) == (int)strlen(text) && !memcmp(sqlite3_column_blob(st, 0), text, strlen(text)));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK && sqlite3_get_autocommit(f->db.db));
}
static int page(struct fixture* f, const uint8_t sig[64]) {
    uint8_t id[32], data[16384]; size_t len = sizeof(data); merkle_key_t after = {{0}}, next; int more;
    (void)sig;
    assert(db_sync_message_id(100, f->inst.my_ed25519_pubkey, &f->revision, id) == 0);
    return db_message_page(&f->inst, GROUP_A, merkle_sync_level_prefix(db_read64(id), MT_MAX_LEVEL), 0, &after, data, &len, &next, &more);
}
static void test_preservation(void) {
    struct fixture f; init(&f); uint8_t sig[64]; sign(&f, 100, "original", sig);
    assert(insert(&f, 100, "original", sig) == 0 && db_sync_verify(f.si) == 0);
    assert(insert(&f, 100, "original", sig) == 1);
    fail_verify = 1; assert(insert(&f, 100, "original", sig) == -1); intact(&f, "original");
    fail_verify = 2; assert(insert(&f, 100, "modified", sig) == -1); intact(&f, "original");
    fail_blob = 1; assert(insert(&f, 100, "original", sig) == -1); intact(&f, "original");
    fail_blob = 1; assert(page(&f, sig) == -1); intact(&f, "original");
    fail_verify = 1; assert(page(&f, sig) == -1); intact(&f, "original");
    assert(insert(&f, 100, "modified", sig) == -2); intact(&f, "original");
    sql(&f, "DELETE FROM peers_" GROUP_A "");
    assert(insert(&f, 100, "original", sig) == -1 && page(&f, sig) == -1); intact(&f, "original");
    assert(db_sync_verify(f.si) == 1 && f.si->index.pending);
    sql(&f, "DROP TABLE peers_" GROUP_A "");
    assert(db_sync_verify(f.si) == -1); intact(&f, "original");
    member_table(&f, 10); member_key(&f, 10, 1, 0);
    assert(db_sync_verify(f.si) == 0);
    destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS history valid copy survives invalid input, crypto/SQL/BLOB failures and missing key");
}
static void test_deletion(void) {
    struct fixture f; init(&f); uint8_t sig[64]; sign(&f, 100, "original", sig);
    assert(insert(&f, 100, "original", sig) == 0);
    uint8_t other_sig[64];
    struct db_message_revision other_revision = f.revision;
    assert(!group_policy_message_generation(f.db.db, GROUP_B, 1, other_revision.generation));
    assert(db_sync_sign_message(f.db.db, GROUP_B, 100, f.inst.my_ed25519_pubkey, &other_revision, "original", 8, f.seed, other_sig) == 0);
    assert(db_sync_insert_signed(f.other, "original", 8, other_sig, 64, 100, NULL, &other_revision) == 0 && db_sync_verify(f.si) == 0);
    sql(&f, "UPDATE history SET data='modified';CREATE TRIGGER deny_delete BEFORE DELETE ON history BEGIN SELECT RAISE(FAIL,'delete denied');END");
    assert(insert(&f, 100, "modified", sig) == -1); intact(&f, "modified");
    sql(&f, "DROP TRIGGER deny_delete");
    fail_commit = 1; assert(insert(&f, 100, "modified", sig) == -1); intact(&f, "modified");
    assert(insert(&f, 100, "modified", sig) == -2 && db_sync_count(f.si) == 0 && f.si->index.pending);
    assert(db_sync_verify(f.si) == 0 && db_sync_count(f.other) == 1);
    assert(insert(&f, 100, "original", sig) == 0); sql(&f, "UPDATE history SET data='modified'");
    assert(insert(&f, 100, "original", sig) == 0); intact(&f, "original");
    sql(&f, "UPDATE history SET data=zeroblob(4001)");
    assert(db_sync_verify(f.si) == 0 && db_sync_count(f.si) == 0);
    const unsigned sizes[] = {0,1,63,65,255};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        assert(insert(&f, 100, "original", sig) == 0);
        char query[96]; snprintf(query, sizeof(query), "UPDATE history SET author_signature=zeroblob(%u)", sizes[i]); sql(&f, query);
        assert(db_sync_verify(f.si) == 0 && db_sync_count(f.si) == 0);
    }
    destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS proven malformed/signature-invalid stored records removed; rollback preserves data and other channel");
}
static void test_index(void) {
    struct fixture f; init(&f); uint8_t sig[64], root[32], prior[32];
    for (uint64_t ts = 1; ts <= 55; ts++) { sign(&f, ts, "record", sig); assert(insert(&f, ts, "record", sig) == 0); }
    assert_unlocked = 1; fail_blob = 1;
    assert(db_sync_verify(f.si) == -1 && db_sync_count(f.si) == 55 && f.si->index.pending);
    fail_commit = 1; assert(db_sync_verify(f.si) == -1 && f.si->index.pending && sqlite3_get_autocommit(f.db.db));
    sql(&f, "CREATE TRIGGER deny_hash BEFORE INSERT ON merkle_tree_hash WHEN NEW.namespace='2:" GROUP_A "' BEGIN SELECT RAISE(FAIL,'hash denied');END");
    assert(db_sync_verify(f.si) == -1 && f.si->index.pending && db_sync_read_root(f.si, root) == 1);
    sql(&f, "DROP TRIGGER deny_hash");
    assert(db_sync_verify(f.si) == 0 && db_sync_read_root(f.si, prior) == 0); assert_unlocked = 0;
    uint64_t next = f.si->next_id;
    sign(&f, 56, "record", sig); fail_commit = 1;
    assert(insert(&f, 56, "record", sig) == -1 && db_sync_count(f.si) == 55 && f.si->next_id == next);
    assert(db_sync_read_root(f.si, root) == 0 && !memcmp(root, prior, 32));
    fail_timer = 1; db_index_request(f.si);
    assert(!fail_timer && f.si->index.pending && !f.si->index.timer && db_sync_verify(f.si) == 0);
    assert(db_sync_read_root(f.si, prior) == 0);
    sql(&f, "UPDATE history SET id=id+900,flags=1,local_attrs='local',delivered_peers=17,delivery_chain='different'");
    assert(db_sync_verify(f.si) == 0 && db_sync_read_root(f.si, root) == 0 && !memcmp(root, prior, 32));
    destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS bounded preflight verifies outside write-lock; index failures recover; root ignores local ids and delivery");
}
static void test_delivery(void) {
    struct fixture f; init(&f); uint8_t a[64], b[64], id_a[32], id_b[32];
    sign(&f, 100, "first", a); assert(insert(&f, 100, "first", a) == 0);
    assert(db_sync_message_id(100, f.inst.my_ed25519_pubkey, &f.revision, id_a) == 0);
    sign(&f, 100, "second", b); assert(insert(&f, 100, "second", b) == 0);
    assert(db_sync_message_id(100, f.inst.my_ed25519_pubkey, &f.revision, id_b) == 0);
    assert(si_delivery_update(f.si, id_a, 1, (uint8_t[32]){[31]=2}) == 0 && si_delivery_update(f.si, id_a, 1, (uint8_t[32]){[31]=2}) == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f.db.db, "SELECT data,delivered_peers,flags FROM history ORDER BY data", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 1) == 1 && sqlite3_column_int(st, 2) == 1);
    assert(sqlite3_step(st) == SQLITE_ROW && !sqlite3_column_int(st, 1) && !sqlite3_column_int(st, 2));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    sql(&f, "CREATE TRIGGER deny_ack BEFORE UPDATE ON history BEGIN SELECT RAISE(FAIL,'ACK denied');END");
    assert(si_delivery_update(f.si, id_b, 1, (uint8_t[32]){[31]=3}) == -1); sql(&f, "DROP TRIGGER deny_ack");
    assert(si_delivery_update(f.si, id_b, 1, (uint8_t[32]){[31]=3}) == 0);
    destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS exact same-timestamp ACK identity and idempotence");
}
/* Настоящие подписи и MESSAGES-страницы: версия меняет содержимое, сохраняя ID и локальный ряд. */
static void test_revisions(void) {
    struct fixture a, b; init(&a); init(&b); uint8_t signature[64], key[32], root1[32], root5[32], other[32];
    sign(&a, 100, "original", signature); struct db_message_revision revision = a.revision;
    assert(insert(&a, 100, "original", signature) == 0 && db_sync_verify(a.si) == 0 && db_sync_verify(b.si) == 0);
    assert(db_sync_message_id(100, a.inst.my_ed25519_pubkey, &revision, key) == 0 && db_sync_read_root(a.si, root1) == 0);
    sql(&a, "UPDATE history SET local_attrs='saved',flags=1,delivered_peers=9,delivery_chain='old'");
    revision.ver = 5; revision.edited_at = 500;
    assert(db_sync_sign_message(a.db.db, GROUP_A, 100, a.inst.my_ed25519_pubkey, &revision, "edited", 6, a.seed, signature) == 0);
    fail_commit = 1;
    assert(db_sync_insert_signed(a.si, "edited", 6, signature, 64, 100, NULL, &revision) == -1);
    intact(&a, "original"); assert(a.si->next_id == 2);
    assert(db_sync_read_root(a.si, other) == 0 && !memcmp(root1, other, 32));
    assert(db_sync_insert_signed(a.si, "edited", 6, signature, 64, 100, NULL, &revision) == 0 && a.si->next_id == 2);
    intact(&a, "edited"); assert(db_sync_count(a.si) == 1);
    assert(db_sync_read_root(a.si, root5) == 0 && memcmp(root1, root5, 32));
    assert(si_delivery_update(a.si, key, 1, (uint8_t[32]){[31]=2}) == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(a.db.db, "SELECT id,ver,edited_at,local_attrs,delivered_peers FROM history", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1 && sqlite3_column_int(st, 1) == 5 &&
        sqlite3_column_int(st, 2) == 500 && !strcmp((const char*)sqlite3_column_text(st, 3), "saved") && !sqlite3_column_int(st, 4));
    assert(sqlite3_finalize(st) == SQLITE_OK);
    assert(si_delivery_update(a.si, key, 5, (uint8_t[32]){[31]=2}) == 0);
    uint8_t wire[DB_WIRE_MAX]; size_t bytes = sizeof(wire); merkle_key_t after = {{0}}, next; int more;
    uint64_t prefix = merkle_sync_level_prefix(db_read64(key), MT_MAX_LEVEL);
    assert(db_message_page(&a.inst, GROUP_A, prefix, 0, &after, wire, &bytes, &next, &more) == 0 && !more);
    assert(db_message_apply(&b.inst, GROUP_A, (uint8_t[32]){2}, prefix, 0, &after, &next, more, wire, bytes) == 0);
    intact(&b, "edited"); assert(db_sync_read_root(b.si, other) == 0 && !memcmp(root5, other, 32));
    assert(db_message_apply(&b.inst, GROUP_A, (uint8_t[32]){2}, prefix, 0, &after, &next, more, wire, bytes) == 0 && b.si->next_id == 2);
    struct db_record parsed; const uint8_t* cursor = wire;
    assert(db_parse_record(b.si, &cursor, wire + bytes, &parsed) == 0 && parsed.revision.ver == 5);
    parsed.revision.ver = 2; parsed.revision.edited_at = 200; parsed.data = "older"; parsed.data_len = 5;
    assert(db_sync_sign_message(a.db.db, GROUP_A, 100, a.inst.my_ed25519_pubkey, &parsed.revision, parsed.data, parsed.data_len, a.seed, parsed.signature) == 0);
    assert(db_insert_one(b.si, &parsed, NULL) == 1); intact(&b, "edited");
    parsed.revision = revision; parsed.data = "conflict"; parsed.data_len = 8;
    assert(db_sync_sign_message(a.db.db, GROUP_A, 100, a.inst.my_ed25519_pubkey, &parsed.revision, parsed.data, parsed.data_len, a.seed, parsed.signature) == 0);
    assert(db_insert_one(b.si, &parsed, NULL) == -2); intact(&b, "edited");
    size_t conflict_bytes = (size_t)db_encode_record(&parsed, wire);
    assert(db_message_apply(&b.inst, GROUP_A, (uint8_t[32]){2}, prefix, 0, &after, &next, 0, wire, conflict_bytes) == -1);
    assert(db_sync_read_root(b.si, other) == 0 && !memcmp(root5, other, 32));
    wire[DB_WIRE_AUTHOR] ^= 1; cursor = wire;
    assert(db_parse_record(b.si, &cursor, wire + conflict_bytes, &parsed) == -1);
    memcpy(parsed.accepted_key, a.inst.my_ed25519_pubkey, 32); parsed.revision = revision; parsed.data = "edited"; parsed.data_len = 6; parsed.sig_len = 64;
    assert(db_sync_sign_message(a.db.db, GROUP_B, 100, a.inst.my_ed25519_pubkey, &revision, parsed.data, parsed.data_len, a.seed, parsed.signature) == 0);
    assert(db_insert_one(b.si, &parsed, NULL) == -2); intact(&b, "edited");
    destroy(&a); destroy(&b);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS revision jump/first-seen v5, rollback, roots, stale ACK, duplicate, old version, conflict and author/channel binding");
}

static int first_calls, second_calls;
static void second_done(struct DB_SYNC_INSTANCE* si, const uint8_t peer[32], void* arg) {
    (void)si; (void)peer; (void)arg; second_calls++;
}
static void first_done(struct DB_SYNC_INSTANCE* si, const uint8_t peer[32], void* arg) {
    (void)peer; struct fixture* f = arg; first_calls++;
    db_sync_remove_done_cbk(&f->inst, first_done, f); db_sync_remove_done_cbk(&f->inst, second_done, f);
    db_sync_instance_remove(si);
}
static void test_callbacks(void) {
    struct fixture f; init(&f);
    db_sync_add_done_cbk(&f.inst, second_done, &f); db_sync_add_done_cbk(&f.inst, first_done, &f);
    db_message_result(&f.inst, GROUP_A, (uint8_t[32]){2}, MT_OK);
    assert(first_calls == 1 && !second_calls && !db_instance_find(&f.db, 10) && !f.db.done_cbks);
    destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS completion can remove itself, next callback and channel safely");
}

static unsigned committed_callbacks, removed_callbacks;
static void committed_notice(struct DB_SYNC_INSTANCE* si, const uint8_t message[32], uint64_t timestamp,
    const char* data, size_t bytes, uint64_t author, const uint8_t author_ed[32], int incoming, int updated, void* arg) {
    (void)message; (void)timestamp; (void)data; (void)bytes; (void)author; (void)incoming; (void)updated;
    struct fixture* f = arg; committed_callbacks++;
    assert(!memcmp(author_ed, f->inst.my_ed25519_pubkey, 32));
    assert(si == f->si && si->holds >= 2); db_sync_instance_remove(si); f->si = NULL;
}
static void remove_committed_history(const uint8_t peer[32], const char* channel, int result, void* arg) {
    struct fixture* f = arg;
    assert(!memcmp(peer, (uint8_t[32]){2}, 32) && !strcmp(channel, GROUP_A) && result == MT_ERR_IO && f->si && f->si->holds);
    removed_callbacks++; db_sync_instance_remove(f->si); f->si = NULL;
}
static void test_committed_callbacks(void) {
    for (int remove_before_notice = 0; remove_before_notice <= 1; remove_before_notice++) {
        struct fixture f; init(&f); uint8_t signature[64]; sign(&f, 100, "committed", signature);
        assert(!insert(&f, 100, "committed", signature) && !db_sync_verify(f.si));
        db_sync_set_insert_cb(f.si, committed_notice, &f);
        struct db_sync_record record = {.timestamp=100, .author=1, .local_id=1, .revision=f.revision,
            .data="committed", .size=9, .result=0, .changed=remove_before_notice};
        memcpy(record.signature, signature, 64);
        memcpy(record.author_ed, f.inst.my_ed25519_pubkey, 32);
        assert(!db_sync_message_id(100, f.inst.my_ed25519_pubkey, &record.revision, record.message_id));
        struct ETCP_CONN connection = {.peer_node_id=2, .peer_ed25519_pubkey={2}, .initialized=1, .links_up=1, .send_input_q=f.inst.connections};
        if (remove_before_notice) {
            f.inst.msync->datasets[MT_DATA_MESSAGES].ops = &empty_ops; /* Допуск уже задан fixture, проверяем lifetime автомата. */
            struct ms_session* session = ms_create(f.inst.msync, &connection, NULL, MT_DATA_MESSAGES, 10, MS_EXCHANGE); assert(session);
            session->requests = u_calloc(1, sizeof(*session->requests)); assert(session->requests);
            session->requests->cb = remove_committed_history; session->requests->arg = &f;
            session->send_q = NULL; /* Отказ отправки ERROR доставляет pending done немедленно, без сети. */
            fail_merkle_dispatch = 1;
        }
        unsigned before_notices = committed_callbacks, before_removed = removed_callbacks;
        db_sync_record_committed(f.si, &record, NULL);
        assert(!f.si && !db_instance_find(&f.db, 10));
        assert(committed_callbacks == before_notices + !remove_before_notice && removed_callbacks == before_removed + remove_before_notice);
        assert(!fail_merkle_dispatch && !connection.ref_count); destroy(&f);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS committed publication survives real Merkle/insert callbacks removing its history");
}

static void copy_pages(struct fixture* source, struct fixture* target, int insert_before_cursor) {
    merkle_key_t after = {{0}}, next; int av = 0, more, pages = 0;
    do {
        uint8_t data[16384]; size_t len = sizeof(data);
        assert(db_message_page(&source->inst, GROUP_A, 0, av, &after, data, &len, &next, &more) == 0);
        assert(db_message_apply(&target->inst, GROUP_A, (uint8_t[32]){2}, 0, av, &after, &next, more, data, len) == 0);
        after = next; av = 1; pages++;
        if (pages == 1 && insert_before_cursor) {
            uint8_t sig[64], key[32]; int found = 0;
            for (uint64_t ts = 1000; ts < 2000; ts++) {
                sign(source, ts, "inserted before cursor", sig);
                assert(db_sync_message_id(ts, source->inst.my_ed25519_pubkey, &source->revision, key) == 0);
                if (memcmp(key, after.bytes, 32) < 0) {
                    assert(insert(source, ts, "inserted before cursor", sig) == 0); found = 1; break;
                }
            }
            assert(found);
        }
    } while (more);
    assert(pages >= 3);
}
static void test_pages(void) {
    concentrate_bucket = 1;
    struct fixture a, b; init(&a); init(&b); uint8_t sig[64], ra[32], rb[32];
    b.si->next_id = 900;
    for (uint64_t ts = 1; ts <= 40; ts++) { sign(&a, ts, "page record", sig); assert(insert(&a, ts, "page record", sig) == 0); }
    assert(db_sync_verify(a.si) == 0 && db_sync_verify(b.si) == 0);
    copy_pages(&a, &b, 1);
    assert(db_sync_count(a.si) == 41 && db_sync_count(b.si) == 40);
    assert(db_sync_read_root(a.si, ra) == 0 && db_sync_read_root(b.si, rb) == 0 && memcmp(ra, rb, 32));
    copy_pages(&a, &b, 0);
    assert(db_sync_count(b.si) == 41 && db_sync_read_root(b.si, rb) == 0 && !memcmp(ra, rb, 32));
    copy_pages(&a, &b, 0); assert(db_sync_count(b.si) == 41);
    destroy(&a); destroy(&b); concentrate_bucket = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS multi-page leaf, different local ids, duplicate replay and insertion before cursor need a new root check");
}
static void test_deleted_author(void) {
    struct fixture f; init(&f); uint8_t sig[64]; sign(&f, 100, "historic", sig);
    struct db_record r = {.timestamp = 100, .author = 99, .data = "historic", .data_len = 8, .sig_len = 64, .revision = f.revision};
    memcpy(r.accepted_key, f.inst.my_ed25519_pubkey, 32);
    assert(db_sync_sign_message(f.db.db, GROUP_A, 100, r.accepted_key, &r.revision, r.data, r.data_len, f.seed, r.signature) == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f.db.db, "INSERT INTO nodes VALUES(99,?,1)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, f.inst.my_ed25519_pubkey, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_finalize(st) == SQLITE_OK);
    sql(&f, "DELETE FROM peers_" GROUP_A ""); member_key(&f, 10, 99, 1);
    assert(!db_sync_message_id(100, r.accepted_key, &r.revision, r.message_id));
    struct db_store store = db_store_for_instance(f.si);
    assert(db_verify_author_sig(&store, &r) == DB_CHECK_VALID && db_insert_one(f.si, &r, NULL) == -1 && db_sync_count(f.si) == 0);
    destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS deleted author signature is authentic but new history is rejected");
}

static void colliding_members(struct fixture* f, const uint8_t other[32]) {
    sql(f, "ALTER TABLE peers_" GROUP_A " RENAME TO fixture_peers_a;"
        "CREATE TABLE peers_" GROUP_A " AS SELECT * FROM fixture_peers_a");
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->db.db, "INSERT INTO peers_" GROUP_A " SELECT node_id,x25519_pubkey,?,record_state,source,"
        "deleted,admitted,identity_verified,join_sig,adm_tags FROM fixture_peers_a", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, other, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_finalize(st) == SQLITE_OK);
}

static void test_colliding_author_projection(void) {
    concentrate_bucket = 1;
    struct fixture f, receiver; init(&f); init(&receiver);
    uint8_t seed[32] = {7}, other[32], signature[64]; size_t bytes = sizeof(other);
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, sizeof(seed));
    assert(key && EVP_PKEY_get_raw_public_key(key, other, &bytes) == 1 && bytes == sizeof(other));
    EVP_PKEY_free(key);
    colliding_members(&f, other); colliding_members(&receiver, other);
    sql(&receiver, "UPDATE peers_" GROUP_A " SET node_id=17");
    assert(!db_sync_verify(receiver.si));
    struct db_message_revision revision = {.nonce={1}, .ver=1};
    assert(!group_policy_key_generation(f.db.db, GROUP_A, f.inst.my_ed25519_pubkey, revision.generation));
    assert(!db_sync_sign_message(f.db.db, GROUP_A, 100, f.inst.my_ed25519_pubkey, &revision, "first", 5, f.seed, signature));
    assert(!db_sync_insert_signed(f.si, "first", 5, signature, 64, 100, NULL, &revision));
    struct db_record r = {.timestamp=100, .data="second", .data_len=6, .sig_len=64, .revision=revision};
    memcpy(r.accepted_key, other, 32);
    assert(!group_policy_key_generation(f.db.db, GROUP_A, other, r.revision.generation));
    assert(!db_sync_sign_message(f.db.db, GROUP_A, 100, other, &r.revision, r.data, r.data_len, seed, r.signature));
    assert(!db_insert_one(f.si, &r, NULL) && r.author == 1 && db_sync_count(f.si) == 2 && !db_sync_verify(f.si));
    merkle_key_t after = {{0}}, next; uint8_t page[2 * DB_WIRE_MAX], source_root[32], target_root[32];
    bytes = sizeof(page); int more;
    assert(!db_message_page(&f.inst, GROUP_A, 0, 0, &after, page, &bytes, &next, &more) && !more);
    assert(!db_message_apply(&receiver.inst, GROUP_A, (uint8_t[32]){2}, 0, 0, &after, &next, more, page, bytes));
    assert(db_sync_count(receiver.si) == 2 && !db_sync_read_root(f.si, source_root) && !db_sync_read_root(receiver.si, target_root));
    assert(!memcmp(source_root, target_root, 32));
    destroy(&f); destroy(&receiver); concentrate_bucket = 0;
}
static void page_inserted(struct DB_SYNC_INSTANCE* si, const uint8_t id[32], uint64_t ts,
        const char* data, size_t len, uint64_t author, const uint8_t author_ed[32], int initial, int updated, void* arg) {
    (void)id; (void)ts; (void)data; (void)len; (void)author; (void)author_ed;
    assert(initial && !updated && sqlite3_get_autocommit(SI_DB(si)) && db_sync_count(si) == 3);
    (*(unsigned*)arg)++;
}
static void test_page_rollback(void) {
    concentrate_bucket = 1;
    struct fixture a, b; init(&a); init(&b); uint8_t sig[64], before[32], after_root[32], data[16384];
    for (uint64_t ts = 1; ts <= 3; ts++) { sign(&a, ts, "atomic page", sig); assert(insert(&a, ts, "atomic page", sig) == 0); }
    assert(db_sync_verify(a.si) == 0 && db_sync_verify(b.si) == 0 && db_sync_read_root(b.si, before) == 0);
    merkle_key_t after = {{0}}, next; int more; size_t len = sizeof(data); unsigned notices = 0;
    assert(db_message_page(&a.inst, GROUP_A, 0, 0, &after, data, &len, &next, &more) == 0 && !more);
    db_sync_set_insert_cb(b.si, page_inserted, &notices);
    sql(&b, "CREATE TRIGGER deny_second BEFORE INSERT ON history WHEN NEW.id=2 BEGIN SELECT RAISE(FAIL,'second denied');END");
    assert(db_message_apply(&b.inst, GROUP_A, (uint8_t[32]){2}, 0, 0, &after, &next, more, data, len) == -1);
    assert(!db_sync_count(b.si) && b.si->next_id == 1 && !b.db.notices && sqlite3_get_autocommit(b.db.db));
    assert(db_sync_read_root(b.si, after_root) == 0 && !memcmp(before, after_root, 32));
    sql(&b, "DROP TRIGGER deny_second"); fail_commit = 1;
    assert(db_message_apply(&b.inst, GROUP_A, (uint8_t[32]){2}, 0, 0, &after, &next, more, data, len) == -1);
    assert(!db_sync_count(b.si) && b.si->next_id == 1 && !b.db.notices && !notices);
    assert(db_sync_read_root(b.si, after_root) == 0 && !memcmp(before, after_root, 32));
    assert(db_message_apply(&b.inst, GROUP_A, (uint8_t[32]){2}, 0, 0, &after, &next, more, data, len) == 0);
    assert(db_sync_count(b.si) == 3 && b.db.notices && !notices);
    uint64_t deadline = get_time_tb() + 10000;
    while (notices != 3 && get_time_tb() < deadline) uasync_poll(b.inst.ua, 1);
    assert(notices == 3 && !b.db.notices);
    assert(db_message_apply(&b.inst, GROUP_A, (uint8_t[32]){2}, 0, 0, &after, &next, more, data, len) == 0 && !b.db.notices && notices == 3);
    destroy(&a); destroy(&b); concentrate_bucket = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS page SQL/commit failures roll back rows and root; notifications observe only committed records");
}
static void test_late_author_key(void) {
    struct fixture f; init(&f); uint8_t sig[64]; sign(&f, 100, "waiting key", sig);
    assert(insert(&f, 100, "waiting key", sig) == 0);
    sql(&f, "DELETE FROM peers_" GROUP_A "; DELETE FROM peers_" GROUP_B "");
    assert(db_sync_verify(f.si) == 1 && f.si->index.pending && f.si->index.waiting_key && db_sync_count(f.si) == 1);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f.db.db, "INSERT INTO nodes VALUES(99,?,1)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, f.inst.my_ed25519_pubkey, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_finalize(st) == SQLITE_OK);
    member_key(&f, 11, 99, 0);
    db_sync_members_changed(&f.inst, GROUP_B);
    for (unsigned i = 0; i < 10; i++) uasync_poll(f.inst.ua, 1);
    assert(f.si->index.pending && f.si->index.waiting_key && db_sync_count(f.si) == 1);
    member_key(&f, 10, 99, 1); db_sync_members_changed(&f.inst, GROUP_A);
    uint64_t deadline = get_time_tb() + 10000;
    while (f.si->index.pending && get_time_tb() < deadline) uasync_poll(f.inst.ua, 1);
    uint8_t root[32];
    assert(!f.si->index.pending && db_sync_count(f.si) == 1 && db_sync_read_root(f.si, root) == 0 && page(&f, sig) == 0);
    destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS only this group's author certificate resumes history; other group/global keys have no authority");
}
static void test_ttl(void) {
    struct fixture f; init(&f); uint8_t sig[64];
    struct utun_config* config = u_calloc(1, sizeof(*config)); assert(config); f.inst.config = config;
    config->global.db_sync_ttl = 3600;
    uint64_t now = db_now_ms(f.si), old = now - 3610000;
    sign(&f, now, "fresh", sig); assert(insert(&f, now, "fresh", sig) == 0);
    for (unsigned i = 0; i < 20; i++) { sign(&f, old + i, "expired", sig); assert(insert(&f, old + i, "expired", sig) == 0); }
    sql(&f, "UPDATE history SET flags=1 WHERE rowid=2");
    assert(db_sync_verify(f.si) == 0);
    db_ttl_tick(f.si);
    assert(db_sync_count(f.si) == 5 && f.si->index.pending && sqlite3_get_autocommit(f.db.db));
    uint64_t deadline = get_time_tb() + 10000;
    while (db_sync_count(f.si) != 2 && get_time_tb() < deadline) uasync_poll(f.inst.ua, 1);
    assert(db_sync_count(f.si) == 2 && db_sync_verify(f.si) == 0);
    destroy(&f); u_free(config);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS TTL uses milliseconds and short batches, preserving fresh and individually acknowledged records");
}
static void test_frozen_model(void) {
    concentrate_bucket = 1;
    struct fixture a, b; init(&a); init(&b); uint8_t signature[64]; unsigned notices = 0;
    for (uint64_t timestamp = 1; timestamp <= 3; timestamp++) {
        sign(&a, timestamp, "frozen model", signature); assert(!insert(&a, timestamp, "frozen model", signature));
    }
    assert(!db_sync_verify(a.si) && !db_sync_verify(b.si));
    struct db_message_sql_scope source = {.next_id=a.si->next_id}, target = {.next_id=b.si->next_id};
    assert(!db_sync_scope_snapshot(a.si, &source.history) && !db_sync_scope_snapshot(b.si, &target.history));
    struct merkle_sync_sql_request request = {.action=MT_SQL_PAGE, .capacity=MT_PAGE_SIZE}; strcpy(request.ns, GROUP_A);
    sql(&a, "BEGIN"); assert(!db_message_sql_ops.work(a.db.db, &source, &request)); sql(&a, "COMMIT");
    assert(request.length && !request.more && request.result == MT_OK);
    request.action = MT_SQL_APPLY_PAGE;
    sql(&b, "CREATE TRIGGER frozen_deny BEFORE INSERT ON history WHEN NEW.id=2 BEGIN SELECT RAISE(FAIL,'second denied');END");
    sql(&b, "BEGIN IMMEDIATE"); assert(db_message_sql_ops.work(b.db.db, &target, &request) != SQLITE_OK);
    sql(&b, "ROLLBACK"); assert(!db_sync_count(b.si) && b.si->next_id == 1);
    sql(&b, "DROP TRIGGER frozen_deny"); memset(&target, 0, sizeof(target)); target.next_id = 1;
    assert(!db_sync_scope_snapshot(b.si, &target.history));
    sql(&b, "BEGIN IMMEDIATE"); assert(!db_message_sql_ops.work(b.db.db, &target, &request)); sql(&b, "COMMIT");
    assert(db_sync_count(b.si) == 3 && b.si->next_id == 1 && target.next_id == 4);
    struct chat_core_ctx cc = {.initialized=1}; b.inst.chat_core = &cc; b.inst.group_generation = target.generation = 1;
    b.inst.chat_content_started = 1; db_sync_set_insert_cb(b.si, page_inserted, &notices);
    db_message_sql_ops.publish(&b.inst, &target, &request); assert(notices == 3 && b.si->next_id == 4);
    b.inst.chat_core = NULL; b.inst.chat_content_started = 0;
    request.next.bytes[31] ^= 1; memset(&target, 0, sizeof(target)); target.next_id = 4;
    assert(!db_sync_scope_snapshot(b.si, &target.history)); sql(&b, "BEGIN IMMEDIATE");
    assert(db_message_sql_ops.work(b.db.db, &target, &request) == SQLITE_CONSTRAINT && request.result == MT_ERR_DATA);
    sql(&b, "ROLLBACK"); assert(db_sync_count(b.si) == 3 && notices == 3);
    destroy(&a); destroy(&b); concentrate_bucket = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS frozen MESSAGES PAGE/WRITE: atomic rollback, cursor rejection and postcommit publication");
}

static void test_frozen_network(void) {
    struct fixture f; init(&f); uint8_t signature[64];
    struct db_network_job push = {.inst=(void*)1,.type=DB_MSG_PUSH,.peer={2}};
    assert(!db_sync_scope_snapshot(f.si, &push.scope));
    sign(&f, 100, "worker PUSH", signature);
    struct db_record record = {.timestamp=100,.author=1,.revision=f.revision,.data="worker PUSH",.data_len=11,.sig_len=64};
    memcpy(record.signature, signature, 64);
    memcpy(record.accepted_key, f.inst.my_ed25519_pubkey, 32);
    assert(!db_sync_message_id(record.timestamp, f.inst.my_ed25519_pubkey, &record.revision, record.message_id));
    push.length = (size_t)db_encode_record(&record, push.wire); push.record.local_id = 1;
    sql(&f, "BEGIN IMMEDIATE"); assert(!db_network_work(f.db.db, &push)); sql(&f, "ROLLBACK");
    assert(!db_sync_count(f.si));
    sql(&f, "BEGIN IMMEDIATE"); assert(!db_network_work(f.db.db, &push)); sql(&f, "COMMIT");
    assert(push.record.result == 0 && db_sync_count(f.si) == 1);
    f.si->next_id = 2; /* Runtime publication применяет этот счётчик после commit. */
    struct db_network_job ack = {.inst=(void*)1,.type=DB_MSG_ACK_PUSH,.peer={2},.scope=push.scope};
    memcpy(ack.self, f.inst.my_ed25519_pubkey, 32);
    memcpy(ack.wire, record.message_id, 32); db_write64(ack.wire + 32, 1);
    sql(&f, "BEGIN IMMEDIATE"); assert(!db_network_work(f.db.db, &ack)); sql(&f, "COMMIT");
    sql(&f, "BEGIN IMMEDIATE"); assert(!db_network_work(f.db.db, &ack)); sql(&f, "COMMIT");
    sign(&f, 101, "expires", signature); assert(!insert(&f, 101, "expires", signature));
    struct db_ttl_job ttl = {.inst=(void*)1,.scope=push.scope,.cutoff=102};
    memcpy(ttl.self, f.inst.my_ed25519_pubkey, 32);
    sql(&f, "BEGIN IMMEDIATE"); assert(!db_ttl_work(f.db.db, &ttl) && ttl.removed == 1); sql(&f, "ROLLBACK");
    assert(db_sync_count(f.si) == 2);
    sql(&f, "BEGIN IMMEDIATE"); assert(!db_ttl_work(f.db.db, &ttl) && ttl.removed == 1); sql(&f, "COMMIT");
    assert(db_sync_count(f.si) == 1); destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS frozen PUSH/ACK/TTL: no runtime access, atomic rollback, idempotent ACK and acknowledged retention");
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
    size_t baseline = u_get_allocated_count();
    test_preservation(); test_deletion(); test_index(); test_delivery(); test_callbacks(); test_committed_callbacks();
    test_pages(); test_deleted_author(); test_page_rollback(); test_late_author_key(); test_ttl(); test_revisions(); test_frozen_model();
    test_frozen_network();
    test_colliding_author_projection();
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "history injected failures=%u allocations=%zu baseline=%zu", fault_hits, u_get_allocated_count(), baseline);
    assert(fault_hits == 12 && u_get_allocated_count() == baseline);
    return 0;
}
