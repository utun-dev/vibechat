/* Реальный сборщик/SQLite, управляемые часы и остановленный SQL worker: минимумы, интервалы и teardown. */
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include "utun_instance.h"
#include "db_schema.h"
#include "routing_layer/topo_node_rtt.h"
#include "routing_layer/topo_node_sqlite.h"
#include "chat/chat_member.h"
#include "transport_layer/etcp.h"
#include "test_clock.h"
#include "test_utils.h"
#include "../lib/mem.h"
#define CHANNEL "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12"

struct fixture {
    struct UTUN_INSTANCE inst;
    struct TOPO_GROUPS groups;
    struct ETCP_CONN conn;
    struct ETCP_LINK links[5];
};

static int seed(sqlite3* db, void* arg) {
    (void)arg;
    assert(!db_schema_init(db) && !db_schema_table(db, "peers_" CHANNEL "", "peers"));
    return sqlite3_exec(db,
        "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES"
        "(7,x'0700000000000000000000000000000000000000000000000000000000000000',x'0700000000000000000000000000000000000000000000000000000000000000'),"
        "(8,x'0800000000000000000000000000000000000000000000000000000000000000',x'0800000000000000000000000000000000000000000000000000000000000000'),"
        "(9,x'0900000000000000000000000000000000000000000000000000000000000000',x'0900000000000000000000000000000000000000000000000000000000000000');"
        "INSERT INTO node_addresses(node_x,family,protocol,address,port,socket_id,address_kind) VALUES"
        "(x'0700000000000000000000000000000000000000000000000000000000000000',4,1,x'7f000001',9001,1,0),(x'0700000000000000000000000000000000000000000000000000000000000000',4,1,x'7f000001',9001,1,1),"
        "(x'0700000000000000000000000000000000000000000000000000000000000000',4,1,x'7f000001',9002,2,0),(x'0700000000000000000000000000000000000000000000000000000000000000',4,2,x'7f000001',9001,3,0),"
        "(x'0700000000000000000000000000000000000000000000000000000000000000',4,6,x'7f000001',9001,4,0),(x'0700000000000000000000000000000000000000000000000000000000000000',6,1,x'00000000000000000000000000000001',9003,5,0),"
        "(x'0800000000000000000000000000000000000000000000000000000000000000',4,1,x'7f000001',9001,1,0);"
        "INSERT INTO peers_" CHANNEL "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,admitted) VALUES"
        "(7,x'0700000000000000000000000000000000000000000000000000000000000000',x'0700000000000000000000000000000000000000000000000000000000000000',zeroblob(64),1,1),(8,x'0800000000000000000000000000000000000000000000000000000000000000',x'0800000000000000000000000000000000000000000000000000000000000000',zeroblob(64),1,1),"
        "(9,x'0900000000000000000000000000000000000000000000000000000000000000',x'0900000000000000000000000000000000000000000000000000000000000000',zeroblob(64),1,1);"
        "CREATE TABLE rtt_audit(kind INTEGER,value INTEGER);"
        "CREATE TRIGGER node_rtt_audit AFTER UPDATE OF rtt ON nodes BEGIN INSERT INTO rtt_audit VALUES(0,NEW.rtt); END;"
        "CREATE TRIGGER address_rtt_audit AFTER UPDATE OF rtt ON node_addresses BEGIN INSERT INTO rtt_audit VALUES(1,NEW.rtt); END",
        NULL, NULL, NULL);
}

static void fixture_init(struct fixture* f) {
    memset(f, 0, sizeof(*f));
    f->inst.ua = uasync_create(); assert(f->inst.ua);
    f->inst.topo_groups = &f->groups; f->groups.instance = &f->inst;
    f->groups.group_list = queue_new(f->inst.ua, 0, 0, 0, "rtt_groups"); assert(f->groups.group_list);
    f->inst.connections = queue_new(f->inst.ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "rtt_connections"); assert(f->inst.connections);
    struct ll_entry* e = queue_entry_new(sizeof(struct conn_queue_entry)); assert(e);
    *(struct conn_queue_entry*)e->data = (struct conn_queue_entry){7, &f->conn, {7}};
    assert(!queue_data_put_with_index(f->inst.connections, e));
    f->conn.instance = &f->inst; f->conn.peer_node_id = 7; f->conn.links = f->links; f->conn.crypto_ctx.peer_public_key[0] = 7;
    for (unsigned i = 0; i < 5; i++) {
        struct ETCP_LINK* link = &f->links[i]; link->etcp = &f->conn;
        link->next = i < 4 ? &f->links[i + 1] : NULL;
        link->initialized = link->link_status = 1;
        struct sockaddr_in* a = (struct sockaddr_in*)&link->remote_addr;
        a->sin_family = AF_INET; a->sin_port = htons(i == 2 ? 9002 : 9001); a->sin_addr.s_addr = htonl(0x7f000001);
    }
    f->links[3].is_tcp = f->links[4].is_tcp = 1; f->links[4].reality_set = 1;
}

