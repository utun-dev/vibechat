/* test_dm_media — реальные A–S–B и S–M, независимые роли supernode/storage.
 * Прямая доставка, offline custody, restart S/M при выключенном A, TTL,
 * квота, точные квитанции, поздние STORE/STORED, возврат оригинала после EXPIRED и отзыв активных операций.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/utun_instance.h"
#include "../src/ntp_time.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/member_sync.h"
#include "../src/chat/chat_setting.h"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_crypto.h"
#include "../src/dm/dm_media.h"
#include "../src/dm/dm_media_priv.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/routing_layer/etcp_router.h"
#include "../src/transport_layer/secure_channel.h"
#include "../src/transport_layer/etcp.h"
#include "../src/media_delivery/file_transfer.h"
#include "test_utils.h"
#include "test_clock.h"
#include "test_net.h"
#include "test_membership.h"

#define CHANNEL "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951" /* Ed25519 для seed {94}. */
#define FILE_BYTES (3 * 32768 + 17)
enum { A, B, S, M, NODES };

struct media_test {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* nodes[NODES];
    struct SC_MYKEYS keys[NODES];
    uint8_t seed[NODES][32];
    uint64_t ids[NODES];
    uint16_t ports[NODES]; /* ОС назначает один endpoint на узел; restart сохраняет этот endpoint. */
    uint8_t ed[NODES][32], ch_x[32], ch_ed[32], ch_priv[32], ch_sig[64];
    char dir[512], conv[64], file[768];
    uint8_t body[2048], id[16], receipt[DM_MEDIA_RECEIPT_SIZE];
    size_t body_len;
    unsigned phase, ticks, heartbeats, work_ticks;
    uint64_t recovery_start;
    int expiry_armed;
    int failed;
    void* timer;
};

#ifdef DM_QUOTA_FAULT_INJECTION
enum quota_fault { QUOTA_NONE, QUOTA_PREPARE, QUOTA_STEP, QUOTA_FINALIZE, QUOTA_EMPTY };
static sqlite3* quota_db;
static enum quota_fault quota_fault;
static unsigned quota_hits, quota_attempts, quota_rejects, quota_stored;
static int quota_sequence;

enum release_fault { RELEASE_NONE, RELEASE_HOLDER_PREPARE, RELEASE_HOLDER_BIND, RELEASE_HOLDER_STEP, RELEASE_HOLDER_FINALIZE,
                     RELEASE_JOB_PREPARE, RELEASE_JOB_BIND, RELEASE_JOB_STEP, RELEASE_JOB_FINALIZE, RELEASE_COMMIT };
static sqlite3* release_db;
static sqlite3* expiry_storage_db;
static enum release_fault release_fault;
static unsigned release_hits, release_checks, expiry_blocks, expiry_notices;
static int expiry_block, expiry_restarted;

enum retire_fault { RETIRE_NONE, RETIRE_INSERT_PREPARE, RETIRE_ID_BIND, RETIRE_HASH_BIND, RETIRE_RECEIPT_BIND,
                    RETIRE_INSERT_STEP, RETIRE_INSERT_FINALIZE, RETIRE_DELETE_PREPARE, RETIRE_DELETE_BIND,
                    RETIRE_DELETE_STEP, RETIRE_DELETE_FINALIZE, RETIRE_COMMIT };
static sqlite3* retire_db;
static enum retire_fault retire_fault;
static unsigned retire_hits, retire_checks, retire_acks, retire_restarted;
static int retire_drop_ack;

static int retire_query(sqlite3* db, const char* sql, int deletion) {
    const char* prefix = deletion ? "DELETE FROM dm_custody WHERE id=?" : "INSERT INTO dm_custody_done";
    return retire_db && db == retire_db && sql && !strncmp(sql, prefix, strlen(prefix));
}

/* Держим отказ до проверки таймером теста: повторный DELETE не должен успеть скрыть неверный commit. */
static int retire_inject(enum retire_fault kind) {
    if (retire_fault != kind) return 0;
    retire_hits++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM retirement test: SQL fault=%d hit=%u", kind, retire_hits);
    return 1;
}

/* Выделить два UPDATE перехода, не затрагивая регистрацию/приём сообщений. */
static int release_query(sqlite3* db, const char* sql, int job) {
    const char* prefix = job ? "UPDATE dm_media_jobs SET holder=NULL,stored=0" : "UPDATE dm_media_holders SET released=";
    return release_db && db == release_db && sql && !strncmp(sql, prefix, strlen(prefix));
}

static int release_inject(enum release_fault kind) {
    if (release_fault != kind) return 0;
    release_fault = RELEASE_NONE; release_hits++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM release test: one-shot SQL fault=%d hit=%u", kind, release_hits);
    return 1;
}

static int quota_query(sqlite3* db, const char* sql) {
    return quota_db && db == quota_db && sql && !strcmp(sql, "SELECT COALESCE(sum(cipher_size),0) FROM dm_custody");
}

/* Последовательные одноразовые ошибки обслуживаются обычными повторами STORE с S. */
static int quota_inject(enum quota_fault kind) {
    if (quota_fault != kind) return 0;
    quota_hits++;
    quota_fault = QUOTA_NONE;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM quota test: fault=%d hit=%u attempt=%u", kind, quota_hits, quota_attempts);
    return 1;
}

extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int len, sqlite3_stmt** st, const char** tail) {
    if ((retire_query(db, sql, 0) && retire_inject(RETIRE_INSERT_PREPARE)) ||
        (retire_query(db, sql, 1) && retire_inject(RETIRE_DELETE_PREPARE))) { *st = NULL; return SQLITE_IOERR; }
    if ((release_query(db, sql, 0) && release_inject(RELEASE_HOLDER_PREPARE)) ||
        (release_query(db, sql, 1) && release_inject(RELEASE_JOB_PREPARE))) { *st = NULL; return SQLITE_IOERR; }
    if (quota_query(db, sql)) {
        quota_attempts++;
        if (quota_sequence) quota_fault = quota_attempts <= 3 ? (enum quota_fault)quota_attempts : QUOTA_NONE;
        if (quota_inject(QUOTA_PREPARE)) { *st = NULL; return SQLITE_IOERR; }
    }
    return __real_sqlite3_prepare_v2(db, sql, len, st, tail);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* data, int bytes, void (*destroy)(void*)) {
    sqlite3* db = sqlite3_db_handle(st);
    const char* sql = sqlite3_sql(st);
    if ((retire_query(db, sql, 0) && index >= 1 && index <= 3 &&
         retire_inject((enum retire_fault)(RETIRE_ID_BIND + index - 1))) ||
        (retire_query(db, sql, 1) && retire_inject(RETIRE_DELETE_BIND))) return SQLITE_NOMEM;
    if ((release_query(db, sql, 0) && release_inject(RELEASE_HOLDER_BIND)) ||
        (release_query(db, sql, 1) && release_inject(RELEASE_JOB_BIND))) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, data, bytes, destroy);
}

