/* Реальные A–S–B/C соединения: S хранит только ciphertext. Запросы переживают
 * офлайн автора/restart; B получает ключ после решения, C получает отказ. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <openssl/sha.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/utun_instance.h"
#include "../src/ntp_time.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_attachment.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/member_sync.h"
#include "../src/chat/db_sync.h"
#include "../src/chat/chat_setting.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/routing_layer/etcp_router.h"
#include "../src/transport_layer/secure_channel.h"
#include "../src/transport_layer/etcp.h"
#include "../src/media_async/media_async.h"
#include "../src/media_async/attachment_send.h"
#include "../src/media_delivery/media_download.h"
#include "test_utils.h"
#include "test_clock.h"
#include "test_net.h"
#include "test_membership.h"
#define CHANNEL "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951" /* Ed25519 для seed {94}. */
#define FILE_BYTES (3 * 32768 + 17)
enum { A, B, S, M, NODES }; /* M — второй запрашивающий клиент; S — storage+supernode. */
struct media_test {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* nodes[NODES];
    struct SC_MYKEYS keys[NODES];
    uint8_t seed[NODES][32];
    uint64_t ids[NODES];
    uint16_t ports[NODES];
    uint8_t ed[NODES][32], ch_x[32], ch_ed[32], ch_priv[32], ch_sig[64], message[32];
    char dir[512], file[768];
    unsigned phase, ticks, heartbeats, hold, forced_cleanup;
    unsigned grant_bind_faults, grant_rollbacks, result_acks; /* Linux: отказ записи GRANT и настоящий повтор до расшифровки. */
    unsigned decision_failures, decision_events; /* Linux: два отказа автора, restart и одно событие после commit. */
    unsigned request_rejections, request_restarts, request_sends, request_acks, request_events;
    unsigned observation_faults, observation_events, observation_recovered;
    int request_external; /* Linux: транзакция фикстуры вокруг первого настоящего REQUEST от B. */
    int grant_state, grant_outbox; /* Снимок получателя до единственного отказа bind. */
    int failed;
    void* timer;
};
static int value(struct UTUN_INSTANCE* inst, const char* sql) {
    if (!inst) return -1;
    sqlite3_stmt* st = NULL; int result = -1;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        result = sqlite3_column_int(st, 0);
    sqlite3_finalize(st); return result;
}
static int live(struct UTUN_INSTANCE* inst, uint64_t peer) {
    return inst && etcp_router_get_path(inst, topo_groups_channel_slot(inst->topo_groups, CHANNEL), peer, ETCP_RT_ID_ATTACHMENT_KEYS).next_hop_node_id != 0;
}
static void failure(struct media_test* t, const char* reason) {
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted attachment test failed phase=%u reason=%s", t->phase, reason);
    t->failed = 1; uasync_stop(t->ua);
}
#ifdef __linux__
static struct media_test* active_test;
static timeout_callback_t backfill_cb;
static etcp_recv_fn attachment_receive;
static void request_restart(void* arg);

/* История уже committed даже при отказе локального индекса; сообщение не должно пропасть из событий UI. */
static void observation_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    struct media_test* t = active_test;
    if (type != CHAT_EVT_MSG_RECEIVED || !t || inst != t->nodes[B] ||
        bytes < 2 + CHAT_MSG_EVENT_SUFFIX_SIZE || bytes != 1 + data[0] + CHAT_MSG_EVENT_SUFFIX_SIZE) return;
    if (memcmp(data + 1 + data[0], t->nodes[A]->my_ed25519_pubkey, 32)) return;
    sqlite3* reader = NULL; sqlite3_stmt* st = NULL;
    int rc = sqlite3_open_v2(sqlite3_db_filename(inst->topo_sqlite_db, "main"), &reader, SQLITE_OPEN_READONLY, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(reader, "SELECT (SELECT count(*) FROM msg_" CHANNEL
        " WHERE json_extract(data,'$.ct')='" ENCRYPTED_ATTACHMENT_TYPE "'),(SELECT count(*) FROM attachment_keys)", -1, &st, NULL);
    int saved = rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1 && sqlite3_column_int(st, 1) == 0;
    if (sqlite3_finalize(st) != SQLITE_OK) saved = 0;
    if (sqlite3_close(reader) != SQLITE_OK) saved = 0;
    if (!saved || t->observation_faults != 1 || !sqlite3_get_autocommit(inst->topo_sqlite_db) ||
        !CC(inst)->journal_effect_timer)
        failure(t, "committed message notification lost or index retry missing after SQL failure");
    t->observation_events++;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS observation failure: reader history=1 index=0 before UI event, bounded backfill queued");
}

static int author_request_count(struct media_test* t) {
    char query[256];
    snprintf(query, sizeof(query), "SELECT count(*) FROM attachment_key_requests WHERE channel='" CHANNEL
        "' AND peer=x'%s' AND incoming=1", sc_public_key_hex(t->nodes[B]->my_ed25519_pubkey, (char[65]){0}));
    return value(t->nodes[A], query);
}

static void rejected_request_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    (void)inst; (void)data; (void)bytes;
    if (type == CHAT_EVT_ATTACHMENT_KEYS_CHANGED) {
        active_test->request_events++; failure(active_test, "REQUEST event inside external transaction");
    }
}

