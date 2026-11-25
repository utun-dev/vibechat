#define TEST_CHANNEL "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"

#include <assert.h>
#include <string.h>
#include "utun_instance.h"
#include "topo_group.h"
#include "topo_recovery.h"
#include "topo_group_connect.h"
#include "topo_node_sqlite.h"
#include "etcp.h"
#include "etcp_api.h"
#include "node_conn_direct.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#include "test_clock.h"

struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst;
    struct TOPO_GROUP* group;
    uint64_t ids[3];
    struct ETCP_CONN* conns[3];
};

static struct TOPO_GROUP_CONN_ITEM* peer(struct fixture* f, int index) {
    for (struct ll_entry* e = f->group->senders_list->head; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* p = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        if (p->node_id == f->ids[index]) return p;
    }
    return NULL;
}

static void poll_events(struct fixture* f) { for (int i = 0; i < 4; i++) uasync_poll(f->ua, 0); }

static void create(struct fixture* f) {
    memset(f, 0, sizeof(*f));
    f->ua = uasync_create(); assert(f->ua);
    f->inst = utun_instance_create_from_str(f->ua,
        "[global]\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n");
    assert(f->inst && utun_instance_init(f->inst) == 0);
    f->group = topo_groups_create_group(f->inst->topo_groups, 42, TOPO_GROUP_TYPE_UTUN, NULL); assert(f->group);
    for (int i = 0; i < 3; i++) {
        uint8_t secret[32], seed[32];
        struct TOPO_NODE* node = u_calloc(1, sizeof(*node)); assert(node);
        assert(!sc_identity_generate(node->public_key, secret, node->ed25519_public_key, seed));
        f->ids[i] = node->node_id = sc_derive_node_id_from_pubkey(node->public_key);
        assert(topo_node_registry_store(f->inst->topo_groups, node));
        struct ETCP_CONN* conn = f->conns[i] = etcp_connection_create(f->inst, "recovery_fixture"); assert(conn);
        conn->peer_node_id = node->node_id; memcpy(conn->peer_ed25519_pubkey, node->ed25519_public_key, 32);
        assert(sc_init_ctx(&conn->crypto_ctx, &f->inst->my_keys) == SC_OK);
        assert(sc_set_peer_public_key(&conn->crypto_ctx, node->public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
        queue_set_callback(conn->send_input_q, NULL, NULL);
        etcp_conn_ready(conn);
    }
}

static void destroy(struct fixture* f) {
    for (int i = 0; i < 3; i++) topo_node_registry_unref(f->inst->topo_groups, f->ids[i]);
    f->inst->running = 0; utun_instance_destroy(f->inst);
    uasync_poll(f->ua, 0); uasync_destroy(f->ua, 0);
}

static void up(struct fixture* f, int i) {
    f->conns[i]->links_up = 1;
    etcp_cbk_fire(f->conns[i], ETCP_CBK_EVENT_UP);
    assert(peer(f, i) && peer(f, i)->conn == f->conns[i]);
}

static void route(struct fixture* f, int target, int via) {
    struct ll_entry* entry = queue_entry_new(sizeof(struct TOPO_GROUP_NODE) - sizeof(struct ll_entry)); assert(entry);
    struct TOPO_GROUP_NODE* node = (struct TOPO_GROUP_NODE*)entry;
    node->node_id = f->ids[target];
    node->identity = topo_node_registry_find(f->inst->topo_groups, node->node_id); assert(node->identity); topo_node_ref(node->identity);
    uint32_t short31; assert(!topo_address_short(node->identity->ed25519_public_key, &short31));
    topo_address_attach(f->group, node, node->identity->ed25519_public_key, short31);
    struct topo_hop hops[] = {{.id=node->node_id}, {.id=f->ids[via]}};
    memcpy(hops[0].ed, node->ed25519, 32); memcpy(hops[1].ed, f->conns[via]->peer_ed25519_pubkey, 32);
    assert(topo_group_add_path(node, f->conns[via], hops, target == via ? 1 : 2, 10) == 0);
    /* This fixture injects the route that a NODEINFO of the current exchange would confirm. */
    if (peer(f, via)) ((struct TOPO_NODEPATH*)node->paths->tail)->exchange = peer(f, via)->local_epoch;
    assert(queue_data_put_with_index(f->group->nodes, entry) == 0);
    topo_recovery_changed(f->group);
}

static void table(struct fixture* f, int i, uint8_t subcmd, uint64_t id) {
    struct TOPOMSG_HEADER msg = { .cmd = ETCP_ID_TOPO_ENTRY, .subcmd = subcmd,
                                 .src_epoch = 7, .dst_epoch = id };
    memcpy(msg.group_key, topo_group_wire_id(f->group), 32);
    struct ll_entry* entry = ll_alloc_lldgram(sizeof(msg)); assert(entry);
    memcpy(entry->dgram, &msg, sizeof(msg)); entry->len = sizeof(msg);
    f->inst->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY](f->conns[i], entry);
}

static void accept_join(struct fixture* f, int i) {
    struct TOPOMSG_JOIN_GROUP msg = { .protocol_version = htons(TOPO_GROUP_PROTOCOL_VERSION), .h = { .cmd = ETCP_ID_TOPO_ENTRY, .subcmd = TOPO_SUBCMD_JOIN_ACCEPT,
         .src_epoch = 7, .dst_epoch = peer(f, i)->local_epoch }, .group_type = TOPO_GROUP_TYPE_UTUN };
    memcpy(msg.h.group_key, topo_group_wire_id(f->group), 32);
    struct ll_entry* e = ll_alloc_lldgram(sizeof(msg)); assert(e);
    memcpy(e->dgram, &msg, sizeof(msg)); e->len = sizeof(msg);
    f->inst->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY](f->conns[i], e);
    assert(peer(f, i)->accepted);
}

