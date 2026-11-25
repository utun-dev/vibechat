#include "../radio/radio_call.h"
#include "chat_devgroup.h"
#include <openssl/crypto.h>
/*
 * chat_headless_control.c — TCP control socket for headless chat CLI
 *
 * JSON line protocol:
 *   Request:  {"id":N,"cmd":"<command>",...params}
 *   Response: {"id":N,"ok":true,"data":{...}} | {"id":N,"ok":false,"error":"..."}
 *   Event:    {"event":"<type>",...}
 *
 * Commands: ping, status, channels, members, messages, send, invite,
 *           invite_nodes, connect, create_channel, subscribe, quit
 */
#include "chat_headless_control.h"
#include "chat_core.h"
#include "../config_updater.h"
#include "chat_channel.h"
#include "group_policy.h"
#include "group_info.h"
#include "group_info_read.h"
#include "member_sync.h"
#include "merkle_sync.h"
#include "../kanban/kanban.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "chat_poll.h"
#include "chat_sync.h"
#include "chat_join.h"
#include "invite_link.h"
#include "invite_build.h"
#include "../dm/dm_core.h"
#include "../dm/dm_media.h"
#include "../dm/dm_history.h"
#include "../dm/dm_crypto.h"
#include "../call/call.h"
#include "../routing_layer/topo_node.h"
#include "../member_files/protocol.h"
#include "../call/call_proto.h"
#include "../radio/radio.h"
#include "../transport_layer/secure_channel.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/etcp_connections.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../media_delivery/media_index.h"
#include "../../lib/file_io.h"
#include "../../lib/ll_queue.h"
#include "../../lib/u_async.h"
#include "../../lib/db_async.h"
#include "../../lib/mem.h"
#include "../../lib/strbuf.h"
#include "../../lib/json_flat.h"
#include "../../lib/socket_compat.h"
#include "../../lib/debug_config.h"
#include "../../lib/platform_compat.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>

#include <sqlite3.h>
#include <openssl/rand.h>

#define HC_ID "headless"

#define RECV_BUF_SIZE 16384
#define SEND_BUF_SIZE 32768
#define MAX_CLIENTS    64
#define MAX_FIELDS     32

struct hc_field {
    char key[32];
    const char* text;               /* Декодированная строка из arena запроса. */
    int integer, kind;              /* kind:1=text,2=int/bool,0=array/object/прочее для feedback. */
};
struct hc_request {
    struct hc_field fields[MAX_FIELDS];
    unsigned count;
    size_t used;
    char arena[RECV_BUF_SIZE];      /* Живёт до возврата синхронного handler. */
};

#ifndef DEBUG_CATEGORY_HEADLESS
#define DEBUG_CATEGORY_HEADLESS DEBUG_CATEGORY_CONTROL
#endif

struct headless_client {
    unsigned refs;                  /* Список listener и собственные DB callbacks; только uasync. */
    socket_t fd;
    void*    socket_id;
    char     recv_buf[RECV_BUF_SIZE];
    int      recv_len;
    char     send_buf[SEND_BUF_SIZE];
    int      send_len;
    int      send_offset;
    int      subscribed;
    int      closing;
    int      draining;
    void*    input_work;            /* Отложенный разбор накопленных строк, отменяется при close. */
    struct hc_command_job* parser;  /* Не более одного READ разбора на клиента; в это время чтение сокета выключено. */
    struct hc_query_job* query;     /* Чтение/создание/отправка DM; следующая команда ждёт завершения. */
    const struct hc_request* request;
    struct headless_invite* invites;
    struct headless_submission* submissions;
    struct   headless_control* hc;
    struct   headless_client* next;
};

struct headless_invite {
    struct headless_invite* next;
    struct headless_client* client;
    struct chat_invite_build* pending;
    int id;
    uint64_t channel; /* Только candidates callback; вход был скопирован до worker READ. */
};

struct headless_submission {
    struct headless_submission* next;
    struct headless_client* client; /* При disconnect обнуляется;последний media completion освобождает объект. */
    int id;
    char destination[1024];
};

struct headless_control {
    unsigned refs;                 /* Владелец inst и каждый ещё живой client. */
    struct UASYNC*           ua;
    struct UTUN_INSTANCE*    inst;
    struct headless_client*  clients;
    int                      client_count;
    socket_t                 listen_fd;
    void*                    listen_sock_id;
    int                      running;
    void*                    cleanup_timer;
    chat_event_handler_fn    previous_handler;
    char bind_ip[INET_ADDRSTRLEN];
    int configured_port;
};

struct hc_command_sql {
    struct hc_request request;      /* Все строки ссылаются только на собственную arena. */
    char json[RECV_BUF_SIZE];       /* Скопированная входная строка, включая NUL. */
};
struct hc_command_job {
    struct hc_command_sql sql;      /* Worker получает только SQL-образ, без доступа к клиенту/сокету. */
    struct headless_client* client; /* Собственная ссылка до done/cleanup, включая disconnect/stop. */
};

static struct headless_control* hc_of(struct UTUN_INSTANCE* inst) {
    return inst ? (struct headless_control*)inst->headless : NULL;
}

static void hc_release(struct headless_control* hc) {
    if (--hc->refs) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_HEADLESS, "headless: context released after last client completion"); u_free(hc);
}

static void hc_client_release(struct headless_client* cli) {
    if (--cli->refs) return;
    struct headless_control* hc = cli->hc;
    DEBUG_DEBUG(DEBUG_CATEGORY_HEADLESS, "headless: client released fd=%d parser=%p query=%p",
        (int)cli->fd, (void*)cli->parser, (void*)cli->query);
    u_free(cli); hc_release(hc);
}

/* ── Forward declarations ── */

static void hc_handle_command(struct headless_client* cli, const char* json);
static void hc_close_client(struct headless_client* cli);
static void client_write_callback(socket_t fd, void* arg);
static void hc_process_input(void* arg);

/* ── JSON helpers ── */

/* Все значения уже проверены/скопированы,SQL cursor закрыт до выполнения команды. */
static int json_get_int(struct headless_client* cli, const char* key, int def) {
    for (unsigned i = 0; i < cli->request->count; i++)
        if (!strcmp(cli->request->fields[i].key, key)) return cli->request->fields[i].integer;
    return def;
}

static int json_get_str(struct headless_client* cli, const char* key, char* out, size_t size) {
    if (size) out[0] = 0;
    for (unsigned i = 0; i < cli->request->count; i++) {
        const struct hc_field* field = &cli->request->fields[i];
        if (strcmp(field->key, key)) continue;
        if (field->kind != 1 || strlen(field->text) >= size) {
            DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: invalid string parameter key=%s capacity=%zu", key, size); return -1;
        }
        memcpy(out, field->text, strlen(field->text) + 1); return 0;
    }
    return -1;
}

/* Один полный разбор объекта;у feedback остаётся исходный JSON с массивами/типами. */
static int hc_parse_request(sqlite3* db, const char* json, struct hc_request* request) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT CASE WHEN json_valid(?1) THEN json_type(?1) END,key,type,value FROM json_each(?1)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    while (rc == SQLITE_OK || rc == SQLITE_ROW) {
        rc = sqlite3_step(st); if (rc != SQLITE_ROW) break;
        const char *root = (const char*)sqlite3_column_text(st, 0), *key = (const char*)sqlite3_column_text(st, 1);
        const char* type = (const char*)sqlite3_column_text(st, 2);
        if (!root || strcmp(root, "object") || !key || !type || request->count == MAX_FIELDS ||
            strlen(key) != (size_t)sqlite3_column_bytes(st, 1) || strlen(key) >= sizeof(request->fields[0].key)) {
            rc = SQLITE_MISMATCH; break;
        }
        int duplicate = 0;
        for (unsigned i = 0; i < request->count; i++) duplicate |= !strcmp(request->fields[i].key, key);
        if (duplicate) { rc = SQLITE_CONSTRAINT; break; }
        struct hc_field* field = &request->fields[request->count++]; strcpy(field->key, key);
        if (!strcmp(type, "text")) {
            const char* value = (const char*)sqlite3_column_text(st, 3); int bytes = sqlite3_column_bytes(st, 3);
            if (!value || strlen(value) != (size_t)bytes || (size_t)bytes + 1 > sizeof(request->arena) - request->used) {
                rc = SQLITE_MISMATCH; break;
            }
            memcpy(request->arena + request->used, value, (size_t)bytes + 1);
            field->kind = 1; field->text = request->arena + request->used; request->used += (size_t)bytes + 1;
        } else if (!strcmp(type, "integer") || !strcmp(type, "true") || !strcmp(type, "false")) {
            sqlite3_int64 value = sqlite3_column_int64(st, 3);
            if (sqlite3_column_type(st, 3) != SQLITE_INTEGER || value < INT_MIN || value > INT_MAX) {
                rc = SQLITE_RANGE; break;
            }
            field->kind = 2; field->integer = (int)value;
        }
        int integer = !strcmp(key, "id") || !strcmp(key, "count") || !strcmp(key, "offset") ||
            !strcmp(key, "as_video") || !strcmp(key, "proto") || !strcmp(key, "enable");
        if (integer && (field->kind != 2 || (!strcmp(type, "true") || !strcmp(type, "false")) &&
            strcmp(key, "enable") && strcmp(key, "as_video"))) { rc = SQLITE_MISMATCH; break; }
        if ((!strcmp(key, "enable") || !strcmp(key, "as_video")) && field->integer != 0 && field->integer != 1) {
            rc = SQLITE_RANGE; break;
        }
        if (!strcmp(key, "offset") && field->integer < 0) { rc = SQLITE_RANGE; break; }
        const struct { const char* key; size_t size; } limits[] = {
            {"cmd",32},{"ch",65},{"name",128},{"data",4096},{"content_type",32},{"path",1024},
            {"link",INVITE_LINK_MAX},{"node_id",32},{"call_id",32},{"conv_id",64},{"pubkey",65},{"ed25519",65},{"addr",128}
        };
        for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) if (!strcmp(key, limits[i].key) &&
            (field->kind != 1 || strlen(field->text) >= limits[i].size)) { rc = SQLITE_MISMATCH; break; }
        if (rc != SQLITE_ROW) break;
        if (!strcmp(key, "ch") && field->text[0]) {
            int valid = !strncmp(field->text, "dm:", 3) ? chat_numeric_id(field->text + 3) != 0 : chat_channel_valid(field->text);
            if (!valid) { rc = SQLITE_MISMATCH; break; }
        }
        if (!strcmp(key, "conv_id") && !chat_numeric_id(field->text)) { rc = SQLITE_MISMATCH; break; }
        if (!strcmp(key, "node_id") || !strcmp(key, "call_id")) {
            const char* digits = field->text;
            if (!strncmp(digits, "0x", 2) || !strncmp(digits, "0X", 2)) digits += 2;
            size_t length = strlen(digits);
            if (!length || length > 16 || strspn(digits, "0123456789abcdefABCDEF") != length ||
                (!strcmp(key, "call_id") && !strtoull(digits, NULL, 16))) { rc = SQLITE_MISMATCH; break; }
        }
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        int error = rc == SQLITE_DONE ? final : rc;
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: request rejected fields=%u rc=%d finalize=%d: %s",
            request->count, rc, final,
            (sqlite3_errcode(db) & 255) == (error & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(error)); return error;
    }
    return 0;
}

/* ── Send helpers ── */

static void cli_send(struct headless_client* cli, const char* data, size_t len) {
    if (!cli || cli->closing || cli->fd == SOCKET_INVALID || !data || len == 0) return;
    int pending = cli->send_len - cli->send_offset;
    if (len > sizeof(cli->send_buf) - (size_t)pending) {
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: slow client closed fd=%d pending=%d incoming=%zu limit=%zu",
            (int)cli->fd, pending, len, sizeof(cli->send_buf)); hc_close_client(cli); return;
    }
    if (pending && cli->send_offset) memmove(cli->send_buf, cli->send_buf + cli->send_offset, (size_t)pending);
    memcpy(cli->send_buf + pending, data, len); cli->send_len = pending + (int)len; cli->send_offset = 0;
    uasync_set_socket_write(cli->hc->ua, cli->socket_id, 1);
    client_write_callback(cli->fd, cli);
}

