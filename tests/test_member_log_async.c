/* Пример общего API: две файловые БД, свои workers, подписи, текущая модель и хвост с101. */
#include "../lib/db_async.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/group/member_log.h"
#include "../src/db_schema.h"
#include "../src/db_storage.h"
#include "test_utils.h"
#include <openssl/evp.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GROUP_KEY "8e50f75f9ab2d7b8bf0e997e8a7d6202689933461e7f4912a4f343ae76ee8de2"

struct node {
    struct UASYNC* ua;
    struct db_async* db;
    char directory[512], path[600];
    int opened;
};
struct example {
    struct node sender, receiver;
    uint8_t private[32], public[32];
    char text[256];
    uint8_t wire[MEMBER_LOG_WIRE_MAX];
    size_t wire_size;
    uint64_t seq;
    int finished, published, forward;
};
static void opened(void* arg, const struct db_async_result* result) {
    struct node* n = arg; assert(!result->status && !n->opened); n->opened = 1;
}
static void poll_until(struct UASYNC* ua, int* flag) {
    uint64_t deadline = get_time_us() + 5000000;
    while (!*flag && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(*flag);
}
static void open_node(struct node* n, struct UASYNC* ua, int fresh) {
    n->ua = ua; n->opened = 0;
    if (fresh) { snprintf(n->directory, sizeof(n->directory), "/tmp/utun-log-async-XXXXXX"); assert(!test_mkdtemp(n->directory)); }
    snprintf(n->path, sizeof(n->path), "%s/chats.db", n->directory);
    n->db = db_async_create(ua, n->path, 8, opened, n); assert(n->db); poll_until(ua, &n->opened);
}
static int schema(sqlite3* db, void* arg) {
    (void)arg;
    int rc = db_schema_table_ex(db, "channels", NULL);
    if (rc) return rc;
    rc = sqlite3_exec(db, "INSERT INTO channels(channel_id,description_json,x25519_pubkey,ed25519_pubkey,signature)"
        " VALUES('" GROUP_KEY "','{\"name\":\"journal\"}',zeroblob(32),X'" GROUP_KEY "',zeroblob(64));"
        " CREATE TABLE demo_cards(id BLOB PRIMARY KEY,body BLOB NOT NULL)", NULL, NULL, NULL);
    if (rc) return rc;
    return db_schema_table(db, "member_faults", NULL) || db_schema_table(db, "member_chains", NULL) || db_schema_table(db, "member_log", NULL) ? SQLITE_ERROR : SQLITE_OK;
}
static void finished(void* arg, const struct db_async_result* result) { assert(!result->status); *(int*)arg = 1; }

/* Учебная проекция: текущий текст одной карточки, без обхода истории.
 * В модели группы callback также проверяет предметные права по SQLite. */
static int apply(sqlite3* db, const struct member_log_record* r) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO demo_cards VALUES(?1,?2) ON CONFLICT(id) DO UPDATE SET body=excluded.body", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, r->scope, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, r->body, (int)r->body_size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); return rc == SQLITE_DONE && !final ? 1 : -1;
}
static int apply_callback(void* db, const struct member_log_record* r) { return apply(db, r); }
static int append(sqlite3* db, void* arg) {
    struct example* e = arg; uint8_t scope[32] = {1}, grant[32] = {2}; struct member_log_record r;
    if (member_log_make(db, GROUP_KEY, e->public, 1000 + e->seq, scope, grant, (const uint8_t*)e->text, strlen(e->text), e->private, &r))
        return SQLITE_ERROR;
    if (member_log_accept(db, &r, e->public, apply_callback, db) != 1 || r.seq != e->seq) return SQLITE_ERROR;
    e->wire_size = sizeof(e->wire);
    return member_log_encode(&r, e->wire, &e->wire_size) ? SQLITE_ERROR : SQLITE_OK;
}
static int receive_one(sqlite3* db, void* arg) {
    struct example* e = arg;
    return member_log_accept_wire(db, GROUP_KEY, e->public, e->wire, e->wire_size, e->public, apply_callback, db) == 1 ? SQLITE_OK : SQLITE_ERROR;
}
static void received_one(void* arg, const struct db_async_result* result) {
    struct example* e = arg; assert(!result->status); e->finished = 1;
}
/* Это точка передачи в сеть: только после подтверждённого commit, вне worker. */
static void appended(void* arg, const struct db_async_result* result) {
    struct example* e = arg; assert(!result->status); e->published++;
    if (e->forward) db_async_submit(e->receiver.db, "accept-block", DB_ASYNC_WRITE, receive_one, e, received_one);
    else e->finished = 1;
}

