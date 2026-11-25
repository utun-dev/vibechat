/* Настоящие DM/SQLite/workers: удаление, повторы, nonce, rollback и поколения. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#include "../src/utun_instance.h"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_crypto.h"
#include "../src/dm/dm_history.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_event.h"
#include "../src/media_async/attachment_send.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../lib/db_async.h"
#include "test_utils.h"

#define REQUIRE(value) do { if (!(value)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history test failed line=%d", __LINE__); return 1; } } while (0)
static int messages, delete_results, delete_success, preparation_failures;
static int snapshots, snapshot_valid;
static uint64_t snapshot_generation;
struct device_identity { uint8_t public_key[32], seed[32]; };

static struct device_identity identities[2];
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    if (type == CHAT_EVT_DM_MSG_RECEIVED) messages++;
    if (type == CHAT_EVT_ATTACHMENT_PREPARATION && len >= 18 && data[len - 1] == 2) preparation_failures++;
    if (type == CHAT_EVT_DM_DELETE_RESULT && len == 25) { delete_results++; delete_success = data[24]; }
    if (type == CHAT_EVT_DM_MESSAGES) {
        snapshots++;
        snapshot_valid = len > 9 + data[0] && data[0] && data[9 + data[0]] == '[';
        if (snapshot_valid) memcpy(&snapshot_generation, data + 1 + data[0], 8);
    }
}

/* Проверяем настоящий trampoline, включая захват поколения и отбрасывание запроса до чтения. */
static int snapshot(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation) {
    struct dm_messages_req* req = u_calloc(1, sizeof(*req));
    if (!req) return -1;
    req->inst = inst; req->generation = generation;
    snprintf(req->conv_id, sizeof(req->conv_id), "%s", conv); dm_messages_trampoline(req);
    unsigned quiet = 0; uint64_t end = get_time_us() + 5000000;
    while (quiet < 2 && get_time_us() < end) {
        uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1;
    }
    if (quiet != 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history test: READ did not complete conv=%s generation=%llu pending=%u",
            conv, (unsigned long long)generation, db_async_pending(inst->db_worker)); return -1;
    }
    return 0;
}

static int scalar(struct UTUN_INSTANCE* inst, const char* sql) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL)) return -1;
    int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st); return n;
}

static int body(struct UTUN_INSTANCE* inst, int seq, uint8_t* out, size_t* len) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT body FROM dm_outbox WHERE seq=?", -1, &st, NULL)) return -1;
    sqlite3_bind_int(st, 1, seq); int rc = sqlite3_step(st);
    int n = rc == SQLITE_ROW ? sqlite3_column_bytes(st, 0) : -1;
    if (n > 0 && (size_t)n <= *len) { memcpy(out, sqlite3_column_blob(st, 0), (size_t)n); *len = (size_t)n; }
    else rc = SQLITE_ERROR;
    sqlite3_finalize(st); return rc == SQLITE_ROW ? 0 : -1;
}

static int peer(struct UTUN_INSTANCE* a, struct UTUN_INSTANCE* b, char conv[64]) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(a->topo_sqlite_db, "INSERT OR REPLACE INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)",
                         -1, &st, NULL)) return -1;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)b->node_id); sqlite3_bind_blob(st, 2, b->my_keys.public_key, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 3, b->my_ed25519_pubkey, 32, SQLITE_STATIC);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if (rc != SQLITE_DONE || dm_start(a, b->node_id, b->my_keys.public_key, b->my_ed25519_pubkey, "peer", NULL, DM_MODE_DEVICE, NULL)) return -1;
    snprintf(conv, 64, "%llu", (unsigned long long)dm_derive_conv_id(a->my_ed25519_pubkey, b->my_ed25519_pubkey)); return 0;
}

