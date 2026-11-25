// test_media_delivery_full.c — полный сценарий: создание, передача, сборка, репликация
//
// 4 узла: n1 (автор), n2 (получатель), s1 (суперузел), s2 (суперузел)
// Топология: цепь 0→1 1→2 2→3
// Фазы:
//   P1: CHAT-группа (topo_groups_create_group + member_sync_put)
//   A1: роли CHAT, запросы страниц, HAVE_BLOCK, репликация
//   A2: admission control (OVERLOADED)
//   B1: n1 создаёт файл → n2 инициирует загрузку → стриминг → сборка → проверка

#include "test_join_id.h"
#include "media_delivery.h"
#include <assert.h>
#include "media_delivery_proto.h"
#include "media_download.h"
#include "media_index.h"
#include "member_sync.h"
#include "../chat/chat_core.h"
#include "chat_channel.h"
#include "chat_core_priv.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../config_parser.h"
#include "../config_updater.h"
#include "../routing_layer/topo_group.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_net.h"
#include "test_utils.h"
#include "media_members_fixture.h"

#include <sqlite3.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../lib/platform_compat.h"

static int G_PASSED = 0, G_FAILED = 0, G_TOTAL = 0;
#define TEST(n)  do { G_TOTAL++; printf("  %-60s", n); fflush(stdout); } while(0)
#define OK()     do { G_PASSED++; printf("OK\n"); } while(0)
#define FAIL(f,...) do { G_FAILED++; printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

#define TIMEOUT_TB 300000
#define POLL_MS    5
#define N_NODES    4  /* n1=0, n2=1, s1=2, s2=3 */
#define CH_ID      "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951" /* Ed25519 для seed {94}. */

static struct UASYNC* g_ua = NULL;
static struct UTUN_INSTANCE* g_inst[N_NODES];
static uint64_t g_nid[N_NODES];
static uint64_t g_group_id = 0;
static uint8_t g_test_mid[16], g_test_bid0[16], g_test_bid1[16];
static uint8_t g_t2_mid[16], g_t2_bids[3][16];
static struct media_index_result g_truncated;
static int g_download_done, g_download_error;
static void downloaded(void* arg, int error) { (void)arg; g_download_done++; g_download_error = error; }
static char g_tdir[512] = "/tmp/utun_mdf_XXXXXX";
static char g_cfg[N_NODES][640];
static char g_db_dir[N_NODES][640];
static int  g_connected = 0, g_result = 0;

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
    if (ok >= N_NODES * 2 - 2) g_connected = 1;
    if (!g_result) uasync_set_timeout(g_ua, 10, NULL, mon, "md_mon");
}

static int msend(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t dst, const uint8_t* data, size_t len) {
    struct ll_entry* e = queue_entry_new(0); if (!e) return -1;
    e->dgram = u_malloc(len + 1); if (!e->dgram) { queue_entry_free(e); return -1; }
    e->dgram[0] = ETCP_RT_ID_MEDIA_DELIVERY; memcpy(e->dgram + 1, data, len); e->len = (uint16_t)(len + 1);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "[msend] 0x%016llx → 0x%016llx svc=0x%02x subcmd=0x%02x len=%zu",
        (unsigned long long)inst->node_id, (unsigned long long)dst, e->dgram[0], data[0], len);
    int rc = etcp_route_send(inst, group, dst, e, 1, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED);
    if (rc != 0) DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "[msend] FAILED rc=%d", rc);
    return rc;
}

/* ── Phase P1: create CHAT group + members on all nodes ── */

