#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "merkle_sync.h"
#include "../src/utun_instance.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/config_updater.h"
#include "../src/tun_if.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "node_conn_direct.h"
#include "test_utils.h"
#include "test_clock.h"
#include "test_net.h"
#include <sqlite3.h>
#include <openssl/evp.h>

/* numeric channel_id == group_id для CHAT-групп (recv требует существующую группу) */
#define MS_NS_TEST       "1001"
#define MS_NS_RND        "1002"
#define MS_NS_STRESS     "1003"

static void _ms_ensure_groups(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->topo_groups) return;
    const char* nss[] = { MS_NS_TEST, MS_NS_RND, MS_NS_STRESS };
    for (int i = 0; i < 3; i++) {
        uint64_t gid = strtoull(nss[i], NULL, 10);
        if (topo_groups_find(inst->topo_groups, gid)) continue;
        if (topo_node_sqlite_member_local_init(inst->topo_sqlite_db, nss[i]) ||
            !topo_groups_create_group(inst->topo_groups, gid, TOPO_GROUP_TYPE_CHAT, nss[i])) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "test CHAT group setup failed ns=%s", nss[i]); abort();
        }
    }
}

static int tests_run = 0;
static int tests_failed = 0;
static int stage_failures = 0;

#define TEST(name) do { tests_run++; printf("  %s: ", name); } while(0)
#define PASS() do { printf("PASS\n"); } while(0)
#define FAIL(fmt, ...) do { printf("FAIL — " fmt "\n", ##__VA_ARGS__); tests_failed++; stage_failures++; } while(0)

/* ── Stage 2: mock data store ── */

#define MS_MAX_ITEMS 8192

struct ms_item { uint64_t key; uint32_t val; };

struct ms_data {
    struct ms_item items[MS_MAX_ITEMS];
    int            count;  /* sorted by key */
};

static int _cmp_item(const void* a, const void* b) {
    uint64_t ka = ((const struct ms_item*)a)->key, kb = ((const struct ms_item*)b)->key;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

static void _data_sort(struct ms_data* d) { qsort(d->items, (size_t)d->count, sizeof(struct ms_item), _cmp_item); }

static void _data_init(struct ms_data* d) { memset(d, 0, sizeof(*d)); }

static void _data_insert(struct ms_data* d, uint64_t key, uint32_t val) {
    for (int i = 0; i < d->count; i++)
        if (d->items[i].key == key) { d->items[i].val = val; return; }
    if (d->count >= MS_MAX_ITEMS) return;
    d->items[d->count].key = key; d->items[d->count].val = val;
    d->count++;
}

static int _data_count_in_prefix(struct ms_data* d, uint8_t level, uint64_t prefix64) {
    int mask_shift = 64 - (int)level * 5;
    uint64_t mask = (mask_shift >= 0 && mask_shift < 64) ? (~0ULL << mask_shift) : UINT64_MAX;
    int cnt = 0;
    for (int i = 0; i < d->count; i++)
        if ((d->items[i].key & mask) == prefix64) cnt++;
    return cnt;
}

/* ── Stage 2: mock merkle_sync_data_ops ── */

struct ms_test_ctx {
    struct ms_data*  data;
    struct UTUN_INSTANCE* inst;
    char             tag;  /* 'A'/'B'/'C' for debug */
};

static void _compute_item_hash(uint64_t key, uint32_t val, uint8_t out[MT_HASH_SIZE]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, &key, 8);
    EVP_DigestUpdate(ctx, &val, 4);
    EVP_DigestFinal_ex(ctx, out, NULL);
    EVP_MD_CTX_free(ctx);
}

static int _bucket_hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix64, EVP_MD_CTX* sha_ctx) {
    (void)ns;
    struct ms_data* d = ((struct ms_test_ctx*)ctx)->data;
    int mask_shift = 64 - (int)level * 5;
    uint64_t mask = (level == 0) ? 0 : ((mask_shift >= 0 && mask_shift < 64) ? (~0ULL << mask_shift) : UINT64_MAX);
    int count = 0;
    for (int i = 0; i < d->count; i++) {
        if ((d->items[i].key & mask) != prefix64) continue;
        uint8_t ih[MT_HASH_SIZE]; _compute_item_hash(d->items[i].key, d->items[i].val, ih);
        EVP_DigestUpdate(sha_ctx, ih, MT_HASH_SIZE);
        count++;
    }
    return count;
}

static int _get_page(void* ctx, const char* ns, uint64_t prefix, int after_valid, const merkle_key_t* after,
        uint8_t* buf, size_t* len, merkle_key_t* next, int* more) {
    (void)ns;
    struct ms_data* d = ((struct ms_test_ctx*)ctx)->data;
    uint16_t count = 0; size_t off = 2;
    memset(next, 0, sizeof(*next)); *more = 0;
    uint64_t after_id; memcpy(&after_id, after->bytes, 8); after_id = be64toh(after_id);
    for (int i = 0; i < d->count; i++) {
        if (merkle_sync_level_prefix(d->items[i].key, MT_MAX_LEVEL) != prefix
            || (after_valid && d->items[i].key <= after_id)) continue;
        if (off + 12 > *len) { if (!count) return -1; *more = 1; break; }
        memcpy(buf + off, &d->items[i].key, 8); off += 8;
        memcpy(buf + off, &d->items[i].val, 4); off += 4;
        uint64_t next_id = htobe64(d->items[i].key); memcpy(next->bytes, &next_id, 8); count++;
    }
    memcpy(buf, &count, 2); *len = off; return 0;
}

static int _apply_items(void* ctx, const char* ns, const uint8_t from_peer[32], const uint8_t* data, size_t len) {
    (void)from_peer;
    struct ms_test_ctx* tc = ctx;
    struct ms_data* d = tc->data;
    if (len < 2) return -1;
    uint16_t count; memcpy(&count, data, 2);
    if (len != 2 + (size_t)count * 12) return -1;
    int changed = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint64_t key; uint32_t val;
        memcpy(&key, data + 2 + (size_t)i * 12, 8);
        memcpy(&val, data + 10 + (size_t)i * 12, 4);
        int found = 0;
        for (int j = 0; j < d->count; j++) {
            if (d->items[j].key != key) continue;
            if (d->items[j].val < val) { d->items[j].val = val; changed = 1; }
            found = 1; break;
        }
        if (!found) { if (d->count == MS_MAX_ITEMS) return -1; _data_insert(d, key, val); changed = 1; }
    }
    if (changed) {
        _data_sort(d);
        /* Страница всегда относится к одному листу: пересчитываем его один раз. */
        uint64_t key; memcpy(&key, data + 2, 8);
        if (merkle_sync_recompute_path(tc->inst, MT_DATA_MEMBERS, ns, key) < 0) return -1;
    }
    return 0;
}

static const struct merkle_sync_data_ops g_test_ops = {
    .update_bucket_hash = _bucket_hash, .get_page = _get_page, .apply_items = _apply_items
};
static struct merkle_sync_data_ops g_owned_ops; /* Сетевые стенды проверяют также владение активным транспортом. */

static const uint8_t* _test_hash(struct UTUN_INSTANCE* inst, const char* ns, uint8_t level, uint64_t prefix) {
    static uint8_t hash[MT_HASH_SIZE];
    if (merkle_sync_read_hash(inst, MT_DATA_MEMBERS, ns, level, prefix, hash) < 0) abort();
    return hash;
}

/* Независимое восстановление дерева из набора записей, без чтения индекса. */
static void _expected_hash(struct ms_test_ctx* tc, const char* ns, uint8_t level, uint64_t prefix, uint8_t hash[MT_HASH_SIZE]) {
    memset(hash, 0, MT_HASH_SIZE);
    int present = 0;
    for (int i = 0; i < tc->data->count; i++)
        if (merkle_sync_level_prefix(tc->data->items[i].key, level) == prefix) { present = 1; break; }
    if (!present) return;
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    uint8_t domain[2] = { 0x4d, level }; EVP_DigestUpdate(c, domain, sizeof(domain));
    if (level == MT_MAX_LEVEL) _bucket_hash(tc, ns, level, prefix, c);
    else {
        for (unsigned i = 0; i < MT_BUCKETS; i++) {
            uint8_t child[MT_HASH_SIZE];
            _expected_hash(tc, ns, level + 1, prefix | ((uint64_t)i << (64 - (level + 1) * 5)), child);
            EVP_DigestUpdate(c, child, sizeof(child));
        }
    }
    EVP_DigestFinal_ex(c, hash, NULL); EVP_MD_CTX_free(c);
}

/* ── Stage 1: prefix arithmetic ── */

static void test_prefix_bytes(void) {
    TEST("prefix_bytes(1)==1");
    if (merkle_sync_prefix_bytes(1) == 1) PASS(); else FAIL("got %d", merkle_sync_prefix_bytes(1));

    TEST("prefix_bytes(2)==2");
    if (merkle_sync_prefix_bytes(2) == 2) PASS(); else FAIL("got %d", merkle_sync_prefix_bytes(2));

    TEST("prefix_bytes(3)==2");
    if (merkle_sync_prefix_bytes(3) == 2) PASS(); else FAIL("got %d", merkle_sync_prefix_bytes(3));

    TEST("prefix_bytes(4)==3");
    if (merkle_sync_prefix_bytes(4) == 3) PASS(); else FAIL("got %d", merkle_sync_prefix_bytes(4));

    TEST("prefix_bytes(5)==4");
    if (merkle_sync_prefix_bytes(5) == 4) PASS(); else FAIL("got %d", merkle_sync_prefix_bytes(5));
}

