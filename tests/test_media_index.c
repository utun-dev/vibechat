// test_media_index.c — юнит-тесты media_index (commit + register_async)
//
// Покрытие:
//   1.  1 чанк, точный размер
//   2.  N чанков, точное совпадение
//   3.  Последний чанк меньше
//   4.  Файл меньше размера чанка
//   5.  Много чанков (>3)
//   6.  media_base с trailing slash
//   7.  media_base без trailing slash
//   8.  node_sign верификация (Ed25519 verify)
//   9.  Разные chat_id
//   10. Полный async flow (register_async + copy=1)

#include "../src/media_delivery/media_index.h"
#include <assert.h>
#include "../src/media_async/media_async.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"

#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define BS 100

static int g_passed = 0;
static int g_failed = 0;
static int g_total  = 0;

#define TEST(name) do { \
    g_total++; \
    printf("  %-55s", name); \
} while(0)

#define OK() do { g_passed++; printf("OK\n"); } while(0)
#define FAIL(fmt, ...) do { \
    g_failed++; \
    printf("FAIL: " fmt "\n", ##__VA_ARGS__); \
} while(0)

/* ─── helpers ─── */

static sqlite3* make_db(void) {
    sqlite3* db = NULL;
    sqlite3_open(":memory:", &db);
    sqlite3_exec(db, "PRAGMA journal_mode=memory", NULL, NULL, NULL);
    return db;
}

static void free_db(sqlite3* db) { if (db) sqlite3_close(db); }

static void derive_pubkey(const uint8_t priv[32], uint8_t pub[32]) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, priv, 32);
    if (pkey) {
        size_t l = 32;
        EVP_PKEY_get_raw_public_key(pkey, pub, &l);
        EVP_PKEY_free(pkey);
    }
}

static int ed25519_verify(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64]) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, 32);
    if (!pkey) return -1;
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) { EVP_PKEY_free(pkey); return -1; }
    if (EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(mdctx); EVP_PKEY_free(pkey); return -1;
    }
    int rc = EVP_DigestVerify(mdctx, sig, 64, msg, len);
    EVP_MD_CTX_free(mdctx); EVP_PKEY_free(pkey);
    return rc == 1 ? 0 : -1;
}

static int row_count(sqlite3* db, const uint8_t* media_id) {
    sqlite3_stmt* st = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM media_files WHERE media_id=?", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, media_id, 16, SQLITE_STATIC);
    int c = 0;
    if (sqlite3_step(st) == SQLITE_ROW) c = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return c;
}

/* заполняет result рандомными данными для тестирования commit */
static void fill_result(struct media_index_result* r, int64_t file_size, int64_t block_size, int num_blocks) {
    memset(r, 0, sizeof(*r));
    r->file_size  = file_size;
    r->block_size = block_size;
    r->num_blocks = num_blocks;
    assert(ma_uuid(r->media_id) == 0);
    assert(ma_uuid(r->content_hash) == 0);
    assert(ma_uuid(r->content_hash + 16) == 0);
    r->block_ids  = u_malloc((size_t)num_blocks * 16);
    r->block_sigs = u_malloc((size_t)num_blocks * 64);
    for (int i = 0; i < num_blocks; i++) {
        assert(ma_uuid(r->block_ids + i * 16) == 0);
        assert(ma_uuid(r->block_sigs + i * 64) == 0);
        assert(ma_uuid(r->block_sigs + i * 64 + 16) == 0);
        assert(ma_uuid(r->block_sigs + i * 64 + 32) == 0);
        assert(ma_uuid(r->block_sigs + i * 64 + 48) == 0);
    }
}

