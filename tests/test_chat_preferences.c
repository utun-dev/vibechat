/* UI-state:ошибка завершения SQL не должна фиксировать значение. */
#define _GNU_SOURCE
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_setting.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Этот стенд инъецирует отказы синхронного API; диагностические запросы DB worker не разделяют его fault state. */
static __thread int fail_finalize;
enum fault { F_NONE, F_PREPARE, F_BIND, F_STEP, F_CHANGES, F_FINALIZE, F_EXEC, F_TEXT };
static __thread enum fault fault;
static __thread const char* target;
static __thread int argument, last_target, fail_rollback, observe_sound;
static unsigned broadcasts, autoconnect_calls;
static sqlite3* reader;
static char channel[65];

static int matches(const char* sql) { return sql && target && !strncmp(sql, target, strlen(target)); }
static int take(enum fault point, const char* sql, int index) {
    if (fault != point || (sql && !matches(sql)) || ((point == F_BIND || point == F_TEXT) && index != argument)) return 0;
    fault = F_NONE; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "preferences test: SQL fault point=%d index=%d target=%s", point, index, target); return 1;
}
extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (take(F_PREPARE, sql, 0)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* text, int bytes, void (*destroy)(void*)) {
    return take(F_BIND, sqlite3_sql(st), index) ? SQLITE_NOMEM : __real_sqlite3_bind_text(st, index, text, bytes, destroy);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    last_target = matches(sqlite3_sql(st));
    return take(F_STEP, sqlite3_sql(st), 0) ? SQLITE_IOERR : __real_sqlite3_step(st);
}
extern int __real_sqlite3_changes(sqlite3*);
int __wrap_sqlite3_changes(sqlite3* db) { return last_target && take(F_CHANGES, NULL, 0) ? 0 : __real_sqlite3_changes(db); }
extern const unsigned char* __real_sqlite3_column_text(sqlite3_stmt*, int);
const unsigned char* __wrap_sqlite3_column_text(sqlite3_stmt* st, int index) {
    return take(F_TEXT, sqlite3_sql(st), index) ? NULL : __real_sqlite3_column_text(st, index);
}
extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    if (fail_rollback && !strcmp(sql, "ROLLBACK")) { fail_rollback = 0; return SQLITE_IOERR; }
    return take(F_EXEC, sql, 0) ? SQLITE_IOERR : __real_sqlite3_exec(db, sql, fn, arg, error);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    const char* sql = st ? sqlite3_sql(st) : NULL;
    int fail = fail_finalize && sql && strstr(sql, "INTO ui_state");
    if (fail) fail_finalize = 0;
    fail |= sql && take(F_FINALIZE, sql, 0);
    int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_IOERR : rc;
}

static void value_is(sqlite3* db, const char* expected) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT value FROM ui_state WHERE key='preferences-test'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), expected));
    assert(sqlite3_finalize(st) == SQLITE_OK);
}