int __wrap_sqlite3_step(sqlite3_stmt* st) {
    sqlite3* db = sqlite3_db_handle(st);
    const char* sql = sqlite3_sql(st);
    if ((retire_query(db, sql, 0) && retire_inject(RETIRE_INSERT_STEP)) ||
        (retire_query(db, sql, 1) && retire_inject(RETIRE_DELETE_STEP))) return SQLITE_IOERR;
    if (release_query(db, sql, 0) && release_inject(RELEASE_HOLDER_STEP)) return SQLITE_IOERR;
    if (release_query(db, sql, 1)) {
        if (release_inject(RELEASE_JOB_STEP)) return SQLITE_IOERR;
        if (expiry_block) {
            expiry_blocks++;
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM expiry test: block job reset attempt=%u", expiry_blocks);
            return SQLITE_IOERR;
        }
    }
    if (quota_query(sqlite3_db_handle(st), sqlite3_sql(st))) {
        if (quota_inject(QUOTA_STEP)) return SQLITE_IOERR;
        if (quota_inject(QUOTA_EMPTY)) return SQLITE_DONE;
    }
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** error) {
    if (retire_db && db == retire_db && !strcmp(sql, "COMMIT") && retire_inject(RETIRE_COMMIT)) {
        if (error) *error = NULL;
        return SQLITE_IOERR;
    }
    if (release_db && db == release_db && !strcmp(sql, "COMMIT") && release_inject(RELEASE_COMMIT)) {
        if (error) *error = NULL;
        return SQLITE_IOERR;
    }
    return __real_sqlite3_exec(db, sql, callback, arg, error);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && quota_query(sqlite3_db_handle(st), sqlite3_sql(st)) && quota_inject(QUOTA_FINALIZE);
    if (st && ((retire_query(sqlite3_db_handle(st), sqlite3_sql(st), 0) && retire_inject(RETIRE_INSERT_FINALIZE)) ||
        (retire_query(sqlite3_db_handle(st), sqlite3_sql(st), 1) && retire_inject(RETIRE_DELETE_FINALIZE)))) fail = 1;
    if (st && ((release_query(sqlite3_db_handle(st), sqlite3_sql(st), 0) && release_inject(RELEASE_HOLDER_FINALIZE)) ||
        (release_query(sqlite3_db_handle(st), sqlite3_sql(st), 1) && release_inject(RELEASE_JOB_FINALIZE)))) fail = 1;
    int rc = __real_sqlite3_finalize(st);
    return fail ? SQLITE_IOERR : rc;
}
extern int __real_etcp_router_conn_send(struct ETCP_ROUTER_CONN*, const uint8_t*, size_t, int);
int __wrap_etcp_router_conn_send(struct ETCP_ROUTER_CONN* conn, const uint8_t* data, size_t len, int mode) {
    if (retire_db && conn->inst->topo_sqlite_db == retire_db && conn->svc_id == ETCP_RT_ID_DM_MEDIA &&
        len && data[0] == DM_MEDIA_DELETE_ACK) {
        retire_acks++;
        if (retire_drop_ack) {
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM retirement test: drop DELETE_ACK count=%u", retire_acks);
            return 0;
        }
    }
    if (expiry_storage_db && conn->inst->topo_sqlite_db == expiry_storage_db &&
        conn->svc_id == ETCP_RT_ID_DM_MEDIA && len && data[0] == DM_MEDIA_EXPIRED) {
        expiry_notices++;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM expiry test: drop EXPIRED notice=%u; S must reconcile its deadline", expiry_notices);
        return 0;
    }
    if (quota_db && conn->inst->topo_sqlite_db == quota_db && conn->svc_id == ETCP_RT_ID_DM_MEDIA && len) {
        if (data[0] == DM_MEDIA_REJECT) quota_rejects++;
        if (data[0] == DM_MEDIA_STORED) quota_stored++;
    }
    return __real_etcp_router_conn_send(conn, data, len, mode);
}
#endif

static int value(struct UTUN_INSTANCE* inst, const char* sql) {
    if (!inst) return -1;
    sqlite3_stmt* st = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        result = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return result;
}

static int live(struct UTUN_INSTANCE* inst, uint64_t peer) {
    return inst && etcp_router_get_path(inst, topo_groups_channel_slot(inst->topo_groups, CHANNEL), peer, ETCP_RT_ID_DM).next_hop_node_id != 0;
}

static void failure(struct media_test* t, const char* reason) {
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "media test failed phase=%u reason=%s", t->phase, reason);
    t->failed = 1;
    uasync_stop(t->ua);
}

static void hex(const uint8_t* p, size_t len, char* out) {
    for (size_t i = 0; i < len; i++) snprintf(out + i * 2, 3, "%02x", p[i]);
}

/* Четыре ядра используют разные БД; клиентские физические линки сходятся на S. */
static int start_node(struct media_test* t, int index) {
    char path[768], pub[65], priv[65], server[65], db[768];
    hex(t->keys[index].public_key, 32, pub); hex(t->seed[index], 32, priv);
    snprintf(db, sizeof(db), "%s/db%d", t->dir, index); utun_mkdir(db, 0700);
    snprintf(path, sizeof(path), "%s/%d.conf", t->dir, index);
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "[global]\nmy_private_key=%s\nmy_public_key=%s\ndb_path=%s\n"
               "[server:s1]\naddr=127.0.0.1:%d\ntype=public\n[allowed_keys]\nallow_all=1\n"
               "[chatserver]\nstorage_autoload=1\ndm_media_delete_on_open=0\n", priv, pub, db, t->ports[index]);
    if (index != S) {
        hex(t->keys[S].public_key, 32, server);
        fprintf(f, "[client:to_s]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", server);
    }
    fclose(f);
    t->nodes[index] = utun_instance_create(t->ua, path);
    if (t->nodes[index]) {
        t->ports[index] = ss_get_port(&test_etcp_bound_socket(t->nodes[index], "s1")->local_addr);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM media fixture: node=%d port=%u retained for restart", index, t->ports[index]);
    }
    if (t->nodes[index] && index != S) test_etcp_set_peer(t->nodes[index], "to_s", t->nodes[S], "s1");
    return t->nodes[index] ? utun_instance_init(t->nodes[index]) : -1;
}

/* Подписать мемберские и административные блоки, не подменяя авторизацию SQL-ом. */
static int setup(struct media_test* t) {
    for (int i = 0; i < NODES; i++) {
        struct UTUN_INSTANCE* inst = t->nodes[i];
        if (topo_node_sqlite_channel_put(inst->topo_sqlite_db, CHANNEL, "dm-media-test", t->nodes[A]->my_ed25519_pubkey,
            t->ch_x, NULL, t->ch_ed, i == A ? t->ch_priv : NULL, t->ch_sig)) return -1;
        if (chat_core_ensure_channel_ready(inst, CHANNEL)) return -1;
        for (int j = 0; j < NODES; j++) {
            uint8_t msg[512], join[64], adm_sig[64], tree[64], update[64];
            uint64_t ts = ntp_time_get_seconds(inst);
            int n = member_sync_build_join_msg(t->ch_x, t->ch_ed, t->nodes[j]->my_ed25519_pubkey, t->keys[j].public_key, msg, sizeof(msg));
            if (n < 0 || sc_ed25519_sign(t->nodes[j]->my_ed25519_privkey, msg, (size_t)n, join) != SC_OK) return -1;
            const char* tags = j == S ? "{\"supernode\":\"yes\",\"ver\":\"1\"}" :
                               j == M ? "{\"storage\":\"yes\",\"ver\":\"1\"}" : "{\"ver\":\"1\"}";
            n = member_sync_build_owner_msg(CHANNEL, t->ed[j], tags, msg, sizeof(msg));
            if (n < 0 || sc_ed25519_sign(t->ch_priv, msg, (size_t)n, adm_sig) != SC_OK) return -1;
            char info[64]; snprintf(info, sizeof(info), "{\"name\":\"node%d\"}", j);
            if (member_sync_sign_join(t->ch_priv, CHANNEL, NULL, t->keys[j].public_key, t->ed[j],
                TEST_JOIN_ID(t->ch_x, t->ch_ed, t->nodes[j]->my_ed25519_pubkey, t->keys[j].public_key), ts, tree) ||
                test_member_profile(t->nodes[j]->my_ed25519_privkey, TEST_JOIN_ID(t->ch_x, t->ch_ed, t->nodes[j]->my_ed25519_pubkey, t->keys[j].public_key), ts, info, update) ||
                member_sync_put(inst, CHANNEL, t->keys[j].public_key, t->ed[j], join, ts,
                                update, ts, info, tags, adm_sig, 0, 0, tree) < 0) return -1;
        }
        if (i != S) member_sync_start(inst, instance_find_conn(inst, t->ids[S]), CHANNEL, NULL, NULL);
    }
    return dm_start(t->nodes[A], t->ids[B], t->keys[B].public_key, t->ed[B], "B", CHANNEL, DM_MODE_DEVICE, NULL);
}