static int check_row(sqlite3* db, const uint8_t* media_id, int chunk,
                      const uint8_t* content_hash, int64_t file_size,
                      int64_t chunk_size, int64_t offset, const uint8_t node_pubkey[32],
                      const char* chat_id, const char* location) {
    sqlite3_stmt* st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT block_id,content_hash,chat_id,location,holder,node_sign,timestamp,file_size,chunk_size,chunk,offset"
        " FROM media_files WHERE media_id=? AND chunk=?",
        -1, &st, NULL);
    if (!st) return -1;
    sqlite3_bind_blob(st, 1, media_id, 16, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, chunk);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return -1; }

    const uint8_t* bid = (const uint8_t*)sqlite3_column_blob(st, 0);
    int bid_len = sqlite3_column_bytes(st, 0);
    const uint8_t* ch = (const uint8_t*)sqlite3_column_blob(st, 1);
    int ch_len = sqlite3_column_bytes(st, 1);
    const char* cid = (const char*)sqlite3_column_text(st, 2);
    const char* loc = (const char*)sqlite3_column_text(st, 3);
    const void* holder = sqlite3_column_blob(st, 4);
    const uint8_t* nsig = (const uint8_t*)sqlite3_column_blob(st, 5);
    int nsig_len = sqlite3_column_bytes(st, 5);
    int64_t ts  = sqlite3_column_int64(st, 6);
    int64_t fsz = sqlite3_column_int64(st, 7);
    int64_t csz = sqlite3_column_int64(st, 8);
    int c       = sqlite3_column_int(st, 9);
    int64_t off = sqlite3_column_int64(st, 10);

    int err = 0;
    if (!bid || bid_len != 16) { printf("bad block_id "); err = -1; }
    if (!ch || ch_len != 32 || memcmp(ch, content_hash, 32) != 0) { printf("bad content_hash "); err = -1; }
    if (!cid || strcmp(cid, chat_id) != 0) { printf("bad chat_id "); err = -1; }
    if (!loc || strcmp(loc, location) != 0) { printf("bad location '%s'!='%s' ", loc ? loc : "NULL", location); err = -1; }
    if (!holder || sqlite3_column_bytes(st, 4) != 32 || memcmp(holder, node_pubkey, 32)) { printf("bad holder "); err = -1; }
    if (!nsig || nsig_len != 64) { printf("bad node_sign "); err = -1; }
    if (ts <= 0) { printf("bad timestamp %lld ", (long long)ts); err = -1; }
    if (fsz != file_size) { printf("bad file_size %lld!=%lld ", (long long)fsz, (long long)file_size); err = -1; }
    if (csz != chunk_size) { printf("bad chunk_size %lld!=%lld ", (long long)csz, (long long)chunk_size); err = -1; }
    if (c != chunk) { printf("bad chunk %d!=%d ", c, chunk); err = -1; }
    if (off != offset) { printf("bad offset %lld!=%lld ", (long long)off, (long long)offset); err = -1; }

    if (err == 0 && nsig && node_pubkey) {
        uint8_t sig_msg[116];
        size_t soff = 0;
        memcpy(sig_msg + soff, media_id, 16); soff += 16;
        memcpy(sig_msg + soff, content_hash, 32); soff += 32;
        memcpy(sig_msg + soff, &ts, 8); soff += 8;
        memcpy(sig_msg + soff, &fsz, 8); soff += 8;
        memcpy(sig_msg + soff, &csz, 8); soff += 8;
        int32_t cn = (int32_t)chunk; memcpy(sig_msg + soff, &cn, 4); soff += 4;
        memcpy(sig_msg + soff, &off, 8); soff += 8;
        memcpy(sig_msg + soff, node_pubkey, 32); soff += 32;
        if (ed25519_verify(node_pubkey, sig_msg, soff, nsig) != 0)
            { printf("bad node_sign verify "); err = -1; }
    }

    sqlite3_finalize(st);
    return err;
}

/* ─── тесты ─── */