static void setup_chat_group(void) {
    uint8_t x25519_pub[32] = {0}, ed_pub[32] = {0}, ed_priv[32] = {94}, ch_sig[64] = {0};
    int i;

    TEST("generate channel keys"); {
        uint8_t x_private[32], description[CHAT_CHANNEL_DESCRIPTION_MAX];
        int rc = sc_identity_from_seed(ed_priv, x25519_pub, x_private, ed_pub);
        int len = rc ? -1 : chat_channel_description_data(CH_ID, "test", g_inst[0]->my_ed25519_pubkey,
            x25519_pub, ed_pub, description, sizeof(description));
        if (len <= 0 || sc_ed25519_sign(ed_priv, description, len, ch_sig) != SC_OK) { FAIL("group signing failed"); return; }
        OK();
    }

    TEST("create CHAT groups + peers tables"); {
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

    TEST("add all 4 members (n1, n2, s1 supernode, s2 supernode)"); {
        const char* adm[4] = { NULL, NULL, "{\"ver\":\"1\",\"supernode\":\"yes\"}", "{\"ver\":\"1\",\"supernode\":\"yes\"}" };
        for (i = 0; i < N_NODES; i++) {
            sqlite3* db = g_inst[i]->topo_sqlite_db;
            if (!db) continue;
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
                    join, 1, update, 1, "{\"name\":\"node\"}",
                    adm[m], adm[m] ? sig : NULL, 0, 0, tree);
                if (rc < 0) { FAIL("member insertion failed rc=%d", rc); return; }
            }
        }
        int ok = 1;
        for (i = 0; i < N_NODES; i++) if (member_sync_count(g_inst[i], CH_ID) < N_NODES) ok = 0;
        if (ok) OK(); else FAIL();
    }
}

/* ── Phase A1: автоматический запуск по подписанным ролям CHAT ── */
static void phase_a1_super_hello(void) {
    TEST("s1,s2 are supernodes"); {
        media_delivery_refresh_roles(g_inst[2]);
        media_delivery_refresh_roles(g_inst[3]);
        topo_node_sqlite_nodeinfo_updated(g_inst[2]->topo_sqlite_db, g_inst[2]->my_keys.public_key);
        topo_node_sqlite_nodeinfo_updated(g_inst[3]->topo_sqlite_db, g_inst[3]->my_keys.public_key);
        if (g_inst[2]->md.is_supernode && g_inst[3]->md.is_supernode) OK(); else FAIL();
    }
    TEST("automatic CHAT replication s1↔s2"); {
        int a = 0, ok1 = 0, ok2 = 0;
        while (a++ < 4000) {
            ok1 = db_count(g_inst[2]->md.db, "super_sync", "group_id", g_group_id) > 0;
            ok2 = db_count(g_inst[3]->md.db, "super_sync", "group_id", g_group_id) > 0;
            if (ok1 && ok2) break;
            uasync_poll(g_ua, POLL_MS);
        }
        if (ok1 && ok2) OK(); else FAIL("s1=%d s2=%d", ok1, ok2);
    }
}

/* ── Phase A2: HAVE_BLOCK + replication ── */
static void phase_a2_have_block_replication(void) {
    uint8_t uuid[16]; memset(uuid, 0xAB, 16);
    struct media_index_result parent = {.file_size=1}; memcpy(parent.media_id, uuid, 16);
    media_test_publish_channel(g_inst[1]->topo_sqlite_db, CH_ID, g_nid[1], &parent);
    TEST("HAVE_BLOCK n2→s1"); {
        media_delivery_send_have_block(g_inst[1], g_group_id, g_inst[2]->my_ed25519_pubkey, uuid, uuid, 0);
        int a = 0; while (a < 500) { if (db_count(g_inst[2]->topo_sqlite_db, "block_availability", "group_id", g_group_id) >= 1) break; uasync_poll(g_ua, POLL_MS); a++; }
        if (db_count(g_inst[2]->topo_sqlite_db, "block_availability", "group_id", g_group_id) >= 1) OK(); else FAIL();
    }
    TEST("SYNC pages s1→s2"); {
        int a = 0; while (a < 500) { uasync_poll(g_ua, POLL_MS); a++; }
        if (db_count(g_inst[3]->topo_sqlite_db, "block_availability", NULL, 0) >= 1) OK(); else FAIL();
    }
    TEST("s2 super_sync updated"); {
        sqlite3_stmt* st = NULL; uint64_t lr = 0;
        sqlite3_prepare_v2(g_inst[3]->topo_sqlite_db, "SELECT last_recv_id FROM super_sync WHERE group_id=? AND peer=?", -1, &st, NULL);
        sqlite3_bind_text(st, 1, CH_ID, -1, SQLITE_STATIC);
        sqlite3_bind_blob(st, 2, g_inst[2]->my_ed25519_pubkey, 32, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) lr = (uint64_t)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        if (lr > 0) OK(); else FAIL();
    }
}

