/* Координатор S: реальные SQLite/таймеры, ошибки чтения/ACK и повторы после restart.
 * Квитанция уже сохранена; сеть перехвачена, криптография и удаление M проверяются test_dm_media. */
#include "../src/db_schema.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/utun_instance.h"
#include "../src/dm/dm_mailbox_media.h"
#include "../src/dm/dm_media.h"
#include "test_clock.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum operation { NO_FAULT, PREPARE, BIND_BLOB, BIND_INT, STEP, FINALIZE };
enum statement { SCAN, ACK, BODY };
static enum operation fault;
static enum statement target;
static unsigned hits, deletes;
static uint64_t last_peer;
static const uint8_t id[16] = {1};
static uint8_t receipt[DM_MEDIA_RECEIPT_SIZE];

static int inject(enum operation op, const char* sql) {
    const char* prefixes[] = { "SELECT node,assignment_id FROM dm_media_holders", "UPDATE dm_media_holders SET released=1",
                              "UPDATE dm_media_jobs SET body=NULL" };
    if (fault != op || !sql || strncmp(sql, prefixes[target], strlen(prefixes[target]))) return 0;
    fault = NO_FAULT; hits++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM cleanup test: statement=%d operation=%d SQL failure", target, op);
    return 1;
}

int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int len, sqlite3_stmt** st, const char** tail) {
    if (inject(PREPARE, sql)) { *st = NULL; return SQLITE_IOERR; }
    return __real_sqlite3_prepare_v2(db, sql, len, st, tail);
}
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* data, int len, void (*destroy)(void*)) {
    return inject(BIND_BLOB, sqlite3_sql(st)) ? SQLITE_NOMEM : __real_sqlite3_bind_blob(st, index, data, len, destroy);
}
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    return inject(BIND_INT, sqlite3_sql(st)) ? SQLITE_NOMEM : __real_sqlite3_bind_int64(st, index, value);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { return inject(STEP, sqlite3_sql(st)) ? SQLITE_IOERR : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && inject(FINALIZE, sqlite3_sql(st));
    int rc = __real_sqlite3_finalize(st);
    return fail ? SQLITE_IOERR : rc;
}

/* Пакеты теряются до явного recv: успех send не является подтверждением удаления M. */
int __wrap_dm_media_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32],
                         uint8_t cmd, const uint8_t* body, size_t len) {
    (void)inst; (void)group;
    unsigned peer = ed[0]; assert(!memcmp(ed, (uint8_t[32]){(uint8_t)peer}, 32));
    if (cmd == DM_MEDIA_RECEIPT) { assert(peer == 11 && len == sizeof(receipt) && !memcmp(body, receipt, len)); return -1; }
    assert(cmd == DM_MEDIA_DELETE && len == 16 + sizeof(receipt) && body[0] == (uint8_t)peer && !memcmp(body + 16, receipt, sizeof(receipt)));
    deletes++; last_peer = peer; return 0;
}
int __wrap_dm_route_group_key(struct UTUN_INSTANCE* inst, const uint8_t peer[32], uint64_t preferred, uint64_t* group, uint8_t service) {
    (void)inst; (void)peer; (void)preferred; (void)group; (void)service; return 0;
}

static void exec_sql(sqlite3* db, const char* sql) { assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK); }
static int scalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); int result = sqlite3_column_int(st, 0);
    assert(sqlite3_finalize(st) == SQLITE_OK); return result;
}

