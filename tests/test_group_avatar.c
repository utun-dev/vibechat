/* Настоящие ядра/UDP/router: подпись группы, четыре блока, backpressure и restart. */
#include "../src/chat/group_avatar.c"
#include "../src/chat/member_sync.h"
#include "test_utils.h"
#include "test_net.h"
#include "test_membership.h"
#include <assert.h>

#define GROUP 6767676767ULL
static char CHANNEL[65];
static struct UTUN_INSTANCE* nodes[2];
static struct UASYNC* ua;
static unsigned received_blocks;
static uint64_t old_version;
static int capture_blocks;
static int cancel_on_update;
static uint8_t group_private[32], group_public[32], group_x[32];

static void hex(const uint8_t* input, char* out) { for (unsigned i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", input[i]); }
static int hold(const struct ETCP_ROUTER_CONN* route, const uint8_t* data, size_t size) {
    (void)route; (void)data; (void)size; return 0;
}
static int hold_after_first(const struct ETCP_ROUTER_CONN* route, const uint8_t* data, size_t size) {
    if (size >= GA_DATA_HEADER && data[1] == GA_DATA && (data[108] || data[109])) return 0;
    return ga_receive_ready(route, data, size);
}
static void capture(int level, const char* category, const char* message) {
    (void)level; (void)category;
    if (!capture_blocks || !strstr(message, "group avatar: block received")) return;
    received_blocks++;
    struct group_avatar_record current; assert(!group_avatar_get(nodes[1], GROUP, &current));
    assert(current.version == old_version); // Даже последний блок ещё не опубликован до проверки всей записи.
}
static void cancel_received_avatar(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    (void)data; (void)size;
    if (type != CHAT_EVT_CHANNEL_UPDATED || !cancel_on_update) return;
    assert(inst == nodes[1] && sqlite3_get_autocommit(inst->topo_sqlite_db));
    cancel_on_update = 0; group_avatar_cancel_group(inst, GROUP);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "test group avatar: cancelled all peer tasks from committed receive event");
}
static void put_member(unsigned destination, unsigned member) {
    uint8_t join[64], tree[64], message[512];
    int n = member_sync_build_join_msg(group_x, group_public, nodes[member]->my_ed25519_pubkey,
        nodes[member]->my_keys.public_key, message, sizeof(message)); assert(n > 0);
    assert(sc_ed25519_sign(nodes[member]->my_ed25519_privkey, message, n, join) == SC_OK);
    assert(member_sync_sign_join(group_private, CHANNEL, NULL, nodes[member]->my_keys.public_key, nodes[member]->my_ed25519_pubkey,
        TEST_JOIN_ID(group_x, group_public, nodes[member]->my_ed25519_pubkey, nodes[member]->my_keys.public_key), 123, tree) == 0);
    assert(member_sync_put(nodes[destination], CHANNEL, nodes[member]->my_keys.public_key,
        nodes[member]->my_ed25519_pubkey, join, 123, NULL, 0, "", NULL, NULL, 0, 0, tree) >= 0);
}
static int wait_record(unsigned node, uint64_t version, const uint8_t hash[32]) {
    uint64_t deadline = get_time_tb() + 150000;
    while (get_time_tb() < deadline) {
        struct group_avatar_record r;
        if (!group_avatar_get(nodes[node], GROUP, &r) && r.version == version && !memcmp(r.hash, hash, 32)) return 1;
        uasync_poll(ua, 2);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "test group avatar: sync timeout node=%u version=%llu", node, (unsigned long long)version); return 0;
}
static void set_key(unsigned node, const uint8_t* key) {
    sqlite3_stmt* st; assert(sqlite3_prepare_v2(nodes[node]->topo_sqlite_db,
        "UPDATE channels SET ed25519_privkey=? WHERE channel_id=?", -1, &st, NULL) == SQLITE_OK);
    if (key) sqlite3_bind_blob(st, 1, key, 32, SQLITE_STATIC); else sqlite3_bind_null(st, 1);
    sqlite3_bind_text(st, 2, CHANNEL, -1, SQLITE_STATIC); assert(sqlite3_step(st) == SQLITE_DONE); sqlite3_finalize(st);
}

static void schema_test(void) {
    sqlite3* db; assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE channels(channel_id TEXT PRIMARY KEY,name TEXT,ed25519_privkey BLOB);"
        "INSERT INTO channels VALUES('42','retained',zeroblob(32))", NULL, NULL, NULL) == SQLITE_OK);
    assert(ga_schema(db) < 0 && sqlite3_get_autocommit(db));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT avatar_version FROM channels", -1, &st, NULL) != SQLITE_OK); sqlite3_finalize(st);
    assert(ga_schema(db) < 0);
    assert(sqlite3_prepare_v2(db, "SELECT name,length(ed25519_privkey) FROM channels", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), "retained") &&
        sqlite3_column_int(st, 1) == 32); sqlite3_finalize(st);
    assert(sqlite3_close(db) == SQLITE_OK);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG); debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
    schema_test();
    utun_instance_set_tun_init_enabled(0); ua = uasync_create(); assert(ua);
    struct SC_MYKEYS keys[2]; uint8_t x_private[32];
    assert(sc_identity_generate(group_x, x_private, group_public, group_private) == SC_OK);
    uint8_t seeds[2][32];
    for (unsigned i = 0; i < 2; i++) assert(!sc_identity_generate_config(keys[i].public_key, seeds[i]));
    char directory[512] = "/tmp/utun-group-avatar-XXXXXX"; assert(!test_mkdtemp(directory));
    for (unsigned i = 0; i < 2; i++) {
        char pub[65], priv[65], peer[65], config[2048]; hex(keys[i].public_key, pub); hex(seeds[i], priv); hex(keys[1-i].public_key, peer);
        snprintf(config, sizeof(config), "[global]\nmy_private_key=%s\nmy_public_key=%s\ndb_path=%s/node%u\n"
            "[server:s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n"
            "[client:peer]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", priv, pub, directory, i, peer);
        nodes[i] = utun_instance_create_from_str(ua, config); assert(nodes[i]);
    }
    for (unsigned i = 0; i < 2; i++) {
        test_etcp_set_peer(nodes[i], "peer", nodes[1-i], "s1");
        assert(!utun_instance_init(nodes[i]));
    }
    uint8_t signature[64] = {0};
    sc_public_key_hex(group_public, CHANNEL);
    assert(!test_group_signature(CHANNEL, "avatar", nodes[0]->my_ed25519_pubkey, group_x, group_public, group_private, signature));
    struct TOPO_PEER_REQUEST* connections[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        assert(!topo_node_sqlite_channel_put(nodes[i]->topo_sqlite_db, CHANNEL, "avatar", nodes[0]->my_ed25519_pubkey,
            group_x, NULL, group_public, i ? NULL : group_private, signature));
        assert(topo_groups_create_group(nodes[i]->topo_groups, GROUP, TOPO_GROUP_TYPE_CHAT, CHANNEL));
        assert(!chat_core_ensure_channel_ready(nodes[i], CHANNEL));
        put_member(i, 0); put_member(i, 1);
        assert(!topo_group_peer_open(topo_groups_find(nodes[i]->topo_groups, GROUP), nodes[1-i]->node_id, &connections[i]));
    }
    uint64_t deadline = get_time_tb() + 150000;
    while ((topo_group_peer_phase(connections[0]) != TOPO_PEER_READY || topo_group_peer_phase(connections[1]) != TOPO_PEER_READY) &&
        get_time_tb() < deadline) uasync_poll(ua, 2);
    assert(topo_group_peer_phase(connections[0]) == TOPO_PEER_READY && topo_group_peer_phase(connections[1]) == TOPO_PEER_READY);
    char* renamed = NULL;
    assert(!group_info_command(nodes[0], CHANNEL, "Group renamed over UDP", NULL, -1, NULL, &renamed)); u_free(renamed);
    deadline = get_time_tb() + 150000;
    struct group_info_record info; struct group_info_profile profile;
    do {
        uasync_poll(ua, 2);
        assert(!group_info_get(nodes[1]->topo_sqlite_db, CHANNEL, &info));
        assert(!group_info_parse(nodes[1]->topo_sqlite_db, info.json, &profile));
    } while (profile.version != 2 && get_time_tb() < deadline);
    assert(profile.version == 2 && !strcmp(profile.name, "Group renamed over UDP"));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "PASS group rename delivered through real UDP/MEMBERS");
    uint8_t image[GROUP_AVATAR_MAX + 1]; for (unsigned i = 0; i < sizeof(image); i++) image[i] = (uint8_t)(i * 73 + 19);
    assert(group_avatar_set(nodes[0], GROUP, image, sizeof(image)) < 0);
    assert(group_avatar_set(nodes[1], GROUP, image, 1) < 0);
    assert(!group_avatar_set(nodes[0], GROUP, image, GROUP_AVATAR_MAX));
    struct group_avatar_record first; assert(!group_avatar_get(nodes[0], GROUP, &first));
    capture_blocks = 1; debug_set_log_hook(capture);
    assert(wait_record(1, first.version, first.hash)); capture_blocks = 0; debug_set_log_hook(NULL);
    assert(received_blocks == 4);
    struct group_avatar_record copy; assert(!group_avatar_get(nodes[1], GROUP, &copy)); assert(!memcmp(copy.data, image, GROUP_AVATAR_MAX));
    copy.signature[0] ^= 1; assert(group_avatar_apply(nodes[1], GROUP, &copy) < 0);
    copy = first; copy.data[0] ^= 1; assert(group_avatar_apply(nodes[1], GROUP, &copy) < 0);
    assert(group_avatar_apply(nodes[1], GROUP + 1, &first) < 0);

    // Повторное описание группы сохраняет ключ владельца, аватарку и UI-состояние.
    assert(!topo_node_sqlite_channel_put(nodes[0]->topo_sqlite_db, CHANNEL, "avatar", nodes[0]->my_ed25519_pubkey,
        group_x, NULL, group_public, NULL, signature));
    assert(!group_avatar_get(nodes[0], GROUP, &copy) && copy.version == first.version);
    uint8_t private_key[32]; assert(!topo_node_sqlite_channel_get_priv(nodes[0]->topo_sqlite_db, CHANNEL, private_key));
    uint8_t wrong_pub[32] = {1};
    assert(topo_node_sqlite_channel_put(nodes[0]->topo_sqlite_db, CHANNEL, "avatar", nodes[0]->my_ed25519_pubkey,
        group_x, NULL, wrong_pub, NULL, signature) < 0);
    assert(!group_avatar_get(nodes[0], GROUP, &copy) && copy.version == first.version);

    etcp_router_set_receive_ready(nodes[1], ETCP_RT_ID_GROUP_AVATAR, hold);
    image[0] ^= 1; assert(!group_avatar_set(nodes[0], GROUP, image, 4097));
    struct group_avatar_record next; assert(!group_avatar_get(nodes[0], GROUP, &next));
    // Проверяем остановку доставки и повтор BUSY ACK, затем сразу переходим к restart.
    struct ETCP_ROUTER_CONN* sender = etcp_router_conn_get(nodes[0], GROUP, nodes[1]->node_id, ETCP_RT_ID_GROUP_AVATAR);
    struct ETCP_ROUTER_CONN* receiver = etcp_router_conn_get(nodes[1], GROUP, nodes[0]->node_id, ETCP_RT_ID_GROUP_AVATAR);
    uint64_t hold_started = get_time_tb();
    uint32_t first_busy_ack = 0;
    int busy_repeated = 0;
    deadline = hold_started + 20000;
    while (get_time_tb() < deadline) {
        uasync_poll(ua, 2);
        if (!receiver->receive_blocked || !sender->peer_receive_blocked) continue;
        if (!first_busy_ack) first_busy_ack = sender->c_stale_ack;
        else if (sender->c_stale_ack != first_busy_ack) { busy_repeated = 1; break; }
    }
    if (!busy_repeated) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "test group avatar: BUSY ACK repeat missing blocked=%u peer_blocked=%u",
                                    receiver->receive_blocked, sender->peer_receive_blocked);
    assert(busy_repeated && sender->tx_acked != sender->tx_sent);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "test group avatar: delivery paused, BUSY ACK repeated elapsed_tb=%llu acked=%u sent=%u",
               (unsigned long long)(get_time_tb() - hold_started), sender->tx_acked, sender->tx_sent);
    assert(!group_avatar_get(nodes[1], GROUP, &copy) && copy.version == first.version);
    etcp_router_conn_restart(nodes[0], GROUP, nodes[1]->node_id, ETCP_RT_ID_GROUP_AVATAR);
    etcp_router_set_receive_ready(nodes[1], ETCP_RT_ID_GROUP_AVATAR, ga_receive_ready);
    etcp_router_resume_receive(nodes[1], GROUP, nodes[0]->node_id, ETCP_RT_ID_GROUP_AVATAR);
    assert(wait_record(1, next.version, next.hash)); assert(group_avatar_apply(nodes[1], GROUP, &first) == 0);

    // Держатель ключа, который не является создателем группы, тоже может обновить картинку.
    set_key(1, group_private); assert(!group_avatar_set(nodes[1], GROUP, image, 1)); set_key(1, NULL);
    assert(!group_avatar_get(nodes[1], GROUP, &next)); assert(wait_record(0, next.version, next.hash));
    copy = next; copy.data[0]++;
    SHA256(copy.data, copy.size, copy.hash); uint8_t signed_bytes[GROUP_AVATAR_MAX + 64];
    assert(sc_ed25519_sign(group_private, signed_bytes, ga_message(&copy, signed_bytes), copy.signature) == SC_OK);
    int accepted = ga_compare(copy.version, copy.hash, next.version, next.hash) > 0;
    assert(group_avatar_apply(nodes[0], GROUP, &copy) == accepted);
    // Событие вправе удалить peer-контекст: приём не обращается к нему после callback.
    chat_event_set_handler(nodes[1], cancel_received_avatar); cancel_on_update = 1;
    image[1] ^= 1; assert(!group_avatar_set(nodes[0], GROUP, image, 17));
    assert(!group_avatar_get(nodes[0], GROUP, &next) && wait_record(1, next.version, next.hash) && !cancel_on_update);
    chat_event_set_handler(nodes[1], NULL);
    assert(!group_avatar_set(nodes[0], GROUP, NULL, 0)); assert(!group_avatar_get(nodes[0], GROUP, &next));
    assert(wait_record(1, next.version, next.hash)); assert(!group_avatar_get(nodes[1], GROUP, &copy) && !copy.size);
    assert(group_avatar_apply(nodes[1], GROUP, &first) == 0);

    // Первый блок уже принят, второй удерживает router. Отзыв удаляет обе очереди до публикации картинки.
    etcp_router_set_receive_ready(nodes[1], ETCP_RT_ID_GROUP_AVATAR, hold_after_first);
    assert(!group_avatar_set(nodes[0], GROUP, image, GROUP_AVATAR_MAX));
    receiver = etcp_router_conn_get(nodes[1], GROUP, nodes[0]->node_id, ETCP_RT_ID_GROUP_AVATAR); assert(receiver);
    deadline = get_time_tb() + 30000;
    while (!receiver->receive_blocked && get_time_tb() < deadline) uasync_poll(ua, 2);
    struct group_avatar* ctx = ga_context(nodes[1]);
    struct ga_peer* partial = ga_find(ctx, GROUP, nodes[0]->my_ed25519_pubkey, 0);
    assert(receiver->receive_blocked && partial && partial->rx && partial->rx_offset == GROUP_AVATAR_BLOCK && ctx->rx_count == 1);
    char sql[160]; snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=0 WHERE node_id=%lld",
                           CHANNEL, (long long)nodes[0]->node_id);
    assert(sqlite3_exec(nodes[1]->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(!test_member_publish_access(nodes[1], CHANNEL));
    topo_group_revalidate_members(topo_groups_find(nodes[1]->topo_groups, GROUP));
    assert(!ga_find(ctx, GROUP, nodes[0]->my_ed25519_pubkey, 0) && !ctx->rx_count && !ctx->tx_count);
    assert(!etcp_router_conn_get(nodes[1], GROUP, nodes[0]->node_id, ETCP_RT_ID_GROUP_AVATAR));
    etcp_router_set_receive_ready(nodes[1], ETCP_RT_ID_GROUP_AVATAR, ga_receive_ready);
    for (unsigned i = 0; i < 20; i++) uasync_poll(ua, 1);
    assert(!group_avatar_get(nodes[1], GROUP, &copy) && copy.version == next.version && !copy.size);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "test group avatar: revoke discarded partial image and router backlog; stored version unchanged");

    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=1", CHANNEL);
    assert(sqlite3_exec(nodes[1]->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(!test_member_publish_access(nodes[1], CHANNEL));
    for (unsigned i = 0; i < 2; i++) {
        topo_group_peer_close(connections[i]); connections[i] = NULL;
        assert(!topo_group_peer_open(topo_groups_find(nodes[i]->topo_groups, GROUP), nodes[1-i]->node_id, &connections[i]));
    }
    deadline = get_time_tb() + 150000;
    while ((topo_group_peer_phase(connections[0]) != TOPO_PEER_READY || topo_group_peer_phase(connections[1]) != TOPO_PEER_READY) &&
        get_time_tb() < deadline) uasync_poll(ua, 2);
    assert(topo_group_peer_phase(connections[0]) == TOPO_PEER_READY && topo_group_peer_phase(connections[1]) == TOPO_PEER_READY);
    assert(!group_avatar_set(nodes[0], GROUP, NULL, 0) && !group_avatar_get(nodes[0], GROUP, &next));
    assert(wait_record(1, next.version, next.hash));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "test group avatar: readmission restores exchange with a new signed version");

    topo_group_peer_close(connections[0]); topo_group_peer_close(connections[1]);
    utun_instance_destroy(nodes[1]); utun_instance_destroy(nodes[0]);
    char config[1024]; snprintf(config, sizeof(config), "[global]\ndb_path=%s/node0\n[chatserver]\nmember_files_enabled=0\n", directory);
    nodes[0] = utun_instance_create_from_str(ua, config); assert(nodes[0]);
    assert(!utun_core_start(nodes[0]) && !chat_service_start(nodes[0]));
    uint64_t restored = topo_groups_channel_slot(nodes[0]->topo_groups, CHANNEL); assert(restored);
    assert(!group_avatar_get(nodes[0], restored, &copy) && copy.version == next.version && !copy.size);
    assert(!group_info_get(nodes[0]->topo_sqlite_db, CHANNEL, &info) &&
        !group_info_parse(nodes[0]->topo_sqlite_db, info.json, &profile) && profile.version == 2 &&
        !strcmp(profile.name, "Group renamed over UDP"));
    utun_instance_destroy(nodes[0]); uasync_destroy(ua, 1);
    puts("group avatar: PASS limits, signatures, four 4KiB blocks, atomicity, restart, key holders and tombstones"); return 0;
}
