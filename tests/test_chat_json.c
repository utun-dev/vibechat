#include "test_utils.h"
/* JSON introspection: допустимое имя канала возвращается без потери байтов и с полным escaping. */
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_core_priv.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <assert.h>
#include <string.h>

enum fault { F_NONE, F_PREPARE, F_BIND, F_STEP, F_FINALIZE, F_TEXT, F_ALLOCATE };
static enum fault fault;
static const char* target;
static int bind_index, text_column, step_number, fail_sync;

static int take(enum fault point, const char* sql, int index) {
    if (fault != point || !target || strncmp(sql, target, strlen(target)) ||
        (point == F_BIND && index != bind_index) || (point == F_TEXT && index != text_column)) return 0;
    if (point == F_STEP && --step_number) return 0;
    fault = F_NONE; DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JSON test: fault stage=%d index=%d target=%s", point, index, target); return 1;
}
extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (take(F_PREPARE, sql, 0)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* value, int bytes, void (*destroy)(void*)) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, value, bytes, destroy);
}
extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}
extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int bytes, void (*destroy)(void*)) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, value, bytes, destroy);
}
extern int __real_sqlite3_bind_int(sqlite3_stmt*, int, int);
int __wrap_sqlite3_bind_int(sqlite3_stmt* st, int index, int value) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int(st, index, value);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (take(F_STEP, sqlite3_sql(st), 0)) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && take(F_FINALIZE, sqlite3_sql(st), 0);
    int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_IOERR : rc;
}
extern const unsigned char* __real_sqlite3_column_text(sqlite3_stmt*, int);
const unsigned char* __wrap_sqlite3_column_text(sqlite3_stmt* st, int column) {
    if (take(F_TEXT, sqlite3_sql(st), column)) return NULL;
    return __real_sqlite3_column_text(st, column);
}
extern void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t bytes, const char* location) {
    if (fault == F_ALLOCATE && strstr(location, "strbuf.c")) { fault = F_NONE; return NULL; }
    return __real_u_realloc_impl(ptr, bytes, location);
}
extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* location) {
    if (fail_sync && strstr(location, "db_sync.c")) { fail_sync = 0; return NULL; }
    return __real_u_calloc_impl(count, bytes, location);
}

static int listing(struct UTUN_INSTANCE* inst, const char* channel, unsigned kind, char* out, size_t capacity, size_t* size) {
    if (!kind) return chat_core_get_channels_json(inst, out, capacity, size);
    if (kind == 1) return chat_core_get_members_json(inst, channel, out, capacity, size);
    return chat_core_get_messages_json(inst, channel, 100, 0, out, capacity, size);
}

static void valid_json(sqlite3* db, const char* json, size_t size) {
    sqlite3_stmt* st = NULL; assert(strlen(json) == size);
    assert(sqlite3_prepare_v2(db, "SELECT json_valid(?),json_type(?)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, json, (int)size, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(st, 2, json, (int)size, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) && !strcmp((const char*)sqlite3_column_text(st, 1), "array"));
    assert(sqlite3_finalize(st) == SQLITE_OK);
}

static void text_value(sqlite3* db, const char* json, const char* path, const char* expected) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT json_extract(?1,?2)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_TEXT);
    assert(sqlite3_column_bytes(st, 0) == (int)strlen(expected) && !memcmp(sqlite3_column_text(st, 0), expected, strlen(expected)));
    assert(sqlite3_finalize(st) == SQLITE_OK);
}

static void execute(sqlite3* db, const char* sql) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JSON fixture: rc=%d: %s", rc, sqlite3_errmsg(db));
    assert(rc == SQLITE_OK);
}

static void set_text(sqlite3* db, const char* sql, const char* value, int bytes) {
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, value, bytes, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_finalize(st) == SQLITE_OK);
}

static void rejected(struct UTUN_INSTANCE* inst, const char* channel, unsigned kind) {
    char out[65536]; size_t size = 999; int changes = sqlite3_total_changes(inst->topo_sqlite_db); out[0] = 'x';
    assert(listing(inst, channel, kind, out, sizeof(out), &size) < 0 && fault == F_NONE && !size && !out[0]);
    assert(changes == sqlite3_total_changes(inst->topo_sqlite_db));
}

static void retry(struct UTUN_INSTANCE* inst, const char* channel, unsigned kind) {
    char out[65536]; size_t size = 0; int changes = sqlite3_total_changes(inst->topo_sqlite_db);
    assert(!listing(inst, channel, kind, out, sizeof(out), &size)); valid_json(inst->topo_sqlite_db, out, size);
    assert(changes == sqlite3_total_changes(inst->topo_sqlite_db));
}