static void drain(struct fixture* f, int index) {
    for (int i = 0; i < 128; i++) {
        uasync_poll(f->ua, 0);
        struct TOPO_GROUP_CONN_ITEM* p = peer(f, index);
        if (!p || !p->conn) return;
        assert(queue_entry_count(p->conn->send_input_q) <= 1);
        struct ll_entry* e = queue_data_get(p->conn->send_input_q);
        if (e) { queue_dgram_free(e); queue_entry_free(e); }
        queue_resume_callback(p->conn->send_input_q);
    }
}

static void ready(struct fixture* f, int i) {
    assert(peer(f, i) && peer(f, i)->local_epoch);
    accept_join(f, i);
    table(f, i, TOPO_SUBCMD_REQUEST_TABLE, peer(f, i)->local_epoch);
    table(f, i, TOPO_SUBCMD_TABLE_COMPLETE, peer(f, i)->local_epoch);
    drain(f, i);
    assert(topo_group_peer_ready(f->group, f->ids[i]));
}

static void partial_and_external_routes(void) {
    struct fixture f; create(&f);
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[0]), 1, 100);
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[1]), 0, 1);
    struct TOPO_RECOVERY_CTX* context = f.group->recovery;
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[1]), 0, 1);
    assert(context == f.group->recovery);
    assert(topo_node_registry_find(f.inst->topo_groups, f.ids[1])->group_ref_count == 2);
    topo_recovery_start(f.group); poll_events(&f);
    assert(peer(&f, 0) && !peer(&f, 1) && !peer(&f, 2)); /* next-hop beats smaller RTT */
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[2]), 1, 200);
    topo_recovery_start(f.group); poll_events(&f);
    assert(f.group->recovery == context && peer(&f, 0) && !peer(&f, 2)); /* merge a new loss into the active round */
    struct NODE_CONN_DIRECT* ownership = peer(&f, 0)->handle;
    up(&f, 0); route(&f, 0, 0); poll_events(&f);
    assert(f.group->recovery == context && !peer(&f, 2)); /* UP and a route are insufficient */
    assert(peer(&f, 0)->handle == ownership); /* no handoff or second NCD handle on UP */
    ready(&f, 0); poll_events(&f);
    assert(f.group->recovery == context && peer(&f, 2) && !peer(&f, 1));
    assert(peer(&f, 0)->handle == ownership && topo_group_peer_ready(f.group, f.ids[0]));
    route(&f, 1, 0); poll_events(&f); /* another READY peer restores the remaining subtree */
    up(&f, 2); route(&f, 2, 2);
    table(&f, 2, TOPO_SUBCMD_TABLE_COMPLETE, peer(&f, 0)->local_epoch); poll_events(&f);
    assert(f.group->recovery && !topo_group_peer_ready(f.group, f.ids[2]));
    ready(&f, 2); poll_events(&f);
    assert(!f.group->recovery && !peer(&f, 1));
    assert(topo_group_peer_ready(f.group, f.ids[0]) && topo_group_peer_ready(f.group, f.ids[2]));
    destroy(&f);
}

