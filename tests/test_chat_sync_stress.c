// test_chat_sync_stress.c — 20 батчей случайных вставок на A и B, синхронизация, замер производительности
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#include "db_sync_fixture.h"
#include "test_membership.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "../src/utun_instance.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/chat/db_sync.h"
#include "topo_node_sqlite.h"
#include "../src/routing_layer/topo_group.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"

#define TOTAL_BATCHES    20
#define MAX_PER_BATCH    100
#define SYNC_TIMEOUT_TB  300000   // 30s
#define TOTAL_TIMEOUT_TB 600000   // 60s
#define POLL_INTERVAL_MS 5

#define NODE_ID_A 0xAAAAAAAAAAAAAAAAULL
#define NODE_ID_B 0xBBBBBBBBBBBBBBBBULL
#define INSTANCE_NAME "chat"
#define INSTANCE_ID   1

static struct UTUN_INSTANCE* inst_a = NULL;
static struct UTUN_INSTANCE* inst_b = NULL;
static struct DB_SYNC_INSTANCE* si_a = NULL;
static struct DB_SYNC_INSTANCE* si_b = NULL;
static struct UASYNC* ua = NULL;
static int test_phase = 0;
static void* timeout_id = NULL;
static uint32_t expected_total = 0;
static char log_path[320];
static unsigned int g_seed;
static char temp_dir[] = "/tmp/utun_chatsync_XXXXXX";
static char config_a[256], config_b[256];

static int write_file(const char* path, const char* fmt, ...) {
    va_list ap; FILE* f = fopen(path, "w"); if (!f) return -1;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fclose(f); return 0;
}
static char* get_pubkey(const char* path) {
    struct utun_config* cfg = parse_config(path); if (!cfg) return NULL;
    char* pub = strdup(cfg->global.my_public_key_hex); free_config(cfg); return pub;
}
static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) { return -1; }
    snprintf(config_a, sizeof(config_a), "%s/a.conf", temp_dir);
    snprintf(config_b, sizeof(config_b), "%s/b.conf", temp_dir);
    char db_path[320];
    snprintf(db_path, sizeof(db_path), "%s/db_a", temp_dir); utun_mkdir(db_path, 0755);
    snprintf(db_path, sizeof(db_path), "%s/db_b", temp_dir); utun_mkdir(db_path, 0755);
    snprintf(log_path, sizeof(log_path), "%s/test.log", temp_dir);
    if (write_file(config_a,
        "[global]\ntun_ip=10.200.0.1/24\ntun_ifname=tun200\n"
        "db_path=%s/db_a\ndb_sync_enabled=1\n\n"
        "[server: srv_a]\naddr=127.0.0.1:0\ntype=public\n\n[allowed_keys]\nallow_all=1\n",
        temp_dir) != 0) return -1;
    if (config_ensure_keys(config_a) != 0) return -1;
    char* pub_a = get_pubkey(config_a); if (!pub_a) return -1;
    if (write_file(config_b,
        "[global]\ntun_ip=10.200.0.2/24\ntun_ifname=tun201\n"
        "db_path=%s/db_b\ndb_sync_enabled=1\n\n"
        "[server: srv_b]\naddr=127.0.0.1:0\ntype=public\n\n"
        "[client: to_a]\nkeepalive=1\npeer_public_key=%s\nlink=srv_b:127.0.0.1:0\n\n"
        "[allowed_keys]\nallow_all=1\n",
        temp_dir, pub_a) != 0) { free(pub_a); return -1; }
    free(pub_a);
    if (config_ensure_keys(config_b) != 0) return -1;
    return 0;
}
static void cleanup_temp_configs(void) {
    unlink(config_a); unlink(config_b); unlink(log_path);
    char pa[320];
    snprintf(pa, sizeof(pa), "%s/db_a/sync", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_a/sync-wal", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_a/sync-shm", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_b/sync", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_b/sync-wal", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_b/sync-shm", temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/db_a", temp_dir); test_rmdir(pa);
    snprintf(pa, sizeof(pa), "%s/db_b", temp_dir); test_rmdir(pa);
    test_rmdir(temp_dir);
}

