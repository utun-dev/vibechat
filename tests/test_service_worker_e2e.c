/* FILE/RW узлы: direct JOIN, приглашение A!=C, MEMBERS, сообщение и приглашение сервера через UDP. */
#include "utun_instance.h"
#include "chat/chat_channel.h"
#include "chat/chat_event.h"
#include "chat/chat_member.h"
#include "chat/chat_sync.h"
#include "chat/group_policy.h"
#include "chat/invite_build.h"
#include "chat/member_sync.h"
#include "db_storage.h"
#include "radio/radio.h"
#include "stream/stream.h"
#include "routing_layer/topo_group.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __linux__
static pthread_t loop;
static struct UASYNC* ua;
static uint64_t channel;
static char channel_text[65];
static unsigned sql_calls;
static int stream_listed, stream_empty;
struct node {
    struct UTUN_INSTANCE* inst;
    char directory[600], config[2400], crossed_channel[65];
    int core, chat, joined, join_code, received, crossed;
};
enum { NODE_COUNT = 4 };
static struct node nodes[NODE_COUNT];
static int reject_join_accept;
void __real_db_async_run(struct db_async*, const struct db_async_task*);
void __wrap_db_async_run(struct db_async* worker, const struct db_async_task* task) {
    if (reject_join_accept && worker == nodes[1].inst->db_worker && !strcmp(task->name, "JOIN-accept")) {
        reject_join_accept = 0;
        struct db_async_result result = {.status=DB_ASYNC_FULL,.message="test: JOIN accept database queue full"};
        task->done(task->data, &result);
        task->cleanup(task->data);
        return;
    }
    __real_db_async_run(worker, task);
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (pthread_equal(loop, pthread_self())) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "worker E2E forbidden loop SQL db=%p: %.500s", (void*)db, sql);
    assert(!pthread_equal(loop, pthread_self())); __atomic_add_fetch(&sql_calls, 1, __ATOMIC_RELAXED);
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*,int,char**,char**), void* arg, char** error) {
    assert(!pthread_equal(loop, pthread_self())); return __real_sqlite3_exec(db, sql, cb, arg, error);
}
int __real_sqlite3_get_table(sqlite3*, const char*, char***, int*, int*, char**);
int __wrap_sqlite3_get_table(sqlite3* db, const char* sql, char*** rows, int* count, int* columns, char** error) {
    assert(!pthread_equal(loop, pthread_self())); return __real_sqlite3_get_table(db, sql, rows, count, columns, error);
}
static void wait_flag(int* flag, const char* stage) {
    uint64_t until = get_time_us() + 30000000;
    while (!*flag && get_time_us() < until) uasync_poll(ua, 1);
    if (!*flag) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "worker E2E timeout stage=%s", stage);
    assert(*flag);
}
static void core_ready(void* data, struct UTUN_INSTANCE* inst, const struct db_async_result* result) {
    struct node* node = data; assert(pthread_equal(loop, pthread_self()) && node->inst == inst && !result->status);
    assert(inst->database_ready && !inst->topo_sqlite_db && db_async_can_write(inst->db_worker)); node->core = 1;
}
static void chat_ready(void* data, const struct db_async_result* result) {
    struct node* node = data; assert(!result->status && node->inst->chat_started && !node->inst->topo_sqlite_db); node->chat = 1;
}
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    struct node* node = NULL;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].inst == inst) node = &nodes[i];
    assert(node);
    if (type == CHAT_EVT_CONNECT_RESULT) {
        uint64_t request; assert(bytes == CHAT_CONNECT_RESULT_SIZE);
        struct TOPO_GROUP* joined_group = topo_groups_find_wire(inst->topo_groups, data + CHAT_CONNECT_RESULT_GROUP_OFF);
        const char* joined_channel = joined_group ? joined_group->channel_id : "";
        memcpy(&node->join_code, data + CHAT_CONNECT_RESULT_CODE_OFF, 4); memcpy(&request, data + CHAT_CONNECT_RESULT_REQUEST_OFF, 8);
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "worker E2E JOIN result node=%016llx request=%llu channel=%s code=%d",
            (unsigned long long)inst->node_id, (unsigned long long)request, joined_channel, node->join_code);
        if (!request) return; /* Входящий committed member отдельно уведомляет инвайтера без локального request. */
        if (request == 200 + (uint64_t)(node - nodes)) {
            assert(!node->join_code && !strcmp(joined_channel, node->crossed_channel) && !node->crossed++);
        } else assert(!strcmp(joined_channel, channel_text) && request == 100 + (uint64_t)(node - nodes) && !node->joined++);
    } else if (type == CHAT_EVT_STREAM_SOURCES && node == &nodes[3]) {
        assert(bytes >= 35 && !strcmp(sc_public_key_hex(data, (char[65]){0}), channel_text) && data[32] == 1);
        if (bytes == 35) { assert(!memcmp(data + 33, "[]", 2)); stream_empty = 1; }
        else {
            char prefix[80]; int len = snprintf(prefix, sizeof(prefix), "[{\"id\":\"%s\"",
                sc_public_key_hex(nodes[0].inst->my_ed25519_pubkey, (char[65]){0}));
            assert(bytes - 33 >= len && !memcmp(data + 33, prefix, len)); stream_listed = 1;
        }
    } else if (type == CHAT_EVT_MSG_RECEIVED && bytes > 9 && data[0] == strlen(channel_text) &&
        !memcmp(data + 1, channel_text, data[0])) node->received++;
}
static void hex_key(char out[65], const uint8_t key[32]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) { out[i * 2] = digits[key[i] >> 4]; out[i * 2 + 1] = digits[key[i] & 15]; }
    out[64] = 0;
}
static void start_node(struct node* node) {
    struct utun_config* parsed = parse_config_from_buf(node->config, strlen(node->config), "service worker E2E"); assert(parsed);
    node->inst = utun_instance_create_from_config_async(ua, parsed, core_ready, node); assert(node->inst);
    chat_event_set_handler(node->inst, event);
}
static void create_node(unsigned index, const char* root) {
    struct node* node = &nodes[index]; uint8_t device_x[32], device_seed[32]; assert(sc_identity_generate_config(device_x, device_seed) == SC_OK);
    char x[65], secret[65]; hex_key(x, device_x); hex_key(secret, device_seed);
    snprintf(node->directory, sizeof(node->directory), "%s/%u", root, index); assert(!mkdir(node->directory, 0700));
    snprintf(node->config, sizeof(node->config), "[global]\ndb_path=%s\nmy_node_name=Worker %u\nmy_public_key=%s\nmy_private_key=%s\n"
        "auto_sockets=no\n[allowed_keys]\nallow_all=yes\n[ntp]\nenabled=no\n"
        "[server:udp]\naddr=127.0.0.1:0\ntype=public\n[chatserver]\nmember_files_enabled=0\nstorage_autoload=0\n%s",
        node->directory, index, x, secret, index == 3 ?
        "group_autoconnect=0\nadd_key=5757575757575757575757575757575757575757575757575757575757575757\n" : "");
    start_node(node);
}
struct channel_reply { int done; struct chat_channel_result result; };
static void channel_created(void* data, const struct chat_channel_result* result) {
    struct channel_reply* reply = data; assert(!reply->done++); reply->result = *result;
}
struct link_reply { int done, result; char link[2048]; };
static void link_ready(void* data, int result, const char* link) {
    struct link_reply* reply = data; assert(!reply->done++); reply->result = result;
    if (link) { assert(strlen(link) < sizeof(reply->link)); strcpy(reply->link, link); }
}
static int pair_ready(unsigned left, unsigned right) {
    struct TOPO_GROUP* a = topo_groups_find(nodes[left].inst->topo_groups, channel);
    struct TOPO_GROUP* b = topo_groups_find(nodes[right].inst->topo_groups, channel);
    return a && b && topo_group_peer_ready(a, nodes[right].inst->node_id) && topo_group_peer_ready(b, nodes[left].inst->node_id);
}
static void join(unsigned inviter, unsigned provider, unsigned joiner) {
    struct link_reply link = {0};
    assert(chat_invite_build_link(nodes[inviter].inst, channel, nodes[provider].inst->my_ed25519_pubkey, link_ready, &link));
    wait_flag(&link.done, "registered link"); assert(link.result == 0 && link.link[0]);
    assert(!chat_sync_join_link(nodes[joiner].inst, 100 + joiner, link.link, strlen(link.link)));
    memset(link.link, 0, sizeof(link.link)); wait_flag(&nodes[joiner].joined, "JOIN result"); assert(!nodes[joiner].join_code);
    uint64_t until = get_time_us() + 30000000;
    while (!pair_ready(provider, joiner) && get_time_us() < until) uasync_poll(ua, 1);
    if (!pair_ready(provider, joiner)) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,
        "worker E2E READY timeout inviter=%u provider=%u joiner=%u", inviter, provider, joiner);
    assert(pair_ready(provider, joiner));
    for (unsigned i = 0; i <= joiner; i++)
        assert(group_policy_member_access(nodes[i].inst, channel_text, nodes[joiner].inst->node_id, 0));
}

