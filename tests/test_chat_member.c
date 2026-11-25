/* Полный снимок участников не выдаёт успех после отказа SQL. */
#define _GNU_SOURCE
#include "../src/chat/chat_member.c"
#include "../src/chat/group_policy.h"
#include "../src/chat/chat_feedback.h"
#include "../src/chat/chat_poll.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/etcp_connections.h"
#include "../src/transport_layer/pkt_normalizer.h"
#include "../src/routing_layer/etcp_router.h"
#include "../lib/strbuf.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>

static int fail_query;
enum fault { F_NONE, F_PREPARE, F_BIND, F_STEP, F_CHANGES, F_FINALIZE, F_EXEC, F_ALLOCATE, F_REALLOCATE };
static enum fault fault;
static const char* target;
static int bind_index, last_target, fail_rollback, arm_after_commit;
static sqlite3* reader;
static uint64_t group_id;
static char channel[65];
static uint8_t mute_ed[32];
static unsigned member_events, mute_events, list_events;
static uint8_t detail[CHAT_MEMBER_DETAIL_MAX_SIZE];
static size_t detail_size;
static unsigned text_events;
static int expect_large_text;
static int expect_router_metrics;
static pthread_t loop_thread;
static int async_fault;

static int matches(const char* sql) { return target && !strncmp(sql, target, strlen(target)); }
static int take(enum fault point, const char* sql, int index) {
    if (fault != point || (sql && !matches(sql)) || (point == F_BIND && index != bind_index)) return 0;
    fault = F_NONE; DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member test: fault stage=%d index=%d target=%s", point, index, target); return 1;
}
extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    if (!pthread_equal(loop_thread, pthread_self()) && !async_fault) return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
    if (take(F_PREPARE, sql, 0)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
}
extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (!pthread_equal(loop_thread, pthread_self()) && !async_fault) return __real_sqlite3_bind_int64(st, index, value);
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}
extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, sqlite3_destructor_type);
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int size, sqlite3_destructor_type free_fn) {
    if ((pthread_equal(loop_thread, pthread_self()) || async_fault) && take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, value, size, free_fn);
}
extern int __real_sqlite3_bind_int(sqlite3_stmt*, int, int);
int __wrap_sqlite3_bind_int(sqlite3_stmt* st, int index, int value) {
    if (!pthread_equal(loop_thread, pthread_self()) && !async_fault) return __real_sqlite3_bind_int(st, index, value);
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int(st, index, value);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (!pthread_equal(loop_thread, pthread_self()) && !async_fault) return __real_sqlite3_step(st);
    last_target = matches(sqlite3_sql(st)); if (take(F_STEP, sqlite3_sql(st), 0)) return async_fault ? SQLITE_BUSY : SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_changes(sqlite3*);
int __wrap_sqlite3_changes(sqlite3* db) {
    if (last_target && take(F_CHANGES, NULL, 0)) return 0;
    return __real_sqlite3_changes(db);
}
extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    if (!pthread_equal(loop_thread, pthread_self()) && !async_fault) return __real_sqlite3_exec(db, sql, cb, arg, error);
    if (fail_rollback && !strcmp(sql, "ROLLBACK")) { fail_rollback = 0; return SQLITE_IOERR; }
    if (take(F_EXEC, sql, 0)) return SQLITE_IOERR;
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}
extern void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* location) {
    if (!pthread_equal(loop_thread, pthread_self()) && !async_fault) return __real_u_malloc_impl(size, location);
    if (fault == F_ALLOCATE && strstr(location, target)) { fault = F_NONE; return NULL; }
    return __real_u_malloc_impl(size, location);
}
extern void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t size, const char* location) {
    if (!pthread_equal(loop_thread, pthread_self()) && !async_fault) return __real_u_realloc_impl(ptr, size, location);
    if (fault == F_REALLOCATE && strstr(location, target)) { fault = F_NONE; return NULL; }
    return __real_u_realloc_impl(ptr, size, location);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    if (!pthread_equal(loop_thread, pthread_self()) && !async_fault) return __real_sqlite3_finalize(st);
    const char* snapshot = "SELECT p.ed25519_pubkey,COALESCE";
    int fail = fail_query && st && !strncmp(sqlite3_sql(st), snapshot, strlen(snapshot));
    if (fail) { fail_query = 0; DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member test: injected snapshot finalize failure"); }
    if (st && take(F_FINALIZE, sqlite3_sql(st), 0)) fail = 1;
    int rc = __real_sqlite3_finalize(st); return fail ? (async_fault ? SQLITE_BUSY : SQLITE_IOERR) : rc;
}

static void finish_members(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 30000000;
    while (CC(inst) && (CC(inst)->member_reads || CC(inst)->member_route_events) && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(CC(inst) && !CC(inst)->member_reads && !CC(inst)->member_route_events);
}

static int stored_mute(void) {
    char sql[160]; snprintf(sql, sizeof(sql), "SELECT radio_muted FROM peers_%s WHERE node_id=7", channel); sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(reader, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int value = sqlite3_column_int(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return value;
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    if (type == CHAT_EVT_STATUS_REFRESH || type == CHAT_EVT_CONN_METRICS) {
        assert(size > 0);
        if (expect_large_text) assert(size > 8192 && memmem(data, (size_t)size, "LINK#299", 8));
        if (type == CHAT_EVT_CONN_METRICS && expect_router_metrics) {
            const char* expected[] = {"--- Router queues ---", "peer=0000000000000009 svc=63 (call audio)",
                "peer=000000000000000B svc=52 (DM)", "sessions: 2\n", "send_q: 202B/2p  wait_ack: 303B/1p",
                "incoming_q: 44B/1p  recv_q: 55B/1p", "send_q: 0B/0p  wait_ack: 0B/0p",
                "transit total: 666B/2p", "input_q: 777B/1p  output_q: 0B/0p"};
            for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
                assert(memmem(data, (size_t)size, expected[i], strlen(expected[i])));
            const char* excluded = "peer=000000000000000C svc=";
            assert(!memmem(data, (size_t)size, excluded, strlen(excluded)));
        }
        text_events++; return;
    }
    if (type == CHAT_EVT_MEMBER_DETAIL) {
        assert(size >= 0 && (size_t)size <= sizeof(detail)); memcpy(detail, data, (size_t)size); detail_size = (size_t)size; return;
    }
    if (type == CHAT_EVT_MEMBER_LIST) {
        assert(size >= data[0] + 3); uint16_t count; memcpy(&count, data + data[0] + 1, 2);
        assert((size_t)size == data[0] + 3 + (size_t)count * CHAT_MEMBER_DISPLAY_SIZE); list_events++; return;
    }
    if (type != CHAT_EVT_MEMBER_UPDATED) return;
    assert(sqlite3_get_autocommit(inst->topo_sqlite_db) && size == data[0] + 2 + CHAT_MEMBER_DISPLAY_SIZE);
    const uint8_t* wire = data + data[0] + 2;
    if (!memcmp(wire + CHAT_MEMBER_ED_OFF, mute_ed, 32)) assert(!!(wire[CHAT_MEMBER_FLAGS_OFF] & CHAT_MEMBER_FLAG_RADIO_MUTED) == stored_mute());
    member_events++;
}

static void mute(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], int muted, void* arg) {
    (void)arg; assert(group == group_id && !memcmp(ed, mute_ed, 32) && sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(stored_mute() == muted && radio_member_muted(inst, group, ed) == muted); mute_events++;
    if (arm_after_commit) { arm_after_commit = 0; target = "SELECT p.ed25519_pubkey,COALESCE"; fault = F_FINALIZE; }
}

static void refused_mute(struct UTUN_INSTANCE* inst) {
    int before = stored_mute(); unsigned display = member_events, audio = mute_events;
    assert(chat_core_toggle_member_radio_mute(inst, channel, mute_ed) < 0 && fault == F_NONE);
    assert(sqlite3_get_autocommit(inst->topo_sqlite_db) && stored_mute() == before && radio_member_muted(inst, group_id, mute_ed) == before);
    assert(display == member_events && audio == mute_events);
}

static void retry_mute(struct UTUN_INSTANCE* inst) {
    int before = stored_mute(); unsigned display = member_events, audio = mute_events;
    assert(!chat_core_toggle_member_radio_mute(inst, channel, mute_ed));
    assert(stored_mute() != before && member_events == display + 1 && mute_events == audio + 1);
}

static void snapshots(struct UTUN_INSTANCE* inst) {
    unsigned cases = 0; target = "SELECT p.ed25519_pubkey,COALESCE"; uint8_t previous[CHAT_MEMBER_DISPLAY_SIZE], output[CHAT_MEMBER_DISPLAY_SIZE];
    memset(previous, 0xa5, sizeof(previous));
    for (int single = 0; single < 2; single++) for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
        if (point == F_CHANGES || (point == F_BIND && !single)) continue;
        bind_index = 1; fault = point;
        if (single) {
            memcpy(output, previous, sizeof(output));
            assert(chat_core_get_single_member(inst, channel, mute_ed, output) < 0 && !memcmp(output, previous, sizeof(output)));
            assert(fault == F_NONE && !chat_core_get_single_member(inst, channel, mute_ed, output));
        } else {
            uint8_t* wire = NULL; int count = 0;
            assert(chat_core_get_member_list(inst, channel, &wire, &count) < 0 && !wire && !count);
            assert(fault == F_NONE && !chat_core_get_member_list(inst, channel, &wire, &count) && count == 4); u_free(wire);
        }
        cases++;
    }
    for (enum fault point = F_ALLOCATE; point <= F_REALLOCATE; point++) {
        uint8_t* wire = NULL; int count = 0;
        target = point == F_REALLOCATE ? "chat_member_read.c" : "chat_member.c"; fault = point;
        assert(chat_core_get_member_list(inst, channel, &wire, &count) < 0 && !wire && !count && fault == F_NONE);
        assert(!chat_core_get_member_list(inst, channel, &wire, &count) && count == 4); u_free(wire); cases++;
    }
    const char* invalid[] = {NULL, "", "0", "01", "1x", "9223372036854775808", "12345678901234567890"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        uint8_t* wire = NULL; int count = 0;
        assert(chat_core_get_member_list(inst, invalid[i], &wire, &count) < 0 && !wire && !count);
    }
    uint8_t* wire = NULL; int count = 0; assert(chat_core_get_member_list(NULL, channel, &wire, &count) < 0 && !wire && !count);
    assert(!chat_core_member_online(NULL, channel, (uint8_t[32]){7}));
    for (int rtt = 0; rtt < 2; rtt++) {
        struct chat_member_list_req* req = u_calloc(1, sizeof(*req)); assert(req); strcpy(req->ch_id, channel);
        if (rtt) chat_core_request_member_rtt_trampoline(req); else chat_core_request_member_list_trampoline(req);
        req = u_malloc(sizeof(*req)); assert(req); req->inst = inst; memset(req->ch_id, '1', sizeof(req->ch_id));
        if (rtt) chat_core_request_member_rtt_trampoline(req); else chat_core_request_member_list_trampoline(req);
    }
    struct chat_member_radio_mute_req* mute_req = u_malloc(sizeof(*mute_req)); assert(mute_req);
    mute_req->inst = inst; memcpy(mute_req->ed, mute_ed, 32); memset(mute_req->ch_id, '1', sizeof(mute_req->ch_id));
    chat_core_toggle_member_radio_mute_trampoline(mute_req);
    on_member_props_changed(mute_ed, channel, inst); finish_members(inst); unsigned before = member_events;
    target = "SELECT p.ed25519_pubkey,COALESCE"; fault = F_FINALIZE; async_fault = 1; on_member_props_changed(mute_ed, channel, inst);
    finish_members(inst); async_fault = 0;
    assert(fault == F_NONE && before == member_events); on_member_props_changed(mute_ed, channel, inst); finish_members(inst);
    assert(member_events == before + 1);
    struct chat_member_display source; memset(&source, 0xa5, sizeof(source)); strcpy(source.name, "N"); strcpy(source.avatar_file, "F");
    source.protocol_state = 2; source.protocol_local = 0x1234; source.protocol_remote = 0x5678;
    chat_core_serialize_member(&source, output);
    struct chat_member_display decoded = {0};
    assert(!chat_core_deserialize_member(output, &decoded));
    assert(!memcmp(decoded.ed25519, source.ed25519, 32));
    assert(decoded.protocol_state == 2 && decoded.protocol_local == 0x1234 && decoded.protocol_remote == 0x5678);
    for (unsigned i = 8; i <= 70; i++) assert(!output[i]);
    for (unsigned i = 104; i <= 172; i++) assert(!output[i]);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS member snapshots:%u SQL/allocation faults,retries,NULL/canonical guards,property event suppression,zero-padded strings", cases);
}

static int veto(void* arg) { (*(unsigned*)arg)++; return 1; }

static void mutes(struct UTUN_INSTANCE* inst) {
    char update[96]; snprintf(update, sizeof(update), "UPDATE \"peers_%s\" SET radio_muted=", channel); target = update; unsigned cases = 0;
    for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
        int bindings = point == F_BIND ? 2 : 1;
        for (int index = 1; index <= bindings; index++) {
            bind_index = index; fault = point; refused_mute(inst); retry_mute(inst); cases++;
        }
    }
    const char* exec[] = {"BEGIN IMMEDIATE /* radio mute */", "COMMIT /* radio mute */"};
    for (unsigned i = 0; i < 2; i++) { target = exec[i]; fault = F_EXEC; refused_mute(inst); retry_mute(inst); cases++; }
    assert(!radio_set_member_muted(inst, group_id, mute_ed, 0));
    target = "radio.c"; fault = F_REALLOCATE; refused_mute(inst); retry_mute(inst); cases++;
    const char* actions[] = {"ABORT,'mute update'", "IGNORE"};
    for (unsigned i = 0; i < 2; i++) {
        char sql[256]; snprintf(sql, sizeof(sql), "CREATE TEMP TRIGGER refuse_mute BEFORE UPDATE OF radio_muted ON peers_%s "
            "BEGIN SELECT RAISE(%s); END", channel, actions[i]);
        assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); refused_mute(inst);
        assert(sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER refuse_mute", NULL, NULL, NULL) == SQLITE_OK); retry_mute(inst);
    }
    unsigned commits = 0; sqlite3_commit_hook(inst->topo_sqlite_db, veto, &commits); refused_mute(inst); assert(commits == 1);
    sqlite3_commit_hook(inst->topo_sqlite_db, NULL, NULL); retry_mute(inst);
    int saved = stored_mute(); unsigned display = member_events, audio = mute_events;
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN;INSERT INTO ui_state(key,value) VALUES('foreign-mute','1')", NULL, NULL, NULL) == SQLITE_OK);
    assert(radio_set_member_muted(inst, group_id, mute_ed, !saved) < 0 && chat_core_toggle_member_radio_mute(inst, channel, mute_ed) < 0);
    assert(!sqlite3_get_autocommit(inst->topo_sqlite_db) && stored_mute() == saved && radio_member_muted(inst, group_id, mute_ed) == saved);
    assert(display == member_events && audio == mute_events);
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    arm_after_commit = 1; retry_mute(inst); assert(!arm_after_commit && fault == F_FINALIZE);
    uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE]; assert(chat_core_get_single_member(inst, channel, mute_ed, wire) < 0 && fault == F_NONE);
    assert(!chat_core_get_single_member(inst, channel, mute_ed, wire) && !!(wire[CHAT_MEMBER_FLAGS_OFF] & CHAT_MEMBER_FLAG_RADIO_MUTED) == stored_mute());
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS member mute:%u SQL/allocation faults,retries,ABORT/IGNORE/veto,foreign transaction,read failure after commit;DB/cache/audio/events agree", cases);
}

static void details(struct UTUN_INSTANCE* inst) {
    finish_members(inst); async_fault = 1;
    char query[96]; snprintf(query, sizeof(query), "SELECT 1 FROM \"peers_%s\" WHERE ed25519_pubkey=", channel); target = query;
    for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
        if (point == F_CHANGES) continue;
        struct chat_member_detail_req* req = u_calloc(1, sizeof(*req)); assert(req);
        *req = (struct chat_member_detail_req){.inst=inst,.request_id=123};
        assert(!sc_hex_to_binary(topo_groups_channel(inst->topo_groups, group_id), req->group, 32)); memcpy(req->ed, mute_ed, 32); bind_index = 1; fault = point; detail_size = 0;
        chat_core_collect_member_detail_trampoline(req); struct chat_member_detail_view view;
        uint64_t end = get_time_us() + 5000000;
        while (!detail_size && get_time_us() < end) uasync_poll(inst->ua, 1);
        assert(fault == F_NONE && !chat_core_parse_member_detail(detail, detail_size, &view));
        assert(view.result == CHAT_MEMBER_DETAIL_ERROR && view.request_id == 123 && !memcmp(view.ed, mute_ed, 32) && !memcmp(view.group, topo_group_wire_id(topo_groups_find(inst->topo_groups, group_id)), 32));
    }
    async_fault = 0;
    struct chat_member_detail_req* req = u_calloc(1, sizeof(*req)); assert(req); req->inst = inst; req->request_id = 124;
    chat_core_collect_member_detail_trampoline(req); struct chat_member_detail_view view;
    assert(!chat_core_parse_member_detail(detail, detail_size, &view) && view.result == CHAT_MEMBER_DETAIL_INVALID && view.request_id == 124);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS member detail:4 SQL faults,complete addressed error replies and invalid request codec");
}

