/* После router: настоящая БД/обработчик, SQL-отказы и проверка committed состояния в точке ACK/события. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_attachment.h"
#include "../src/chat/chat_event.h"
#include "../src/routing_layer/etcp_router.h"
#include "../src/transport_layer/etcp.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_membership.h"
#include "test_utils.h"

enum { REQUEST_ACK = 2, GRANT, DENY, RESULT_ACK };
enum operation { NO_OPERATION, READ, KEY, STATE, OUTBOX };
enum failure { NO_FAILURE, BEGIN, PREPARE, BIND, STEP, FINALIZE, CHANGES, COMMIT };
/* Инъекции относятся только к SQL тестового потока; фоновый DB worker не меняет их состояние. */
static __thread enum failure fault;
static __thread enum operation fault_operation, last_operation;
static __thread int fault_index, fail_rollback;
static __thread unsigned fault_hits;
static unsigned cases;

struct snapshot {
    uint8_t key[32];
    int key_bytes, error, incoming, state, queued;
};
struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst;
    struct ETCP_ROUTER_CONN route; /* Наблюдаем только отправку ACK, сетевое покрытие — в test_encrypted_attachment. */
    sqlite3* reader; /* Отдельное readonly-соединение видит только commit. */
    char directory[256], channel[65];
    uint8_t peer_ed[32], id[16], message[32], hash[32], key[32];
    struct snapshot expected;
    unsigned acks, events;
    int allow_result, room;
};
static __thread struct fixture* active;

static enum operation operation(const char* sql) {
    if (!sql) return NO_OPERATION;
    static const char* prefixes[] = {NULL, "SELECT r.incoming,r.state,o.command", "UPDATE attachment_keys SET key=?,error=0 WHERE",
        "UPDATE attachment_key_requests SET state=? WHERE", "DELETE FROM attachment_key_outbox WHERE"};
    for (enum operation op = READ; op <= OUTBOX; op++)
        if (!strncmp(sql, prefixes[op], strlen(prefixes[op]))) return op;
    return NO_OPERATION;
}

static int take(enum failure point, enum operation op, int index) {
    if (fault != point || fault_operation != op || (point == BIND && fault_index != index)) return 0;
    fault = NO_FAILURE; fault_hits++;
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "result test: injected point=%d operation=%d bind=%d", point, op, index); return 1;
}

extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    if (active && active->inst && db == active->inst->topo_sqlite_db) {
        if (!strcmp(sql, "BEGIN IMMEDIATE") && take(BEGIN, NO_OPERATION, 0)) return SQLITE_BUSY;
        if (!strcmp(sql, "COMMIT") && take(COMMIT, NO_OPERATION, 0)) return SQLITE_BUSY;
        if (!strcmp(sql, "ROLLBACK") && fail_rollback) {
            fail_rollback = 0;
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "result test: injected rollback failure"); return SQLITE_IOERR;
        }
    }
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}

extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int length, sqlite3_stmt** st, const char** tail) {
    enum operation op = operation(sql);
    if (op && take(PREPARE, op, 0)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, sql, length, st, tail);
}

extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int length, void (*destroy)(void*)) {
    enum operation op = operation(sqlite3_sql(st));
    if (op && take(BIND, op, index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, value, length, destroy);
}

extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* value, int length, void (*destroy)(void*)) {
    enum operation op = operation(sqlite3_sql(st));
    if (op && take(BIND, op, index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, value, length, destroy);
}

extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    enum operation op = operation(sqlite3_sql(st));
    if (op && take(BIND, op, index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}

extern int __real_sqlite3_bind_int(sqlite3_stmt*, int, int);
int __wrap_sqlite3_bind_int(sqlite3_stmt* st, int index, int value) {
    enum operation op = operation(sqlite3_sql(st));
    if (op && take(BIND, op, index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int(st, index, value);
}

extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    last_operation = operation(sqlite3_sql(st));
    if (last_operation && take(STEP, last_operation, 0)) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}

extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    enum operation op = st ? operation(sqlite3_sql(st)) : NO_OPERATION;
    int failed = op && take(FINALIZE, op, 0);
    int rc = __real_sqlite3_finalize(st); return failed ? SQLITE_IOERR : rc;
}

extern int __real_sqlite3_changes(sqlite3*);
int __wrap_sqlite3_changes(sqlite3* db) {
    if (last_operation && take(CHANGES, last_operation, 0)) return 0;
    return __real_sqlite3_changes(db);
}

static void sql(sqlite3* db, const char* text) {
    int rc = sqlite3_exec(db, text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "result test SQL rc=%d: %s", rc, sqlite3_errmsg(db));
    assert(rc == SQLITE_OK);
}

static struct snapshot snapshot(struct fixture* f) {
    sqlite3_stmt* st = NULL; struct snapshot result = {0};
    assert(sqlite3_prepare_v2(f->reader, "SELECT a.key,a.error,r.incoming,r.state,coalesce(o.command,0)"
        " FROM attachment_keys a JOIN attachment_key_requests r ON r.channel=a.channel AND r.message_id=a.message_id"
        " LEFT JOIN attachment_key_outbox o ON o.channel=r.channel AND o.message_id=r.message_id AND o.peer=r.peer"
        " WHERE a.channel=? AND a.message_id=? AND r.peer=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->message, 32, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_blob(st, 3, f->peer_ed, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    result.key_bytes = sqlite3_column_bytes(st, 0);
    assert(result.key_bytes == 0 || result.key_bytes == 32);
    if (result.key_bytes) memcpy(result.key, sqlite3_column_blob(st, 0), 32);
    result.error = sqlite3_column_int(st, 1); result.incoming = sqlite3_column_int(st, 2);
    result.state = sqlite3_column_int(st, 3); result.queued = sqlite3_column_int(st, 4);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    return result;
}

/* Reader проверяет обе целевые строки и не затронутую соседнюю запись без чтения секретов в лог. */
static void equal(struct fixture* f, struct snapshot expected) {
    struct snapshot current = snapshot(f);
    assert(current.key_bytes == expected.key_bytes && current.error == expected.error && current.incoming == expected.incoming);
    assert(current.state == expected.state && current.queued == expected.queued);
    assert(!current.key_bytes || !memcmp(current.key, expected.key, 32));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT key,error FROM attachment_keys WHERE channel='900'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 && sqlite3_column_int(st, 1) == 7);
    uint8_t key[32] = {99}; assert(!memcmp(sqlite3_column_blob(st, 0), key, 32));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

int __real_etcp_router_peer_reachable(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32], uint8_t);
int __wrap_etcp_router_peer_reachable(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], uint8_t svc) {
    if (active && inst == active->inst && group == active->route.group_id &&
        memcmp(peer, active->peer_ed, 32) == 0 && svc == ETCP_RT_ID_ATTACHMENT_KEYS) return 1;
    return __real_etcp_router_peer_reachable(inst, group, peer, svc);
}

extern struct ETCP_ROUTER_CONN* __real_etcp_router_conn_get_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32], uint8_t);
struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_get_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], uint8_t service) {
    if (active && inst == active->inst && group == active->route.group_id && memcmp(peer, active->peer_ed, 32) == 0 && service == ETCP_RT_ID_ATTACHMENT_KEYS)
        return &active->route;
    return __real_etcp_router_conn_get_key(inst, group, peer, service);
}

extern int __real_etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN*);
int __wrap_etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN* conn) {
    if (active && conn == &active->route) return active->room;
    return __real_etcp_router_conn_send_q_has_room(conn);
}