/* Снимок подписанного тела нужен для атак/replay и не заменяет UDP-передачу. */
static int snapshot(struct media_test* t, unsigned seq) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(t->nodes[A]->topo_sqlite_db, "SELECT body FROM dm_files WHERE seq=? AND dir=1", -1, &st, NULL)) return -1;
    sqlite3_bind_int(st, 1, (int)seq);
    int rc = sqlite3_step(st);
    size_t n = rc == SQLITE_ROW ? (size_t)sqlite3_column_bytes(st, 0) : 0;
    if (n && n <= sizeof(t->body)) memcpy(t->body, sqlite3_column_blob(st, 0), n);
    sqlite3_finalize(st);
    const uint8_t* desc = n <= sizeof(t->body) ? dm_message_media(t->body, n) : NULL;
    if (!desc) return -1;
    memcpy(t->id, desc, 16); t->body_len = n;
    return 0;
}

/* После настоящего STORED синхронно приблизить срок S/M; скорость worker не определяет исход TTL-теста. */
static int expire_fixture(struct media_test* t) {
    uint64_t deadline = ntp_time_get_seconds(t->nodes[S]);
    const char* sql[] = { "UPDATE dm_media_jobs SET expires=?,retry_at=0 WHERE id=?",
                          "UPDATE dm_custody SET expires=?,retry_at=0 WHERE id=?" };
    for (int i = 0; i < 2; i++) {
        sqlite3* db = t->nodes[S + i]->topo_sqlite_db;
        sqlite3_stmt* st = NULL;
        int rc = sqlite3_prepare_v2(db, sql[i], -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)deadline);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, t->id, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final != SQLITE_OK || sqlite3_changes(db) != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM expiry test: deadline setup failed node=%d rc=%d finalize=%d", S + i, rc, final);
            return -1;
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM expiry test: STORED confirmed before matching S/M deadline=%llu",
               (unsigned long long)deadline);
    return 0;
}

/* Полученный файл сверяется побайтно; размер или ready-флаг недостаточны. */
static int file_correct(struct media_test* t, int node) {
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(t->nodes[node]->topo_sqlite_db, "SELECT local_path,receipt FROM dm_files WHERE id=? AND ready=1", -1, &st, NULL)) return 0;
    sqlite3_bind_blob(st, 1, t->id, 16, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    char path[1024] = "";
    if (rc == SQLITE_ROW && sqlite3_column_bytes(st, 1) == sizeof(t->receipt)) {
        snprintf(path, sizeof(path), "%s", (const char*)sqlite3_column_text(st, 0));
        memcpy(t->receipt, sqlite3_column_blob(st, 1), sizeof(t->receipt));
    }
    sqlite3_finalize(st);
    FILE* f = path[0] ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    int ok = 1;
    for (unsigned i = 0; i < FILE_BYTES; i++) if (fgetc(f) != (int)((i * 73 + 19) & 255)) { ok = 0; break; }
    if (ok && fgetc(f) != EOF) ok = 0;
    fclose(f);
    return ok;
}

/* Вход после проверки router, для проверки SQL rollback и подписанных ACK. */
static void control(struct media_test* t, int dst, uint64_t source, uint8_t cmd, const uint8_t* p, size_t len) {
    struct UTUN_INSTANCE* inst = t->nodes[dst];
    struct ETCP_CONN* conn = instance_find_conn(inst, t->ids[dst == S ? M : S]);
    struct ll_entry* e = ll_alloc_lldgram((uint16_t)(ROUTER_SVC_PAYLOAD_OFF + 1 + len));
    if (!conn || !e) { if (e) { queue_dgram_free(e); queue_entry_free(e); } failure(t, "control fixture unavailable"); return; }
    e->len = (uint16_t)(ROUTER_SVC_PAYLOAD_OFF + 1 + len);
    memset(e->dgram, 0, e->len);
    e->dgram[0] = ETCP_RT_ID_DM_MEDIA;
    for (unsigned i = 0; i < sizeof(t->ids) / sizeof(t->ids[0]); i++) {
        if (t->ids[i] == source) memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, t->ed[i], 32);
    }
    uint64_t group = topo_groups_channel_slot(inst->topo_groups, CHANNEL); memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &group, 8);
    e->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_SIGNED; e->dgram[ROUTER_SVC_PAYLOAD_OFF] = cmd;
    memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, p, len);
    inst->router_bindings.callbacks[ETCP_RT_ID_DM_MEDIA](conn, e);
}

static void cancelled(void* arg, int error) {
    int* calls = arg;
    *calls += error == -2 ? 1 : 100;
}

/* Известный file-message, два S и одна условная копия: проверяем полномочия без ожидания TTL/сети. */
static int source_withdraw_checks(struct media_test* t) {
    struct UTUN_INSTANCE* b = t->nodes[B];
    uint64_t expiry = ntp_time_get_seconds(b) + 1000;
    uint8_t offer[DM_MEDIA_OFFER_HEADER + sizeof(t->body)], gone[DM_MEDIA_GONE_SIZE];
    uint8_t holder_ed[32], x[32], secret[32], seed[32];
    if (sc_identity_generate(x, secret, holder_ed, seed)) return -1;
    char holder[65], issuer[65], author[65];
    sc_public_key_hex(holder_ed, holder); sc_public_key_hex(t->ed[M], issuer); sc_public_key_hex(t->ed[A], author);
    memcpy(offer, b->my_ed25519_pubkey, 32); memcpy(offer + DM_MEDIA_OFFER_HOLDER_OFF, holder_ed, 32);
    memcpy(offer + DM_MEDIA_OFFER_EXPIRES_OFF, &expiry, 8); memcpy(offer + DM_MEDIA_OFFER_HEADER, t->body, t->body_len);
    memcpy(gone, t->id, 16); SHA256(t->body, t->body_len, gone + 16); memcpy(gone + DM_MEDIA_GONE_HOLDER_OFF, holder_ed, 32);
    char sql[512], tags[1024]; sqlite3_stmt* st = NULL;
    snprintf(sql, sizeof(sql), "SELECT adm_tags FROM peers_%s WHERE node_id=%lld", CHANNEL, (long long)t->ids[M]);
    if (sqlite3_prepare_v2(b->topo_sqlite_db, sql, -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) return -1;
    snprintf(tags, sizeof(tags), "%s", sqlite3_column_text(st, 0)); sqlite3_finalize(st);
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET adm_tags='{\"supernode\":\"yes\"}' WHERE node_id=%lld", CHANNEL, (long long)t->ids[M]);
    if (dm_media_exec(b, sql) || test_member_publish_access(b, CHANNEL)) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM source fixture: published injected owner role; memory supernode=%d storage=%d",
        dm_peer_role(b, t->ids[M], "supernode"), dm_peer_role(b, t->ids[M], "storage"));
    snprintf(sql, sizeof(sql), "UPDATE dm_files SET ready=0,holder=x'%s' WHERE dir=0 AND seq=1", holder);
    if (dm_media_exec(b, sql)) return -1;
    char holder_sql[192], sources_sql[192];
    snprintf(holder_sql, sizeof(holder_sql), "SELECT count(*) FROM dm_files WHERE dir=0 AND seq=1 AND holder=x'%s'", holder);
    snprintf(sources_sql, sizeof(sources_sql), "SELECT count(*) FROM dm_file_sources WHERE node=x'%s' AND expires>0", holder);
    control(t, B, t->ids[M], DM_MEDIA_SOURCE_GONE, gone, sizeof(gone));
    if (value(b, holder_sql) != 1) return -1; // личная HAVE-копия без OFFER
    snprintf(sql, sizeof(sql), "DELETE FROM dm_file_sources WHERE node=x'%s' AND issuer=x'%s'", holder, issuer);
    if (dm_media_exec(b, sql)) return -1;
    control(t, B, t->ids[S], DM_MEDIA_OFFER, offer, DM_MEDIA_OFFER_HEADER + t->body_len);
    control(t, B, t->ids[M], DM_MEDIA_SOURCE_GONE, gone, sizeof(gone));
    int holder_state = value(b, holder_sql);
    int source_count = value(b, sources_sql);
    if (holder_state != 1 || source_count != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "DM source fixture: foreign withdrawal holder=%d expected=1 sources=%d expected=1",
            holder_state, source_count); return -1;
    }
    /* Новый независимый сценарий: тот же M уже действительно издаёт OFFER. */
    snprintf(sql, sizeof(sql), "DELETE FROM dm_file_sources WHERE node=x'%s' AND issuer=x'%s'", holder, issuer);
    if (dm_media_exec(b, sql)) return -1;
    control(t, B, t->ids[M], DM_MEDIA_OFFER, offer, DM_MEDIA_OFFER_HEADER + t->body_len);
    control(t, B, t->ids[S], DM_MEDIA_SOURCE_GONE, gone, sizeof(gone));
    holder_state = value(b, holder_sql);
    source_count = value(b, sources_sql);
    if (holder_state != 1 || source_count != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "DM source fixture: independent OFFER/withdrawal holder=%d expected=1 sources=%d expected=1 M_role=%d",
            holder_state, source_count, dm_peer_role(b, t->ids[M], "supernode")); return -1;
    }
    control(t, B, t->ids[S], DM_MEDIA_OFFER, offer, DM_MEDIA_OFFER_HEADER + t->body_len); // позднее объявление не отменяет отзыв S
    if (value(b, sources_sql) != 1) return -1;
    control(t, B, t->ids[M], DM_MEDIA_SOURCE_GONE, gone, sizeof(gone));
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM dm_files WHERE dir=0 AND seq=1 AND holder=x'%s'", author);
    if (value(b, sql) != 1) return -1;
    /* Удаляем только искусственную копию и возвращаем готовность настоящего файла. */
    snprintf(sql, sizeof(sql), "DELETE FROM dm_file_sources WHERE node=x'%s'", holder);
    if (dm_media_exec(b, sql) || dm_media_exec(b, "UPDATE dm_files SET ready=1,expires=0 WHERE dir=0 AND seq=1")) return -1;
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET adm_tags=? WHERE node_id=?", CHANNEL);
    if (sqlite3_prepare_v2(b->topo_sqlite_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, tags, -1, SQLITE_STATIC); sqlite3_bind_int64(st, 2, (sqlite3_int64)t->ids[M]);
    if (dm_media_step(b, st) || test_member_publish_access(b, CHANNEL)) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM source checks PASS: foreign S denied, two issuers independent, late OFFER denied, last withdrawal resets holder");
    return 0;
}