static void test_level_prefix_basic(void) {
    uint64_t key = 0xFEDCBA9876543210ULL;

    TEST("level_prefix(0xFEDCBA.., 1)==0xF800..");
    uint64_t got = merkle_sync_level_prefix(key, 1);
    if (got == 0xF800000000000000ULL) PASS(); else FAIL("expected f800000000000000 got %016llx", (unsigned long long)got);

    TEST("level_prefix(0,3)==0");
    got = merkle_sync_level_prefix(0, 3);
    if (got == 0) PASS(); else FAIL("got %016llx", (unsigned long long)got);

    TEST("level_prefix(MAX,1)==0xF800000000000000");
    got = merkle_sync_level_prefix(UINT64_MAX, 1);
    if (got == 0xF800000000000000ULL) PASS(); else FAIL("got %016llx", (unsigned long long)got);

    TEST("level_prefix(MAX,5)==0xFFFFFF8000000000");
    got = merkle_sync_level_prefix(UINT64_MAX, 5);
    if (got == 0xFFFFFF8000000000ULL) PASS(); else FAIL("got %016llx", (unsigned long long)got);

    TEST("level_prefix(0,0)==0");
    got = merkle_sync_level_prefix(0, 0);
    if (got == 0) PASS(); else FAIL("got %016llx", (unsigned long long)got);
}

static void test_level_prefix_idempotent(void) {
    TEST("level_prefix idempotent L3");
    uint64_t key = 0x0123456789ABCDEFULL;
    uint64_t p = merkle_sync_level_prefix(key, 3);
    uint64_t p2 = merkle_sync_level_prefix(p, 3);
    if (p == p2) PASS(); else FAIL("p=%016llx p2=%016llx", (unsigned long long)p, (unsigned long long)p2);
}

static void test_level_prefix_bucket_partition(void) {
    TEST("same bucket → same prefix L1..L2");
    uint64_t k1 = 0x8900000000000000ULL;
    uint64_t k2 = 0x8911111111111111ULL;
    int ok = 1;
    for (uint8_t l = 1; l <= 2; l++)
        if (merkle_sync_level_prefix(k1, l) != merkle_sync_level_prefix(k2, l)) ok = 0;
    if (merkle_sync_level_prefix(k1, 3) == merkle_sync_level_prefix(k2, 3)) ok = 0;
    if (ok) PASS(); else FAIL("partition broken");

    TEST("different L1 bucket → different prefix L1");
    uint64_t a = 0x0000000000000000ULL;
    uint64_t b = 0x0800000000000000ULL;
    if (merkle_sync_level_prefix(a, 1) != merkle_sync_level_prefix(b, 1)) PASS(); else FAIL("should differ");
}

/* ── Stage 2: tree building tests ── */

static void _ensure_table(sqlite3* db) { if (merkle_tree_init(db) < 0) abort(); }

static int _db_count_rows(sqlite3* db, const char* ns) {
    char tree_ns[25]; snprintf(tree_ns, sizeof(tree_ns), "0:%s", ns);
    sqlite3_stmt* st = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM merkle_tree_hash WHERE namespace=?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, tree_ns, -1, SQLITE_STATIC);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int _db_compare_trees(sqlite3* da, sqlite3* db, const char* ns) {
    char tree_ns[25]; snprintf(tree_ns, sizeof(tree_ns), "0:%s", ns);
    sqlite3_stmt* sa = NULL, *sb = NULL;
    sqlite3_prepare_v2(da, "SELECT level,prefix64,hash FROM merkle_tree_hash WHERE namespace=? ORDER BY level,prefix64",
                       -1, &sa, NULL);
    sqlite3_prepare_v2(db, "SELECT level,prefix64,hash FROM merkle_tree_hash WHERE namespace=? ORDER BY level,prefix64",
                       -1, &sb, NULL);
    sqlite3_bind_text(sa, 1, tree_ns, -1, SQLITE_STATIC);
    sqlite3_bind_text(sb, 1, tree_ns, -1, SQLITE_STATIC);
    int mismatch = 0;
    while (1) {
        int ra = sqlite3_step(sa), rb = sqlite3_step(sb);
        if (ra == SQLITE_DONE && rb == SQLITE_DONE) break;
        if (ra == SQLITE_DONE) { mismatch++; break; }
        if (rb == SQLITE_DONE) { mismatch++; break; }
        if (sqlite3_column_int(sa,0) != sqlite3_column_int(sb,0) ||
            (uint64_t)sqlite3_column_int64(sa,1) != (uint64_t)sqlite3_column_int64(sb,1)) {
            mismatch++; break;
        }
        if (memcmp(sqlite3_column_blob(sa,2), sqlite3_column_blob(sb,2), MT_HASH_SIZE) != 0) { mismatch++; break; }
    }
    sqlite3_finalize(sa); sqlite3_finalize(sb);
    return mismatch;
}

static void test_tree_empty_ns(void) {
    TEST("empty namespace → no rows in merkle_tree_hash");
    sqlite3* db = NULL; sqlite3_open(":memory:", &db);
    _ensure_table(db);

    static struct UTUN_INSTANCE inst; memset(&inst, 0, sizeof(inst));
    inst.topo_sqlite_db = db;
    struct ms_data data; _data_init(&data);
    struct ms_test_ctx ctx = { .data = &data, .inst = &inst };
    inst.ua = uasync_create();
    if (merkle_sync_init(&inst, 0x72, &g_test_ops, &ctx) != 0) abort();

    merkle_sync_recompute_path(&inst, MT_DATA_MEMBERS, MS_NS_TEST, 0x1234ULL);
    if (_db_count_rows(db, MS_NS_TEST) == 0) PASS(); else FAIL("got %d rows", _db_count_rows(db, MS_NS_TEST));

    merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db);
}

static void test_tree_single_item(void) {
    TEST("single item → 5 rows (L1..L5) with non-zero hash");
    sqlite3* db = NULL; sqlite3_open(":memory:", &db);
    _ensure_table(db);

    static struct UTUN_INSTANCE inst; memset(&inst, 0, sizeof(inst));
    inst.topo_sqlite_db = db;
    struct ms_data data; _data_init(&data);
    uint64_t key = 0xABCD000000000000ULL;
    _data_insert(&data, key, 42);
    _data_sort(&data);
    struct ms_test_ctx ctx = { .data = &data, .inst = &inst };
    inst.ua = uasync_create();
    if (merkle_sync_init(&inst, 0x72, &g_test_ops, &ctx) != 0) abort();

    merkle_sync_recompute_path(&inst, MT_DATA_MEMBERS, MS_NS_TEST, key);
    int rows = _db_count_rows(db, MS_NS_TEST);
    if (rows != 6) { FAIL("expected 6 rows got %d", rows); merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db); return; }

    /* check hashes are non-zero */
    int ok = 1;
    for (uint8_t lv = 1; lv <= 5 && ok; lv++) {
        uint64_t pf = merkle_sync_level_prefix(key, lv);
        const uint8_t* h = _test_hash(&inst, MS_NS_TEST, lv, pf);
        int zero = 1;
        for (int i = 0; i < MT_HASH_SIZE; i++) if (h[i] != 0) zero = 0;
        if (zero) { FAIL("L%d zero hash", lv); ok = 0; }
    }
    if (ok) PASS();

    merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db);
}

static void test_tree_multi_item_same_bucket(void) {
    TEST("two items same L1 bucket → 5 rows, L1 hash aggregates both");
    sqlite3* db = NULL; sqlite3_open(":memory:", &db);
    _ensure_table(db);

    static struct UTUN_INSTANCE inst; memset(&inst, 0, sizeof(inst));
    inst.topo_sqlite_db = db;
    struct ms_data data; _data_init(&data);
    uint64_t k1 = 0xAA00000000000000ULL;
    uint64_t k2 = 0xAB00000000000000ULL; /* bits 63-59: 10101 for both */
    _data_insert(&data, k1, 1); _data_insert(&data, k2, 2);
    _data_sort(&data);
    struct ms_test_ctx ctx = { .data = &data, .inst = &inst };
    inst.ua = uasync_create();
    if (merkle_sync_init(&inst, 0x72, &g_test_ops, &ctx) != 0) abort();

    /* recompute for both */
    merkle_sync_recompute_path(&inst, MT_DATA_MEMBERS, MS_NS_TEST, k1);
    merkle_sync_recompute_path(&inst, MT_DATA_MEMBERS, MS_NS_TEST, k2);

    uint64_t pf1 = merkle_sync_level_prefix(k1, 1);
    uint64_t pf2 = merkle_sync_level_prefix(k2, 1);
    if (pf1 != pf2) { FAIL("L1 prefixes should be equal"); merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db); return; }

    /* verify: both items contribute to same bucket hash */
    uint8_t expected_hash[MT_HASH_SIZE];
    _expected_hash(&ctx, MS_NS_TEST, 1, pf1, expected_hash);
    const uint8_t* stored = _test_hash(&inst, MS_NS_TEST, 1, pf1);
    if (memcmp(expected_hash, stored, MT_HASH_SIZE) == 0) PASS(); else FAIL("hash mismatch");

    merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db);
}

static void test_tree_different_buckets(void) {
    TEST("items in different L1 buckets → independent hashes");
    sqlite3* db = NULL; sqlite3_open(":memory:", &db);
    _ensure_table(db);

    static struct UTUN_INSTANCE inst; memset(&inst, 0, sizeof(inst));
    inst.topo_sqlite_db = db;
    struct ms_data data; _data_init(&data);
    uint64_t kA = 0x0000000000000000ULL;  /* bucket 0 */
    uint64_t kB = 0x0800000000000000ULL;  /* bucket 1 */
    _data_insert(&data, kA, 100); _data_insert(&data, kB, 200);
    _data_sort(&data);
    struct ms_test_ctx ctx = { .data = &data, .inst = &inst };
    inst.ua = uasync_create();
    if (merkle_sync_init(&inst, 0x72, &g_test_ops, &ctx) != 0) abort();

    merkle_sync_recompute_path(&inst, MT_DATA_MEMBERS, "ns", kA);
    merkle_sync_recompute_path(&inst, MT_DATA_MEMBERS, "ns", kB);

    uint64_t pfA = merkle_sync_level_prefix(kA, 1);
    uint64_t pfB = merkle_sync_level_prefix(kB, 1);
    if (pfA == pfB) { FAIL("same L1 prefix"); merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db); return; }

    const uint8_t* hA = _test_hash(&inst, "ns", 1, pfA);
    uint8_t copyA[MT_HASH_SIZE]; memcpy(copyA, hA, MT_HASH_SIZE);
    const uint8_t* hB = _test_hash(&inst, "ns", 1, pfB);
    uint8_t copyB[MT_HASH_SIZE]; memcpy(copyB, hB, MT_HASH_SIZE);
    if (memcmp(copyA, copyB, MT_HASH_SIZE) != 0) PASS(); else FAIL("hashes should differ");

    merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db);
}

