// test_media_delivery_chat.c — сквозной тест распространения медиа через db_sync
//
// 6 узлов в цепочку:
//   n1(0) — n2(1) — s1(2) — s2(3) — st1(4) — st2(5)
//   автор     клиент   супер    супер    storage  storage
//
// Сценарий:
//   1. n1 индексирует медиафайл (media_index_commit) и публикует сообщение с метаданными
//      через db_sync (реальная репликация по цепочке каскадом).
//   2. storage-узлы (st1/st2) по insert-callback'у запускают auto-download
//      (media_download_start), скачивают блоки у автора, собирают файл.
//   3. Проверяем, что суперузлы (s1/s2) узнали о блоках (block_availability от HAVE_BLOCK)
//      и отреплицировались между собой (super_sync.last_recv_id > 0).
//   4. Второй клиент n2 скачивает медиа «по запросу» и собирает файл.
//
// Используется настоящий db_sync для доставки сообщения и настоящий media-движок
// для доставки блоков. Связка «сообщение → auto-download» задаётся тестовым insert-callback'ом;
// per-instance chat_core хранит подписки на роли и общую БД каждого узла.

#include "test_join_id.h"
#include "media_delivery.h"
#include <assert.h>
#include "media_delivery_proto.h"
#include "media_download.h"
#include "media_index.h"
#include "member_sync.h"
#include "../chat/chat_core.h"
#include "db_sync.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/secure_channel.h"
#include "../config_parser.h"
#include "../config_updater.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_net.h"
#include "test_utils.h"
#include "db_sync_fixture.h"

#include <sqlite3.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include "test_membership.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

static int G_PASSED = 0, G_FAILED = 0, G_TOTAL = 0;
#define TEST(n)  do { G_TOTAL++; printf("  %-58s", n); fflush(stdout); } while(0)
#define OK()     do { G_PASSED++; printf("OK\n"); } while(0)
#define FAIL(f,...) do { G_FAILED++; printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

#define TIMEOUT_TB 1200000   /* 120 s общий таймаут */
#define POLL_MS    5
#define N_NODES    6
#define CH_ID      "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951" /* Ed25519 для seed {94}. */

/* роли узлов */
#define I_N1  0   /* автор */
#define I_N2  1   /* клиент (скачивает позже) */
#define I_S1  2   /* суперузел */
#define I_S2  3   /* суперузел */
#define I_ST1 4   /* storage */
#define I_ST2 5   /* storage */

#define MEDIA_FNAME "photo.bin"
#define MEDIA_FILE_SIZE  15360
#define MEDIA_BLOCK_SIZE 5120
#define MEDIA_NUM_BLOCKS 3

static struct UASYNC* g_ua = NULL;
static struct UTUN_INSTANCE* g_inst[N_NODES];
static uint64_t g_nid[N_NODES];
static struct DB_SYNC_INSTANCE* g_si[N_NODES];
static uint64_t g_group_id = 0;

static char g_tdir[512] = "/tmp/utun_mdc_XXXXXX";
static char g_cfg[N_NODES][640];
static char g_db_dir[N_NODES][640];
static int  g_connected = 0, g_result = 0;

/* опубликованное медиа (автор) — глобально для n2-загрузки и проверок */
static uint8_t g_media_id[16];
static uint8_t g_content_hash[32];
static uint8_t g_block_ids[MEDIA_NUM_BLOCKS][16];
static uint8_t g_block_sigs[MEDIA_NUM_BLOCKS][64];
static uint8_t g_file_data[MEDIA_FILE_SIZE];

/* флаги завершения загрузок */
static int g_dl_done[N_NODES];
static int g_dl_err[N_NODES];

/* ── helpers ── */

static int wf(const char* p, const char* f, ...) {
    va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1;
    va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0;
}
static char* gv(const char* p, const char* k) {
    struct utun_config* c = parse_config(p); if (!c) return NULL;
    char* r = (strcmp(k, "pub") == 0) ? u_strdup(c->global.my_public_key_hex) : u_strdup(c->global.my_private_key_hex);
    free_config(c); return r;
}
static int db_count(sqlite3* db, const char* tbl, const char* wh, uint64_t val) {
    char sql[256];
    if (wh && !strcmp(wh, "group_id")) snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE group_id=\'%s\'", tbl, CH_ID);
    else if (wh) snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE %s=%llu", tbl, wh, (unsigned long long)val);
    else snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", tbl);
    sqlite3_stmt* s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    int n = -1;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s); return n;
}

static void to_cb(void* arg) { (void)arg; fprintf(stderr, "  TIMEOUT\n"); g_result = 2; }