static int assignment_id(struct media_test* t, uint8_t out[16]) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(t->nodes[S]->topo_sqlite_db,
        "SELECT assignment_id FROM dm_media_holders WHERE id=? AND node=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, t->id, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, t->ed[M], 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 16;
    if (found) memcpy(out, sqlite3_column_blob(st, 0), 16);
    int final = sqlite3_finalize(st);
    if (!found || final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM test: assignment unavailable rc=%d final=%d", rc, final);
    return found && final == SQLITE_OK ? 0 : -1;
}

#ifdef DM_QUOTA_FAULT_INJECTION
/* Снимок точного назначения S; тело/квитанция не должны меняться переходом освобождения. */
static int release_state(struct media_test* t, sqlite3_int64 state[7]) {
    sqlite3_stmt* st = NULL;
    sqlite3* db = t->nodes[S]->topo_sqlite_db;
    int rc = sqlite3_prepare_v2(db, "SELECT j.holder,j.stored,j.expired,j.expires,j.retry_at,h.released,j.delete_on_open"
        " FROM dm_media_jobs j JOIN dm_media_holders h ON h.id=j.id WHERE j.id=? AND h.node=?", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, t->id, 16, SQLITE_STATIC); sqlite3_bind_blob(st, 2, t->ed[M], 32, SQLITE_STATIC);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            state[0] = sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), t->ed[M], 32) ? (sqlite3_int64)t->ids[M] : 0;
            for (int i = 1; i < 7; i++) state[i] = sqlite3_column_int64(st, i);
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM release test: state read failed rc=%d", rc);
    return rc == SQLITE_ROW ? 0 : -1;
}

/* После синхронного component-контроля вернуть живую фикстуру до следующего uasync callback. */
static int release_restore(struct media_test* t, const sqlite3_int64 state[7]) {
    sqlite3_stmt* st = NULL;
    sqlite3* db = t->nodes[S]->topo_sqlite_db;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE dm_media_jobs SET holder=?,stored=?,expired=?,expires=?,retry_at=?,delete_on_open=? WHERE id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        if (state[0]) sqlite3_bind_blob(st, 1, t->ed[M], 32, SQLITE_STATIC); else sqlite3_bind_null(st, 1);
        for (int i = 1; i < 5; i++) sqlite3_bind_int64(st, i + 1, state[i]);
        sqlite3_bind_int64(st, 6, state[6]); sqlite3_bind_blob(st, 7, t->id, 16, SQLITE_STATIC); rc = sqlite3_step(st);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto fail;
    rc = sqlite3_prepare_v2(db, "UPDATE dm_media_holders SET released=? WHERE id=? AND node=?", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, state[5]); sqlite3_bind_blob(st, 2, t->id, 16, SQLITE_STATIC);
        sqlite3_bind_blob(st, 3, t->ed[M], 32, SQLITE_STATIC); rc = sqlite3_step(st);
    }
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM release test: fixture restore failed rc=%d error=%s", rc, sqlite3_errmsg(db));
    return -1;
}

/* Реальный зарегистрированный job: отказ каждого UPDATE/COMMIT откатывает оба состояния, повтор завершает переход. */
static int release_failures(struct media_test* t) {
    sqlite3_int64 before[7], after[7], completed[7], original[7];
    if (release_state(t, original)) return -1;
    uint8_t packet[41] = {0}; if (assignment_id(t, packet)) return -1;
    memcpy(packet + 16, t->id, 16); packet[32] = 1;
    release_db = t->nodes[S]->topo_sqlite_db;
    const uint8_t commands[] = { DM_MEDIA_EXPIRED, DM_MEDIA_REJECT };
    for (size_t c = 0; c < sizeof(commands); c++) {
        uint8_t cmd = commands[c];
        memcpy(before, original, sizeof(before));
        if (cmd == DM_MEDIA_REJECT) { before[1] = before[3] = 0; before[6] = -1; }
        if (release_restore(t, before)) return -1;
        for (enum release_fault kind = RELEASE_HOLDER_PREPARE; kind <= RELEASE_COMMIT; kind++) {
            release_hits = 0; release_fault = kind;
            control(t, S, t->ids[M], cmd, packet, cmd == DM_MEDIA_EXPIRED ? 32 : 33);
            release_fault = RELEASE_NONE;
            if (t->failed || release_hits != 1 || release_state(t, after) || memcmp(before, after, sizeof(before)) ||
                !sqlite3_get_autocommit(release_db)) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM release test: partial transition cmd=%u fault=%d hits=%u", cmd, kind, release_hits);
                return -1;
            }
            control(t, S, t->ids[M], cmd, packet, cmd == DM_MEDIA_EXPIRED ? 32 : 33);
            if (release_state(t, completed) || completed[0] || completed[1] || completed[2] != (cmd == DM_MEDIA_EXPIRED) ||
                completed[3] != before[3] || (cmd == DM_MEDIA_EXPIRED ? completed[4] != 0 :
                    completed[4] < (sqlite3_int64)ntp_time_get_seconds(t->nodes[S]) + 59) ||
                completed[5] != (cmd == DM_MEDIA_EXPIRED ? 2 : 3) ||
                value(t->nodes[S], "SELECT count(*) FROM dm_media_jobs WHERE receipt IS NOT NULL") ||
                value(t->nodes[S], "SELECT count(*) FROM dm_media_jobs WHERE body IS NULL")) return -1;
            control(t, S, t->ids[M], cmd, packet, cmd == DM_MEDIA_EXPIRED ? 32 : 33);
            uint64_t expiry = (uint64_t)original[3]; memcpy(packet + 32, &expiry, 8);
            control(t, S, t->ids[M], DM_MEDIA_STORED, packet, sizeof(packet));
            packet[32] = 1;
            if (release_state(t, after) || memcmp(completed, after, sizeof(after)) || release_restore(t, before)) return -1;
            release_checks++;
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM release test: cmd=%u fault=%d rollback/retry/duplicate/late STORED PASS check=%u",
                       cmd, kind, release_checks);
        }
    }
    release_db = NULL;
    return release_restore(t, original);
}

