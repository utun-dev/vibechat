/* Один CTR-сеанс в нескольких потоках: невыровненные участки, ограничение очереди и shutdown без poll. */
#include "transport_layer/crypto_pool.h"
#include "transport_layer/transport_lane.h"
#include "transport_layer/secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include <pthread.h>
#include <openssl/evp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); abort(); } } while (0)
struct test_group {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned entered, done, gate;
    pthread_t owner;
};
struct test_job {
    struct test_group *group;
    struct sc_stream_key key;
    uint64_t offset;
    uint8_t bytes[1031], expected[1031];
    int rc;
};

static void work(void *arg) {
    struct test_job *job = arg;
    struct test_group *g = job->group;
    if (g->gate) {
        pthread_mutex_lock(&g->mutex);
        if (++g->entered >= g->gate) pthread_cond_broadcast(&g->cond);
        struct timespec deadline;
        CHECK(!clock_gettime(CLOCK_REALTIME, &deadline)); deadline.tv_sec += 5;
        while (g->entered < g->gate) CHECK(!pthread_cond_timedwait(&g->cond, &g->mutex, &deadline));
        pthread_mutex_unlock(&g->mutex);
    }
    job->rc = sc_stream_xor_at(&job->key, job->offset, job->bytes, sizeof(job->bytes));
}
static void done(void *arg) {
    struct test_job *job = arg;
    CHECK(pthread_equal(pthread_self(), job->group->owner));
    CHECK(!job->rc && !memcmp(job->bytes, job->expected, sizeof(job->bytes)));
    job->group->done++;
}

static void check_pool(struct UASYNC *ua, unsigned count, int drain) {
    struct test_group g = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, count, pthread_self()};
    struct test_job jobs[8] = {0};
    uint8_t key[16] = {1,2,3}, salt[8] = {7,8,9};
    struct sc_stream_state stream = {0};
    CHECK(!sc_stream_init(key, &stream, 0, salt));
    uint8_t handshake[51] = {0};
    CHECK(!sc_stream_xor(&stream, handshake, sizeof(handshake)) && stream.offset == sizeof(handshake));
    struct crypto_pool *pool = crypto_pool_create(ua, count, 8, 8 * sizeof(jobs[0]), "test");
    CHECK(pool);
    for (unsigned i = 0; i < 8; ++i) {
        struct test_job *j = &jobs[i];
        j->group = &g; j->key = stream.key; j->offset = stream.offset;
        for (unsigned k = 0; k < sizeof(j->bytes); ++k) j->bytes[k] = j->expected[k] = (uint8_t)(k + i);
        CHECK(!sc_stream_xor(&stream, j->expected, sizeof(j->expected)));
        struct crypto_task task = {work, done, j, sizeof(*j)};
        CHECK(!crypto_pool_submit(pool, &task));
    }
    if (count) {
        struct crypto_task extra = {work, done, &jobs[0], sizeof(jobs[0])};
        CHECK(crypto_pool_submit(pool, &extra) == -2);
    }
    if (drain) {
        uint64_t deadline = get_time_us() + 5000000;
        while (g.done < 8 && get_time_us() < deadline) uasync_poll(ua, 100);
        CHECK(g.done == 8);
    }
    crypto_pool_destroy(pool);
    CHECK(g.done == 8 && (!count || g.entered == 8));
    sc_stream_cleanup(&stream);
    pthread_cond_destroy(&g.cond); pthread_mutex_destroy(&g.mutex);
}

static void check_counter_carry(void) {
    /* Сравниваем переход через младшие 32 бита с независимым последовательным EVP. */
    struct sc_stream_key key = {{1}, {0}};
    uint64_t offset = (UINT64_C(1) << 36) - 7;
    uint8_t actual[71] = {0}, expected[80] = {0}, iv[16] = {0};
    memset(iv + 12, 255, 4);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new(); int n = 0;
    CHECK(ctx && EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, key.key, iv) == 1);
    CHECK(EVP_EncryptUpdate(ctx, expected, &n, expected, sizeof(expected)) == 1 && n == sizeof(expected));
    CHECK(!sc_stream_xor_at(&key, offset, actual, sizeof(actual)));
    CHECK(!memcmp(actual, expected + 9, sizeof(actual)));
    CHECK(sc_stream_xor_at(&key, UINT64_MAX, actual, 2) == SC_ERR_INVALID_ARG);
    EVP_CIPHER_CTX_free(ctx);
}

static void lane_owner(void *arg) { *(pthread_t *)arg = pthread_self(); }

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    check_counter_carry();
    check_pool(ua, 0, 1); check_pool(ua, 1, 1); check_pool(ua, 4, 1); check_pool(ua, 4, 0);
    for (unsigned n = 0; n <= 1; ++n) {
        struct transport_lane *lane = transport_lane_create(ua, n, "test-lane"); CHECK(lane);
        pthread_t owner;
        transport_lane_call(lane, lane_owner, &owner);
        CHECK(!!pthread_equal(owner, pthread_self()) == !n);
        transport_lane_destroy(lane);
    }
    uasync_destroy(ua, 0);
    puts("PASS: parallel CTR, unaligned ranges/carry, inline/threaded lanes, queue limits and shutdown");
    return 0;
}