static void cli_send_str(struct headless_client* cli, const char* s) {
    if (s) cli_send(cli, s, strlen(s));
}

static void send_response(struct headless_client* cli, int id, const char* ok_data, const char* error) {
    struct strbuf out = strbuf_new(); int rc;
    if (error) {
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: command failed id=%d reason=%s", id, error);
        rc = strbuf_addf(&out, "{\"id\":%d,\"ok\":false,\"error\":", id);
        if (rc >= 0) rc = strbuf_add_json_string(&out, error);
        if (rc >= 0) rc = strbuf_addf(&out, "}\n");
    } else rc = strbuf_addf(&out, "{\"id\":%d,\"ok\":true,\"data\":%s}\n", id, ok_data ? ok_data : "null");
    if (rc < 0 || out.len > SEND_BUF_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: response rejected id=%d bytes=%zu queued=%d rc=%d",
            id, out.len, cli->send_len, rc);
        char failure[160]; int bytes = snprintf(failure, sizeof(failure),
            "{\"id\":%d,\"ok\":false,\"error\":\"Response too large or out of memory; request fewer records\"}\n", id);
        cli_send(cli, failure, bytes);
    } else cli_send(cli, strbuf_str(&out), out.len);
    strbuf_free(&out);
}

/* ── Broadcast event ── */

static void hc_broadcast_event(struct headless_control* hc, const char* json) {
    size_t jlen = strlen(json);
    char buf[4096]; int blen = snprintf(buf, sizeof(buf), "%s\n", json);
    if (blen <= 0 || (size_t)blen >= sizeof(buf)) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: event exceeds limit bytes=%zu", jlen); return; }
    for (struct headless_client* c = hc->clients; c; c = c->next)
        if (c->subscribed && !c->draining) cli_send(c, buf, (size_t)blen);
}

/* ── chat_event callback ── */

static void hc_emit_chat_event(struct headless_control* hc, int type, const uint8_t* data, int len) {
    if (type == 10) return; /* skip status text */
    if (type == CHAT_EVT_MSG_RECEIVED || type == CHAT_EVT_MEMBERS_CHANGED || type == CHAT_EVT_CHANNEL_UPDATED ||
        type == CHAT_EVT_INVITE_RECEIVED || type == CHAT_EVT_DM_MSG_RECEIVED || type == CHAT_EVT_DM_CONV_UPDATED ||
        type == CHAT_EVT_CALL_INCOMING || type == CHAT_EVT_FEEDBACK_CHANGED) {
        if (!data || len < 1 || !data[0] || data[0] > 64 || len < 1 + data[0]) {
            DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: invalid event prefix type=%d bytes=%d", type, len); return;
        }
        char channel[65]; memcpy(channel, data + 1, data[0]); channel[data[0]] = 0;
        int direct = type == CHAT_EVT_DM_MSG_RECEIVED || type == CHAT_EVT_DM_CONV_UPDATED;
        if (strlen(channel) != data[0] || (direct ? !chat_numeric_id(channel) : !chat_channel_valid(channel))) {
            DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: invalid event identity type=%d bytes=%d", type, len); return;
        }
        int expected = type == CHAT_EVT_MSG_RECEIVED ? data[0] + 1 + CHAT_MSG_EVENT_SUFFIX_SIZE :
            type == CHAT_EVT_MEMBERS_CHANGED || type == CHAT_EVT_CHANNEL_UPDATED ? data[0] + 1 :
            type == CHAT_EVT_DM_MSG_RECEIVED ? data[0] + 33 :
            type == CHAT_EVT_DM_CONV_UPDATED ? data[0] + 9 :
            type == CHAT_EVT_CALL_INCOMING ? data[0] + 41 : type == CHAT_EVT_FEEDBACK_CHANGED ? data[0] + 34 : len;
        if (len != expected) { DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: invalid event size type=%d bytes=%d expected=%d", type, len, expected); return; }
    }
    char json[1024]; json[0] = '\0';
    if (type == CHAT_EVT_FEEDBACK_CHANGED && data && len >= 34 && len == data[0] + 34) {
        uint8_t cl = data[0]; char message_id[65];
        for (unsigned i = 0; i < 32; i++) snprintf(message_id + i * 2, 3, "%02x", data[1 + cl + i]);
        snprintf(json, sizeof(json), "{\"event\":\"feedback\",\"ch\":\"%.*s\",\"message_id\":\"%s\",\"kind\":%u}",
            (int)cl, data + 1, message_id, data[33 + cl]);
    } else if (type == CHAT_EVT_MSG_RECEIVED) {
        if (data[len - 1] > 1) {
            DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "headless: invalid MSG_RECEIVED length=%d", len);
            return;
        }
        uint8_t cl = data[0];
        char author[65];
        sc_public_key_hex(data + 1 + cl, author);
        snprintf(json, sizeof(json), "{\"event\":\"msg\",\"ch\":\"%.*s\",\"author_id\":\"%s\",\"initial_sync\":%u}",
                 (int)cl, data + 1, author, data[len - 1]);
    } else if (type == 5 && data && len >= 2) { /* MEMBERS_CHANGED */
        uint8_t cl = data[0];
        snprintf(json, sizeof(json), "{\"event\":\"members_changed\",\"ch\":\"%.*s\"}", (int)cl, data + 1);
    } else if (type == 4 && data && len >= 2) { /* CHANNEL_UPDATED */
        uint8_t cl = data[0];
        snprintf(json, sizeof(json), "{\"event\":\"channel_updated\",\"ch\":\"%.*s\"}", (int)cl, data + 1);
    } else if (type == CHAT_EVT_INVITE_RECEIVED) {
        uint8_t cl = data[0];
        if (len >= 2 + cl) {
            uint8_t nl = data[1 + cl];
            int off = 2 + cl + nl;
            if (len >= off + 33) {
                if (len != off + 33 + data[off + 32] || memchr(data + 2 + cl, 0, nl)) {
                    DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: malformed invite event bytes=%d", len); return;
                }
                char name[256]; memcpy(name, data + 2 + cl, nl); name[nl] = 0;
                struct strbuf event = strbuf_new();
                int rc = strbuf_addf(&event, "{\"event\":\"invite_received\",\"ch\":\"%.*s\",\"ch_name\":", (int)cl, data + 1);
                if (rc >= 0) rc = strbuf_add_json_string(&event, name);
                if (rc >= 0) rc = strbuf_addf(&event, ",\"from_x25519\":\"%s\"}", sc_public_key_hex(data + off, (char[65]){0}));
                if (rc >= 0) hc_broadcast_event(hc, strbuf_str(&event));
                else DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: invite event allocation failed");
                strbuf_free(&event); return;
            }
        }
    } else if (type == CHAT_EVT_DM_MSG_RECEIVED) {
        uint8_t cl = data[0];
        snprintf(json, sizeof(json), "{\"event\":\"dm_msg\",\"conv\":\"%.*s\",\"author_id\":\"%s\"}",
                 (int)cl, data + 1, sc_public_key_hex(data + 1 + cl, (char[65]){0}));
    } else if (type == 33 && data && len >= 2) { /* DM_CONV_UPDATED */
        uint8_t cl = data[0];
        snprintf(json, sizeof(json), "{\"event\":\"dm_conv\",\"conv\":\"%.*s\"}", (int)cl, data + 1);
    } else if (type == CHAT_EVT_CALL_INCOMING) {
        uint8_t cl = data[0];
        uint64_t cid; memcpy(&cid, data + 1 + cl + 32, 8);
        snprintf(json, sizeof(json), "{\"event\":\"call_incoming\",\"ch\":\"%.*s\",\"caller_ed25519\":\"%s\",\"call_id\":\"0x%016llx\"}",
                 (int)cl, data + 1, sc_public_key_hex(data + 1 + cl, (char[65]){0}), (unsigned long long)cid);
    } else if (type == CHAT_EVT_RADIO_CALL && data && len == CHAT_RADIO_CALL_EVENT_SIZE) {
        uint64_t cid; memcpy(&cid, data, 8);
        snprintf(json, sizeof(json), "{\"event\":\"radio_call\",\"call_id\":\"0x%016llx\",\"ch\":\"%s\","
            "\"peer_ed25519\":\"%s\",\"state\":%u,\"reason\":%u,\"incoming\":%u}",
            (unsigned long long)cid, sc_public_key_hex(data + 8, (char[65]){0}), sc_public_key_hex(data + CHAT_RADIO_CALL_PEER, (char[65]){0}),
            data[CHAT_RADIO_CALL_STATE], data[CHAT_RADIO_CALL_STATE + 1], data[CHAT_RADIO_CALL_STATE + 2]);
    } else if (type == CHAT_EVT_CALL_RINGING && data && len >= 8) { /* CALL_RINGING */
        uint64_t cid; memcpy(&cid, data, 8);
        snprintf(json, sizeof(json), "{\"event\":\"call_ringing\",\"call_id\":\"0x%016llx\"}", (unsigned long long)cid);
    } else if (type == CHAT_EVT_CALL_ACCEPTED && data && len == 41) {
        uint64_t cid; memcpy(&cid, data, 8);
        snprintf(json, sizeof(json), "{\"event\":\"call_accepted\",\"call_id\":\"0x%016llx\",\"peer_ed25519\":\"%s\"}",
                 (unsigned long long)cid, sc_public_key_hex(data + 9, (char[65]){0}));
    } else if (type == CHAT_EVT_CALL_ENDED && data && len >= 9) { /* CALL_ENDED */
        uint64_t cid; memcpy(&cid, data, 8);
        snprintf(json, sizeof(json), "{\"event\":\"call_ended\",\"call_id\":\"0x%016llx\",\"reason\":%d}", (unsigned long long)cid, data[8]);
    } else if (type == CHAT_EVT_CALL_DECLINED && data && len >= 9) { /* CALL_DECLINED */
        uint64_t cid; memcpy(&cid, data, 8);
        snprintf(json, sizeof(json), "{\"event\":\"call_declined\",\"call_id\":\"0x%016llx\",\"reason\":%d}", (unsigned long long)cid, data[8]);
    } else if (type == CHAT_EVT_CALL_ERROR && data && len == 9) {
        uint64_t cid; memcpy(&cid, data, 8);
        snprintf(json, sizeof(json), "{\"event\":\"call_error\",\"call_id\":\"0x%016llx\",\"error\":%u}", (unsigned long long)cid, data[8]);
    } else if (type == CHAT_EVT_CONNECT_RESULT && data && len == CHAT_CONNECT_RESULT_SIZE) {
        uint64_t request; int32_t result;
        const uint8_t* group = data + CHAT_CONNECT_RESULT_GROUP_OFF;
        memcpy(&result, data + CHAT_CONNECT_RESULT_CODE_OFF, 4);
        memcpy(&request, data + CHAT_CONNECT_RESULT_REQUEST_OFF, 8);
        snprintf(json, sizeof(json), "{\"event\":\"connect_result\",\"x25519\":\"%s\",\"group_ed25519\":\"%s\",\"request\":%llu,\"result\":%d}",
            sc_public_key_hex(data, (char[65]){0}), sc_public_key_hex(group, (char[65]){0}), (unsigned long long)request, (int)result);
    }
    if (json[0]) hc_broadcast_event(hc, json);
}

/* Headless дополняет прежнего получателя;после его callback к hc больше не обращаемся. */
static void hc_on_chat_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    struct headless_control* hc = hc_of(inst);
    if (!hc) return;
    chat_event_handler_fn previous = hc->previous_handler;
    hc_emit_chat_event(hc, type, data, len);
    if (previous) previous(inst, type, data, len);
}

/* ── Write callback ── */