static int test_single_chunk(void) {
    TEST("single chunk, exact size (100/100)");
    sqlite3* db = make_db();
    media_index_init(db);
    uint8_t priv[32], pub[32];
    assert(ma_uuid(priv) == 0); assert(ma_uuid(priv + 16) == 0);
    derive_pubkey(priv, pub);

    struct media_index_result r;
    fill_result(&r, 100, 100, 1);

    int rc = media_index_commit(db, &r, pub, priv, "ch1",
                                 "/data/ch1/file.bin", "/data");
    if (rc != 0) { FAIL("commit=%d", rc); media_index_result_free(&r); free_db(db); return -1; }
    if (row_count(db, r.media_id) != 1) { FAIL("row count != 1"); media_index_result_free(&r); free_db(db); return -1; }

    int cr = check_row(db, r.media_id, 0, r.content_hash, 100, 100, 0, pub, "ch1",
                        "ch1/file.bin");
    if (cr != 0) { FAIL("check_row"); media_index_result_free(&r); free_db(db); return -1; }

    OK();
    media_index_result_free(&r);
    free_db(db);
    return 0;
}

static int test_exact_multiple(void) {
    TEST("3 chunks, exact match (300/100)");
    sqlite3* db = make_db();
    media_index_init(db);
    uint8_t priv[32], pub[32];
    assert(ma_uuid(priv) == 0); assert(ma_uuid(priv + 16) == 0); derive_pubkey(priv, pub);

    struct media_index_result r;
    fill_result(&r, 300, 100, 3);

    if (media_index_commit(db, &r, pub, priv, "ch2", "/data/ch2/file.bin", "/data") != 0)
        { FAIL("commit"); media_index_result_free(&r); free_db(db); return -1; }
    if (row_count(db, r.media_id) != 3) { FAIL("row count != 3"); media_index_result_free(&r); free_db(db); return -1; }

    if (check_row(db, r.media_id, 0, r.content_hash, 300, 100, 0, pub, "ch2", "ch2/file.bin") != 0)
        { FAIL("chunk 0"); media_index_result_free(&r); free_db(db); return -1; }
    if (check_row(db, r.media_id, 1, r.content_hash, 300, 100, 100, pub, "ch2", "ch2/file.bin") != 0)
        { FAIL("chunk 1"); media_index_result_free(&r); free_db(db); return -1; }
    if (check_row(db, r.media_id, 2, r.content_hash, 300, 100, 200, pub, "ch2", "ch2/file.bin") != 0)
        { FAIL("chunk 2"); media_index_result_free(&r); free_db(db); return -1; }

    OK();
    media_index_result_free(&r); free_db(db);
    return 0;
}

static int test_partial_last(void) {
    TEST("last chunk smaller (250/100 → 3 chunks)");
    sqlite3* db = make_db();
    media_index_init(db);
    uint8_t priv[32], pub[32];
    assert(ma_uuid(priv) == 0); assert(ma_uuid(priv + 16) == 0); derive_pubkey(priv, pub);

    struct media_index_result r;
    fill_result(&r, 250, 100, 3);

    if (media_index_commit(db, &r, pub, priv, "ch3", "/data/ch3/file.bin", "/data") != 0)
        { FAIL("commit"); media_index_result_free(&r); free_db(db); return -1; }
    if (row_count(db, r.media_id) != 3) { FAIL("row count != 3"); media_index_result_free(&r); free_db(db); return -1; }

    if (check_row(db, r.media_id, 0, r.content_hash, 250, 100, 0, pub, "ch3", "ch3/file.bin") != 0)
        { FAIL("chunk 0"); media_index_result_free(&r); free_db(db); return -1; }
    if (check_row(db, r.media_id, 1, r.content_hash, 250, 100, 100, pub, "ch3", "ch3/file.bin") != 0)
        { FAIL("chunk 1"); media_index_result_free(&r); free_db(db); return -1; }
    if (check_row(db, r.media_id, 2, r.content_hash, 250, 50, 200, pub, "ch3", "ch3/file.bin") != 0)
        { FAIL("chunk 2 (last, chunk_size=50)"); media_index_result_free(&r); free_db(db); return -1; }

    OK();
    media_index_result_free(&r); free_db(db);
    return 0;
}

