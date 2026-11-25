/* Ошибки чтения DM не сбрасывают seq/историю/hidden и не разрешают создание вместо загрузки. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/evp.h>
#include <openssl/sha.h>
#include "../src/utun_instance.h"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_crypto.h"
#include "../src/dm/dm_history.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_event.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"

enum fault { FAULT_NONE, FAULT_PREPARE, FAULT_BIND, FAULT_STEP, FAULT_FINALIZE, FAULT_XKEY, FAULT_EDKEY, FAULT_CRYPTO };
enum operation { OP_START, OP_RECEIVE, OP_SEND, OP_LIST, OP_INVITE, OP_FILE };
static enum fault fault;
static const char* fault_query;
static int injections, events, invites, invite_result, file_failures;
static unsigned checks;

static int inject(enum fault kind, const char* sql) {
    if (fault != kind || injections || (sql && !strstr(sql, fault_query))) return 0;
    injections++;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM load test: inject fault=%d query=%s", kind, sql ? sql : "X25519 derive");
    return 1;
}

extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (inject(FAULT_PREPARE, sql)) { *st = NULL; return SQLITE_IOERR; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* text, int bytes, void (*destroy)(void*)) {
    if (inject(FAULT_BIND, sqlite3_sql(st))) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, text, bytes, destroy);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (inject(FAULT_STEP, sqlite3_sql(st))) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int failed = st && inject(FAULT_FINALIZE, sqlite3_sql(st));
    int rc = __real_sqlite3_finalize(st);
    return failed ? SQLITE_IOERR : rc;
}
extern int __real_EVP_PKEY_derive(EVP_PKEY_CTX*, unsigned char*, size_t*);
int __wrap_EVP_PKEY_derive(EVP_PKEY_CTX* ctx, unsigned char* shared, size_t* bytes) {
    return inject(FAULT_CRYPTO, NULL) ? 0 : __real_EVP_PKEY_derive(ctx, shared, bytes);
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    events++;
    if (type == CHAT_EVT_ATTACHMENT_PREPARATION && len >= 18 && data[len - 1] == 2) file_failures++;
    if (type == CHAT_EVT_DM_INVITE_RESULT && len == 60) {
        invites++;
        memcpy(&invite_result, data + 8, 4);
    }
}

static sqlite3_int64 scalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_int64 value = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    return value;
}

/* Сравниваем все поля всех DM-строк, включая точные ключи/подписи/тела и файловые очереди. */
static int snapshot(sqlite3* db, uint8_t hash[32]) {
    const char* tables[] = { "dm_conversations", "dm_messages", "dm_outbox", "dm_history",
                             "dm_deleted_messages", "dm_files", "dm_local_files" };
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        char sql[128]; sqlite3_stmt* st = NULL;
        snprintf(sql, sizeof(sql), "SELECT * FROM %s ORDER BY rowid", tables[i]);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
        SHA256_Update(&ctx, tables[i], strlen(tables[i]));
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            for (int col = 0; col < sqlite3_column_count(st); col++) {
                int type = sqlite3_column_type(st, col), bytes = sqlite3_column_bytes(st, col);
                SHA256_Update(&ctx, &type, sizeof(type)); SHA256_Update(&ctx, &bytes, sizeof(bytes));
                if (bytes) SHA256_Update(&ctx, sqlite3_column_blob(st, col), (size_t)bytes);
            }
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return -1;
    }
    SHA256_Final(hash, &ctx);
    return 0;
}

static int keys(struct UTUN_INSTANCE* inst, struct UTUN_INSTANCE* peer, enum fault damage) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db,
        "UPDATE dm_conversations SET peer_x25519=?,peer_ed25519=? WHERE conv_id=?", -1, &st, NULL)) return -1;
    sqlite3_bind_blob(st, 1, peer->my_keys.public_key, damage == FAULT_XKEY ? 1 : 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, peer->my_ed25519_pubkey, damage == FAULT_EDKEY ? 1 : 32, SQLITE_STATIC);
    char conv[21]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)dm_derive_conv_id(inst->my_ed25519_pubkey, peer->my_ed25519_pubkey));
    sqlite3_bind_text(st, 3, conv, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int peer(struct UTUN_INSTANCE* inst, struct UTUN_INSTANCE* other, const char* group) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db,
        "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)", -1, &st, NULL)) return -1;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)other->node_id);
    sqlite3_bind_blob(st, 2, other->my_keys.public_key, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 3, other->my_ed25519_pubkey, 32, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? dm_start(inst, other->node_id, other->my_keys.public_key,
                                       other->my_ed25519_pubkey, "peer", group, DM_MODE_DEVICE, NULL) : -1;
}

struct device_identity { uint8_t public_key[32], seed[32]; };