/* Router уже проверил реальный E2E пакет. Внешняя транзакция открывается до входа в обработчик сервиса. */
static void receive_with_transaction(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct media_test* t = active_test;
    if (!t || conn->instance != t->nodes[A] || t->request_rejections || entry->len < ROUTER_SVC_PAYLOAD_OFF + 85 ||
        memcmp(entry->dgram + ROUTER_SVC_SRC_ED_OFF, t->nodes[B]->my_ed25519_pubkey, 32) ||
        entry->len != ROUTER_SVC_PAYLOAD_OFF + 85 || entry->dgram[ROUTER_SVC_PAYLOAD_OFF + 4] != 1) {
        attachment_receive(conn, entry); return;
    }
    struct UTUN_INSTANCE* a = t->nodes[A]; sqlite3* db = a->topo_sqlite_db;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { failure(t, "cannot open REQUEST outer transaction"); attachment_receive(conn, entry); return; }
    rc = sqlite3_exec(db, "UPDATE attachment_keys SET error=7 WHERE channel='" CHANNEL "'", NULL, NULL, NULL);
    chat_event_handler_fn previous = a->chat_event_handler;
    chat_event_set_handler(a, rejected_request_event); t->request_external = 1; t->request_rejections++;
    attachment_receive(conn, entry); /* Освобождает entry; фикстура больше не обращается к нему. */
    int preserved = rc == SQLITE_OK && !t->failed && !sqlite3_get_autocommit(db) && !author_request_count(t) &&
        value(a, "SELECT error FROM attachment_keys") == 7 && !t->request_acks && !t->request_events;
    rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    t->request_external = 0; chat_event_set_handler(a, previous);
    if (!preserved || rc != SQLITE_OK || !sqlite3_get_autocommit(db)) { failure(t, "REQUEST changed outer transaction or sent ACK"); return; }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS REQUEST rejection: external marker preserved, no author row/ACK/event, owner rollback completed");
    if (!uasync_set_timeout(t->ua, 100, t, request_restart, "restart after rejected REQUEST"))
        failure(t, "cannot schedule REQUEST author restart");
}

extern int __real_etcp_router_bind(struct UTUN_INSTANCE*, uint8_t, etcp_recv_fn);
int __wrap_etcp_router_bind(struct UTUN_INSTANCE* inst, uint8_t service, etcp_recv_fn callback) {
    if (service == ETCP_RT_ID_ATTACHMENT_KEYS) { attachment_receive = callback; callback = receive_with_transaction; }
    return __real_etcp_router_bind(inst, service, callback);
}

/* Авторское событие должно видеть уже сохранённые решение и GRANT через независимый reader. */
static void decision_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    if (type != CHAT_EVT_ATTACHMENT_KEYS_CHANGED) return;
    struct media_test* t = active_test;
    if (!t || inst != t->nodes[A] || t->decision_failures != 2 || bytes != (int)strlen(CHANNEL) + 1 ||
        data[0] != strlen(CHANNEL) || !sqlite3_get_autocommit(inst->topo_sqlite_db)) {
        if (t) failure(t, "author event before committed decision"); return;
    }
    sqlite3* reader = NULL; sqlite3_stmt* st = NULL;
    int rc = sqlite3_open_v2(sqlite3_db_filename(inst->topo_sqlite_db, "main"), &reader, SQLITE_OPEN_READONLY, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(reader, "SELECT r.state,o.command FROM attachment_key_requests r"
        " JOIN attachment_key_outbox o ON o.channel=r.channel AND o.message_id=r.message_id AND o.peer=r.peer"
        " WHERE r.channel=? AND r.message_id=? AND r.peer=? AND r.incoming=1", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, CHANNEL, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, t->message, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, t->nodes[B]->my_ed25519_pubkey, 32, SQLITE_STATIC);
    int saved = rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 2 && sqlite3_column_int(st, 1) == 3;
    if (sqlite3_finalize(st) != SQLITE_OK) saved = 0;
    if (sqlite3_close(reader) != SQLITE_OK) saved = 0;
    if (!saved) failure(t, "author event without committed state and outbox");
    t->decision_events++;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS author decision: independent reader state=2 GRANT=1 before UI event");
}

/* Два доказанных отказа сохраняют ожидающий запрос; повтор выполняется после настоящего restart автора. */
static int failed_decisions(struct media_test* t) {
    struct UTUN_INSTANCE* a = t->nodes[A]; sqlite3* db = a->topo_sqlite_db;
    chat_event_set_handler(a, decision_event);
    int rc = sqlite3_exec(db, "CREATE TEMP TRIGGER reject_author_queue BEFORE INSERT ON attachment_key_outbox"
        " WHEN NEW.command=3 BEGIN SELECT RAISE(IGNORE); END", NULL, NULL, NULL);
    int ignored = rc == SQLITE_OK && chat_attachment_decide(a, CHANNEL, t->message, t->nodes[B]->my_ed25519_pubkey, 1) == -1;
    rc = sqlite3_exec(db, "DROP TRIGGER IF EXISTS reject_author_queue", NULL, NULL, NULL);
    if (!ignored || rc != SQLITE_OK || t->failed || !sqlite3_get_autocommit(db)) goto fail;
    t->decision_failures++;
    rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    int outer = rc == SQLITE_OK && chat_attachment_decide(a, CHANNEL, t->message, t->nodes[B]->my_ed25519_pubkey, 1) == -1 && !sqlite3_get_autocommit(db);
    rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (!outer || rc != SQLITE_OK || t->failed || !sqlite3_get_autocommit(db)) goto fail;
    t->decision_failures++;
    if (value(a, "SELECT count(*) FROM attachment_key_requests WHERE incoming=1 AND state=0") != 2 ||
        value(a, "SELECT count(*) FROM attachment_key_outbox") != 0 || t->decision_events) goto fail;
    chat_event_set_handler(a, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS author rollback: ignored outbox and external transaction, two pending requests, no queue/event");
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "author decision fixture failed rc=%d: %s", rc, sqlite3_errmsg(db));
    chat_event_set_handler(a, NULL); return -1;
}