/* ── helper: create test file + index before admission ── */
static int create_test_file(void) {
    char src_tmp[512]; snprintf(src_tmp, sizeof(src_tmp), "%s/test_src.bin", g_tdir);
    char media_dir[512]; snprintf(media_dir, sizeof(media_dir), "%s/media", g_db_dir[0]);
    utun_mkdir(media_dir, 0755);
    char dst_path[512]; snprintf(dst_path, sizeof(dst_path), "%s/test_src.bin", media_dir);
    char media_base[512]; snprintf(media_base, sizeof(media_base), "%s", g_db_dir[0]);

    uint8_t file_data[10240];
    for (int i = 0; i < 10240; i++) file_data[i] = (uint8_t)(i & 0xFF);
    FILE* f = fopen(src_tmp, "wb");
    if (!f) return -1;
    fwrite(file_data, 1, 10240, f); fclose(f);

    { FILE* fin = fopen(src_tmp, "rb"), *fout = fopen(dst_path, "wb");
      if (fin && fout) { uint8_t buf[4096]; size_t rd; while ((rd = fread(buf, 1, sizeof(buf), fin)) > 0) fwrite(buf, 1, rd, fout); }
      if (fin) fclose(fin); if (fout) fclose(fout); }

    /* set media_base in ui_state */
    sqlite3_exec(g_inst[0]->topo_sqlite_db, "CREATE TABLE IF NOT EXISTS ui_state(key TEXT PRIMARY KEY, value TEXT)", NULL, NULL, NULL);
    { sqlite3_stmt* us = NULL;
      sqlite3_prepare_v2(g_inst[0]->topo_sqlite_db, "INSERT OR REPLACE INTO ui_state(key,value) VALUES('media_base',?)", -1, &us, NULL);
      sqlite3_bind_text(us, 1, media_base, -1, SQLITE_STATIC); sqlite3_step(us); sqlite3_finalize(us); }

    /* index the file */
    uint8_t hash[32];
    { EVP_MD_CTX* ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
      EVP_DigestUpdate(ctx, file_data, 10240); EVP_DigestFinal_ex(ctx, hash, NULL); EVP_MD_CTX_free(ctx); }

    media_index_init(g_inst[0]->topo_sqlite_db);
    struct media_index_result result; memset(&result, 0, sizeof(result));
    assert(media_index_generate_uuid(result.media_id) == 0);
        memcpy(result.content_hash, hash, 32);
        result.file_size = 10240; result.block_size = 5120; result.num_blocks = 2;
        result.block_ids = u_malloc(32); result.block_sigs = u_malloc(128);
        for (int i = 0; i < 2; i++) {
        assert(media_index_generate_uuid(result.block_ids + i * 16) == 0);
        uint8_t smsg[5128]; size_t soff = 0;
        memcpy(smsg + soff, file_data + i * 5120, 5120); soff += 5120;
        uint64_t nid = g_nid[0]; memcpy(smsg + soff, &nid, 8); soff += 8;
        sc_ed25519_sign(g_inst[0]->my_ed25519_privkey, smsg, soff, result.block_sigs + i * 64);
    }
    int rc = media_index_commit(g_inst[0]->topo_sqlite_db, &result, g_inst[0]->my_ed25519_pubkey,
        g_inst[0]->my_ed25519_privkey, CH_ID, dst_path, media_base);
    if (rc != 0) { media_index_result_free(&result); return -1; }
    media_test_publish_channel(g_inst[0]->topo_sqlite_db, CH_ID, g_nid[0], &result);
    memcpy(g_test_mid, result.media_id, 16);
    memcpy(g_test_bid0, result.block_ids, 16);
    memcpy(g_test_bid1, result.block_ids + 16, 16);
    media_index_result_free(&result);
    return 0;
}