static void node_names(struct UTUN_INSTANCE* inst) {
    target = "SELECT name FROM nodes"; char out[128];
    for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
        if (point == F_CHANGES) continue;
        strcpy(out, "previous"); bind_index = 1; fault = point;
        assert(chat_core_get_node_name(inst, inst->my_keys.public_key, out, sizeof(out)) < 0 && !out[0] && fault == F_NONE);
        assert(!chat_core_get_node_name(inst, inst->my_keys.public_key, out, sizeof(out)) && !strcmp(out, inst->name));
    }
    out[0] = 'x'; assert(chat_core_get_node_name(inst, inst->my_keys.public_key, out, 0) < 0 && out[0] == 'x');
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS node names:4 SQL faults/retries,cleared failure output and zero-size guard");
}

/* Очереди без callbacks: снимок обязан сохранить элементы и байтовые счётчики. */
static struct ll_queue* metrics_queue(struct UTUN_INSTANCE* inst, unsigned count, uint16_t bytes) {
    struct ll_queue* q = queue_new(inst->ua, 0, 0, 0, "metrics_test"); assert(q);
    for (unsigned i = 0; i < count; i++) {
        struct ll_entry* e = ll_alloc_lldgram(bytes); assert(e); e->len = bytes;
        assert(!queue_data_put(q, e));
    }
    return q;
}

