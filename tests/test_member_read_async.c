/* Файловый SQL-снимок участников: worker копирует данные, uasync добавляет live-поля без SQL. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_devgroup.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/chat_member_read.h"
#include "../src/group/group_protocol.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/transport_layer/etcp.h"
#include "../src/db_storage.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"

static pthread_t loop_thread;
static int forbid_sql;
#ifdef __linux__
static int reading, fault;
static unsigned allocation, fail_allocation;
static int forbid_member_sql, native_fault;
static const char* fail_calloc_module;
static int fail_event_allocation;
struct read_gate { pthread_mutex_t mutex; pthread_cond_t changed; int entered, released; };
static struct read_gate* native_gate;

static void hold_native(void) {
    struct read_gate* gate = native_gate;
    if (!gate) return;
    pthread_mutex_lock(&gate->mutex);
    if (!gate->entered) {
        gate->entered = 1; pthread_cond_broadcast(&gate->changed);
        while (!gate->released) pthread_cond_wait(&gate->changed, &gate->mutex);
    }
    pthread_mutex_unlock(&gate->mutex);
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    int member = !strncmp(sql, "SELECT p.ed25519_pubkey,COALESCE", sizeof("SELECT p.ed25519_pubkey,COALESCE") - 1) ||
        !strncmp(sql, "SELECT 1 FROM \"peers_", sizeof("SELECT 1 FROM \"peers_") - 1);
    if (pthread_equal(loop_thread, pthread_self())) assert(!forbid_sql && !(forbid_member_sql && member));
    else {
        if (native_fault && member) { native_fault = 0; *st = NULL; return SQLITE_BUSY; }
        if (reading && fault == 1) { fault = 0; *st = NULL; return SQLITE_BUSY; }
    }
    return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
}
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (!pthread_equal(loop_thread, pthread_self()) && reading && fault == 2) { fault = 0; return SQLITE_NOMEM; }
    return __real_sqlite3_bind_int64(st, index, value);
}
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, sqlite3_destructor_type);
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int size, sqlite3_destructor_type release) {
    if (!pthread_equal(loop_thread, pthread_self()) && reading && fault == 2) { fault = 0; return SQLITE_NOMEM; }
    return __real_sqlite3_bind_blob(st, index, value, size, release);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (!pthread_equal(loop_thread, pthread_self()) && reading && fault == 3) { fault = 0; return SQLITE_INTERRUPT; }
    return __real_sqlite3_step(st);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int member = !pthread_equal(loop_thread, pthread_self()) && st &&
        (!strncmp(sqlite3_sql(st), "SELECT p.ed25519_pubkey,COALESCE", sizeof("SELECT p.ed25519_pubkey,COALESCE") - 1) ||
         !strncmp(sqlite3_sql(st), "SELECT 1 FROM \"peers_", sizeof("SELECT 1 FROM \"peers_") - 1));
    int fail = member && reading && fault == 4;
    int rc = __real_sqlite3_finalize(st);
    if (member) hold_native();
    if (fail) { fault = 0; return SQLITE_BUSY; }
    return rc;
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t size, const char* where) {
    if (!pthread_equal(loop_thread, pthread_self()) && reading && strstr(where, "chat_member_read.c:") &&
        ++allocation == fail_allocation) return NULL;
    return __real_u_realloc_impl(ptr, size, where);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (pthread_equal(loop_thread, pthread_self()) && fail_calloc_module && strstr(where, fail_calloc_module)) {
        fail_calloc_module = NULL; return NULL;
    }
    return __real_u_calloc_impl(count, size, where);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* where) {
    if (pthread_equal(loop_thread, pthread_self()) && fail_event_allocation && strstr(where, "chat_member.c:")) {
        fail_event_allocation = 0; return NULL;
    }
    return __real_u_malloc_impl(size, where);
}
#endif

struct query {
    char channel[65];
    const uint8_t *self, *only_ed;
    struct chat_member_display* rows;
    size_t count;
    int injected, code, done, existence, found;
    unsigned oom, attempted;
};
static int read_work(sqlite3* db, void* data) {
    assert(!pthread_equal(loop_thread, pthread_self())); struct query* q = data;
#ifdef __linux__
    fault = q->injected; allocation = 0; fail_allocation = q->oom; reading = 1;
#endif
    q->code = q->existence ? chat_member_exists_read(db, q->channel, q->only_ed, &q->found) :
        chat_member_rows_read(db, q->channel, q->self, q->only_ed, &q->rows, &q->count);
#ifdef __linux__
    assert(!q->injected || !fault); q->attempted = allocation; reading = 0;
#endif
    /* Ошибка примитива сохраняется отдельно, чтобы проверить восстановление после каждого injected отказа. */
    return SQLITE_OK;
}
static void read_done(void* data, const struct db_async_result* result) {
    struct query* q = data; assert(pthread_equal(loop_thread, pthread_self()) && !q->done && !result->status);
    if (q->code != SQLITE_OK) assert(!q->rows && !q->count);
    q->done = 1;
}
static void read_once(struct UTUN_INSTANCE* inst, struct query* q) {
    q->done = 0; q->rows = NULL; q->count = 0; q->found = 17;
    db_async_submit(inst->db_worker, "member-static-read", DB_ASYNC_READ, read_work, q, read_done);
    uint64_t end = get_time_us() + 5000000;
    while (!q->done && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(q->done);
}
static struct chat_member_display* find(struct query* q, const uint8_t ed[32]) {
    for (size_t i = 0; i < q->count; i++) if (!memcmp(q->rows[i].ed25519, ed, 32)) return &q->rows[i];
    return NULL;
}
static void change(sqlite3* db, const char* channel, const char* fields) {
    char sql[2048]; int bytes = snprintf(sql, sizeof(sql), "UPDATE peers_%s SET %s WHERE node_id=1", channel, fields);
    assert(bytes > 0 && (size_t)bytes < sizeof(sql)); assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

#ifdef __linux__
struct member_events {
    char channel[65], first_name[64], last_name[64];
    unsigned lists, updates;
    unsigned details;
    uint8_t detail_calls[32], detail_result[32], detail_hops[32];
    int expected_mute, check_mute, delete_on_update;
};
static struct member_events* native_events;

static void display_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    assert(pthread_equal(loop_thread, pthread_self()));
    struct member_events* events = native_events;
    if (events && type == CHAT_EVT_MEMBER_DETAIL) {
        struct chat_member_detail_view view; assert(!chat_core_parse_member_detail(data, (size_t)size, &view));
        uint8_t group[32];
        if (sc_hex_to_binary(events->channel, group, sizeof(group)) || memcmp(group, view.group, 32)) return;
        assert(view.request_id > 0 && view.request_id < 32 && !events->detail_calls[view.request_id]++);
        events->detail_result[view.request_id] = view.result; events->detail_hops[view.request_id] = view.hop_count; events->details++; return;
    }
    if (!events || (type != CHAT_EVT_MEMBER_LIST && type != CHAT_EVT_MEMBER_UPDATED)) return;
    assert(size > 0 && data[0] < sizeof(events->channel)); size_t cl = data[0];
    if (strlen(events->channel) != cl || memcmp(data + 1, events->channel, cl)) return;
    size_t offset = cl + (type == CHAT_EVT_MEMBER_LIST ? 3 : 2); uint16_t count = 1;
    assert((size_t)size >= offset);
    if (type == CHAT_EVT_MEMBER_LIST) { memcpy(&count, data + cl + 1, 2); events->lists++; }
    else events->updates++;
    assert((size_t)size == offset + (size_t)count * CHAT_MEMBER_DISPLAY_SIZE);
    for (size_t i = 0; i < count; i++) {
        struct chat_member_display member;
        assert(!chat_core_deserialize_member(data + offset + i * CHAT_MEMBER_DISPLAY_SIZE, &member));
        if (member.is_self) assert(member.online && member.protocol_local);
        if (!memcmp(member.ed25519, (const uint8_t[32]){1}, 32)) {
            if (!events->first_name[0]) strcpy(events->first_name, member.name);
            strcpy(events->last_name, member.name);
            if (events->check_mute) assert(!!(member.flags & CHAT_MEMBER_FLAG_RADIO_MUTED) == events->expected_mute);
        }
    }
    if (type == CHAT_EVT_MEMBER_UPDATED && events->delete_on_update) {
        events->delete_on_update = 0; chat_core_delete_channel(inst, events->channel);
    }
}

static void finish_display(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000;
    while (CC(inst) && (CC(inst)->member_reads || CC(inst)->member_route_events) && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(CC(inst) && !CC(inst)->member_reads && !CC(inst)->member_route_events);
}
static void request_display(struct UTUN_INSTANCE* inst, const char* channel, int rtt) {
    struct chat_member_list_req* request = u_calloc(1, sizeof(*request)); assert(request);
    request->inst = inst; strcpy(request->ch_id, channel);
    if (rtt) chat_core_request_member_rtt_trampoline(request); else chat_core_request_member_list_trampoline(request);
}
static void wait_gate(struct UTUN_INSTANCE* inst, struct read_gate* gate) {
    int entered = 0; uint64_t end = get_time_us() + 5000000;
    while (!entered && get_time_us() < end) {
        uasync_poll(inst->ua, 1); pthread_mutex_lock(&gate->mutex); entered = gate->entered; pthread_mutex_unlock(&gate->mutex);
    }
    assert(entered);
}
static void release_gate(struct read_gate* gate) {
    pthread_mutex_lock(&gate->mutex); gate->released = 1; pthread_cond_broadcast(&gate->changed); pthread_mutex_unlock(&gate->mutex);
}
static void reset_gate(struct read_gate* gate) {
    pthread_mutex_lock(&gate->mutex); gate->entered = gate->released = 0; pthread_mutex_unlock(&gate->mutex);
}
static void tick(void* arg) { (*(int*)arg)++; }
static void* delayed_release(void* arg) { usleep(30000); release_gate(arg); return NULL; }

static void request_detail(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, unsigned id) {
    struct chat_member_detail_req* request = u_calloc(1, sizeof(*request)); assert(request);
    *request = (struct chat_member_detail_req){.inst=inst,.ed={(uint8_t)node},.request_id=id};
    assert(!sc_hex_to_binary(channel, request->group, sizeof(request->group)));
    if (node == inst->node_id) memcpy(request->ed, inst->my_ed25519_pubkey, 32);
    chat_core_collect_member_detail_trampoline(request);
}
static void wait_details(struct UTUN_INSTANCE* inst, struct member_events* events, unsigned count) {
    uint64_t end = get_time_us() + 5000000;
    while (events->details < count && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(events->details == count);
}

/* SQL отвечает только за наличие записи; route, stop/delete и новый запуск проверяются в uasync после READ. */
static void native_details(struct UTUN_INSTANCE* inst, const char* channel) {
    finish_display(inst); forbid_member_sql = 1; struct member_events events = {0}; strcpy(events.channel, channel);
    native_events = &events; chat_event_set_handler(inst, display_event);
    request_detail(inst, channel, inst->node_id, 1); assert(!events.details); wait_details(inst, &events, 1);
    assert(events.detail_result[1] == CHAT_MEMBER_DETAIL_OK);
    request_detail(inst, channel, UINT64_MAX, 2); wait_details(inst, &events, 2);
    assert(events.detail_result[2] == CHAT_MEMBER_DETAIL_NOT_MEMBER);
    native_fault = 1; request_detail(inst, channel, 1, 3); wait_details(inst, &events, 3);
    assert(!native_fault && events.detail_result[3] == CHAT_MEMBER_DETAIL_ERROR);
    request_detail(inst, channel, 1, 4); wait_details(inst, &events, 4); assert(events.detail_result[4] == CHAT_MEMBER_DETAIL_OK);
    struct db_async* worker = inst->db_worker; inst->db_worker = NULL;
    request_detail(inst, channel, 1, 5); assert(events.details == 5 && events.detail_result[5] == CHAT_MEMBER_DETAIL_ERROR);
    inst->db_worker = worker;
    const char* allocations[] = {"chat_status.c:", "db_async.c:"};
    for (unsigned i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++) {
        fail_calloc_module = allocations[i]; request_detail(inst, channel, 1, 6 + i);
        assert(!fail_calloc_module && events.details == 6 + i && events.detail_result[6 + i] == CHAT_MEMBER_DETAIL_ERROR);
    }
    struct read_gate gate = {.mutex=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER};
    native_gate = &gate; request_detail(inst, channel, 1, 8); wait_gate(inst, &gate);
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel); assert(group);
    assert(!topo_node_find_by_id(group, 1));
    struct TOPO_GROUP_NODE* node = (struct TOPO_GROUP_NODE*)queue_entry_new(sizeof(*node) - sizeof(struct ll_entry)); assert(node);
    memset((uint8_t*)node + sizeof(struct ll_entry), 0, sizeof(*node) - sizeof(struct ll_entry)); node->node_id = 1;
    assert(queue_data_put_with_index(group->nodes, &node->ll) == 0);
    uint8_t ed[32] = {1}; uint32_t short_address;
    assert(!topo_address_short(ed, &short_address)); topo_address_attach(group, node, ed, short_address);
    struct ETCP_CONN route = {.instance=inst, .peer_node_id=1, .links_up=1}; struct topo_hop hops[] = {{.id=1, .ed={1}}};
    assert(!topo_group_add_path(node, &route, hops, 1, 0));
    int timer = 0; assert(uasync_set_timeout(inst->ua, 10, &timer, tick, "member_detail_live"));
    uint64_t end = get_time_us() + 5000000; while (!timer && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(timer == 1 && events.details == 7); release_gate(&gate); wait_details(inst, &events, 8); native_gate = NULL;
    assert(events.detail_result[8] == CHAT_MEMBER_DETAIL_OK && events.detail_hops[8] == 1);
    topo_nodeq_clear_paths(node); topo_address_detach(group, node); queue_remove_data(group->nodes, &node->ll); queue_entry_free(&node->ll);
    uint64_t temporary = chat_core_create_channel_auto(inst, "Deleted detail"); assert(temporary); finish_display(inst);
    strcpy(events.channel, topo_groups_channel(inst->topo_groups, temporary));
    reset_gate(&gate); native_gate = &gate; request_detail(inst, events.channel, inst->node_id, 9); wait_gate(inst, &gate);
    chat_core_delete_channel(inst, events.channel); release_gate(&gate); wait_details(inst, &events, 9); native_gate = NULL;
    assert(events.detail_result[9] == CHAT_MEMBER_DETAIL_UNAVAILABLE);
    strcpy(events.channel, channel); reset_gate(&gate); native_gate = &gate;
    request_detail(inst, channel, inst->node_id, 10); wait_gate(inst, &gate);
    group_service_stop(inst); assert(!chat_service_start(inst)); request_detail(inst, channel, inst->node_id, 11);
    release_gate(&gate); wait_details(inst, &events, 11); finish_display(inst); native_gate = NULL;
    assert(events.detail_result[10] == CHAT_MEMBER_DETAIL_UNAVAILABLE && events.detail_result[11] == CHAT_MEMBER_DETAIL_OK);
    native_events = NULL; forbid_member_sql = 0; pthread_cond_destroy(&gate.changed); pthread_mutex_destroy(&gate.mutex);
}

/* Настоящий UI-вход: пауза после копии SQL обнаруживает откат флагов и использование уже удалённого runtime. */
static void native_requests(struct UTUN_INSTANCE* inst, const char* channel) {
    finish_display(inst); forbid_member_sql = 1;
    struct member_events events = {0}; strcpy(events.channel, channel); native_events = &events;
    chat_event_set_handler(inst, display_event);
    struct read_gate gate = {.mutex=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER};
    native_gate = &gate; request_display(inst, channel, 0); wait_gate(inst, &gate);
    unsigned pending = db_async_pending(inst->db_worker); assert(pending >= 1);
    for (int i = 0; i < 12; i++) { request_display(inst, channel, 0); request_display(inst, channel, 1); }
    assert(db_async_pending(inst->db_worker) == pending); /* Другие сервисы могут иметь свои задания за удержанным READ. */
    change(inst->topo_sqlite_db, channel, "local_nick='Newest'");
    int timer = 0; assert(uasync_set_timeout(inst->ua, 10, &timer, tick, "member_read_live"));
    uint64_t end = get_time_us() + 5000000;
    while (!timer && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(timer == 1 && !events.lists && !events.updates);
    release_gate(&gate); finish_display(inst); native_gate = NULL;
    assert(events.lists == 2 && events.updates == 68 && !strcmp(events.first_name, "Changed") && !strcmp(events.last_name, "Newest"));

    reset_gate(&gate); native_gate = &gate; request_display(inst, channel, 0); wait_gate(inst, &gate);
    unsigned lists = events.lists, updates = events.updates;
    events.check_mute = 1; events.expected_mute = 0; forbid_member_sql = 0;
    assert(!chat_core_toggle_member_radio_mute(inst, channel, (uint8_t[32]){1})); forbid_member_sql = 1;
    assert(events.updates == updates + 1);
    release_gate(&gate); finish_display(inst); native_gate = NULL;
    assert(events.lists == lists + 1 && events.updates == updates + 1);

    native_fault = 1; lists = events.lists; request_display(inst, channel, 0); finish_display(inst);
    assert(!native_fault && events.lists == lists);
    request_display(inst, channel, 0); finish_display(inst); assert(events.lists == lists + 1);
    const char* allocation_modules[] = {"chat_member.c:", "db_async.c:"};
    for (unsigned i = 0; i < sizeof(allocation_modules) / sizeof(allocation_modules[0]); i++) {
        lists = events.lists; fail_calloc_module = allocation_modules[i]; request_display(inst, channel, 0); finish_display(inst);
        assert(!fail_calloc_module && events.lists == lists);
        request_display(inst, channel, 0); finish_display(inst); assert(events.lists == lists + 1);
    }
    lists = events.lists; fail_event_allocation = 1; request_display(inst, channel, 0); finish_display(inst);
    assert(!fail_event_allocation && events.lists == lists);
    request_display(inst, channel, 0); finish_display(inst); assert(events.lists == lists + 1);
    struct db_async* worker = inst->db_worker; inst->db_worker = NULL; lists = events.lists;
    request_display(inst, channel, 0); assert(!CC(inst)->member_reads && events.lists == lists); inst->db_worker = worker;

    events.delete_on_update = 1; updates = events.updates; request_display(inst, channel, 1); finish_display(inst);
    assert(!events.delete_on_update && events.updates == updates + 1 &&
        !topo_groups_find_channel(inst->topo_groups, channel));
    uint64_t second = chat_core_create_channel_auto(inst, "Restarted members"); assert(second);
    strcpy(events.channel, topo_groups_channel(inst->topo_groups, second)); finish_display(inst); events.check_mute = 0;
    lists = events.lists; updates = events.updates;
    reset_gate(&gate); native_gate = &gate; request_display(inst, events.channel, 0); wait_gate(inst, &gate);
    group_service_stop(inst); assert(!CC(inst)); assert(!chat_service_start(inst));
    request_display(inst, events.channel, 0); release_gate(&gate); finish_display(inst); native_gate = NULL;
    assert(events.lists == lists + 1 && events.updates == updates);

    reset_gate(&gate); native_gate = &gate; lists = events.lists; updates = events.updates;
    request_display(inst, events.channel, 0); wait_gate(inst, &gate); request_display(inst, events.channel, 1);
    request_detail(inst, events.channel, inst->node_id, 30); request_detail(inst, events.channel, 1, 31);
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, second);
    if (group) chat_member_state_changed(group, inst->my_ed25519_pubkey);
    pthread_t releaser; assert(!pthread_create(&releaser, NULL, delayed_release, &gate));
    utun_instance_destroy(inst); assert(!pthread_join(releaser, NULL)); native_gate = NULL;
    assert(events.lists == lists && events.updates == updates && events.detail_calls[30] == 1 && events.detail_calls[31] == 1 &&
        events.detail_result[30] == CHAT_MEMBER_DETAIL_UNAVAILABLE && events.detail_result[31] == CHAT_MEMBER_DETAIL_UNAVAILABLE);
    native_events = NULL; forbid_member_sql = 0;
    pthread_cond_destroy(&gate.changed); pthread_mutex_destroy(&gate.mutex);
}
#endif

int main(void) {
    loop_thread = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
    size_t before = u_get_allocated_count(); char directory[128] = "/tmp/utun-member-read-XXXXXX"; assert(!test_mkdtemp(directory));
    struct UASYNC* ua = uasync_create(); assert(ua); char config[1024];
    snprintf(config, sizeof(config), "[global]\nmy_node_name=Static worker\ndb_path=%s\nauto_sockets=no\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst && inst->db_worker);
    assert(!utun_core_start(inst) && !chat_service_start(inst)); uint64_t group = chat_core_create_channel_auto(inst, "Members"); assert(group);
    struct query q = {.self=inst->my_ed25519_pubkey}; strcpy(q.channel, topo_groups_channel(inst->topo_groups, group));
    assert(!chat_devgroup_rename(inst, "My devices"));
    for (uint64_t node = 1; node <= 33; node++) {
        uint8_t peer_ed[32] = {0}; peer_ed[0] = (uint8_t)node;
        assert(node != inst->node_id && !topo_node_sqlite_member_placeholder_put(inst->topo_sqlite_db, q.channel, node,
            inst->my_keys.public_key, peer_ed));
    }
    change(inst->topo_sqlite_db, q.channel, "local_nick='Local name',storage=3,radio_muted=1,"
        "adm_tags='{\"admin\":\"edee\",\"moder\":\"eed\",\"supernode\":\"yes\",\"verified\":\"yes\",\"storage\":\"yes\"}',"
        "userinfo='{\"avatar_server\":\"0100000000000000000000000000000000000000000000000000000000000000\",\"avatar_file\":\""
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.json\"}'");
    read_once(inst, &q); assert(q.code == SQLITE_OK && q.count == 34);
    struct chat_member_display* local = find(&q, inst->my_ed25519_pubkey); struct chat_member_display* first = find(&q, (const uint8_t[32]){1});
    assert(local && first && local->is_self && !local->online && local->rtt == UINT16_MAX && !strcmp(local->devgroup_name, "My devices"));
    assert(!strcmp(first->name, "Local name") && first->storage == 3 && !first->storage_role && first->verified);
    assert(first->flags == (CHAT_MEMBER_FLAG_ADMIN | CHAT_MEMBER_FLAG_SUPERNODE | CHAT_MEMBER_FLAG_RADIO_MUTED));
    assert(!memcmp(first->avatar_server, (const uint8_t[32]){1}, 32) && strlen(first->avatar_file) == 69 && !first->online && !first->protocol_local);
    assert(!first->is_self && first->ed25519[0] == 1 && !memcmp(local->ed25519, inst->my_ed25519_pubkey, 32));
    size_t retained_count = q.count; struct chat_member_display* retained = q.rows; q.rows = NULL;
    q.only_ed = first->ed25519;
    read_once(inst, &q); assert(!q.code && q.count == 1 && !memcmp(q.rows[0].ed25519, (const uint8_t[32]){1}, 32)); u_free(q.rows);
    q.only_ed = NULL;
    assert(!chat_devgroup_rename(inst, "Updated devices")); change(inst->topo_sqlite_db, q.channel, "local_nick='Changed'");
    sqlite3* db = inst->topo_sqlite_db; struct chat_core_ctx* cc = CC(inst); sqlite3* cc_db = cc->db;
    inst->topo_sqlite_db = cc->db = NULL; forbid_sql = 1;
    for (size_t i = 0; i < retained_count; i++) assert(!chat_member_apply_live(inst, q.channel, &retained[i]));
    assert(local->online && local->protocol_local && !strcmp(local->devgroup_name, "My devices") && !strcmp(first->name, "Local name"));
    forbid_sql = 0; inst->topo_sqlite_db = db; cc->db = cc_db;
    read_once(inst, &q);
    struct chat_member_display *updated_first = find(&q, (const uint8_t[32]){1}), *updated_self = find(&q, inst->my_ed25519_pubkey);
    assert(q.code == SQLITE_OK && updated_first && updated_self);
    assert(!strcmp(updated_first->name, "Changed") && !strcmp(updated_self->devgroup_name, "Updated devices")); u_free(q.rows);
#ifdef __linux__
    for (int single = 0; single < 2; single++) {
        q.only_ed = single ? (const uint8_t[32]){1} : NULL;
        for (int injected = 1; injected <= 4; injected++) {
            if (!single && injected == 2) continue;
            q.injected = injected; read_once(inst, &q); assert(q.code != SQLITE_OK && !q.rows && !q.count);
            q.injected = 0; read_once(inst, &q); assert(q.code == SQLITE_OK && q.count == (single ? 1 : 34)); u_free(q.rows);
        }
        for (unsigned allocation = 1; allocation <= (single ? 1u : 2u); allocation++) {
            q.oom = allocation; read_once(inst, &q); assert(q.code == SQLITE_NOMEM && q.attempted == allocation && !q.rows && !q.count);
            q.oom = 0; read_once(inst, &q); assert(q.code == SQLITE_OK); u_free(q.rows);
        }
    }
#endif

    q.only_ed = (const uint8_t[32]){255};
    read_once(inst, &q); assert(q.code == SQLITE_OK && !q.rows && !q.count);
    q.existence = 1;
    read_once(inst, &q); assert(q.code == SQLITE_OK && q.found == 0); q.only_ed = (const uint8_t[32]){1};
    read_once(inst, &q); assert(q.code == SQLITE_OK && q.found == 1);
#ifdef __linux__
    for (int point = 1; point <= 4; point++) {
        q.injected = point; read_once(inst, &q); assert(q.code != SQLITE_OK && q.found == 17);
        q.injected = 0; read_once(inst, &q); assert(q.code == SQLITE_OK && q.found == 1);
    }
#endif
    q.existence = 0;
    change(inst->topo_sqlite_db, q.channel, "adm_tags='{'"); read_once(inst, &q); assert(q.code != SQLITE_OK && !q.rows && !q.count);
    change(inst->topo_sqlite_db, q.channel, "adm_tags='{\"admin\":\"yes\\u0000hidden\"}'");
    read_once(inst, &q); assert(q.code == SQLITE_CORRUPT && !q.rows && !q.count);
    change(inst->topo_sqlite_db, q.channel, "adm_tags='{}'"); read_once(inst, &q); assert(q.code == SQLITE_OK && q.count == 1); u_free(q.rows);
#ifdef __linux__
    native_details(inst, q.channel);
    native_requests(inst, q.channel);
#else
    utun_instance_destroy(inst);
#endif
    uasync_destroy(ua, 0);
    assert(!strcmp(local->devgroup_name, "My devices") && !strcmp(first->name, "Local name")); u_free(retained);
    assert(!db_storage_clear(directory));
    const char* directories[] = {"member_files/.uploads", "member_files", "dm_pending", "dm_media"};
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        char path[256]; snprintf(path, sizeof(path), "%s/%s", directory, directories[i]);
        assert(!test_rmdir(path));
    }
    assert(!test_rmdir(directory));
    if (u_get_allocated_count() != before) u_report_unfreed_blocks();
    assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "[PASS] member READ: frozen rows/devgroup, live without SQL, errors/OOM/retry and owned lifetime");
    return 0;
}