static void test_timeout(void* arg) { (void)arg; test_phase = 2; }

static int histories_match(void) {
    uint8_t a[32], b[32];
    return db_sync_count(si_a) == expected_total && db_sync_count(si_b) == expected_total
        && db_sync_read_root(si_a, a) == 0 && db_sync_read_root(si_b, b) == 0 && !memcmp(a, b, 32);
}
static int wait_for(const char* desc, int timeout_tb, uint64_t* elapsed_out) {
    uint64_t start = get_time_tb();
    uint32_t ca0 = si_a ? db_sync_count(si_a) : 0, cb0 = si_b ? db_sync_count(si_b) : 0;
    DEBUG_WARN(DEBUG_CATEGORY_DEBUG, "SYNC_START %s A=%u B=%u expect=%u", desc, ca0, cb0, expected_total);
    while (!histories_match()
           && (get_time_tb() - start) < (uint64_t)timeout_tb && test_phase == 0)
        uasync_poll(ua, POLL_INTERVAL_MS);
    uint32_t ca = si_a ? db_sync_count(si_a) : 0, cb = si_b ? db_sync_count(si_b) : 0;
    uint64_t elapsed = (get_time_tb() - start) / 10;
    int ok = histories_match();
    if (ok)
        DEBUG_WARN(DEBUG_CATEGORY_DEBUG, "SYNC_DONE %s A=%u B=%u %llums", desc, ca, cb, (unsigned long long)elapsed);
    else {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "SYNC_FAIL %s A=%u B=%u expect=%u %llums",
                    desc, ca, cb, expected_total, (unsigned long long)elapsed);
        if (test_phase == 0) test_phase = 2;
    }
    if (elapsed_out) *elapsed_out = (ok ? elapsed : 0);
    return ok;
}

/* Изолированная фикстура db_sync: полное членство задано заранее; проверка join-подписей — в test_chat_join_e2e. */
static void seed_members(struct UTUN_INSTANCE* self, struct UTUN_INSTANCE* peer) {
    sqlite3* db = self ? self->topo_sqlite_db : NULL;
    if (!db || !peer || !peer->node_id) return;
    static char ch_id[65];
    static struct test_group_keys keys; static uint8_t signature[64]; static int initialized;
    const uint8_t owner[32] = {1};
    if (!initialized) {
        if (test_group_keys_generate(&keys)) abort();
        sc_public_key_hex(keys.ed, ch_id);
        if (test_group_signature(ch_id, INSTANCE_NAME, owner, keys.x.public_key, keys.ed, keys.ed_private, signature)) abort();
        initialized = 1;
    }
    uint8_t z64[64] = {0};
    if (topo_node_sqlite_channel_put(db, ch_id, INSTANCE_NAME, owner, keys.x.public_key, NULL, keys.ed, NULL, signature)) abort();
    if (topo_node_sqlite_node_update_verified(db, peer->node_id, "stress peer", peer->my_keys.public_key,
        peer->my_ed25519_pubkey, 1, 0) < 0 || topo_node_sqlite_member_block_put(db, ch_id, peer->node_id, z64, 1,
        NULL, 0, peer->my_keys.public_key, peer->my_ed25519_pubkey, "{}", 0, z64) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "stress member fixture failed peer=%016llx", (unsigned long long)peer->node_id); abort();
    }
    if (topo_node_sqlite_member_block_put(db, ch_id, self->node_id, z64, 1, NULL, 0,
        self->my_keys.public_key, self->my_ed25519_pubkey, "{}", 0, z64) < 0) abort();
    char admission[128]; snprintf(admission, sizeof(admission), "UPDATE peers_%s SET admitted=1,identity_verified=1", ch_id);
    if (sqlite3_exec(db, admission, NULL, NULL, NULL) != SQLITE_OK) abort();
    if (test_member_publish_access(self, ch_id)) abort();
    if (!topo_groups_find(self->topo_groups, INSTANCE_ID))
        topo_groups_create_group(self->topo_groups, INSTANCE_ID, TOPO_GROUP_TYPE_CHAT, ch_id);
}