static struct UTUN_INSTANCE* start(struct UASYNC* ua, const char* directory, int index) {
    char config[1536], pub[65], priv[65];
    for (unsigned i = 0; i < 32; i++) {
        snprintf(pub + 2 * i, 3, "%02x", identities[index].public_key[i]);
        snprintf(priv + 2 * i, 3, "%02x", identities[index].seed[i]);
    }
    snprintf(config, sizeof(config), "[global]\nmy_public_key=%s\nmy_private_key=%s\ndb_path=%s/%d\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", pub, priv, directory, index);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    if (!inst || utun_core_start(inst) || chat_service_start(inst)) return NULL;
    chat_event_set_handler(inst, event); return inst;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_DM, DEBUG_LEVEL_DEBUG);
    char directory[512] = "/tmp/utun_dm_history_XXXXXX", conv[64], path[768];
    REQUIRE(!test_mkdtemp(directory));
    REQUIRE(!sc_identity_generate_config(identities[0].public_key, identities[0].seed) && !sc_identity_generate_config(identities[1].public_key, identities[1].seed));
    struct UASYNC* ua = uasync_create(); REQUIRE(ua);
    struct UTUN_INSTANCE* a = start(ua, directory, 0); struct UTUN_INSTANCE* b = start(ua, directory, 1);
    REQUIRE(a && b && !peer(a, b, conv) && !peer(b, a, conv));
    REQUIRE(dm_history_generation(b, conv) == 1);
    REQUIRE(!dm_send(a, conv, "text", (const uint8_t*)"one", 3));
    REQUIRE(!dm_send(a, conv, "text", (const uint8_t*)"two", 3));
    uint8_t first[2048], second[2048], file_body[2048], receipt[DM_RECEIPT_SIZE];
    size_t first_len = sizeof(first), second_len = sizeof(second), file_len = sizeof(file_body);
    REQUIRE(!body(a, 1, first, &first_len) && !body(a, 2, second, &second_len));
    REQUIRE(!dm_accept_message(b, second, second_len, receipt)); /* seq=1 ещё не получен. */
    REQUIRE(!dm_send(b, conv, "text", (const uint8_t*)"reply", 5));
    REQUIRE(!snapshot(b, conv, 1) && snapshots == 1 && snapshot_valid && snapshot_generation == 1);
    int before = messages;
    REQUIRE(!sqlite3_exec(b->topo_sqlite_db, "CREATE TEMP TRIGGER fail_delete BEFORE DELETE ON dm_messages"
        " BEGIN SELECT RAISE(ABORT,'injected history delete failure'); END", NULL, NULL, NULL));
    REQUIRE(dm_history_delete(b, conv, 1) < 0 && dm_history_generation(b, conv) == 1);
    REQUIRE(scalar(b, "SELECT count(*) FROM dm_deleted_messages") == 0 && scalar(b, "SELECT count(*) FROM dm_messages") == 2);
    REQUIRE(!sqlite3_exec(b->topo_sqlite_db, "DROP TRIGGER fail_delete", NULL, NULL, NULL));
    struct dm_delete_req* deletion = u_calloc(1, sizeof(*deletion)); REQUIRE(deletion);
    deletion->inst = b; deletion->generation = 1; deletion->request_id = 123; snprintf(deletion->conv_id, 64, "%s", conv);
    dm_delete_trampoline(deletion);
    REQUIRE(delete_results == 1 && delete_success && dm_history_generation(b, conv) == 2);
    REQUIRE(scalar(b, "SELECT count(*) FROM dm_messages") == 0 && scalar(b, "SELECT count(*) FROM dm_outbox") == 0);
    REQUIRE(scalar(b, "SELECT last_out_seq FROM dm_conversations") == 1);
    char json[8192]; size_t length;
    REQUIRE(!dm_list_conversations_json(b, json, sizeof(json), &length) && !strcmp(json, "[]"));
    REQUIRE(!snapshot(b, conv, 0) && snapshots == 1); /* Скрытая история не попадает в GUI. */
    REQUIRE(!dm_accept_message(b, second, second_len, receipt) && messages == before);
    REQUIRE(!dm_verify_receipt(a, receipt, second, second_len));
    REQUIRE(scalar(b, "SELECT count(*) FROM dm_messages") == 0 && scalar(b, "SELECT hidden FROM dm_history") == 1);
    uint8_t conflict[2048]; memcpy(conflict, second, second_len); conflict[16] ^= 1;
    REQUIRE(!sc_ed25519_sign(a->my_ed25519_privkey, conflict, second_len - 64, conflict + second_len - 64));
    REQUIRE(dm_accept_message(b, conflict, second_len, receipt) < 0);
    REQUIRE(!dm_accept_message(b, first, first_len, receipt) && messages == before + 1);
    REQUIRE(scalar(b, "SELECT hidden FROM dm_history") == 0); /* Впервые доставленное seq=1 остаётся новым. */
    REQUIRE(!snapshot(b, conv, 1) && snapshots == 1); /* Старый запрос не читает повторно открытый PM. */
    REQUIRE(!snapshot(b, conv, 2) && snapshots == 2 && snapshot_valid && snapshot_generation == 2);
    puts("PASS: history snapshots carry the exact generation; hidden and obsolete requests publish nothing");
    REQUIRE(!dm_send(b, conv, "text", (const uint8_t*)"after", 5));
    REQUIRE(scalar(b, "SELECT last_out_seq FROM dm_conversations") == 2);
    puts("PASS: atomic delete/rollback, exact replay receipts/conflicts, out-of-order delivery and preserved outgoing seq");

    snprintf(path, sizeof(path), "%s/source.bin", directory);
    FILE* source = fopen(path, "wb"); REQUIRE(source);
    REQUIRE(fwrite("payload", 1, 7, source) == 7 && !fclose(source));
    struct attachment_info info = { .kind = ATTACHMENT_FILE, .name = "source.bin" };
    REQUIRE(!dm_send_file(a, conv, path, &info, 0, 0));
    for (unsigned i = 0; i < 500 && scalar(a, "SELECT count(*) FROM dm_files") == 0; i++) uasync_poll(ua, 20);
    REQUIRE(!body(a, 3, file_body, &file_len) && !dm_accept_message(b, file_body, file_len, receipt));
    REQUIRE(!dm_history_delete(b, conv, 2));
    REQUIRE(!dm_accept_message(b, file_body, file_len, receipt));
    uint8_t saved[DM_MEDIA_RECEIPT_SIZE]; memset(saved, 0x55, sizeof(saved));
    REQUIRE(dm_history_deleted(b, file_body, file_len, 0, saved) == 1 && saved[0] == 0);
    REQUIRE(scalar(b, "SELECT count(*) FROM dm_files") == 0);

    /* Подписанная квитанция загрузки хранится отдельно от квитанции сообщения. */
    REQUIRE(!dm_send_file(a, conv, path, &info, 0, 0));
    for (unsigned i = 0; i < 500 && scalar(a, "SELECT count(*) FROM dm_files") < 2; i++) uasync_poll(ua, 20);
    file_len = sizeof(file_body); REQUIRE(!body(a, 4, file_body, &file_len) && !dm_accept_message(b, file_body, file_len, receipt));
    uint8_t file_receipt[DM_MEDIA_RECEIPT_SIZE];
    memcpy(file_receipt, DM_FILE_RECEIVED_DEVICE, 8); memcpy(file_receipt + 8, file_body, 16); memcpy(file_receipt + DM_FACT_AUTHOR_OFF, a->my_ed25519_pubkey, 32);
    memcpy(file_receipt + DM_FACT_RECIPIENT_OFF, b->my_ed25519_pubkey, 32); memcpy(file_receipt + DM_FILE_FACT_ID_OFF, dm_message_media(file_body, file_len), 16);
    SHA256(file_body, file_len, file_receipt + DM_FILE_FACT_HASH_OFF);
    REQUIRE(!sc_ed25519_sign(b->my_ed25519_privkey, file_receipt, DM_FILE_FACT_SIG_OFF, file_receipt + DM_FILE_FACT_SIG_OFF));
    REQUIRE(!dm_media_verify_receipt(a->topo_sqlite_db, file_receipt, file_body, file_len));
    sqlite3_stmt* complete = NULL;
    REQUIRE(!sqlite3_prepare_v2(b->topo_sqlite_db, "UPDATE dm_files SET receipt=?,ready=1", -1, &complete, NULL));
    sqlite3_bind_blob(complete, 1, file_receipt, sizeof(file_receipt), SQLITE_STATIC);
    REQUIRE(sqlite3_step(complete) == SQLITE_DONE); sqlite3_finalize(complete);
    REQUIRE(!sqlite3_exec(b->topo_sqlite_db, "CREATE TEMP TRIGGER fail_late_delete BEFORE INSERT ON dm_history"
        " BEGIN SELECT RAISE(ABORT,'injected failure after media manifest'); END", NULL, NULL, NULL));
    int remembered = scalar(b, "SELECT count(*) FROM dm_deleted_messages");
    REQUIRE(dm_history_delete(b, conv, 3) < 0 && scalar(b, "SELECT count(*) FROM dm_files") == 1);
    REQUIRE(scalar(b, "SELECT count(*) FROM dm_deleted_messages") == remembered && dm_history_generation(b, conv) == 3);
    REQUIRE(!sqlite3_exec(b->topo_sqlite_db, "DROP TRIGGER fail_late_delete", NULL, NULL, NULL));
    REQUIRE(!dm_history_delete(b, conv, 3));
    REQUIRE(dm_history_deleted(b, file_body, file_len, 0, saved) == 1 && !memcmp(saved, file_receipt, sizeof(saved)));
    REQUIRE(!dm_history_delete(a, conv, 1));
    REQUIRE(!dm_media_accept_receipt(a, file_receipt)); /* Поздний ACK удалённого исходящего файла. */
    file_receipt[DM_FILE_FACT_HASH_OFF] ^= 1;
    REQUIRE(!sc_ed25519_sign(b->my_ed25519_privkey, file_receipt, DM_FILE_FACT_SIG_OFF, file_receipt + DM_FILE_FACT_SIG_OFF));
    REQUIRE(dm_media_accept_receipt(a, file_receipt) < 0);
    puts("PASS: media metadata rollback, retained real file receipts and exact validation of late outgoing receipts");
    REQUIRE(!peer(b, a, conv));
    uint64_t generation = dm_history_generation(b, conv);
    REQUIRE(generation == 4 && !dm_send_file(b, conv, path, &info, 0, 0));
    REQUIRE(!dm_history_delete(b, conv, generation) && !peer(b, a, conv));
    REQUIRE(dm_history_delete(b, conv, generation) < 0); /* Старый запрос не удаляет новую историю. */
    struct dm_send_req* stale = u_calloc(1, sizeof(*stale) + 4); REQUIRE(stale);
    stale->inst = b; stale->generation = generation; stale->data_len = 4; stale->data = (uint8_t*)(stale + 1);
    snprintf(stale->conv_id, sizeof(stale->conv_id), "%s", conv); memcpy(stale->data, "late", 4); dm_send_trampoline(stale);
    for (unsigned i = 0; i < 500 && scalar(b, "SELECT count(*) FROM dm_local_files WHERE busy>0") > 0; i++) uasync_poll(ua, 20);
    REQUIRE(scalar(b, "SELECT count(*) FROM dm_files") == 0 && scalar(b, "SELECT count(*) FROM dm_messages") == 0);
    REQUIRE(!access(path, F_OK)); /* Исходный пользовательский файл не принадлежит задаче. */
    struct attachment_send_req* req = u_calloc(1, sizeof(*req)); REQUIRE(req);
    req->inst = b; req->is_dm = 1; req->dm_generation = dm_history_generation(b, conv); req->info.kind = ATTACHMENT_VOICE;
    req->preset = 1; req->pcm = u_calloc(48000, sizeof(int16_t)); req->pcm_count = 48000; REQUIRE(req->pcm);
    snprintf(req->target, sizeof(req->target), "%s", conv); attachment_send_trampoline(req);
    generation = dm_history_generation(b, conv);
    REQUIRE(!dm_history_delete(b, conv, generation) && !peer(b, a, conv));
    for (unsigned i = 0; i < 500 && scalar(b, "SELECT count(*) FROM dm_local_files WHERE busy>0") > 0; i++) uasync_poll(ua, 20);
    REQUIRE(scalar(b, "SELECT count(*) FROM dm_messages") == 0 && scalar(b, "SELECT count(*) FROM dm_files") == 0);
    int failures_before = preparation_failures;
    req = u_calloc(1, sizeof(*req)); REQUIRE(req);
    req->inst = b; req->is_dm = 1; req->dm_generation = generation; req->info.kind = ATTACHMENT_VOICE;
    snprintf(req->target, sizeof(req->target), "%s", conv); attachment_send_trampoline(req);
    struct dm_file_req* old_file = u_calloc(1, sizeof(*old_file)); REQUIRE(old_file);
    old_file->inst = b; old_file->generation = generation; old_file->info = info;
    snprintf(old_file->conv, sizeof(old_file->conv), "%s", conv); snprintf(old_file->path, sizeof(old_file->path), "%s", path);
    dm_send_file_trampoline(old_file);
    REQUIRE(preparation_failures == failures_before);
    puts("PASS: deleted media replays do not register files or fake download receipts; old file/voice workers cannot send into reopened PM");

    /* Перезапуск с занятой durable записью: владельца уже нет, уборка должна продолжиться. */
    snprintf(path, sizeof(path), "%s/restart-owned.bin", directory);
    source = fopen(path, "wb"); REQUIRE(source); REQUIRE(fputc(42, source) != EOF && !fclose(source));
    uint64_t current = dm_history_generation(b, conv);
    REQUIRE(!dm_media_file_hold(b, conv, current, path));
    REQUIRE(!dm_history_delete(b, conv, current));
    utun_instance_destroy(b);
    b = start(ua, directory, 1); REQUIRE(b);
    REQUIRE(dm_history_generation(b, conv) == current + 1 && !dm_history_current(b, conv, current));
    REQUIRE(!dm_accept_message(b, first, first_len, receipt));
    REQUIRE(scalar(b, "SELECT count(*) FROM dm_messages") == 0 && scalar(b, "SELECT last_out_seq FROM dm_conversations") == 2);
    dm_media_forget_committed(b, conv);
    for (unsigned i = 0; i < 500 && !access(path, F_OK); i++) uasync_poll(ua, 20);
    REQUIRE(access(path, F_OK) != 0);
    REQUIRE(!dm_list_conversations_json(b, json, sizeof(json), &length) && !strcmp(json, "[]"));
    puts("PASS: restart retains replay protection, counters and hidden state; durable file cleanup resumes after owner loss");
    REQUIRE(!peer(b, a, conv) && !dm_send(b, conv, "", (const uint8_t*)"x", 1));
    first_len = sizeof(first); REQUIRE(!body(b, 3, first, &first_len));
    REQUIRE(!dm_history_delete(b, conv, dm_history_generation(b, conv)));
    REQUIRE(dm_history_deleted(b, first, first_len, 1, NULL) == 1);
    REQUIRE(!peer(a, b, conv) && !dm_send(a, conv, "file_ab", (const uint8_t*)"x", 1));
    first_len = sizeof(first); REQUIRE(!body(a, 5, first, &first_len));
    memcpy(first + DM_MSG_FIXED_HDR, "file\0ab", 7);
    REQUIRE(!sc_ed25519_sign(a->my_ed25519_privkey, first, first_len - 64, first + first_len - 64));
    REQUIRE(!dm_accept_message(b, first, first_len, receipt));
    REQUIRE(!dm_history_delete(b, conv, dm_history_generation(b, conv)));
    REQUIRE(dm_history_deleted(b, first, first_len, 0, NULL) == 1);
    puts("PASS: replay hashes preserve exact protocol bytes for empty and embedded-NUL content types");
    utun_instance_destroy(b); utun_instance_destroy(a); uasync_destroy(ua, 0);
    puts("test_dm_history: ALL PASS");
    return 0;
}
