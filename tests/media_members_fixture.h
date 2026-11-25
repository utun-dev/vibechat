/* Минимальная CHAT-топология и результат проверки member_sync для unit-тестов медиа.
 * Не моделирует подписи участников: сетевые тесты используют test_membership.h. */
#ifndef MEDIA_MEMBERS_FIXTURE_H
#define MEDIA_MEMBERS_FIXTURE_H
#include "../src/utun_instance.h"
#include "../src/db_schema.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../src/transport_layer/secure_channel.h"
#include "../src/attachments/store.h"
#include "../src/media_delivery/media_index.h"
#include <openssl/evp.h>
#include <stdlib.h>
#include <stdio.h>

static void media_test_private(uint64_t node, uint8_t private[32]);
static uint8_t* media_test_group_key(uint64_t id, uint8_t key[32]);
static inline const char* media_test_channel(uint64_t group, char out[65]) {
    uint8_t key[32]; return sc_public_key_hex(media_test_group_key(group, key), out);
}

/* Имитирует уже проверенный родитель модели; сырая индексация сама по себе публикацией не является. */
static void media_test_publish_channel(sqlite3* db, const char* channel, uint64_t author, const struct media_index_result* result) {
    int owns = sqlite3_get_autocommit(db);
    if (owns && sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL)) abort();
    struct attachment_object object = {.scope=ATTACHMENT_SCOPE_GROUP,
        .size=(uint64_t)result->file_size, .manifest="transport-fixture", .manifest_size=17};
    struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_CHAT, .message={1}};
    strcpy(object.space, channel); strcpy(ref.space, channel);
    if (topo_node_sqlite_member_keys(db, channel, author, 0, NULL, object.author)) {
        uint8_t seed[32]; media_test_private(author, seed); size_t length = sizeof(object.author);
        EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, sizeof(seed));
        if (!key || EVP_PKEY_get_raw_public_key(key, object.author, &length) != 1 || length != sizeof(object.author)) abort();
        EVP_PKEY_free(key);
    }
    memcpy(object.id, result->media_id, 16); memcpy(object.hash, result->content_hash, 32);
    memcpy(ref.object, result->media_id, 16); memcpy(ref.message+16, result->media_id, 16);
    if (attachment_store_observe_write(db, &object, &ref, NULL) || (owns && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL))) abort();
}

static inline void media_test_publish(sqlite3* db, uint64_t group, uint64_t author, const struct media_index_result* result) {
    char channel[65]; media_test_channel(group, channel);
    media_test_publish_channel(db, channel, author, result);
}

/* Читаемые номера участников стенда отделены от настоящих адресов. Seed — младшие два байта Ed25519 seed.
 * Держатели 1000..1012 упорядочены по вычисленному ID для проверки сортировки при выдаче QUERY. */
static const struct { unsigned slot, seed; uint64_t id; } media_devices[] = {
    {256, 355, UINT64_C(0x4f33770332cb96b6)},
    {257, 3, UINT64_C(0x070f68ca8e4fc107)},
    {258, 9, UINT64_C(0x0abc96cd3da3535c)},
    {512, 12, UINT64_C(0x0b628940888aa022)},
    {768, 4, UINT64_C(0x0c6cb49b36b82dc1)},
    {769, 30, UINT64_C(0x11feb5e8a37f04d4)},
    {770, 18, UINT64_C(0x2137512e4fff6735)},
    {771, 43, UINT64_C(0x250cf26f69351ac8)},
    {772, 36, UINT64_C(0x2649741ed42c441c)},
    {1024, 29, UINT64_C(0x26e4fe5413274e07)},
    {2304, 33, UINT64_C(0x2cfa7e7eb368afd0)},
    {1000, 24, UINT64_C(0x3d184205465b60df)},
    {1001, 20, UINT64_C(0x3dea4d83a436d24e)},
    {1002, 37, UINT64_C(0x52663840b38931d8)},
    {1003, 11, UINT64_C(0x5bde67193a1e389b)},
    {1004, 7, UINT64_C(0x613195a8d9dea655)},
    {1005, 38, UINT64_C(0x67a5472e94417bf7)},
    {1006, 15, UINT64_C(0x6a6a8160b53b5097)},
    {1007, 21, UINT64_C(0x6cd24f95449eb777)},
    {1008, 40, UINT64_C(0x6e27c5a1ec014527)},
    {1009, 19, UINT64_C(0x78f0a5eac89bea13)},
    {1010, 49, UINT64_C(0x7a781bafb314ae07)},
    {1011, 34, UINT64_C(0x7e2b2241d23f5e33)},
    {1012, 52, UINT64_C(0x7f059251ca480329)},
};

static uint64_t media_fixture_id(unsigned slot) {
    for (size_t i = 0; i < sizeof(media_devices) / sizeof(media_devices[0]); i++)
        if (media_devices[i].slot == slot) return media_devices[i].id;
    abort();
}

/* Разные Ed seeds держателей: подпись одного узла не подходит для чужого holder_id. */
static void media_test_private(uint64_t node, uint8_t private[32]) {
    memset(private, 0, 32);
    for (size_t i = 0; i < sizeof(media_devices) / sizeof(media_devices[0]); i++) if (media_devices[i].id == node) {
        private[0] = (uint8_t)media_devices[i].seed; private[1] = (uint8_t)(media_devices[i].seed >> 8); return;
    }
    private[0] = node == 256 ? 1 : 2;
    if (node != 256) for (unsigned i = 0; i < 8; i++) private[8 + i] = (uint8_t)(node >> (8 * i));
    for (unsigned i = 0; i <= UINT8_MAX; i++) {
        uint8_t ed[32]; size_t size = sizeof(ed); private[2] = (uint8_t)i;
        EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private, 32);
        int ok = key && EVP_PKEY_get_raw_public_key(key, ed, &size) == 1 && size == sizeof(ed);
        EVP_PKEY_free(key);
        if (!ok) abort();
        if (!(ed[31] & 0x80)) return;
    }
    abort();
}