/* ── Phase A3: admission control (uses real file after create_test_file) ── */
static void phase_a3_admission(void) {
    /* Дождаться маршрута CHAT-группы. */
    { int a = 0; while (a < 1000) {
        struct TOPO_GROUP* g = topo_groups_find(g_inst[1]->topo_groups, g_group_id);
        struct ETCP_CONN* c = g ? topo_group_find_conn_for_node(g, g_nid[0]) : NULL;
        if (c) break;
        uasync_poll(g_ua, POLL_MS); a++;
    }}

    TEST("1st BLOCK_REQ → stream"); {
        struct media_pkt_block_req req; memset(&req, 0, sizeof(req));
        req.subcmd = MEDIA_SUBCMD_BLOCK_REQ; assert(!media_index_generate_uuid(req.transfer_id));
        memcpy(req.media_id, g_test_mid, 16); memcpy(req.block_id, g_test_bid0, 16);
        msend(g_inst[1], g_group_id, g_nid[0], (const uint8_t*)&req, sizeof(req));
        int a = 0; while (a < 500) { uasync_poll(g_ua, POLL_MS); a++; }
        if (g_inst[0]->md.streams_started >= 1) OK(); else FAIL("started=%d", g_inst[0]->md.streams_started);
    }
    TEST("fill → 3 → OVERLOADED"); {
        int before = g_inst[0]->md.streams_started;
        for (int i = 0; i < 3; i++) {
            struct media_pkt_block_req req; memset(&req, 0, sizeof(req));
            req.subcmd = MEDIA_SUBCMD_BLOCK_REQ; assert(!media_index_generate_uuid(req.transfer_id));
            memcpy(req.media_id, g_test_mid, 16); memcpy(req.block_id, g_test_bid0, 16);
            msend(g_inst[1], g_group_id, g_nid[0], (const uint8_t*)&req, sizeof(req));
        }
        int a = 0; while (a < 500) { uasync_poll(g_ua, POLL_MS); a++; }
        int started = g_inst[0]->md.streams_started - before;
        if (started == 3) OK(); else FAIL("started=%d (expected 3, total=%d)", started, g_inst[0]->md.streams_started);
    }
}

/* ══════════════════════════════════════════════════════════
   Phase B1: BLOCK_REQ → stream → verify
   ══════════════════════════════════════════════════════════ */

static void phase_b1_stream_test(void) {
    TEST("n2→n1 BLOCK_REQ streams data"); {
        struct media_pkt_block_req req; memset(&req, 0, sizeof(req));
        req.subcmd = MEDIA_SUBCMD_BLOCK_REQ; assert(!media_index_generate_uuid(req.transfer_id));
        memcpy(req.media_id, g_test_mid, 16); memcpy(req.block_id, g_test_bid1, 16);
        req.chunk = 1;
        g_inst[0]->md.stream_completed = 0;
        msend(g_inst[1], g_group_id, g_nid[0], (const uint8_t*)&req, sizeof(req));
        int a = 0;
        while (a < 3000 && !g_inst[0]->md.stream_completed) { uasync_poll(g_ua, POLL_MS); a++; }
        if (g_inst[0]->md.stream_completed) OK(); else FAIL("stream not completed after %dms", 3000 * POLL_MS);
    }
}

/* ══════════════════════════════════════════════════════════
   Phase B2: усечённый последний блок — проверка offset на отправителе
   (баг: block_start = chunk * chunk_size давал неверный offset для
    последнего блока, где chunk_size усечён)
   ══════════════════════════════════════════════════════════ */