static void test_tree_delete_empty_bucket(void) {
    TEST("delete last item → bucket deleted from DB, get_hash returns zero");
    sqlite3* db = NULL; sqlite3_open(":memory:", &db);
    _ensure_table(db);

    static struct UTUN_INSTANCE inst; memset(&inst, 0, sizeof(inst));
    inst.topo_sqlite_db = db;
    struct ms_data data; _data_init(&data);
    uint64_t k = 0xCCCC000000000000ULL;
    _data_insert(&data, k, 7); _data_sort(&data);
    struct ms_test_ctx ctx = { .data = &data, .inst = &inst };
    inst.ua = uasync_create();
    if (merkle_sync_init(&inst, 0x72, &g_test_ops, &ctx) != 0) abort();

    merkle_sync_recompute_path(&inst, MT_DATA_MEMBERS, MS_NS_TEST, k);
    if (_db_count_rows(db, MS_NS_TEST) != 6) { FAIL("expected 6 rows"); merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db); return; }

    /* remove item and recompute */
    data.count = 0;
    merkle_sync_recompute_path(&inst, MT_DATA_MEMBERS, MS_NS_TEST, k);
    if (_db_count_rows(db, MS_NS_TEST) != 0) { FAIL("expected 0 rows got %d", _db_count_rows(db, MS_NS_TEST)); merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db); return; }

    uint64_t pf = merkle_sync_level_prefix(k, 3);
    const uint8_t* h = _test_hash(&inst, MS_NS_TEST, 3, pf);
    int zero = 1;
    for (int i = 0; i < MT_HASH_SIZE; i++) if (h[i] != 0) zero = 0;
    if (zero) PASS(); else FAIL("hash not zero after delete");

    merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db);
}

static void test_tree_consistency(void) {
    TEST("recompute vs stored hash consistency — random items");
    sqlite3* db = NULL; sqlite3_open(":memory:", &db);
    _ensure_table(db);

    static struct UTUN_INSTANCE inst; memset(&inst, 0, sizeof(inst));
    inst.topo_sqlite_db = db;
    struct ms_data data; _data_init(&data);

    /* random items across the keyspace */
    srand(12345);
    for (int i = 0; i < 50; i++) {
        uint64_t k = ((uint64_t)rand() << 32) | (uint64_t)rand();
        _data_insert(&data, k, (uint32_t)rand());
    }
    _data_sort(&data);

    struct ms_test_ctx ctx = { .data = &data, .inst = &inst };
    inst.ua = uasync_create();
    if (merkle_sync_init(&inst, 0x72, &g_test_ops, &ctx) != 0) abort();

    for (int i = 0; i < data.count; i++)
        merkle_sync_recompute_path(&inst, MT_DATA_MEMBERS, "ns", data.items[i].key);

    /* verify: recompute each bucket, compare with stored hash */
    sqlite3_stmt* st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT level, prefix64 FROM merkle_tree_hash WHERE namespace=? ORDER BY level, prefix64",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, "0:ns", -1, SQLITE_STATIC);
    int ok = 1;
    while (sqlite3_step(st) == SQLITE_ROW && ok) {
        uint8_t lv = (uint8_t)sqlite3_column_int(st, 0);
        uint64_t pf = (uint64_t)sqlite3_column_int64(st, 1);

        /* compute expected hash from data */
        uint8_t expected[MT_HASH_SIZE];
        _expected_hash(&ctx, "ns", lv, pf, expected);

        const uint8_t* stored = _test_hash(&inst, "ns", lv, pf);
        if (memcmp(expected, stored, MT_HASH_SIZE) != 0) {
            FAIL("mismatch L%d P%016llx", lv, (unsigned long long)pf);
            ok = 0;
        }
    }
    sqlite3_finalize(st);
    if (ok) PASS();

    merkle_sync_destroy(&inst); uasync_destroy(inst.ua, 0); sqlite3_close(db);
}

static int run_stage1(void) {
    printf("--- Stage 1: prefix arithmetic ---\n");
    test_prefix_bytes();
    test_level_prefix_basic();
    test_level_prefix_idempotent();
    test_level_prefix_bucket_partition();
    return tests_failed;
}

static int run_stage2(void) {
    printf("--- Stage 2: tree building with in-memory SQLite ---\n");
    stage_failures = 0;
    test_tree_empty_ns();
    test_tree_single_item();
    test_tree_multi_item_same_bucket();
    test_tree_different_buckets();
    test_tree_delete_empty_bucket();
    test_tree_consistency();
    return stage_failures;
}

/* ── Stage 3-6: integration with UTUN_INSTANCE + ETCP ── */

#define INTG_TIMEOUT_TB  120000
#define PHASE_TIMEOUT_TB 100000
#define PHASE3_TIMEOUT_TB 200000
#define POLL_MS          1
#define MS_SVC_ID        0x72

static char intg_temp_dir[] = "/tmp/utun_msync_XXXXXX";
static struct UTUN_INSTANCE* i_a = NULL;
static struct UTUN_INSTANCE* i_b = NULL;
static struct UTUN_INSTANCE* i_c = NULL;
static struct UASYNC* i_ua = NULL;
static int i_phase = 0;
static void* i_timeout_id = NULL;

static struct ms_data data_a, data_b, data_c;
static struct ms_test_ctx ctx_a, ctx_b, ctx_c;

static int done_sync = 0;
static int cancel_on_done;
static int _cond_done(void) { return done_sync; }

static void _on_sync_done(const uint8_t peer[32], const char* ns, int result, void* arg) {
    (void)arg; done_sync = 1;
    if (cancel_on_done) { cancel_on_done = 0; merkle_sync_cancel(i_a, MT_DATA_MEMBERS, peer, ns); }
    printf("  sync_done: peer=%s ns=%s result=%d\n", sc_public_key_hex(peer, (char[65]){0}), ns, result);
    if (result != MT_OK) { tests_failed++; stage_failures++; }
}

static int _write_cfg(const char* path, const char* fmt, ...) {
    va_list ap; FILE* f = fopen(path, "w");
    if (!f) return -1;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f); return 0;
}

static char* _get_pubkey(const char* path) {
    struct utun_config* cfg = parse_config(path);
    if (!cfg) return NULL;
    char* pub = u_strdup(cfg->global.my_public_key_hex);
    free_config(cfg); return pub;
}

static void _intg_timeout(void* arg) { (void)arg; printf("  GLOBAL TIMEOUT\n"); i_phase = 2; }

/* Оба конца должны принимать Merkle: стартовая задержка времени и согласование ETCP уже завершены. */
static int merkle_peer_ready(struct UTUN_INSTANCE* local, struct UTUN_INSTANCE* peer) {
    if (!local || !peer || local->ntp.startup_wait) return 0;
    struct ETCP_CONN* conn = instance_find_conn(local, peer->node_id);
    return conn && conn->initialized && conn->links_up && !conn->session_required && !conn->reinit_pending && !conn->close_requested;
}

static int _cond_links_up(void) {
    return merkle_peer_ready(i_a, i_b) && merkle_peer_ready(i_b, i_a) &&
        (!i_c || (merkle_peer_ready(i_a, i_c) && merkle_peer_ready(i_c, i_a)));
}

static int _wait_for(const char* desc, int (*cond)(void), int timeout_tb) {
    uint64_t start = get_time_tb();
    while (!cond() && (get_time_tb() - start) < (uint64_t)timeout_tb)
        test_clock_poll(i_ua, POLL_MS);
    if (cond()) return 1;
    printf("  TIMEOUT: %s\n", desc); i_phase = 2;
    return 0;
}

static void _intg_setup_contexts(void) {
    g_owned_ops = g_test_ops; g_owned_ops.keep_transport = 1; cancel_on_done = 0;
    memset(&data_a, 0, sizeof(data_a)); memset(&data_b, 0, sizeof(data_b)); memset(&data_c, 0, sizeof(data_c));
    ctx_a.data = &data_a; ctx_a.inst = NULL; ctx_a.tag = 'A';
    ctx_b.data = &data_b; ctx_b.inst = NULL; ctx_b.tag = 'B';
    ctx_c.data = &data_c; ctx_c.inst = NULL; ctx_c.tag = 'C';
    done_sync = 0;
}

