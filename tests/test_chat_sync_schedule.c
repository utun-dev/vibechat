#include "../src/group/group_protocol.h"
/* Реальные callbacks, БД, lifecycle и uasync; наблюдаем только границу запуска member_sync.
 * Private-реализация включена как в test_merkle_protocol: публичный API не расширяем ради теста. */
#include "chat/member_sync.h"
#include "transport_layer/etcp_api.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include "test_clock.h"
#include "test_membership.h"
#include "test_utils.h"
#include "test_join_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int observe_start(struct UTUN_INSTANCE* inst, struct ETCP_CONN* peer, const char* channel, merkle_sync_done_cb cb, void* arg);
static int observe_pull(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* channel,
    merkle_sync_done_cb cb, void* arg);
static int observe_admin_start(struct UTUN_INSTANCE* inst, uint8_t dataset, struct ETCP_CONN* peer, const char* channel,
    merkle_sync_done_cb cb, void* arg);
static int discard_send(struct ETCP_CONN* conn, struct ll_entry* entry);
static void* schedule_timeout(struct UASYNC* ua, int tb, void* arg, void (*cb)(void*), const char* name);
static void* cache_malloc(uint32_t bytes, const char* site);
static void* cache_realloc(void* old, uint32_t bytes, const char* site);
#define member_sync_start observe_start
#define merkle_sync_join_pull observe_pull
#define merkle_sync_start observe_admin_start
#define etcp_send discard_send
#define uasync_set_timeout schedule_timeout
#define u_malloc_impl cache_malloc
#define u_realloc_impl cache_realloc
#include "../src/chat/chat_sync.c"
#undef member_sync_start
#undef merkle_sync_join_pull
#undef merkle_sync_start
#undef etcp_send
#undef uasync_set_timeout
#undef u_malloc_impl
#undef u_realloc_impl

static unsigned cache_allocations, fail_cache_allocation;
static void* cache_malloc(uint32_t bytes, const char* site) {
    if (++cache_allocations == fail_cache_allocation) return NULL;
    return u_malloc_impl(bytes, site);
}
static void* cache_realloc(void* old, uint32_t bytes, const char* site) {
    if (++cache_allocations == fail_cache_allocation) return NULL;
    return u_realloc_impl(old, bytes, site);
}

struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst;
    struct ETCP_CONN* peers[3];
    struct SC_MYKEYS peer_keys[3];
    struct NODE_CONN_DIRECT* owners[3]; /* Базовый спрос транспорта в тесте; observe_start не запускает настоящий Merkle. */
    char channel[2][65];
    uint64_t gid[2];
};

static struct { uint64_t peer, channel; } starts[128];
static unsigned start_count;
static int fail_timer, fail_refresh_timer;
static const char* fail_join_timer;
static struct fixture* injecting;

static int observe_start(struct UTUN_INSTANCE* inst, struct ETCP_CONN* peer, const char* channel, merkle_sync_done_cb cb, void* arg) {
    (void)inst; (void)cb; (void)arg;
    assert(start_count < sizeof(starts) / sizeof(starts[0]));
    starts[start_count].peer = peer->peer_node_id; starts[start_count++].channel = topo_groups_channel_slot(inst->topo_groups, channel);
    if (injecting) {
        struct fixture* f = injecting; injecting = NULL;
        cs_on_conn_down(f->peers[1], ETCP_CBK_EVENT_DOWN, f->inst->chat_sync);
        cs_schedule_peer_sync(f->inst->chat_sync, f->peers[2], "during_flush");
    }
    return 0;
}

static int observe_pull(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* channel,
    merkle_sync_done_cb cb, void* arg) {
    struct ETCP_CONN* peer = instance_find_conn_by_key(inst, path->exit_x);
    assert(peer && !memcmp(path->inviter, peer->peer_ed25519_pubkey, 32));
    return observe_start(inst, peer, channel, cb, arg);
}

static int observe_admin_start(struct UTUN_INSTANCE* inst, uint8_t dataset, struct ETCP_CONN* peer, const char* channel,
    merkle_sync_done_cb cb, void* arg) {
    /* Здесь проверяется очередь обычного MEMBERS. Живой MEMBER_ADMIN проверяет test_chat_poll_network. */
    assert(dataset == MT_DATA_MEMBER_ADMIN && !cb && !arg && member_sync_admin_allowed(inst, channel, peer));
    return 0;
}

static int discard_send(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn;
    queue_dgram_free(entry); queue_entry_free(entry);
    return 0;
}

static void* schedule_timeout(struct UASYNC* ua, int tb, void* arg, void (*cb)(void*), const char* name) {
    if (fail_timer && !strcmp(name, "cs_sync")) { fail_timer = 0; return NULL; }
    if (fail_refresh_timer && !strcmp(name, "cs_refresh")) { fail_refresh_timer = 0; return NULL; }
    if (fail_join_timer && !strcmp(name, fail_join_timer)) { fail_join_timer = NULL; return NULL; }
    return uasync_set_timeout(ua, tb, arg, cb, name);
}

static void run_ms(struct fixture* f, unsigned ms) { test_clock_run_for(f->ua, ms * 10); }

