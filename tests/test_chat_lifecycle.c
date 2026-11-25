#include "../src/group/group_protocol.h"
/* Старт chat_core не должен игнорировать отказ обязательной подписки. */
#define _GNU_SOURCE
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/member_sync.h"
#include "../src/chat/db_sync.h"
#include "../src/routing_layer/topo_group.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <openssl/sha.h>

static int fail_props, fail_routes, fail_malloc, fail_context, fail_rollback, fail_sync;
static int fail_done_subscription;
enum fault { F_NONE, F_PREPARE, F_BIND, F_STEP, F_CHANGES, F_FINALIZE, F_EXEC, F_CATALOG };
static enum fault fault;
static const char* target;
static int argument, last_target;
static sqlite3* reader;
static struct nodeinfo_cbk_entry* preserved_callback;
static unsigned ready;
static uint64_t route_group;
static int delete_from_route_event, route_inspect;
static unsigned fail_route_event;
static unsigned route_seen, route_events;
static pthread_t loop_thread;
extern int __real_member_sync_add_props_cbk(struct UTUN_INSTANCE*, node_props_changed_fn, void*);
int __wrap_member_sync_add_props_cbk(struct UTUN_INSTANCE* inst, node_props_changed_fn callback, void* arg) {
    if (fail_props) { fail_props = 0; fail_malloc = 1; }
    return __real_member_sync_add_props_cbk(inst, callback, arg);
}
extern int __real_utun_add_nodeinfo_cbk(struct UTUN_INSTANCE*, nodeinfo_cbk_fn, void*);
int __wrap_utun_add_nodeinfo_cbk(struct UTUN_INSTANCE* inst, nodeinfo_cbk_fn fn, void* arg) {
    if (fail_routes) { fail_routes = 0; fail_malloc = 2; }
    return __real_utun_add_nodeinfo_cbk(inst, fn, arg);
}
extern void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* location) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_u_malloc_impl(bytes, location);
    if ((fail_malloc == 1 && strstr(location, "member_sync.c")) || (fail_malloc == 2 && strstr(location, "topo_group.c"))) {
        fail_malloc = 0; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "lifecycle test: subscription allocation refused at %s", location); return NULL;
    }
    return __real_u_malloc_impl(bytes, location);
}
extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* location) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_u_calloc_impl(count, bytes, location);
    if (fail_done_subscription && strstr(location, "db_sync.c") && !--fail_done_subscription) return NULL;
    if (fail_context && strstr(location, "chat_core.c")) { fail_context = 0; return NULL; }
    if (fail_route_event && strstr(location, "chat_member.c") && !--fail_route_event) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "lifecycle test: deferred route allocation refused at %s", location); return NULL;
    }
    return __real_u_calloc_impl(count, bytes, location);
}
static int matches(const char* sql) { return pthread_equal(loop_thread, pthread_self()) && sql && target && !strncmp(sql, target, strlen(target)); }
static int take(enum fault point, const char* sql, int index) {
    if (!pthread_equal(loop_thread, pthread_self())) return 0;
    if (fault != point || (sql && !matches(sql)) || (point == F_BIND && index != argument)) return 0;
    fault = F_NONE; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "lifecycle test: SQL fault point=%d index=%d target=%s", point, index, target); return 1;
}
extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (take(F_PREPARE, sql, 0)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* value, int bytes, void (*destroy)(void*)) {
    return take(F_BIND, sqlite3_sql(st), index) ? SQLITE_NOMEM : __real_sqlite3_bind_text(st, index, value, bytes, destroy);
}
extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int bytes, void (*destroy)(void*)) {
    return take(F_BIND, sqlite3_sql(st), index) ? SQLITE_NOMEM : __real_sqlite3_bind_blob(st, index, value, bytes, destroy);
}
extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    return take(F_BIND, sqlite3_sql(st), index) ? SQLITE_NOMEM : __real_sqlite3_bind_int64(st, index, value);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_sqlite3_step(st);
    last_target = matches(sqlite3_sql(st)); return take(F_STEP, sqlite3_sql(st), 0) ? SQLITE_IOERR : __real_sqlite3_step(st);
}
extern int __real_sqlite3_changes(sqlite3*);
int __wrap_sqlite3_changes(sqlite3* db) { return last_target && take(F_CHANGES, NULL, 0) ? 0 : __real_sqlite3_changes(db); }
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && take(F_FINALIZE, sqlite3_sql(st), 0), rc = __real_sqlite3_finalize(st); return fail ? SQLITE_IOERR : rc;
}
extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_sqlite3_exec(db, sql, fn, arg, error);
    if (fail_rollback && !strcmp(sql, "ROLLBACK")) { fail_rollback = 0; return SQLITE_IOERR; }
    return take(F_EXEC, sql, 0) ? SQLITE_IOERR : __real_sqlite3_exec(db, sql, fn, arg, error);
}
extern int __real_sqlite3_get_table(sqlite3*, const char*, char***, int*, int*, char**);
int __wrap_sqlite3_get_table(sqlite3* db, const char* sql, char*** result, int* rows, int* columns, char** error) {
    if (take(F_CATALOG, sql, 0)) { *result = NULL; *rows = 0; *columns = 0; if (error) *error = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_get_table(db, sql, result, rows, columns, error);
}
extern struct DB_SYNC_INSTANCE* __real_db_sync_instance_activate(struct UTUN_INSTANCE*, const struct db_sync_bootstrap*);
struct DB_SYNC_INSTANCE* __wrap_db_sync_instance_activate(struct UTUN_INSTANCE* inst, const struct db_sync_bootstrap* prepared) {
    if (fail_sync && !--fail_sync) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "lifecycle test: sync allocation refused group=%s", prepared->channel); return NULL;
    }
    return __real_db_sync_instance_activate(inst, prepared);
}