static int _intg_create_cfgs(char* cfg_a, char* cfg_b, char* cfg_c, size_t sz) {
    /* reset template (mkdtemp modifies it) */
    strcpy(intg_temp_dir, "/tmp/utun_msync_XXXXXX");
    if (test_mkdtemp(intg_temp_dir) != 0) { printf("  mkdtemp failed\n"); return -1; }
    snprintf(cfg_a, sz, "%s/a.conf", intg_temp_dir);
    snprintf(cfg_b, sz, "%s/b.conf", intg_temp_dir);
    snprintf(cfg_c, sz, "%s/c.conf", intg_temp_dir);
    char dbp[256];
    snprintf(dbp, sizeof(dbp), "%s/db_a", intg_temp_dir); utun_mkdir(dbp, 0755);
    snprintf(dbp, sizeof(dbp), "%s/db_b", intg_temp_dir); utun_mkdir(dbp, 0755);
    snprintf(dbp, sizeof(dbp), "%s/db_c", intg_temp_dir); utun_mkdir(dbp, 0755);

    _write_cfg(cfg_a,
        "[global]\ntun_ip=10.220.0.1/24\n"
        "tun_ifname=tun220\nkeepalive_adaptive=0\ndb_path=%s/db_a\n\n"
        "[server: srv_a]\naddr=127.0.0.1:0\ntype=public\n\n"
        "[allowed_keys]\nallow_all=1\n", intg_temp_dir);
    config_ensure_keys(cfg_a);
    char* pub_a = _get_pubkey(cfg_a); if (!pub_a) return -1;

    _write_cfg(cfg_b,
        "[global]\ntun_ip=10.220.0.2/24\n"
        "tun_ifname=tun221\nkeepalive_adaptive=0\ndb_path=%s/db_b\n\n"
        "[server: srv_b]\naddr=127.0.0.1:0\ntype=public\n\n"
        "[client: to_a]\nkeepalive=1\npeer_public_key=%s\nlink=srv_b:127.0.0.1:0\n\n"
        "[allowed_keys]\nallow_all=1\n", intg_temp_dir, pub_a);
    config_ensure_keys(cfg_b);

    _write_cfg(cfg_c,
        "[global]\ntun_ip=10.220.0.3/24\n"
        "tun_ifname=tun222\nkeepalive_adaptive=0\ndb_path=%s/db_c\n\n"
        "[server: srv_c]\naddr=127.0.0.1:0\ntype=public\n\n"
        "[client: to_a]\nkeepalive=1\npeer_public_key=%s\nlink=srv_c:127.0.0.1:0\n\n"
        "[allowed_keys]\nallow_all=1\n", intg_temp_dir, pub_a);
    config_ensure_keys(cfg_c);

    u_free(pub_a);
    return 0;
}