/* Проверить полный откат до очередного повтора автора; ACK получателя ещё не отправлялся. */
static void verify_grant_rollback(void* arg) {
    struct media_test* t = arg;
    struct UTUN_INSTANCE* b = t->nodes[B];
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM attachment_key_outbox WHERE command=3 AND peer=x'%s'",
        sc_public_key_hex(t->nodes[B]->my_ed25519_pubkey, (char[65]){0}));
    if (!b || !sqlite3_get_autocommit(b->topo_sqlite_db) || value(b, "SELECT coalesce(length(key),0) FROM attachment_keys") != 0 ||
        value(b, "SELECT state FROM attachment_key_requests WHERE incoming=0") != t->grant_state ||
        value(b, "SELECT count(*) FROM attachment_key_outbox") != t->grant_outbox || t->result_acks || value(t->nodes[A], sql) != 1) {
        failure(t, "GRANT SQL fault changed committed result or acknowledged before retry"); return;
    }
    t->grant_rollbacks++;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS grant rollback: key absent state=%d request_outbox=%d ACK=0 author retains GRANT for retry",
        t->grant_state, t->grant_outbox);
}

extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* data, int bytes, void (*destroy)(void*)) {
    struct media_test* t = active_test;
    const char* observation = "INSERT INTO attachment_keys(channel,crypto_id,message_id,author";
    if (t && !t->observation_faults && t->nodes[B] && index == 5 && sqlite3_db_handle(st) == t->nodes[B]->topo_sqlite_db &&
        !strncmp(sqlite3_sql(st), observation, strlen(observation))) {
        t->observation_faults++;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "encrypted attachment fixture: one observation hash bind NOMEM bytes=%d", bytes);
        return SQLITE_NOMEM;
    }
    if (t && !t->grant_bind_faults && t->nodes[B] && index == 1 && sqlite3_db_handle(st) == t->nodes[B]->topo_sqlite_db &&
        strstr(sqlite3_sql(st), "UPDATE attachment_keys SET key=?,error=0 WHERE channel=? AND message_id=? AND author=?")) {
        t->grant_bind_faults++;
        t->grant_state = value(t->nodes[B], "SELECT state FROM attachment_key_requests WHERE incoming=0");
        t->grant_outbox = value(t->nodes[B], "SELECT count(*) FROM attachment_key_outbox");
        if (!uasync_set_timeout(t->ua, 100, t, verify_grant_rollback, "verify grant rollback"))
            failure(t, "cannot schedule GRANT rollback check");
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "encrypted attachment fixture: one GRANT key bind failure bytes=%d rc=SQLITE_NOMEM", bytes);
        return SQLITE_NOMEM;
    }
    return __real_sqlite3_bind_blob(st, index, data, bytes, destroy);
}

