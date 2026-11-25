// test_media_download_timeout.c — интеграционный тест watchdog-таймера скачивания
//
// Проверяет, что реальный uasync-таймер простоя срабатывает:
//   - загрузка с недостижимым автором → watchdog (100ms) → re-QUERY → суперноды
//     исчерпаны → done_cb(-1).
//   - загрузка с держателем, который «завис» (нет чанков) → фейловер → 3 попытки →
//     done_cb(-1).
//
// Таймауты укорочены (dl_stall_timeout_tb=100ms), фейковое время не используется —
// гоняем реальный цикл uasync_poll.

#include "media_delivery.h"
#include "media_delivery_proto.h"
#include "media_download.h"
#include "media_index.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/utun_instance.h"
#include "../lib/platform_compat.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media_members_fixture.h"

/* Транспорт принимает пакет; недостижимый получатель не отвечает. */
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t dst[32],
                           struct ll_entry* entry, int force, int mode) {
    (void)inst; (void)group; (void)dst; (void)force; (void)mode;
    queue_dgram_free(entry); queue_entry_free(entry); return 0;
}

static int G_PASSED = 0, G_FAILED = 0, G_TOTAL = 0;
#define TEST(n)  do { G_TOTAL++; printf("  %-58s", n); fflush(stdout); } while(0)
#define OK()     do { G_PASSED++; printf("OK\n"); } while(0)
#define FAIL(f,...) do { G_FAILED++; printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

static struct UASYNC* g_ua = NULL;
static int g_done = 0, g_err = 0;

static void make_uuid(uint8_t b[16]) { for (int i = 0; i < 16; i++) b[i] = (uint8_t)(rand() & 0xFF); }

static void done_cb(void* a, int e) { (void)a; g_done++; g_err = e; printf("  [done_cb err=%d]\n", e); }

static struct UTUN_INSTANCE* make_minimal(void) {
    struct UTUN_INSTANCE* inst = u_calloc(1, sizeof(*inst));
    if (!inst) return NULL;
    inst->ua = g_ua;
    inst->node_id = 0xDEADBEEFDEADBEEFULL;
    sqlite3* db = NULL; sqlite3_open(":memory:", &db);
    inst->topo_sqlite_db = db;
    const uint64_t peers[] = {0xBADF00D00000001ULL, 0xBADF00D00000002ULL};
    media_test_group(inst, 0x1234, peers, 2);
    if (media_index_init(db) || media_index_set_base(db, "/tmp")) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "timeout test media schema initialization failed"); exit(1);
    }
    if (sqlite3_exec(db, "CREATE TABLE nodes(node_id INTEGER PRIMARY KEY,ed25519_pubkey BLOB);"
        "INSERT INTO nodes VALUES(0xBADF00D00000001,zeroblob(32)),(0xBADF00D00000002,zeroblob(32))",
        NULL, NULL, NULL) != SQLITE_OK) exit(1);
    inst->md.inst = inst; inst->md.db = db; inst->md.initialized = 1;
    inst->md.dl_stall_timeout_tb = 1000;   /* 100ms */
    inst->md.query_timeout_tb = 1000;
    inst->md.dl_max_attempts = 3;
    return inst;
}

static void make_result(struct media_index_result* r, int num_blocks) {
    memset(r, 0, sizeof(*r));
    make_uuid(r->media_id);
    r->num_blocks = num_blocks; r->file_size = 100 * num_blocks; r->block_size = 100;
    r->block_ids = u_calloc((size_t)num_blocks, 16);
    r->block_sigs = u_calloc((size_t)num_blocks, 64);
    for (int i = 0; i < num_blocks; i++) make_uuid(r->block_ids + i * 16);
}