static void _intg_cleanup(void) {
    if (i_timeout_id) { uasync_cancel_timeout(i_ua, i_timeout_id); i_timeout_id = NULL; }
    if (i_c) { merkle_sync_destroy(i_c); i_c->running = 0; utun_instance_destroy(i_c); i_c = NULL; }
    if (i_b) { merkle_sync_destroy(i_b); i_b->running = 0; utun_instance_destroy(i_b); i_b = NULL; }
    if (i_a) { merkle_sync_destroy(i_a); i_a->running = 0; utun_instance_destroy(i_a); i_a = NULL; }
    if (i_ua) { uasync_destroy(i_ua, 0); i_ua = NULL; }
    char pa[320];
    snprintf(pa, sizeof(pa), "%s/a.conf", intg_temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/b.conf", intg_temp_dir); unlink(pa);
    snprintf(pa, sizeof(pa), "%s/c.conf", intg_temp_dir); unlink(pa);
    for (const char* d = "abc"; *d; d++) {
        snprintf(pa, sizeof(pa), "%s/db_%c/chats.db", intg_temp_dir, *d); unlink(pa);
        snprintf(pa, sizeof(pa), "%s/db_%c/chats.db-wal", intg_temp_dir, *d); unlink(pa);
        snprintf(pa, sizeof(pa), "%s/db_%c/chats.db-shm", intg_temp_dir, *d); unlink(pa);
        snprintf(pa, sizeof(pa), "%s/db_%c", intg_temp_dir, *d); test_rmdir(pa);
    }
    test_rmdir(intg_temp_dir);
}

static int _intg_init_two(void) {
    i_phase = 0; i_timeout_id = NULL; i_a = i_b = i_c = NULL;
    char cfg_a[256], cfg_b[256], cfg_c[256];
    if (_intg_create_cfgs(cfg_a, cfg_b, cfg_c, sizeof(cfg_a)) != 0) return -1;
    utun_instance_set_tun_init_enabled(0);
    i_ua = uasync_create(); if (!i_ua) { _intg_cleanup(); return -1; }
    i_a = utun_instance_create(i_ua, cfg_a);
    i_b = utun_instance_create(i_ua, cfg_b);
    if (i_a && i_b) test_etcp_set_peer(i_b, "to_a", i_a, "srv_a");
    if (!i_a || !i_b || utun_instance_init(i_a) != 0 || utun_instance_init(i_b) != 0)
        { _intg_cleanup(); return -1; }
    i_timeout_id = uasync_set_timeout(i_ua, INTG_TIMEOUT_TB, NULL, _intg_timeout, "ms_intg_timeout");
    if (!_wait_for("ETCP and time ready", _cond_links_up, PHASE_TIMEOUT_TB)) { _intg_cleanup(); return -1; }

    _intg_setup_contexts();
    ctx_a.inst = i_a; ctx_b.inst = i_b;
    _ms_ensure_groups(i_a); _ms_ensure_groups(i_b);
    if (merkle_sync_init(i_a, MS_SVC_ID, &g_owned_ops, &ctx_a) != 0 ||
        merkle_sync_init(i_b, MS_SVC_ID, &g_owned_ops, &ctx_b) != 0)
        { _intg_cleanup(); return -1; }
    printf("  2 instances ready A=%016llx B=%016llx\n",
           (unsigned long long)i_a->node_id, (unsigned long long)i_b->node_id);
    return 0;
}

static void _intg_ins_many(struct ms_data* d, struct UTUN_INSTANCE* inst, int base, int count) {
    for (int i = 0; i < count; i++) {
        uint64_t key = (uint64_t)(base + i) * 0x100000000000000ULL;
        _data_insert(d, key, (uint32_t)(base + i));
    }
    _data_sort(d);
    for (int i = 0; i < d->count; i++)
        merkle_sync_recompute_path(inst, MT_DATA_MEMBERS, MS_NS_TEST, d->items[i].key);
}

static int _intg_compare_data(struct ms_data* a, struct ms_data* b) {
    if (a->count != b->count) { printf("  count mismatch: %d vs %d\n", a->count, b->count); return -1; }
    for (int i = 0; i < a->count; i++) {
        if (a->items[i].key != b->items[i].key || a->items[i].val != b->items[i].val) {
            printf("  data[%d] mismatch\n", i); return -1;
        }
    }
    return 0;
}

static int _intg_verify_all(struct UTUN_INSTANCE* a, struct UTUN_INSTANCE* b,
                              struct ms_data* da, struct ms_data* db, const char* ns) {
    if (_db_compare_trees(a->topo_sqlite_db, b->topo_sqlite_db, ns) != 0) { printf("  TREE MISMATCH\n"); return -1; }
    if (_intg_compare_data(da, db) != 0) { printf("  DATA MISMATCH\n"); return -1; }
    return 0;
}

/* ── Stage 3: integration tests ── */

static void test_peer_empty(void) {
    TEST("peer empty — B has 5 items, A empty, sync A->B");
    if (_intg_init_two() != 0) { FAIL("setup failed"); return; }
    _intg_ins_many(&data_b, i_b, 0, 5);
    done_sync = 0;
    merkle_sync_start(i_a, MT_DATA_MEMBERS, instance_find_conn_by_key(i_a, i_b->my_keys.public_key), MS_NS_TEST, _on_sync_done, NULL);
    if (!_wait_for("sync done", _cond_done, PHASE_TIMEOUT_TB))
        { FAIL("sync timeout"); _intg_cleanup(); return; }
    printf("  A data=%d B data=%d, A tree=%d B tree=%d\n",
           data_a.count, data_b.count,
           _db_count_rows(i_a->topo_sqlite_db, MS_NS_TEST),
           _db_count_rows(i_b->topo_sqlite_db, MS_NS_TEST));
    if (_intg_verify_all(i_a, i_b, &data_a, &data_b, MS_NS_TEST) != 0)
        { FAIL("verify failed"); _intg_cleanup(); return; }
    PASS();
    _intg_cleanup();
}

static void test_hash_match(void) {
    TEST("hash match — identical data, sync immediate");
    size_t allocations = u_get_allocated_count();
    if (_intg_init_two() != 0) { FAIL("setup failed"); return; }
    _intg_ins_many(&data_a, i_a, 0, 5);
    _intg_ins_many(&data_b, i_b, 0, 5);
    done_sync = 0;
    cancel_on_done = 1; /* Успешный callback удаляет свою сессию до release локального NCD handle. */
    struct ETCP_CONN* conn = instance_find_conn(i_a, i_b->node_id);
    /* Первый hold привязывает также ожидающие SQL handles. Фиксируем это до снимка владельцев Merkle. */
    struct NODE_CONN_DIRECT* anchor = NULL;
    if (node_conn_direct_hold(conn, &anchor) < 0) { FAIL("fixture transport hold failed"); _intg_cleanup(); return; }
    int owners = node_conn_direct_handle_count(conn);
    int started = merkle_sync_start(i_a, MT_DATA_MEMBERS, instance_find_conn_by_key(i_a, i_b->my_keys.public_key), MS_NS_TEST, _on_sync_done, NULL);
    int retained = node_conn_direct_handle_count(conn);
    node_conn_direct_close(anchor);
    printf("  hash-match ownership: start=%d owners=%d->%d after_fixture_release=%d\n",
        started, owners, retained, node_conn_direct_handle_count(conn));
    if (started || retained != owners + 1 || node_conn_direct_handle_count(conn) != owners)
        { FAIL("round has no transport owner"); _intg_cleanup(); return; }
    if (!_wait_for("identical hashes", _cond_done, 2000)) { FAIL("sync did not complete quickly"); _intg_cleanup(); return; }
    if (!done_sync) { FAIL("sync did not complete quickly"); _intg_cleanup(); return; }
    if (_intg_verify_all(i_a, i_b, &data_a, &data_b, MS_NS_TEST) != 0)
        { FAIL("verify failed"); _intg_cleanup(); return; }
    _intg_cleanup();
    if (cancel_on_done || u_get_allocated_count() != allocations) { FAIL("completion/cancel leaked transport ownership"); return; }
    PASS();
}

static void test_divergence_merge(void) {
    TEST("divergence merge — A has 3 items, B has 2 different");
    if (_intg_init_two() != 0) { FAIL("setup failed"); return; }
    _intg_ins_many(&data_a, i_a, 0, 3);
    _intg_ins_many(&data_b, i_b, 10, 2);
    done_sync = 0;
    merkle_sync_start(i_a, MT_DATA_MEMBERS, instance_find_conn_by_key(i_a, i_b->my_keys.public_key), MS_NS_TEST, _on_sync_done, NULL);
    if (!_wait_for("sync done", _cond_done, PHASE_TIMEOUT_TB))
        { FAIL("sync timeout"); _intg_cleanup(); return; }
    if (_intg_verify_all(i_a, i_b, &data_a, &data_b, MS_NS_TEST) != 0)
        { FAIL("verify failed"); _intg_cleanup(); return; }
    PASS();
    _intg_cleanup();
}

static int run_stage3(void) {
    printf("--- Stage 3: integration 2 instances, basic scenarios ---\n");
    stage_failures = 0;
    test_peer_empty();
    test_hash_match();
    test_divergence_merge();
    return stage_failures;
}

/* ── Stage 4: randomized 2-instance sync ── */

static void test_randomized_two(void) {
    TEST("randomized 2-instance — 25 iterations");
    srand(42);
    int iter;
    for (iter = 0; iter < 25 && stage_failures == 0; iter++) {
        if (_intg_init_two() != 0) { FAIL("setup iter %d", iter); break; }
        int na = rand() % 101, nb = rand() % 101;
        for (int i = 0; i < na; i++) {
            uint64_t k = ((uint64_t)rand() << 32) | (uint64_t)rand();
            _data_insert(&data_a, k, (uint32_t)rand());
        }
        for (int i = 0; i < nb; i++) {
            uint64_t k = ((uint64_t)rand() << 32) | (uint64_t)rand();
            _data_insert(&data_b, k, (uint32_t)rand());
        }
        _data_sort(&data_a); _data_sort(&data_b);
        for (int i = 0; i < data_a.count; i++) merkle_sync_recompute_path(i_a, MT_DATA_MEMBERS, MS_NS_RND, data_a.items[i].key);
        for (int i = 0; i < data_b.count; i++) merkle_sync_recompute_path(i_b, MT_DATA_MEMBERS, MS_NS_RND, data_b.items[i].key);

        done_sync = 0;
        merkle_sync_start(i_a, MT_DATA_MEMBERS, instance_find_conn_by_key(i_a, i_b->my_keys.public_key), MS_NS_RND, _on_sync_done, NULL);
        if (!_wait_for("sync", _cond_done, PHASE_TIMEOUT_TB)) { FAIL("timeout iter %d", iter); _intg_cleanup(); break; }
        if (_intg_verify_all(i_a, i_b, &data_a, &data_b, MS_NS_RND) != 0) {
            printf("  A data=%d B data=%d A tree=%d B tree=%d\n",
                   data_a.count, data_b.count,
                   _db_count_rows(i_a->topo_sqlite_db, MS_NS_RND),
                   _db_count_rows(i_b->topo_sqlite_db, MS_NS_RND));
            /* dump items only in A */
            for (int ai = 0; ai < data_a.count; ai++) {
                int found = 0;
                for (int bi = 0; bi < data_b.count; bi++)
                    if (data_a.items[ai].key == data_b.items[bi].key) { found = 1; break; }
                if (!found) printf("  only A: key=%016llx val=%u\n", (unsigned long long)data_a.items[ai].key, data_a.items[ai].val);
            }
            for (int bi = 0; bi < data_b.count; bi++) {
                int found = 0;
                for (int ai = 0; ai < data_a.count; ai++)
                    if (data_b.items[bi].key == data_a.items[ai].key) { found = 1; break; }
                if (!found) printf("  only B: key=%016llx val=%u\n", (unsigned long long)data_b.items[bi].key, data_b.items[bi].val);
            }
            FAIL("verify iter %d", iter); _intg_cleanup(); break;
        }

        /* consistency check: recompute each bucket hash, verify stored matches */
        int ok = 1;
        sqlite3_stmt* st = NULL;
        sqlite3_prepare_v2(i_a->topo_sqlite_db,
            "SELECT level,prefix64 FROM merkle_tree_hash WHERE namespace=? ORDER BY level,prefix64",
            -1, &st, NULL);
        sqlite3_bind_text(st, 1, MS_NS_RND, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW && ok) {
            uint8_t lv = (uint8_t)sqlite3_column_int(st, 0);
            uint64_t pf = (uint64_t)sqlite3_column_int64(st, 1);
            uint8_t expected[MT_HASH_SIZE];
            _expected_hash(&ctx_a, MS_NS_RND, lv, pf, expected);
            const uint8_t* stored = _test_hash(i_a, MS_NS_RND, lv, pf);
            uint8_t scopy[MT_HASH_SIZE]; memcpy(scopy, stored, MT_HASH_SIZE);
            if (memcmp(expected, scopy, MT_HASH_SIZE) != 0) { ok = 0; }
        }
        sqlite3_finalize(st);
        if (!ok) { FAIL("tree consistency iter %d", iter); _intg_cleanup(); break; }
        _intg_cleanup();
    }
    if (iter == 25) PASS();
}

/* ── Stage 5: randomized 3-instance star-topology sync ── */

static int _intg_init_three(void) {
    i_phase = 0; i_timeout_id = NULL; i_a = i_b = i_c = NULL;
    char cfg_a[256], cfg_b[256], cfg_c[256];
    if (_intg_create_cfgs(cfg_a, cfg_b, cfg_c, sizeof(cfg_a)) != 0) return -1;
    utun_instance_set_tun_init_enabled(0);
    i_ua = uasync_create(); if (!i_ua) { _intg_cleanup(); return -1; }
    i_a = utun_instance_create(i_ua, cfg_a);
    i_b = utun_instance_create(i_ua, cfg_b);
    i_c = utun_instance_create(i_ua, cfg_c);
    if (i_a && i_b && i_c) {
        test_etcp_set_peer(i_b, "to_a", i_a, "srv_a");
        test_etcp_set_peer(i_c, "to_a", i_a, "srv_a");
    }
    if (!i_a || !i_b || !i_c || utun_instance_init(i_a) != 0 || utun_instance_init(i_b) != 0 || utun_instance_init(i_c) != 0)
        { _intg_cleanup(); return -1; }
    i_timeout_id = uasync_set_timeout(i_ua, INTG_TIMEOUT_TB*5, NULL, _intg_timeout, "ms_intg_timeout");
    if (!_wait_for("ETCP and time ready", _cond_links_up, PHASE3_TIMEOUT_TB)) { _intg_cleanup(); return -1; }
    /* let connections settle */
    test_clock_run_for(i_ua, 200);
    _intg_setup_contexts();
    ctx_a.inst = i_a; ctx_b.inst = i_b; ctx_c.inst = i_c;
    _ms_ensure_groups(i_a); _ms_ensure_groups(i_b); _ms_ensure_groups(i_c);
    if (merkle_sync_init(i_a, MS_SVC_ID, &g_owned_ops, &ctx_a) != 0 ||
        merkle_sync_init(i_b, MS_SVC_ID, &g_owned_ops, &ctx_b) != 0 ||
        merkle_sync_init(i_c, MS_SVC_ID, &g_owned_ops, &ctx_c) != 0)
        { _intg_cleanup(); return -1; }
    printf("  3 instances ready A=%016llx B=%016llx C=%016llx\n",
           (unsigned long long)i_a->node_id, (unsigned long long)i_b->node_id, (unsigned long long)i_c->node_id);
    return 0;
}

static int _cond_all_synced(void) {
    if (data_a.count != data_b.count || data_b.count != data_c.count) return 0;
    if (_db_compare_trees(i_a->topo_sqlite_db, i_b->topo_sqlite_db, MS_NS_RND) != 0) return 0;
    if (_db_compare_trees(i_b->topo_sqlite_db, i_c->topo_sqlite_db, MS_NS_RND) != 0) return 0;
    return 1;
}

static void test_randomized_three(void) {
    TEST("randomized 3-instance star — 5 iterations");
    srand(1234);
    int iter;
    for (iter = 0; iter < 5 && stage_failures == 0; iter++) {
        if (_intg_init_three() != 0) { FAIL("setup iter %d", iter); break; }
        int na = rand() % 51, nb = rand() % 51, nc = rand() % 51;
        for (int i = 0; i < na; i++) { uint64_t k = ((uint64_t)rand()<<32)|(uint64_t)rand(); _data_insert(&data_a, k, (uint32_t)rand()); }
        for (int i = 0; i < nb; i++) { uint64_t k = ((uint64_t)rand()<<32)|(uint64_t)rand(); _data_insert(&data_b, k, (uint32_t)rand()); }
        for (int i = 0; i < nc; i++) { uint64_t k = ((uint64_t)rand()<<32)|(uint64_t)rand(); _data_insert(&data_c, k, (uint32_t)rand()); }
        _data_sort(&data_a); for (int i = 0; i < data_a.count; i++) merkle_sync_recompute_path(i_a, MT_DATA_MEMBERS, MS_NS_RND, data_a.items[i].key);
        _data_sort(&data_b); for (int i = 0; i < data_b.count; i++) merkle_sync_recompute_path(i_b, MT_DATA_MEMBERS, MS_NS_RND, data_b.items[i].key);
        _data_sort(&data_c); for (int i = 0; i < data_c.count; i++) merkle_sync_recompute_path(i_c, MT_DATA_MEMBERS, MS_NS_RND, data_c.items[i].key);

        done_sync = 0; i_phase = 0;
        /* B→A: B syncs with A, gets A's data, B: SYNCED */
        merkle_sync_start(i_b, MT_DATA_MEMBERS, instance_find_conn_by_key(i_b, i_a->my_keys.public_key), MS_NS_RND, _on_sync_done, NULL);
        if (!_wait_for("sync B", _cond_done, PHASE_TIMEOUT_TB)) { FAIL("timeout B iter %d", iter); _intg_cleanup(); break; }
        done_sync = 0; i_phase = 0;
        /* C→A: C syncs with A, gets A∪B, A broadcasts C's new items to B (relay) */
        merkle_sync_start(i_c, MT_DATA_MEMBERS, instance_find_conn_by_key(i_c, i_a->my_keys.public_key), MS_NS_RND, _on_sync_done, NULL);
        if (!_wait_for("sync C", _cond_done, PHASE_TIMEOUT_TB)) { FAIL("timeout C iter %d", iter); _intg_cleanup(); break; }
        /* Автоматический следующий раунд A↔B завершается по условию, не по числу poll(). */
        if (!_wait_for("automatic convergence of all peers", _cond_all_synced, PHASE3_TIMEOUT_TB)) {
            FAIL("automatic sync timeout iter %d", iter); _intg_cleanup(); break;
        }
        if (_intg_compare_data(&data_a, &data_b) != 0 || _intg_compare_data(&data_b, &data_c) != 0)
            { FAIL("data mismatch iter %d", iter); _intg_cleanup(); break; }
        if (_db_compare_trees(i_a->topo_sqlite_db, i_b->topo_sqlite_db, MS_NS_RND) != 0 ||
            _db_compare_trees(i_b->topo_sqlite_db, i_c->topo_sqlite_db, MS_NS_RND) != 0)
            { FAIL("tree mismatch iter %d", iter); _intg_cleanup(); break; }
        _intg_cleanup();
    }
    if (iter == 5) PASS();
}

/* ── Stage 6: protocol edge cases ── */

static void test_cancel(void) {
    TEST("cancel — start sync, immediately cancel, done_cb NOT called");
    if (_intg_init_two() != 0) { FAIL("setup failed"); return; }
    _intg_ins_many(&data_b, i_b, 0, 5);
    done_sync = 0;
    merkle_sync_start(i_a, MT_DATA_MEMBERS, instance_find_conn_by_key(i_a, i_b->my_keys.public_key), MS_NS_TEST, _on_sync_done, NULL);
    merkle_sync_cancel(i_a, MT_DATA_MEMBERS, i_b->my_ed25519_pubkey, MS_NS_TEST);
    test_clock_run_for(i_ua, 1000);
    if (!done_sync) PASS(); else FAIL("done_cb was called");
    _intg_cleanup();
}

static int sw_cb1_called = 0, sw_cb2_called = 0;
static void _sw_done1(const uint8_t* p, const char* n, int r, void* a) { (void)p;(void)n;(void)r;(void)a; sw_cb1_called = 1; }
static void _sw_done2(const uint8_t* p, const char* n, int r, void* a) { (void)p;(void)n;(void)r;(void)a; sw_cb2_called = 1; }

static void test_start_overwrite(void) {
    TEST("concurrent starts — both callbacks complete");
    if (_intg_init_two() != 0) { FAIL("setup failed"); return; }
    _intg_ins_many(&data_b, i_b, 0, 3);
    sw_cb1_called = sw_cb2_called = 0;
    merkle_sync_start(i_a, MT_DATA_MEMBERS, instance_find_conn_by_key(i_a, i_b->my_keys.public_key), MS_NS_TEST, _sw_done1, NULL);
    merkle_sync_start(i_a, MT_DATA_MEMBERS, instance_find_conn_by_key(i_a, i_b->my_keys.public_key), MS_NS_TEST, _sw_done2, NULL);
    uint64_t deadline = get_time_tb() + 5000;
    while (!sw_cb2_called && i_phase == 0 && get_time_tb() < deadline) test_clock_poll(i_ua, 10);
    if (sw_cb1_called && sw_cb2_called) PASS(); else FAIL("cb1=%d cb2=%d", sw_cb1_called, sw_cb2_called);
    _intg_cleanup();
}

static int run_stage4(void) {
    printf("--- Stage 4: randomized 2-instance sync (25 iters) ---\n");
    stage_failures = 0;
    test_randomized_two();
    return stage_failures;
}

static int run_stage5(void) {
    printf("--- Stage 5: randomized 3-instance star sync (5 iters) ---\n");
    stage_failures = 0;
    test_randomized_three();
    return stage_failures;
}

/* ── Stage 7: stress test — 10 spammers + 2 A↔B observers, star topology ── */

#define STRESS_N           13   /* 1 hub + 10 spammers + 2 observers */
#define STRESS_HUB         0
#define STRESS_SPAM_BEGIN  1
#define STRESS_SPAM_END    10  /* indices 1..10 */
#define STRESS_OBS_A       11
#define STRESS_OBS_B       12
#define STRESS_SPAM_MS     5000
#define STRESS_SYNC_TB     200000  /* 20s timeout for final sync */
#define STRESS_LINK_TB     300000  /* 30s for all links up */
#define STRESS_MAX_ROUNDS  5
#define STRESS_DRAIN_TB    2000    /* 200ms drain after spam stop */
#define STRESS_ROUND_GAP_TB 500    /* 50ms gap between final rounds */
#define STRESS_SAFETY_TB   400000  /* 40s safety for spam+converge */

struct str_state {
    struct UASYNC*          ua;
    struct UTUN_INSTANCE*   inst[STRESS_N];
    struct ms_data          data[STRESS_N];
    struct ms_test_ctx      ctx[STRESS_N];
    uint32_t               versions[STRESS_N]; /* Последняя версия каждого владельца записи. */
    unsigned int           observer_starts[2];
    uint64_t               spam_started_us;
    void*                   global_timeout;   /* setup: links-up timeout */
    void*                   safety_timer;     /* spam+converge safety */
    void*                   stop_timer;       /* spam stop */
    void*                   drain_timer;      /* drain after spam stop */
    void*                   round_timer;      /* gap between final rounds */
    int                     phase;            /* 0=running, 1=pass, 2=fail */
    int                     spam_active;
    int                     sync_pending;     /* initial sync only (balanced) */
    int                     final_pending;    /* outstanding final-round syncs */
    int                     final_round;      /* final round number */
};

static struct str_state* gs = NULL;

static void _str_cleanup(void);
static int  _str_cond_sync_done(void);
static int  _str_cond_links_up(void);
static void _str_stop_spam(void* arg);
static void _str_drain_cb(void* arg);
static void _str_round_cb(void* arg);
static void _str_start_final_round(void);
static void _str_final_done(const uint8_t peer[32], const char* ns, int result, void* arg);
static void _str_check_roots(void);
static void _str_verify(void);
static void _str_fail_timeout(void* arg);

static void _str_sync_done(const uint8_t peer[32], const char* ns, int result, void* arg) {
    struct str_state* s = (struct str_state*)arg;
    if (!s || s->phase != 0) return;
    if (result != MT_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "stress initial sync: peer=%s ns=%s result=%d",
                    sc_public_key_hex(peer, (char[65]){0}), ns, result);
        s->phase = 2;
    }
    __sync_fetch_and_sub(&s->sync_pending, 1);
}