static void fixture(sqlite3* db, int pending) {
    exec_sql(db, "DELETE FROM dm_media_holders; DELETE FROM dm_media_jobs; DELETE FROM dm_file_targets");
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "INSERT INTO dm_media_jobs(id,author,seq,body,body_hash,holder,stored,receipt,recipient_ed)"
        " VALUES(?,x'0b00000000000000000000000000000000000000000000000000000000000000',1,x'01020304',zeroblob(32),x'6500000000000000000000000000000000000000000000000000000000000000',1,?,zeroblob(32))", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, id, sizeof(id), SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, receipt, sizeof(receipt), SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    exec_sql(db, "INSERT INTO dm_file_targets SELECT id,recipient_ed,1,receipt,NULL FROM dm_media_jobs");
    assert(sqlite3_prepare_v2(db, "INSERT INTO dm_media_holders(id,node,assignment_id,released)"
        " VALUES(?,x'6500000000000000000000000000000000000000000000000000000000000000',x'65000000000000000000000000000000',?)",
                             -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, id, sizeof(id), SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_int(st, 2, !pending) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

/* Ответы привязаны к сохранённому назначению; номер группы не входит в его идентичность. */
static void reply(struct dm_mb_media* media, uint64_t group, uint64_t peer, uint8_t cmd, const uint8_t* body, size_t len) {
    uint8_t packet[41] = {0}; packet[0] = (uint8_t)peer;
    assert(len + 16 <= sizeof(packet)); memcpy(packet + 16, body, len);
    dm_mailbox_media_recv(media, group, (const uint8_t[32]){(uint8_t)peer}, cmd, packet, len + 16);
}

static void check_receipt(sqlite3* db) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT receipt FROM dm_media_jobs", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == sizeof(receipt));
    assert(!memcmp(sqlite3_column_blob(st, 0), receipt, sizeof(receipt)));
    assert(sqlite3_finalize(st) == SQLITE_OK && sqlite3_get_autocommit(db));
}

/* Каждый отказ чтения должен сохранить тело и отложить отправку DELETE до успешного снимка. */
static void scan_failures(struct UTUN_INSTANCE* inst) {
    const enum operation ops[] = {PREPARE, BIND_BLOB, STEP, FINALIZE};
    for (unsigned i = 0; i < sizeof(ops) / sizeof(*ops); i++) {
        fixture(inst->topo_sqlite_db, 1);
        unsigned before = deletes;
        target = SCAN; fault = ops[i]; hits = 0;
        test_clock_next_timer(inst->ua);
        assert(hits == 1 && deletes == before);
        assert(scalar(inst->topo_sqlite_db, "SELECT body IS NOT NULL FROM dm_media_jobs") == 1);
        assert(scalar(inst->topo_sqlite_db, "SELECT released FROM dm_media_holders") == 0);
        check_receipt(inst->topo_sqlite_db);
        exec_sql(inst->topo_sqlite_db, "UPDATE dm_media_jobs SET retry_at=0");
        test_clock_next_timer(inst->ua);
        assert(deletes == before + 1 && last_peer == 101);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM cleanup test: scan prepare/bind/step/finalize preserve pending body and retry PASS");
}

/* Старый обработчик ACK ещё использует autocommit; очистка body теперь откатывается целиком при любой ошибке. */
static void write_failures(struct UTUN_INSTANCE* inst, struct dm_mb_media* media) {
    for (target = ACK; target <= BODY; target++) for (enum operation op = PREPARE; op <= FINALIZE; op++) {
        if (op == BIND_INT) continue;
        fixture(inst->topo_sqlite_db, target == ACK);
        fault = op; hits = 0;
        if (target == ACK) reply(media, 77, 101, DM_MEDIA_DELETE_ACK, id, sizeof(id));
        else test_clock_next_timer(inst->ua);
        assert(hits == 1);
        if (target == ACK) {
            assert(scalar(inst->topo_sqlite_db, "SELECT released FROM dm_media_holders") == (op == FINALIZE));
            reply(media, 77, 101, DM_MEDIA_DELETE_ACK, id, sizeof(id));
        } else {
            assert(scalar(inst->topo_sqlite_db, "SELECT body IS NULL FROM dm_media_jobs") == 0);
        }
        check_receipt(inst->topo_sqlite_db);
        exec_sql(inst->topo_sqlite_db, "UPDATE dm_media_jobs SET retry_at=0");
        test_clock_next_timer(inst->ua);
        assert(scalar(inst->topo_sqlite_db, "SELECT body IS NULL FROM dm_media_jobs") == 1);
        check_receipt(inst->topo_sqlite_db);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM cleanup test: ACK/body SQL failures retry without losing receipt PASS");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_DM, DEBUG_LEVEL_DEBUG);
    size_t before = u_get_allocated_count();
    test_clock_init(10000); test_clock_sync_wall_time();
    for (unsigned i = 0; i < sizeof(receipt); i++) receipt[i] = (uint8_t)(i + 3);
    char path[] = "/tmp/utun_dm_cleanup_XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd);
    struct UTUN_INSTANCE inst = {.node_id=99, .chat_content_started=1}; inst.my_ed25519_pubkey[0] = 99; inst.ua = uasync_create(); assert(inst.ua);
    assert(sqlite3_open(path, &inst.topo_sqlite_db) == SQLITE_OK);
    assert(!db_schema_table(inst.topo_sqlite_db, "nodes", NULL));
    struct dm_mb_media* media = dm_mailbox_media_create(&inst); assert(media);
    scan_failures(&inst); write_failures(&inst, media);

    /* Таймер не владеет чужой транзакцией и не должен откатить изменения её caller. */
    fixture(inst.topo_sqlite_db, 1); unsigned held = deletes;
    exec_sql(inst.topo_sqlite_db, "BEGIN IMMEDIATE;UPDATE dm_media_jobs SET seq=777");
    test_clock_next_timer(inst.ua);
    assert(!sqlite3_get_autocommit(inst.topo_sqlite_db) && deletes == held);
    assert(scalar(inst.topo_sqlite_db, "SELECT seq FROM dm_media_jobs") == 777);
    exec_sql(inst.topo_sqlite_db, "ROLLBACK");
    assert(scalar(inst.topo_sqlite_db, "SELECT seq FROM dm_media_jobs") == 1);

    /* После доставки истёкшее обещание offline M освобождает body без DELETE_ACK. */
    fixture(inst.topo_sqlite_db, 1); held = deletes;
    exec_sql(inst.topo_sqlite_db, "UPDATE dm_media_jobs SET expires=1"); test_clock_next_timer(inst.ua);
    assert(deletes == held && scalar(inst.topo_sqlite_db, "SELECT released FROM dm_media_holders") == 2);
    assert(scalar(inst.topo_sqlite_db, "SELECT body IS NULL FROM dm_media_jobs") == 1);
    check_receipt(inst.topo_sqlite_db);

    fixture(inst.topo_sqlite_db, 1);
    test_clock_next_timer(inst.ua); // DELETE потерян; повтор восстанавливается из той же БД.
    unsigned sent = deletes;
    dm_mailbox_media_destroy(media); assert(sqlite3_close(inst.topo_sqlite_db) == SQLITE_OK);
    assert(sqlite3_open(path, &inst.topo_sqlite_db) == SQLITE_OK);
    media = dm_mailbox_media_create(&inst); assert(media);
    test_clock_next_timer(inst.ua); assert(deletes == sent + 1);
    assert(scalar(inst.topo_sqlite_db, "SELECT body IS NOT NULL FROM dm_media_jobs") == 1);
    check_receipt(inst.topo_sqlite_db);
    reply(media, 77, 999, DM_MEDIA_DELETE_ACK, id, sizeof(id));
    assert(scalar(inst.topo_sqlite_db, "SELECT released FROM dm_media_holders") == 0);
    uint8_t stale[32] = {101}; memcpy(stale + 16, id, sizeof(id)); stale[15] = 1;
    dm_mailbox_media_recv(media, 78, (const uint8_t[32]){101}, DM_MEDIA_DELETE_ACK, stale, sizeof(stale));
    assert(scalar(inst.topo_sqlite_db, "SELECT released FROM dm_media_holders") == 0);
    assert(scalar(inst.topo_sqlite_db, "SELECT holder=x'6500000000000000000000000000000000000000000000000000000000000000' FROM dm_media_jobs") == 1);
    exec_sql(inst.topo_sqlite_db, "INSERT INTO dm_media_holders SELECT id,x'6600000000000000000000000000000000000000000000000000000000000000',x'66000000000000000000000000000000',0 FROM dm_media_jobs");
    reply(media, 78, 101, DM_MEDIA_DELETE_ACK, id, sizeof(id));
    assert(scalar(inst.topo_sqlite_db, "SELECT released FROM dm_media_holders WHERE node=x'6500000000000000000000000000000000000000000000000000000000000000'") == 1);
    reply(media, 77, 101, DM_MEDIA_DELETE_ACK, id, sizeof(id));
    exec_sql(inst.topo_sqlite_db, "UPDATE dm_media_jobs SET retry_at=0"); test_clock_next_timer(inst.ua);
    assert(last_peer == 102 && scalar(inst.topo_sqlite_db, "SELECT body IS NOT NULL FROM dm_media_jobs") == 1);
    reply(media, 77, 102, DM_MEDIA_DELETE_ACK, id, sizeof(id));
    exec_sql(inst.topo_sqlite_db, "UPDATE dm_media_jobs SET retry_at=0"); test_clock_next_timer(inst.ua);
    assert(scalar(inst.topo_sqlite_db, "SELECT body IS NULL FROM dm_media_jobs") == 1);
    uint8_t stored[25] = {1};
    reply(media, 77, 101, DM_MEDIA_STORED, stored, sizeof(stored));
    assert(last_peer == 101 && scalar(inst.topo_sqlite_db, "SELECT count(*) FROM dm_media_holders WHERE released=0") == 0);
    check_receipt(inst.topo_sqlite_db);
    dm_mailbox_media_destroy(media); assert(sqlite3_close(inst.topo_sqlite_db) == SQLITE_OK);
    uasync_destroy(inst.ua, 0); assert(!unlink(path)); test_clock_finish();
    assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM cleanup test: restart, lost/duplicate/foreign ACK, multiple holders, late STORED PASS");
    return 0;
}