static void client_write_callback(socket_t fd, void* arg) {
    struct headless_client* cli = (struct headless_client*)arg;
    struct headless_control* hc = cli ? cli->hc : NULL;
    if (!cli || cli->closing || fd != cli->fd) return;
    if (cli->send_len <= 0) { if (hc && hc->ua) uasync_set_socket_write(hc->ua, cli->socket_id, 0); return; }
    ssize_t r = socket_send(cli->fd, cli->send_buf + cli->send_offset, (size_t)(cli->send_len - cli->send_offset));
    if (r < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK)) return;
    if (r <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: send failed (r=%zd err=%d) — closing client", r, socket_get_error());
        hc_close_client(cli);
        return;
    }
    cli->send_offset += (int)r;
    if (cli->send_offset >= cli->send_len) {
        cli->send_len = 0; cli->send_offset = 0;
        if (hc && hc->ua) uasync_set_socket_write(hc->ua, cli->socket_id, 0);
        if (cli->draining) hc_close_client(cli);
    }
}

/* ── Close client ── */

static void hc_close_client(struct headless_client* cli) {
    struct headless_control* hc = cli ? cli->hc : NULL;
    if (!cli || cli->closing) return;
    cli->closing = 1;
    if (cli->input_work) { uasync_call_soon_cancel(hc->ua, cli->input_work); cli->input_work = NULL; }
    while (cli->invites) chat_invite_build_cancel(cli->invites->pending);
    while (cli->submissions) {
        struct headless_submission* pending = cli->submissions; cli->submissions = pending->next; pending->client = NULL;
    }
    if (hc && hc->ua && cli->socket_id) uasync_remove_socket_t(hc->ua, cli->fd);
    cli->socket_id = NULL;
    if (cli->fd != SOCKET_INVALID) { socket_close_wrapper(cli->fd); cli->fd = SOCKET_INVALID; }
}

/* ── Read callback ── */

/* Разбор следующей строки всегда отложен: даже немедленный отказ очереди не создаёт рекурсивную цепочку. */
static void hc_schedule_input(struct headless_client* cli) {
    if (cli->closing || cli->draining || !cli->hc->running || cli->input_work || cli->parser || cli->query) return;
    cli->input_work = uasync_call_soon(cli->hc->ua, cli, hc_process_input);
    if (!cli->input_work) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: input scheduling failed fd=%d queued=%d", (int)cli->fd, cli->recv_len);
        hc_close_client(cli);
    }
}
static int hc_parse_work(sqlite3* db, void* arg) {
    struct hc_command_sql* sql = arg; return hc_parse_request(db, sql->json, &sql->request);
}
static int hc_parse_guard(void* arg) {
    struct hc_command_job* job = arg;
    return !job->client->closing && job->client->hc->running && job->client->parser == job;
}
static void hc_parse_cleanup(void* arg) {
    struct hc_command_job* job = arg; hc_client_release(job->client); u_free(job);
}
static void hc_parse_done(void* arg, const struct db_async_result* result) {
    struct hc_command_job* job = arg; struct headless_client* cli = job->client;
    if (cli->parser != job) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: parser ownership lost client=%p", (void*)cli); return;
    }
    if (!cli->closing && cli->hc->running) {
        if (result->status) {
            DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: parser READ failed fd=%d status=%d rc=%d: %s",
                (int)cli->fd, result->status, result->sqlite_code, result->message);
            send_response(cli, 0, NULL, "invalid request");
        }
        else { cli->request = &job->sql.request; hc_handle_command(cli, job->sql.json); }
    }
    cli->request = NULL; cli->parser = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_HEADLESS, "headless: parsed client=%p fd=%d fields=%u status=%d queued=%d",
        (void*)cli, (int)cli->fd, job->sql.request.count, result->status, cli->recv_len);
    hc_schedule_input(cli);
}

/* Входной буфер хранит и хвост следующей команды. На клиента только один READ; TCP получает backpressure. */
static void hc_process_input(void* arg) {
    struct headless_client* cli = arg; cli->input_work = NULL;
    if (cli->closing || cli->draining || !cli->hc->running || cli->parser || cli->query) return;
    int skipped = 0; while (skipped < cli->recv_len && cli->recv_buf[skipped] == '\n') skipped++;
    if (skipped) { cli->recv_len -= skipped; memmove(cli->recv_buf, cli->recv_buf + skipped, (size_t)cli->recv_len); }
    const char* newline = memchr(cli->recv_buf, '\n', (size_t)cli->recv_len);
    if (!newline) {
        if (cli->recv_len == RECV_BUF_SIZE) {
            DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: oversized input fd=%d bytes=%d", (int)cli->fd, cli->recv_len);
            hc_close_client(cli); return;
        }
        int rc = uasync_set_socket_read(cli->hc->ua, cli->socket_id, 1);
        if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: enable read failed fd=%d rc=%d", (int)cli->fd, rc); hc_close_client(cli); }
        return;
    }
    struct hc_command_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: parser request allocation failed fd=%d", (int)cli->fd); hc_close_client(cli); return;
    }
    size_t bytes = (size_t)(newline - cli->recv_buf); memcpy(job->sql.json, cli->recv_buf, bytes);
    cli->recv_len -= (int)bytes + 1; memmove(cli->recv_buf, newline + 1, (size_t)cli->recv_len);
    job->client = cli; cli->refs++; cli->parser = job;
    int rc = uasync_set_socket_read(cli->hc->ua, cli->socket_id, 0);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: pause read failed fd=%d rc=%d", (int)cli->fd, rc);
        hc_close_client(cli); cli->parser = NULL; hc_parse_cleanup(job); return;
    }
    struct UTUN_INSTANCE* inst = cli->hc->inst;
    if (inst->db_worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="headless-parse", .mode=DB_ASYNC_READ, .work=hc_parse_work,
            .data=&job->sql, .done=hc_parse_done, .cleanup=hc_parse_cleanup, .guard=hc_parse_guard}); return;
    }
    sqlite3* ram = chat_core_get_db(inst); const char* file = ram ? sqlite3_db_filename(ram, "main") : NULL;
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_ABORT};
    snprintf(result.message, sizeof(result.message), "headless parser has no asynchronous reader");
    if (file && !*file) {
        result.sqlite_code = hc_parse_work(ram, &job->sql); result.status = result.sqlite_code ? DB_ASYNC_SQL : DB_ASYNC_OK;
        snprintf(result.message, sizeof(result.message), "RAM headless parse: %s", sqlite3_errstr(result.sqlite_code));
    }
    hc_parse_done(job, &result); hc_parse_cleanup(job);
}

static void client_read_callback(socket_t fd, void* arg) {
    struct headless_client* cli = (struct headless_client*)arg;
    if (!cli || cli->closing || cli->draining || cli->parser || cli->query || cli->input_work || fd != cli->fd) return;
    int available = RECV_BUF_SIZE - cli->recv_len; if (available > 4096) available = 4096;
    if (!available) { hc_process_input(cli); return; }
    ssize_t r = recv(fd, cli->recv_buf + cli->recv_len, available, 0);
    if (r < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK)) return;
    if (r <= 0) {
        if (r < 0) DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: receive failed err=%d", socket_get_error());
        hc_close_client(cli); return;
    }
    if (memchr(cli->recv_buf + cli->recv_len, 0, (size_t)r)) {
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: NUL input fd=%d bytes=%zd", (int)fd, r); hc_close_client(cli); return;
    }
    cli->recv_len += (int)r; hc_process_input(cli);
}

/* ── Command handlers ── */

static void hc_handle_ping(struct headless_client* cli, int id, const char* json) {
    (void)json;
    char address[65], data[144];
    bytes_to_hex(cli->hc->inst->my_ed25519_pubkey, 32, address, sizeof(address));
    snprintf(data, sizeof(data), "{\"pong\":true,\"uptime_sec\":0,\"ed25519\":\"%s\"}", address);
    send_response(cli, id, data, NULL);
}

static void hc_handle_status(struct headless_client* cli, int id, const char* json) {
    (void)json;
    struct headless_control* hc = cli->hc;
    struct strbuf text = strbuf_new(), encoded = strbuf_new(); struct uasync_load load;
    int rc;
    if (uasync_get_load(hc->ua, &load) == ERR_OK && load.valid)
        rc = strbuf_addf(&text, "uasync CPU: %.1f%%, average %.1f%% (window %.0f ms, age %.0f ms)\n",
            load.current_percent, load.average_percent, load.window_us / 1000.0, (get_time_us() - load.sample_time_us) / 1000.0);
    else rc = strbuf_addf(&text, "uasync CPU: unavailable\n");
    struct NTP_TIME* ntp = &hc->inst->ntp;
    if (rc >= 0) rc = strbuf_addf(&text, "NTP: enabled=%s synced=%s offset=%.1fs\nConnections: %d active\n",
        ntp->enabled ? "yes" : "no", ntp->synced ? "yes" : "no", ntp->offset_us / 1000000.0,
        hc->inst->connections ? queue_entry_count(hc->inst->connections) : 0);
    for (struct ll_entry* e = hc->inst->connections ? hc->inst->connections->head : NULL; e && rc >= 0; e = e->next) {
        struct conn_queue_entry* conn = (struct conn_queue_entry*)e->data;
        if (!conn) continue;
        char name[64] = "";
        if (conn->conn && chat_core_get_node_name(hc->inst, conn->conn->crypto_ctx.peer_public_key, name, sizeof(name))) { rc = -1; break; }
        rc = strbuf_addf(&text, "  [%04llX]->[%04llX] %s ETCP:%s\n", (unsigned long long)(hc->inst->node_id & 0xffff),
            (unsigned long long)(conn->peer_node_id & 0xffff), name[0] ? name : "?", conn->conn && conn->conn->links_up ? "UP" : "DOWN");
        if (text.len > SEND_BUF_SIZE) rc = -1;
    }
    if (rc >= 0) rc = strbuf_add_json_string(&encoded, strbuf_str(&text));
    if (rc < 0) DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: status snapshot failed bytes=%zu", text.len);
    send_response(cli, id, rc < 0 ? NULL : strbuf_str(&encoded), rc < 0 ? "status snapshot failed or too large" : NULL);
    strbuf_free(&text); strbuf_free(&encoded);
}

enum hc_query_kind { HC_CHANNELS, HC_MEMBERS, HC_MESSAGES, HC_CONVERSATIONS, HC_DM_MESSAGES, HC_DM_START, HC_DM_SEND,
    HC_DEVGROUP, HC_GROUP_INFO, HC_GROUP_POLICY, HC_FEEDBACK, HC_KANBAN, HC_SEND, HC_CREATE_CHANNEL };
struct hc_query_sql {
    enum hc_query_kind kind;          /* Тип зафиксирован до READ. */
    char channel[65];                 /* Собственный канонический ID, пуст для channels. */
    int count, offset;
    struct chat_json_rows rows;       /* SQL-свойства; живые статусы добавляет done. */
    char json[RECV_BUF_SIZE];
    size_t length;
};
struct hc_query_job {
    struct hc_query_sql sql;          /* Единственные данные, доступные work. */
    struct headless_client* client;   /* Ссылка до cleanup даже после disconnect/stop. */
    int id;
};

