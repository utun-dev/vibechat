/* Настоящие core/SQLite/подписи; вход после router, независимый reader в ACK/событии. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/sha.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_attachment.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/attachments/service_priv.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/member_sync.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/etcp_router.h"
#include "../src/transport_layer/etcp.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_membership.h"
#include "test_utils.h"

enum operation { NO_OPERATION, IDENTITY, AUTHOR, READ, INSERT, OUTBOX, PENDING, ELIGIBLE, PUMP_READ, PUMP_RETRY };
enum failure { NO_FAILURE, BEGIN, PREPARE, BIND, STEP, FINALIZE, CHANGES, COMMIT };
static __thread enum failure fault;
static __thread enum operation fault_operation, last_operation;
static __thread int fault_index, fail_rollback;
static unsigned fault_hits, cases;

struct snapshot {
    uint8_t key[32];
    int exists, incoming, state, command, error, key_bytes;
    int64_t created, retry;
};
struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst;
    struct ETCP_ROUTER_CONN route; /* Только наблюдение ACK; сеть/crypto проверяет encrypted_attachment. */
    sqlite3* reader;
    char directory[256], channel[65];
    uint8_t message[32], id[16], hash[32], key[32];
    uint64_t peer;
    uint8_t peer_ed[32];
    struct snapshot expected;
    int64_t started;
    int allow_result, route_up, room, send_rc, corrupt, flags, outgoing, pump_only;
    unsigned acks, events;
};
static __thread struct fixture* active;
static timeout_callback_t attachment_tick;

/* Вызываем зарегистрированный production callback без ожидания секунды; старый таймер отменяем. */
extern void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int delay, void* arg, timeout_callback_t callback, const char* name) {
    if (name && !strcmp(name, "attachment keys")) attachment_tick = callback;
    return __real_uasync_set_timeout(ua, delay, arg, callback, name);
}

static enum operation operation(const char* query) {
    if (!query) return NO_OPERATION;
    static const char* prefixes[] = {NULL, "SELECT crypto_id,cipher_hash,key FROM attachment_keys WHERE",
        "SELECT 1 FROM msg_",
        "SELECT incoming,state FROM attachment_key_requests WHERE", "INSERT INTO attachment_key_requests(",
        "INSERT INTO attachment_key_outbox(", "SELECT channel,message_id,crypto_id,author,key,cipher_hash,cipher_path,plain_path",
        "SELECT 1 FROM attachment_keys a WHERE", "SELECT channel,message_id,peer,command FROM attachment_key_outbox",
        "UPDATE attachment_key_outbox SET retry_at=unixepoch()+5 WHERE"};
    for (enum operation op = IDENTITY; op <= PUMP_RETRY; op++)
        if (!strncmp(query, prefixes[op], strlen(prefixes[op]))) return op;
    return NO_OPERATION;
}

static int take(enum failure point, enum operation op, int index) {
    if (fault != point || fault_operation != op || (point == BIND && fault_index != index)) return 0;
    fault = NO_FAILURE; fault_hits++;
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "request test: injected point=%d operation=%d bind=%d", point, op, index); return 1;
}

extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* query, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    if (active && db == active->inst->topo_sqlite_db) {
        const char* begin = active->pump_only ? "BEGIN IMMEDIATE /* attachment outbox */" : "BEGIN IMMEDIATE";
        const char* commit = active->pump_only ? "COMMIT /* attachment outbox */" : "COMMIT";
        if (!strcmp(query, begin) && take(BEGIN, NO_OPERATION, 0)) return SQLITE_BUSY;
        if (!strcmp(query, commit) && take(COMMIT, NO_OPERATION, 0)) return SQLITE_BUSY;
        if (!strcmp(query, "ROLLBACK") && fail_rollback) {
            fail_rollback = 0; DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "request test: injected rollback failure"); return SQLITE_IOERR;
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
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "request test SQL rc=%d: %s", rc, sqlite3_errmsg(f->inst->topo_sqlite_db));
    assert(rc == SQLITE_OK);
    if (!strncmp(query, "UPDATE peers_", 13)) assert(!test_member_publish_access(f->inst, f->channel));
}

/* Проверяем SQLite timestamps её же часами: libc time() может отставать у границы секунды. */
static int64_t database_time(struct fixture* f) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT unixepoch()", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int64_t now = sqlite3_column_int64(st, 0);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK); return now;
}

