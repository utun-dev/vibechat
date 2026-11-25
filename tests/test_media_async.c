// test_media_async.c — стресс-тест примитивов media_async (многопоточность + корректность)
//
// 4 потока одновременно выполняют ma_* операции на файлах 1K/5K/10K.
// Тест длится 2 секунды, затем потоки останавливаются.
// Проверяется корректность каждого результата и отсутствие заклиниваний.

#include "../src/media_async/media_async.h"
#include "../lib/platform_compat.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <openssl/evp.h>

#define NUM_THREADS      4
#define TEST_TIMEOUT_SEC 2
#define NUM_FILES        3
#define SIZE_1K          1024
#define SIZE_5K          5120
#define SIZE_10K         10240
#define NUM_OPS          5

static int g_running = 1; /* Атомарный stop-флаг; статистика читается только после pthread_join. */

static const char* g_op_names[NUM_OPS] = {
    "SHA256-file", "Sign-block", "Copy-file",
    "UUID", "FileSize"
};

struct thread_stats {
    int tid;
    int op_count;
    int errors;
    int op_counts[NUM_OPS];
    double max_time_us[NUM_OPS];   // максимальное время (микросекунды) по каждой операции
};

static char        g_test_dir[256];
static char        g_file_paths[NUM_FILES][512];
static uint8_t     g_file_hashes[NUM_FILES][32];
static int         g_file_sizes[NUM_FILES];
static uint8_t     g_test_data[NUM_FILES][SIZE_10K];
static uint8_t     g_ed25519_privkey[32];
static uint8_t     g_expected_sigs[NUM_FILES][64];

static int64_t now_us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static void fill_test_data(uint8_t* buf, int size, int seed) {
    unsigned s = (unsigned)seed;
    for (int i = 0; i < size; i++) {
        s = s * 1103515245 + 12345;
        buf[i] = (uint8_t)((s >> 16) & 0xFF);
    }
}

static void* worker_thread(void* arg) {
    struct thread_stats* st = (struct thread_stats*)arg;
    unsigned s = (unsigned)(time(NULL) ^ (st->tid * 0x9E3779B9 << 8));

    while (__atomic_load_n(&g_running, __ATOMIC_RELAXED)) {
        s = s * 1103515245 + 12345;
        int op = (int)((s >> 16) % 4);  // random: 0=SHA256, 1=Sign, 2=Copy, 3=UUID+FileSize
        s = s * 1103515245 + 12345;
        int fi = (int)((s >> 16) % NUM_FILES);
        int size = g_file_sizes[fi];

        int real_op = -1;
        int64_t t0, elapsed;

        switch (op) {
        case 0: { // SHA256 file
            real_op = 0;
            uint8_t hash[32];
            t0 = now_us();
            int rc = ma_sha256_file(g_file_paths[fi], hash);
            elapsed = now_us() - t0;
            if (rc != 0 || memcmp(hash, g_file_hashes[fi], 32) != 0) st->errors++;
            break;
        }
        case 1: { // Ed25519 sign
            real_op = 1;
            uint8_t sig[64];
            t0 = now_us();
            int rc = ma_sign_block(g_ed25519_privkey, g_test_data[fi], (size_t)size, sig);
            elapsed = now_us() - t0;
            if (rc != 0 || memcmp(sig, g_expected_sigs[fi], 64) != 0) st->errors++;
            break;
        }
        case 2: { // copy file
            real_op = 2;
            char dst[512];
            snprintf(dst, sizeof(dst), "%s/cp_t%d_n%d.bin", g_test_dir, st->tid, st->op_count);
            t0 = now_us();
            int rc = ma_copy_file(g_file_paths[fi], dst);
            elapsed = now_us() - t0;
            if (rc == 0) {
                uint8_t hash[32];
                if (ma_sha256_file(dst, hash) != 0 || memcmp(hash, g_file_hashes[fi], 32) != 0)
                    st->errors++;
                unlink(dst);
            } else st->errors++;
            break;
        }
        case 3: { // UUID
            real_op = 3;
            uint8_t uuid[16];
            t0 = now_us();
            if (ma_uuid(uuid)) st->errors++;
            elapsed = now_us() - t0;
            int zero = 1;
            for (int i = 0; i < 16; i++) { if (uuid[i] != 0) { zero = 0; break; } }
            if (zero) st->errors++;

            // file_size отдельно с замером времени
            t0 = now_us();
            int64_t fsz = ma_file_size(g_file_paths[fi]);
            int64_t et = now_us() - t0;
            if (fsz != g_file_sizes[fi]) st->errors++;
            st->op_counts[4]++;
            if (et > st->max_time_us[4]) st->max_time_us[4] = (double)et;
            break;
        }
        }

        st->op_count++;
        if (real_op >= 0) {
            st->op_counts[real_op]++;
            if (elapsed > st->max_time_us[real_op])
                st->max_time_us[real_op] = (double)elapsed;
        }
    }
    return NULL;
}

