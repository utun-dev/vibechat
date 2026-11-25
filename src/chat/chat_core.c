/*
 * chat_core.c — жизненный цикл чата: init, destroy, per-instance состояние (inst->chat_core)
 *
 * Под-модули (каждый в своём .c):
 *   chat_msg.c     — отправка сообщений, DB-операции для chat_sync
 *   chat_channel.c — создание/подключение каналов, db_sync instance
 *   chat_profile.c — имя узла, адреса, ui_state
 *   chat_status.c  — сбор статуса NTP+connections
 *
 * Runtime и сеть принадлежат uasync. SQL-only снимки выполняются DB worker;
 * синхронные JSON helpers остаются для RAM/диагностики в потоке ядра.
 */

#include "chat_core_priv.h"
#include "../member_files/protocol.h"
#include "../group/group_protocol.h"
#include "../db_schema.h"
#include "group_policy.h"
#include "chat_devgroup.h"
#include "chat_clipboard.h"
#include "chat_local.h"
#include "chat_whisper.h"
#include "chat_attachment.h"
#include "group_avatar.h"
#include "chat_event.h"
#include "chat_setting.h"
#include "chat_state.h"
#include "chat_member.h"
#include "member_sync.h"
#include "chat_sync.h"
#include "invite_build.h"

#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../transport_layer/secure_channel.h"
#include "../media_delivery/media_index.h"
#include "../../lib/mem.h"
#include "../../lib/strbuf.h"
#include "../../lib/json_flat.h"
#include "../../lib/db_async.h"

#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "../../lib/platform_compat.h"

/* ─── хелперы ─── */

void sanitize_ch_id(const char* ch_id, char* out, size_t out_sz) {
    size_t i = 0;
    while (*ch_id && i < out_sz - 1) {
        char c = *ch_id++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_')
            out[i++] = c;
        else
            out[i++] = '_';
    }
    out[i] = '\0';
}

void msg_table_name(const char* ch_id, char* buf, size_t sz) {
    char san[65]; sanitize_ch_id(ch_id, san, sizeof(san));
    snprintf(buf, sz, "history_msg_%s", san);
}
void peers_table_name(const char* ch_id, char* buf, size_t sz) {
    char san[65]; sanitize_ch_id(ch_id, san, sizeof(san));
    snprintf(buf, sz, "peers_%s", san);
}

static int db_exec(struct chat_core_ctx* cc, const char* sql) {
    char* err = NULL;
    int rc = sqlite3_exec(cc->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: SQL rc=%d (%s) statement=%.120s: %s", CC_ID,
            rc, sqlite3_errstr(rc), sql, err ? err : sqlite3_errmsg(cc->db));
        sqlite3_free(err);
    }
    return rc;
}

/* ─── жизненный цикл ─── */

struct chat_core_bootstrap {
    uint8_t ed25519[32];
    char path[512], media_base[MEDIA_BASE_MAX];
    char (*channels)[65];
    int channel_count;
};
void chat_core_bootstrap_free(struct chat_core_bootstrap* prepared) {
    if (!prepared) return; u_free(prepared->channels); u_free(prepared);
}
int chat_core_prepare(sqlite3* db, const struct chat_core_bootstrap_input* input, struct chat_core_bootstrap** out) {
    if (out) *out = NULL;
    if (!db || sqlite3_get_autocommit(db) || !input || !out || !input->node_id || !input->now_sec ||
        input->now_sec > INT64_MAX || !input->db_path || strlen(input->db_path) >= 512 ||
        (input->name && strlen(input->name) >= 64)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat SQL bootstrap requires caller WRITE/frozen identity/path"); return SQLITE_MISUSE;
    }
    struct chat_core_bootstrap* prepared = u_calloc(1, sizeof(*prepared));
    if (!prepared) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat SQL bootstrap allocation failed"); return SQLITE_NOMEM; }
    memcpy(prepared->ed25519, input->ed25519, sizeof(prepared->ed25519)); snprintf(prepared->path, sizeof(prepared->path), "%s", input->db_path);
    const char* stage = "avatar/media schema"; int rc = SQLITE_OK; sqlite3_stmt* st = NULL;
    char** channels = NULL; int rows = 0, columns = 0;
    if (group_avatar_schema_init(db) < 0 || media_index_init(db)) { rc = SQLITE_ERROR; goto done; }
    stage = "chat schema";
    const char* tables[] = {"local_identity", "accounts", "ui_state"};
    for (unsigned i = 0; i < 3; i++) if (db_schema_table(db, tables[i], NULL)) { rc = SQLITE_ERROR; goto done; }
    stage = "local node";
    const char* name = input->name && input->name[0] ? input->name : "Me";
    if (topo_node_sqlite_node_update_verified(db, input->node_id, name, input->x25519, input->ed25519,
        input->now_sec, (time_t)input->now_sec) < 0) { rc = SQLITE_ERROR; goto done; }
    stage = "local identity";
    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO local_identity(id,name,x25519_pubkey,ed25519_pubkey,created_at,updated_at)"
        " VALUES(1,?,?,?,?,?)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, input->x25519, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, input->ed25519, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)input->now_sec);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 5, (sqlite3_int64)input->now_sec);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) rc = sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT;
    { int final = sqlite3_finalize(st); st = NULL; if (rc == SQLITE_OK) rc = final; }
    if (rc) goto done;
    stage = "media base";
    const char* slash = strrchr(input->db_path, '/');
    if (slash) snprintf(prepared->media_base, sizeof(prepared->media_base), "%.*s",
        slash == input->db_path ? 1 : (int)(slash - input->db_path), input->db_path);
    else strcpy(prepared->media_base, ".");
    rc = sqlite3_prepare_v2(db, "SELECT value FROM ui_state WHERE key='media_base'", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int exists = rc == SQLITE_ROW, final = sqlite3_finalize(st); st = NULL;
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) rc = final;
    if (rc) goto done;
    rc = exists ? media_index_base_read(db, prepared->media_base) : media_index_base_set(db, prepared->media_base);
    if (rc) goto done;
    if (!exists) { rc = media_index_base_read(db, prepared->media_base); if (rc) goto done; }
    stage = "channel catalog";
    rc = sqlite3_get_table(db, "SELECT channel_id,length(CAST(channel_id AS BLOB)) FROM channels ORDER BY created_at ASC",
        &channels, &rows, &columns, NULL);
    if (rc) goto done;
    if ((rows && columns != 2) || rows < 0 || (uint64_t)rows * sizeof(*prepared->channels) > UINT32_MAX) { rc = SQLITE_CORRUPT; goto done; }
    if (rows) {
        prepared->channels = u_malloc((uint32_t)rows * sizeof(*prepared->channels));
        if (!prepared->channels) { rc = SQLITE_NOMEM; goto done; }
    }
    for (int i = 0; i < rows; i++) {
        const char *channel = channels[(size_t)(i + 1) * columns], *bytes = channels[(size_t)(i + 1) * columns + 1];
        if (!chat_channel_valid(channel) || !bytes || strlen(channel) != strtoul(bytes, NULL, 10)) {
            rc = SQLITE_CORRUPT; goto done;
        }
        strcpy(prepared->channels[i], channel);
    }
    prepared->channel_count = rows;
done:
    sqlite3_free_table(channels);
    { int final = sqlite3_finalize(st); if (rc == SQLITE_OK) rc = final; }
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat SQL bootstrap failed stage=%s rc=%d: %s", stage, rc, sqlite3_errstr(rc));
        chat_core_bootstrap_free(prepared); return rc;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "chat SQL bootstrap staged node=%016llx channels=%d media_base=%s; commit pending",
        (unsigned long long)input->node_id, rows, prepared->media_base);
    *out = prepared; return SQLITE_OK;
}
static struct chat_core_ctx* chat_core_allocate(struct UTUN_INSTANCE* inst, const char* path) {
    if (inst->group_generation == UINT64_MAX || strlen(path) >= sizeof(((struct chat_core_ctx*)0)->db_path)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat runtime generation/path exhausted"); return NULL;
    }
    struct chat_core_ctx* cc = u_calloc(1, sizeof(*cc));
    if (!cc) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat runtime allocation failed"); return NULL; }
    inst->group_generation++; inst->chat_core = cc;
    strcpy(cc->db_path, path); cc->inst = inst; cc->db = inst->topo_sqlite_db;
    return cc;
}
static void chat_core_install(struct UTUN_INSTANCE* inst, const struct chat_core_bootstrap* prepared) {
    struct chat_core_ctx* cc = CC(inst);
    snprintf(cc->media_base, sizeof(cc->media_base), "%s", prepared->media_base);
    if (inst->config) memcpy(&inst->chat_settings, &inst->config->global.chat_settings, sizeof(inst->chat_settings));
    cc->initialized = 1;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "chat runtime activated node=%016llx channels=%d generation=%llu",
        (unsigned long long)inst->node_id, prepared->channel_count, (unsigned long long)inst->group_generation);
}
int chat_core_activate(struct UTUN_INSTANCE* inst, const struct chat_core_bootstrap* prepared) {
    if (!inst || !inst->core_started || !prepared || memcmp(prepared->ed25519, inst->my_ed25519_pubkey, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat activation requires running core/committed identity"); return -1;
    }
    if (CC(inst)) return CC(inst)->initialized && !strcmp(CC(inst)->db_path, prepared->path) ? 0 : -1;
    struct chat_core_ctx* cc = chat_core_allocate(inst, prepared->path); if (!cc) return -1;
    if (chat_member_init(inst)) { inst->chat_core = NULL; u_free(cc); return -1; }
    chat_core_install(inst, prepared); return 0;
}
const char* chat_core_media_base(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL; return cc && cc->initialized ? cc->media_base : NULL;
}
void chat_core_publish_ready(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || cc->readiness_published) return;
    cc->readiness_published = 1;
    chat_event_post(inst, CHAT_EVT_DB_READY, NULL, 0);
}
int chat_core_init(struct UTUN_INSTANCE* inst, const char* db_path) {
    if (!inst || !db_path || !inst->core_started || !inst->topo_sqlite_db || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat legacy init requires running core/synchronous DB/path"); return -1;
    }
    if (CC(inst)) {
        if (CC(inst)->initialized && CC(inst)->db == inst->topo_sqlite_db && !strcmp(CC(inst)->db_path, db_path)) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat legacy init requires absent context or same ready DB/path"); return -1;
    }
    struct chat_core_ctx* cc = chat_core_allocate(inst, db_path); if (!cc) return -1;
    struct chat_core_bootstrap* prepared = NULL; int transaction = 0, subscribed = 0; const char* stage = "transaction admission";
    if (!sqlite3_get_autocommit(cc->db)) goto fail;
    stage = "begin"; if (db_exec(cc, "BEGIN IMMEDIATE /* chat init */") != SQLITE_OK) goto fail; transaction = 1;
    struct chat_core_bootstrap_input input = {.node_id=inst->node_id, .now_sec=ntp_time_get_seconds(inst), .name=inst->name, .db_path=db_path};
    memcpy(input.x25519, inst->my_keys.public_key, 32); memcpy(input.ed25519, inst->my_ed25519_pubkey, 32);
    stage = "SQL prepare"; if (chat_core_prepare(cc->db, &input, &prepared) != SQLITE_OK) goto fail;
    stage = "member subscriptions"; if (chat_member_init(inst)) goto fail; subscribed = 1;
    stage = "commit"; if (db_exec(cc, "COMMIT /* chat init */") != SQLITE_OK) goto fail; transaction = 0;
    chat_core_install(inst, prepared); stage = "channel activation";
    for (int i = 0; i < prepared->channel_count; i++) if (chat_core_ensure_channel_ready(inst, prepared->channels[i])) goto fail;
    chat_core_bootstrap_free(prepared); chat_core_publish_ready(inst); return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat legacy init failed stage=%s ready=%d: %s", stage, cc->initialized, sqlite3_errmsg(cc->db));
    chat_core_bootstrap_free(prepared);
    if (transaction && !sqlite3_get_autocommit(cc->db)) {
        if (db_exec(cc, "ROLLBACK") != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat legacy init rollback failed");
        if (!sqlite3_get_autocommit(cc->db)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat legacy transaction remains open; stopping owner"); utun_instance_stop(inst); }
    }
    if (cc->initialized) { chat_core_destroy(inst); return -1; }
    if (subscribed) chat_member_destroy(inst);
    inst->chat_core = NULL; u_free(cc); return -1;
}

void chat_core_quiesce(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized) return;
    cc->initialized = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "chat core quiesced generation=%llu", (unsigned long long)inst->group_generation);
}