static void fixture_free(struct fixture* f) {
    topo_node_rtt_stop(&f->inst);
    if (f->inst.db_worker) assert(!db_async_destroy(f->inst.db_worker));
    if (f->inst.topo_sqlite_db) assert(!sqlite3_close(f->inst.topo_sqlite_db));
    queue_clear(f->inst.connections); queue_free(f->inst.connections); queue_free(f->groups.group_list);
    uasync_poll(f->inst.ua, 0);
    assert(f->inst.ua->timer_alloc_count == f->inst.ua->timer_free_count);
    uasync_destroy(f->inst.ua, 0);
}

static sqlite3_int64 scalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = NULL; assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW); sqlite3_int64 value = sqlite3_column_int64(st, 0);
    assert(!sqlite3_finalize(st)); return value;
}

static void sample(struct fixture* f, unsigned link, uint16_t value) {
    f->links[link].rtt_last = value; topo_node_rtt_link_sample(&f->links[link]);
}

static struct TOPO_GROUP_NODE* average_node(struct TOPO_GROUP* group, struct ETCP_CONN* conn, uint64_t id) {
    struct TOPO_GROUP_NODE* node = (struct TOPO_GROUP_NODE*)queue_entry_new(sizeof(*node)); assert(node);
    memset((uint8_t*)node + sizeof(node->ll), 0, sizeof(*node) - sizeof(node->ll)); node->node_id = id;
    assert(!queue_data_put_with_index(group->nodes, &node->ll));
    uint8_t ed[32] = {(uint8_t)id}; uint32_t address;
    assert(!topo_address_short(ed, &address)); topo_address_attach(group, node, ed, address);
    struct topo_hop hops[] = {{.id=id}, {.id=conn->peer_node_id}};
    assert(!topo_group_add_path(node, conn, hops, id == conn->peer_node_id ? 1 : 2, id == conn->peer_node_id ? 0 : 50));
    return node;
}

static unsigned average_direct_events, average_transit_events;
static void average_event(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, void* arg) {
    (void)group; (void)arg;
    if (node->node_id == 7) average_direct_events++;
    else average_transit_events++;
}