static struct UTUN_INSTANCE* start(struct UASYNC* ua, const char* directory, int index, struct device_identity* identity) {
    char config[1536], pub[65], priv[65];
    for (unsigned i = 0; i < 32; i++) {
        snprintf(pub + 2 * i, 3, "%02x", identity->public_key[i]);
        snprintf(priv + 2 * i, 3, "%02x", identity->seed[i]);
    }
    snprintf(config, sizeof(config), "[global]\nmy_public_key=%s\nmy_private_key=%s\ndb_path=%s/%d\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", pub, priv, directory, index);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    if (!inst) return NULL;
    if (utun_core_start(inst) || chat_service_start(inst)) { utun_instance_destroy(inst); return NULL; }
    chat_event_set_handler(inst, event);
    return inst;
}

static int read_body(sqlite3* db, int seq, uint8_t* body, size_t* length) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT body FROM dm_outbox WHERE seq=?", -1, &st, NULL)) return -1;
    sqlite3_bind_int(st, 1, seq);
    int rc = sqlite3_step(st), bytes = rc == SQLITE_ROW ? sqlite3_column_bytes(st, 0) : 0;
    if (bytes > 0 && (size_t)bytes <= *length) {
        memcpy(body, sqlite3_column_blob(st, 0), (size_t)bytes); *length = (size_t)bytes;
    } else rc = SQLITE_ERROR;
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : -1;
}

static int exercise(struct UTUN_INSTANCE* inst, struct UTUN_INSTANCE* other, const char* conv, uint64_t group,
                    enum operation op, enum fault failure, const uint8_t* body, size_t bytes, const char* path) {
    sqlite3* db = inst->topo_sqlite_db;
    if (keys(inst, other, failure)) return -1;
    uint8_t before[32], after[32], receipt[DM_RECEIPT_SIZE], untouched[DM_RECEIPT_SIZE];
    memset(receipt, 0x55, sizeof(receipt)); memcpy(untouched, receipt, sizeof(receipt));
    if (snapshot(db, before)) return -1;
    int changes = sqlite3_total_changes(db), old_events = events, old_invites = invites, old_files = file_failures;
    fault_query = "SELECT peer_x25519, peer_ed25519";
    injections = 0; fault = failure;
    int rc = -1;
    if (op == OP_START) rc = dm_start(inst, other->node_id, other->my_keys.public_key, other->my_ed25519_pubkey,
        "changed", "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16", DM_MODE_DEVICE, NULL);
    else if (op == OP_RECEIVE) rc = dm_accept_message(inst, body, bytes, receipt);
    else if (op == OP_SEND) rc = dm_send(inst, conv, "text", (const uint8_t*)"rejected", 8);
    else if (op == OP_LIST) {
        char json[8192]; size_t length;
        rc = dm_list_messages_json(inst, conv, 10, 0, json, sizeof(json), &length);
    } else if (op == OP_INVITE) {
        struct dm_invite_req* req = u_calloc(1, sizeof(*req));
        if (!req) { fault = FAULT_NONE; return -1; }
        req->inst = inst; req->request_id = 1; memcpy(req->peer_ed, other->my_ed25519_pubkey, 32); strcpy(req->channel_id, topo_groups_channel(inst->topo_groups, group));
        strcpy(req->source_ch_id, topo_groups_channel(inst->topo_groups, group));
        dm_invite_trampoline(req);
        rc = invites == old_invites + 1 && invite_result == DM_INVITE_START_ERROR ? -1 : 0;
    } else {
        struct attachment_info info = { .kind = ATTACHMENT_FILE, .name = "input.bin" };
        rc = dm_send_file(inst, conv, path, &info, 0, 0);
    }
    fault = FAULT_NONE;
    int expected_hits = failure == FAULT_XKEY || failure == FAULT_EDKEY ? 0 : 1;
    int bad_result = op == OP_FILE ? rc != 0 || file_failures != old_files + 1 : rc >= 0;
    if (snapshot(db, after) || bad_result || injections != expected_hits || memcmp(before, after, sizeof(before)) ||
        sqlite3_total_changes(db) != changes || events != old_events + (op == OP_INVITE || op == OP_FILE) ||
        (op == OP_RECEIVE && memcmp(receipt, untouched, sizeof(receipt))) || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM load test: op=%d fault=%d result=%d injections=%d events=%d->%d changes=%d->%d",
                    op, failure, rc, injections, old_events, events, changes, sqlite3_total_changes(db));
        return -1;
    }
    if (keys(inst, other, FAULT_NONE)) return -1;
    checks++;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM load test: preserved op=%d fault=%d check=%u", op, failure, checks);
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_DM, getenv("UTUN_TEST_DEBUG") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_INFO);
    size_t allocations = u_get_allocated_count();
    struct UASYNC* ua = NULL; struct UTUN_INSTANCE* a = NULL; struct UTUN_INSTANCE* b = NULL;
    struct device_identity identities[2];
    char directory[512] = "/tmp/utun_dm_load_XXXXXX", conv[64], group_id[65], input[768];
    uint8_t first[2048], second[2048], receipt[DM_RECEIPT_SIZE];
    size_t first_len = sizeof(first), second_len = sizeof(second);
    int failed = 1;
