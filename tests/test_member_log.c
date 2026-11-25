/* Настоящие подписи и SQLite: линейность, хвост с N, атомарность и отказанные права. */
#include "../src/group/member_log.h"
#include "../src/db_schema.h"
#include "../lib/debug_config.h"
#include "test_utils.h"
#include <openssl/evp.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GROUP_A "0100000000000000000000000000000000000000000000000000000000000000"
#define GROUP_B "0200000000000000000000000000000000000000000000000000000000000000"

struct model { sqlite3* db; int decision, fail_after_write, calls; };
static void sql(sqlite3* db, const char* query) {
    int rc = sqlite3_exec(db, query, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "test SQL failed rc=%d error=%s", rc, sqlite3_errmsg(db));
    assert(rc == SQLITE_OK);
}
static sqlite3* database(void) {
    sqlite3* db = NULL; assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    sql(db, "PRAGMA foreign_keys=ON; CREATE TABLE channels(channel_id TEXT PRIMARY KEY,ed25519_pubkey BLOB UNIQUE,name TEXT,owner_node_id BLOB,x25519_pubkey BLOB,signature BLOB); "
        "INSERT INTO channels(channel_id,ed25519_pubkey) VALUES('" GROUP_A "',X'" GROUP_A "'),"
        "('" GROUP_B "',X'" GROUP_B "');"
        " CREATE TABLE model(value BLOB NOT NULL)");
    assert(!db_schema_table(db, "member_faults", NULL) && !db_schema_table(db, "member_chains", NULL) && !db_schema_table(db, "member_log", NULL)); return db;
}
static int apply(void* arg, const struct member_log_record* r) {
    struct model* m = arg; m->calls++;
    if (m->decision != 1) return m->decision;
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(m->db, "INSERT INTO model(value) VALUES(?1)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, r->body, (int)r->body_size, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    return m->fail_after_write ? -1 : 1;
}
static sqlite3_int64 count(sqlite3* db, const char* table) {
    char query[96]; snprintf(query, sizeof(query), "SELECT count(*) FROM %s", table); sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, query, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    sqlite3_int64 rows = sqlite3_column_int64(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return rows;
}
/* Приватный ключ используется как Ed25519 seed; публичный берётся из OpenSSL. */
static void keys(uint8_t private[32], uint8_t public[32], uint8_t seed) {
    memset(private, seed, 32); EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private, 32);
    size_t bytes = 32; assert(key && EVP_PKEY_get_raw_public_key(key, public, &bytes) == 1 && bytes == 32); EVP_PKEY_free(key);
}
static sqlite3* clone(sqlite3* source, const char* path) {
    sqlite3* db = NULL; assert(sqlite3_open(path, &db) == SQLITE_OK);
    sqlite3_backup* backup = sqlite3_backup_init(db, "main", source, "main"); assert(backup);
    assert(sqlite3_backup_step(backup, -1) == SQLITE_DONE && sqlite3_backup_finish(backup) == SQLITE_OK); return db;
}

/* Проверяются ровно последние100 подписей, плюс хеш предшественника; чужие цепочки не читаются. */
static int deny_log_body(void* arg, int action, const char* table, const char* column, const char* db, const char* caller) {
    (void)arg; (void)db; (void)caller;
    return action == SQLITE_READ && table && column && !strcmp(table, "member_log") && !strcmp(column, "body") ?
        SQLITE_DENY : SQLITE_OK;
}
static void startup_verification(sqlite3* source, const uint8_t member[32]) {
    assert(!member_log_verify_own(source, member));
    assert(member_log_verify_own_read(source, member) == SQLITE_MISUSE);
    sql(source, "BEGIN IMMEDIATE"); assert(!member_log_verify_own_read(source, member) && !sqlite3_get_autocommit(source));
    assert(!sqlite3_set_authorizer(source, deny_log_body, NULL));
    assert(member_log_verify_own_read(source, member) == SQLITE_AUTH && !sqlite3_get_autocommit(source));
    assert(!sqlite3_set_authorizer(source, NULL, NULL));
    assert(!member_log_verify_own_read(source, member)); sql(source, "ROLLBACK");
    sqlite3* db = clone(source, ":memory:");
    sql(db, "UPDATE member_log SET body=X'00' WHERE seq<=20");
    assert(!member_log_verify_own(db, member) && sqlite3_get_autocommit(db));
    assert(sqlite3_close(db) == SQLITE_OK);
    const char* faults[] = {
        "UPDATE member_log SET body=X'00' WHERE seq=21",
        "UPDATE member_log SET signature=zeroblob(64) WHERE seq=120",
        "UPDATE member_chains SET head_hash=zeroblob(32)",
        "UPDATE member_chains SET head_seq=119",
        "DELETE FROM member_log WHERE seq=80",
        "DELETE FROM member_log WHERE seq=120",
        "DELETE FROM member_log WHERE seq=20"
    };
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        db = clone(source, ":memory:"); sql(db, faults[i]);
        assert(!member_log_verify_own(db, (uint8_t[32]){1}));
        sqlite3_int64 bytes; unsigned char* before = sqlite3_serialize(db, "main", &bytes, 0); assert(before);
        assert(member_log_verify_own(db, member) < 0 && sqlite3_get_autocommit(db));
        sqlite3_int64 after_bytes; unsigned char* after = sqlite3_serialize(db, "main", &after_bytes, 0);
        assert(after && bytes == after_bytes && !memcmp(before, after, (size_t)bytes));
        sqlite3_free(before); sqlite3_free(after); assert(sqlite3_close(db) == SQLITE_OK);
    }
    db = clone(source, ":memory:");
    sql(db, "DELETE FROM member_log WHERE seq>79; UPDATE member_chains SET head_seq=79,"
        "head_hash=(SELECT block_hash FROM member_log WHERE seq=79)");
    assert(!member_log_verify_own(db, member));
    sql(db, "UPDATE member_log SET signature=zeroblob(64) WHERE seq=1");
    assert(member_log_verify_own(db, member) < 0 && sqlite3_get_autocommit(db));
    assert(sqlite3_close(db) == SQLITE_OK);
    puts("PASS startup: own last100 signatures and predecessor hash, shorter chains, gaps/head corruption, no data mutations");
}