extern int __real_etcp_router_conn_send(struct ETCP_ROUTER_CONN*, const uint8_t*, size_t, int);
int __wrap_etcp_router_conn_send(struct ETCP_ROUTER_CONN* conn, const uint8_t* data, size_t length, int mode) {
    struct media_test* t = active_test;
    if (t && conn->svc_id == ETCP_RT_ID_ATTACHMENT_KEYS && length == 85 && !memcmp(data, "AK01", 4)) {
        if (conn->inst == t->nodes[B] && conn->remote_node_id == t->ids[A] && data[4] == 1) {
            t->request_sends++;
            if (!t->observation_recovered) {
                sqlite3* reader = NULL; sqlite3_stmt* st = NULL;
                int rc = sqlite3_open_v2(sqlite3_db_filename(conn->inst->topo_sqlite_db, "main"), &reader, SQLITE_OPEN_READONLY, NULL);
                if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(reader, "SELECT crypto_id,cipher_hash FROM attachment_keys"
                    " WHERE channel='" CHANNEL "' AND message_id=? AND key IS NULL AND downloaded=1 AND wanted=1", -1, &st, NULL);
                if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, data + 5, 32, SQLITE_STATIC);
                int saved = rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 16 &&
                    sqlite3_column_bytes(st, 1) == 32 && !memcmp(sqlite3_column_blob(st, 0), data + 37, 16) &&
                    !memcmp(sqlite3_column_blob(st, 1), data + 53, 32);
                if (sqlite3_finalize(st) != SQLITE_OK) saved = 0;
                if (sqlite3_close(reader) != SQLITE_OK) saved = 0;
                if (!saved || t->observation_faults != 1 || t->observation_events != 1 || !sqlite3_get_autocommit(conn->inst->topo_sqlite_db)) {
                    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "REQUEST observation saved=%d faults=%u events=%u autocommit=%d",
                        saved, t->observation_faults, t->observation_events, sqlite3_get_autocommit(conn->inst->topo_sqlite_db));
                    failure(t, "REQUEST without recovered committed manifest index after SQL failure");
                }
                t->observation_recovered++;
                DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS observation recovery: independent reader identity/hash/downloaded/wanted committed before real REQUEST");
            }
        }
        if (conn->inst == t->nodes[A] && conn->remote_node_id == t->ids[B] && data[4] == 2) {
            sqlite3* reader = NULL; sqlite3_stmt* st = NULL;
            int rc = sqlite3_open_v2(sqlite3_db_filename(conn->inst->topo_sqlite_db, "main"), &reader, SQLITE_OPEN_READONLY, NULL);
            if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(reader, "SELECT state FROM attachment_key_requests"
                " WHERE channel=? AND message_id=? AND peer=? AND incoming=1", -1, &st, NULL);
            if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, CHANNEL, -1, SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, data + 5, 32, SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, t->nodes[B]->my_ed25519_pubkey, 32, SQLITE_STATIC);
            int saved = rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0;
            if (sqlite3_finalize(st) != SQLITE_OK) saved = 0;
            if (sqlite3_close(reader) != SQLITE_OK) saved = 0;
            if (t->request_external || !saved || !sqlite3_get_autocommit(conn->inst->topo_sqlite_db))
                failure(t, "REQUEST_ACK before committed request");
            t->request_acks++;
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS REQUEST_ACK: readonly pending request visible before real ACK, sends=%u", t->request_sends);
        }
    }
    if (t && t->nodes[B] && conn->inst == t->nodes[B] && conn->svc_id == ETCP_RT_ID_ATTACHMENT_KEYS &&
        length == 85 && !memcmp(data, "AK01", 4) && data[4] == 5) {
        sqlite3* reader = NULL; sqlite3_stmt* st = NULL;
        int rc = sqlite3_open_v2(sqlite3_db_filename(conn->inst->topo_sqlite_db, "main"), &reader, SQLITE_OPEN_READONLY, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(reader, "SELECT length(key) FROM attachment_keys WHERE message_id=?", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, data + 5, 32, SQLITE_STATIC);
        int ready = rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 32;
        if (sqlite3_finalize(st) != SQLITE_OK) ready = 0;
        if (sqlite3_close(reader) != SQLITE_OK) ready = 0;
        if (!ready || !sqlite3_get_autocommit(conn->inst->topo_sqlite_db)) failure(t, "RESULT_ACK before independent reader sees key");
        t->result_acks++;
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "encrypted attachment fixture: real RESULT_ACK after committed key readable=%d", ready);
    }
    return __real_etcp_router_conn_send(conn, data, length, mode);
}

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int delay, void* arg, timeout_callback_t cb, const char* name) {
    if (name && !strcmp(name, "chat_media_bl")) backfill_cb = cb;
    return __real_uasync_set_timeout(ua, delay, arg, cb, name);
}
void __real_media_download_handle_chunk(struct UTUN_INSTANCE*, uint64_t, const uint8_t*, const uint8_t*, size_t);
void __wrap_media_download_handle_chunk(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t from[32], const uint8_t* data, size_t len) {
    __real_media_download_handle_chunk(inst, group, from, data, len);
    struct media_test* t = active_test;
    if (!t || t->forced_cleanup || inst != t->nodes[S] || !inst->md.downloads || !inst->md.downloads->head) return;
    struct media_download* dl = (struct media_download*)inst->md.downloads->head->data;
    struct media_download_block* block = &dl->blocks[0];
    if (!block->source || !block->bytes_received || block->bytes_received >= block->expected_size) return;
    if (!backfill_cb) { failure(t, "startup cleanup callback not captured"); return; }
    char temporary[FILE_IO_PATH_MAX], orphan[FILE_IO_PATH_MAX];
    snprintf(temporary, sizeof(temporary), "%s", block->source->storage.temporary);
    int length = snprintf(orphan, sizeof(orphan), "%s.unowned", temporary);
    if (length < 0 || length >= (int)sizeof(orphan)) { failure(t, "orphan fixture path"); return; }
    FILE* file = fopen(orphan, "wb");
    if (!file) { failure(t, "orphan fixture creation"); return; }
    int error = fputs("stale", file) < 0;
    if (fclose(file)) error = 1;
    if (error) { failure(t, "orphan fixture write/close"); return; }
    uint32_t prefix = block->bytes_received;
    t->forced_cleanup++;
    struct chat_core_ctx* cc = CC(inst);
    if (cc->media_backfill_timer) uasync_cancel_timeout(inst->ua, cc->media_backfill_timer);
    backfill_cb(inst); /* Реальный startup callback между двумя настоящими CHUNK. */
    if (ma_file_size(temporary) != prefix || access(orphan, F_OK) == 0) {
        failure(t, "cleanup removed active block or retained orphan"); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "encrypted fixture: cleanup kept active block prefix=%u and removed stale sibling", prefix);
}
#endif
static void hex(const uint8_t* p, size_t len, char* out) {
    for (size_t i = 0; i < len; i++) snprintf(out + i * 2, 3, "%02x", p[i]);
}