extern void __real_chat_sync_broadcast_sound_setting(struct UTUN_INSTANCE*, const char*, int);
void __wrap_chat_sync_broadcast_sound_setting(struct UTUN_INSTANCE* inst, const char* ch, int on) {
    if (observe_sound) {
        assert(sqlite3_get_autocommit(inst->topo_sqlite_db) && !strcmp(ch, channel));
        char key[128]; snprintf(key, sizeof(key), "ch_%s_sound_on_message", ch);
        sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(reader, "SELECT value FROM ui_state WHERE key=?", -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
        const char* text = (const char*)sqlite3_column_text(st, 0); assert(text && text[0] == '0' + on && !text[1]);
        assert(sqlite3_finalize(st) == SQLITE_OK); broadcasts++;
    }
    __real_chat_sync_broadcast_sound_setting(inst, ch, on);
}
extern void __real_chat_core_apply_group_autoconnect(struct UTUN_INSTANCE*);
void __wrap_chat_core_apply_group_autoconnect(struct UTUN_INSTANCE* inst) {
    autoconnect_calls++; __real_chat_core_apply_group_autoconnect(inst);
}

static void write_faults(struct UTUN_INSTANCE* inst, int sound) {
    const char* queries[] = {"SELECT value FROM ui_state", "INSERT INTO ui_state"}; unsigned cases = 0;
    for (size_t q = 0; q < 2; q++) for (enum fault point = F_PREPARE; point <= F_TEXT; point++) {
        if (point == F_EXEC || (point == F_CHANGES && !q) || (point == F_TEXT && q)) continue;
        for (int index = 1; index <= (q && point == F_BIND ? 2 : 1); index++) {
            assert(sound ? !chat_core_set_sound_on_message(inst, channel, 1) : !chat_core_save_ui_state(inst, "preferences-test", "old"));
            unsigned count = broadcasts; target = queries[q]; fault = point; argument = point == F_TEXT ? 0 : index;
            int rc = sound ? chat_core_set_sound_on_message(inst, channel, 0) : chat_core_save_ui_state(inst, "preferences-test", "new");
            assert(rc < 0 && fault == F_NONE && sqlite3_get_autocommit(inst->topo_sqlite_db) && broadcasts == count);
            if (sound) assert(chat_core_get_sound_on_message(inst, channel) == 1);
            else value_is(reader, "old");
            assert(sound ? !chat_core_set_sound_on_message(inst, channel, 0) : !chat_core_save_ui_state(inst, "preferences-test", "new"));
            assert(broadcasts == count + (unsigned)sound); cases++;
        }
    }
    const char* commands[] = {"BEGIN IMMEDIATE /* ui_state */", "COMMIT /* ui_state */"};
    for (size_t i = 0; i < 2; i++) {
        assert(sound ? !chat_core_set_sound_on_message(inst, channel, 1) : !chat_core_save_ui_state(inst, "preferences-test", "old"));
        unsigned count = broadcasts; target = commands[i]; fault = F_EXEC;
        int rc = sound ? chat_core_set_sound_on_message(inst, channel, 0) : chat_core_save_ui_state(inst, "preferences-test", "new");
        assert(rc < 0 && fault == F_NONE && broadcasts == count && sqlite3_get_autocommit(inst->topo_sqlite_db));
        if (sound) assert(chat_core_get_sound_on_message(inst, channel) == 1);
        else value_is(reader, "old");
        assert(sound ? !chat_core_set_sound_on_message(inst, channel, 0) : !chat_core_save_ui_state(inst, "preferences-test", "new")); cases++;
    }
    printf("PASS preferences writer sound=%d:%u SQL faults with unchanged reader/no broadcast and successful retries\n", sound, cases);
}

static int veto(void* arg) { (*(unsigned*)arg)++; return 1; }

static void read_faults(struct UTUN_INSTANCE* inst) {
    assert(!chat_core_set_sound_on_message(inst, channel, 0)); unsigned count = broadcasts, cases = 0;
    target = "SELECT value FROM ui_state";
    for (enum fault point = F_PREPARE; point <= F_TEXT; point++) {
        if (point == F_CHANGES || point == F_EXEC) continue;
        fault = point; argument = point == F_TEXT ? 0 : 1;
        assert(chat_core_get_sound_on_message(inst, channel) < 0 && fault == F_NONE);
        assert(chat_core_get_sound_on_message(inst, channel) == 0 && broadcasts == count); cases++;
    }
    const char* bad[] = {"'yes'", "'2'", "NULL", "X'31'", "CAST(X'3100' AS TEXT)"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char sql[256]; snprintf(sql, sizeof(sql), "UPDATE ui_state SET value=%s WHERE key='ch_%s_sound_on_message'", bad[i], channel);
        assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK && chat_core_get_sound_on_message(inst, channel) < 0);
    }
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE ui_state SET value='0' WHERE key='ch_%s_sound_on_message'", channel);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK && chat_core_get_sound_on_message(inst, channel) == 0);
    assert(chat_core_get_sound_on_message(NULL, channel) < 0 && chat_core_set_sound_on_message(inst, "01", 1) < 0);
    assert(broadcasts == count);
    printf("PASS preferences reader:%u SQL faults/retries;invalid booleans,types,NUL,and channel rejected\n", cases);
}