static int hc_query_work(sqlite3* db, void* arg) {
    struct hc_query_sql* sql = arg;
    if (sql->kind == HC_CHANNELS) return chat_json_channels_read(db, sizeof(sql->json), &sql->rows);
    if (sql->kind == HC_MEMBERS) return chat_json_members_read(db, sql->channel, sizeof(sql->json), &sql->rows);
    return chat_json_messages_read(db, sql->channel, sql->count, sql->offset, sql->json, sizeof(sql->json), &sql->length);
}
static int hc_query_guard(void* arg) {
    struct hc_query_job* job = arg; return !job->client->closing && job->client->hc->running && job->client->query == job;
}
static void hc_query_cleanup(void* arg) {
    struct hc_query_job* job = arg; chat_json_rows_free(&job->sql.rows); hc_client_release(job->client); u_free(job);
}
/* Освободить слот до следующей строки; SQL-снимок может принадлежать общему API подсистемы. */
static void hc_query_publish(struct hc_query_job* job, int failed, const char* json, size_t length) {
    struct headless_client* cli = job->client; struct hc_query_sql* sql = &job->sql;
    if (cli->query != job) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: query ownership lost client=%p", (void*)cli); return; }
    if (!cli->closing && cli->hc->running) {
        const char* error = sql->kind == HC_SEND ? "message save failed" : sql->kind == HC_KANBAN ? "kanban operation failed" :
            sql->kind == HC_FEEDBACK ? "interaction failed" :
            sql->kind == HC_CREATE_CHANNEL ? "channel creation failed" :
            sql->kind == HC_DEVGROUP ? "devgroup operation failed" :
            sql->kind == HC_GROUP_INFO ? "group description unavailable" : sql->kind == HC_GROUP_POLICY ? "group policy unavailable" :
            sql->kind == HC_CHANNELS ? "failed to query channels" :
            sql->kind == HC_MEMBERS ? "failed to query members" : sql->kind == HC_MESSAGES ? "failed to query messages" :
            sql->kind == HC_CONVERSATIONS ? "failed to query conversations" :
            sql->kind == HC_DM_START ? "failed to start DM" : sql->kind == HC_DM_SEND ? "failed to send DM" : "failed to query DM messages";
        char reason[128];
        if (failed && json && !json_flat_get(json, "error", reason, sizeof(reason))) error = reason;
        send_response(cli, job->id, failed ? NULL : json, failed ? error : NULL);
    }
    cli->query = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_HEADLESS, "headless: query complete client=%p id=%d kind=%d status=%d bytes=%zu queued=%d",
        (void*)cli, job->id, sql->kind, failed, length, cli->recv_len);
    hc_schedule_input(cli);
}
static void hc_query_done(void* arg, const struct db_async_result* result) {
    struct hc_query_job* job = arg; struct headless_client* cli = job->client; struct hc_query_sql* sql = &job->sql;
    int rc = result->status;
    if (!rc && cli->query == job && !cli->closing && cli->hc->running && sql->kind != HC_MESSAGES)
        rc = chat_json_rows_render(cli->hc->inst, sql->kind == HC_MEMBERS ? sql->channel : NULL,
            &sql->rows, sql->json, sizeof(sql->json), &sql->length);
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: query failed id=%d kind=%d ch=%s status=%d rc=%d: %s",
        job->id, sql->kind, sql->channel, result->status, result->sqlite_code, result->message);
    hc_query_publish(job, rc, sql->json, sql->length);
}
static void hc_dm_query_done(void* arg, const struct dm_json_result* result) {
    hc_query_publish(arg, result->status, result->json, result->length); hc_query_cleanup(arg);
}
static void hc_dm_start_done(void* arg, const struct dm_start_result* result) {
    struct hc_query_job* job = arg; size_t length = 0;
    if (!result->status) length = (size_t)snprintf(job->sql.json, sizeof(job->sql.json),
        "{\"conv_id\":\"%llu\",\"started\":true}", (unsigned long long)result->conversation);
    hc_query_publish(job, result->status, job->sql.json, length); hc_query_cleanup(job);
}
static void hc_dm_send_done(void* arg, const struct dm_send_result* result) {
    hc_query_publish(arg, result->status, "{\"sent\":true}", sizeof("{\"sent\":true}") - 1); hc_query_cleanup(arg);
}
static struct hc_query_job* hc_command_begin(struct headless_client* cli, int id, enum hc_query_kind kind) {
    struct hc_query_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless command allocation failed kind=%d", kind); send_response(cli, id, NULL, "out of memory"); return NULL; }
    job->client = cli; job->id = id; job->sql.kind = kind; cli->refs++; cli->query = job; return job;
}
static void hc_devgroup_done(void* data, int status, const char* json) {
    hc_query_publish(data, status, json, json ? strlen(json) : 0); hc_query_cleanup(data);
}
static void hc_feedback_done(void* data, const struct chat_feedback_command_result* result) {
    hc_query_publish(data, result->status, result->json, result->json ? strlen(result->json) : 0); hc_query_cleanup(data);
}
static void hc_kanban_done(void* data, const struct kanban_execute_result* result) {
    hc_query_publish(data, result->status, result->json, result->json ? strlen(result->json) : 0); hc_query_cleanup(data);
}
static void hc_message_done(void* data, const struct chat_message_result* result) {
    hc_query_publish(data, result->status, "{\"sent\":true}", sizeof("{\"sent\":true}") - 1); hc_query_cleanup(data);
}

struct hc_group_job {
    struct hc_query_job reply; /* Первый объект: общий client lifetime/последовательная обработка команд. */
    uint64_t generation, registration;
    struct member_sync_write_context context;
    struct group_info_write info;
    struct group_policy_mutation policy;
    int writing;
    char name[128], description[GROUP_INFO_MAX + 1], token[65], keep[RECV_BUF_SIZE];
    char* result;
};
static int hc_group_guard(void* data) {
    struct hc_group_job* job = data; struct UTUN_INSTANCE* inst = job->reply.client->hc->inst;
    if (!hc_query_guard(&job->reply) || inst->destroying || !inst->group_started || chat_core_generation(inst) != job->generation) return 0;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, job->reply.sql.channel) : NULL;
    return group && !group->stopping && group->generation == job->registration;
}
static int hc_group_work(sqlite3* db, void* data) {
    struct hc_group_job* job = data; const char* channel = job->reply.sql.channel;
    if (job->reply.sql.kind == HC_GROUP_INFO) {
        if (!job->writing) return group_info_snapshot_read(db, channel, job->info.self_ed, &job->result);
        int rc = group_info_command_write(db, &job->info); job->result = job->info.result; job->info.result = NULL; return rc;
    }
    if (!job->writing) return group_policy_preview_read(db, channel, job->policy.all_members, job->keep, &job->result);
    int rc = group_policy_command_write(db, &job->policy); job->result = job->policy.json; job->policy.json = NULL; return rc;
}
static void hc_group_done(void* data, const struct db_async_result* result) {
    struct hc_group_job* job = data; struct UTUN_INSTANCE* inst = job->reply.client->hc->inst;
    int current = hc_group_guard(job), failed = result->status || !current;
    if (failed) {
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless group command id=%d kind=%d current=%d status=%d rc=%d: %s",
            job->reply.id, job->reply.sql.kind, current, result->status, result->sqlite_code, result->message);
        u_free(job->result); job->result = NULL;
    } else if (job->reply.sql.kind == HC_GROUP_INFO) {
        failed = job->info.status;
        if (job->info.access) { group_policy_snapshot_publish(inst, job->info.access); job->info.access = NULL; }
        if (job->info.changed && hc_group_guard(job)) group_info_publish(inst, job->reply.sql.channel);
    } else if (job->writing && !job->policy.status) {
        struct group_policy_update* admission = job->policy.admission; job->policy.admission = NULL;
        group_policy_publish(inst, admission);
        if (hc_group_guard(job)) merkle_sync_changed(inst, MT_DATA_MEMBERS, job->reply.sql.channel);
    }
    hc_query_publish(&job->reply, failed, job->result, job->result ? strlen(job->result) : 0);
}
static void hc_group_free(void* data) {
    struct hc_group_job* job = data; group_policy_snapshot_free(job->info.access); group_policy_discard(job->policy.admission);
    u_free(job->result); OPENSSL_cleanse(&job->context, sizeof(job->context)); hc_query_cleanup(&job->reply);
}
static void hc_group_submit(struct headless_client* cli, int id, enum hc_query_kind kind, const char* channel,
    const char* name, const char* description, const char* token, const char* keep, int all) {
    struct UTUN_INSTANCE* inst = cli->hc->inst; struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, channel) : NULL;
    if (!group || group->stopping) { send_response(cli, id, NULL, "group unavailable"); return; }
    struct hc_group_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless group command allocation failed"); send_response(cli, id, NULL, "out of memory"); return; }
    job->reply.client = cli; job->reply.id = id; job->reply.sql.kind = kind; snprintf(job->reply.sql.channel, sizeof(job->reply.sql.channel), "%s", channel);
    job->generation = chat_core_generation(inst); job->registration = group->generation;
    job->context.self = inst->node_id; job->context.now = ntp_time_get_us(inst);

    memcpy(job->context.public_key, inst->my_ed25519_pubkey, 32); memcpy(job->context.private_key, inst->my_ed25519_privkey, 32);
    if (name) snprintf(job->name, sizeof(job->name), "%s", name);
    if (description) snprintf(job->description, sizeof(job->description), "%s", description);
    if (token) snprintf(job->token, sizeof(job->token), "%s", token);
    snprintf(job->keep, sizeof(job->keep), "%s", keep ? keep : "[]");
    job->writing = kind == HC_GROUP_INFO ? name || description : token != NULL;
    job->info = (struct group_info_write){.channel=job->reply.sql.channel,.clipboard_members=-1,
        .name=name ? job->name : NULL,.json=description ? job->description : NULL,.expected=token ? job->token : NULL};
    memcpy(job->info.self_ed, inst->my_ed25519_pubkey, 32);
    job->policy = (struct group_policy_mutation){.channel=job->reply.sql.channel,.token=token ? job->token : NULL,
        .keep=job->keep,.all_members=all,.context=&job->context};
    cli->refs++; cli->query = &job->reply;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="headless-group",.mode=job->writing ? DB_ASYNC_WRITE : DB_ASYNC_READ,
        .data=job,.guard=hc_group_guard,.work=hc_group_work,.done=hc_group_done,.cleanup=hc_group_free});
}

/* Команда владеет копией аргументов и клиентом; всё продолжение, включая отказ очереди, завершается одним done. */
static void hc_handle_query(struct headless_client* cli, int id, enum hc_query_kind kind) {
    char channel[65] = "";
    if ((kind == HC_MEMBERS || kind == HC_MESSAGES) &&
        (json_get_str(cli, "ch", channel, sizeof(channel)) || !chat_channel_valid(channel))) {
        send_response(cli, id, NULL, "missing or invalid 'ch' param"); return;
    }
    if (kind == HC_DM_MESSAGES &&
        (json_get_str(cli, "conv_id", channel, sizeof(channel)) || !chat_numeric_id(channel))) {
        send_response(cli, id, NULL, "missing or invalid 'conv_id' param"); return;
    }
    struct hc_query_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: query allocation failed id=%d kind=%d", id, kind);
        send_response(cli, id, NULL, "out of memory"); return;
    }
    job->client = cli; job->id = id; job->sql.kind = kind; memcpy(job->sql.channel, channel, sizeof(channel));
    job->sql.count = json_get_int(cli, "count", 20); job->sql.offset = json_get_int(cli, "offset", 0);
    cli->refs++; cli->query = job; struct UTUN_INSTANCE* inst = cli->hc->inst;
    if (kind == HC_CONVERSATIONS || kind == HC_DM_MESSAGES) {
        dm_json_read(inst, &(struct dm_json_query){.conv=kind == HC_DM_MESSAGES ? job->sql.channel : NULL,
            .capacity=sizeof(job->sql.json), .count=job->sql.count, .offset=job->sql.offset, .include_hidden=1,
            .valid=hc_query_guard}, hc_dm_query_done, job); return;
    }
    if (inst->db_worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="headless-query", .mode=DB_ASYNC_READ, .work=hc_query_work,
            .data=&job->sql, .done=hc_query_done, .cleanup=hc_query_cleanup, .guard=hc_query_guard}); return;
    }
    sqlite3* ram = chat_core_get_db(inst); const char* file = ram ? sqlite3_db_filename(ram, "main") : NULL;
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_ABORT};
    snprintf(result.message, sizeof(result.message), "headless query has no asynchronous reader");
    if (file && !*file) {
        result.sqlite_code = hc_query_work(ram, &job->sql); result.status = result.sqlite_code ? DB_ASYNC_SQL : DB_ASYNC_OK;
        snprintf(result.message, sizeof(result.message), "RAM headless query: %s", sqlite3_errstr(result.sqlite_code));
    }
    hc_query_done(job, &result); hc_query_cleanup(job);
}