static void stalled_and_exhausted(void) {
    struct fixture f; create(&f);
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[0]), 1, 10);
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[1]), 0, 20);
    topo_recovery_start(f.group); poll_events(&f);
    up(&f, 0);
    accept_join(&f, 0);
    drain(&f, 0); /* Complete the preceding UP/ACCEPT callbacks before simulating a stalled exchange. */
    uint64_t old_progress = get_time_tb() - TOPO_RECOVERY_SYNC_TIMEOUT_MS * 10ULL - 1;
    peer(&f, 0)->progress = old_progress;
    /* Реальный шаг обмена обновляет срок, пустая повторная проверка — нет. */
    table(&f, 0, TOPO_SUBCMD_REQUEST_TABLE, peer(&f, 0)->local_epoch); drain(&f, 0); poll_events(&f);
    assert(peer(&f, 0) && !peer(&f, 1) && peer(&f, 0)->progress > old_progress);
    peer(&f, 0)->progress = old_progress;
    topo_recovery_changed(f.group); poll_events(&f);
    assert(!peer(&f, 0) && peer(&f, 1));
    up(&f, 1); ready(&f, 1); poll_events(&f); /* READY without routes cannot claim success */
    assert(!f.group->recovery && !topo_node_find_by_id(f.group, f.ids[0]) && !topo_node_find_by_id(f.group, f.ids[1]));
    topo_recovery_changed(f.group); topo_recovery_start(f.group); poll_events(&f);
    assert(!f.group->recovery && !peer(&f, 0)); /* exhausted round does not restart itself */
    destroy(&f);
}

static void cancel_and_group_stop(void) {
    struct fixture f; create(&f);
    struct TOPO_PEER_REQUEST* other = NULL;
    assert(topo_group_peer_open(f.group, f.ids[0], &other) == 0);
    struct NODE_CONN_DIRECT* ownership = peer(&f, 0)->handle;
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[0]), 1, 10);
    topo_recovery_start(f.group); poll_events(&f);
    assert(peer(&f, 0)->handle == ownership);
    topo_recovery_cancel_all(f.group); poll_events(&f);
    assert(!f.group->recovery && peer(&f, 0)->handle == ownership);
    assert(topo_group_peer_phase(other) == TOPO_PEER_CONNECTING);
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[1]), 1, 10);
    topo_recovery_start(f.group); poll_events(&f);
    topo_groups_remove_group(f.inst->topo_groups, f.group->group_id); f.group = NULL;
    assert(topo_group_peer_phase(other) == TOPO_PEER_FAILED);
    topo_group_peer_close(other); poll_events(&f);
    destroy(&f);
}

static void restored_during_connect(void) {
    struct fixture f; create(&f);
    f.conns[1]->links_up = 1;
    assert(topo_group_new_conn(f.group, f.conns[1]) == 0);
    route(&f, 1, 1); ready(&f, 1);
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[0]), 1, 10);
    topo_recovery_start(f.group); poll_events(&f);
    assert(peer(&f, 0) && !peer(&f, 0)->conn);
    route(&f, 0, 1); poll_events(&f);
    assert(!f.group->recovery && !peer(&f, 0) && topo_group_peer_ready(f.group, f.ids[1]));
    destroy(&f);
}

static void connect_deadline(void) {
    struct fixture f; create(&f);
    f.inst->etcp_connect_timeout_tb = 100000; /* recovery must time out before NCD's own 10-second timer */
    topo_recovery_add_node(f.group, topo_node_registry_find(f.inst->topo_groups, f.ids[0]), 1, 10);
    topo_recovery_start(f.group); poll_events(&f);
    assert(peer(&f, 0));
    uint64_t deadline = get_time_tb() + (TOPO_RECOVERY_CONNECT_TIMEOUT_MS + 1000) * 10ULL;
    while (f.group->recovery && get_time_tb() < deadline) test_clock_poll(f.ua, 100);
    assert(!f.group->recovery && !peer(&f, 0));
    destroy(&f);
}