static int insert_record(struct DB_SYNC_INSTANCE* si, struct UTUN_INSTANCE* inst, const char* data, size_t len) {
    uint64_t ts = db_sync_next_timestamp(si);
    uint8_t sig[64]; struct db_message_revision revision = {.nonce = {1}, .ver = 1};
    if (db_sync_local_revision(si, &revision)) return -1;
    if (db_sync_sign_message(inst->topo_sqlite_db, db_sync_instance_channel(si), ts, inst->my_ed25519_pubkey, &revision, data, len,
        inst->my_ed25519_privkey, sig) < 0) return -1;
    return db_sync_insert_signed(si, data, len, sig, 64, ts, NULL, &revision);
}

int main(void) {
    g_seed = (unsigned)time(NULL);
    srand(g_seed);

    debug_config_init();
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_TRACE);
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_TRACE);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_TRACE);
    debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);

    if (create_temp_configs() != 0) { fprintf(stderr, "FAIL: config creation\n"); return 1; }
    debug_enable_file_output(log_path, 1);
    DEBUG_WARN(DEBUG_CATEGORY_DEBUG, "START seed=%u temp=%s", g_seed, temp_dir);

    utun_instance_set_tun_init_enabled(0);
    ua = uasync_create();
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "uasync_create"); cleanup_temp_configs(); return 1; }
    inst_a = utun_instance_create(ua, config_a);
    inst_b = utun_instance_create(ua, config_b);
    if (!inst_a || !inst_b) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "instance_create"); cleanup_temp_configs(); return 1; }
    test_etcp_set_peer(inst_b, "to_a", inst_a, "srv_a");
    if (utun_instance_init(inst_a) != 0 || utun_instance_init(inst_b) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "instance_init"); cleanup_temp_configs(); return 1;
    }
    seed_members(inst_a, inst_b);
    seed_members(inst_b, inst_a);
    si_a = db_sync_instance_add(inst_a, INSTANCE_NAME, INSTANCE_ID);
    si_b = db_sync_instance_add(inst_b, INSTANCE_NAME, INSTANCE_ID);
    if (!si_a || !si_b) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "db_sync_instance_add"); return 1; }
    if (setup_history_model(inst_a) < 0 || setup_history_model(inst_b) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "history model registration"); return 1;
    }
    db_sync_instance_set_gated(si_a, 0); db_sync_instance_set_gated(si_b, 0);
    timeout_id = uasync_set_timeout(ua, TOTAL_TIMEOUT_TB, NULL, test_timeout, "total_timeout");

    uint64_t t0 = get_time_tb();

    // Phase 1: local inserts on A
    if (insert_record(si_a, inst_a, "{\"test\":1}", 11) != 0
     || insert_record(si_a, inst_a, "{\"test\":2}", 11) != 0
     || db_sync_count(si_a) != 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "PHASE1_FAIL A=%u", db_sync_count(si_a));
        test_phase = 2; goto cleanup;
    }
    DEBUG_WARN(DEBUG_CATEGORY_DEBUG, "PHASE1_OK A=%u", db_sync_count(si_a));

    // Phase 2: initial sync A→B
    expected_total = 2;
    { uint64_t p2elapsed; if (!wait_for("phase2", SYNC_TIMEOUT_TB, &p2elapsed)) { test_phase = 2; goto cleanup; } }
    DEBUG_WARN(DEBUG_CATEGORY_DEBUG, "PHASE2_OK A=%u B=%u", db_sync_count(si_a), db_sync_count(si_b));

    // Phase 3: 20 random batches
    int global_seq = 3;
    uint64_t insert_total_tb = 0;
    int insert_count = 0;
    uint32_t records_a = 0, records_b = 0;

    uint64_t t_insert = get_time_tb();
    int large = getenv("UTUN_TEST_HISTORY_LARGE") != NULL;
    for (int b = 0; b < TOTAL_BATCHES && test_phase == 0; b++) {
        int n = large ? 250 : rand() % (MAX_PER_BATCH + 1);
        if (n == 0) continue;
        int side = large ? b & 1 : rand() & 1;
        struct DB_SYNC_INSTANCE* si = side ? si_b : si_a;
        uint64_t node = side ? NODE_ID_B : NODE_ID_A;

        uint64_t bt0 = get_time_tb();
        for (int i = 0; i < n; i++) {
            char buf[384];
            snprintf(buf, sizeof(buf),
                "{\"seq\":%d,\"n\":%llu,\"ch\":\"test\",\"ct\":\"text\",\"d\":\"msg_%d\","
                "\"pad\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}",
                global_seq + i, (unsigned long long)node, global_seq + i);
            if (insert_record(si, side ? inst_b : inst_a, buf, strlen(buf)) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "INSERT_FAIL seq=%d", global_seq + i);
                test_phase = 2; break;
            }
        }
        uint64_t bt = get_time_tb() - bt0;
        insert_total_tb += bt;
        insert_count += n;
        if (side) records_b += n; else records_a += n;

        global_seq += n;
        expected_total += n;
        if (b % 4 == 3) uasync_poll(ua, POLL_INTERVAL_MS);
    }
    if (test_phase != 0) goto cleanup;

    uint64_t insert_total_ms = insert_total_tb / 10;
    DEBUG_WARN(DEBUG_CATEGORY_DEBUG, "INSERT_DONE records=%d A=%u B=%u avg_=%lluus/rec",
               insert_count, records_a, records_b,
               insert_count > 0 ? (unsigned long long)(insert_total_tb * 100 / insert_count) : 0);

    // Phase 4: sync wait
    uint64_t sync_elapsed = 0;
    if (!wait_for("sync_final", SYNC_TIMEOUT_TB, &sync_elapsed)) { test_phase = 2; goto cleanup; }

    uint64_t total_ms = (get_time_tb() - t0) / 10;
    if (test_phase == 0) {
        uint32_t ca = db_sync_count(si_a), cb = db_sync_count(si_b);
        DEBUG_WARN(DEBUG_CATEGORY_DEBUG, "ALL_OK records=%d insert=%llums sync=%llums total=%llums A=%u B=%u",
                   insert_count, (unsigned long long)insert_total_ms,
                   (unsigned long long)sync_elapsed, (unsigned long long)total_ms, ca, cb);
        printf("PROFILE: records=%d insert_avg=%lluus sync=%llums total=%llums A=%u B=%u\n",
               insert_count,
               insert_count > 0 ? (unsigned long long)(insert_total_tb * 100 / insert_count) : 0,
               (unsigned long long)sync_elapsed, (unsigned long long)total_ms, ca, cb);
    }