/* Startup/restart удаляет только строгие собственные stale temp; живые owners обоих деревьев сохраняются. */
static int start_with_recovery(struct UTUN_INSTANCE* inst, const char* base) {
    struct file_io_temp live[2] = {0};
    char target[2][1024], stale[2][1100], foreign[2][1100];
    const char* trees[] = {"media", "decrypted"};
    const char* extensions[] = {"uenc", "file"};
    int result = -1;
    for (unsigned i = 0; i < 2; i++) {
        char directory[900]; snprintf(directory, sizeof(directory), "%s/%s", base, trees[i]);
        if (utun_mkdir(directory, 0700) && errno != EEXIST) goto done;
        snprintf(directory, sizeof(directory), "%s/%s/%s", base, trees[i], CHANNEL);
        if (utun_mkdir(directory, 0700) && errno != EEXIST) goto done;
        snprintf(target[i], sizeof(target[i]), "%s/00000000000000000000000000000001.%s", directory, extensions[i]);
        snprintf(stale[i], sizeof(stale[i]), "%s.tmp.00000000000000000000000000000002", target[i]);
        snprintf(foreign[i], sizeof(foreign[i]), "%s.tmp.foreign", target[i]);
        const char* paths[] = {target[i], stale[i], foreign[i]};
        for (unsigned j = 0; j < 3; j++) {
            FILE* file = fopen(paths[j], "wb");
            if (!file) goto done;
            int failed = fputs("retained", file) < 0;
            if (fclose(file)) failed = 1;
            if (failed) goto done;
        }
        if (file_io_temp_open(&live[i], target[i], FILE_IO_DURABLE) || fputs("live", live[i].file) < 0) goto done;
        if (i ? file_io_temp_finish(&live[i]) : fflush(live[i].file)) goto done;
    }
    if (utun_instance_init(inst)) goto done;
    for (unsigned i = 0; i < 2; i++) {
        if (ma_file_size(stale[i]) >= 0 || ma_file_size(target[i]) != 8 || ma_file_size(foreign[i]) != 8 ||
            ma_file_size(live[i].temporary) != 4 || file_io_remove_orphan(live[i].temporary) != 0) goto done;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment restart recovery: both trees, open/finished owners, final and foreign files");
    result = 0;
done:
    for (unsigned i = 0; i < 2; i++) file_io_temp_discard(&live[i]);
    if (result) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery fixture failed base=%s errno=%d", base, errno);
    return result;
}

static int start_node(struct media_test* t, int index) {
    char path[768], pub[65], priv[65], server[65], db[768];
    hex(t->keys[index].public_key, 32, pub); hex(t->seed[index], 32, priv);
    snprintf(db, sizeof(db), "%s/db%d", t->dir, index); utun_mkdir(db, 0700);
    snprintf(path, sizeof(path), "%s/%d.conf", t->dir, index);
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "[global]\nmy_private_key=%s\nmy_public_key=%s\ndb_path=%s\n"
               "[server:s1]\naddr=127.0.0.1:%d\ntype=public\n[allowed_keys]\nallow_all=1\n"
               "[chatserver]\nstorage_autoload=0\n", priv, pub, db, t->ports[index]);
    if (index != S) {
        hex(t->keys[S].public_key, 32, server);
        fprintf(f, "[client:to_s]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", server);
    }
    fclose(f);
    t->nodes[index] = utun_instance_create(t->ua, path);
    if (t->nodes[index]) {
        t->ports[index] = ss_get_port(&test_etcp_bound_socket(t->nodes[index], "s1")->local_addr);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "encrypted attachment fixture: node=%d port=%u retained for restart", index, t->ports[index]);
    }
    if (t->nodes[index] && index != S) test_etcp_set_peer(t->nodes[index], "to_s", t->nodes[S], "s1");
    int rc = t->nodes[index] ? start_with_recovery(t->nodes[index], db) : -1;
#ifdef __linux__
    if (!rc && index == B) chat_event_set_handler(t->nodes[index], observation_event);
#endif
    return rc;
}

#ifdef __linux__
/* Отказ не удалил исходящую очередь B; автор перезапускается вне своего callback. */
static void request_restart(void* arg) {
    struct media_test* t = arg;
    if (author_request_count(t) || value(t->nodes[B], "SELECT state FROM attachment_key_requests WHERE incoming=0") != 0 ||
        value(t->nodes[B], "SELECT count(*) FROM attachment_key_outbox WHERE command=1") != 1 || t->request_acks || t->request_events) {
        failure(t, "rejected REQUEST lost recipient retry"); return;
    }
    utun_instance_destroy(t->nodes[A]); t->nodes[A] = NULL;
    if (start_node(t, A)) { failure(t, "author restart after rejected REQUEST"); return; }
    t->request_restarts++;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS REQUEST retry retained: B state=0 REQUEST=1, author restarted; waiting for real resend");
}
#endif

/* Фикстура создаёт канал напрямую, поэтому сама снимает gate после реального Merkle round. */
static void members_ready(const uint8_t peer[32], const char* channel, int result, void* arg) {
    struct UTUN_INSTANCE* inst = arg;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "encrypted fixture: members verified local=%llu peer=%s channel=%s result=%d",
               (unsigned long long)inst->node_id, sc_public_key_hex(peer, (char[65]){0}), channel, result);
    struct DB_SYNC_INSTANCE* sync = db_sync_instance_find(inst, topo_groups_channel_slot(inst->topo_groups, CHANNEL));
    if (result == MT_OK && sync) db_sync_instance_set_gated(sync, 0);
}

