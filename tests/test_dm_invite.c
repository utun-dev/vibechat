/* Приглашение в другую группу через настоящую PM: отмена, ошибки, UDP-доставка и JOIN. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/json_flat.h"
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/chat_join.h"
#include "../src/chat/chat_sync.h"
#include "../src/chat/db_sync.h"
#include "../src/chat/merkle_sync.h"
#include "../src/chat/invite_build.h"
#include "../src/chat/invite_link.h"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_crypto.h"
#include "../src/dm/dm_history.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/transport_layer/secure_channel.h"
#include "../src/transport_layer/etcp.h"
#include "test_utils.h"
#include "test_clock.h"

static struct UTUN_INSTANCE* nodes[2];
static int results[16], calls[16], joined;
static uint8_t joining_channel[32];
static uint64_t dropped_group;
static int dropped_requests;
static etcp_recv_fn history_receivers[2];
static int ready_events, unavailable_events;

static void peer_ready(struct TOPO_GROUP* group, const uint8_t peer[32], int ready, void* arg) {
    (void)group; (void)peer; (void)arg;
    if (ready) ready_events++; else unavailable_events++;
}

/* Потеря прикладных пакетов после надёжной ETCP-доставки: первый обмен обязан истечь и повториться. */
static void drop_history(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct TOPO_GROUP* group = entry->len >= MS_HEADER ? topo_groups_find_wire(conn->instance->topo_groups, entry->dgram + MS_SCOPE_OFF) : NULL;
    uint64_t gid = group ? group->group_id : 0;
    int index = conn->instance == nodes[0] ? 0 : 1;
    if (gid != dropped_group || !group || entry->dgram[MS_DATASET_OFF] != MT_DATA_MESSAGES) {
        history_receivers[index](conn, entry); return;
    }
    if (entry->dgram[MS_TYPE_OFF] == 0x10) { /* MS_WAKE — новый запрос follower после истечения попытки. */
        dropped_requests++;
        DEBUG_INFO(DEBUG_CATEGORY_DM, "invite test: dropped history request=%d group=%016llx", dropped_requests, gid);
        if (dropped_requests == 2)
            for (int i = 0; i < 2; i++) etcp_bind(nodes[i], ETCP_ID_MEMBER_SYNC, history_receivers[i]);
    }
    queue_dgram_free(entry); queue_entry_free(entry);
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (type == CHAT_EVT_DM_INVITE_RESULT && len == 60) {
        uint64_t id, conv; int32_t result;
        memcpy(&id, data, 8); memcpy(&result, data + 8, 4);
        memcpy(&conv, data + 44, 8);
        if (id >= 16 || memcmp(data + 12, nodes[1]->my_ed25519_pubkey, 32) || conv != dm_derive_conv_id(nodes[0]->my_ed25519_pubkey, nodes[1]->my_ed25519_pubkey)) abort();
        calls[id]++; results[id] = result;
    } else if (inst == nodes[1] && type == CHAT_EVT_CONNECT_RESULT && len == CHAT_CONNECT_RESULT_SIZE) {
        int32_t result; memcpy(&result, data + CHAT_CONNECT_RESULT_CODE_OFF, 4);
        if (!memcmp(data + CHAT_CONNECT_RESULT_GROUP_OFF, joining_channel, 32)) joined = result == 0 ? 1 : -1;
    }
}

struct link_result { int ready; char link[1024]; };
static void link_ready(void* arg, int result, const char* link) {
    struct link_result* state = arg;
    state->ready = result == CHAT_JOIN_OK ? 1 : -1;
    if (link) snprintf(state->link, sizeof(state->link), "%s", link);
}

static int scalar(struct UTUN_INSTANCE* inst, const char* sql, uint64_t* value) {
    sqlite3_stmt* statement = NULL;
    int rc = sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &statement, NULL);
    if (rc == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        *value = (uint64_t)sqlite3_column_int64(statement, 0); rc = 0;
    } else rc = -1;
    sqlite3_finalize(statement);
    return rc;
}

static int message_count(void) {
    uint64_t count = 0;
    return scalar(nodes[0], "SELECT count(*) FROM dm_messages WHERE dir=1", &count) ? -1 : (int)count;
}