static void snapshot(sqlite3* db, uint8_t out[32]) {
    sqlite3_int64 bytes = 0; unsigned char* image = sqlite3_serialize(db, "main", &bytes, 0);
    assert(image && bytes > 0 && SHA256(image, (size_t)bytes, out)); sqlite3_free(image);
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    if (type == CHAT_EVT_MEMBER_UPDATED && route_group && bytes > 0 && data[0] == 64 && bytes > data[0]) {
        char ch[65]; memcpy(ch, data + 1, data[0]); ch[data[0]] = 0;
        if (topo_groups_channel_slot(inst->topo_groups, ch) == route_group) {
            route_events++;
            if (delete_from_route_event) { delete_from_route_event = 0; chat_core_delete_channel(inst, ch); }
        }
    }
    if (type != CHAT_EVT_DB_READY) return;
    assert(chat_core_is_initialized(inst) && sqlite3_get_autocommit(inst->topo_sqlite_db));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(reader, "SELECT x25519_pubkey,ed25519_pubkey FROM local_identity WHERE id=1", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), inst->my_keys.public_key, 32));
    assert(sqlite3_column_bytes(st, 1) == 32 && !memcmp(sqlite3_column_blob(st, 1), inst->my_ed25519_pubkey, 32));
    assert(sqlite3_finalize(st) == SQLITE_OK); ready++;
}

static void refused(struct UTUN_INSTANCE* inst, const char* path) {
    uint8_t before[32], after[32]; snapshot(reader, before); ready = 0; size_t allocations = u_get_allocated_count();
    assert(chat_core_init(inst, path) < 0 && !fault && !fail_props && !fail_routes && !fail_malloc && !fail_context);
    assert(!ready && !inst->chat_core && inst->core_started && sqlite3_get_autocommit(inst->topo_sqlite_db));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && inst->nodeinfo_cbks == preserved_callback);
    assert(u_get_allocated_count() == allocations);
}

