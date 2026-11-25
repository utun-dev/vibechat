#include "../../../src/db_storage.h"
#include "chat/chat_devgroup.h"
/*
 * android_jni_bridge.c — JNI bridge implementation
 *
 * C side of the bridge. Wires Kotlin ↔ uTun stack:
 * - nativeStart(configText) → instance_lite_start()
 * - nativeJoinChannel(chId, nodeId, pubkey, addrs) → chat_sync_join_link()
 * - Chat events → JNI callback → Kotlin
 */
#include "android_jni_bridge.h"
#include "android_udp_log.h"
#include "../../../src/transport_layer/etcp_link_io.h"
#include "../libutun_lite/utun_config_api.h"
#include "../../../src/chat/invite_link.h"
#include "../../../src/chat/object_link.h"
#include "../libutun_lite/instance_lite.h"
#include "../libutun_lite/voice_recorder.h"
#include "../libutun_lite/attachment_sender.h"
#include "../libutun_lite/video_sender.h"
#include "../libutun_lite/photo_sender.h"
#include "../libutun_lite/standby.h"
#include "../../../src/call/call_audio.h"
#include "../../../src/call/call.h"
#include "../../../src/radio/radio.h"
#include "../../../src/radio/radio_call.h"
#include "../../../src/radio/radio_audio.h"
#include "../../../src/dm/dm_core.h"
#include "../../../src/dm/dm_crypto.h"
#include "../../../src/dm/dm_media.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/audio_compressor.h"
#include "../../../lib/u_async.h"
#include "../../../lib/mem.h"
#include "../../../lib/speex_ns.h"
#include "../../../lib/sqlite3.h"
#include "../../../src/chat/chat_core.h"
#include "../../../src/chat/member_sync.h"
#include "../../../src/media_async/encrypted_file.h"
#include "../../../src/media_async/attachment_send.h"
#include "../../../src/chat/chat_event.h"
#include "../../../src/chat/chat_clipboard.h"
#include "../../../src/chat/chat_local.h"
#include "../../../src/chat/chat_poll.h"
#include "../../../lib/strbuf.h"
#include "../../../lib/json_flat.h"
#include "../../../src/chat/chat_member.h"
#include "../../../src/chat/group_avatar.h"
#include "../../../src/chat/group_policy.h"
#include "../../../src/chat/group_info.h"
#include "../../../src/kanban/kanban.h"
#include "../../../src/member_files/member_files.h"
#include "../../../src/chat/invite_build.h"
#include "../../../src/chat/chat_setting.h"
#include "../../../src/chat/chat_state.h"
#include "../../../src/routing_layer/topo_node_sqlite.h"
#include "../../../src/utun_instance.h"
#include "../../../src/routing_layer/topo_group.h"
#include "../../../src/transport_layer/etcp.h"
#include "../../../src/transport_layer/etcp_connections.h"
#include "../../../src/transport_layer/auto_socket.h"
#include "../../../src/transport_layer/secure_channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include "../../../lib/platform_compat.h"
#include <sys/stat.h>

#ifdef __ANDROID__
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include "android_ble.h"
#include "../../cgathui_common/ble_etcp.h"
#define  LOG_TAG  "utun-bridge"
#else
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG   3
#define ANDROID_LOG_INFO    4
#define ANDROID_LOG_WARN    5
#define ANDROID_LOG_ERROR   6
#endif

enum { BLEV_TRACE = 0, BLEV_DEBUG = 1, BLEV_INFO = 2, BLEV_WARN = 3, BLEV_ERROR = 4 };

static int blevel_to_android(int blevel) {
    switch (blevel) {
        case BLEV_TRACE: return ANDROID_LOG_VERBOSE;
        case BLEV_DEBUG: return ANDROID_LOG_DEBUG;
        case BLEV_INFO:  return ANDROID_LOG_INFO;
        case BLEV_WARN:  return ANDROID_LOG_WARN;
        case BLEV_ERROR: return ANDROID_LOG_ERROR;
        default:         return ANDROID_LOG_VERBOSE;
    }
}

static utun_log_fn   g_on_log   = NULL;
static utun_event_fn g_on_event = NULL;

static char g_db_path[512];
static pthread_mutex_t g_noise_mutex = PTHREAD_MUTEX_INITIALIZER; /* Настройка NS и запуск audio упорядочены вместе. */
static int g_noise_suppress_db = SPEEX_NS_DEFAULT_DB; /* Подтверждённый DataStore-снимок, в том числе до запуска ядра. */

static void bridge_log(int level, const char* fmt, ...) {
    va_list ap;
    char fmt_buf[1024];
    va_start(ap, fmt);
    vsnprintf(fmt_buf, sizeof(fmt_buf), fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    __android_log_print(blevel_to_android(level), LOG_TAG, "%s", fmt_buf);
#else
    fprintf(stderr, "%s\n", fmt_buf);
#endif

    utun_log_fn on_log = __atomic_load_n(&g_on_log, __ATOMIC_ACQUIRE);
    if (on_log) on_log(level, "JNI", fmt_buf);
}

static void bridge_debug_hook(int level, const char* category, const char* message) {
    /* DEBUG_*: ERROR=1..TRACE=5; bridge/Kotlin: TRACE=0..ERROR=4. */
    int bridge_level = level >= DEBUG_LEVEL_ERROR && level <= DEBUG_LEVEL_TRACE ? DEBUG_LEVEL_TRACE - level : BLEV_DEBUG;
#ifdef __ANDROID__
    __android_log_print(blevel_to_android(bridge_level), LOG_TAG, "[%s] %s",
                        category ? category : "?", message ? message : "");
#else
    fprintf(stderr, "[%s] %s\n", category ? category : "?", message ? message : "");
#endif
    udp_log_write(message, (int)strlen(message));
    if (level == DEBUG_LEVEL_ERROR) udp_log_flush();
    utun_log_fn on_log = __atomic_load_n(&g_on_log, __ATOMIC_ACQUIRE);
    if (on_log) on_log(bridge_level, category, message);
}

static utun_config_string_fn  g_cfg_string  = NULL;
static utun_config_int64_fn   g_cfg_int64   = NULL;
static utun_config_int_fn     g_cfg_int     = NULL;

static const char* cfg_wrapper_string(const char* key, const char* def) {
    if (g_cfg_string) { const char* val = g_cfg_string(key); return val ? val : def; }
    return def;
}
static int64_t cfg_wrapper_int64(const char* key, int64_t def) {
    if (g_cfg_int64) return g_cfg_int64(key);
    return def;
}
static int cfg_wrapper_int(const char* key, int def) {
    if (g_cfg_int) return g_cfg_int(key);
    return def;
}

/* ── Public API ── */

void utun_bridge_log(int level, const char* category, const char* message) {
    utun_log_fn on_log = __atomic_load_n(&g_on_log, __ATOMIC_ACQUIRE);
    if (on_log) on_log(level, category, message);
}

void utun_bridge_event(int event_type, const char* json_data) {
    if (g_on_event) g_on_event(event_type, json_data);
}

bool utun_bridge_init(const char* db_path, int control_port,
                       utun_log_fn on_log,
                       utun_event_fn on_event,
                       const struct utun_bridge_config* config) {
    if (db_path && strlen(db_path) >= sizeof(g_db_path)) { bridge_log(BLEV_ERROR, "bridge init: database path too long"); return false; }
    snprintf(g_db_path, sizeof(g_db_path), "%s", db_path ? db_path : "");
    __atomic_store_n(&g_on_log, on_log, __ATOMIC_RELEASE);
    g_on_event = on_event;
    debug_set_log_hook(bridge_debug_hook);

    g_cfg_string = config ? config->get_string : NULL;
    g_cfg_int64  = config ? config->get_int64 : NULL;
    g_cfg_int    = config ? config->get_int : NULL;
    utun_config_provider_t provider = {
        .get_string = cfg_wrapper_string,
        .get_int64  = cfg_wrapper_int64,
        .get_int    = cfg_wrapper_int,
        .get_bool   = NULL,
    };
    utun_config_set_provider(config ? &provider : NULL);

    bridge_log(BLEV_INFO, "bridge init: db=%s port=%d (stub, use nativeStart for real init)",
               db_path ? db_path : "(null)", control_port);
    int result = voice_recorder_init(db_path);
    if (result) bridge_log(BLEV_ERROR, "bridge init: voice recorder failed rc=%d", result);
    return result == 0;
}

int utun_bridge_destroy(void) {
    if (instance_lite_stop()) { bridge_log(BLEV_ERROR, "bridge destroy refused: worker is not joined"); return -1; }
    voice_recorder_deinit();
    utun_config_set_provider(NULL);
    __atomic_store_n(&g_on_log, NULL, __ATOMIC_RELEASE);
    g_on_event = NULL;
    g_cfg_string = NULL;
    g_cfg_int64  = NULL;
    g_cfg_int    = NULL;
    bridge_log(BLEV_INFO, "bridge destroy");
    return 0;
}

/* Удалять только после остановки ядра: отсутствие файла нормально, остальные ошибки не скрывать. */
int utun_bridge_clear_database(const char* db_path) {
    if (instance_lite_get_uasync()) {
        bridge_log(BLEV_ERROR, "clearDatabase: refused before worker cleanup"); return -EBUSY;
    }
    return db_storage_clear(db_path);
}

int utun_bridge_start(const char* config_text) {
    bridge_log(BLEV_INFO, "bridge start: launching utun instance");
    int rc = instance_lite_start(config_text);
    if (rc != 0) {
        bridge_log(BLEV_ERROR, "bridge start: instance_lite_start failed rc=%d", rc);
    } else {
        bridge_log(BLEV_INFO, "bridge start: instance thread created");
    }
    return rc;
}

void utun_bridge_poll(void) {
}

/* Единственная передача simple request владельцу worker; отказ оставляет arg у нас. */
static int bridge_post_request(void (*callback)(void*), void* arg, struct UTUN_INSTANCE** instance_field) {
    if (!instance_lite_post(callback, arg, instance_field, NULL)) return 0;
    bridge_log(BLEV_WARN, "Core request rejected: worker unavailable or allocation failed");
    u_free(arg); return -1;
}

int utun_bridge_send_message(const char* channel_id, const char* text, const uint8_t reply_to[32], uint64_t request_id,
                             const char* reply_quote) {
    if (!channel_id || !text || !reply_to || (reply_quote && strlen(reply_quote) > DB_SYNC_MESSAGE_MAX)) {
        bridge_log(BLEV_ERROR, "send: invalid arguments/quote size"); return -1;
    }

    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "send: core stopped"); return -1; }

    size_t datalen = strlen(text);
    struct chat_msg_submit* req = u_calloc(1, sizeof(struct chat_msg_submit) + datalen);
    if (!req) { bridge_log(BLEV_ERROR, "send: allocation failed"); return -1; }

    req->inst = instance_lite_get_instance();
    strncpy(req->channel_id, channel_id, sizeof(req->channel_id) - 1);
    strncpy(req->content_type, "text/plain", sizeof(req->content_type) - 1);
    req->data = (uint8_t*)(req + 1);
    req->data_len = (uint32_t)datalen;
    memcpy(req->data, text, datalen);
    req->timestamp = 0;
    memcpy(req->reply_to, reply_to, sizeof(req->reply_to));
    if (reply_quote) strcpy(req->reply_quote, reply_quote);
    req->request_id = request_id;

    if (bridge_post_request(chat_core_submit_trampoline, req, &req->inst)) return -1;
    bridge_log(BLEV_INFO, "send queued ch=%s bytes=%zu request=%llu", channel_id, datalen, (unsigned long long)request_id);
    return 0;
}

static void bridge_create_group(const char* name, const char* type) {
    if (!name || !name[0] || strlen(name) >= 128 || !type || (strcmp(type, "chat") && strcmp(type, "kanban"))) {
        bridge_log(BLEV_ERROR, "create group: invalid name/type"); return;
    }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "create group: uasync not running"); return; }
    struct chat_create_auto_arg* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "create group: OOM"); return; }
    memcpy(req->name, name, strlen(name) + 1); strcpy(req->group_type, type);
    bridge_post_request(chat_core_create_channel_auto_trampoline, req, &req->inst);
}
void utun_bridge_create_channel(const char* name, const char* channel_id) {
    (void)channel_id; bridge_create_group(name, "chat");
}

void utun_bridge_connect_node(const char* address, int port, const char* pubkey_hex) {
    bridge_log(BLEV_INFO, "connect %s:%d pubkey=%s", address ? address : "", port, pubkey_hex ? pubkey_hex : "");
}

/* Real implementation: calls chat_sync_join_link() */
#include "chat/chat_sync.h"
#include "chat/invite_link.h"
#include "chat/chat_core.h"
#include "chat/chat_core_priv.h"

int utun_bridge_request_invite_candidates(const char* channel_id) {
    if (!channel_id || !channel_id[0]) {
        bridge_log(BLEV_ERROR, "request_invite_candidates: null ch_id");
        return -1;
    }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "request_invite_candidates: uasync not running"); return -1; }
    struct chat_invite_cand_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "request_invite_candidates: OOM"); return -1; }
    snprintf(req->ch_id, sizeof(req->ch_id), "%s", channel_id);
    bridge_log(BLEV_INFO, "request_invite_candidates ch=%s", channel_id);
    return bridge_post_request(chat_invite_candidates_trampoline, req, &req->inst);
}

int utun_bridge_request_invite_link(const char* channel_id, const uint8_t target[32], uint64_t request_id) {
    if (!channel_id || !channel_id[0]) {
        bridge_log(BLEV_ERROR, "request_invite_link: null ch_id");
        return -1;
    }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "request_invite_link: uasync not running"); return -1; }
    struct chat_invite_build_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "request_invite_link: OOM"); return -1; }
    strncpy(req->ch_id, channel_id, sizeof(req->ch_id) - 1);
    req->request_id = request_id;
    if (target) memcpy(req->target, target, 32);
    bridge_log(BLEV_INFO, "request_invite_link ch=%s target=%s",
               channel_id, sc_public_key_hex(target, (char[65]){0}));
    return bridge_post_request(chat_invite_build_link_trampoline, req, &req->inst);
}

void utun_bridge_connect_channel(const char* channel_id) {
    if (!channel_id || !channel_id[0]) {
        bridge_log(BLEV_ERROR, "connect_channel: null ch_id");
        return;
    }
    if (!instance_lite_is_running()) { bridge_log(BLEV_WARN, "connect_channel: core not ready"); return; }
    struct chat_create_auto_arg* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "connect_channel: request allocation failed"); return; }
    snprintf(req->name, sizeof(req->name), "%s", channel_id);
    bridge_log(BLEV_INFO, "connect_channel ch=%s", channel_id);
    bridge_post_request(chat_core_connect_channel_trampoline, req, &req->inst);
}

void utun_bridge_connect_member_node(const char* channel_id, const uint8_t ed[32]) {
    if (!chat_channel_valid(channel_id) || !ed || mf_zero(ed, 32)) {
        bridge_log(BLEV_ERROR, "connect_member_node: invalid args ch=%s peer=%s",
                   channel_id ? channel_id : "(null)", sc_public_key_hex(ed, (char[65]){0}));
        return;
    }
    if (!instance_lite_is_running()) { bridge_log(BLEV_WARN, "connect_member_node: core not ready"); return; }
    struct chat_connect_node_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "connect_member_node: OOM"); return; }
    strncpy(req->ch_id, channel_id, sizeof(req->ch_id) - 1);
    memcpy(req->ed, ed, sizeof(req->ed));
    bridge_log(BLEV_INFO, "connect_member_node ch=%s peer=%s", channel_id, sc_public_key_hex(ed, (char[65]){0}));
    bridge_post_request(chat_core_connect_node_trampoline, req, &req->inst);
}

static int bridge_get_node_name(sqlite3* db, const uint8_t ed[32], const char* channel_id, char* out, size_t sz) {
    out[0] = 0; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT name FROM nodes WHERE ed25519_pubkey=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(st, 0);
        if (name) snprintf(out, sz, "%s", name);
        rc = !name && sqlite3_column_type(st, 0) != SQLITE_NULL ? SQLITE_NOMEM : SQLITE_OK;
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (!rc && !*out && channel_id && *channel_id) {
        char table[80], sql[256]; peers_table_name(channel_id, table, sizeof(table)); st = NULL;
        snprintf(sql, sizeof(sql), "SELECT userinfo FROM \"%s\" WHERE ed25519_pubkey=? AND record_state=0", table);
        rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const char* profile = (const char*)sqlite3_column_text(st, 0);
            if (profile) json_flat_get(profile, "name", out, sz);
            rc = !profile && sqlite3_column_type(st, 0) != SQLITE_NULL ? SQLITE_NOMEM : SQLITE_OK;
        } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
        final = sqlite3_finalize(st); if (!rc) rc = final;
    }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "native author name peer=%s channel=%s rc=%d: %s",
        sc_public_key_hex(ed, (char[65]){0}), channel_id ? channel_id : "", rc, sqlite3_errstr(rc));
    return rc;
}

/* Формирует человекочитаемое тело превью последнего сообщения по content_type и "d"-полю. */
static void bridge_format_preview_body(const char* ct, const char* d, char* out, size_t out_sz) {
    if (ct && !strcmp(ct, ENCRYPTED_ATTACHMENT_TYPE)) { out[0] = 0; return; }
    int is_text = (!ct || !ct[0] || strcmp(ct, "text/plain") == 0 || strcmp(ct, "voice_transcription") == 0);
    if (is_text) {
        size_t tl = strlen(d);
        if (tl > 60) tl = 60;
        if (tl >= out_sz) tl = out_sz - 1;
        memcpy(out, d, tl); out[tl] = '\0';
        return;
    }
    if (strcmp(ct, "audio/opus") == 0) {
        int dur_ms = 0;
        if (d[0] == '*') {
            const char* pipe = strchr(d, '|');
            size_t b64len = pipe ? (size_t)(pipe - d - 1) : strlen(d) - 1;
            uint8_t dec[256]; size_t declen = b64_decode(d + 1, b64len, dec, sizeof(dec));
            if (declen > 0) {
                char raw[256]; if (declen >= sizeof(raw)) declen = sizeof(raw) - 1;
                memcpy(raw, dec, declen); raw[declen] = '\0';
                const char* dur = strstr(raw, ";dur=");
                if (dur) dur_ms = atoi(dur + 5);
            }
        }
        if (dur_ms > 0) {
            int s = dur_ms / 1000;
            snprintf(out, out_sz, "Voice message (%d:%02d)", s / 60, s % 60);
        } else {
            snprintf(out, out_sz, "Voice message");
        }
        return;
    }
    if (strncmp(ct, "video/", 6) == 0) { snprintf(out, out_sz, "Video"); return; }
    if (strncmp(ct, "image/", 6) == 0) { snprintf(out, out_sz, "Photo"); return; }
    /* файл и прочие нетекстовые: декодируем base64-префикс в имя файла */
    if (d[0] == '*') {
        const char* pipe = strchr(d, '|');
        size_t b64len = pipe ? (size_t)(pipe - d - 1) : strlen(d) - 1;
        uint8_t dec[512]; size_t declen = b64_decode(d + 1, b64len, dec, sizeof(dec));
        if (declen > 0) {
            if (declen >= out_sz) declen = out_sz - 1;
            memcpy(out, dec, declen); out[declen] = '\0';
            return;
        }
    }
    snprintf(out, out_sz, "File");
}

static int bridge_key_compare(const void* left, const void* right) { return memcmp(left, right, 32); }
/* Worker получает копию Ed25519 подключённых устройств, принадлежащую запросу. */
static int bridge_online_snapshot(struct UTUN_INSTANCE* inst, uint8_t (**out)[32], size_t* count) {
    *out = NULL; *count = 0;
    size_t capacity = inst->connections ? queue_entry_count(inst->connections) : 0;
    if (!capacity) return SQLITE_OK;
    if (capacity > UINT32_MAX / 32) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "native online snapshot is too large capacity=%zu", capacity); return SQLITE_TOOBIG;
    }
    uint8_t (*keys)[32] = u_malloc(capacity * sizeof(*keys));
    if (!keys) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "native online snapshot allocation failed capacity=%zu", capacity); return SQLITE_NOMEM; }
    for (struct ll_entry* entry = inst->connections->head; entry; entry = entry->next) {
        const struct ETCP_CONN* conn = ((struct conn_queue_entry*)entry->data)->conn;
        if (conn && conn->initialized && conn->links_up && memcmp(conn->peer_ed25519_pubkey, inst->my_ed25519_pubkey, 32))
            memcpy(keys[(*count)++], conn->peer_ed25519_pubkey, 32);
    }
    qsort(keys, *count, sizeof(*keys), bridge_key_compare);
    *out = keys; return SQLITE_OK;
}
static char* bridge_channels_json(sqlite3* db, const uint8_t (*online_ids)[32], size_t online_count, int* error) {
    struct strbuf out = strbuf_new();
    sqlite3_stmt *st = NULL, *last = NULL, *peers = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT channel_id,name,last_msg_at,group_type,ed25519_pubkey FROM channels ORDER BY last_msg_at DESC,name ASC", -1, &st, NULL);
    if (rc != SQLITE_OK || strbuf_addf(&out, "[") < 0) goto failed;
    unsigned count = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* channel = (const char*)sqlite3_column_text(st, 0);
        const char* name = (const char*)sqlite3_column_text(st, 1);
        if (!chat_channel_valid(channel)) { rc = SQLITE_CORRUPT; bridge_log(BLEV_ERROR, "get_channels: invalid stored channel ID"); goto failed; }
        const void* group_key = sqlite3_column_blob(st, 4);
        if (!group_key || sqlite3_column_bytes(st, 4) != 32) { rc = SQLITE_CORRUPT; goto failed; }
        char group_hex[65]; sc_public_key_hex(group_key, group_hex);
        char messages_table[80], peers_table[80], sql[512], preview[160] = "";
        msg_table_name(channel, messages_table, sizeof(messages_table));
        peers_table_name(channel, peers_table, sizeof(peers_table));
        const char* group_type = (const char*)sqlite3_column_text(st, 3); int encrypted = 0;
        if (group_type && !strcmp(group_type, "chat")) {
        snprintf(sql, sizeof(sql), "SELECT accepted_key,"
            "CASE WHEN json_valid(CAST(data AS TEXT)) THEN json_extract(CAST(data AS TEXT),'$.ct') END,"
            "CASE WHEN json_valid(CAST(data AS TEXT)) THEN json_extract(CAST(data AS TEXT),'$.d') END"
            " FROM \"%s\" ORDER BY id DESC LIMIT 1", messages_table);
        rc = sqlite3_prepare_v2(db, sql, -1, &last, NULL); if (rc) goto failed;
        int last_rc = sqlite3_step(last);
        if (last_rc == SQLITE_ROW) {
            const uint8_t* author = sqlite3_column_blob(last, 0);
            if (!author || sqlite3_column_bytes(last, 0) != 32) { rc = SQLITE_CORRUPT; goto failed; }
            const char* type = (const char*)sqlite3_column_text(last, 1);
            const char* data = (const char*)sqlite3_column_text(last, 2);
            char body[160] = "", author_name[128];
            encrypted = type && !strcmp(type, ENCRYPTED_ATTACHMENT_TYPE);
            bridge_format_preview_body(type, data ? data : "", body, sizeof(body));
            rc = bridge_get_node_name(db, author, channel, author_name, sizeof(author_name)); if (rc) goto failed;
            if (!author_name[0]) sc_public_key_hex(author, author_name);
            snprintf(preview, sizeof(preview), "%s: %s", author_name, body);
        } else if (last_rc != SQLITE_DONE) { rc = last_rc; goto failed; }
        rc = sqlite3_finalize(last); last = NULL; if (rc) goto failed;
        }
        snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey FROM \"%s\" WHERE record_state=0 AND admitted=1", peers_table);
        rc = sqlite3_prepare_v2(db, sql, -1, &peers, NULL); if (rc) goto failed;
        int online = 0, peer_rc;
        while ((peer_rc = sqlite3_step(peers)) == SQLITE_ROW) {
            if (sqlite3_column_type(peers, 0) != SQLITE_BLOB || sqlite3_column_bytes(peers, 0) != 32) { rc = SQLITE_CORRUPT; goto failed; }
            const void* key = sqlite3_column_blob(peers, 0);
            if (!key) { rc = SQLITE_NOMEM; goto failed; }
            if (online_count && bsearch(key, online_ids, online_count, sizeof(*online_ids), bridge_key_compare)) online++;
        }
        if (peer_rc != SQLITE_DONE) { rc = peer_rc; goto failed; }
        rc = sqlite3_finalize(peers); peers = NULL; if (rc) goto failed;
        if (strbuf_addf(&out, "%s{\"id\":", count++ ? "," : "") < 0 ||
            strbuf_add_json_string(&out, channel) < 0 || strbuf_addf(&out, ",\"name\":") < 0 ||
            strbuf_add_json_string(&out, name ? name : channel) < 0 ||
            strbuf_addf(&out, ",\"groupType\":") < 0 || strbuf_add_json_string(&out, group_type ? group_type : "") < 0 ||
            strbuf_addf(&out, ",\"groupEd\":\"%s\",\"lastMsgAt\":%lld,\"lastMsg\":", group_hex, (long long)sqlite3_column_int64(st, 2)) < 0 ||
            strbuf_add_json_string(&out, preview) < 0 ||
            strbuf_addf(&out, ",\"peersOnline\":%d,\"lastMsgEncrypted\":%s}", online, encrypted ? "true" : "false") < 0) goto failed;
    }
    if (rc != SQLITE_DONE || strbuf_addf(&out, "]") < 0) goto failed;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto failed;
    *error = SQLITE_OK;
    return strbuf_detach(&out);
failed:
    *error = rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : sqlite3_extended_errcode(db);
    if (!*error) *error = SQLITE_NOMEM;
    bridge_log(BLEV_ERROR, "get_channels: complete snapshot failed: %s", sqlite3_errmsg(db));
    sqlite3_finalize(last); sqlite3_finalize(peers); sqlite3_finalize(st); strbuf_free(&out);
    return NULL;
}

char* utun_bridge_get_channels_json(void) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance(); sqlite3* db = chat_core_get_db(inst);
    if (!chat_core_is_initialized(inst) || !db) { bridge_log(BLEV_WARN, "get_channels: chat/database unavailable"); return NULL; }
    uint8_t (*online)[32] = NULL; size_t count = 0; int error;
    if (bridge_online_snapshot(inst, &online, &count)) return NULL;
    char* json = bridge_channels_json(db, online, count, &error); u_free(online); return json;
}