static void boundaries(struct UTUN_INSTANCE* inst, const char* channel) {
    uint8_t guarded[65540]; char full[65536];
    for (unsigned kind = 0; kind < 3; kind++) {
        size_t bytes = 0; assert(!listing(inst, channel, kind, full, sizeof(full), &bytes));
        size_t capacities[] = {0, 1, 2, 3, bytes - 1, bytes, bytes + 1};
        for (unsigned c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++) {
            memset(guarded, 0xa5, sizeof(guarded)); size_t capacity = capacities[c], size = 999;
            int rc = listing(inst, channel, kind, (char*)guarded + 2, capacity, &size);
            assert(guarded[0] == 0xa5 && guarded[1] == 0xa5);
            for (size_t i = capacity + 2; i < sizeof(guarded); i++) assert(guarded[i] == 0xa5);
            if (capacity <= bytes) assert(rc < 0 && !size && (!capacity || !guarded[2]));
            else assert(!rc && size == bytes && !memcmp(guarded + 2, full, bytes + 1));
        }
        size_t size = 99; full[0] = 'x'; assert(listing(NULL, channel, kind, full, sizeof(full), &size) < 0 && !size && !full[0]);
        full[0] = 'x'; assert(listing(inst, channel, kind, full, sizeof(full), NULL) < 0 && !full[0]);
    }
    puts("PASS JSON: zero/tiny/exact capacities,canaries,NULL guards,no truncated success");
}

static void faults(struct UTUN_INSTANCE* inst, const char* channel) {
    const char* queries[] = {"SELECT c.channel_id", "SELECT 1 FROM sqlite_master WHERE type IN('table','view') AND name=", "SELECT count(*) FROM \"history_msg_",
        "SELECT ed25519_pubkey FROM \"peers_", "SELECT n.x25519_pubkey,n.name", "SELECT family,protocol,address", "SELECT json_object('id',m.id"};
    const unsigned kinds[] = {0, 0, 0, 0, 1, 1, 2}; const int bindings[] = {0, 1, 0, 0, 0, 1, 2}; unsigned cases = 0;
    for (unsigned q = 0; q < sizeof(kinds) / sizeof(kinds[0]); q++) {
        target = queries[q];
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            if (point == F_BIND && !bindings[q]) continue;
            for (int index = 1; index <= (point == F_BIND ? bindings[q] : 1); index++) {
                bind_index = index; step_number = 1; fault = point; rejected(inst, channel, kinds[q]); retry(inst, channel, kinds[q]); cases++;
            }
        }
    }
    const unsigned outer[] = {0, 4, 6};
    for (unsigned i = 0; i < 3; i++) {
        target = queries[outer[i]]; step_number = 2; fault = F_STEP; rejected(inst, channel, i); retry(inst, channel, i); cases++;
        int columns[] = {i == 0 ? 0 : i == 1 ? 1 : 0, i == 0 ? 1 : 4, 3};
        unsigned count = i == 0 ? 3 : i == 1 ? 2 : 1;
        for (unsigned column = 0; column < count; column++) {
            text_column = columns[column]; fault = F_TEXT; rejected(inst, channel, i); retry(inst, channel, i); cases++;
        }
        if (i < 2) { fault = F_ALLOCATE; rejected(inst, channel, i); retry(inst, channel, i); cases++; }
    }
    printf("PASS JSON:%u prepare/bind/step/finalize/text/allocation faults and retries,including failure after a completed row\n", cases);
}