#define CHECK(condition) do { if (!(condition)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM load test: failed line=%d", __LINE__); goto done; } } while (0)
    CHECK(!test_mkdtemp(directory) && !sc_identity_generate_config(identities[0].public_key, identities[0].seed) && !sc_identity_generate_config(identities[1].public_key, identities[1].seed));
    ua = uasync_create(); CHECK(ua);
    a = start(ua, directory, 0, &identities[0]); b = start(ua, directory, 1, &identities[1]); CHECK(a && b);
    uint64_t group = chat_core_create_channel_auto(b, "load test"); CHECK(group);
    strcpy(group_id, topo_groups_channel(b->topo_groups, group));
    CHECK(!peer(a, b, group_id) && !peer(b, a, group_id));
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)dm_derive_conv_id(a->my_ed25519_pubkey, b->my_ed25519_pubkey));
    CHECK(!dm_send(a, conv, "text", (const uint8_t*)"one", 3) && !dm_send(a, conv, "text", (const uint8_t*)"two", 3));
    CHECK(!read_body(a->topo_sqlite_db, 1, first, &first_len) && !read_body(a->topo_sqlite_db, 2, second, &second_len));
    CHECK(!dm_accept_message(b, second, second_len, receipt) && !dm_send(b, conv, "text", (const uint8_t*)"reply", 5));
    snprintf(input, sizeof(input), "%s/input.bin", directory);
    FILE* file = fopen(input, "wb"); CHECK(file);
    int wrote = fputs("input", file), closed = fclose(file); CHECK(wrote >= 0 && !closed);
    for (int hidden = 0; hidden < 2; hidden++) {
        if (hidden) CHECK(!dm_history_delete(b, conv, 1));
        for (enum operation op = OP_START; op <= OP_INVITE; op++) {
            for (enum fault failure = FAULT_PREPARE; failure <= FAULT_CRYPTO; failure++)
                CHECK(!exercise(b, a, conv, group, op, failure, first, first_len, input));
        }
        if (!hidden) {
            enum fault media_faults[] = {FAULT_PREPARE, FAULT_STEP, FAULT_XKEY, FAULT_CRYPTO};
            for (size_t i = 0; i < sizeof(media_faults) / sizeof(media_faults[0]); i++)
                CHECK(!exercise(b, a, conv, group, OP_FILE, media_faults[i], first, first_len, input));
        }
        CHECK(scalar(b->topo_sqlite_db, "SELECT last_out_seq FROM dm_conversations") == 1);
        CHECK(scalar(b->topo_sqlite_db, "SELECT last_in_seq FROM dm_conversations") == 2);
        CHECK(dm_history_generation(b, conv) == (uint64_t)(hidden + 1));
    }
    utun_instance_destroy(b); b = start(ua, directory, 1, &identities[1]); CHECK(b);
    CHECK(dm_history_generation(b, conv) == 2 && scalar(b->topo_sqlite_db, "SELECT hidden FROM dm_history") == 1);
    CHECK(!dm_start(b, a->node_id, a->my_keys.public_key, a->my_ed25519_pubkey, "peer", group_id, DM_MODE_DEVICE, NULL));
    CHECK(!dm_send(b, conv, "text", (const uint8_t*)"after restart", 13));
    CHECK(scalar(b->topo_sqlite_db, "SELECT last_out_seq FROM dm_conversations") == 2);
    CHECK(!dm_accept_message(b, first, first_len, receipt) && !dm_verify_receipt(a, receipt, first, first_len));
    CHECK(scalar(b->topo_sqlite_db, "SELECT last_in_seq FROM dm_conversations") == 2);
    /* Ошибка SELECT отсутствующей строки тоже не разрешает создание; успешный SELECT разрешает. */
    utun_instance_destroy(b); b = start(ua, directory, 2, &identities[1]); CHECK(b);
    CHECK(scalar(b->topo_sqlite_db, "SELECT count(*) FROM dm_conversations") == 0);
    for (enum fault failure = FAULT_PREPARE; failure <= FAULT_FINALIZE; failure++)
        CHECK(!exercise(b, a, conv, group, OP_START, failure, first, first_len, input));
    CHECK(!exercise(b, a, conv, group, OP_START, FAULT_CRYPTO, first, first_len, input));
    CHECK(!peer(b, a, group_id) && scalar(b->topo_sqlite_db, "SELECT last_out_seq FROM dm_conversations") == 0);
    failed = 0;
done:
    fault = FAULT_NONE;
    if (b) utun_instance_destroy(b);
    if (a) utun_instance_destroy(a);
    if (ua) uasync_destroy(ua, 0);
    if (allocations != u_get_allocated_count()) failed = 1;
    printf("test_dm_load: checks=%u allocations=%zu->%zu %s\n", checks, allocations, u_get_allocated_count(), failed ? "FAIL" : "PASS");
    return failed;
}