static char* bridge_messages_json(sqlite3* db, const uint8_t self[32], const char* base, const char* channel_id,
    int limit, const uint8_t* anchor, int exact, int* error) {
    char table[80], sql[2048], selection[1024]; msg_table_name(channel_id, table, sizeof(table));
    if (exact && anchor) snprintf(selection, sizeof(selection), "SELECT * FROM \"%s\" WHERE message_id=?2", table);
    else if (anchor) snprintf(selection, sizeof(selection),
        "SELECT * FROM \"%s\" ORDER BY timestamp DESC,message_id DESC LIMIT ?1 OFFSET min("
        "max(0,(SELECT count(*) FROM \"%s\" n JOIN \"%s\" a ON a.message_id=?2 "
        "WHERE (n.timestamp,n.message_id)>(a.timestamp,a.message_id)) - ?1/2),"
        "max(0,(SELECT count(*) FROM \"%s\")-?1))", table, table, table, table);
    else snprintf(selection, sizeof(selection), "SELECT * FROM \"%s\" ORDER BY timestamp DESC,message_id DESC LIMIT ?1", table);
    snprintf(sql, sizeof(sql),
        "SELECT m.id,m.timestamp,m.local_attrs,m.message_id,"
        "CASE WHEN json_valid(CAST(data AS TEXT)) THEN json_extract(CAST(data AS TEXT),'$.ct') END,"
        "CASE WHEN json_valid(CAST(data AS TEXT)) THEN json_extract(CAST(data AS TEXT),'$.d') END,"
        "CASE WHEN json_valid(CAST(data AS TEXT)) THEN json_extract(CAST(data AS TEXT),'$.reply_to') END,"
        "a.ready,a.downloaded,a.decrypting,a.error,a.plain_path,a.info,a.content_type,COALESCE(r.state,-1),length(a.key)=32,m.ver,m.edited_at,"
        "CASE WHEN json_valid(CAST(data AS TEXT)) THEN json_extract(CAST(data AS TEXT),'$.reply_quote') END,m.accepted_key"
        " FROM (%s) m LEFT JOIN attachment_keys a ON a.channel=?3 AND a.message_id=m.message_id"
        " LEFT JOIN attachment_key_requests r ON r.channel=a.channel AND r.message_id=a.message_id AND r.incoming=0"
        " ORDER BY m.timestamp,m.message_id", selection);
    sqlite3_stmt* st = NULL;
    *error = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (*error != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge: messages query ch=%s: %s", channel_id, sqlite3_errmsg(db)); return NULL;
    }
    /* В exact запросе ?1 отсутствует, но индекс занят до ?2: SQLite допускает его bind. */
    *error = sqlite3_bind_int(st, 1, limit > 0 ? limit : 50);
    if (!*error && anchor) *error = sqlite3_bind_blob(st, 2, anchor, 32, SQLITE_STATIC);
    if (!*error) *error = sqlite3_bind_text(st, 3, channel_id, -1, SQLITE_STATIC);
    struct strbuf out = strbuf_new(); int step = SQLITE_OK, result = *error ? -1 : strbuf_addf(&out, "[");
    unsigned count = 0;
    while (result >= 0 && (step = sqlite3_step(st)) == SQLITE_ROW) {
        int64_t local_id = sqlite3_column_int64(st, 0), ts = sqlite3_column_int64(st, 1);
        const char* attrs = (const char*)sqlite3_column_text(st, 2);
        const uint8_t* id = sqlite3_column_blob(st, 3);
        const char* type = (const char*)sqlite3_column_text(st, 4), *body = (const char*)sqlite3_column_text(st, 5);
        const uint8_t* author_ed = sqlite3_column_blob(st, 19);
        if (!id || sqlite3_column_bytes(st, 3) != 32 || sqlite3_column_bytes(st, 5) > DB_SYNC_MESSAGE_MAX ||
            !author_ed || sqlite3_column_bytes(st, 19) != 32) {
            *error = SQLITE_CORRUPT;
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge: corrupt message ch=%s id=%lld", channel_id, (long long)local_id); result = -1; break;
        }
        char message_id[65], text[DB_SYNC_MESSAGE_MAX + 1];
        for (unsigned i = 0; i < 32; i++) snprintf(message_id + 2 * i, 3, "%02x", id[i]);
        snprintf(text, sizeof(text), "%s", body ? body : ""); if (!type) type = "text/plain";
        int encrypted = !strcmp(type, ENCRYPTED_ATTACHMENT_TYPE);
        /* У вложений '*' кодирует отображаемое имя; poll использует отдельный b64: формат. */
        if (!encrypted && strcmp(type, "text/plain") && text[0] == '*') {
            char* pipe = strchr(text, '|');
            if (pipe) {
                uint8_t decoded[1024]; size_t bytes = b64_decode(text + 1, pipe - text - 1, decoded, sizeof(decoded));
                size_t suffix = strlen(pipe);
                if (bytes && bytes + suffix < sizeof(text)) { memmove(text + bytes, pipe, suffix + 1); memcpy(text, decoded, bytes); }
            }
        }
        char author[128], author_hex[65], file_path[1024] = "";
        sc_public_key_hex(author_ed, author_hex);
        *error = bridge_get_node_name(db, author_ed, channel_id, author, sizeof(author));
        if (*error) { result = -1; break; }
        if (!author[0]) snprintf(author, sizeof(author), "%s", author_hex);
        if (!encrypted && attrs) {
            sqlite3_stmt* file = NULL;
            int file_rc = sqlite3_prepare_v2(db, "SELECT CASE WHEN json_valid(?1) THEN json_extract(?1,'$.fp') END", -1, &file, NULL);
            if (!file_rc) file_rc = sqlite3_bind_text(file, 1, attrs, -1, SQLITE_STATIC);
            if (!file_rc) file_rc = sqlite3_step(file);
            if (file_rc == SQLITE_ROW) {
                if (sqlite3_column_type(file, 0) == SQLITE_TEXT) {
                    int bytes = snprintf(file_path, sizeof(file_path), "%s/media/%s/%s",
                        base, channel_id, sqlite3_column_text(file, 0));
                    if (bytes >= (int)sizeof(file_path)) {
                        file_path[0] = 0; DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "bridge: media path too long ch=%s", channel_id);
                    }
                }
                file_rc = SQLITE_OK;
            }
            int final = sqlite3_finalize(file); if (!file_rc) file_rc = final;
            if (file_rc) {
                *error = file_rc; result = -1;
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge: local attributes query ch=%s rc=%d", channel_id, file_rc); break;
            }
        }
        const char* encryption_state = "";
        struct attachment_info info = {0};
        uint64_t file_size = 0;
        if (encrypted) {
            /* Шифротекст никогда не передаём UI как готовый файл; метаданные доступны только после decrypt. */
            const char* pipe = body ? strchr(body, '|') : NULL;
            if (pipe) file_size = strtoull(pipe + 1, NULL, 10);
            text[0] = 0;
            const char* path = (const char*)sqlite3_column_text(st, 11);
            const char* plain_type = (const char*)sqlite3_column_text(st, 13);
            const int ready = sqlite3_column_int(st, 7), downloaded = sqlite3_column_int(st, 8);
            const int decrypting = sqlite3_column_int(st, 9), error = sqlite3_column_int(st, 10);
            const int state = sqlite3_column_int(st, 14), key = sqlite3_column_int(st, 15);
            struct stat file;
            int available = ready && path && !stat(path, &file) && S_ISREG(file.st_mode);
            if (available && (!plain_type || !plain_type[0] || attachment_decode(sqlite3_column_blob(st, 12),
                    sqlite3_column_bytes(st, 12), &info))) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "bridge: encrypted metadata invalid ch=%s id=%lld", channel_id, (long long)local_id);
                available = 0; encryption_state = "error";
            }
            if (available) {
                int n = snprintf(file_path, sizeof(file_path), "%s", path);
                if (n >= (int)sizeof(file_path)) {
                    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "bridge: decrypted path too long ch=%s id=%lld", channel_id, (long long)local_id);
                    file_path[0] = 0; encryption_state = "error";
                } else {
                    encryption_state = "ready"; type = plain_type; file_size = (uint64_t)file.st_size;
                    snprintf(text, sizeof(text), "%s", info.name);
                }
            } else if (!encryption_state[0]) {
                if (error) encryption_state = "error";
                else if (state == 3) encryption_state = "denied";
                else if (decrypting) encryption_state = "decrypting";
                else if (ready) encryption_state = "download"; /* Открытая копия могла быть удалена кешем. */
                else if (downloaded && key) encryption_state = "decrypting";
                else if (state == 1) encryption_state = "pending";
                else if (state == 0 || downloaded) encryption_state = "requesting";
                else encryption_state = "download";
            }
        }
        result = strbuf_addf(&out, "%s{\"id\":%lld,\"messageId\":\"%s\",\"authorAddress\":\"%s\",\"author\":",
            count++ ? "," : "", (long long)local_id, message_id, author_hex);
        if (result >= 0) result = strbuf_add_json_string(&out, author);
        if (result >= 0) result = strbuf_addf(&out, ",\"ts\":%lld,\"isOutgoing\":%s,\"text\":",
            (long long)ts, !memcmp(author_ed, self, 32) ? "true" : "false");
        if (result >= 0) result = strbuf_add_json_string(&out, text);
        if (result >= 0) result = strbuf_addf(&out, ",\"ver\":%lld,\"editedAt\":%lld",
            (long long)sqlite3_column_int64(st, 16), (long long)sqlite3_column_int64(st, 17));
        if (result >= 0) result = strbuf_addf(&out, ",\"contentType\":");
        if (result >= 0) result = strbuf_add_json_string(&out, type);
        if (result >= 0) result = strbuf_addf(&out, ",\"filePath\":");
        if (result >= 0) result = strbuf_add_json_string(&out, file_path);
        if (result >= 0) result = strbuf_addf(&out, ",\"localAttrs\":");
        if (result >= 0) result = strbuf_add_json_string(&out, attrs);
        if (result >= 0) result = strbuf_addf(&out, ",\"replyTo\":");
        if (result >= 0) result = strbuf_add_json_string(&out, (const char*)sqlite3_column_text(st, 6));
        if (result >= 0) result = strbuf_addf(&out, ",\"replyQuote\":");
        if (result >= 0) result = strbuf_add_json_string(&out, (const char*)sqlite3_column_text(st, 18));
        if (result >= 0 && encrypted) {
            result = strbuf_addf(&out, ",\"encryptionState\":\"%s\",\"fileSize\":%llu,\"durationMs\":%u,"
                "\"width\":%u,\"height\":%u,\"waveform\":", encryption_state, (unsigned long long)file_size,
                info.duration_ms, info.width, info.height);
            if (result >= 0) result = strbuf_add_json_string(&out, info.waveform);
        }
        if (result >= 0) result = strbuf_addf(&out, "}");
    }
    { int final = sqlite3_finalize(st); if (!*error && final) *error = final; }
    if (*error || result < 0 || step != SQLITE_DONE || strbuf_addf(&out, "]") < 0) {
        if (!*error) *error = step != SQLITE_OK && step != SQLITE_ROW && step != SQLITE_DONE ? step : SQLITE_NOMEM;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge: messages snapshot failed ch=%s: %s", channel_id, sqlite3_errmsg(db));
        strbuf_free(&out); return NULL;
    }
    return strbuf_detach(&out);
}

char* utun_bridge_get_messages_json(const char* channel, int limit, const uint8_t* anchor, int exact) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance(); sqlite3* db = chat_core_get_db(inst); int error;
    if (!chat_core_is_initialized(inst) || !db || !chat_channel_valid(channel)) {
        bridge_log(BLEV_WARN, "get_messages: chat/database/channel unavailable"); return NULL;
    }
    return bridge_messages_json(db, inst->my_ed25519_pubkey, g_db_path, channel, limit, anchor, exact, &error);
}

/* ── DM (личные сообщения) ── */

int utun_bridge_dm_start(const uint8_t peer_ed[32], const char* source_ch_id, const char* peer_name) {
    if (!peer_ed) { bridge_log(BLEV_ERROR, "dm_start: missing peer key"); return -1; }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "dm_start: core not running"); return -1; }
    struct dm_start_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "dm_start: OOM"); return -1; }
    req->inst = instance_lite_get_instance();
    memcpy(req->peer_ed, peer_ed, 32);
    if (source_ch_id) strncpy(req->source_ch_id, source_ch_id, sizeof(req->source_ch_id) - 1);
    if (peer_name) strncpy(req->peer_name, peer_name, sizeof(req->peer_name) - 1);
    bridge_log(BLEV_INFO, "dm_start queued peer=%s ch=%s",
               sc_public_key_hex(peer_ed, (char[65]){0}), source_ch_id ? source_ch_id : "");
    return bridge_post_request(dm_start_trampoline, req, &req->inst);
}

struct bridge_dm_start_call {
    uint8_t ed[32];             /* Полный адрес удалённого устройства. */
    uint64_t conversation;      /* Фактический committed ID; 0 при отказе. */
    const char* source;
    const char* name;
    enum dm_mode mode;
    instance_lite_complete_fn complete;
    void* completion_arg;
};
/* Последнее действие — complete: после него frontend может освободить весь call и строки. */
static void bridge_dm_start_done(void* arg, const struct dm_start_result* result) {
    struct bridge_dm_start_call* call = arg;
    if (!result->status) call->conversation = result->conversation;
    else DEBUG_ERROR(DEBUG_CATEGORY_DM, "Android DM start failed peer=%s sqlite=%d", sc_public_key_hex(call->ed, (char[65]){0}), result->sqlite_code);
    instance_lite_complete_fn complete = call->complete; void* completion_arg = call->completion_arg;
    complete(completion_arg);
}
static void bridge_dm_start_begin(void* arg, instance_lite_complete_fn complete, void* completion_arg) {
    struct bridge_dm_start_call* call = arg; call->complete = complete; call->completion_arg = completion_arg;
    dm_start_submit(instance_lite_get_instance(), &(struct dm_start_query){.peer_ed=call->ed, .mode=call->mode,
        .source=call->source, .name=call->name ? call->name : ""}, bridge_dm_start_done, call);
}
uint64_t utun_bridge_dm_start_wait(const uint8_t peer_ed[32], const char* source, const char* name, int groups) {
    if (!peer_ed || groups < 0 || groups > 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "Android DM start: missing peer or invalid mode groups=%d", groups); return 0;
    }
    struct bridge_dm_start_call call = {.source=source, .name=name, .mode=groups ? DM_MODE_GROUPS : DM_MODE_DEVICE};
    memcpy(call.ed, peer_ed, 32);
    if (instance_lite_call_deferred(bridge_dm_start_begin, &call)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "Android DM start refused/cancelled peer=%s", sc_public_key_hex(peer_ed, (char[65]){0})); return 0;
    }
    return call.conversation;
}

int utun_bridge_dm_send(const char* conv_id, const uint8_t* text, size_t length, uint64_t generation, uint64_t request_id) {
    if (!conv_id || !conv_id[0] || strlen(conv_id) >= sizeof(((struct dm_send_req*)0)->conv_id) || !text ||
        !length || length > DM_DATA_MAX || memchr(text, 0, length) || !generation || !request_id) {
        bridge_log(BLEV_ERROR, "dm_send: invalid target/text length=%zu generation=%llu request=%llu", length,
                   (unsigned long long)generation, (unsigned long long)request_id); return -1;
    }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "dm_send: core unavailable request=%llu",
        (unsigned long long)request_id); return -1; }
    struct dm_send_req* req = u_calloc(1, sizeof(*req) + length);
    if (!req) { bridge_log(BLEV_ERROR, "dm_send: allocation failed request=%llu", (unsigned long long)request_id); return -1; }
    strcpy(req->conv_id, conv_id);
    req->generation = generation; req->request_id = request_id;
    req->data_len = (uint32_t)length;
    req->data = (uint8_t*)(req + 1);
    memcpy(req->data, text, length);
    if (bridge_post_request(dm_send_trampoline, req, &req->inst)) return -1;
    bridge_log(BLEV_DEBUG, "dm_send queued conv=%s bytes=%zu request=%llu", conv_id, length, (unsigned long long)request_id);
    return 0;
}

void utun_bridge_dm_request_conversations(void) {
    if (!instance_lite_is_running()) { bridge_log(BLEV_WARN, "DM conversations: core stopped"); return; }
    struct dm_conversations_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "DM conversations: OOM"); return; }
    bridge_post_request(dm_conversations_trampoline, req, &req->inst);
}

void utun_bridge_dm_request_messages(const char* conv_id, uint64_t generation) {
    if (!conv_id || !conv_id[0] || !generation || !instance_lite_is_running()) {
        bridge_log(BLEV_WARN, "DM history: invalid request/core stopped"); return;
    }
    struct dm_messages_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "DM history: OOM"); return; }
    req->generation = generation;
    snprintf(req->conv_id, sizeof(req->conv_id), "%s", conv_id);
    bridge_post_request(dm_messages_trampoline, req, &req->inst);
}

static void dm_file_discard(void* arg) {
    struct dm_file_req* req = arg;
    if (req->remove_source && remove(req->path) && errno != ENOENT)
        bridge_log(BLEV_WARN, "DM file cancellation cleanup failed path=%s errno=%d: %s", req->path, errno, strerror(errno));
    u_free(req);
}

int utun_bridge_dm_send_file(const char* conv_id, const char* path, const char* name, uint64_t generation, uint64_t request_id) {
    if (!conv_id || !path || !name || !generation || !request_id || !instance_lite_is_running()) {
        bridge_log(BLEV_WARN, "DM file: invalid request/core stopped"); return -1;
    }
    struct dm_file_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "DM file: OOM"); return -1; }
    if (strlen(conv_id) >= sizeof(req->conv) || strlen(path) >= sizeof(req->path) || strlen(name) >= sizeof(req->info.name)) {
        bridge_log(BLEV_ERROR, "DM file: path, conversation or name too long"); u_free(req); return -1;
    }
    req->inst = instance_lite_get_instance();
    req->generation = generation;
    req->request_id = request_id;
    snprintf(req->conv, sizeof(req->conv), "%s", conv_id);
    snprintf(req->path, sizeof(req->path), "%s", path);
    req->info.kind = ATTACHMENT_FILE;
    req->remove_source = 1; /* PM picker передаёт собственный уникальный cache-файл. */
    snprintf(req->info.name, sizeof(req->info.name), "%s", name);
    if (attachment_validate(&req->info)) { bridge_log(BLEV_ERROR, "DM file: invalid name"); u_free(req); return -1; }
    if (instance_lite_post(dm_send_file_trampoline, req, &req->inst, dm_file_discard)) {
        bridge_log(BLEV_WARN, "DM file: worker rejected request=%llu", (unsigned long long)request_id); u_free(req); return -1;
    }
    return 0;
}

void utun_bridge_set_debug_level(const char* category, const char* level) {
    if (!level) { bridge_log(BLEV_ERROR, "Debug level: missing level"); return; }
    if (!category || !category[0]) {
        debug_level_t lvl = debug_level_from_name(level);
        debug_set_level(lvl);
    } else {
        debug_set_category_level_by_name(category, level);
    }
    bridge_log(BLEV_INFO, "Debug level applied: %s=%s", category && category[0] ? category : "global", level);
}

void utun_bridge_chat_setting_set(const char* name, const char* value) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst) { bridge_log(BLEV_ERROR, "chat_setting_set: instance not running"); return; }
    chat_core_set_setting(inst, name, value);
}

int utun_bridge_chat_setting_get_int(const char* name, int def) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    return chat_setting_get_int(inst, name, def);
}

/* ──────────────────────────────────────────────────────────────────
 * Channel members (queried from shared DB)
 * ────────────────────────────────────────────────────────────────── */

#include "../../../src/routing_layer/topo_node_sqlite.h"
#include <openssl/evp.h>

static char* bridge_members_json(sqlite3* db, const uint8_t self[32], const char* channel_id, int* error) {
    char peers_tbl[80], sql[640]; peers_table_name(channel_id, peers_tbl, sizeof(peers_tbl));
    snprintf(sql, sizeof(sql),
        "SELECT COALESCE(n.name,''),COALESCE(p.local_nick,''),COALESCE(p.adm_tags,''),"
        "COALESCE(p.node_type,0),COALESCE(n.rtt,0),COALESCE(p.storage,0),COALESCE(p.userinfo,''),"
        "p.source,length(p.adm_tags_sig),COALESCE(p.deleted,0),p.radio_muted,hex(p.ed25519_pubkey),p.ed25519_pubkey=?"
        " FROM \"%s\" p LEFT JOIN nodes n ON p.ed25519_pubkey=n.ed25519_pubkey"
        " WHERE p.record_state=0 AND (p.admitted=1 OR p.source!=0 OR p.deleted!=0) ORDER BY p.ed25519_pubkey", peers_tbl);
    struct strbuf out = strbuf_new();
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, self, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK || strbuf_addf(&out, "[") < 0) goto failed;
    unsigned count = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(st, 0);
        const char* local_nick = (const char*)sqlite3_column_text(st, 1);
        const char* adm_tags = (const char*)sqlite3_column_text(st, 2);
        int node_type = sqlite3_column_int(st, 3);
        int node_rtt = sqlite3_column_int(st, 4);
        int storage_flag = sqlite3_column_int(st, 5);
        const char* ed = (const char*)sqlite3_column_text(st, 11);
        if (!ed || sqlite3_column_bytes(st, 11) != 64) { rc = ed ? SQLITE_CORRUPT : SQLITE_NOMEM; goto failed; }

        int is_admin = 0, is_super = 0, is_moder = 0, is_verified = 0, is_deleted = sqlite3_column_int(st, 9);
        int storage_role = 0;
        if (adm_tags) {
            char buf[MS_ADM_TAGS_MAX + 1];
            if (json_flat_get(adm_tags, "supernode", buf, sizeof(buf)) == 0 && strcmp(buf, "yes") == 0) is_super = 1;
            if (json_flat_get(adm_tags, "admin", buf, sizeof(buf)) == 0) {
                const char* last = buf; for (const char* p = buf; *p; p++) if (*p == 'e' || *p == 'd') last = p;
                if (*last == 'e') is_admin = 1;
            }
            if (json_flat_get(adm_tags, "moder", buf, sizeof(buf)) == 0) {
                const char* last = buf; for (const char* p = buf; *p; p++) if (*p == 'e' || *p == 'd') last = p;
                if (*last == 'e') is_moder = 1;
            }
            if (json_flat_get(adm_tags, "verified", buf, sizeof(buf)) == 0 && strcmp(buf, "yes") == 0) is_verified = 1;
            if (json_flat_get(adm_tags, "deleted", buf, sizeof(buf)) == 0 && strcmp(buf, "yes") == 0) is_deleted = 1;
            if (!is_deleted && sqlite3_column_int(st, 7) == 0 && sqlite3_column_int(st, 8) == 64 &&
                json_flat_get(adm_tags, "storage", buf, sizeof(buf)) == 0 && strcmp(buf, "yes") == 0) storage_role = 1;
        }

        if (strbuf_addf(&out, "%s{\"n\":", count++ ? "," : "") < 0 ||
            strbuf_add_json_string(&out, local_nick && local_nick[0] ? local_nick : name) < 0 ||
            strbuf_addf(&out, ",\"on\":0,\"ad\":%d,\"sp\":%d,\"md\":%d,\"st\":%d,"
                "\"vd\":%d,\"dl\":%d,\"tp\":%d,\"rt\":%d,\"is\":%d,\"ui\":",
                is_admin, is_super, is_moder, storage_flag,
                is_verified, is_deleted, node_type, node_rtt, !sqlite3_column_int(st, 7) && sqlite3_column_int(st, 12)) < 0 ||
            strbuf_add_json_string(&out, (const char*)sqlite3_column_text(st, 6)) < 0 ||
            strbuf_addf(&out, ",\"sr\":%d,\"rm\":%d,\"ed\":\"%s\"}", storage_role, sqlite3_column_int(st, 10), ed) < 0) goto failed;
    }
    if (rc != SQLITE_DONE || strbuf_addf(&out, "]") < 0) goto failed;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto failed;
    *error = SQLITE_OK;
    return strbuf_detach(&out);
failed:
    *error = rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : SQLITE_NOMEM;
    bridge_log(BLEV_ERROR, "Members complete snapshot failed channel=%s: %s", channel_id, sqlite3_errmsg(db));
    sqlite3_finalize(st); strbuf_free(&out); return NULL;
}

char* utun_bridge_get_channel_members_json(const char* channel) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance(); sqlite3* db = chat_core_get_db(inst); int error;
    if (!chat_core_is_initialized(inst) || !db || !chat_channel_valid(channel)) {
        bridge_log(BLEV_WARN, "Members snapshot: core/database/channel unavailable"); return NULL;
    }
    return bridge_members_json(db, inst->my_ed25519_pubkey, channel, &error);
}

static char* bridge_detail_json(sqlite3* db, const char* channel_id, const uint8_t peer[32], int* error) {
    struct strbuf out = strbuf_new(); sqlite3_stmt* st = NULL; const char* stage = "node";
    uint8_t transport[32]; int has_transport = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT COALESCE(name,''),COALESCE(last_seen_at,0),COALESCE(created_at,0),x25519_pubkey"
        " FROM nodes WHERE ed25519_pubkey=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, peer, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto failed;
    char peer_hex[65];
    if (strbuf_addf(&out, "{\"ed\":\"%s\"", sc_public_key_hex(peer, peer_hex)) < 0) { rc = SQLITE_NOMEM; goto failed; }
    if (rc == SQLITE_ROW) {
        const void* x = sqlite3_column_blob(st, 3);
        if (!x || sqlite3_column_bytes(st, 3) != 32) { rc = SQLITE_CORRUPT; goto failed; }
        memcpy(transport, x, 32); has_transport = 1;
        if (strbuf_addf(&out, ",\"n\":") < 0 || strbuf_add_json_string(&out, (const char*)sqlite3_column_text(st, 0)) < 0 ||
            strbuf_addf(&out, ",\"on\":0,\"ls\":%lld,\"cr\":%lld", (long long)sqlite3_column_int64(st, 1),
                (long long)sqlite3_column_int64(st, 2)) < 0) { rc = SQLITE_NOMEM; goto failed; }
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc) goto failed;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto failed;
    char connection_order[256] = ""; stage = "connection order";
    if (has_transport) {
        rc = topo_node_sqlite_connection_order_read(db, transport, connection_order); if (rc) goto failed;
    }
    if (strbuf_addf(&out, ",\"connection_order\":") < 0 || strbuf_add_json_string(&out, connection_order) < 0) {
        rc = SQLITE_NOMEM; goto failed;
    }
    char table[80], sql[512]; peers_table_name(channel_id, table, sizeof(table)); stage = "inviter";
    snprintf(sql, sizeof(sql), "SELECT p.signed_by,length(p.signature),p.source,COALESCE(NULLIF(i.local_nick,''),n.name,'')"
        " FROM \"%s\" p LEFT JOIN \"%s\" i ON i.ed25519_pubkey=p.signed_by AND i.record_state=0"
        " LEFT JOIN nodes n ON n.ed25519_pubkey=p.signed_by WHERE p.ed25519_pubkey=? AND p.record_state=0", table, table);
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, peer, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        int known = sqlite3_column_int(st, 1) == 64 && !sqlite3_column_int(st, 2);
        uint8_t signer[32];
        rc = topo_node_sqlite_read_optional_key(st, 0, signer); if (rc) goto failed;
        char hex[65]; const char* inviter = member_sync_signer(signer) ? sc_public_key_hex(signer, hex) : "group";
        if (strbuf_addf(&out, ",\"inviter_id\":") < 0 ||
            (known ? strbuf_add_json_string(&out, inviter) : strbuf_addf(&out, "null")) < 0 ||
            strbuf_addf(&out, ",\"inviter_name\":") < 0 ||
            strbuf_add_json_string(&out, known ? (const char*)sqlite3_column_text(st, 3) : "") < 0) {
            rc = SQLITE_NOMEM; goto failed;
        }
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc) goto failed;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto failed;
    stage = "addresses";
    rc = sqlite3_prepare_v2(db, "SELECT a.family,a.protocol,a.address,a.port,a.socket_id,a.addr_type,COALESCE(a.rtt,0),"
        "a.nat_mapping,a.nat_inbound,a.nat_source FROM node_addresses a JOIN nodes ON nodes.x25519_pubkey=a.node_x"
        " WHERE nodes.ed25519_pubkey=? ORDER BY socket_id,addr_type,family,port", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, peer, 32, SQLITE_STATIC);
    if (rc) goto failed;
    if (strbuf_addf(&out, ",\"adr\":[") < 0) { rc = SQLITE_NOMEM; goto failed; }
    int first = 1;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int family = sqlite3_column_int(st, 0), protocol = sqlite3_column_int(st, 1);
        const void* address = sqlite3_column_blob(st, 2); char text[INET6_ADDRSTRLEN];
        if ((family != 4 && family != 6) || !address || sqlite3_column_bytes(st, 2) != (family == 4 ? 4 : 16) ||
            !inet_ntop(family == 4 ? AF_INET : AF_INET6, address, text, sizeof(text))) { rc = SQLITE_CORRUPT; goto failed; }
        const char* transport = protocol & 4 ? "TCP+REALITY" : protocol == 1 ? "UDP" : protocol == 2 ? "TCP" : "UDP/TCP";
        if (strbuf_addf(&out, "%s{\"fm\":%d,\"pt\":\"%s\",\"ad\":\"%s\",\"po\":%d,\"si\":%d,\"at\":%d,\"rt\":%d,"
            "\"nmapping\":%d,\"ninbound\":%d,\"nsource\":%d}", first ? "" : ",", family, transport, text,
            sqlite3_column_int(st, 3), sqlite3_column_int(st, 4), sqlite3_column_int(st, 5), sqlite3_column_int(st, 6),
            sqlite3_column_int(st, 7), sqlite3_column_int(st, 8), sqlite3_column_int(st, 9)) < 0) { rc = SQLITE_NOMEM; goto failed; }
        first = 0;
    }
    if (rc != SQLITE_DONE) goto failed;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto failed;
    if (strbuf_addf(&out, "]}") < 0) { rc = SQLITE_NOMEM; goto failed; }
    *error = SQLITE_OK; return strbuf_detach(&out);