static void corruption(struct UTUN_INSTANCE* inst, const char* channel) {
    sqlite3* db = inst->topo_sqlite_db; const char hidden[] = {'a', 0, 'b'}; char sql[256];
    execute(db, "BEGIN");
    execute(db, "UPDATE channels SET description_json='{\"name\":\"a\\u0000b\",\"ver\":1}'");
    rejected(inst, channel, 0); execute(db, "ROLLBACK");
    execute(db, "BEGIN"); set_text(db, "UPDATE nodes SET name=?", hidden, sizeof(hidden));
    rejected(inst, channel, 0); rejected(inst, channel, 1); execute(db, "ROLLBACK");
    execute(db, "BEGIN"); snprintf(sql, sizeof(sql), "UPDATE peers_%s SET adm_tags=?", channel); set_text(db, sql, hidden, sizeof(hidden));
    rejected(inst, channel, 1); execute(db, "ROLLBACK");
    char bad_x[160], bad_ed[160], bad_type[160];
    snprintf(bad_x, sizeof(bad_x), "UPDATE peers_%s SET x25519_pubkey=zeroblob(31)", channel);
    snprintf(bad_ed, sizeof(bad_ed), "UPDATE peers_%s SET ed25519_pubkey=zeroblob(33)", channel);
    snprintf(bad_type, sizeof(bad_type), "UPDATE peers_%s SET x25519_pubkey=printf('%%032d',1)", channel);
    const char* malformed[] = {bad_x, bad_ed, bad_type, "UPDATE node_addresses SET protocol=9", "UPDATE node_addresses SET address='abcd'"};
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        execute(db, "BEGIN"); execute(db, malformed[i]); rejected(inst, channel, 1); execute(db, "ROLLBACK");
    }
    execute(db, "PRAGMA foreign_keys=OFF;PRAGMA ignore_check_constraints=ON;BEGIN;UPDATE channels SET channel_id='01'"); rejected(inst, channel, 0); execute(db, "ROLLBACK;PRAGMA ignore_check_constraints=OFF;PRAGMA foreign_keys=ON");
    execute(db, "BEGIN;INSERT INTO ui_state(key,value) VALUES('JSON caller transaction','retained')");
    for (unsigned kind = 0; kind < 3; kind++) { retry(inst, channel, kind); assert(!sqlite3_get_autocommit(db)); }
    target = "SELECT json_object('id',m.id"; fault = F_FINALIZE; rejected(inst, channel, 2); assert(!sqlite3_get_autocommit(db));
    execute(db, "ROLLBACK");
    for (unsigned kind = 0; kind < 3; kind++) retry(inst, channel, kind);
    puts("PASS JSON: hidden NUL,invalid key/address/ID rows rejected;caller transaction preserved on success/failure");
}

static void deferred_channel(struct UTUN_INSTANCE* inst) {
    fail_sync = 1; assert(!chat_core_create_channel_auto(inst, "committed, activation pending") && !fail_sync);
    sqlite3_stmt* saved = NULL;
    assert(!sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT channel_id FROM channels WHERE name=\'committed, activation pending\'", -1, &saved, NULL));
    assert(sqlite3_step(saved) == SQLITE_ROW && sqlite3_column_bytes(saved, 0) == 64);
    char channel[65]; strcpy(channel, (const char*)sqlite3_column_text(saved, 0));
    assert(!sqlite3_finalize(saved) && !si_find(inst, channel));
    char out[65536]; size_t size = 0; assert(!chat_core_get_channels_json(inst, out, sizeof(out), &size)); valid_json(inst->topo_sqlite_db, out, size);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT json_extract(value,'$.msgs') FROM json_each(?1) "
        "WHERE json_extract(value,'$.id')=?2", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, out, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER && !sqlite3_column_int(st, 0));
    assert(sqlite3_finalize(st) == SQLITE_OK && !si_find(inst, channel));
    assert(!chat_core_ensure_channel_ready(inst, channel) && si_find(inst, channel));
    assert(!chat_core_get_channels_json(inst, out, sizeof(out), &size)); valid_json(inst->topo_sqlite_db, out, size);
    puts("PASS JSON: committed channel without history remains visible with zero messages;runtime retry succeeds");
}