void chat_core_destroy(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc) return;
    chat_core_quiesce(inst);
    chat_local_stop(inst);
    chat_invite_build_stop(inst);
    chat_clipboard_destroy(inst);
    chat_devgroup_destroy(inst);
    chat_whisper_destroy(inst);
    chat_member_destroy(inst);
    chat_attachment_destroy(inst);
    member_sync_destroy(inst); /* Also releases callback lists after partial startup. */

    inst->chat_core = NULL;
    u_free(cc);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: destroyed", CC_ID);
}

sqlite3* chat_core_get_db(struct UTUN_INSTANCE* inst) { return (inst && CC(inst)) ? CC(inst)->db : NULL; }
int chat_core_is_initialized(struct UTUN_INSTANCE* inst) { return (inst && CC(inst)) ? CC(inst)->initialized : 0; }
uint64_t chat_core_generation(struct UTUN_INSTANCE* inst) {
    return chat_core_is_initialized(inst) ? inst->group_generation : 0;
}

/* ─── chat settings (GUI → uasync) ─── */

int chat_core_set_setting(struct UTUN_INSTANCE* inst, const char* name, const char* value) {
    int rc = chat_setting_set(inst, name, value);
    if (!rc && !strcmp(name, "group_autoconnect")) chat_core_apply_group_autoconnect(inst);
    return rc;
}

void chat_core_set_setting_trampoline(void* arg) {
    struct chat_setting_arg* a = (struct chat_setting_arg*)arg;
    if (!a) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "chat setting: null request"); return; }
    const char* end = memchr(a->data, 0, sizeof(a->data));
    if (!end || end == a->data + sizeof(a->data) - 1 || !memchr(end + 1, 0, sizeof(a->data) - (size_t)(end + 1 - a->data)))
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "chat setting: unterminated name/value request");
    else chat_core_set_setting(a->inst, a->data, end + 1);
    u_free(arg);
}

/* ─── per-channel «play sound on new message» ─── */

int chat_core_get_sound_on_message(struct UTUN_INSTANCE* inst, const char* ch_id) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || !chat_channel_valid(ch_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound setting: invalid read context/channel"); return -1;
    }
    char key[128]; snprintf(key, sizeof(key), "ch_%s_sound_on_message", ch_id);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(cc->db, "SELECT value FROM ui_state WHERE key=?", -1, &st, NULL), on = 1;
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        int type = sqlite3_column_type(st, 0);
        const char* v = (const char*)sqlite3_column_text(st, 0);
        if (type != SQLITE_TEXT || sqlite3_column_bytes(st, 0) != 1 || !v || (*v != '0' && *v != '1')) rc = SQLITE_CORRUPT;
        else on = *v == '1';
    }
    int final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound setting: read ch=%s rc=%d finalize=%d: %s", ch_id, rc, final, sqlite3_errmsg(cc->db)); return -1;
    }
    return on;
}

int chat_core_set_sound_on_message(struct UTUN_INSTANCE* inst, const char* ch_id, int on) {
    if (!chat_channel_valid(ch_id)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound setting: invalid channel"); return -1; }
    char key[128]; snprintf(key, sizeof(key), "ch_%s_sound_on_message", ch_id);
    char val[2]; snprintf(val, sizeof(val), "%d", on ? 1 : 0);
    if (chat_core_save_ui_state(inst, key, val)) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: sound_on_message ch=%s = %d", CC_ID, ch_id, on ? 1 : 0);
    chat_sync_broadcast_sound_setting(inst, ch_id, on ? 1 : 0);
    return 0;
}

void chat_core_set_sound_on_message_trampoline(void* arg) {
    struct chat_sound_arg* a = (struct chat_sound_arg*)arg;
    if (!a) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound setting: null request"); return; }
    if (!memchr(a->ch_id, 0, sizeof(a->ch_id))) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound setting: unterminated channel request");
    else chat_sound_save(a->inst, a->ch_id, a->on, NULL, NULL);
    u_free(arg);
}

/* ─── headless API ─── */

/* Результат чтения публикуется целиком;даже неверные аргументы не оставляют старый JSON. */
static struct chat_core_ctx* json_context(struct UTUN_INSTANCE* inst, char* buf, size_t size, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (buf && size) buf[0] = 0;
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || !buf || size < 3 || !out_len) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat JSON: invalid context/output capacity=%zu", size); return NULL;
    }
    return cc;
}

static int json_copy(const struct strbuf* json, char* buf, size_t capacity, size_t* out_len) {
    if (json->len >= capacity) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat JSON: output exceeds capacity bytes=%zu capacity=%zu", json->len, capacity); return -1;
    }
    memcpy(buf, strbuf_str(json), json->len + 1); *out_len = json->len; return 0;
}

/* Полный UTF-8: исключаем overlong, surrogate и значения вне Unicode. */
int chat_utf8_valid(const char* text) {
    const unsigned char* p = (const unsigned char*)text;
    while (*p) {
        uint32_t c = *p++, minimum; unsigned count;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; minimum = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; minimum = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; minimum = 0x10000; c &= 7; }
        else return 0;
        while (count--) { if ((*p & 0xc0) != 0x80) return 0; c = (c << 6) | (*p++ & 63); }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}

/* Не превращать NOMEM или скрытый NUL SQLite TEXT в пустую/усечённую строку. */
static int json_read_text(sqlite3_stmt* st, int column, const char** out) {
    *out = NULL; int type = sqlite3_column_type(st, column);
    if (type == SQLITE_NULL) return SQLITE_OK;
    if (type != SQLITE_TEXT) return SQLITE_CORRUPT;
    const char* text = (const char*)sqlite3_column_text(st, column);
    if (!text) return SQLITE_NOMEM;
    if (strlen(text) != (size_t)sqlite3_column_bytes(st, column)) return SQLITE_CORRUPT;
    *out = text; return SQLITE_OK;
}