failed:
    *error = rc;
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "native member detail channel=%s peer=%s stage=%s rc=%d: %s",
        channel_id, sc_public_key_hex(peer, (char[65]){0}), stage, rc, sqlite3_errstr(rc));
    sqlite3_finalize(st); strbuf_free(&out); return NULL;
}

char* utun_bridge_get_member_detail_json(const char* channel, const uint8_t ed[32]) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance(); sqlite3* db = chat_core_get_db(inst); int error;
    if (!chat_core_is_initialized(inst) || !db || !chat_channel_valid(channel) || !ed || mf_zero(ed, 32)) return u_strdup("{}");
    return bridge_detail_json(db, channel, ed, &error);
}

struct bridge_json_job {
    struct UTUN_INSTANCE* inst; /* Только guard/done; SQL использует замороженные поля ниже. */
    uint64_t generation;
    uint8_t self_ed[32], peer[32];
    enum utun_bridge_json_kind kind;
    char channel[65], base[512];
    uint8_t anchor[32]; int anchored, exact, limit;
    uint8_t (*online)[32]; size_t online_count;
    char* json;
    utun_bridge_json_done_fn done; void* arg;
};
static int bridge_json_current(void* arg) {
    struct bridge_json_job* job = arg;
    return !job->inst->destroying && chat_core_generation(job->inst) == job->generation;
}
static int bridge_json_work(sqlite3* db, void* arg) {
    struct bridge_json_job* job = arg; int error = SQLITE_OK;
    switch (job->kind) {
        case UTUN_BRIDGE_JSON_CHANNELS: job->json = bridge_channels_json(db, job->online, job->online_count, &error); break;
        case UTUN_BRIDGE_JSON_MESSAGES:
            job->json = bridge_messages_json(db, job->self_ed, job->base, job->channel, job->limit,
                job->anchored ? job->anchor : NULL, job->exact, &error); break;
        case UTUN_BRIDGE_JSON_MEMBERS: job->json = bridge_members_json(db, job->self_ed, job->channel, &error); break;
        case UTUN_BRIDGE_JSON_DETAIL: job->json = bridge_detail_json(db, job->channel, job->peer, &error); break;
    }
    if (!error && !job->json) error = SQLITE_NOMEM;
    return error;
}
static void bridge_json_done(void* arg, const struct db_async_result* result) {
    struct bridge_json_job* job = arg; struct db_async_result outcome = *result;
    if (!outcome.status && !bridge_json_current(job)) {
        outcome.status = DB_ASYNC_CANCELLED; outcome.sqlite_code = SQLITE_ABORT;
        snprintf(outcome.message, sizeof(outcome.message), "native JSON owner changed after SQL");
    }
    char* json = outcome.status ? NULL : job->json;
    if (!outcome.status) job->json = NULL;
    if (outcome.status) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "native JSON complete kind=%d channel=%s status=%d rc=%d: %s",
        job->kind, job->channel, outcome.status, outcome.sqlite_code, outcome.message);
    else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "native JSON complete kind=%d channel=%s bytes=%zu", job->kind, job->channel, strlen(json));
    job->done(job->arg, &outcome, json);
}
static void bridge_json_free(void* arg) {
    struct bridge_json_job* job = arg; u_free(job->online); u_free(job->json); u_free(job);
}
void utun_bridge_json_read(struct UTUN_INSTANCE* inst, const struct utun_bridge_json_query* query,
    utun_bridge_json_done_fn done, void* arg) {
    struct db_async_result refused = {.status=DB_ASYNC_INVALID, .sqlite_code=SQLITE_MISUSE};
    uint64_t generation = chat_core_generation(inst);
    if (!done || !generation || inst->destroying || !query || query->kind < UTUN_BRIDGE_JSON_CHANNELS ||
        query->kind > UTUN_BRIDGE_JSON_DETAIL || (query->kind != UTUN_BRIDGE_JSON_CHANNELS && !chat_channel_valid(query->channel)) ||
        (query->kind == UTUN_BRIDGE_JSON_DETAIL && (!query->peer || mf_zero(query->peer, 32)))) goto fail;
    struct bridge_json_job* job = u_calloc(1, sizeof(*job));
    if (!job) { refused.status = DB_ASYNC_NOMEM; refused.sqlite_code = SQLITE_NOMEM; goto fail; }
    job->inst = inst; job->generation = generation; memcpy(job->self_ed, inst->my_ed25519_pubkey, 32);
    if (query->kind == UTUN_BRIDGE_JSON_DETAIL) memcpy(job->peer, query->peer, sizeof(job->peer));
    job->kind = query->kind; job->limit = query->limit; job->exact = query->exact; job->done = done; job->arg = arg;
    if (query->kind != UTUN_BRIDGE_JSON_CHANNELS) strcpy(job->channel, query->channel);
    if (query->anchor) { memcpy(job->anchor, query->anchor, 32); job->anchored = 1; }
    if (inst->config) snprintf(job->base, sizeof(job->base), "%s", inst->config->global.db_path);
    if (job->kind == UTUN_BRIDGE_JSON_CHANNELS) {
        int rc = bridge_online_snapshot(inst, &job->online, &job->online_count);
        if (rc) { bridge_json_free(job); refused.sqlite_code = rc; refused.status = rc == SQLITE_NOMEM ? DB_ASYNC_NOMEM : DB_ASYNC_INVALID; goto fail; }
    }
    if (inst->db_worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="native-json", .mode=DB_ASYNC_READ,
            .data=job, .work=bridge_json_work, .guard=bridge_json_current, .done=bridge_json_done, .cleanup=bridge_json_free}); return;
    }
    sqlite3* db = chat_core_get_db(inst); const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    if (!db || !file || *file) { bridge_json_free(job); refused.status = DB_ASYNC_CLOSED; refused.sqlite_code = SQLITE_OK; goto fail; }
    int rc = bridge_json_work(db, job);
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    bridge_json_done(job, &result); bridge_json_free(job); return;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "native JSON refused status=%d rc=%d", refused.status, refused.sqlite_code);
    if (done) done(arg, &refused, NULL);
}

int utun_bridge_toggle_member_radio_mute(const char* channel_id, const uint8_t ed[32]) {
    if (!instance_lite_is_running() || !chat_channel_valid(channel_id) || !ed || mf_zero(ed, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: Android mute request rejected: core unavailable or invalid member"); return -1;
    }
    struct chat_member_radio_mute_req* req = u_calloc(1, sizeof(*req));
    if (!req) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: Android mute request allocation failed"); u_free(req); return -1;
    }
    memcpy(req->ed, ed, sizeof(req->ed)); strcpy(req->ch_id, channel_id);
    if (bridge_post_request(chat_core_toggle_member_radio_mute_trampoline, req, &req->inst)) return -1;
    return 0;
}

void utun_bridge_set_member_nick(const char* channel_id, const uint8_t ed[32], const char* nick) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    sqlite3* db = chat_core_get_db(inst);
    if (!chat_core_is_initialized(inst) || !ed || chat_member_write_nickname(db, channel_id, ed, nick ? nick : "")) {
        bridge_log(BLEV_ERROR, "set_member_nick failed peer=%s", sc_public_key_hex(ed, (char[65]){0})); return;
    }
    bridge_log(BLEV_INFO, "set_member_nick saved ch=%s peer=%s", channel_id, sc_public_key_hex(ed, (char[65]){0}));
}

struct bridge_member_job {
    struct UTUN_INSTANCE* inst; /* Только guard/done; instance живёт до DB drain. */
    uint64_t generation;
    enum utun_bridge_member_query_kind kind;
    char channel[65];
    char* nickname;
    uint8_t ed[32];
    int value;
    utun_bridge_member_done_fn done;
    void* arg;
};
static int bridge_member_current(void* arg) {
    struct bridge_member_job* job = arg;
    return !job->inst->destroying && chat_core_generation(job->inst) == job->generation;
}
static int bridge_member_work(sqlite3* db, void* arg) {
    struct bridge_member_job* job = arg; char sql[256]; sqlite3_stmt* st = NULL;
    if (job->kind == UTUN_BRIDGE_LOCAL_NICK) {
        int code = chat_member_write_nickname(db, job->channel, job->ed, job->nickname ? job->nickname : "");
        job->value = !code; return code;
    }
    if (job->kind == UTUN_BRIDGE_OWNER)
        snprintf(sql, sizeof(sql), "SELECT typeof(ed25519_privkey),length(ed25519_privkey) FROM channels WHERE channel_id=?");
    else snprintf(sql, sizeof(sql), "SELECT 1 FROM peers_%s WHERE record_state=0 AND ed25519_pubkey=(SELECT ed25519_pubkey FROM local_identity WHERE id=1)", job->channel);
    const char* stage = "prepare"; int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) stage = "bind";
    if (!rc && job->kind == UTUN_BRIDGE_OWNER) rc = sqlite3_bind_text(st, 1, job->channel, -1, SQLITE_STATIC);
    if (!rc) { stage = "step"; rc = sqlite3_step(st); }
    if (rc == SQLITE_DONE) { job->value = 0; rc = SQLITE_OK; }
    else if (rc == SQLITE_ROW) {
        job->value = 1; rc = SQLITE_OK;
        if (job->kind == UTUN_BRIDGE_OWNER) {
            const char* type = (const char*)sqlite3_column_text(st, 0);
            if (!type) rc = SQLITE_NOMEM;
            else if (!strcmp(type, "null")) job->value = 0;
            else if (strcmp(type, "blob") || sqlite3_column_int(st, 1) != 32) rc = SQLITE_CORRUPT;
        }
    }
    int final = sqlite3_finalize(st); if (!rc) { stage = "finalize"; rc = final; }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge member SQL kind=%d channel=%s peer=%s stage=%s rc=%d: %s",
        job->kind, job->channel, sc_public_key_hex(job->ed, (char[65]){0}), stage, rc, sqlite3_errstr(rc));
    return rc;
}
static void bridge_member_done(void* arg, const struct db_async_result* result) {
    struct bridge_member_job* job = arg; struct db_async_result outcome = *result;
    if (!outcome.status && !bridge_member_current(job)) {
        outcome.status = DB_ASYNC_CANCELLED; outcome.sqlite_code = SQLITE_ABORT;
        snprintf(outcome.message, sizeof(outcome.message), "member query service changed after SQL");
    }
    if (outcome.status) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "bridge member complete kind=%d channel=%s status=%d rc=%d",
        job->kind, job->channel, outcome.status, outcome.sqlite_code);
    else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "bridge member complete kind=%d channel=%s value=%d", job->kind, job->channel, job->value);
    job->done(job->arg, &outcome, outcome.status ? -1 : job->value);
}
static void bridge_member_free(void* arg) {
    struct bridge_member_job* job = arg; u_free(job->nickname); u_free(job);
}
void utun_bridge_member_query(struct UTUN_INSTANCE* inst, enum utun_bridge_member_query_kind kind,
    const char* channel, const uint8_t* ed, const char* nickname, utun_bridge_member_done_fn done, void* arg) {
    struct db_async_result refused = {.status=DB_ASYNC_INVALID, .sqlite_code=SQLITE_MISUSE};
    uint64_t generation = chat_core_generation(inst);
    if (!done || !generation || inst->destroying || !chat_channel_valid(channel) ||
        kind < UTUN_BRIDGE_OWNER || kind > UTUN_BRIDGE_LOCAL_NICK ||
        (kind == UTUN_BRIDGE_LOCAL_NICK && (!ed || !memcmp(ed, (uint8_t[32]){0}, 32)))) goto fail;
    struct bridge_member_job* job = u_calloc(1, sizeof(*job));
    if (!job) { refused.status = DB_ASYNC_NOMEM; refused.sqlite_code = SQLITE_NOMEM; goto fail; }
    job->inst = inst; job->generation = generation; job->kind = kind; job->done = done; job->arg = arg;
    strcpy(job->channel, channel);
    if (ed) memcpy(job->ed, ed, 32);
    if (kind == UTUN_BRIDGE_LOCAL_NICK && nickname && *nickname) {
        job->nickname = u_strdup(nickname);
        if (!job->nickname) { bridge_member_free(job); refused.status = DB_ASYNC_NOMEM; refused.sqlite_code = SQLITE_NOMEM; goto fail; }
    }
    if (inst->db_worker && (kind != UTUN_BRIDGE_LOCAL_NICK || db_async_can_write(inst->db_worker))) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="bridge-member", .mode=kind == UTUN_BRIDGE_LOCAL_NICK ? DB_ASYNC_WRITE : DB_ASYNC_READ,
            .work=bridge_member_work, .data=job, .guard=bridge_member_current, .done=bridge_member_done, .cleanup=bridge_member_free});
        return;
    }
    sqlite3* db = chat_core_get_db(inst); const char* filename = db ? sqlite3_db_filename(db, "main") : NULL;
    if (!db || (!inst->db_worker && (!filename || *filename))) {
        bridge_member_free(job); refused.status = DB_ASYNC_CLOSED; refused.sqlite_code = SQLITE_OK; goto fail;
    }
    int write = kind == UTUN_BRIDGE_LOCAL_NICK, own = 0;
    int rc = !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : SQLITE_OK;
    if (!rc && write) { rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL); own = !rc; }
    if (!rc) rc = bridge_member_work(db, job);
    if (!rc && own) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc && own) {
        int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge member rollback channel=%s rc=%d", channel, rollback);
        if (!sqlite3_get_autocommit(db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge member transaction remains open channel=%s; stopping database owner", channel);
            utun_instance_stop(inst);
        }
    }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    bridge_member_done(job, &result); bridge_member_free(job); return;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge member refused kind=%d status=%d rc=%d", kind, refused.status, refused.sqlite_code);
    if (done) done(arg, &refused, -1);
}

struct member_tags_edit_req {
    struct UTUN_INSTANCE* inst;
    char     ch_id[65];
    uint8_t ed[32];
    int      admin, supernode, moder, storage, verified, deleted;
};

static void member_flags_done(void* arg, const struct db_async_result* result) {
    struct member_tags_edit_req* req = (struct member_tags_edit_req*)arg;
    bridge_log(result->status ? BLEV_ERROR : BLEV_INFO,
               "set_member_flags committed=%d ch=%s peer=%s admin=%d super=%d moder=%d storage=%d verified=%d deleted=%d status=%d rc=%d: %s",
               !result->status,
               req->ch_id, sc_public_key_hex(req->ed, (char[65]){0}), req->admin, req->supernode,
               req->moder, req->storage, req->verified, req->deleted, result->status, result->sqlite_code, result->message);
    u_free(req);
}

static void member_tags_edit_trampoline(void* arg) {
    struct member_tags_edit_req* req = arg;
    if (!req || !req->inst) { bridge_log(BLEV_ERROR, "set_member_flags: missing request/core"); u_free(arg); return; }
    char json[160];
    snprintf(json, sizeof(json), "{\"admin\":\"%s\",\"moder\":\"%s\",\"supernode\":\"%s\",\"storage\":\"%s\","
        "\"verified\":\"%s\",\"deleted\":\"%s\"}", req->admin ? "yes" : "no", req->moder ? "yes" : "no",
        req->supernode ? "yes" : "no", req->storage ? "yes" : "no", req->verified ? "yes" : "no", req->deleted ? "yes" : "no");
    chat_core_save_member_async(req->inst, req->ch_id, req->ed, NULL, json, member_flags_done, req);
}

int utun_bridge_is_channel_owner(const char* channel_id) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    sqlite3* db = chat_core_get_db(inst);
    if (!chat_core_is_initialized(inst) || !db || !channel_id || !channel_id[0]) return 0;
    uint8_t priv[32];
    return topo_node_sqlite_channel_get_priv(db, channel_id, priv) == 0 ? 1 : 0;
}

void utun_bridge_set_member_flags(const char* channel_id, const uint8_t ed[32],
                                   int admin, int supernode, int moder, int storage_flag,
                                   int verified, int deleted) {
    if (!channel_id || !ed || !memcmp(ed, (uint8_t[32]){0}, 32)) { bridge_log(BLEV_ERROR, "set_member_flags: invalid args"); return; }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "set_member_flags: uasync not running"); return; }
    struct member_tags_edit_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "set_member_flags: OOM"); return; }
    strncpy(req->ch_id, channel_id, sizeof(req->ch_id) - 1);
    memcpy(req->ed, ed, 32);
    req->admin = admin; req->supernode = supernode; req->moder = moder;
    req->storage = storage_flag; req->verified = verified; req->deleted = deleted;
    bridge_post_request(member_tags_edit_trampoline, req, &req->inst);
    bridge_log(BLEV_DEBUG, "set_member_flags queued ch=%s peer=%s admin=%d super=%d moder=%d storage=%d verified=%d deleted=%d",
               channel_id, sc_public_key_hex(ed, (char[65]){0}), admin, supernode, moder, storage_flag, verified, deleted);
}

/* Рабочий снимок sockaddr; пустой AF_UNSPEC допустим до появления адреса/результата NAT. */
static int bridge_address_parts(const struct sockaddr_storage* address, char* text, size_t size, int* family, int* port) {
    text[0] = 0; *family = 0; *port = 0;
    if (!address || !address->ss_family) return 0;
    const void* bytes;
    if (address->ss_family == AF_INET) {
        const struct sockaddr_in* value = (const struct sockaddr_in*)address;
        *family = 4; *port = ntohs(value->sin_port); bytes = &value->sin_addr;
    } else if (address->ss_family == AF_INET6) {
        const struct sockaddr_in6* value = (const struct sockaddr_in6*)address;
        *family = 6; *port = ntohs(value->sin6_port); bytes = &value->sin6_addr;
    } else {
        bridge_log(BLEV_ERROR, "Network snapshot: unsupported address family=%d", address->ss_family); return -1;
    }
    if (inet_ntop(address->ss_family, bytes, text, size)) return 0;
    bridge_log(BLEV_ERROR, "Network snapshot: inet_ntop failed family=%d errno=%d: %s", address->ss_family, errno, strerror(errno));
    return -1;
}

char* utun_bridge_get_member_links_json(const uint8_t ed[32]) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst || !ed) { bridge_log(BLEV_WARN, "Member links snapshot: no core/Ed25519"); return NULL; }
    struct ETCP_CONN* connection = instance_find_conn_by_ed(inst, ed);
    if (!connection) return u_strdup("{}");
    struct strbuf out = strbuf_new();
    if (strbuf_addf(&out, "{\"cn\":true,\"in\":%s,\"lu\":%d,\"lk\":[",
                    connection->initialized ? "true" : "false", connection->links_up) < 0) goto failed;
    unsigned count = 0;
    for (struct ETCP_LINK* link = connection->links; link; link = link->next) {
        struct etcp_link_snapshot snapshot;
        if (etcp_link_get_snapshot(link, &snapshot)) goto failed;
        char local_text[INET6_ADDRSTRLEN] = {0}, remote_text[INET6_ADDRSTRLEN] = {0};
        const struct etcp_link_address* addresses[] = {&snapshot.local, &snapshot.remote};
        char* texts[] = {local_text, remote_text};
        for (int i = 0; i < 2; i++) {
            int family = addresses[i]->family == 4 ? AF_INET : AF_INET6;
            if (addresses[i]->family && !inet_ntop(family, addresses[i]->bytes, texts[i], INET6_ADDRSTRLEN)) {
                bridge_log(BLEV_ERROR, "Member links snapshot: inet_ntop failed errno=%d: %s", errno, strerror(errno)); goto failed;
            }
        }
        if (strbuf_addf(&out, "%s{\"id\":%d,\"pt\":", count++ ? "," : "", snapshot.id) < 0 ||
            strbuf_add_json_string(&out, snapshot.protocol) < 0 ||
            strbuf_addf(&out, ",\"di\":%d,\"ip\":%s,\"lf\":%d,\"la\":\"%s\",\"lp\":%d,"
                "\"rf\":%d,\"ra\":\"%s\",\"rp\":%d,\"st\":%d,\"ls\":%d,\"sn\":",
                snapshot.direction, snapshot.has_ip ? "true" : "false", snapshot.local.family, local_text, snapshot.local.port,
                snapshot.remote.family, remote_text, snapshot.remote.port, snapshot.state, snapshot.status) < 0 ||
            strbuf_add_json_string(&out, snapshot.socket_name) < 0 || strbuf_addf(&out, "}") < 0) goto failed;
    }
    if (strbuf_addf(&out, "]}") < 0) goto failed;
    bridge_log(BLEV_DEBUG, "Member links snapshot peer=%s count=%u", sc_public_key_hex(ed, (char[65]){0}), count);
    return strbuf_detach(&out);
failed:
    bridge_log(BLEV_ERROR, "Member links snapshot failed peer=%s", sc_public_key_hex(ed, (char[65]){0}));
    strbuf_free(&out); return NULL;
}

/* ──────────────────────────────────────────────────────────────────
 * Передача прав админа (канальный приватный ключ) другому узлу
 * ────────────────────────────────────────────────────────────────── */

void utun_bridge_transfer_admin(const char* channel_id, const char* ed_hex) {
    uint8_t ed[32];
    if (!channel_id || sc_hex_to_binary(ed_hex, ed, sizeof(ed))) { bridge_log(BLEV_ERROR, "transfer_admin: invalid channel or Ed25519"); return; }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "transfer_admin: uasync not running"); return; }
    struct chat_transfer_admin_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "transfer_admin: OOM"); return; }
    strncpy(req->ch_id, channel_id, sizeof(req->ch_id) - 1);
    memcpy(req->target_ed, ed, 32);
    bridge_post_request(chat_core_transfer_admin_trampoline, req, &req->inst);
    bridge_log(BLEV_INFO, "transfer_admin queued ch=%s ed=%s", channel_id, ed_hex);
}

/* ──────────────────────────────────────────────────────────────────
 * Удаление канала (локальное)
 * ────────────────────────────────────────────────────────────────── */

void utun_bridge_delete_channel(const char* channel_id) {
    if (!channel_id || !channel_id[0]) { bridge_log(BLEV_ERROR, "delete_channel: empty ch_id"); return; }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "delete_channel: uasync not running"); return; }
    struct chat_delete_channel_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "delete_channel: OOM"); return; }
    strncpy(req->ch_id, channel_id, sizeof(req->ch_id) - 1);
    bridge_post_request(chat_core_delete_channel_trampoline, req, &req->inst);
    bridge_log(BLEV_INFO, "delete_channel queued ch=%s", channel_id);
}

/* ── Per-channel «play sound on message» ── */

int utun_bridge_get_sound_on_message(const char* channel_id) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst || !chat_core_is_initialized(inst) || !channel_id || !channel_id[0]) return 1;
    int on = chat_core_get_sound_on_message(inst, channel_id);
    return on < 0 ? 1 : on; /* UI fallback после залогированной ошибки,в сеть он не отправляется. */
}

void utun_bridge_set_sound_on_message(const char* channel_id, int on) {
    if (!channel_id || !channel_id[0]) { bridge_log(BLEV_ERROR, "set_sound: empty ch_id"); return; }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "set_sound: uasync not running"); return; }
    struct chat_sound_arg* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "set_sound: OOM"); return; }
    strncpy(req->ch_id, channel_id, sizeof(req->ch_id) - 1);
    req->on = on ? 1 : 0;
    bridge_post_request(chat_core_set_sound_on_message_trampoline, req, &req->inst);
    bridge_log(BLEV_INFO, "set_sound queued ch=%s on=%d", channel_id, on ? 1 : 0);
}

/* ──────────────────────────────────────────────────────────────────
 * Local sockets state (for auto-mode UI display)
 * ────────────────────────────────────────────────────────────────── */

#include "../../../lib/socket_compat.h"

static void bridge_count_socket_links(const struct ETCP_CONN* connection, const struct ETCP_SOCKET* socket, int* up, int* down) {
    if (!connection) return;
    for (const struct ETCP_LINK* link = connection->links; link; link = link->next) {
        if (link->conn != socket) continue;
        if (link->link_state == 3) (*up)++; else (*down)++;
    }
}

char* utun_bridge_get_local_sockets_json(void) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst || !chat_core_is_initialized(inst)) {
        bridge_log(BLEV_WARN, "Local sockets snapshot: core unavailable"); return NULL;
    }
    struct strbuf out = strbuf_new();
    if (strbuf_addf(&out, "[") < 0) goto failed;
    unsigned count = 0;
    for (struct ETCP_SOCKET* socket = inst->etcp_sockets; socket; socket = socket->next) {
        if (socket->probe_only) continue;
        const struct sockaddr_storage* local = socket->interface_addr.ss_family ? &socket->interface_addr : &socket->local_addr;
        char address[INET6_ADDRSTRLEN], external[INET6_ADDRSTRLEN];
        int family, port, external_family, external_port;
        if (bridge_address_parts(local, address, sizeof(address), &family, &port) ||
            bridge_address_parts(&socket->nat_addr, external, sizeof(external), &external_family, &external_port)) goto failed;
        int up = 0, down = 0;
        if (inst->connections) for (struct ll_entry* entry = inst->connections->head; entry; entry = entry->next) {
            struct conn_queue_entry* value = (struct conn_queue_entry*)entry->data;
            if (value) bridge_count_socket_links(value->conn, socket, &up, &down);
        }
        const char* protocol = socket->is_tcp ? socket->reality_enabled ? "TCP+REALITY" : "TCP" : "UDP";
        if (strbuf_addf(&out, "%s{\"si\":%u,\"nm\":", count++ ? "," : "", socket->sock_id) < 0 ||
            strbuf_add_json_string(&out, socket->name) < 0 ||
            strbuf_addf(&out, ",\"ad\":\"%s\",\"po\":%d,\"pt\":\"%s\",\"st\":2,\"lu\":%d,\"ld\":%d,"
                "\"nmapping\":%u,\"ninbound\":%u,\"nsource\":%u,\"external\":\"%s\",\"externalPort\":%d}",
                address, port, protocol, up, down, socket->address_profile.mapping, socket->address_profile.inbound,
                socket->address_profile.source, external, external_port) < 0) goto failed;
    }
    if (strbuf_addf(&out, "]") < 0) goto failed;
    bridge_log(BLEV_DEBUG, "Local sockets snapshot count=%u", count);
    return strbuf_detach(&out);