static void mon(void* arg) {
    (void)arg;
    if (g_result) return;
    int ok = 0;
    for (int i = 0; i < N_NODES; i++) {
        struct ll_entry* e = g_inst[i]->connections->head;
        while (e) { struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
            if (ce->conn && ce->conn->links) { struct ETCP_LINK* l; for (l = ce->conn->links; l; l = l->next)
              if (l->initialized && ce->conn->crypto_ctx.initialized) ok++; } e = e->next; }
    }
    if (ok >= N_NODES * (N_NODES - 1)) g_connected = 1;   /* полный mesh: N*(N-1) линков */
    if (!g_result) uasync_set_timeout(g_ua, 10, NULL, mon, "mdc_mon");
}

/* pubkey узла известен в in-memory registry (BGP сошёлся) */
static int pubkey_known(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    struct TOPO_NODE* ni = inst && inst->topo_groups ? topo_node_registry_find(inst->topo_groups, node_id) : NULL;
    if (!ni) return 0;
    uint64_t chk; memcpy(&chk, ni->ed25519_public_key, 8);
    return chk != 0;
}

/* ждём, пока все узлы узнают ed25519-ключи всех остальных (нужно db_sync для верификации подписи) */
static int wait_pubkeys(void) {
    int a = 0;
    while (a < 4000) {
        int ok = 1;
        for (int i = 0; i < N_NODES; i++)
            for (int m = 0; m < N_NODES; m++)
                if (m != i && !pubkey_known(g_inst[i], g_nid[m])) ok = 0;
        if (ok) return 1;
        uasync_poll(g_ua, POLL_MS); a++;
    }
    return 0;
}

/* ждём, пока BGP CHAT-группы сойдутся: каждый узел имеет маршрут (path) до каждого
 * другого мембера в своей CHAT-группе. Без этого media-доставка «через группу» не имеет
 * маршрута (topo_group_find_conn_for_node == NULL) и встаёт с «no route». */
static int wait_chat_bgp(void) {
    int a = 0;
    while (a < 8000) {
        int ok = 1;
        for (int i = 0; i < N_NODES && ok; i++) {
            struct TOPO_GROUP* g = topo_groups_find(g_inst[i]->topo_groups, g_group_id);
            if (!g) { ok = 0; break; }
            for (int m = 0; m < N_NODES; m++) {
                if (m == i) continue;
                struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(g, g_nid[m]);
                if (!nq || !nq->paths || !nq->paths->head) { ok = 0; break; }
            }
        }
        if (ok) return 1;
        uasync_poll(g_ua, POLL_MS); a++;
    }
    return 0;
}

static void sha256_buf(const uint8_t* data, size_t len, uint8_t out[32]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, out, NULL);
    EVP_MD_CTX_free(ctx);
}

static int file_sha256(const char* path, uint8_t out[32]) {
    FILE* f = fopen(path, "rb"); if (!f) return -1;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    uint8_t buf[8192]; size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) EVP_DigestUpdate(ctx, buf, rd);
    EVP_DigestFinal_ex(ctx, out, NULL);
    EVP_MD_CTX_free(ctx); fclose(f);
    return 0;
}

static int file_exists(const char* p) { return access(p, F_OK) == 0; }

/* nodeinfo_updated пересчитывает node_type по адресам и может сбросить тип суперузла,
 * если узел не имеет DIRECT-адреса. Перед каждой загрузкой принудительно выставляем
 * node_type=4 для s1/s2 (md_dl_collect_supernodes читает именно это поле). */
static void ensure_supernode_type(int idx) {
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE \"peers_%s\" SET node_type=4 WHERE node_id IN (%llu,%llu)",
             CH_ID, (unsigned long long)g_nid[I_S1], (unsigned long long)g_nid[I_S2]);
    sqlite3_exec(g_inst[idx]->topo_sqlite_db, sql, NULL, NULL, NULL);
}

/* ── завершение загрузки ── */

static void dl_done_cb(void* arg, int err) {
    intptr_t idx = (intptr_t)arg;
    g_dl_done[idx] = 1; g_dl_err[idx] = err;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "[mdc] download done node=%d err=%d", (int)idx, err);
}

/* media_base узла = каталог db_path (как chat_core/md_start_download) */
static void node_media_base(int idx, char* out, size_t sz) { snprintf(out, sz, "%s", g_db_dir[idx]); }
static void node_dest(int idx, char* out, size_t sz) {
    snprintf(out, sz, "%s/media/%s/%s", g_db_dir[idx], CH_ID, MEDIA_FNAME);
}

/* ── парсинг тела медиа-сообщения (как md_start_download в chat_msg.c) ── */

static int hex_byte(const char* p) {
    int v = 0;
    for (int i = 0; i < 2; i++) { char c = p[i]; v <<= 4;
        v |= (c >= '0' && c <= '9') ? (c - '0') : (c >= 'a' && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10); }
    return v;
}