int chat_json_strings_read(sqlite3* db, const char* text, size_t limit,
    int (*visit)(const char* key, const char* value, void* arg), void* arg) {
    if (!db || !text || limit > INT_MAX || strnlen(text, limit + 1) > limit) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "flat JSON: invalid arguments/size limit=%zu", limit); return SQLITE_MISUSE;
    }
    if (!chat_utf8_valid(text)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "flat JSON: invalid UTF-8"); return SQLITE_CONSTRAINT; }
    sqlite3_stmt* st = NULL; const char* stage = "validate prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT json_valid(?1),CASE WHEN json_valid(?1) THEN json_type(?1) END", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, text, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto error;
    const char* type = NULL; rc = json_read_text(st, 1, &type); if (rc != SQLITE_OK) goto error;
    if (!sqlite3_column_int(st, 0) || !type || strcmp(type, "object")) { stage = "not a valid object"; rc = SQLITE_CONSTRAINT; goto error; }
    rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "field schema";
    rc = sqlite3_prepare_v2(db, "SELECT count(*),count(DISTINCT key),coalesce(sum(type<>'text'),0) FROM json_each(?1)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, text, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto error;
    if (sqlite3_column_int64(st, 0) != sqlite3_column_int64(st, 1) || sqlite3_column_int64(st, 2)) {
        stage = "duplicate/nonstring fields"; rc = SQLITE_CONSTRAINT; goto error;
    }
    rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "fields";
    rc = sqlite3_prepare_v2(db, "SELECT key,value FROM json_each(?1)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, text, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto error;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *key, *value; rc = json_read_text(st, 0, &key); if (rc == SQLITE_OK) rc = json_read_text(st, 1, &value);
        /* При чтении json_each встроенный NUL принадлежит входному JSON. */
        if (rc == SQLITE_CORRUPT) rc = SQLITE_CONSTRAINT;
        if (rc != SQLITE_OK) goto error;
        if (!key || !value || !chat_utf8_valid(key) || !chat_utf8_valid(value)) {
            stage = "invalid field text"; rc = SQLITE_CONSTRAINT; goto error;
        }
        if (visit && visit(key, value, arg)) { stage = "field callback"; rc = SQLITE_CONSTRAINT; goto error; }
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    return 0;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "flat JSON: stage=%s rc=%d: %s", stage, rc, sqlite3_errmsg(db));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "flat JSON: cleanup finalize rc=%d", final);
    return rc;
}

int chat_json_strings(sqlite3* db, const char* text, size_t limit,
    int (*visit)(const char* key, const char* value, void* arg), void* arg) {
    return chat_json_strings_read(db, text, limit, visit, arg) == SQLITE_OK ? 0 : -1;
}

/* Сохранённый канал может ещё не иметь history-таблицы при отложенной runtime-подготовке. */
static int json_message_count(sqlite3* db, const char* channel, sqlite3_int64* count) {
    char table[80], sql[128]; msg_table_name(channel, table, sizeof(table)); sqlite3_stmt* st = NULL;
    const char* stage = "history table lookup";
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type IN('table','view') AND name=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto error;
    rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    if (!found) {
        *count = 0; DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "chat JSON: history not prepared ch=%s", channel); return 0;
    }
    stage = "history count"; snprintf(sql, sizeof(sql), "SELECT count(*) FROM \"%s\"", table);
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto error;
    sqlite3_int64 result = sqlite3_column_int64(st, 0);
    rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    *count = result; return 0;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat JSON: ch=%s stage=%s rc=%d: %s", channel, stage, rc, sqlite3_errmsg(db));
    int finish = sqlite3_finalize(st);
    if (finish != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat JSON: count cleanup finalize rc=%d", finish);
    return rc;
}

void chat_json_rows_free(struct chat_json_rows* rows) {
    if (!rows) return;
    for (size_t i = 0; i < rows->count; i++) { u_free(rows->rows[i].fields); u_free(rows->rows[i].members); }
    u_free(rows->rows); memset(rows, 0, sizeof(*rows));
}

/* Ограничить и сам JSON, и вспомогательные ID: большой список не даёт неограниченных выделений. */
#define CHAT_JSON_SNAPSHOT_MAX (8u * 1024u * 1024u)
static int json_row_new(struct chat_json_rows* rows, struct chat_json_row** row) {
    if (rows->count == 65535 || rows->bytes > CHAT_JSON_SNAPSHOT_MAX - sizeof(**row)) return SQLITE_TOOBIG;
    if (rows->count == rows->allocated) {
        size_t count = rows->allocated ? rows->allocated * 2 : 8; if (count > 65535) count = 65535;
        void* grown = u_realloc(rows->rows, count * sizeof(*rows->rows)); if (!grown) return SQLITE_NOMEM;
        rows->rows = grown; rows->allocated = count;
    }
    *row = &rows->rows[rows->count++]; memset(*row, 0, sizeof(**row)); rows->bytes += sizeof(**row); return SQLITE_OK;
}

static int json_row_fields(struct chat_json_rows* rows, struct chat_json_row* row, struct strbuf* fields,
    size_t capacity, size_t* json_bytes) {
    if (fields->len >= capacity || *json_bytes >= capacity - fields->len ||
        fields->len + 1 > CHAT_JSON_SNAPSHOT_MAX - rows->bytes) return SQLITE_TOOBIG;
    *json_bytes += fields->len; rows->bytes += fields->len + 1; row->fields = strbuf_detach(fields); return SQLITE_OK;
}

