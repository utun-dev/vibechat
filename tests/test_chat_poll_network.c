/* Три ядра и UDP: A ↔ B ↔ C, голосование/реакции и возврат C после остановки чата. */
#include "test_join_id.h"
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "chat/chat_poll.h"
#include "chat/chat_feedback.h"
#include "chat/chat_log.h"
#include "chat/group_policy.h"
#include "chat/member_sync.h"
#include "chat/merkle_sync.h"
#include "chat/db_sync.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "transport_layer/node_conn_direct.h"
#include "transport_layer/etcp_connections.h"
#include "transport_layer/etcp.h"
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "../lib/debug_config.h"
#include "test_net.h"
#include "test_utils.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct UASYNC* ua;
static struct UTUN_INSTANCE* nodes[4];
static char channel[65], message_hex[65];
static uint8_t message_id[32];

static void hex(const uint8_t* data, size_t len, char* out) {
    for (size_t i = 0; i < len; i++) snprintf(out + i * 2, 3, "%02x", data[i]);
}

static struct UTUN_INSTANCE* create_node(unsigned port) {
    char directory[]="/tmp/utun-poll-network-XXXXXX"; assert(!test_mkdtemp(directory));
    uint8_t device_x[32], device_seed[32]; assert(sc_identity_generate_config(device_x, device_seed) == SC_OK);
    char private_key[65], public_key[65], config[1024]; hex(device_seed, 32, private_key); hex(device_x, 32, public_key);
    snprintf(config, sizeof(config),
        "[global]\ndb_path=%s\nmy_private_key=%s\nmy_public_key=%s\nkeepalive_adaptive=0\n"
        "[server: udp]\naddr=127.0.0.1:%u\ntype=public\n[allowed_keys]\nallow_all=1\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory, private_key, public_key, port);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && utun_core_start(inst) == 0 && chat_service_start(inst) == 0); return inst;
}

/* Все три заранее принятых участника имеют настоящие подписи root/join. */
static void seed_members(uint8_t channel_x[32], uint8_t channel_ed[32], uint8_t owner_private[32], int count) {
    for (int author = 0; author < count; author++) {
        uint8_t body[MEMBER_JOIN_MSG_SIZE], join[64], tree[64];
        char sql[160]; sqlite3_stmt* st = NULL; uint64_t join_ts = 1;
        snprintf(sql, sizeof(sql), "SELECT join_sig,join_ts FROM peers_%s WHERE node_id=? AND record_state=0", channel);
        assert(sqlite3_prepare_v2(nodes[author]->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)nodes[author]->node_id);
        int rc = sqlite3_step(st); assert(rc == SQLITE_ROW || rc == SQLITE_DONE);
        if (rc == SQLITE_ROW) {
            assert(sqlite3_column_bytes(st, 0) == 64); memcpy(join, sqlite3_column_blob(st, 0), 64);
            join_ts = (uint64_t)sqlite3_column_int64(st, 1);
        } else {
            assert(member_sync_build_join_msg(channel_x, channel_ed, nodes[author]->my_ed25519_pubkey,
                nodes[author]->my_keys.public_key, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
            assert(sc_ed25519_sign(nodes[author]->my_ed25519_privkey, body, sizeof(body), join) == SC_OK);
        }
        sqlite3_finalize(st);
        assert(member_sync_sign_join(owner_private, channel, NULL,
            nodes[author]->my_keys.public_key, nodes[author]->my_ed25519_pubkey,
                TEST_JOIN_ID(channel_x, channel_ed, nodes[author]->my_ed25519_pubkey, nodes[author]->my_keys.public_key), join_ts, tree) == 0);
        for (int dst = 0; dst < count; dst++) {
            assert(member_sync_put(nodes[dst], channel, nodes[author]->my_keys.public_key,
                nodes[author]->my_ed25519_pubkey, join, join_ts, NULL, 0, "", NULL, NULL, 0, 0, tree) >= 0);
        }
    }
}

/* Поднимаем транспорт только соседних пар; C получает данные A через B. */
static struct NODE_CONN_DIRECT* connect_node(int from, int to, unsigned port) {
    struct TOPO_ADDR4 address = { .addr = {127, 0, 0, 1}, .port = port, .protocol = TOPO_PROTO_UDP };
    struct TOPO_NODE node = { .node_id = nodes[to]->node_id, .v4_addrs = &address };
    memcpy(node.public_key, nodes[to]->my_keys.public_key, 32); struct NODE_CONN_DIRECT* handle = NULL;
    assert(node_conn_direct_open_node(nodes[from], node.node_id, NULL, NULL, &handle, &node, NULL) != NCD_ERR); return handle;
}

static int wait_ready(struct TOPO_PEER_REQUEST* handle) {
    uint64_t deadline = get_time_tb() + 15u * 10000u;
    while (get_time_tb() < deadline) {
        if (topo_group_peer_phase(handle) == TOPO_PEER_READY) return 1;
        uasync_poll(ua, 5);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll network: group readiness timeout phase=%d", topo_group_peer_phase(handle)); return 0;
}

/* C открывает групповой запрос после входящего UP; до него адрес B ещё не получен. */
static int wait_transport(struct NODE_CONN_DIRECT* ab, struct NODE_CONN_DIRECT* bc) {
    struct ETCP_CONN *a = NULL, *b = NULL;
    uint64_t deadline = get_time_tb() + 15u * 10000u;
    while (get_time_tb() < deadline) {
        a = node_conn_direct_get_conn(ab); b = node_conn_direct_get_conn(bc);
        struct ETCP_CONN* c = instance_find_conn(nodes[2], nodes[1]->node_id);
        if (a && b && c && a->initialized && b->initialized && c->initialized
            && a->links_up && b->links_up && c->links_up) return 1;
        uasync_poll(ua, 5);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll network: bootstrap timeout ab=%p bc=%p", (void*)a, (void*)b); return 0;
}

/* Новый узел должен получить входящий UP до группового запроса без bootstrap-адресов. */
static int wait_new_transport(int from, int to, struct NODE_CONN_DIRECT* handle) {
    uint64_t deadline = get_time_tb() + 15u * 10000u;
    while (get_time_tb() < deadline) {
        struct ETCP_CONN* outgoing = node_conn_direct_get_conn(handle);
        struct ETCP_CONN* incoming = instance_find_conn(nodes[to], nodes[from]->node_id);
        if (outgoing && incoming && outgoing->initialized && incoming->initialized && outgoing->links_up && incoming->links_up) return 1;
        uasync_poll(ua, 5);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll network: new transport timeout from=%d to=%d", from, to); return 0;
}

static sqlite3_int64 field(struct UTUN_INSTANCE* inst, const char* json, const char* path) {
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT json_extract(?1,?2)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW); sqlite3_int64 n = sqlite3_column_int64(st, 0); sqlite3_finalize(st); return n;
}

static int wait_result(int index, int voters, int option_one, int reactions) {
    uint64_t deadline = get_time_tb() + 15u * 10000u;
    while (get_time_tb() < deadline) {
        char* result = NULL;
        if (chat_feedback_snapshot(nodes[index]->topo_sqlite_db, nodes[index]->my_ed25519_pubkey, channel, message_id, 0, &result) == 0) {
            int ok = !field(nodes[index], result, "$.pending") && field(nodes[index], result, "$.poll.voters") == voters
                && field(nodes[index], result, "$.poll.options[1].count") == option_one
                && field(nodes[index], result, "$.reactions[0].count") == reactions;
            u_free(result); if (ok) return 1;
        }
        uasync_poll(ua, 5);
    }
    char* result = NULL; chat_feedback_snapshot(nodes[index]->topo_sqlite_db, nodes[index]->my_ed25519_pubkey, channel, message_id, 0, &result);
    sqlite3_stmt* diagnostic=NULL; char diagnostic_sql[256];
    snprintf(diagnostic_sql,sizeof(diagnostic_sql),"SELECT node_id,admitted,adm_tags FROM peers_%s WHERE record_state=0",channel);
    assert(!sqlite3_prepare_v2(nodes[index]->topo_sqlite_db,diagnostic_sql,-1,&diagnostic,NULL));
    while(sqlite3_step(diagnostic)==SQLITE_ROW) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,
        "poll network member node=%d member=%016llx admitted=%d tags=%s",index,
        (unsigned long long)sqlite3_column_int64(diagnostic,0),sqlite3_column_int(diagnostic,1),sqlite3_column_text(diagnostic,2));
    assert(!sqlite3_finalize(diagnostic));
    assert(!sqlite3_prepare_v2(nodes[index]->topo_sqlite_db,
        "SELECT hex(id),hex(substr(wire,34,32)) FROM chat_seals WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?)",-1,&diagnostic,NULL));
    sqlite3_bind_text(diagnostic,1,channel,-1,SQLITE_STATIC);
    while(sqlite3_step(diagnostic)==SQLITE_ROW) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,"poll network seal node=%d id=%s grant=%s",index,
        sqlite3_column_text(diagnostic,0),sqlite3_column_text(diagnostic,1));
    assert(!sqlite3_finalize(diagnostic));
    for(int n=0;n<4;n++) if(nodes[n]) {
        assert(!sqlite3_prepare_v2(nodes[n]->topo_sqlite_db,
            "SELECT (SELECT count(*) FROM chat_heads WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)),(SELECT max(seq) FROM member_log WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)),"
            "(SELECT max(head_seq) FROM member_chains WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1))",-1,&diagnostic,NULL));
        sqlite3_bind_text(diagnostic,1,channel,-1,SQLITE_STATIC); assert(sqlite3_step(diagnostic)==SQLITE_ROW);
        uint8_t root[32]; assert(!merkle_sync_read_hash(nodes[n],MT_DATA_CHAT_LOG,channel,0,0,root));
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,"poll journal node=%d rows=%d wire_seq=%lld accepted_seq=%lld root=%02x%02x%02x%02x",n,
            sqlite3_column_int(diagnostic,0),(long long)sqlite3_column_int64(diagnostic,1),(long long)sqlite3_column_int64(diagnostic,2),root[0],root[1],root[2],root[3]);
        assert(!sqlite3_finalize(diagnostic));
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll network: result timeout node=%d expected voters=%d option1=%d reactions=%d actual=%s",
        index, voters, option_one, reactions, result ? result : "missing"); u_free(result); return 0;
}

static void run_command(int index, const char* command, const char* tail) {
    char request[1024]; snprintf(request, sizeof(request), "{\"cmd\":\"%s\",\"ch\":\"%s\",\"message_id\":\"%s\"%s}",
        command, channel, message_hex, tail);
    char* result = NULL; int rc = chat_feedback_command(nodes[index], request, &result);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll network: command failed node=%d request=%s result=%s",
        index, request, result ? result : "missing");
    assert(rc == 0); u_free(result);
}

static void subscribe(int a, int b) {
    assert(member_sync_start(nodes[a], instance_find_conn_by_key(nodes[a], nodes[b]->my_keys.public_key), channel, NULL, NULL) == 0);
    assert(merkle_sync_start(nodes[a], MT_DATA_CHAT_LOG, instance_find_conn_by_key(nodes[a], nodes[b]->my_keys.public_key), channel, NULL, NULL) == 0);
    db_sync_instance_set_gated(db_sync_instance_find(nodes[a], topo_groups_channel_slot(nodes[0]->topo_groups, channel)), 0);
    db_sync_instance_set_gated(db_sync_instance_find(nodes[b], topo_groups_channel_slot(nodes[0]->topo_groups, channel)), 0);
    db_sync_reinitiate(db_sync_instance_find(nodes[a], topo_groups_channel_slot(nodes[0]->topo_groups, channel)), nodes[b]->my_ed25519_pubkey);
}

/* Владелец подписывает исключение/возврат; распространяется обычным member-sync. */
static void owner_deleted(int dst, int author, unsigned ver, int deleted, const uint8_t owner_private[32]) {
    static char seal[65]; char tags[256]; uint8_t body[320], signature[64];
    if(deleted) {
        assert(!sqlite3_exec(nodes[dst]->topo_sqlite_db,"BEGIN IMMEDIATE /* revoke checkpoint */",NULL,NULL,NULL));
        assert(!chat_log_seal(nodes[dst]->topo_sqlite_db,channel,nodes[author]->my_ed25519_pubkey,seal));
        assert(!sqlite3_exec(nodes[dst]->topo_sqlite_db,"COMMIT /* revoke checkpoint */",NULL,NULL,NULL));
        chat_log_changed(nodes[dst],channel);
    }
    snprintf(tags,sizeof(tags),"{\"ver\":\"%u\",\"deleted\":\"%s\",\"_message_epoch\":\"1\",\"_chat_seal\":\"%s\"}",
        ver,deleted?"yes":"no",seal);
    int len = member_sync_build_owner_msg(channel, nodes[author]->my_ed25519_pubkey, tags, body, sizeof(body)); assert(len > 0);
    assert(sc_ed25519_sign(owner_private, body, len, signature) == SC_OK);
    assert(member_sync_put(nodes[dst], channel, nodes[author]->my_keys.public_key,
        nodes[author]->my_ed25519_pubkey, NULL, 0, NULL, 0, NULL, tags, signature, 0, 0, NULL) & MS_APPLY_CHANGED);
}

static int wait_feedback_roots(int a, int b) {
    uint64_t deadline = get_time_tb() + 15u * 10000u; uint8_t left[32], right[32];
    while (get_time_tb() < deadline) {
        assert(merkle_sync_read_hash(nodes[a], MT_DATA_CHAT_LOG, channel, 0, 0, left) == 0);
        assert(merkle_sync_read_hash(nodes[b], MT_DATA_CHAT_LOG, channel, 0, 0, right) == 0);
        if (!memcmp(left, right, 32)) return 1;
        uasync_poll(ua, 5);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll network: feedback roots differ nodes=%d/%d", a, b); return 0;
}

static int future_received;
static void clock_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    if (inst == nodes[2] && type == CHAT_EVT_MSG_RECEIVED && size == 65 + CHAT_MSG_EVENT_SUFFIX_SIZE &&
        !memcmp(data + 1, channel, 64) && !memcmp(data + 65, nodes[1]->my_ed25519_pubkey, 32)) future_received = 1;
}
static void journal_message_at(struct UTUN_INSTANCE* inst, uint64_t timestamp) {
    static const char text[] = "{\"ct\":\"text/plain\",\"d\":\"clock fixture\"}";
    struct db_sync_record record = {.timestamp = timestamp, .data = text, .size = sizeof(text) - 1};
    memcpy(record.author_ed, inst->my_ed25519_pubkey, 32);
    record.revision.nonce[0] = 1;
    sqlite3* db = inst->topo_sqlite_db;
    assert(!sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    assert(!group_policy_key_generation(db, channel, record.author_ed, record.revision.generation));
    int history_changed;
    assert(!chat_log_message(db, channel, &record, inst->my_ed25519_privkey, &history_changed));
    assert(!member_log_commit(db));
    chat_log_local_committed(inst, channel, record.revision.ver, history_changed);
}
static uint64_t send_clock_message(struct UTUN_INSTANCE* inst) {
    struct chat_msg_submit req = {.data = (uint8_t*)"local clock", .data_len = 11};
    strcpy(req.channel_id, channel);
    strcpy(req.content_type, "text/plain");
    assert(!chat_core_submit_message(inst, &req));
    return req.timestamp;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, getenv("UTUN_TEST_DEBUG") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_INFO);
    ua = uasync_create(); assert(ua); unsigned ports[4];
    for (int i = 0; i < 3; i++) {
        nodes[i] = create_node(0);
        ports[i] = ss_get_port(&test_etcp_bound_socket(nodes[i], "udp")->local_addr);
    }
    chat_core_create_channel_auto(nodes[0], "Poll network");
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(nodes[0]->topo_sqlite_db, "SELECT channel_id FROM channels", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(st, 0)); sqlite3_finalize(st);
    uint8_t x[32], ed[32], signature[64], owner_private[32];
    assert(topo_node_sqlite_channel_get(nodes[0]->topo_sqlite_db, channel, NULL, 0, NULL, x, ed, signature) == 0);
    assert(topo_node_sqlite_channel_get_priv(nodes[0]->topo_sqlite_db, channel, owner_private) == 0);
    for (int i = 1; i < 3; i++) {
        assert(topo_node_sqlite_channel_put(nodes[i]->topo_sqlite_db, channel, "Poll network", nodes[0]->my_ed25519_pubkey,
            x, NULL, ed, i==1?owner_private:NULL, signature) == 0); chat_core_ensure_channel_ready(nodes[i], channel);
    }
    seed_members(x, ed, owner_private, 3);
    struct NODE_CONN_DIRECT* ab = connect_node(0, 1, ports[1]), *bc = connect_node(1, 2, ports[2]);
    assert(wait_transport(ab, bc));
    struct TOPO_PEER_REQUEST *groups[5] = {0}; uint64_t gid = topo_groups_channel_slot(nodes[0]->topo_groups, channel);
    assert(topo_group_peer_open(topo_groups_find(nodes[0]->topo_groups, gid), nodes[1]->node_id, &groups[0]) == 0);
    assert(topo_group_peer_open(topo_groups_find(nodes[1]->topo_groups, gid), nodes[2]->node_id, &groups[1]) == 0);
    assert(topo_group_peer_open(topo_groups_find(nodes[2]->topo_groups, gid), nodes[1]->node_id, &groups[2]) == 0);
    for (int i = 0; i < 3; i++) assert(wait_ready(groups[i]));
    subscribe(0, 1); subscribe(1, 2);
    struct member_log_head initial_a, initial_b;
    assert(!member_log_head(nodes[0]->topo_sqlite_db, channel, nodes[0]->my_ed25519_pubkey, &initial_a));
    assert(!member_log_head(nodes[1]->topo_sqlite_db, channel, nodes[1]->my_ed25519_pubkey, &initial_b));
    struct chat_poll poll = { .mode = CHAT_POLL_MULTIPLE, .allow_custom_answer = 1, .option_count = 2 };
    strcpy(poll.question, "Сетевой опрос"); strcpy(poll.options[0], "Первый"); strcpy(poll.options[1], "Второй");
    /* Отзываем голосующего A; автор опроса B остаётся допущен, поэтому сам опрос продолжает синхронизацию. */
    assert(chat_poll_create(nodes[1], channel, &poll, message_id) == 0); hex(message_id, 32, message_hex);
    assert(wait_result(0, 0, 0, 0) && wait_result(2, 0, 0, 0));
    run_command(0, "vote", ",\"choices\":[0]"); run_command(1, "vote", ",\"choices\":[1]");
    run_command(2, "vote", ",\"custom\":\"Ответ C 👍\""); run_command(0, "react", ",\"emoji\":0,\"on\":true");
    assert(wait_result(0, 3, 1, 1) && wait_result(2, 3, 1, 1));
    puts("PASS: signed poll, votes, custom Unicode answer and reactions converge through the intermediate node");
    topo_group_peer_close(groups[2]); groups[2] = NULL; chat_service_stop(nodes[2]);
    run_command(0, "vote", ",\"choices\":[1]"); run_command(1, "vote", ",\"choices\":[]");
    run_command(0, "react", ",\"clear\":true"); assert(wait_result(1, 2, 1, 0));
    assert(chat_service_start(nodes[2]) == 0);
    assert(topo_group_peer_open(topo_groups_find(nodes[2]->topo_groups, gid), nodes[1]->node_id, &groups[2]) == 0);
    assert(wait_ready(groups[2])); subscribe(1, 2);
    assert(wait_result(2, 2, 1, 0) && wait_result(0, 2, 1, 0));
    struct chat_feedback_record old;
    assert(chat_feedback_get(nodes[2]->topo_sqlite_db, channel, message_id, nodes[0]->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &old) == 1);
    assert(old.ver == initial_a.seq + 4 && old.data_len == 0);
    puts("PASS: returning peer receives new versions; withdrawn votes and reactions stay withdrawn");

    run_command(0, "react", ",\"emoji\":0,\"on\":true");
    assert(wait_result(1, 2, 1, 1) && wait_result(2, 2, 1, 1));
    struct chat_feedback_record saved_reaction;
    assert(chat_feedback_get(nodes[2]->topo_sqlite_db, channel, message_id, nodes[0]->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &saved_reaction) == 1);
    assert(saved_reaction.ver == initial_a.seq + 5 && saved_reaction.data_len == 2);
    owner_deleted(1, 0, 1, 1, owner_private);
    uint64_t revoke_until = get_time_tb() + 15u * 10000u;
    while (topo_node_sqlite_member_allowed(nodes[0]->topo_sqlite_db, channel, nodes[0]->node_id) && get_time_tb() < revoke_until)
        uasync_poll(ua, 5);
    assert(!topo_node_sqlite_member_allowed(nodes[0]->topo_sqlite_db, channel, nodes[0]->node_id));
    struct ETCP_CONN* denied_conn = instance_find_conn_by_key(nodes[1], nodes[0]->my_keys.public_key);
    assert(denied_conn && denied_conn->links_up && member_sync_admin_allowed(nodes[1], channel, denied_conn));
    assert(!topo_group_chat_connection_allowed(topo_groups_find(nodes[1]->topo_groups, gid), denied_conn));
    assert(wait_result(1, 2, 1, 1) && wait_result(2, 2, 1, 1) && wait_feedback_roots(1, 2));
    topo_group_peer_close(groups[2]); groups[2] = NULL; chat_service_stop(nodes[2]);
    assert(chat_service_start(nodes[2]) == 0);
    assert(topo_group_peer_open(topo_groups_find(nodes[2]->topo_groups, gid), nodes[1]->node_id, &groups[2]) == 0);
    assert(wait_ready(groups[2])); subscribe(1, 2);
    assert(wait_result(2, 2, 1, 1));
    assert(chat_feedback_get(nodes[2]->topo_sqlite_db, channel, message_id, nodes[0]->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &old) == 1);
    assert(old.ver == saved_reaction.ver && !memcmp(old.signature, saved_reaction.signature, 64));
    assert(chat_feedback_get(nodes[2]->topo_sqlite_db, channel, message_id, nodes[1]->my_ed25519_pubkey, CHAT_FEEDBACK_ANSWER, &old) == 1);
    assert(old.ver == initial_b.seq + 3 && old.data_len == 0);
    puts("PASS: revoke seals votes/reactions; restart preserves shared historical prefix and cancellations");

    /* Новый D получает подтверждённую историю A уже после его исключения. */
    nodes[3] = create_node(0); ports[3] = ss_get_port(&test_etcp_bound_socket(nodes[3], "udp")->local_addr);
    assert(topo_node_sqlite_channel_put(nodes[3]->topo_sqlite_db, channel, "Poll network", nodes[0]->my_ed25519_pubkey,
        x, NULL, ed, NULL, signature) == 0); chat_core_ensure_channel_ready(nodes[3], channel);
    struct NODE_CONN_DIRECT* bd = connect_node(1, 3, ports[3]);
    assert(wait_new_transport(1, 3, bd));
    struct ETCP_CONN* unknown = instance_find_conn_by_key(nodes[1], nodes[3]->my_keys.public_key);
    assert(unknown && !member_sync_admin_allowed(nodes[1], channel, unknown));
    seed_members(x, ed, owner_private, 4);
    uint64_t admission_deadline = get_time_tb() + 150000;
    while (!topo_node_sqlite_member_allowed(nodes[1]->topo_sqlite_db, channel, nodes[3]->node_id) &&
        get_time_tb() < admission_deadline) uasync_poll(ua, 5);
    assert(topo_node_sqlite_member_allowed(nodes[1]->topo_sqlite_db, channel, nodes[3]->node_id));
    assert(topo_group_peer_open(topo_groups_find(nodes[1]->topo_groups, gid), nodes[3]->node_id, &groups[3]) == 0);
    assert(topo_group_peer_open(topo_groups_find(nodes[3]->topo_groups, gid), nodes[1]->node_id, &groups[4]) == 0);
    assert(wait_ready(groups[3]) && wait_ready(groups[4])); subscribe(1, 3);
    assert(wait_result(3, 2, 1, 1) && wait_feedback_roots(1, 3));
    assert(chat_feedback_get(nodes[3]->topo_sqlite_db, channel, message_id, nodes[0]->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &old) == 1);
    assert(chat_feedback_get(nodes[3]->topo_sqlite_db, channel, message_id, nodes[1]->my_ed25519_pubkey, CHAT_FEEDBACK_ANSWER, &old) == 1);
    assert(old.ver == initial_b.seq + 3 && old.data_len == 0);
    puts("PASS: new peer receives sealed history of the excluded author and saved cancellations");
    owner_deleted(1, 0, 2, 0, owner_private);
    assert(wait_result(2, 2, 1, 1) && wait_result(3, 2, 1, 1));
    assert(wait_feedback_roots(1, 2) && wait_feedback_roots(1, 3));
    assert(chat_feedback_get(nodes[3]->topo_sqlite_db, channel, message_id, nodes[0]->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &old) == 1);
    assert(old.ver == saved_reaction.ver && !memcmp(old.signature, saved_reaction.signature, 64));
    puts("PASS: signed restoration reactivates cached states and delivers them to the new peer with matching Merkle roots");
    /* Автор должен узнать новый допуск: ещё не получивший MEMBERS узел подпишет неприменяемый старый хвост. */
    uint8_t generation_at_admin[32],generation_at_author[32];
    assert(!group_policy_message_generation(nodes[1]->topo_sqlite_db,channel,nodes[0]->node_id,generation_at_admin));
    uint64_t restored_deadline=get_time_tb()+150000;
    do {
        assert(!group_policy_message_generation(nodes[0]->topo_sqlite_db,channel,nodes[0]->node_id,generation_at_author));
        if(!memcmp(generation_at_admin,generation_at_author,32) && topo_node_sqlite_member_allowed(nodes[0]->topo_sqlite_db,channel,nodes[0]->node_id)) break;
        uasync_poll(ua,5);
    } while(get_time_tb()<restored_deadline);
    assert(!memcmp(generation_at_admin,generation_at_author,32) && topo_node_sqlite_member_allowed(nodes[0]->topo_sqlite_db,channel,nodes[0]->node_id));
    /* Возвращённый автор должен снова передавать изменения через прежний UP-транспорт. */
    run_command(0, "react", ",\"clear\":true");
    assert(wait_result(1, 2, 1, 0) && wait_result(2, 2, 1, 0) && wait_result(3, 2, 1, 0));
    assert(wait_feedback_roots(1, 2) && wait_feedback_roots(1, 3));
    assert(chat_feedback_get(nodes[3]->topo_sqlite_db, channel, message_id, nodes[0]->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &old) == 1);
    assert(old.ver == initial_a.seq + 6 && !old.data_len);
    puts("PASS: restored author can publish a new cancellation over the surviving transport");

    chat_event_set_handler(nodes[2], clock_event);
    journal_message_at(nodes[1], INT64_MAX / 1000);
    uint64_t clock_deadline = get_time_tb() + 150000;
    while (!future_received && get_time_tb() < clock_deadline) uasync_poll(ua, 5);
    assert(future_received);
    uint64_t local_now = send_clock_message(nodes[2]);
    assert(local_now < INT64_MAX / 1000);
    uint64_t own_checkpoint = local_now + 60000;
    journal_message_at(nodes[2], own_checkpoint);
    topo_group_peer_close(groups[2]); groups[2] = NULL;
    chat_service_stop(nodes[2]);
    assert(!chat_service_start(nodes[2]));
    uint64_t after_restart = send_clock_message(nodes[2]);
    assert(after_restart > own_checkpoint && after_restart < INT64_MAX / 1000);
    puts("PASS: remote history cannot advance the local writer clock; own journal time survives restart");
    for (int i = 0; i < 5; i++) if (groups[i]) topo_group_peer_close(groups[i]);

    chat_service_stop(nodes[1]); chat_service_stop(nodes[3]);
    struct NODE_CONN_DIRECT* ac = connect_node(0, 2, ports[2]);
    assert(wait_new_transport(0, 2, ac));
    owner_deleted(0, 0, 3, 1, owner_private);
    assert(!topo_node_sqlite_member_allowed(nodes[0]->topo_sqlite_db, channel, nodes[0]->node_id));
    assert(topo_node_sqlite_member_allowed(nodes[2]->topo_sqlite_db, channel, nodes[2]->node_id));
    owner_deleted(0, 2, 1, 1, owner_private);
    revoke_until = get_time_tb() + 15u * 10000u;
    while (topo_node_sqlite_member_allowed(nodes[2]->topo_sqlite_db, channel, nodes[2]->node_id) && get_time_tb() < revoke_until)
        uasync_poll(ua, 5);
    assert(!topo_node_sqlite_member_allowed(nodes[2]->topo_sqlite_db, channel, nodes[2]->node_id));
    assert(member_sync_admin_allowed(nodes[0], channel, node_conn_direct_get_conn(ac)));
    puts("PASS: an excluded peer relays the signed revocation over MEMBER_ADMIN");
    node_conn_direct_close(ac);
    node_conn_direct_close(ab); node_conn_direct_close(bc); node_conn_direct_close(bd);
    for (int i = 0; i < 4; i++) utun_instance_destroy(nodes[i]);
    uasync_poll(ua, 0); assert(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0); puts("ALL PASS: network polls and offline recovery"); return 0;
}