static int poll_until_done(int max_ms) {
    int el = 0;
    while (!g_done && el < max_ms) { uasync_poll(g_ua, 5); el += 5; }
    return g_done;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) { debug_set_category_level_by_name("media", "trace"); debug_set_category_level_by_name("general", "info"); }
    srand(1);
    printf("=== test_media_download_timeout ===\n");
    g_ua = uasync_create();

    /* ── 1. недостижимый автор → watchdog → done_cb(-1) ── */
    TEST("unreachable author → watchdog → fail"); {
        struct UTUN_INSTANCE* inst = make_minimal();
        struct media_index_result r; make_result(&r, 1);
        g_done = 0; g_err = 0;
        media_test_publish(inst->topo_sqlite_db, 0x1234, 0xBADF00D00000001ULL, &r);
        int rc = media_download_start(inst, 0x1234, &r, "/tmp/utun_mdt_1.bin", "/tmp", media_test_public(0xBADF00D00000001ULL, (uint8_t[32]){0}), done_cb, NULL, NULL, NULL);
        if (rc != 0) { FAIL("start rc=%d", rc); }
        else if (poll_until_done(3000) && g_err != 0) OK();
        else FAIL("done=%d err=%d", g_done, g_err);

        media_index_result_free(&r);
        /* нет активной загрузки после finish */
        if (inst->md.downloads) { struct ll_entry* e = inst->md.downloads->head; int n = 0; while (e) { n++; e = e->next; } if (n != 0) FAIL("downloads leftover=%d", n); }
        media_delivery_destroy(inst); media_test_groups_free(inst); sqlite3_close(inst->topo_sqlite_db); u_free(inst);
    }

    /* ── 2. зависший держатель → фейловер → re-QUERY → fail ── */
    TEST("stalled holder → failover → fail"); {
        struct UTUN_INSTANCE* inst = make_minimal();
        struct media_index_result r; make_result(&r, 1);
        g_done = 0; g_err = 0;
        media_test_publish(inst->topo_sqlite_db, 0x1234, 0xBADF00D00000002ULL, &r);
        int rc = media_download_start(inst, 0x1234, &r, "/tmp/utun_mdt_2.bin", "/tmp", media_test_public(0xBADF00D00000002ULL, (uint8_t[32]){0}), done_cb, NULL, NULL, NULL);
        if (rc != 0) {
            FAIL("start rc=%d", rc);
        } else {
            /* имитируем QUERY_RESP: автор сообщает, что держит блок */
            struct media_pkt_query_resp_entry e;
            memset(&e, 0, sizeof(e));
            e.num_holders = 1; media_test_public(0xBADF00D00000002ULL, e.holders[0]);
            memcpy(e.block_id, r.block_ids, 16);
            uint8_t pkt[MEDIA_QUERY_RESP_HDR_SIZE + sizeof(e)] = {0};
            struct media_download* dl = (struct media_download*)inst->md.downloads->head->data;
            struct media_pkt_query_resp* response = (struct media_pkt_query_resp*)pkt;
            response->subcmd = MEDIA_SUBCMD_QUERY_RESP; response->num_blocks = 1;
            memcpy(response->media_id, r.media_id, 16); memcpy(response->request_id, dl->query_id, 16);
            memcpy(pkt + MEDIA_QUERY_RESP_HDR_SIZE, &e, sizeof(e));
            media_download_handle_query_resp(inst, dl->group_id, dl->query_peer, pkt, sizeof(pkt));

            /* держатель «завис» — чанков нет, watchdog сделает фейловер → попытки → fail */
            if (poll_until_done(5000) && g_err != 0) OK();
            else FAIL("done=%d err=%d (expected error after 3 attempts)", g_done, g_err);
        }

        media_index_result_free(&r);
        media_delivery_destroy(inst); media_test_groups_free(inst); sqlite3_close(inst->topo_sqlite_db); u_free(inst);
    }

    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED);
    if (g_ua) { uasync_destroy(g_ua, 0); g_ua = NULL; }
    return G_FAILED > 0 ? 1 : 0;
}