static void hc_handle_send(struct headless_client* cli, int id, const char* json) {
    char ch[65], ct[32], data[4096];
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) { send_response(cli, id, NULL, "missing 'ch' param"); return; }
    if (json_get_str(cli, "content_type", ct, sizeof(ct)) < 0) { ct[0] = 't'; ct[1] = 'e'; ct[2] = 'x'; ct[3] = 't'; ct[4] = '\0'; }
    if (json_get_str(cli, "data", data, sizeof(data)) < 0) { send_response(cli, id, NULL, "missing 'data' param"); return; }
    struct hc_query_job* job = hc_command_begin(cli, id, HC_SEND); if (!job) return;
    struct chat_msg_submit req = {.inst=cli->hc->inst,.data=(uint8_t*)data,.data_len=strlen(data)};
    snprintf(req.channel_id, sizeof(req.channel_id), "%s", ch); snprintf(req.content_type, sizeof(req.content_type), "%s", ct);
    chat_core_submit_message_async(cli->hc->inst, &req, hc_message_done, job);
}

static int hc_mkdirs(const char* path) {
    char tmp[1024]; snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (utun_mkdir(tmp, 0755) != 0 && errno != EEXIST) { tmp[i] = '/'; return -1; }
        tmp[i] = '/';
    }
    if (utun_mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Callback не зависит от lifetime control-сервера после disconnect/stop. */
static void hc_media_ready(void* arg, int success) {
    struct headless_submission* pending = arg; struct headless_client* cli = pending->client;
    if (!cli) { u_free(pending); return; }
    struct headless_submission** slot = &cli->submissions;
    while (*slot && *slot != pending) slot = &(*slot)->next;
    if (*slot) *slot = pending->next;
    if (cli->draining) { u_free(pending); return; }
    struct strbuf response = strbuf_new(); int rc = -1, id = pending->id;
    if (success) {
        rc = strbuf_addf(&response, "{\"attached\":true,\"dest\":");
        if (rc >= 0) rc = strbuf_add_json_string(&response, pending->destination);
        if (rc >= 0) rc = strbuf_addf(&response, "}");
    }
    u_free(pending);
    send_response(cli, id, rc >= 0 ? strbuf_str(&response) : NULL, rc >= 0 ? NULL : "attachment publication failed");
    strbuf_free(&response);
}

static void hc_handle_attach(struct headless_client* cli, int id, const char* json) {
    struct headless_control* hc = cli->hc;
    char ch[65], path[1024], ct[32];
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) { send_response(cli, id, NULL, "missing 'ch' param"); return; }
    if (json_get_str(cli, "path", path, sizeof(path)) < 0) { send_response(cli, id, NULL, "missing 'path' param"); return; }
    if (json_get_str(cli, "content_type", ct, sizeof(ct)) < 0)
        snprintf(ct, sizeof(ct), "application/octet-stream");
    int as_video = json_get_int(cli, "as_video", 0);
    if (!chat_channel_valid(ch) || !si_find(hc->inst, ch) || (as_video != 0 && as_video != 1)) {
        send_response(cli, id, NULL, "invalid/unavailable channel or video flag"); return;
    }
    struct file_io_info info;
    if (file_io_inspect(path, 1, &info) || info.type != FILE_IO_REGULAR || info.size <= 0) {
        send_response(cli, id, NULL, "regular nonempty file required"); return;
    }

    if (!hc->inst || !hc->inst->media_async) { send_response(cli, id, NULL, "media engine not initialized"); return; }

    const char* media_base = chat_core_media_base(hc->inst);
    if (!media_base) { send_response(cli, id, NULL, "media base unavailable"); return; }

    const char* base = strrchr(path, '/'); base = base ? base + 1 : path;
    if (!*base || strlen(base) >= 256) { send_response(cli, id, NULL, "invalid attachment name"); return; }
    char name[256]; strcpy(name, base);

    char dir[640]; snprintf(dir, sizeof(dir), "%s/media/%s", media_base, ch);
    if (hc_mkdirs(dir) != 0) { send_response(cli, id, NULL, "failed to create media dir"); return; }

    uint8_t uuid[16]; char hex[33], dest[1024];
    if (RAND_bytes(uuid, sizeof(uuid)) != 1) { send_response(cli, id, NULL, "attachment UUID unavailable"); return; }
    for (unsigned i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", uuid[i]);
    snprintf(dest, sizeof(dest), "%s/%s.%s", dir, hex, as_video ? "mp4" : "bin");

    struct chat_msg_submit* req = u_calloc(1, sizeof(*req));
    if (!req) { send_response(cli, id, NULL, "out of memory"); return; }
    struct headless_submission* pending = u_calloc(1, sizeof(*pending));
    if (!pending) { u_free(req); send_response(cli, id, NULL, "out of memory"); return; }
    pending->client = cli; pending->id = id; strcpy(pending->destination, dest);
    pending->next = cli->submissions; cli->submissions = pending;
    req->inst = hc->inst;
    snprintf(req->channel_id, sizeof(req->channel_id), "%s", ch);
    snprintf(req->content_type, sizeof(req->content_type), "%s", as_video ? "video/mp4" : ct);
    snprintf(req->media_src, sizeof(req->media_src), "%s", path);
    snprintf(req->media_dest, sizeof(req->media_dest), "%s", dest);
    req->media_copy  = as_video ? 0 : 1;
    req->media_video = as_video ? 1 : 0;
    req->data = (uint8_t*)name;
    req->data_len = (uint32_t)strlen(name);

    DEBUG_INFO((int)DEBUG_CATEGORY_HEADLESS, "headless: attach ch=%s src=%s dst=%s video=%d",
               ch, path, dest, as_video);

    chat_core_submit_media_message(hc->inst, req, hc_media_ready, pending);
    u_free(req);
}

static struct headless_client* hc_invite_detach(struct headless_invite* req, int* id) {
    struct headless_client* cli = req->client;
    struct headless_invite** p = &cli->invites;
    while (*p && *p != req) p = &(*p)->next;
    if (*p) *p = req->next;
    *id = req->id; u_free(req); return cli;
}
static void hc_invite_ready(void* arg, int result, const char* link) {
    int id; struct headless_client* cli = hc_invite_detach(arg, &id);
    if (cli->closing || cli->draining) return;
    if (result != CHAT_JOIN_OK) {
        send_response(cli, id, NULL, result == CHAT_JOIN_TIMEOUT ? "invite registration timed out" : "invite registration failed");
        return;
    }
    struct InviteData d; char err[128];
    if (invite_link_decode(link, strlen(link), &d, err, sizeof(err)) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: cannot decode confirmed invite: %s", err);
        send_response(cli, id, NULL, "failed to decode built link");
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_HEADLESS, "headless: invite confirmed x25519=%s addrs=%d",
               sc_public_key_hex(d.pubkey, (char[65]){0}), (int)d.addrCount);
    char resp[INVITE_LINK_MAX + 256];
    snprintf(resp, sizeof(resp),
        "{\"link\":\"%s\",\"x25519\":\"%s\",\"addrs\":%d}",
        link, sc_public_key_hex(d.pubkey, (char[65]){0}), (int)d.addrCount);
    send_response(cli, id, resp, NULL);
}

static void hc_handle_invite(struct headless_client* cli, int id, const char* json) {
    struct headless_control* hc = cli->hc;
    char ch[65];
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) { send_response(cli, id, NULL, "missing 'ch' param"); return; }
    if (!hc->inst) { send_response(cli, id, NULL, "no instance"); return; }

    uint64_t channel_id = topo_groups_channel_slot(hc->inst->topo_groups, ch);
    if (channel_id == 0) { send_response(cli, id, NULL, "invalid channel_id"); return; }

    uint8_t target[32] = {0};
    for (unsigned i = 0; i < cli->request->count; i++) {
        const struct hc_field* field = &cli->request->fields[i];
        if (!strcmp(field->key, "node_id")) {
            DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: numeric invite target ch=%s", ch);
            send_response(cli, id, NULL, "invite takes a public Ed25519 key"); return;
        }
        if (strcmp(field->key, "ed25519")) continue;
        if (field->kind != 1 || sc_hex_to_binary(field->text, target, 32) || mf_zero(target, 32)) {
            DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: invalid invite target ch=%s", ch);
            send_response(cli, id, NULL, "invalid target Ed25519"); return;
        }
    }

    struct headless_invite* req = u_calloc(1, sizeof(*req));
    if (!req) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: invite allocation failed");
        send_response(cli, id, NULL, "out of memory"); return;
    }
    req->client = cli; req->id = id;
    req->pending = chat_invite_build_link(hc->inst, channel_id, target, hc_invite_ready, req);
    if (!req->pending) { u_free(req); send_response(cli, id, NULL, "failed to build invite link (see log)"); return; }
    req->next = cli->invites;
    cli->invites = req;
}

static void hc_candidates_ready(void* data, int result, const uint8_t best[32], const uint8_t (*ids)[32], size_t count);
static void hc_server_link_ready(void* data, int result, const char* link);
static void hc_handle_invite_nodes(struct headless_client* cli, int id, const char* json) {
    struct headless_control* hc = cli->hc;
    char ch[65];
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) { send_response(cli, id, NULL, "missing 'ch' param"); return; }
    uint64_t channel_id = topo_groups_channel_slot(hc->inst->topo_groups, ch);
    if (channel_id == 0) { send_response(cli, id, NULL, "invalid channel_id"); return; }

    struct headless_invite* request = u_calloc(1, sizeof(*request));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless candidates allocation failed"); send_response(cli, id, NULL, "out of memory"); return; }
    request->client = cli; request->id = id; request->channel = channel_id;
    request->pending = chat_invite_candidates_start(hc->inst, channel_id, INVITE_ADDR_MAX, hc_candidates_ready, request);
    if (!request->pending) { u_free(request); send_response(cli, id, NULL, "candidate snapshot unavailable"); return; }
    request->next = cli->invites; cli->invites = request;
}
static void hc_candidates_ready(void* data, int result, const uint8_t best[32], const uint8_t (*ids)[32], size_t count) {
    struct headless_invite* request = data; uint64_t channel = request->channel;
    int id; struct headless_client* cli = hc_invite_detach(request, &id);
    if (cli->closing || cli->draining) return;
    if (result != CHAT_JOIN_OK) { send_response(cli, id, NULL, "candidate snapshot unavailable"); return; }
    struct strbuf resp = strbuf_new();
    int rc = strbuf_addf(&resp, "{\"ch\":\"%llu\",\"auto_ed25519\":\"%s\",\"nodes\":[",
        (unsigned long long)channel, sc_public_key_hex(best, (char[65]){0}));
    struct UTUN_INSTANCE* inst = cli->hc->inst;
    struct TOPO_GROUP* group = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, channel) : NULL;
    for (size_t i = 0; i < count && rc >= 0; i++) {
        const struct TOPO_GROUP_NODE* node = group ? topo_node_find_by_ed(group, ids[i]) : NULL;
        const char* name = inst && !memcmp(inst->my_ed25519_pubkey, ids[i], 32) ? inst->name :
            node && node->identity && node->identity->node_name ? node->identity->node_name : "";
        rc = strbuf_addf(&resp, "%s{\"ed25519\":\"%s\",\"name\":", i ? "," : "", sc_public_key_hex(ids[i], (char[65]){0}));
        if (rc >= 0) rc = strbuf_add_json_string(&resp, name);
        if (rc >= 0) rc = strbuf_addf(&resp, "}");
    }
    if (rc >= 0) rc = strbuf_addf(&resp, "]}");
    if (rc < 0) DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: invite candidates formatting failed");
    send_response(cli, id, rc < 0 ? NULL : strbuf_str(&resp), rc < 0 ? "candidate snapshot failed" : NULL);
    strbuf_free(&resp);
}