static void semantics(struct UTUN_INSTANCE* inst) {
    assert(!chat_core_save_ui_state(inst, "preferences-test", "old") && !chat_core_set_sound_on_message(inst, channel, 1));
    unsigned commits = 0, count = broadcasts; sqlite3_commit_hook(inst->topo_sqlite_db, veto, &commits);
    assert(!chat_core_save_ui_state(inst, "preferences-test", "old") && !chat_core_set_sound_on_message(inst, channel, 1));
    assert(!commits && broadcasts == count + 1);
    assert(chat_core_set_sound_on_message(inst, channel, 0) < 0 && commits == 1 && broadcasts == count + 1);
    sqlite3_commit_hook(inst->topo_sqlite_db, NULL, NULL); assert(chat_core_get_sound_on_message(inst, channel) == 1);
    assert(!chat_core_set_sound_on_message(inst, channel, 0));
    const char* modes[] = {"ABORT", "IGNORE"};
    for (size_t i = 0; i < 2; i++) {
        char sql[256]; snprintf(sql, sizeof(sql), "CREATE TRIGGER reject_pref BEFORE UPDATE ON ui_state BEGIN SELECT RAISE(%s%s);END",
            modes[i], i ? "" : ",'injected preference error'");
        assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); count = broadcasts;
        assert(chat_core_set_sound_on_message(inst, channel, 1) < 0 && broadcasts == count && chat_core_get_sound_on_message(inst, channel) == 0);
        assert(sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER reject_pref", NULL, NULL, NULL) == SQLITE_OK);
    }
    assert(!chat_core_set_sound_on_message(inst, channel, 1));
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN;INSERT INTO ui_state VALUES('pref-foreign','1')", NULL, NULL, NULL) == SQLITE_OK);
    count = broadcasts; assert(chat_core_set_sound_on_message(inst, channel, 1) < 0 && chat_core_save_ui_state(inst, "preferences-test", "old") < 0);
    assert(!sqlite3_get_autocommit(inst->topo_sqlite_db) && broadcasts == count);
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    unsigned applied = autoconnect_calls; struct chat_setting_state before = inst->chat_settings;
    assert(chat_core_set_setting(inst, "group_autoconnect", "bogus") == -2 && autoconnect_calls == applied);
    assert(!memcmp(&before, &inst->chat_settings, sizeof(before)) && chat_core_set_setting(inst, "unknown-pref", "1") == -1);
    assert(!chat_core_set_setting(inst, "group_autoconnect", "0") && autoconnect_calls == applied + 1);
    assert(chat_core_set_setting(NULL, "group_autoconnect", "0") == -2 && autoconnect_calls == applied + 1);
    chat_core_save_ui_state_trampoline(NULL); chat_core_set_setting_trampoline(NULL); chat_core_set_sound_on_message_trampoline(NULL);
    struct save_ui_state_arg* a = u_malloc(sizeof(*a)); assert(a); a->inst = inst; memset(a->data, 'x', sizeof(a->data));
    chat_core_save_ui_state_trampoline(a);
    a = u_malloc(sizeof(*a)); assert(a); a->inst = inst; memset(a->data, 'x', sizeof(a->data)); a->data[1] = 0;
    chat_core_save_ui_state_trampoline(a);
    struct chat_setting_arg* b = u_malloc(sizeof(*b)); assert(b); b->inst = inst; memset(b->data, 'x', sizeof(b->data));
    chat_core_set_setting_trampoline(b);
    struct chat_sound_arg* c = u_malloc(sizeof(*c)); assert(c); c->inst = inst; memset(c->ch_id, 'x', sizeof(c->ch_id));
    c->on = 0; count = broadcasts; chat_core_set_sound_on_message_trampoline(c); assert(broadcasts == count);
    puts("PASS preferences:no-op retries delivery without commit,veto/ABORT/IGNORE/foreign txn,validation and malformed requests");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); size_t baseline = u_get_allocated_count();
    utun_instance_set_tun_init_enabled(0); struct UASYNC* ua = uasync_create(); assert(ua);
    char directory[512] = "/tmp/utun-preferences-XXXXXX"; assert(!test_mkdtemp(directory)); char config[1024], database[600];
    snprintf(config, sizeof(config), "[global]\nmy_node_name=preferences test\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !utun_core_start(inst) && !chat_service_start(inst));
    snprintf(database, sizeof(database), "%s/chats.db", directory);
    assert(sqlite3_open_v2(database, &reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(!chat_core_save_ui_state(inst, "preferences-test", "old")); value_is(reader, "old");
    fail_finalize = 1; assert(chat_core_save_ui_state(inst, "preferences-test", "new") < 0 && !fail_finalize);
    value_is(reader, "old");
    uint64_t group = chat_core_create_channel_auto(inst, "preferences"); assert(group);
    strcpy(channel, topo_groups_channel(inst->topo_groups, group)); observe_sound = 1;
    assert(chat_core_get_sound_on_message(inst, channel) == 1); write_faults(inst, 0); write_faults(inst, 1); read_faults(inst); semantics(inst);
    assert(!chat_core_save_ui_state(inst, "preferences-test", "old")); target = "COMMIT /* ui_state */";
    fault = F_EXEC; fail_rollback = 1; inst->running = 1;
    assert(chat_core_save_ui_state(inst, "preferences-test", "uncommitted") < 0 && !fail_rollback && !inst->running);
    assert(!sqlite3_get_autocommit(inst->topo_sqlite_db)); value_is(reader, "old");
    utun_instance_destroy(inst); assert(sqlite3_close(reader) == SQLITE_OK);
    uasync_poll(ua, 0); uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    puts("PASS preferences:failed finalize preserves committed UI state"); return 0;
}