extern int __real_etcp_router_conn_send(struct ETCP_ROUTER_CONN*, const uint8_t*, size_t, int);
int __wrap_etcp_router_conn_send(struct ETCP_ROUTER_CONN* conn, const uint8_t* data, size_t length, int mode) {
    if (!active || conn != &active->route) return __real_etcp_router_conn_send(conn, data, length, mode);
    assert(active->allow_result && length == 85 && !memcmp(data, "AK01", 4) && data[4] == RESULT_ACK);
    assert(mode == (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) && sqlite3_get_autocommit(active->inst->topo_sqlite_db));
    equal(active, active->expected); active->acks++; return 0;
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int length) {
    if (type != CHAT_EVT_ATTACHMENT_KEYS_CHANGED) return;
    assert(active && active->inst == inst && active->allow_result && sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(length == (int)strlen(active->channel) + 1 && data[0] == strlen(active->channel));
    equal(active, active->expected); active->events++;
}

/* Вход после router с локальной фикстурой; криптографию и реальную доставку проверяет сетевой тест. */
static void receive(struct fixture* f, int command, int allowed) {
    f->allow_result = allowed; f->acks = f->events = 0;
    struct ll_entry* entry = ll_alloc_lldgram(ROUTER_SVC_PAYLOAD_OFF + 117); assert(entry);
    entry->len = ROUTER_SVC_PAYLOAD_OFF + (command == GRANT ? 117 : 85); memset(entry->dgram, 0, entry->len);
    entry->dgram[0] = ETCP_RT_ID_ATTACHMENT_KEYS; entry->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
    memcpy(entry->dgram + ROUTER_SVC_SRC_ED_OFF, f->peer_ed, 32); memcpy(entry->dgram + ROUTER_SVC_DST_ED_OFF, f->inst->my_ed25519_pubkey, 32);
    memcpy(entry->dgram + ROUTER_SVC_GROUP_OFF, &f->route.group_id, 8);
    uint8_t* body = entry->dgram + ROUTER_SVC_PAYLOAD_OFF;
    memcpy(body, "AK01", 4); body[4] = (uint8_t)command;
    memcpy(body + 5, f->message, 32); memcpy(body + 37, f->id, 16); memcpy(body + 53, f->hash, 32);
    if (command == GRANT) memcpy(body + 85, f->key, 32);
    struct ETCP_CONN conn = {.instance = f->inst};
    f->inst->router_bindings.callbacks[ETCP_RT_ID_ATTACHMENT_KEYS](&conn, entry);
    if (allowed) assert(f->events == 1 && f->acks == (unsigned)(f->room && (command == GRANT || command == DENY)));
    else assert(!f->events && !f->acks);
}

static void reset(struct fixture* f, int command) {
    f->room = 1; memset(f->key, 0, sizeof(f->key)); f->key[0] = 55;
    int author = command == RESULT_ACK, state = author ? 2 : command == REQUEST_ACK ? 0 : 1;
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->inst->topo_sqlite_db, "UPDATE attachment_keys SET key=?,error=1,author=? WHERE channel=?", -1, &st, NULL) == SQLITE_OK);
    assert((author ? sqlite3_bind_blob(st, 1, f->key, 32, SQLITE_STATIC) : sqlite3_bind_null(st, 1)) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, author ? f->inst->my_ed25519_pubkey : f->peer_ed, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 3, f->channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_finalize(st) == SQLITE_OK);
    char query[768];
    snprintf(query, sizeof(query), "UPDATE attachment_key_requests SET incoming=%d,state=%d WHERE channel='%s';"
        "DELETE FROM attachment_key_outbox WHERE channel='%s';"
        "INSERT INTO attachment_key_outbox(channel,message_id,peer,command) SELECT channel,message_id,x'%s',%d"
        " FROM attachment_keys WHERE channel='%s'", author, state, f->channel, f->channel, sc_public_key_hex(f->peer_ed, (char[65]){0}), author ? GRANT : 1, f->channel);
    sql(f->inst->topo_sqlite_db, query);
    f->expected = snapshot(f); f->expected.state = command == REQUEST_ACK ? 1 : command == GRANT ? 2 : command == DENY ? 3 : 4;
    f->expected.queued = 0;
    if (command == GRANT) { f->expected.key_bytes = 32; memcpy(f->expected.key, f->key, 32); f->expected.error = 0; }
}

