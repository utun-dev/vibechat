/* Настоящие DM/SQLite: целые UTF-8 имена при создании, загрузке, fallback и restart. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/utun_instance.h"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_crypto.h"
#include "../src/chat/chat_core.h"
#include "../lib/debug_config.h"
#include "../lib/json_flat.h"
#include "../lib/mem.h"
#include "test_utils.h"

#define EXPECT(condition) do { if (!(condition)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM name test failed line=%d", __LINE__); return -1; \
} } while (0)
static unsigned cases, snapshots;

struct device_identity { uint8_t public_key[32], seed[32]; };

static struct UTUN_INSTANCE* start(struct UASYNC* ua, const char* directory, const struct device_identity* identity) {
    char config[1536], pub[65], priv[65];
    for (unsigned i = 0; i < 32; i++) {
        snprintf(pub + 2 * i, 3, "%02x", identity->public_key[i]);
        snprintf(priv + 2 * i, 3, "%02x", identity->seed[i]);
    }
    snprintf(config, sizeof(config), "[global]\nmy_public_key=%s\nmy_private_key=%s\ndb_path=%s\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", pub, priv, directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    if (inst && (utun_core_start(inst) || chat_service_start(inst))) { utun_instance_destroy(inst); return NULL; }
    return inst;
}

static int stored(sqlite3* db, const char* conv, const char* expected, size_t len) {
    sqlite3_stmt* st = NULL;
    EXPECT(sqlite3_prepare_v2(db, "SELECT CAST(peer_name AS BLOB) FROM dm_conversations WHERE conv_id=?", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    int exact = rc == SQLITE_ROW && (size_t)sqlite3_column_bytes(st, 0) == len &&
                (!len || !memcmp(sqlite3_column_blob(st, 0), expected, len));
    sqlite3_finalize(st);
    EXPECT(exact);
    return 0;
}

static int replace(sqlite3* db, const char* conv, const char* name, size_t len) {
    sqlite3_stmt* st = NULL;
    EXPECT(sqlite3_prepare_v2(db, "UPDATE dm_conversations SET peer_name=? WHERE conv_id=?", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, name, (int)len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, conv, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    EXPECT(rc == SQLITE_DONE);
    return 0;
}

/* Точные ожидаемые строки проверяются здесь; полный JSON отдельно сохраняется для строгого внешнего парсера. */
static int snapshot(struct UTUN_INSTANCE* inst, const char* conv, const char* expected, FILE* file) {
    char json[65536], token[96], member[1024], actual[256]; size_t len = 0;
    int changes = sqlite3_total_changes(inst->topo_sqlite_db);
    EXPECT(!dm_list_conversations_json(inst, json, sizeof(json), &len));
    EXPECT(len == strlen(json) && sqlite3_total_changes(inst->topo_sqlite_db) == changes);
    snprintf(token, sizeof(token), "\"conv_id\":\"%s\"", conv);
    const char* row = strstr(json, token);
    const char* name = row ? strstr(row, "\"peer_name\":") : NULL;
    EXPECT(name);
    /* json_flat_get останавливается после первого ключа, до числовых полей снимка. */
    snprintf(member, sizeof(member), "{%s", name);
    EXPECT(!json_flat_get(member, "peer_name", actual, sizeof(actual)) && !strcmp(actual, expected));
    EXPECT(fwrite(json, 1, len, file) == len && fputc('\n', file) != EOF);
    snapshots++;
    return 0;
}

static int check_case(struct UTUN_INSTANCE* inst, const char* input, size_t len, const char* expected,
                      FILE* file, char last[64]) {
    struct SC_MYKEYS peer; uint8_t ed[32], seed[32];
    EXPECT(!sc_identity_generate(peer.public_key, peer.private_key, ed, seed));
    uint64_t id = sc_derive_node_id_from_pubkey(peer.public_key);
    snprintf(last, 64, "%llu", (unsigned long long)dm_derive_conv_id(inst->my_ed25519_pubkey, ed));
    sqlite3* db = inst->topo_sqlite_db;
    sqlite3_stmt* st = NULL;
    EXPECT(sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey,name) VALUES(?,?,?,?)",
                            -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
    sqlite3_bind_blob(st, 2, peer.public_key, 32, SQLITE_STATIC); sqlite3_bind_blob(st, 3, ed, 32, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, input, (int)len, SQLITE_STATIC);
    int rc = sqlite3_step(st); sqlite3_finalize(st); EXPECT(rc == SQLITE_DONE);
    EXPECT(!dm_start(inst, id, peer.public_key, ed, input, NULL, DM_MODE_DEVICE, NULL));
    EXPECT(!stored(db, last, expected, strlen(expected)) && !snapshot(inst, last, expected, file));

    /* Старый SQL может содержать полное длинное имя, оборванный UTF-8 или NUL внутри TEXT. */
    EXPECT(!replace(db, last, input, len));
    EXPECT(!snapshot(inst, last, expected, file) && !stored(db, last, input, len));
    char messages[8192]; size_t bytes = 0;
    int changes = sqlite3_total_changes(db);
    EXPECT(!dm_list_messages_json(inst, last, 10, 0, messages, sizeof(messages), &bytes) && !strcmp(messages, "[]"));
    EXPECT(sqlite3_total_changes(db) == changes && !stored(db, last, input, len));
    EXPECT(!dm_send(inst, last, "text", (const uint8_t*)"one", 3));
    EXPECT(!stored(db, last, expected, strlen(expected)) && !snapshot(inst, last, expected, file));

    /* Пустое сохранённое имя разрешается через nodes: этот путь тоже обязан выдавать целый UTF-8. */
    EXPECT(!replace(db, last, "", 0) && !snapshot(inst, last, expected, file) && !stored(db, last, "", 0));
    /* Убрать повреждение фикстуры: следующие снимки не должны повторять предупреждения прошлых случаев. */
    EXPECT(!replace(db, last, expected, strlen(expected)));
    EXPECT(sqlite3_prepare_v2(db, "UPDATE nodes SET name=? WHERE node_id=?", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, expected, -1, SQLITE_STATIC); sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
    rc = sqlite3_step(st); sqlite3_finalize(st); EXPECT(rc == SQLITE_DONE);
    cases++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM name test: case=%u input=%zu kept=%zu create/load/save/fallback PASS",
               cases, len, strlen(expected));
    return 0;
}