static void retry(struct UTUN_INSTANCE* inst, const char* path) {
    ready = 0; assert(!chat_core_init(inst, path) && ready == 1 && !chat_core_init(inst, path) && ready == 1);
    assert(chat_core_init(inst, "different/chats.db") < 0 && ready == 1);
    chat_core_destroy(inst); assert(!inst->chat_core && inst->nodeinfo_cbks == preserved_callback);
}

static int deny_accounts(void* arg, int action, const char* name, const char* a, const char* db, const char* source) {
    (void)a; (void)db; (void)source;
    if (action == SQLITE_READ && name && !strcmp(name, "sqlite_master")) { (*(unsigned*)arg)++; return SQLITE_DENY; }
    return SQLITE_OK;
}

static int veto(void* arg) { (*(unsigned*)arg)++; return 1; }
static void preserved(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, void* arg) {
    if (route_inspect) {
        assert(group->group_id == route_group && node->node_id == ((struct UTUN_INSTANCE*)arg)->node_id); route_seen++;
    }
}

static void sql_faults(struct UTUN_INSTANCE* inst, const char* path) {
    const char* queries[] = {"INSERT OR REPLACE INTO local_identity", "SELECT value FROM ui_state WHERE key='media_base'"}; unsigned cases = 0;
    for (size_t q = 0; q < 2; q++) for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
        if (q && (point == F_BIND || point == F_CHANGES)) continue;
        for (int index = 1; index <= (!q && point == F_BIND ? 5 : 1); index++) {
            target = queries[q]; fault = point; argument = index; refused(inst, path); retry(inst, path); cases++;
        }
    }
    target = "SELECT type,sql FROM sqlite_master WHERE name=?"; fault = F_PREPARE;
    refused(inst, path); retry(inst, path); cases++;
    const char* commands[] = {"BEGIN IMMEDIATE /* chat init */", "COMMIT /* chat init */"};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        target = commands[i]; fault = F_EXEC; refused(inst, path); retry(inst, path); cases++;
    }
    target = "SELECT channel_id,length"; fault = F_CATALOG; refused(inst, path); retry(inst, path); cases++;
    unsigned commits = 0; sqlite3_commit_hook(inst->topo_sqlite_db, veto, &commits); refused(inst, path);
    sqlite3_commit_hook(inst->topo_sqlite_db, NULL, NULL); assert(commits == 1); retry(inst, path);
    fail_context = 1; refused(inst, path); retry(inst, path);
    fail_props = 1; refused(inst, path); retry(inst, path); fail_routes = 1; refused(inst, path); retry(inst, path); target = NULL;
    printf("PASS lifecycle:%u SQL faults/retries,commit veto,context/properties/routes allocation;exact DB,callbacks,and ready preserved\n", cases);
}