int chat_json_channels_read(sqlite3* db, size_t capacity, struct chat_json_rows* out) {
    if (!db || !out || out->rows || capacity < 3 || capacity > CHAT_JSON_SNAPSHOT_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channels JSON: invalid SQL snapshot capacity=%zu", capacity); return SQLITE_MISUSE;
    }
    struct strbuf json = strbuf_new(); sqlite3_stmt *st = NULL, *peers = NULL; const char* stage = "prepare";
    size_t json_bytes = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT c.channel_id,c.name,lower(hex(c.owner_node_id)),n.name,c.group_type FROM channels c"
        " LEFT JOIN nodes n ON n.ed25519_pubkey=c.owner_node_id ORDER BY c.created_at,c.channel_id", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *channel, *name, *owner_name, *type; stage = "channel text";
        rc = json_read_text(st, 0, &channel);
        if (rc == SQLITE_OK) rc = json_read_text(st, 1, &name);
        if (rc == SQLITE_OK) rc = json_read_text(st, 3, &owner_name);
        if (rc == SQLITE_OK) rc = json_read_text(st, 4, &type);
        if (rc != SQLITE_OK) goto error;
        const char* owner; rc = json_read_text(st, 2, &owner); if (rc != SQLITE_OK) goto error;
        if (!chat_channel_valid(channel)) { stage = "invalid stored channel ID"; rc = SQLITE_CORRUPT; goto error; }
        sqlite3_int64 messages; struct chat_json_row* row;
        stage = "snapshot row"; rc = json_row_new(out, &row); if (rc != SQLITE_OK) goto error;
        snprintf(row->channel, sizeof(row->channel), "%s", channel);
        stage = "history count";
        rc = json_message_count(db, channel, &messages); if (rc != SQLITE_OK) goto error;
        char table[80], sql[192]; peers_table_name(channel, table, sizeof(table));
        snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey FROM \"%s\" WHERE record_state=0 AND deleted=0 AND admitted=1", table);
        size_t allocated = 0;
        stage = "members"; rc = sqlite3_prepare_v2(db, sql, -1, &peers, NULL); if (rc != SQLITE_OK) goto error;
        while ((rc = sqlite3_step(peers)) == SQLITE_ROW) {
            if (row->member_count == 65535 || out->bytes > CHAT_JSON_SNAPSHOT_MAX - sizeof(*row->members)) {
                stage = "member snapshot limit"; rc = SQLITE_TOOBIG; goto error;
            }
            if (row->member_count == allocated) {
                size_t count = allocated ? allocated * 2 : 8; if (count > 65535) count = 65535;
                void* grown = u_realloc(row->members, count * sizeof(*row->members));
                if (!grown) { stage = "member allocation"; rc = SQLITE_NOMEM; goto error; }
                row->members = grown; allocated = count;
            }
            if (sqlite3_column_type(peers, 0) != SQLITE_BLOB || sqlite3_column_bytes(peers, 0) != 32) {
                stage = "invalid member Ed25519"; rc = SQLITE_CORRUPT; goto error;
            }
            memcpy(row->members[row->member_count++], sqlite3_column_blob(peers, 0), 32); out->bytes += sizeof(*row->members);
        }
        if (rc != SQLITE_DONE) goto error;
        stage = "members finalize"; rc = sqlite3_finalize(peers); peers = NULL; if (rc != SQLITE_OK) goto error;
        stage = "JSON"; rc = SQLITE_NOMEM;
        if (strbuf_addf(&json, "\"id\":") < 0 || strbuf_add_json_string(&json, channel) < 0 ||
            strbuf_addf(&json, ",\"name\":") < 0 || strbuf_add_json_string(&json, name) < 0 ||
            strbuf_addf(&json, ",\"group_type\":") < 0 || strbuf_add_json_string(&json, type) < 0 ||
            strbuf_addf(&json, ",\"owner_id\":\"%s\",\"owner_name\":", owner) < 0 ||
            strbuf_add_json_string(&json, owner_name) < 0 ||
            strbuf_addf(&json, ",\"peers\":%zu,\"msgs\":%lld", row->member_count, (long long)messages) < 0) goto error;
        stage = "snapshot fields"; rc = json_row_fields(out, row, &json, capacity, &json_bytes); if (rc != SQLITE_OK) goto error;
        stage = "channels step";
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "channels finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "channels JSON: SQL snapshot rows=%zu bytes=%zu", out->count, out->bytes); return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channels JSON: stage=%s rc=%d bytes=%zu capacity=%zu: %s",
        stage, rc, out->bytes, capacity, (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int finish = sqlite3_finalize(peers);
    if (finish != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channels JSON: members cleanup rc=%d", finish);
    finish = sqlite3_finalize(st);
    if (finish != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channels JSON: cleanup rc=%d", finish);
    strbuf_free(&json); chat_json_rows_free(out); return rc;
}

int chat_json_messages_read(sqlite3* db, const char* ch_id, int count, int offset,
                                 char* buf, size_t buf_size, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (buf && buf_size) buf[0] = 0;
    if (!db || !buf || buf_size < 3 || !out_len || !chat_channel_valid(ch_id) || offset < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "messages JSON: invalid output/channel/offset=%d capacity=%zu", offset, buf_size);
        return SQLITE_MISUSE;
    }
    char table[80], sql[1024]; msg_table_name(ch_id, table, sizeof(table));
    snprintf(sql, sizeof(sql),
        "SELECT json_object('id',m.id,'message_id',lower(hex(m.message_id)),'ts',m.timestamp,"
        "'author_id',lower(hex(m.accepted_key)),'author_name',coalesce(n.name,''),"
        "'content_type',CASE WHEN json_valid(CAST(m.data AS TEXT)) THEN json_extract(CAST(m.data AS TEXT),'$.ct') ELSE 'text' END,"
        "'data',coalesce(CAST(m.data AS TEXT),''),'local_attrs',coalesce(m.local_attrs,''),'ver',m.ver,'edited_at',m.edited_at,'archived',m.archived)"
        " FROM \"%s\" m LEFT JOIN nodes n ON n.ed25519_pubkey=m.accepted_key"
        " ORDER BY m.timestamp DESC,m.message_id DESC LIMIT ? OFFSET ?", table);
    sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind count"; rc = sqlite3_bind_int(st, 1, count > 0 ? count : 100); }
    if (rc == SQLITE_OK) { stage = "bind offset"; rc = sqlite3_bind_int(st, 2, offset); }
    if (rc != SQLITE_OK) goto error;
    size_t off = 1; buf[0] = '['; stage = "step";
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* row = (const char*)sqlite3_column_text(st, 0); size_t bytes = sqlite3_column_bytes(st, 0);
        size_t separator = off > 1, available = buf_size - off;
        if (!row || available < separator + 2 || bytes > available - separator - 2) {
            stage = "JSON capacity/read"; rc = row ? SQLITE_TOOBIG : SQLITE_NOMEM; goto error;
        }
        if (separator) buf[off++] = ',';
        memcpy(buf + off, row, bytes); off += bytes;
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    buf[off++] = ']'; buf[off] = 0; *out_len = off; return 0;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "messages JSON: ch=%s stage=%s rc=%d capacity=%zu: %s", ch_id, stage, rc, buf_size,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int finish = sqlite3_finalize(st);
    if (finish != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "messages JSON: cleanup rc=%d", finish);
    buf[0] = 0; return rc;
}

int chat_json_members_read(sqlite3* db, const char* ch_id, size_t capacity, struct chat_json_rows* out) {
    if (!db || !out || out->rows || capacity < 3 || capacity > CHAT_JSON_SNAPSHOT_MAX || !chat_channel_valid(ch_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "members JSON: invalid SQL snapshot/channel capacity=%zu", capacity); return SQLITE_MISUSE;
    }
    char table[80], sql[384]; peers_table_name(ch_id, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT n.x25519_pubkey,n.name,p.x25519_pubkey,p.ed25519_pubkey,p.adm_tags,p.deleted"
        " FROM \"%s\" p LEFT JOIN nodes n ON p.x25519_pubkey=n.x25519_pubkey AND p.ed25519_pubkey=n.ed25519_pubkey WHERE p.record_state=0 ORDER BY p.ed25519_pubkey", table);
    sqlite3_stmt *st = NULL, *addresses = NULL; struct strbuf json = strbuf_new(); const char* stage = "prepare";
    size_t json_bytes = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL); if (rc != SQLITE_OK) goto error;
    stage = "members step";
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t* node_x = sqlite3_column_blob(st, 0);
        if (node_x && sqlite3_column_bytes(st, 0) != 32) { stage = "address owner X25519"; rc = SQLITE_CORRUPT; goto error; }
        const char *name, *tags; stage = "member text";
        rc = json_read_text(st, 1, &name); if (rc == SQLITE_OK) rc = json_read_text(st, 4, &tags);
        if (rc != SQLITE_OK) goto error;
        char keys[2][65] = {{0}};
        for (int i = 0; i < 2; i++) if (sqlite3_column_type(st, i + 2) != SQLITE_NULL) {
            stage = "key size/type"; rc = SQLITE_CORRUPT;
            if (sqlite3_column_type(st, i + 2) != SQLITE_BLOB || sqlite3_column_bytes(st, i + 2) != 32) goto error;
            const uint8_t* key = sqlite3_column_blob(st, i + 2); if (!key) { rc = SQLITE_NOMEM; goto error; }
            for (int b = 0; b < 32; b++) snprintf(keys[i] + b * 2, 3, "%02x", key[b]);
        }
        uint8_t dg[32]; char dg_name[64] = "", dg_hex[65] = "";
        const uint8_t* ed = keys[1][0] ? sqlite3_column_blob(st, 3) : NULL;
        int dg_found = ed ? devgroup_member(db, ed, dg, dg_name) : 0;
        if (dg_found < 0) { stage = "devgroup"; rc = -dg_found; goto error; }
        if (dg_found) for (int i = 0; i < 32; i++) snprintf(dg_hex + 2*i, 3, "%02x", dg[i]);
        struct chat_json_row* row; stage = "snapshot row"; rc = json_row_new(out, &row); if (rc != SQLITE_OK) goto error;
        if (!keys[1][0]) { stage = "missing member Ed25519"; rc = SQLITE_CORRUPT; goto error; }
        memcpy(row->node, sqlite3_column_blob(st, 3), 32); row->deleted = sqlite3_column_int(st, 5) != 0;
        stage = "JSON"; rc = SQLITE_NOMEM;
        if (strbuf_addf(&json, "\"node_id\":\"%s\",\"name\":", keys[1]) < 0 ||
            strbuf_add_json_string(&json, name) < 0 || strbuf_addf(&json, ",\"adm_tags\":") < 0 ||
            strbuf_add_json_string(&json, tags) < 0 ||
            strbuf_addf(&json, ",\"devgroup\":\"%s\",\"devgroup_name\":", dg_hex) < 0 ||
            strbuf_add_json_string(&json, dg_name) < 0 ||
            strbuf_addf(&json, ",\"x25519\":\"%s\",\"ed25519\":\"%s\",\"addrs\":[", keys[0], keys[1]) < 0) goto error;
        stage = "addresses prepare";
        rc = sqlite3_prepare_v2(db, "SELECT family,protocol,address,port,rtt FROM node_addresses WHERE node_x=? ORDER BY family,protocol",
            -1, &addresses, NULL);
        if (rc == SQLITE_OK) {
            stage = "addresses bind";
            rc = node_x ? sqlite3_bind_blob(addresses, 1, node_x, 32, SQLITE_STATIC) : sqlite3_bind_null(addresses, 1);
        }
        if (rc != SQLITE_OK) goto error;
        const char* separator = ""; stage = "addresses step";
        while ((rc = sqlite3_step(addresses)) == SQLITE_ROW) {
            int family = sqlite3_column_int(addresses, 0), protocol = sqlite3_column_int(addresses, 1);
            int bytes = sqlite3_column_bytes(addresses, 2), port = sqlite3_column_int(addresses, 3);
            const void* address = sqlite3_column_blob(addresses, 2); char ip[64];
            if ((family != 4 && family != 6) || sqlite3_column_type(addresses, 2) != SQLITE_BLOB ||
                bytes != (family == 4 ? 4 : 16) || !address ||
                (protocol != 1 && protocol != 2 && protocol != 6) || port < 0 || port > UINT16_MAX ||
                !inet_ntop(family == 4 ? AF_INET : AF_INET6, address, ip, sizeof(ip))) {
                stage = "invalid stored address"; rc = SQLITE_CORRUPT; goto error;
            }
            stage = "JSON"; rc = SQLITE_NOMEM;
            if (strbuf_addf(&json, "%s{\"ip\":", separator) < 0 || strbuf_add_json_string(&json, ip) < 0 ||
                strbuf_addf(&json, ",\"port\":%d,\"proto\":\"%s\",\"rtt\":%lld}", port,
                    protocol == 1 ? "UDP" : protocol == 2 ? "TCP" : "TCP+REALITY", (long long)sqlite3_column_int64(addresses, 4)) < 0) goto error;
            separator = ","; if (json.len >= capacity) { rc = SQLITE_TOOBIG; goto error; }
            stage = "addresses step";
        }
        if (rc != SQLITE_DONE) goto error;
        stage = "addresses finalize"; rc = sqlite3_finalize(addresses); addresses = NULL; if (rc != SQLITE_OK) goto error;
        stage = "JSON"; rc = SQLITE_NOMEM; if (strbuf_addf(&json, "]") < 0) goto error;
        stage = "snapshot fields"; rc = json_row_fields(out, row, &json, capacity, &json_bytes); if (rc != SQLITE_OK) goto error;
        stage = "members step";
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "members finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "members JSON: SQL snapshot ch=%s rows=%zu bytes=%zu", ch_id, out->count, out->bytes);
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "members JSON: ch=%s stage=%s rc=%d bytes=%zu capacity=%zu: %s",
        ch_id, stage, rc, out->bytes, capacity,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int finish = sqlite3_finalize(addresses);
    if (finish != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "members JSON: addresses cleanup rc=%d", finish);
    finish = sqlite3_finalize(st);
    if (finish != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "members JSON: cleanup rc=%d", finish);
    strbuf_free(&json); chat_json_rows_free(out); return rc;
}

