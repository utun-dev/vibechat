#define GROUP_A "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"
#define GROUP_B "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16"

#include <assert.h>
#include <string.h>
#include <openssl/evp.h>
#include "utun_instance.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/topo_node_rtt.h"
#include "routing_layer/route_lib.h"
#include "transport_layer/secure_channel.h"
#include "transport_layer/etcp.h"
#include "config_parser.h"
#include "../lib/mem.h"
#include "../lib/memory_pool.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"

static void sign_record(struct UTUN_INSTANCE* inst, struct TOPO_NODE* ni) {
    uint8_t msg[TOPO_SIG_MSG_MAX_SIZE];
    int len = topo_node_build_sig_msg(ni, msg, sizeof(msg));
    assert(len > 0);
    assert(sc_ed25519_sign(inst->my_ed25519_privkey, msg, (size_t)len, ni->x25519_self_sig) == SC_OK);
}

static struct TOPO_NODE* record(struct UTUN_INSTANCE* inst, uint64_t timestamp, const char* name, int address) {
    struct TOPO_NODE* ni = u_calloc(1, sizeof(*ni));
    assert(ni);
    ni->node_id = inst->node_id;
    ni->timestamp = timestamp;
    strcpy(ni->connection_order, "RUT2-1");
    ni->node_name = u_strdup(name);
    memcpy(ni->public_key, inst->my_keys.public_key, 32);
    memcpy(ni->ed25519_public_key, inst->my_ed25519_pubkey, 32);
    if (address) {
        ni->v4_addrs = memory_pool_alloc(inst->topo_groups->v4_addr_pool);
        assert(ni->v4_addrs);
        *ni->v4_addrs = (struct TOPO_ADDR4){ .addr = {127,0,0,1}, .port = 9000, .protocol = TOPO_PROTO_UDP };
    }
    sign_record(inst, ni);
    return ni;
}

/* Старый CHECK не перестраивается: запуск отклоняется с сохранением адресов. */
static void test_zero_port_schema(void) {
    sqlite3 *db = NULL; assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE node_addresses("
        "id INTEGER PRIMARY KEY,node_id INTEGER,family INTEGER,protocol INTEGER,address BLOB,"
        "port INTEGER CHECK(port > 0 AND port <= 65535),rtt INTEGER,addr_type INTEGER,config_type INTEGER,"
        "nat_mapping INTEGER,nat_inbound INTEGER,nat_source INTEGER,address_kind INTEGER,socket_id INTEGER,"
        "options BLOB,created_at INTEGER);"
        "CREATE UNIQUE INDEX idx_na_unique ON node_addresses(node_id,family,socket_id,protocol,address,port,address_kind);"
        "INSERT INTO node_addresses VALUES(7,1,4,2,X'CB00712C',443,15,4,2,0,0,1,1,3,X'0102',123)",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_init(db) < 0);
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT port,rtt,nat_source,length(options),created_at FROM node_addresses WHERE id=7",
                             -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 443);
    assert(sqlite3_column_int(statement, 1) == 15 && sqlite3_column_int(statement, 2) == ADDRESS_SOURCE_ETCP);
    assert(sqlite3_column_int(statement, 3) == 2 && sqlite3_column_int(statement, 4) == 123);
    sqlite3_finalize(statement);
    assert(sqlite3_exec(db, "UPDATE node_addresses SET port=0 WHERE id=7", NULL, NULL, NULL) == SQLITE_CONSTRAINT);
    assert(topo_node_sqlite_init(db) < 0);
    sqlite3_close(db);
}

/* Прерывает VM SQLite для проверки незавершённого чтения списка. */
static int interrupt_sql(void* arg) { (void)arg; return 1; }

