/* Длительное потоковое шифрование PM не мешает heartbeat uasync. */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <openssl/sha.h>

#include "../lib/platform_compat.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../src/dm/dm_crypto.h"
#include "../src/media_async/media_async.h"

struct worker_test {
    struct UASYNC* ua;
    struct media_async* ma;
    pthread_t event_thread;
    void* heartbeat;
    unsigned beats, encryptions;
    uint64_t start, last, max_gap;
    int result, completed;
};

static void work(void* arg) {
    struct worker_test* t = arg;
    t->result = -1;
    if (pthread_equal(pthread_self(), t->event_thread)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "PM crypto executed in the event thread"); return;
    }
    FILE* source = tmpfile();
    FILE* cipher = tmpfile();
    FILE* plain = tmpfile();
    if (!source || !cipher || !plain) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "worker fixture file creation failed"); goto done; }
    uint8_t data[65536], key[32] = {11}, id[16] = {23}, hash[32], expected[32], actual[32];
    memset(data, 0xa7, sizeof(data));
    SHA256_CTX digest;
    SHA256_Init(&digest);
    for (unsigned i = 0; i < 128; i++) {
        if (fwrite(data, 1, sizeof(data), source) != sizeof(data)) goto done;
        SHA256_Update(&digest, data, sizeof(data));
    }
    SHA256_Final(expected, &digest);
    uint64_t until = get_time_tb() + 5000; /* Не менее 0,5 с реальной файловой криптографии. */
    uint64_t size = 0;
    do {
        rewind(source); rewind(cipher);
        /* Каждый новый шифротекст получает отдельный media_id. */
        memcpy(id, &t->encryptions, sizeof(t->encryptions));
        if (media_encrypt_stream(key, (const uint8_t[32]){42}, id, source, cipher, &size, hash)) goto done;
        t->encryptions++;
    } while (get_time_tb() < until);
    rewind(cipher);
    if (size != 8 * 1024 * 1024 || media_decrypt_stream(key, (const uint8_t[32]){42}, id, size, hash, cipher, plain)) goto done;
    rewind(plain);
    SHA256_Init(&digest);
    size_t n;
    while ((n = fread(data, 1, sizeof(data), plain))) SHA256_Update(&digest, data, n);
    if (ferror(plain)) goto done;
    SHA256_Final(actual, &digest);
    t->result = memcmp(expected, actual, sizeof(actual)) ? -1 : 0;
done:
    if (source) fclose(source);
    if (cipher) fclose(cipher);
    if (plain) fclose(plain);
    if (t->result) DEBUG_ERROR(DEBUG_CATEGORY_DM, "PM worker crypto verification failed");
}

static void done(void* arg, int error) {
    struct worker_test* t = arg;
    if (error || !pthread_equal(pthread_self(), t->event_thread)) t->result = -1;
    t->completed = 1;
    uasync_stop(t->ua);
}

static void heartbeat(void* arg) {
    struct worker_test* t = arg;
    t->heartbeat = NULL;
    uint64_t now = get_time_tb(), gap = now - t->last;
    if (gap > t->max_gap) t->max_gap = gap;
    t->last = now; t->beats++;
    if (now - t->start > 100000) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "PM worker timeout"); uasync_stop(t->ua); return; }
    t->heartbeat = uasync_set_timeout(t->ua, 100, t, heartbeat, "pm_worker_heartbeat");
    if (!t->heartbeat) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "heartbeat allocation failed"); uasync_stop(t->ua); }
}

struct cancellation_test { int worked, calls, error; };
static void cancellation_work(void* arg) { ((struct cancellation_test*)arg)->worked = 1; }
static void cancellation_done(void* arg, int error) {
    struct cancellation_test* test = arg;
    test->calls++; test->error = error;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    struct worker_test t = {0};
    t.event_thread = pthread_self(); t.ua = uasync_create(); t.ma = media_async_create();
    if (!t.ua || !t.ma) return 1;
    t.start = t.last = get_time_tb();
    t.heartbeat = uasync_set_timeout(t.ua, 100, &t, heartbeat, "pm_worker_heartbeat");
    media_async_submit(t.ma, t.ua, work, &t, done, &t);
    if (!t.completed) uasync_mainloop(t.ua);
    if (t.heartbeat) uasync_cancel_timeout(t.ua, t.heartbeat);
    int ok = t.completed && !t.result && t.beats >= 10;
    media_async_destroy(t.ma);
    struct cancellation_test cancellation = {0};
    t.ma = media_async_create();
    media_async_submit(t.ma, t.ua, cancellation_work, &cancellation, cancellation_done, &cancellation);
    media_async_destroy(t.ma); /* Join worker и отмена уже posted completion до poll. */
    uasync_poll(t.ua, 0);
    ok = ok && cancellation.worked && cancellation.calls == 1 && cancellation.error == MEDIA_ASYNC_CANCELLED;
    uasync_destroy(t.ua, 0);
    printf("test_dm_worker: 8MiB encrypt/decrypt, runs=%u heartbeat=%u max_gap=%.1fms %s\n",
           t.encryptions, t.beats, t.max_gap / 10.0, ok ? "PASS" : "FAIL");
    return !ok;
}