static int test_file_smaller_than_block(void) {
    TEST("file smaller than block (50/100 → 1 chunk)");
    sqlite3* db = make_db();
    media_index_init(db);
    uint8_t priv[32], pub[32];
    assert(ma_uuid(priv) == 0); assert(ma_uuid(priv + 16) == 0); derive_pubkey(priv, pub);

    struct media_index_result r;
    fill_result(&r, 50, 100, 1);

    if (media_index_commit(db, &r, pub, priv, "ch4", "/data/ch4/file.bin", "/data") != 0)
        { FAIL("commit"); media_index_result_free(&r); free_db(db); return -1; }
    if (row_count(db, r.media_id) != 1) { FAIL("row count != 1"); media_index_result_free(&r); free_db(db); return -1; }

    if (check_row(db, r.media_id, 0, r.content_hash, 50, 50, 0, pub, "ch4", "ch4/file.bin") != 0)
        { FAIL("chunk 0"); media_index_result_free(&r); free_db(db); return -1; }

    OK();
    media_index_result_free(&r); free_db(db);
    return 0;
}

static int test_many_chunks(void) {
    TEST("8 chunks (800/100)");
    sqlite3* db = make_db();
    media_index_init(db);
    uint8_t priv[32], pub[32];
    assert(ma_uuid(priv) == 0); assert(ma_uuid(priv + 16) == 0); derive_pubkey(priv, pub);

    struct media_index_result r;
    fill_result(&r, 800, 100, 8);

    if (media_index_commit(db, &r, pub, priv, "ch5", "/data/ch5/file.bin", "/data") != 0)
        { FAIL("commit"); media_index_result_free(&r); free_db(db); return -1; }
    if (row_count(db, r.media_id) != 8) { FAIL("row count != 8"); media_index_result_free(&r); free_db(db); return -1; }

    // все block_id уникальны
    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) {
            if (memcmp(r.block_ids + i * 16, r.block_ids + j * 16, 16) == 0)
                { FAIL("block_id collision i=%d j=%d", i, j); media_index_result_free(&r); free_db(db); return -1; }
        }
    }

    for (int i = 0; i < 8; i++) {
        int64_t cs = 100, off = i * 100;
        if (check_row(db, r.media_id, i, r.content_hash, 800, cs, off, pub, "ch5", "ch5/file.bin") != 0)
            { FAIL("chunk %d", i); media_index_result_free(&r); free_db(db); return -1; }
    }

    OK();
    media_index_result_free(&r); free_db(db);
    return 0;
}

static int test_location_trailing_slash(void) {
    TEST("media_base with trailing slash");
    sqlite3* db = make_db();
    media_index_init(db);
    uint8_t priv[32], pub[32];
    assert(ma_uuid(priv) == 0); assert(ma_uuid(priv + 16) == 0); derive_pubkey(priv, pub);

    struct media_index_result r;
    fill_result(&r, 100, 100, 1);

    if (media_index_commit(db, &r, pub, priv, "tc", "/root/media/tc/v.bin", "/root/") != 0)
        { FAIL("commit"); media_index_result_free(&r); free_db(db); return -1; }
    if (check_row(db, r.media_id, 0, r.content_hash, 100, 100, 0, pub, "tc", "media/tc/v.bin") != 0)
        { FAIL("check_row"); media_index_result_free(&r); free_db(db); return -1; }

    OK();
    media_index_result_free(&r); free_db(db);
    return 0;
}

static int test_location_no_slash(void) {
    TEST("media_base without trailing slash");
    sqlite3* db = make_db();
    media_index_init(db);
    uint8_t priv[32], pub[32];
    assert(ma_uuid(priv) == 0); assert(ma_uuid(priv + 16) == 0); derive_pubkey(priv, pub);

    struct media_index_result r;
    fill_result(&r, 100, 100, 1);

    if (media_index_commit(db, &r, pub, priv, "tc2", "/root/media/tc2/v.bin", "/root") != 0)
        { FAIL("commit"); media_index_result_free(&r); free_db(db); return -1; }
    if (check_row(db, r.media_id, 0, r.content_hash, 100, 100, 0, pub, "tc2", "media/tc2/v.bin") != 0)
        { FAIL("check_row"); media_index_result_free(&r); free_db(db); return -1; }

    OK();
    media_index_result_free(&r); free_db(db);
    return 0;
}