static inline uint8_t* media_test_public(uint64_t node, uint8_t ed[32]) {
    uint8_t seed[32], x[32], secret[32]; media_test_private(node, seed);
    if (sc_identity_from_seed(seed, x, secret, ed)) abort();
    return ed;
}

static void media_test_member_keys(struct UTUN_INSTANCE* inst, uint64_t group) {
    const char* channel = media_test_channel(group, (char[65]){0});
    char sql[256]; sqlite3_stmt *read = NULL, *write = NULL;
    snprintf(sql, sizeof(sql), "SELECT node_id FROM peers_%s", channel);
    if (sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &read, NULL) != SQLITE_OK) abort();
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET ed25519_pubkey=?,x25519_pubkey=? WHERE node_id=?", channel);
    if (sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &write, NULL) != SQLITE_OK) abort();
    int rc;
    while ((rc = sqlite3_step(read)) == SQLITE_ROW) {
        uint64_t node = (uint64_t)sqlite3_column_int64(read, 0); uint8_t private[32], public[32]; size_t size = 32;
        media_test_private(node, private);
        EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private, 32);
        if (!key || EVP_PKEY_get_raw_public_key(key, public, &size) != 1 || size != 32) abort();
        EVP_PKEY_free(key);
        uint8_t x[32], secret[32], signing[32];
        if (sc_identity_from_seed(private, x, secret, signing) || memcmp(public, signing, 32)) abort();
        if (node == inst->node_id) { memcpy(inst->my_keys.public_key, x, 32); memcpy(inst->my_keys.private_key, secret, 32); }
        if (sqlite3_bind_blob(write, 2, x, 32, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_blob(write, 1, public, 32, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int64(write, 3, (sqlite3_int64)node) != SQLITE_OK || sqlite3_step(write) != SQLITE_DONE ||
            sqlite3_reset(write) != SQLITE_OK) abort();
        if (node == inst->node_id) { memcpy(inst->my_ed25519_privkey, private, 32); memcpy(inst->my_ed25519_pubkey, public, 32); }
    }
    if (rc != SQLITE_DONE || sqlite3_finalize(read) != SQLITE_OK || sqlite3_finalize(write) != SQLITE_OK) abort();
}

static uint8_t* media_test_group_key(uint64_t id, uint8_t key[32]) {
    memset(key, 0, 32);
    for (int i = 0; i < 8; i++) key[i] = (uint8_t)(id >> (i * 8));
    key[31] = 1;
    return key;
}

static void media_test_group(struct UTUN_INSTANCE* inst, uint64_t id, const uint64_t* peers, size_t count) {
    if (!inst->topo_groups) {
        inst->topo_groups = u_calloc(1, sizeof(*inst->topo_groups));
        if (!inst->topo_groups) abort();
        inst->topo_groups->instance = inst;
        inst->topo_groups->group_list = queue_new(inst->ua, 16, 0, 8, "media_test_groups");
        if (!inst->topo_groups->group_list) abort();
    }
    if (!topo_groups_find(inst->topo_groups, id)) {
        struct TOPO_GROUP* g = (void*)queue_entry_new(sizeof(*g) - sizeof(struct ll_entry));
        if (!g) abort();
        g->instance = inst; g->group_id = id; g->group_type = TOPO_GROUP_TYPE_CHAT;
        media_test_group_key(id, g->wire_id);
        sc_public_key_hex(g->wire_id, g->channel_id);
        sqlite3_stmt* st = NULL;
        if (db_schema_table(inst->topo_sqlite_db, "channels", NULL) ||
            sqlite3_prepare_v2(inst->topo_sqlite_db,
                "INSERT INTO channels(channel_id,description_json,x25519_pubkey,ed25519_pubkey,signature)"
                " VALUES(?, '{\"name\":\"media fixture\"}',?2,?2,zeroblob(64))", -1, &st, NULL) != SQLITE_OK ||
            sqlite3_bind_text(st, 1, g->channel_id, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_blob(st, 2, g->wire_id, 32, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_step(st) != SQLITE_DONE || sqlite3_finalize(st) != SQLITE_OK ||
            queue_data_put_with_index(inst->topo_groups->group_list, &g->ll)) abort();
    }
    const char* channel = media_test_channel(id, (char[65]){0});
    char sql[512];
    snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS peers_%s(node_id INTEGER PRIMARY KEY,record_state INTEGER DEFAULT 0,source INTEGER DEFAULT 0,"
        "deleted INTEGER DEFAULT 0,admitted INTEGER DEFAULT 1,identity_verified INTEGER DEFAULT 1,"
        "x25519_pubkey BLOB DEFAULT(zeroblob(32)),ed25519_pubkey BLOB DEFAULT(zeroblob(32)),"
        "node_type INTEGER DEFAULT 0,adm_tags TEXT DEFAULT '{}')",
        channel);
    if (sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) != SQLITE_OK) abort();
    for (size_t i = 0; i <= count; i++) {
        snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO peers_%s(node_id) VALUES(%lld)",
                 channel, (long long)(i == count ? inst->node_id : peers[i]));
        if (sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) != SQLITE_OK) abort();
    }
    media_test_member_keys(inst, id);
}

/* Только для минимальных групп этого стенда, после остановки media/router. */
static void media_test_groups_free(struct UTUN_INSTANCE* inst) {
    if (!inst->topo_groups) return;
    queue_clear(inst->topo_groups->group_list); queue_free(inst->topo_groups->group_list);
    u_free(inst->topo_groups); inst->topo_groups = NULL;
}
#endif