failed:
    bridge_log(BLEV_ERROR, "Local sockets snapshot failed; previous snapshot retained");
    strbuf_free(&out); return NULL;
}

static void bridge_network_change_run(void* unused) {
    (void)unused;
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst) return;
    auto_socket_on_network_change(inst);
    bridge_log(BLEV_DEBUG, "network change notified to auto_socket");
}

void utun_bridge_on_network_change(void) {
    if (instance_lite_post(bridge_network_change_run, NULL, NULL, NULL))
        bridge_log(BLEV_DEBUG, "Network change notification rejected: core not ready");
}

int utun_bridge_set_active(int active) {
    return instance_lite_set_active(active);
}

int utun_bridge_set_phone_state(int state, unsigned policy) {
    return instance_lite_set_phone_state(state, policy);
}

static void bridge_notify_network_active_trampoline(void* arg) {
    (void)arg;
    bridge_log(BLEV_DEBUG, "network activity notified to standby");
    standby_notify_network_activity();
}

void utun_bridge_notify_network_active(void) {
    if (instance_lite_post(bridge_notify_network_active_trampoline, NULL, NULL, NULL))
        bridge_log(BLEV_DEBUG, "Network activity notification rejected: core not ready");
}

void utun_bridge_collect_conn_list(void) {
    instance_lite_collect_conn_list();
}

void utun_bridge_collect_conn_metrics(uint64_t peer_node_id) {
    instance_lite_collect_conn_metrics(peer_node_id);
}

int utun_bridge_restart(const char* config_text) {
    bridge_log(BLEV_INFO, "bridge restart: posting to instance");
#ifdef __ANDROID__
    {
        void instance_event_handler(int type, const uint8_t* data, int len);
        instance_lite_set_event_handler(instance_event_handler);
    }
#endif
    return instance_lite_restart(config_text);
}

int utun_bridge_update_sockets(const char* config_text) {
    bridge_log(BLEV_INFO, "bridge update_sockets: posting to instance");
    return instance_lite_update_sockets(config_text);
}

void utun_bridge_ping(void) {
    instance_lite_ping();
}

int utun_bridge_is_responsive(void) {
    return instance_lite_is_responsive();
}

/* ──────────────────────────────────────────────────────────────────
 * Voice recording
 * ────────────────────────────────────────────────────────────────── */

int utun_bridge_voice_start(const char* channel_id, const uint8_t reply_to[32], uint64_t request_id, uint64_t generation,
                            const char* reply_quote) {
    voice_recorder_init(g_db_path); /* idempotent: гарантирует g_rec после stop/restart */
    return voice_recorder_start(channel_id, 48000, 1, reply_to, request_id, generation, reply_quote);
}

int utun_bridge_voice_feed(const int16_t* samples, int count) {
    return voice_recorder_feed(samples, count);
}

int utun_bridge_voice_stop(int* out_duration_ms) {
    return voice_recorder_stop(out_duration_ms);
}

void utun_bridge_voice_cancel(void) {
    voice_recorder_cancel();
}

int utun_bridge_voice_is_active(void) {
    return voice_recorder_is_active();
}

void utun_bridge_voice_set_preset(int preset) {
    voice_recorder_set_preset(preset);
}

int utun_bridge_voice_get_preset(void) {
    return voice_recorder_get_preset();
}

void utun_bridge_voice_set_compressor(int enabled) {
    voice_recorder_set_compressor_enabled(enabled);
}

int utun_bridge_voice_get_compressor(void) {
    return voice_recorder_is_compressor_enabled();
}

void utun_bridge_voice_set_compressor_config(int max_gain_db, int lookback_ms, int lookahead_ms,
                                              float rise_rate_per_sec, float target_level_db) {
    voice_recorder_set_compressor_config(max_gain_db, lookback_ms, lookahead_ms, rise_rate_per_sec, target_level_db);
}

float utun_bridge_voice_get_peak_level(void) {
    return voice_recorder_get_peak_level();
}

int utun_bridge_attachment_send(const char* channel_id, const char* file_path, const char* db_path,
                                const uint8_t reply_to[32], uint64_t request_id, const char* reply_quote) {
    return attachment_send(channel_id, file_path, db_path, reply_to, request_id, reply_quote);
}

int utun_bridge_attachment_download(const char* channel_id, int64_t msg_id) {
    if (!channel_id || msg_id <= 0) { bridge_log(BLEV_ERROR, "attachment_download: invalid args"); return -1; }
    if (!instance_lite_is_running()) { bridge_log(BLEV_WARN, "attachment_download: core not ready"); return -1; }
    struct attachment_dl_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "attachment_download: request allocation failed"); return -1; }
    snprintf(req->channel_id, sizeof(req->channel_id), "%s", channel_id);
    req->msg_id = msg_id;
    bridge_log(BLEV_INFO, "attachment_download: ch=%s msg_id=%lld", channel_id, (long long)msg_id);
    return bridge_post_request(chat_core_attachment_download_trampoline, req, &req->inst);
}

int utun_bridge_video_send(const char* channel_id, const char* file_path, const char* db_path,
                           int duration_ms, int width, int height, const uint8_t reply_to[32], uint64_t request_id, uint64_t generation,
                           const char* reply_quote) {
    return video_send(channel_id, file_path, db_path, duration_ms, width, height, reply_to, request_id, generation, reply_quote);
}

int utun_bridge_photo_send(const char* channel_id, const char* file_path, const char* db_path,
                           int width, int height, const uint8_t reply_to[32], uint64_t request_id,
                           const char* content_type, uint64_t generation, const char* reply_quote) {
    return photo_send(channel_id, file_path, db_path, width, height, reply_to, request_id, content_type, generation, reply_quote);
}

/* ──────────────────────────────────────────────────────────────────
 * Voice playback (Opus decode)
 * ────────────────────────────────────────────────────────────────── */

#include "../../../lib/opus_codec.h"

struct voice_decode_handle {
    opus_codec_decoder_t* dec;
    FILE* file;
    int sample_rate;
    int channels;
    int frame_samples;
    int total_samples;
    int pcm_consumed;
};

#define OPUS_MAGIC_D  0x5355504F

void* utun_bridge_voice_decode_open(const char* file_path,
                                     int* out_sample_rate, int* out_channels,
                                     int* out_total_samples) {
    if (!file_path || !out_sample_rate || !out_channels || !out_total_samples) return NULL;

    FILE* f = fopen(file_path, "rb");
    if (!f) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "voice_decode: cannot open %s", file_path); return NULL; }

    uint32_t magic = 0; uint32_t sr = 0; uint16_t ch = 0, fs = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != OPUS_MAGIC_D) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "voice_decode: bad magic"); fclose(f); return NULL; }
    if (fread(&sr, 4, 1, f) != 1 || fread(&ch, 2, 1, f) != 1 || fread(&fs, 2, 1, f) != 1 ||
        (sr != 8000 && sr != 12000 && sr != 16000 && sr != 24000 && sr != 48000) ||
        (ch != 1 && ch != 2) || fs != sr / 50) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice decode: invalid header rate=%u channels=%u frame=%u", sr, ch, fs);
        fclose(f); return NULL;
    }

    opus_codec_decoder_t* dec = opus_codec_decoder_create((int)sr, (int)ch);
    if (!dec) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice decode: decoder allocation failed"); fclose(f); return NULL; }

    struct voice_decode_handle* h = u_calloc(1, sizeof(*h));
    if (!h) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice decode: handle allocation failed"); opus_codec_decoder_destroy(dec); fclose(f); return NULL; }

    h->dec = dec; h->file = f;
    h->sample_rate = (int)sr; h->channels = (int)ch; h->frame_samples = (int)fs;

    /* count total frames/samples */
    long pos = ftell(f);
    h->total_samples = 0;
    for (;;) {
        uint16_t plen = 0;
        if (fread(&plen, 2, 1, f) != 1) goto invalid_stream;
        if (!plen) {
            if (!h->total_samples || fgetc(f) != EOF || ferror(f)) goto invalid_stream;
            break;
        }
        if (plen > 4000 || h->total_samples > INT_MAX - (int)fs * ch || fseek(f, plen, SEEK_CUR)) goto invalid_stream;
        h->total_samples += (int)fs * (int)ch;
    }
    if (fseek(f, pos, SEEK_SET)) goto invalid_stream;

    *out_sample_rate = h->sample_rate;
    *out_channels = h->channels;
    *out_total_samples = h->total_samples;

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "voice_decode_open: %s rate=%d ch=%d fs=%d total=%d",
               file_path, h->sample_rate, h->channels, h->frame_samples, h->total_samples);
    return h;
invalid_stream:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice decode: truncated/invalid stream path=%s samples=%d", file_path, h->total_samples);
    utun_bridge_voice_decode_close(h); return NULL;
}

int utun_bridge_voice_decode_read(void* handle, int16_t* buf, int max_samples) {
    struct voice_decode_handle* h = (struct voice_decode_handle*)handle;
    if (!h || !buf || max_samples <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice decode: invalid read handle=%p buffer=%p samples=%d", handle, (void*)buf, max_samples);
        return -1;
    }
    if (h->pcm_consumed >= h->total_samples) return 0;

    int frame = h->frame_samples * h->channels;
    int remaining = h->total_samples - h->pcm_consumed;
    int capacity = max_samples < remaining ? max_samples : remaining;
    DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "voice decode: requested=%d remaining=%d capacity=%d frame=%d",
                max_samples, remaining, capacity, frame);
    if (capacity < frame) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "voice decode: read buffer too small samples=%d frame=%d", max_samples, frame);
        return 0;
    }
    uint8_t packet[4096];
    int written = 0;

    /* capacity ограничена остатком записи; вычитание не переполняется даже при max_samples=INT_MAX. */
    while (capacity - written >= frame) {
        uint16_t plen;
        if (fread(&plen, 2, 1, h->file) != 1 || plen == 0 || plen > 4000 || fread(packet, 1, plen, h->file) != plen) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice decode: stream changed after validation"); return -1;
        }

        int decoded = opus_codec_decode(h->dec, packet, plen, buf + written, h->frame_samples);
        if (decoded != h->frame_samples) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice decode: invalid packet result=%d", decoded); return -1; }
        written += frame;
    }
    h->pcm_consumed += written;
    if (h->pcm_consumed == h->total_samples)
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "voice decode: complete samples=%d rate=%d channels=%d",
                    h->pcm_consumed, h->sample_rate, h->channels);
    return written;
}

void utun_bridge_voice_decode_close(void* handle) {
    if (!handle) return;
    struct voice_decode_handle* h = (struct voice_decode_handle*)handle;
    if (h->dec) opus_codec_decoder_destroy(h->dec);
    if (h->file && fclose(h->file)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "Voice decoder file close failed errno=%d: %s", errno, strerror(errno));
    u_free(h);
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "voice_decode_close");
}

/* ──────────────────────────────────────────────────────────────────
 * P2P аудио-звонок (сигналинг)
 * ────────────────────────────────────────────────────────────────── */

static int bridge_call_start(const char* ch_id, const uint8_t peer[32], uint64_t call_id, int devices) {
    if (!ch_id || !ch_id[0] || strlen(ch_id) >= sizeof(((struct call_start_arg*)0)->ch_id) || !call_id ||
        (strncmp(ch_id, "dm:", 3) && (!peer || mf_zero(peer, 32)))) {
        bridge_log(BLEV_ERROR, "call_start: bad args ch=%s peer=%s id=0x%llx",
                   ch_id ? ch_id : "(null)", sc_public_key_hex(peer, (char[65]){0}), (unsigned long long)call_id);
        return -1;
    }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "call_start: core unavailable"); return -1; }
    struct call_start_arg* a = (struct call_start_arg*)u_calloc(1, sizeof(*a));
    if (!a) { bridge_log(BLEV_ERROR, "call_start: allocation failed"); u_free(a); return -1; }
    snprintf(a->ch_id, sizeof(a->ch_id), "%s", ch_id);
    if (peer) memcpy(a->peer, peer, sizeof(a->peer));
    a->call_id = call_id;
    bridge_log(BLEV_INFO, "call_start ch=%s peer=%s id=0x%llx",
               ch_id, sc_public_key_hex(peer, (char[65]){0}), (unsigned long long)call_id);
    a->devices = devices;
    if (bridge_post_request(call_start_trampoline, a, &a->inst)) return -1;
    return 0;
}
int utun_bridge_call_start(const char* ch, const uint8_t peer[32], uint64_t id) { return bridge_call_start(ch, peer, id, 0); }


static int bridge_call_id_action(uint64_t call_id, const char* name,
                                  void (*trampoline)(void*)) {
    if (call_id == 0) { bridge_log(BLEV_ERROR, "call_%s: bad call_id=0", name); return -1; }
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "call_%s: core unavailable", name); return -1; }
    struct call_id_arg* a = (struct call_id_arg*)u_calloc(1, sizeof(*a));
    if (!a) { bridge_log(BLEV_ERROR, "call_%s: allocation failed", name); u_free(a); return -1; }
        a->call_id = call_id;
    bridge_log(BLEV_INFO, "call_%s id=0x%llx", name, (unsigned long long)call_id);
    if (bridge_post_request(trampoline, a, &a->inst)) return -1;
    return 0;
}

int utun_bridge_call_accept(uint64_t call_id)  { return bridge_call_id_action(call_id, "accept", call_accept_trampoline); }
int utun_bridge_call_decline(uint64_t call_id) { return bridge_call_id_action(call_id, "decline", call_decline_trampoline); }
int utun_bridge_call_hangup(uint64_t call_id)  { return bridge_call_id_action(call_id, "hangup", call_hangup_trampoline); }

/* ──────────────────────────────────────────────────────────────────
 * P2P аудио-звонок (медиа: call_audio.c)
 * ────────────────────────────────────────────────────────────────── */

int utun_bridge_call_audio_start(uint64_t call_id) {
    pthread_mutex_lock(&g_noise_mutex);
    int result = call_audio_start(call_id);
    if (!result && call_audio_set_noise_suppression(g_noise_suppress_db)) {
        call_audio_release(call_id); result = -1;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Android call start: noise suppression apply failed id=%016llx", (unsigned long long)call_id);
    }
    pthread_mutex_unlock(&g_noise_mutex);
    return result;
}

/* DataStore уже сохранён; текущие звонок/рация меняются между кадрами, запись — со следующего start. */
int utun_bridge_set_noise_suppression(int suppress_db) {
    if (suppress_db && (suppress_db < SPEEX_NS_MIN_DB || suppress_db > SPEEX_NS_MAX_DB)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Android noise suppression: invalid value=%d", suppress_db); return -1;
    }
    pthread_mutex_lock(&g_noise_mutex);
    g_noise_suppress_db = suppress_db;
    int voice = voice_recorder_init(g_db_path);
    if (!voice) voice = voice_recorder_set_noise_suppression(suppress_db);
    int call = call_audio_set_noise_suppression(suppress_db);
    int radio = radio_audio_set_noise_suppression(suppress_db);
    pthread_mutex_unlock(&g_noise_mutex);
    if (voice || call || radio) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Android noise suppression apply failed value=%d voice=%d call=%d radio=%d",
                    suppress_db, voice, call, radio); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "Android noise suppression applied value=%ddB voice=next_recording call/radio=live", suppress_db);
    return 0;
}

/* Сохранить настоящий микрофон для AEC/AGC; mute зануляет только результат перед Opus. */
int utun_bridge_call_audio_feed(uint64_t call_id, const int16_t* pcm, int count, int muted, int64_t capture_pts_us) {
    return call_audio_feed_pcm_muted(call_id, pcm, count, muted, capture_pts_us);
}

void utun_bridge_call_audio_stop(void) {
    call_audio_stop();
}

int utun_bridge_call_audio_pull(uint64_t call_id, int16_t* out, int max_samples) {
    return call_audio_pull_pcm(call_id, out, max_samples);
}

void utun_bridge_call_audio_set_aec(int enabled) {
    call_audio_set_aec_enabled(enabled);
}

void utun_bridge_call_audio_set_aec_delay(int frames) {
    call_audio_set_aec_delay_frames(frames);
}

/* ──────────────────────────────────────────────────────────────────
 * Рация (PTT walkie-talkie): подписка + аудио-движок
 * ────────────────────────────────────────────────────────────────── */

void utun_bridge_radio_set_active(uint64_t group_id, int on) {
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "radio_set_active: no instance/ua"); return; }
    struct radio_set_active_arg* a = (struct radio_set_active_arg*)u_calloc(1, sizeof(*a));
    if (!a) { bridge_log(BLEV_ERROR, "radio_set_active: request allocation failed"); return; }
    a->group_id = group_id;
    a->on = on;
    bridge_post_request(radio_set_active_trampoline, a, &a->inst);
}

int utun_bridge_radio_subscribers(uint64_t group_id) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst) return 0;
    return radio_subscribers(inst, group_id);
}

int utun_bridge_radio_audio_start(uint64_t group_id, int capture_channels) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio_audio_start: no instance"); return -1; }
    if (radio_audio_init(inst) != 0) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio_audio_start: radio_audio_init failed"); return -1; }
    pthread_mutex_lock(&g_noise_mutex);
    int result = radio_audio_start(group_id, capture_channels);
    if (!result) result = radio_audio_set_noise_suppression(g_noise_suppress_db);
    pthread_mutex_unlock(&g_noise_mutex);
    return result;
}

/* Тот же mutex, что у настройки NS: start не может восстановить устаревшую силу поверх нового значения. */
int utun_bridge_radio_capture_start(uint64_t group_id, int channels, int mode) {
    pthread_mutex_lock(&g_noise_mutex);
    int result = radio_audio_capture_start(group_id, channels, mode);
    if (!result) result = radio_audio_set_noise_suppression(g_noise_suppress_db);
    pthread_mutex_unlock(&g_noise_mutex);
    return result;
}

int utun_bridge_radio_audio_feed(uint64_t group_id, const int16_t* pcm, int count) {
    return radio_audio_feed_capture_pcm(group_id, pcm, count);
}

void utun_bridge_radio_audio_stop(void) {
    radio_audio_stop();
}

int utun_bridge_radio_audio_pull(uint64_t group_id, int16_t* out, int max_samples) {
    return radio_audio_pull_pcm(group_id, out, max_samples);
}

int utun_bridge_radio_auto_mode(void) {
    return radio_audio_auto_mode();
}

void utun_bridge_radio_talk_begin(uint64_t group_id) {
    radio_audio_talk_begin(group_id);
}

void utun_bridge_radio_talk_end(uint64_t group_id) {
    radio_audio_talk_end(group_id);
}

/* ──────────────────────────────────────────────────────────────────
 * JNI functions (compiled only for Android via NDK)
 * ────────────────────────────────────────────────────────────────── */

#ifdef __ANDROID__

static JavaVM* g_jvm = NULL;
static jobject g_config_callback = NULL;
static jmethodID g_radio_config_mid = NULL; /* Публикуется в init, очищается только после join. */
static int g_radio_config_available;        /* Producer читает только atomic-флаг, не JNI global ref. */
static jobject g_log_callback = NULL;
static pthread_mutex_t g_log_callback_lock = PTHREAD_MUTEX_INITIALIZER; /* Замена global ref / получение своего local ref. */
static __thread int g_log_callback_active; /* Callback не может рекурсивно пересылать native-лог того же потока. */
static jobject g_event_callback = NULL;
static jmethodID g_event_mid = NULL;
static pthread_mutex_t g_event_callback_lock = PTHREAD_MUTEX_INITIALIZER;

/* Только worker: свежая таблица BLE с адресом, RSSI=127 при неизвестном значении и счётчиком объявлений. */
static char* bluetooth_peers_json(void) {
    struct ble_neighbor peers[BLE_NEIGHBORS_MAX];
    size_t count = ble_etcp_neighbors(android_ble_driver(), peers, BLE_NEIGHBORS_MAX);
    struct strbuf out = strbuf_new();
    if (strbuf_addf(&out, "[") < 0) goto failed;
    for (size_t i = 0; i < count; i++) {
        const struct ble_neighbor* p = &peers[i];
        if (strbuf_addf(&out, "%s{\"id\":\"%016llx\",\"name\":", i ? "," : "", (unsigned long long)p->identity.node_id) < 0 ||
            strbuf_add_json_string(&out, p->name) < 0 || strbuf_addf(&out, ",\"address\":") < 0 ||
            strbuf_add_json_string(&out, p->endpoint.id) < 0 ||
            strbuf_addf(&out, ",\"rssi\":%d,\"broadcasts\":%llu}", p->rssi, (unsigned long long)p->broadcast_received) < 0) goto failed;
    }
    if (strbuf_addf(&out, "]") < 0) goto failed;
    DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "BLE devices snapshot count=%zu", count);
    return strbuf_detach(&out);
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE devices snapshot allocation/serialization failed count=%zu", count);
    strbuf_free(&out); return NULL;
}

enum bridge_query_kind { QUERY_CHANNELS, QUERY_MESSAGES, QUERY_MEMBERS, QUERY_DETAIL, QUERY_LINKS, QUERY_SIGNING_KEY, QUERY_SET_NICK,
                         QUERY_OWNER, QUERY_MEMBERSHIP, QUERY_SETTING, QUERY_SOUND, QUERY_RADIO_SUBSCRIBERS, QUERY_RADIO_START,
                         QUERY_BLUETOOTH_PEERS, QUERY_DM_MEMBERS };
/* Аргументы живут до deferred complete; SQL-задачи имеют собственные копии. JSON принадлежит JNI caller. */
struct bridge_query {
    enum bridge_query_kind kind;
    const char* channel;
    const char* value;
    const uint8_t* anchor;
    const uint8_t* ed;
    uint64_t node;
    uint8_t own_ed[32];
    int limit, exact;
    int64_t number;
    char* json;
    uint8_t* bytes; size_t size; /* Собственный бинарный снимок; JNI освобождает после копирования. */
    int status;
    instance_lite_complete_fn complete;
    void* completion_arg;
};
static void bridge_query_run(void* arg) {
    struct bridge_query* q = arg;
    switch (q->kind) {
        case QUERY_LINKS: q->json = utun_bridge_get_member_links_json(q->ed); break;
        case QUERY_BLUETOOTH_PEERS: q->json = bluetooth_peers_json(); break;
        case QUERY_SIGNING_KEY: {
            struct UTUN_INSTANCE* inst = instance_lite_get_instance();
            if (!inst || !chat_core_is_initialized(inst)) q->status = -1;
            else memcpy(q->own_ed, inst->my_ed25519_pubkey, sizeof(q->own_ed));
            break;
        }
        case QUERY_CHANNELS: case QUERY_MESSAGES: case QUERY_MEMBERS: case QUERY_DETAIL:
        case QUERY_SET_NICK: case QUERY_OWNER: case QUERY_MEMBERSHIP: case QUERY_SOUND: case QUERY_DM_MEMBERS:
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "deferred member query reached synchronous dispatcher kind=%d", q->kind);
            q->status = -1; break;
        case QUERY_SETTING: q->number = utun_bridge_chat_setting_get_int(q->channel, (int)q->number); break;
        case QUERY_RADIO_SUBSCRIBERS:
        case QUERY_RADIO_START: {
            struct UTUN_INSTANCE* inst = instance_lite_get_instance();
            uint64_t route = inst ? topo_groups_channel_slot(inst->topo_groups, q->channel) : 0;
            if (!route) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "Radio group unavailable group=%s", q->channel); break; }
            if (q->kind == QUERY_RADIO_SUBSCRIBERS) q->number = utun_bridge_radio_subscribers(route);
            else if (!utun_bridge_radio_audio_start(route, q->limit)) q->number = (int64_t)route;
            break;
        }
    }
}
static void bridge_query_member_done(void* arg, const struct db_async_result* result, int value) {
    struct bridge_query* q = arg; q->status = result->status; q->number = result->status ? 0 : value;
    q->complete(q->completion_arg);
}
static void bridge_query_sound_done(void* arg, const struct db_async_result* result, int on) {
    struct bridge_query* q = arg; q->status = result->status; q->number = result->status ? 1 : on;
    q->complete(q->completion_arg);
}
static void bridge_query_dm_members_done(void* arg, const struct dm_members_result* result) {
    struct bridge_query* q = arg; q->status = result->status;
    if (!result->status) {
        q->bytes = u_malloc(result->size);
        if (q->bytes) { memcpy(q->bytes, result->data, result->size); q->size = result->size; }
        else { q->status = -1; DEBUG_ERROR(DEBUG_CATEGORY_DM, "native DM member snapshot copy failed bytes=%zu", result->size); }
    }
    q->complete(q->completion_arg);
}
static void bridge_query_json_done(void* arg, const struct db_async_result* result, char* json) {
    struct bridge_query* q = arg; q->status = result->status; q->json = json; q->complete(q->completion_arg);
}
static void bridge_query_begin(void* arg, instance_lite_complete_fn complete, void* completion_arg) {
    struct bridge_query* q = arg; q->complete = complete; q->completion_arg = completion_arg;
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (q->kind == QUERY_CHANNELS || q->kind == QUERY_MESSAGES || q->kind == QUERY_MEMBERS || q->kind == QUERY_DETAIL) {
        enum utun_bridge_json_kind kind = q->kind == QUERY_CHANNELS ? UTUN_BRIDGE_JSON_CHANNELS :
            q->kind == QUERY_MESSAGES ? UTUN_BRIDGE_JSON_MESSAGES : q->kind == QUERY_MEMBERS ? UTUN_BRIDGE_JSON_MEMBERS : UTUN_BRIDGE_JSON_DETAIL;
        utun_bridge_json_read(inst, &(struct utun_bridge_json_query){.kind=kind, .channel=q->channel, .peer=q->ed,
            .limit=q->limit, .exact=q->exact, .anchor=q->anchor}, bridge_query_json_done, q); return;
    }
    if (q->kind == QUERY_DM_MEMBERS) {
        dm_members_submit(inst, &(struct dm_members_query){.conv=q->channel, .generation=q->node}, bridge_query_dm_members_done, q); return;
    }
    if (q->kind == QUERY_SOUND) { chat_sound_read(inst, q->channel, bridge_query_sound_done, q); return; }
    if (q->kind == QUERY_SET_NICK || q->kind == QUERY_OWNER || q->kind == QUERY_MEMBERSHIP) {
        enum utun_bridge_member_query_kind kind = q->kind == QUERY_SET_NICK ? UTUN_BRIDGE_LOCAL_NICK :
            q->kind == QUERY_OWNER ? UTUN_BRIDGE_OWNER : UTUN_BRIDGE_MEMBERSHIP;
        utun_bridge_member_query(inst, kind, q->channel, q->ed, q->value, bridge_query_member_done, q); return;
    }
    bridge_query_run(q); complete(completion_arg);
}
static int bridge_query_call(struct bridge_query* query) {
    /* Runtime getters могут вызываться из native event callback: call исполняет их inline в worker.
     * SQL-операции ждут только во frontend; deferred из самого uasync отклоняется без взаимного ожидания. */
    int deferred = query->kind == QUERY_CHANNELS || query->kind == QUERY_MESSAGES || query->kind == QUERY_MEMBERS ||
        query->kind == QUERY_DETAIL || query->kind == QUERY_SET_NICK || query->kind == QUERY_OWNER ||
        query->kind == QUERY_MEMBERSHIP || query->kind == QUERY_SOUND || query->kind == QUERY_DM_MEMBERS;
    int rc = deferred ? instance_lite_call_deferred(bridge_query_begin, query) : instance_lite_call(bridge_query_run, query);
    if (!rc && !query->status) return 0;
    bridge_log(BLEV_WARN, "Core query unavailable/cancelled kind=%d status=%d", query->kind, query->status); return -1;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

/* JNI log callback: удерживает свой local ref, передаёт обычный UTF-8 как byte[], освобождает все ссылки.
 * Ошибки идут прямо в logcat: bridge_log/DEBUG_* снова вызвали бы сломанный callback.
 * Свои Java-исключения описываем/снимаем; уже существующее исключение вызывающего JNI-кода сохраняем. */

static void jni_on_log(int level, const char* category, const char* message) {
    if (g_log_callback_active) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: recursive callback suppressed level=%d", level); return;
    }
    if (!g_jvm || !category || !message) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: invalid input vm=%p category=%p message=%p level=%d",
                            (void*)g_jvm, (const void*)category, (const void*)message, level); return;
    }
    g_log_callback_active = 1;
    JNIEnv* env = NULL;
    int attached = 0;
    jobject callback = NULL;
    jclass cls = NULL;
    jbyteArray values[2] = {NULL, NULL};
    const char* text[2] = {category, message};
    const char* fields[2] = {"category", "message"};
    jint res = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        res = (*g_jvm)->AttachCurrentThread(g_jvm, (void*)&env, NULL);
        if (res != JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: AttachCurrentThread failed rc=%d level=%d", res, level);
            goto finish;
        }
        attached = 1;
    } else if (res != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: GetEnv failed rc=%d level=%d", res, level); goto finish;
    }
    if ((*env)->ExceptionCheck(env)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: preserving preexisting Java exception level=%d", level);
        goto finish;
    }
    pthread_mutex_lock(&g_log_callback_lock);
    int configured = g_log_callback != NULL;
    if (configured) callback = (*env)->NewLocalRef(env, g_log_callback);
    pthread_mutex_unlock(&g_log_callback_lock);
    if (!configured) goto finish;
    if (!callback) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: NewLocalRef failed level=%d", level); goto cleanup;
    }
    cls = (*env)->GetObjectClass(env, callback);
    if (!cls) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: GetObjectClass failed level=%d", level); goto cleanup;
    }
    jmethodID mid = (*env)->GetMethodID(env, cls, "onNativeLog", "(I[B[B)V");
    if (!mid) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: GetMethodID onNativeLog(I[B[B)V failed level=%d", level);
        goto cleanup;
    }
    for (unsigned i = 0; i < 2; i++) {
        size_t length = strlen(text[i]);
        if (length > INT_MAX) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: %s too long bytes=%zu level=%d", fields[i], length, level);
            goto cleanup;
        }
        values[i] = (*env)->NewByteArray(env, (jsize)length);
        if (!values[i]) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: NewByteArray %s failed bytes=%zu level=%d", fields[i], length, level);
            goto cleanup;
        }
        if (length) (*env)->SetByteArrayRegion(env, values[i], 0, (jsize)length, (const jbyte*)text[i]);
        if ((*env)->ExceptionCheck(env)) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: SetByteArrayRegion %s failed bytes=%zu level=%d",
                                fields[i], length, level); goto cleanup;
        }
    }
    (*env)->CallVoidMethod(env, callback, mid, (jint)level, values[0], values[1]);
    if ((*env)->ExceptionCheck(env)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: onNativeLog threw level=%d", level);
    }