static int setup(struct media_test* t) {
    const uint64_t ts = ntp_time_get_seconds(t->nodes[A]);
    for (int i = 0; i < NODES; i++) {
        struct UTUN_INSTANCE* inst = t->nodes[i];
        if (topo_node_sqlite_channel_put(inst->topo_sqlite_db, CHANNEL, "encrypted-attachment-test", t->nodes[A]->my_ed25519_pubkey,
            t->ch_x, NULL, t->ch_ed, i == A ? t->ch_priv : NULL, t->ch_sig)) return -1;
        struct group_info_record description;
        if (group_info_get(inst->topo_sqlite_db, CHANNEL, &description) ||
            group_info_verify(inst->topo_sqlite_db, CHANNEL, &description)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted attachment fixture: invalid signed group description node=%d", i);
            return -1;
        }
        chat_core_ensure_channel_ready(inst, CHANNEL);
        for (int j = 0; j < NODES; j++) {
            uint8_t msg[512], join[64], adm_sig[64], certificate[64], update[64];
            int n = member_sync_build_join_msg(t->ch_x, t->ch_ed, t->nodes[j]->my_ed25519_pubkey, t->keys[j].public_key, msg, sizeof(msg));
            if (n < 0 || sc_ed25519_sign(t->nodes[j]->my_ed25519_privkey, msg, (size_t)n, join) != SC_OK) return -1;
            const char* tags = j == S ? "{\"supernode\":\"yes\",\"storage\":\"yes\",\"ver\":\"1\"}" : "{\"ver\":\"1\"}";
            n = member_sync_build_owner_msg(CHANNEL, t->ed[j], tags, msg, sizeof(msg));
            if (n < 0 || sc_ed25519_sign(t->ch_priv, msg, (size_t)n, adm_sig) != SC_OK) return -1;
            char info[64]; snprintf(info, sizeof(info), "{\"name\":\"node%d\"}", j);
            if (member_sync_sign_join(t->ch_priv, CHANNEL, NULL, t->keys[j].public_key, t->ed[j],
                TEST_JOIN_ID(t->ch_x, t->ch_ed, t->nodes[j]->my_ed25519_pubkey, t->keys[j].public_key), ts, certificate)) return -1;
            if (test_member_profile(t->nodes[j]->my_ed25519_privkey,
                TEST_JOIN_ID(t->ch_x, t->ch_ed, t->nodes[j]->my_ed25519_pubkey, t->keys[j].public_key), ts, info, update)) return -1;
            if (member_sync_put(inst, CHANNEL, t->keys[j].public_key, t->ed[j], join, ts,
                                update, ts, info, tags, adm_sig, j == S, 0, certificate) < 0) return -1;
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "encrypted attachment fixture: signed snapshot verified nodes=%d members=%d version=%llu",
        NODES, NODES, (unsigned long long)ts);
    for (int i = 0; i < NODES; i++) {
        if (i == S) continue;
        if (member_sync_start(t->nodes[i], instance_find_conn(t->nodes[i], t->ids[S]), CHANNEL, members_ready, t->nodes[i]) < 0 ||
            member_sync_start(t->nodes[S], instance_find_conn(t->nodes[S], t->ids[i]), CHANNEL, members_ready, t->nodes[S]) < 0) return -1;
    }
    return 0;
}


/* Ключи и метаданные до подтверждения отсутствуют даже у скачавшего storage. */
static int snapshot(struct media_test* t) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(t->nodes[A]->topo_sqlite_db, "SELECT message_id FROM attachment_keys WHERE message_id IS NOT NULL", -1, &st, NULL)) return -1;
    int ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32;
    if (ok) memcpy(t->message, sqlite3_column_blob(st, 0), 32);
    sqlite3_finalize(st); return ok ? 0 : -1;
}
static int readable(struct media_test* t, int index) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(t->nodes[index]->topo_sqlite_db, "SELECT plain_path FROM attachment_keys WHERE ready=1 AND length(key)=32", -1, &st, NULL)) return 0;
    char path[1024] = "";
    if (sqlite3_step(st) == SQLITE_ROW) snprintf(path, sizeof(path), "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    FILE* f = path[0] ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    int ok = 1;
    for (unsigned i = 0; i < FILE_BYTES; i++) if (fgetc(f) != (int)((i * 73 + 19) & 255)) { ok = 0; break; }
    if (fgetc(f) != EOF) ok = 0;
    fclose(f); return ok;
}
static void download(struct media_test* t, int index) {
    int id = value(t->nodes[index], "SELECT id FROM msg_" CHANNEL " ORDER BY id LIMIT 1");
    if (id <= 0) { failure(t, "history not available before download"); return; }
    chat_core_attachment_download(t->nodes[index], CHANNEL, id);
}
/* Вход ПОСЛЕ router позволяет отдельно проверить требования флагов и точной идентичности. */
static void inject(struct media_test* t, int source, int target, unsigned flags, int corrupt) {
    struct UTUN_INSTANCE* inst = t->nodes[target];
    struct ETCP_CONN* conn = instance_find_conn(inst, t->ids[S]);
    sqlite3_stmt* st = NULL;
    if (!conn || sqlite3_prepare_v2(t->nodes[A]->topo_sqlite_db,
        "SELECT crypto_id,cipher_hash,key FROM attachment_keys", -1, &st, NULL) || sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st); failure(t, "control fixture"); return;
    }
    struct ll_entry* e = ll_alloc_lldgram(ROUTER_SVC_PAYLOAD_OFF + 117);
    if (!e) { sqlite3_finalize(st); failure(t, "control allocation"); return; }
    memset(e->dgram, 0, ROUTER_SVC_PAYLOAD_OFF + 117); e->len = ROUTER_SVC_PAYLOAD_OFF + 117;
    e->dgram[0] = ETCP_RT_ID_ATTACHMENT_KEYS;
    memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, t->nodes[source]->my_ed25519_pubkey, 32);
    memcpy(e->dgram + ROUTER_SVC_DST_ED_OFF, inst->my_ed25519_pubkey, 32);
    uint64_t group = topo_groups_channel_slot(inst->topo_groups, CHANNEL); memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &group, 8); e->dgram[ROUTER_SVC_FLAGS_OFF] = flags;
    uint8_t* b = e->dgram + ROUTER_SVC_PAYLOAD_OFF;
    memcpy(b, "AK01", 4); b[4] = 3; memcpy(b + 5, t->message, 32);
    memcpy(b + 37, sqlite3_column_blob(st, 0), 16); memcpy(b + 53, sqlite3_column_blob(st, 1), 32);
    memcpy(b + 85, sqlite3_column_blob(st, 2), 32); sqlite3_finalize(st);
    if (corrupt) b[53] ^= 1;
    inst->router_bindings.callbacks[ETCP_RT_ID_ATTACHMENT_KEYS](conn, e);
}
static void tick(void* arg) {
    struct media_test* t = arg; t->ticks++; t->heartbeats++;
    struct UTUN_INSTANCE* a = t->nodes[A];
    struct UTUN_INSTANCE* b = t->nodes[B];
    struct UTUN_INSTANCE* s = t->nodes[S];
    struct UTUN_INSTANCE* c = t->nodes[M];
    if (t->ticks % 200 == 0) DEBUG_INFO(DEBUG_CATEGORY_MEDIA,
        "encrypted attachment test waiting phase=%u ticks=%u messages=A:%d B:%d S:%d C:%d", t->phase, t->ticks,
        value(a, "SELECT count(*) FROM msg_" CHANNEL), value(b, "SELECT count(*) FROM msg_" CHANNEL),
        value(s, "SELECT count(*) FROM msg_" CHANNEL), value(c, "SELECT count(*) FROM msg_" CHANNEL));
    switch (t->phase) {
    case 0:
        if (live(a, t->ids[S]) && live(b, t->ids[S]) && live(c, t->ids[S])) {
            if (setup(t)) { failure(t, "signed group members"); return; } t->phase++;
        }
        /* UTUN routes precede CHAT routes; setup once physical links are UP. */
        else if (t->ticks >= 20 && instance_find_conn(a, t->ids[S]) && instance_find_conn(b, t->ids[S]) && instance_find_conn(c, t->ids[S])) {
            if (setup(t)) { failure(t, "signed group members"); return; } t->phase++;
        }
        break;
    case 1:
#ifdef __linux__
        if (!backfill_cb) break;
#endif
        if (live(a, t->ids[B]) && live(a, t->ids[M])) {
            if (chat_setting_set(s, "storage_autoload", "1")) { failure(t, "storage autoload setting"); return; }
            struct attachment_send_req* req = u_calloc(1, sizeof(*req));
            if (!req) { failure(t, "send allocation"); return; }
            req->inst = a; req->encrypted = 1; req->info.kind = ATTACHMENT_FILE;
            snprintf(req->target, sizeof(req->target), "%s", CHANNEL);
            snprintf(req->source, sizeof(req->source), "%s", t->file);
            snprintf(req->info.name, sizeof(req->info.name), "private-name.bin");
            attachment_send_trampoline(req); t->phase++;
        }
        break;
    case 2:
        if (value(s, "SELECT count(*) FROM msg_" CHANNEL) == 1 && value(b, "SELECT count(*) FROM msg_" CHANNEL) == 1 &&
            value(c, "SELECT count(*) FROM msg_" CHANNEL) == 1 && !snapshot(t)) {
            if (value(b, "SELECT count(*) FROM msg_" CHANNEL " WHERE instr(CAST(data AS TEXT),'private-name.bin')>0") != 0) {
                failure(t, "plaintext filename leaked into group history"); return;
            }
            t->phase++;
        }
        break;
    case 3:
        if (value(s, "SELECT downloaded FROM attachment_keys") == 1) {
            if (value(s, "SELECT count(*) FROM attachment_key_requests") || value(s, "SELECT count(*) FROM attachment_keys WHERE key IS NOT NULL")) {
                failure(t, "storage requested or received a key"); return;
            }
            utun_instance_destroy(a); t->nodes[A] = NULL;
            download(t, B); download(t, M); t->phase++;
        }
        break;
    case 4:
        if (value(b, "SELECT count(*) FROM attachment_key_outbox") == 1 && value(c, "SELECT count(*) FROM attachment_key_outbox") == 1) {
            if (readable(t, B) || readable(t, M)) { failure(t, "plaintext before author consent"); return; }
            if (start_node(t, A)) { failure(t, "author restart"); return; } t->phase++;
        }
        break;
    case 5:
        if (value(a, "SELECT count(*) FROM attachment_key_requests WHERE incoming=1 AND state=0") == 2) {
            inject(t, S, B, ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED, 0);
            inject(t, A, B, ROUTER_FLAG_SIGNED, 0);
            inject(t, A, B, ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED, 1);
            if (value(b, "SELECT count(*) FROM attachment_keys WHERE key IS NOT NULL") != 0) { failure(t, "forged control accepted"); return; }
            utun_instance_destroy(a); t->nodes[A] = NULL;
            if (start_node(t, A)) { failure(t, "pending request restart"); return; } t->phase++;
        }
        break;
    case 6:
        if (live(a, t->ids[B]) && live(a, t->ids[M])) {
#ifdef __linux__
            if (!t->decision_failures) {
                if (failed_decisions(t)) { failure(t, "author SQL failures changed state or reported success"); return; }
                utun_instance_destroy(a); t->nodes[A] = NULL;
                if (start_node(t, A)) { failure(t, "author restart after rejected decisions"); return; }
                break;
            }
            chat_event_set_handler(a, decision_event);
#endif
            int approved = chat_attachment_decide(a, CHANNEL, t->message, t->nodes[B]->my_ed25519_pubkey, 1);
#ifdef __linux__
            chat_event_set_handler(a, NULL);
#endif
            if (approved || value(a, "SELECT count(*) FROM attachment_key_requests WHERE incoming=1 AND state=0") != 1 ||
                chat_attachment_decide(a, CHANNEL, t->message, t->nodes[M]->my_ed25519_pubkey, 0)) { failure(t, "author decisions after restart"); return; }
            t->phase++;
        }
        break;
    case 7:
        if (readable(t, B) && value(c, "SELECT state FROM attachment_key_requests WHERE incoming=0") == 3 &&
            value(a, "SELECT count(*) FROM attachment_key_outbox") == 0) {
            if (readable(t, M) || value(c, "SELECT count(*) FROM attachment_keys WHERE key IS NOT NULL") ||
                value(s, "SELECT count(*) FROM attachment_keys WHERE key IS NOT NULL")) { failure(t, "key disclosed to denied/storage participant"); return; }
            for (int i = 0; i < NODES; i++) {
                if (value(t->nodes[i], "SELECT count(*) FROM media_files WHERE location NOT LIKE '%.uenc'") != 0) {
                    failure(t, "plaintext indexed for network distribution"); return;
                }
            }
            utun_instance_destroy(b); t->nodes[B] = NULL;
            if (start_node(t, B)) { failure(t, "recipient restart"); return; } t->phase++;
        }
        break;
    case 8:
        if (live(b, t->ids[A]) && readable(t, B)) {
            inject(t, A, B, ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED, 0);
            if (!readable(t, B) || value(b, "SELECT count(*) FROM attachment_key_requests") != 1 ||
                value(a, "SELECT count(*) FROM attachment_key_requests") != 2) { failure(t, "duplicate grant changed persisted access"); return; }
            sqlite3_stmt* cached = NULL;
            if (sqlite3_prepare_v2(b->topo_sqlite_db, "SELECT plain_path FROM attachment_keys", -1, &cached, NULL) ||
                sqlite3_step(cached) != SQLITE_ROW || remove((const char*)sqlite3_column_text(cached, 0))) {
                sqlite3_finalize(cached); failure(t, "cache eviction fixture"); return;
            }
            sqlite3_finalize(cached); download(t, B);
            t->phase++;
        }
        break;
    case 9:
        if (readable(t, B) && ++t->hold >= 20) {
            if (value(a, "SELECT count(*) FROM attachment_key_requests") != 2) { failure(t, "cache restore requested another consent"); return; }
            chat_core_delete_channel(c, CHANNEL);
            if (value(c, "SELECT count(*) FROM attachment_keys") || value(c, "SELECT count(*) FROM attachment_key_requests") ||
                value(c, "SELECT count(*) FROM attachment_key_outbox")) { failure(t, "deleted channel retained grants/requests"); return; }
            t->phase++; uasync_stop(t->ua); return;
        }
        break;
    }
    if (t->failed) return;
    if (t->ticks > 12000) { failure(t, "timeout"); return; }
    t->timer = uasync_set_timeout(t->ua, 100, t, tick, "encrypted_attachment_test");
}
int main(void) {
    struct media_test t = {0};
#ifdef __linux__
    active_test = &t;
#endif
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level_by_name("media", "info");
    debug_set_category_level_by_name("member_sync", "info");
    debug_set_category_level_by_name("chat_sync", "warn");
    debug_set_category_level_by_name("chat", "warn");
    test_clock_init(1000); test_clock_sync_wall_time();
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level_by_name("dm", "debug"); debug_set_category_level_by_name("media", "debug");
        debug_set_category_level_by_name("chat_sync", "debug"); debug_set_category_level_by_name("member_sync", "debug");
        debug_set_category_level_by_name("bgp", "info"); debug_set_category_level_by_name("etcp_route", "debug");
        debug_set_category_level_by_name("connection", "debug"); debug_set_category_level_by_name("etcp", "info");
        debug_set_category_level_by_name("general", "debug");
    }
    utun_instance_set_tun_init_enabled(0);
    snprintf(t.dir, sizeof(t.dir), "/tmp/utun_encrypted_attachment_XXXXXX");
    if (test_mkdtemp(t.dir)) return 1;
    char log_path[768]; snprintf(log_path, sizeof(log_path), "%s/test.log", t.dir);
    debug_enable_file_output(log_path, 1); debug_enable_console(0);
    for (int i = 0; i < NODES; i++) {
        if (sc_identity_generate(t.keys[i].public_key, t.keys[i].private_key, t.ed[i], t.seed[i])) return 1;
        t.ids[i] = sc_derive_node_id_from_pubkey(t.keys[i].public_key);
    }
    uint8_t channel_x_private[32];
    t.ch_priv[0] = 94;
    if (sc_identity_from_seed(t.ch_priv, t.ch_x, channel_x_private, t.ch_ed)) return 1;
    if (test_group_signature(CHANNEL, "encrypted-attachment-test", t.ed[A], t.ch_x, t.ch_ed, t.ch_priv, t.ch_sig)) return 1;
    snprintf(t.file, sizeof(t.file), "%s/source.bin", t.dir);
    FILE* f = fopen(t.file, "wb");
    if (!f) return 1;
    for (unsigned i = 0; i < FILE_BYTES; i++) fputc((int)((i * 73 + 19) & 255), f);
    fclose(f);
    t.ua = uasync_create();
    if (!t.ua || start_node(&t, S) || start_node(&t, A) || start_node(&t, M) || start_node(&t, B)) return 1;
    t.timer = uasync_set_timeout(t.ua, 100, &t, tick, "dm_media_test");
    test_clock_mainloop(t.ua);