cleanup:
    DEBUG_WARN(DEBUG_CATEGORY_DEBUG, "CLEANUP A=%u B=%u phase=%d",
                 si_a ? db_sync_count(si_a) : 0, si_b ? db_sync_count(si_b) : 0, test_phase);
    if (timeout_id && ua) { uasync_cancel_timeout(ua, timeout_id); timeout_id = NULL; }
    if (si_a) { db_sync_instance_remove(si_a); si_a = NULL; }
    if (si_b) { db_sync_instance_remove(si_b); si_b = NULL; }
    if (inst_a) { inst_a->running = 0; if (!inst_a->chat_sync) merkle_sync_destroy(inst_a); utun_instance_destroy(inst_a); inst_a = NULL; }
    if (inst_b) { inst_b->running = 0; if (!inst_b->chat_sync) merkle_sync_destroy(inst_b); utun_instance_destroy(inst_b); inst_b = NULL; }
    if (ua) { uasync_destroy(ua, 0); ua = NULL; }
    debug_disable_file_output();

    int result = (test_phase == 0) ? 0 : 1;
    fprintf(stderr, "=== %s seed=%u (log: %s) ===\n", result ? "FAIL" : "PASS", g_seed, log_path);
    if (!result) cleanup_temp_configs();
    return result;
}