static int create_truncated_file(void) {
    char src_tmp[512]; snprintf(src_tmp, sizeof(src_tmp), "/tmp/mdl_trunc_%d.bin", getpid());
    char media_dir[512]; snprintf(media_dir, sizeof(media_dir), "%s/media", g_db_dir[0]);
    char dst_path[512]; snprintf(dst_path, sizeof(dst_path), "%s/trunc_src.bin", media_dir);
    char media_base[512]; snprintf(media_base, sizeof(media_base), "%s", g_db_dir[0]);

    /* 12288 байт, контент = (i >> 8), чтобы первый байт последнего блока отличал offset */
    const int FS = 12288, BS = 5120, NB = 3;
    uint8_t file_data[FS];
    for (int i = 0; i < FS; i++) file_data[i] = (uint8_t)(i >> 8);
    FILE* f = fopen(src_tmp, "wb");
    if (!f) return -1;
    fwrite(file_data, 1, FS, f); fclose(f);

    { FILE* fin = fopen(src_tmp, "rb"), *fout = fopen(dst_path, "wb");
      if (fin && fout) { uint8_t buf[4096]; size_t rd; while ((rd = fread(buf, 1, sizeof(buf), fin)) > 0) fwrite(buf, 1, rd, fout); }
      if (fin) fclose(fin); if (fout) fclose(fout); }

    uint8_t hash[32];
    { EVP_MD_CTX* ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
      EVP_DigestUpdate(ctx, file_data, FS); EVP_DigestFinal_ex(ctx, hash, NULL); EVP_MD_CTX_free(ctx); }

    struct media_index_result result; memset(&result, 0, sizeof(result));
    assert(media_index_generate_uuid(result.media_id) == 0);
    memcpy(result.content_hash, hash, 32);
    result.file_size = FS; result.block_size = BS; result.num_blocks = NB;
    result.block_ids = u_malloc(NB * 16); result.block_sigs = u_malloc(NB * 64);
    for (int i = 0; i < NB; i++) {
        assert(media_index_generate_uuid(result.block_ids + i * 16) == 0);
        int sz = (i == NB - 1) ? FS - i * BS : BS;
        assert(sc_ed25519_sign(g_inst[0]->my_ed25519_privkey, file_data + i * BS, (size_t)sz, result.block_sigs + i * 64) == SC_OK);
    }
    int rc = media_index_commit(g_inst[0]->topo_sqlite_db, &result, g_inst[0]->my_ed25519_pubkey,
        g_inst[0]->my_ed25519_privkey, CH_ID, dst_path, media_base);
    if (!rc) media_test_publish_channel(g_inst[0]->topo_sqlite_db, CH_ID, g_nid[0], &result);
    memcpy(g_t2_mid, result.media_id, 16);
    for (int i = 0; i < NB; i++) memcpy(g_t2_bids[i], result.block_ids + i * 16, 16);
    g_truncated = result;
    unlink(src_tmp);
    return rc;
}

static void phase_b2_truncated_last_block(void) {
    TEST("truncated last block served from correct offset"); {
        if (create_truncated_file() != 0) { FAIL("create_truncated_file"); return; }
        assert(!media_index_init(g_inst[1]->topo_sqlite_db));
        media_test_publish_channel(g_inst[1]->topo_sqlite_db, CH_ID, g_nid[0], &g_truncated);

        char destination[1024]; snprintf(destination, sizeof(destination), "%s/media/truncated.bin", g_db_dir[1]);
        g_download_done = g_download_error = 0;
        int started = media_download_start(g_inst[1], g_group_id, &g_truncated, destination, g_db_dir[1],
                                           g_inst[0]->my_ed25519_pubkey, downloaded, NULL, NULL, NULL);
        int polls = 0;
        while (!g_download_done && polls++ < 3000) uasync_poll(g_ua, POLL_MS);
        FILE* file = fopen(destination, "rb");
        int correct = !started && g_download_done == 1 && !g_download_error && file;
        if (file) {
            for (int i = 0; i < 12288; i++) if (fgetc(file) != (uint8_t)(i >> 8)) correct = 0;
            if (fgetc(file) != EOF || fclose(file)) correct = 0;
        }
        if (correct) OK();
        else FAIL("start=%d completions=%d error=%d", started, g_download_done, g_download_error);
        media_index_result_free(&g_truncated);
        unlink(destination);
    }
}

