// test_media_delivery_download.c — юнит-тесты скачивания блоков (блочный конечный автомат)
//
// Покрытие:
//   - запись чанков по offset (идемпотентность при дублях)
//   - BLOCK_DONE: сигнатура валидна → сборка; неверная → фейловер
//   - фейловер блока по простою (фейковое время)
//   - исчерпание попыток → FAILED
//   - проверка простоя md_dl_check_stall (только зависшие блоки)
//   - OVERLOADED → следующий держатель / WAIT
//   - RELAY_FULL → добавление держателей
//   - пустой QUERY_RESP
//   - назначение держателей + разброс по узлам
//   - дедуп повторного media_download_start

#include "test_utils.h"
#include "media_delivery.h"
#include "media_delivery_proto.h"
#include "media_download.h"
#include "media_index.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "../transport_layer/secure_channel.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/conn_mgr.h"
#include "../routing_layer/conn_mgr_priv.h"

#include <sqlite3.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Доставка принята транспортом, ответа нет. Владение entry совпадает с реальным router во всех исходах. */
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t dst[32],
                           struct ll_entry* entry, int force, int mode) {
    (void)inst; (void)group; (void)dst; (void)force; (void)mode;
    queue_dgram_free(entry); queue_entry_free(entry); return 0;
}

#define TEMP_DIR "/tmp/test_mdl_dl_XXXXXX"
#define CHUNK_SIZE 100
#define NUM_BLOCKS 3

static int g_passed = 0, g_failed = 0, g_total = 0;
static char g_temp_dir[256];
static struct UASYNC* g_ua = NULL;

#define TEST(name) do { g_total++; printf("  %-58s", name); fflush(stdout); } while(0)
#define OK()       do { g_passed++; printf("OK\n"); } while(0)
#define FAIL(fmt, ...) do { g_failed++; printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)

/* ── helpers ── */

static void make_uuid(uint8_t buf[16]) { for (int i = 0; i < 16; i++) buf[i] = (uint8_t)(rand() & 0xFF); }

static void write_file(const char* path, const uint8_t* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}

static int file_exists(const char* path) { return access(path, F_OK) == 0; }

static int file_size(const char* path) {
    struct stat st; if (stat(path, &st) != 0) return -1; return (int)st.st_size;
}

static void test_setup(void) {
    snprintf(g_temp_dir, sizeof(g_temp_dir), "%s", TEMP_DIR);
    if (test_mkdtemp(g_temp_dir) != 0) { fprintf(stderr, "mkdtemp failed\n"); exit(1); }
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) { debug_set_category_level_by_name("media", "trace"); debug_set_category_level_by_name("general", "info"); }
    g_ua = uasync_create();
}

static void test_cleanup(void) {
    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", g_temp_dir); system(cmd);
    if (g_ua) { uasync_destroy(g_ua, 0); g_ua = NULL; }
}

/* ── минимальный UTUN_INSTANCE с media_delivery ── */

#include "../src/utun_instance.h"
#include "media_members_fixture.h"

static struct UTUN_INSTANCE* make_minimal_instance(sqlite3* db) {
    if (media_index_init(db) || media_index_set_base(db, g_temp_dir)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "download test media schema initialization failed"); exit(1);
    }
    struct UTUN_INSTANCE* inst = u_calloc(1, sizeof(*inst));
    if (!inst) return NULL;
    inst->ua = g_ua;
    inst->node_id = 0xDEADBEEFDEADBEEFULL;
    inst->topo_sqlite_db = db;
    const uint64_t peers[] = {0xAAA, 0xAAAA000000000001ULL, 0x1111, 0x2222, 0x3333, 0x9001};
    media_test_group(inst, 0x1234, peers, sizeof(peers) / sizeof(*peers));
    uint8_t author_ed[32];
    media_test_public(0xAAAA000000000001ULL, author_ed);
    if (sqlite3_exec(db, "CREATE TABLE nodes(node_id INTEGER PRIMARY KEY,ed25519_pubkey BLOB)", NULL, NULL, NULL)) exit(1);
    sqlite3_stmt* row = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO nodes VALUES(0xAAA,?)", -1, &row, NULL) != SQLITE_OK
        || sqlite3_bind_blob(row, 1, author_ed, 32, SQLITE_STATIC) != SQLITE_OK
        || sqlite3_step(row) != SQLITE_DONE || sqlite3_finalize(row) != SQLITE_OK) exit(1);

    inst->md.inst = inst;
    inst->md.db = db;
    inst->md.initialized = 1;
    inst->md.dl_stall_timeout_tb = 20000;   /* 2s */
    inst->md.dl_max_attempts = 3;
    return inst;
}