static int parse_media_body(const char* body, struct media_index_result* out) {
    const char* p = body;
    while (*p && *p != '|') p++;
    if (!*p) return -1; p++;
    out->file_size = strtoll(p, (char**)&p, 10);
    if (*p != '|') return -1; p++;
    out->block_size = strtoll(p, (char**)&p, 10);
    if (*p != '|') return -1; p++;
    int nb = (int)strtol(p, (char**)&p, 10);
    while (*p && *p != '|') p++;
    if (!*p) return -1; p++;
    if (nb <= 0 || nb > 100 || out->file_size <= 0) return -1;

    for (int i = 0; i < 16; i++) out->media_id[i] = (uint8_t)hex_byte(p + i * 2);
    p += 32; if (*p != '|') return -1; p++;
    for (int i = 0; i < 32; i++) out->content_hash[i] = (uint8_t)hex_byte(p + i * 2);
    p += 64; if (*p != '|') return -1; p++;

    out->num_blocks = nb;
    out->block_ids = u_malloc((size_t)nb * 16);
    out->block_sigs = u_malloc((size_t)nb * 64);
    if (!out->block_ids || !out->block_sigs) { media_index_result_free(out); return -1; }
    for (int n = 0; n < nb; n++) {
        for (int i = 0; i < 16; i++) out->block_ids[n * 16 + i] = (uint8_t)hex_byte(p + i * 2);
        p += 32; if (*p != ',') { media_index_result_free(out); return -1; } p++;
        for (int i = 0; i < 64; i++) out->block_sigs[n * 64 + i] = (uint8_t)hex_byte(p + i * 2);
        p += 128;
        if (n < nb - 1 && *p == ',') p++;
    }
    return 0;
}

/* ── insert-callback: storage-узлы запускают auto-download ── */

static void storage_on_insert(struct DB_SYNC_INSTANCE* si, const uint8_t message_id[32], uint64_t ts,
                              const char* data, size_t len, uint64_t author, const uint8_t author_ed[32],
                              int initial_sync, int updated, void* arg) {
    (void)si; (void)message_id; (void)ts; (void)initial_sync; (void)updated;
    int idx = (int)(intptr_t)arg;
    struct UTUN_INSTANCE* inst = g_inst[idx];
    if (!memcmp(author_ed, inst->my_ed25519_pubkey, 32)) return;

    char buf[8192]; size_t blen = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, blen); buf[blen] = '\0';
    const char* ct = strstr(buf, "\"ct\":\"");
    const char* dpos = strstr(buf, "\"d\":\"");
    if (!ct || !dpos) return;

    const char* ct_val = ct + 6;
    char content_type[32] = {0}; int cti = 0;
    while (ct_val[cti] && ct_val[cti] != '\"' && cti < (int)sizeof(content_type) - 1) content_type[cti++] = ct_val[cti];

    const char* d_start = dpos + 5;
    char body[8192]; size_t bi = 0;
    while (*d_start && *d_start != '\"' && bi < sizeof(body) - 1) body[bi++] = *d_start++;
    body[bi] = '\0';

    struct media_index_result result;
    memset(&result, 0, sizeof(result));
    if (parse_media_body(body, &result) != 0) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "[mdc] parse_media_body failed"); return; }

    char dest[1024], mbase[512];
    node_media_base(idx, mbase, sizeof(mbase));
    node_dest(idx, dest, sizeof(dest));

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "[mdc] auto-download on node=%d ch=%s ct=%s blocks=%d author=0x%016llx dest=%s",
               idx, CH_ID, content_type, result.num_blocks, (unsigned long long)author, dest);
    ensure_supernode_type(idx);
    media_download_start(inst, g_group_id, &result, dest, mbase, author_ed, dl_done_cb, (void*)(intptr_t)idx, NULL, NULL);
    media_index_result_free(&result);
}

/* ── публикация медиа автором ── */