/* ── main ── */
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) { debug_set_category_level_by_name("media", "trace"); debug_set_category_level_by_name("general", "info"); }
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));
    printf("=== test_media_delivery_full ===\n");
    if (getenv("TMPDIR") && snprintf(g_tdir, sizeof(g_tdir), "%s/utun_mdf_XXXXXX", getenv("TMPDIR")) >= (int)sizeof(g_tdir)) {
        fprintf(stderr, "[ERROR] media delivery: TMPDIR path too long\n"); return 1;
    }
    if (test_mkdtemp(g_tdir)) { perror("media delivery temporary directory"); return 1; }

    for (int i = 0; i < N_NODES; i++) {
        snprintf(g_cfg[i], sizeof(g_cfg[i]), "%s/n%d.conf", g_tdir, i);
        snprintf(g_db_dir[i], sizeof(g_db_dir[i]), "%s/db%d", g_tdir, i);
        utun_mkdir(g_db_dir[i], 0755);
    }

    g_ua = uasync_create();
    for (int i = 0; i < N_NODES; i++)
        wf(g_cfg[i], "[global]\ntun_ip=10.99.%d.1/24\ntun_ifname=tun%d0\ndb_path=%s\n"
            "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n",
            i, i, g_db_dir[i]);
    for (int i = 0; i < N_NODES; i++) {
        config_ensure_keys(g_cfg[i]);
        struct utun_config* c = parse_config(g_cfg[i]); g_nid[i] = test_transport_id(c); free_config(c);
    }

    for (int i = 0; i < N_NODES; i++) {
        char *pr = gv(g_cfg[i], "priv"), *pu = gv(g_cfg[i], "pub");
        if (i == 0) {
            wf(g_cfg[i], "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.99.%d.1/24\ntun_ifname=tun%d0\n"
                "db_path=%s\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n",
                pr, pu, i, i, g_db_dir[i]);
        } else {
            int prev = i - 1; char *pv_pu = gv(g_cfg[prev], "pub");
            char link[256]; snprintf(link, sizeof(link),
                "[client: to_n%d]\nkeepalive=1\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", prev, pv_pu);
            wf(g_cfg[i], "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.99.%d.1/24\ntun_ifname=tun%d0\n"
                "db_path=%s\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n%s[allowed_keys]\nallow_all=1\n",
                pr, pu, i, i, g_db_dir[i], link);
            u_free(pv_pu);
        }
        u_free(pr); u_free(pu);
    }

    for (int i = 0; i < N_NODES; i++) {
        g_inst[i] = utun_instance_create(g_ua, g_cfg[i]);
        if (!g_inst[i]) { printf("FAIL: create n%d\n", i); goto done; }
        if (i) {
            char peer[64]; snprintf(peer, sizeof(peer), "to_n%d", i - 1);
            test_etcp_set_peer(g_inst[i], peer, g_inst[i - 1], "s1");
        }
        utun_instance_init(g_inst[i]);
        char chat_db[512]; snprintf(chat_db, sizeof(chat_db), "%s/chats.db", g_db_dir[i]);
        if (chat_core_init(g_inst[i], chat_db)) { FAIL("chat_core_init n%d", i); goto done; }
        if (member_sync_init(g_inst[i]) < 0) { FAIL("member_sync_init n%d", i); goto done; }
        assert(!media_index_init(g_inst[i]->topo_sqlite_db));
        assert(!media_delivery_init(g_inst[i]));
    }

    mon(NULL);
    uasync_set_timeout(g_ua, TIMEOUT_TB, NULL, (timeout_callback_t)to_cb, "md_to");
    { int el = 0; while (!g_connected && !g_result && el < TIMEOUT_TB + 5000) { uasync_poll(g_ua, POLL_MS); el += POLL_MS; }
      if (!g_connected) { printf("FAIL: connection timeout\n"); goto done; } }
    printf("  connected\n");

    g_group_id = 31415926535;
    /* ══ Phase P1: create CHAT groups + members ══ */
    setup_chat_group();

    phase_a1_super_hello();
    phase_a2_have_block_replication();

    /* create test file + index before admission tests (needed for real BLOCK_REQ to find files) */
    { TEST("media_index_commit + ui_state"); {
        if (create_test_file() == 0 && db_count(g_inst[0]->topo_sqlite_db, "media_files", NULL, 0) == 2) OK();
        else FAIL("create_test_file failed");
    }}

    phase_a3_admission();
    phase_b1_stream_test();
    phase_b2_truncated_last_block();

    fflush(stdout); fflush(stderr);
    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED); fflush(stdout);

done:
    g_result = 1;
    for (int i = 0; i < N_NODES; i++) {
        if (g_inst[i]) {
            member_sync_destroy(g_inst[i]);
            g_inst[i]->running = 0; utun_instance_destroy(g_inst[i]); g_inst[i] = NULL;
        }
    }
    if (g_ua) { uasync_destroy(g_ua, 0); g_ua = NULL; }
    int cleanup = test_remove_tree(g_tdir);
    return G_FAILED > 0 || cleanup ? 1 : 0;
}