static void request(uint64_t id, uint64_t group, const char* source, int cancel) {
    struct dm_invite_req* req = u_calloc(1, sizeof(*req));
    if (!req) abort();
    req->inst = nodes[0]; req->request_id = id; memcpy(req->peer_ed, nodes[1]->my_ed25519_pubkey, 32);
    const char* channel = topo_groups_channel(nodes[0]->topo_groups, group);
    if (channel) strcpy(req->channel_id, channel);
    req->cancel = cancel;
    snprintf(req->source_ch_id, sizeof(req->source_ch_id), "%s", source);
    snprintf(req->peer_name, sizeof(req->peer_name), "B");
    dm_invite_trampoline(req);
}

static int wait_result(struct UASYNC* ua, int id, int expected) {
    uint64_t deadline = get_time_tb() + 30000;
    while (!calls[id] && get_time_tb() < deadline) test_clock_poll(ua, 100);
    return calls[id] == 1 && results[id] == expected;
}

static int join_link(struct UASYNC* ua, const char* link) {
    struct InviteData invite; char error[128]; uint8_t addresses[1024];
    if (invite_link_decode(link, strlen(link), &invite, error, sizeof(error))) return -1;
    int bytes = invite_serialize_addrs(&invite, addresses, sizeof(addresses));
    if (bytes < 0) return -1;
    uint64_t slot = chat_join_key_group(nodes[0], invite.join_key);
    const uint8_t* group = topo_group_wire_id(topo_groups_find(nodes[0]->topo_groups, slot));
    if (!group) return -1;
    memcpy(joining_channel, group, 32); joined = 0;
    if (chat_sync_join_link(nodes[1], 1, link, strlen(link))) return -1;
    uint64_t deadline = get_time_tb() + 200000;
    while (!joined && get_time_tb() < deadline) test_clock_poll(ua, 100);
    if (joined != 1) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite test: JOIN failed ch=%s result=%d",
        sc_public_key_hex(joining_channel, (char[65]){0}), joined); return -1; }
    return 0;
}

static int start_node(struct UASYNC* ua, const char* directory, int index, int port) {
    uint8_t device_x[32], device_seed[32];
    if (sc_identity_generate_config(device_x, device_seed) != SC_OK) return -1;
    if (index == 1) {
        int master = getenv("UTUN_TEST_INVITEE_MASTER") != NULL;
        while ((sc_derive_node_id_from_pubkey(device_x) > nodes[0]->node_id) != master)
            if (sc_identity_generate_config(device_x, device_seed) != SC_OK) return -1;
    }
    char pub[65], priv[65], path[768], database[768];
    for (int i = 0; i < 32; i++) {
        snprintf(pub + i * 2, 3, "%02x", device_x[i]);
        snprintf(priv + i * 2, 3, "%02x", device_seed[i]);
    }
    snprintf(database, sizeof(database), "%s/db%d", directory, index);
    if (utun_mkdir(database, 0700)) return -1;
    snprintf(path, sizeof(path), "%s/%d.conf", directory, index);
    FILE* config = fopen(path, "w");
    if (!config) return -1;
    fprintf(config, "[global]\nmy_public_key=%s\nmy_private_key=%s\ndb_path=%s\nmy_node_name=%c\n"
        "[server:s1]\naddr=127.0.0.1:%d\ntype=public\n[allowed_keys]\nallow_all=1\n"
        "[chatserver]\nstorage_autoload=0\n", pub, priv, database, 'A' + index, port);
    fclose(config);
    nodes[index] = utun_instance_create(ua, path);
    if (!nodes[index] || utun_instance_init(nodes[index])) return -1;
    chat_event_set_handler(nodes[index], event);
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    test_clock_init(1000); test_clock_sync_wall_time();
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_DM, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_INFO);
    }
    utun_instance_set_tun_init_enabled(0);
    struct UASYNC* ua = uasync_create();
    char directory[512] = "/tmp/utun_dm_invite_XXXXXX";
    int failed = 1;