static int author_index_file(void) {
    static unsigned file_number;
    char media_dir[512]; snprintf(media_dir, sizeof(media_dir), "%s/media", g_db_dir[I_N1]);
    utun_mkdir(media_dir, 0755);
    char dst_path[512]; snprintf(dst_path, sizeof(dst_path), "%s/%u-%s", media_dir, ++file_number, MEDIA_FNAME);
    char media_base[512]; snprintf(media_base, sizeof(media_base), "%s", g_db_dir[I_N1]);

    FILE* f = fopen(dst_path, "wb"); if (!f) return -1;
    fwrite(g_file_data, 1, MEDIA_FILE_SIZE, f); fclose(f);

    sha256_buf(g_file_data, MEDIA_FILE_SIZE, g_content_hash);

    struct media_index_result result; memset(&result, 0, sizeof(result));
    media_index_init(g_inst[I_N1]->topo_sqlite_db);
    assert(media_index_generate_uuid(result.media_id) == 0);
    memcpy(result.content_hash, g_content_hash, 32);
    result.file_size = MEDIA_FILE_SIZE; result.block_size = MEDIA_BLOCK_SIZE; result.num_blocks = MEDIA_NUM_BLOCKS;
    result.block_ids = u_malloc(MEDIA_NUM_BLOCKS * 16);
    result.block_sigs = u_malloc(MEDIA_NUM_BLOCKS * 64);
    for (int n = 0; n < MEDIA_NUM_BLOCKS; n++) {
        assert(media_index_generate_uuid(result.block_ids + n * 16) == 0);
        int off = n * MEDIA_BLOCK_SIZE;
        int sz = (n == MEDIA_NUM_BLOCKS - 1) ? MEDIA_FILE_SIZE - off : MEDIA_BLOCK_SIZE;
        /* подпись блока = Ed25519(данные блока) — ровно как проверяет download */
        sc_ed25519_sign(g_inst[I_N1]->my_ed25519_privkey, g_file_data + off, (size_t)sz, result.block_sigs + n * 64);
    }
    int rc = media_index_commit(g_inst[I_N1]->topo_sqlite_db, &result, g_inst[I_N1]->my_ed25519_pubkey,
        g_inst[I_N1]->my_ed25519_privkey, CH_ID, dst_path, media_base);

    /* ui_state.media_base — откуда block_req-хендлер открывает файл на стриминг */
    {
        sqlite3_exec(g_inst[I_N1]->topo_sqlite_db, "CREATE TABLE IF NOT EXISTS ui_state(key TEXT PRIMARY KEY, value TEXT)", NULL, NULL, NULL);
        sqlite3_stmt* us = NULL;
        sqlite3_prepare_v2(g_inst[I_N1]->topo_sqlite_db, "INSERT OR REPLACE INTO ui_state(key,value) VALUES('media_base',?)", -1, &us, NULL);
        sqlite3_bind_text(us, 1, media_base, -1, SQLITE_STATIC); sqlite3_step(us); sqlite3_finalize(us);
    }

    /* сохраняем глобально */
    memcpy(g_media_id, result.media_id, 16);
    for (int n = 0; n < MEDIA_NUM_BLOCKS; n++) {
        memcpy(g_block_ids[n], result.block_ids + n * 16, 16);
        memcpy(g_block_sigs[n], result.block_sigs + n * 64, 64);
    }
    media_index_result_free(&result);
    return rc;
}

static int author_publish_message(void) {
    /* body: <base>|<fsize>|<bsize>|<nb>|<mid_hex>|<hash_hex>|<id0>,<sig0>,... */
    char body[8192]; size_t off = 0;
    off += snprintf(body + off, sizeof(body) - off, "%s|%d|%d|%d|",
                    MEDIA_FNAME, MEDIA_FILE_SIZE, MEDIA_BLOCK_SIZE, MEDIA_NUM_BLOCKS);
    for (int i = 0; i < 16; i++) off += snprintf(body + off, sizeof(body) - off, "%02x", g_media_id[i]);
    off += snprintf(body + off, sizeof(body) - off, "|");
    for (int i = 0; i < 32; i++) off += snprintf(body + off, sizeof(body) - off, "%02x", g_content_hash[i]);
    off += snprintf(body + off, sizeof(body) - off, "|");
    for (int n = 0; n < MEDIA_NUM_BLOCKS; n++) {
        if (n > 0) body[off++] = ',';
        for (int i = 0; i < 16; i++) off += snprintf(body + off, sizeof(body) - off, "%02x", g_block_ids[n][i]);
        body[off++] = ',';
        for (int i = 0; i < 64; i++) off += snprintf(body + off, sizeof(body) - off, "%02x", g_block_sigs[n][i]);
    }

    char json[8700];
    snprintf(json, sizeof(json), "{\"n\":%llu,\"ch\":\"%s\",\"ct\":\"application/octet-stream\",\"d\":\"%s\"}",
             (unsigned long long)g_nid[I_N1], CH_ID, body);

    struct chat_msg_submit req={.data=(uint8_t*)body,.data_len=strlen(body)};
    snprintf(req.channel_id,sizeof(req.channel_id),"%s",CH_ID); strcpy(req.content_type,"application/octet-stream");
    int rc=chat_core_submit_message(g_inst[I_N1],&req); uint64_t ts=req.timestamp;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL,"[mdc] publish media message ts=%llu bytes=%zu result=%d",(unsigned long long)ts,req.data_len,rc);
    if (rc == 0) {
        /* анонсируем блоки суперузлам (как on_media_registered в chat_msg.c) */
        ensure_supernode_type(I_N1);
        media_delivery_announce_media(g_inst[I_N1], g_group_id, g_media_id, &g_block_ids[0][0], MEDIA_NUM_BLOCKS);
    }
    return rc;
}

/* ── фазы ── */