static void _str_spam_cb(void* arg);

static void _str_spam_schedule(struct str_state* s, int idx) {
    if (!s->spam_active) return;
    int delay_tb = ((rand() % 30) + 1) * 10;  /* 1-30ms → 10-300 timebase units */
    uasync_set_timeout(s->ua, (uint32_t)delay_tb, (void*)(intptr_t)idx, _str_spam_cb, "spam");
}

static void _str_spam_cb(void* arg) {
    if (!gs || !gs->spam_active) return;
    int idx = (int)(intptr_t)arg;
    if (idx < STRESS_SPAM_BEGIN || idx > STRESS_SPAM_END) { _str_spam_schedule(gs, idx); return; }

    /* spammer: bump own member, sync to hub */
    uint64_t key = ((uint64_t)idx) << 60;
    _data_insert(&gs->data[idx], key, ++gs->versions[idx]);
    merkle_sync_recompute_path(gs->inst[idx], MT_DATA_MEMBERS, MS_NS_STRESS, key);
    merkle_sync_start(gs->inst[idx], MT_DATA_MEMBERS, instance_find_conn_by_key(gs->inst[idx], gs->inst[STRESS_HUB]->my_keys.public_key), MS_NS_STRESS, NULL, NULL);

    _str_spam_schedule(gs, idx);
}

static void _str_obs_spam_cb(void* arg) {
    if (!gs || !gs->spam_active) return;
    int idx = (int)(intptr_t)arg;

    gs->observer_starts[idx - STRESS_OBS_A]++;
    merkle_sync_start(gs->inst[idx], MT_DATA_MEMBERS, instance_find_conn_by_key(gs->inst[idx], gs->inst[STRESS_HUB]->my_keys.public_key), MS_NS_STRESS, NULL, NULL);

    int delay_tb = ((rand() % 10) + 1) * 10;  /* 1-10ms */
    uasync_set_timeout(gs->ua, (uint32_t)delay_tb, arg, _str_obs_spam_cb, "obs_spam");
}

static void _str_global_timeout(void* arg) {
    (void)arg;
    if (gs) { gs->global_timeout = NULL; gs->phase = 2; }
}

static int _str_cond_links_up(void) {
    if (!gs) return 0;
    for (int i = 1; i < STRESS_N; i++) {
        if (!merkle_peer_ready(gs->inst[STRESS_HUB], gs->inst[i]) || !merkle_peer_ready(gs->inst[i], gs->inst[STRESS_HUB])) return 0;
    }
    return 1;
}

static int _str_wait_for(const char* desc, int (*cond)(void), int timeout_tb) {
    struct str_state* s = gs;
    uint64_t start = get_time_tb();
    while (!cond() && (get_time_tb() - start) < (uint64_t)timeout_tb && s->phase == 0)
        test_clock_poll(s->ua, 1);
    if (s->phase == 0 && cond()) return 1;
    printf("  STRESS TIMEOUT: %s\n", desc); s->phase = 2;
    return 0;
}