static int test_different_chat_ids(void) {
    TEST("two commits with different chat_ids");
    sqlite3* db = make_db();
    media_index_init(db);
    uint8_t priv[32], pub[32];
    assert(ma_uuid(priv) == 0); assert(ma_uuid(priv + 16) == 0); derive_pubkey(priv, pub);

    uint8_t priv2[32], pub2[32];
    assert(ma_uuid(priv2) == 0); assert(ma_uuid(priv2 + 16) == 0); derive_pubkey(priv2, pub2);
    struct media_index_result r1, r2;
    fill_result(&r1, 100, 100, 1);
    fill_result(&r2, 200, 100, 2);

    if (media_index_commit(db, &r1, pub, priv, "chatA", "/d/cA/f.bin", "/d") != 0)
        { FAIL("commit1"); goto err; }
    if (media_index_commit(db, &r2, pub2, priv2, "chatB", "/d/cB/f.bin", "/d") != 0)
        { FAIL("commit2"); goto err; }
    if (row_count(db, r1.media_id) != 1) { FAIL("r1 count"); goto err; }
    if (row_count(db, r2.media_id) != 2) { FAIL("r2 count"); goto err; }
    if (check_row(db, r1.media_id, 0, r1.content_hash, 100, 100, 0, pub, "chatA", "cA/f.bin") != 0)
        { FAIL("r1 row"); goto err; }
    if (check_row(db, r2.media_id, 0, r2.content_hash, 200, 100, 0, pub2, "chatB", "cB/f.bin") != 0)
        { FAIL("r2 row 0"); goto err; }
    if (check_row(db, r2.media_id, 1, r2.content_hash, 200, 100, 100, pub2, "chatB", "cB/f.bin") != 0)
        { FAIL("r2 row 1"); goto err; }

    OK();
    media_index_result_free(&r1); media_index_result_free(&r2); free_db(db);
    return 0;
err:
    media_index_result_free(&r1); media_index_result_free(&r2); free_db(db);
    return -1;
}

/* ─── интеграционный тест: register_async ─── */

struct async_ctx {
    volatile int done;
    int err;
    struct media_index_result result;
};

static void on_registered(void* arg, int err, const struct media_index_result* r) {
    struct async_ctx* ctx = (struct async_ctx*)arg;
    ctx->err = err;
    if (!err && r) {
        memcpy(&ctx->result, r, sizeof(*r));
        if (r->block_ids) {
            ctx->result.block_ids = u_malloc((size_t)r->num_blocks * 16);
            memcpy(ctx->result.block_ids, r->block_ids, (size_t)r->num_blocks * 16);
        }
        if (r->block_sigs) {
            ctx->result.block_sigs = u_malloc((size_t)r->num_blocks * 64);
            memcpy(ctx->result.block_sigs, r->block_sigs, (size_t)r->num_blocks * 64);
        }
    } else memset(&ctx->result, 0, sizeof(ctx->result));
    ctx->done = 1;
}

static void on_timeout(void* arg) {
    volatile int* done = (volatile int*)arg;
    if (done) *done = 1;
}

static int create_file(const char* path, int size) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    for (int i = 0; i < size; i++) fputc((i * 7 + 13) & 0xFF, f);
    fclose(f);
    return 0;
}