/* После READ статусы берутся из живых маршрутов/соединений, без SQLite и без повторного разбора JSON. */
int chat_json_rows_render(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_json_rows* rows,
    char* buf, size_t capacity, size_t* length) {
    if (length) *length = 0;
    if (buf && capacity) buf[0] = 0;
    if (!inst || !CC(inst) || !CC(inst)->initialized || !rows || !buf || capacity < 3 || !length ||
        (channel && !chat_channel_valid(channel))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat JSON: invalid renderer context/output capacity=%zu", capacity); return -1;
    }
    struct strbuf json = strbuf_new(); const char* stage = "allocation";
    if (strbuf_addf(&json, "[") < 0) goto error;
    for (size_t i = 0; i < rows->count; i++) {
        const struct chat_json_row* row = &rows->rows[i];
        if (strbuf_addf(&json, "%s{%s", i ? "," : "", row->fields) < 0) goto error;
        if (channel) {
            int online = !row->deleted && chat_core_member_online(inst, channel, row->node);
            int connected = inst->connections && instance_find_conn_by_ed(inst, row->node) != NULL;
            if (strbuf_addf(&json, ",\"online\":%d,\"connected\":%d}", online, connected) < 0) goto error;
        } else {
            size_t online = 0;
            for (size_t n = 0; n < row->member_count; n++) online += chat_core_member_online(inst, row->channel, row->members[n]);
            if (strbuf_addf(&json, ",\"online\":%zu}", online) < 0) goto error;
        }
        if (json.len >= capacity) { stage = "capacity"; goto error; }
    }
    if (strbuf_addf(&json, "]") < 0) goto error;
    int rc = json_copy(&json, buf, capacity, length); strbuf_free(&json); return rc;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat JSON: render failed stage=%s rows=%zu bytes=%zu capacity=%zu",
        stage, rows->count, json.len, capacity); strbuf_free(&json); return -1;
}

int chat_core_get_channels_json(struct UTUN_INSTANCE* inst, char* buf, size_t buf_size, size_t* out_len) {
    struct chat_core_ctx* cc = json_context(inst, buf, buf_size, out_len); if (!cc) return -1;
    struct chat_json_rows rows = {0}; int rc = chat_json_channels_read(cc->db, buf_size, &rows);
    if (rc == SQLITE_OK) rc = chat_json_rows_render(inst, NULL, &rows, buf, buf_size, out_len);
    chat_json_rows_free(&rows); return rc ? -1 : 0;
}

int chat_core_get_members_json(struct UTUN_INSTANCE* inst, const char* ch_id, char* buf, size_t buf_size, size_t* out_len) {
    struct chat_core_ctx* cc = json_context(inst, buf, buf_size, out_len); if (!cc) return -1;
    struct chat_json_rows rows = {0}; int rc = chat_json_members_read(cc->db, ch_id, buf_size, &rows);
    if (rc == SQLITE_OK) rc = chat_json_rows_render(inst, ch_id, &rows, buf, buf_size, out_len);
    chat_json_rows_free(&rows); return rc ? -1 : 0;
}

int chat_core_get_messages_json(struct UTUN_INSTANCE* inst, const char* ch_id, int count, int offset,
    char* buf, size_t buf_size, size_t* out_len) {
    struct chat_core_ctx* cc = json_context(inst, buf, buf_size, out_len); if (!cc) return -1;
    return chat_json_messages_read(cc->db, ch_id, count, offset, buf, buf_size, out_len) ? -1 : 0;
}

int chat_core_get_node_name(struct UTUN_INSTANCE* inst, const uint8_t x[32], char* out, size_t sz) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (out && sz) out[0] = 0;
    if (cc && cc->initialized && x && out && sz && db_async_can_write(inst->db_worker)) {
        if (!memcmp(x, inst->my_keys.public_key, 32)) snprintf(out, sz, "%s", inst->name);
        else {
            struct TOPO_NODE* node = inst->topo_groups ? topo_node_registry_lookup(inst->topo_groups, sc_derive_node_id_from_pubkey(x), x) : NULL;
            if (node && node->node_name) snprintf(out, sz, "%s", node->node_name);
        }
        return 0;
    }
    if (!cc || !cc->initialized || !cc->db || !x || !out || !sz) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "node name: invalid context/output X25519=%s", sc_public_key_hex(x, (char[65]){0})); return -1;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(cc->db, "SELECT name FROM nodes WHERE x25519_pubkey=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, x, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* n = (const char*)sqlite3_column_text(st, 0);
        if (n) snprintf(out, sz, "%s", n);
        else if (sqlite3_errcode(cc->db) == SQLITE_NOMEM) rc = SQLITE_NOMEM;
    }
    int final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        out[0] = 0; DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "node name: lookup node=%016llx rc=%d finalize=%d: %s",
            sc_public_key_hex(x, (char[65]){0}), rc, final, sqlite3_errmsg(cc->db)); return -1;
    }
    return 0;
}

/* ─── adm_tags editing (channel owner only) ─── */

struct chat_member_tags_key {
    char  key[CHAT_MEMBER_TAGS_KEY_SZ];
    char* val;
    char* raw;    /* malloc'd: serialized append-only string (admin/moder) or NULL (= val is raw) */
};

struct chat_member_tags {
    char    ch_id[65];
    uint64_t node_id;
    sqlite3* db;                         /* Заимствована до остановки сервиса. */
    char baseline[MS_ADM_TAGS_MAX + 1]; /* Проверенный owner-блок на момент загрузки/commit. */
    uint8_t signature[64], x25519[32], ed25519[32];
    int version, parsing_edit;
    int     key_count;
    struct chat_member_tags_key keys[CHAT_MEMBER_TAGS_MAX_KEYS];
};

static int tags_find(struct chat_member_tags* t, const char* key) {
    for (int i = 0; i < t->key_count; i++)
        if (strcmp(t->keys[i].key, key) == 0) return i;
    return -1;
}

static const char* tags_append_effective(const char* raw, uint64_t* ts_out) {
    if (!raw || !raw[0]) { if (ts_out) *ts_out = 0; return "no"; }
    const char* last = raw;
    for (const char* p = raw; *p; p++)
        if (*p == 'e' || *p == 'd') last = p;
    if (ts_out) *ts_out = strtoull(last + 1, NULL, 10);
    return *last == 'e' ? "yes" : "no";
}

static int tags_is_append_key(const char* key) {
    return strcmp(key, "admin") == 0 || strcmp(key, "moder") == 0;
}

static int tags_json_cb(const char* key, const char* json_val, void* arg) {
    struct chat_member_tags* t = (struct chat_member_tags*)arg;
    if (!key[0] || t->key_count >= CHAT_MEMBER_TAGS_MAX_KEYS || strlen(key) >= CHAT_MEMBER_TAGS_KEY_SZ) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: tags JSON key limit exceeded", CC_ID); return -1;
    }
    if (t->parsing_edit && tags_is_append_key(key) && strcmp(json_val, "yes") && strcmp(json_val, "no")) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: edited role must be yes/no key=%s", key); return -1;
    }
    struct chat_member_tags_key* k = &t->keys[t->key_count];
    snprintf(k->key, CHAT_MEMBER_TAGS_KEY_SZ, "%s", key);
    k->val = u_strdup(tags_is_append_key(key) ? tags_append_effective(json_val, NULL) : json_val);
    k->raw = tags_is_append_key(key) ? u_strdup(json_val) : NULL;
    if (!k->val || (tags_is_append_key(key) && !k->raw)) {
        u_free(k->val); u_free(k->raw); memset(k, 0, sizeof(*k));
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: tags JSON allocation failed", CC_ID); return -1;
    }
    t->key_count++;
    return 0;
}

static int member_sql_failure(sqlite3* db, int fallback) {
    int rc = sqlite3_extended_errcode(db);
    return rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : fallback;
}

/* Полностью собственный SQL-снимок; result сохраняет operational SQLite ошибку до cleanup. */
static struct chat_member_tags* member_tags_read(sqlite3* db, const char* ch_id, const uint8_t key[32], int* result) {
    *result = SQLITE_MISUSE;
    if (!db || !chat_channel_valid(ch_id) || !key || mf_zero(key, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: invalid load context/channel/member"); return NULL;
    }
    struct chat_member_tags* t = u_calloc(1, sizeof(*t));
    if (!t) { *result = SQLITE_NOMEM; DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: tags_load — OOM", CC_ID); return NULL; }
    snprintf(t->ch_id, sizeof(t->ch_id), "%s", ch_id);
    t->db = db;

    char peers_tbl[80]; peers_table_name(ch_id, peers_tbl, sizeof(peers_tbl));
    sqlite3_stmt* st = NULL;
    char sql[320]; snprintf(sql, sizeof(sql), "SELECT adm_tags,adm_tags_sig,x25519_pubkey,ed25519_pubkey,source,node_id"
        " FROM \"%s\" WHERE ed25519_pubkey=?1 AND record_state=0", peers_tbl);
    const char* stage = "prepare"; int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind Ed25519"; rc = sqlite3_bind_blob(st, 1, key, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    if (rc != SQLITE_ROW) goto error;
    stage = "fields"; rc = SQLITE_CORRUPT;
    int type = sqlite3_column_type(st, 0), bytes = sqlite3_column_bytes(st, 0), sig_type = sqlite3_column_type(st, 1);
    const char* text = (const char*)sqlite3_column_text(st, 0);
    const void *signature = sqlite3_column_blob(st, 1), *x = sqlite3_column_blob(st, 2), *ed = sqlite3_column_blob(st, 3);
    if ((type != SQLITE_NULL && type != SQLITE_TEXT) || (type == SQLITE_TEXT && (!text || strlen(text) != (size_t)bytes)) ||
        bytes > MS_ADM_TAGS_MAX || sqlite3_column_type(st, 2) != SQLITE_BLOB || sqlite3_column_bytes(st, 2) != 32 || !x ||
        sqlite3_column_type(st, 3) != SQLITE_BLOB || sqlite3_column_bytes(st, 3) != 32 || !ed ||
        sqlite3_column_type(st, 4) != SQLITE_INTEGER || sqlite3_column_int(st, 4)) goto error;
    if (bytes) memcpy(t->baseline, text, (size_t)bytes + 1);
    if (sig_type != SQLITE_NULL) {
        if (sig_type != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 64 || !signature) goto error;
        memcpy(t->signature, signature, 64);
    }
    t->node_id = (uint64_t)sqlite3_column_int64(st, 5);
    memcpy(t->x25519, x, 32); memcpy(t->ed25519, ed, 32);
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    t->version = member_sync_owner_version(db, t->baseline);
    if (t->version < 0) goto invalid;
    if (t->version) {
        uint8_t key[32], body[MS_OWNER_MSG_MAX];
        int length = member_sync_build_owner_msg(ch_id, t->ed25519, t->baseline, body, sizeof(body));
        if (length < 0 || topo_node_sqlite_channel_get(db, ch_id, NULL, 0, NULL, NULL, key, NULL) ||
            sc_ed25519_verify(key, body, (size_t)length, t->signature) != SC_OK) goto invalid;
        if (chat_json_strings(db, t->baseline, MS_ADM_TAGS_MAX, tags_json_cb, t)) goto invalid;
    } else if (memcmp(t->signature, (uint8_t[64]){0}, 64)) goto invalid;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "tags: loaded ch=%s peer=%s version=%d keys=%d",
        ch_id, sc_public_key_hex(key, (char[65]){0}), t->version, t->key_count);
    *result = SQLITE_OK; return t;
invalid:
    *result = member_sql_failure(db, SQLITE_AUTH);
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: invalid signed owner block ch=%s peer=%s", ch_id, sc_public_key_hex(key, (char[65]){0}));
    chat_member_tags_free(t); return NULL;
error:
    *result = rc == SQLITE_DONE ? SQLITE_NOTFOUND : rc;
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: load ch=%s peer=%s stage=%s rc=%d: %s",
        ch_id, sc_public_key_hex(key, (char[65]){0}), stage, rc, sqlite3_errmsg(db));
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: load cleanup finalize=%d", rc);
    chat_member_tags_free(t); return NULL;
}