/* Корень всех ветвей имеет level=0. У нагрузки ключи лежат за пределами ветви (1,0). */
static int stress_roots_equal(struct str_state* s) {
    uint8_t hub[MT_HASH_SIZE];
    memcpy(hub, _test_hash(s->inst[STRESS_HUB], MS_NS_STRESS, 0, 0), sizeof(hub));
    for (int i = 1; i < STRESS_N; i++) {
        const uint8_t* peer = _test_hash(s->inst[i], MS_NS_STRESS, 0, 0);
        if (!memcmp(hub, peer, sizeof(hub))) continue;
        printf("  roots differ round=%d node=%d hub=%02x%02x%02x%02x peer=%02x%02x%02x%02x\n",
            s->final_round, i, hub[0], hub[1], hub[2], hub[3], peer[0], peer[1], peer[2], peer[3]);
        return 0;
    }
    return 1;
}

static int _str_init(void) {
    struct str_state* s = u_calloc(1, sizeof(*s));
    if (!s) return -1;
    gs = s;
    s->spam_active = 0;
    s->phase = 0;
    s->sync_pending = 0;

    /* --- temp dir --- */
    char tmpld[128] = "/tmp/utun_str_XXXXXX";
    if (test_mkdtemp(tmpld) != 0) { printf("  mkdtemp failed\n"); u_free(s); gs = NULL; return -1; }

    /* --- create configs --- */
    char cfgs[STRESS_N][256];

    utun_instance_set_tun_init_enabled(0);
    s->ua = uasync_create();
    if (!s->ua) goto fail;

    /* hub config */
    snprintf(cfgs[0], sizeof(cfgs[0]), "%s/cfg_0.conf", tmpld);
    {
        char dbp[256]; snprintf(dbp, sizeof(dbp), "%s/db_0", tmpld); utun_mkdir(dbp, 0755);
        _write_cfg(cfgs[0],
            "[global]\ntun_ip=10.240.0.1/24\ntun_ifname=tun240\nkeepalive_adaptive=0\ndb_path=%s\n\n"
            "[server:s0]\naddr=127.0.0.1:0\ntype=public\n\n"
            "[allowed_keys]\nallow_all=1\n", dbp);
        config_ensure_keys(cfgs[0]);
    }

    char* hub_pub = _get_pubkey(cfgs[0]); if (!hub_pub) goto fail;
    /* spoke configs */
    for (int i = 1; i < STRESS_N; i++) {
        snprintf(cfgs[i], sizeof(cfgs[i]), "%s/cfg_%d.conf", tmpld, i);
        char dbp[256]; snprintf(dbp, sizeof(dbp), "%s/db_%d", tmpld, i); utun_mkdir(dbp, 0755);
        _write_cfg(cfgs[i],
            "[global]\ntun_ip=10.240.0.%d/24\ntun_ifname=tun240\nkeepalive_adaptive=0\ndb_path=%s\n\n"
            "[server:s%d]\naddr=127.0.0.1:0\ntype=public\n\n"
            "[client:to_hub]\nkeepalive=1\npeer_public_key=%s\nlink=s%d:127.0.0.1:0\n\n"
            "[allowed_keys]\nallow_all=1\n",
            i + 1, dbp, i, hub_pub, i);
        config_ensure_keys(cfgs[i]);
    }
    u_free(hub_pub);

    /* --- create instances --- */
    for (int i = 0; i < STRESS_N; i++) {
        s->inst[i] = utun_instance_create(s->ua, cfgs[i]);
        if (s->inst[i] && i) test_etcp_set_peer(s->inst[i], "to_hub", s->inst[STRESS_HUB], "s0");
        if (!s->inst[i] || utun_instance_init(s->inst[i]) != 0) {
            printf("  FAIL: instance %d init\n", i);
            goto fail;
        }
    }
    printf("  %d instances created, waiting for links...\n", STRESS_N);

    s->global_timeout = uasync_set_timeout(s->ua, STRESS_LINK_TB, NULL, _str_global_timeout, "str_timeout");
    if (!_str_wait_for("ETCP and time ready", _str_cond_links_up, STRESS_LINK_TB)) goto fail;
    printf("  all peers ready for Merkle\n");
    uasync_cancel_timeout(s->ua, s->global_timeout); s->global_timeout = NULL;

    /* --- init merkle_sync on all --- */
    for (int i = 0; i < STRESS_N; i++) {
        memset(&s->data[i], 0, sizeof(s->data[i]));
        s->ctx[i].data = &s->data[i];
        s->ctx[i].inst = s->inst[i];
        s->ctx[i].tag = (char)('A' + i);
        _ms_ensure_groups(s->inst[i]);
        if (merkle_sync_init(s->inst[i], MS_SVC_ID, &g_test_ops, &s->ctx[i]) != 0) {
            printf("  FAIL: merkle_sync_init %d\n", i); goto fail;
        }
    }
    printf("  merkle_sync inited on all\n");

    /* --- seed each instance with its own member; sync to hub --- */
    for (int i = 1; i < STRESS_N; i++) {
        uint64_t key = ((uint64_t)i) << 60;
        s->versions[i] = (uint32_t)i;
        _data_insert(&s->data[i], key, s->versions[i]);
        merkle_sync_recompute_path(s->inst[i], MT_DATA_MEMBERS, MS_NS_STRESS, key);
    }
    if (stress_roots_equal(s)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "stress seed: different member sets have equal roots before synchronization");
        goto fail;
    }
    /* initial sync: each spoke → hub */
    s->sync_pending = STRESS_N - 1;
    for (int i = 1; i < STRESS_N; i++)
        merkle_sync_start(s->inst[i], MT_DATA_MEMBERS, instance_find_conn_by_key(s->inst[i], s->inst[STRESS_HUB]->my_keys.public_key), MS_NS_STRESS, _str_sync_done, s);
    if (!_str_wait_for("initial sync", _str_cond_sync_done, STRESS_SYNC_TB)) goto fail;

    /* observers: sync with hub too */

    return 0;

fail:
    _str_cleanup();
    return -1;
}

static int _str_cond_sync_done(void) {
    return gs ? __sync_fetch_and_add(&gs->sync_pending, 0) <= 0 : 0;
}

static void _str_cleanup(void) {
    struct str_state* s = gs;
    if (!s) return;
    s->spam_active = 0;
    if (s->global_timeout && s->ua) { uasync_cancel_timeout(s->ua, s->global_timeout); s->global_timeout = NULL; }
    if (s->safety_timer && s->ua)  { uasync_cancel_timeout(s->ua, s->safety_timer);  s->safety_timer = NULL; }
    if (s->stop_timer && s->ua)    { uasync_cancel_timeout(s->ua, s->stop_timer);    s->stop_timer = NULL; }
    if (s->drain_timer && s->ua)   { uasync_cancel_timeout(s->ua, s->drain_timer);   s->drain_timer = NULL; }
    if (s->round_timer && s->ua)   { uasync_cancel_timeout(s->ua, s->round_timer);   s->round_timer = NULL; }
    for (int i = STRESS_N - 1; i >= 0; i--) {
        if (!s->inst[i]) continue;
        merkle_sync_destroy(s->inst[i]);
        s->inst[i]->running = 0;
        utun_instance_destroy(s->inst[i]);
        s->inst[i] = NULL;
    }
    if (s->ua) { uasync_destroy(s->ua, 0); s->ua = NULL; }
    u_free(s);
    gs = NULL;
}

static void _str_stop_spam(void* arg) {
    (void)arg;
    if (!gs) return;
    gs->stop_timer = NULL;
    gs->spam_active = 0;
    printf("  spam stopped after %lluus; writer updates:", (unsigned long long)(get_time_us() - gs->spam_started_us));
    for (int i = STRESS_SPAM_BEGIN; i <= STRESS_SPAM_END; i++) printf(" %u", gs->versions[i] - (uint32_t)i);
    printf("; observer starts: %u %u\n", gs->observer_starts[0], gs->observer_starts[1]);
    /* короткий дренаж — даём in-flight спам-синкам утихнуть перед финальными раундами */
    gs->drain_timer = uasync_set_timeout(gs->ua, STRESS_DRAIN_TB, NULL, _str_drain_cb, "str_drain");
}

static void _str_drain_cb(void* arg) {
    (void)arg;
    if (!gs) return;
    gs->drain_timer = NULL;
    _str_start_final_round();
}

static void _str_start_final_round(void) {
    struct str_state* s = gs;
    if (!s) return;
    s->final_round++;
    s->final_pending = STRESS_N - 1;
    printf("  final round %d: syncing %d spokes -> hub\n", s->final_round, s->final_pending);
    for (int i = 1; i < STRESS_N; i++)
        merkle_sync_start(s->inst[i], MT_DATA_MEMBERS, instance_find_conn_by_key(s->inst[i], s->inst[STRESS_HUB]->my_keys.public_key), MS_NS_STRESS, _str_final_done, NULL);
}

static void _str_final_done(const uint8_t peer[32], const char* ns, int result, void* arg) {
    (void)arg;
    if (!gs || gs->phase != 0) return;
    if (result != MT_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "stress final sync: round=%d peer=%s ns=%s result=%d",
                    gs->final_round, sc_public_key_hex(peer, (char[65]){0}), ns, result);
        gs->phase = 2;
        return;
    }
    gs->final_pending--;
    if (gs->final_pending <= 0) _str_check_roots();
}