static int test_register_async_flow(void) {
    TEST("register_async: copy file + verify DB");
    char tempd[256];
    snprintf(tempd, sizeof(tempd), "/tmp/test_media_idx_%d", (int)getpid());
    utun_mkdir(tempd, 0755);

    char src[512]; snprintf(src, sizeof(src), "%s/src.bin", tempd);
    char dst[512]; snprintf(dst, sizeof(dst), "%s/media/ch_test/dst.bin", tempd);
    {
        char t1[512], t2[512];
        snprintf(t1, sizeof(t1), "%s/media", tempd);         utun_mkdir(t1, 0755);
        snprintf(t2, sizeof(t2), "%s/media/ch_test", tempd); utun_mkdir(t2, 0755);
    }

    create_file(src, 400);  // 400 bytes → 4 chunks of 100

    struct UASYNC* ua = uasync_create();
    if (!ua) { FAIL("uasync_create"); rmdir(tempd); return -1; }

    struct async_ctx ctx = {0};
    void* th = uasync_set_timeout(ua, 50000, &ctx.done, on_timeout, "tout_async");

    struct media_async* ma = media_async_create();
    sqlite3* db = make_db();
    media_index_init(db);

    uint8_t priv[32], pub[32];
    assert(ma_uuid(priv) == 0); assert(ma_uuid(priv + 16) == 0); derive_pubkey(priv, pub);

    media_index_register_async(ma, ua, db, pub, priv, "ch_test",
                                src, dst, 1, tempd, on_registered, &ctx);

    while (!ctx.done) uasync_poll(ua, 100);

    uasync_cancel_timeout(ua, th);

    if (ctx.err != 0) { FAIL("register_async err=%d", ctx.err); goto cleanup; }
    { struct stat st; if (stat(dst, &st) != 0) { FAIL("dst file not created"); goto cleanup; } }

    int nb = ctx.result.num_blocks;
    int64_t bs = ctx.result.block_size;
    int64_t fs = ctx.result.file_size;
    if (fs != 400) { FAIL("file_size=%lld != 400", (long long)fs); goto cleanup; }
    if (nb < 1 || nb > 400) { FAIL("num_blocks=%d unexpected", nb); goto cleanup; }
    if (row_count(db, ctx.result.media_id) != nb) { FAIL("row count %d != %d", row_count(db, ctx.result.media_id), nb); goto cleanup; }

    for (int i = 0; i < nb; i++) {
        int64_t cs = (i == nb - 1) ? fs - i * bs : bs;
        int64_t off = i * bs;
        if (check_row(db, ctx.result.media_id, i, ctx.result.content_hash, fs, cs, off, pub, "ch_test",
                       "media/ch_test/dst.bin") != 0)
            { FAIL("chunk %d", i); goto cleanup; }
    }

    // block_ids уникальны
    for (int i = 0; i < nb; i++)
        for (int j = i+1; j < nb; j++)
            if (memcmp(ctx.result.block_ids + i*16, ctx.result.block_ids + j*16, 16) == 0)
                { FAIL("block_id collision i=%d j=%d", i, j); goto cleanup; }

    OK();

cleanup:
    media_index_result_free(&ctx.result);
    free_db(db);
    media_async_destroy(ma);
    uasync_destroy(ua, 0);
    unlink(src); unlink(dst);
    { char t1[512]; snprintf(t1,sizeof(t1),"%s/media/ch_test",tempd); rmdir(t1); }
    { char t2[512]; snprintf(t2,sizeof(t2),"%s/media",tempd);       rmdir(t2); }
    rmdir(tempd);
    return 0;
}

/* ─── main ─── */

int main(void) {
    debug_set_level(DEBUG_LEVEL_ERROR);
    printf("=== test_media_index ===\n\n");

    test_single_chunk();
    test_exact_multiple();
    test_partial_last();
    test_file_smaller_than_block();
    test_many_chunks();
    test_location_trailing_slash();
    test_location_no_slash();
    test_different_chat_ids();
    test_register_async_flow();

    printf("\nResults: %d/%d passed, %d failed\n", g_passed, g_total, g_failed);
    fflush(stdout);
    return g_failed ? 1 : 0;
}