struct chat_member_tags* chat_member_tags_load(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32]) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: synchronous load requires legacy database"); return NULL;
    }
    int rc; return member_tags_read(cc->db, ch_id, ed, &rc);
}

int chat_member_tags_set(struct chat_member_tags* t, const char* key, const char* val) {
    if (!t || !key || !key[0] || !strcmp(key, "ver") ||
        (val && tags_is_append_key(key) && strcmp(val, "yes") && strcmp(val, "no"))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: invalid edit key/role or reserved version"); return -1;
    }
    if (strlen(key) >= CHAT_MEMBER_TAGS_KEY_SZ) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: tags_set — key too long", CC_ID); return -1; }
    if (val && strlen(val) > MS_ADM_TAGS_MAX) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "%s: tags_set — val too long", CC_ID); return -1; }

    int idx = tags_find(t, key);
    if (!val) {
        if (idx >= 0) { u_free(t->keys[idx].raw); u_free(t->keys[idx].val); t->keys[idx] = t->keys[--t->key_count]; }
        return 0;
    }
    char* copy = u_strdup(val);
    if (!copy) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: tags_set allocation failed", CC_ID); return -1; }
    if (idx >= 0) {
        u_free(t->keys[idx].val); t->keys[idx].val = copy;
    } else if (t->key_count < CHAT_MEMBER_TAGS_MAX_KEYS - (tags_find(t, "ver") < 0)) {
        struct chat_member_tags_key* k = &t->keys[t->key_count++];
        snprintf(k->key, CHAT_MEMBER_TAGS_KEY_SZ, "%s", key);
        k->val = copy;
        k->raw = NULL;
    } else {
        u_free(copy); DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: tags_set — max keys reached", CC_ID); return -1;
    }
    return 0;
}

const char* chat_member_tags_get(struct chat_member_tags* t, const char* key) {
    if (!t || !key) return NULL;
    int idx = tags_find(t, key);
    return idx >= 0 ? t->keys[idx].val : NULL;
}

int chat_member_tags_replace(struct chat_member_tags* t, const char* json) {
    if (!t || !json || strlen(json) > MS_ADM_TAGS_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: tags_replace invalid input or size", CC_ID); return -1;
    }
    struct chat_member_tags parsed = {.db = t->db, .parsing_edit = 1};
    int rc = chat_json_strings(t->db, json, MS_ADM_TAGS_MAX, tags_json_cb, &parsed);
    char version[16]; snprintf(version, sizeof(version), "%d", t->version);
    const char* requested = chat_member_tags_get(&parsed, "ver");
    if (requested && strcmp(requested, version)) rc = -1;
    if (!requested && parsed.key_count == CHAT_MEMBER_TAGS_MAX_KEYS) rc = -1;
    if (rc != 0) {
        for (int i = 0; i < parsed.key_count; i++) { u_free(parsed.keys[i].raw); u_free(parsed.keys[i].val); }
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "tags: replace rejected JSON/version/key limit"); return -1;
    }
    for (int i = 0; i < t->key_count; i++) { u_free(t->keys[i].raw); u_free(t->keys[i].val); }
    t->key_count = parsed.key_count; memcpy(t->keys, parsed.keys, sizeof(t->keys));
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "%s: tags_replace ch=%s nid=0x%016llx keys=%d",
                CC_ID, t->ch_id, (unsigned long long)t->node_id, t->key_count);
    return 0;
}

/* Сравнение желаемых значений;отсутствующий admin/moder означает отключённую роль. */
static int tags_same_edit(struct chat_member_tags* edit, struct chat_member_tags* current) {
    for (int pass = 0; pass < 2; pass++) {
        struct chat_member_tags *a = pass ? current : edit, *b = pass ? edit : current;
        for (int i = 0; i < a->key_count; i++) {
            const char *key = a->keys[i].key, *value = a->keys[i].val, *other = chat_member_tags_get(b, key);
            if (!strcmp(key, "ver") || !strcmp(key, "_kanban_seal") || !strcmp(key, "_chat_seal") || !strcmp(key, GROUP_MESSAGE_EPOCH_TAG)) continue;
            if (!other && tags_is_append_key(key)) other = "no";
            if (!other || strcmp(value, other)) return 0;
        }
    }
    return 1;
}

static int tags_json_pair(struct strbuf* json, const char* key, const char* value) {
    if (strbuf_addf(json, ",") < 0 || strbuf_add_json_string(json, key) < 0 || strbuf_addf(json, ":") < 0 ||
        strbuf_add_json_string(json, value) < 0 || json->len > MS_ADM_TAGS_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: JSON allocation/size limit key=%s", key); return -1;
    }
    return 0;
}

/* История ролей берётся из проверенного текущего блока,новая версия строится отдельно от editor. */
static int tags_build_json(uint64_t seconds, struct chat_member_tags* edit,
                           struct chat_member_tags* current, struct strbuf* json) {
    if (current->version == INT_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: version exhausted"); return -1; }
    if (strbuf_addf(json, "{\"ver\":\"%d\"", current->version + 1) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: JSON allocation failed"); return -1;
    }
    for (int i = 0; i < edit->key_count; i++) {
        const char* key = edit->keys[i].key;
        if (strcmp(key, "ver") && strcmp(key, GROUP_MESSAGE_EPOCH_TAG) && strcmp(key, "_kanban_seal") && strcmp(key, "_chat_seal") && !tags_is_append_key(key)
            && tags_json_pair(json, key, edit->keys[i].val)) return -1;
    }
    const char* epoch = chat_member_tags_get(current, GROUP_MESSAGE_EPOCH_TAG);
    if (epoch && tags_json_pair(json, GROUP_MESSAGE_EPOCH_TAG, epoch)) return -1;
    const char* seal = chat_member_tags_get(current, "_kanban_seal");
    if (seal && tags_json_pair(json, "_kanban_seal", seal)) return -1;
    const char* chat_seal = chat_member_tags_get(current, "_chat_seal");
    if (chat_seal && tags_json_pair(json, "_chat_seal", chat_seal)) return -1;
    const char* roles[] = {"admin", "moder"};
    for (size_t i = 0; i < 2; i++) {
        const char* desired = chat_member_tags_get(edit, roles[i]);
        int index = tags_find(current, roles[i]);
        const char* history = index < 0 ? "" : current->keys[index].raw;
        int enabled = desired && !strcmp(desired, "yes"), previous = !strcmp(tags_append_effective(history, NULL), "yes");
        char appended[MS_ADM_TAGS_MAX + 1];
        if (enabled != previous) {
            int bytes = snprintf(appended, sizeof(appended), "%s%c%llu", history, enabled ? 'e' : 'd', (unsigned long long)seconds);
            if (!seconds || bytes < 0 || (size_t)bytes >= sizeof(appended)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: role history/time limit key=%s", roles[i]); return -1;
            }
            history = appended;
        }
        if (history[0] && tags_json_pair(json, roles[i], history)) return -1;
    }
    if (strbuf_addf(json, "}") < 0 || json->len > MS_ADM_TAGS_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: complete JSON exceeds memory/size limit"); return -1;
    }
    return 0;
}

/* Выполнить UPDATE внутри транзакции владельца; пустое имя снимает локальный псевдоним. */
int chat_member_write_nickname(sqlite3* db, const char* channel, const uint8_t ed[32], const char* nickname) {
    if (!db || !chat_channel_valid(channel) || !ed || mf_zero(ed, 32) || !nickname) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "nickname write: invalid database/group/node/name"); return SQLITE_MISUSE;
    }
    char table[80], sql[256]; peers_table_name(channel, table, sizeof(table));
    snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET local_nick=?1 WHERE ed25519_pubkey=?2 AND record_state=0", table);
    sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind nickname"; rc = sqlite3_bind_text(st, 1, nickname[0] ? nickname : NULL, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind Ed25519"; rc = sqlite3_bind_blob(st, 2, ed, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    if (rc == SQLITE_DONE && sqlite3_changes(db) != 1) { stage = "missing member or changed Ed25519"; rc = SQLITE_NOTFOUND; }
    if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: ch=%s peer=%s stage=%s rc=%d: %s",
        channel, sc_public_key_hex(ed, (char[65]){0}), stage, rc, sqlite3_errmsg(db));
    int finish = sqlite3_finalize(st);
    if (finish != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: nickname finalize=%d: %s", finish, sqlite3_errmsg(db));
    return rc == SQLITE_DONE ? finish : rc;
}