/* создаёт загрузку и регистрирует в очереди; возвращает указатель на данные в очереди */
static struct media_download* make_test_dl(sqlite3* db, struct UTUN_INSTANCE* inst, int num_blocks) {
    if (!inst->md.downloads)
        inst->md.downloads = queue_new(inst->ua, 256, offsetof(struct media_download, group_id), MEDIA_DOWNLOAD_KEY_SIZE, "md_dl_test");

    struct media_download* dl = u_calloc(1, sizeof(*dl));
    if (!dl) return NULL;
    make_uuid(dl->media_id);
    dl->num_blocks = num_blocks;
    snprintf(dl->dest_path, sizeof(dl->dest_path), "%s/test.bin", g_temp_dir);
    snprintf(dl->media_base, sizeof(dl->media_base), "%s", g_temp_dir);
    dl->blocks = u_calloc((size_t)num_blocks, sizeof(struct media_download_block));
    dl->block_sigs = u_calloc((size_t)num_blocks, 64);
    for (int i = 0; i < num_blocks; i++) {
        make_uuid(dl->blocks[i].block_id);
        memset(dl->block_sigs + i * 64, (uint8_t)(0x42 + i), 64);
        dl->blocks[i].state = MD_BLK_IDLE;
        dl->blocks[i].holder_idx = -1;
        dl->blocks[i].expected_size = CHUNK_SIZE;
    }
    dl->block_size = CHUNK_SIZE;
    dl->file_size = (int64_t)CHUNK_SIZE * num_blocks;
    dl->active = 1;
    dl->inst = inst;
    dl->group_id = 0x1234;
    media_test_public(0xAAAA000000000001ULL, dl->author_pubkey);

    struct ll_entry* qe = queue_entry_new(sizeof(*dl));
    if (!qe) { u_free(dl->blocks); u_free(dl->block_sigs); u_free(dl); return NULL; }
    memcpy(qe->data, dl, sizeof(*dl));
    u_free(dl);
    queue_data_put_with_index(inst->md.downloads, qe);
    ((struct media_download*)qe->data)->entry = qe;
    ((struct media_download*)qe->data)->refs = 1;
    dl = (struct media_download*)qe->data;
    for (int bi = 0; bi < num_blocks; bi++) {
        make_uuid(dl->blocks[bi].transfer_id);
        dl->blocks[bi].source = md_block_create(&inst->md, dl->group_id, dl->media_id,
            dl->blocks[bi].block_id, (uint32_t)bi, CHUNK_SIZE, dl->dest_path);
        if (!dl->blocks[bi].source) exit(1);
    }
    return (struct media_download*)qe->data;
}

static void free_test_dl(struct UTUN_INSTANCE* inst, struct media_download* dl) {
    if (!dl) return;
    media_download_cancel(inst, dl->media_id, NULL, 0);
    media_delivery_destroy(inst);
    media_test_groups_free(inst);
}

/* Полный ответ на текущую пачку; тесты задают только найденных держателей. */
static void query_response(struct media_download* dl, uint8_t* wire, int count) {
    memset(wire, 0, MEDIA_QUERY_RESP_HDR_SIZE + (size_t)count * sizeof(struct media_pkt_query_resp_entry));
    struct media_pkt_query_resp* r = (struct media_pkt_query_resp*)wire;
    r->subcmd = MEDIA_SUBCMD_QUERY_RESP; r->num_blocks = (uint16_t)count;
    memcpy(r->media_id, dl->media_id, 16); memset(r->request_id, 0xA5, 16); memcpy(dl->query_id, r->request_id, 16);
    media_test_public(0x9001, dl->query_peer); dl->query_count = count; dl->query_deadline_tb = get_time_tb() + 20000;
    struct media_pkt_query_resp_entry* entries = (struct media_pkt_query_resp_entry*)(wire + MEDIA_QUERY_RESP_HDR_SIZE);
    for (int bi = 0; bi < count; bi++) { dl->query_blocks[bi] = bi; memcpy(entries[bi].block_id, dl->blocks[bi].block_id, 16); }
}

