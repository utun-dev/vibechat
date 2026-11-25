#define _GNU_SOURCE
/* Файловый READ/WRITE локальных mutes: отсутствие SQL в loop, FIFO, rollback и смена контекста. */
#include "../src/radio/radio.c"
#include "../src/db_schema.h"
#include "../lib/db_sql.h"
#include <assert.h>
#include <pthread.h>
#define CHANNEL "0707070707070707070707070707070707070707070707070707070707070707"

static pthread_t loop_thread;
static int forbid_sql, read_fault, fail_rows, veto_commit;
static __thread int fail_request;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_read, entered, released;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    const char prefix[] = "SELECT ed25519_pubkey FROM \"peers_";
    if (!strncmp(sql, prefix, sizeof(prefix) - 1)) {
        int fault = __atomic_exchange_n(&read_fault, 0, __ATOMIC_SEQ_CST);
        if (fault) { *st = NULL; return fault; }
        pthread_mutex_lock(&lock);
        if (pause_read) {
            entered = 1; pthread_cond_broadcast(&cond);
            while (!released) pthread_cond_wait(&cond, &lock);
            pause_read = 0;
        }
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (fail_request && pthread_equal(loop_thread, pthread_self()) && strstr(where, "radio.c:")) { fail_request = 0; return NULL; }
    return __real_u_calloc_impl(count, bytes, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t bytes, const char* where) {
    if (!pthread_equal(loop_thread, pthread_self()) && strstr(where, "radio.c:") &&
        __atomic_exchange_n(&fail_rows, 0, __ATOMIC_SEQ_CST)) return NULL;
    return __real_u_realloc_impl(ptr, bytes, where);
}
/* Отдельная проверка механизма БД: публикация NODEINFO имеет собственные тесты и здесь не выполняется. */
void __wrap_topo_group_set_radio(struct TOPO_GROUP* group, int on) {
    assert(pthread_equal(loop_thread, pthread_self())); group->radio_active = !!on;
}
static void ready(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); assert(!(*(int*)arg)++);
}
static int commit_hook(void* arg) { (void)arg; return __atomic_exchange_n(&veto_commit, 0, __ATOMIC_SEQ_CST); }
static int seed_work(sqlite3* db, void* arg) {
    (void)arg; assert(!db_schema_table(db, "peers_" CHANNEL "", NULL));
    assert(sqlite3_exec(db, "INSERT INTO peers_" CHANNEL "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,radio_muted)"
        " VALUES(4,zeroblob(32),x'0400000000000000000000000000000000000000000000000000000000000000',zeroblob(64),1,0),(5,zeroblob(32),x'0500000000000000000000000000000000000000000000000000000000000000',zeroblob(64),1,1),"
        "(6,zeroblob(32),x'0600000000000000000000000000000000000000000000000000000000000000',zeroblob(64),1,0)", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_commit_hook(db, commit_hook, NULL); return SQLITE_OK;
}
static void idle(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000;
    while (db_async_pending(inst->db_worker) && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(!db_async_pending(inst->db_worker));
}
static void arm_pause(void) {
    pthread_mutex_lock(&lock); pause_read = 1; entered = released = 0; pthread_mutex_unlock(&lock);
}
static void wait_paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) {
        uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock);
    }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void tick(void* arg) { *(int*)arg = 1; }
struct reply { int calls, result, muted; };
static void saved(void* arg, int result, int muted) {
    struct reply* reply = arg; assert(pthread_equal(loop_thread, pthread_self()) && !reply->calls++);
    reply->result = result; reply->muted = muted;
}
static const uint8_t member_ed[32] = {5};
static unsigned audio_events;
static int delete_from_audio;
static void audio(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], int muted, void* arg) {
    (void)arg; assert(group == 7 && !memcmp(ed, member_ed, 32) && radio_member_muted(inst, group, ed) == muted); audio_events++;
    if (delete_from_audio) { delete_from_audio = 0; radio_destroy(topo_groups_find(inst->topo_groups, group)); }
}
static void write_mute(struct UTUN_INSTANCE* inst, int value, int result, int muted) {
    struct reply reply = {0}; radio_member_mute_save(inst, 7, member_ed, value, saved, &reply); idle(inst);
    assert(reply.calls == 1 && reply.result == result && (result || reply.muted == muted));
    if (!result) assert(radio_member_muted(inst, 7, member_ed) == muted);
}
static void read_stored(void* arg, const struct db_sql_result* result) {
    int* value = arg; assert(!result->outcome.status && result->rows == 1 && result->columns == 1);
    assert(result->values[0].type == SQLITE_INTEGER); *value = (int)result->values[0].i64;
}
static void check_stored(struct UTUN_INSTANCE* inst, int expected) {
    int value = -1; db_query(inst->db_worker, "SELECT radio_muted FROM peers_" CHANNEL " WHERE node_id=5", DB_NO_ARGS, read_stored, &value);
    idle(inst); assert(value == expected);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua); char path[] = "/tmp/utun-radio-mutes-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=4}; struct TOPO_GROUPS groups = {.instance=&inst}; inst.topo_groups = &groups;
    groups.group_list = queue_new(ua, 16, offsetof(struct TOPO_GROUP, group_id) - sizeof(struct ll_entry), 8, "radio mute group");
    assert(groups.group_list); struct ll_entry* entry = queue_entry_new(sizeof(struct TOPO_GROUP) - sizeof(*entry)); assert(entry);
    struct TOPO_GROUP* group = (struct TOPO_GROUP*)entry; group->instance = &inst; group->group_id = 7;
    group->group_type = TOPO_GROUP_TYPE_CHAT; strcpy(group->channel_id, CHANNEL); assert(queue_data_put_with_index(groups.group_list, entry) >= 0);
    assert(!radio_init_instance(&inst) && !radio_init(group)); radio_set_mute_cb(&inst, audio, NULL);
    forbid_sql = 1; int opened = 0; inst.db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst.db_worker);
    idle(&inst); assert(opened); int seeded = 0;
    db_async_submit(inst.db_worker, "seed radio", DB_ASYNC_WRITE, seed_work, &seeded, ready); idle(&inst); assert(seeded);
    assert(!inst.topo_sqlite_db); arm_pause(); radio_set_active(&inst, 7, 1); wait_paused(&inst);
    radio_set_active(&inst, 7, 1); assert(db_async_pending(inst.db_worker) == 1 && !group->radio_active && radio_start_pending(&inst, 7));
    int alive = 0; assert(uasync_set_timeout(ua, 10, &alive, tick, "radio mute live"));
    uint64_t end = get_time_us() + 5000000; while (!alive && get_time_us() < end) uasync_poll(ua, 1); assert(alive);
    radio_set_active(&inst, 7, 0); assert(!radio_start_pending(&inst, 7)); unpause(); idle(&inst);
    assert(!group->radio_active && radio_member_muted(&inst, 7, member_ed));
    radio_set_active(&inst, 7, 1); assert(group->radio_active); radio_set_active(&inst, 7, 0);
    radio_destroy(group); assert(!radio_init(group)); __atomic_store_n(&read_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST);
    radio_set_active(&inst, 7, 1); idle(&inst);
    assert(!group->radio_active && !radio_start_pending(&inst, 7) && !radio_of(group)->muted_loaded);
    radio_set_active(&inst, 7, 1); idle(&inst); assert(group->radio_active && radio_member_muted(&inst, 7, member_ed));
    radio_set_active(&inst, 7, 0); assert(radio_set_member_muted(&inst, 7, member_ed, 0) < 0);
    struct reply first = {0}, second = {0};
    radio_member_mute_save(&inst, 7, member_ed, 2, saved, &first); radio_member_mute_save(&inst, 7, member_ed, 2, saved, &second); idle(&inst);
    assert(first.calls == 1 && !first.result && !first.muted && second.calls == 1 && !second.result && second.muted && audio_events == 2);
    check_stored(&inst, 1); unsigned old_audio = audio_events;
    fail_request = 1; write_mute(&inst, 0, -1, 0); assert(!fail_request);
    __atomic_store_n(&fail_rows, 1, __ATOMIC_SEQ_CST); write_mute(&inst, 1, -1, 0); check_stored(&inst, 1);
    __atomic_store_n(&veto_commit, 1, __ATOMIC_SEQ_CST); write_mute(&inst, 0, -1, 0); check_stored(&inst, 1);
    __atomic_store_n(&read_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST); write_mute(&inst, 0, -1, 0); check_stored(&inst, 1);
    assert(audio_events == old_audio && radio_member_muted(&inst, 7, member_ed)); write_mute(&inst, 0, 0, 0); check_stored(&inst, 0);
    /* WRITE после UPDATE ждёт диска; старый runtime удалён и заново создан до completion. */
    arm_pause(); struct reply detached = {0}; radio_member_mute_save(&inst, 7, member_ed, 1, saved, &detached); wait_paused(&inst);
    radio_destroy(group); assert(!radio_init(group)); unpause(); idle(&inst);
    assert(detached.calls == 1 && detached.result == -1 && !radio_of(group)->muted_loaded); check_stored(&inst, 1);
    /* Ожидающий WRITE отменяется guard до SQL после уничтожения его контекста. */
    arm_pause(); radio_set_active(&inst, 7, 1); wait_paused(&inst); struct reply cancelled = {0};
    radio_member_mute_save(&inst, 7, member_ed, 0, saved, &cancelled); radio_destroy(group); assert(!radio_init(group));
    unpause(); idle(&inst); assert(cancelled.calls == 1 && cancelled.result == -1 && !group->radio_active); check_stored(&inst, 1);
    radio_set_active(&inst, 7, 1); idle(&inst); assert(group->radio_active && radio_member_muted(&inst, 7, member_ed));
    radio_set_active(&inst, 7, 0); delete_from_audio = 1;
    struct reply audio_deleted = {0}; radio_member_mute_save(&inst, 7, member_ed, 0, saved, &audio_deleted); idle(&inst);
    assert(audio_deleted.calls == 1 && !audio_deleted.result && !delete_from_audio && !group->radio); assert(!radio_init(group));
    radio_set_active(&inst, 7, 1); idle(&inst); assert(group->radio_active && !radio_member_muted(&inst, 7, member_ed));
    radio_set_active(&inst, 7, 0);
    __atomic_store_n(&read_fault, SQLITE_IOERR, __ATOMIC_SEQ_CST); write_mute(&inst, 1, -1, 0);
    write_mute(&inst, 1, -1, 0); assert(!radio_member_muted(&inst, 7, member_ed));
    radio_destroy(group); radio_destroy_instance(&inst); assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL;
    assert(queue_data_get(groups.group_list) == entry); queue_entry_free(entry); queue_free(groups.group_list); uasync_destroy(ua, 0);
    forbid_sql = 0; assert(!remove(path) && u_get_allocated_count() == before);
    puts("PASS radio mutes: no loop SQL/legacy, live timer, FIFO toggle, atomic rollback, faults, cancellation and context teardown"); return 0;
}