#define CHECK(condition, reason) do { if (!(condition)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "invite test: %s", reason); goto done; } } while (0)
    CHECK(ua && !test_mkdtemp(directory), "temporary directory/event loop");
    CHECK(!start_node(ua, directory, 0, 0) && !start_node(ua, directory, 1, 0), "start nodes");
    uint64_t source = chat_core_create_channel_auto(nodes[0], "source");
    CHECK(source, "create source group");
    char source_id[65]; strcpy(source_id, topo_groups_channel(nodes[0]->topo_groups, source));
    struct link_result first = {0};
    CHECK(chat_invite_build_link(nodes[0], source, 0, link_ready, &first), "build initial invite");
    uint64_t deadline = get_time_tb() + 30000;
    while (!first.ready && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(first.ready == 1 && !join_link(ua, first.link), "join common source group");
    uint64_t target = chat_core_create_channel_auto(nodes[0], "target \"группа\"\n<>&");
    CHECK(target, "create target group with Unicode, quotes and newline in signed title");
    CHECK(target != source && !message_count(), "initial empty PM");
    char target_id[65]; strcpy(target_id, topo_groups_channel(nodes[0]->topo_groups, target));
    for (int i = 0; i < 5; i++) {
        char text[64]; snprintf(text, sizeof(text), "history before invitation %d", i);
        struct chat_msg_submit message = {0};
        snprintf(message.channel_id, sizeof(message.channel_id), "%s", target_id);
        snprintf(message.content_type, sizeof(message.content_type), "text");
        message.data = (uint8_t*)text; message.data_len = (uint32_t)strlen(text);
        chat_core_submit_message(nodes[0], &message);
    }
    CHECK(chat_core_count(nodes[0], target_id) == 5 && !chat_core_count(nodes[1], target_id), "five messages exist before PM invite");

    request(1, target, source_id, 0);
    CHECK(!message_count() && !calls[1], "no PM before registration ACK");
    request(2, target, source_id, 0);
    CHECK(wait_result(ua, 2, DM_INVITE_BUSY), "busy request retains first operation");
    request(1, target, source_id, 1);
    CHECK(wait_result(ua, 1, DM_INVITE_CANCELLED) && !message_count(), "cancel before ACK sends nothing");
    request(7, target, source_id, 0);
    CHECK(!calls[7], "invite waits for ACK before delete");
    char deleted_conv[64];
    snprintf(deleted_conv, sizeof(deleted_conv), "%llu", (unsigned long long)dm_derive_conv_id(nodes[0]->my_ed25519_pubkey, nodes[1]->my_ed25519_pubkey));
    CHECK(!dm_history_delete(nodes[0], deleted_conv, dm_history_generation(nodes[0], deleted_conv)), "delete with pending invitation");
    CHECK(wait_result(ua, 7, DM_INVITE_CANCELLED) && !message_count(), "delete cancels invitation without publishing PM");
    CHECK(!dm_start(nodes[0], nodes[1]->node_id, nodes[1]->my_keys.public_key, nodes[1]->my_ed25519_pubkey,
          "B", source_id, DM_MODE_DEVICE, NULL),
          "explicit start reopens PM after deletion");
    request(3, 0, source_id, 0);
    CHECK(wait_result(ua, 3, DM_INVITE_BUILD_ERROR) && !message_count(), "invalid group sends nothing");

    CHECK(sqlite3_exec(nodes[0]->topo_sqlite_db, "UPDATE dm_conversations SET group_id=''", NULL, NULL, NULL) == SQLITE_OK,
          "simulate automatically created PM without group route");
    CHECK(sqlite3_exec(nodes[0]->topo_sqlite_db, "CREATE TEMP TRIGGER fail_invite BEFORE INSERT ON dm_outbox "
        "BEGIN SELECT RAISE(ABORT,'test invite commit failure'); END", NULL, NULL, NULL) == SQLITE_OK, "install failed commit");
    request(4, target, source_id, 0);
    CHECK(wait_result(ua, 4, DM_INVITE_SEND_ERROR) && !message_count(), "failed commit returns error and rolls back PM");
    CHECK(sqlite3_exec(nodes[0]->topo_sqlite_db, "DROP TRIGGER fail_invite", NULL, NULL, NULL) == SQLITE_OK, "remove failed commit");

    request(5, target, "", 0); /* из шапки существующей PM исходная группа не передаётся */
    request(4, target, "", 1); /* запоздалая отмена другой операции не отменяет текущую */
    CHECK(wait_result(ua, 5, DM_INVITE_OK) && message_count() == 1, "one invitation committed for fixed peer");
    uint64_t route = 0;
    CHECK(!scalar(nodes[0], "SELECT group_id FROM dm_conversations LIMIT 1", &route) && !route,
          "send transaction preserves an unspecified route hint");
    char conv[64], messages[8192]; size_t length = 0;
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)dm_derive_conv_id(nodes[0]->my_ed25519_pubkey, nodes[1]->my_ed25519_pubkey));
    deadline = get_time_tb() + 100000;
    char* link = NULL;
    do {
        test_clock_poll(ua, 100);
        if (!dm_list_messages_json(nodes[1], conv, 10, 0, messages, sizeof(messages), &length)) link = strstr(messages, "utun://");
    } while (!link && get_time_tb() < deadline);
    CHECK(link, "invite delivered to B over source group's UDP route");
    CHECK(dm_route_group(nodes[0], nodes[1]->node_id, route, &route) && route == source,
          "delivered PM resolves the live shared-group route without a stored hint");
    char incoming_name[128], outgoing_name[128]; const char* tags = strstr(messages, "\"tags\":");
    CHECK(strstr(messages, "\"ct\":\"invite\"") && tags &&
          !json_flat_get(tags + 7, "group_name", incoming_name, sizeof(incoming_name)) && !strcmp(incoming_name, "target \"группа\"\n<>&"),
          "encrypted group name survives delivery and JSON escaping without sender-language text");
    char outgoing[8192]; size_t outgoing_len = 0;
    CHECK(!dm_list_messages_json(nodes[0], conv, 10, 0, outgoing, sizeof(outgoing), &outgoing_len) &&
          (tags = strstr(outgoing, "\"tags\":")) && !json_flat_get(tags + 7, "group_name", outgoing_name, sizeof(outgoing_name)) &&
          !strcmp(incoming_name, outgoing_name),
          "sender and recipient receive the same group-name tag");
    char* end = strchr(link, '"'); CHECK(end, "complete invite text in PM"); *end = '\0';
    struct InviteData received; char error[128];
    CHECK(!invite_link_decode(link, strlen(link), &received, error, sizeof(error)) && chat_join_key_group(nodes[0], received.join_key) == target,
          "PM invites target group rather than source group");
    CHECK(JOIN_KEY_TTL_SECONDS == 600 && chat_join_key_matches(nodes[0], target, received.join_key, nodes[0]->my_ed25519_pubkey),
          "delivered link is registered with ten minute lifetime");
    if (getenv("UTUN_TEST_HISTORY_TIMEOUT")) {
        dropped_group = target;
        for (int i = 0; i < 2; i++) {
            history_receivers[i] = nodes[i]->api_bindings.callbacks[ETCP_ID_MEMBER_SYNC];
            etcp_bind(nodes[i], ETCP_ID_MEMBER_SYNC, drop_history);
        }
    }
    CHECK(!join_link(ua, link), "B joins target group using received PM link");
    chat_core_connect_channel(nodes[1], target_id);
    deadline = get_time_tb() + 100000;
    while (!topo_node_sqlite_member_in_channel(nodes[1]->topo_sqlite_db, target_id, nodes[1]->node_id) &&
           get_time_tb() < deadline) test_clock_poll(ua, 100);
    DEBUG_INFO(DEBUG_CATEGORY_DM, "invite test: membership after JOIN ch=%s A_has_B=%d B_has_B=%d", target_id,
               topo_node_sqlite_member_in_channel(nodes[0]->topo_sqlite_db, target_id, nodes[1]->node_id),
               topo_node_sqlite_member_in_channel(nodes[1]->topo_sqlite_db, target_id, nodes[1]->node_id));
    CHECK(topo_node_sqlite_member_in_channel(nodes[0]->topo_sqlite_db, target_id, nodes[1]->node_id) &&
          topo_node_sqlite_member_in_channel(nodes[1]->topo_sqlite_db, target_id, nodes[1]->node_id), "membership committed on both nodes");
    deadline = get_time_tb() + 400000;
    while (chat_core_count(nodes[1], target_id) != 5 && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(chat_core_count(nodes[1], target_id) == 5, "history created before invitation arrives after JOIN");
    CHECK(!getenv("UTUN_TEST_HISTORY_TIMEOUT") || dropped_requests == 2, "requester retries after lost initial exchange");
    CHECK(!chat_core_get_messages_json(nodes[1], target_id, 10, 0, messages, sizeof(messages), &length), "read received history");
    for (int i = 0; i < 5; i++) {
        char text[64]; snprintf(text, sizeof(text), "history before invitation %d", i);
        CHECK(strstr(messages, text), "every old message is present");
    }
    struct DB_SYNC_INSTANCE* history_a = db_sync_instance_find(nodes[0], target);
    struct DB_SYNC_INSTANCE* history_b = db_sync_instance_find(nodes[1], target);
    uint8_t hash_a[32], hash_b[32];
    CHECK(history_a && history_b && !db_sync_verify(history_a) && !db_sync_verify(history_b) &&
          !db_sync_read_root(history_a, hash_a) && !db_sync_read_root(history_b, hash_b) && !memcmp(hash_a, hash_b, 32),
          "received history signatures and full Merkle roots agree");
    struct TOPO_GROUP* target_group = topo_groups_find(nodes[1]->topo_groups, target);
    struct ETCP_CONN* shared_conn = instance_find_conn(nodes[1], nodes[0]->node_id);
    CHECK(target_group && shared_conn && topo_group_peer_ready(target_group, nodes[0]->node_id), "target session is READY");
    CHECK(!topo_group_add_peer_ready_cbk(target_group, peer_ready, NULL), "observe target session transitions");
    topo_group_peer_leave(target_group, nodes[0]->node_id);
    CHECK(unavailable_events == 1 && !topo_group_peer_ready(target_group, nodes[0]->node_id) && shared_conn->links_up,
          "leaving target publishes readiness loss while source group's transport stays UP");
    const char* missed = "message while target session is unavailable";
    struct chat_msg_submit offline = {0};
    snprintf(offline.channel_id, sizeof(offline.channel_id), "%s", target_id);
    snprintf(offline.content_type, sizeof(offline.content_type), "text");
    offline.data = (uint8_t*)missed; offline.data_len = (uint32_t)strlen(missed);
    chat_core_submit_message(nodes[0], &offline);
    CHECK(!topo_group_new_conn(target_group, shared_conn), "restart target session on shared transport");
    deadline = get_time_tb() + 100000;
    while (chat_core_count(nodes[1], target_id) != 6 && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(ready_events == 1 && chat_core_count(nodes[1], target_id) == 6 &&
          !chat_core_get_messages_json(nodes[1], target_id, 10, 0, messages, sizeof(messages), &length) && strstr(messages, missed),
          "new READY session resumes history and receives the missed message");
    topo_group_remove_peer_ready_cbk(target_group, peer_ready, NULL);
    const char* text = "joined through PM invitation";
    struct chat_msg_submit message = {0};
    snprintf(message.channel_id, sizeof(message.channel_id), "%s", target_id);
    snprintf(message.content_type, sizeof(message.content_type), "text");
    message.data = (uint8_t*)text; message.data_len = (uint32_t)strlen(text);
    chat_core_submit_message(nodes[1], &message);
    deadline = get_time_tb() + 100000;
    while (chat_core_count(nodes[0], target_id) != 7 && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(!chat_core_get_messages_json(nodes[0], target_id, 10, 0, messages, sizeof(messages), &length) && strstr(messages, text),
          "joined client can send a group message to the inviter");
    request(6, target, "", 0);
    dm_core_destroy(nodes[0]);
    CHECK(calls[6] == 1 && results[6] == DM_INVITE_CANCELLED && message_count() == 1, "teardown cancels pending invitation");
    failed = 0;
done:
    for (int i = 0; i < 2; i++) if (nodes[i]) utun_instance_destroy(nodes[i]);
    if (ua) uasync_destroy(ua, 0);
    test_clock_finish();
    printf("PM invite: cancellation, failure, delivery and JOIN: %s\n", failed ? "FAIL" : "OK");
    return failed;
}