static struct snapshot snapshot(struct fixture* f) {
    sqlite3_stmt* st = NULL; struct snapshot result = {0};
    assert(sqlite3_prepare_v2(f->reader, "SELECT a.key,a.error,r.message_id IS NOT NULL,coalesce(r.incoming,-1),coalesce(r.state,-1),"
        "coalesce(r.created_at,0),coalesce(o.command,0),coalesce(o.retry_at,0) FROM attachment_keys a"
        " LEFT JOIN attachment_key_requests r ON r.channel=a.channel AND r.message_id=a.message_id AND r.peer=?3"
        " LEFT JOIN attachment_key_outbox o ON o.channel=a.channel AND o.message_id=a.message_id AND o.peer=?3"
        " WHERE a.channel=?1 AND a.message_id=?2", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->message, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 3, f->peer_ed, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    result.key_bytes = sqlite3_column_bytes(st, 0); assert(result.key_bytes == 0 || result.key_bytes == 32);
    if (result.key_bytes) memcpy(result.key, sqlite3_column_blob(st, 0), 32);
    result.error = sqlite3_column_int(st, 1); result.exists = sqlite3_column_int(st, 2);
    result.incoming = sqlite3_column_int(st, 3); result.state = sqlite3_column_int(st, 4);
    result.created = sqlite3_column_int64(st, 5); result.command = sqlite3_column_int(st, 6); result.retry = sqlite3_column_int64(st, 7);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    return result;
}

static void equal(struct fixture* f) {
    struct snapshot current = snapshot(f);
    if (f->expected.created < 0) {
        int64_t now = database_time(f);
        if (current.created < f->started || current.created > now)
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "request test: created=%lld outside SQLite bounds=%lld..%lld",
                (long long)current.created, (long long)f->started, (long long)now);
        assert(current.created >= f->started && current.created <= now); f->expected.created = current.created;
    }
    assert(current.key_bytes == f->expected.key_bytes && !memcmp(current.key, f->expected.key, 32));
    assert(current.error == f->expected.error && current.exists == f->expected.exists);
    assert(current.incoming == f->expected.incoming && current.state == f->expected.state && current.created == f->expected.created);
    assert(current.command == f->expected.command && current.retry == f->expected.retry);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT key,error FROM attachment_keys WHERE channel='900'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 && sqlite3_column_int(st, 1) == 7);
    uint8_t key[32] = {99}; assert(!memcmp(sqlite3_column_blob(st, 0), key, 32));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

int __real_etcp_router_peer_reachable(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32], uint8_t);
int __wrap_etcp_router_peer_reachable(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], uint8_t svc) {
    if (active && inst == active->inst && group == active->route.group_id &&
        memcmp(peer, active->peer_ed, 32) == 0 && svc == ETCP_RT_ID_ATTACHMENT_KEYS) return active->route_up;
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
int __wrap_etcp_router_conn_send(struct ETCP_ROUTER_CONN* conn, const uint8_t* data, size_t length, int flags) {
    if (!active || conn != &active->route) return __real_etcp_router_conn_send(conn, data, length, flags);
    assert(active->allow_result && length == 85 && !memcmp(data, "AK01", 4) && data[4] == (active->outgoing ? 1 : 2));
    assert(flags == (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) && sqlite3_get_autocommit(active->inst->topo_sqlite_db));
    assert(!memcmp(data + 5, active->message, 32) && !memcmp(data + 37, active->id, 16) && !memcmp(data + 53, active->hash, 32));
    if (active->outgoing) {
        active->expected.retry = snapshot(active).retry;
        assert(active->expected.retry >= active->started + 5 && active->expected.retry <= database_time(active) + 5);
    }
    equal(active); active->acks++; return active->send_rc;
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    if (type != CHAT_EVT_ATTACHMENT_KEYS_CHANGED) return;
    struct fixture* f = active;
    if (f && !f->allow_result) {
        struct snapshot seen = snapshot(f);
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "request test: unexpected event outgoing=%d committed request=%d state=%d command=%d autocommit=%d",
            f->outgoing, seen.exists, seen.state, seen.command, sqlite3_get_autocommit(inst->topo_sqlite_db));
    }
    assert(f && f->inst == inst && f->allow_result && sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(bytes == (int)strlen(f->channel) + 1 && data[0] == strlen(f->channel) && !memcmp(data + 1, f->channel, strlen(f->channel)));
    equal(f); f->events++;
}

static void receive(struct fixture* f, int success) {
    f->expected = snapshot(f); f->allow_result = success; f->acks = f->events = 0; f->started = database_time(f);
    if (success) {
        if (!f->expected.exists || f->expected.state == CHAT_ATTACHMENT_REQUEST_CANCELLED) {
            f->expected.exists = 1; f->expected.incoming = 1; f->expected.state = 0; f->expected.created = -1;
        }
        if (f->expected.state) { f->expected.command = f->expected.state == 3 ? 4 : 3; f->expected.retry = 0; }
    }
    struct ll_entry* entry = ll_alloc_lldgram(ROUTER_SVC_PAYLOAD_OFF + 85); assert(entry);
    entry->len = ROUTER_SVC_PAYLOAD_OFF + 85; memset(entry->dgram, 0, entry->len);
    entry->dgram[0] = ETCP_RT_ID_ATTACHMENT_KEYS; entry->dgram[ROUTER_SVC_FLAGS_OFF] = f->flags;
    memcpy(entry->dgram + ROUTER_SVC_SRC_ED_OFF, f->peer_ed, 32); memcpy(entry->dgram + ROUTER_SVC_DST_ED_OFF, f->inst->my_ed25519_pubkey, 32);
    memcpy(entry->dgram + ROUTER_SVC_GROUP_OFF, &f->route.group_id, 8);
    uint8_t* body = entry->dgram + ROUTER_SVC_PAYLOAD_OFF; memcpy(body, "AK01", 4); body[4] = 1;
    memcpy(body + 5, f->message, 32); memcpy(body + 37, f->id, 16); memcpy(body + 53, f->hash, 32);
    if (f->corrupt) body[53] ^= 1;
    struct ETCP_CONN conn = {.instance = f->inst};
    assert(f->inst->router_bindings.callbacks[ETCP_RT_ID_ATTACHMENT_KEYS]);
    f->inst->router_bindings.callbacks[ETCP_RT_ID_ATTACHMENT_KEYS](&conn, entry);
    equal(f); assert(f->events == (unsigned)success && f->acks == (unsigned)(success && f->route_up && f->room));
}

static void reset(struct fixture* f, int state, int queued) {
    char query[768];
    snprintf(query, sizeof(query), "DELETE FROM attachment_key_requests WHERE channel='%s';DELETE FROM attachment_key_outbox WHERE channel='%s'",
        f->channel, f->channel); sql(f, query);
    if (state >= 0) {
        snprintf(query, sizeof(query), "INSERT INTO attachment_key_requests(channel,message_id,peer,incoming,state,created_at)"
            " SELECT channel,message_id,x'%s',1,%d,123 FROM attachment_keys WHERE channel='%s'",
            sc_public_key_hex(f->peer_ed, (char[65]){0}), state, f->channel); sql(f, query);
    }
    if (queued) {
        snprintf(query, sizeof(query), "INSERT INTO attachment_key_outbox(channel,message_id,peer,command,retry_at)"
            " SELECT channel,message_id,peer,%d,17 FROM attachment_key_requests WHERE channel='%s'", state == 3 ? 4 : 3, f->channel);
        sql(f, query);
    }
    f->route_up = f->room = 1; f->send_rc = f->corrupt = 0; f->flags = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
}

static void setup(struct fixture* f) {
    memset(f, 0, sizeof(*f)); strcpy(f->directory, "/tmp/utun-attachment-request-XXXXXX"); assert(!test_mkdtemp(f->directory));
    char config[1024], path[512];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", f->directory);
    f->ua = uasync_create(); assert(f->ua); f->inst = utun_instance_create_from_str(f->ua, config);
    assert(f->inst && !utun_core_start(f->inst) && !chat_service_start(f->inst));
    chat_core_create_channel_auto(f->inst, "request contract");
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->inst->topo_sqlite_db, "SELECT channel_id FROM channels WHERE name='request contract'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); snprintf(f->channel, sizeof(f->channel), "%s", sqlite3_column_text(st, 0));
    assert(sqlite3_finalize(st) == SQLITE_OK);
    uint8_t x[32], ed[32], private_key[32], peer_ed[32], seed[64], join[64], certificate[64], body[MEMBER_JOIN_MSG_SIZE];
    struct SC_MYKEYS peer; assert(sc_identity_generate(peer.public_key, peer.private_key, peer_ed, seed) == SC_OK);
    memcpy(f->peer_ed, peer_ed, 32);
    f->peer = sc_derive_node_id_from_pubkey(peer.public_key); assert(f->peer && f->peer != f->inst->node_id);

    assert(topo_node_sqlite_channel_get(f->inst->topo_sqlite_db, f->channel, NULL, 0, NULL, x, ed, NULL) == 0);
    assert(topo_node_sqlite_channel_get_priv(f->inst->topo_sqlite_db, f->channel, private_key) == 0);
    assert(member_sync_build_join_msg(x, ed, peer_ed, peer.public_key, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
    assert(sc_ed25519_sign(seed, body, sizeof(body), join) == SC_OK);
    assert(member_sync_sign_join(private_key, f->channel, NULL, peer.public_key, peer_ed, TEST_JOIN_ID(x, ed, peer_ed, peer.public_key), 1, certificate) == 0);
    assert(member_sync_put(f->inst, f->channel, peer.public_key, peer_ed, join, 1,
        NULL, 0, "", NULL, NULL, 0, 0, certificate) >= 0);
    assert(member_sync_verified_ed25519(f->inst, f->channel, f->peer, peer_ed) == 0);
    struct chat_msg_submit request = {.inst = f->inst, .data = (uint8_t*)"request identity", .data_len = 16};
    strcpy(request.channel_id, f->channel); strcpy(request.content_type, "text");
    assert(chat_core_submit_message(f->inst, &request) == 0); memcpy(f->message, request.message_id, 32);
    f->key[0] = 55; f->id[0] = 2; f->hash[0] = 3;
    assert(sqlite3_prepare_v2(f->inst->topo_sqlite_db, "INSERT INTO attachment_keys(channel,crypto_id,message_id,author,key,cipher_hash,cipher_path,plain_path)"
        " VALUES(?,?,?,?,?,?,'','')", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->id, 16, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_blob(st, 3, f->message, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 4, f->inst->my_ed25519_pubkey, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 5, f->key, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_blob(st, 6, f->hash, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    sql(f, "INSERT INTO attachment_keys(channel,crypto_id,author,key,cipher_path,plain_path,error)"
        " VALUES('900',zeroblob(16),CAST(x'99'||zeroblob(31) AS BLOB),x'6300000000000000000000000000000000000000000000000000000000000000','','',7)");
    snprintf(path, sizeof(path), "%s/chats.db", f->directory);
    assert(sqlite3_open_v2(path, &f->reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    f->route.inst = f->inst; f->route.group_id = topo_groups_channel_slot(f->inst->topo_groups, f->channel); f->route.remote_node_id = f->peer;
    f->route.svc_id = ETCP_RT_ID_ATTACHMENT_KEYS; active = f; chat_event_set_handler(f->inst, event); reset(f, -1, 0);
}

static void cleanup(struct fixture* f) {
    chat_event_set_handler(f->inst, NULL); active = NULL; utun_instance_destroy(f->inst);
    assert(sqlite3_close(f->reader) == SQLITE_OK); uasync_destroy(f->ua, 0);
}

static void failed_then_retry(struct fixture* f, int state, int queued, enum failure point, enum operation op, int index) {
    reset(f, state, queued); fault = point; fault_operation = op; fault_index = index; unsigned previous = fault_hits;
    receive(f, 0); assert(!fault && fault_hits == previous + 1 && sqlite3_get_autocommit(f->inst->topo_sqlite_db));
    receive(f, 1); receive(f, 1); cases++;
}

static void test_faults(void) {
    struct fixture f; setup(&f); const int states[] = {-1, 0, 2, 3, 4};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) for (int queued = 0; queued <= (states[i] >= 2); queued++) {
        int state = states[i]; failed_then_retry(&f, state, queued, BEGIN, NO_OPERATION, 0);
        for (enum operation op = IDENTITY; op <= OUTBOX; op++) {
            if ((op == INSERT && state >= 0) || (op == OUTBOX && state < 2)) continue;
            failed_then_retry(&f, state, queued, PREPARE, op, 0);
            int bindings = op == IDENTITY ? 2 : op == OUTBOX ? 4 : 3;
            for (int index = 1; index <= bindings; index++) failed_then_retry(&f, state, queued, BIND, op, index);
            failed_then_retry(&f, state, queued, STEP, op, 0); failed_then_retry(&f, state, queued, FINALIZE, op, 0);
            if (op == INSERT || op == OUTBOX) failed_then_retry(&f, state, queued, CHANGES, op, 0);
        }
        failed_then_retry(&f, state, queued, COMMIT, NO_OPERATION, 0);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment requests: %u SQL faults/retries, new and duplicate 0/2/3/4, exact committed snapshots", cases);
}

static int veto(void* arg) { (*(unsigned*)arg)++; return 1; }

static void test_controls(void) {
    struct fixture f; setup(&f); const int states[] = {-1, 0, 2, 3, 4};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        reset(&f, states[i], 0); sql(&f, "BEGIN;UPDATE attachment_keys SET error=8 WHERE channel='900'");
        receive(&f, 0); assert(!sqlite3_get_autocommit(f.inst->topo_sqlite_db));
        sqlite3_stmt* st = NULL;
        assert(sqlite3_prepare_v2(f.inst->topo_sqlite_db, "SELECT error FROM attachment_keys WHERE channel='900'", -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 8 && sqlite3_finalize(st) == SQLITE_OK);
        sql(&f, "ROLLBACK"); receive(&f, 1);
    }
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_request BEFORE INSERT ON attachment_key_requests BEGIN SELECT RAISE(ABORT,'request'); END",
        "CREATE TEMP TRIGGER reject_request BEFORE INSERT ON attachment_key_requests BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_request BEFORE INSERT ON attachment_key_outbox BEGIN SELECT RAISE(ABORT,'queue'); END",
        "CREATE TEMP TRIGGER reject_request BEFORE INSERT ON attachment_key_outbox BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_request BEFORE UPDATE ON attachment_key_outbox BEGIN SELECT RAISE(ABORT,'upsert'); END",
        "CREATE TEMP TRIGGER reject_request BEFORE UPDATE ON attachment_key_outbox BEGIN SELECT RAISE(IGNORE); END"
    };
    for (unsigned i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
        reset(&f, i < 2 ? -1 : 2, i >= 4); sql(&f, triggers[i]); receive(&f, 0);
        sql(&f, "DROP TRIGGER reject_request"); receive(&f, 1);
    }
    for (int state = -1; state <= 2; state += 3) {
        reset(&f, state, 0); unsigned commits = 0; sqlite3_commit_hook(f.inst->topo_sqlite_db, veto, &commits);
        receive(&f, 0); assert(commits == 1); sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL); receive(&f, 1);
    }
    for (int state = 1; state <= 6; state += 5) { reset(&f, state, 1); receive(&f, 0); }
    reset(&f, 0, 0); char query[256];
    snprintf(query, sizeof(query), "UPDATE attachment_key_requests SET incoming=0 WHERE channel='%s'", f.channel);
    sql(&f, query); receive(&f, 0);
    reset(&f, -1, 0); f.corrupt = 1; receive(&f, 0); f.corrupt = 0;
    f.flags = ROUTER_FLAG_SIGNED; receive(&f, 0); f.flags |= ROUTER_FLAG_ENCRYPTED;
    snprintf(query, sizeof(query), "UPDATE peers_%s SET deleted=1 WHERE node_id=%lld", f.channel, (long long)(sqlite3_int64)f.peer);
    sql(&f, query); assert(!test_member_publish_access(f.inst, f.channel)); receive(&f, 0);
    snprintf(query, sizeof(query), "UPDATE peers_%s SET deleted=0 WHERE node_id=%lld", f.channel, (long long)(sqlite3_int64)f.peer);
    sql(&f, query); assert(!test_member_publish_access(f.inst, f.channel)); receive(&f, 1); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment requests: foreign transaction preserved, ABORT/IGNORE, commit veto, direction/state/identity/member checks");
}

static void test_ack_retry(void) {
    struct fixture f; setup(&f);
    for (int unavailable = 0; unavailable < 3; unavailable++) {
        reset(&f, -1, 0);
        if (unavailable == 0) f.route_up = 0;
        else if (unavailable == 1) f.room = 0;
        else f.send_rc = -1;
        receive(&f, 1); struct snapshot saved = snapshot(&f);
        f.route_up = f.room = 1; f.send_rc = 0; receive(&f, 1);
        assert(saved.created == f.expected.created && f.expected.command == 0);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment requests: missing route, backpressure and send failure retain request; duplicate retries ACK without new consent");
}

static void test_rollback_failure(void) {
    struct fixture f; setup(&f); fault = COMMIT; fault_operation = NO_OPERATION; fail_rollback = 1; receive(&f, 0);
    assert(!fault && !fail_rollback && !f.inst->running && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    assert(!f.inst->router_bindings.callbacks[ETCP_RT_ID_ATTACHMENT_KEYS]);
    chat_event_set_handler(f.inst, NULL); active = NULL; utun_instance_destroy(f.inst);
    equal(&f); assert(sqlite3_close(f.reader) == SQLITE_OK); uasync_destroy(f.ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment requests: failed rollback sends no ACK/event, quiesces/stops core, owner close rolls back request");
}

/* Получатель уже имеет проверенный ciphertext; именно эти локальные строки потребляет таймер. */
static void outgoing_reset(struct fixture* f, int queued) {
    reset(f, -1, 0); f->outgoing = 1;
    char query[768];
    snprintf(query, sizeof(query), "UPDATE attachment_keys SET author=x'%s',key=NULL,downloaded=1,wanted=1,ready=0,decrypting=0,error=0"
        " WHERE channel='%s'", sc_public_key_hex(f->peer_ed, (char[65]){0}), f->channel); sql(f, query);
    if (queued) {
        snprintf(query, sizeof(query), "INSERT INTO attachment_key_outbox(channel,message_id,peer,command,retry_at)"
            " SELECT channel,message_id,author,4,9223372036854775807 FROM attachment_keys WHERE channel='%s'", f->channel); sql(f, query);
    }
}

static void outgoing_tick(struct fixture* f, int create, int sends) {
    f->expected = snapshot(f); f->allow_result = create || sends; f->acks = f->events = 0; f->started = database_time(f);
    if (create) {
        f->expected.exists = 1; f->expected.incoming = 0; f->expected.state = 0;
        f->expected.created = -1; f->expected.command = 1; f->expected.retry = 0;
    }
    assert(attachment_tick && ATTACHMENTS(f->inst)->attachment_timer);
    uasync_cancel_timeout(f->ua, ATTACHMENTS(f->inst)->attachment_timer); attachment_tick(f->inst);
    if (sends || (create && (!f->route_up || !f->room))) {
        f->expected.retry = snapshot(f).retry;
        assert(f->expected.retry >= f->started + 5 && f->expected.retry <= database_time(f) + 5);
    }
    equal(f); assert(f->events == (unsigned)create && f->acks == (unsigned)sends);
}

static void test_outgoing_controls(void) {
    struct fixture f; setup(&f); outgoing_reset(&f, 0);
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_request BEFORE INSERT ON attachment_key_requests BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_request BEFORE INSERT ON attachment_key_requests BEGIN SELECT RAISE(ABORT,'request'); END",
        "CREATE TEMP TRIGGER reject_request BEFORE INSERT ON attachment_key_outbox BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_request BEFORE INSERT ON attachment_key_outbox BEGIN SELECT RAISE(ABORT,'outbox'); END",
        "CREATE TEMP TRIGGER reject_request BEFORE UPDATE ON attachment_key_outbox BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_request BEFORE UPDATE ON attachment_key_outbox BEGIN SELECT RAISE(ABORT,'upsert'); END"
    };
    for (unsigned i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
        outgoing_reset(&f, i >= 4); sql(&f, triggers[i]); outgoing_tick(&f, 0, 0);
        sql(&f, "DROP TRIGGER reject_request"); outgoing_tick(&f, 1, 1); outgoing_tick(&f, 0, 0);
    }
    outgoing_reset(&f, 0); sql(&f, "BEGIN;UPDATE attachment_keys SET error=8 WHERE channel='900'");
    outgoing_tick(&f, 0, 0); assert(!sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    sql(&f, "ROLLBACK"); outgoing_tick(&f, 1, 1);
    /* Уже сохранённая очередь во внешней транзакции также не уходит в сеть. */
    sql(&f, "UPDATE attachment_key_outbox SET retry_at=0;BEGIN;UPDATE attachment_keys SET error=8 WHERE channel='900'");
    outgoing_tick(&f, 0, 0); assert(!sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    sql(&f, "ROLLBACK"); outgoing_tick(&f, 0, 1);
    outgoing_reset(&f, 0); unsigned commits = 0; sqlite3_commit_hook(f.inst->topo_sqlite_db, veto, &commits);
    outgoing_tick(&f, 0, 0); assert(commits == 1); sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL);
    outgoing_tick(&f, 1, 1); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS outgoing requests: ABORT/IGNORE, commit veto, foreign transaction postpones creation and send");
}

static void test_outgoing_faults(void) {
    struct fixture f; setup(&f); unsigned count = 0;
    for (int queued = 0; queued < 2; queued++) {
        const enum operation operations[] = {NO_OPERATION, PENDING, ELIGIBLE, INSERT, OUTBOX, NO_OPERATION};
        for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
            enum operation op = operations[i];
            int binds = op == ELIGIBLE || op == INSERT ? 3 : op == OUTBOX ? 4 : 0;
            int writes = op == INSERT || op == OUTBOX;
            int points = op ? 3 + binds + writes : 1;
            for (int point = 0; point < points; point++) {
                outgoing_reset(&f, queued);
                fault = !op ? (i ? COMMIT : BEGIN) : point == 0 ? PREPARE : point <= binds ? BIND :
                    point == binds + 1 ? STEP : point == binds + 2 ? FINALIZE : CHANGES;
                fault_operation = op; fault_index = point; unsigned previous = fault_hits;
                outgoing_tick(&f, 0, 0); assert(!fault && fault_hits == previous + 1 && sqlite3_get_autocommit(f.inst->topo_sqlite_db));
                outgoing_tick(&f, 1, 1); outgoing_tick(&f, 0, 0); count++;
            }
        }
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS outgoing requests: %u SQL faults/retries, both outbox paths, exact committed snapshots", count);
}

static void test_outgoing_rollback_failure(void) {
    struct fixture f; setup(&f); outgoing_reset(&f, 0);
    fault = COMMIT; fault_operation = NO_OPERATION; fail_rollback = 1; outgoing_tick(&f, 0, 0);
    assert(!fault && !fail_rollback && !f.inst->running && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    assert(!ATTACHMENTS(f.inst)->attachment_timer && !f.inst->router_bindings.callbacks[ETCP_RT_ID_ATTACHMENT_KEYS]);
    chat_event_set_handler(f.inst, NULL); active = NULL; utun_instance_destroy(f.inst);
    equal(&f); assert(sqlite3_close(f.reader) == SQLITE_OK); uasync_destroy(f.ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS outgoing requests: rollback failure stops core without event/send or rearming timer");
}

static void test_outgoing_restart(void) {
    struct fixture f; setup(&f);
    for (int unavailable = 0; unavailable < 3; unavailable++) {
        outgoing_reset(&f, 0);
        if (!unavailable) f.route_up = 0;
        else if (unavailable == 1) f.room = 0;
        else f.send_rc = -1;
        outgoing_tick(&f, 1, unavailable == 2);
        struct snapshot saved = snapshot(&f);
        uint64_t previous_node = f.inst->node_id;
        char config[1536];
        snprintf(config, sizeof(config), "[global]\ndb_path=%s\nmy_public_key=%s\nmy_private_key=%s\n[ntp]\nenabled=no\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", f.directory,
            f.inst->config->global.my_public_key_hex, f.inst->config->global.my_private_key_hex);
        chat_event_set_handler(f.inst, NULL); active = NULL; utun_instance_destroy(f.inst);
        f.inst = utun_instance_create_from_str(f.ua, config);
        assert(f.inst && !utun_core_start(f.inst) && !chat_service_start(f.inst));
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "restart test: self=%016llx previous=%016llx admitted self=%d peer=%d",
            (unsigned long long)f.inst->node_id, (unsigned long long)previous_node,
            topo_node_sqlite_member_allowed(f.inst->topo_sqlite_db, f.channel, f.inst->node_id),
            topo_node_sqlite_member_allowed(f.inst->topo_sqlite_db, f.channel, f.peer));
        assert(f.inst->node_id == previous_node && topo_node_sqlite_member_allowed(f.inst->topo_sqlite_db, f.channel, previous_node));
        f.route.inst = f.inst; f.route_up = f.room = 1; f.send_rc = 0; active = &f;
        chat_event_set_handler(f.inst, event);
        struct snapshot restored = snapshot(&f);
        assert(restored.exists && restored.incoming == 0 && restored.state == 0 && restored.command == 1 && restored.retry == 0);
        assert(restored.created == saved.created); outgoing_tick(&f, 0, 1);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS outgoing requests: no route/backpressure/send error preserve queue and date across real instance restart");
}

static void pump_reset(struct fixture* f) {
    outgoing_reset(f, 0); f->pump_only = 1;
    char query[768];
    snprintf(query, sizeof(query), "INSERT INTO attachment_key_requests(channel,message_id,peer,incoming,state,created_at)"
        " SELECT channel,message_id,author,0,0,123 FROM attachment_keys WHERE channel='%s';"
        " INSERT INTO attachment_key_outbox(channel,message_id,peer,command,retry_at)"
        " SELECT channel,message_id,author,1,0 FROM attachment_keys WHERE channel='%s'", f->channel, f->channel); sql(f, query);
}

static void test_pump(void) {
    struct fixture f; setup(&f); unsigned count = 0;
    const enum operation operations[] = {NO_OPERATION, PUMP_READ, PUMP_RETRY, NO_OPERATION};
    for (unsigned op = 0; op < sizeof(operations) / sizeof(operations[0]); op++) {
        int bindings = operations[op] == PUMP_RETRY ? 3 : 0, points = operations[op] ? 3 + bindings + (bindings != 0) : 1;
        for (int point = 0; point < points; point++) {
            pump_reset(&f); fault_operation = operations[op]; fault_index = point;
            fault = !operations[op] ? (op ? COMMIT : BEGIN) : point == 0 ? PREPARE : point <= bindings ? BIND :
                point == bindings + 1 ? STEP : point == bindings + 2 ? FINALIZE : CHANGES;
            unsigned previous = fault_hits; outgoing_tick(&f, 0, 0);
            assert(!fault && fault_hits == previous + 1 && sqlite3_get_autocommit(f.inst->topo_sqlite_db));
            outgoing_tick(&f, 0, 1); outgoing_tick(&f, 0, 0); count++;
        }
    }
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_retry BEFORE UPDATE ON attachment_key_outbox BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_retry BEFORE UPDATE ON attachment_key_outbox BEGIN SELECT RAISE(ABORT,'retry'); END"
    };
    for (unsigned i = 0; i < 2; i++) {
        pump_reset(&f); sql(&f, triggers[i]); outgoing_tick(&f, 0, 0);
        sql(&f, "DROP TRIGGER reject_retry"); outgoing_tick(&f, 0, 1);
    }
    pump_reset(&f); unsigned commits = 0; sqlite3_commit_hook(f.inst->topo_sqlite_db, veto, &commits);
    outgoing_tick(&f, 0, 0); assert(commits == 1); sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL); outgoing_tick(&f, 0, 1);
    pump_reset(&f); fault = COMMIT; fault_operation = NO_OPERATION; fail_rollback = 1; outgoing_tick(&f, 0, 0);
    assert(!fault && !fail_rollback && !f.inst->running && !ATTACHMENTS(f.inst)->attachment_timer && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    chat_event_set_handler(f.inst, NULL); active = NULL; utun_instance_destroy(f.inst); equal(&f);
    assert(sqlite3_close(f.reader) == SQLITE_OK); uasync_destroy(f.ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS outbox:%u SQL faults, ABORT/IGNORE/veto/rollback failure; no send before retry commit, exact queue/date preserved", count);
}

/* Настоящий durable отзыв: callback видит commit, поздний REQUEST требует нового согласия. */
static void test_revocation(void) {
    struct fixture f; setup(&f); char query[256];
    const int states[] = {0, 2, 3, 4};
    for (unsigned i = 0; i < sizeof(states) / sizeof(*states); i++) {
        reset(&f, states[i], states[i] != 0);
        uint64_t revoked = i == 1 ? f.inst->node_id : f.peer;
        snprintf(query, sizeof(query), "UPDATE peers_%s SET admitted=0 WHERE node_id=%lld", f.channel, (long long)revoked); sql(&f, query);
        f.expected = snapshot(&f); f.expected.command = 0; f.expected.retry = 0;
        if (states[i] == 0 || states[i] == 2) f.expected.state = CHAT_ATTACHMENT_REQUEST_CANCELLED;
        f.allow_result = 1; f.events = f.acks = 0;
        assert(!chat_attachment_revalidate_members(f.inst, f.route.group_id)); equal(&f);
        assert(f.events == 1 && !f.acks && f.expected.key_bytes == 32);
        assert(!chat_attachment_revalidate_members(f.inst, 0) && f.events == 1);
        receive(&f, 0);
        snprintf(query, sizeof(query), "UPDATE peers_%s SET admitted=1 WHERE node_id=%lld", f.channel, (long long)revoked); sql(&f, query);
        if (f.expected.state == CHAT_ATTACHMENT_REQUEST_CANCELLED) { receive(&f, 1); assert(snapshot(&f).state == 0 && !snapshot(&f).command); }
    }
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_revoke BEFORE UPDATE OF wanted ON attachment_keys BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_revoke BEFORE UPDATE OF state ON attachment_key_requests BEGIN SELECT RAISE(ABORT,'revoke'); END",
        "CREATE TEMP TRIGGER reject_revoke BEFORE DELETE ON attachment_key_outbox BEGIN SELECT RAISE(IGNORE); END"
    };
    for (unsigned i = 0; i < 4; i++) {
        outgoing_reset(&f, 0); outgoing_tick(&f, 1, 1);
        snprintf(query, sizeof(query), "UPDATE peers_%s SET admitted=0 WHERE node_id=%lld", f.channel, (long long)f.peer); sql(&f, query);
        f.expected = snapshot(&f); f.allow_result = 0; f.events = f.acks = 0;
        /* Никаких частичных изменений и событий при ошибке транзакции. */
        unsigned commits = 0;
        if (i < 3) sql(&f, triggers[i]); else sqlite3_commit_hook(f.inst->topo_sqlite_db, veto, &commits);
        assert(chat_attachment_revalidate_members(f.inst, 0) == -1); equal(&f); assert(!f.events && !f.acks);
        if (i < 3) sql(&f, "DROP TRIGGER reject_revoke");
        else { sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL); assert(commits == 1); }
        sql(&f, "BEGIN"); assert(chat_attachment_revalidate_members(f.inst, 0) == -1); sql(&f, "ROLLBACK"); equal(&f);
        f.expected.state = CHAT_ATTACHMENT_REQUEST_CANCELLED; f.expected.command = 0; f.expected.retry = 0; f.allow_result = 1;
        assert(!chat_attachment_revalidate_members(f.inst, 0)); equal(&f); assert(f.events == 1 && !f.acks);
        snprintf(query, sizeof(query), "UPDATE peers_%s SET admitted=1 WHERE node_id=%lld", f.channel, (long long)f.peer); sql(&f, query);
        outgoing_tick(&f, 0, 0); /* Readmission сам по себе не возобновляет отменённый запрос. */
        assert(!chat_attachment_downloaded(f.inst, f.channel, f.message, 0)); outgoing_tick(&f, 0, 0);
        assert(!chat_attachment_downloaded(f.inst, f.channel, f.message, 1)); outgoing_tick(&f, 0, 0);
        assert(!chat_attachment_want(f.inst, f.channel, f.message)); outgoing_tick(&f, 1, 1);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment revoke: peer/self, retained keys/decisions, atomic rollback, no automatic restart, new request/consent");
}

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0); size_t baseline = u_get_allocated_count();
    if (argc != 2 || strcmp(argv[1], "--outgoing")) { test_faults(); test_controls(); test_ack_retry(); test_rollback_failure(); }
    test_outgoing_controls(); test_outgoing_faults(); test_outgoing_restart(); test_outgoing_rollback_failure();
    test_pump(); test_revocation();
    assert(!fault && !fail_rollback && u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment requests: reader before ACK/events, stable duplicate date, neighboring key and allocations preserved");
    return 0;
}