/* Настоящие события TIMESTAMP/метрик, без БД и таймеров публикации; getter ничего не накапливает. */
static void runtime_average_checks(void) {
    struct fixture f; fixture_init(&f); f.conn.links_up = 1;
    queue_free(f.groups.group_list); f.groups.group_list = queue_new(f.inst.ua, 16, 0, 8, "average_groups");
    assert(f.groups.group_list);
    struct TOPO_GROUP* groups[2]; struct TOPO_GROUP_NODE* direct[2]; struct TOPO_GROUP_NODE* transit[2];
    for (unsigned i = 0; i < 2; i++) {
        groups[i] = (struct TOPO_GROUP*)queue_entry_new(sizeof(*groups[i])); assert(groups[i]);
        memset((uint8_t*)groups[i] + sizeof(struct ll_entry), 0, sizeof(*groups[i]) - sizeof(struct ll_entry));
        groups[i]->instance = &f.inst; groups[i]->group_id = 42 + i; groups[i]->group_type = TOPO_GROUP_TYPE_CHAT;
        strcpy(groups[i]->channel_id, i ? "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951" : CHANNEL);
        groups[i]->nodes = queue_new(f.inst.ua, 16, 0, 8, "average_nodes"); assert(groups[i]->nodes);
        assert(!queue_data_put_with_index(f.groups.group_list, &groups[i]->ll));
        direct[i] = average_node(groups[i], &f.conn, 7); transit[i] = average_node(groups[i], &f.conn, 9);
    }
    assert(!utun_add_nodeinfo_cbk(&f.inst, average_event, NULL));
    average_direct_events = average_transit_events = 0;
    unsigned timers = f.inst.ua->timer_alloc_count;
    sample(&f, 0, 100);
    assert(topo_get_chain_rtt_avg(direct[0]) == 100 && topo_get_chain_rtt_avg(transit[0]) == 150);
    assert(average_direct_events == 2 && average_transit_events == 2);
    test_clock_run_for(f.inst.ua, 1000); sample(&f, 0, 200);
    test_clock_run_for(f.inst.ua, 1000); sample(&f, 0, 200); /* Одинаковые реальные замеры тоже учитываются. */
    assert(topo_get_chain_rtt(direct[0]) == 200 && topo_get_chain_rtt_avg(direct[0]) == 100);
    assert(direct[0]->rtt_average.count == 3 && average_transit_events == 2);
    topo_node_rtt_probe_sample(&f.inst, (const uint8_t[32]){7}, 500, &f.links[0].remote_addr);
    assert(direct[0]->rtt_average.count == 3 && transit[0]->rtt_average.count == 3);
    assert(!topo_node_rtt_average_update(groups[0], direct[0], 0));
    assert(direct[0]->rtt_average.count == 3);
    test_clock_run_for(f.inst.ua, 800); assert(topo_get_chain_rtt_avg(direct[0]) == 100);
    test_clock_run_for(f.inst.ua, 400); sample(&f, 0, 900);
    for (unsigned i = 0; i < 2; i++) {
        assert(topo_get_chain_rtt_avg(direct[i]) == 167 && topo_get_chain_rtt_avg(transit[i]) == 217);
        assert(direct[i]->rtt_average.count == 1 && direct[i]->rtt_average.sum == 900);
    }
    assert(average_transit_events == 4);
    struct chat_member_display member = {.ed25519={9}};
    assert(!chat_member_apply_live(&f.inst, CHANNEL, &member));
    assert(member.online && member.rtt == 217 && member.intermediaries == 1);
    uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE]; chat_core_serialize_member(&member, wire);
    struct chat_member_display decoded; assert(!chat_core_deserialize_member(wire, &decoded)); assert(decoded.rtt == 217);
    test_clock_run_for(f.inst.ua, 4000);
    assert(topo_get_chain_rtt_avg(direct[0]) == 167); /* Без нового события публикации нет. */
    sample(&f, 0, 1000); assert(topo_get_chain_rtt_avg(direct[0]) == 1000);
    struct TOPO_NODEPATH* path = topo_node_best_path(transit[0]); path->cumulative_rtt = 75;
    assert(!topo_node_rtt_average_update(groups[0], transit[0], 0));
    assert(transit[0]->rtt_average.sum == 2125 && transit[1]->rtt_average.sum == 1050);
    assert(!topo_node_rtt_average_update(groups[0], transit[0], 0) && transit[0]->rtt_average.count == 2);
    struct ETCP_CONN other = {.instance=&f.inst, .peer_node_id=8, .links_up=1, .rtt_last=300};
    assert(topo_group_remove_path(transit[0], &f.conn) == 1);
    struct topo_hop hops[] = {{.id=9}, {.id=8}}; assert(!topo_group_add_path(transit[0], &other, hops, 2, 50));
    assert(topo_get_chain_rtt_avg(transit[0]) == 350);
    assert(topo_node_rtt_average_update(groups[0], transit[0], 0));
    assert(transit[0]->rtt_average.count == 1 && topo_get_chain_rtt_avg(transit[1]) == 1050);
    f.conn.reset_id++; f.conn.rtt_last = 400;
    assert(topo_get_chain_rtt_avg(direct[0]) == 400 && topo_node_rtt_average_update(groups[0], direct[0], 0));
    f.conn.rtt_last = 0;
    assert(topo_get_chain_rtt_avg(direct[0]) == UINT16_MAX);
    assert(topo_node_rtt_average_update(groups[0], direct[0], 0) && !direct[0]->rtt_average.count);
    other.rtt_last = UINT16_MAX;
    assert(topo_node_rtt_average_update(groups[0], transit[0], 1));
    assert(topo_get_chain_rtt_avg(transit[0]) == UINT16_MAX);
    assert(!f.inst.rtt_pending && f.inst.ua->timer_alloc_count == timers);
    utun_remove_nodeinfo_cbk(&f.inst, average_event, NULL);
    for (unsigned i = 0; i < 2; i++) {
        struct ll_entry* e;
        while ((e = queue_data_get(groups[i]->nodes))) { topo_address_detach(groups[i], (struct TOPO_GROUP_NODE*)e); topo_nodeq_clear_paths((struct TOPO_GROUP_NODE*)e); queue_entry_free(e); }
        queue_free(groups[i]->nodes); queue_remove_data(f.groups.group_list, &groups[i]->ll); queue_entry_free(&groups[i]->ll);
    }
    fixture_free(&f);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS event-driven 300ms RTT: rounding, idle, path/session, transit, groups, GUI wire, no timers/SQL");
}