/* Встречные JOIN через общий ETCP и два независимых SQL worker. */
static void crossed_join(void) {
    uint64_t groups[2];
    struct link_reply links[2] = {{0}};
    for (unsigned i = 0; i < 2; i++) {
        struct channel_reply created = {0};
        chat_channel_create_auto_submit(nodes[i].inst, "Crossed worker JOIN", "chat", channel_created, &created);
        wait_flag(&created.done, "crossed join group commit"); assert(!created.result.status && created.result.committed);
        groups[i] = topo_groups_channel_slot(nodes[i].inst->topo_groups, created.result.channel); assert(groups[i]);
        strcpy(nodes[1 - i].crossed_channel, created.result.channel);
        assert(chat_invite_build_link(nodes[i].inst, groups[i], nodes[i].inst->my_ed25519_pubkey, link_ready, &links[i]));
    }
    assert(strcmp(nodes[0].crossed_channel, nodes[1].crossed_channel));
    for (unsigned i = 0; i < 2; i++) {
        wait_flag(&links[i].done, "crossed join link"); assert(!links[i].result && links[i].link[0]);
    }
    for (unsigned i = 0; i < 2; i++) {
        assert(!chat_sync_join_link(nodes[i].inst, 200 + i, links[1 - i].link, strlen(links[1 - i].link)));
        memset(links[1 - i].link, 0, sizeof(links[1 - i].link));
    }
    for (unsigned i = 0; i < 2; i++) {
        wait_flag(&nodes[i].crossed, "crossed JOIN result");
        assert(group_policy_member_access(nodes[1 - i].inst, nodes[i].crossed_channel, nodes[i].inst->node_id, 0));
    }
}
/* После реального JOIN подписка должна обновить и список участников, и маршрут голосовых кадров. */
static const uint8_t opus_frame[] = {0xf8, 0xff, 0xfe};
static void radio_frame(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint16_t stream, uint16_t seq,
                        uint8_t fin, const uint8_t* opus, int len, void* arg) {
    (void)stream;
    int* received = arg;
    assert(inst == nodes[1].inst && group == channel && !memcmp(ed, nodes[0].inst->my_ed25519_pubkey, 32));
    assert(seq == *received && (fin ? seq == 1 : seq == 0));
    if (!fin) assert(len == sizeof(opus_frame) && !memcmp(opus, opus_frame, sizeof(opus_frame)));
    ++*received;
}
static void check_radio(void) {
    struct UTUN_INSTANCE *sender = nodes[0].inst, *listener = nodes[1].inst;
    int received = 0;
    radio_set_frame_cb(listener, radio_frame, &received);
    for (int on = 1; on >= 0; --on) {
        radio_set_active(listener, channel, on);
        uint64_t deadline = get_time_us() + 5000000;
        while (radio_subscribers(sender, channel) != on && get_time_us() < deadline) uasync_poll(ua, 1);
        struct chat_member_display member = {0}; memcpy(member.ed25519, listener->my_ed25519_pubkey, 32);
        assert(!chat_member_apply_live(sender, channel_text, &member));
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "worker E2E radio on=%d subscribers=%d member_flags=%u", on, radio_subscribers(sender, channel), member.flags);
        assert(radio_subscribers(sender, channel) == on && !!(member.flags & CHAT_MEMBER_FLAG_RADIO) == on);
        if (on) {
            assert(!radio_talk_begin(sender, channel) && !radio_talk_send(sender, channel, opus_frame, sizeof(opus_frame)));
            assert(!radio_talk_end(sender, channel));
            deadline = get_time_us() + 5000000;
            while (received < 2 && get_time_us() < deadline) uasync_poll(ua, 1);
            assert(received == 2);
        }
    }
    radio_set_frame_cb(listener, NULL, NULL);
}
/* Публикация и её профиль уже существуют до перезапуска получателя. Путь появляется после первого пустого снимка. */
static void check_restored_stream(void) {
    struct node* viewer = &nodes[3]; struct STREAM_HANDLE* publisher = NULL;
    assert(stream_publish_open(nodes[0].inst, channel, NULL, &publisher) == STREAM_OK);
    assert(stream_watch_sources(viewer->inst, channel, 0) == STREAM_OK);
    wait_flag(&stream_listed, "existing stream profile replicated");
    utun_instance_destroy(viewer->inst); viewer->core = viewer->chat = 0;
    start_node(viewer); wait_flag(&viewer->core, "restore viewer database"); assert(!utun_core_start(viewer->inst));
    chat_service_start_async(viewer->inst, chat_ready, viewer); wait_flag(&viewer->chat, "restore stream channel");
    stream_listed = stream_empty = 0; assert(stream_watch_sources(viewer->inst, channel, 0) == STREAM_OK);
    wait_flag(&stream_empty, "restored stream channel before route");
    struct TOPO_PEER_REQUEST* peer = NULL;
    assert(!topo_group_peer_open(topo_groups_find(viewer->inst->topo_groups, channel), nodes[0].inst->node_id, &peer));
    wait_flag(&stream_listed, "existing stream visible after route");
    stream_empty = 0; stream_close(publisher); wait_flag(&stream_empty, "existing stream ended");
    assert(stream_watch_sources(viewer->inst, 0, 0) == STREAM_OK); topo_group_peer_close(peer);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] restored channel discovered existing publisher on route arrival without selecting it again");
}
struct reply { int done; struct db_async_result result; };
static void changed(void* data, const struct db_async_result* result) {
    struct reply* reply = data; assert(!reply->done++); reply->result = *result;
}
struct message_reply { int done; struct chat_message_result result; };
static void message_saved(void* data, const struct chat_message_result* result) {
    struct message_reply* reply = data; assert(!reply->done++); reply->result = *result;
}
struct message_probe { uint8_t id[32], author[32]; int done; };
static int probe_message(sqlite3* db, void* data) {
    struct message_probe* probe = data; char sql[256]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT accepted_key,data,length(author_signature) FROM msg_%s WHERE message_id=?", channel_text);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL); if (!rc) rc = sqlite3_bind_blob(st, 1, probe->id, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 &&
        !memcmp(sqlite3_column_blob(st, 0), probe->author, 32) && sqlite3_column_int(st, 2) == 64);
    const char* text = (const char*)sqlite3_column_text(st, 1); assert(text && strstr(text, "FILE worker E2E"));
    return sqlite3_finalize(st);
}
static void message_probed(void* data, const struct db_async_result* result) {
    assert(!result->status); ((struct message_probe*)data)->done = 1;
}