/* При ошибке чтения свободное M не выдаёт REJECT/STORED и не меняет durable состояние. */
static int quota_failures(struct media_test* t, const uint8_t* request, size_t len) {
    struct UTUN_INSTANCE* m = t->nodes[M];
    quota_db = m->topo_sqlite_db;
    int changes = sqlite3_total_changes(quota_db);
    int super_changes = sqlite3_total_changes(t->nodes[S]->topo_sqlite_db);
    for (enum quota_fault kind = QUOTA_PREPARE; kind <= QUOTA_EMPTY; kind++) {
        quota_fault = kind; quota_hits = quota_attempts = quota_rejects = quota_stored = 0;
        control(t, M, t->ids[S], DM_MEDIA_STORE, request, len);
        quota_fault = QUOTA_NONE;
        if (t->failed || quota_hits != 1 || quota_attempts != 1 || quota_rejects || quota_stored ||
            sqlite3_total_changes(quota_db) != changes || !sqlite3_get_autocommit(quota_db) ||
            sqlite3_total_changes(t->nodes[S]->topo_sqlite_db) != super_changes ||
            value(m, "SELECT count(*) FROM dm_custody") || value(m, "SELECT count(*) FROM dm_custody_done")) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM quota test: error admitted/refused custody fault=%d hits=%u rejects=%u stored=%u",
                        kind, quota_hits, quota_rejects, quota_stored);
            quota_db = NULL;
            return -1;
        }
    }
    quota_db = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM quota test: 4 SQL failures preserve custody and emit no REJECT/STORED");
    return 0;
}
#endif

/* Ошибки регистрации не расходуют seq; квота проверяется до получения байтов. */
static int protocol_checks(struct media_test* t) {
    struct UTUN_INSTANCE* a = t->nodes[A];
    struct UTUN_INSTANCE* m = t->nodes[M];
    const uint8_t* desc = dm_message_media(t->body, t->body_len);
    uint8_t input[DM_MEDIA_DESC_SIZE + ATTACHMENT_WIRE_MAX];
    struct attachment_info info = { .kind = ATTACHMENT_FILE, .name = "rollback.bin" };
    int metadata_len = attachment_encode(&info, input + DM_MEDIA_DESC_SIZE, ATTACHMENT_WIRE_MAX);
    if (metadata_len < 0) return -1;
    memcpy(input, desc, DM_MEDIA_DESC_SIZE);
    if (sqlite3_exec(a->topo_sqlite_db, "CREATE TEMP TRIGGER reject_dm_file BEFORE INSERT ON dm_files"
        " BEGIN SELECT RAISE(ABORT,'injected media registration failure'); END", NULL, NULL, NULL)) return -1;
    int rejected = dm_send(a, t->conv, "file", input, DM_MEDIA_DESC_SIZE + metadata_len) < 0;
    if (sqlite3_exec(a->topo_sqlite_db, "DROP TRIGGER reject_dm_file", NULL, NULL, NULL)) return -1;
    if (!rejected || value(a, "SELECT last_out_seq FROM dm_conversations") != 1 ||
        value(a, "SELECT count(*) FROM dm_messages") != 1 || value(a, "SELECT count(*) FROM dm_outbox") != 0) return -1;
    char json[8192]; size_t len;
    if (dm_list_messages_json(a, t->conv, 200, 0, json, sizeof(json), &len) ||
        !strstr(json, "\"media_status\":\"delivered\"") || !strstr(json, "\"media_path\":") ||
        !strstr(json, "\"content_type\":\"audio/opus\"") || !strstr(json, "\"duration_ms\":3000")) return -1;
    uint8_t request[DM_MEDIA_STORE_HEADER + sizeof(t->body)] = {5};
    memcpy(request + DM_MEDIA_STORE_RECIPIENT_OFF, t->ed[B], 32); memcpy(request + DM_MEDIA_STORE_HEADER, t->body, t->body_len);
    uint8_t* metadata = (uint8_t*)dm_message_media(request + DM_MEDIA_STORE_HEADER, t->body_len);
    metadata[0] ^= 0x80;
    uint64_t size = 2 * 1024 * 1024;
    memcpy(metadata + 16, &size, 8);
    if (sc_ed25519_sign(a->my_ed25519_privkey, request + DM_MEDIA_STORE_HEADER, t->body_len - DM_MSG_SIG_SIZE,
                       request + DM_MEDIA_STORE_HEADER + t->body_len - DM_MSG_SIG_SIZE) ||
        dm_verify_message(m, t->ids[B], request + DM_MEDIA_STORE_HEADER, t->body_len) || chat_setting_set(m, "dm_media_storage_mb", "1")) return -1;
#ifdef DM_QUOTA_FAULT_INJECTION
    quota_db = m->topo_sqlite_db; quota_rejects = quota_stored = 0;
#endif
    control(t, M, t->ids[S], DM_MEDIA_STORE, request, DM_MEDIA_STORE_HEADER + t->body_len);
    if (value(m, "SELECT count(*) FROM dm_custody") != 0 || chat_setting_set(m, "dm_media_storage_mb", "1024")) return -1;
#ifdef DM_QUOTA_FAULT_INJECTION
    if (quota_rejects != 1 || quota_stored) return -1;
    control(t, M, t->ids[S], DM_MEDIA_STORE, request, DM_MEDIA_STORE_HEADER + t->body_len);
    if (quota_rejects != 2 || quota_stored || value(m, "SELECT count(*) FROM dm_custody")) return -1;
    quota_db = NULL;
    request[0]++; /* Новая попытка после окончательного отказа имеет другой ID. */
    if (quota_failures(t, request, DM_MEDIA_STORE_HEADER + t->body_len)) return -1;
#endif
    char path[768]; snprintf(path, sizeof(path), "%s/cancelled.download", t->dir);
    int calls = 0;
    struct file_transfer_op* op = file_transfer_pull(t->nodes[B]->file_transfer, topo_groups_channel_slot(t->nodes[B]->topo_groups, CHANNEL), t->nodes[A]->my_ed25519_pubkey, t->id,
        dm_media_cipher_size(FILE_BYTES), path, cancelled, &calls);
    if (!op) return -1;
    file_transfer_cancel_object(t->nodes[B]->file_transfer, t->id);
    if (calls != 1) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "media protocol checks passed: SQL rollback, quota, JSON state, CM operation cancellation");
    return 0;
}

/* Таймер служит heartbeat: большие криптозадачи не должны останавливать сценарий. */
/* Одни и те же transfer/custody гарантии для разных типов вложений. */
static int send_fixture(struct media_test* t, enum attachment_kind kind, const char* name) {
    struct attachment_info info = { .kind = kind };
    snprintf(info.name, sizeof(info.name), "%s", name);
    if (kind == ATTACHMENT_VOICE) { info.duration_ms = 3000; memset(info.waveform, 'A', 100); }
    if (kind == ATTACHMENT_VIDEO) { info.duration_ms = 3000; info.width = 640; info.height = 480; }
    return dm_send_file(t->nodes[A], t->conv, t->file, &info, 0, 0);
}

