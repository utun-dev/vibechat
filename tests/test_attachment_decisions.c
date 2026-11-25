/* Настоящие core/SQLite/подписанный участник: решение+очередь видны reader до единственного события. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_attachment.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/member_sync.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/etcp_router.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_membership.h"
#include "test_utils.h"

enum operation { NO_OPERATION, AUTHOR, STATE, OUTBOX };
enum failure { NO_FAILURE, BEGIN, PREPARE, BIND, STEP, FINALIZE, CHANGES, COMMIT };
static __thread enum failure fault;
static __thread enum operation fault_operation, last_operation;
static __thread int fault_index, fail_rollback;
static unsigned fault_hits, cases;

struct snapshot {
    uint8_t key[32];
    int key_bytes, error, incoming, state, command, retry, created;
};
struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst;
    sqlite3* reader;
    char directory[256], channel[65];
    uint8_t message[32], key[32];
    uint64_t peer;
    uint8_t peer_ed[32];
    struct snapshot expected;
    int allow_success;
    unsigned events;
};
static __thread struct fixture* active;

static enum operation operation(const char* query) {
    if (!query) return NO_OPERATION;
    static const char* prefixes[] = {NULL, "SELECT 1 FROM msg_", "UPDATE attachment_key_requests SET state=?",
        "INSERT INTO attachment_key_outbox("};
    for (enum operation op = AUTHOR; op <= OUTBOX; op++)
        if (!strncmp(query, prefixes[op], strlen(prefixes[op]))) return op;
    return NO_OPERATION;
}

static int take(enum failure point, enum operation op, int index) {
    if (fault != point || fault_operation != op || (point == BIND && fault_index != index)) return 0;
    fault = NO_FAILURE; fault_hits++;
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "decision test: injected point=%d operation=%d bind=%d", point, op, index); return 1;
}

extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* query, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    if (active && db == active->inst->topo_sqlite_db) {
        if (!strcmp(query, "BEGIN IMMEDIATE") && take(BEGIN, NO_OPERATION, 0)) return SQLITE_BUSY;
        if (!strcmp(query, "COMMIT") && take(COMMIT, NO_OPERATION, 0)) return SQLITE_BUSY;
        if (!strcmp(query, "ROLLBACK") && fail_rollback) {
            fail_rollback = 0; DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "decision test: injected rollback failure"); return SQLITE_IOERR;
        }
    }
    return __real_sqlite3_exec(db, query, cb, arg, error);
}

extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* query, int length, sqlite3_stmt** st, const char** tail) {
    enum operation op = operation(query);
    if (op && take(PREPARE, op, 0)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, query, length, st, tail);
}

extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int length, void (*destroy)(void*)) {
    if (take(BIND, operation(sqlite3_sql(st)), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, value, length, destroy);
}

extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* value, int length, void (*destroy)(void*)) {
    if (take(BIND, operation(sqlite3_sql(st)), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, value, length, destroy);
}

extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (take(BIND, operation(sqlite3_sql(st)), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}

extern int __real_sqlite3_bind_int(sqlite3_stmt*, int, int);
int __wrap_sqlite3_bind_int(sqlite3_stmt* st, int index, int value) {
    if (take(BIND, operation(sqlite3_sql(st)), index)) return SQLITE_NOMEM;
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

static void sql(struct fixture* f, const char* query) {
    int rc = sqlite3_exec(f->inst->topo_sqlite_db, query, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "decision test SQL rc=%d: %s", rc, sqlite3_errmsg(f->inst->topo_sqlite_db));
    assert(rc == SQLITE_OK);
}

static struct snapshot snapshot(struct fixture* f) {
    sqlite3_stmt* st = NULL; struct snapshot result = {0};
    assert(sqlite3_prepare_v2(f->reader, "SELECT a.key,a.error,r.incoming,r.state,coalesce(o.command,0),coalesce(o.retry_at,0),r.created_at"
        " FROM attachment_keys a JOIN attachment_key_requests r ON r.channel=a.channel AND r.message_id=a.message_id"
        " LEFT JOIN attachment_key_outbox o ON o.channel=r.channel AND o.message_id=r.message_id AND o.peer=r.peer"
        " WHERE a.channel=? AND a.message_id=? AND r.peer=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->message, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 3, f->peer_ed, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    result.key_bytes = sqlite3_column_bytes(st, 0); assert(result.key_bytes == 32);
    memcpy(result.key, sqlite3_column_blob(st, 0), 32); result.error = sqlite3_column_int(st, 1);
    result.incoming = sqlite3_column_int(st, 2); result.state = sqlite3_column_int(st, 3);
    result.command = sqlite3_column_int(st, 4); result.retry = sqlite3_column_int(st, 5); result.created = sqlite3_column_int(st, 6);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    return result;
}

static void equal(struct fixture* f, struct snapshot expected) {
    struct snapshot current = snapshot(f);
    assert(current.key_bytes == expected.key_bytes && !memcmp(current.key, expected.key, 32) && current.error == expected.error);
    assert(current.incoming == expected.incoming && current.state == expected.state && current.command == expected.command);
    assert(current.retry == expected.retry && current.created == expected.created);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT key,error FROM attachment_keys WHERE channel='900'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 && sqlite3_column_int(st, 1) == 7);
    uint8_t key[32] = {99}; assert(!memcmp(sqlite3_column_blob(st, 0), key, 32));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    if (type != CHAT_EVT_ATTACHMENT_KEYS_CHANGED) return;
    struct fixture* f = active; assert(f && inst == f->inst && data[0] == strlen(f->channel));
    assert(!memcmp(data + 1, f->channel, strlen(f->channel)));
    if (f->allow_success) assert(bytes == data[0] + 1 && sqlite3_get_autocommit(inst->topo_sqlite_db));
    else assert(bytes == data[0] + 2 && data[bytes - 1] == 0);
    equal(f, f->expected); f->events++;
}

static void decide(struct fixture* f, int approve, int gui, int success) {
    f->expected = snapshot(f); f->allow_success = success; f->events = 0;
    if (success) { f->expected.state = approve ? 2 : 3; f->expected.command = approve ? 3 : 4; f->expected.retry = 0; }
    if (gui) {
        struct chat_attachment_decision* request = u_calloc(1, sizeof(*request)); assert(request);
        request->inst = f->inst; memcpy(request->peer, f->peer_ed, 32); request->approve = approve;
        strcpy(request->channel, f->channel); memcpy(request->message, f->message, 32);
        chat_attachment_decision_trampoline(request);
    } else assert(chat_attachment_decide(f->inst, f->channel, f->message, f->peer_ed, approve) == (success ? 0 : -1));
    equal(f, f->expected); assert(f->events == (unsigned)(gui || success));
}

static void reset(struct fixture* f, int queued) {
    char query[640];
    snprintf(query, sizeof(query), "UPDATE attachment_key_requests SET incoming=1,state=0 WHERE channel='%s';"
        "DELETE FROM attachment_key_outbox WHERE channel='%s'", f->channel, f->channel);
    sql(f, query);
    if (queued) {
        snprintf(query, sizeof(query), "INSERT INTO attachment_key_outbox(channel,message_id,peer,command,retry_at)"
            " SELECT channel,message_id,peer,4,17 FROM attachment_key_requests WHERE channel='%s'", f->channel);
        sql(f, query);
    }
}

static void setup(struct fixture* f) {
    memset(f, 0, sizeof(*f)); strcpy(f->directory, "/tmp/utun-attachment-decision-XXXXXX"); assert(!test_mkdtemp(f->directory));
    char config[1024], query[512];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", f->directory);
    f->ua = uasync_create(); assert(f->ua); f->inst = utun_instance_create_from_str(f->ua, config);
    assert(f->inst && !utun_core_start(f->inst) && !chat_service_start(f->inst));
    chat_core_create_channel_auto(f->inst, "decision contract");
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->inst->topo_sqlite_db, "SELECT channel_id FROM channels WHERE name='decision contract'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); snprintf(f->channel, sizeof(f->channel), "%s", sqlite3_column_text(st, 0));
    assert(sqlite3_finalize(st) == SQLITE_OK);
    uint8_t channel_x[32], channel_ed[32], channel_priv[32], peer_ed[32], peer_seed[64], join[64], certificate[64], body[MEMBER_JOIN_MSG_SIZE];
    struct SC_MYKEYS peer; assert(sc_identity_generate(peer.public_key, peer.private_key, peer_ed, peer_seed) == SC_OK);
    memcpy(f->peer_ed, peer_ed, 32);
    f->peer = sc_derive_node_id_from_pubkey(peer.public_key); assert(f->peer && f->peer != f->inst->node_id);

    assert(topo_node_sqlite_channel_get(f->inst->topo_sqlite_db, f->channel, NULL, 0, NULL, channel_x, channel_ed, NULL) == 0);
    assert(topo_node_sqlite_channel_get_priv(f->inst->topo_sqlite_db, f->channel, channel_priv) == 0);
    assert(member_sync_build_join_msg(channel_x, channel_ed, peer_ed, peer.public_key, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
    assert(sc_ed25519_sign(peer_seed, body, sizeof(body), join) == SC_OK);
    assert(member_sync_sign_join(channel_priv, f->channel, NULL, peer.public_key, peer_ed,
        TEST_JOIN_ID(channel_x, channel_ed, peer_ed, peer.public_key), 1, certificate) == 0);
    assert(member_sync_put(f->inst, f->channel, peer.public_key, peer_ed, join, 1,
        NULL, 0, "", NULL, NULL, 0, 0, certificate) >= 0);
    assert(member_sync_verified_ed25519(f->inst, f->channel, f->peer, peer_ed) == 0);
    struct chat_msg_submit request = {.inst = f->inst, .data = (uint8_t*)"decision identity", .data_len = 17};
    strcpy(request.channel_id, f->channel); strcpy(request.content_type, "text");
    assert(chat_core_submit_message(f->inst, &request) == 0); memcpy(f->message, request.message_id, 32); f->key[0] = 55;
    assert(sqlite3_prepare_v2(f->inst->topo_sqlite_db, "INSERT INTO attachment_keys(channel,crypto_id,message_id,author,key,cipher_path,plain_path)"
        " VALUES(?,zeroblob(16),?,?,?,'','')", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->message, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 3, f->inst->my_ed25519_pubkey, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 4, f->key, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    snprintf(query, sizeof(query), "INSERT INTO attachment_key_requests(channel,message_id,peer,incoming,state,created_at)"
        " SELECT channel,message_id,x'%s',1,0,123 FROM attachment_keys WHERE channel='%s'", sc_public_key_hex(f->peer_ed, (char[65]){0}), f->channel);
    sql(f, query);
    sql(f, "INSERT INTO attachment_keys(channel,crypto_id,author,key,cipher_path,plain_path,error)"
        " VALUES('900',zeroblob(16),CAST(x'99'||zeroblob(31) AS BLOB),x'6300000000000000000000000000000000000000000000000000000000000000','','',7)");
    snprintf(query, sizeof(query), "%s/chats.db", f->directory);
    assert(sqlite3_open_v2(query, &f->reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    active = f; chat_event_set_handler(f->inst, event);
}

static void cleanup(struct fixture* f) {
    chat_event_set_handler(f->inst, NULL); active = NULL; utun_instance_destroy(f->inst);
    assert(sqlite3_close(f->reader) == SQLITE_OK); uasync_destroy(f->ua, 0);
}

static void failed_then_retry(struct fixture* f, int approve, int queued, enum failure point, enum operation op, int index) {
    reset(f, queued); fault = point; fault_operation = op; fault_index = index; unsigned previous = fault_hits;
    decide(f, approve, 0, 0);
    assert(!fault && fault_hits == previous + 1 && sqlite3_get_autocommit(f->inst->topo_sqlite_db));
    decide(f, approve, 0, 1); cases++;
}

static void test_faults(void) {
    struct fixture f; setup(&f);
    for (int approve = 0; approve < 2; approve++) for (int queued = 0; queued < 2; queued++) {
        failed_then_retry(&f, approve, queued, BEGIN, NO_OPERATION, 0);
        for (enum operation op = AUTHOR; op <= OUTBOX; op++) {
            failed_then_retry(&f, approve, queued, PREPARE, op, 0);
            for (int index = 1; index <= (op == AUTHOR ? 3 : 4); index++)
                failed_then_retry(&f, approve, queued, BIND, op, index);
            failed_then_retry(&f, approve, queued, STEP, op, 0);
            failed_then_retry(&f, approve, queued, FINALIZE, op, 0);
            if (op == STATE || op == OUTBOX) failed_then_retry(&f, approve, queued, CHANGES, op, 0);
        }
        failed_then_retry(&f, approve, queued, COMMIT, NO_OPERATION, 0);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment decisions: %u SQL faults/retries, approve/deny, absent/existing queue, exact snapshots", cases);
}

static int veto(void* arg) { (*(unsigned*)arg)++; return 1; }

static void test_controls(void) {
    struct fixture f; setup(&f);
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_decision BEFORE UPDATE OF state ON attachment_key_requests BEGIN SELECT RAISE(ABORT,'state'); END",
        "CREATE TEMP TRIGGER reject_decision BEFORE UPDATE OF state ON attachment_key_requests BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_decision BEFORE INSERT ON attachment_key_outbox BEGIN SELECT RAISE(ABORT,'queue'); END",
        "CREATE TEMP TRIGGER reject_decision BEFORE INSERT ON attachment_key_outbox BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_decision BEFORE UPDATE ON attachment_key_outbox BEGIN SELECT RAISE(ABORT,'upsert'); END",
        "CREATE TEMP TRIGGER reject_decision BEFORE UPDATE ON attachment_key_outbox BEGIN SELECT RAISE(IGNORE); END"
    };
    for (int approve = 0; approve < 2; approve++) {
        reset(&f, 0); unsigned commits = 0; sqlite3_commit_hook(f.inst->topo_sqlite_db, veto, &commits);
        decide(&f, approve, 0, 0); assert(commits == 1); sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL);
        decide(&f, approve, 0, 1);
        for (unsigned i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
            reset(&f, i >= 4); sql(&f, triggers[i]); decide(&f, approve, 1, 0);
            sql(&f, "DROP TRIGGER reject_decision"); decide(&f, approve, 1, 1);
            decide(&f, approve, 1, 0); decide(&f, !approve, 0, 0);
        }
        reset(&f, 1); sql(&f, "BEGIN;UPDATE attachment_keys SET error=8 WHERE channel='900'");
        decide(&f, approve, 0, 0); decide(&f, approve, 1, 0);
        assert(!sqlite3_get_autocommit(f.inst->topo_sqlite_db));
        sqlite3_stmt* st = NULL;
        assert(sqlite3_prepare_v2(f.inst->topo_sqlite_db, "SELECT error FROM attachment_keys WHERE channel='900'", -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 8 && sqlite3_finalize(st) == SQLITE_OK);
        sql(&f, "ROLLBACK"); decide(&f, approve, 1, 1);
        reset(&f, 0); char query[256];
        snprintf(query, sizeof(query), "UPDATE peers_%s SET deleted=1 WHERE node_id=%lld", f.channel, (long long)(sqlite3_int64)f.peer);
        sql(&f, query); assert(!test_member_publish_access(f.inst, f.channel)); decide(&f, approve, 0, 0);
        snprintf(query, sizeof(query), "UPDATE peers_%s SET deleted=0 WHERE node_id=%lld", f.channel, (long long)(sqlite3_int64)f.peer);
        sql(&f, query); assert(!test_member_publish_access(f.inst, f.channel)); decide(&f, approve, 0, 1);
        reset(&f, 0);
        snprintf(query, sizeof(query), "UPDATE attachment_key_requests SET incoming=0 WHERE channel='%s'", f.channel);
        sql(&f, query); decide(&f, approve, 0, 0);
    }
    assert(chat_attachment_decide(NULL, f.channel, f.message, f.peer_ed, 1) == -1);
    assert(chat_attachment_decide(f.inst, f.channel, NULL, f.peer_ed, 1) == -1);
    assert(chat_attachment_decide(f.inst, f.channel, f.message, 0, 1) == -1);
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment decisions: commit veto, ABORT/IGNORE, external transaction, one GUI event, stale decisions and membership");
}

static void test_rollback_failure(void) {
    struct fixture f; setup(&f); fault = COMMIT; fault_operation = NO_OPERATION; fail_rollback = 1;
    decide(&f, 1, 0, 0);
    assert(!fault && !fail_rollback && !f.inst->running && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    assert(!f.inst->router_bindings.callbacks[ETCP_RT_ID_ATTACHMENT_KEYS]); decide(&f, 1, 0, 0);
    chat_event_set_handler(f.inst, NULL); active = NULL; utun_instance_destroy(f.inst);
    equal(&f, f.expected); assert(sqlite3_close(f.reader) == SQLITE_OK); uasync_destroy(f.ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment decisions: rollback failure quiesces/stops core, owner close restores previous state/queue");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0); size_t baseline = u_get_allocated_count();
    test_faults(); test_controls(); test_rollback_failure();
    assert(!fault && !fail_rollback && u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment decisions: committed state+outbox in callback, neighboring key and allocations preserved");
    return 0;
}