static void parser_codes(sqlite3* db) {
    target = "SELECT json_valid"; bind_index = step_number = 1;
    for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
        fault = point; step_number = 1;
        int expected = point <= F_BIND ? SQLITE_NOMEM : SQLITE_IOERR;
        int rc = chat_json_strings_read(db, "{\"field\":\"value\"}", 128, NULL, NULL);
        assert(rc == expected && fault == F_NONE);
    }
    target = NULL;
    assert(chat_json_strings_read(db, "{\"field\":1}", 128, NULL, NULL) == SQLITE_CONSTRAINT);
    assert(!chat_json_strings_read(db, "{\"field\":\"value\"}", 128, NULL, NULL));
    puts("PASS JSON parser preserves primary prepare/bind/step/finalize error codes through cleanup");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); size_t baseline = u_get_allocated_count();
    utun_instance_set_tun_init_enabled(0); struct UASYNC* ua = uasync_create(); assert(ua);
    char directory[512]="/tmp/utun-json-XXXXXX",config[1024]; assert(!test_mkdtemp(directory));
    snprintf(config,sizeof(config),"[global]\ndb_path=%s\n[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n",directory);
    struct UTUN_INSTANCE* inst=utun_instance_create_from_str(ua,config);
    assert(inst && !utun_core_start(inst) && !chat_service_start(inst));
    parser_codes(inst->topo_sqlite_db);
    char name[64] = "Канал "; size_t name_bytes = strlen(name);
    for (int c = 1; c < 32; c++) name[name_bytes++] = (char)c;
    memcpy(name + name_bytes, " \"\\", 4);
    uint64_t id = chat_core_create_channel_auto(inst, name); assert(id); char channel[65];
    strcpy(channel, topo_groups_channel(inst->topo_groups, id));
    char json[4096]; size_t size = 0; assert(!chat_core_get_channels_json(inst, json, sizeof(json), &size));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT json_valid(?)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, json, (int)size, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int valid = sqlite3_column_int(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK);
    if (!valid) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JSON test: channel name with ASCII01 valid=%d bytes=%zu", valid, size);
    assert(valid); text_value(inst->topo_sqlite_db, json, "$[0].name", name);
    char node_name[256]; memset(node_name, '"', sizeof(node_name) - 1); node_name[sizeof(node_name) - 1] = 0;
    set_text(inst->topo_sqlite_db, "UPDATE nodes SET name=?", node_name, -1);
    char tags[8193]; memset(tags, 'a', sizeof(tags) - 1); memcpy(tags, "{\"pad\":\"", 8);
    memcpy(tags + sizeof(tags) - 3, "\"}", 3); char sql[512]; snprintf(sql, sizeof(sql), "UPDATE peers_%s SET adm_tags=?", channel);
    set_text(inst->topo_sqlite_db, sql, tags, -1);
    snprintf(sql, sizeof(sql), "DELETE FROM node_addresses;INSERT INTO node_addresses(node_x,family,protocol,address,port,rtt) VALUES"
        "(x'%s',4,1,x'7f000001',4000,7),(x'%s',4,6,x'7f000001',443,8),(x'%s',6,2,x'00000000000000000000000000000001',4001,9)",
        sc_public_key_hex(inst->my_keys.public_key, (char[65]){0}), sc_public_key_hex(inst->my_keys.public_key, (char[65]){0}),
        sc_public_key_hex(inst->my_keys.public_key, (char[65]){0})); execute(inst->topo_sqlite_db, sql);
    const char message[] = "Текст \x01 \"\\\n";
    struct chat_msg_submit req = {.inst = inst, .data = (uint8_t*)message, .data_len = sizeof(message) - 1};
    strcpy(req.channel_id, channel); strcpy(req.content_type, "text/plain"); assert(!chat_core_submit_message(inst, &req));
    char full[65536]; assert(!chat_core_get_members_json(inst, channel, full, sizeof(full), &size)); valid_json(inst->topo_sqlite_db, full, size);
    text_value(inst->topo_sqlite_db, full, "$[0].name", node_name); text_value(inst->topo_sqlite_db, full, "$[0].adm_tags", tags);
    text_value(inst->topo_sqlite_db, full, "$[0].addrs[0].ip", "127.0.0.1");
    text_value(inst->topo_sqlite_db, full, "$[0].addrs[1].proto", "TCP+REALITY"); text_value(inst->topo_sqlite_db, full, "$[0].addrs[2].ip", "::1");
    execute(inst->topo_sqlite_db, "BEGIN;UPDATE nodes SET ed25519_pubkey=zeroblob(32)");
    assert(!chat_core_get_members_json(inst, channel, full, sizeof(full), &size));
    char ed_hex[65]; sc_public_key_hex(inst->my_ed25519_pubkey, ed_hex);
    text_value(inst->topo_sqlite_db, full, "$[0].node_id", ed_hex);
    text_value(inst->topo_sqlite_db, full, "$[0].name", "");
    text_value(inst->topo_sqlite_db, full, "$[0].addrs", "[]");
    execute(inst->topo_sqlite_db, "ROLLBACK");
    assert(!chat_core_get_channels_json(inst, full, sizeof(full), &size)); text_value(inst->topo_sqlite_db, full, "$[0].owner_name", node_name);
    boundaries(inst, channel); faults(inst, channel); corruption(inst, channel); deferred_channel(inst);
    utun_instance_destroy(inst); uasync_poll(ua, 0); uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    puts("PASS JSON full escaping,complete outputs,SQL faults,deferred channel;allocations restored"); return 0;
}