static void diagnostics(struct UTUN_INSTANCE* inst) {
    struct ETCP_CONN connection = {.instance = inst, .peer_node_id = 9, .unacked_bytes = UINT32_MAX};
    struct ETCP_LINK* links = u_calloc(300, sizeof(*links)); assert(links); connection.links = links;
    struct ll_queue* acks = queue_new(inst->ua, 0, 0, 0, "member_test_acks"); assert(acks);
    for (int i = 0; i < 300; i++) {
        links[i].etcp = &connection; links[i].ack_q = acks; links[i].next = i < 299 ? links + i + 1 : NULL;
    }
    struct ll_entry* entry = queue_entry_new(sizeof(struct conn_queue_entry)); assert(entry);
    struct conn_queue_entry* item = (struct conn_queue_entry*)entry->data; item->peer_node_id = 9; item->conn = &connection;
    assert(!queue_data_put_with_index(inst->connections, entry)); expect_large_text = 1; unsigned before = text_events;
    struct ll_queue* saved_router = inst->router_conns;
    inst->router_conns = metrics_queue(inst, 0, 0);
    struct ETCP_ROUTER_CONN* sessions[3];
    for (unsigned i = 0; i < 3; i++) {
        sessions[i] = (struct ETCP_ROUTER_CONN*)queue_entry_new(sizeof(*sessions[i]) - sizeof(struct ll_entry)); assert(sessions[i]);
        sessions[i]->inst = inst; sessions[i]->remote_node_id = i ? 10 + i : 9;
        sessions[i]->svc_id = i ? ETCP_RT_ID_DM : ETCP_RT_ID_CALL_AUDIO;
        assert(!queue_data_put(inst->router_conns, &sessions[i]->ll));
    }
    sessions[1]->recv_conn = &connection;
    struct ll_queue* queues[] = {metrics_queue(inst, 2, 101), metrics_queue(inst, 1, 303), metrics_queue(inst, 1, 44),
        metrics_queue(inst, 1, 55), metrics_queue(inst, 2, 333), metrics_queue(inst, 1, 777)};
    sessions[0]->send_q = queues[0]; sessions[0]->inflight_q = queues[1];
    sessions[0]->incoming_q = queues[2]; sessions[0]->recv_q = queues[3];
    struct TRANSIT_QUEUE* transit = (struct TRANSIT_QUEUE*)queue_entry_new(sizeof(*transit) - sizeof(struct ll_entry)); assert(transit);
    transit->q = queues[4]; transit->conn = &connection; transit->src_node_id = 11; transit->dst_node_id = 12;
    connection.transit_queues = metrics_queue(inst, 0, 0); assert(!queue_data_put(connection.transit_queues, &transit->ll));
    struct PKTNORM normalizer = {.input = queues[5]}; connection.normalizer = &normalizer;
    expect_router_metrics = 1;
    chat_core_collect_status_trampoline(inst);
    struct chat_node_arg* req = u_malloc(sizeof(*req)); assert(req); *req = (struct chat_node_arg){inst, 9}; chat_core_collect_conn_metrics_trampoline(req);
    assert(text_events == before + 2);
    target = "strbuf.c"; fault = F_REALLOCATE; chat_core_collect_status_trampoline(inst); assert(fault == F_NONE && text_events == before + 2);
    chat_core_collect_status_trampoline(inst); assert(text_events == before + 3); expect_large_text = 0;
    req = u_malloc(sizeof(*req)); assert(req); *req = (struct chat_node_arg){inst, 9};
    fault = F_REALLOCATE; chat_core_collect_conn_metrics_trampoline(req); assert(fault == F_NONE && text_events == before + 3);
    req = u_malloc(sizeof(*req)); assert(req); *req = (struct chat_node_arg){inst, 9}; chat_core_collect_conn_metrics_trampoline(req);
    assert(text_events == before + 4); expect_router_metrics = 0;
    const unsigned counts[] = {2, 1, 1, 1, 2, 1};
    const size_t bytes[] = {202, 303, 44, 55, 666, 777};
    for (unsigned i = 0; i < sizeof(queues) / sizeof(queues[0]); i++) {
        assert(queue_entry_count(queues[i]) == (int)counts[i] && queue_total_bytes(queues[i]) == bytes[i]);
        assert(!queue_check_consistency(queues[i])); queue_clear(queues[i]); queue_free(queues[i]);
    }
    queue_clear(connection.transit_queues); queue_free(connection.transit_queues); connection.transit_queues = NULL;
    queue_clear(inst->router_conns); queue_free(inst->router_conns); inst->router_conns = saved_router;
    connection.normalizer = NULL;
    queue_remove_data(inst->connections, entry); queue_entry_free(entry); u_free(links); queue_free(acks);
    if (SIZE_MAX > UINT32_MAX) {
        size_t baseline = u_get_allocated_count(); struct strbuf text; strbuf_init(&text, (size_t)UINT32_MAX + 1);
        assert(!text.buf && !text.cap && !text.len && u_get_allocated_count() == baseline);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS diagnostics:300 links,full status/metrics beyond8KiB,OOM suppresses partial event,retry,allocator capacity boundary");
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS router metrics:peer/RX scope,unrelated peer excluded,queue bytes/packets,transit,normalizer,null queues,read-only");
}

static void member_limit(struct UTUN_INSTANCE* inst) {
    char sql[1024]; snprintf(sql, sizeof(sql), "WITH RECURSIVE n(value) AS(SELECT 100000 UNION ALL SELECT value+1 FROM n WHERE value<200000) "
        "INSERT INTO peers_%s(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,source) "
        "SELECT value,zeroblob(32),randomblob(32),zeroblob(64),0,1 FROM n WHERE value<>%llu LIMIT(65535-(SELECT count(*) FROM peers_%s))",
        channel, (unsigned long long)inst->node_id, channel);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    uint8_t* wire = NULL; int count = 0; assert(!chat_core_get_member_list(inst, channel, &wire, &count) && count == UINT16_MAX);
    assert(wire[CHAT_MEMBER_ONLINE_OFF] && !memcmp(wire + CHAT_MEMBER_ED_OFF, inst->my_ed25519_pubkey, 32));
    for (int i = 2; i < count; i++) {
        const uint8_t* row = wire + i * CHAT_MEMBER_DISPLAY_SIZE;
        const uint8_t* previous = row - CHAT_MEMBER_DISPLAY_SIZE;
        if (memcmp(previous + CHAT_MEMBER_ED_OFF, row + CHAT_MEMBER_ED_OFF, 32) >= 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "member order at %d previous=%s current=%s online=%u/%u", i,
                sc_public_key_hex(previous + CHAT_MEMBER_ED_OFF, (char[65]){0}), sc_public_key_hex(row + CHAT_MEMBER_ED_OFF, (char[65]){0}), previous[CHAT_MEMBER_ONLINE_OFF], row[CHAT_MEMBER_ONLINE_OFF]);
            assert(0);
        }
    }
    u_free(wire); struct chat_member_list_req* req = u_malloc(sizeof(*req)); assert(req);
    req->inst = inst; strcpy(req->ch_id, channel); unsigned before = list_events; chat_core_request_member_list_trampoline(req);
    finish_members(inst); assert(list_events == before + 1);
    uint64_t extra = inst->node_id == 9 ? 10 : 9;
    assert(!topo_node_sqlite_member_placeholder_put(inst->topo_sqlite_db, channel, extra, inst->my_keys.public_key, (const uint8_t[32]){0x8e}));
    wire = NULL; count = 0; assert(chat_core_get_member_list(inst, channel, &wire, &count) < 0 && !wire && !count);
    req = u_malloc(sizeof(*req)); assert(req); req->inst = inst; strcpy(req->ch_id, channel); chat_core_request_member_list_trampoline(req);
    finish_members(inst);
    assert(list_events == before + 1 && !topo_node_sqlite_member_del(inst->topo_sqlite_db, channel, extra));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS member limit:65535 exact sorted members and complete event;65536 refused without wrapped count/truncated event");
}

