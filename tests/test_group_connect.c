#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include "utun_instance.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_group_connect.h"
#include "routing_layer/topo_node_sqlite.h"
#include "transport_layer/etcp.h"
#include "transport_layer/etcp_api.h"
#include "transport_layer/node_conn_direct.h"
#include "../lib/mem.h"
#include "../lib/memory_pool.h"
#include "../lib/debug_config.h"
#include "test_clock.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif
#include "test_membership.h"
#define CHANNEL "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12"

#define PEERS 8

struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst;
    struct TOPO_GROUP* group;
    uint64_t ids[PEERS];
    struct ETCP_CONN* conns[PEERS];
    struct NODE_CONN_DIRECT* owners[PEERS];
};

static struct TOPO_GROUP_CONN_ITEM* peer(struct fixture* f, int index) {
    for (struct ll_entry* e = f->group->senders_list->head; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* p = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        if (p->node_id == f->ids[index]) return p;
    }
    return NULL;
}

static void poll_events(struct fixture* f) { for (int i = 0; i < 8; i++) uasync_poll(f->ua, 0); }

/* Типы/роли реальные, транспортные события задаём явно без удалённых процессов. */
static void create(struct fixture* f, int device, int supernode) {
    memset(f, 0, sizeof(*f));
    f->ua = uasync_create(); assert(f->ua);
#ifdef UTUN_HAVE_STANDBY
    standby_init(f->ua);
#endif
    char config[512];
    snprintf(config, sizeof(config), "[global]\nauto_sockets=no\nclient_type=%d\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[chatserver]\ngroup_autoconnect=0\n", device);
    f->inst = utun_instance_create_from_str(f->ua, config);
    assert(f->inst && utun_instance_init(f->inst) == 0);
    sqlite3* db = f->inst->topo_sqlite_db;
    uint8_t x[32], x_private[32], ed[32], seed[32], sig[64] = {1};
    memset(seed, 66, sizeof(seed)); assert(sc_identity_from_seed(seed, x, x_private, ed) == SC_OK);
    uint8_t descriptor_sig[64];
    assert(!test_group_signature(CHANNEL, "connect", f->inst->my_ed25519_pubkey, x, ed, seed, descriptor_sig));
    assert(topo_node_sqlite_channel_put(db, CHANNEL, "connect", f->inst->my_ed25519_pubkey, x, NULL, ed, NULL, descriptor_sig) == 0);
    assert(topo_node_sqlite_member_block_put(db, CHANNEL, f->inst->node_id, sig, 1, sig, 1,
        f->inst->my_keys.public_key, f->inst->my_ed25519_pubkey, "{}", 0, sig) == 0);
    if (supernode) assert(topo_node_sqlite_member_owner_put(db, CHANNEL, f->inst->my_ed25519_pubkey, "{\"supernode\":\"yes\"}", sig, 0) == 0);
    f->group = topo_groups_create_group(f->inst->topo_groups, 42, TOPO_GROUP_TYPE_CHAT, CHANNEL); assert(f->group);
    for (int i = 0; i < PEERS; i++) {
        uint8_t x_private[32], ed_private[32];
        struct TOPO_NODE* node = u_calloc(1, sizeof(*node)); assert(node);
        assert(sc_identity_generate(node->public_key, x_private, node->ed25519_public_key, ed_private) == SC_OK);
        f->ids[i] = node->node_id = sc_derive_node_id_from_pubkey(node->public_key);
        node->timestamp = 1;
        node->client_type = i < 5 ? CLIENT_TYPE_DESKTOP : i < 7 ? CLIENT_TYPE_MOBILE : CLIENT_TYPE_SERVER;
        node->v4_addrs = memory_pool_alloc(f->inst->topo_groups->v4_addr_pool); assert(node->v4_addrs);
        *node->v4_addrs = (struct TOPO_ADDR4){ .addr = {127,0,0,1}, .port = (uint16_t)(21000 + i),
                                            .protocol = TOPO_PROTO_UDP, .type = TOPO_ADDR_INTERFACE };
        uint8_t message[TOPO_SIG_MSG_MAX_SIZE];
        int size = topo_node_build_sig_msg(node, message, sizeof(message)); assert(size > 0);
        assert(sc_ed25519_sign(ed_private, message, (size_t)size, node->x25519_self_sig) == SC_OK);
        assert(topo_node_verify(node) == 0);
        assert(topo_node_registry_store(f->inst->topo_groups, node));
        assert(topo_node_sqlite_node_update_verified(db, node->node_id, "peer", node->public_key,
                                                   node->ed25519_public_key, 1, 1) == 0);
        assert(topo_node_sqlite_snapshot_put(db, node) == 0);
        assert(topo_node_sqlite_addrs_put(db, node->node_id, node) == 0);
        assert(topo_node_sqlite_member_block_put(db, CHANNEL, node->node_id, sig, 1, sig, 1,
                                               node->public_key, node->ed25519_public_key, "{}", 0, sig) == 0);
        assert(sqlite3_exec(db, "UPDATE peers_" CHANNEL " SET admitted=1,identity_verified=1", NULL, NULL, NULL) == SQLITE_OK);
        if (i < 3) assert(topo_node_sqlite_member_owner_put(db, CHANNEL, node->ed25519_public_key, "{\"supernode\":\"yes\"}", sig, 0) == 0);
        assert(topo_node_sqlite_nodeinfo_updated(db, node->public_key) == 0);
        assert(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
        assert(topo_node_sqlite_update_rtt(db, node->public_key, (uint16_t)(i + 1)) == SQLITE_OK);
        assert(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
        /* История нижнего класса не должна обходить суперузлы. */
        if (i >= 3) assert(topo_node_sqlite_set_connected(db, CHANNEL, node->ed25519_public_key, 1) == 0);
        struct ETCP_CONN* conn = f->conns[i] = etcp_connection_create(f->inst, "connect_fixture"); assert(conn);
        conn->peer_node_id = node->node_id; memcpy(conn->peer_ed25519_pubkey, node->ed25519_public_key, 32);
        assert(sc_init_ctx(&conn->crypto_ctx, &f->inst->my_keys) == SC_OK);
        assert(sc_set_peer_public_key(&conn->crypto_ctx, node->public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
        queue_set_callback(conn->send_input_q, NULL, NULL);
        etcp_conn_ready(conn);
        /* Другой владелец позволяет проверить отмену без закрытия общего транспорта. */
        assert(node_conn_direct_open(f->inst, node->node_id, NULL, NULL, &f->owners[i], NULL) != NCD_ERR);
    }
}

static void destroy(struct fixture* f) {
#ifdef UTUN_HAVE_STANDBY
    standby_deinit();
#endif
    topo_groups_remove_group(f->inst->topo_groups, 42);
    for (int i = 0; i < PEERS; i++) {
        node_conn_direct_close(f->owners[i]);
        topo_node_registry_unref(f->inst->topo_groups, f->ids[i]);
    }
    f->inst->running = 0; utun_instance_destroy(f->inst);
    uasync_poll(f->ua, 0); uasync_destroy(f->ua, 0);
}

/* UP без JOIN_ACCEPT/TABLE_COMPLETE обязан завершить автоматическую попытку. */
static void up(struct fixture* f, int index) {
    f->conns[index]->links_up = 1;
    etcp_cbk_fire(f->conns[index], ETCP_CBK_EVENT_UP); poll_events(f);
    assert(peer(f, index) && peer(f, index)->retained && !peer(f, index)->requests);
    assert(!topo_group_peer_ready(f->group, f->ids[index]));
}

static void down(struct fixture* f, int index) {
    f->conns[index]->links_up = 0;
    etcp_cbk_fire(f->conns[index], ETCP_CBK_EVENT_DOWN); poll_events(f);
}

/* Потеря транспорта до первого UP должна разрешить следующий класс. */
static void fail(struct fixture* f, int index) {
    assert(peer(f, index) && peer(f, index)->requests);
    topo_group_remove_conn(f->group, f->conns[index], TOPO_REMOVE_TRANSPORT_DOWN); poll_events(f);
    assert(!peer(f, index));
    assert(!f->conns[index]->close_requested);
}

static void start(struct fixture* f) {
    assert(topo_group_connect_init(f->group) == 0); poll_events(f);
    assert(peer(f, 0) && peer(f, 1) && !peer(f, 2));
    for (int i = 3; i < PEERS; i++) assert(!peer(f, i));
}

static void two_supernodes(void) {
    struct fixture f; create(&f, CLIENT_TYPE_DESKTOP, 0); start(&f);
    up(&f, 0);
    assert(topo_group_connect_active_count(f.group) == 1 && peer(&f, 1));
    for (int i = 3; i < PEERS; i++) assert(!peer(&f, i));
    up(&f, 1);
    assert(topo_group_connect_active_count(f.group) == 2 && !peer(&f, 2));
    topo_group_connect_init(f.group); poll_events(&f);
    assert(peer(&f, 0)->retained && peer(&f, 1)->retained);
    down(&f, 0);
    assert(peer(&f, 2) && !peer(&f, 0)); /* сначала ещё не проверенный суперузел */
    for (int i = 3; i < PEERS; i++) assert(!peer(&f, i));
    destroy(&f);
}

static void retry_done(void* arg) { *(int*)arg = 1; }

/* Повтор должен сработать от таймера даже при достигнутой резервной цели. */
static void wait_retry(struct fixture* f) {
    int done = 0;
    assert(uasync_set_timeout(f->ua, 52000, &done, retry_done, "connect_test_retry"));
    while (!done) test_clock_poll(f->ua, 100);
    poll_events(f);
}

static void fallback(int device) {
    struct fixture f; create(&f, device, 0); start(&f);
    fail(&f, 0); assert(peer(&f, 2) && !peer(&f, 3));
    fail(&f, 1); assert(!peer(&f, 3));
    fail(&f, 2);
    assert(peer(&f, 3) && peer(&f, 4) && !peer(&f, 5));
    up(&f, 3); fail(&f, 4);
    assert(!peer(&f, 5)); /* одного UP десктопа достаточно, чтобы запретить Android */
    down(&f, 3);
    assert(peer(&f, 5) && peer(&f, 6));
    up(&f, 5); up(&f, 6);
    assert(topo_group_connect_active_count(f.group) == 2);
    wait_retry(&f);
    assert(peer(&f, 0) || peer(&f, 1) || peer(&f, 2));
    topo_group_connect_restart(f.group); poll_events(&f);
    assert(peer(&f, 0)); up(&f, 0);
    assert(peer(&f, 5) && peer(&f, 6)); /* сохраняем даже без READY */
    assert(!peer(&f, 3) && !peer(&f, 4));
    assert(!f.conns[3]->close_requested && !f.conns[4]->close_requested);
    down(&f, 5);
    assert(!peer(&f, 5) && peer(&f, 6) && !peer(&f, 7));
    assert(!f.group->recovery);
    destroy(&f);
}

static void all_supernodes(void) {
    struct fixture f; create(&f, CLIENT_TYPE_SERVER, 1); start(&f);
    up(&f, 0); assert(peer(&f, 2));
    up(&f, 1); up(&f, 2);
    assert(topo_group_connect_active_count(f.group) == 3);
    for (int i = 3; i < PEERS; i++) assert(!peer(&f, i));
    down(&f, 0); assert(!peer(&f, 0));
    wait_retry(&f); assert(peer(&f, 0));
    for (int i = 3; i < PEERS; i++) assert(!peer(&f, i));
    destroy(&f);
}

/* Обновлённый SQL-порядок должен менять выбор уже известных кандидатов. */
static void refreshed_priority(void) {
    struct fixture f; create(&f, CLIENT_TYPE_DESKTOP, 0); start(&f);
    fail(&f, 0); fail(&f, 1); fail(&f, 2);
    assert(peer(&f, 3) && peer(&f, 4));
    assert(topo_node_sqlite_set_connected(f.inst->topo_sqlite_db, CHANNEL, f.conns[2]->peer_ed25519_pubkey, 1) == 0);
    assert(sqlite3_exec(f.inst->topo_sqlite_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_update_rtt(f.inst->topo_sqlite_db, f.conns[2]->crypto_ctx.peer_public_key, 1) == SQLITE_OK);
    assert(sqlite3_exec(f.inst->topo_sqlite_db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    topo_group_connect_restart(f.group); poll_events(&f);
    assert(peer(&f, 2) && peer(&f, 0) && !peer(&f, 1));
    destroy(&f);
}

/* Проверяем реальный срок повтора, включая отсутствие немедленного повторного JOIN. */
static void expect_retry(struct fixture* f, int index, int delay_ms) {
    assert(!peer(f, index));
    test_clock_run_for(f->ua, (delay_ms - 200) * 10);
    assert(!peer(f, index));
    test_clock_run_for(f->ua, 4000); poll_events(f);
    assert(peer(f, index));
}

/* Отказ/согласование проходят тот же приёмник, что и пакеты удалённого пира. */
static void group_message(struct fixture* f, int index, uint8_t subcmd) {
    struct TOPO_GROUP_CONN_ITEM* p = peer(f, index);
    struct TOPOMSG_JOIN_GROUP msg = { .protocol_version = htons(TOPO_GROUP_PROTOCOL_VERSION), .h = { .cmd = ETCP_ID_TOPO_ENTRY, .subcmd = subcmd,
        .src_epoch = 7, .dst_epoch = subcmd == TOPO_SUBCMD_JOIN_GROUP ? 0 : p->local_epoch },
        .group_type = subcmd == TOPO_SUBCMD_JOIN_REJECT ? TOPO_JOIN_NO_GROUP : TOPO_GROUP_TYPE_CHAT };
    memcpy(msg.h.group_key, topo_group_wire_id(f->group), 32);
    size_t size = subcmd == TOPO_SUBCMD_REQUEST_TABLE || subcmd == TOPO_SUBCMD_TABLE_COMPLETE ? sizeof(msg.h) : sizeof(msg);
    struct ll_entry* entry = ll_alloc_lldgram(size); assert(entry);
    memcpy(entry->dgram, &msg, size); entry->len = size;
    f->inst->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY](f->conns[index], entry);
}

/* READY действительно завершает обмен и сбрасывает историю предыдущих REJECT. */
static void complete_join(struct fixture* f, int index) {
    group_message(f, index, TOPO_SUBCMD_JOIN_ACCEPT);
    group_message(f, index, TOPO_SUBCMD_REQUEST_TABLE);
    for (int n = 0; n < 128 && !peer(f, index)->table_sent; n++) {
        poll_events(f);
        struct ll_entry* entry = queue_data_get(f->conns[index]->send_input_q);
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        queue_resume_callback(f->conns[index]->send_input_q);
    }
    assert(peer(f, index)->table_sent);
    group_message(f, index, TOPO_SUBCMD_TABLE_COMPLETE); poll_events(f);
    assert(topo_group_peer_ready(f->group, f->ids[index]));
}

/* История недоступности растёт при неудачах до UP и ограничена 30 секундами. */
static void unavailable_retries(void) {
    struct fixture f; create(&f, CLIENT_TYPE_SERVER, 1); start(&f);
    up(&f, 1); up(&f, 2);
    const int delays[] = {5000, 10000, 15000, 20000, 25000, 30000, 30000};
    for (size_t n = 0; n < sizeof(delays) / sizeof(delays[0]); n++) {
        fail(&f, 0);
        expect_retry(&f, 0, delays[n]);
    }
    up(&f, 0); down(&f, 0);
    expect_retry(&f, 0, 5000); /* После восстановления транспорта снова начинаем с 5 секунд. */
    destroy(&f);
}

/* MEMBERS может прийти после таймаута ручного bootstrap к ещё не классифицированному узлу. */
static void delayed_admission(void) {
    struct fixture f; create(&f, CLIENT_TYPE_DESKTOP, 0);
    int index = PEERS - 1;
    assert(sqlite3_exec(f.inst->topo_sqlite_db, "UPDATE peers_" CHANNEL " SET admitted=0", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_group_connect_node_once(f.group, f.conns[index]->peer_ed25519_pubkey) == 0);
    f.conns[index]->links_up = 1; etcp_cbk_fire(f.conns[index], ETCP_CBK_EVENT_UP); poll_events(&f);
    assert(peer(&f, index) && peer(&f, index)->requests && !peer(&f, index)->conn);
    test_clock_run_for(f.ua, 21000); poll_events(&f);
    assert(!peer(&f, index) && !f.conns[index]->close_requested && f.conns[index]->links_up);
    assert(topo_group_connect_defer_join(f.group, f.conns[index]->crypto_ctx.peer_public_key));
    topo_group_connect_admitted(f.group, f.conns[index]);
    assert(topo_group_connect_defer_join(f.group, f.conns[index]->crypto_ctx.peer_public_key)); /* До committed допуска обход запрещён. */
    assert(sqlite3_exec(f.inst->topo_sqlite_db, "UPDATE peers_" CHANNEL " SET admitted=1,identity_verified=1", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_group_new_conn(f.group, f.conns[index]) == 0 && !peer(&f, index)); /* Старая задержка ещё мешает JOIN. */
    topo_group_connect_admitted(f.group, f.conns[index]);
    assert(!topo_group_connect_defer_join(f.group, f.conns[index]->crypto_ctx.peer_public_key));
    assert(topo_group_new_conn(f.group, f.conns[index]) == 0); poll_events(&f);
    assert(peer(&f, index) && peer(&f, index)->retained && !peer(&f, index)->requests);
    complete_join(&f, index);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] delayed admission resumes unknown bootstrap peer on existing transport through READY");
    destroy(&f);
}

/* REJECT учитывается до и после освобождения запроса при UP; общий транспорт остаётся живым. */
static void rejected_retries(int early) {
    struct fixture f; create(&f, CLIENT_TYPE_SERVER, 1); start(&f);
    up(&f, 1); up(&f, 2);
    if (early) {
        f.conns[0]->links_up = 1;
        etcp_cbk_fire(f.conns[0], ETCP_CBK_EVENT_UP);
        assert(peer(&f, 0)->requests); /* Отказ приходит раньше отложенного шага планировщика. */
    } else up(&f, 0);
    const int delays[] = {3000, 120000, 120000};
    for (size_t n = 0; n < sizeof(delays) / sizeof(delays[0]); n++) {
        group_message(&f, 0, TOPO_SUBCMD_JOIN_REJECT); poll_events(&f);
        assert(!f.conns[0]->close_requested && f.conns[0]->links_up);
        assert(peer(&f, 1) && peer(&f, 2));
        topo_group_connect_admitted(f.group, f.conns[0]);
        assert(topo_group_connect_defer_join(f.group, f.conns[0]->crypto_ctx.peer_public_key)); /* Локальный допуск не отменяет удалённый REJECT. */
        assert(topo_group_new_conn(f.group, f.conns[0]) == 0 && !peer(&f, 0));
        struct TOPO_PEER_REQUEST* blocked = NULL;
        assert(topo_group_peer_open(f.group, f.ids[0], &blocked) < 0 && !blocked);
        topo_group_connect_restart(f.group); poll_events(&f); /* Изменение сети не снимает задержку REJECT. */
        expect_retry(&f, 0, delays[n]);
        assert(!peer(&f, 0)->requests && !topo_group_peer_ready(f.group, f.ids[0]));
    }
    complete_join(&f, 0);
    group_message(&f, 0, TOPO_SUBCMD_JOIN_REJECT); poll_events(&f);
    expect_retry(&f, 0, 3000);
    group_message(&f, 0, TOPO_SUBCMD_JOIN_REJECT); poll_events(&f);
    group_message(&f, 0, TOPO_SUBCMD_JOIN_GROUP); poll_events(&f);
    assert(peer(&f, 0)); /* Входящий JOIN разрешён даже во время 120-секундной задержки. */
    complete_join(&f, 0);
    destroy(&f);
}

/* Тип берётся из сохранённого snapshot; приватный адрес и пустые адреса не снимают роль. */
static void persisted_classification(void) {
    struct fixture f; create(&f, CLIENT_TYPE_DESKTOP, 0);
    struct topo_connect_peer* list = NULL; size_t count = 0;
    assert(topo_node_sqlite_get_connect_peers(f.inst->topo_sqlite_db, CHANNEL, &list, &count) == 0);
    assert(count == PEERS + 1);
    for (size_t i = 0; i < count; i++) {
        for (int n = 0; n < PEERS; n++) if (list[i].node_id == f.ids[n]) {
            assert(list[i].supernode == (n < 3));
            assert(list[i].client_type == (n < 5 ? CLIENT_TYPE_DESKTOP : n < 7 ? CLIENT_TYPE_MOBILE : CLIENT_TYPE_SERVER));
        }
    }
    u_free(list);
    struct TOPO_NODE* ni = topo_node_sqlite_snapshot_load(f.inst->topo_sqlite_db, f.inst->topo_groups, f.ids[0]); assert(ni);
    memory_pool_free(f.inst->topo_groups->v4_addr_pool, ni->v4_addrs); ni->v4_addrs = NULL;
    assert(topo_node_sqlite_addrs_put(f.inst->topo_sqlite_db, ni->node_id, ni) == 0);
    assert(topo_node_sqlite_nodeinfo_updated(f.inst->topo_sqlite_db, ni->public_key) == 0);
    assert(topo_node_sqlite_get_node_type(f.inst->topo_sqlite_db, CHANNEL, ni->node_id) == 4);
    uint64_t* ids = NULL; int number = 0;
    assert(topo_node_sqlite_get_supernode_peers(f.inst->topo_sqlite_db, CHANNEL, &ids, &number) == 0 && number == 3);
    u_free(ids); topo_node_destroy(f.inst->topo_groups, ni);
    destroy(&f);
}

#ifdef UTUN_HAVE_STANDBY
/* DOWN в SLEEP меняет состояние сразу, а новые попытки ждут активной фазы. */
static void standby_down(void) {
    struct fixture f; create(&f, CLIENT_TYPE_MOBILE, 0); start(&f);
    up(&f, 0); up(&f, 1);
    standby_set_intervals_ms(100, 200, 0);
    assert(standby_set_enabled(f.inst, 1) == 0);
    while (!standby_get_sleep_tb()) test_clock_poll(f.ua, 100);
    down(&f, 0);
    assert(topo_group_connect_active_count(f.group) == 1 && !peer(&f, 0) && !peer(&f, 2));
    assert(standby_set_enabled(f.inst, 0) == 0); poll_events(&f);
    assert(peer(&f, 2));
    destroy(&f);

    create(&f, CLIENT_TYPE_MOBILE, 0);
    standby_set_intervals_ms(100, 200, 0);
    assert(standby_set_enabled(f.inst, 1) == 0);
    while (!standby_get_sleep_tb()) test_clock_poll(f.ua, 100);
    assert(topo_group_connect_init(f.group) == 0); poll_events(&f);
    assert(!peer(&f, 0) && !peer(&f, 1));
    /* Остаток SLEEP может обнулиться раньше доставки фазового callback. */
    while (!standby_get_active_remaining_tb()) test_clock_poll(f.ua, 100);
    poll_events(&f); assert(peer(&f, 0) && peer(&f, 1));
    assert(standby_set_enabled(f.inst, 0) == 0);
    destroy(&f);
}
#endif

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_INFO);
    test_clock_init(10000); /* До ближайшего таймера, максимум 1 сек: сценарии включают несколько 120-секундных backoff. */
    test_clock_sync_wall_time();
    utun_instance_set_tun_init_enabled(0);
    persisted_classification();
    two_supernodes();
    fallback(CLIENT_TYPE_DESKTOP);
    fallback(CLIENT_TYPE_MOBILE);
    all_supernodes();
    refreshed_priority();
    unavailable_retries();
    delayed_admission();
    rejected_retries(0);
    rejected_retries(1);
#ifdef UTUN_HAVE_STANDBY
    standby_down();
#endif
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect tests passed: UP, fallback, ownership, unavailable 5..30s, REJECT 3/120s, READY reset");
    test_clock_finish();
    return 0;
}
