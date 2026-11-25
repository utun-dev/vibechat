#include "test_utils.h"
/* Старый формат отклоняется без сброса; новая история сохраняет версии после restart. */
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_feedback.h"
#include "../src/chat/db_sync.h"
#include "../src/routing_layer/topo_group.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void execute(sqlite3* db, const char* sql) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "schema SQL=%s: %s", sql, sqlite3_errmsg(db));
    assert(rc == SQLITE_OK);
}
static sqlite3_int64 number(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    sqlite3_int64 value = sqlite3_column_int64(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return value;
}
static struct chat_msg_submit send_text(struct UTUN_INSTANCE* inst, const char* channel, const char* text, const uint8_t* reply) {
    struct chat_msg_submit req = {.inst = inst, .data = (uint8_t*)text, .data_len = strlen(text)};
    snprintf(req.channel_id, sizeof(req.channel_id), "%s", channel); strcpy(req.content_type, "text/plain");
    if (reply) memcpy(req.reply_to, reply, 32);
    assert(chat_core_submit_message(inst, &req) == 0); return req;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_INFO);
    struct UASYNC* ua = uasync_create(); assert(ua);
    const char* config =
        "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n";
    char directory[512]="/tmp/utun-history-XXXXXX",file_config[2048]; assert(!test_mkdtemp(directory));
    snprintf(file_config,sizeof(file_config),"[global]\ndb_path=%s\n%s",directory,config);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, file_config); assert(inst && utun_core_start(inst) == 0);
    assert(chat_service_start(inst) == 0); chat_core_create_channel_auto(inst, "Versioned history");
    sqlite3* db = inst->topo_sqlite_db; sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT channel_id FROM channels", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    char channel[65], table[72], sql[2048]; snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(st, 0)); sqlite3_finalize(st);
    snprintf(table, sizeof(table), "msg_%s", channel);
    struct chat_msg_submit first = send_text(inst, channel, "До правки", NULL);
    snprintf(sql, sizeof(sql), "SELECT ver FROM \"%s\" WHERE id=1", table);
    uint64_t first_ver = (uint64_t)number(db, sql);
    struct chat_msg_submit reply = send_text(inst, channel, "Ответ", first.message_id);
    uint8_t reaction[] = {1, 0};
    assert(chat_feedback_set(inst, channel, first.message_id, CHAT_FEEDBACK_REACTION, reaction, sizeof(reaction), NULL) == 1);
    snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET local_attrs='saved',flags=1,delivered_peers=2,delivery_chain='peer' WHERE id=1", table);
    execute(db, sql);
    assert(chat_core_edit_message(inst, channel, first.message_id, first_ver, "После правки 👍", strlen("После правки 👍")) == 0);
    assert(chat_core_count(inst, channel) == 2);
    snprintf(sql, sizeof(sql), "SELECT ver FROM \"%s\" WHERE id=1", table); assert(number(db, sql) == (sqlite3_int64)first_ver + 3);
    snprintf(sql, sizeof(sql), "SELECT timestamp FROM \"%s\" WHERE id=1", table); assert(number(db, sql) == (sqlite3_int64)first.timestamp);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM \"%s\" WHERE id=1 AND local_attrs='saved' AND delivered_peers=0 AND delivery_chain=''", table);
    assert(number(db, sql) == 1);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM \"%s\" r JOIN \"%s\" o ON lower(hex(o.message_id))=json_extract(r.data,'$.reply_to')", table, table);
    assert(number(db, sql) == 1);
    struct chat_feedback_record feedback;
    assert(chat_feedback_get(inst->topo_sqlite_db, channel, first.message_id, inst->my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &feedback) == 1);
    assert(chat_core_edit_message(inst, channel, first.message_id, first_ver, "Stale", 5) == -2);
    assert(chat_core_edit_message(inst, channel, first.message_id, first_ver + 3, "После правки 👍", strlen("После правки 👍")) == 0);
    snprintf(sql, sizeof(sql), "SELECT ver FROM \"%s\" WHERE id=1", table); assert(number(db, sql) == (sqlite3_int64)first_ver + 3);
    char json[8192]; size_t bytes;
    assert(chat_core_get_messages_json(inst, channel, 10, 0, json, sizeof(json), &bytes) == 0);
    char revision[48]; snprintf(revision, sizeof(revision), "\"ver\":%llu", (unsigned long long)(first_ver + 3));
    assert(strstr(json, revision) && strstr(json, "\"edited_at\":") && strstr(json, "После правки"));
    chat_service_stop(inst); assert(chat_service_start(inst) == 0);
    assert(chat_core_count(inst, channel) == 2);
    assert(chat_core_edit_message(inst, channel, first.message_id, first_ver + 3, "После перезапуска", strlen("После перезапуска")) == 0);
    snprintf(sql, sizeof(sql), "SELECT ver FROM \"%s\" WHERE id=1", table); assert(number(db, sql) == (sqlite3_int64)first_ver + 4);
    assert(chat_core_edit_message(inst, channel, reply.message_id, 1, "", 0) == -1);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS edit preserves IDs, timestamp, local attrs, replies, reactions; rejects stale editor and survives restart");

#define LEGACY_CHANNEL "9191919191919191919191919191919191919191919191919191919191919191"
    /* Старые таблицы и feedback сохраняются при каждом отказе, преобразований нет. */
    assert(topo_groups_create_group(inst->topo_groups, 9001, TOPO_GROUP_TYPE_CHAT, LEGACY_CHANNEL));
    execute(db, "CREATE TABLE msg_" LEGACY_CHANNEL "(timestamp INTEGER,author_signature BLOB,data BLOB);"
        "INSERT INTO msg_" LEGACY_CHANNEL " VALUES(1,zeroblob(64),'old');");
    assert(chat_feedback_ensure_channel(inst->topo_sqlite_db, LEGACY_CHANNEL) == 0);
    execute(db, "INSERT INTO feedback_" LEGACY_CHANNEL " VALUES(zeroblob(32),0,zeroblob(32),zeroblob(32),1,1,x'0100',zeroblob(64));");
    for (unsigned i = 0; i < 2; i++) {
        assert(!db_sync_instance_add(inst, "msg_" LEGACY_CHANNEL "", 9001));
        assert(sqlite3_get_autocommit(db) && number(db, "SELECT count(*) FROM msg_" LEGACY_CHANNEL " WHERE data='old'") == 1);
        assert(number(db, "SELECT count(*) FROM feedback_" LEGACY_CHANNEL "") == 1);
        assert(number(db, "SELECT count(*) FROM sqlite_master WHERE name IN ('history_msg_" LEGACY_CHANNEL "','archive_msg_" LEGACY_CHANNEL "')") == 0);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS old group format rejected intact, without clearing history or feedback");
    utun_instance_destroy(inst); uasync_poll(ua, 0);
    assert(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0); puts("ALL PASS: group message revisions and history format"); return 0;
}