static char local_attributes[2048];
static void read_local_attributes(void) {
    char sql[160]; snprintf(sql, sizeof(sql), "SELECT local_attrs FROM msg_%s LIMIT 1", channel);
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(reader, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); const char* text = (const char*)sqlite3_column_text(st, 0);
    snprintf(local_attributes, sizeof(local_attributes), "%s", text ? text : ""); assert(sqlite3_finalize(st) == SQLITE_OK);
}

static void message_queries(struct UTUN_INSTANCE* inst) {
    struct chat_msg_submit req = {.inst = inst, .data = (uint8_t*)"hello", .data_len = 5};
    strcpy(req.channel_id, channel); strcpy(req.content_type, "text"); assert(!chat_core_submit_message(inst, &req));
    assert(chat_core_count(inst, channel) == 1);
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT author_signature,message_id FROM msg_%s LIMIT 1", channel);
    sqlite3_stmt* st = NULL; uint8_t signature[64], message_id[32];
    assert(sqlite3_prepare_v2(reader, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_bytes(st, 0) == 64); memcpy(signature, sqlite3_column_blob(st, 0), 64);
    assert(sqlite3_column_bytes(st, 1) == 32); memcpy(message_id, sqlite3_column_blob(st, 1), 32);
    assert(sqlite3_finalize(st) == SQLITE_OK);
    char attributes[1024]; snprintf(attributes, sizeof(attributes), "{\"keep\":\"%0500d\",\"pl\":10,\"nested\":{\"pl\":1}}", 1);
    assert(!chat_core_update_local_attrs(inst, channel, req.timestamp, signature, attributes));
    chat_core_mark_voice_played(inst, channel, message_id); read_local_attributes();
    assert(strlen(local_attributes) > 500 && strstr(local_attributes, "\"pl\":1,") && strstr(local_attributes, "\"nested\":{\"pl\":1}"));
    assert(chat_core_update_local_attrs(inst, channel, req.timestamp, signature, "[]") < 0);
    const enum fault points[] = {F_PREPARE, F_BIND, F_STEP, F_FINALIZE, F_EXEC, F_EXEC};
    for (unsigned mode = 0; mode < 2; mode++) for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        assert(!chat_core_update_local_attrs(inst, channel, req.timestamp, signature, "{}"));
        target = i == 4 ? "BEGIN IMMEDIATE /* local attrs */" : i == 5 ? "COMMIT /* local attrs */" : "UPDATE \"msg_";
        fault = points[i]; bind_index = 1;
        if (mode) chat_core_mark_voice_played(inst, channel, message_id);
        else assert(chat_core_update_local_attrs(inst, channel, req.timestamp, signature, attributes) < 0);
        assert(fault == F_NONE && sqlite3_get_autocommit(inst->topo_sqlite_db)); read_local_attributes(); assert(!strcmp(local_attributes, "{}"));
    }
    target = NULL;
    chat_core_mark_voice_played(inst, channel, message_id); read_local_attributes(); assert(!strcmp(local_attributes, "{\"pl\":1}"));
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    assert(chat_core_update_local_attrs(inst, channel, req.timestamp, signature, "{}") < 0);
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);

    uint8_t buf[1024], before[1024]; size_t length;
    for (unsigned kind = 0; kind < 4; kind++) {
        const char* prefixes[] = {"SELECT channel_id FROM channels ORDER", "SELECT ed25519_pubkey FROM peers_",
            "SELECT x25519_pubkey,ed25519_pubkey FROM nodes", "SELECT family,protocol,address,port,rtt"};
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            if (point == F_CHANGES || ((kind < 2) && point == F_BIND)) continue;
            memset(buf, 0xa5, sizeof(buf)); memcpy(before, buf, sizeof(buf)); length = 77;
            target = prefixes[kind]; fault = point; bind_index = 1;
            int rc = kind == 0 ? chat_core_list_channels(inst, buf, sizeof(buf), &length)
                : kind == 1 ? chat_core_list_peers(inst, channel, buf, sizeof(buf), &length)
                : chat_core_load_nodeinfo(inst, inst->my_ed25519_pubkey, buf, sizeof(buf), &length);
            assert(rc < 0 && fault == F_NONE && !length && !memcmp(buf, before, sizeof(buf)));
        }
    }
    target = NULL;
    assert(!chat_core_list_channels(inst, buf, 2, &length) && length == 2 && !buf[0] && !buf[1]);
    assert(!chat_core_list_peers(inst, channel, buf, 34, &length) && length == 34 && buf[0] == 1 && !buf[1] &&
           !memcmp(buf + 2, inst->my_ed25519_pubkey, 32));
    assert(!chat_core_load_nodeinfo(inst, inst->my_ed25519_pubkey, buf, 65, &length) && length == 65 && !buf[64]);
    memset(buf, 0xa5, sizeof(buf)); assert(chat_core_list_channels(inst, buf, 1, &length) < 0 && !length && buf[0] == 0xa5);
    assert(chat_core_load_nodeinfo(inst, inst->my_ed25519_pubkey, buf, 64, &length) < 0 && !length && buf[0] == 0xa5);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS message queries:JSON preservation beyond256 bytes,12 write faults/rollback,14 reader faults,exact buffer bounds");
}