static void setup(struct fixture* f) {
    memset(f, 0, sizeof(*f)); strcpy(f->directory, "/tmp/utun-attachment-result-XXXXXX"); assert(!test_mkdtemp(f->directory));
    char config[1024], path[512];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", f->directory);
    f->ua = uasync_create(); assert(f->ua); f->inst = utun_instance_create_from_str(f->ua, config);
    assert(f->inst && !utun_core_start(f->inst) && !chat_service_start(f->inst));
    chat_core_create_channel_auto(f->inst, "result contract");
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->inst->topo_sqlite_db, "SELECT channel_id FROM channels WHERE name='result contract'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); snprintf(f->channel, sizeof(f->channel), "%s", sqlite3_column_text(st, 0));
    assert(sqlite3_finalize(st) == SQLITE_OK);
    struct SC_MYKEYS peer; uint8_t seed[32], owner_seed[32];
    assert(sc_identity_generate(peer.public_key, peer.private_key, f->peer_ed, seed) == SC_OK);
    assert(!topo_node_sqlite_channel_get_priv(f->inst->topo_sqlite_db, f->channel, owner_seed));
    assert(test_member_seed(f->inst, f->channel, peer.public_key, f->peer_ed, seed, owner_seed, "{}", NULL, NULL) >= 0);
    f->message[0] = 1; f->id[0] = 2; f->hash[0] = 3;
    assert(sqlite3_prepare_v2(f->inst->topo_sqlite_db, "INSERT INTO attachment_keys(channel,crypto_id,message_id,author,cipher_hash,cipher_path,plain_path)"
        " VALUES(?,?,?,?,?,'','')", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->id, 16, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_blob(st, 3, f->message, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 4, f->peer_ed, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_blob(st, 5, f->hash, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    char query[768];
    snprintf(query, sizeof(query), "INSERT INTO attachment_key_requests(channel,message_id,peer,incoming,state,created_at)"
        " SELECT channel,message_id,author,0,1,0 FROM attachment_keys WHERE channel='%s'", f->channel);
    assert(!test_member_publish_access(f->inst, f->channel));
    sql(f->inst->topo_sqlite_db, query);
    sql(f->inst->topo_sqlite_db, "INSERT INTO attachment_keys(channel,crypto_id,author,key,cipher_path,plain_path,error)"
        " VALUES('900',zeroblob(16),CAST(x'99'||zeroblob(31) AS BLOB),x'6300000000000000000000000000000000000000000000000000000000000000','','',7)");
    snprintf(path, sizeof(path), "%s/chats.db", f->directory);
    assert(sqlite3_open_v2(path, &f->reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    f->route.inst = f->inst; f->route.group_id = topo_groups_channel_slot(f->inst->topo_groups, f->channel); memcpy(f->route.remote_ed, f->peer_ed, 32);
    f->route.svc_id = ETCP_RT_ID_ATTACHMENT_KEYS; active = f; chat_event_set_handler(f->inst, event);
}

static void cleanup(struct fixture* f) {
    chat_event_set_handler(f->inst, NULL); utun_instance_destroy(f->inst); f->inst = NULL;
    assert(sqlite3_close(f->reader) == SQLITE_OK); uasync_destroy(f->ua, 0); active = NULL;
}

static void failed_then_retry(struct fixture* f, int command, enum failure point, enum operation op, int index) {
    reset(f, command); struct snapshot before = snapshot(f);
    fault = point; fault_operation = op; fault_index = index;
    unsigned old_hits = fault_hits; receive(f, command, 0);
    assert(fault == NO_FAILURE && fault_hits == old_hits + 1 && sqlite3_get_autocommit(f->inst->topo_sqlite_db));
    equal(f, before); receive(f, command, 1); equal(f, f->expected); cases++;
}

static void test_faults(void) {
    struct fixture f; setup(&f);
    for (int command = REQUEST_ACK; command <= RESULT_ACK; command++) {
        failed_then_retry(&f, command, BEGIN, NO_OPERATION, 0);
        for (enum operation op = READ; op <= OUTBOX; op++) {
            if (op == KEY && command != GRANT) continue;
            failed_then_retry(&f, command, PREPARE, op, 0);
            int bindings = op == READ ? 3 : op == KEY ? 5 : 4;
            for (int i = 1; i <= bindings; i++) failed_then_retry(&f, command, BIND, op, i);
            failed_then_retry(&f, command, STEP, op, 0);
            failed_then_retry(&f, command, FINALIZE, op, 0);
            if (op != READ) failed_then_retry(&f, command, CHANGES, op, 0);
        }
        failed_then_retry(&f, command, COMMIT, NO_OPERATION, 0);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment results: SQL fault/retry cases=%u, exact previous key/state/outbox and neighboring key", cases);
}

static int veto(void* arg) { (*(unsigned*)arg)++; return 1; }

static void test_controls(void) {
    struct fixture f; setup(&f); reset(&f, GRANT); struct snapshot before = snapshot(&f);
    unsigned commits = 0; sqlite3_commit_hook(f.inst->topo_sqlite_db, veto, &commits);
    receive(&f, GRANT, 0); assert(commits == 1); sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL); equal(&f, before);
    receive(&f, GRANT, 1);
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_result BEFORE UPDATE OF key ON attachment_keys BEGIN SELECT RAISE(ABORT,'key failure'); END",
        "CREATE TEMP TRIGGER reject_result BEFORE UPDATE OF key ON attachment_keys BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_result BEFORE UPDATE OF state ON attachment_key_requests BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_result BEFORE DELETE ON attachment_key_outbox BEGIN SELECT RAISE(IGNORE); END"
    };
    for (unsigned i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
        reset(&f, GRANT); before = snapshot(&f); sql(f.inst->topo_sqlite_db, triggers[i]);
        receive(&f, GRANT, 0); equal(&f, before);
        sql(f.inst->topo_sqlite_db, "DROP TRIGGER reject_result"); receive(&f, GRANT, 1);
    }
    reset(&f, GRANT); before = snapshot(&f);
    sql(f.inst->topo_sqlite_db, "BEGIN;UPDATE attachment_keys SET error=8 WHERE channel='900'");
    receive(&f, GRANT, 0); assert(!sqlite3_get_autocommit(f.inst->topo_sqlite_db)); equal(&f, before);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f.inst->topo_sqlite_db, "SELECT error FROM attachment_keys WHERE channel='900'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 8 && sqlite3_finalize(st) == SQLITE_OK);
    sql(f.inst->topo_sqlite_db, "ROLLBACK"); receive(&f, GRANT, 1);
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment results: actual commit veto, key/state/outbox trigger errors/skips, external transaction preserved");
}

static void test_duplicates(void) {
    struct fixture f; setup(&f); reset(&f, GRANT);
    f.room = 0; receive(&f, GRANT, 1); equal(&f, f.expected); f.room = 1;
    receive(&f, GRANT, 1); receive(&f, GRANT, 1);
    struct snapshot before = snapshot(&f); f.key[0] ^= 1; receive(&f, GRANT, 0); equal(&f, before);
    receive(&f, DENY, 0); equal(&f, before);
    receive(&f, REQUEST_ACK, 1); equal(&f, before);
    reset(&f, DENY); receive(&f, DENY, 1); receive(&f, DENY, 1);
    before = snapshot(&f); receive(&f, GRANT, 0); equal(&f, before);
    reset(&f, RESULT_ACK); receive(&f, RESULT_ACK, 1); receive(&f, RESULT_ACK, 1);
    reset(&f, RESULT_ACK);
    char query[768]; snprintf(query, sizeof(query), "UPDATE attachment_key_requests SET state=3 WHERE channel='%s';"
        "UPDATE attachment_key_outbox SET command=4 WHERE channel='%s'", f.channel, f.channel);
    sql(f.inst->topo_sqlite_db, query); f.expected.state = 3;
    receive(&f, RESULT_ACK, 1); receive(&f, RESULT_ACK, 1);
    for (int command = REQUEST_ACK; command <= RESULT_ACK; command++) {
        reset(&f, command);
        snprintf(query, sizeof(query), "UPDATE attachment_key_requests SET state=5 WHERE channel='%s';"
            "DELETE FROM attachment_key_outbox WHERE channel='%s'", f.channel, f.channel);
        sql(f.inst->topo_sqlite_db, query); before = snapshot(&f);
        receive(&f, command, 0); equal(&f, before);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment results: backpressure, duplicate GRANT/DENY/ACK, cancelled request rejects late results");
}

static void test_rollback_failure(void) {
    struct fixture f; setup(&f); reset(&f, GRANT); struct snapshot before = snapshot(&f);
    fault = COMMIT; fault_operation = NO_OPERATION; fail_rollback = 1; receive(&f, GRANT, 0);
    assert(fault == NO_FAILURE && !fail_rollback && !f.inst->running && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    equal(&f, before); assert(!f.inst->router_bindings.callbacks[ETCP_RT_ID_ATTACHMENT_KEYS]);
    chat_event_set_handler(f.inst, NULL); utun_instance_destroy(f.inst); f.inst = NULL;
    equal(&f, before); assert(sqlite3_close(f.reader) == SQLITE_OK); uasync_destroy(f.ua, 0); active = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment results: failed rollback quiesces module, no ACK/event, owner close rolls back result");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0); size_t baseline = u_get_allocated_count();
    test_faults(); test_controls(); test_duplicates(); test_rollback_failure();
    assert(!fault && !fail_rollback && u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment results: independent reader before ACK/events, allocations restored");
    return 0;
}