int main(void) {
    int ret = 0;

    snprintf(g_test_dir, sizeof(g_test_dir), "/tmp/test_media_async_%d", (int)getpid());
    if (utun_mkdir(g_test_dir, 0755) != 0) {
        perror("mkdir"); return 1;
    }

    g_file_sizes[0] = SIZE_1K;
    g_file_sizes[1] = SIZE_5K;
    g_file_sizes[2] = SIZE_10K;

    for (int i = 0; i < NUM_FILES; i++) {
        fill_test_data(g_test_data[i], g_file_sizes[i], 42 + i * 100);
        snprintf(g_file_paths[i], sizeof(g_file_paths[i]), "%s/test_%dk.bin", g_test_dir, g_file_sizes[i] / 1024);
        FILE* f = fopen(g_file_paths[i], "wb");
        if (!f) { perror("fopen"); ret = 1; goto cleanup; }
        fwrite(g_test_data[i], 1, (size_t)g_file_sizes[i], f);
        fclose(f);
        ma_sha256_file(g_file_paths[i], g_file_hashes[i]);
    }

    {
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
        if (!pctx) { fprintf(stderr, "EVP_PKEY_CTX_new_id failed\n"); ret = 1; goto cleanup; }
        EVP_PKEY_keygen_init(pctx);
        EVP_PKEY* pkey = NULL;
        if (EVP_PKEY_keygen(pctx, &pkey) != 1 || !pkey) {
            fprintf(stderr, "EVP_PKEY_keygen failed\n"); EVP_PKEY_CTX_free(pctx); ret = 1; goto cleanup;
        }
        EVP_PKEY_CTX_free(pctx);
        size_t keylen = 32;
        EVP_PKEY_get_raw_private_key(pkey, g_ed25519_privkey, &keylen);
        EVP_PKEY_free(pkey);
        for (int i = 0; i < NUM_FILES; i++)
            ma_sign_block(g_ed25519_privkey, g_test_data[i], (size_t)g_file_sizes[i], g_expected_sigs[i]);
    }

    pthread_t threads[NUM_THREADS];
    struct thread_stats stats[NUM_THREADS];
    memset(stats, 0, sizeof(stats));
    for (int i = 0; i < NUM_THREADS; i++) {
        stats[i].tid = i;
        if (pthread_create(&threads[i], NULL, worker_thread, &stats[i]) != 0) {
            fprintf(stderr, "pthread_create(%d) failed\n", i);
            __atomic_store_n(&g_running, 0, __ATOMIC_RELAXED);
            for (int j = 0; j < i; j++) pthread_join(threads[j], NULL);
            ret = 1; goto cleanup;
        }
    }

    sleep(TEST_TIMEOUT_SEC);
    __atomic_store_n(&g_running, 0, __ATOMIC_RELAXED);

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    printf("\n=== media_async stress test ===\n");
    printf("Timeout: %d sec,  Threads: %d,  Files: 1K/5K/10K\n\n", TEST_TIMEOUT_SEC, NUM_THREADS);

    int total_ops = 0, total_errors = 0;
    int totals_op[NUM_OPS] = {0};
    double max_time_all[NUM_OPS] = {0};

    for (int i = 0; i < NUM_THREADS; i++) {
        printf("Thread %d: %d ops (", i, stats[i].op_count);
        for (int o = 0; o < NUM_OPS; o++) {
            if (o > 0) printf(" ");
            printf("%s:%d", g_op_names[o], stats[i].op_counts[o]);
            totals_op[o] += stats[i].op_counts[o];
        }
        printf(")  errors=%d\n", stats[i].errors);

        printf("         max-time(us): ");
        for (int o = 0; o < NUM_OPS; o++) {
            if (o > 0) printf(" ");
            printf("%s:%.0f", g_op_names[o], stats[i].max_time_us[o]);
            if (stats[i].max_time_us[o] > max_time_all[o]) max_time_all[o] = stats[i].max_time_us[o];
        }
        printf("\n");

        total_ops += stats[i].op_count;
        total_errors += stats[i].errors;
    }

    printf("\nTotal: %d ops, %d errors\n", total_ops, total_errors);
    printf("Op totals: ");
    for (int o = 0; o < NUM_OPS; o++) printf("%s:%d ", g_op_names[o], totals_op[o]);
    printf("\nMax time(us): ");
    for (int o = 0; o < NUM_OPS; o++) printf("%s:%.0f ", g_op_names[o], max_time_all[o]);
    printf("\n\n");

    /* ─── sanity checks ─── */

    // минимальный throughput: хотя бы 1000 ops на поток за 2 сек
    int min_ops_per_thread = 1000;
    // разбалансировка потоков: самый медленный поток не хуже 0.1× от самого быстрого
    double balance_ratio = 0.1;
    // макс допустимое время SHA256/Sign/Copy/USB операции (микросекунды)
    double max_us_sha      = 100000;  // 100ms
    double max_us_sign     = 100000;
    double max_us_copy     = 100000;
    double max_us_uuid     =  5000;   // 5ms
    double max_us_filesize =  5000;

    int min_ops = stats[0].op_count, max_ops = stats[0].op_count;
    for (int i = 1; i < NUM_THREADS; i++) {
        if (stats[i].op_count < min_ops) min_ops = stats[i].op_count;
        if (stats[i].op_count > max_ops) max_ops = stats[i].op_count;
    }

    printf("Sanity checks:\n");

    if (total_errors > 0) {
        printf("  FAIL: %d errors\n", total_errors);
        ret = 1;
    }

    if (total_ops < min_ops_per_thread * NUM_THREADS) {
        printf("  FAIL: total ops %d < %d (throughput too low)\n",
               total_ops, min_ops_per_thread * NUM_THREADS);
        ret = 1;
    } else {
        printf("  OK: total ops=%d >= %d\n", total_ops, min_ops_per_thread * NUM_THREADS);
    }

    if (max_ops > 0 && min_ops < (int)(max_ops * balance_ratio)) {
        printf("  FAIL: thread imbalance min=%d max=%d (ratio %.3f < %.1f)\n",
               min_ops, max_ops, max_ops > 0 ? (double)min_ops / max_ops : 0, balance_ratio);
        ret = 1;
    } else {
        printf("  OK: thread balance min=%d max=%d (ratio %.3f)\n",
               min_ops, max_ops, max_ops > 0 ? (double)min_ops / max_ops : 0);
    }

    double limits[NUM_OPS] = { max_us_sha, max_us_sign, max_us_copy, max_us_uuid, max_us_filesize };
    for (int o = 0; o < NUM_OPS; o++) {
        if (max_time_all[o] > limits[o]) {
            printf("  FAIL: %s max-time %.0fus > %.0fus\n", g_op_names[o], max_time_all[o], limits[o]);
            ret = 1;
        } else if (max_time_all[o] <= 0.0) {
            printf("  WARN: %s max-time 0 (no measurements?)\n", g_op_names[o]);
        } else {
            printf("  OK: %s max-time %.0fus\n", g_op_names[o], max_time_all[o]);
        }
    }

    if (ret == 0) {
        printf("\nPASS: %d ops, 0 errors, all checks OK\n", total_ops);
    } else {
        printf("\nFAIL: sanity checks failed\n");
    }

cleanup:
    for (int i = 0; i < NUM_FILES; i++) unlink(g_file_paths[i]);
    rmdir(g_test_dir);
    return ret;
}