static void sender_backpressure(void) {
    struct fixture f; create(&f);
    f.conns[0]->links_up = 1; f.conns[2]->links_up = 1;
    assert(topo_group_new_conn(f.group, f.conns[0]) == 0); accept_join(&f, 0);
    route(&f, 1, 2); route(&f, 2, 2);
    table(&f, 0, TOPO_SUBCMD_REQUEST_TABLE, peer(&f, 0)->local_epoch);
    table(&f, 0, TOPO_SUBCMD_REQUEST_TABLE, peer(&f, 0)->local_epoch); /* one snapshot */
    for (int i = 0; i < 50; i++) {
        assert(topo_group_send_nodeinfo(f.group, f.group->local_node, f.conns[0]) == 0);
        uasync_poll(f.ua, 0);
    }
    assert(!peer(&f, 0)->table_sent && queue_entry_count(f.conns[0]->send_input_q) == 1);
    struct TOPO_GROUP_NODE* removed = topo_node_find_by_id(f.group, f.ids[1]);
    topo_group_remove_path(removed, f.conns[2]); topo_nodeq_remove_node(f.group, removed);
    int local = 0, remote = 0, complete = 0;
    for (int i = 0; i < 128; i++) {
        uasync_poll(f.ua, 0);
        assert(queue_entry_count(f.conns[0]->send_input_q) <= 1);
        struct ll_entry* e = queue_data_get(f.conns[0]->send_input_q);
        if (e && e->dgram[0] == ETCP_ID_TOPO_ENTRY) {
            struct TOPOMSG_HEADER* h = (struct TOPOMSG_HEADER*)e->dgram;
            if (h->subcmd == TOPO_SUBCMD_NODEINFO) {
                const struct TOPOMSG_NODE* node = &((const struct TOPOMSG_NODEINFO_PKT*)e->dgram)->node;
                uint64_t id = sc_derive_node_id_from_pubkey(node->public_key);
                assert(id != f.ids[1]); /* removed during backpressure: never serialize a stale pointer */
                if (id == f.inst->node_id) local++; else { assert(id == f.ids[2]); remote++; }
            } else if (h->subcmd == TOPO_SUBCMD_TABLE_COMPLETE) {
                assert(local == 1 && remote == 1); complete++;
            }
        }
        if (e) { queue_dgram_free(e); queue_entry_free(e); }
        queue_resume_callback(f.conns[0]->send_input_q);
    }
    /* 50 одинаковых обновлений после снимка не повторяют уже переданный анонс. */
    assert(complete == 1 && local == 1 && remote == 1 && peer(&f, 0)->table_sent);
    /* Cancel a blocked old generation; only the new JOIN may be sent afterwards. */
    etcp_fire_conn_status(f.conns[0], ETCP_CONN_STATUS_REINIT); accept_join(&f, 0);
    table(&f, 0, TOPO_SUBCMD_REQUEST_TABLE, peer(&f, 0)->local_epoch);
    uint64_t old = peer(&f, 0)->local_epoch;
    etcp_fire_conn_status(f.conns[0], ETCP_CONN_STATUS_REINIT);
    for (int i = 0; i < 32; i++) {
        uasync_poll(f.ua, 0);
        struct ll_entry* e = queue_data_get(f.conns[0]->send_input_q);
        if (e && e->dgram[0] == ETCP_ID_TOPO_ENTRY) {
            struct TOPOMSG_HEADER* h = (struct TOPOMSG_HEADER*)e->dgram;
            assert(h->src_epoch != old && h->subcmd == TOPO_SUBCMD_JOIN_GROUP);
        }
        if (e) { queue_dgram_free(e); queue_entry_free(e); }
        queue_resume_callback(f.conns[0]->send_input_q);
    }
    accept_join(&f, 0); table(&f, 0, TOPO_SUBCMD_REQUEST_TABLE, peer(&f, 0)->local_epoch);
    poll_events(&f);
    topo_groups_remove_group(f.inst->topo_groups, f.group->group_id); f.group = NULL;
    poll_events(&f); destroy(&f);
}

static void persistent_request(void) {
    struct fixture f; create(&f);
    struct TOPO_PEER_REQUEST* request = NULL;
    assert(topo_group_peer_open(f.group, f.ids[0], &request) == 0);
    struct NODE_CONN_DIRECT* ownership = peer(&f, 0)->handle;
    up(&f, 0); ready(&f, 0);
    f.conns[0]->links_up = 0;
    etcp_cbk_fire(f.conns[0], ETCP_CBK_EVENT_DOWN); poll_events(&f);
    assert(topo_group_peer_phase(request) == TOPO_PEER_FAILED);
    assert(peer(&f, 0)->handle == ownership && topo_group_peer_conn(request) == f.conns[0]);
    up(&f, 0); ready(&f, 0);
    assert(topo_group_peer_phase(request) == TOPO_PEER_READY && peer(&f, 0)->handle == ownership);
    topo_group_peer_close(request); destroy(&f);
}