static void memory_checks(void) {
    struct fixture f; fixture_init(&f); sqlite3* db = NULL;
    assert(!sqlite3_open(":memory:", &db) && !seed(db, NULL)); f.inst.topo_sqlite_db = db;
    sample(&f, 0, 100); assert(f.conn.rtt_last == 100);
    test_clock_run_for(f.inst.ua, 4000);
    sample(&f, 0, 200); sample(&f, 2, 60); assert(f.conn.rtt_last == 60);
    test_clock_run_for(f.inst.ua, 4000);
    assert(!scalar(db, "SELECT COUNT(*) FROM rtt_audit"));
    test_clock_run_for(f.inst.ua, 2100);
    assert(scalar(db, "SELECT rtt FROM nodes WHERE node_id=7") == 120);
    assert(scalar(db, "SELECT COUNT(*) FROM node_addresses WHERE port=9001 AND protocol=1 AND node_x=x'0700000000000000000000000000000000000000000000000000000000000000' AND rtt=150") == 2);
    assert(scalar(db, "SELECT rtt FROM node_addresses WHERE port=9002") == 60);
    assert(scalar(db, "SELECT COUNT(*) FROM node_addresses WHERE rtt IS NULL") == 4);
    assert(!f.inst.rtt_pending);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS mean of node minima, independent endpoint means, aliases, no early save and idle flush");

    sample(&f, 1, 80); sample(&f, 0, 40); sample(&f, 1, 90);
    test_clock_run_for(f.inst.ua, 10100);
    assert(scalar(db, "SELECT rtt FROM nodes WHERE node_id=7") == 47); /* minima 60,40,40 */
    assert(scalar(db, "SELECT rtt FROM node_addresses WHERE port=9001 AND protocol=1 LIMIT 1") == 53); /* 80,40,40 */
    f.links[0].link_status = 0; etcp_conn_update_rtt(&f.conn); assert(f.conn.rtt_last == 60);
    f.links[2].closing = 1; etcp_conn_update_rtt(&f.conn); assert(f.conn.rtt_last == 90);
    f.links[1].initialized = 0; etcp_conn_update_rtt(&f.conn); assert(!f.conn.rtt_last);
    f.links[0].link_status = f.links[1].initialized = 1; f.links[2].closing = 0;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS shared remote endpoint minimum, rounding and exclusion of DOWN/closing/uninitialized links");

    sample(&f, 3, 30); sample(&f, 4, 20);
    struct sockaddr_in6* v6 = (struct sockaddr_in6*)&f.links[1].remote_addr;
    memset(v6, 0, sizeof(*v6)); v6->sin6_family = AF_INET6; v6->sin6_port = htons(9003); v6->sin6_addr.s6_addr[15] = 1;
    sample(&f, 1, 50); test_clock_run_for(f.inst.ua, 10100);
    assert(scalar(db, "SELECT rtt FROM node_addresses WHERE protocol=2") == 30);
    assert(scalar(db, "SELECT rtt FROM node_addresses WHERE protocol=6") == 20);
    assert(scalar(db, "SELECT rtt FROM node_addresses WHERE family=6") == 50);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS UDP/TCP/REALITY and IPv4/IPv6 endpoint isolation");

    topo_node_rtt_probe_sample(&f.inst, (const uint8_t[32]){7}, 500, &f.links[0].remote_addr);
    assert(!f.inst.rtt_pending); /* Измеренные линки не заменяются пробами. */
    topo_node_rtt_probe_sample(&f.inst, (const uint8_t[32]){8}, 300, &f.links[0].remote_addr);
    topo_node_rtt_probe_sample(&f.inst, (const uint8_t[32]){8}, 100, &f.links[0].remote_addr);
    test_clock_run_for(f.inst.ua, 10100);
    assert(scalar(db, "SELECT rtt FROM nodes WHERE node_id=8") == 200);
    assert(scalar(db, "SELECT rtt FROM node_addresses WHERE node_x=x'0800000000000000000000000000000000000000000000000000000000000000'") == 200);
    topo_node_rtt_probe_sample(&f.inst, (const uint8_t[32]){9}, 10, &f.links[2].remote_addr); /* Нет строки адреса. */
    test_clock_run_for(f.inst.ua, 10100);
    assert(scalar(db, "SELECT rtt FROM nodes WHERE node_id=9") == 10);
    assert(!scalar(db, "SELECT COUNT(*) FROM node_addresses WHERE node_x=x'0900000000000000000000000000000000000000000000000000000000000000'"));
    struct topo_connect_peer* peers = NULL; size_t count = 0;
    assert(!topo_node_sqlite_get_connect_peers(db, CHANNEL, &peers, &count) && count == 3);
    assert(peers[0].node_id == 9 && peers[1].node_id == 7 && peers[2].node_id == 8); u_free(peers);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS probe fallback, missing endpoint and candidate ordering by nodes.rtt");

    struct TOPO_ADDR4 address = {.addr={127,0,0,1}, .port=9001, .socket_id=1, .protocol=TOPO_PROTO_UDP};
    struct TOPO_NODE node = {.node_id=7, .public_key={7}, .v4_addrs=&address};
    assert(!topo_node_sqlite_addrs_put(db, 7, &node));
    assert(scalar(db, "SELECT rtt FROM node_addresses WHERE node_x=x'0700000000000000000000000000000000000000000000000000000000000000'") == 53);
    assert(scalar(db, "SELECT COUNT(*) FROM node_addresses WHERE node_x=x'0700000000000000000000000000000000000000000000000000000000000000'") == 1);
    address.port = 9010; assert(!topo_node_sqlite_addrs_put(db, 7, &node));
    assert(scalar(db, "SELECT COUNT(*) FROM node_addresses WHERE node_x=x'0700000000000000000000000000000000000000000000000000000000000000' AND rtt IS NULL") == 1);
    assert(!scalar(db, "SELECT COUNT(*) FROM sqlite_temp_master WHERE name='node_address_rtt'"));
    assert(!scalar(db, "SELECT COUNT(*) FROM pragma_table_info('peers_" CHANNEL "') WHERE name IN('creator_sig','node_RTT')"));
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS NODEINFO preserves unchanged address RTT and discards removed endpoints");

    assert(!sqlite3_exec(db, "CREATE TRIGGER reject_rtt BEFORE UPDATE OF rtt ON node_addresses"
        " BEGIN SELECT RAISE(ABORT,'RTT veto'); END", NULL, NULL, NULL));
    address.port = 9010;
    ((struct sockaddr_in*)&f.links[0].remote_addr)->sin_port = htons(9010);
    sample(&f, 0, 100); test_clock_run_for(f.inst.ua, 10100);
    assert(scalar(db, "SELECT rtt FROM nodes WHERE node_id=7") == 23); /* До ошибочной транзакции. */
    assert(f.inst.rtt_pending && !scalar(db, "SELECT COUNT(*) FROM node_addresses WHERE node_x=x'0700000000000000000000000000000000000000000000000000000000000000' AND rtt IS NOT NULL"));
    assert(!sqlite3_exec(db, "DROP TRIGGER reject_rtt", NULL, NULL, NULL));
    sample(&f, 4, 40); test_clock_run_for(f.inst.ua, 10100);
    assert(scalar(db, "SELECT rtt FROM nodes WHERE node_id=7") == 25); /* Сохранены неудавшийся20 и новый30. */
    assert(scalar(db, "SELECT rtt FROM node_addresses WHERE node_x=x'0700000000000000000000000000000000000000000000000000000000000000'") == 100);
    assert(!f.inst.rtt_pending);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS failed address WRITE rolls back node RTT and retries all samples");

    sample(&f, 0, 200);
    assert(!sqlite3_exec(db, "BEGIN", NULL, NULL, NULL));
    test_clock_run_for(f.inst.ua, 10100);
    assert(!sqlite3_get_autocommit(db)); /* Чужая транзакция не откатывается. */
    assert(!sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL));
    topo_node_rtt_stop(&f.inst); assert(!f.inst.rtt_pending);
    fixture_free(&f);
}