static void _str_verify(void) {
    struct str_state* s = gs;
    int ok = 1;

    // Нагрузка должна менять записи; равные корни сами по себе не доказывают доставку последних версий.
    for (int owner = STRESS_SPAM_BEGIN; owner <= STRESS_SPAM_END; owner++) {
        if (s->versions[owner] - (uint32_t)owner >= 10) continue;
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "stress writer=%d made fewer than ten updates", owner);
        ok = 0;
    }
    if (s->observer_starts[0] < 10 || s->observer_starts[1] < 10) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "stress observer starts too few: %u %u",
                    s->observer_starts[0], s->observer_starts[1]);
        ok = 0;
    }
    for (int i = 0; i < STRESS_N && ok; i++) {
        if (s->data[i].count != STRESS_N - 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "stress inst=%d items=%d expected=%d", i, s->data[i].count, STRESS_N - 1);
            ok = 0;
            break;
        }
        for (int owner = 1; owner < STRESS_N && ok; owner++) {
            uint64_t key = ((uint64_t)owner) << 60;
            int found = 0;
            for (int j = 0; j < s->data[i].count; j++) {
                if (s->data[i].items[j].key == key && s->data[i].items[j].val == s->versions[owner]) { found = 1; break; }
            }
            if (!found) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "stress inst=%d missing final version: owner=%d version=%u",
                            i, owner, s->versions[owner]);
                ok = 0;
            }
        }
    }

    /* Независимо пересчитать хеш каждой сохранённой строки, включая настоящий корень. */
    int na = _db_count_rows(s->inst[STRESS_HUB]->topo_sqlite_db, MS_NS_STRESS);
    for (int i = 0; i < STRESS_N && ok; i++) {
        sqlite3_stmt* st = NULL;
        int rc = sqlite3_prepare_v2(s->inst[i]->topo_sqlite_db,
            "SELECT level,prefix64,hash FROM merkle_tree_hash WHERE namespace=? ORDER BY level,prefix64", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, "0:" MS_NS_STRESS, -1, SQLITE_STATIC);
        int checked = 0, roots = 0;
        while (rc == SQLITE_OK && (rc = sqlite3_step(st)) == SQLITE_ROW) {
            uint8_t lv = (uint8_t)sqlite3_column_int(st, 0);
            uint64_t pf = (uint64_t)sqlite3_column_int64(st, 1);
            uint8_t expected[MT_HASH_SIZE];
            _expected_hash(&s->ctx[i], MS_NS_STRESS, lv, pf, expected);
            const void* stored = sqlite3_column_blob(st, 2);
            if (!stored || sqlite3_column_bytes(st, 2) != MT_HASH_SIZE || memcmp(expected, stored, MT_HASH_SIZE)) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "stress hash mismatch node=%d level=%u prefix=%016llx", i, lv, (unsigned long long)pf);
                ok = 0; break;
            }
            roots += lv == 0 && pf == 0;
            checked++; rc = SQLITE_OK;
        }
        int final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final || checked != na || roots != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "stress index node=%d rows=%d/%d roots=%d step=%d finalize=%d", i, checked, na, roots, rc, final);
            ok = 0;
        }
    }

    /* print summary */
    printf("  tree rows: %d  data items per node:", na);
    for (int i = 0; i < STRESS_N && i < 6; i++) printf(" %d", s->data[i].count);
    printf("..\n");

    s->phase = ok ? 1 : 2;
}

static void _str_check_roots(void) {
    struct str_state* s = gs;
    if (!s) return;
    if (stress_roots_equal(s)) {
        printf("  roots converged after %d round(s)\n", s->final_round);
        _str_verify();
    } else if (s->final_round < STRESS_MAX_ROUNDS) {
        s->round_timer = uasync_set_timeout(s->ua, STRESS_ROUND_GAP_TB, NULL, _str_round_cb, "str_round");
    } else {
        printf("  no convergence after %d rounds\n", s->final_round);
        s->phase = 2;
    }
}

static void _str_round_cb(void* arg) {
    (void)arg;
    if (!gs) return;
    gs->round_timer = NULL;
    _str_start_final_round();
}

static void _str_fail_timeout(void* arg) {
    (void)arg;
    if (!gs) return;
    gs->safety_timer = NULL;
    printf("  SAFETY TIMEOUT round=%d pending=%d spam_active=%d\n", gs->final_round, gs->final_pending, gs->spam_active);
    gs->phase = 2;
}

static void test_stress_spam(void) {
    TEST("stress: 10 spammers + 2 obs, 5s spam, verify merkle roots");
    if (_str_init() != 0) { FAIL("init failed"); _str_cleanup(); return; }

    struct str_state* s = gs;

    /* --- start spam --- */
    s->spam_active = 1;
    s->spam_started_us = get_time_us();
    srand(12345);
    for (int i = STRESS_SPAM_BEGIN; i <= STRESS_SPAM_END; i++) {
        /* stagger initial delays */
        int d = (rand() % 30) + 1;
        uasync_set_timeout(s->ua, (uint32_t)(d * 10), (void*)(intptr_t)i, _str_spam_cb, "spam");
    }
    /* observers sync with hub aggressively */
    uasync_set_timeout(s->ua, 10, (void*)(intptr_t)STRESS_OBS_A, _str_obs_spam_cb, "obsA");
    uasync_set_timeout(s->ua, 15, (void*)(intptr_t)STRESS_OBS_B, _str_obs_spam_cb, "obsB");

    /* --- spam stop + safety timeout: событиями, без холостых ожиданий --- */
    s->stop_timer = uasync_set_timeout(s->ua, (uint32_t)(STRESS_SPAM_MS * 10), NULL, _str_stop_spam, "str_stop");
    s->safety_timer = uasync_set_timeout(s->ua, STRESS_SAFETY_TB, NULL, _str_fail_timeout, "str_safety");

    printf("  spamming for %dms...\n", STRESS_SPAM_MS);
    /* Событийный цикл: фазы переключаются таймерами и done-коллбэками.
     * poll с ограниченным таймаутом (10ms), т.к. uasync_poll(ua,-1) при уже
     * просроченном ближайшем таймере уходит в бесконечный epoll_wait и не
     * обрабатывает expired-таймеры. */
    while (s->phase == 0) {
        // Пять секунд активной нагрузки — реальные; ускоряем только подготовку и финальный дренаж.
        if (s->spam_active) uasync_poll(s->ua, 100);
        else test_clock_poll(s->ua, 100);
    }

    if (s->phase != 1) { FAIL("convergence failed (phase=%d)", s->phase); _str_cleanup(); return; }
    PASS();
    _str_cleanup();
}
static int _cond_pair_equal(void) {
    if (data_a.count != data_b.count) return 0;
    for (int i = 0; i < data_a.count; i++)
        if (data_a.items[i].key != data_b.items[i].key || data_a.items[i].val != data_b.items[i].val) return 0;
    return _db_compare_trees(i_a->topo_sqlite_db, i_b->topo_sqlite_db, MS_NS_TEST) == 0;
}

static void test_large_leaf_and_automatic_update(void) {
    TEST("6000 records in one leaf (>64K), then automatic update without start/broadcast");
    if (_intg_init_two() != 0) { FAIL("setup failed"); return; }
    for (uint64_t i = 0; i < 6000; i++) _data_insert(&data_b, i, (uint32_t)i);
    _data_sort(&data_b);
    if (merkle_sync_recompute_path(i_b, MT_DATA_MEMBERS, MS_NS_TEST, 0) < 0) { FAIL("recompute failed"); _intg_cleanup(); return; }
    done_sync = 0;
    merkle_sync_start(i_a, MT_DATA_MEMBERS, instance_find_conn_by_key(i_a, i_b->my_keys.public_key), MS_NS_TEST, _on_sync_done, NULL);
    if (!_wait_for("large leaf", _cond_done, PHASE_TIMEOUT_TB) || !_cond_pair_equal()) {
        FAIL("large leaf did not converge"); _intg_cleanup(); return;
    }
    data_b.items[0].val = 9999;
    merkle_sync_recompute_path(i_b, MT_DATA_MEMBERS, MS_NS_TEST, 0);
    if (!_wait_for("automatic change", _cond_pair_equal, PHASE_TIMEOUT_TB)) {
        FAIL("automatic change did not propagate"); _intg_cleanup(); return;
    }
    PASS(); _intg_cleanup();
}

static int run_stage6(void) {
    printf("--- Stage 6: protocol edge cases ---\n");
    stage_failures = 0;
    test_cancel();
    test_start_overwrite();
    test_large_leaf_and_automatic_update();
    return stage_failures;
}

static int run_stage7(void) {
    printf("--- Stage 7: stress test (10 spammers + 2 A↔B obs, 5s) ---\n");
    stage_failures = 0;
    test_stress_spam();
    return stage_failures;
}

int main(void) {
    printf("=== test_merkle_sync ===\n");
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_DEBUG);
    test_clock_init(100);

    if (run_stage1() != 0) { printf("=== Stage 1 FAILED ===\n"); return 1; }
    printf("=== Stage 1 PASSED (%d tests) ===\n\n", tests_run);

    if (run_stage2() != 0) { printf("=== Stage 2 FAILED ===\n"); return 1; }
    printf("=== Stage 2 PASSED (%d tests) ===\n\n", tests_run);

    if (run_stage3() != 0) { printf("=== Stage 3 FAILED ===\n"); return 1; }
    printf("=== Stage 3 PASSED (%d tests) ===\n\n", tests_run);

    if (run_stage4() != 0) { printf("=== Stage 4 FAILED ===\n"); return 1; }
    printf("=== Stage 4 PASSED (%d tests) ===\n\n", tests_run);

    if (run_stage5() != 0) { printf("=== Stage 5 FAILED ===\n"); return 1; }
    printf("=== Stage 5 PASSED (%d tests) ===\n\n", tests_run);

    if (run_stage6() != 0) { printf("=== Stage 6 FAILED ===\n"); return 1; }
    printf("=== Stage 6 PASSED (%d tests) ===\n\n", tests_run);

    if (run_stage7() != 0) { printf("=== Stage 7 FAILED ===\n"); return 1; }
    printf("=== Stage 7 PASSED (%d tests) ===\n\n", tests_run);

    printf("=== ALL PASS (%d tests) ===\n", tests_run);
    test_clock_finish();
    return 0;
}