static void manual_planner_cancel(void) {
    struct fixture f; create(&f);
    assert(f.inst->topo_sqlite_db);
    assert(topo_node_sqlite_init(f.inst->topo_sqlite_db) == 0);
    f.group->group_type = TOPO_GROUP_TYPE_CHAT; strcpy(f.group->channel_id, TEST_CHANNEL);
    /* The membership table is the only authorization source. */
    uint8_t key[32], signature[64] = {1};
    assert(!sc_hex_to_binary(TEST_CHANNEL, key, 32));
    memcpy(f.group->wire_id, key, 32);
    assert(topo_node_sqlite_channel_put(f.inst->topo_sqlite_db, TEST_CHANNEL, "test", (const uint8_t[32]){1}, key, NULL, key, NULL, signature) == 0);
    assert(!topo_node_sqlite_member_placeholder_put(f.inst->topo_sqlite_db, TEST_CHANNEL, f.ids[0], f.conns[0]->crypto_ctx.peer_public_key, f.conns[0]->peer_ed25519_pubkey));
    assert(topo_node_sqlite_member_in_channel(f.inst->topo_sqlite_db, TEST_CHANNEL, f.ids[0]));
    assert(!topo_node_sqlite_member_allowed(f.inst->topo_sqlite_db, TEST_CHANNEL, f.ids[0]));
    struct topo_connect_peer* candidates = NULL; size_t count = 0;
    assert(!topo_node_sqlite_get_connect_peers(f.inst->topo_sqlite_db, TEST_CHANNEL, &candidates, &count) && !count && !candidates);
    struct TOPO_PEER_REQUEST* denied = NULL;
    assert(topo_group_peer_open(f.group, f.ids[0], &denied) == -1 && !denied && !peer(&f, 0));
    assert(topo_node_sqlite_member_block_put(f.inst->topo_sqlite_db, TEST_CHANNEL, f.ids[0], signature, 1, signature, 1,
        f.conns[0]->crypto_ctx.peer_public_key, f.conns[0]->peer_ed25519_pubkey, "{}", 0, signature) == 0);
    assert(sqlite3_exec(f.inst->topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL " SET admitted=1,identity_verified=1", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_member_allowed(f.inst->topo_sqlite_db, TEST_CHANNEL, f.ids[0]));
    assert(!topo_node_sqlite_get_connect_peers(f.inst->topo_sqlite_db, TEST_CHANNEL, &candidates, &count) && count == 1);
    assert(candidates[0].node_id == f.ids[0]); u_free(candidates); candidates = NULL;
    assert(!topo_node_sqlite_member_owner_put(f.inst->topo_sqlite_db, TEST_CHANNEL, f.conns[0]->peer_ed25519_pubkey, "{\"deleted\":\"yes\"}", signature, 0));
    assert(!topo_node_sqlite_member_allowed(f.inst->topo_sqlite_db, TEST_CHANNEL, f.ids[0]));
    assert(!topo_node_sqlite_get_connect_peers(f.inst->topo_sqlite_db, TEST_CHANNEL, &candidates, &count) && !count && !candidates);
    assert(topo_group_peer_open(f.group, f.ids[0], &denied) == -1 && !denied && !peer(&f, 0));
    assert(!topo_node_sqlite_member_owner_put(f.inst->topo_sqlite_db, TEST_CHANNEL, f.conns[0]->peer_ed25519_pubkey, "{}", signature, 0));
    assert(!topo_node_sqlite_member_block_put(f.inst->topo_sqlite_db, TEST_CHANNEL, f.inst->node_id, signature, 1, signature, 1,
        f.inst->my_keys.public_key, f.inst->my_ed25519_pubkey, "{}", 0, signature));
    assert(sqlite3_exec(f.inst->topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL " SET admitted=1,identity_verified=1", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_group_connect_node_once(f.group, f.conns[0]->peer_ed25519_pubkey) == 0);
    struct NODE_CONN_DIRECT* ownership = peer(&f, 0)->handle;
    assert(topo_group_connect_node_once(f.group, f.conns[0]->peer_ed25519_pubkey) == 0 && peer(&f, 0)->handle == ownership);
    up(&f, 0); poll_events(&f);
    assert(topo_group_connect_active_count(f.group) == 1); /* Планировщику достаточно UP; recovery по-прежнему ждёт READY. */
    topo_groups_remove_group(f.inst->topo_groups, f.group->group_id); f.group = NULL;
    poll_events(&f); destroy(&f);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    test_clock_init(1000);
    utun_instance_set_tun_init_enabled(0);
    persistent_request();
    manual_planner_cancel();
    sender_backpressure();
    partial_and_external_routes();
    stalled_and_exhausted();
    cancel_and_group_stop();
    restored_during_connect();
    connect_deadline();
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "recovery tests: partial routes, readiness, progress, exhaustion and shared cancellation passed");
    test_clock_finish();
    return 0;
}