/* Два канала: A состоит только в первом, B в обоих, C только во втором. */
static void setup(struct fixture* f) {
    memset(f, 0, sizeof(*f)); start_count = 0; fail_timer = 0; injecting = NULL;
    f->ua = uasync_create(); assert(f->ua);
    f->inst = utun_instance_create_from_str(f->ua,
        "[global]\nmy_node_name=schedule test\n[server: udp]\naddr=127.0.0.1:0\ntype=public\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n");
    assert(f->inst && utun_core_start(f->inst) == 0 && chat_service_start(f->inst) == 0);
    chat_core_create_channel_auto(f->inst, "schedule 1"); chat_core_create_channel_auto(f->inst, "schedule 2");
    unsigned channels = 0;
    for (struct ll_entry* e = f->inst->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        assert(group->group_type == TOPO_GROUP_TYPE_CHAT && channels < 2);
        strcpy(f->channel[channels], group->channel_id);
        f->gid[channels++] = group->group_id;
    }
    assert(channels == 2);
    for (unsigned i = 0; i < 3; i++) {
        struct SC_MYKEYS keys; uint8_t ed[32], seed[32];
        assert(sc_identity_generate(keys.public_key, keys.private_key, ed, seed) == SC_OK);
        f->peer_keys[i] = keys;
        struct ETCP_CONN* conn = etcp_connection_create(f->inst, "schedule peer"); assert(conn);
        conn->peer_node_id = sc_derive_node_id_from_pubkey(keys.public_key);
        memcpy(conn->peer_ed25519_pubkey, ed, 32);
        conn->initialized = 1; conn->links_up = 1; conn->state = 1;
        assert(sc_init_ctx(&conn->crypto_ctx, &f->inst->my_keys) == SC_OK);
        assert(sc_set_peer_public_key(&conn->crypto_ctx, keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
        assert(etcp_conn_index_peer(conn) == 0); f->peers[i] = conn;
        assert(!node_conn_direct_hold(conn, &f->owners[i]));
        for (unsigned ch = 0; ch < 2; ch++) {
            if ((i == 0 && ch == 1) || (i == 2 && ch == 0)) continue;
            /* Подготовить состав до запуска callback планировщика. */
            sqlite3* db = f->inst->topo_sqlite_db;
            uint8_t gx[32], ged[32], group_seed[32], body[MEMBER_JOIN_MSG_SIZE], join[64], invitation[64], profile[64];
            assert(!topo_node_sqlite_channel_get(db, f->channel[ch], NULL, 0, NULL, gx, ged, NULL));
            assert(!topo_node_sqlite_channel_get_priv(db, f->channel[ch], group_seed));
            assert(member_sync_build_join_msg(gx, ged, ed, keys.public_key, body, sizeof(body)) == sizeof(body));
            assert(!sc_ed25519_sign(seed, body, sizeof(body), join));
            assert(!member_sync_sign_join(group_seed, f->channel[ch], NULL, keys.public_key, ed, TEST_JOIN_ID(gx, ged, ed, keys.public_key), 1, invitation));
            const char* userinfo = "{\"name\":\"source\"}";
            int size = member_sync_build_update_msg(TEST_JOIN_ID(gx, ged, ed, keys.public_key), 1, userinfo, body, sizeof(body));
            assert(size > 0 && !sc_ed25519_sign(seed, body, (size_t)size, profile));
            struct group_policy_update* update = NULL;
            assert(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
            struct member_sync_write_context writer = {.self=f->inst->node_id};
            struct ms_member_rec member = {.node_id=conn->peer_node_id,.x25519=keys.public_key,.ed25519=ed,.join_sig=join,
                .join_ts=1,.signature=invitation,.update_sig=profile,.update_ts=1,.userinfo=userinfo};
            assert(member_sync_record_write(db, &writer, f->channel[ch], NULL, &member) & MS_APPLY_CHANGED);
            assert(!group_policy_project(db, f->inst->node_id, f->channel[ch], &update));
            assert(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
            group_policy_snapshot_publish(f->inst, group_policy_update_take_snapshot(update));
            group_policy_discard(update);
        }
    }
    /* Кеш планировщика cs->channels намеренно не обновляем: доступ берётся из опубликованного MEMBERS. */
}

static void cleanup(struct fixture* f) {
    for (unsigned i = 0; i < 3; i++) node_conn_direct_close(f->owners[i]);
    utun_instance_destroy(f->inst); uasync_poll(f->ua, 0);
    assert(f->ua->timer_alloc_count == f->ua->timer_free_count);
    assert(f->ua->socket_alloc_count == f->ua->socket_free_count);
    uasync_destroy(f->ua, 0);
}

static unsigned peer_starts(struct fixture* f, unsigned peer) {
    unsigned count = 0;
    for (unsigned i = 0; i < start_count; i++) if (starts[i].peer == f->peers[peer]->peer_node_id) count++;
    return count;
}

static void test_reinit_isolation(void) {
    struct fixture f; setup(&f);
    for (unsigned i = 0; i < 3; i++) etcp_fire_conn_status(f.peers[i], ETCP_CONN_STATUS_UP);
    assert(!start_count); run_ms(&f, 1100);
    assert(start_count == 4 && peer_starts(&f, 0) == 1 && peer_starts(&f, 1) == 2 && peer_starts(&f, 2) == 1);
    start_count = 0;
    for (unsigned i = 0; i < 12; i++) {
        etcp_fire_conn_status(f.peers[0], ETCP_CONN_STATUS_REINIT); run_ms(&f, 200);
        assert(!peer_starts(&f, 1) && !peer_starts(&f, 2));
    }
    run_ms(&f, 1100); assert(start_count == 1 && starts[0].channel == f.gid[0]);
    run_ms(&f, 4000); assert(start_count == 1);
    cleanup(&f); puts("PASS: repeated REINIT is isolated; unchanged peers and idle service start no new rounds");
}

static void test_coalesce_and_deadline(void) {
    struct fixture f; setup(&f);
    cs_schedule_peer_sync(f.inst->chat_sync, f.peers[1], "transport_up");
    assert(node_conn_direct_handle_count(f.peers[1]) == 3); /* Базовый владелец и два разных канала. */
    for (unsigned i = 0; i < 7; i++) {
        run_ms(&f, 100); cs_schedule_peer_sync(f.inst->chat_sync, f.peers[0], "reinit");
        cs_schedule_peer_sync(f.inst->chat_sync, f.peers[1], "transport_up");
        assert(node_conn_direct_handle_count(f.peers[1]) == 3); /* Повторные задания не добавляют владельцев. */
    }
    run_ms(&f, 400);
    assert(start_count == 3 && peer_starts(&f, 0) == 1 && peer_starts(&f, 1) == 2 && !peer_starts(&f, 2));
    for (unsigned i = 0; i < 3; i++) assert(node_conn_direct_handle_count(f.peers[i]) == 1);
    cleanup(&f); puts("PASS: duplicate pairs coalesce and other peers cannot move the first deadline");
}

static void test_sleep_and_cancel(void) {
    struct fixture f; setup(&f);
    etcp_fire_conn_status(f.peers[0], ETCP_CONN_STATUS_UP); etcp_fire_conn_status(f.peers[1], ETCP_CONN_STATUS_UP);
    f.peers[0]->peer_sleep_phase = 1; utun_fire_peer_sleep_cbk(f.peers[0], 1);
    run_ms(&f, 1100); assert(start_count == 2 && peer_starts(&f, 1) == 2);
    start_count = 0; f.peers[0]->peer_sleep_phase = 0;
    utun_fire_peer_sleep_cbk(f.peers[0], 0);
    etcp_fire_conn_status(f.peers[0], ETCP_CONN_STATUS_UP); run_ms(&f, 1100);
    assert(start_count == 1 && peer_starts(&f, 0) == 1);
    start_count = 0;
    etcp_fire_conn_status(f.peers[0], ETCP_CONN_STATUS_UP); etcp_fire_conn_status(f.peers[1], ETCP_CONN_STATUS_UP);
    f.peers[0]->links_up = 0; etcp_fire_conn_status(f.peers[0], ETCP_CONN_STATUS_DOWN);
    run_ms(&f, 1100); assert(start_count == 2 && peer_starts(&f, 1) == 2);
    start_count = 0;
    etcp_fire_conn_status(f.peers[1], ETCP_CONN_STATUS_UP);
    char sql[160]; snprintf(sql, sizeof(sql), "UPDATE peers_%s SET deleted=1 WHERE node_id=%lld", f.channel[0],
        (long long)f.peers[1]->peer_node_id);
    assert(sqlite3_exec(f.inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(!test_member_publish_access(f.inst, f.channel[0]));
    run_ms(&f, 1100); assert(start_count == 1 && starts[0].channel == f.gid[1]);
    cleanup(&f); puts("PASS: sleep, DOWN and changed membership cancel only the affected requests");
}

static void setup_wait_ready(struct fixture* f, const struct ETCP_CONN* peer) {
    struct chat_sync* cs = f->inst->chat_sync;
    struct join_recovery* item = u_calloc(1, sizeof(*item)); assert(item);
    strcpy(item->channel, f->channel[0]);
    strcpy(cs->join.recovery_channel, item->channel);
    memcpy(item->hints.pubkey, peer->crypto_ctx.peer_public_key, 32);
    memcpy(item->hints.inviter_pubkey, peer->peer_ed25519_pubkey, 32);
    item->retry_at = UINT64_MAX;
    item->next = cs->recovery; cs->recovery = item;
    cs->join.phase = CS_JOIN_WAIT_READY;
}

static void test_join_existing_transport(void) {
    struct fixture f; setup(&f);
    assert(!start_count);
    char sql[160]; snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=0 WHERE node_id=%lld", f.channel[0], (long long)f.inst->node_id);
    assert(!sqlite3_exec(f.inst->topo_sqlite_db, sql, NULL, NULL, NULL) && !test_member_publish_access(f.inst, f.channel[0]));
    assert(cs_join_reserve(f.inst->chat_sync, f.gid[0], f.peers[1]->crypto_ctx.peer_public_key, 0, 0) == 0);
    setup_wait_ready(&f, f.peers[1]);
    memcpy(f.inst->chat_sync->join.inviter_ed, f.peers[1]->peer_ed25519_pubkey, 32);
    cs_join_begin_members(f.inst->chat_sync, "ready", 1);
    run_ms(&f, 1100);
    assert(start_count == 1 && starts[0].peer == f.peers[1]->peer_node_id && starts[0].channel == f.gid[0]);
    cleanup(&f); puts("PASS: JOIN_READY starts the pinned MEMBERS endpoint on the existing transport");
}

static void test_reentrant_cancel(void) {
    struct fixture f; setup(&f);
    etcp_fire_conn_status(f.peers[0], ETCP_CONN_STATUS_UP); etcp_fire_conn_status(f.peers[1], ETCP_CONN_STATUS_UP);
    injecting = &f; run_ms(&f, 1100);
    assert(start_count == 1 && peer_starts(&f, 0) == 1);
    run_ms(&f, 1100); assert(start_count == 2 && peer_starts(&f, 2) == 1 && !peer_starts(&f, 1));
    cleanup(&f); puts("PASS: callbacks can cancel remaining pairs and defer newly queued work");
}

static void test_lifecycle_and_timer_failure(void) {
    struct fixture f; setup(&f);
    fail_timer = 1; etcp_fire_conn_status(f.peers[0], ETCP_CONN_STATUS_UP);
    assert(!f.inst->chat_sync->sync_pending && !f.inst->chat_sync->sync_timer);
    etcp_fire_conn_status(f.peers[1], ETCP_CONN_STATUS_UP);
    chat_sync_on_channel_deleted(f.inst, f.channel[0], NULL); run_ms(&f, 1100);
    assert(start_count == 1 && starts[0].channel == f.gid[1]);
    start_count = 0; etcp_fire_conn_status(f.peers[1], ETCP_CONN_STATUS_UP);
    group_service_stop(f.inst); run_ms(&f, 1100); assert(!start_count);
    assert(chat_service_start(f.inst) == 0); assert(!start_count);
    run_ms(&f, 1100); assert(start_count == 4);
    group_service_stop(f.inst); fail_refresh_timer = 1;
    assert(chat_service_start(f.inst) < 0 && !f.inst->chat_sync && !fail_refresh_timer);
    assert(!chat_service_start(f.inst));
    cleanup(&f); puts("PASS: timer failure recovers; channel/stop cancellation and startup retain correct scope");
}

static struct fixture* deleting;
static void delete_from_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (!deleting || type != CHAT_EVT_MEMBER_UPDATED) return;
    assert(len > 1 && data[0] == 64);
    char channel[65]; memcpy(channel, data + 1, data[0]); channel[data[0]] = 0;
    deleting = NULL; chat_core_delete_channel(inst, channel);
}

static void test_cache_snapshot(void) {
    struct fixture f; setup(&f);
    char sql[512];
    snprintf(sql, sizeof(sql), "WITH RECURSIVE n(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM n WHERE id<300)"
        " INSERT INTO peers_%s(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts)"
        " SELECT id,zeroblob(32),randomblob(32),zeroblob(64),1 FROM n", f.channel[0]);
    assert(sqlite3_exec(f.inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    cache_allocations = 0; assert(!cs_refresh_channels(f.inst->chat_sync));
    unsigned allocations = cache_allocations; assert(allocations > 4);
    struct chat_sync* cs = f.inst->chat_sync;
    int largest = 0; for (int i = 0; i < cs->channel_count; i++) if (cs->channels[i].peer_count > largest) largest = cs->channels[i].peer_count;
    assert(largest >= 303);
    for (unsigned i = 1; i <= allocations; i++) {
        struct channel_cache* old = cs->channels; int count = cs->channel_count;
        cache_allocations = 0; fail_cache_allocation = i;
        assert(cs_refresh_channels(cs) < 0 && cs->channels == old && cs->channel_count == count);
        fail_cache_allocation = 0; assert(!cs_refresh_channels(cs));
    }
    cs->pending_inv_in_count = 4;
    uint8_t inviter[32] = {99}, group_key[32] = {73}, other_group[32] = {74};
    for (int i = 0; i < 4; i++) {
        memcpy(cs->pending_inv_in[i].group_ed, i % 2 ? group_key : other_group, 32);
        memcpy(cs->pending_inv_in[i].inviter, inviter, 32);
    }
    chat_sync_deny_invite(f.inst, other_group, inviter); assert(cs->pending_inv_in_count == 2);
    chat_sync_on_channel_deleted(f.inst, f.channel[1], group_key); assert(!cs->pending_inv_in_count);
    assert(!cs_refresh_channels(cs));
    deleting = &f; chat_event_set_handler(f.inst, delete_from_event);
    cs_on_peer_status_changed(cs, f.peers[1]->peer_ed25519_pubkey, 1);
    run_ms(&f, 100); assert(!deleting);
    chat_event_set_handler(f.inst, NULL);
    cleanup(&f); printf("PASS: 300 peers, %u cache allocation failures/retries, callback deletion and pending invite cleanup\n", allocations);
}

static unsigned carrier_sends, carrier_closes;
static int carrier_open(void* ctx, const uint8_t key[32], uint64_t* handle) {
    (void)ctx; (void)key; (void)handle; assert(!"unexpected carrier open"); return -1;
}
static int carrier_send(void* ctx, uint64_t handle, const uint8_t* data, size_t len) {
    assert(ctx && handle == 123 && len >= 18 && data[CHAT_SYNC_TYPE_OFF] == CS_MSG_JOIN_RELAY); carrier_sends++; return 0;
}
static void carrier_close(void* ctx, uint64_t handle, int success) {
    assert(ctx && handle == 123 && !success); carrier_closes++;
}
static int carrier_info_max(void* arg, uint64_t handle, const uint8_t* frame, size_t size) {
    struct cs_group_read_job* job = arg;
    uint8_t body[CHAT_JOIN_BODY_MAX]; size_t bytes;
    assert(handle == job->route.handle && size <= CHAT_JOIN_WIRE_MAX);
    assert(test_join_open(job->inst, frame, size, body, &bytes) == CS_MSG_JOIN_INFO_RESP);
    size_t pos = 64;
    assert(body[pos] == 1 && (size_t)body[pos + 1] * 256 + body[pos + 2] == GROUP_INFO_MAX);
    pos += 3 + GROUP_INFO_MAX + 32 + 64 + 32 + 32 + 64 + 8 + 64 + 8;
    assert(pos + 2 + MS_USERINFO_MAX + 32 + 64 == bytes && (size_t)body[pos] * 256 + body[pos + 1] == MS_USERINFO_MAX);
    return 0;
}

static void test_max_carrier_info(void) {
    struct fixture f; setup(&f);
    struct cs_group_read_job job = {.kind=CS_GROUP_INFO,.attempt=7,.peer=f.peers[0]->peer_node_id};
    strcpy(job.channel, f.channel[0]);
    job.inst = f.inst;
    job.route.peer = job.peer; job.route.handle = 123;
    job.path.attempt = 7; job.path.handle = job.route.handle;
    memcpy(job.path.inviter, f.inst->my_ed25519_pubkey, 32);
    memcpy(job.path.guest, f.peers[0]->peer_ed25519_pubkey, 32);
    memcpy(job.path.exit_x, f.inst->my_keys.public_key, 32);
    memcpy(job.route.public_key, f.peers[0]->crypto_ctx.peer_public_key, 32);
    memset(job.snapshot.description.json, 'x', GROUP_INFO_MAX); memset(job.snapshot.profile.json, 'y', MS_USERINFO_MAX);
    struct chat_join_carrier provider = {.ctx=&job,.open=carrier_open,.send=carrier_info_max,.close=carrier_close};
    assert(!chat_sync_set_join_carrier(f.inst, &provider));
    assert(!cs_group_read_send(f.inst->chat_sync, &job));
    assert(!chat_sync_set_join_carrier(f.inst, NULL)); cleanup(&f);
}
static void test_initial_time_wait(void) {
    struct fixture f; setup(&f);
    struct chat_sync* cs = f.inst->chat_sync;
    f.inst->ntp.startup_wait = 1;
    for (unsigned i = 0; i < 3; i++) etcp_fire_conn_status(f.peers[i], ETCP_CONN_STATUS_UP);
    cs_schedule_peer_sync(cs, f.peers[1], "during_time_wait");
    assert(cs->sync_pending && !cs->sync_timer);
    for (struct pending_sync* job = cs->sync_pending; job; job = job->next)
        assert(job->transport && node_conn_direct_get_conn(job->transport) == instance_find_conn_by_key(f.inst, node_conn_direct_get_key(job->transport)));
    assert(cs_join_reserve(cs, f.gid[0], f.peers[0]->crypto_ctx.peer_public_key, 0, 0) == 0);
    cs->join.key = 1; memcpy(cs->join.inviter_ed, f.peers[0]->peer_ed25519_pubkey, 32);
    assert(cs_join_open_transport(cs, f.peers[0]->peer_node_id, NULL) == 0 && !cs->info_req_timer);
    run_ms(&f, 4000); assert(!start_count && cs->join.phase == 1);
    for (struct pending_sync* job = cs->sync_pending; job; job = job->next)
        assert(job->transport && node_conn_direct_get_conn(job->transport) == instance_find_conn_by_key(f.inst, node_conn_direct_get_key(job->transport)));
    f.inst->ntp.startup_wait = 0; chat_sync_time_ready(f.inst);
    assert(cs->join.phase == 2 && cs->info_req_timer);
    run_ms(&f, 1100); assert(start_count == 4);
    assert(!cs->sync_pending);
    cs_join_finish(cs, CHAT_CONNECT_CANCELLED);
    struct chat_join_carrier provider = {.ctx = &f, .open = carrier_open, .send = carrier_send, .close = carrier_close};
    assert(chat_sync_set_join_carrier(f.inst, &provider) == 0);
    assert(cs_join_reserve(cs, f.gid[0], f.peers[0]->crypto_ctx.peer_public_key, 0, 0) == 0);
    cs->join.key = 1; memcpy(cs->join.inviter_ed, f.peers[0]->peer_ed25519_pubkey, 32);
    cs->join.phase = 1; cs->join.route.handle = 123;
    f.inst->ntp.startup_wait = 1;
    chat_sync_join_carrier_ready(f.inst, 123, cs->join.route.public_key);
    assert(cs->join.carrier_ready && cs->join.phase == 1 && !carrier_sends);
    f.inst->ntp.startup_wait = 0; chat_sync_time_ready(f.inst);
    assert(cs->join.phase == 2 && carrier_sends == 1);
    cs_join_finish(cs, CHAT_CONNECT_CANCELLED); assert(carrier_closes == 1);
    // Входящий ответ уже принят транспортом: сохраняем до открытия обмена и обрабатываем ровно один раз.
    assert(cs_join_reserve(cs, f.gid[0], f.peers[0]->crypto_ctx.peer_public_key, 0, 0) == 0);
    setup_wait_ready(&f, f.peers[0]);
    struct chat_join_route route = cs->join.route;
    memcpy(cs->join.inviter_ed, f.peers[0]->peer_ed25519_pubkey, 32);
    struct UTUN_INSTANCE inviter = {.my_keys = f.peer_keys[0]};
    memcpy(inviter.my_ed25519_pubkey, cs->join.inviter_ed, 32);
    struct chat_join_path path = cs_guest_path(cs, cs->join.attempt, cs->join.inviter_ed, route.public_key, 0);
    uint8_t data[512];
    const uint8_t* group_ed = topo_group_wire_id(topo_groups_find(f.inst->topo_groups, f.gid[0]));
    size_t size = test_join_make(&inviter, &path, CS_MSG_JOIN_READY, group_ed, 32, data, sizeof(data));
    assert(size);
    f.inst->ntp.startup_wait = 1; cs_receive(cs, &route, data, size);
    assert(cs->time_wait_count == 1 && cs->join.phase == 3);
    f.inst->ntp.startup_wait = 0; chat_sync_time_ready(f.inst);
    assert(!cs->time_wait_count && !cs->time_wait_frames && !cs->join.channel);
    // Закрытый carrier не должен ожить от отложенного кадра; остановка освобождает оставшиеся кадры.
    f.inst->ntp.startup_wait = 1; route.handle = 123;
    cs_receive(cs, &route, data, size); assert(cs->time_wait_count == 1);
    chat_sync_join_carrier_closed(f.inst, 123); assert(!cs->time_wait_count);
    route.handle = 0; cs_receive(cs, &route, data, size); assert(cs->time_wait_count == 1);
    route.handle = 123; cs_receive(cs, &route, data, size); assert(cs->time_wait_count == 2);
    assert(chat_sync_set_join_carrier(f.inst, NULL) == 0);
    assert(cs->time_wait_count == 1 && !cs->time_wait_frames->route.handle);
    cleanup(&f); puts("PASS: initial time wait retains sync/JOIN, drains incoming frames and cleans cancelled work");
}

static unsigned recovery_events;
static int recovery_result;
static void recovery_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* bytes, int size) {
    (void)inst;
    if (type != CHAT_EVT_CONNECT_RESULT) return;
    int32_t code;
    assert(size == CHAT_CONNECT_RESULT_SIZE);
    memcpy(&code, bytes + CHAT_CONNECT_RESULT_CODE_OFF, sizeof(code));
    recovery_events++; recovery_result = code;
}

static void recovery_flag(void* data, const struct db_async_result* result) {
    assert(!result->status);
    *(int*)data = 1;
}
static void recovery_wait(struct fixture* f, const int* flag) {
    uint64_t until = get_time_us() + 5000000;
    while ((flag ? !*flag : db_async_pending(f->inst->db_worker)) && get_time_us() < until) uasync_poll(f->ua, 1);
    assert(flag ? *flag : !db_async_pending(f->inst->db_worker));
}
static int recovery_nowait(sqlite3* db, void* data) { (void)data; return sqlite3_busy_timeout(db, 0); }

struct recovery_block { int ready, done; };
static void recovery_block_ready(void* data, const struct db_async_result* result) {
    recovery_flag(&((struct recovery_block*)data)->ready, result);
}
static void recovery_block_done(void* data, const struct db_async_result* result) {
    recovery_flag(&((struct recovery_block*)data)->done, result);
}
static struct db_async_tx* recovery_block_queue(struct fixture* f, struct recovery_block* block) {
    *block = (struct recovery_block){0};
    struct db_async_tx* tx = db_tx_begin(f->inst->db_worker, "hold INFO guard", DB_ASYNC_READ,
        recovery_block_ready, recovery_block_done, block);
    assert(tx); recovery_wait(f, &block->ready); return tx;
}

static int recovery_send_info(void* arg, uint64_t handle, const uint8_t* frame, size_t size) {
    struct chat_sync* cs = arg;
    assert(handle == 123);
    cs_receive(cs, &cs->join.route, frame, size);
    return 0;
}

static void recovery_receive_info(struct fixture* f, unsigned peer, uint64_t request) {
    struct ETCP_CONN* conn = f->peers[peer];
    struct chat_sync* cs = f->inst->chat_sync;
    assert(!cs_join_reserve(cs, f->gid[0], conn->crypto_ctx.peer_public_key, request, 0));
    assert(!node_conn_direct_hold(conn, &cs->join.handle));
    node_conn_direct_set_callback(cs->join.handle, cs_join_transport_event, cs);
    cs->join.phase = 2; cs->join.key = 1;
    memcpy(cs->join.inviter_ed, conn->peer_ed25519_pubkey, 32);
    memcpy(cs->join.inviter_pubkey, conn->crypto_ctx.peer_public_key, 32);
    struct chat_join_carrier carrier = {.ctx=cs,.send=recovery_send_info};
    struct UTUN_INSTANCE sender = {.my_keys=f->peer_keys[peer],.join_carrier=&carrier};
    memcpy(sender.my_ed25519_pubkey, cs->join.inviter_ed, 32);
    struct chat_sync sender_sync = {.inst=&sender};
    struct cs_group_read_job job = {.kind=CS_GROUP_INFO};
    job.identity.node = conn->peer_node_id;
    memcpy(job.identity.ed, sender.my_ed25519_pubkey, 32);
    memcpy(job.identity.x, sender.my_keys.public_key, 32);
    assert(!chat_sync_group_read(f->inst->topo_sqlite_db, &job.identity, f->channel[0], &job.snapshot));
    job.path = cs_guest_path(cs, cs->join.attempt, cs->join.inviter_ed, cs->join.route.public_key, 123);
    f->inst->ntp.startup_wait = 0;
    assert(!cs_group_read_send(&sender_sync, &job));
    f->inst->ntp.startup_wait = 1; /* Останавливаем фоновый pull на время проверки SQLite. */
}

struct recovery_restart {
    uint64_t self;
    struct member_sync_prepared* members;
    struct chat_sync_catalog* catalog;
    int done;
};
static int recovery_restart_prepare(sqlite3* db, void* data) {
    struct recovery_restart* restart = data;
    int rc = member_sync_prepare(db, restart->self, &restart->members);
    return rc ? rc : chat_sync_catalog_read(db, &restart->catalog);
}
static void recovery_restart_done(void* data, const struct db_async_result* result) {
    recovery_flag(&((struct recovery_restart*)data)->done, result);
}

static void test_recovery_write(void) {
    struct fixture f; setup(&f);
    char directory[1024], path[1100]; const char* tmp = getenv("TMPDIR");
    snprintf(directory, sizeof(directory), "%s/utun-join-recovery-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    assert(!test_mkdtemp(directory)); snprintf(path, sizeof(path), "%s/chats.db", directory);
    sqlite3* disk = NULL; assert(!sqlite3_open(path, &disk));
    sqlite3_backup* copy = sqlite3_backup_init(disk, "main", f.inst->topo_sqlite_db, "main");
    assert(copy && sqlite3_backup_step(copy, -1) == SQLITE_DONE && !sqlite3_backup_finish(copy));
    char sql[180]; snprintf(sql, sizeof(sql), "DELETE FROM peers_%s WHERE node_id=%lld", f.channel[0], (long long)f.inst->node_id);
    assert(!sqlite3_exec(disk, sql, NULL, NULL, NULL));
    group_policy_forget(f.inst, f.channel[0]);
    int ready = 0;
    f.inst->db_worker = db_async_create(f.ua, path, 2, recovery_flag, &ready); assert(f.inst->db_worker);
    recovery_wait(&f, &ready);
    db_async_submit(f.inst->db_worker, "disable busy wait", DB_ASYNC_DIRECT, recovery_nowait, &ready, recovery_flag);
    recovery_wait(&f, NULL);
    chat_event_set_handler(f.inst, recovery_event); recovery_events = 0;

    struct chat_sync* cs = f.inst->chat_sync;
    uint8_t first_key[32]; memcpy(first_key, f.peers[0]->crypto_ctx.peer_public_key, sizeof(first_key));
    struct recovery_block block;
    struct db_async_tx* tx = recovery_block_queue(&f, &block);
    recovery_receive_info(&f, 0, 101);
    assert(cs->join.phase == CS_JOIN_ACCEPT_WRITE && !cs->join_timer && db_async_pending(f.inst->db_worker) == 2);
    assert(!sqlite3_exec(disk, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    db_tx_commit(tx); recovery_wait(&f, NULL);
    assert(block.done && cs->join.accept_retry && !cs->join_timer && !cs->recovery && !recovery_events);
    assert(!sqlite3_exec(disk, "ROLLBACK", NULL, NULL, NULL));
    run_ms(&f, 110); recovery_wait(&f, NULL);
    assert(cs->join.phase == CS_JOIN_WAIT_READY && cs->join_timer && !cs->join.accept_retry && !recovery_events);
    assert(cs->recovery && !memcmp(cs->recovery->hints.pubkey, first_key, 32));
    etcp_connection_close(f.peers[0]); f.peers[0] = NULL;
    assert(!cs->join.handle && cs->join.phase == CS_JOIN_WAIT_MEMBERS && !recovery_events);

    chat_sync_destroy(f.inst);
    struct recovery_restart restart = {.self=f.inst->node_id};
    db_async_submit(f.inst->db_worker, "reload JOIN recovery", DB_ASYNC_WRITE, recovery_restart_prepare, &restart, recovery_restart_done);
    recovery_wait(&f, &restart.done);
    assert(restart.catalog->recovery && !restart.catalog->recovery->next);
    assert(!member_sync_activate(f.inst, restart.members) && !group_avatar_activate(f.inst) && !chat_sync_activate(f.inst, restart.catalog));
    member_sync_prepared_free(restart.members); chat_sync_catalog_free(restart.catalog);
    cs = f.inst->chat_sync;
    assert(cs->recovery && !memcmp(cs->recovery->hints.pubkey, first_key, 32));
    assert(!cs_recovery_finish(cs, f.channel[0], 0, CHAT_CONNECT_CANCELLED)); recovery_wait(&f, NULL);
    assert(!cs->recovery && recovery_events == 1 && recovery_result == CHAT_CONNECT_CANCELLED);
    recovery_events = 0;

    tx = recovery_block_queue(&f, &block); ready = 0;
    db_async_submit(f.inst->db_worker, "fill INFO queue", DB_ASYNC_DIRECT, recovery_nowait, &ready, recovery_flag);
    recovery_receive_info(&f, 1, 102);
    assert(cs->join.accept_retry && !cs->join_timer && !recovery_events);
    db_tx_commit(tx); recovery_wait(&f, NULL); assert(block.done && ready);
    run_ms(&f, 110); recovery_wait(&f, NULL);
    assert(cs->join.phase == CS_JOIN_WAIT_READY && cs->join_timer && cs->recovery && !recovery_events);
    cs_join_begin_members(cs, "ready", 1);
    assert(cs->join.phase == CS_JOIN_WAIT_MEMBERS);
    chat_sync_cancel_join(f.inst, 102); recovery_wait(&f, NULL);
    assert(!cs->join.attempt && !cs->recovery && recovery_events == 1 && recovery_result == CHAT_CONNECT_CANCELLED);
    recovery_events = 0;

    assert(!sqlite3_exec(disk, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    recovery_receive_info(&f, 1, 103); recovery_wait(&f, NULL);
    assert(cs->join.accept_retry && !recovery_events);
    assert(!sqlite3_exec(disk, "ROLLBACK", NULL, NULL, NULL));
    chat_sync_cancel_join(f.inst, 103);
    assert(!cs->join.accept_retry);
    recovery_wait(&f, NULL); run_ms(&f, 110);
    assert(!cs->join.attempt && !cs->recovery && recovery_events == 1 && recovery_result == CHAT_CONNECT_CANCELLED);
    recovery_events = 0;

    assert(!sqlite3_exec(disk, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    recovery_receive_info(&f, 1, 104); recovery_wait(&f, NULL);
    assert(cs->join.accept_retry);
    cs->join.accept_retry->expires = get_time_tb();
    assert(!sqlite3_exec(disk, "ROLLBACK", NULL, NULL, NULL));
    run_ms(&f, 110);
    assert(!cs->join.attempt && !cs->recovery && recovery_events == 1 && recovery_result == CHAT_CONNECT_TIMEOUT);
    recovery_events = 0;

    fail_join_timer = "join_members";
    recovery_receive_info(&f, 1, 105); recovery_wait(&f, NULL);
    cs_join_begin_members(cs, "ready", 1);
    assert(!fail_join_timer && !cs->join.attempt && cs->recovery && recovery_events == 1 && recovery_result == CHAT_CONNECT_ERROR);
    assert(!cs_recovery_finish(cs, f.channel[0], 0, CHAT_CONNECT_CANCELLED)); recovery_wait(&f, NULL);
    recovery_events = 0;

    recovery_receive_info(&f, 1, 106);
    assert(cs->join.phase == CS_JOIN_ACCEPT_WRITE && db_async_pending(f.inst->db_worker) == 1);
    chat_sync_cancel_join(f.inst, 106);
    assert(cs->join.phase == CS_JOIN_CANCEL_PENDING);
    recovery_wait(&f, NULL);
    assert(!cs->join.attempt && !cs->recovery && recovery_events == 1 && recovery_result == CHAT_CONNECT_CANCELLED);
    recovery_events = 0;

    assert(!sqlite3_exec(disk, "CREATE TRIGGER reject_recovery BEFORE INSERT ON chat_join_recovery "
        "BEGIN SELECT RAISE(ABORT,'JOIN recovery save fault'); END", NULL, NULL, NULL));
    recovery_receive_info(&f, 1, 107); recovery_wait(&f, NULL);
    assert(!cs->join.attempt && !cs->join.accept_retry && !cs->join_timer && !cs->recovery);
    assert(recovery_events == 1 && recovery_result == CHAT_CONNECT_ERROR);
    struct chat_sync_catalog* catalog = NULL;
    assert(!chat_sync_catalog_read(disk, &catalog) && !catalog->recovery); chat_sync_catalog_free(catalog);
    assert(!sqlite3_close(disk) && !db_async_destroy(f.inst->db_worker)); f.inst->db_worker = NULL;
    assert(!test_unlink(path) && !test_rmdir(directory));
    cleanup(&f); puts("PASS: JOIN recovery commits before request; BUSY, FULL, transport close, reload, cancel and rollback");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_DEBUG);
    test_clock_init(1000);
    size_t baseline = u_get_allocated_count();
    test_reinit_isolation(); test_coalesce_and_deadline(); test_sleep_and_cancel();
    test_join_existing_transport(); test_reentrant_cancel(); test_lifecycle_and_timer_failure();
    test_cache_snapshot(); test_max_carrier_info(); test_initial_time_wait(); test_recovery_write();
    test_clock_finish();
    assert(u_get_allocated_count() == baseline);
    puts("ALL PASS: targeted chat synchronization scheduling"); return 0;
}