static pthread_mutex_t pause_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cond = PTHREAD_COND_INITIALIZER;
static int entered, released;
static int pause_worker(sqlite3* db, void* data) {
    (void)db; (void)data;
    pthread_mutex_lock(&pause_mutex); entered = 1; pthread_cond_signal(&pause_cond);
    while (!released) pthread_cond_wait(&pause_cond, &pause_mutex);
    pthread_mutex_unlock(&pause_mutex); return SQLITE_OK;
}
static void done(void* data, const struct db_async_result* result) { assert(!result->status); if (data) (*(int*)data)++; }
static void drain(struct fixture* f) {
    uint64_t end = get_time_us() + 5000000;
    while (db_async_pending(f->inst.db_worker) && get_time_us() < end) uasync_poll(f->inst.ua, 1);
    assert(!db_async_pending(f->inst.db_worker));
}
struct inspection { int mean, writes; };
static int inspect(sqlite3* db, void* data) {
    struct inspection* i = data;
    assert(scalar(db, "SELECT rtt FROM nodes WHERE node_id=7") == i->mean);
    assert(scalar(db, "SELECT COUNT(*) FROM rtt_audit WHERE kind=0") == i->writes);
    return SQLITE_OK;
}
static void inspect_worker(struct fixture* f, int mean, int writes) {
    struct inspection i = {mean, writes};
    db_async_submit(f->inst.db_worker, "inspect-rtt", DB_ASYNC_READ, inspect, &i, done); drain(f);
}
static void block_worker(struct fixture* f) {
    pthread_mutex_lock(&pause_mutex); entered = released = 0; pthread_mutex_unlock(&pause_mutex);
    db_async_submit(f->inst.db_worker, "pause-rtt", DB_ASYNC_READ, pause_worker, NULL, done);
    uint64_t end = get_time_us() + 5000000;
    for (;;) {
        pthread_mutex_lock(&pause_mutex); int ready = entered; pthread_mutex_unlock(&pause_mutex);
        if (ready) break;
        assert(get_time_us() < end); uasync_poll(f->inst.ua, 1);
    }
}
static void release_worker(void) {
    pthread_mutex_lock(&pause_mutex); released = 1; pthread_cond_signal(&pause_cond); pthread_mutex_unlock(&pause_mutex);
}
static void asynchronous_checks(void) {
    struct fixture f; fixture_init(&f);
    char directory[512] = "/tmp/utun-rtt-XXXXXX", path[600]; assert(!test_mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/chats.db", directory);
    int opened = 0; f.inst.db_worker = db_async_create(f.inst.ua, path, 8, done, &opened); assert(f.inst.db_worker);
    drain(&f); assert(opened == 1);
    db_async_submit(f.inst.db_worker, "seed-rtt", DB_ASYNC_WRITE, seed, NULL, done); drain(&f);
    block_worker(&f);
    sample(&f, 0, 100); sample(&f, 0, 300); test_clock_run_for(f.inst.ua, 10100);
    assert(db_async_pending(f.inst.db_worker) == 2);
    sample(&f, 0, 500); sample(&f, 0, 700); /* Новые замеры не меняют frozen SQL-входы. */
    test_clock_run_for(f.inst.ua, 20100); assert(db_async_pending(f.inst.db_worker) == 2);
    release_worker(); drain(&f); inspect_worker(&f, 200, 1);
    test_clock_run_for(f.inst.ua, 5000); inspect_worker(&f, 200, 1);
    test_clock_run_for(f.inst.ua, 5100); drain(&f); inspect_worker(&f, 600, 2);
    assert(!f.inst.rtt_pending);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS frozen async snapshots, updates during SQL, no concurrent WRITE and one-second interval after commit");

    block_worker(&f); sample(&f, 0, 900); test_clock_run_for(f.inst.ua, 10100);
    topo_node_rtt_stop(&f.inst); assert(!f.inst.rtt_pending);
    memset(f.links, 0, sizeof(f.links)); f.conn.links = NULL; /* SQL не использует runtime линков. */
    release_worker(); drain(&f); inspect_worker(&f, 900, 3);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS stopped collector and removed links with a queued WRITE");
    fixture_free(&f); assert(!remove(path) && !rmdir(directory));
}

/* Синхронное ядро владеет RW SQLite, worker только читает: нельзя направлять сохранение RTT этому reader. */
static void readonly_worker_checks(void) {
    struct fixture f; fixture_init(&f);
    char directory[512] = "/tmp/utun-rtt-reader-XXXXXX", path[600]; assert(!test_mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/chats.db", directory);
    assert(!sqlite3_open(path, &f.inst.topo_sqlite_db)); sqlite3* db = f.inst.topo_sqlite_db;
    assert(!sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL) && !seed(db, NULL));
    int opened = 0;
    const struct db_async_open_options options = {.limit=8, .readonly=1};
    f.inst.db_worker = db_async_open(f.inst.ua, path, &options, done, &opened); assert(f.inst.db_worker);
    drain(&f); assert(opened == 1 && !db_async_can_write(f.inst.db_worker));
    block_worker(&f);
    sample(&f, 0, 100); sample(&f, 0, 300); test_clock_run_for(f.inst.ua, 10100);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "RTT reader fixture mean=%lld writes=%lld reader_pending=%u collector_pending=%d",
        (long long)scalar(db, "SELECT rtt FROM nodes WHERE node_id=7"),
        (long long)scalar(db, "SELECT COUNT(*) FROM rtt_audit WHERE kind=0"), db_async_pending(f.inst.db_worker), f.inst.rtt_pending != NULL);
    assert(scalar(db, "SELECT rtt FROM nodes WHERE node_id=7") == 200);
    assert(!f.inst.rtt_pending && db_async_pending(f.inst.db_worker) == 1);
    release_worker(); drain(&f); inspect_worker(&f, 200, 1);
    assert(!sqlite3_exec(db, "CREATE TRIGGER reject_rtt BEFORE UPDATE OF rtt ON node_addresses"
        " BEGIN SELECT RAISE(ABORT,'RTT reader-mode veto'); END", NULL, NULL, NULL));
    sample(&f, 0, 500); test_clock_run_for(f.inst.ua, 10100);
    inspect_worker(&f, 200, 1); assert(f.inst.rtt_pending);
    assert(!sqlite3_exec(db, "DROP TRIGGER reject_rtt", NULL, NULL, NULL));
    test_clock_run_for(f.inst.ua, 10100); inspect_worker(&f, 500, 2); assert(!f.inst.rtt_pending);
    assert(scalar(db, "SELECT rtt FROM node_addresses WHERE port=9001 AND protocol=1 LIMIT 1") == 500);
    assert(!sqlite3_exec(db, "BEGIN", NULL, NULL, NULL));
    sample(&f, 0, 700); test_clock_run_for(f.inst.ua, 10100);
    assert(!sqlite3_get_autocommit(db) && f.inst.rtt_pending);
    assert(!sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL));
    test_clock_run_for(f.inst.ua, 10100); inspect_worker(&f, 700, 3); assert(!f.inst.rtt_pending);
    assert(!db_async_can_write(f.inst.db_worker));
    sample(&f, 0, 900); topo_node_rtt_stop(&f.inst);
    test_clock_run_for(f.inst.ua, 10100); inspect_worker(&f, 700, 3);
    fixture_free(&f); assert(!remove(path) && !rmdir(directory));
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS core writer with READONLY worker: commit, rollback/retry without new samples, foreign transaction and stop");
}

int main(void) {
    setbuf(stdout, NULL);
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
    test_clock_init(1000); size_t before = u_get_allocated_count();
    runtime_average_checks(); memory_checks(); asynchronous_checks(); readonly_worker_checks();
    assert(u_get_allocated_count() == before); test_clock_finish();
    puts("PASS node RTT: node/endpoint averages, timers, async snapshots, rollback and teardown"); return 0;
}