static void check_cascade(void) {
    struct UTUN_INSTANCE* owner = nodes[0].inst;
    struct UTUN_INSTANCE* member = nodes[2].inst;
    for (int admitted = 0; admitted <= 1; admitted++) {
        struct reply role = {0};
        chat_core_save_member_async(owner, channel_text, nodes[1].inst->my_ed25519_pubkey, NULL,
            admitted ? "{\"admin\":\"yes\"}" : "{\"admin\":\"no\"}", changed, &role);
        wait_flag(&role.done, admitted ? "admin restoration" : "admin revocation");
        assert(!role.result.status);
        uint64_t until = get_time_us() + 10000000;
        while (group_policy_member_access(member, channel_text, member->node_id, 0) != admitted && get_time_us() < until)
            uasync_poll(ua, 1);
        assert(group_policy_member_access(owner, channel_text, member->node_id, 0) == admitted);
        assert(group_policy_member_access(member, channel_text, member->node_id, 0) == admitted);
        assert(group_policy_member_verified(member, channel_text, member->my_ed25519_pubkey));
    }
}
int main(void) {
    loop = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_INFO); debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_INFO);
    size_t before = u_get_allocated_count(); char root[] = "/tmp/utun-service-worker-XXXXXX"; assert(!test_mkdtemp(root));
    ua = uasync_create(); assert(ua);
    for (unsigned i = 0; i < NODE_COUNT; i++) create_node(i, root);
    for (unsigned i = 0; i < NODE_COUNT; i++) {
        wait_flag(&nodes[i].core, "core bootstrap"); assert(!utun_core_start(nodes[i].inst));
        chat_service_start_async(nodes[i].inst, chat_ready, &nodes[i]);
    }
    for (unsigned i = 0; i < NODE_COUNT; i++) wait_flag(&nodes[i].chat, "chat activation");
    struct channel_reply created = {0}; chat_channel_create_auto_submit(nodes[0].inst, "FILE worker E2E", "chat", channel_created, &created);
    wait_flag(&created.done, "group commit"); assert(!created.result.status && created.result.committed);
    channel = topo_groups_channel_slot(nodes[0].inst->topo_groups, created.result.channel); strcpy(channel_text, created.result.channel);
    reject_join_accept = 1;
    join(0, 0, 1);
    assert(!reject_join_accept);
    check_radio();
    char owner[MS_ADM_TAGS_MAX + 1] = "{\"admin\":\"yes\",\"padding\":\"";
    size_t prefix = strlen(owner), zeros = MS_ADM_TAGS_MAX - prefix - 64;
    memset(owner + prefix, 'x', zeros);
    memcpy(owner + prefix + zeros, "\"}", 3);
    struct reply granted = {0}; chat_core_save_member_async(nodes[0].inst, channel_text, nodes[1].inst->my_ed25519_pubkey, NULL,
        owner, changed, &granted); wait_flag(&granted.done, "admin grant"); assert(!granted.result.status);
    uint64_t until = get_time_us() + 30000000;
    while (group_policy_can_invite(nodes[1].inst, channel_text, nodes[1].inst->my_ed25519_pubkey) != 1 && get_time_us() < until) uasync_poll(ua, 1);
    assert(group_policy_can_invite(nodes[1].inst, channel_text, nodes[1].inst->my_ed25519_pubkey) == 1);
    join(1, 0, 2);
    char text[] = "FILE worker E2E message"; struct chat_msg_submit input = {0};
    strcpy(input.channel_id, channel_text); strcpy(input.content_type, "text"); input.data = (uint8_t*)text; input.data_len = strlen(text);
    struct message_reply sent = {0}; chat_core_submit_message_async(nodes[0].inst, &input, message_saved, &sent);
    memset(text, 'x', strlen(text)); wait_flag(&sent.done, "message commit"); assert(!sent.result.status && sent.result.committed);
    wait_flag(&nodes[2].received, "message delivery");
    struct message_probe probe = {0};
    memcpy(probe.author, nodes[0].inst->my_ed25519_pubkey, 32); memcpy(probe.id, sent.result.message_id, 32);
    db_async_submit(nodes[2].inst->db_worker, "E2E delivered message", DB_ASYNC_READ, probe_message, &probe, message_probed);
    wait_flag(&probe.done, "message SQL probe");
    /* Сервер впервые открывает NCD к отправителю, когда SERVER_JOIN_REQ уже пришёл по рабочему ETCP. */
    struct link_reply server_link = {0};
    assert(chat_server_link_start(nodes[3].inst, link_ready, &server_link));
    wait_flag(&server_link.done, "server link"); assert(!server_link.result && server_link.link[0]);
    assert(!chat_sync_offer_link(nodes[0].inst, 100, channel, server_link.link, strlen(server_link.link)));
    memset(server_link.link, 0, sizeof(server_link.link));
    wait_flag(&nodes[0].joined, "server JOIN result"); assert(!nodes[0].join_code);
    until = get_time_us() + 30000000;
    while (!pair_ready(0, 3) && get_time_us() < until) uasync_poll(ua, 1);
    assert(pair_ready(0, 3));
    check_restored_stream();
    crossed_join();
    check_cascade();
    for (unsigned i = 0; i < NODE_COUNT; i++) { utun_instance_destroy(nodes[i].inst); nodes[i].inst = NULL; }
    uasync_destroy(ua, 0);
    const char* directories[] = {"member_files/.uploads", "member_files", "dm_pending", "dm_media"};
    for (unsigned i = 0; i < NODE_COUNT; i++) {
        assert(!db_storage_clear(nodes[i].directory));
        for (unsigned j = 0; j < sizeof(directories) / sizeof(*directories); j++) {
            char path[700]; snprintf(path, sizeof(path), "%s/%s", nodes[i].directory, directories[j]); assert(!test_rmdir(path));
        }
        assert(!test_rmdir(nodes[i].directory));
    }
    assert(!test_rmdir(root)); if (u_get_allocated_count() != before) u_report_unfreed_blocks();
    assert(u_get_allocated_count() == before && __atomic_load_n(&sql_calls, __ATOMIC_RELAXED) > 100);
    puts("PASS FILE service workers: direct/delegated JOIN, role replication, signed message delivery and server JOIN over active UDP"); return 0;
}
#else
int main(void) { puts("SKIP strict worker SQL interception requires Linux linker wrapping"); return 0; }
#endif