static void add_holder(struct media_download* dl, int bi, uint64_t node_id) {
    struct media_download_block* b = &dl->blocks[bi];
    if (b->num_holders < MD_MAX_HOLDERS_PER_BLOCK)
        media_test_public(node_id, b->holders[b->num_holders++]);
}

/* ── 1. запись чанков по offset ── */

static void test_chunk_offset_write(void) {
    TEST("chunk write by offset (dup/idempotent)"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 1);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }

        add_holder(dl, 0, 0x1111); dl->blocks[0].holder_idx = 0; dl->blocks[0].state = MD_BLK_REQ; dl->inflight = 1;
        uint8_t cdata[60]; for (int i = 0; i < 60; i++) cdata[i] = (uint8_t)i;

        uint8_t pkt[MEDIA_BLOCK_CHUNK_HDR_SIZE + sizeof(cdata)];
        struct media_pkt_block_chunk* ch = (struct media_pkt_block_chunk*)pkt;
        memset(ch, 0, MEDIA_BLOCK_CHUNK_HDR_SIZE);
        ch->subcmd = MEDIA_SUBCMD_BLOCK_CHUNK;
        memcpy(ch->media_id, dl->media_id, 16);
        memcpy(ch->block_id, dl->blocks[0].block_id, 16);
        memcpy(ch->transfer_id, dl->blocks[0].transfer_id, 16);
        ch->chunk = 0; ch->offset = 0; ch->data_len = (uint16_t)sizeof(cdata);
        memcpy(pkt + MEDIA_BLOCK_CHUNK_HDR_SIZE, cdata, sizeof(cdata));

        media_download_handle_chunk(inst, dl->group_id, media_test_public(0x1111, (uint8_t[32]){0}), pkt, sizeof(pkt));
        const char* tmp = dl->blocks[0].source->storage.temporary;
        int s1 = file_size(tmp);

        /* дубль с тем же offset — не должен добавить данные */
        uint64_t progress = dl->blocks[0].last_progress_tb;
        media_download_handle_chunk(inst, dl->group_id, media_test_public(0x1111, (uint8_t[32]){0}), pkt, sizeof(pkt));
        int s2 = file_size(tmp);

        /* Только непрерывный хвост укладывается в объявленный размер блока. */
        ch->offset = 60; ch->data_len = 40;
        int duplicate_ignored = progress == dl->blocks[0].last_progress_tb;
        media_download_handle_chunk(inst, dl->group_id, media_test_public(0x1111, (uint8_t[32]){0}), pkt, sizeof(*ch) + 40);
        int s3 = file_size(tmp);

        int ok = s1 == 60 && s2 == 60 && s3 == 100 && dl->blocks[0].bytes_received == 100 && duplicate_ignored;
        if (ok) OK(); else FAIL("s1=%d s2=%d s3=%d bytes=%u", s1, s2, s3, dl->blocks[0].bytes_received);

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 2. BLOCK_DONE: сборка ── */