/* Перехватываем именно реальный WAL xSync, сохраняя остальные методы системного VFS. */
static const sqlite3_io_methods* disk_methods;
static int sync_calls, fail_sync;
static int disk_sync(sqlite3_file* file, int flags) {
    sync_calls++;
    return fail_sync ? SQLITE_IOERR_FSYNC : disk_methods->xSync(file, flags);
}
static void durable_commit(const uint8_t member[32], const uint8_t private[32], const uint8_t public[32]) {
    char directory[512] = "/tmp/utun-member-log-XXXXXX", path[600]; assert(!test_mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/chats.db", directory);
    sqlite3* memory = database(); sql(memory, "BEGIN IMMEDIATE");
    assert(member_log_commit(memory) < 0 && !sqlite3_get_autocommit(memory)); sql(memory, "ROLLBACK");
    sqlite3* db = clone(memory, path); assert(sqlite3_close(memory) == SQLITE_OK);
    sql(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0; PRAGMA foreign_keys=ON");
    sqlite3* observer = NULL; assert(sqlite3_open_v2(path, &observer, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    struct model model = {db, 1, 0, 0}; struct member_log_record r; struct member_log_head head;
    uint8_t scope[32] = {1}, grant[32] = {2};
    sql(db, "BEGIN IMMEDIATE");
    assert(!member_log_make(db, GROUP_A, member, 1, scope, grant, (const uint8_t*)"durable", 7, private, &r));
    assert(member_log_accept(db, &r, public, apply, &model) == 1);
    sqlite3_file* wal = NULL;
    assert(sqlite3_file_control(db, "main", SQLITE_FCNTL_JOURNAL_POINTER, &wal) == SQLITE_OK && wal && wal->pMethods);
    disk_methods = wal->pMethods; sqlite3_io_methods watched = *disk_methods; watched.xSync = disk_sync; wal->pMethods = &watched;
    sync_calls = 0;
    assert(!count(observer, "member_log") && !count(observer, "model") && !sync_calls);
    assert(!member_log_commit(db) && sync_calls > 0 && sqlite3_get_autocommit(db));
    assert(count(observer, "member_log") == 1 && count(observer, "model") == 1);
    assert(!member_log_head(observer, GROUP_A, member, &head) && head.seq == 1 && !memcmp(head.hash, r.hash, 32));
    sql(db, "PRAGMA synchronous=NORMAL; BEGIN IMMEDIATE");
    assert(!member_log_make(db, GROUP_A, member, 2, scope, grant, (const uint8_t*)"refused", 7, private, &r));
    assert(member_log_accept(db, &r, public, apply, &model) == 1);
    int before = sync_calls;
    assert(member_log_commit(db) < 0 && sync_calls == before && !sqlite3_get_autocommit(db)); sql(db, "ROLLBACK");
    sql(db, "PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    assert(member_log_accept(db, &r, public, apply, &model) == 1);
    fail_sync = 1; assert(member_log_commit(db) < 0 && sync_calls > before); fail_sync = 0;
    if (!sqlite3_get_autocommit(db)) sql(db, "ROLLBACK");
    wal->pMethods = disk_methods;
    assert(sqlite3_close(observer) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK);
    assert(sqlite3_open(path, &db) == SQLITE_OK && !member_log_verify_own(db, member));
    assert(!member_log_head(db, GROUP_A, member, &head) && head.seq == 1 && count(db, "model") == 1);
    sql(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    assert(member_log_commit(db) < 0 && !sqlite3_get_autocommit(db)); sql(db, "ROLLBACK");
    assert(sqlite3_close(db) == SQLITE_OK && !test_unlink(path) && !test_rmdir(directory));
    puts("PASS durability: real WAL sync precedes success; NORMAL/memory/non-WAL refused; sync failure prevents success; reopen consistent");
}
/* Получатель всегда запрашивает от своей головы+1; тест может остановиться на заданном N. */
static void copy(sqlite3* src, struct model* dst, const uint8_t member[32], uint64_t through, const uint8_t public[32]) {
    for (;;) {
        struct member_log_head head; assert(!member_log_head(dst->db, GROUP_A, member, &head)); if (head.seq >= through) break;
        uint8_t bytes[600]; size_t size = sizeof(bytes); uint64_t next; int more;
        assert(!member_log_tail(src, GROUP_A, member, head.seq + 1, bytes, &size, &next, &more) && size);
        sql(dst->db, "BEGIN IMMEDIATE"); size_t offset = 0;
        while (offset < size) {
            assert(size - offset >= 2); size_t n = (size_t)bytes[offset] * 256 + bytes[offset + 1]; offset += 2;
            assert(n <= size - offset && n >= MEMBER_LOG_WIRE_MIN);
            uint64_t seq = 0; for (unsigned i = MEMBER_LOG_SEQUENCE_OFFSET; i < MEMBER_LOG_SEQUENCE_OFFSET + 8; i++) seq = (seq << 8) | bytes[offset + i];
            if (seq > through) break;
            assert(member_log_accept_wire(dst->db, GROUP_A, member, bytes + offset, n, public, apply, dst) == 1); offset += n;
        }
        sql(dst->db, "COMMIT");
    }
}

/* Редкая диагностика действительно делает двоичный поиск, а не читает весь журнал. */
static void binary_comparison(sqlite3* source, const uint8_t member[32], const uint8_t private[32], const uint8_t public[32], unsigned first) {
    sqlite3 *local = database(), *remote = database(); struct model a = {local, 1, 0, 0}, b = {remote, 1, 0, 0};
    copy(source, &a, member, 120, public); copy(source, &b, member, first - 1, public);
    uint8_t scope[32] = {1}, grant[32] = {2}; struct member_log_record r;
    sql(remote, "BEGIN IMMEDIATE");
    for (unsigned i = first; i <= 120; i++) {
        assert(!member_log_make(remote, GROUP_A, member, 10000 + i, scope, grant, (const uint8_t*)"other", 5, private, &r));
        assert(r.seq == i && member_log_accept(remote, &r, public, apply, &b) == 1);
    }
    sql(remote, "COMMIT");
    struct member_log_probe p; assert(member_log_probe_begin(local, GROUP_A, member, 120, &p) == 1 && p.query == 120);
    int rc = 1, requests = 0;
    while (rc == 1) {
        uint8_t wire[MEMBER_LOG_WIRE_MAX]; size_t size = sizeof(wire);
        assert(member_log_get(remote, GROUP_A, member, p.query, wire, &size) == 1);
        sql(local, "BEGIN IMMEDIATE"); rc = member_log_probe_reply(local, &p, public, wire, size); assert(rc >= 0);
        sql(local, "COMMIT"); requests++; assert(requests <= 9);
    }
    assert(rc == MEMBER_LOG_BLOCKED && p.same == first - 1 && p.different == first && !p.query);
    struct member_log_head h; assert(!member_log_head(local, GROUP_A, member, &h) && h.blocked && h.seq == 120);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(local, "SELECT seq,second_wire FROM member_faults WHERE group_id=X'" GROUP_A "'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == first);
    assert(!member_log_decode(sqlite3_column_blob(st, 1), (size_t)sqlite3_column_bytes(st, 1), public, &r) && r.seq == first);
    assert(sqlite3_finalize(st) == SQLITE_OK && count(local, "member_log") == 120 && count(local, "model") == 120);
    /* Подтверждённая компрометация не исчезает при новом экземпляре БД. */
    sqlite3_int64 bytes; unsigned char* image = sqlite3_serialize(local, "main", &bytes, 0); assert(image);
    sqlite3* reopened = NULL; assert(sqlite3_open(":memory:", &reopened) == SQLITE_OK);
    assert(sqlite3_deserialize(reopened, "main", image, bytes, bytes, SQLITE_DESERIALIZE_FREEONCLOSE) == SQLITE_OK);
    assert(!member_log_head(reopened, GROUP_A, member, &h) && h.blocked && h.seq == 120);
    assert(sqlite3_close(reopened) == SQLITE_OK);
    assert(sqlite3_close(local) == SQLITE_OK && sqlite3_close(remote) == SQLITE_OK);
    remote = database(); b.db = remote; copy(source, &b, member, 120, public);
    /* Проверка совпавшего префикса при разных длинах не блокирует автора. */
    sql(remote, "BEGIN IMMEDIATE");
    assert(member_log_probe_begin(remote, GROUP_A, member, 59, &p) == 1 && p.query == 59);
    uint8_t wire[MEMBER_LOG_WIRE_MAX]; size_t size = sizeof(wire);
    assert(member_log_get(source, GROUP_A, member, 59, wire, &size) == 1);
    assert(!member_log_probe_reply(remote, &p, public, wire, size) && !p.query && p.same == 59); sql(remote, "COMMIT");
    assert(!member_log_head(remote, GROUP_A, member, &h) && !h.blocked);
    /* Неверная подпись и ответ на другой номер не меняют ни контекст, ни блокировку. */
    assert(member_log_probe_begin(remote, GROUP_A, member, 120, &p) == 1); struct member_log_probe before = p;
    sql(remote, "BEGIN IMMEDIATE"); assert(member_log_probe_reply(remote, &p, public, wire, size) < 0);
    sql(remote, "ROLLBACK"); assert(!memcmp(&p, &before, sizeof(p)));
    size = sizeof(wire); assert(member_log_get(source, GROUP_A, member, 120, wire, &size) == 1); wire[size - 1] ^= 1;
    sql(remote, "BEGIN IMMEDIATE"); assert(member_log_probe_reply(remote, &p, public, wire, size) < 0);
    sql(remote, "ROLLBACK"); assert(!memcmp(&p, &before, sizeof(p)));
    assert(!member_log_head(remote, GROUP_A, member, &h) && !h.blocked);
    assert(sqlite3_close(remote) == SQLITE_OK);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_enable_console(1);
    sqlite3 *src = database(), *dst = database(); struct model writer = {src, 1, 0, 0}, reader = {dst, 1, 0, 0};
    uint8_t private[32], public[32], wrong_private[32], wrong_public[32], scope[32] = {1}, grant[32] = {2};
    keys(private, public, 7); keys(wrong_private, wrong_public, 9);
    const uint8_t* member = public; struct member_log_record records[122];
    struct member_log_head head; assert(!member_log_head(src, GROUP_A, member, &head) && !head.seq);
    sql(src, "BEGIN IMMEDIATE");
    for (unsigned i = 1; i <= 120; i++) {
        assert(!member_log_make(src, GROUP_A, member, 1000 - i, scope, grant, (const uint8_t*)"payload", 7, private, &records[i]));
        assert(records[i].seq == i && (i == 1 || records[i].time_us > records[i - 1].time_us));
        assert(member_log_accept(src, &records[i], public, apply, &writer) == 1);
    }
    sql(src, "COMMIT"); assert(count(src, "member_log") == 120 && count(src, "model") == 120);
    startup_verification(src, member); durable_commit(member, private, public);
    sql(dst, "BEGIN IMMEDIATE"); assert(member_log_accept(dst, &records[101], public, apply, &reader) < 0);
    sql(dst, "ROLLBACK"); assert(!count(dst, "member_log") && !reader.calls);
    copy(src, &reader, member, 100, public); assert(!member_log_head(dst, GROUP_A, member, &head) && head.seq == 100);
    uint8_t page[MEMBER_LOG_WIRE_MAX + 2]; size_t size = sizeof(page); uint64_t next; int more;
    assert(!member_log_tail(src, GROUP_A, member, 101, page, &size, &next, &more));
    struct member_log_record decoded; size_t n = (size_t)page[0] * 256 + page[1];
    assert(!member_log_decode(page + 2, n, public, &decoded) && decoded.seq == 101);
    copy(src, &reader, member, 120, public); assert(count(dst, "model") == 120);
    sql(dst, "BEGIN IMMEDIATE"); int calls = reader.calls;
    assert(!member_log_accept(dst, &records[100], public, apply, &reader) && calls == reader.calls); sql(dst, "COMMIT");
    assert(member_log_checkpoint(dst, GROUP_A, member, 100, records[100].hash) == 1);
    assert(!member_log_checkpoint(dst, GROUP_A, member, 100, records[99].hash));
    assert(!member_log_checkpoint(dst, GROUP_A, member, 121, records[120].hash));
    uint8_t zero[32] = {0}; assert(member_log_checkpoint(dst, GROUP_A, member, 0, zero) == 1);
    assert(!member_log_checkpoint(dst, GROUP_A, member, 0, records[1].hash));

    /* Конфликт уже существующего номера и чужой ключ ничего не меняют. */
    sql(dst, "BEGIN IMMEDIATE");
    assert(member_log_accept(dst, &records[100], wrong_public, apply, &reader) < 0); sql(dst, "ROLLBACK");
    uint8_t tampered[MEMBER_LOG_WIRE_MAX]; size = sizeof(tampered);
    assert(!member_log_encode(&records[100], tampered, &size)); tampered[MEMBER_LOG_HEADER_SIZE] ^= 1;
    assert(member_log_decode(tampered, size, public, &decoded) < 0);
    struct member_log_record conflict = records[100]; conflict.hash[0] ^= 1;
    sql(dst, "BEGIN IMMEDIATE"); assert(member_log_accept(dst, &conflict, public, apply, &reader) < 0); sql(dst, "ROLLBACK");
    assert(count(dst, "member_log") == 120 && count(dst, "model") == 120);
    size = sizeof(tampered); assert(!member_log_encode(&records[100], tampered, &size));
    sql(dst, "BEGIN IMMEDIATE");
    assert(member_log_accept_wire(dst, GROUP_B, member, tampered, size, public, apply, &reader) < 0); sql(dst, "ROLLBACK");
    sql(dst, "BEGIN IMMEDIATE");
    assert(member_log_accept_wire(dst, GROUP_A, (uint8_t[32]){1}, tampered, size, public, apply, &reader) < 0); sql(dst, "ROLLBACK");

    /* Callback уже изменил текущие данные: rollback отменяет и их, и журнал, и голову. */
    sql(src, "BEGIN IMMEDIATE");
    assert(!member_log_make(src, GROUP_A, member, 1, scope, grant, (const uint8_t*)"next", 4, private, &records[121]));
    writer.fail_after_write = 1; assert(member_log_accept(src, &records[121], public, apply, &writer) < 0);
    sql(src, "ROLLBACK"); writer.fail_after_write = 0;
    assert(count(src, "model") == 120 && count(src, "member_log") == 120);
    assert(!member_log_head(src, GROUP_A, member, &head) && head.seq == 120);
    sql(src, "BEGIN IMMEDIATE"); writer.decision = MEMBER_LOG_DEFERRED;
    assert(member_log_accept(src, &records[121], public, apply, &writer) == MEMBER_LOG_DEFERRED); sql(src, "ROLLBACK");
    assert(!member_log_head(src, GROUP_A, member, &head) && head.seq == 120);

    /* Запрещённая правка остаётся в цепочке и не мешает следующему разрешённому действию. */
    sql(src, "BEGIN IMMEDIATE"); writer.decision = 0;
    assert(member_log_accept(src, &records[121], public, apply, &writer) == 1); sql(src, "COMMIT");
    assert(count(src, "member_log") == 121 && count(src, "model") == 120);
    sql(src, "BEGIN IMMEDIATE"); writer.decision = 1;
    assert(!member_log_make(src, GROUP_A, member, 1, scope, grant, (const uint8_t*)"allowed", 7, private, &decoded));
    assert(decoded.seq == 122 && member_log_accept(src, &decoded, public, apply, &writer) == 1); sql(src, "COMMIT");
    assert(count(src, "model") == 121);
    size = sizeof(page); assert(!member_log_tail(src, GROUP_A, member, 123, page, &size, &next, &more) && !size && next == 123 && !more);
    size = 1; assert(member_log_tail(src, GROUP_A, member, 1, page, &size, &next, &more) < 0);
    /* Один мембер имеет отдельную цепочку в каждой группе; номер и подпись привязаны к группе. */
    assert(!member_log_head(src, GROUP_B, member, &head) && !head.seq);
    sql(src, "BEGIN IMMEDIATE");
    assert(!member_log_make(src, GROUP_B, member, 1, scope, grant, (const uint8_t*)"other group", 11, private, &decoded));
    assert(decoded.seq == 1 && !memcmp(decoded.previous, zero, 32));
    assert(member_log_accept(src, &decoded, public, apply, &writer) == 1); sql(src, "COMMIT");
    assert(!member_log_head(src, GROUP_A, member, &head) && head.seq == 122);
    assert(!member_log_head(src, GROUP_B, member, &head) && head.seq == 1);
    conflict = decoded; strcpy(conflict.channel, GROUP_A);
    sql(src, "BEGIN IMMEDIATE"); assert(member_log_accept(src, &conflict, public, apply, &writer) < 0); sql(src, "ROLLBACK");
    assert(!member_log_checkpoint(src, GROUP_B, member, 100, records[100].hash));
    binary_comparison(src, member, private, public, 1);
    binary_comparison(src, member, private, public, 60);
    binary_comparison(src, member, private, public, 120);
    /* Две валидные подписи одного номера блокируют цепочку; третьи записи не принимаются. */
    sqlite3* other = database(); struct member_log_record different;
    sql(other, "BEGIN IMMEDIATE");
    assert(!member_log_make(other, GROUP_A, member, 5000, scope, grant, (const uint8_t*)"conflict", 8, private, &different));
    sql(other, "ROLLBACK"); assert(different.seq == 1);
    sql(dst, "BEGIN IMMEDIATE"); calls = reader.calls;
    assert(member_log_accept(dst, &different, public, apply, &reader) == MEMBER_LOG_BLOCKED); sql(dst, "COMMIT");
    assert(!member_log_head(dst, GROUP_A, member, &head) && head.blocked && head.seq == 120);
    sql(dst, "BEGIN IMMEDIATE");
    assert(member_log_accept(dst, &records[121], public, apply, &reader) == MEMBER_LOG_BLOCKED);
    assert(member_log_make(dst, GROUP_A, member, 9999, scope, grant, (const uint8_t*)"blocked", 7, private, &decoded) < 0);
    sql(dst, "COMMIT"); assert(reader.calls == calls && count(dst, "member_log") == 120);
    /* В другой группе тот же мембер имеет независимую цепочку. */
    sql(dst, "BEGIN IMMEDIATE");
    assert(!member_log_make(dst, GROUP_B, member, 1, scope, grant, (const uint8_t*)"allowed", 7, private, &decoded));
    assert(member_log_accept(dst, &decoded, public, apply, &reader) == 1); sql(dst, "COMMIT");
    assert(!member_log_head(dst, GROUP_B, member, &head) && !head.blocked && head.seq == 1);
    /* Порча локального блока не превращается в обвинение автора. */
    struct model damaged = {other, 1, 0, 0};
    sql(other, "BEGIN IMMEDIATE"); assert(member_log_accept(other, &records[1], public, apply, &damaged) == 1); sql(other, "COMMIT");
    sql(other, "UPDATE member_log SET body=X'00' WHERE seq=1");
    sql(other, "BEGIN IMMEDIATE"); assert(member_log_accept(other, &different, public, apply, &damaged) < 0); sql(other, "ROLLBACK");
    assert(!member_log_head(other, GROUP_A, member, &head) && !head.blocked && head.seq == 1);
    assert(sqlite3_close(other) == SQLITE_OK);
    assert(sqlite3_close(dst) == SQLITE_OK && sqlite3_close(src) == SQLITE_OK);
    puts("PASS member log: signed per-group chains, tail from 101, atomic rollback, compromised author quarantine and binary first-divergence search");
    return 0;
}