/* Перенести подписанную роль M на автора: личная копия и custody должны жить независимо. */
static int storage_on_author(struct media_test* t) {
    for (int dst = 0; dst < NODES; dst++) for (int src = A; src <= M; src += M - A) {
        uint8_t msg[512], join[64], adm[64], tree[64]; uint64_t ts = ntp_time_get_seconds(t->nodes[dst]);
        int n = member_sync_build_join_msg(t->ch_x, t->ch_ed, t->nodes[src]->my_ed25519_pubkey, t->keys[src].public_key, msg, sizeof(msg));
        if (n < 0 || sc_ed25519_sign(t->nodes[src]->my_ed25519_privkey, msg, (size_t)n, join)) return -1;
        const char* tags = src == A ? "{\"storage\":\"yes\",\"ver\":\"2\"}" : "{\"storage\":\"no\",\"ver\":\"2\"}";
        n = member_sync_build_owner_msg(CHANNEL, t->ed[src], tags, msg, sizeof(msg));
        if (n < 0 || sc_ed25519_sign(t->ch_priv, msg, (size_t)n, adm) ||
            member_sync_sign_join(t->ch_priv, CHANNEL, NULL, t->keys[src].public_key, t->ed[src],
                TEST_JOIN_ID(t->ch_x, t->ch_ed, t->nodes[src]->my_ed25519_pubkey, t->keys[src].public_key), ts, tree) ||
            member_sync_put(t->nodes[dst], CHANNEL, t->keys[src].public_key, t->ed[src], join, ts,
                NULL, 0, NULL, tags, adm, 0, 0, tree) < 0) return -1;
    }
    return chat_setting_set(t->nodes[A], "dm_media_delete_on_open", "1") ||
        chat_setting_set(t->nodes[B], "storage_autoload", "0");
}