static void test_done_assembly(void) {
    TEST("BLOCK_DONE all blocks → assembly"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 2);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }
        uint8_t d0[CHUNK_SIZE], d1[CHUNK_SIZE];
        memset(d0, 0xA0, CHUNK_SIZE); memset(d1, 0xB1, CHUNK_SIZE);
        write_file(dl->blocks[0].source->storage.temporary, d0, CHUNK_SIZE);
        write_file(dl->blocks[1].source->storage.temporary, d1, CHUNK_SIZE);
        uint8_t author_key[32]; media_test_private(0xAAAA000000000001ULL, author_key);
        if (sc_ed25519_sign(author_key, d0, CHUNK_SIZE, dl->block_sigs) != SC_OK
            || sc_ed25519_sign(author_key, d1, CHUNK_SIZE, dl->block_sigs + 64) != SC_OK) exit(1);
        EVP_MD_CTX* hash = EVP_MD_CTX_new(); unsigned hash_size;
        if (!hash || EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1 || EVP_DigestUpdate(hash, d0, CHUNK_SIZE) != 1
            || EVP_DigestUpdate(hash, d1, CHUNK_SIZE) != 1 || EVP_DigestFinal_ex(hash, dl->content_hash, &hash_size) != 1) exit(1);
        EVP_MD_CTX_free(hash);
        for (int bi = 0; bi < 2; bi++) {
            add_holder(dl, bi, 0x1111); dl->blocks[bi].holder_idx = 0;
            dl->blocks[bi].state = MD_BLK_RECV; dl->blocks[bi].bytes_received = CHUNK_SIZE;
        }
        dl->inflight = 2;
        char dest[1024]; snprintf(dest, sizeof(dest), "%s", dl->dest_path);

        for (int bi = 0; bi < 2; bi++) {
            uint8_t pkt[MEDIA_BLOCK_DONE_SIZE]; memset(pkt, 0, sizeof(pkt));
            struct media_pkt_block_done* bd = (struct media_pkt_block_done*)pkt;
            bd->subcmd = MEDIA_SUBCMD_BLOCK_DONE;
            memcpy(bd->media_id, dl->media_id, 16); memcpy(bd->block_id, dl->blocks[bi].block_id, 16);
            bd->chunk = (uint32_t)bi; bd->total_size = CHUNK_SIZE;
            memcpy(bd->transfer_id, dl->blocks[bi].transfer_id, 16);
            media_download_handle_done(inst, dl->group_id, media_test_public(0x1111, (uint8_t[32]){0}), pkt, sizeof(pkt));
        }

        int sz = file_size(dest);
        if (sz == CHUNK_SIZE * 2) OK(); else FAIL("assembled size %d != %d", sz, CHUNK_SIZE * 2);
        /* dl уже освобождён через md_dl_finish */
        media_delivery_destroy(inst);
        media_test_groups_free(inst);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 3. BLOCK_DONE: неверная сигнатура → фейловер ── */

static void test_done_bad_sig(void) {
    TEST("BLOCK_DONE bad sig → failover (no assembly)"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 1);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }
        add_holder(dl, 0, 0x1111);
        dl->blocks[0].holder_idx = 0;

        uint8_t data[CHUNK_SIZE]; memset(data, 0xFF, CHUNK_SIZE);
        write_file(dl->blocks[0].source->storage.temporary, data, CHUNK_SIZE);
        dl->blocks[0].state = MD_BLK_RECV; dl->blocks[0].bytes_received = CHUNK_SIZE; dl->inflight = 1;

        uint8_t pkt[MEDIA_BLOCK_DONE_SIZE]; memset(pkt, 0, sizeof(pkt));
        struct media_pkt_block_done* bd = (struct media_pkt_block_done*)pkt;
        bd->subcmd = MEDIA_SUBCMD_BLOCK_DONE;
        memcpy(bd->media_id, dl->media_id, 16); memcpy(bd->block_id, dl->blocks[0].block_id, 16);
        bd->chunk = 0; bd->total_size = CHUNK_SIZE;
        memcpy(bd->transfer_id, dl->blocks[0].transfer_id, 16);
        memset(dl->block_sigs, 0xAA, 64); /* Неверная подпись в доверенных метаданных. */
        media_download_handle_done(inst, dl->group_id, media_test_public(0x1111, (uint8_t[32]){0}), pkt, sizeof(pkt));

        /* файл не собран; блок в WAIT (1 держатель, попытка ушла) */
        int ok = dl->blocks_validated == 0 && !dl->assembled && dl->blocks[0].attempts == 1 && dl->blocks[0].state == MD_BLK_REQ;
        if (ok) OK(); else FAIL("validated=%d assembled=%d attempts=%d state=%d", dl->blocks_validated, dl->assembled, dl->blocks[0].attempts, dl->blocks[0].state);

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 4. фейловер блока по простою (фейковое время) ── */