/* Отказ SQL не должен выглядеть успешным сохранением или чтением узла. */
static void test_verified_write(void) {
    sqlite3* db = NULL; uint8_t key[32] = {1};
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK && !topo_node_sqlite_init(db));
    assert(!topo_node_sqlite_node_update_verified(db, 7, "before", key, key, 100, 1));
    assert(sqlite3_exec(db, "CREATE TRIGGER reject_node BEFORE INSERT ON nodes "
        "BEGIN SELECT RAISE(ABORT,'test insert rejection'); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_node_update_verified(db, 8, "rejected", key, key, 101, 1) < 0);
    assert(sqlite3_exec(db, "DROP TRIGGER reject_node; CREATE TRIGGER reject_node BEFORE UPDATE ON nodes "
        "BEGIN SELECT RAISE(ABORT,'test update rejection'); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_node_update_verified(db, 7, "rejected", key, key, 101, 1) < 0);
    assert(topo_node_sqlite_node_set_online(db, 7, 0) < 0 && topo_node_sqlite_node_get_online(db, 7) == 1);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT name,update_ts FROM nodes ORDER BY node_id", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), "before"));
    assert(sqlite3_column_int64(st, 1) == 100 && sqlite3_step(st) == SQLITE_DONE); sqlite3_finalize(st);
    assert(sqlite3_exec(db, "DROP TRIGGER reject_node", NULL, NULL, NULL) == SQLITE_OK);
    uint8_t unrelated[32] = {2};
    struct TOPO_NODE displaced = {.node_id=7}; memcpy(displaced.public_key, unrelated, 32);
    assert(topo_node_sqlite_addrs_write(db, 7, &displaced) == SQLITE_CONSTRAINT);
    assert(!topo_node_sqlite_node_write(db, 7, "colliding route alias", unrelated, unrelated, 102, 1));
    struct TOPO_ADDR4 shared_address = {.addr={192,0,2,1}, .port=4000, .protocol=TOPO_PROTO_UDP};
    struct TOPO_NODE first = {.node_id=7, .v4_addrs=&shared_address}; memcpy(first.public_key, key, 32);
    displaced.v4_addrs = &shared_address;
    assert(!topo_node_sqlite_addrs_write(db, 7, &first) && !topo_node_sqlite_addrs_write(db, 7, &displaced));
    assert(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    const struct topo_rtt_endpoint endpoint = {.family=4, .protocol=TOPO_PROTO_UDP, .port=4000, .address={192,0,2,1}};
    assert(!topo_node_sqlite_update_address_rtt(db, key, &endpoint, 30));
    assert(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, "SELECT node_x,rtt FROM node_addresses ORDER BY node_x", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && !memcmp(sqlite3_column_blob(st, 0), key, 32) && sqlite3_column_int(st, 1) == 30);
    assert(sqlite3_step(st) == SQLITE_ROW && !memcmp(sqlite3_column_blob(st, 0), unrelated, 32) && sqlite3_column_type(st, 1) == SQLITE_NULL);
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(sqlite3_exec(db, "DELETE FROM node_addresses", NULL, NULL, NULL) == SQLITE_OK);
    uint8_t ambiguous[32]; assert(topo_node_sqlite_get_ed25519_pubkey(db, 7, ambiguous) == TOPO_SQLITE_LOOKUP_ERROR);
    assert(!topo_node_sqlite_node_write(db, 7, "same public key", key, key, 100, 1));
    assert(topo_node_sqlite_node_write(db, 7, "wrong Ed25519", key, unrelated, 101, 1) == SQLITE_CONSTRAINT);
    assert(sqlite3_exec(db, "DELETE FROM nodes WHERE hex(x25519_pubkey) LIKE '02%'", NULL, NULL, NULL) == SQLITE_OK);
    assert(!topo_node_sqlite_node_update_verified(db, 7, "after", key, key, 101, 1));
    assert(sqlite3_prepare_v2(db, "SELECT name,update_ts FROM nodes WHERE node_id=7", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), "after"));
    assert(sqlite3_column_int64(st, 1) == 101); sqlite3_finalize(st);
    uint8_t signature[64] = {0}, group_ed[32], group_x[32], secret[32];
    assert(!sc_identity_from_seed((const uint8_t[32]){94}, group_x, secret, group_ed));
    assert(!topo_node_sqlite_channel_put(db, GROUP_A, "test", (const uint8_t[32]){1}, group_x, NULL, group_ed, NULL, signature));
    assert(!topo_node_sqlite_member_placeholder_put(db, GROUP_A, 7, key, key));
    uint8_t other_x[32], other_ed[32];
    assert(!sc_identity_from_seed((const uint8_t[32]){95}, other_x, secret, other_ed));
    assert(!topo_node_sqlite_channel_put(db, GROUP_B, "test", (const uint8_t[32]){1}, other_x, NULL, other_ed, NULL, signature));
    assert(!topo_node_sqlite_member_placeholder_put(db, GROUP_B, 7, key, key));
    assert(!topo_node_sqlite_set_connected(db, GROUP_A, key, 1));
    uint64_t* ids = NULL; int count = 0;
    assert(!topo_node_sqlite_get_connected_peers(db, GROUP_A, &ids, &count) && count == 1 && ids[0] == 7); u_free(ids);
    uint8_t (*channels)[32] = NULL;
    assert(!topo_node_sqlite_get_member_channels(db, key, &channels, &count) && count == 2);
    assert(memcmp(channels[0], channels[1], 32)); u_free(channels);
    for (int query = 0; query < 5; query++) {
        ids = (void*)(uintptr_t)1; channels = (void*)(uintptr_t)1; count = 99;
        sqlite3_progress_handler(db, 1, interrupt_sql, NULL);
        int rc = query == 0 ? topo_node_sqlite_get_connected_peers(db, GROUP_A, &ids, &count) :
                 query == 1 ? topo_node_sqlite_get_supernode_peers(db, GROUP_A, &ids, &count) :
                 query == 2 ? topo_node_sqlite_get_public_peers(db, GROUP_A, &ids, &count, 0) :
                 query == 3 ? topo_node_sqlite_get_local_peers(db, GROUP_A, &ids, &count, 0) :
                              topo_node_sqlite_get_member_channels(db, key, &channels, &count);
        sqlite3_progress_handler(db, 0, NULL, NULL);
        assert(rc < 0 && (query == 4 ? !channels : !ids) && !count && !sqlite3_next_stmt(db, NULL));
    }
    assert(sqlite3_exec(db, "UPDATE peers_" GROUP_A " SET node_type=2; UPDATE peers_" GROUP_B " SET node_type=4;"
        "CREATE TRIGGER reject_classification BEFORE UPDATE ON peers_" GROUP_B " "
        "BEGIN SELECT RAISE(ABORT,'test classification rejection'); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_nodeinfo_updated(db, key) < 0);
    assert(topo_node_sqlite_get_node_type(db, GROUP_A, 7) == 2 && topo_node_sqlite_get_node_type(db, GROUP_B, 7) == 4);
    assert(sqlite3_exec(db, "DROP TRIGGER reject_classification; CREATE TRIGGER reject_channel BEFORE DELETE ON channels "
        "BEGIN SELECT RAISE(ABORT,'test channel deletion rejection'); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_channel_delete(db, GROUP_B) < 0 && topo_node_sqlite_member_in_channel(db, GROUP_B, 7));
    assert(!topo_node_sqlite_channel_get(db, GROUP_B, NULL, 0, NULL, NULL, NULL, NULL));
    assert(sqlite3_exec(db, "DROP TRIGGER reject_channel", NULL, NULL, NULL) == SQLITE_OK);
    assert(!topo_node_sqlite_nodeinfo_updated(db, key) && topo_node_sqlite_get_node_type(db, GROUP_A, 7) == 0);
    assert(!topo_node_sqlite_channel_delete(db, GROUP_B));
    assert(topo_node_sqlite_channel_get(db, GROUP_B, NULL, 0, NULL, NULL, NULL, NULL) < 0);
    assert(sqlite3_exec(db, "CREATE TRIGGER reject_member BEFORE DELETE ON peers_" GROUP_A " "
        "BEGIN SELECT RAISE(ABORT,'test member deletion rejection'); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_member_del(db, GROUP_A, 7) < 0 && topo_node_sqlite_member_in_channel(db, GROUP_A, 7));
    assert(sqlite3_exec(db, "UPDATE channels SET x25519_pubkey=X'01'", NULL, NULL, NULL) == SQLITE_CONSTRAINT);
    /* Имитируем уже повреждённую БД для проверки читающей стороны. */
    assert(sqlite3_exec(db, "DROP TRIGGER reject_member; UPDATE peers_" GROUP_A " SET join_sig=X'01'; "
        "PRAGMA ignore_check_constraints=ON; UPDATE channels SET x25519_pubkey=X'01'; PRAGMA ignore_check_constraints=OFF",
        NULL, NULL, NULL) == SQLITE_OK);
    uint64_t timestamp = 77; memset(signature, 0xa5, sizeof(signature));
    assert(topo_node_sqlite_member_get_join(db, GROUP_A, key, signature, &timestamp) < 0);
    assert(timestamp == 77 && signature[0] == 0xa5);
    memset(key, 0xa5, sizeof(key));
    assert(topo_node_sqlite_channel_get(db, GROUP_A, NULL, 0, NULL, key, NULL, NULL) < 0 && key[0] == 0xa5);
    assert(!topo_node_sqlite_member_del(db, GROUP_A, 7) && !topo_node_sqlite_member_in_channel(db, GROUP_A, 7));
    assert(!topo_node_sqlite_node_set_online(db, 7, 0) && !topo_node_sqlite_node_get_online(db, 7));
    assert(sqlite3_exec(db, "DROP TABLE nodes", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_node_update_verified(db, 7, "no table", key, key, 102, 1) < 0);
    sqlite3_close(db);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] verified node writes: insert/update, rejected writes, prepare failure and stored state");
}

static void test_snapshot_format_upgrade(struct UTUN_INSTANCE* inst) {
    sqlite3* saved = inst->topo_sqlite_db;
    sqlite3* db = NULL; assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    inst->topo_sqlite_db = db;
    assert(sqlite3_exec(db, "CREATE TABLE node_snapshots(node_id INTEGER PRIMARY KEY,timestamp INTEGER NOT NULL,record BLOB NOT NULL);"
        "INSERT INTO node_snapshots VALUES(7,9000000,X'0102030405')", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_init(db) < 0);
    sqlite3_stmt* legacy = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT timestamp,hex(record) FROM node_snapshots WHERE node_id=7", -1, &legacy, NULL) == SQLITE_OK);
    assert(sqlite3_step(legacy) == SQLITE_ROW && sqlite3_column_int64(legacy, 0) == 9000000);
    assert(!strcmp((const char*)sqlite3_column_text(legacy, 1), "0102030405")); sqlite3_finalize(legacy);
    assert(sqlite3_close(db) == SQLITE_OK && sqlite3_open(":memory:", &db) == SQLITE_OK);
    inst->topo_sqlite_db = db;
    assert(!topo_node_sqlite_init(db));
    uint64_t timestamp = 0;
    assert(!topo_node_sqlite_snapshot_timestamp(db, 7, &timestamp) && timestamp == 0);
    uint8_t group_x[32], group_ed[32], secret[32];
    assert(!sc_identity_from_seed((const uint8_t[32]){94}, group_x, secret, group_ed));
    assert(!topo_node_sqlite_channel_put(db, GROUP_A, "upgrade", (const uint8_t[32]){1}, group_x, NULL, group_ed, NULL,
        (const uint8_t[64]){0}));
    assert(!topo_node_sqlite_member_placeholder_put(db, GROUP_A, 7, inst->my_keys.public_key, inst->my_ed25519_pubkey));
    assert(sqlite3_exec(db, "UPDATE peers_" GROUP_A " SET source=0,admitted=1", NULL, NULL, NULL) == SQLITE_OK);
    struct topo_connect_peer* peers = NULL; size_t count = 0;
    assert(!topo_node_sqlite_get_connect_peers(db, GROUP_A, &peers, &count) && count == 1 && peers[0].client_type == UINT8_MAX);
    u_free(peers);
    struct TOPO_NODE* self = record(inst, 123, "format fixture", 1); assert(self);
    assert(!topo_node_sqlite_snapshot_put(db, self));
    assert(sqlite3_exec(db, "UPDATE node_snapshots SET record=X'' WHERE format=3", NULL, NULL, NULL) == SQLITE_OK);
    assert(!topo_node_sqlite_snapshot_put(db, self)); // Та же версия снова заполняет инвалидированный кеш.
    assert(!topo_node_sqlite_init(db));
    struct TOPO_NODE* loaded = topo_node_sqlite_snapshot_load(db, inst->topo_groups, inst->node_id);
    assert(loaded && loaded->timestamp == self->timestamp); topo_node_destroy(inst->topo_groups, loaded);
    topo_node_destroy(inst->topo_groups, self); sqlite3_close(db); inst->topo_sqlite_db = saved;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] NODEINFO old schema rejected intact; current snapshots and connect candidates survive restart");
}

/* Новый wire и SQL сохраняют адреса, протоколы, классификацию и REALITY; смена формата оставляет bootstrap. */
static void test_compact_addresses(struct UTUN_INSTANCE *inst) {
    test_zero_port_schema();
    sqlite3 *saved_db = inst->topo_sqlite_db;
    assert(sqlite3_open(":memory:", &inst->topo_sqlite_db) == SQLITE_OK);
    sqlite3 *db = inst->topo_sqlite_db;
    assert(!topo_node_sqlite_init(db));
    assert(!topo_node_sqlite_node_update_verified(db, inst->node_id, "address fixture", inst->my_keys.public_key,
                                                inst->my_ed25519_pubkey, 100, 100));
    struct TOPO_NODE *node = record(inst, INT64_MAX - 100, "address fixture", 1);
    memcpy(node->v4_addrs->addr, (uint8_t[]){194,63,142,140}, 4);
    node->v4_addrs->port = 1333; node->v4_addrs->socket_id = 5;
    node->v4_sock_meta = memory_pool_alloc(inst->topo_groups->v4_sock_meta_pool);
    node->v6_sock_meta = memory_pool_alloc(inst->topo_groups->v6_sock_meta_pool);
    node->v6_addrs = memory_pool_alloc(inst->topo_groups->v6_addr_pool);
    node->reality_socks = u_calloc(1, sizeof(*node->reality_socks));
    assert(node->v4_sock_meta && node->v6_sock_meta && node->v6_addrs && node->reality_socks);
    *node->v4_sock_meta = (struct TOPO_SOCKMETA4){.id = 5, .config_type = CFG_SERVER_TYPE_PUBLIC,
        .address_profile = {ADDRESS_MAP_REAL_IP, ADDRESS_IN_OPEN, ADDRESS_SOURCE_ETCP}};
    *node->v6_sock_meta = (struct TOPO_SOCKMETA6){.id = 6, .config_type = CFG_SERVER_TYPE_PUBLIC,
        .address_profile = {ADDRESS_MAP_REAL_IP, ADDRESS_IN_CONTACT, ADDRESS_SOURCE_ETCP}};
    *node->v6_addrs = (struct TOPO_ADDR6){.addr = {0x2a,0x04,0x52,0,0,0,0,0,0,0,0,0,0,0,0,0xf},
        .port = 443, .socket_id = 6, .protocol = TOPO_PROTO_TCP | TOPO_PROTO_REALITY};
    node->reality_socks->socket_id = 6; strcpy(node->reality_socks->server_name, "example.org");
    sign_record(inst, node);
    assert(sizeof(struct TOPOMSG_SOCKMETA4) == 5 && sizeof(struct TOPOMSG_SOCKMETA6) == 5);
    assert(!topo_node_sqlite_snapshot_put(db, node) && !topo_node_sqlite_addrs_put(db, inst->node_id, node));
    struct TOPO_NODE *loaded = topo_node_sqlite_snapshot_load(db, inst->topo_groups, inst->node_id);
    assert(loaded && !topo_node_verify(loaded));
    assert(!memcmp(loaded->v4_addrs->addr, node->v4_addrs->addr, 4) && loaded->v4_addrs->port == 1333);
    assert(loaded->v4_sock_meta->address_profile.inbound == ADDRESS_IN_OPEN);
    assert(!memcmp(loaded->v6_addrs->addr, node->v6_addrs->addr, 16) && loaded->v6_addrs->port == 443);
    assert(loaded->v6_addrs->protocol == (TOPO_PROTO_TCP | TOPO_PROTO_REALITY));
    assert(loaded->v6_sock_meta->address_profile.inbound == ADDRESS_IN_CONTACT);
    assert(!strcmp(loaded->reality_socks->server_name, "example.org"));
    topo_node_destroy(inst->topo_groups, loaded);
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT family,protocol,port,socket_id,addr_type,length(options)"
        " FROM node_addresses ORDER BY family", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 4);
    assert(sqlite3_column_int(statement, 1) == TOPO_PROTO_UDP && sqlite3_column_int(statement, 2) == 1333);
    assert(sqlite3_column_int(statement, 3) == 5 && sqlite3_column_int(statement, 4) == ADDR_TYPE_DIRECT);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 6);
    assert(sqlite3_column_int(statement, 1) == (TOPO_PROTO_TCP | TOPO_PROTO_REALITY));
    assert(sqlite3_column_int(statement, 2) == 443 && sqlite3_column_int(statement, 3) == 6);
    assert(sqlite3_column_int(statement, 4) == ADDR_TYPE_CONTACT && sqlite3_column_int(statement, 5) == TOPO_ADDR_REALITY_OPTS_SIZE);
    assert(sqlite3_step(statement) == SQLITE_DONE); sqlite3_finalize(statement);

    /* Публичный IP интерфейса не становится NAT UNKNOWN из-за отсутствия измерения входящих. */
    struct address_profile known = node->v4_sock_meta->address_profile;
    node->v4_sock_meta->address_profile = (struct address_profile){0};
    assert(!topo_node_sqlite_addrs_put(db, inst->node_id, node));
    assert(sqlite3_prepare_v2(db, "SELECT addr_type FROM node_addresses WHERE family=4", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == ADDR_TYPE_NETIF);
    sqlite3_finalize(statement);
    node->v4_sock_meta->address_profile = known;
    assert(!topo_node_sqlite_addrs_put(db, inst->node_id, node));

    /* Лишние поля старого формата запрещают запуск; снимок и адреса не меняются. */
    assert(sqlite3_exec(db, "ALTER TABLE node_addresses ADD COLUMN nat_evidence INTEGER DEFAULT 1;"
        "ALTER TABLE node_addresses ADD COLUMN nat_checked INTEGER DEFAULT 1", NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sqlite_init(db) < 0);
    uint64_t saved_timestamp;
    assert(!topo_node_sqlite_snapshot_timestamp(db, inst->node_id, &saved_timestamp) && saved_timestamp == node->timestamp);
    loaded = topo_node_sqlite_snapshot_load(db, inst->topo_groups, inst->node_id);
    assert(loaded && loaded->timestamp == node->timestamp && loaded->v4_addrs && loaded->v6_addrs && loaded->reality_socks);
    assert(loaded->v4_sock_meta->address_profile.source == known.source);
    assert(loaded->v4_addrs->port == 1333 && loaded->v6_addrs->port == 443);
    assert(!strcmp(loaded->reality_socks->server_name, "example.org"));
    topo_node_destroy(inst->topo_groups, loaded);
    node->timestamp = 0;
    assert(!topo_node_sign_self(inst, node) && node->timestamp == INT64_MAX - 99);
    assert(topo_node_sqlite_init(db) < 0);
    loaded = topo_node_sqlite_snapshot_load(db, inst->topo_groups, inst->node_id);
    assert(loaded && loaded->timestamp == node->timestamp && !topo_node_verify(loaded));
    topo_node_destroy(inst->topo_groups, loaded); topo_node_destroy(inst->topo_groups, node);
    sqlite3_close(db); inst->topo_sqlite_db = saved_db;
}

/* Каждый размер буфера: длина, NUL, защитные байты, сохранённый префикс и маркер усечения. */
static void test_node_format(struct TOPO_GROUP* group) {
    char full[8192];
    int full_len = topo_node_format_all(group, full, sizeof(full));
    assert(full_len > 0 && (size_t)full_len == strlen(full) && !strstr(full, "[TRUNCATED]"));
    assert(strstr(full, "=== BGP Nodes dump:"));
    if (group->nodes->head) assert(strstr(full, "  v4addr[0]:") && strstr(full, "  path[0]:"));
    const char marker[] = "\n[TRUNCATED]\n";
    debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_ERROR);
    for (size_t capacity = 0; capacity <= (size_t)full_len + 2; capacity++) {
        uint8_t* storage = u_malloc(capacity + 2);
        assert(storage); memset(storage, 0xa5, capacity + 2);
        char* buf = (char*)storage + 1;
        int length = topo_node_format_all(group, buf, capacity);
        assert(storage[0] == 0xa5 && storage[capacity + 1] == 0xa5);
        if (!capacity) { assert(!length); u_free(storage); continue; }
        size_t expected = (size_t)full_len < capacity ? (size_t)full_len : capacity - 1;
        assert(length >= 0 && (size_t)length == expected);
        assert(memchr(buf, '\0', capacity) == buf + length);
        if (capacity > (size_t)full_len) assert(!memcmp(buf, full, full_len + 1));
        else {
            size_t marker_len = expected < sizeof(marker) - 1 ? expected : sizeof(marker) - 1;
            assert(!memcmp(buf, full, expected - marker_len));
            assert(!memcmp(buf + expected - marker_len, marker, marker_len));
        }
        u_free(storage);
    }
    char empty[] = "old";
    assert(!topo_node_format_all(NULL, empty, sizeof(empty)) && !empty[0]);
    assert(!topo_node_format_all(group, NULL, sizeof(empty)));
    debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] node dump: nodes=%d capacities=0..%d, lengths, guards, NUL and truncation",
               queue_entry_count(group->nodes), full_len + 2);
}

static void test_bgp(struct UTUN_INSTANCE* signer) {
    struct UTUN_INSTANCE receiver = { .ua = signer->ua, .topo_groups = signer->topo_groups };
    struct TOPO_GROUP group = { .instance = &receiver, .group_id = 10, .group_type = TOPO_GROUP_TYPE_CHAT, .wire_id = { [31]=10 } };
    struct ETCP_CONN from = { .instance = &receiver, .peer_node_id = 7 };
    group.nodes = queue_new(receiver.ua, 16, 0, 8, "snapshot_bgp");
    test_node_format(&group);
    struct TOPO_NODE* ni = record(signer, INT64_MAX - 7, "BGP", 1);
    from.peer_node_id = ni->node_id; from.links_up = 1;
    memcpy(from.crypto_ctx.peer_public_key, ni->public_key, 32); memcpy(from.peer_ed25519_pubkey, ni->ed25519_public_key, 32);
    struct TOPO_GROUP_NODE nq = {0};
    nq.phone_status = TOPO_FLAG_PHONE_KNOWN;
    uint8_t packet[4096] = {0};
    memcpy(((struct TOPOMSG_HEADER*)packet)->group_key, group.wire_id, 32);
    int len = topo_node_serialize(ni, &nq, 0, packet + sizeof(struct TOPOMSG_HEADER), sizeof(packet) - sizeof(struct TOPOMSG_HEADER), 0);
    assert(len > 0);
    assert(topo_group_process_nodeinfo(&group, &from, packet, (size_t)len + sizeof(struct TOPOMSG_HEADER)) == 0);
    struct TOPO_GROUP_NODE* peer = topo_node_find_by_id(&group, ni->node_id);
    assert(peer && peer->last_timestamp == ni->timestamp && queue_entry_count(peer->paths) == 1);
    assert(topo_group_process_nodeinfo(&group, &from, packet, (size_t)len + sizeof(struct TOPOMSG_HEADER)) == 0);
    assert(queue_entry_count(peer->paths) == 1);
    test_node_format(&group);
    struct TOPOMSG_NODEINFO_PKT* wire = (struct TOPOMSG_NODEINFO_PKT*)packet;
    wire->node.local_v4_subnets = 1; /* malformed CHAT не меняет уже принятую запись и маршрут */
    assert(topo_group_process_nodeinfo(&group, &from, packet, (size_t)len + sizeof(struct TOPOMSG_HEADER)) < 0);
    assert(topo_node_find_by_id(&group, ni->node_id) == peer && queue_entry_count(peer->paths) == 1);
    assert(peer->last_timestamp == ni->timestamp && topo_node_registry_find(signer->topo_groups, ni->node_id)->timestamp == ni->timestamp);
    wire->node.local_v4_subnets = 0;
    wire->node.timestamp++; /* подделка не удаляет узел и ранее принятый маршрут */
    assert(topo_group_process_nodeinfo(&group, &from, packet, (size_t)len + sizeof(struct TOPOMSG_HEADER)) < 0);
    assert(topo_node_find_by_id(&group, ni->node_id) == peer && queue_entry_count(peer->paths) == 1);
    wire->node.timestamp--;
    nq.phone_status |= TOPO_FLAG_PHONE_BUSY; /* Позднее объявление старого звонка. */
    ni->timestamp--;
    sign_record(signer, ni);
    len = topo_node_serialize(ni, &nq, 0, packet + sizeof(struct TOPOMSG_HEADER), sizeof(packet) - sizeof(struct TOPOMSG_HEADER), 0);
    assert(topo_group_process_nodeinfo(&group, &from, packet, (size_t)len + sizeof(struct TOPOMSG_HEADER)) == 0);
    assert(peer->last_timestamp == INT64_MAX - 7 && queue_entry_count(peer->paths) == 1);
    assert(peer->phone_status == TOPO_FLAG_PHONE_KNOWN);
    topo_group_remove_path(peer, &from);
    topo_nodeq_remove_node(&group, peer);
    topo_node_destroy(signer->topo_groups, ni);
    queue_free(group.nodes);
}

/* Замена подсети при том же количестве и откат ошибок сохраняют согласованные local_node/route table. */
static void test_local_group_update(struct UTUN_INSTANCE* inst) {
    sqlite3* saved_db = inst->topo_sqlite_db;
    assert(sqlite3_open(":memory:", &inst->topo_sqlite_db) == SQLITE_OK && !topo_node_sqlite_init(inst->topo_sqlite_db));
    struct CFG_ROUTE_ENTRY subnet = {.ip.family = AF_INET, .netmask = 24};
    subnet.ip.addr.v4.s_addr = htonl(0x0a010000);
    struct utun_config config = {.my_subnets = &subnet};
    inst->config = &config; inst->rt = route_table_create(); assert(inst->rt);
    struct TOPO_GROUP group = {.instance = inst, .group_type = TOPO_GROUP_TYPE_UTUN, .group_id = TOPO_GROUP_UTUN};
    memcpy(group.ed25519_public_key, inst->my_ed25519_pubkey, 32);
    assert(topo_group_update_my_nodeinfo(inst, &group) == 1 && route_lookup(inst->rt, 0x0a010001));
    subnet.ip.addr.v4.s_addr = htonl(0x0a020000);
    assert(topo_group_update_my_nodeinfo(inst, &group) == 1 && route_lookup(inst->rt, 0x0a020001));
    assert(!route_lookup(inst->rt, 0x0a010001));
    struct TOPO_GROUP_NODE* before = group.local_node;
    struct TOPO_NODE* identity = topo_node_registry_find(inst->topo_groups, inst->node_id);
    uint64_t timestamp = identity->timestamp; uint32_t refs = identity->group_ref_count;
    assert(sqlite3_exec(inst->topo_sqlite_db, "CREATE TRIGGER reject_local BEFORE UPDATE ON node_snapshots "
        "BEGIN SELECT RAISE(ABORT,'test local identity rejection'); END", NULL, NULL, NULL) == SQLITE_OK);
    subnet.ip.addr.v4.s_addr = htonl(0x0a030000);
    assert(topo_group_update_my_nodeinfo(inst, &group) < 0 && group.local_node == before);
    assert(route_lookup(inst->rt, 0x0a020001) && !route_lookup(inst->rt, 0x0a030001));
    assert(identity->timestamp == timestamp && identity->group_ref_count == refs && !topo_node_verify(identity));
    assert(sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER reject_local", NULL, NULL, NULL) == SQLITE_OK);
    struct TOPO_SUBNET4 remote_sub = {.addr = {10, 3, 0, 0}, .prefix_length = 24};
    struct TOPO_NODESUBNETS remote_subnets = {.v4_subnets = &remote_sub};
    struct TOPO_GROUP_NODE remote = {.node_id = 999, .subnets = &remote_subnets};
    assert(route_insert(inst->rt, &remote, false));
    assert(topo_group_update_my_nodeinfo(inst, &group) < 0 && group.local_node == before);
    assert(route_lookup(inst->rt, 0x0a020001)->v_node_info == before && route_lookup(inst->rt, 0x0a030001)->v_node_info == &remote);
    assert(identity->timestamp == timestamp && identity->group_ref_count == refs && !topo_node_verify(identity));
    route_table_destroy(inst->rt); inst->rt = NULL;
    topo_nodeq_free_group_fields(inst->topo_groups, group.local_node); u_free(group.local_node);
    assert(!inst->topo_groups->node_registry->head);
    sqlite3_close(inst->topo_sqlite_db); inst->topo_sqlite_db = saved_db; inst->config = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] local group: same-count subnet replacement, signature failure and route conflict rollback");
}

/* Два разных ключа попали в одну числовую корзину. Фикстура задаёт индекс напрямую. */
static void test_registry_collision(struct TOPO_GROUPS* groups) {
    struct TOPO_NODE* a = u_calloc(1, sizeof(*a));
    struct TOPO_NODE* b = u_calloc(1, sizeof(*b));
    assert(a && b);
    a->node_id = b->node_id = 42; a->public_key[0] = 1; b->public_key[0] = 2;
    assert(topo_node_registry_put(groups, a) == a && topo_node_registry_put(groups, b) == b);
    assert(!topo_node_registry_find(groups, 42));
    assert(topo_node_registry_lookup(groups, 42, a->public_key) == a);
    assert(topo_node_registry_lookup(groups, 42, b->public_key) == b);
    topo_node_ref(b);
    topo_node_registry_release(groups, a);
    assert(topo_node_registry_find(groups, 42) == b && b->group_ref_count == 2);
    topo_node_registry_release(groups, b);
    topo_node_registry_release(groups, b);
    assert(!topo_node_registry_find(groups, 42));
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_DEBUG);
    struct UTUN_INSTANCE inst = {0};
    struct TOPO_GROUPS groups = { .instance = &inst };
    inst.topo_groups = &groups;
    inst.ua = uasync_create();
    assert(inst.ua);
    groups.node_registry = queue_new(inst.ua, 16, 0, 8, "snapshot_test");
    groups.v4_addr_pool = memory_pool_init(sizeof(struct TOPO_ADDR4), "snapshot_v4");
    groups.v4_sock_meta_pool = memory_pool_init(sizeof(struct TOPO_SOCKMETA4), "snapshot_meta4");
    groups.v6_sock_meta_pool = memory_pool_init(sizeof(struct TOPO_SOCKMETA6), "snapshot_meta6");
    groups.v6_addr_pool = memory_pool_init(sizeof(struct TOPO_ADDR6), "snapshot_v6");
    groups.v4_subnet_pool = memory_pool_init(sizeof(struct TOPO_SUBNET4), "snapshot_subnet4");
    groups.v6_subnet_pool = memory_pool_init(sizeof(struct TOPO_SUBNET6), "snapshot_subnet6");
    assert(groups.v4_addr_pool && groups.v4_sock_meta_pool && groups.v6_sock_meta_pool && groups.v6_addr_pool);
    assert(!sc_identity_generate(inst.my_keys.public_key, inst.my_keys.private_key, inst.my_ed25519_pubkey, inst.my_ed25519_privkey));
    inst.node_id = sc_derive_node_id_from_pubkey(inst.my_keys.public_key);
    assert(sqlite3_open(":memory:", &inst.topo_sqlite_db) == SQLITE_OK);
    assert(topo_node_sqlite_init(inst.topo_sqlite_db) == 0);
    test_registry_collision(&groups);
    test_verified_write();
    test_snapshot_format_upgrade(&inst);
    test_compact_addresses(&inst);
    test_local_group_update(&inst);

    struct TOPO_NODE* first = record(&inst, 100, "old", 1);
    assert(topo_node_verify(first) == 0);
    first->timestamp++;
    assert(topo_node_verify(first) < 0); /* timestamp защищён подписью */
    first->timestamp--;
    assert(topo_node_registry_store(&groups, first) == first);
    struct TOPO_NODE* stale = record(&inst, 99, "stale", 0);
    assert(topo_node_registry_store(&groups, stale) == first);
    topo_node_registry_unref(&groups, inst.node_id);
    assert(first->timestamp == 100 && first->v4_addrs && !strcmp(first->node_name, "old"));

    struct TOPO_NODE* conflict = record(&inst, 100, "conflict", 0);
    assert(topo_node_registry_store(&groups, conflict) == first);
    topo_node_registry_unref(&groups, inst.node_id);
    assert(first->v4_addrs && !strcmp(first->node_name, "old"));

    assert(topo_node_sqlite_snapshot_put(inst.topo_sqlite_db, first) == 0);
    struct TOPO_NODE* loaded = topo_node_sqlite_snapshot_load(inst.topo_sqlite_db, &groups, inst.node_id);
    assert(loaded && loaded->timestamp == 100 && loaded->v4_addrs && topo_node_verify(loaded) == 0);
    assert(!strcmp(loaded->connection_order, "RUT2-1"));
    char display_order[256];
    assert(!topo_node_sqlite_get_connection_order(inst.topo_sqlite_db, inst.my_keys.public_key, display_order));
    assert(!strcmp(display_order, "RUT2-1"));
    assert(!topo_node_sqlite_get_connection_order(inst.topo_sqlite_db, (const uint8_t[32]){0}, display_order) && !display_order[0]);

    strcpy(loaded->connection_order, "TUR3");
    assert(topo_node_verify(loaded) < 0);
    topo_node_destroy(&groups, loaded);

    struct TOPO_NODE* fresh = record(&inst, 101, "new", 0);
    assert(topo_node_registry_store(&groups, fresh) == first);
    topo_node_registry_unref(&groups, inst.node_id);
    assert(first->timestamp == 101 && !first->v4_addrs && !strcmp(first->node_name, "new"));
    assert(topo_node_verify(first) == 0); /* удаление адресов не оставляет частей старой записи */
    assert(topo_node_sqlite_snapshot_put(inst.topo_sqlite_db, first) == 0);
    loaded = record(&inst, 100, "old", 1);
    assert(topo_node_sqlite_snapshot_put(inst.topo_sqlite_db, loaded) == 0);
    topo_node_destroy(&groups, loaded);
    loaded = topo_node_sqlite_snapshot_load(inst.topo_sqlite_db, &groups, inst.node_id);
    assert(loaded && loaded->timestamp == 101 && !loaded->v4_addrs && topo_node_verify(loaded) == 0);
    topo_node_destroy(&groups, loaded);

    /* Сохранённая версия из будущего: локальные часы/перезапуск не откатывают timestamp. */
    topo_node_registry_unref(&groups, inst.node_id);
    first = topo_node_registry_store(&groups, record(&inst, 99, "stale after restart", 1));
    assert(first && first->timestamp == 101 && !first->v4_addrs && topo_node_verify(first) == 0);
    first->timestamp = INT64_MAX - 10;
    sign_record(&inst, first);
    assert(topo_node_sqlite_snapshot_put(inst.topo_sqlite_db, first) == 0);
    first->timestamp = 0;
    assert(topo_node_sign_self(&inst, first) == 0);
    assert(first->timestamp == INT64_MAX - 9 && topo_node_verify(first) == 0);
    assert(topo_node_sign_self(&inst, first) == 0);
    assert(first->timestamp == INT64_MAX - 8 && topo_node_verify(first) == 0);
    test_bgp(&inst);
    uint64_t before = first->timestamp;
    assert(sqlite3_exec(inst.topo_sqlite_db,
        "CREATE TRIGGER fail_snapshot BEFORE UPDATE ON node_snapshots BEGIN SELECT RAISE(ABORT,'test write failure'); END",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(topo_node_sign_self(&inst, first) < 0);
    assert(first->timestamp == before && topo_node_verify(first) == 0);

    topo_node_registry_unref(&groups, inst.node_id);
    assert(!groups.node_registry->head);
    queue_free(groups.node_registry);
    memory_pool_destroy(groups.v4_addr_pool);
    memory_pool_destroy(groups.v4_sock_meta_pool);
    memory_pool_destroy(groups.v6_sock_meta_pool);
    memory_pool_destroy(groups.v6_addr_pool);
    memory_pool_destroy(groups.v4_subnet_pool);
    memory_pool_destroy(groups.v6_subnet_pool);
    sqlite3_close(inst.topo_sqlite_db);
    uasync_destroy(inst.ua, 0);
    return 0;
}