static void hc_handle_server_link(struct headless_client* cli, int id, const char* json) {
    (void)json;
    struct headless_invite* request = u_calloc(1, sizeof(*request));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless server link allocation failed"); send_response(cli, id, NULL, "out of memory"); return; }
    request->client = cli; request->id = id;
    request->pending = chat_server_link_start(cli->hc->inst, hc_server_link_ready, request);
    if (!request->pending) { u_free(request); send_response(cli, id, NULL, "server link unavailable"); return; }
    request->next = cli->invites; cli->invites = request;
}
static void hc_server_link_ready(void* data, int result, const char* link) {
    int id; struct headless_client* cli = hc_invite_detach(data, &id);
    if (cli->closing || cli->draining) return;
    if (result != CHAT_JOIN_OK || !link) { send_response(cli, id, NULL, "server link unavailable"); return; }
    char response[INVITE_LINK_MAX + 32];
    snprintf(response, sizeof(response), "{\"link\":\"%s\"}", link);
    send_response(cli, id, response, NULL);
}

static void hc_handle_server_add(struct headless_client* cli, int id, const char* json) {
    char channel[65], link[INVITE_LINK_MAX]; struct InviteData target;
    if (json_get_str(cli, "ch", channel, sizeof(channel)) < 0 || !chat_channel_valid(channel) ||
        json_get_str(cli, "link", link, sizeof(link)) < 0 || invite_link_decode(link, strlen(link), &target, NULL, 0) ||
        target.kind != INVITE_KIND_SERVER) {
        send_response(cli, id, NULL, "valid 'ch' and SERVER 'link' required"); return;
    }
    int rc = chat_sync_offer_link(cli->hc->inst, 0, topo_groups_channel_slot(cli->hc->inst->topo_groups, channel), link, strlen(link));
    if (rc) { send_response(cli, id, NULL, "server addition could not start (see log)"); return; }
    send_response(cli, id, "{\"adding\":true}", NULL);
}

static void hc_handle_connect(struct headless_client* cli, int id, const char* json) {
    struct headless_control* hc = cli->hc;
    char link[INVITE_LINK_MAX];
    if (json_get_str(cli, "link", link, sizeof(link)) < 0) { send_response(cli, id, NULL, "missing 'link' param"); return; }
    struct InviteData d; char err[256];
    if (invite_link_decode(link, strlen(link), &d, err, sizeof(err)) < 0) { send_response(cli, id, NULL, err); return; }
    if (!hc->inst) { send_response(cli, id, NULL, "no instance"); return; }

    char x[65]; sc_public_key_hex(d.pubkey, x);
    DEBUG_INFO(DEBUG_CATEGORY_HEADLESS, "headless: connect x25519=%s addrs=%d",
        x, (int)d.addrCount);

    int result = chat_sync_join_link(hc->inst, 0, link, strlen(link));
    if (result) { send_response(cli, id, NULL, "join could not start (see log)"); return; }

    char resp[512]; snprintf(resp, sizeof(resp),
        "{\"x25519\":\"%s\",\"addrs\":%d,\"connecting\":true}",
        x, (int)d.addrCount);
    send_response(cli, id, resp, NULL);
}

static void hc_channel_created(void* arg, const struct chat_channel_result* result) {
    struct hc_query_job* job = arg;
    char response[128]; size_t length = 0;
    if (!result->status) length = (size_t)snprintf(response, sizeof(response), "{\"created\":true,\"channel_id\":\"%s\"}",
        result->channel);
    else DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: create channel failed rc=%d committed=%d", result->sqlite_code, result->committed);
    hc_query_publish(job, result->status, length ? response : NULL, length);
    hc_query_cleanup(job);
}

static void hc_handle_create_channel(struct headless_client* cli, int id, const char* json) {
    char name[256];
    if (json_get_str(cli, "name", name, sizeof(name)) < 0) { send_response(cli, id, NULL, "missing 'name' param"); return; }
    char type[8] = "chat";
    for (unsigned i = 0; i < cli->request->count; i++) if (!strcmp(cli->request->fields[i].key, "group_type")) {
        if (json_get_str(cli, "group_type", type, sizeof(type))) { send_response(cli, id, NULL, "invalid group_type"); return; }
    }
    struct hc_query_job* job = hc_command_begin(cli, id, HC_CREATE_CHANNEL);
    if (job) chat_channel_create_auto_submit(cli->hc->inst, name, type, hc_channel_created, job);
}

static void hc_handle_invite_to(struct headless_client* cli, int id, const char* json) {
    struct headless_control* hc = cli->hc;
    char ch[65], node_id_str[65], addr[128];
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) { send_response(cli, id, NULL, "missing 'ch' param"); return; }
    if (json_get_str(cli, "ed25519", node_id_str, sizeof(node_id_str)) < 0) { send_response(cli, id, NULL, "missing 'ed25519' param"); return; }
    if (json_get_str(cli, "addr", addr, sizeof(addr)) < 0) { send_response(cli, id, NULL, "missing 'addr' param"); return; }
    int proto = json_get_int(cli, "proto", 1);
    if (!hc->inst) { send_response(cli, id, NULL, "no instance"); return; }

    uint8_t ed[32];
    if (sc_hex_to_binary(node_id_str, ed, sizeof(ed))) {
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: invite_to invalid Ed25519 ch=%s", ch);
        send_response(cli, id, NULL, "expected the remote Ed25519 public key (64 hex digits)"); return;
    }

    char ip[128]; snprintf(ip, sizeof(ip), "%s", addr);
    char* colon = strrchr(ip, ':');
    if (!colon) { send_response(cli, id, NULL, "invalid addr (no port)"); return; }
    *colon = '\0';
    char* end = NULL; errno = 0; long port = strtol(colon + 1, &end, 10);
    if (errno || end == colon + 1 || *end || port <= 0 || port > 65535 || (proto != INVITE_PROTO_UDP && proto != INVITE_PROTO_TCP)) {
        send_response(cli, id, NULL, "invalid port/protocol"); return;
    }

    struct InviteAddr endpoint = {.proto = (uint8_t)proto, .port = (uint16_t)port};
    if (inet_pton(AF_INET, ip, endpoint.address) == 1) endpoint.family = 4;
    else if (inet_pton(AF_INET6, ip, endpoint.address) == 1) endpoint.family = 6;
    else { send_response(cli, id, NULL, "invalid addr ip"); return; }

    DEBUG_INFO((int)DEBUG_CATEGORY_HEADLESS, "headless: invite_to ch=%s node=%s addr=%s:%ld proto=%d",
               ch, node_id_str, ip, port, proto);

    int rc = chat_sync_invite_to_channel(hc->inst, topo_groups_channel_slot(hc->inst->topo_groups, ch), ed, &endpoint);
    if (rc) {
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: invite_to start failed ch=%s peer=%s rc=%d", ch, node_id_str, rc);
        send_response(cli, id, NULL, "invitation could not start"); return;
    }

    char resp[256]; snprintf(resp, sizeof(resp),
        "{\"channel_id\":\"%s\",\"ed25519\":\"%s\",\"inviting\":true}",
        ch, node_id_str);
    send_response(cli, id, resp, NULL);
}

static void hc_handle_subscribe(struct headless_client* cli, int id, const char* json) {
    int enable = json_get_int(cli, "enable", 1);
    cli->subscribed = enable;
    char resp[64]; snprintf(resp, sizeof(resp), "{\"subscribed\":%s}", enable ? "true" : "false");
    send_response(cli, id, resp, NULL);
}

static void hc_handle_quit(struct headless_client* cli, int id, const char* json) {
    (void)json;
    send_response(cli, id, "{\"bye\":true}", NULL);
    if (cli->closing) return;
    cli->draining = 1; uasync_set_socket_read(cli->hc->ua, cli->socket_id, 0);
    if (!cli->send_len) hc_close_client(cli);
}

/* ── DM (прямой чат) ── */

static void hc_handle_dm_start(struct headless_client* cli, int id, const char* json) {
    struct headless_control* hc = cli->hc;
    char key[65], ch[65]; uint8_t ed[32];
    if (json_get_str(cli, "ed25519", key, sizeof(key)) < 0) { send_response(cli, id, NULL, "missing 'ed25519' param"); return; }
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) ch[0] = '\0';
    if (!hc->inst || sc_hex_to_binary(key, ed, sizeof(ed))) {
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: dm_start rejected invalid Ed25519 or inactive core");
        send_response(cli, id, NULL, "invalid Ed25519 key"); return;
    }
    char mode_name[16] = "device";
    if (json_get_str(cli, "mode", mode_name, sizeof(mode_name)) < 0) strcpy(mode_name, "device");
    if (strcmp(mode_name, "device") && strcmp(mode_name, "groups")) {
        send_response(cli, id, NULL, "mode must be 'device' or 'groups'"); return;
    }
    enum dm_mode mode = !strcmp(mode_name, "groups") ? DM_MODE_GROUPS : DM_MODE_DEVICE;

    struct hc_query_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: DM start request allocation failed");
        send_response(cli, id, NULL, "out of memory"); return;
    }
    job->client = cli; job->id = id; job->sql.kind = HC_DM_START; cli->refs++; cli->query = job;
    dm_start_submit(hc->inst, &(struct dm_start_query){.mode=mode, .peer_ed=ed,
        .source=ch[0] ? ch : NULL, .valid=hc_query_guard}, hc_dm_start_done, job);
}

/* Отправить сообщение в DM-беседу. */
static void hc_handle_dm_send(struct headless_client* cli, int id, const char* json) {
    char conv[64], data[4096];
    if (json_get_str(cli, "conv_id", conv, sizeof(conv)) < 0) { send_response(cli, id, NULL, "missing 'conv_id' param"); return; }
    if (json_get_str(cli, "data", data, sizeof(data)) < 0) { send_response(cli, id, NULL, "missing 'data' param"); return; }
    struct hc_query_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: DM send request allocation failed");
        send_response(cli, id, NULL, "out of memory"); return;
    }
    job->client = cli; job->id = id; job->sql.kind = HC_DM_SEND; cli->refs++; cli->query = job;
    dm_send_submit(cli->hc->inst, &(struct dm_send_query){.conv=conv, .type="text", .data=(const uint8_t*)data,
        .size=(uint32_t)strlen(data), .valid=hc_query_guard}, hc_dm_send_done, job);
}