#ifdef __linux__
    if (!t.failed && t.forced_cleanup != 1) failure(&t, "cleanup during active block was not exercised");
    if (!t.failed && (t.grant_bind_faults != 1 || t.grant_rollbacks != 1 || !t.result_acks))
        failure(&t, "GRANT SQL fault, rollback and real ACK after retry were not exercised");
    if (!t.failed && (t.decision_failures != 2 || t.decision_events != 1))
        failure(&t, "author decision failures, restart and committed event were not exercised");
    if (!t.failed && (t.request_rejections != 1 || t.request_restarts != 1 || t.request_sends < 2 || !t.request_acks ||
        t.request_events || t.request_external)) failure(&t, "REQUEST rejection, restart, real retry and committed ACK were not exercised");
    if (!t.failed && (t.observation_faults != 1 || t.observation_events != 1 || t.observation_recovered != 1))
        failure(&t, "observation SQL failure, committed message notification and real index recovery were not exercised");
#endif
    for (int i = 0; i < NODES; i++) if (t.nodes[i]) utun_instance_destroy(t.nodes[i]);
    uasync_destroy(t.ua, 0);
    test_clock_finish();
    printf("test_encrypted_attachment: phase=%u heartbeats=%u forced_cleanup=%u %s artifacts=%s\n", t.phase, t.heartbeats,
           t.forced_cleanup, !t.failed && t.phase == 10 ? "PASS" : "FAIL", t.dir);
    return t.failed || t.phase != 10;
}