static void test_failover_stall(void) {
    TEST("failover on stall → next holder"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 1);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }
        add_holder(dl, 0, 0x1111);
        add_holder(dl, 0, 0x2222);
        dl->blocks[0].holder_idx = 0;
        dl->blocks[0].state = MD_BLK_RECV;
        dl->blocks[0].bytes_received = 12345;
        dl->inflight = 1;
        uint64_t now = 1000000000ULL;
        dl->blocks[0].last_progress_tb = now - 30000; /* 3s назад */

        int rc = md_dl_failover_block(dl, 0, now);
        int ok = rc == 0 && dl->blocks[0].attempts == 1 && dl->blocks[0].holder_idx == 1
              && dl->blocks[0].bytes_received == 0 && dl->blocks[0].state == MD_BLK_REQ;
        if (ok) OK(); else FAIL("rc=%d attempts=%d hidx=%d bytes=%u state=%d", rc, dl->blocks[0].attempts, dl->blocks[0].holder_idx, dl->blocks[0].bytes_received, dl->blocks[0].state);

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 5. исчерпание попыток → FAILED ── */

static void test_attempts_exhausted(void) {
    TEST("attempts exhausted → FAILED"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 1);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }
        add_holder(dl, 0, 0x1111);
        dl->blocks[0].holder_idx = 0;
        dl->blocks[0].state = MD_BLK_RECV;
        dl->blocks[0].attempts = 2; /* уже 2 провала */
        dl->inflight = 1;

        int rc = md_dl_failover_block(dl, 0, 1000000000ULL);
        int ok = rc == -1 && dl->blocks[0].state == MD_BLK_FAILED && dl->blocks[0].attempts == 3;
        if (ok) OK(); else FAIL("rc=%d state=%d attempts=%d", rc, dl->blocks[0].state, dl->blocks[0].attempts);

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 6. md_dl_check_stall: только зависшие блоки ── */

static void test_check_stall(void) {
    TEST("check_stall fails over only stalled block"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 2);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }
        for (int bi = 0; bi < 2; bi++) { add_holder(dl, bi, 0x1111); add_holder(dl, bi, 0x2222); dl->blocks[bi].holder_idx = 0; dl->blocks[bi].state = MD_BLK_RECV; }
        dl->inflight = 2;
        uint64_t now = 1000000000ULL;
        dl->blocks[0].last_progress_tb = now - 30000;  /* завис */
        dl->blocks[1].last_progress_tb = now - 100;    /* живой */

        int rc = md_dl_check_stall(dl, now);
        int ok = rc == 0 && dl->blocks[0].attempts == 1 && dl->blocks[1].attempts == 0;
        if (ok) OK(); else FAIL("rc=%d b0.attempts=%d b1.attempts=%d", rc, dl->blocks[0].attempts, dl->blocks[1].attempts);

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 7. OVERLOADED → следующий держатель ── */

static void test_overloaded_next_holder(void) {
    TEST("OVERLOADED → next holder (no attempt)"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 1);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }
        add_holder(dl, 0, 0x1111);
        add_holder(dl, 0, 0x2222);
        dl->blocks[0].holder_idx = 0;
        dl->blocks[0].state = MD_BLK_RECV;
        dl->inflight = 1;

        struct media_pkt_block_overloaded ov; memset(&ov, 0, sizeof(ov));
        ov.subcmd = MEDIA_SUBCMD_BLOCK_OVERLOADED;
        memcpy(ov.media_id, dl->media_id, 16); memcpy(ov.block_id, dl->blocks[0].block_id, 16);
        ov.retry_after_ms = 2000;
        memcpy(ov.transfer_id, dl->blocks[0].transfer_id, 16);
        media_download_handle_overloaded(inst, dl->group_id, media_test_public(0x1111, (uint8_t[32]){0}), (const uint8_t*)&ov, sizeof(ov));

        int ok = dl->blocks[0].attempts == 0 && dl->blocks[0].holder_idx == 1 && dl->blocks[0].state == MD_BLK_REQ;
        if (ok) OK(); else FAIL("attempts=%d hidx=%d state=%d", dl->blocks[0].attempts, dl->blocks[0].holder_idx, dl->blocks[0].state);

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 8. OVERLOADED без кандидатов → WAIT ── */