/* Ручная загрузка либо явное открытие; строковые числа сохраняют все 64 бита JSON. */
static void hc_handle_dm_file(struct headless_client* cli, int id, const char* json) {
    (void)json;
    char conv[64], action[16], author[65], seq[32], generation[32];
    uint8_t a[32];
    if (json_get_str(cli, "conv_id", conv, sizeof(conv)) < 0 || json_get_str(cli, "action", action, sizeof(action)) < 0 ||
        json_get_str(cli, "author", author, sizeof(author)) < 0 || json_get_str(cli, "seq", seq, sizeof(seq)) < 0 ||
        json_get_str(cli, "generation", generation, sizeof(generation)) < 0 ||
        (strcmp(action, "download") && strcmp(action, "opened"))) {
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless DM file: invalid arguments request=%d", id);
        send_response(cli, id, NULL, "required: conv_id, author (Ed25519 hex), seq, generation, action=download|opened"); return;
    }
    char *s_end, *g_end; errno = 0;
    uint64_t s = strtoull(seq, &s_end, 10), g = strtoull(generation, &g_end, 10);
    if (errno || !s || !g || seq[0] == '-' || generation[0] == '-' || *s_end || *g_end ||
        sc_hex_to_binary(author, a, 32) || mf_zero(a, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless DM file: rejected request=%d conv=%s action=%s", id, conv, action);
        send_response(cli, id, NULL, "invalid attachment action identity"); return;
    }
    if (db_async_can_write(cli->hc->inst->db_worker)) {
        struct dm_media_action_req* req = u_calloc(1, sizeof(*req));
        if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless DM action allocation failed"); send_response(cli, id, NULL, "out of memory"); return; }
        req->inst = cli->hc->inst; snprintf(req->conv, sizeof(req->conv), "%s", conv);
        memcpy(req->author, a, 32); req->seq = s; req->generation = g; req->opened = !strcmp(action, "opened");
        dm_media_action_trampoline(req); send_response(cli, id, "{\"accepted\":true}", NULL); return;
    }
    int rejected = !dm_history_current(cli->hc->inst, conv, g) || dm_media_user_action(cli->hc->inst, conv, a, s, !strcmp(action, "opened"));
    send_response(cli, id, rejected ? NULL : "{\"accepted\":true}", rejected ? "attachment action rejected (check identity, generation and local file)" : NULL);
}

/* ── Звонки (P2P аудио) ── */

static uint64_t hc_gen_call_id(void) {
    uint64_t id;
    if (RAND_bytes((uint8_t*)&id, sizeof(id)) != 1 || !id) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: call ID RNG failed"); return 0; }
    return id;
}

static void hc_handle_call_start(struct headless_client* cli, int id, const char* json) {
    char ch[65], ed[65], call_id_str[32];
    uint8_t peer[32];
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) { send_response(cli, id, NULL, "missing 'ch' param"); return; }
    if (json_get_str(cli, "ed25519", ed, sizeof(ed)) < 0 || sc_hex_to_binary(ed, peer, sizeof(peer)) || mf_zero(peer, sizeof(peer))) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "headless: invalid call destination channel=%s", ch);
        send_response(cli, id, NULL, "expected Ed25519 as 64 hex digits"); return;
    }
    if (!cli->hc->inst) { send_response(cli, id, NULL, "core unavailable"); return; }
    uint64_t call_id = 0;
    if (json_get_str(cli, "call_id", call_id_str, sizeof(call_id_str)) == 0) call_id = strtoull(call_id_str, NULL, 16);
    if (!call_id) call_id = hc_gen_call_id();
    if (!call_id) { send_response(cli, id, NULL, "call ID unavailable"); return; }

    if (json_get_int(cli, "devices", 0)) call_start_devices(cli->hc->inst, ch, peer, call_id);
    else call_start(cli->hc->inst, ch, peer, call_id);
    char resp[256]; snprintf(resp, sizeof(resp), "{\"call_id\":\"0x%016llx\",\"calling\":true}", (unsigned long long)call_id);
    send_response(cli, id, resp, NULL);
}

static void hc_handle_call_accept(struct headless_client* cli, int id, const char* json) {
    char call_id_str[32];
    if (json_get_str(cli, "call_id", call_id_str, sizeof(call_id_str)) < 0) { send_response(cli, id, NULL, "missing 'call_id' param"); return; }
    uint64_t call_id = strtoull(call_id_str, NULL, 16);
    call_accept(cli->hc->inst, call_id);
    send_response(cli, id, "{\"accepted\":true}", NULL);
}

static void hc_handle_call_decline(struct headless_client* cli, int id, const char* json) {
    char call_id_str[32];
    if (json_get_str(cli, "call_id", call_id_str, sizeof(call_id_str)) < 0) { send_response(cli, id, NULL, "missing 'call_id' param"); return; }
    uint64_t call_id = strtoull(call_id_str, NULL, 16);
    call_decline(cli->hc->inst, call_id);
    send_response(cli, id, "{\"declined\":true}", NULL);
}

static void hc_handle_call_hangup(struct headless_client* cli, int id, const char* json) {
    char call_id_str[32];
    if (json_get_str(cli, "call_id", call_id_str, sizeof(call_id_str)) < 0) { send_response(cli, id, NULL, "missing 'call_id' param"); return; }
    uint64_t call_id = strtoull(call_id_str, NULL, 16);
    call_hangup(cli->hc->inst, call_id);
    send_response(cli, id, "{\"hungup\":true}", NULL);
}

/* ── Рация (PTT walkie-talkie) ── */

static void hc_handle_radio_on(struct headless_client* cli, int id, const char* json) {
    char ch[65];
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) { send_response(cli, id, NULL, "missing 'ch' param"); return; }
    uint64_t group_id = topo_groups_channel_slot(cli->hc->inst->topo_groups, ch);
    struct UTUN_INSTANCE* inst = cli->hc->inst;
    struct TOPO_GROUP* group = inst && group_id ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    const uint8_t* key = group && group->group_type == TOPO_GROUP_TYPE_CHAT ? topo_group_wire_id(group) : NULL;
    if (!key) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "headless radio: unknown group ch=%s", ch);
        send_response(cli, id, NULL, "invalid ch"); return;
    }
    radio_set_active(inst, group_id, 1);
    char resp[192]; snprintf(resp, sizeof(resp), "{\"radio\":true,\"ch\":\"%s\",\"group_ed25519\":\"%s\"}",
        ch, sc_public_key_hex(key, (char[65]){0}));
    send_response(cli, id, resp, NULL);
}

static void hc_handle_radio_off(struct headless_client* cli, int id, const char* json) {
    char ch[65];
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) { send_response(cli, id, NULL, "missing 'ch' param"); return; }
    uint64_t group_id = topo_groups_channel_slot(cli->hc->inst->topo_groups, ch);
    if (!group_id || !cli->hc->inst) { send_response(cli, id, NULL, "invalid ch"); return; }
    radio_set_active(cli->hc->inst, group_id, 0);
    char resp[128]; snprintf(resp, sizeof(resp), "{\"radio\":false,\"ch\":\"%s\"}", ch);
    send_response(cli, id, resp, NULL);
}

static void hc_handle_radio_subscribers(struct headless_client* cli, int id, const char* json) {
    char ch[65];
    if (json_get_str(cli, "ch", ch, sizeof(ch)) < 0) { send_response(cli, id, NULL, "missing 'ch' param"); return; }
    uint64_t group_id = topo_groups_channel_slot(cli->hc->inst->topo_groups, ch);
    int n = radio_subscribers(cli->hc->inst, group_id);
    char resp[128]; snprintf(resp, sizeof(resp), "{\"subscribers\":%d,\"ch\":\"%s\"}", n, ch);
    send_response(cli, id, resp, NULL);
}

/* ── Command dispatch ── */

typedef void (*hc_cmd_fn)(struct headless_client* cli, int id, const char* json);

/* Опросы и реакции используют общий командный API ядра. */
static void hc_handle_feedback(struct headless_client* cli, int id, const char* json) {
    struct hc_query_job* job = hc_command_begin(cli, id, HC_FEEDBACK);
    if (job) chat_feedback_command_submit(cli->hc->inst, json, hc_feedback_done, job);
}

static void hc_handle_devgroup(struct headless_client* cli, int id, const char* cmd) {
    struct chat_devgroup_request req = {.inst = cli->hc->inst};
    if (!strcmp(cmd, "devgroup_status")) req.action = CHAT_DEVGROUP_STATUS;
    else if (!strcmp(cmd, "devgroup_leave")) req.action = CHAT_DEVGROUP_LEAVE;
    else if (!strcmp(cmd, "devgroup_rename")) {
        req.action = CHAT_DEVGROUP_RENAME;
        if (json_get_str(cli, "name", req.text, sizeof(req.text))) goto invalid;
    } else if (!strcmp(cmd, "devgroup_accept")) {
        req.action = CHAT_DEVGROUP_ACCEPT;
        if (json_get_str(cli, "invite", req.text, sizeof(req.text)) || json_get_str(cli, "pin", req.pin, sizeof(req.pin))) goto invalid;
    } else if (!strcmp(cmd, "devgroup_invite")) {
        char channel[65], peer[65]; req.action = CHAT_DEVGROUP_INVITE;
        if (json_get_str(cli, "ch", channel, sizeof(channel)) || json_get_str(cli, "peer", peer, sizeof(peer)) ||
            !chat_channel_valid(channel) || sc_hex_to_binary(peer, req.peer, 32)) goto invalid;
        strcpy(req.group, channel);
    } else goto invalid;
    struct hc_query_job* job = hc_command_begin(cli, id, HC_DEVGROUP);
    if (job) chat_devgroup_execute_submit(&req, hc_devgroup_done, job);
    OPENSSL_cleanse(&req, sizeof(req)); return;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "devgroup: invalid headless command");
    OPENSSL_cleanse(&req, sizeof(req)); send_response(cli, id, NULL, "invalid devgroup command");
}

/* Headless тоже подтверждает включение рации только после события SELECTED. */
static void hc_handle_radio_call(struct headless_client* cli, int id, const char* json) {
    (void)json;
    char text[65]; struct radio_call_request req = {.inst = cli->hc->inst};
    req.action = json_get_int(cli, "action", 0); req.devices = json_get_int(cli, "devices", 0);
    if (req.action < RADIO_CALL_START || req.action > RADIO_CALL_ANSWER || !req.inst) goto invalid;
    if (json_get_str(cli, "call_id", text, sizeof(text)) == 0) req.id = strtoull(text, NULL, 16);
    if (req.action == RADIO_CALL_START) {
        if (!req.id) req.id = hc_gen_call_id();
        if (json_get_str(cli, "ch", text, sizeof(text)) < 0 || !chat_channel_valid(text)) goto invalid;
        if (sc_hex_to_binary(text, req.group, sizeof(req.group))) goto invalid;
        if (json_get_str(cli, "ed25519", text, sizeof(text)) < 0 ||
            sc_hex_to_binary(text, req.peer, sizeof(req.peer)) || mf_zero(req.peer, sizeof(req.peer))) goto invalid;
    }
    if (!req.id) goto invalid;
    radio_call_execute(&req);
    snprintf(text, sizeof(text), "{\"call_id\":\"0x%016llx\"}", (unsigned long long)req.id);
    send_response(cli, id, text, NULL); return;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_RADIO, "headless invalid radio call action=%d", req.action);
    send_response(cli, id, NULL, "invalid radio_call parameters");
}

static const char* cmd_names[] = { "ping", "status", "send", "attach", "invite", "invite_nodes", "connect", "create_channel", "invite_to", "subscribe", "dm_start", "dm_send", "radio_call", "call_start", "call_accept", "call_decline", "call_hangup", "radio_on", "radio_off", "radio_subscribers", "server_link", "server_add", "quit", NULL };
static hc_cmd_fn cmd_handlers[] = { hc_handle_ping, hc_handle_status, hc_handle_send, hc_handle_attach, hc_handle_invite, hc_handle_invite_nodes, hc_handle_connect, hc_handle_create_channel, hc_handle_invite_to, hc_handle_subscribe, hc_handle_dm_start, hc_handle_dm_send, hc_handle_radio_call, hc_handle_call_start, hc_handle_call_accept, hc_handle_call_decline, hc_handle_call_hangup, hc_handle_radio_on, hc_handle_radio_off, hc_handle_radio_subscribers, hc_handle_server_link, hc_handle_server_add, hc_handle_quit };

