/* Настоящие ядра/SQLite: снимок адресатов, собственные реплики, ACK и выход. */
#include "test_join_id.h"
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_devgroup.h"
#include "chat/member_sync.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "dm/dm_core.h"
#include "dm/dm_crypto.h"
#include "dm/dm_history.h"
#include "dm/dm_media_receipt.h"
#include "media_async/media_crypto.h"
#include "member_files/protocol.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "test_utils.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct UTUN_INSTANCE* nodes[5];
static char channel[65];

static int scalar(int node, const char* sql) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(nodes[node]->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int value = sqlite3_column_int(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return value;
}

static void create_nodes(struct UASYNC* ua, const char* root) {
    for (unsigned i = 0; i < 5; i++) {
        uint8_t device_x[32], device_seed[32]; assert(sc_identity_generate_config(device_x, device_seed) == SC_OK);
        char pub[65], priv[65], config[2048];
        for (unsigned k = 0; k < 32; k++) {
            snprintf(pub + 2 * k, 3, "%02x", device_x[k]);
            snprintf(priv + 2 * k, 3, "%02x", device_seed[k]);
        }
        snprintf(config, sizeof(config), "[global]\nmy_public_key=%s\nmy_private_key=%s\ndb_path=%s/%u\n"
            "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", pub, priv, root, i);
        nodes[i] = utun_instance_create_from_str(ua, config);
        assert(nodes[i] && !utun_core_start(nodes[i]) && !chat_service_start(nodes[i]));
    }
}

/* Общая CHAT-область даёт права маршрутизации; действующих маршрутов специально нет. */
static void seed_channel(void) {
    chat_core_create_channel_auto(nodes[0], "DM group test");
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(nodes[0]->topo_sqlite_db, "SELECT channel_id FROM channels", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(st, 0)); sqlite3_finalize(st);
    uint8_t x[32], ed[32], sig[64], priv[32];
    assert(!topo_node_sqlite_channel_get(nodes[0]->topo_sqlite_db, channel, NULL, 0, NULL, x, ed, sig));
    assert(!topo_node_sqlite_channel_get_priv(nodes[0]->topo_sqlite_db, channel, priv));
    for (unsigned i = 1; i < 5; i++) {
        assert(!topo_node_sqlite_channel_put(nodes[i]->topo_sqlite_db, channel, "DM group test", nodes[0]->my_ed25519_pubkey,
            x, NULL, ed, NULL, sig));
        chat_core_ensure_channel_ready(nodes[i], channel);
    }
    for (unsigned dst = 0; dst < 5; dst++) for (unsigned author = 0; author < 5; author++) {
        uint8_t body[MEMBER_JOIN_MSG_SIZE], join[64], tree[64];
        assert(member_sync_build_join_msg(x, ed, nodes[author]->my_ed25519_pubkey, nodes[author]->my_keys.public_key, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
        assert(sc_ed25519_sign(nodes[author]->my_ed25519_privkey, body, sizeof(body), join) == SC_OK);
        assert(!member_sync_sign_join(priv, channel, NULL, nodes[author]->my_keys.public_key, nodes[author]->my_ed25519_pubkey,
            TEST_JOIN_ID(x, ed, nodes[author]->my_ed25519_pubkey, nodes[author]->my_keys.public_key), 1, tree));
        assert(member_sync_put(nodes[dst], channel, nodes[author]->my_keys.public_key,
            nodes[author]->my_ed25519_pubkey, join, 1, NULL, 0, "", NULL, NULL, 0, 0, tree) >= 0);
    }
}

static void seed_groups(void) {
    for (unsigned i = 0; i < 4; i += 2) {
        assert(!chat_devgroup_rename(nodes[i], i ? "B" : "A"));
        struct devgroup_record r; uint8_t ed[32], x[32], receipt[16] = {0}; receipt[0] = (uint8_t)(i + 1);
        assert(devgroup_load_local(nodes[i], &r, ed, x) == 1);
        assert(!chat_devgroup_import(nodes[i + 1], &r, ed, x, receipt, nodes[i]->my_ed25519_pubkey));
    }
    for (unsigned dst = 0; dst < 5; dst++) for (unsigned src = 0; src < 5; src++) {
        struct TOPO_NODE* n = topo_node_registry_find(nodes[src]->topo_groups, nodes[src]->node_id); assert(n);
        assert(!devgroup_project(nodes[dst]->topo_sqlite_db, n));
    }
}

static void check_members(int node, const char* conv, int own, int peer) {
    uint8_t* data = NULL; size_t bytes = 0; uint64_t generation = dm_history_generation(nodes[node], conv);
    assert(!dm_members_snapshot(nodes[node], conv, generation, &data, &bytes));
    assert(data && data[0] == strlen(conv) && !memcmp(data + 1, conv, data[0]));
    assert(mf_get64(data + 1 + data[0]) == generation);
    unsigned count = data[9 + data[0]] | ((unsigned)data[10 + data[0]] << 8); assert(count == (unsigned)(own + peer));
    assert(bytes == 11 + data[0] + count * DM_MEMBER_WIRE_SIZE);
    unsigned sides[2] = {0};
    for (unsigned i = 0; i < count; i++) {
        const uint8_t* row = data + 11 + data[0] + i * DM_MEMBER_WIRE_SIZE; struct chat_member_display member;
        assert(row[0] <= 1 && !chat_core_deserialize_member(row + 1, &member)); sides[row[0]]++;
        assert(member.name[0] && member.is_self == !memcmp(member.ed25519, nodes[node]->my_ed25519_pubkey, 32));
        assert(member.online == member.is_self); /* Никаких маршрутов: собственный узел online, остальные остаются в панели offline. */
    }
    assert(sides[0] == (unsigned)own && sides[1] == (unsigned)peer); u_free(data);
    assert(dm_members_snapshot(nodes[node], conv, generation + 1, &data, &bytes) < 0 && !data && !bytes);
}

static uint64_t start(int from, int to, enum dm_mode mode, char conv[64]) {
    uint64_t id = 0;
    assert(!dm_start(nodes[from], nodes[to]->node_id, nodes[to]->my_keys.public_key, nodes[to]->my_ed25519_pubkey,
        "peer", channel, mode, &id));
    assert(id); snprintf(conv, 64, "%llu", (unsigned long long)id); return id;
}

static size_t send_body(int from, const char* conv, const char* text, uint8_t body[DM_BODY_MAX]) {
    assert(!dm_send(nodes[from], conv, "text", (const uint8_t*)text, (uint32_t)strlen(text)));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(nodes[from]->topo_sqlite_db,
        "SELECT body FROM dm_outbox WHERE conv_id=? ORDER BY seq DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); assert(sqlite3_step(st) == SQLITE_ROW);
    size_t len = (size_t)sqlite3_column_bytes(st, 0); assert(len && len <= DM_BODY_MAX);
    memcpy(body, sqlite3_column_blob(st, 0), len); sqlite3_finalize(st); return len;
}

/* Четыре участника раскрывают один файл; политика M считает только устройства получателя. */
static void group_file(const char* conv, const char* root) {
    uint8_t data[1024] = {7}, key[32], secret[32], restored[32];
    struct dm_party parties[2];
    assert(!dm_conversation_access(nodes[0], conv, parties, NULL));
    memcpy(data + DM_MEDIA_DESC_SIZE, "DMKEY001", 8);
    uint8_t* keys = data + DM_MEDIA_DESC_SIZE + 8;
    uint64_t id = strtoull(conv, NULL, 10);
    assert(!dm_file_keys_create(id, nodes[0]->my_ed25519_pubkey, data, parties, keys, key));
    for (unsigned i = 0; i < 4; i++) {
        assert(!dm_party_private(nodes[i], parties + i / 2, secret));
        assert(!dm_file_keys_open(id, nodes[0]->my_ed25519_pubkey, data, parties, i / 2, secret, keys, restored));
        assert(!memcmp(key, restored, sizeof(key)));
    }
    char path[1024], cipher[1024];
    snprintf(path, sizeof(path), "%s/0/dm_media/07000000000000000000000000000000.file", root);
    snprintf(cipher, sizeof(cipher), "%s/0/dm_media/07000000000000000000000000000000.cipher", root);
    FILE* plain = fopen(path, "wb+"); FILE* encrypted = fopen(cipher, "wb"); assert(plain && encrypted);
    assert(fwrite("payload", 1, 7, plain) == 7); rewind(plain); uint64_t size;
    assert(!media_encrypt_stream(key, nodes[0]->my_ed25519_pubkey, data, plain, encrypted, &size, data + 24));
    assert(!fclose(plain) && !fclose(encrypted)); memcpy(data + 16, &size, 8);
    struct attachment_info info = {.kind = ATTACHMENT_FILE}; snprintf(info.name, sizeof(info.name), "group.bin");
    int n = attachment_encode(&info, data + DM_MEDIA_DESC_SIZE + DM_MEDIA_KEYS_HEADER_SIZE, ATTACHMENT_WIRE_MAX); assert(n > 0);
    assert(!dm_send(nodes[0], conv, "file", data, DM_MEDIA_DESC_SIZE + DM_MEDIA_KEYS_HEADER_SIZE + n));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(nodes[0]->topo_sqlite_db, "SELECT body FROM dm_files", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); uint8_t body[DM_BODY_MAX], r[DM_MEDIA_RECEIPT_SIZE], ack[DM_RECEIPT_SIZE];
    size_t len = (size_t)sqlite3_column_bytes(st, 0); memcpy(body, sqlite3_column_blob(st, 0), len); sqlite3_finalize(st);
    for (unsigned i = 1; i < 4; i++) assert(!dm_accept_message(nodes[i], body, len, ack));
    struct dm_message_view view; assert(!dm_message_parse(body, len, &view));
    assert(dm_media_user_action(nodes[2], conv, view.author_ed, view.seq, 1) < 0); /* Само наличие сообщения — не открытие. */
    assert(dm_media_receipt_make(nodes[1], body, len, 1, r) < 0); /* Свой узел не удаляет серверный файл. */
    assert(dm_media_receipt_make(nodes[4], body, len, 0, r) < 0);
    int downloaded = -1, opened = -1; sqlite3* db = nodes[0]->topo_sqlite_db;
    assert(!dm_media_retention_done(db, data, 0, &downloaded) && !downloaded);
    assert(!dm_media_retention_done(db, data, 1, &opened) && !opened);
    assert(!dm_media_receipt_make(nodes[2], body, len, 0, r) && !dm_media_receipt_save(nodes[0]->topo_sqlite_db, r, body, len));
    assert(!dm_media_receipt_save(nodes[0]->topo_sqlite_db, r, body, len)); /* Повтор не увеличивает число получателей. */
    assert(!dm_media_retention_done(db, data, 0, &downloaded) && !downloaded);
    assert(!dm_media_retention_done(db, data, 1, &opened) && !opened);
    assert(!dm_media_receipt_make(nodes[2], body, len, 1, r) && !dm_media_receipt_save(nodes[0]->topo_sqlite_db, r, body, len));
    assert(!dm_media_retention_done(db, data, 0, &downloaded) && !downloaded);
    assert(!dm_media_retention_done(db, data, 1, &opened) && opened);
    assert(!dm_media_receipt_make(nodes[3], body, len, 0, r) && !dm_media_receipt_save(nodes[0]->topo_sqlite_db, r, body, len));
    assert(!dm_media_retention_done(db, data, 0, &downloaded) && downloaded);
    assert(scalar(0, "SELECT count(*) FROM dm_file_targets WHERE side=0 AND received IS NULL") == 1);
    r[DM_FILE_FACT_HASH_OFF] ^= 1; assert(dm_media_receipt_save(nodes[0]->topo_sqlite_db, r, body, len) == SQLITE_AUTH);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_DM, DEBUG_LEVEL_DEBUG);
    char root[512] = "/tmp/utun_dm_groups_XXXXXX"; assert(!test_mkdtemp(root));
    struct UASYNC* ua = uasync_create(); assert(ua); create_nodes(ua, root); seed_channel(); seed_groups();
    char conv[64], other[64]; uint64_t group_id = start(0, 2, DM_MODE_GROUPS, conv);
    assert(start(0, 3, DM_MODE_GROUPS, other) == group_id);
    check_members(0, conv, 2, 2);
    uint8_t body[DM_BODY_MAX], reply[DM_BODY_MAX], receipt[DM_RECEIPT_SIZE];
    size_t len = send_body(0, conv, "from A1", body);
    struct dm_message_view view; assert(!dm_message_parse(body, len, &view) && view.target_count == 4 && view.seq == 1);
    assert(scalar(0, "SELECT count(*) FROM dm_delivery") == 3);
    for (int dst = 1; dst <= 3; dst++) {
        assert(!dm_accept_message(nodes[dst], body, len, receipt));
        assert(!dm_accept_receipt(nodes[0], receipt));
        assert(scalar(0, "SELECT count(*) FROM dm_delivery WHERE receipt IS NULL") == 3 - dst);
        assert(scalar(0, "SELECT count(*) FROM dm_outbox") == (dst < 3));
        assert(!dm_accept_message(nodes[dst], body, len, receipt));
        assert(scalar(dst, "SELECT count(*) FROM dm_messages") == 1);
    }
    assert(scalar(1, "SELECT dir FROM dm_messages") == 1 && scalar(1, "SELECT count(*) FROM dm_outbox") == 0);
    assert(scalar(1, "SELECT last_out_seq FROM dm_conversations") == 0);
    check_members(1, conv, 2, 2); check_members(2, conv, 2, 2);
    for (int author = 1; author <= 3; author++) {
        size_t n = send_body(author, conv, "another author", reply);
        assert(!dm_message_parse(reply, n, &view) && view.seq == 1);
        assert(!dm_accept_message(nodes[0], reply, n, receipt));
    }
    assert(scalar(0, "SELECT count(*) FROM dm_messages WHERE seq=1") == 4);
    assert(scalar(0, "SELECT count(*) FROM dm_messages WHERE dir=0") == 2);
    char json[16384]; size_t json_len;
    assert(!dm_list_messages_json(nodes[0], conv, 200, 0, json, sizeof(json), &json_len));
    assert(strstr(json, "from A1") && strstr(json, "another author") && !strstr(json, "\"pending\":true"));
    uint64_t mixed = start(0, 4, DM_MODE_GROUPS, other);
    check_members(0, other, 2, 1);
    assert(mixed != group_id && mixed != start(0, 4, DM_MODE_DEVICE, other));
    size_t n = send_body(0, other, "device to D", reply); assert(!dm_message_parse(reply, n, &view));
    assert(view.mode == DM_MODE_DEVICE && view.seq == 2 && !dm_accept_message(nodes[4], reply, n, receipt));
    assert(start(0, 1, DM_MODE_DEVICE, other) == dm_derive_conv_id(nodes[0]->my_ed25519_pubkey, nodes[1]->my_ed25519_pubkey));
    n = send_body(0, other, "own device", reply); assert(!dm_accept_message(nodes[1], reply, n, receipt));
    group_file(conv, root);
    assert(!chat_devgroup_leave(nodes[1]));
    assert(!devgroup_project(nodes[0]->topo_sqlite_db, topo_node_registry_find(nodes[1]->topo_groups, nodes[1]->node_id)));
    check_members(0, conv, 1, 2);
    assert(scalar(0, "SELECT count(*) FROM dm_file_targets") == 3); /* Текущий состав не меняет delivery snapshot. */
    /* Выход атомарен: ошибка SQL не удаляет секреты и не закрывает беседу. */
    assert(!sqlite3_exec(nodes[0]->topo_sqlite_db, "CREATE TEMP TRIGGER fail_close BEFORE UPDATE OF closed ON dm_conversations"
        " BEGIN SELECT RAISE(ABORT,'injected closure failure'); END", NULL, NULL, NULL));
    assert(chat_devgroup_leave(nodes[0]) < 0 && !dm_conversation_access(nodes[0], conv, NULL, NULL));
    assert(!sqlite3_exec(nodes[0]->topo_sqlite_db, "DROP TRIGGER fail_close", NULL, NULL, NULL));
    send_body(0, conv, "pending at exit", reply);
    assert(!chat_devgroup_leave(nodes[0]));
    check_members(0, conv, 0, 0);
    assert(scalar(0, "SELECT count(*) FROM devgroup_local") == 0);
    assert(scalar(0, "SELECT count(*) FROM dm_outbox o JOIN dm_conversations c USING(conv_id) WHERE c.mode=1") == 0);
    assert(dm_send(nodes[0], conv, "text", (const uint8_t*)"forbidden", 9) < 0);
    n = send_body(2, conv, "after exit", reply); assert(dm_accept_message(nodes[0], reply, n, receipt) < 0);
    assert(!dm_list_messages_json(nodes[0], conv, 200, 0, json, sizeof(json), &json_len) && strstr(json, "from A1"));
    assert(!dm_conversation_access(nodes[0], other, NULL, NULL)); /* DEVICE остаётся действующей. */
    for (unsigned i = 0; i < 5; i++) utun_instance_destroy(nodes[i]);
    uasync_destroy(ua, 0); return 0;
}