static sqlite3_int64 number(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    sqlite3_int64 result = sqlite3_column_int64(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return result;
}

static void validation(struct UTUN_INSTANCE* inst, const char* path) {
    uint8_t before[32], after[32]; snapshot(reader, before); ready = 0;
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN;CREATE TABLE lifecycle_outer(value);INSERT INTO lifecycle_outer VALUES(7)", NULL, NULL, NULL) == SQLITE_OK);
    void* sync_before = inst->db_sync;
    assert(chat_service_start(inst) < 0 && inst->db_sync == sync_before);
    assert(chat_core_init(inst, path) < 0 && !inst->chat_core && !sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(number(inst->topo_sqlite_db, "SELECT value FROM lifecycle_outer") == 7);
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    char long_path[513]; memset(long_path, 'x', sizeof(long_path)); long_path[512] = 0;
    assert(chat_core_init(inst, long_path) < 0 && chat_core_init(NULL, path) < 0 && chat_core_init(inst, NULL) < 0);
    chat_core_destroy(NULL); snapshot(reader, after); assert(!memcmp(before, after, 32) && !ready && !inst->chat_core);
    assert(number(reader, "SELECT count(*) FROM accounts") == 0);
    const char* modes[] = {"ABORT", "IGNORE"};
    for (size_t i = 0; i < 2; i++) {
        char sql[256]; snprintf(sql, sizeof(sql), "CREATE TRIGGER reject_identity BEFORE INSERT ON local_identity BEGIN SELECT RAISE(%s%s);END",
            modes[i], i ? "" : ",'injected identity rejection'");
        assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); refused(inst, path);
        assert(sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER reject_identity", NULL, NULL, NULL) == SQLITE_OK); retry(inst, path);
    }
    puts("PASS lifecycle:foreign transaction preserved,NULL/oversized path rejected,ABORT/IGNORE rollback,no demonstration contacts");
}

static void activation(struct UTUN_INSTANCE* inst, const char* path) {
    ready = 0; assert(!chat_service_start(inst) && ready == 1);
    uint64_t groups[2] = {chat_core_create_channel_auto(inst, "first saved"), chat_core_create_channel_auto(inst, "second saved")};
    assert(groups[0] && groups[1]);
    struct chat_msg_submit request = {.inst = inst, .data = (uint8_t*)"startup retained", .data_len = 16};
    char second[65]; strcpy(second, topo_groups_channel(inst->topo_groups, groups[1]));
    strcpy(request.channel_id, topo_groups_channel(inst->topo_groups, groups[0])); strcpy(request.content_type, "text/plain");
    assert(!chat_core_submit_message(inst, &request)); group_service_stop(inst);
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT rowid FROM channels WHERE channel_id='%s'", request.channel_id);
    sqlite3_int64 row = number(reader, sql);
    assert(sqlite3_exec(inst->topo_sqlite_db,"PRAGMA foreign_keys=OFF; PRAGMA ignore_check_constraints=ON",NULL,NULL,NULL)==SQLITE_OK);
    const char* invalid[] = {"'01'", "CAST(X'313200' AS TEXT)", "'9223372036854775808'"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        snprintf(sql, sizeof(sql), "UPDATE channels SET channel_id=%s WHERE rowid=%lld", invalid[i], (long long)row);
        assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); refused(inst, path);
        snprintf(sql, sizeof(sql), "UPDATE channels SET channel_id='%s' WHERE rowid=%lld", request.channel_id, (long long)row);
        assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    }
    assert(sqlite3_exec(inst->topo_sqlite_db,"PRAGMA ignore_check_constraints=OFF; PRAGMA foreign_keys=ON",NULL,NULL,NULL)==SQLITE_OK);
    ready = 0; fail_sync = 2;
    assert(chat_service_start(inst) < 0 && !fail_sync && !inst->chat_started && inst->core_started);
    group_service_stop(inst); ready = 0;
    assert(!topo_groups_find_channel(inst->topo_groups, request.channel_id) && !topo_groups_find_channel(inst->topo_groups, second));
    assert(inst->nodeinfo_cbks == preserved_callback && number(reader, "SELECT count(*) FROM channels") == 2);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM msg_%s", request.channel_id); assert(number(reader, sql) == 1);
    assert(!chat_service_start(inst) && ready == 1 && chat_core_count(inst, request.channel_id) == 1);
    assert(topo_groups_find_channel(inst->topo_groups, request.channel_id) && topo_groups_find_channel(inst->topo_groups, second)); group_service_stop(inst);
    puts("PASS lifecycle:corrupt catalog rejected;second channel activation failure cleans all runtime,keeps data,and retries without false ready");
}

static void wait_members(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000;
    while (CC(inst) && (CC(inst)->member_reads || CC(inst)->member_route_events) && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(CC(inst) && !CC(inst)->member_reads && !CC(inst)->member_route_events);
}

static void route_notifications(struct UTUN_INSTANCE* inst, struct UASYNC* ua) {
    ready = 0; assert(!chat_service_start(inst) && ready == 1);
    route_group = chat_core_create_channel_auto(inst, "delete after route dispatch"); assert(route_group); wait_members(inst);
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, route_group); assert(group && group->local_node);
    route_seen = route_events = 0; route_inspect = delete_from_route_event = 1;
    topo_fire_nodeinfo_cbk(inst, group, group->local_node);
    topo_fire_nodeinfo_cbk(inst, group, group->local_node);
    assert(group->local_node->node_id == inst->node_id && route_seen == 2 && delete_from_route_event && !route_events);
    route_inspect = 0; wait_members(inst);
    assert(!delete_from_route_event && route_events == 1 && !topo_groups_find(inst->topo_groups, route_group));
    route_group = chat_core_create_channel_auto(inst, "cancel pending route event"); assert(route_group); wait_members(inst);
    group = topo_groups_find(inst->topo_groups, route_group); assert(group && group->local_node);
    route_events = 0;
    for (unsigned failure = 1; failure <= 2; failure++) {
        fail_route_event = failure; topo_fire_nodeinfo_cbk(inst, group, group->local_node); assert(!fail_route_event);
        uasync_poll(ua, 0); assert(!route_events);
    }
    topo_fire_nodeinfo_cbk(inst, group, group->local_node); assert(!route_events);
    route_group = 0; group_service_stop(inst);
    uasync_poll(ua, 0); assert(!route_events);
    puts("PASS lifecycle:route observers and caller retain live group/node until dispatch returns;deferred event may delete channel");
}

int main(void) {
    loop_thread = pthread_self();
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); size_t baseline = u_get_allocated_count();
    utun_instance_set_tun_init_enabled(0); struct UASYNC* ua = uasync_create(); assert(ua);
    char directory[512] = "/tmp/utun-lifecycle-XXXXXX"; assert(!test_mkdtemp(directory)); char config[1024], database[600];
    snprintf(config, sizeof(config), "[global]\nmy_node_name=lifecycle test\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst && !utun_core_start(inst));
    chat_event_set_handler(inst, event); snprintf(database, sizeof(database), "%s/chats.db", directory);
    assert(sqlite3_open_v2(database, &reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(!utun_add_nodeinfo_cbk(inst, preserved, inst)); preserved_callback = inst->nodeinfo_cbks;
    unsigned denied = 0; assert(sqlite3_set_authorizer(inst->topo_sqlite_db, deny_accounts, &denied) == SQLITE_OK);
    refused(inst, database); assert(denied == 1); assert(sqlite3_set_authorizer(inst->topo_sqlite_db, NULL, NULL) == SQLITE_OK);
    retry(inst, database); sql_faults(inst, database); validation(inst, database); activation(inst, database); route_notifications(inst, ua);
    for (int failure = 1; failure <= 2; failure++) {
        group_service_stop(inst); assert(!db_sync_enable(inst) && !chat_core_init(inst, database));
        fail_done_subscription = failure;
        assert(chat_media_startup_backfill_init(inst) < 0 && !fail_done_subscription && !CC(inst)->media_backfill_inst);
        assert(!chat_media_startup_backfill_init(inst) && !chat_media_startup_backfill_init(inst));
        group_service_stop(inst);
    }
    uint8_t before[32], after[32]; snapshot(reader, before); ready = 0;
    target = "COMMIT /* chat init */"; fault = F_EXEC; fail_rollback = 1; inst->running = 1;
    assert(chat_core_init(inst, database) < 0 && fault == F_NONE && !fail_rollback && !inst->running && !inst->chat_core);
    assert(!ready && !sqlite3_get_autocommit(inst->topo_sqlite_db) && inst->nodeinfo_cbks == preserved_callback);
    snapshot(reader, after); assert(!memcmp(before, after, 32)); utun_instance_destroy(inst);
    snapshot(reader, after); assert(!memcmp(before, after, 32));
    assert(sqlite3_close(reader) == SQLITE_OK);
    uasync_poll(ua, 0); uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    puts("PASS lifecycle:subscription failure aborts startup without ready events;core survives and retry succeeds"); return 0;
}