static void setup_chat_group(void) {
    uint8_t x25519_pub[32] = {0}, ed_pub[32] = {0}, ed_priv[32] = {94}, ch_sig[64] = {0};
    int i;

    TEST("generate channel keys"); {
        uint8_t x_private[32];
        if (!sc_identity_from_seed(ed_priv, x25519_pub, x_private, ed_pub) &&
            !test_group_signature(CH_ID, "test", g_inst[0]->my_ed25519_pubkey, x25519_pub, ed_pub, ed_priv, ch_sig)) OK(); else FAIL();
    }

    TEST("create CHAT group + peers tables"); {
        for (i = 0; i < N_NODES; i++) {
            sqlite3* db = g_inst[i]->topo_sqlite_db;
            struct TOPO_GROUPS* tg = g_inst[i]->topo_groups;
            if (!db || !tg) continue;
            topo_node_sqlite_channel_put(db, CH_ID, "test", g_inst[0]->my_ed25519_pubkey, x25519_pub, NULL, ed_pub, NULL, ch_sig);
            if (topo_groups_find(tg, g_group_id)) continue;
            topo_groups_create_group(tg, g_group_id, TOPO_GROUP_TYPE_CHAT, CH_ID);
        }
        OK();
    }

    TEST("add 6 members (s1/s2 supernode, st1/st2 storage)"); {
        const char* adm[N_NODES] = { NULL, NULL, "{\"ver\":\"1\",\"supernode\":\"yes\"}", "{\"ver\":\"1\",\"supernode\":\"yes\"}",
            "{\"ver\":\"1\",\"storage\":\"yes\"}", "{\"ver\":\"1\",\"storage\":\"yes\"}" };
        for (i = 0; i < N_NODES; i++) {
            for (int m = 0; m < N_NODES; m++) {
                uint8_t sig[64], msg[MS_OWNER_MSG_MAX];
                if (adm[m]) {
                    int n = member_sync_build_owner_msg(CH_ID, g_inst[m]->my_ed25519_pubkey, adm[m], msg, sizeof(msg));
                    if (n < 0 || sc_ed25519_sign(ed_priv, msg, n, sig) != SC_OK) { FAIL("owner signing failed"); return; }
                }
                uint8_t join[64], tree[64], update[64];
                int bytes = member_sync_build_join_msg(x25519_pub, ed_pub, g_inst[m]->my_ed25519_pubkey, g_inst[m]->my_keys.public_key, msg, sizeof(msg));
                if (bytes < 0 || sc_ed25519_sign(g_inst[m]->my_ed25519_privkey, msg, bytes, join) != SC_OK ||
                    member_sync_sign_join(ed_priv, CH_ID, NULL, g_inst[m]->my_keys.public_key, g_inst[m]->my_ed25519_pubkey,
                        TEST_JOIN_ID(x25519_pub, ed_pub, g_inst[m]->my_ed25519_pubkey, g_inst[m]->my_keys.public_key), 1, tree)) {
                    FAIL("membership signing failed"); return;
                }
                bytes = member_sync_build_update_msg(TEST_JOIN_ID(x25519_pub, ed_pub, g_inst[m]->my_ed25519_pubkey, g_inst[m]->my_keys.public_key), 1, "{\"name\":\"node\"}", msg, sizeof(msg));
                if (bytes < 0 || sc_ed25519_sign(g_inst[m]->my_ed25519_privkey, msg, bytes, update) != SC_OK) {
                    FAIL("profile signing failed"); return;
                }
                int rc = member_sync_put(g_inst[i], CH_ID,
                    g_inst[m]->my_keys.public_key, g_inst[m]->my_ed25519_pubkey,
                    join, 1, update, 1, "{\"name\":\"node\"}", adm[m], adm[m] ? sig : NULL, 0, 0, tree);
                if (rc < 0) { FAIL("member insertion failed rc=%d", rc); return; }
                /* заполняем nodes-таблицу (нужно для верификации Ed25519 подписи блоков/автора) */
                topo_node_sqlite_node_update_verified(g_inst[i]->topo_sqlite_db, g_nid[m],
                    "node", g_inst[m]->my_keys.public_key, g_inst[m]->my_ed25519_pubkey,
                    (uint64_t)time(NULL), time(NULL));
            }
        }
        int ok = 1;
        for (i = 0; i < N_NODES; i++) if (member_sync_count(g_inst[i], CH_ID) < N_NODES) ok = 0;
        if (ok) OK(); else FAIL();
    }

    TEST("supernodes node_type=4 in peers table"); {
        char sql[256];
        for (i = 0; i < N_NODES; i++) {
            snprintf(sql, sizeof(sql), "UPDATE \"peers_%s\" SET node_type=4 WHERE node_id IN (%llu,%llu)",
                     CH_ID, (unsigned long long)g_nid[I_S1], (unsigned long long)g_nid[I_S2]);
            sqlite3_exec(g_inst[i]->topo_sqlite_db, sql, NULL, NULL, NULL);
        }
        int ok = 1;
        for (i = 0; i < N_NODES; i++) {
            int n = db_count(g_inst[i]->topo_sqlite_db, "peers_" CH_ID, "node_type", 4);
            if (n < 2) ok = 0;
        }
        if (ok) OK(); else FAIL();
    }

    TEST("media_delivery supernode s1,s2"); {
        media_delivery_refresh_roles(g_inst[I_S1]);
        media_delivery_refresh_roles(g_inst[I_S2]);
        if (g_inst[I_S1]->md.is_supernode && g_inst[I_S2]->md.is_supernode) OK(); else FAIL();
    }

    TEST("automatic SYNC s1<->s2 (via CHAT)"); {
        int a = 0, ok = 0;
        while (a < 4000) {
            ok = db_count(g_inst[I_S1]->md.db, "super_sync", "group_id", g_group_id) > 0
              && db_count(g_inst[I_S2]->md.db, "super_sync", "group_id", g_group_id) > 0;
            if (ok) break;
            uasync_poll(g_ua, POLL_MS); a++;
        }
        if (ok) OK(); else FAIL();
    }
}