static void tick(void* arg) {
    struct media_test* t = arg;
    t->timer = NULL; t->ticks++; t->heartbeats++;
    struct UTUN_INSTANCE* a = t->nodes[A];
    struct UTUN_INSTANCE* b = t->nodes[B];
    struct UTUN_INSTANCE* s = t->nodes[S];
    struct UTUN_INSTANCE* m = t->nodes[M];
    switch (t->phase) {
    case 0:
        if (instance_find_conn(a, t->ids[S]) && instance_find_conn(b, t->ids[S]) && instance_find_conn(m, t->ids[S])) {
            if (setup(t)) { failure(t, "signed fixture setup"); return; }
            t->phase++;
        }
        break;
    case 1:
        if (live(a, t->ids[B]) && live(m, t->ids[A])) {
            if (send_fixture(t, ATTACHMENT_VOICE, "direct.opus")) { failure(t, "direct voice enqueue"); return; }
            t->phase++;
        }
        break;
    case 2:
        if (value(a, "SELECT count(*) FROM dm_files WHERE dir=1 AND receipt IS NOT NULL") == 1) {
            if (snapshot(t, 1) || !file_correct(t, B) || value(s, "SELECT count(*) FROM dm_media_jobs") != 0 ||
                value(m, "SELECT count(*) FROM dm_custody") != 0) { failure(t, "direct transfer/custody isolation"); return; }
            uint8_t bad[DM_MEDIA_RECEIPT_SIZE]; memcpy(bad, t->receipt, sizeof(bad)); bad[56] ^= 1;
            if (!dm_media_verify_receipt(a->topo_sqlite_db, bad, t->body, t->body_len)) { failure(t, "forged media receipt accepted"); return; }
            int protocol = protocol_checks(t), sources = protocol ? 0 : source_withdraw_checks(t);
            if (protocol || sources) {
                DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "DM media invariants: protocol=%d sources=%d", protocol, sources);
                failure(t, "media protocol invariants"); return;
            }
#ifdef DM_QUOTA_FAULT_INJECTION
            quota_db = m->topo_sqlite_db; quota_sequence = 1;
            quota_hits = quota_attempts = quota_rejects = quota_stored = 0;
#endif
            utun_instance_destroy(b); t->nodes[B] = NULL; t->phase++;
        }
        break;
    case 3:
        if (!live(a, t->ids[B])) {
            if (send_fixture(t, ATTACHMENT_VIDEO, "offline.mp4")) { failure(t, "offline video enqueue"); return; }
            t->phase++;
        }
        break;
    case 4:
#ifdef DM_QUOTA_FAULT_INJECTION
        if (quota_rejects || value(s, "SELECT count(*) FROM dm_media_holders WHERE released<>0")) {
            failure(t, "transient quota read failure permanently rejected M"); return;
        }
#endif
        if (value(s, "SELECT count(*) FROM dm_media_jobs WHERE stored=1") == 1 && value(m, "SELECT count(*) FROM dm_custody WHERE ready=1") == 1) {
#ifdef DM_QUOTA_FAULT_INJECTION
            if (quota_hits != 3 || quota_attempts != 4 || !quota_stored ||
                value(s, "SELECT count(*) FROM dm_media_holders WHERE released=0") != 1) {
                failure(t, "custody did not recover through four STORE attempts on the same M"); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM quota test: recovered hits=%u STORE=%u REJECT=%u STORED=%u holders=1",
                       quota_hits, quota_attempts, quota_rejects, quota_stored);
            quota_db = NULL;
            quota_sequence = 0;
#endif
            if (snapshot(t, 2)) { failure(t, "offline body snapshot"); return; }
#ifdef DM_QUOTA_FAULT_INJECTION
            if (release_failures(t)) { failure(t, "atomic storage release"); return; }
#endif
            utun_instance_destroy(a); t->nodes[A] = NULL;
            utun_instance_destroy(m); t->nodes[M] = NULL;
            utun_instance_destroy(s); t->nodes[S] = NULL;
            if (start_node(t, S) || start_node(t, M) || start_node(t, B)) { failure(t, "custody restart"); return; }
#ifdef DM_QUOTA_FAULT_INJECTION
            retire_db = t->nodes[M]->topo_sqlite_db;
            retire_fault = RETIRE_INSERT_PREPARE; retire_drop_ack = 1;
#endif
            t->phase++;
        }
        break;
    case 5:
#ifdef DM_QUOTA_FAULT_INJECTION
        if (!retire_restarted) {
            if (!retire_hits) break;
            if (retire_acks || !file_correct(t, B) || !sqlite3_get_autocommit(retire_db) ||
                value(m, "SELECT count(*) FROM dm_custody WHERE ready=-1 AND receipt IS NOT NULL") != 1 ||
                value(m, "SELECT count(*) FROM dm_custody_done") ||
                value(s, "SELECT count(*) FROM dm_media_jobs WHERE receipt IS NOT NULL AND body IS NOT NULL") != 1 ||
                value(s, "SELECT count(*) FROM dm_media_holders WHERE released=0") != 1) {
                failure(t, "failed retirement emitted ACK or lost durable custody/tombstone state"); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM retirement test: fault=%d rollback intact; no ACK PASS", retire_fault);
            retire_checks++; retire_hits = 0;
            if (retire_fault < RETIRE_COMMIT) { retire_fault++; break; }
            retire_fault = RETIRE_NONE; retire_db = NULL;
            utun_instance_destroy(m); t->nodes[M] = NULL;
            utun_instance_destroy(s); t->nodes[S] = NULL;
            if (start_node(t, S) || start_node(t, M)) { failure(t, "retirement retry restart"); return; }
            retire_db = t->nodes[M]->topo_sqlite_db; retire_restarted = 1;
            break;
        }
        if (retire_restarted == 1) {
            if (!retire_acks || value(m, "SELECT count(*) FROM dm_custody_done WHERE receipt IS NOT NULL") != 1) break;
            if (retire_checks != RETIRE_COMMIT || value(m, "SELECT count(*) FROM dm_custody") ||
                value(s, "SELECT count(*) FROM dm_media_jobs WHERE receipt IS NOT NULL AND body IS NOT NULL") != 1 ||
                value(s, "SELECT count(*) FROM dm_media_holders WHERE released=0") != 1) {
                failure(t, "lost DELETE_ACK released S before retry"); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM retirement test: 11 SQL faults, restart M, lost ACK; restarting S for replay");
            retire_drop_ack = 0;
            utun_instance_destroy(s); t->nodes[S] = NULL;
            if (start_node(t, S)) { failure(t, "lost DELETE_ACK coordinator restart"); return; }
            retire_restarted = 2;
            break;
        }
#endif
        if (file_correct(t, B) && value(m, "SELECT count(*) FROM dm_custody") == 0 &&
            value(s, "SELECT count(*) FROM dm_media_jobs WHERE receipt IS NOT NULL AND body IS NULL") == 1) {
            char json[8192]; size_t length;
            if (dm_list_messages_json(b, t->conv, 20, 0, json, sizeof(json), &length) ||
                !strstr(json, "\"content_type\":\"video/mp4\"") || !strstr(json, "\"duration_ms\":3000") ||
                !strstr(json, "\"width\":640") || !strstr(json, "\"height\":480")) {
                failure(t, "encrypted video metadata lost after offline custody restart"); return;
            }
            uint8_t stored[41] = {0};
            if (assignment_id(t, stored)) { failure(t, "delivered assignment snapshot"); return; }
            memcpy(stored + 16, t->id, 16);
            uint64_t expiry = ntp_time_get_seconds(s) + 100; memcpy(stored + 32, &expiry, 8);
            control(t, S, t->ids[M], DM_MEDIA_STORED, stored, sizeof(stored));
            control(t, S, t->ids[M], DM_MEDIA_EXPIRED, stored, 32);
            uint8_t put[DM_MEDIA_STORE_HEADER + sizeof(t->body)]; memcpy(put, stored, 16);
            memcpy(put + DM_MEDIA_STORE_RECIPIENT_OFF, t->ed[B], 32); memcpy(put + DM_MEDIA_STORE_HEADER, t->body, t->body_len);
            control(t, M, t->ids[S], DM_MEDIA_STORE, put, DM_MEDIA_STORE_HEADER + t->body_len);
            if (value(m, "SELECT count(*) FROM dm_custody") != 0 ||
                value(s, "SELECT count(*) FROM dm_media_jobs WHERE receipt IS NULL") != 0) { failure(t, "late custody resurrected artifact"); return; }
            if (start_node(t, A)) { failure(t, "source restart"); return; }
            t->phase++;
        }
        break;
    case 6:
        if (live(a, t->ids[B]) && value(a, "SELECT count(*) FROM dm_files WHERE receipt IS NOT NULL") == 2) {
            utun_instance_destroy(b); t->nodes[B] = NULL;
            if (chat_setting_set(m, "dm_media_ttl_sec", "3600")) { failure(t, "TTL setting"); return; }
#ifdef DM_QUOTA_FAULT_INJECTION
            release_db = s->topo_sqlite_db; expiry_storage_db = m->topo_sqlite_db; expiry_block = 1;
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM expiry test: lost EXPIRED and SQL failures enabled until M retires custody");
#endif
            t->phase++;
        }
        break;
    case 7:
        if (!live(a, t->ids[B])) {
            if (send_fixture(t, ATTACHMENT_FILE, "expires.bin")) { failure(t, "TTL file enqueue"); return; }
            t->phase++;
        }
        break;
    case 8:
        if (!t->expiry_armed) {
            if (value(s, "SELECT count(*) FROM dm_media_jobs WHERE seq=3 AND stored=1") != 1 ||
                value(m, "SELECT count(*) FROM dm_custody WHERE ready=1") != 1) break;
            if (snapshot(t, 3) || expire_fixture(t)) { failure(t, "confirmed custody expiry setup"); return; }
            t->expiry_armed = 1;
            break;
        }
        if (value(m, "SELECT count(*) FROM dm_custody_done WHERE receipt IS NULL") == 1) {
#ifdef DM_QUOTA_FAULT_INJECTION
            if (!expiry_restarted) {
                if (!expiry_notices || !expiry_blocks) break;
                sqlite3_int64 state[7];
                if (snapshot(t, 3) || release_state(t, state) || state[0] != (sqlite3_int64)t->ids[M] ||
                    state[1] != 1 || state[2] || state[5] || !sqlite3_get_autocommit(release_db)) {
                    failure(t, "timer release failure left partial state"); return;
                }
                DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM expiry test: M retired; SQL rollback intact blocks=%u lost EXPIRED=%u; restart S",
                           expiry_blocks, expiry_notices);
                expiry_block = 0; release_db = NULL;
                utun_instance_destroy(s); t->nodes[S] = NULL;
                if (start_node(t, S)) { failure(t, "expiry coordinator restart"); return; }
                release_db = t->nodes[S]->topo_sqlite_db; expiry_restarted = 1;
                break;
            }
#endif
            if (value(m, "SELECT count(*) FROM dm_custody") != 0 || value(s, "SELECT count(*) FROM dm_media_jobs WHERE expired=1") != 1) break;
            if (!instance_find_conn(s, t->ids[M])) break;
            if (snapshot(t, 3)) { failure(t, "expired body snapshot"); return; }
            uint8_t stored[41] = {0};
            if (assignment_id(t, stored)) { failure(t, "expired assignment snapshot"); return; }
            memcpy(stored + 16, t->id, 16);
            uint64_t expiry = ntp_time_get_seconds(s) + 100; memcpy(stored + 32, &expiry, 8);
            control(t, S, t->ids[M], DM_MEDIA_STORED, stored, sizeof(stored));
            uint8_t put[DM_MEDIA_STORE_HEADER + sizeof(t->body)]; memcpy(put, stored, 16);
            memcpy(put + DM_MEDIA_STORE_RECIPIENT_OFF, t->ed[B], 32); memcpy(put + DM_MEDIA_STORE_HEADER, t->body, t->body_len);
            control(t, M, t->ids[S], DM_MEDIA_STORE, put, DM_MEDIA_STORE_HEADER + t->body_len);
            if (value(m, "SELECT count(*) FROM dm_custody") != 0 ||
                value(s, "SELECT count(*) FROM dm_media_jobs WHERE receipt IS NULL AND (stored<>0 OR holder IS NOT NULL OR expired<>1)") ||
                value(s, "SELECT count(*) FROM dm_media_jobs WHERE receipt IS NOT NULL") != 1) {
                failure(t, "late STORE/STORED extended TTL or forged delivery"); return;
            }
#ifdef DM_QUOTA_FAULT_INJECTION
            sqlite3_int64 state[7];
            if (release_checks != 18 || !expiry_restarted || !expiry_notices || !expiry_blocks ||
                release_state(t, state) || state[5] != 2 || !sqlite3_get_autocommit(release_db)) {
                failure(t, "expiry did not recover after SQL failure and lost control"); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM expiry test: release checks=%u blocked=%u lost EXPIRED=%u restart=1 recovered PASS",
                       release_checks, expiry_blocks, expiry_notices);
#endif
            utun_instance_destroy(a); t->nodes[A] = NULL;
            if (start_node(t, B)) { failure(t, "receiver return after TTL"); return; }
            t->phase++;
        }
        break;
    case 9:
        if (value(b, "SELECT count(*) FROM dm_files WHERE dir=0 AND seq=3 AND ready=0 AND receipt IS NULL") == 1 &&
            value(b, "SELECT count(*) FROM dm_file_sources WHERE expires=-1") == 1) {
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM recovery test: custody source gone while A offline; returning original author");
            if (start_node(t, A)) { failure(t, "source return after EXPIRED"); return; }
            t->phase++;
        }
        break;
    case 10:
        if (live(a, t->ids[B]) && live(b, t->ids[A])) {
            t->recovery_start = ntp_time_get_seconds(b);
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM recovery test: A/B routes restored; waiting for verified original");
            t->phase++;
        }
        break;
    case 11:
        if (file_correct(t, B) && value(a, "SELECT count(*) FROM dm_files WHERE dir=1 AND seq=3 AND receipt IS NOT NULL") == 1 &&
            value(s, "SELECT count(*) FROM dm_media_jobs WHERE seq=3 AND receipt IS NOT NULL AND body IS NULL") == 1) {
            if (dm_media_verify_receipt(a->topo_sqlite_db, t->receipt, t->body, t->body_len)) {
                failure(t, "recovered file receipt does not match original body"); return;
            }
            uint8_t gone[DM_MEDIA_GONE_SIZE]; memcpy(gone, t->id, 16); SHA256(t->body, t->body_len, gone + 16); memcpy(gone + DM_MEDIA_GONE_HOLDER_OFF, t->ed[M], 32);
            control(t, B, t->ids[S], DM_MEDIA_SOURCE_GONE, gone, sizeof(gone));
            if (!file_correct(t, B) || value(m, "SELECT count(*) FROM dm_custody") ||
                value(b, "SELECT count(*) FROM dm_files WHERE dir=0 AND seq=3 AND ready=1 AND receipt IS NOT NULL") != 1) {
                failure(t, "late EXPIRED changed verified file or revived custody"); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM recovery test: exact original, signed receipt, S cleanup, late EXPIRED PASS");
            if (storage_on_author(t) || send_fixture(t, ATTACHMENT_FILE, "manual.bin")) {
                failure(t, "colocated storage fixture"); return;
            }
            t->phase++; break;
        }
        if (ntp_time_get_seconds(b) >= t->recovery_start + 16) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM recovery test: timeout A_to_B=%d B_to_A=%d B_ready=%d B_receipt=%d",
                        live(a, t->ids[B]), live(b, t->ids[A]), value(b, "SELECT ready FROM dm_files WHERE dir=0 AND seq=3"),
                        value(b, "SELECT count(*) FROM dm_files WHERE dir=0 AND seq=3 AND receipt IS NOT NULL"));
            failure(t, "available original cannot complete receive after EXPIRED"); return;
        }
        break;
    case 12:
        if (value(a, "SELECT count(*) FROM dm_custody WHERE ready=1 AND delete_on_open=1") == 1 &&
            value(b, "SELECT count(*) FROM dm_files WHERE seq=4 AND ready=0 AND receipt IS NULL") == 1) {
            if (snapshot(t, 4)) { failure(t, "manual file snapshot"); return; }
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM colocation: manual B remained pending, A stored independent ciphertext; restart A");
            utun_instance_destroy(a); t->nodes[A] = NULL;
            if (start_node(t, A)) { failure(t, "colocated A restart"); return; }
            t->phase++;
        }
        break;
    case 13:
        if (live(b, t->ids[A]) && live(a, t->ids[B])) {
            if (value(a, "SELECT count(*) FROM dm_custody WHERE ready=1 AND delete_on_open=1") != 1 ||
                chat_setting_get_int(a, "dm_media_delete_on_open", -1) != 0 ||
                dm_media_user_action(b, t->conv, t->ed[A], 4, 0)) { failure(t, "frozen policy/manual download"); return; }
            t->phase++;
        }
        break;
    case 14:
        if (file_correct(t, B) && value(a, "SELECT count(*) FROM dm_files WHERE seq=4 AND receipt IS NOT NULL") == 1) {
            if (value(a, "SELECT count(*) FROM dm_custody WHERE ready=1 AND delete_on_open=1") != 1) {
                failure(t, "FILE_RECEIVED deleted delete-on-open custody"); return;
            }
            if (dm_media_user_action(b, t->conv, t->ed[A], 4, 1)) { failure(t, "user open acknowledgement"); return; }
            t->phase++;
        }
        break;
    case 15:
        if (value(a, "SELECT count(*) FROM dm_custody") == 0 &&
            value(s, "SELECT count(*) FROM dm_media_jobs WHERE seq=4 AND receipt IS NOT NULL AND body IS NULL") == 1) {
            if (!file_correct(t, B) || value(a, "SELECT count(*) FROM dm_files WHERE seq=4 AND ready=1") != 1 ||
                value(a, "SELECT count(*) FROM dm_custody_done WHERE delete_on_open=1") != 1) {
                failure(t, "server deletion changed personal replica"); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM colocation: frozen policy survived restart, manual download != opening, TAKEN retired only custody PASS");
            sqlite3_stmt* st = NULL;
            if (sqlite3_prepare_v2(a->topo_sqlite_db, "SELECT local_path FROM dm_files WHERE seq=4", -1, &st, NULL)) {
                failure(t, "source recovery fixture prepare"); return;
            }
            int removed = sqlite3_step(st) == SQLITE_ROW && !remove((const char*)sqlite3_column_text(st, 0));
            sqlite3_finalize(st);
            if (!removed || dm_media_user_action(a, t->conv, t->ed[A], 4, 0)) { failure(t, "source recovery request"); return; }
            t->phase++;
        }
        break;
    case 16:
        if (file_correct(t, A)) {
            if (value(a, "SELECT count(*) FROM dm_custody") || !file_correct(t, B)) {
                failure(t, "source recovery changed receiver/custody"); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "DM source recovery: downloaded verified replica without creating own FILE_RECEIVED PASS");
            t->phase++; uasync_stop(t->ua); return;
        }
        break;
    }
    if (t->failed) return;
    if (t->ticks > 18000) { failure(t, "timeout"); return; }
    t->timer = uasync_set_timeout(t->ua, 100, t, tick, "dm_media_test");
}

