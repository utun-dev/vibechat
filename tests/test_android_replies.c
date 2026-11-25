/* Android bridge + настоящее ядро: ссылки, подтверждения, media workers и старая история.
 * Подменяем только владельца instance, чтобы весь сценарий выполнялся в одном uasync-потоке. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_event.h"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_crypto.h"
#include "../src/dm/dm_history.h"
#include "../src/transport_layer/secure_channel.h"
#include "../src/media_async/attachment_send.h"
#include "../src/media_async/encrypted_file.h"
#include "../src/attachments/recovery.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/transport_layer/etcp_connections.h"
#include "../src/config_parser.h"
#include "../tools/chatgui-android/jni_bridge/android_jni_bridge.h"
#include "../tools/chatgui-android/libutun_lite/instance_lite.h"
#include "test_utils.h"

#define REQUIRE(value) do { if (!(value)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "Android reply check failed line=%d", __LINE__); return 1; } } while (0)
static struct UTUN_INSTANCE* instance;
static struct UASYNC* async;
static int results[32];
static unsigned result_counts[32];
static const char quote_text[] = "😀 \"original\"\nnext line";
struct UTUN_INSTANCE* __wrap_instance_lite_get_instance(void) { return instance; }
struct UASYNC* __wrap_instance_lite_get_uasync(void) { return async; }
int __wrap_instance_lite_is_running(void) { return instance && uasync_is_running(async); }
int __wrap_instance_lite_post(void (*callback)(void*), void* arg, struct UTUN_INSTANCE** field, void (*discard)(void*)) {
    (void)discard;
    if (!instance || !uasync_is_running(async)) return -1;
    struct posted_task* task = u_calloc(1, sizeof(*task));
    if (!task) return -1;
    if (field) *field = instance;
    task->callback = callback; task->arg = arg;
    uasync_post_reserved(async, task); return 0;
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int length) {
    (void)inst;
    if (type != CHAT_EVT_MSG_SUBMITTED || length != 9) return;
    uint64_t id = 0;
    for (int i = 0; i < 8; i++) id |= (uint64_t)data[i] << (8 * i);
    if (id < 32) { results[id] = data[8] == 1 ? 1 : data[8] == 2 ? -2 : -1; result_counts[id]++; }
}

static int wait_result(int id) {
    for (int i = 0; i < 500 && !results[id]; i++) uasync_poll(async, 20);
    return results[id];
}

static int extract(const char* json, const char* path, char* value, size_t capacity) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(instance->topo_sqlite_db, "SELECT json_extract(?1,?2)", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) snprintf(value, capacity, "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : -1;
}

static int check_reply(const char* channel, const char* original, const char* type) {
    char* json = utun_bridge_get_messages_json(channel, 1, NULL, 0);
    char reply[65], content_type[64], quote[512];
    int ok = !extract(json, "$[0].replyTo", reply, sizeof(reply)) && !strcmp(reply, original) &&
        !extract(json, "$[0].contentType", content_type, sizeof(content_type)) && !strcmp(content_type, type) &&
        !extract(json, "$[0].replyQuote", quote, sizeof(quote)) && !strcmp(quote, quote_text);
    u_free(json); return ok;
}

/* Настоящий контейнер проходит через bridge; ciphertext и manifest не становятся доступным файлом в UI. */
static int check_encrypted_attachment(const char* channel, const char* directory, const uint8_t original[32]) {
    char source[1024], value[1024];
    snprintf(source, sizeof(source), "%s/private-source", directory);
    FILE* file = fopen(source, "wb"); REQUIRE(file);
    REQUIRE(fwrite("private image", 1, 13, file) == 13 && !fclose(file));
    struct attachment_send_req* req = u_calloc(1, sizeof(*req)); REQUIRE(req);
    req->inst = instance; req->encrypted = 1; req->request_id = 10;
    memcpy(req->reply_to, original, 32); strcpy(req->reply_quote, quote_text);
    snprintf(req->target, sizeof(req->target), "%s", channel);
    snprintf(req->source, sizeof(req->source), "%s", source);
    snprintf(req->info.name, sizeof(req->info.name), "Секрет | 😀.png");
    snprintf(req->content_type, sizeof(req->content_type), "image/png");
    req->info.kind = ATTACHMENT_PNG; req->info.width = 1080; req->info.height = 1920;
    attachment_send_trampoline(req); REQUIRE(wait_result(10) == 1);
    sqlite3_stmt* st = NULL; uint8_t id[32];
    REQUIRE(sqlite3_prepare_v2(instance->topo_sqlite_db, "SELECT message_id FROM attachment_keys", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32);
    memcpy(id, sqlite3_column_blob(st, 0), sizeof(id)); sqlite3_finalize(st);
    char* json = utun_bridge_get_messages_json(channel, 1, id, 1); REQUIRE(json);
    REQUIRE(!extract(json, "$[0].replyQuote", value, sizeof(value)) && !strcmp(value, quote_text));
    REQUIRE(!extract(json, "$[0].encryptionState", value, sizeof(value)) && !strcmp(value, "ready"));
    REQUIRE(!extract(json, "$[0].text", value, sizeof(value)) && !strcmp(value, "Секрет | 😀.png"));
    REQUIRE(!extract(json, "$[0].contentType", value, sizeof(value)) && !strcmp(value, "image/png"));
    REQUIRE(!extract(json, "$[0].filePath", value, sizeof(value)) && !access(value, F_OK) && !strstr(value, ".uenc"));
    REQUIRE(!extract(json, "$[0].fileSize", value, sizeof(value)) && !strcmp(value, "13"));
    REQUIRE(!extract(json, "$[0].width", value, sizeof(value)) && !strcmp(value, "1080"));
    REQUIRE(!extract(json, "$[0].height", value, sizeof(value)) && !strcmp(value, "1920")); u_free(json);
    json = utun_bridge_get_channels_json(); REQUIRE(json);
    REQUIRE(!extract(json, "$[0].lastMsgEncrypted", value, sizeof(value)) && !strcmp(value, "1"));
    REQUIRE(!strstr(json, ".uenc")); u_free(json);

    /* Состояния получателя читаются в одной транзакции, затем восстанавливается авторская запись. */
    REQUIRE(sqlite3_exec(instance->topo_sqlite_db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    const struct { const char* state; const char* sql; } states[] = {
        {"download", "UPDATE attachment_keys SET ready=0,downloaded=0,key=NULL"},
        {"requesting", "INSERT INTO attachment_key_requests SELECT channel,message_id,author,0,0,0 FROM attachment_keys"},
        {"pending", "UPDATE attachment_key_requests SET state=1"},
        {"denied", "UPDATE attachment_key_requests SET state=3"},
        {"decrypting", "UPDATE attachment_key_requests SET state=1; UPDATE attachment_keys SET decrypting=1"},
        {"error", "UPDATE attachment_keys SET error=1"},
        {"error", "UPDATE attachment_keys SET error=0,ready=1,info=X'00'"},
        {"download", "UPDATE attachment_keys SET ready=1,decrypting=0,plain_path='/nonexistent/android-private-file'"}
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        REQUIRE(sqlite3_exec(instance->topo_sqlite_db, states[i].sql, NULL, NULL, NULL) == SQLITE_OK);
        json = utun_bridge_get_messages_json(channel, 1, id, 1); REQUIRE(json);
        REQUIRE(!extract(json, "$[0].encryptionState", value, sizeof(value)) && !strcmp(value, states[i].state));
        REQUIRE(!extract(json, "$[0].text", value, sizeof(value)) && !value[0]);
        REQUIRE(!extract(json, "$[0].filePath", value, sizeof(value)) && !value[0]);
        REQUIRE(!extract(json, "$[0].contentType", value, sizeof(value)) && !strcmp(value, ENCRYPTED_ATTACHMENT_TYPE));
        u_free(json);
    }
    REQUIRE(sqlite3_exec(instance->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    printf("[PASS] Android encrypted attachment: private metadata, access states and cache recovery\n");
    return 0;
}

/* JSON адресов для Android сохраняет IPv4/IPv6, протокол, порт, socket_id и тип профиля. */
static int check_addresses(const char *channel) {
    struct TOPO_SOCKMETA4 meta4 = {.id = 5, .config_type = CFG_SERVER_TYPE_PUBLIC};
    struct TOPO_SOCKMETA6 meta6 = {.id = 6, .config_type = CFG_SERVER_TYPE_PUBLIC,
        .address_profile = {ADDRESS_MAP_REAL_IP, ADDRESS_IN_OPEN, ADDRESS_SOURCE_ETCP}};
    struct TOPO_ADDR4 addr4 = {.addr = {194,63,142,140}, .port = 1333, .socket_id = 5, .protocol = TOPO_PROTO_UDP};
    struct TOPO_ADDR6 addr6 = {.addr = {0x2a,0x04,0x52,0,0,0,0,0,0,0,0,0,0,0,0,0xf},
        .port = 443, .socket_id = 6, .protocol = TOPO_PROTO_TCP};
    struct TOPO_NODE node = {.node_id = instance->node_id, .v4_sock_meta = &meta4, .v4_addrs = &addr4,
        .v6_sock_meta = &meta6, .v6_addrs = &addr6};
    memcpy(node.public_key, instance->my_keys.public_key, sizeof(node.public_key));
    REQUIRE(!topo_node_sqlite_addrs_put(instance->topo_sqlite_db, instance->node_id, &node));
    char *json = utun_bridge_get_member_detail_json(channel, instance->my_ed25519_pubkey);
    char value[128];
    REQUIRE(json && !extract(json, "$.adr[0].ad", value, sizeof(value)) && !strcmp(value, "194.63.142.140"));
    REQUIRE(!extract(json, "$.adr[0].po", value, sizeof(value)) && !strcmp(value, "1333"));
    REQUIRE(!extract(json, "$.adr[0].at", value, sizeof(value)) && !strcmp(value, "0")); /* адрес интерфейса, не NAT */
    REQUIRE(!extract(json, "$.adr[0].pt", value, sizeof(value)) && !strcmp(value, "UDP"));
    REQUIRE(!extract(json, "$.adr[0].si", value, sizeof(value)) && !strcmp(value, "5"));
    REQUIRE(!extract(json, "$.adr[1].ad", value, sizeof(value)) && !strcmp(value, "2a04:5200::f"));
    REQUIRE(!extract(json, "$.adr[1].at", value, sizeof(value)) && !strcmp(value, "1"));
    REQUIRE(!extract(json, "$.adr[1].pt", value, sizeof(value)) && !strcmp(value, "TCP"));
    REQUIRE(!extract(json, "$.adr[1].po", value, sizeof(value)) && !strcmp(value, "443"));
    REQUIRE(!extract(json, "$.adr[1].si", value, sizeof(value)) && !strcmp(value, "6"));
    REQUIRE(!extract(json, "$.adr[0].nmapping", value, sizeof(value)) && !strcmp(value, "0"));
    REQUIRE(!extract(json, "$.adr[0].nsource", value, sizeof(value)) && !strcmp(value, "0"));
    u_free(json);
    /* addr_type=0 остаётся тем же, но измеренный REAL_IP должен дойти до GUI отдельно. */
    meta4.address_profile = (struct address_profile){ADDRESS_MAP_REAL_IP, ADDRESS_IN_UNKNOWN, ADDRESS_SOURCE_ETCP};
    REQUIRE(!topo_node_sqlite_addrs_put(instance->topo_sqlite_db, instance->node_id, &node));
    json = utun_bridge_get_member_detail_json(channel, instance->my_ed25519_pubkey);
    REQUIRE(json && !extract(json, "$.adr[0].at", value, sizeof(value)) && !strcmp(value, "0"));
    REQUIRE(!extract(json, "$.adr[0].nmapping", value, sizeof(value)) && !strcmp(value, "1"));
    REQUIRE(!extract(json, "$.adr[0].ninbound", value, sizeof(value)) && !strcmp(value, "0"));
    REQUIRE(!extract(json, "$.adr[0].nsource", value, sizeof(value)) && !strcmp(value, "1"));
    u_free(json);
    meta4.address_profile.mapping = ADDRESS_MAP_UNKNOWN;
    addr4.type = TOPO_ADDR_NAT; meta4.address_profile.source = ADDRESS_SOURCE_ETCP;
    REQUIRE(!topo_node_sqlite_addrs_put(instance->topo_sqlite_db, instance->node_id, &node));
    json = utun_bridge_get_member_detail_json(channel, instance->my_ed25519_pubkey);
    REQUIRE(json && !extract(json, "$.adr[0].at", value, sizeof(value)) && !strcmp(value, "4")); /* NAT, тип неизвестен */
    u_free(json);
    meta4.address_profile.mapping = ADDRESS_MAP_EIM;
    REQUIRE(!topo_node_sqlite_addrs_put(instance->topo_sqlite_db, instance->node_id, &node));
    json = utun_bridge_get_member_detail_json(channel, instance->my_ed25519_pubkey);
    REQUIRE(json && !extract(json, "$.adr[0].at", value, sizeof(value)) && !strcmp(value, "6")); /* EIM? */
    u_free(json); return 0;
}

/* Собственные сокеты передают профиль независимо от адреса интерфейса; тестовые порты скрыты. */
static int check_local_sockets(void) {
    struct ETCP_SOCKET* saved = instance->etcp_sockets;
    struct ETCP_SOCKET tcp = {.sock_id = 7, .is_tcp = 1, .name = "wlan\"😀\\tcp\n",
        .address_profile = {ADDRESS_MAP_UNKNOWN, ADDRESS_IN_UNKNOWN, ADDRESS_SOURCE_ETCP}};
    struct ETCP_SOCKET udp = {.sock_id = 8, .name = "as_wlan0_v6_udp",
        .address_profile = {ADDRESS_MAP_EIM, ADDRESS_IN_OPEN, ADDRESS_SOURCE_ETCP}};
    struct ETCP_SOCKET probe = {.sock_id = 9, .probe_only = 1};
    struct sockaddr_in* local4 = (struct sockaddr_in*)&tcp.interface_addr;
    local4->sin_family = AF_INET; local4->sin_port = htons(9892); inet_pton(AF_INET, "192.168.40.250", &local4->sin_addr);
    struct sockaddr_in* external4 = (struct sockaddr_in*)&tcp.nat_addr;
    external4->sin_family = AF_INET; inet_pton(AF_INET, "194.63.142.140", &external4->sin_addr);
    struct sockaddr_in6* local6 = (struct sockaddr_in6*)&udp.interface_addr;
    local6->sin6_family = AF_INET6; local6->sin6_port = htons(9893); inet_pton(AF_INET6, "fd00::1", &local6->sin6_addr);
    struct sockaddr_in6* external6 = (struct sockaddr_in6*)&udp.nat_addr;
    external6->sin6_family = AF_INET6; external6->sin6_port = htons(4101); inet_pton(AF_INET6, "2a04:5200::f", &external6->sin6_addr);
    tcp.next = &udp; udp.next = &probe; instance->etcp_sockets = &tcp;
    char* json = utun_bridge_get_local_sockets_json();
    instance->etcp_sockets = saved;
    char value[128];
    REQUIRE(json && !extract(json, "$[0].nmapping", value, sizeof(value)) && !strcmp(value, "0"));
    REQUIRE(!extract(json, "$[0].nm", value, sizeof(value)) && !strcmp(value, tcp.name));
    REQUIRE(!extract(json, "$[0].nsource", value, sizeof(value)) && !strcmp(value, "1"));
    REQUIRE(!extract(json, "$[0].ad", value, sizeof(value)) && !strcmp(value, "192.168.40.250"));
    REQUIRE(!extract(json, "$[0].external", value, sizeof(value)) && !strcmp(value, "194.63.142.140"));
    REQUIRE(!extract(json, "$[0].externalPort", value, sizeof(value)) && !strcmp(value, "0"));
    REQUIRE(!extract(json, "$[1].nmapping", value, sizeof(value)) && !strcmp(value, "2"));
    REQUIRE(!extract(json, "$[1].ninbound", value, sizeof(value)) && !strcmp(value, "1"));
    REQUIRE(!extract(json, "$[1].external", value, sizeof(value)) && !strcmp(value, "2a04:5200::f"));
    REQUIRE(!extract(json, "$[1].externalPort", value, sizeof(value)) && !strcmp(value, "4101"));
    REQUIRE(!extract(json, "$[#-1].si", value, sizeof(value)) && !strcmp(value, "8"));
    u_free(json); return 0;
}

/* Android PM ждёт commit, а отказ очереди не создаёт ожидание события; текст сохраняет обычный UTF-8. */
static int check_dm_submissions(const char* directory) {
    struct SC_MYKEYS peer; uint8_t ed[32], seed[32];
    REQUIRE(!sc_identity_generate(peer.public_key, peer.private_key, ed, seed));
    uint64_t node = sc_derive_node_id_from_pubkey(peer.public_key);
    sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(instance->topo_sqlite_db,
        "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey,name) VALUES(?,?,?,'Android PM peer')", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)node);
    sqlite3_bind_blob(st, 2, peer.public_key, 32, SQLITE_STATIC); sqlite3_bind_blob(st, 3, ed, 32, SQLITE_STATIC);
    int rc = sqlite3_step(st); sqlite3_finalize(st); REQUIRE(rc == SQLITE_DONE);
    REQUIRE(!dm_start(instance, node, peer.public_key, ed, "Android PM peer", NULL, DM_MODE_DEVICE, NULL));
    char conv[64], before[8192], after[8192], value[256]; size_t length = 0;
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)dm_derive_conv_id(instance->my_ed25519_pubkey, ed));
    uint64_t generation = dm_history_generation(instance, conv); REQUIRE(generation);
    const uint8_t text[] = "Привет 😀 \"PM\"\nследующая строка";
    REQUIRE(!utun_bridge_dm_send(conv, text, sizeof(text) - 1, generation, 11) && wait_result(11) == 1);
    REQUIRE(!dm_list_messages_json(instance, conv, 20, 0, before, sizeof(before), &length));
    REQUIRE(!extract(before, "$[0].data", value, sizeof(value)) && !strcmp(value, (const char*)text));
    REQUIRE(!utun_bridge_dm_send(conv, text, sizeof(text) - 1, generation + 1, 12) && wait_result(12) == -1);
    REQUIRE(sqlite3_exec(instance->topo_sqlite_db, "CREATE TEMP TRIGGER android_dm_write_fail BEFORE INSERT ON dm_outbox"
        " BEGIN SELECT RAISE(FAIL,'android fixture write failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(!utun_bridge_dm_send(conv, text, sizeof(text) - 1, generation, 13) && wait_result(13) == -1);
    REQUIRE(sqlite3_exec(instance->topo_sqlite_db, "DROP TRIGGER android_dm_write_fail", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(!dm_list_messages_json(instance, conv, 20, 0, after, sizeof(after), &length) && !strcmp(before, after));
    REQUIRE(!utun_bridge_dm_send(conv, text, sizeof(text) - 1, generation, 14) && wait_result(14) == 1);
    const uint8_t embedded_nul[] = {'a', 0, 'b'};
    REQUIRE(utun_bridge_dm_send(conv, embedded_nul, sizeof(embedded_nul), generation, 15) == -1);
    REQUIRE(utun_bridge_dm_send(conv, text, 0, generation, 15) == -1);
    REQUIRE(utun_bridge_dm_send(conv, text, sizeof(text) - 1, 0, 15) == -1);
    REQUIRE(utun_bridge_dm_send(conv, text, sizeof(text) - 1, generation, 0) == -1);
    uint8_t oversized[DM_DATA_MAX + 1] = {0};
    REQUIRE(utun_bridge_dm_send(conv, oversized, sizeof(oversized), generation, 16) == -1);
    struct UTUN_INSTANCE* saved = instance; instance = NULL;
    rc = utun_bridge_dm_send(conv, text, sizeof(text) - 1, generation, 17); instance = saved;
    REQUIRE(rc == -1);
    REQUIRE(utun_bridge_call_start(NULL, ed, 1) == -1 && utun_bridge_call_start("7", NULL, 1) == -1);
    REQUIRE(utun_bridge_call_accept(0) == -1 && utun_bridge_call_decline(0) == -1 && utun_bridge_call_hangup(0) == -1);
    instance = NULL;
    REQUIRE(utun_bridge_call_start("7", ed, 1) == -1 && utun_bridge_call_accept(1) == -1);
    REQUIRE(utun_bridge_call_decline(1) == -1 && utun_bridge_call_hangup(1) == -1);
    instance = saved;
    uasync_poll(async, 0); REQUIRE(!results[15] && !results[16] && !results[17]);
    char path[1024], target[80]; uint8_t no_reply[32] = {0};
    snprintf(path, sizeof(path), "%s/PM source.bin", directory); snprintf(target, sizeof(target), "dm:%s", conv);
    for (int request = 18; request <= 23; request++) {
        FILE* source = fopen(path, "wb"); REQUIRE(source && fwrite("PM attachment", 1, 13, source) == 13 && !fclose(source));
        if (request == 20) {
            REQUIRE(!dm_list_messages_json(instance, conv, 20, 0, before, sizeof(before), &length));
            REQUIRE(sqlite3_exec(instance->topo_sqlite_db, "CREATE TEMP TRIGGER android_dm_media_fail BEFORE INSERT ON dm_outbox"
                " BEGIN SELECT RAISE(FAIL,'android media write failure'); END", NULL, NULL, NULL) == SQLITE_OK);
        }
        if (request == 21) rc = utun_bridge_photo_send(target, path, directory, 24, 24, no_reply, request, "image/jpeg", generation, NULL);
        else if (request == 22) rc = utun_bridge_video_send(target, path, directory, 1000, 24, 24, no_reply, request, generation, NULL);
        else {
            if (request == 23) REQUIRE(!remove(path));
            rc = utun_bridge_dm_send_file(conv, path, "Документ 😀.bin", request == 19 ? generation + 1 : generation, request);
        }
        REQUIRE(!rc && wait_result(request) == (request == 19 || request == 20 || request == 23 ? -1 : 1));
        if (request == 20) {
            REQUIRE(sqlite3_exec(instance->topo_sqlite_db, "DROP TRIGGER android_dm_media_fail", NULL, NULL, NULL) == SQLITE_OK);
            REQUIRE(!dm_list_messages_json(instance, conv, 20, 0, after, sizeof(after), &length) && !strcmp(before, after));
        }
        for (int retry = 0; retry < 500 && access(path, F_OK) == 0; retry++) uasync_poll(async, 20);
        REQUIRE(access(path, F_OK) != 0 && result_counts[request] == 1);
    }
    REQUIRE(utun_bridge_dm_send_file(conv, path, "file.bin", generation, 0) == -1);
    for (int request = 24; request <= 25; request++) {
        REQUIRE(!utun_bridge_voice_start(target, no_reply, request, request == 25 ? generation + 1 : generation, NULL));
        int16_t voice[960] = {0};
        for (int frame = 0; frame < 30; frame++) REQUIRE(!utun_bridge_voice_feed(voice, 960));
        struct timespec pause = {0, 650000000}; nanosleep(&pause, NULL);
        int duration = 0;
        REQUIRE(!utun_bridge_voice_stop(&duration) && duration >= 500);
        REQUIRE(wait_result(request) == (request == 25 ? -1 : 1) && result_counts[request] == 1);
    }
    printf("[PASS] Android PM: UTF-8, commit ACK, stale generation, SQL rollback/retry and rejected requests\n");
    printf("[PASS] Android PM media: file/photo/video/voice commit ACK, SQL rollback, stale generation and cache cleanup\n");
    return 0;
}

static int deny_snapshot(void* target, int action, const char* table, const char* column, const char* db, const char* trigger) {
    (void)column; (void)db; (void)trigger;
    return action == SQLITE_READ && table && !strcmp(table, target) ? SQLITE_DENY : SQLITE_OK;
}
static int deny_publication(void* target, int action, const char* table, const char* column, const char* db, const char* trigger) {
    (void)column; (void)db; (void)trigger;
    return action == SQLITE_INSERT && table && !strcmp(table, target) ? SQLITE_DENY : SQLITE_OK;
}
static int check_snapshot_errors(const char* channel) {
    sqlite3* db = instance->topo_sqlite_db;
    REQUIRE(sqlite3_set_authorizer(db, deny_snapshot, "channels") == SQLITE_OK);
    char* channels = utun_bridge_get_channels_json();
    REQUIRE(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK && !channels);
    char table[80]; msg_table_name(channel, table, sizeof(table));
    REQUIRE(sqlite3_set_authorizer(db, deny_snapshot, table) == SQLITE_OK);
    char* messages = utun_bridge_get_messages_json(channel, 10, NULL, 0);
    REQUIRE(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK && !messages);
    peers_table_name(channel, table, sizeof(table));
    REQUIRE(sqlite3_set_authorizer(db, deny_snapshot, table) == SQLITE_OK);
    char* members = utun_bridge_get_channel_members_json(channel);
    REQUIRE(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK && !members);
    messages = utun_bridge_get_messages_json(channel, 10, NULL, 0);
    channels = utun_bridge_get_channels_json();
    members = utun_bridge_get_channel_members_json(channel);
    REQUIRE(messages && !strcmp(messages, "[]") && channels && strcmp(channels, "[]") && members && strcmp(members, "[]"));
    u_free(messages); u_free(channels); u_free(members);
    printf("[PASS] Android snapshots: SQL denial returns failure, empty history stays valid, retry restores full snapshot\n");
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    char directory[512] = "/tmp/utun_android_replies_XXXXXX", config[1024], channel[65], hex[65];
    REQUIRE(!test_mkdtemp(directory));
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    async = uasync_create(); REQUIRE(async);
    instance = utun_instance_create_from_str(async, config); REQUIRE(instance);
    REQUIRE(!utun_core_start(instance) && !chat_service_start(instance));
    uasync_mark_running(async);
    chat_event_set_handler(instance, event);
    chat_core_create_channel_auto(instance, "Android replies");
    char* json = utun_bridge_get_channels_json();
    REQUIRE(!extract(json, "$[0].id", channel, sizeof(channel))); u_free(json);
    REQUIRE(!check_snapshot_errors(channel));
    REQUIRE(!check_addresses(channel));
    REQUIRE(!check_local_sockets());
    REQUIRE(!check_dm_submissions(directory));
    uint8_t empty[32] = {0}, original[32], missing[32]; memset(missing, 0xff, sizeof(missing));
    REQUIRE(!utun_bridge_send_message(channel, "Привет 😀 \"original\"\nnext line", empty, 1, NULL) && wait_result(1) == 1);
    json = utun_bridge_get_messages_json(channel, 1, NULL, 0);
    REQUIRE(!extract(json, "$[0].messageId", hex, sizeof(hex)) && strlen(hex) == 64);
    REQUIRE(strstr(json, "Привет 😀")); u_free(json);
    for (unsigned i = 0; i < 32; i++) { unsigned value; REQUIRE(sscanf(hex + 2 * i, "%2x", &value) == 1); original[i] = value; }

    /* Оригинал выходит из последнего окна; одинаковые timestamp сортируются по подписи. */
    for (int i = 0; i < 220; i++) {
        char body[32]; snprintf(body, sizeof(body), "History %d", i);
        struct chat_msg_submit req = {0}; req.inst = instance;
        snprintf(req.channel_id, sizeof(req.channel_id), "%s", channel); strcpy(req.content_type, "text/plain");
        req.data = (uint8_t*)body; req.data_len = strlen(body);
        REQUIRE(!chat_core_submit_message(instance, &req));
    }
    json = utun_bridge_get_messages_json(channel, 100, NULL, 0); REQUIRE(!strstr(json, hex)); u_free(json);
    json = utun_bridge_get_messages_json(channel, 100, original, 0); REQUIRE(strstr(json, hex));
    char length[32];
    sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(instance->topo_sqlite_db, "SELECT json_array_length(?)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC); REQUIRE(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 100);
    sqlite3_finalize(st); u_free(json);
    json = utun_bridge_get_messages_json(channel, 1, original, 1);
    REQUIRE(strstr(json, "Привет 😀") && !extract(json, "$[0].replyTo", length, sizeof(length)) && !length[0]); u_free(json);
    json = utun_bridge_get_messages_json(channel, 1, missing, 1); REQUIRE(!strcmp(json, "[]")); u_free(json);
    REQUIRE(!utun_bridge_send_message(channel, "Ordinary reply", original, 26, NULL) && wait_result(26) == 1);
    json = utun_bridge_get_messages_json(channel, 1, NULL, 0);
    REQUIRE(!extract(json, "$[0].replyQuote", length, sizeof(length)) && !length[0]); u_free(json);
    char owned_quote[sizeof(quote_text)]; strcpy(owned_quote, quote_text);
    REQUIRE(!utun_bridge_send_message(channel, "Reply to old message", original, 2, owned_quote));
    memset(owned_quote, 'x', sizeof(owned_quote) - 1); REQUIRE(wait_result(2) == 1);
    REQUIRE(check_reply(channel, hex, "text/plain"));
    REQUIRE(!utun_bridge_send_message(channel, "Missing original", missing, 3, quote_text) && wait_result(3) == -1);
    REQUIRE(check_reply(channel, hex, "text/plain"));
    char oversized[3901]; memset(oversized, 'q', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = 0;
    REQUIRE(!utun_bridge_send_message(channel, "reply exceeds the signed JSON budget", original, 27, oversized) && wait_result(27) == -2);
    REQUIRE(check_reply(channel, hex, "text/plain"));
    REQUIRE(!utun_bridge_send_message(channel, "quote requires original", empty, 28, quote_text) && wait_result(28) == -1);
    REQUIRE(result_counts[27] == 1 && result_counts[28] == 1);

    char file[1024]; snprintf(file, sizeof(file), "%s/source.bin", directory);
    FILE* source = fopen(file, "wb"); REQUIRE(source);
    REQUIRE(fwrite("Android attachment", 1, 18, source) == 18 && fclose(source) == 0);
    REQUIRE(!utun_bridge_attachment_send(channel, file, directory, original, 4, quote_text) && wait_result(4) == 1);
    REQUIRE(check_reply(channel, hex, "application/octet-stream"));
    REQUIRE(access(file, F_OK) != 0);
    source = fopen(file, "wb"); REQUIRE(source && fwrite("Android attachment", 1, 18, source) == 18 && !fclose(source));
    REQUIRE(!utun_bridge_photo_send(channel, file, directory, 24, 24, original, 5, "image/jpeg", 0, quote_text) && wait_result(5) == 1);
    REQUIRE(check_reply(channel, hex, "image/jpeg"));
    REQUIRE(access(file, F_OK) != 0);
    source = fopen(file, "wb"); REQUIRE(source && fwrite("Android attachment", 1, 18, source) == 18 && !fclose(source));
    /* Мост принимает уже подготовленный MP4: содержимое не декодируется повторно. */
    REQUIRE(!utun_bridge_video_send(channel, file, directory, 1000, 24, 24, original, 6, 0, quote_text) && wait_result(6) == 1);
    REQUIRE(check_reply(channel, hex, "video/mp4"));
    REQUIRE(access(file, F_OK) != 0);

    REQUIRE(!utun_bridge_voice_start(channel, original, 7, 0, quote_text));
    int16_t pcm[960] = {0};
    for (int i = 0; i < 30; i++) { REQUIRE(!utun_bridge_voice_feed(pcm, 960)); uasync_poll(async, 20); }
    /* Poll может вернуться немедленно по событию; длительность измеряется CLOCK_MONOTONIC. */
    struct timespec pause = {0, 650000000}; nanosleep(&pause, NULL);
    int duration = 0;
    REQUIRE(!utun_bridge_voice_stop(&duration) && duration >= 500 && wait_result(7) == 1);
    REQUIRE(check_reply(channel, hex, "audio/opus"));
    REQUIRE(!utun_bridge_voice_start(channel, original, 8, 0, quote_text)); utun_bridge_voice_cancel();
    uasync_poll(async, 0); REQUIRE(results[8] == 0);
    REQUIRE(utun_bridge_voice_stop(&duration) < 0);
    REQUIRE(!utun_bridge_attachment_send(channel, "/nonexistent/android-reply", directory, original, 9, quote_text) && wait_result(9) == -1);
    REQUIRE(check_reply(channel, hex, "audio/opus"));

    /* Отказ проекции оставляет READY intent; restart публикует тот же фрагмент вместе с файлом. */
    attachment_recovery_stop(instance);
    char table[80]; snprintf(table, sizeof(table), "msg_%s", channel);
    REQUIRE(sqlite3_set_authorizer(instance->topo_sqlite_db, deny_publication, table) == SQLITE_OK);
    source = fopen(file, "wb"); REQUIRE(source && fwrite("recover quote", 1, 13, source) == 13 && !fclose(source));
    REQUIRE(!utun_bridge_attachment_send(channel, file, directory, original, 29, quote_text) && wait_result(29) == -1);
    REQUIRE(sqlite3_set_authorizer(instance->topo_sqlite_db, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_prepare_v2(instance->topo_sqlite_db,
        "SELECT json_extract(CAST(request AS TEXT),'$.reply_quote') FROM attachment_jobs WHERE owner=1 AND state=2", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), quote_text));
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    chat_service_stop(instance); REQUIRE(!chat_service_start(instance));
    for (int i = 0; i < 500 && !check_reply(channel, hex, "application/octet-stream"); i++) uasync_poll(async, 20);
    REQUIRE(check_reply(channel, hex, "application/octet-stream"));
    REQUIRE(!check_encrypted_attachment(channel, directory, original));
    utun_bridge_destroy();
    utun_instance_destroy(instance); uasync_destroy(async, 0);
    printf("test_android_replies: text, media, voice, results, history and restart PASS\n");
    return 0;
}