cleanup:
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    for (unsigned i = 0; i < 2; i++) if (values[i]) (*env)->DeleteLocalRef(env, values[i]);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (callback) (*env)->DeleteLocalRef(env, callback);
finish:
    if (attached) {
        res = (*g_jvm)->DetachCurrentThread(g_jvm);
        if (res != JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: DetachCurrentThread failed rc=%d level=%d", res, level);
        }
    }
    g_log_callback_active = 0;
}

/* Обычный UTF-8 в обе стороны; JNI modified UTF-8 оставляем только ASCII-литералу имени кодировки. */
static char* jni_utf8(JNIEnv* env, jstring value) {
    if (!value || (*env)->ExceptionCheck(env)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI UTF-8 input unavailable"); return NULL;
    }
    char* result = NULL;
    jclass strings = (*env)->GetObjectClass(env, value);
    jmethodID method = strings ? (*env)->GetMethodID(env, strings, "getBytes", "(Ljava/lang/String;)[B") : NULL;
    jstring charset = method ? (*env)->NewStringUTF(env, "UTF-8") : NULL;
    jbyteArray data = charset ? (*env)->CallObjectMethod(env, value, method, charset) : NULL;
    if (data && !(*env)->ExceptionCheck(env)) {
        jsize bytes = (*env)->GetArrayLength(env, data);
        result = u_malloc((size_t)bytes + 1);
        if (result) {
            (*env)->GetByteArrayRegion(env, data, 0, bytes, (jbyte*)result);
            if ((*env)->ExceptionCheck(env) || memchr(result, 0, (size_t)bytes)) {
                u_free(result); result = NULL;
            } else result[bytes] = 0;
        }
    }
    if (data) (*env)->DeleteLocalRef(env, data);
    if (charset) (*env)->DeleteLocalRef(env, charset);
    if (strings) (*env)->DeleteLocalRef(env, strings);
    if (!result) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI UTF-8 input conversion failed or contains NUL");
    return result;
}

static jstring jni_text(JNIEnv* env, const char* value) {
    if (!value || (*env)->ExceptionCheck(env)) return NULL;
    size_t bytes = strlen(value);
    if (bytes > INT_MAX) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI UTF-8 output too long bytes=%zu", bytes); return NULL;
    }
    jstring result = NULL;
    jclass strings = (*env)->FindClass(env, "java/lang/String");
    jmethodID method = strings ? (*env)->GetMethodID(env, strings, "<init>", "([BLjava/lang/String;)V") : NULL;
    jbyteArray data = method ? (*env)->NewByteArray(env, (jsize)bytes) : NULL;
    jstring charset = data ? (*env)->NewStringUTF(env, "UTF-8") : NULL;
    if (charset) {
        (*env)->SetByteArrayRegion(env, data, 0, (jsize)bytes, (const jbyte*)value);
        if (!(*env)->ExceptionCheck(env)) result = (*env)->NewObject(env, strings, method, data, charset);
    }
    if (data) (*env)->DeleteLocalRef(env, data);
    if (charset) (*env)->DeleteLocalRef(env, charset);
    if (strings) (*env)->DeleteLocalRef(env, strings);
    if (!result) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI UTF-8 output allocation failed bytes=%zu", bytes);
    return result;
}

/* ── JNI event callback (chat_event → Kotlin) ── */

static void jni_on_event(int type, const uint8_t* data, int len) {
    if (!g_jvm || len < 0 || (len && !data)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI event: invalid input type=%d bytes=%d", type, len); return;
    }
    JNIEnv* env = NULL;
    int attached = 0;
    jobject callback = NULL;
    jbyteArray jdata = NULL;
    jint res = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI event: attach failed type=%d", type);
            return;
        }
        attached = 1;
    } else if (res != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI event: GetEnv failed rc=%d type=%d", res, type);
        return;
    }
    if ((*env)->ExceptionCheck(env)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI event: preserving existing exception type=%d", type); goto finish;
    }
    pthread_mutex_lock(&g_event_callback_lock);
    jmethodID method = g_event_mid;
    if (g_event_callback && method) callback = (*env)->NewLocalRef(env, g_event_callback);
    pthread_mutex_unlock(&g_event_callback_lock);
    if (!callback) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI event: callback unavailable type=%d", type); goto cleanup;
    }
    if (len > 0) {
        jdata = (*env)->NewByteArray(env, len);
        if (!jdata) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI event: payload allocation failed type=%d bytes=%d", type, len); goto cleanup;
        }
        (*env)->SetByteArrayRegion(env, jdata, 0, len, (const jbyte*)data);
        if ((*env)->ExceptionCheck(env)) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI event: payload copy failed type=%d bytes=%d", type, len); goto cleanup;
        }
    }
    (*env)->CallVoidMethod(env, callback, method, (jint)type, jdata);
    if ((*env)->ExceptionCheck(env)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI event: callback threw type=%d bytes=%d", type, len);
    }
cleanup:
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (jdata) (*env)->DeleteLocalRef(env, jdata);
    if (callback) (*env)->DeleteLocalRef(env, callback);
finish:
    if (attached && (*g_jvm)->DetachCurrentThread(g_jvm) != JNI_OK)
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI event: detach failed type=%d", type);
}

void instance_event_handler(int type, const uint8_t* data, int len) {
    jni_on_event(type, data, len);
}

/* ── JNI call-audio: PCM отдаётся через nativeCallAudioPull (см. ниже) ── */

static jstring jni_get_string_callback(JNIEnv* env, const char* key) {
    if (!g_config_callback) return NULL;
    jclass cls = (*env)->GetObjectClass(env, g_config_callback);
    jmethodID mid = cls ? (*env)->GetMethodID(env, cls, "getString", "(Ljava/lang/String;)Ljava/lang/String;") : NULL;
    jstring jkey = mid ? jni_text(env, key) : NULL;
    jstring result = jkey ? (jstring)(*env)->CallObjectMethod(env, g_config_callback, mid, jkey) : NULL;
    if (jkey) (*env)->DeleteLocalRef(env, jkey);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    return result;
}

static const char* jni_cfg_get_string(const char* key) {
    JNIEnv* env = NULL;
    static __thread char buf[4096];
    buf[0] = 0;
    if (!g_jvm) { __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI config: JVM unavailable"); return NULL; }
    int attached = 0;
    jint rc = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        rc = (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
        if (rc == JNI_OK) attached = 1;
    }
    if (rc != JNI_OK) { __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI config: attach/GetEnv failed rc=%d", rc); return NULL; }
    if ((*env)->ExceptionCheck(env)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI config: preserving existing exception key=%s", key); goto finish;
    }
    jstring js = jni_get_string_callback(env, key);
    if (js && !(*env)->ExceptionCheck(env)) {
        char* result = jni_utf8(env, js);
        if (result) {
            size_t length = strlen(result);
            if (length < sizeof(buf)) memcpy(buf, result, length + 1);
            else __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI config: value too long key=%s bytes=%zu", key, length);
            u_free(result);
        }
    }
    if (js) (*env)->DeleteLocalRef(env, js);
    if ((*env)->ExceptionCheck(env)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI config: callback failed key=%s", key);
        (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env);
    }
finish:
    if (attached && (*g_jvm)->DetachCurrentThread(g_jvm) != JNI_OK)
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI config: detach failed key=%s", key);
    return buf[0] ? buf : NULL;
}

static int64_t jni_cfg_get_int64(const char* key) {
    const char* s = jni_cfg_get_string(key);
    return s ? (int64_t)atoll(s) : 0;
}

static int jni_cfg_get_int(const char* key) {
    const char* s = jni_cfg_get_string(key);
    return s ? atoi(s) : 0;
}

/* ── nativeStart(configText) — real uTun instance launch ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeStart(
    JNIEnv* env, jobject thiz, jstring configText) {
    (void)thiz;
    if (!configText) {
        bridge_log(BLEV_ERROR, "nativeStart: null config");
        return JNI_FALSE;
    }
    const char* cfg = jni_utf8(env, configText);
    if (!cfg) return JNI_FALSE;

    /* Set up event forwarder (bridge config/log are set by nativeInit) */
    instance_lite_set_event_handler(instance_event_handler);

    int rc = utun_bridge_start(cfg);
    u_free((void*)cfg);
    return (rc >= 0) ? JNI_TRUE : JNI_FALSE;
}

/* ── nativeStop() ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeStop(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (utun_bridge_destroy()) return JNI_FALSE;
    udp_log_stop();
    return JNI_TRUE;
}

/* ── nativeInit (legacy, just saves callbacks) ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeInit(
    JNIEnv* env, jobject thiz, jstring dbPath, jint controlPort,
    jobject configCallback, jobject logCb) {
    (void)thiz;

    __atomic_store_n(&g_radio_config_available, 0, __ATOMIC_RELEASE);
    g_radio_config_mid = NULL;
    if (g_config_callback) (*env)->DeleteGlobalRef(env, g_config_callback);
    g_config_callback = configCallback ? (*env)->NewGlobalRef(env, configCallback) : NULL;
    if (g_config_callback) {
        jclass cls = (*env)->GetObjectClass(env, g_config_callback);
        g_radio_config_mid = cls ? (*env)->GetMethodID(env, cls, "onRadioSettingsApplied", "(JZ)V") : NULL;
        if (cls) (*env)->DeleteLocalRef(env, cls);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "Android radio config ACK method is not configured");
        }
    }
    __atomic_store_n(&g_radio_config_available, g_radio_config_mid != NULL, __ATOMIC_RELEASE);

    if ((*env)->ExceptionCheck(env)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: nativeInit has pending config exception"); return JNI_FALSE;
    }
    jobject next_log_callback = logCb ? (*env)->NewGlobalRef(env, logCb) : NULL;
    if (logCb && !next_log_callback) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: nativeInit NewGlobalRef failed"); return JNI_FALSE;
    }
    pthread_mutex_lock(&g_log_callback_lock);
    jobject previous_log_callback = g_log_callback;
    g_log_callback = next_log_callback;
    pthread_mutex_unlock(&g_log_callback_lock);
    if (previous_log_callback) (*env)->DeleteGlobalRef(env, previous_log_callback);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "JNI log callback configured=%d; UTF-8 byte arrays", logCb != NULL);

    const char* db = dbPath ? jni_utf8(env, dbPath) : NULL;
    if (dbPath && !db) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "JNI log: nativeInit db path access failed"); return JNI_FALSE;
    }

    utun_log_fn log_cb = logCb ? jni_on_log : NULL;

    struct utun_bridge_config cfg = {
        .get_string = jni_cfg_get_string,
        .get_int    = jni_cfg_get_int,
    };

    bool ok = utun_bridge_init(db, (int)controlPort, log_cb, NULL, &cfg);
    if (db) u_free((void*)db);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/* ── nativeJoinChannel ── */

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeJoinChannel(
    JNIEnv* env, jobject thiz, jstring link, jlong request) {
    (void)thiz;
    if (!link) return CHAT_CONNECT_INVALID;
    const char* text = jni_utf8(env, link);
    if (!text) return CHAT_CONNECT_ERROR;
    int result = instance_lite_join_link((uint64_t)request, text, strlen(text));
    u_free((void*)text); return result;
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeAddServer(
    JNIEnv* env, jobject thiz, jstring channel, jstring link, jlong request) {
    (void)thiz;
    const char* key = channel ? jni_utf8(env, channel) : NULL;
    const char* text = link ? jni_utf8(env, link) : NULL;
    int result = CHAT_CONNECT_INVALID;
    if (text && chat_channel_valid(key)) result = instance_lite_add_server((uint64_t)request, key, text, strlen(text));
    else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "Android server addition requires full channel key and link");
    u_free((void*)key); u_free((void*)text); return result;
}

/* Представление результата общего C-кодека для предпросмотра Kotlin.
 * uint64 key/node передаются десятичными строками: числовой JSON может потерять биты при преобразовании в Double/Long. */
static int invite_hex_json(struct strbuf* out, const uint8_t* bytes, size_t len) {
    char hex[257];
    if (len > 128) return -1;
    for (size_t i = 0; i < len; i++) snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    hex[len * 2] = 0; return strbuf_add_json_string(out, hex);
}
JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_nativeDecodeInvite(
    JNIEnv* env, jobject thiz, jstring link) {
    (void)thiz;
    if (!link) return NULL;
    const char* text = jni_utf8(env, link);
    if (!text) return NULL;
    struct InviteData data;
    int result = invite_link_decode(text, strlen(text), &data, NULL, 0);
    u_free((void*)text);
    if (result) return NULL;
    struct strbuf out = strbuf_new();
    if (strbuf_addf(&out, "{\"kind\":%u,\"key\":\"%llu\",\"pubkey\":",
        data.kind, (unsigned long long)data.join_key) < 0 ||
        invite_hex_json(&out, data.pubkey, 32) < 0) goto failed;
    if (data.reality.has) {
        if (strbuf_addf(&out, ",\"reality\":{\"short\":") < 0 || invite_hex_json(&out, data.reality.short_id, 8) < 0 ||
            strbuf_addf(&out, ",\"pubkey\":") < 0 || invite_hex_json(&out, data.reality.server_pubkey, 32) < 0 ||
            strbuf_addf(&out, ",\"version\":") < 0 || invite_hex_json(&out, data.reality.version, 3) < 0 ||
            strbuf_addf(&out, ",\"name\":") < 0 || invite_hex_json(&out, (const uint8_t*)data.reality.server_name, strlen(data.reality.server_name)) < 0 ||
            strbuf_addf(&out, "}") < 0) goto failed;
    }
    if (strbuf_addf(&out, ",\"addresses\":[") < 0) goto failed;
    for (unsigned i = 0; i < data.addrCount; i++) {
        struct InviteAddr* a = &data.addrs[i];
        if (strbuf_addf(&out, "%s{\"family\":%d,\"socket\":%u,\"proto\":%u,\"port\":%u,\"ip\":",
            i ? "," : "", a->family, a->socketId, a->proto, a->port) < 0 ||
            invite_hex_json(&out, a->address, a->family == 6 ? 16 : 4) < 0 || strbuf_addf(&out, "}") < 0) goto failed;
    }
    if (strbuf_addf(&out, "]}") < 0) goto failed;
    jstring value = jni_text(env, strbuf_str(&out)); strbuf_free(&out); return value;
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "invite preview allocation failed"); strbuf_free(&out); return NULL;
}

/* ── nativeDeriveNodeId ── */

/* Чистый общий кодек, доступный также до запуска ядра; URL/подписи не попадают в лог. */
JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_decodeObjectLink(JNIEnv* env, jobject thiz, jstring link) {
    (void)thiz;
    const char* text = link ? jni_utf8(env, link) : NULL;
    if (!text) return NULL;
    struct object_link_target target;
    int rc = object_link_parse(text, strlen(text), &target, NULL, 0); u_free((void*)text);
    if (rc) return NULL;
    char json[256];
    snprintf(json, sizeof(json), "{\"kind\":%d,\"group\":\"%s\",\"card\":\"%s\",\"message\":\"%s\"}",
        target.kind, target.group, target.card, target.message);
    return jni_text(env, json);
}

JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_buildObjectLink(JNIEnv* env, jobject thiz,
    jstring group, jstring card, jstring message, jstring group_label, jstring card_label, jstring message_label) {
    (void)thiz;
    jstring args[] = {card, message, group_label, card_label, message_label, group}; const char* text[6] = {0};
    jstring result = NULL; struct object_link_target target = {0}; char url[OBJECT_LINK_MAX + 1];
    for (unsigned i = 0; i < 6; i++) {
        text[i] = args[i] ? jni_utf8(env, args[i]) : NULL;
        if (!text[i]) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "object link: JNI UTF-8 allocation failed part=%u", i); goto done; }
    }
    if (strlen(text[0]) > 64 || strlen(text[1]) > 64 || strlen(text[5]) != 64) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "object link: JNI ID too long"); goto done;
    }
    memcpy(target.group, text[5], 64);
    memcpy(target.card, text[0], strlen(text[0])); memcpy(target.message, text[1], strlen(text[1]));
    target.kind = !*target.card ? OBJECT_LINK_CHAT_MESSAGE : !*target.message ? OBJECT_LINK_CARD : OBJECT_LINK_CARD_MESSAGE;
    if (object_link_build(&target, text[2], text[3], text[4], url, sizeof(url)) >= 0) result = jni_text(env, url);
done:
    for (unsigned i = 0; i < 6; i++) u_free((void*)text[i]);
    return result;
}

JNIEXPORT jlong JNICALL Java_com_utun_chat_data_NativeLib_nativeDeriveNodeId(
    JNIEnv* env, jobject thiz, jbyteArray pubkey) {
    (void)thiz;
    if (!pubkey) return 0;
    jsize pk_len = (*env)->GetArrayLength(env, pubkey);
    if (pk_len < 32) return 0;
    uint8_t* pk = (uint8_t*)(*env)->GetByteArrayElements(env, pubkey, NULL);
    if (!pk) { bridge_log(BLEV_ERROR, "Node ID: public key JNI access failed"); return 0; }
    uint64_t node_id = sc_derive_node_id_from_pubkey(pk);
    (*env)->ReleaseByteArrayElements(env, pubkey, (jbyte*)pk, JNI_ABORT);
    return (jlong)node_id;
}

/* ── nativeRequestInviteCandidates / nativeRequestInviteLink ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRequestInviteCandidates(
    JNIEnv* env, jobject thiz, jstring channelId) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    int result = utun_bridge_request_invite_candidates(ch);
    if (ch) u_free((void*)ch);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRequestInviteLink(
    JNIEnv* env, jobject thiz, jstring channelId, jbyteArray target, jlong requestId) {
    (void)thiz;
    uint8_t ed[32];
    if (target) {
        if ((*env)->GetArrayLength(env, target) != sizeof(ed)) return JNI_FALSE;
        (*env)->GetByteArrayRegion(env, target, 0, sizeof(ed), (jbyte*)ed);
        if ((*env)->ExceptionCheck(env)) return JNI_FALSE;
    }
    const char* ch = jni_utf8(env, channelId);
    int result = utun_bridge_request_invite_link(ch, target ? ed : NULL, (uint64_t)requestId);
    if (ch) u_free((void*)ch);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCancelInviteLink(JNIEnv* env, jobject thiz, jlong request_id) {
    (void)env; (void)thiz;
    struct chat_invite_cancel_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "cancel_invite_link: OOM"); return JNI_FALSE; }
    req->request_id = (uint64_t)request_id;
    return bridge_post_request(chat_invite_cancel_window_trampoline, req, &req->inst) == 0 ? JNI_TRUE : JNI_FALSE;
}

/* ── nativeSetEventCallback(eventCb) ── */

static void local_devices_run(void* unused) {
    (void)unused; chat_local_peers_trampoline(instance_lite_get_instance());
}
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRequestLocalDevices(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return instance_lite_post(local_devices_run, NULL, NULL, NULL) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeSendLocalInvite(
    JNIEnv* env, jobject thiz, jbyteArray address, jlong request, jbyteArray link) {
    (void)thiz;
    jsize len = link ? (*env)->GetArrayLength(env, link) : 0;
    if (!instance_lite_is_running() || !address || (*env)->GetArrayLength(env, address) != CHAT_LOCAL_ADDRESS_SIZE ||
        !request || len <= 0 || len > LOCAL_TRANSPORT_DATA_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "invalid/stopped local send request bytes=%d", (int)len); return JNI_FALSE;
    }
    struct chat_local_send_request* req = u_calloc(1, sizeof(*req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "local send request allocation failed"); return JNI_FALSE; }
    uint8_t wire[CHAT_LOCAL_ADDRESS_SIZE];
    (*env)->GetByteArrayRegion(env, address, 0, sizeof(wire), (jbyte*)wire);
    if ((*env)->ExceptionCheck(env) || chat_local_address_decode(&req->address, wire)) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "invalid local invitation destination"); u_free(req); return JNI_FALSE;
    }
    req->request_id = (uint64_t)request;
    (*env)->GetByteArrayRegion(env, link, 0, len, (jbyte*)req->link);
    if ((*env)->ExceptionCheck(env)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "copying local invitation from JNI failed"); u_free(req); return JNI_FALSE;
    }
    int posted = bridge_post_request(chat_local_send_trampoline, req, &req->inst);
    return posted == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeSetEventCallback(
    JNIEnv* env, jobject thiz, jobject callback) {
    (void)thiz;
    jobject next = callback ? (*env)->NewGlobalRef(env, callback) : NULL;
    if (callback && !next) { bridge_log(BLEV_ERROR, "JNI event: callback allocation failed"); return; }
    jmethodID method = NULL;
    if (next) {
        jclass cls = (*env)->GetObjectClass(env, next);
        method = cls ? (*env)->GetMethodID(env, cls, "onEvent", "(I[B)V") : NULL;
        if (cls) (*env)->DeleteLocalRef(env, cls);
        if (!method) {
            (*env)->DeleteGlobalRef(env, next); bridge_log(BLEV_ERROR, "JNI event: onEvent method lookup failed"); return;
        }
    }
    pthread_mutex_lock(&g_event_callback_lock);
    jobject old = g_event_callback; g_event_callback = next; g_event_mid = method;
    pthread_mutex_unlock(&g_event_callback_lock);
    if (old) (*env)->DeleteGlobalRef(env, old);
    bridge_log(BLEV_INFO, "JNI event callback configured=%d", next != NULL);
}

/* ── Pass-through JNI wrappers (unchanged from before) ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeDestroy(
    JNIEnv* env, jobject thiz) {
    (void)thiz;
    if (utun_bridge_destroy()) return JNI_FALSE;
    udp_log_stop();
    __atomic_store_n(&g_radio_config_available, 0, __ATOMIC_RELEASE);
    g_radio_config_mid = NULL;
    if (g_config_callback) { (*env)->DeleteGlobalRef(env, g_config_callback); g_config_callback = NULL; }
    pthread_mutex_lock(&g_log_callback_lock);
    jobject previous_log_callback = g_log_callback;
    g_log_callback = NULL;
    pthread_mutex_unlock(&g_log_callback_lock);
    if (previous_log_callback) (*env)->DeleteGlobalRef(env, previous_log_callback);
    Java_com_utun_chat_data_NativeLib_nativeSetEventCallback(env, thiz, NULL);
    return JNI_TRUE;
}

/* Пустой массив означает отсутствие ссылки; иначе принимаем только полный глобальный ID. */
static int jni_message_id(JNIEnv* env, jbyteArray value, uint8_t id[32]) {
    memset(id, 0, 32);
    int length = value ? (*env)->GetArrayLength(env, value) : -1;
    if (length != 0 && length != 32) { bridge_log(BLEV_ERROR, "message ID: invalid byte length=%d", length); return -1; }
    if (length) (*env)->GetByteArrayRegion(env, value, 0, 32, (jbyte*)id);
    if ((*env)->ExceptionCheck(env)) { bridge_log(BLEV_ERROR, "message ID: JNI copy failed"); return -1; }
    return 0;
}