static void test_overloaded_wait(void) {
    TEST("OVERLOADED no more holders → WAIT"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 1);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }
        add_holder(dl, 0, 0x1111); /* один держатель */
        dl->blocks[0].author_tried = 1;
        dl->blocks[0].holder_idx = 0;
        dl->blocks[0].state = MD_BLK_RECV;
        dl->inflight = 1;

        struct media_pkt_block_overloaded ov; memset(&ov, 0, sizeof(ov));
        ov.subcmd = MEDIA_SUBCMD_BLOCK_OVERLOADED;
        memcpy(ov.media_id, dl->media_id, 16); memcpy(ov.block_id, dl->blocks[0].block_id, 16);
        memcpy(ov.transfer_id, dl->blocks[0].transfer_id, 16);
        media_download_handle_overloaded(inst, dl->group_id, media_test_public(0x1111, (uint8_t[32]){0}), (const uint8_t*)&ov, sizeof(ov));

        int ok = dl->blocks[0].state == MD_BLK_WAIT && dl->blocks[0].attempts == 0;
        if (ok) OK(); else FAIL("state=%d attempts=%d", dl->blocks[0].state, dl->blocks[0].attempts);

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 9. RELAY_FULL → добавление держателей ── */

static void test_relay_full(void) {
    TEST("RELAY_FULL → add holders + retry"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 1);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }
        add_holder(dl, 0, 0x1111); /* 1 держатель */
        dl->blocks[0].holder_idx = 0;
        dl->blocks[0].state = MD_BLK_RECV;
        dl->inflight = 1;

        uint8_t packet[MEDIA_RELAY_FULL_HDR_SIZE + 2 * 32] = {0};
        struct media_pkt_block_relay_full* full = (struct media_pkt_block_relay_full*)packet;
        full->subcmd = MEDIA_SUBCMD_BLOCK_RELAY_FULL; full->num_relay_nodes = 2;
        memcpy(full->media_id, dl->media_id, 16); memcpy(full->block_id, dl->blocks[0].block_id, 16);
        memcpy(full->transfer_id, dl->blocks[0].transfer_id, 16);
        media_test_public(0x3333, packet + sizeof(*full)); media_test_public(0x4444, packet + sizeof(*full) + 32);
        media_download_handle_relay_full(inst, dl->group_id, media_test_public(0x1111, (uint8_t[32]){0}), packet, sizeof(packet));

        int ok = dl->blocks[0].num_holders == 3 && dl->blocks[0].holder_idx == 1 && dl->blocks[0].state == MD_BLK_REQ;
        if (ok) OK(); else FAIL("holders=%d state=%d", dl->blocks[0].num_holders, dl->blocks[0].state);

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 10. пустой QUERY_RESP ── */

static void test_query_resp_empty(void) {
    TEST("QUERY_RESP empty result → author fallback"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 1);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }

        uint8_t pkt[MEDIA_QUERY_RESP_HDR_SIZE + sizeof(struct media_pkt_query_resp_entry)];
        query_response(dl, pkt, 1);
        media_download_handle_query_resp(inst, dl->group_id, dl->query_peer, pkt, sizeof(pkt));
        if (dl->blocks[0].num_holders == 1 && !memcmp(dl->blocks[0].holders[0], dl->author_pubkey, 32)
            && dl->blocks[0].state == MD_BLK_REQ && !dl->query_deadline_tb) OK();
        else FAIL("empty result did not use author fallback");

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 11. назначение держателей + разброс ── */

