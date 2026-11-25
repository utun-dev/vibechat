/* Реальные core/SQLite/локальная регистрация invite;проверяем отказы каждого SQL-этапа чтения. */
#define _GNU_SOURCE
#include "../src/chat/invite_build.c"
#include <assert.h>

enum fault { NONE, PREPARE, BIND, STEP, FINALIZE };
static enum fault fault;
static const char* target;
static unsigned skip_steps, calls;
static int last_result;
static uint64_t expected_channel;
static struct UTUN_INSTANCE* active;
static int matches(sqlite3_stmt* st) { return st && target && !strncmp(sqlite3_sql(st), target, strlen(target)); }
extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (fault == PREPARE && target && !strncmp(sql, target, strlen(target))) { fault = NONE; *st = NULL; return SQLITE_IOERR; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, sqlite3_destructor_type);
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int size, sqlite3_destructor_type destroy) {
    if (fault == BIND && matches(st)) { fault = NONE; return SQLITE_IOERR; }
    return __real_sqlite3_bind_blob(st, index, value, size, destroy);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (fault == STEP && matches(st)) {
        if (skip_steps) skip_steps--; else { fault = NONE; return SQLITE_IOERR; }
    }
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = fault == FINALIZE && matches(st); if (fail) fault = NONE;
    int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_IOERR : rc;
}
static void ready(void* arg, int result, const char* link) {
    (void)arg; calls++; last_result = result;
    if (result == CHAT_JOIN_OK) {
        struct InviteData decoded; char error[128];
        assert(link && !invite_link_decode(link, strlen(link), &decoded, error, sizeof(error)));
        assert(chat_join_key_group(active, decoded.join_key) == expected_channel && !memcmp(decoded.pubkey, active->my_keys.public_key, 32));
        assert(!memcmp(decoded.inviter_pubkey, active->my_ed25519_pubkey, 32));
        assert(chat_join_key_matches(active, expected_channel, decoded.join_key, active->my_ed25519_pubkey));
    } else assert(!link);
}
static int read_query(unsigned query, struct UTUN_INSTANCE* inst, uint64_t channel) {
    uint8_t ids[4][32] = {{77},{88},{99},{111}}, before[4][32]; memcpy(before, ids, sizeof(ids));
    struct InviteAddr addresses[INVITE_ADDR_MAX]; struct InviteReality reality;
    int rc;
    if (!query) {
        rc = chat_invite_candidate_nodes(inst, channel, ids, 4);
        if (rc < 0) assert(!memcmp(ids, before, sizeof(ids)));
    } else if (query == 1) rc = node_has_both_public(CC(inst)->db, inst->my_ed25519_pubkey);
    else rc = collect_node_addrs_from_db(CC(inst)->db, inst->my_ed25519_pubkey, addresses, INVITE_ADDR_MAX, 0, &reality);
    return rc;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
    size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    struct UASYNC* ua = uasync_create(); assert(ua);
    active = utun_instance_create_from_str(ua, "[global]\nmy_node_name=invite test\n[server: udp]\naddr=127.0.0.1:0\n"
        "type=public\n[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n");
    assert(active && !utun_core_start(active) && !chat_service_start(active));
    expected_channel = chat_core_create_channel_auto(active, "invite test"); assert(expected_channel);
    const char* queries[] = {"SELECT ed25519_pubkey,node_type", "SELECT family FROM node_addresses", "SELECT family,socket_id"};
    unsigned cases = 0;
    for (unsigned q = 0; q < 3; q++) for (enum fault point = PREPARE; point <= FINALIZE; point++) {
        if (!q && point == BIND) continue;
        target = queries[q]; fault = point; skip_steps = !q && point == STEP ? 1 : 0;
        assert(read_query(q, active, expected_channel) < 0 && fault == NONE);
        assert(read_query(q, active, expected_channel) >= 0); cases++;
    }
    target = queries[0]; fault = FINALIZE; uint8_t sentinel[32] = {77}, selected[32]; memcpy(selected, sentinel, sizeof(selected));
    assert(chat_invite_best_node(active, expected_channel, selected) < 0 && fault == NONE && !memcmp(selected, sentinel, 32));
    assert(!chat_invite_best_node(active, expected_channel, selected) && !memcmp(selected, active->my_ed25519_pubkey, 32));
    target = NULL;
    struct chat_invite_build* pending = chat_invite_build_link(active, expected_channel, 0, ready, NULL);
    assert(pending && !calls);
    for (unsigned i = 0; i < 100 && !calls; i++) uasync_poll(ua, 1);
    assert(calls == 1 && last_result == CHAT_JOIN_OK);
    pending = chat_invite_build_link(active, expected_channel, active->my_ed25519_pubkey, ready, NULL); assert(pending);
    chat_invite_build_cancel(pending); assert(calls == 2 && last_result == CHAT_JOIN_CANCELLED);
    assert(!chat_invite_build_link(active, expected_channel, (const uint8_t[32]){42}, ready, NULL));
    memcpy(selected, sentinel, 32); assert(chat_invite_best_node(NULL, expected_channel, selected) < 0 && !memcmp(selected, sentinel, 32));
    assert(chat_invite_best_node(active, 0, selected) < 0 && !memcmp(selected, sentinel, 32));
    utun_instance_destroy(active); active = NULL; uasync_poll(ua, 0); uasync_destroy(ua, 0);
    assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS invite:%u SQL fault/retries,atomic outputs,shared address projection,self ACK/cancel,invalid target", cases);
    return 0;
}