static void hc_handle_command(struct headless_client* cli, const char* json) {
    if (!cli || cli->closing || !json || !cli->request) return;
    const struct hc_request* request = cli->request;
    int id = json_get_int(cli, "id", 0);
    char cmd[32];
    if (json_get_str(cli, "cmd", cmd, sizeof(cmd)) || !*cmd) { send_response(cli, id, NULL, "missing/invalid cmd"); goto done; }
    if (!strcmp(cmd, "channels") || !strcmp(cmd, "members") || !strcmp(cmd, "messages") ||
        !strcmp(cmd, "dm_list") || !strcmp(cmd, "dm_messages")) {
        hc_handle_query(cli, id, !strcmp(cmd, "channels") ? HC_CHANNELS : !strcmp(cmd, "members") ? HC_MEMBERS :
            !strcmp(cmd, "messages") ? HC_MESSAGES : !strcmp(cmd, "dm_list") ? HC_CONVERSATIONS : HC_DM_MESSAGES);
        goto done;
    }
    if (!strcmp(cmd, "kanban_board") || !strcmp(cmd, "kanban_command") || !strcmp(cmd, "kanban_history")) {
        char channel[65], entity[65], operation[12001];
        if (json_get_str(cli, "ch", channel, sizeof(channel))) { send_response(cli, id, NULL, "missing/invalid channel"); goto done; }
        if (!strcmp(cmd, "kanban_board")) strcpy(operation, "{\"action\":\"snapshot\"}");
        else if (!strcmp(cmd, "kanban_history")) {
            if (json_get_str(cli, "entity", entity, sizeof(entity)) || strlen(entity) != 64 || strspn(entity, "0123456789abcdef") != 64) {
                send_response(cli, id, NULL, "invalid kanban entity"); goto done;
            }
            snprintf(operation, sizeof(operation), "{\"action\":\"history\",\"entity\":\"%s\"}", entity);
        } else if (json_get_str(cli, "operation", operation, sizeof(operation))) { send_response(cli, id, NULL, "invalid kanban operation"); goto done; }
        struct hc_query_job* job = hc_command_begin(cli, id, HC_KANBAN);
        if (job) kanban_execute_submit(cli->hc->inst, channel, operation, hc_kanban_done, job);
        goto done;
    }
    if (!strcmp(cmd, "group_info") || !strcmp(cmd, "rename_channel")) {
        char channel[65], name[128] = {0}, description[GROUP_INFO_MAX + 1] = {0}, token[65] = {0};
        int has_name = 0, has_json = 0; char* result = NULL;
        if (json_get_str(cli, "ch", channel, sizeof(channel))) {
            send_response(cli, id, NULL, "missing/invalid channel"); goto done;
        }
        for (unsigned i = 0; i < request->count; i++) {
            const char* key = request->fields[i].key;
            if ((!strcmp(key, "name") && (++has_name, json_get_str(cli, key, name, sizeof(name)))) ||
                (!strcmp(key, "description") && (++has_json, json_get_str(cli, key, description, sizeof(description)))) ||
                (!strcmp(key, "token") && json_get_str(cli, key, token, sizeof(token)))) {
                DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: invalid group description parameter key=%s", key);
                send_response(cli, id, NULL, "invalid description parameter"); goto done;
            }
        }
        if (!strcmp(cmd, "rename_channel") && !has_name) {
            send_response(cli, id, NULL, "missing name"); goto done;
        }
        if (db_async_can_write(cli->hc->inst->db_worker)) {
            hc_group_submit(cli, id, HC_GROUP_INFO, channel, has_name ? name : NULL, has_json ? description : NULL,
                token[0] ? token : NULL, NULL, 0); goto done;
        }
        int rc = group_info_command(cli->hc->inst, channel, has_name ? name : NULL, has_json ? description : NULL,
            -1, token[0] ? token : NULL, &result);
        if (rc) {
            char error[128] = "group description unavailable";
            if (result) json_flat_get(result, "error", error, sizeof(error));
            send_response(cli, id, NULL, error);
        } else send_response(cli, id, result, NULL);
        u_free(result); goto done;
    }
    if (!strcmp(cmd, "group_policy")) {
        char channel[65], token[65], keep[RECV_BUF_SIZE]; char* result = NULL;
        if (json_get_str(cli, "ch", channel, sizeof(channel))) { send_response(cli, id, NULL, "missing channel"); goto done; }
        token[0] = 0; strcpy(keep, "[]");
        for (unsigned i = 0; i < request->count; i++) {
            const char* key = request->fields[i].key;
            if ((!strcmp(key, "token") && (json_get_str(cli, key, token, sizeof(token)) || strlen(token) != 64)) ||
                (!strcmp(key, "keep") && json_get_str(cli, key, keep, sizeof(keep))) ||
                (!strcmp(key, "all_members") && request->fields[i].kind != 2)) {
                DEBUG_WARN(DEBUG_CATEGORY_HEADLESS, "headless: invalid group policy parameter key=%s", key);
                send_response(cli, id, NULL, "invalid group policy parameter"); goto done;
            }
        }
        if (db_async_can_write(cli->hc->inst->db_worker)) {
            hc_group_submit(cli, id, HC_GROUP_POLICY, channel, NULL, NULL, token[0] ? token : NULL, keep,
                json_get_int(cli, "all_members", 0)); goto done;
        }
        group_policy_command(cli->hc->inst, channel, json_get_int(cli, "all_members", 0), token[0] ? token : NULL, keep, &result);
        send_response(cli, id, result, result ? NULL : "group policy unavailable"); u_free(result); goto done;
    }
    if (!strcmp(cmd, "poll_create") || !strcmp(cmd, "vote") || !strcmp(cmd, "react")
        || !strcmp(cmd, "feedback") || !strcmp(cmd, "reaction_catalog")) {
        hc_handle_feedback(cli, id, json); goto done;
    }
    if (!strncmp(cmd, "devgroup_", 9)) { hc_handle_devgroup(cli, id, cmd); goto done; }
    if (!strcmp(cmd, "dm_file")) { hc_handle_dm_file(cli, id, json); goto done; }
    for (int i = 0; cmd_names[i]; i++) {
        if (strcmp(cmd, cmd_names[i]) == 0) { cmd_handlers[i](cli, id, json); goto done; }
    }
    send_response(cli, id, NULL, "unknown command");
done:
    return;
}

/* ── Accept callback ── */

static void accept_callback(socket_t fd, void* arg) {
    struct headless_control* hc = (struct headless_control*)arg;
    struct sockaddr_storage addr; socklen_t alen = sizeof(addr);
    socket_t cfd = accept(fd, (struct sockaddr*)&addr, &alen);
    if (cfd == SOCKET_INVALID) { int e = socket_get_error(); if (e != ERR_AGAIN && e != ERR_WOULDBLOCK) DEBUG_ERROR((int)DEBUG_CATEGORY_HEADLESS, "headless: accept failed: %s", socket_strerror(e)); return; }

    if (socket_set_nonblocking(cfd)) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: accepted socket nonblocking failed err=%d", socket_get_error());
        socket_close_wrapper(cfd); return;
    }

    if (hc->client_count >= MAX_CLIENTS) { socket_close_wrapper(cfd); DEBUG_WARN((int)DEBUG_CATEGORY_HEADLESS, "headless: max clients reached"); return; }

    struct headless_client* cli = u_calloc(1, sizeof(*cli));
    if (!cli) { socket_close_wrapper(cfd); DEBUG_ERROR((int)DEBUG_CATEGORY_HEADLESS, "headless: failed to allocate client"); return; }
    cli->fd = cfd;
    cli->hc = hc; cli->refs = 1; hc->refs++;

    cli->socket_id = uasync_add_socket_t(hc->ua, cfd, client_read_callback, client_write_callback, NULL, "chatctl_client", cli);
    if (!cli->socket_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: failed to register client fd"); hc_close_client(cli); hc_client_release(cli); return;
    }

    cli->next = hc->clients; hc->clients = cli; hc->client_count++;
    DEBUG_INFO((int)DEBUG_CATEGORY_HEADLESS, "headless: client connected fd=%d total=%d", (int)cfd, hc->client_count);
}

/* ── Periodic cleanup ── */

static void hc_cleanup(void* arg) {
    struct headless_control* hc = (struct headless_control*)arg;
    hc->cleanup_timer = NULL;
    struct headless_client** prev = &hc->clients;
    while (*prev) {
        if ((*prev)->closing) {
            struct headless_client* dead = *prev; *prev = dead->next;
            if (dead->socket_id && hc->ua) uasync_remove_socket_t(hc->ua, dead->fd);
            if (dead->fd != SOCKET_INVALID) socket_close_wrapper(dead->fd);
            hc_client_release(dead); hc->client_count--;
        } else prev = &(*prev)->next;
    }
    hc->cleanup_timer = uasync_set_timeout(hc->ua, 5000, hc, hc_cleanup, "hc_cleanup");
    if (!hc->cleanup_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: cleanup timer allocation failed;requesting owner stop"); utun_instance_stop(hc->inst);
    }
}

/* ── Init / Destroy ── */

int chat_headless_control_init(struct UASYNC* ua, struct UTUN_INSTANCE* inst,
                                const char* bind_ip, int port) {
    if (!ua || !inst || ua != inst->ua || !chat_core_is_initialized(inst) || !bind_ip || port < 0 || port > 65535) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: invalid init arguments port=%d", port); return -1;
    }
    struct headless_control* hc = hc_of(inst);
    struct in_addr address;
    if (inet_pton(AF_INET, bind_ip, &address) != 1) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: invalid IPv4 bind=%s", bind_ip); return -1; }
    if (hc && hc->running) {
        if (hc->configured_port == port && !strcmp(hc->bind_ip, bind_ip)) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: listener already running with different binding"); return -1;
    }

    if (!hc) {
        hc = u_calloc(1, sizeof(*hc));
        if (!hc) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: context allocation failed"); return -1; }
        hc->refs = 1; inst->headless = hc;
    }
    hc->ua = ua; hc->inst = inst; hc->listen_fd = SOCKET_INVALID;
    strcpy(hc->bind_ip, bind_ip); hc->configured_port = port;

    hc->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (hc->listen_fd == SOCKET_INVALID) goto fail;

    if (socket_set_reuseaddr(hc->listen_fd, 1) || socket_set_nonblocking(hc->listen_fd)) goto fail;

    struct sockaddr_in sin; memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET; sin.sin_port = htons((uint16_t)port);
    sin.sin_addr = address;

    if (bind(hc->listen_fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        DEBUG_ERROR((int)DEBUG_CATEGORY_HEADLESS, "headless: bind(%s:%d) failed: %s", bind_ip, port, socket_strerror(socket_get_error()));
        goto fail;
    }
    if (listen(hc->listen_fd, 5) < 0) goto fail;
    uint16_t bound_port = socket_get_bound_port(hc->listen_fd);
    if (!bound_port) goto fail;

    hc->listen_sock_id = uasync_add_socket_t(ua, hc->listen_fd, accept_callback, NULL, NULL, "chatctl_listen", hc);
    if (!hc->listen_sock_id) goto fail;

    hc->previous_handler = inst->chat_event_handler;
    chat_event_set_handler(inst, hc_on_chat_event);
    hc->cleanup_timer = uasync_set_timeout(ua, 5000, hc, hc_cleanup, "hc_cleanup");
    if (!hc->cleanup_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: cleanup timer allocation failed");
        chat_headless_control_destroy(inst); return -1;
    }

    hc->running = 1;
    DEBUG_INFO((int)DEBUG_CATEGORY_HEADLESS, "headless: listening on %s:%u", bind_ip, bound_port);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: listener init failed bind=%s:%d socket_error=%d", bind_ip, port, socket_get_error());
    chat_headless_control_destroy(inst); return -1;
}

uint16_t chat_headless_control_get_port(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_HEADLESS, "headless: get port without instance"); return 0; }
    struct headless_control* hc = hc_of(inst);
    return hc && hc->running ? socket_get_bound_port(hc->listen_fd) : 0;
}

void chat_headless_control_destroy(struct UTUN_INSTANCE* inst) {
    struct headless_control* hc = hc_of(inst);
    if (!hc) return;
    hc->running = 0;
    if (hc->cleanup_timer) uasync_cancel_timeout(hc->ua, hc->cleanup_timer);
    if (inst->chat_event_handler == hc_on_chat_event) chat_event_set_handler(inst, hc->previous_handler);
    if (hc->listen_sock_id && hc->ua) uasync_remove_socket_t(hc->ua, hc->listen_fd);
    if (hc->listen_fd != SOCKET_INVALID) { socket_close_wrapper(hc->listen_fd); hc->listen_fd = SOCKET_INVALID; }
    struct headless_client* c = hc->clients;
    while (c) { struct headless_client* n = c->next; hc_close_client(c); hc_client_release(c); c = n; }
    hc->clients = NULL; hc->client_count = 0;
    inst->headless = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_HEADLESS, "headless: listener stopped, retained clients=%u", hc->refs - 1); hc_release(hc);
    DEBUG_INFO((int)DEBUG_CATEGORY_HEADLESS, "headless: destroyed");
}