static void archive_lifecycle(struct UTUN_INSTANCE* inst) {
    chat_event_set_handler(inst, NULL);
    struct chat_msg_submit request = {.content_type = "text", .data = (uint8_t*)"archive boundary", .data_len = 16};
    snprintf(request.channel_id, sizeof(request.channel_id), "%s", channel);
    assert(!chat_core_submit_message(inst, &request));
    uint8_t reaction[8], emoji = 5; int reaction_size = chat_reactions_encode(&emoji, 1, reaction, sizeof(reaction));
    assert(reaction_size > 0 && chat_feedback_set(inst, channel, request.message_id, CHAT_FEEDBACK_REACTION,
        reaction, reaction_size, NULL) == 1);
    char* feedback_before = NULL; assert(!chat_feedback_snapshot(inst->topo_sqlite_db, inst->my_ed25519_pubkey, channel, request.message_id, 0, &feedback_before));
    struct DB_SYNC_INSTANCE* si = db_sync_instance_find(inst, group_id); assert(si);
    uint32_t before = chat_core_count(inst, channel);
    char sql[256], data[DB_SYNC_MESSAGE_MAX + 1]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT data,nonce,generation,author_signature FROM msg_%s WHERE message_id=?", channel);
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, request.message_id, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    size_t length = sqlite3_column_bytes(st, 0); assert(length < sizeof(data)); memcpy(data, sqlite3_column_blob(st, 0), length);
    struct db_message_revision old = {.ver = 1}; uint8_t signature[64];
    memcpy(old.nonce, sqlite3_column_blob(st, 1), 16); memcpy(old.generation, sqlite3_column_blob(st, 2), 32);
    memcpy(signature, sqlite3_column_blob(st, 3), 64); assert(sqlite3_finalize(st) == SQLITE_OK);
    struct chat_member_tags* tags = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(tags);
    assert(!chat_member_tags_set(tags, "deleted", "yes"));
    target = "COMMIT /* tags */"; fault = F_EXEC;
    assert(chat_member_tags_commit(inst, tags) < 0 && fault == F_NONE && sqlite3_get_autocommit(inst->topo_sqlite_db)); target = NULL;
    uint8_t retained_generation[32];
    assert(chat_core_count(inst, channel) == before && db_sync_count(si) == before);
    assert(!group_policy_message_generation(inst->topo_sqlite_db, channel, inst->node_id, retained_generation)
        && !memcmp(retained_generation, old.generation, 32));
    assert(!chat_member_tags_commit(inst, tags)); chat_member_tags_free(tags);
    assert(chat_core_count(inst, channel) == before && db_sync_count(si) == before && !db_sync_verify(si));
    char* feedback_after = NULL; assert(!chat_feedback_snapshot(inst->topo_sqlite_db, inst->my_ed25519_pubkey, channel, request.message_id, 0, &feedback_after));
    assert(!strcmp(feedback_before, feedback_after)); u_free(feedback_after);
    char json[8192]; size_t bytes;
    assert(!chat_core_get_messages_json(inst, channel, 100, 0, json, sizeof(json), &bytes) && strstr(json, "\"archived\":0"));
    assert(!chat_core_update_local_attrs(inst, channel, request.timestamp, signature, "{\"archive_local\":true}"));
    assert(chat_core_edit_message(inst, channel, request.message_id, 1, "forbidden", 9) < 0);
    tags = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(tags);
    assert(!chat_member_tags_set(tags, "deleted", "no") && !chat_member_tags_commit(inst, tags)); chat_member_tags_free(tags);
    assert(db_sync_count(si)==before && chat_core_count(inst, channel) == before);
    assert(db_sync_insert_signed(si, data, length, signature, 64, request.timestamp, NULL, &old) == -1);
    assert(chat_feedback_set(inst, channel, request.message_id, CHAT_FEEDBACK_REACTION, NULL, 0, NULL) == 1);
    assert(!chat_feedback_snapshot(inst->topo_sqlite_db, inst->my_ed25519_pubkey, channel, request.message_id, 0, &feedback_after));
    assert(strcmp(feedback_before, feedback_after)); u_free(feedback_before); u_free(feedback_after);
    struct chat_msg_submit fresh = {.content_type = "text", .data = (uint8_t*)"new membership", .data_len = 14};
    snprintf(fresh.channel_id, sizeof(fresh.channel_id), "%s", channel); assert(!chat_core_submit_message(inst, &fresh));
    assert(db_sync_count(si) == before+1 && chat_core_count(inst, channel) == before + 1);
    chat_service_stop(inst); assert(chat_service_start(inst) == 0);
    assert(chat_core_count(inst, channel) == before + 1 && db_sync_count(db_sync_instance_find(inst, group_id)) == before+1);
    assert(!chat_core_get_messages_json(inst, channel, 100, 0, json, sizeof(json), &bytes) && strstr(json, "archive_local"));
    chat_event_set_handler(inst, event);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS archive lifecycle: revoke/freeze, local attrs, immutable edit, fresh admission, old replay denied, restart");
}