/* Caller WRITE: подпись, nick, seals, MEMBERS и admission — один атомарный SQL результат без runtime. */
static int member_tags_write(sqlite3* db, struct member_sync_write_context* context, struct chat_member_tags* t,
    const char* nickname, struct chat_member_tags** saved, struct group_policy_update** admission, int* changes) {
    *saved = NULL; *admission = NULL; *changes = 0;
    if (sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: write requires caller transaction"); return SQLITE_MISUSE;
    }
    int rc; struct strbuf json = strbuf_new();
    struct chat_member_tags* current = member_tags_read(db, t->ch_id, t->ed25519, &rc);
    if (!current) goto done;
    if (memcmp(current->ed25519, t->ed25519, 32) || strcmp(current->baseline, t->baseline) || memcmp(current->signature, t->signature, 64)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "tags: stale editor ch=%s node=%016llx expected=%d current=%d",
            t->ch_id, (unsigned long long)t->node_id, t->version, current->version); rc = SQLITE_BUSY; goto done;
    }
    if (tags_same_edit(t, current)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "tags: unchanged ch=%s node=%016llx ver=%d", t->ch_id,
            (unsigned long long)t->node_id, t->version);
        goto write_nickname;
    }
    if (tags_build_json(context->now / 1000000, t, current, &json)) { rc = SQLITE_NOMEM; goto done; }
    const char* old_moder = chat_member_tags_get(current, "moder");
    const char* new_moder = chat_member_tags_get(t, "moder");
    if (old_moder && !strcmp(old_moder, "yes") && (!new_moder || strcmp(new_moder, "yes"))) {
        char* owner_json = strbuf_detach(&json);
        if (group_protocol_seal_role_write(db, context, t->ch_id, t->ed25519, &owner_json)) {
            rc = member_sql_failure(db, SQLITE_ERROR); u_free(owner_json); goto done;
        }
        json.buf = owner_json; json.len = strlen(owner_json); json.cap = json.len + 1;
    }
    uint8_t private_key[32] = {0}, body[MS_OWNER_MSG_MAX];
    int bytes = member_sync_build_owner_msg(t->ch_id, t->ed25519, json.buf, body, sizeof(body));
    int signed_ok = bytes >= 0 && !topo_node_sqlite_channel_get_priv(db, t->ch_id, private_key) &&
        sc_ed25519_sign(private_key, body, (size_t)bytes, current->signature) == SC_OK;
    OPENSSL_cleanse(private_key, sizeof(private_key));
    if (!signed_ok) {
        rc = member_sql_failure(db, SQLITE_AUTH);
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: channel signing failed ch=%s rc=%d", t->ch_id, rc); goto done;
    }
    struct ms_member_rec record = {.node_id=t->node_id,.x25519=current->x25519,.ed25519=current->ed25519,
        .adm_tags=json.buf,.adm_tags_sig=current->signature};
    *changes = member_sync_record_write(db, context, t->ch_id, NULL, &record);
    if (*changes < 0 || !(*changes & MS_APPLY_CHANGED)) {
        rc = *changes == MT_ERR_IO ? context->sqlite_code : SQLITE_CONSTRAINT;
        if (!rc) rc = SQLITE_ERROR;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: owner version not written ch=%s flags=%d rc=%d", t->ch_id, *changes, rc);
        goto done;
    }
write_nickname:
    rc = nickname ? chat_member_write_nickname(db, t->ch_id, t->ed25519, nickname) : SQLITE_OK;
    if (rc != SQLITE_OK) goto done;
    if (*changes) {
        if (group_policy_seal_revocations_write(db, context, t->ch_id)) {
            rc = context->sqlite_code ? context->sqlite_code : member_sql_failure(db, SQLITE_ERROR); goto done;
        }
        if (group_policy_project(db, context->self, t->ch_id, admission)) {
            rc = member_sql_failure(db, SQLITE_ERROR); goto done;
        }
        struct chat_member_tags* sealed = member_tags_read(db, t->ch_id, t->ed25519, &rc);
        if (!sealed) goto done;
        chat_member_tags_free(current); current = sealed;
    }
    *saved = current; current = NULL; rc = SQLITE_OK;
done:
    chat_member_tags_free(current); strbuf_free(&json); return rc;
}

static void member_write_context(struct UTUN_INSTANCE* inst, struct member_sync_write_context* context) {
    memset(context, 0, sizeof(*context)); context->self = inst->node_id; context->now = ntp_time_get_us(inst);

    memcpy(context->public_key, inst->my_ed25519_pubkey, 32); memcpy(context->private_key, inst->my_ed25519_privkey, 32);
}

/* Legacy RAM/READONLY editor использует тот же SQL-only writer и публикует только после своего commit. */
static int member_tags_commit(struct UTUN_INSTANCE* inst, struct chat_member_tags* t, const char* nickname) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!t || !cc || !cc->initialized || !cc->db || t->db != cc->db || !sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: commit requires active owner DB without outer transaction"); return -1;
    }
    int rc = sqlite3_exec(cc->db, "BEGIN IMMEDIATE /* tags */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: begin rc=%d: %s", rc, sqlite3_errmsg(cc->db)); return -1; }
    int result = -1, changes = 0; struct member_sync_write_context context;
    member_write_context(inst, &context);
    struct group_policy_update* admission = NULL; struct chat_member_tags* current = NULL;
    rc = member_tags_write(cc->db, &context, t, nickname, &current, &admission, &changes);
    OPENSSL_cleanse(context.private_key, sizeof(context.private_key));
    if (rc != SQLITE_OK) { if (rc == SQLITE_BUSY) result = -2; goto done; }
    if (!changes && !nickname) { result = 0; goto done; }
    rc = sqlite3_exec(cc->db, "COMMIT /* tags */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: commit rc=%d: %s", rc, sqlite3_errmsg(cc->db)); goto done; }
    if (!changes) { result = 0; goto done; }
    char channel[65]; memcpy(channel, t->ch_id, sizeof(channel));
    for (int i = 0; i < t->key_count; i++) { u_free(t->keys[i].raw); u_free(t->keys[i].val); }
    *t = *current; u_free(current);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "tags: committed ch=%s peer=%s ver=%d keys=%d", channel,
        sc_public_key_hex(t->ed25519, (char[65]){0}), t->version, t->key_count);
    group_policy_publish(inst, admission);
    member_sync_record_committed(inst, channel, t->ed25519, (unsigned)changes); return 0;
done:
    group_policy_discard(admission);
    chat_member_tags_free(current);
    if (!sqlite3_get_autocommit(cc->db)) {
        rc = sqlite3_exec(cc->db, "ROLLBACK", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: rollback rc=%d: %s", rc, sqlite3_errmsg(cc->db)); result = -1; }
        if (!sqlite3_get_autocommit(cc->db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: transaction remains open;requesting owner stop/DB close");
            utun_instance_stop(inst); result = -1;
        }
    }
    return result;
}

int chat_member_tags_commit(struct UTUN_INSTANCE* inst, struct chat_member_tags* t) {
    return member_tags_commit(inst, t, NULL);
}

void chat_member_tags_free(struct chat_member_tags* t) {
    if (!t) return;
    for (int i = 0; i < t->key_count; i++) { u_free(t->keys[i].raw); u_free(t->keys[i].val); }
    u_free(t);
}

/* Частичный JSON меняет только указанные поля; ver проверяет исходный снимок формы. */
static int member_tags_patch(struct chat_member_tags* t, const char* json) {
    int rc = 0;
    if (json[0]) {
        struct chat_member_tags* patch = u_calloc(1, sizeof(*patch));
        if (!patch) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: patch allocation failed"); rc = -1; }
        else {
            patch->db = t->db; patch->version = t->version;
            rc = chat_member_tags_replace(patch, json);
            for (int i = 0; !rc && i < patch->key_count; i++) {
                struct chat_member_tags_key* key = &patch->keys[i];
                if (strcmp(key->key, "ver")) rc = chat_member_tags_set(t, key->key, key->val);
            }
            chat_member_tags_free(patch);
        }
    }
    return rc;
}

struct member_save_job {
    struct UTUN_INSTANCE* inst; /* Только guard/done; instance живёт до DB drain. */
    db_async_done_fn done;
    void* arg;
    uint64_t core_generation, group_generation;
    char channel[65];
    char *nickname, *json;
    uint8_t ed[32];
    struct member_sync_write_context context;
    struct group_policy_update* admission;
    int changes;
};

static int member_save_current(struct member_save_job* job) {
    struct UTUN_INSTANCE* inst = job->inst;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, job->channel) : NULL;
    return !inst->destroying && chat_core_generation(inst) == job->core_generation && group && !group->stopping &&
        group->generation == job->group_generation && (!db_async_can_write(inst->db_worker) || inst->group_started);
}

static int member_save_guard(void* data) {
    struct member_save_job* job = data;
    if (!member_save_current(job)) return 0;
    member_write_context(job->inst, &job->context); return 1;
}