static void setup_db_sync(void) {
    TEST("db_sync instances on all nodes"); {
        int ok = 1;
        for (int i = 0; i < N_NODES; i++) {
            chat_core_ensure_channel_ready(g_inst[i],CH_ID);
            g_si[i] = db_sync_instance_find(g_inst[i],g_group_id);
            if (!g_si[i]) { ok = 0; DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "[mdc] db_sync_instance_add failed node=%d", i); }
            else if (setup_history_model(g_inst[i]) < 0) {
                ok = 0; DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "[mdc] history model registration failed node=%d", i);
            } else db_sync_instance_set_gated(g_si[i], 0);
        }
        if (ok) OK(); else FAIL();
    }

    TEST("register auto-download insert-cb on storage nodes"); {
        db_sync_set_insert_cb(g_si[I_ST1], storage_on_insert, (void*)(intptr_t)I_ST1);
        db_sync_set_insert_cb(g_si[I_ST2], storage_on_insert, (void*)(intptr_t)I_ST2);
        OK();
    }
}

/* Дождаться объекта через штатный ручной запрос чата, затем взять путь из индекса. */
static int manual_download(int idx, int messages, char location[MEDIA_PATH_MAX]) {
    struct UTUN_INSTANCE* inst = g_inst[idx];
    const char* query = "SELECT max(id) FROM msg_" CH_ID " HAVING count(*)>=?";
    sqlite3_stmt* st = NULL;
    uint64_t start = get_time_tb();
    int64_t id = 0;
    while (!id && get_time_tb() - start < 60000) {
        if (!sqlite3_prepare_v2(inst->topo_sqlite_db, query, -1, &st, NULL) &&
            sqlite3_bind_int(st, 1, messages) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st); st = NULL;
        if (!id) uasync_poll(g_ua, POLL_MS);
    }
    if (!id) return -1;
    ensure_supernode_type(idx);
    chat_core_attachment_download(inst, CH_ID, id);
    query = "SELECT location FROM media_files WHERE chat_id='" CH_ID "' AND media_id=? AND chunk=0";
    start = get_time_tb();
    location[0] = 0;
    while (!location[0] && get_time_tb() - start < 60000) {
        if (!sqlite3_prepare_v2(inst->topo_sqlite_db, query, -1, &st, NULL) &&
            sqlite3_bind_blob(st, 1, g_media_id, 16, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
            const char* path = (const char*)sqlite3_column_text(st, 0);
            if (path) snprintf(location, MEDIA_PATH_MAX, "%s", path);
        }
        sqlite3_finalize(st); st = NULL;
        if (!location[0]) uasync_poll(g_ua, POLL_MS);
    }
    return location[0] ? 0 : -1;
}

/* ── main ── */

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level_by_name("media", "debug");
    debug_set_category_level_by_name("etcp_route", "debug");
    debug_set_category_level_by_name("general", "info");
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level_by_name("media", "info");
        debug_set_category_level_by_name("general", "info");
        debug_set_category_level_by_name("chat_sync", "info");
        debug_set_category_level_by_name("etcp_route", "info");
        debug_set_category_level_by_name("bgp", "trace");
    }
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));
    printf("=== test_media_delivery_chat ===\n");
    if (getenv("TMPDIR") && snprintf(g_tdir, sizeof(g_tdir), "%s/utun_mdc_XXXXXX", getenv("TMPDIR")) >= (int)sizeof(g_tdir)) {
        fprintf(stderr, "[ERROR] media chat: TMPDIR path too long\n"); return 1;
    }
    if (test_mkdtemp(g_tdir)) { perror("media chat temporary directory"); return 1; }
    const char* logs = getenv("UTUN_TEST_LOG_DIR"); if (!logs || !*logs) logs = "logs";
    if (utun_mkdir(logs, 0755) < 0 && errno != EEXIST) { perror("media chat log directory"); test_remove_tree(g_tdir); return 1; }
    char log_path[1024]; snprintf(log_path, sizeof(log_path), "%s/test_media_delivery_chat.log", logs);
    debug_enable_file_output(log_path, 1); debug_enable_console(0);
    printf("[INFO] media chat diagnostics: %s\n", log_path); fflush(stdout);

    for (int i = 0; i < MEDIA_FILE_SIZE; i++) g_file_data[i] = (uint8_t)((i * 31 + 17) & 0xFF);

    for (int i = 0; i < N_NODES; i++) {
        snprintf(g_cfg[i], sizeof(g_cfg[i]), "%s/n%d.conf", g_tdir, i);
        snprintf(g_db_dir[i], sizeof(g_db_dir[i]), "%s/db%d", g_tdir, i);
        utun_mkdir(g_db_dir[i], 0755);
    }

    g_ua = uasync_create();
    for (int i = 0; i < N_NODES; i++)
        wf(g_cfg[i], "[global]\ntun_ip=10.98.%d.1/24\ntun_ifname=tun%d0\ndb_path=%s\ndb_sync_enabled=1\n"
            "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n",
            i, i, g_db_dir[i]);
    for (int i = 0; i < N_NODES; i++) {
        config_ensure_keys(g_cfg[i]);
        struct utun_config* c = parse_config(g_cfg[i]); g_nid[i] = test_transport_id(c); free_config(c);
    }

    for (int i = 0; i < N_NODES; i++) {
        char *pr = gv(g_cfg[i], "priv"), *pu = gv(g_cfg[i], "pub");
        char clients[4096] = "";
        for (int j = 0; j < i; j++) {
            char *pj_pu = gv(g_cfg[j], "pub");
            char link[256]; snprintf(link, sizeof(link),
                "[client: to_n%d]\nkeepalive=1\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", j, pj_pu);
            strncat(clients, link, sizeof(clients) - strlen(clients) - 1);
            u_free(pj_pu);
        }
        wf(g_cfg[i], "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.98.%d.1/24\ntun_ifname=tun%d0\n"
            "db_path=%s\ndb_sync_enabled=1\n[chatserver]\nstorage_autoload=0\n"
            "[server: s1]\naddr=127.0.0.1:0\ntype=public\n%s[allowed_keys]\nallow_all=1\n",
            pr, pu, i, i, g_db_dir[i], clients);
        u_free(pr); u_free(pu);
    }

    for (int i = 0; i < N_NODES; i++) {
        g_inst[i] = utun_instance_create(g_ua, g_cfg[i]);
        if (!g_inst[i]) { printf("FAIL: create n%d\n", i); goto done; }
        for (int j = 0; j < i; j++) {
            char peer[64]; snprintf(peer, sizeof(peer), "to_n%d", j);
            test_etcp_set_peer(g_inst[i], peer, g_inst[j], "s1");
        }
        utun_instance_init(g_inst[i]);
        char chat_db[512]; snprintf(chat_db, sizeof(chat_db), "%s/chats.db", g_db_dir[i]);
        if (chat_service_start(g_inst[i])) {
            FAIL("chat/media initialization n%d", i); goto done;
        }
    }

    mon(NULL);
    uasync_set_timeout(g_ua, TIMEOUT_TB, NULL, (timeout_callback_t)to_cb, "mdc_to");
    { int el = 0; while (!g_connected && !g_result && el < TIMEOUT_TB + 5000) { uasync_poll(g_ua, POLL_MS); el += POLL_MS; }
      if (!g_connected) { printf("FAIL: connection timeout\n"); goto done; } }
    printf("  connected\n");

    g_group_id = 31415926535;

    /* media_files-таблица нужна на КАЖДОМ узле (в chat_core_init её создаёт media_index_init) —
       иначе узел, принявший BLOCK_REQ, падает с "no such table: media_files". */
    for (int i = 0; i < N_NODES; i++) media_index_init(g_inst[i]->topo_sqlite_db);

    setup_chat_group();
    setup_db_sync();

    TEST("BGP pubkeys known (db_sync verification)"); {
        if (wait_pubkeys()) OK(); else FAIL();
    }

    TEST("CHAT group BGP converged (routing ready)"); {
        if (wait_chat_bgp()) OK(); else FAIL();
    }

    TEST("author: index file + media_index_commit"); {
        if (author_index_file() == 0 && db_count(g_inst[I_N1]->topo_sqlite_db, "media_files", NULL, 0) == MEDIA_NUM_BLOCKS) OK();
        else FAIL();
    }

    TEST("author: publish media message via db_sync"); {
        if (author_publish_message() == 0) OK(); else FAIL();
    }

    /* Фаза 1: сообщение распространилось на все узлы */
    TEST("message propagated to all 6 nodes"); {
        int all = 0;
        uint64_t start = get_time_tb();
        while ((get_time_tb() - start) < (uint64_t)60000) {
            all = 1;
            for (int i = 0; i < N_NODES; i++) if (db_sync_count(g_si[i]) < 1) all = 0;
            if (all) break;
            uasync_poll(g_ua, POLL_MS);
        }
        if (all) OK(); else FAIL("counts: %d %d %d %d %d %d",
            db_sync_count(g_si[0]), db_sync_count(g_si[1]), db_sync_count(g_si[2]),
            db_sync_count(g_si[3]), db_sync_count(g_si[4]), db_sync_count(g_si[5]));
    }

    /* Фаза 2: storage скачали и собрали файл */
    TEST("storage st1/st2 downloaded + assembled media"); {
        char d1[1024], d2[1024];
        node_dest(I_ST1, d1, sizeof(d1)); node_dest(I_ST2, d2, sizeof(d2));
        uint64_t start = get_time_tb();
        while ((get_time_tb() - start) < (uint64_t)60000 && (!g_dl_done[I_ST1] || !g_dl_done[I_ST2]))
            uasync_poll(g_ua, POLL_MS);
        if (g_dl_done[I_ST1] && g_dl_done[I_ST2] && g_dl_err[I_ST1] == 0 && g_dl_err[I_ST2] == 0
            && file_exists(d1) && file_exists(d2)) OK();
        else FAIL("st1 done=%d err=%d exists=%d; st2 done=%d err=%d exists=%d",
            g_dl_done[I_ST1], g_dl_err[I_ST1], file_exists(d1), g_dl_done[I_ST2], g_dl_err[I_ST2], file_exists(d2));
    }

    TEST("storage file content hash matches"); {
        char d1[1024], d2[1024];
        node_dest(I_ST1, d1, sizeof(d1)); node_dest(I_ST2, d2, sizeof(d2));
        uint8_t h1[32], h2[32];
        int ok = (file_sha256(d1, h1) == 0 && file_sha256(d2, h2) == 0
                  && memcmp(h1, g_content_hash, 32) == 0 && memcmp(h2, g_content_hash, 32) == 0);
        if (ok) OK(); else FAIL();
    }

    /* Фаза 3: суперузлы узнали о блоках + репликация между собой */
    TEST("supernodes have block_availability (from storage HAVE_BLOCK)"); {
        int ok = 0;
        uint64_t start = get_time_tb();
        while ((get_time_tb() - start) < (uint64_t)60000) {
            int n1 = db_count(g_inst[I_S1]->topo_sqlite_db, "block_availability", NULL, 0);
            int n2 = db_count(g_inst[I_S2]->topo_sqlite_db, "block_availability", NULL, 0);
            if (n1 >= 2 && n2 >= 2) { ok = 1; break; }
            uasync_poll(g_ua, POLL_MS);
        }
        if (ok) OK(); else FAIL("s1=%d s2=%d",
            db_count(g_inst[I_S1]->topo_sqlite_db, "block_availability", NULL, 0),
            db_count(g_inst[I_S2]->topo_sqlite_db, "block_availability", NULL, 0));
    }

    TEST("supernode replication s1↔s2 (super_sync)"); {
        int ok = 0;
        uint64_t start = get_time_tb();
        while ((get_time_tb() - start) < (uint64_t)60000) {
            sqlite3_stmt* st = NULL; uint64_t lr = 0;
            sqlite3_prepare_v2(g_inst[I_S2]->topo_sqlite_db, "SELECT last_recv_id FROM super_sync WHERE group_id=? AND peer=?", -1, &st, NULL);
            sqlite3_bind_text(st, 1, CH_ID, -1, SQLITE_STATIC);
            sqlite3_bind_blob(st, 2, g_inst[I_S1]->my_ed25519_pubkey, 32, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) lr = (uint64_t)sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            if (lr > 0) { ok = 1; break; }
            uasync_poll(g_ua, POLL_MS);
        }
        if (ok) OK(); else FAIL();
    }

    TEST("manual downloads preserve two same-name files"); {
        char locations[2][MEDIA_PATH_MAX], base[MEDIA_BASE_MAX], path[MEDIA_PATH_MAX];
        uint8_t hashes[2][32], actual[32];
        node_media_base(I_N2, base, sizeof(base));
        memcpy(hashes[0], g_content_hash, 32);
        int ok = !manual_download(I_N2, 1, locations[0]);
        if (ok) {
            db_sync_set_insert_cb(g_si[I_ST1], NULL, NULL); db_sync_set_insert_cb(g_si[I_ST2], NULL, NULL);
            g_file_data[0] ^= 0xff;
            ok = !author_index_file() && !author_publish_message();
            memcpy(hashes[1], g_content_hash, 32);
            ok = ok && !manual_download(I_N2, 2, locations[1]) && strcmp(locations[0], locations[1]);
        }
        for (int i = 0; ok && i < 2; i++) ok = !media_index_path(base, locations[i], path) &&
            !file_sha256(path, actual) && !memcmp(actual, hashes[i], 32);
        if (ok) OK(); else FAIL("same-name attachments require two unique paths with their original SHA-256");
    }

    fflush(stdout); fflush(stderr);
    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED); fflush(stdout);

done:
    g_result = 1;
    for (int i = 0; i < N_NODES; i++) if (g_inst[i]) {
        g_inst[i]->running = 0;
        if (!g_inst[i]->chat_sync) merkle_sync_destroy(g_inst[i]);
        utun_instance_destroy(g_inst[i]); g_inst[i] = NULL;
    }
    if (g_ua) { uasync_destroy(g_ua, 0); g_ua = NULL; }
    debug_disable_file_output();
    int cleanup = test_remove_tree(g_tdir);
    return G_FAILED > 0 || cleanup ? 1 : 0;
}