static int run(struct UTUN_INSTANCE* inst, FILE* file, char last[64], char expected[256]) {
    char input[512];
    const size_t lengths[] = {0, 1, 62, 63, 64, 127};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        size_t len = lengths[i], keep = len < 63 ? len : 63;
        memset(input, 'A', len); input[len] = 0; memset(expected, 'A', keep); expected[keep] = 0;
        EXPECT(!check_case(inst, input, len, expected, file, last));
    }
    const char* points[] = {"\xd0\xaf", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "\xf4\x8f\xbf\xbf"};
    for (size_t p = 0; p < sizeof(points) / sizeof(points[0]); p++) {
        size_t width = strlen(points[p]);
        for (size_t prefix = 58; prefix <= 64; prefix++) {
            memset(input, 'a', prefix); memcpy(input + prefix, points[p], width); input[prefix + width] = 'z';
            input[prefix + width + 1] = 0;
            size_t keep = prefix < 63 ? prefix : 63;
            memcpy(expected, input, keep);
            if (prefix + width <= 63) {
                memcpy(expected + keep, points[p], width); keep += width;
                if (keep < 63) expected[keep++] = 'z';
            }
            expected[keep] = 0;
            EXPECT(!check_case(inst, input, prefix + width + 1, expected, file, last));
        }
        for (size_t count = 63 / width; count <= 63 / width + 1; count++) {
            for (size_t n = 0; n < count; n++) memcpy(input + n * width, points[p], width);
            input[count * width] = 0;
            size_t keep = (count * width <= 63 ? count : 63 / width) * width;
            memcpy(expected, input, keep); expected[keep] = 0;
            EXPECT(!check_case(inst, input, count * width, expected, file, last));
        }
    }
    const char* invalid[] = {"\x80", "\xc0\xaf", "\xe0\x80\x80", "\xf0\x80\x80\x80", "\xed\xa0\x80",
                             "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff", "\xc2", "\xe2\x82", "\xf0\x9f\x98", "\xd0q"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        for (size_t prefix = 0; prefix <= 2; prefix += 2) {
            memset(input, 'a', prefix); memcpy(input + prefix, invalid[i], strlen(invalid[i]) + 1);
            memset(expected, 'a', prefix); expected[prefix] = 0;
            EXPECT(!check_case(inst, input, prefix + strlen(invalid[i]), expected, file, last));
        }
    }
    memcpy(input, "OK\0hidden", 9); input[9] = 0; snprintf(expected, 256, "OK");
    EXPECT(!check_case(inst, input, 9, expected, file, last));
    snprintf(input, sizeof(input), "quote\" slash\\ line\n%s", points[0]); memcpy(expected, input, strlen(input) + 1);
    EXPECT(!check_case(inst, input, strlen(input), expected, file, last));
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_DM, getenv("UTUN_TEST_DEBUG") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    size_t allocations = u_get_allocated_count();
    char directory[512] = "/tmp/utun_dm_names_XXXXXX", path[768], last[64], expected[256];
    struct UASYNC* ua = NULL; struct UTUN_INSTANCE* inst = NULL; FILE* file = NULL;
    struct device_identity identity; int failed = 1;
    if (test_mkdtemp(directory) || sc_identity_generate_config(identity.public_key, identity.seed) || !(ua = uasync_create())) goto done;
    if (!(inst = start(ua, directory, &identity))) goto done;
    snprintf(path, sizeof(path), "%s/names.jsonl", directory); file = fopen(path, "wb");
    if (!file || run(inst, file, last, expected)) goto done;
    utun_instance_destroy(inst); inst = start(ua, directory, &identity);
    if (!inst || snapshot(inst, last, expected, file)) goto done;
    failed = 0;
done:
    if (failed) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM name test failed after cases=%u snapshots=%u", cases, snapshots);
    if (file && fclose(file)) failed = 1;
    if (inst) utun_instance_destroy(inst);
    if (ua) uasync_destroy(ua, 0);
    if (allocations != u_get_allocated_count()) failed = 1;
    printf("test_dm_names: cases=%u snapshots=%u allocations=%zu->%zu %s artifacts=%s\n",
           cases, snapshots, allocations, u_get_allocated_count(), failed ? "FAIL" : "PASS", directory);
    return failed;
}