static void test_query_resp_assign(void) {
    TEST("QUERY_RESP assigns holders (spread)"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 3);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }

        /* 2 держателя на каждый блок */
        uint64_t n1 = 0x1111, n2 = 0x2222;
        uint8_t pkt[MEDIA_QUERY_RESP_HDR_SIZE + 3 * sizeof(struct media_pkt_query_resp_entry)];
        query_response(dl, pkt, 3);
        struct media_pkt_query_resp_entry* entries = (struct media_pkt_query_resp_entry*)(pkt + MEDIA_QUERY_RESP_HDR_SIZE);
        for (int i = 0; i < 3; i++) {
            entries[i].num_holders = 2; media_test_public(n1, entries[i].holders[0]); media_test_public(n2, entries[i].holders[1]);
        }
        media_download_handle_query_resp(inst, dl->group_id, dl->query_peer, pkt, sizeof(pkt));

        /* блоки разложены: holder_idx = i % num_holders (стартовый разброс) */
        int ok = 1;
        for (int i = 0; i < 3; i++)
            if (dl->blocks[i].num_holders != 2) ok = 0;
        if (dl->num_peers != 2) ok = 0;
        struct media_download_block* second = &dl->blocks[1];
        if (second->holder_idx != 1 || md_dl_failover_block(dl, 1, get_time_tb()) || second->holder_idx != 0 ||
            second->num_holders != 2 || second->state != MD_BLK_REQ) ok = 0;
        if (ok) OK(); else FAIL("h0=%d h1=%d h2=%d peers=%d", dl->blocks[0].num_holders, dl->blocks[1].num_holders, dl->blocks[2].num_holders, dl->num_peers);

        free_test_dl(inst, dl);
        sqlite3_close(db); u_free(inst);
    }
}

/* ── 12. дедуп ── */

static int dedup_callbacks;
static void dedup_done_cb(void* arg, int err) { *(int*)arg = err; dedup_callbacks++; }

static void test_dedup(void) {
    TEST("shared download keeps other request after unsubscribe"); {
        sqlite3* db = NULL; sqlite3_open(":memory:", &db);
        struct UTUN_INSTANCE* inst = make_minimal_instance(db);
        struct media_download* dl = make_test_dl(db, inst, 1);
        if (!dl) { FAIL("make_test_dl"); sqlite3_close(db); u_free(inst); return; }

        struct media_index_result r; memset(&r, 0, sizeof(r));
        memcpy(r.media_id, dl->media_id, 16);
        r.num_blocks = 1; r.file_size = CHUNK_SIZE; r.block_size = CHUNK_SIZE;
        r.block_ids = dl->blocks[0].block_id; r.block_sigs = dl->block_sigs; dedup_callbacks = 0;
        int first = 1, second = 1, rejected = 1;
        int rc = media_download_start(inst, dl->group_id, &r, dl->dest_path, g_temp_dir, dl->author_pubkey, dedup_done_cb, &first, NULL, NULL);
        rc |= media_download_start(inst, dl->group_id, &r, dl->dest_path, g_temp_dir, dl->author_pubkey, dedup_done_cb, &second, NULL, NULL);
        r.content_hash[0] = 1;
        int conflict = media_download_start(inst, dl->group_id, &r, dl->dest_path, g_temp_dir, dl->author_pubkey,
            dedup_done_cb, &rejected, NULL, NULL);
        int ok = !rc && conflict == -1 && !dedup_callbacks && inst->md.downloads->count == 1;
        ok = ok && !media_download_unsubscribe(inst, dl->group_id, dl->media_id, &first) && first == -2 && second == 1 &&
            dedup_callbacks == 1 && dl->active && inst->md.downloads->count == 1;
        free_test_dl(inst, dl);
        if (ok && second == -2 && rejected == 1 && dedup_callbacks == 2) OK();
        else FAIL("rc=%d first=%d second=%d callbacks=%d", rc, first, second, dedup_callbacks);
        sqlite3_close(db); u_free(inst);
    }
}

static void late_up(void* arg) { (*(int*)arg)++; }