int main(void) {
    loop_thread = pthread_self();
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    char directory[512] = "/tmp/utun-member-sql-XXXXXX"; assert(!test_mkdtemp(directory)); char config[1024], database[600];
    snprintf(config, sizeof(config), "[global]\nmy_node_name=member test\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !utun_core_start(inst) && !chat_service_start(inst));
    group_id = chat_core_create_channel_auto(inst, "member SQL"); assert(group_id);
    finish_members(inst);
    strcpy(channel, topo_groups_channel(inst->topo_groups, group_id));
    uint8_t* wire = NULL; int count = 0; fail_query = 1;
    int rc = chat_core_get_member_list(inst, channel, &wire, &count);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "member test: finalize fault result=%d count=%d fault_pending=%d", rc, count, fail_query);
    assert(rc < 0 && !wire && !count && !fail_query);
    assert(!chat_core_get_member_list(inst, channel, &wire, &count) && count == 1); u_free(wire);
    const uint64_t peers[] = {7, 8, UINT64_MAX};
    for (unsigned i = 0; i < 3; i++) {
        struct SC_MYKEYS keys; uint8_t ed[32], seed[32];
        assert(!sc_identity_generate(keys.public_key, keys.private_key, ed, seed));
        assert(!topo_node_sqlite_member_placeholder_put(inst->topo_sqlite_db, channel, peers[i], keys.public_key, ed));
        if (peers[i] == 7) memcpy(mute_ed, ed, 32);
    }
    snprintf(database, sizeof(database), "%s/chats.db", directory); assert(sqlite3_open_v2(database, &reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    chat_event_set_handler(inst, event); radio_set_mute_cb(inst, mute, NULL);
    snapshots(inst); mutes(inst); details(inst); node_names(inst); diagnostics(inst); message_queries(inst); finish_members(inst);
    archive_lifecycle(inst); finish_members(inst); member_limit(inst);
    assert(!radio_set_member_muted(inst, group_id, mute_ed, 0)); unsigned display = member_events, audio = mute_events;
    target = "COMMIT /* radio mute */"; fault = F_EXEC; fail_rollback = 1; inst->running = 1;
    assert(chat_core_toggle_member_radio_mute(inst, channel, mute_ed) < 0 && fault == F_NONE && !fail_rollback && !inst->running);
    assert(!sqlite3_get_autocommit(inst->topo_sqlite_db) && !stored_mute() && !radio_member_muted(inst, group_id, mute_ed));
    assert(display == member_events && audio == mute_events);
    utun_instance_destroy(inst); assert(!stored_mute() && sqlite3_close(reader) == SQLITE_OK);
    uasync_poll(ua, 0); uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS member snapshot SQL refusal and retry"); return 0;
}