/* Обычный UTF-8 из Kotlin; пустой массив означает отсутствие цитаты. */
static int jni_reply_quote(JNIEnv* env, jbyteArray value, char out[DB_SYNC_MESSAGE_MAX + 1]) {
    int length = value ? (*env)->GetArrayLength(env, value) : -1;
    if (length < 0 || length > DB_SYNC_MESSAGE_MAX) {
        bridge_log(BLEV_ERROR, "quote: invalid UTF-8 bytes=%d limit=%d", length, DB_SYNC_MESSAGE_MAX); return -1;
    }
    if (length) (*env)->GetByteArrayRegion(env, value, 0, length, (jbyte*)out);
    if ((*env)->ExceptionCheck(env) || memchr(out, 0, (size_t)length)) {
        bridge_log(BLEV_ERROR, "quote: JNI copy failed or embedded NUL bytes=%d", length); return -1;
    }
    out[length] = 0; return 0;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeSendMessage(
    JNIEnv* env, jobject thiz, jstring channelId, jbyteArray text, jbyteArray replyTo, jlong requestId, jbyteArray replyQuote) {
    (void)thiz;
    uint8_t reply[32];
    char quote[DB_SYNC_MESSAGE_MAX + 1];
    if (jni_message_id(env, replyTo, reply) < 0 || jni_reply_quote(env, replyQuote, quote) < 0 || !text) return JNI_FALSE;
    jsize length = (*env)->GetArrayLength(env, text);
    if (length <= 0 || length > DB_SYNC_MESSAGE_MAX) { bridge_log(BLEV_ERROR, "send: invalid text bytes=%d", length); return JNI_FALSE; }
    char tx[DB_SYNC_MESSAGE_MAX + 1];
    (*env)->GetByteArrayRegion(env, text, 0, length, (jbyte*)tx); tx[length] = 0;
    if ((*env)->ExceptionCheck(env)) { bridge_log(BLEV_ERROR, "send: text copy failed"); return JNI_FALSE; }
    if (memchr(tx, 0, (size_t)length)) { bridge_log(BLEV_ERROR, "send: embedded NUL in text"); return JNI_FALSE; }
    const char* ch = jni_utf8(env, channelId);
    int rc = utun_bridge_send_message(ch, tx, reply, (uint64_t)requestId, quote);
    if (ch) u_free((void*)ch);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeEditMessage(
    JNIEnv* env, jobject thiz, jstring channelId, jbyteArray messageId, jlong version, jbyteArray text, jlong requestId) {
    (void)thiz;
    uint8_t id[32];
    if (!instance_lite_is_running() || !text || !channelId || version <= 0 || !messageId ||
        (*env)->GetArrayLength(env, messageId) != 32 || jni_message_id(env, messageId, id) < 0) {
        bridge_log(BLEV_ERROR, "edit: unavailable core/invalid target"); return JNI_FALSE;
    }
    jsize length = (*env)->GetArrayLength(env, text);
    if (length <= 0 || length > DB_SYNC_MESSAGE_MAX) { bridge_log(BLEV_ERROR, "edit: invalid bytes=%d", length); return JNI_FALSE; }
    struct chat_msg_submit* req = u_calloc(1, sizeof(*req) + length);
    if (!req) { bridge_log(BLEV_ERROR, "edit: allocation failed"); u_free(req); return JNI_FALSE; }
    req->data = (uint8_t*)(req + 1); req->data_len = length;
    req->expected_ver = (uint64_t)version; req->request_id = (uint64_t)requestId; memcpy(req->message_id, id, 32);
    (*env)->GetByteArrayRegion(env, text, 0, length, (jbyte*)req->data);
    const char* channel = jni_utf8(env, channelId);
    if (!channel || (*env)->ExceptionCheck(env) || strlen(channel) >= sizeof(req->channel_id)) {
        bridge_log(BLEV_ERROR, "edit: JNI copy failed");
        if (channel) u_free((void*)channel);
        u_free(req); return JNI_FALSE;
    }
    snprintf(req->channel_id, sizeof(req->channel_id), "%s", channel);
    u_free((void*)channel);
    int posted = bridge_post_request(chat_core_submit_trampoline, req, &req->inst);
    bridge_log(BLEV_INFO, "edit queued ver=%llu request=%llu", (unsigned long long)version, (unsigned long long)requestId);
    return posted == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeCreateChannel(
    JNIEnv* env, jobject thiz, jstring name, jstring channelId, jstring groupType) {
    (void)thiz;
    const char* n = jni_utf8(env, name);
    const char* c = jni_utf8(env, channelId);
    const char* type = jni_utf8(env, groupType);
    bridge_create_group(n, type);
    if (type) u_free((void*)type);
    if (c) u_free((void*)c);
    if (n) u_free((void*)n);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeKanbanRequest(
    JNIEnv* env, jobject thiz, jstring group, jlong request, jstring json) {
    (void)thiz;
    const char* channel = jni_utf8(env, group); const char* body = jni_utf8(env, json);
    int valid = chat_channel_valid(channel); size_t size = body ? strlen(body) : 0;
    struct kanban_request* req = valid && body && size <= 12000 ? u_calloc(1, sizeof(*req) + size + 1) : NULL;
    if (req) { strcpy(req->group, channel); req->request = (uint64_t)request; memcpy(req->json, body, size); }
    int rc = req ? bridge_post_request(kanban_gui_command, req, &req->inst) : -1;
    if (!req) bridge_log(BLEV_ERROR, "kanban request rejected: invalid payload/allocation");
    u_free((void*)channel); u_free((void*)body); return !rc ? JNI_TRUE : JNI_FALSE;
}

/* Composer ждёт commit независимо от времени жизни экрана доски. */
static void android_kanban_message_done(void* arg, const struct kanban_execute_result* result) {
    struct kanban_request* req = arg;
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "Android card message complete group=%s request=%llu status=%d committed=%d sqlite=%d",
        req->group, (unsigned long long)req->request, result->status, result->committed, result->sqlite_code);
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "Android card message failed: %s", result->json);
    chat_event_submitted(req->inst, req->request, result->status == 0);
    u_free(req);
}

static void android_kanban_message_start(void* arg) {
    struct kanban_request* req = arg;
    kanban_execute_submit(req->inst, req->group, req->json, android_kanban_message_done, req);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeKanbanMessageSend(
    JNIEnv* env, jobject thiz, jstring group, jlong request, jstring json) {
    (void)thiz;
    const char* channel = jni_utf8(env, group); const char* body = jni_utf8(env, json);
    int valid = chat_channel_valid(channel); size_t size = body ? strlen(body) : 0;
    struct kanban_request* req = valid && size && size <= 12000 ? u_calloc(1, sizeof(*req) + size + 1) : NULL;
    if (req) { strcpy(req->group, channel); req->request = (uint64_t)request; memcpy(req->json, body, size); }
    int rc = req ? bridge_post_request(android_kanban_message_start, req, &req->inst) : -1;
    if (!req) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "Android card message rejected group=%s bytes=%zu request=%lld",
        channel ? channel : "(null)", size, (long long)request);
    u_free((void*)channel); u_free((void*)body); return !rc ? JNI_TRUE : JNI_FALSE;
}

/* Только cache-файлы Android: caller сохраняет source при отказе очереди, worker удаляет после принятия. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeKanbanAttachmentSend(
    JNIEnv* env, jobject thiz, jstring group, jstring card, jstring path, jstring type,
    jint width, jint height, jint duration, jlong request) {
    (void)thiz;
    const char* channel = jni_utf8(env, group); const char* entity = jni_utf8(env, card);
    const char* source = jni_utf8(env, path); const char* mime = jni_utf8(env, type);
    struct attachment_send_req* req = NULL; int rc = -1;
    const char* name = source ? strrchr(source, '/') : NULL; name = name ? name + 1 : source;
    if (!channel || !chat_channel_valid(channel) || !entity || strlen(entity) != 64 ||
        strspn(entity, "0123456789abcdef") != 64 || !source || !*source || strlen(source) >= 1024 || !mime ||
        !name || strlen(name) >= 256 || width < 0 || width > 16384 || height < 0 || height > 16384 ||
        duration < 0 || duration > ATTACHMENT_DURATION_MAX_MS) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "Android card attachment invalid group=%s card=%s source_bytes=%zu name_bytes=%zu "
            "mime=%s size=%dx%d duration=%d request=%lld", channel ? channel : "(null)", entity ? entity : "(null)",
            source ? strlen(source) : 0, name ? strlen(name) : 0, mime ? mime : "(null)", width, height, duration, (long long)request);
        goto done;
    }
    req = u_calloc(1, sizeof(*req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "Android card attachment allocation failed"); goto done; }
    if (!strcmp(mime, "application/octet-stream")) req->info.kind = ATTACHMENT_FILE;
    else if (!strcmp(mime, "image/jpeg")) req->info.kind = ATTACHMENT_JPEG;
    else if (!strcmp(mime, "image/png")) req->info.kind = ATTACHMENT_PNG;
    else if (!strcmp(mime, "video/mp4")) req->info.kind = ATTACHMENT_VIDEO;
    else { DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "Android card attachment unsupported MIME=%s", mime); goto done; }
    snprintf(req->target, sizeof(req->target), "%s", channel); snprintf(req->card, sizeof(req->card), "%s", entity);
    snprintf(req->source, sizeof(req->source), "%s", source); snprintf(req->info.name, sizeof(req->info.name), "%s", name);
    req->info.width = (uint16_t)width; req->info.height = (uint16_t)height; req->info.duration_ms = (uint32_t)duration;
    if (attachment_validate(&req->info)) {
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "Android card attachment metadata rejected group=%s card=%s mime=%s", channel, entity, mime);
        goto done;
    }
    req->temporary_source = 1; req->request_id = (uint64_t)request;
    rc = instance_lite_post(attachment_send_trampoline, req, &req->inst, attachment_send_discard);
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "Android card attachment queue rejected group=%s card=%s request=%lld",
        channel, entity, (long long)request);
    else {
        DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "Android card attachment queued group=%s card=%s mime=%s size=%dx%d duration=%d request=%lld",
            channel, entity, mime, width, height, duration, (long long)request);
        req = NULL;
    }
done:
    u_free(req); u_free((void*)channel); u_free((void*)entity); u_free((void*)source); u_free((void*)mime);
    return !rc ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeConnectNode(
    JNIEnv* env, jobject thiz, jstring address, jint port, jstring pubkeyHex) {
    (void)thiz;
    const char* a = jni_utf8(env, address);
    const char* p = jni_utf8(env, pubkeyHex);
    utun_bridge_connect_node(a, (int)port, p);
    if (p) u_free((void*)p);
    if (a) u_free((void*)a);
}

JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_nativeGetChannels(
    JNIEnv* env, jobject thiz) {
    (void)thiz;
    struct bridge_query query = {.kind = QUERY_CHANNELS};
    if (bridge_query_call(&query)) return NULL;
    char* json = query.json;
    jstring result = jni_text(env, json);
    u_free(json);
    return result;
}

JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_nativeGetMessages(
    JNIEnv* env, jobject thiz, jstring channelId, jint limit, jbyteArray anchor, jboolean exact) {
    (void)thiz;
    uint8_t id[32];
    if (jni_message_id(env, anchor, id) < 0) return NULL;
    const char* ch = jni_utf8(env, channelId);
    if (!ch) return NULL;
    struct bridge_query query = {.kind = QUERY_MESSAGES, .channel = ch, .limit = limit,
        .anchor = (*env)->GetArrayLength(env, anchor) ? id : NULL, .exact = exact};
    bridge_query_call(&query);
    jstring result = jni_text(env, query.json);
    u_free(query.json);
    u_free((void*)ch);
    return result;
}

/* UTF-8 JSON приходит байтами, чтобы emoji не превращались в JNI modified UTF-8. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeDevgroupCommand(
    JNIEnv* env, jobject thiz, jint action, jstring group, jstring peer, jbyteArray text, jbyteArray pin, jlong request_id) {
    (void)thiz;
    if (!text || !pin || !instance_lite_is_running()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup JNI: core unavailable/invalid request"); return JNI_FALSE;
    }
    jsize n = (*env)->GetArrayLength(env, text), m = (*env)->GetArrayLength(env, pin);
    if (n >= 128 || m >= 5) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup JNI: input too long"); return JNI_FALSE; }
    struct chat_devgroup_request* req = u_calloc(1, sizeof(*req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup JNI: allocation failed"); return JNI_FALSE; }
    req->action = action; req->request_id = (uint64_t)request_id;
    if (action == CHAT_DEVGROUP_INVITE) {
        const char* key = group ? jni_utf8(env, group) : NULL;
        const char* hex = peer ? jni_utf8(env, peer) : NULL;
        int rc = !chat_channel_valid(key) || sc_hex_to_binary(hex, req->peer, 32);
        if (!rc) strcpy(req->group, key);
        u_free((void*)key); u_free((void*)hex);
        if (rc) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup JNI: invalid peer Ed25519"); u_free(req); return JNI_FALSE; }
    }
    (*env)->GetByteArrayRegion(env, text, 0, n, (jbyte*)req->text);
    (*env)->GetByteArrayRegion(env, pin, 0, m, (jbyte*)req->pin);
    if ((*env)->ExceptionCheck(env) || memchr(req->text, 0, n) || memchr(req->pin, 0, m)) {
        memset(req, 0, sizeof(*req)); u_free(req); DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup JNI: invalid text"); return JNI_FALSE;
    }
    return bridge_post_request(chat_devgroup_trampoline, req, &req->inst) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeClipboardSend(
    JNIEnv* env, jobject thiz, jlong request, jint type, jbyteArray data, jbyteArray group, jbyteArray peer) {
    (void)thiz;
    jsize size = data ? (*env)->GetArrayLength(env, data) : 0;
    if (!request || size <= 0 || ((group != NULL) != (peer != NULL)) || (group && (*env)->GetArrayLength(env, group) != 32) ||
        (peer && (*env)->GetArrayLength(env, peer) != 32) || (type != CHAT_CLIPBOARD_TEXT && type != CHAT_CLIPBOARD_PNG) ||
        (unsigned)size > (type == CHAT_CLIPBOARD_TEXT ? CHAT_CLIPBOARD_TEXT_MAX : CHAT_CLIPBOARD_IMAGE_MAX)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard JNI invalid send type=%d bytes=%d", type, size); return JNI_FALSE;
    }
    struct chat_clipboard_request* req = u_calloc(1, sizeof(*req) + size);
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard JNI allocation failed"); return JNI_FALSE; }
    req->request = (uint64_t)request; req->type = type; req->size = (uint32_t)size;
    if (group) (*env)->GetByteArrayRegion(env, group, 0, 32, (jbyte*)req->group);
    if (peer) (*env)->GetByteArrayRegion(env, peer, 0, 32, (jbyte*)req->peer_ed);
    if (!(*env)->ExceptionCheck(env)) (*env)->GetByteArrayRegion(env, data, 0, size, (jbyte*)(req + 1));
    if ((*env)->ExceptionCheck(env)) { u_free(req); DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard JNI copy failed"); return JNI_FALSE; }
    return bridge_post_request(chat_clipboard_trampoline, req, &req->inst) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeClipboardApplied(
    JNIEnv* env, jobject thiz, jbyteArray header, jboolean success) {
    (void)thiz;
    if (!header || (*env)->GetArrayLength(env, header) != CHAT_CLIPBOARD_EVENT_HEADER) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard JNI invalid apply header"); return JNI_FALSE;
    }
    uint8_t bytes[CHAT_CLIPBOARD_EVENT_HEADER]; (*env)->GetByteArrayRegion(env, header, 0, sizeof(bytes), (jbyte*)bytes);
    if ((*env)->ExceptionCheck(env)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard JNI header copy failed"); return JNI_FALSE; }
    struct chat_clipboard_request* req = u_calloc(1, sizeof(*req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard JNI apply allocation failed"); return JNI_FALSE; }
    req->action = 1; req->success = success; memcpy(req->id, bytes, 16);
    memcpy(req->group, bytes + CHAT_CLIPBOARD_EVENT_GROUP_OFF, 32);
    memcpy(req->peer_ed, bytes + CHAT_CLIPBOARD_EVENT_PEER_ED_OFF, 32);
    return bridge_post_request(chat_clipboard_trampoline, req, &req->inst) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeFeedbackCommand(
    JNIEnv* env, jobject thiz, jbyteArray json, jlong request_id) {
    (void)thiz;
    if (!json || !instance_lite_is_running()) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge: feedback core unavailable"); return JNI_FALSE;
    }
    jsize len = (*env)->GetArrayLength(env, json);
    if (len <= 0 || len > 16384) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge: feedback command size=%d", len); return JNI_FALSE;
    }
    struct chat_feedback_request* req = u_malloc(sizeof(*req) + len + 1);
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge: feedback allocation failed"); return JNI_FALSE; }
    req->request_id = (uint64_t)request_id;
    (*env)->GetByteArrayRegion(env, json, 0, len, (jbyte*)(req + 1));
    if ((*env)->ExceptionCheck(env)) { u_free(req); DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "bridge: feedback JNI copy failed"); return JNI_FALSE; }
    ((char*)(req + 1))[len] = 0; int posted = bridge_post_request(chat_feedback_command_trampoline, req, &req->inst); return posted == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_utun_chat_data_NativeLib_nativeDmStart(
    JNIEnv* env, jobject thiz, jstring peer, jstring sourceChId, jstring peerName, jboolean groups) {
    (void)thiz;
    const char* ch = sourceChId ? jni_utf8(env, sourceChId) : NULL;
    const char* nm = peerName ? jni_utf8(env, peerName) : NULL;
    const char* address = jni_utf8(env, peer);
    uint8_t ed[32]; uint64_t conversation = 0;
    if (address && !sc_hex_to_binary(address, ed, sizeof(ed)) && (!sourceChId || ch) && (!peerName || nm))
        conversation = utun_bridge_dm_start_wait(ed, ch, nm, groups != JNI_FALSE);
    else bridge_log(BLEV_WARN, "dm_start: invalid address or JNI string");
    u_free((void*)address);
    if (nm) u_free((void*)nm);
    if (ch) u_free((void*)ch);
    return (jlong)conversation;
}

/* Действие привязано к автору/seq и поколению; JNI подтверждает только очередь. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeDmMediaAction(
    JNIEnv* env, jobject thiz, jstring conv, jstring address, jlong seq, jlong generation, jboolean opened) {
    (void)thiz;
    const char* id = jni_utf8(env, conv);
    if (!id) { bridge_log(BLEV_ERROR, "DM attachment: missing conversation"); return JNI_FALSE; }
    const char* hex = jni_utf8(env, address);
    uint8_t author[32];
    int valid = !sc_hex_to_binary(hex, author, 32) && !mf_zero(author, 32);
    u_free((void*)hex);
    struct dm_media_action_req* req = u_calloc(1, sizeof(*req));
    if (!req || !*id || strlen(id) >= sizeof(req->conv) || !valid || seq <= 0 || generation <= 0) {
        bridge_log(BLEV_ERROR, "DM attachment: invalid action/allocation failed"); u_free((void*)id); u_free(req); return JNI_FALSE;
    }
    snprintf(req->conv, sizeof(req->conv), "%s", id); u_free((void*)id);
    memcpy(req->author, author, 32); req->seq = (uint64_t)seq; req->generation = (uint64_t)generation; req->opened = opened ? 1 : 0;
    return bridge_post_request(dm_media_action_trampoline, req, &req->inst) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeDmSend(
    JNIEnv* env, jobject thiz, jstring convId, jbyteArray text, jlong generation, jlong request) {
    (void)thiz;
    if (!convId || !text) { bridge_log(BLEV_ERROR, "dm_send JNI: missing target/text"); return JNI_FALSE; }
    jsize length = (*env)->GetArrayLength(env, text);
    if (length <= 0 || length > DM_DATA_MAX) {
        bridge_log(BLEV_ERROR, "dm_send JNI: invalid text bytes=%d", length); return JNI_FALSE;
    }
    uint8_t data[DM_DATA_MAX];
    (*env)->GetByteArrayRegion(env, text, 0, length, (jbyte*)data);
    if ((*env)->ExceptionCheck(env)) { bridge_log(BLEV_ERROR, "dm_send JNI: text copy failed"); return JNI_FALSE; }
    const char* c = jni_utf8(env, convId);
    if (!c) { bridge_log(BLEV_ERROR, "dm_send JNI: target copy failed"); return JNI_FALSE; }
    int rc = utun_bridge_dm_send(c, data, (size_t)length, (uint64_t)generation, (uint64_t)request);
    u_free((void*)c);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

/* UI получает подтверждение commit через CHAT_EVT_DM_DELETE_RESULT, а не через return JNI. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeDmDelete(
    JNIEnv* env, jobject thiz, jstring conversation, jlong generation, jlong request) {
    (void)thiz;
    if (!instance_lite_is_running() || generation <= 0 || request <= 0) {
        bridge_log(BLEV_WARN, "DM delete: stopped core or invalid generation/request"); return JNI_FALSE;
    }
    const char* conv = jni_utf8(env, conversation);
    struct dm_delete_req* req = u_calloc(1, sizeof(*req));
    if (!conv || !req || !conv[0] || strlen(conv) >= sizeof(req->conv_id) || strspn(conv, "0123456789") != strlen(conv)) {
        bridge_log(BLEV_ERROR, "DM delete: invalid conversation or allocation failed");
        if (conv) u_free((void*)conv);
        u_free(req); return JNI_FALSE;
    }
    req->generation = (uint64_t)generation; req->request_id = (uint64_t)request;
    snprintf(req->conv_id, sizeof(req->conv_id), "%s", conv);
    u_free((void*)conv);
    int posted = bridge_post_request(dm_delete_trampoline, req, &req->inst);
    return posted == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeDmInvite(
    JNIEnv* env, jobject thiz, jlong request_id, jstring peer, jstring source, jstring channel, jstring name, jboolean cancel) {
    (void)thiz;
    if (!instance_lite_is_running()) { bridge_log(BLEV_ERROR, "invite PM: core unavailable"); return JNI_FALSE; }
    struct dm_invite_req* req = u_calloc(1, sizeof(*req));
    if (!req) { bridge_log(BLEV_ERROR, "invite PM: request allocation failed"); return JNI_FALSE; }
    const char* address = cancel ? NULL : jni_utf8(env, peer);
    int valid = cancel || (address && !sc_hex_to_binary(address, req->peer_ed, sizeof(req->peer_ed)));
    u_free((void*)address);
    const char* src = valid ? jni_utf8(env, source) : NULL;
    const char* nm = src ? jni_utf8(env, name) : NULL;
    if (!src || !nm) {
        u_free((void*)src);
        bridge_log(BLEV_ERROR, "invite PM: invalid address or JNI string"); u_free(req); return JNI_FALSE;
    }
    req->request_id = (uint64_t)request_id;
    const char* key = !cancel && channel ? jni_utf8(env, channel) : NULL;
    int key_ok = cancel || chat_channel_valid(key);
    if (key_ok && key) strcpy(req->channel_id, key);
    u_free((void*)key); req->cancel = cancel ? 1 : 0;
    snprintf(req->source_ch_id, sizeof(req->source_ch_id), "%s", src);
    snprintf(req->peer_name, sizeof(req->peer_name), "%s", nm);
    u_free((void*)src); u_free((void*)nm);
    if (!key_ok) { DEBUG_WARN(DEBUG_CATEGORY_DM, "invite PM: invalid group key"); u_free(req); return JNI_FALSE; }
    int posted = bridge_post_request(dm_invite_trampoline, req, &req->inst);
    return posted == 0 ? JNI_TRUE : JNI_FALSE;
}

/* Чтение членства для выбора групп и открытия уже присоединённого канала. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeIsLocalChannelMember(
    JNIEnv* env, jobject thiz, jstring channel) {
    (void)thiz;
    const char* id = jni_utf8(env, channel);
    if (!id) { bridge_log(BLEV_ERROR, "channel membership: string conversion failed"); return JNI_FALSE; }
    struct bridge_query query = {.kind = QUERY_MEMBERSHIP, .channel = id};
    bridge_query_call(&query);
    int member = (int)query.number;
    u_free((void*)id);
    return member ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeDmRequestConversations(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    utun_bridge_dm_request_conversations();
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeDmRequestCallRoute(
    JNIEnv* env, jobject thiz, jstring convId) {
    (void)thiz;
    if (!instance_lite_is_running()) { bridge_log(BLEV_WARN, "PM call route: core stopped"); return; }
    const char* conv = jni_utf8(env, convId);
    if (!conv) return;
    struct dm_messages_req* req = u_calloc(1, sizeof(*req));
    if (req) {

        snprintf(req->conv_id, sizeof(req->conv_id), "%s", conv);
        bridge_post_request(dm_call_route_trampoline, req, &req->inst);
    } else bridge_log(BLEV_ERROR, "PM call route: allocation failed");
    u_free((void*)conv);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeDmRequestMessages(
    JNIEnv* env, jobject thiz, jstring convId, jlong generation) {
    (void)thiz;
    const char* c = jni_utf8(env, convId);
    utun_bridge_dm_request_messages(c, (uint64_t)generation);
    if (c) u_free((void*)c);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeDmSendFile(
    JNIEnv* env, jobject thiz, jstring convId, jstring path, jstring name, jlong generation, jlong requestId) {
    (void)thiz;
    if (!convId || !path || !name) { bridge_log(BLEV_ERROR, "DM file JNI: null argument"); return JNI_FALSE; }
    const char* c = jni_utf8(env, convId);
    const char* p = c ? jni_utf8(env, path) : NULL;
    const char* n = p ? jni_utf8(env, name) : NULL;
    int rc = n ? utun_bridge_dm_send_file(c, p, n, (uint64_t)generation, (uint64_t)requestId) : -1;
    if (!n) bridge_log(BLEV_ERROR, "DM file JNI: string copy failed");
    if (n) u_free((void*)n);
    if (p) u_free((void*)p);
    if (c) u_free((void*)c);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_NativeLib_nativeGetMySigningKey(JNIEnv* env, jobject thiz) {
    (void)thiz;
    struct bridge_query query = {.kind = QUERY_SIGNING_KEY};
    if (bridge_query_call(&query)) return NULL;
    jbyteArray result = (*env)->NewByteArray(env, (jsize)sizeof(query.own_ed));
    if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)sizeof(query.own_ed), (const jbyte*)query.own_ed);
    else bridge_log(BLEV_ERROR, "own signing key: Java snapshot allocation failed");
    return result;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRestart(
    JNIEnv* env, jobject thiz, jstring configText) {
    (void)thiz;
    if (!configText) return JNI_FALSE;
    const char* cfg = jni_utf8(env, configText);
    if (!cfg) return JNI_FALSE;
    int result = utun_bridge_restart(cfg);
    u_free((void*)cfg);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeUpdateSockets(
    JNIEnv* env, jobject thiz, jstring configText) {
    (void)thiz;
    if (!configText) return JNI_FALSE;
    const char* cfg = jni_utf8(env, configText);
    if (!cfg) return JNI_FALSE;
    int result = utun_bridge_update_sockets(cfg);
    u_free((void*)cfg);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeIsResponsive(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return utun_bridge_is_responsive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeIsRunning(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return instance_lite_is_running() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativePing(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    utun_bridge_ping();
}

static void cpu_load_run(void* unused) {
    (void)unused; chat_core_collect_cpu_load_trampoline(instance_lite_get_instance());
}
/* GUI запрашивает готовый снимок асинхронно, CPU-часы читает только поток ядра. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRequestCpuLoad(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return instance_lite_post(cpu_load_run, NULL, NULL, NULL) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeConnectChannel(
    JNIEnv* env, jobject thiz, jstring channelId) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    utun_bridge_connect_channel(ch);
    if (ch) u_free((void*)ch);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeConnectMemberNode(
    JNIEnv* env, jobject thiz, jstring channelId, jstring address) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    const char* key = jni_utf8(env, address);
    uint8_t ed[32];
    if (sc_hex_to_binary(key, ed, 32)) bridge_log(BLEV_WARN, "connect_member_node: invalid Ed25519");
    else utun_bridge_connect_member_node(ch, ed);
    u_free((void*)ch); u_free((void*)key);
}

JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_nativeGetDebugCategories(JNIEnv* env, jobject thiz) {
    (void)thiz;
    struct strbuf names = strbuf_new();
    for (int category = 1; category < DEBUG_CATEGORY_COUNT; category++) {
        const char* name = debug_get_category_name(category);
        if (strcmp(name, "UNKNOWN") == 0) continue;
        if (strbuf_addf(&names, "%s%s", names.len ? "," : "", name) < 0) {
            bridge_log(BLEV_ERROR, "Debug categories: allocation failed");
            strbuf_free(&names); return NULL;
        }
    }
    jstring result = jni_text(env, strbuf_str(&names));
    if (!result) bridge_log(BLEV_ERROR, "Debug categories: Java string allocation failed");
    strbuf_free(&names); return result;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeSetDebugLevel(
    JNIEnv* env, jobject thiz, jstring category, jstring level) {
    (void)thiz;
    const char* c = jni_utf8(env, category);
    const char* l = jni_utf8(env, level);
    utun_bridge_set_debug_level(c, l);
    if (l) u_free((void*)l);
    if (c) u_free((void*)c);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeSetChatSetting(
    JNIEnv* env, jobject thiz, jstring name, jstring value) {
    (void)thiz;
    const char* n = jni_utf8(env, name);
    const char* v = jni_utf8(env, value);
    if (n && v) {
        struct chat_setting_arg* req = u_calloc(1, sizeof(*req));
        size_t name_size = strlen(n) + 1, value_size = strlen(v) + 1;
        if (!req || name_size + value_size > sizeof(req->data)) {
            bridge_log(BLEV_ERROR, "Chat setting rejected: allocation/size name=%s", n); u_free(req);
        } else {
            memcpy(req->data, n, name_size); memcpy(req->data + name_size, v, value_size);
            bridge_post_request(chat_core_set_setting_trampoline, req, &req->inst);
        }
    }
    if (v) u_free((void*)v);
    if (n) u_free((void*)n);
}

 JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeGetChatSettingInt(
     JNIEnv* env, jobject thiz, jstring name, jint def) {
     (void)thiz;
     const char* n = jni_utf8(env, name);
     struct bridge_query query = {.kind = QUERY_SETTING, .channel = n, .number = def};
     bridge_query_call(&query);
     int val = (int)query.number;
     if (n) u_free((void*)n);
     return (jint)val;
 }

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeGetSoundOnMessage(
     JNIEnv* env, jobject thiz, jstring chId) {
     (void)thiz;
     const char* c = jni_utf8(env, chId);
     struct bridge_query query = {.kind = QUERY_SOUND, .channel = c, .number = 1};
     bridge_query_call(&query);
     int on = (int)query.number;
     if (c) u_free((void*)c);
     return (jint)on;
 }

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeSetSoundOnMessage(
     JNIEnv* env, jobject thiz, jstring chId, jboolean on) {
     (void)thiz;
     const char* c = jni_utf8(env, chId);
     utun_bridge_set_sound_on_message(c, on ? 1 : 0);
     if (c) u_free((void*)c);
 }

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeSubscribeLogs(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    (void)thiz; (void)env;
    pthread_mutex_lock(&g_log_callback_lock);
    int configured = g_log_callback != NULL;
    __atomic_store_n(&g_on_log, enabled && configured ? jni_on_log : NULL, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_log_callback_lock);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "JNI log forwarding enabled=%d callback=%d", enabled != JNI_FALSE, configured);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeSetUdpLogTarget(
    JNIEnv* env, jobject thiz, jstring ip, jint port) {
    (void)thiz;
    const char* ip_str = jni_utf8(env, ip);
    udp_log_set_target(ip_str, (int)port);
    u_free((void*)ip_str);
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeReplaceStoppedKeys(JNIEnv* env, jobject thiz, jstring path) {
    (void)thiz;
    char* directory = path ? jni_utf8(env, path) : NULL;
    int status = instance_lite_replace_stopped_keys(directory);
    u_free(directory);
    return status;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRegenerateKeys(
    JNIEnv* env, jobject thiz, jstring configText) {
    (void)thiz;
    if (!configText) { bridge_log(BLEV_ERROR, "key regeneration: config is NULL"); return JNI_FALSE; }
    const char* config = jni_utf8(env, configText);
    if (!config) { bridge_log(BLEV_ERROR, "key regeneration: config string unavailable"); return JNI_FALSE; }
    int result = instance_lite_regenerate_keys(config);
    u_free((void*)config);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_nativeClearDatabase(
    JNIEnv* env, jobject thiz, jstring directory) {
    (void)thiz;
    const char* path = jni_utf8(env, directory);
    if (!path) return jni_text(env, strerror(EINVAL));
    int result = utun_bridge_clear_database(path);
    u_free((void*)path);
    return result ? jni_text(env, strerror(-result)) : NULL;
}

/* ── Voice recording JNI ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceStart(
    JNIEnv* env, jobject thiz, jstring channelId, jbyteArray replyTo, jlong requestId, jlong generation, jbyteArray replyQuote) {
    (void)thiz;
    uint8_t reply[32];
    char quote[DB_SYNC_MESSAGE_MAX + 1];
    if (jni_message_id(env, replyTo, reply) < 0 || jni_reply_quote(env, replyQuote, quote) < 0) return JNI_FALSE;
    const char* ch = jni_utf8(env, channelId);
    int rc = utun_bridge_voice_start(ch, reply, (uint64_t)requestId, (uint64_t)generation, quote);
    if (ch) u_free((void*)ch);
    return (rc == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceFeed(
    JNIEnv* env, jobject thiz, jshortArray samples) {
    (void)env; (void)thiz;
    if (!samples) return JNI_FALSE;
    jsize len = (*env)->GetArrayLength(env, samples);
    jshort* data = (*env)->GetShortArrayElements(env, samples, NULL);
    if (!data) { bridge_log(BLEV_ERROR, "Voice capture: JNI PCM access failed"); return JNI_FALSE; }
    int rc = utun_bridge_voice_feed((const int16_t*)data, (int)len);
    (*env)->ReleaseShortArrayElements(env, samples, data, JNI_ABORT);
    return (rc == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceStop(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    int duration_ms = 0;
    return utun_bridge_voice_stop(&duration_ms) == 0 ? (jint)duration_ms : -1;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceCancel(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    utun_bridge_voice_cancel();
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceIsActive(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return utun_bridge_voice_is_active() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceSetPreset(
    JNIEnv* env, jobject thiz, jint preset) {
    (void)env; (void)thiz;
    utun_bridge_voice_set_preset((int)preset);
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceGetPreset(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return (jint)utun_bridge_voice_get_preset();
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceSetCompressor(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    (void)env; (void)thiz;
    utun_bridge_voice_set_compressor(enabled ? 1 : 0);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceGetCompressor(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return utun_bridge_voice_get_compressor() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceSetCompressorConfig(
    JNIEnv* env, jobject thiz, jint maxGainDb, jint lookbackMs, jint lookaheadMs,
    jfloat riseRate, jfloat targetLevelDb, jboolean gainOnlyOnSpeech, jint vadMaxGainDb) {
    (void)env; (void)thiz;
    if (audio_compressor_set_vad_max_gain((int)vadMaxGainDb)) return;
    if (audio_compressor_set_shared_settings((int)targetLevelDb, gainOnlyOnSpeech ? 1 : 0)) return;
    utun_bridge_voice_set_compressor_config((int)maxGainDb, (int)lookbackMs, (int)lookaheadMs,
                                             (float)riseRate, (float)targetLevelDb);
}

JNIEXPORT jfloat JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceGetPeakLevel(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return (jfloat)utun_bridge_voice_get_peak_level();
}

/* ── Attachment JNI ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeAttachmentSend(
    JNIEnv* env, jobject thiz, jstring channelId, jstring filePath, jbyteArray replyTo, jlong requestId, jbyteArray replyQuote) {
    (void)thiz;
    uint8_t reply[32];
    char quote[DB_SYNC_MESSAGE_MAX + 1];
    if (jni_message_id(env, replyTo, reply) < 0 || jni_reply_quote(env, replyQuote, quote) < 0) return JNI_FALSE;
    const char* ch = jni_utf8(env, channelId);
    const char* fp = jni_utf8(env, filePath);
    int rc = utun_bridge_attachment_send(ch, fp, g_db_path, reply, (uint64_t)requestId, quote);
    if (fp) u_free((void*)fp);
    if (ch) u_free((void*)ch);
    return (rc == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeAttachmentDownload(
    JNIEnv* env, jobject thiz, jstring channelId, jlong msgId) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    int rc = utun_bridge_attachment_download(ch, (int64_t)msgId);
    if (ch) u_free((void*)ch);
    return (rc == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeVideoSend(
    JNIEnv* env, jobject thiz, jstring channelId, jstring filePath,
    jint durationMs, jint width, jint height, jbyteArray replyTo, jlong requestId, jlong generation, jbyteArray replyQuote) {
    (void)thiz;
    uint8_t reply[32];
    char quote[DB_SYNC_MESSAGE_MAX + 1];
    if (jni_message_id(env, replyTo, reply) < 0 || jni_reply_quote(env, replyQuote, quote) < 0) return JNI_FALSE;
    const char* ch = jni_utf8(env, channelId);
    const char* fp = jni_utf8(env, filePath);
    int rc = utun_bridge_video_send(ch, fp, g_db_path, (int)durationMs, (int)width, (int)height, reply,
        (uint64_t)requestId, (uint64_t)generation, quote);
    if (fp) u_free((void*)fp);
    if (ch) u_free((void*)ch);
    return (rc == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativePhotoSend(
    JNIEnv* env, jobject thiz, jstring channelId, jstring filePath,
    jint width, jint height, jbyteArray replyTo, jlong requestId, jstring contentType, jlong generation, jbyteArray replyQuote) {
    (void)thiz;
    uint8_t reply[32];
    char quote[DB_SYNC_MESSAGE_MAX + 1];
    if (jni_message_id(env, replyTo, reply) < 0 || jni_reply_quote(env, replyQuote, quote) < 0) return JNI_FALSE;
    const char* ch = jni_utf8(env, channelId);
    const char* fp = jni_utf8(env, filePath);
    const char* type = jni_utf8(env, contentType);
    int rc = utun_bridge_photo_send(ch, fp, g_db_path, (int)width, (int)height, reply, (uint64_t)requestId, type,
        (uint64_t)generation, quote);
    if (type) u_free((void*)type);
    if (fp) u_free((void*)fp);
    if (ch) u_free((void*)ch);
    return (rc == 0) ? JNI_TRUE : JNI_FALSE;
}

/* ── Voice playback JNI ── */

JNIEXPORT jlong JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceDecodeOpen(
    JNIEnv* env, jobject thiz, jstring filePath, jintArray outInfo) {
    (void)thiz;
    if (!filePath || !outInfo || (*env)->GetArrayLength(env, outInfo) < 3) {
        bridge_log(BLEV_ERROR, "Voice decode: expected path and IntArray(3)"); return 0;
    }
    const char* fp = jni_utf8(env, filePath);
    int sr = 0, ch = 0, total = 0;
    void* h = utun_bridge_voice_decode_open(fp, &sr, &ch, &total);
    if (h) {
        jint info[3] = {sr, ch, total};
        (*env)->SetIntArrayRegion(env, outInfo, 0, 3, info);
        if ((*env)->ExceptionCheck(env)) {
            bridge_log(BLEV_ERROR, "Voice decode: output metadata JNI copy failed"); utun_bridge_voice_decode_close(h); h = NULL;
        }
    }
    if (fp) u_free((void*)fp);
    return (jlong)(intptr_t)h;
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceDecodeRead(
    JNIEnv* env, jobject thiz, jlong handle, jshortArray buf) {
    (void)thiz;
    if (handle == 0 || !buf) return -1;
    jsize max = (*env)->GetArrayLength(env, buf);
    jshort* data = (*env)->GetShortArrayElements(env, buf, NULL);
    if (!data) { bridge_log(BLEV_ERROR, "Voice decode: JNI PCM buffer access failed"); return -1; }
    int read = utun_bridge_voice_decode_read((void*)(intptr_t)handle, (int16_t*)data, (int)max);
    (*env)->ReleaseShortArrayElements(env, buf, data, 0);
    return (jint)read;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeVoiceDecodeClose(
    JNIEnv* env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    utun_bridge_voice_decode_close((void*)(intptr_t)handle);
}

/* ── Channel members JNI ── */

/* Живой снимок группы читается в uasync; результат приходит как MEMBER_LIST. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRequestMemberList(
    JNIEnv* env, jobject thiz, jstring channelId) {
    (void)thiz;
    if (!instance_lite_is_running() || !channelId) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "request_member_list: missing instance/uasync/channel"); return JNI_FALSE;
    }
    const char* ch = jni_utf8(env, channelId);
    if (!ch) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "request_member_list: cannot read channel"); return JNI_FALSE; }
    struct chat_member_list_req* req = u_calloc(1, sizeof(*req));
    if (!req || !ch[0] || strlen(ch) >= sizeof(req->ch_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "request_member_list: allocation/invalid channel ch=%s", ch);
        u_free(req); u_free((void*)ch); return JNI_FALSE;
    }
    memcpy(req->ch_id, ch, strlen(ch) + 1);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "request_member_list: queued ch=%s", ch);
    u_free((void*)ch);
    int posted = bridge_post_request(chat_core_request_member_list_trampoline, req, &req->inst);
    return posted == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_nativeGetChannelMembers(
    JNIEnv* env, jobject thiz, jstring channelId) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    struct bridge_query query = {.kind = QUERY_MEMBERS, .channel = ch};
    bridge_query_call(&query);
    char* json = query.json;
    jstring result = jni_text(env, json);
    u_free(json);
    u_free((void*)ch);
    return result;
}

JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_NativeLib_nativeDmMembers(
    JNIEnv* env, jobject thiz, jstring convId, jlong generation) {
    (void)thiz;
    const char* conv = jni_utf8(env, convId);
    struct bridge_query query = {.kind = QUERY_DM_MEMBERS, .channel = conv, .node = (uint64_t)generation};
    jbyteArray result = NULL;
    if (conv && generation > 0 && !bridge_query_call(&query) && query.bytes) {
        result = (*env)->NewByteArray(env, (jsize)query.size);
        if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)query.size, (const jbyte*)query.bytes);
        else bridge_log(BLEV_ERROR, "DM members: Java snapshot allocation failed");
    } else bridge_log(BLEV_WARN, "DM members: snapshot unavailable");
    u_free(query.bytes); u_free((void*)conv); return result;
}

JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_nativeGetMemberDetail(
    JNIEnv* env, jobject thiz, jstring channelId, jstring peer) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    const char* hex = jni_utf8(env, peer);
    uint8_t ed[32];
    struct bridge_query query = {.kind = QUERY_DETAIL, .channel = ch, .ed = ed};
    if (ch && hex && !sc_hex_to_binary(hex, ed, sizeof(ed))) bridge_query_call(&query);
    else bridge_log(BLEV_WARN, "Member detail: invalid channel or Ed25519");
    u_free((void*)hex);
    char* json = query.json;
    jstring result = jni_text(env, json);
    u_free(json);
    u_free((void*)ch);
    return result;
}

static int bridge_editor_key(JNIEnv* env, jstring key, uint8_t ed[32]);

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeToggleMemberRadioMute(
    JNIEnv* env, jobject thiz, jstring channelId, jstring key) {
    (void)thiz;
    uint8_t ed[32];
    if (!bridge_editor_key(env, key, ed)) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: invalid mute member key"); return JNI_FALSE; }
    if (!channelId) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: null Android mute channel"); return JNI_FALSE; }
    const char* ch = jni_utf8(env, channelId);
    if (!ch) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: Android mute channel conversion failed"); return JNI_FALSE; }
    int rc = utun_bridge_toggle_member_radio_mute(ch, ed);
    u_free((void*)ch);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

/* Оба редактора получают Ed25519 из той же локальной карточки, которую открыл пользователь. */
static int bridge_editor_key(JNIEnv* env, jstring key, uint8_t ed[32]) {
    const char* hex = key ? jni_utf8(env, key) : NULL;
    int valid = hex && strspn(hex, "0123456789abcdefABCDEF") == 64 && !sc_hex_to_binary(hex, ed, 32);
    u_free((void*)hex);
    if (!valid) bridge_log(BLEV_WARN, "member edit: missing or malformed Ed25519");
    return valid;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeSetMemberNick(
    JNIEnv* env, jobject thiz, jstring channelId, jstring key, jstring nick) {
    (void)thiz; uint8_t ed[32]; if (!bridge_editor_key(env, key, ed)) return;
    const char* ch = jni_utf8(env, channelId);
    const char* nk = nick ? jni_utf8(env, nick) : NULL;
    if (ch && (!nick || nk)) {
        struct bridge_query query = {.kind = QUERY_SET_NICK, .channel = ch, .ed = ed, .value = nk};
        bridge_query_call(&query);
    }
    if (nk) u_free((void*)nk);
    u_free((void*)ch);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeIsChannelOwner(
    JNIEnv* env, jobject thiz, jstring channelId) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    struct bridge_query query = {.kind = QUERY_OWNER, .channel = ch};
    bridge_query_call(&query);
    int owner = (int)query.number;
    u_free((void*)ch);
    return (jboolean)owner;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeTransferAdmin(
    JNIEnv* env, jobject thiz, jstring channelId, jstring publicKey) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    const char* ed = jni_utf8(env, publicKey);
    utun_bridge_transfer_admin(ch, ed);
    u_free((void*)ed);
    u_free((void*)ch);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeDeleteChannel(
    JNIEnv* env, jobject thiz, jstring channelId) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    utun_bridge_delete_channel(ch);
    u_free((void*)ch);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeSetMemberFlags(
    JNIEnv* env, jobject thiz, jstring channelId, jstring key,
    jboolean admin, jboolean supernode, jboolean moder, jboolean storage,
    jboolean verified, jboolean deleted) {
    (void)thiz;
    const char* ch = jni_utf8(env, channelId);
    uint8_t ed[32];
    if (bridge_editor_key(env, key, ed))
        utun_bridge_set_member_flags(ch, ed, admin, supernode, moder, storage, verified, deleted);
    u_free((void*)ch);
}

JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_nativeGetMemberLinks(
    JNIEnv* env, jobject thiz, jstring peer) {
    (void)thiz;
    uint8_t ed[32];
    if (!bridge_editor_key(env, peer, ed)) return NULL;
    struct bridge_query query = {.kind = QUERY_LINKS, .ed = ed};
    if (bridge_query_call(&query)) return NULL;
    char* json = query.json;
    jstring result = jni_text(env, json);
    u_free(json);
    return result;
}

/* Синхронный снимок из worker; Kotlin вызывает на Dispatchers.IO, UTF-8 не теряется в JNI. */
JNIEXPORT jstring JNICALL Java_com_utun_chat_data_NativeLib_nativeGetBluetoothPeers(JNIEnv* env, jobject thiz) {
    (void)thiz;
    struct bridge_query query = {.kind = QUERY_BLUETOOTH_PEERS};
    if (bridge_query_call(&query) || !query.json) return NULL;
    jstring result = jni_text(env, query.json);
    u_free(query.json);
    if (!result) DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE devices snapshot: Java string allocation failed");
    return result;
}

/* ── Local sockets JSON ── */

static void local_sockets_trampoline(void* arg) {
    (void)arg;
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst) { bridge_log(BLEV_WARN, "local sockets snapshot: core stopped"); return; }
    char* json = utun_bridge_get_local_sockets_json();
    if (!json) { bridge_log(BLEV_ERROR, "local sockets snapshot allocation failed"); return; }
    chat_event_post(inst, CHAT_EVT_LOCAL_SOCKETS, (const uint8_t*)json, (int)strlen(json));
    u_free(json);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeRequestLocalSockets(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (instance_lite_post(local_sockets_trampoline, NULL, NULL, NULL))
        bridge_log(BLEV_DEBUG, "local sockets request: core not ready");
}

/* ── Network change notification ── */

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeOnNetworkChanged(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    utun_bridge_on_network_change();
}

/* ── App foreground/background (Android lifecycle) → client_activity ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeSetActive(
    JNIEnv* env, jobject thiz, jboolean active) {
    (void)env; (void)thiz;
    return utun_bridge_set_active(active ? 1 : 0) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeNotifyNetworkActive(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    utun_bridge_notify_network_active();
}

/* ── Connection monitor ── */

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeCollectConnList(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    utun_bridge_collect_conn_list();
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeCollectConnMetrics(
    JNIEnv* env, jobject thiz, jlong peerNodeId) {
    (void)env; (void)thiz;
    utun_bridge_collect_conn_metrics((uint64_t)peerNodeId);
}

/* ── P2P аудио-звонок (сигналинг) ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCallStart(
    JNIEnv* env, jobject thiz, jstring channelId, jstring peerAddress, jlong callId, jboolean devices) {
    (void)thiz;
    uint8_t peer[32] = {0};
    if (!channelId || (peerAddress && !bridge_editor_key(env, peerAddress, peer))) {
        bridge_log(BLEV_ERROR, "call_start JNI: missing channel or invalid destination");
        return JNI_FALSE;
    }
    const char* ch = jni_utf8(env, channelId);
    if (!ch) { bridge_log(BLEV_ERROR, "call_start JNI: channel copy failed"); return JNI_FALSE; }
    int rc = bridge_call_start(ch, peer, (uint64_t)callId, devices);
    u_free((void*)ch);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAccept(
    JNIEnv* env, jobject thiz, jlong callId) {
    (void)env; (void)thiz;
    return utun_bridge_call_accept((uint64_t)callId) == 0 ? JNI_TRUE : JNI_FALSE;
}

/* Команда адресного вызова рации: копируем запрос до передачи в поток ядра. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioCall(
    JNIEnv* env, jobject thiz, jlong id, jstring group, jstring address, jint action, jboolean devices) {
    (void)thiz;
    uint8_t peer[32] = {0}, key[32] = {0};
    if (!instance_lite_is_running() || !id || action < RADIO_CALL_START || action > RADIO_CALL_ANSWER ||
        (action == RADIO_CALL_START ? !bridge_editor_key(env, group, key) || mf_zero(key, 32) || !bridge_editor_key(env, address, peer) || mf_zero(peer, 32) : group || address)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call JNI rejected id=%lld action=%d", (long long)id, action);
        return JNI_FALSE;
    }
    struct radio_call_request* req = u_calloc(1, sizeof(*req));
    if (!req) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call JNI allocation failed"); u_free(req); return JNI_FALSE;
    }
    req->id = (uint64_t)id; memcpy(req->group, key, 32); req->action = action; req->devices = devices;
    memcpy(req->peer, peer, sizeof(req->peer));
    int posted = bridge_post_request(radio_call_trampoline, req, &req->inst);
    return posted == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCallDecline(
    JNIEnv* env, jobject thiz, jlong callId) {
    (void)env; (void)thiz;
    return utun_bridge_call_decline((uint64_t)callId) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCallHangup(
    JNIEnv* env, jobject thiz, jlong callId) {
    (void)env; (void)thiz;
    return utun_bridge_call_hangup((uint64_t)callId) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCallContinue(
    JNIEnv* env, jobject thiz, jlong callId) {
    (void)env; (void)thiz;
    return bridge_call_id_action((uint64_t)callId, "continue", call_continue_trampoline) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeSetPhoneState(
    JNIEnv* env, jobject thiz, jint state, jint policy) {
    (void)env; (void)thiz;
    return utun_bridge_set_phone_state(state, (unsigned)policy) == 0 ? JNI_TRUE : JNI_FALSE;
}

/* ── P2P аудио-звонок (медиа) ── */

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioStart(
    JNIEnv* env, jobject thiz, jlong callId) {
    (void)env; (void)thiz;
    return (utun_bridge_call_audio_start((uint64_t)callId) == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioFeed(
    JNIEnv* env, jobject thiz, jlong callId, jshortArray samples, jboolean muted, jlong capturePtsUs) {
    (void)thiz;
    if (!samples) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call capture: null PCM");
        return JNI_FALSE;
    }
    jsize len = (*env)->GetArrayLength(env, samples);
    jshort* data = (*env)->GetShortArrayElements(env, samples, NULL);
    if (!data) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call capture: JNI PCM access failed");
        return JNI_FALSE;
    }
    int rc = utun_bridge_call_audio_feed((uint64_t)callId, (const int16_t*)data, (int)len, muted != JNI_FALSE, capturePtsUs);
    (*env)->ReleaseShortArrayElements(env, samples, data, JNI_ABORT);
    return (rc == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioStop(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    utun_bridge_call_audio_stop();
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioProgress(
    JNIEnv* env, jobject thiz, jlong callId, jlong epoch, jlong position, jlong atUs, jlong nowUs, jboolean measured) {
    (void)env; (void)thiz;
    call_audio_playback_progress(callId, epoch, position, atUs, nowUs, measured);
}

/* Предвыделенный снимок: epoch, source_epoch, played, written, pts, valid, measured, held_back_us. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioClock(
    JNIEnv* env, jobject thiz, jlong callId, jlong nowUs, jlongArray out) {
    (void)thiz;
    if (!out || (*env)->GetArrayLength(env, out) != 8) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call clock: expected LongArray(8)");
        return JNI_FALSE;
    }
    struct call_clock_snapshot s;
    if (call_audio_get_clock(callId, nowUs, &s) != 0) return JNI_FALSE;
    jlong values[8] = {s.epoch, s.source_epoch, s.played_samples, s.written_samples,
                      s.pts_us, s.valid, s.measured, s.held_back_us};
    (*env)->SetLongArrayRegion(env, out, 0, 8, values);
    if ((*env)->ExceptionCheck(env)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call clock: JNI snapshot copy failed");
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

/* Диагностика включается тем же фильтром call=debug, что и нативные сводки. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioDiagnostics(
    JNIEnv* env, jobject thiz, jlong callId, jlongArray out) {
    (void)thiz;
    if (!debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CALL)) return JNI_FALSE;
    if (!out || (*env)->GetArrayLength(env, out) != 8) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call diagnostics: expected LongArray(8)");
        return JNI_FALSE;
    }
    struct call_jitter_diagnostics d;
    if (call_audio_get_diagnostics(callId, &d) != 0) return JNI_FALSE;
    jlong values[8] = {d.encoded_ms, d.processing_ms, d.reserve_ms, d.arrival_spread_ms,
                      d.pcm_samples, d.tempo_x100, d.underruns, d.waiting};
    (*env)->SetLongArrayRegion(env, out, 0, 8, values);
    if ((*env)->ExceptionCheck(env)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call diagnostics: JNI snapshot copy failed");
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

/* Снимок графиков: header[version,now,rx_count,tx_count,observed_us,missing_us],
 * 61 доля гистограммы в ppm, RX по 10 полей, TX по 8 полей. Только копии; очереди читает uasync. */
JNIEXPORT jlongArray JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioHistory(
    JNIEnv* env, jobject thiz, jlong callId) {
    (void)thiz;
    struct call_audio_history snapshot;
    if (call_audio_get_history((uint64_t)callId, &snapshot) != 0) return NULL;
    jlong values[6 + CALL_HISTORY_BINS + CALL_HISTORY_POINTS * 18];
    unsigned offset = 0;
    values[offset++] = 2; values[offset++] = snapshot.now_ms;
    values[offset++] = snapshot.rx_count; values[offset++] = snapshot.tx_count;
    values[offset++] = (jlong)(snapshot.observed_ms * 1000 + .5);
    values[offset++] = (jlong)(snapshot.missing_ms * 1000 + .5);
    for (unsigned i = 0; i < CALL_HISTORY_BINS; ++i) values[offset++] = (jlong)(snapshot.histogram[i] * 1000000 + .5);
    for (unsigned i = 0; i < snapshot.rx_count; ++i) {
        const struct call_history_rx* p = &snapshot.rx[i];
        values[offset++] = p->at_ms; values[offset++] = p->depth_ms; values[offset++] = p->reserve_ms;
        values[offset++] = p->tempo_x100; values[offset++] = p->low_ms; values[offset++] = p->high_ms;
        values[offset++] = p->rendered_samples; values[offset++] = p->missing_samples;
        values[offset++] = p->headroom_low_ms; values[offset++] = p->headroom_high_ms;
    }
    for (unsigned i = 0; i < snapshot.tx_count; ++i) {
        const struct call_history_tx* p = &snapshot.tx[i];
        values[offset++] = p->at_ms; values[offset++] = p->queued_bytes; values[offset++] = p->inflight_bytes;
        values[offset++] = p->transport_bytes; values[offset++] = (jlong)p->wire_bytes_per_sec;
        values[offset++] = p->peer_id; values[offset++] = p->tx_epoch; values[offset++] = p->wire_tx_bytes;
    }
    jlongArray result = (*env)->NewLongArray(env, (jsize)offset);
    if (!result) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call history: JNI array allocation failed count=%u", offset); return NULL; }
    (*env)->SetLongArrayRegion(env, result, 0, (jsize)offset, values);
    if ((*env)->ExceptionCheck(env)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call history: JNI copy failed"); return NULL; }
    return result;
}

/* ── nativeCallAudioPull(callId, out) → возвращает число сэмплов ──
 * out — предвыделенный ShortArray (960), заполняется из аудио-потока Kotlin. */
JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioPull(
    JNIEnv* env, jobject thiz, jlong callId, jshortArray out) {
    (void)thiz;
    if (!out) return 0;
    jsize len = (*env)->GetArrayLength(env, out);
    jshort* data = (*env)->GetShortArrayElements(env, out, NULL);
    if (!data) return 0;
    int n = utun_bridge_call_audio_pull((uint64_t)callId, (int16_t*)data, (int)len);
    (*env)->ReleaseShortArrayElements(env, out, data, 0);
    return (jint)n;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioSetAec(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    (void)env; (void)thiz;
    utun_bridge_call_audio_set_aec(enabled ? 1 : 0);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioSetAecDelay(
    JNIEnv* env, jobject thiz, jint frames) {
    (void)env; (void)thiz;
    utun_bridge_call_audio_set_aec_delay((int)frames);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeSetNoiseSuppression(
    JNIEnv* env, jobject thiz, jint suppressDb) {
    (void)env; (void)thiz;
    return utun_bridge_set_noise_suppression((int)suppressDb) ? JNI_FALSE : JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeCallAudioResetCapture(
    JNIEnv* env, jobject thiz, jlong callId) {
    (void)env; (void)thiz;
    call_audio_reset_io((uint64_t)callId, 0);
}

/* ── Рация (PTT walkie-talkie) ── */

struct android_radio_subscription {
    struct UTUN_INSTANCE* inst;
    char group[65];
    int on;
};
static void android_radio_set_active(void* arg) {
    struct android_radio_subscription* request = arg;
    uint64_t route = topo_groups_channel_slot(request->inst->topo_groups, request->group);
    if (route) radio_set_active(request->inst, route, request->on);
    else DEBUG_WARN(DEBUG_CATEGORY_RADIO, "Radio subscription group unavailable group=%s", request->group);
    u_free(request);
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioSetActive(
    JNIEnv* env, jobject thiz, jstring group, jboolean on) {
    (void)thiz;
    const char* channel = group ? jni_utf8(env, group) : NULL;
    if (!chat_channel_valid(channel)) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "Radio subscription rejected invalid group"); u_free((void*)channel); return; }
    struct android_radio_subscription* request = u_calloc(1, sizeof(*request));
    if (request) { strcpy(request->group, channel); request->on = on; }
    u_free((void*)channel);
    if (request) bridge_post_request(android_radio_set_active, request, &request->inst);
    else DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Radio subscription allocation failed");
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioSubscribers(
    JNIEnv* env, jobject thiz, jstring group) {
    (void)thiz;
    const char* channel = group ? jni_utf8(env, group) : NULL;
    struct bridge_query query = {.kind = QUERY_RADIO_SUBSCRIBERS, .channel = channel};
    if (chat_channel_valid(channel)) bridge_query_call(&query);
    else DEBUG_WARN(DEBUG_CATEGORY_RADIO, "Radio subscribers rejected invalid group");
    u_free((void*)channel); return (jint)query.number;
}

JNIEXPORT jlong JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioAudioStart(
    JNIEnv* env, jobject thiz, jstring group, jint channels) {
    (void)thiz;
    const char* channel = group ? jni_utf8(env, group) : NULL;
    struct bridge_query query = {.kind = QUERY_RADIO_START, .channel = channel, .limit = channels};
    if (chat_channel_valid(channel)) bridge_query_call(&query);
    else DEBUG_WARN(DEBUG_CATEGORY_RADIO, "Radio open rejected invalid group");
    u_free((void*)channel); return (jlong)query.number;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioCaptureStart(
    JNIEnv* env, jobject thiz, jlong groupId, jint channels, jint mode) {
    (void)env; (void)thiz;
    return utun_bridge_radio_capture_start((uint64_t)groupId, (int)channels, (int)mode) == 0 ? JNI_TRUE : JNI_FALSE;
}

static void radio_config_applied(int64_t request_id, int success) {
    JNIEnv* env = NULL;
    int attached = 0;
    if (!g_jvm || !__atomic_load_n(&g_radio_config_available, __ATOMIC_ACQUIRE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config callback unavailable request=%lld", (long long)request_id);
        return;
    }
    jint rc = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config: attach JVM failed");
            return;
        }
        attached = 1;
    } else if (rc != JNI_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config: GetEnv failed rc=%d", rc);
        return;
    }
    (*env)->CallVoidMethod(env, g_config_callback, g_radio_config_mid, (jlong)request_id, success ? JNI_TRUE : JNI_FALSE);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env);
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config callback failed request=%lld", (long long)request_id);
    }
    if (attached) (*g_jvm)->DetachCurrentThread(g_jvm);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioConfigure(
    JNIEnv* env, jobject thiz, jlong request_id, jintArray settings) {
    (void)thiz;
    if (!g_jvm || !__atomic_load_n(&g_radio_config_available, __ATOMIC_ACQUIRE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config: acknowledgement callback unavailable request=%lld", (long long)request_id);
        return JNI_FALSE;
    }
    if (!settings || (*env)->GetArrayLength(env, settings) != INSTANCE_LITE_RADIO_SETTINGS_COUNT) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config: invalid snapshot request=%lld", (long long)request_id);
        return JNI_FALSE;
    }
    jint values[INSTANCE_LITE_RADIO_SETTINGS_COUNT];
    (*env)->GetIntArrayRegion(env, settings, 0, INSTANCE_LITE_RADIO_SETTINGS_COUNT, values);
    if ((*env)->ExceptionCheck(env)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config: cannot copy snapshot"); return JNI_FALSE;
    }
    return instance_lite_configure_radio(request_id, values, radio_config_applied) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloatArray JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioInputStatus(JNIEnv* env, jobject thiz) {
    (void)thiz;
    struct radio_audio_input_status status = radio_audio_get_input_status();
    jfloat values[] = {status.level_dbfs, (float)status.valid, (float)status.blocked, status.vad_probability, (float)status.vad_valid,
                      status.vad_gain_db, (float)status.vad_compressor_enabled};
    jfloatArray result = (*env)->NewFloatArray(env, 7);
    if (!result) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio input status allocation failed"); return NULL; }
    (*env)->SetFloatArrayRegion(env, result, 0, 7, values);
    return result;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioCaptureStop(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    radio_audio_capture_stop();
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioCaptureDiscontinuity(
    JNIEnv* env, jobject thiz, jlong groupId) {
    (void)env; (void)thiz;
    radio_audio_capture_discontinuity((uint64_t)groupId);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioAudioFeed(
    JNIEnv* env, jobject thiz, jlong groupId, jshortArray samples) {
    (void)thiz;
    if (!samples) return JNI_FALSE;
    jsize len = (*env)->GetArrayLength(env, samples);
    jshort* data = (*env)->GetShortArrayElements(env, samples, NULL);
    if (!data) { bridge_log(BLEV_ERROR, "Radio capture: JNI PCM access failed"); return JNI_FALSE; }
    int rc = utun_bridge_radio_audio_feed((uint64_t)groupId, (const int16_t*)data, (int)len);
    (*env)->ReleaseShortArrayElements(env, samples, data, JNI_ABORT);
    return (rc == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioAudioStop(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    utun_bridge_radio_audio_stop();
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioTransmitting(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return radio_audio_transmitting() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioAutoMode(
    JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return (jint)utun_bridge_radio_auto_mode();
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioAudioPull(
    JNIEnv* env, jobject thiz, jlong groupId, jshortArray out, jlong playedFrames) {
    (void)thiz;
    if (!out) return 0;
    jsize len = (*env)->GetArrayLength(env, out);
    jshort* data = (*env)->GetShortArrayElements(env, out, NULL);
    if (!data) return 0;
    radio_audio_playback_progress((uint64_t)groupId, (uint64_t)playedFrames, 0);
    int n = utun_bridge_radio_audio_pull((uint64_t)groupId, (int16_t*)data, (int)len);
    (*env)->ReleaseShortArrayElements(env, out, data, 0);
    return (jint)n;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioTalkBegin(
    JNIEnv* env, jobject thiz, jlong groupId) {
    (void)env; (void)thiz;
    utun_bridge_radio_talk_begin((uint64_t)groupId);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeRadioTalkEnd(
    JNIEnv* env, jobject thiz, jlong groupId) {
    (void)env; (void)thiz;
    utun_bridge_radio_talk_end((uint64_t)groupId);
}

/* Снимок читает DB worker; callback в uasync формирует локальное JNI-событие CHAT_EVT_GROUP_AVATAR_SNAPSHOT.
 * id:8LE, group:Ed25519, version:8LE, hash:32, can_edit/failed:1, size:2LE, data:size. */
static void android_avatar_snapshot_done(void* arg, const struct group_avatar_result* result) {
    struct group_avatar_gui_request* req = arg;
    const struct group_avatar_record* record = &result->record;
    int failed = result->status != 0, can_edit = !failed && result->can_edit;
    char channel[65];
    sc_public_key_hex(req->group, channel);
    if (failed) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "Android avatar: snapshot failed group=%s id=%llu status=%d rc=%d", channel,
        (unsigned long long)req->id, result->status, result->sqlite_code);
    if (req->inst->destroying || result->status == DB_ASYNC_CANCELLED) { u_free(req); return; }
    size_t size = failed ? 0 : record->size;
    uint8_t* event = u_calloc(1, 84 + size);
    if (!event) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "Android avatar: snapshot allocation failed id=%llu", (unsigned long long)req->id);
        u_free(req); return;
    }
    mf_put64(event, req->id); memcpy(event + 8, req->group, 32); mf_put64(event + 40, failed ? 0 : record->version);
    if (!failed) memcpy(event + 48, record->hash, 32); event[80] = can_edit; event[81] = failed;
    event[82] = size & 255; event[83] = size >> 8;
    if (size) memcpy(event + 84, record->data, size);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "Android avatar: snapshot group=%s id=%llu version=%llu size=%zu edit=%d failed=%d",
                channel, (unsigned long long)req->id, (unsigned long long)record->version, size, can_edit, failed);
    jni_on_event(CHAT_EVT_GROUP_AVATAR_SNAPSHOT, event, (int)(84 + size));
    u_free(event); u_free(req);
}

static void android_avatar_snapshot(void* arg) {
    struct group_avatar_gui_request* req = arg;
    struct TOPO_GROUP* group = topo_groups_find_wire(req->inst->topo_groups, req->group);
    group_avatar_submit(req->inst, group && group->group_type == TOPO_GROUP_TYPE_CHAT ? group->group_id : 0,
        android_avatar_snapshot_done, req);
}

/* Имя NULL запрашивает снимок; token защищает редактирование от конкурентной смены описания. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeGroupInfo(
    JNIEnv* env, jobject thiz, jlong id, jstring group, jstring name, jstring token, jint clipboard_members) {
    (void)thiz;
    const char* key = group ? jni_utf8(env, group) : NULL;
    const char* n = name ? jni_utf8(env, name) : NULL;
    const char* t = token ? jni_utf8(env, token) : NULL;
    if (!id || !chat_channel_valid(key) || (name && (!n || !*n || strlen(n) > 127)) || (token && (!t || strlen(t) != 64)) || clipboard_members < 0 || clipboard_members > 2) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "Android group description invalid request id=%lld group=%s", (long long)id, key ? key : "-");
        u_free((void*)key); u_free((void*)n); u_free((void*)t); return JNI_FALSE;
    }
    struct group_info_request* q = u_calloc(1, sizeof(*q));
    if (q) {
        q->id = id; strcpy(q->group, key);
        q->clipboard_members = clipboard_members;
        if (n) memcpy(q->name, n, strlen(n) + 1);
        if (t) memcpy(q->expected, t, 65);
    } else DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "Android group description request allocation failed");
    u_free((void*)key); u_free((void*)n); u_free((void*)t);
    return q && !bridge_post_request(group_info_gui_command, q, &q->inst) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeGroupPolicy(
    JNIEnv* env, jobject thiz, jlong id, jstring group, jboolean all, jstring token, jstring keep) {
    (void)thiz;
    const char* key = group ? jni_utf8(env, group) : NULL;
    const char* t = token ? jni_utf8(env, token) : NULL;
    const char* k = keep ? jni_utf8(env, keep) : NULL;
    if (!id || !chat_channel_valid(key) || !k || strlen(k) > 1024 * 1024 || (token && !t) || (t && strlen(t) != 64)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "Android policy invalid request id=%lld group=%s", (long long)id, key ? key : "-");
        u_free((void*)key); u_free((void*)t); u_free((void*)k); return JNI_FALSE;
    }
    struct group_policy_request* q = u_calloc(1, sizeof(*q) + strlen(k) + 1);
    if (q) {
        q->id = id; strcpy(q->group, key); q->all_members = all ? 1 : 0;
        if (t) memcpy(q->token, t, 65);
        memcpy(q->keep, k, strlen(k) + 1);
    } else DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "Android policy request allocation failed");
    u_free((void*)key); u_free((void*)t); u_free((void*)k);
    return q && !bridge_post_request(group_policy_gui_command, q, &q->inst) ? JNI_TRUE : JNI_FALSE;
}

static int android_file_address(JNIEnv* env, jbyteArray key, uint8_t ed[MF_ADDRESS_SIZE]) {
    if (!key || (*env)->GetArrayLength(env, key) != MF_ADDRESS_SIZE) return -1;
    (*env)->GetByteArrayRegion(env, key, 0, MF_ADDRESS_SIZE, (jbyte*)ed);
    return (*env)->ExceptionCheck(env) || mf_zero(ed, MF_ADDRESS_SIZE) ? -1 : 0;
}

static int android_file_is_self(const uint8_t ed[MF_ADDRESS_SIZE]) {
    struct bridge_query query = {.kind = QUERY_SIGNING_KEY};
    return !bridge_query_call(&query) && !memcmp(query.own_ed, ed, MF_ADDRESS_SIZE);
}

/* Только операции аватарок; файловые пути ограничены каталогом avatars. Владение req переходит uasync. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeAvatarOperation(
    JNIEnv* env, jobject thiz, jint op, jlong id, jbyteArray group,
    jstring remote, jstring local, jbyteArray data, jbyteArray server_key, jbyteArray owner_key) {
    (void)thiz;
    uint8_t group_ed[MF_ADDRESS_SIZE] = {0};
    if (!instance_lite_is_running() || !id || op < 0 || op > 8 || (op != 7 && android_file_address(env, group, group_ed))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "Android avatar: unavailable/invalid operation=%d id=%lld group=%s", op,
                   (long long)id, sc_public_key_hex(group_ed, (char[65]){0})); return JNI_FALSE;
    }
    const char* path = remote ? jni_utf8(env, remote) : NULL;
    const char* file = local ? jni_utf8(env, local) : NULL;
    void* req = NULL; uasync_post_callback_t callback = NULL;
    struct UTUN_INSTANCE** instance_field = NULL;
    if ((remote && !path) || (local && !file)) goto invalid;
    uint8_t server_ed[MF_ADDRESS_SIZE], owner_ed[MF_ADDRESS_SIZE];
    if (((op >= 1 && op <= 4) || op == 8) && (android_file_address(env, server_key, server_ed) ||
        android_file_address(env, owner_key, owner_ed) || !path ||
        (op == 3 || op == 8 ? strcmp(path, "avatars") != 0 :
         strncmp(path, "avatars/", 8) != 0 || !mf_avatar_file_valid(path + 8)))) goto invalid;
    if (op == 0 || op == 6) {
        struct group_avatar_gui_request* r = u_calloc(1, sizeof(*r)); req = r;
        if (!r) goto invalid;
        instance_field = &r->inst; r->id = id; memcpy(r->group, group_ed, MF_ADDRESS_SIZE);
        if (op == 6) {
            jsize size = data ? (*env)->GetArrayLength(env, data) : 0;
            if (size > GROUP_AVATAR_MAX) goto invalid;
            r->size = size;
            if (size) (*env)->GetByteArrayRegion(env, data, 0, size, (jbyte*)r->data);
            if ((*env)->ExceptionCheck(env)) goto invalid;
        }
        callback = op == 0 ? android_avatar_snapshot : group_avatar_gui_set;
    } else if (op == 1 || op == 2) {
        if (!file || !file[0] || strlen(file) > MF_PATH_MAX) goto invalid;
        struct mf_transfer_request* r = u_calloc(1, sizeof(*r)); req = r;
        if (!r) goto invalid;
        instance_field = &r->inst; r->id = id; memcpy(r->group_key, group_ed, MF_ADDRESS_SIZE);
        memcpy(r->server, server_ed, MF_ADDRESS_SIZE); memcpy(r->owner, owner_ed, MF_ADDRESS_SIZE);
        uint8_t limit[8];
        if (!data || (*env)->GetArrayLength(env, data) != 8) goto invalid;
        (*env)->GetByteArrayRegion(env, data, 0, 8, (jbyte*)limit);
        if ((*env)->ExceptionCheck(env)) goto invalid;
        r->upload = op == 2; r->max_bytes = mf_get64(limit);
        if (!r->max_bytes || r->max_bytes > mf_avatar_file_limit(path + 8)) goto invalid;
        memcpy(r->remote, path, strlen(path) + 1); memcpy(r->local, file, strlen(file) + 1);
        callback = member_files_gui_transfer;
    } else if (op == 3 || op == 4 || op == 8) {
        struct mf_gui_request* r = u_calloc(1, sizeof(*r)); req = r;
        if (!r) goto invalid;
        instance_field = &r->inst; memcpy(r->group, group_ed, MF_ADDRESS_SIZE); memcpy(r->server, server_ed, MF_ADDRESS_SIZE);
        r->message.kind = MF_REQUEST; r->message.command = op == 3 ? MF_MKDIR : op == 8 ? MF_LIST : MF_STAT;
        r->message.request_id = id; memcpy(r->message.owner, owner_ed, MF_ADDRESS_SIZE);
        if (op == 8) {
            uint8_t page[24];
            if (!data || (*env)->GetArrayLength(env, data) != 24) goto invalid;
            (*env)->GetByteArrayRegion(env, data, 0, 24, (jbyte*)page);
            if ((*env)->ExceptionCheck(env)) goto invalid;
            r->message.cursor = mf_get64(page); memcpy(r->message.version, page + 8, 16);
        }
        memcpy(r->message.path, path, strlen(path) + 1); callback = member_files_gui_request;
    } else if (op == 5) {
        if (server_key && (android_file_address(env, server_key, server_ed) || !mf_avatar_manifest_valid(path))) goto invalid;
        struct chat_avatar_req* r = u_calloc(1, sizeof(*r)); req = r;
        if (!r) goto invalid;
        instance_field = &r->inst; r->id = id; memcpy(r->group, group_ed, MF_ADDRESS_SIZE);
        if (server_key) { memcpy(r->server, server_ed, sizeof(r->server)); memcpy(r->file, path, strlen(path) + 1); }
        callback = chat_core_set_my_avatar_trampoline;
    } else {
        struct mf_cancel_request* r = u_calloc(1, sizeof(*r)); req = r;
        if (!r) goto invalid;
        instance_field = &r->inst; r->id = id; callback = member_files_gui_cancel;
    }
    if (path) u_free((void*)path);
    if (file) u_free((void*)file);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "Android avatar: queued operation=%d id=%lld group=%s",
                op, (long long)id, sc_public_key_hex(group_ed, (char[65]){0}));
    return bridge_post_request(callback, req, instance_field) == 0 ? JNI_TRUE : JNI_FALSE;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "Android avatar: invalid arguments/allocation operation=%d id=%lld group=%s",
                op, (long long)id, sc_public_key_hex(group_ed, (char[65]){0}));
    if (path) u_free((void*)path);
    if (file) u_free((void*)file);
    u_free(req); return JNI_FALSE;
}

/* Общие файловые команды. Wire уже UTF-8, поэтому JNI modified UTF-8 не портит имена с emoji. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeFileRequest(
    JNIEnv* env, jobject thiz, jbyteArray group, jbyteArray server, jbyteArray wire) {
    (void)thiz;
    uint8_t group_ed[MF_ADDRESS_SIZE] = {0};
    jsize size = wire ? (*env)->GetArrayLength(env, wire) : 0;
    struct mf_gui_request* q = NULL;
    if (!instance_lite_is_running() || android_file_address(env, group, group_ed) || !server || size < MF_WIRE_HEADER || size > MF_WIRE_MAX) goto invalid;
    uint8_t bytes[MF_WIRE_MAX];
    (*env)->GetByteArrayRegion(env, wire, 0, size, (jbyte*)bytes);
    if ((*env)->ExceptionCheck(env)) goto invalid;
    q = u_calloc(1, sizeof(*q));
    if (!q || android_file_address(env, server, q->server) || mf_decode(bytes, size, &q->message)) goto invalid;
    struct mf_message* m = &q->message;
    int read = m->command == MF_LIST || m->command == MF_STAT || m->command == MF_QUOTA;
    int write = m->command == MF_MKDIR || m->command == MF_RENAME || m->command == MF_REMOVE_PATH;
    if (m->kind != MF_REQUEST || (!read && !write) || m->data_len || m->offset || m->size || m->more ||
        !mf_path_valid(m->path, read) || (mf_zero(m->owner, MF_ADDRESS_SIZE) && (m->command != MF_LIST || m->path[0])) ||
        (m->command == MF_RENAME ? !mf_path_valid(m->target, 0) : m->target[0] != 0)) goto invalid;
    if (write && (!android_file_is_self(m->owner) ||
        (!strncasecmp(m->path, "avatars", 7) && (!m->path[7] || m->path[7] == '/')))) goto invalid;
    if (m->command == MF_RENAME && !strncasecmp(m->target, "avatars", 7) && (!m->target[7] || m->target[7] == '/')) goto invalid;
    memcpy(q->group, group_ed, MF_ADDRESS_SIZE);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "Android files: request id=%llu cmd=%s group=%s server=%s owner=%s path=%s",
        (unsigned long long)m->request_id, mf_command_text(m->command), sc_public_key_hex(group_ed, (char[65]){0}),
        sc_public_key_hex(q->server, (char[65]){0}), sc_public_key_hex(m->owner, (char[65]){0}), m->path);
    int posted = bridge_post_request(member_files_gui_request, q, &q->inst); return posted == 0 ? JNI_TRUE : JNI_FALSE;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Android files: request rejected group=%s bytes=%d ready=%d",
        sc_public_key_hex(group_ed, (char[65]){0}), (int)size, instance_lite_is_running());
    u_free(q); return JNI_FALSE;
}

/* Копирование настоящего UTF-8 пути с проверкой длины и отсутствия NUL. */
static int android_file_path(JNIEnv* env, jbyteArray bytes, char* out) {
    jsize size = bytes ? (*env)->GetArrayLength(env, bytes) : 0;
    if (size < 1 || size > MF_PATH_MAX) return -1;
    (*env)->GetByteArrayRegion(env, bytes, 0, size, (jbyte*)out);
    if ((*env)->ExceptionCheck(env) || memchr(out, 0, size)) return -1;
    out[size] = 0; return 0;
}

/* Handle/локальный файл принадлежат общему transfer, Kotlin ждёт его терминальное событие. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeFileTransfer(
    JNIEnv* env, jobject thiz, jlong id, jbyteArray group, jbyteArray server, jbyteArray owner,
    jbyteArray remote, jbyteArray local, jboolean upload, jbyteArray version) {
    (void)thiz;
    uint8_t group_ed[MF_ADDRESS_SIZE] = {0};
    struct mf_transfer_request* q = NULL;
    if (!instance_lite_is_running() || !id || android_file_address(env, group, group_ed) || !server || !owner ||
        !version || (*env)->GetArrayLength(env, version) != 16) goto invalid;
    q = u_calloc(1, sizeof(*q));
    if (!q || android_file_address(env, server, q->server) || android_file_address(env, owner, q->owner) ||
        android_file_path(env, remote, q->remote) || android_file_path(env, local, q->local) ||
        !mf_path_valid(q->remote, 0) || q->local[0] != '/') goto invalid;
    (*env)->GetByteArrayRegion(env, version, 0, 16, (jbyte*)q->version);
    if ((*env)->ExceptionCheck(env) || (!upload && mf_zero(q->version, 16))) goto invalid;
    if (upload && (!android_file_is_self(q->owner) ||
        (!strncasecmp(q->remote, "avatars", 7) && (!q->remote[7] || q->remote[7] == '/')))) goto invalid;
    q->id = id; memcpy(q->group_key, group_ed, MF_ADDRESS_SIZE); q->upload = upload != JNI_FALSE;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "Android files: transfer queued id=%llu group=%s server=%s owner=%s upload=%d path=%s",
        (unsigned long long)id, sc_public_key_hex(group_ed, (char[65]){0}), sc_public_key_hex(q->server, (char[65]){0}),
        sc_public_key_hex(q->owner, (char[65]){0}), q->upload, q->remote);
    int posted = bridge_post_request(member_files_gui_transfer, q, &q->inst); return posted == 0 ? JNI_TRUE : JNI_FALSE;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Android files: transfer rejected id=%llu group=%s upload=%d",
        (unsigned long long)id, sc_public_key_hex(group_ed, (char[65]){0}), upload);
    u_free(q); return JNI_FALSE;
}

/* Отмена по уникальному ID не затрагивает передачи других владельцев. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeFileCancel(JNIEnv* env, jobject thiz, jlong id) {
    (void)env; (void)thiz;
    if (!id || !instance_lite_is_running()) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "Android files: cancel unavailable id=%llu", (unsigned long long)id); return JNI_FALSE;
    }
    struct mf_cancel_request* q = u_calloc(1, sizeof(*q));
    if (!q) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Android files: cancel allocation failed"); return JNI_FALSE; }
    q->id = id;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "Android files: cancel queued id=%llu", (unsigned long long)id);
    int posted = bridge_post_request(member_files_gui_cancel, q, &q->inst); return posted == 0 ? JNI_TRUE : JNI_FALSE;
}

#endif /* __ANDROID__ */