/* Повторный QUERY и несколько блоков сохраняют один CM запрос; TIMEOUT отменяет поздний UP. */
static void test_cm_handle_lifetime(void) {
    TEST("CM handle survives QUERY; TIMEOUT cancels late UP");
    sqlite3* db = NULL; sqlite3_open(":memory:", &db);
    struct UTUN_INSTANCE* inst = make_minimal_instance(db);
    struct media_download* dl = make_test_dl(db, inst, 2);
    struct CONN_MGR_HANDLE* h = u_calloc(1, sizeof(*h));
    if (!dl || !h) { fprintf(stderr, "CM fixture allocation failed\n"); exit(1); }
    int late = 0;
    h->ua = g_ua; h->deliver_up_token = uasync_call_soon(g_ua, &late, late_up);
    dl->num_peers = 1; media_test_public(0x1111, dl->peers[0].ed); dl->peers[0].cm_handle = h;
    uint8_t wire[MEDIA_QUERY_RESP_HDR_SIZE + 2 * sizeof(struct media_pkt_query_resp_entry)] = {MEDIA_SUBCMD_QUERY_RESP};
    query_response(dl, wire, 2);
    struct media_pkt_query_resp_entry* entries = (struct media_pkt_query_resp_entry*)(wire + MEDIA_QUERY_RESP_HDR_SIZE);
    for (int bi = 0; bi < 2; bi++) { entries[bi].num_holders = 1; media_test_public(0x1111, entries[bi].holders[0]); }
    media_download_handle_query_resp(inst, dl->group_id, dl->query_peer, wire, sizeof(wire));
    media_download_handle_query_resp(inst, dl->group_id, dl->query_peer, wire, sizeof(wire));
    int retained = dl->num_peers == 1 && dl->peers[0].cm_handle == h && dl->blocks[0].state == MD_BLK_CONN
        && dl->blocks[1].state == MD_BLK_CONN && dl->blocks[0].holder_idx == 0 && dl->blocks[1].holder_idx == 0;
    md_dl_conn_cb(h, dl->peers[0].ed, dl->group_id, CONN_EVENT_TIMEOUT, dl);
    /* После close handle недействителен: проверяем владельца и отсутствие позднего UP. */
    int closed = !dl->peers[0].cm_handle;
    uasync_poll(g_ua, 0);
    int processed = !dl->pump_timer && dl->blocks[0].state == MD_BLK_REQ && dl->blocks[1].state == MD_BLK_REQ;
    free_test_dl(inst, dl); sqlite3_close(db); u_free(inst);
    uasync_poll(g_ua, 0);
    if (retained && closed && processed && !late) OK();
    else FAIL("retained=%d closed=%d processed=%d late=%d", retained, closed, processed, late);
}

/* Вызов pump из другого события не должен обходить срок OVERLOADED. */
static void test_overloaded_deadline(void) {
    TEST("pump preserves OVERLOADED retry deadline");
    sqlite3* db = NULL; sqlite3_open(":memory:", &db);
    struct UTUN_INSTANCE* inst = make_minimal_instance(db);
    struct media_download* dl = make_test_dl(db, inst, 1);
    add_holder(dl, 0, 0x1111); dl->blocks[0].holder_idx = 0; dl->blocks[0].state = MD_BLK_REQ; dl->inflight = 1;
    dl->blocks[0].author_tried = 1;
    struct media_pkt_block_overloaded ov = {.subcmd = MEDIA_SUBCMD_BLOCK_OVERLOADED, .retry_after_ms = 1000};
    memcpy(ov.media_id, dl->media_id, 16); memcpy(ov.block_id, dl->blocks[0].block_id, 16);
    memcpy(ov.transfer_id, dl->blocks[0].transfer_id, 16);
    media_download_handle_overloaded(inst, dl->group_id, media_test_public(0x1111, (uint8_t[32]){0}), (const uint8_t*)&ov, sizeof(ov));
    int rc = md_dl_check_stall(dl, get_time_tb());
    int ok = !rc && dl->blocks[0].state == MD_BLK_WAIT && dl->blocks[0].wait_reason == MD_WAIT_RETRY
        && dl->blocks[0].retry_at_tb > get_time_tb() && !dl->blocks[0].attempts && !dl->inflight;
    free_test_dl(inst, dl); sqlite3_close(db); u_free(inst);
    if (ok) OK(); else FAIL("retry deadline was bypassed");
}

int main(void) {
    test_setup();
    printf("=== test_media_delivery_download ===\n");

    test_chunk_offset_write();
    test_done_assembly();
    test_done_bad_sig();
    test_failover_stall();
    test_attempts_exhausted();
    test_check_stall();
    test_overloaded_next_holder();
    test_overloaded_wait();
    test_relay_full();
    test_query_resp_empty();
    test_query_resp_assign();
    test_dedup();
    test_cm_handle_lifetime();
    test_overloaded_deadline();

    printf("\n%d/%d passed, %d failed\n", g_passed, g_total, g_failed);
    test_cleanup();
    return g_failed > 0 ? 1 : 0;
}