struct transfer {
    struct example* example;
    uint8_t page[700];
    size_t size;
    uint64_t from, next;
    int more, finished, requests;
};
static int tail(sqlite3* db, void* arg) {
    struct transfer* t = arg; t->size = sizeof(t->page);
    return member_log_tail(db, GROUP_KEY, t->example->public, t->from, t->page, &t->size, &t->next, &t->more) ? SQLITE_ERROR : SQLITE_OK;
}
static int accept_tail(sqlite3* db, void* arg) {
    struct transfer* t = arg; size_t offset = 0;
    while (offset < t->size) {
        if (t->size - offset < 2) return SQLITE_ERROR;
        size_t bytes = (size_t)t->page[offset] * 256 + t->page[offset + 1]; offset += 2;
        if (bytes > t->size - offset || member_log_accept_wire(db, GROUP_KEY, t->example->public,
            t->page + offset, bytes, t->example->public, apply_callback, db) != 1) return SQLITE_ERROR;
        offset += bytes;
    }
    return SQLITE_OK;
}
static void send_tail(void* arg, const struct db_async_result* result);
static void received_tail(void* arg, const struct db_async_result* result) {
    struct transfer* t = arg; assert(!result->status);
    if (t->more) {
        t->from = t->next; t->requests++;
        db_async_submit(t->example->sender.db, "tail-from-next", DB_ASYNC_READ, tail, t, send_tail);
    } else t->finished = 1;
}
static void send_tail(void* arg, const struct db_async_result* result) {
    struct transfer* t = arg; assert(!result->status && t->size);
    db_async_submit(t->example->receiver.db, "accept-tail", DB_ASYNC_WRITE, accept_tail, t, received_tail);
}
struct check { struct example* example; uint64_t seq; int finished; };
static int inspect(sqlite3* db, void* arg) {
    struct check* c = arg; struct member_log_head head;
    if (member_log_head(db, GROUP_KEY, c->example->public, &head) || head.seq != c->seq || head.blocked) return SQLITE_ERROR;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT count(*),(SELECT count(*) FROM demo_cards),"
        "(SELECT body FROM demo_cards LIMIT 1) FROM member_log", -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    char expected[256]; snprintf(expected, sizeof(expected), "{\"text\":\"Карточка, версия %llu\"}", (unsigned long long)c->seq);
    int good = rc == SQLITE_ROW && sqlite3_column_int64(st, 0) == (sqlite3_int64)c->seq && sqlite3_column_int(st, 1) == 1 &&
        sqlite3_column_bytes(st, 2) == (int)strlen(expected) && !memcmp(sqlite3_column_blob(st, 2), expected, strlen(expected));
    int final = sqlite3_finalize(st); if (!good || final) return SQLITE_ERROR;
    return SQLITE_OK;
}
static int verify(sqlite3* db, void* arg) {
    struct check* c = arg;
    int rc = member_log_verify_own_read(db, c->example->public);
    return rc ? rc : inspect(db, arg);
}
static void checked(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct check*)arg)->finished = 1; }

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); size_t baseline = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct example e = {0}; memset(e.private, 7, 32);
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, e.private, 32); size_t bytes = 32;
    assert(key && EVP_PKEY_get_raw_public_key(key, e.public, &bytes) == 1 && bytes == 32); EVP_PKEY_free(key);
    open_node(&e.sender, ua, 1); open_node(&e.receiver, ua, 1);
    int ready = 0; db_async_submit(e.sender.db, "schema", DB_ASYNC_WRITE, schema, &ready, finished); poll_until(ua, &ready);
    ready = 0; db_async_submit(e.receiver.db, "schema", DB_ASYNC_WRITE, schema, &ready, finished); poll_until(ua, &ready);
    for (unsigned i = 1; i <= 120; i++) {
        e.seq = i; e.forward = i <= 100; e.finished = 0;
        snprintf(e.text, sizeof(e.text), "{\"text\":\"Карточка, версия %u\"}", i);
        db_async_submit(e.sender.db, "append-signed", DB_ASYNC_WRITE, append, &e, appended); poll_until(ua, &e.finished);
    }
    assert(e.published == 120);
    struct check c = {.example=&e, .seq=100};
    db_async_submit(e.receiver.db, "head-100", DB_ASYNC_READ, inspect, &c, checked); poll_until(ua, &c.finished);
    struct transfer t = {.example=&e, .from=101, .requests=1};
    db_async_submit(e.sender.db, "tail-from-101", DB_ASYNC_READ, tail, &t, send_tail); poll_until(ua, &t.finished);
    assert(t.next == 121 && t.requests > 1);
    assert(!db_async_destroy(e.sender.db) && !db_async_destroy(e.receiver.db));
    open_node(&e.sender, ua, 0); open_node(&e.receiver, ua, 0);
    c = (struct check){.example=&e, .seq=120};
    db_async_submit(e.sender.db, "verify-own-100", DB_ASYNC_READ, verify, &c, checked); poll_until(ua, &c.finished);
    c.finished = 0;
    db_async_submit(e.receiver.db, "verify-own-100", DB_ASYNC_WRITE, verify, &c, checked); poll_until(ua, &c.finished);
    assert(!db_async_destroy(e.sender.db) && !db_async_destroy(e.receiver.db));
    uasync_poll(ua, 0); uasync_destroy(ua, 0);
    assert(!db_storage_clear(e.sender.directory) && !db_storage_clear(e.receiver.directory));
    assert(!test_rmdir(e.sender.directory) && !test_rmdir(e.receiver.directory));
    assert(u_get_allocated_count() == baseline);
    puts("PASS async member log example: durable append -> publication -> peer commit; tail from101; latest card vs120 revisions; restart validation");
    return 0;
}