int chat_core_save_member_write(sqlite3* db, struct member_sync_write_context* context, const char* channel, const uint8_t ed[32],
    const char* nickname, const char* owner_json, struct group_policy_update** admission, int* changes) {
    if (admission) *admission = NULL;
    if (changes) *changes = 0;
    if (!db || sqlite3_get_autocommit(db) || !context || !context->self || !chat_channel_valid(channel) || !ed || mf_zero(ed, 32) || !admission || !changes) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member patch WRITE requires caller transaction/identity/channel/outputs"); return SQLITE_MISUSE;
    }
    if ((nickname && strlen(nickname) > 255) || (owner_json && strlen(owner_json) > MS_ADM_TAGS_MAX)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member patch WRITE string size exceeds limit ch=%s", channel); return SQLITE_TOOBIG;
    }
    if (!owner_json || !owner_json[0]) return nickname ? chat_member_write_nickname(db, channel, ed, nickname) : SQLITE_OK;
    int rc; struct chat_member_tags* tags = member_tags_read(db, channel, ed, &rc);
    if (!tags) return rc == SQLITE_NOTFOUND ? SQLITE_BUSY : rc;
    char supplied[32], expected[32]; snprintf(expected, sizeof(expected), "%d", tags->version);
    if (!json_flat_get(owner_json, "ver", supplied, sizeof(supplied)) && strcmp(supplied, expected)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member save stale version ch=%s peer=%s expected=%s requested=%s",
            channel, sc_public_key_hex(ed, (char[65]){0}), expected, supplied);
        rc = SQLITE_BUSY;
    } else if (member_tags_patch(tags, owner_json)) rc = member_sql_failure(db, SQLITE_MISMATCH);
    else {
        struct chat_member_tags* saved = NULL;
        rc = member_tags_write(db, context, tags, nickname, &saved, admission, changes);
        chat_member_tags_free(saved);
    }
    chat_member_tags_free(tags); if (rc) *changes = 0; return rc;
}

static int member_save_work(sqlite3* db, void* data) {
    struct member_save_job* job = data;
    return chat_core_save_member_write(db, &job->context, job->channel, job->ed, job->nickname, job->json, &job->admission, &job->changes);
}

static void member_save_done(void* data, const struct db_async_result* result) {
    struct member_save_job* job = data;
    if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save ch=%s node=%016llx status=%d rc=%d: %s",
        job->channel, sc_public_key_hex(job->ed, (char[65]){0}), result->status, result->sqlite_code, result->message);
    else {
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "member save committed ch=%s peer=%s nick=%d owner=%d flags=%d live=%d",
            job->channel, sc_public_key_hex(job->ed, (char[65]){0}), job->nickname != NULL, job->json && job->json[0],
            job->changes, member_save_current(job));
        if (member_save_current(job) && job->admission) {
            struct group_policy_update* admission = job->admission; job->admission = NULL;
            group_policy_publish(job->inst, admission);
        }
        if (member_save_current(job) && job->changes)
            member_sync_record_committed(job->inst, job->channel, job->ed, (unsigned)job->changes);
        if (member_save_current(job)) {
            uint8_t event[65]; size_t length = strlen(job->channel);
            event[0] = (uint8_t)length; memcpy(event + 1, job->channel, length);
            chat_event_post(job->inst, CHAT_EVT_MEMBERS_CHANGED, event, (int)length + 1);
        }
    }
    if (job->done) job->done(job->arg, result);
}

static void member_save_free(void* data) {
    struct member_save_job* job = data;
    group_policy_discard(job->admission); OPENSSL_cleanse(job->context.private_key, sizeof(job->context.private_key));
    u_free(job->nickname); u_free(job->json); u_free(job);
}

void chat_core_save_member_async(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32],
    const char* nickname, const char* owner_json, db_async_done_fn done, void* arg) {
    struct TOPO_GROUP* group = inst && inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, channel) : NULL;
    struct db_async_result failure = {.status=DB_ASYNC_INVALID,.sqlite_code=SQLITE_MISUSE};
    if (!inst || !chat_core_generation(inst) || !chat_channel_valid(channel) || !ed || mf_zero(ed, 32) || !group || group->stopping ||
        (nickname && strlen(nickname) > 255) || (owner_json && strlen(owner_json) > MS_ADM_TAGS_MAX)) {
        snprintf(failure.message, sizeof(failure.message), "invalid member save context/channel/strings"); goto refused;
    }
    struct member_save_job* job = u_calloc(1, sizeof(*job));
    if (!job) { failure.status = DB_ASYNC_NOMEM; failure.sqlite_code = SQLITE_NOMEM; goto refused; }
    job->inst = inst; job->done = done; job->arg = arg;
    job->core_generation = chat_core_generation(inst); job->group_generation = group->generation;
    memcpy(job->channel, channel, strlen(channel) + 1);
    memcpy(job->ed, ed, 32);
    if (nickname) job->nickname = u_strdup(nickname);
    if (owner_json) job->json = u_strdup(owner_json);
    if ((nickname && !job->nickname) || (owner_json && !job->json)) {
        member_save_free(job); failure.status = DB_ASYNC_NOMEM; failure.sqlite_code = SQLITE_NOMEM; goto refused;
    }
    if (db_async_can_write(inst->db_worker)) {
        struct db_async_task task = {.name="member-save",.mode=DB_ASYNC_WRITE,.guard=member_save_guard,
            .work=member_save_work,.data=job,.done=member_save_done,.cleanup=member_save_free};
        db_async_run(inst->db_worker, &task); return;
    }
    sqlite3* db = chat_core_get_db(inst);
    if (!db || !sqlite3_get_autocommit(db)) {
        snprintf(failure.message, sizeof(failure.message), "legacy member save requires owned database transaction");
        member_save_done(job, &failure); member_save_free(job); return;
    }
    int owns = 0, rc = SQLITE_ABORT;
    if (member_save_guard(job)) {
        rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* member-save */", NULL, NULL, NULL); owns = rc == SQLITE_OK;
        if (!rc) rc = member_save_work(db, job);
        if (!rc) rc = sqlite3_exec(db, "COMMIT /* member-save */", NULL, NULL, NULL);
    }
    failure.status = rc == SQLITE_OK ? DB_ASYNC_OK : (rc == SQLITE_ABORT ? DB_ASYNC_CANCELLED : DB_ASYNC_SQL);
    failure.sqlite_code = rc; snprintf(failure.message, sizeof(failure.message), "%s", sqlite3_errstr(rc));
    if (owns && !sqlite3_get_autocommit(db)) {
        int rollback = sqlite3_exec(db, "ROLLBACK /* member-save */", NULL, NULL, NULL);
        if (rollback || !sqlite3_get_autocommit(db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save rollback rc=%d open=%d", rollback, !sqlite3_get_autocommit(db));
            if (!sqlite3_get_autocommit(db)) utun_instance_stop(inst);
        }
    }
    member_save_done(job, &failure); member_save_free(job); return;
refused:
    if (!failure.message[0]) snprintf(failure.message, sizeof(failure.message), "%s", sqlite3_errstr(failure.sqlite_code));
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save refused status=%d rc=%d: %s", failure.status, failure.sqlite_code, failure.message);
    if (done) done(arg, &failure);
}

/* Ник и подписанные поля подтверждаются вместе; SQL/подпись/конфликт не дают частичного сохранения. */
int chat_core_save_member(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32],
                         const char* nickname, const char* owner_json) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || !chat_channel_valid(channel) || !ed || mf_zero(ed, 32) || !nickname || !owner_json ||
        !sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: invalid context/channel/member or outer transaction"); return -1;
    }
    int result = -1;
    if (owner_json[0]) {
        struct chat_member_tags* t = chat_member_tags_load(inst, channel, ed);
        if (!t) return -1;
        if (!member_tags_patch(t, owner_json)) result = member_tags_commit(inst, t, nickname);
        chat_member_tags_free(t);
    } else {
        int rc = sqlite3_exec(cc->db, "BEGIN IMMEDIATE /* nickname */", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: begin rc=%d: %s", rc, sqlite3_errmsg(cc->db)); return -1; }
        if (!chat_member_write_nickname(cc->db, channel, ed, nickname)) {
            rc = sqlite3_exec(cc->db, "COMMIT /* nickname */", NULL, NULL, NULL);
            if (rc == SQLITE_OK) result = 0;
            else DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: commit rc=%d: %s", rc, sqlite3_errmsg(cc->db));
        }
        if (!sqlite3_get_autocommit(cc->db)) {
            rc = sqlite3_exec(cc->db, "ROLLBACK", NULL, NULL, NULL);
            if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: rollback rc=%d: %s", rc, sqlite3_errmsg(cc->db));
            if (!sqlite3_get_autocommit(cc->db)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: transaction remains open; requesting owner stop");
                utun_instance_stop(inst); result = -1;
            }
        }
    }
    if (result) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: refused ch=%s peer=%s result=%d",
        channel, sc_public_key_hex(ed, (char[65]){0}), result);
    else {
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "member save: committed ch=%s peer=%s nickname_bytes=%zu owner_edit=%d",
            channel, sc_public_key_hex(ed, (char[65]){0}), strlen(nickname), !!owner_json[0]);
        uint8_t event[65]; size_t length = strlen(channel); event[0] = (uint8_t)length; memcpy(event + 1, channel, length);
        chat_event_post(inst, CHAT_EVT_MEMBERS_CHANGED, event, length + 1);
    }
    return result;
}

/* ─── trampoline for chatgui ─── */
void chat_member_tags_commit_trampoline(void* arg) {
    struct chat_member_tags_req* req = (struct chat_member_tags_req*)arg;
    if (!req || !req->inst || !memchr(req->ch_id, 0, sizeof(req->ch_id)) || !memchr(req->json, 0, sizeof(req->json))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "tags: invalid trampoline request"); u_free(arg); return;
    }
    chat_core_save_member_async(req->inst, req->ch_id, req->ed, NULL, req->json, NULL, NULL);
    u_free(arg);
}