int main(void) {
    struct media_test t = {0};
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    size_t allocations = u_get_allocated_count();
    test_clock_init(1000); test_clock_sync_wall_time();
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level_by_name("dm", "debug"); debug_set_category_level_by_name("media", "debug");
        debug_set_category_level_by_name("bgp", "info"); debug_set_category_level_by_name("etcp_route", "info");
        debug_set_category_level_by_name("connection", "debug"); debug_set_category_level_by_name("etcp", "info");
    }
    utun_instance_set_tun_init_enabled(0);
    snprintf(t.dir, sizeof(t.dir), "/tmp/utun_dm_media_XXXXXX");
    if (test_mkdtemp(t.dir)) return 1;
    for (int i = 0; i < NODES; i++) {
        if (sc_identity_generate(t.keys[i].public_key, t.keys[i].private_key, t.ed[i], t.seed[i])) return 1;
        t.ids[i] = sc_derive_node_id_from_pubkey(t.keys[i].public_key);
    }
    uint8_t channel_x_private[32];
    t.ch_priv[0] = 94;
    if (sc_identity_from_seed(t.ch_priv, t.ch_x, channel_x_private, t.ch_ed) ||
        test_group_signature(CHANNEL, "dm-media-test", t.ed[A], t.ch_x, t.ch_ed, t.ch_priv, t.ch_sig)) return 1;
    snprintf(t.conv, sizeof(t.conv), "%llu", (unsigned long long)dm_derive_conv_id(t.ed[A], t.ed[B]));
    snprintf(t.file, sizeof(t.file), "%s/source.bin", t.dir);
    FILE* f = fopen(t.file, "wb");
    if (!f) return 1;
    for (unsigned i = 0; i < FILE_BYTES; i++) fputc((int)((i * 73 + 19) & 255), f);
    fclose(f);
    t.ua = uasync_create();
    if (!t.ua || start_node(&t, S) || start_node(&t, A) || start_node(&t, M) || start_node(&t, B)) return 1;
    t.timer = uasync_set_timeout(t.ua, 100, &t, tick, "dm_media_test");
    test_clock_mainloop(t.ua);
#ifdef DM_QUOTA_FAULT_INJECTION
    quota_db = NULL; release_db = NULL; expiry_storage_db = NULL; retire_db = NULL;
#endif
    for (int i = 0; i < NODES; i++) if (t.nodes[i]) utun_instance_destroy(t.nodes[i]);
    uasync_destroy(t.ua, 0);
    test_clock_finish();
    printf("DM media allocations: %zu->%zu\n", allocations, u_get_allocated_count());
    if (allocations != u_get_allocated_count()) t.failed = 1;
    printf("test_dm_media: phase=%u heartbeats=%u %s artifacts=%s\n", t.phase, t.heartbeats,
           !t.failed && t.phase == 17 ? "PASS" : "FAIL", t.dir);
    return t.failed || t.phase != 17;
}
