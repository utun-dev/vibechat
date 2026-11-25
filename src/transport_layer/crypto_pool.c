#include "crypto_pool.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct pool_job {
    struct pool_job *next, *all_next, *all_prev;
    struct crypto_pool *pool;
    struct crypto_task task;
    struct posted_task *post;
    uint64_t queued_us, finished_us;
};
struct crypto_pool {
    struct UASYNC *owner;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t threads[64];
    unsigned thread_count, limit, pending, active, peak, pending_peak;
    size_t byte_limit, bytes;
    struct pool_job *head, *tail, *all;
    uint64_t completed, wait_us, work_us;
    uint64_t wait_max_us, work_max_us, done_us, done_max_us, delivered, next_log_us;
    int stopping, delivering;
    char name[32];
};

/* done/pending — owner, work/wait — workers под mutex. Сводка раз в секунду при crypto=debug и при stop. */
static void report(struct crypto_pool *pool, debug_level_t level) {
    pthread_mutex_lock(&pool->mutex);
    uint64_t count = pool->completed, wait = pool->wait_us, work = pool->work_us;
    uint64_t wait_max = pool->wait_max_us, work_max = pool->work_max_us;
    unsigned peak = pool->peak, active = pool->active;
    pthread_mutex_unlock(&pool->mutex);
    debug_output(level, DEBUG_CATEGORY_CRYPTO, __func__, __FILE__, __LINE__,
        "pool %s: jobs=%llu delivered=%llu pending=%u peak_pending=%u active=%u peak_parallel=%u "
        "wait_avg_us=%.2f wait_max_us=%llu crypto_avg_us=%.2f crypto_max_us=%llu done_avg_us=%.2f done_max_us=%llu",
        pool->name, (unsigned long long)count, (unsigned long long)pool->delivered, pool->pending, pool->pending_peak, active, peak,
        count ? (double)wait / count : 0, (unsigned long long)wait_max, count ? (double)work / count : 0,
        (unsigned long long)work_max, pool->delivered ? (double)pool->done_us / pool->delivered : 0,
        (unsigned long long)pool->done_max_us);
}

static void complete(void *arg) {
    struct pool_job *job = arg;
    struct crypto_pool *pool = job->pool;
    uint64_t now = get_time_us(), delay = now - job->finished_us;
    pool->delivered++; pool->done_us += delay;
    if (delay > pool->done_max_us) pool->done_max_us = delay;
    if (job->all_prev) job->all_prev->all_next = job->all_next;
    else pool->all = job->all_next;
    if (job->all_next) job->all_next->all_prev = job->all_prev;
    pool->pending--; pool->bytes -= job->task.bytes;
    struct crypto_task task = job->task;
    u_free(job);
    pool->delivering++;
    task.done(task.arg);
    pool->delivering--;
    if (now >= pool->next_log_us && debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CRYPTO)) {
        pool->next_log_us = now + 1000000;
        report(pool, DEBUG_LEVEL_DEBUG);
    }
}

static void *worker(void *arg) {
    struct crypto_pool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->head && !pool->stopping) pthread_cond_wait(&pool->ready, &pool->mutex);
        struct pool_job *job = pool->head;
        if (!job) { pthread_mutex_unlock(&pool->mutex); break; }
        pool->head = job->next;
        if (!pool->head) pool->tail = NULL;
        if (++pool->active > pool->peak) pool->peak = pool->active;
        pthread_mutex_unlock(&pool->mutex);
        uint64_t start = get_time_us();
        job->task.work(job->task.arg);
        uint64_t finish = get_time_us();
        job->finished_us = finish;
        pthread_mutex_lock(&pool->mutex);
        pool->active--; pool->completed++;
        pool->wait_us += start - job->queued_us; pool->work_us += finish - start;
        if (start - job->queued_us > pool->wait_max_us) pool->wait_max_us = start - job->queued_us;
        if (finish - start > pool->work_max_us) pool->work_max_us = finish - start;
        pthread_mutex_unlock(&pool->mutex);
        /* post зарезервирован до принятия: завершение не теряется при OOM. */
        uasync_post_reserved(pool->owner, job->post);
    }
    return NULL;
}

struct crypto_pool *crypto_pool_create(struct UASYNC *owner, unsigned threads, unsigned pending, size_t bytes, const char *name) {
    if (!owner || threads > 64 || !pending || !bytes || !name) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid pool configuration: threads=%u pending=%u bytes=%zu", threads, pending, bytes);
        return NULL;
    }
    struct crypto_pool *pool = u_calloc(1, sizeof(*pool));
    if (!pool) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "pool allocation failed"); return NULL; }
    pool->owner = owner; pool->limit = pending; pool->byte_limit = bytes;
    snprintf(pool->name, sizeof(pool->name), "%s", name);
    int err = pthread_mutex_init(&pool->mutex, NULL);
    if (err) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "pool mutex failed: %d", err); u_free(pool); return NULL; }
    err = pthread_cond_init(&pool->ready, NULL);
    if (err) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "pool condition failed: %d", err);
        pthread_mutex_destroy(&pool->mutex); u_free(pool); return NULL;
    }
    for (unsigned i = 0; i < threads; ++i) {
        err = pthread_create(&pool->threads[i], NULL, worker, pool);
        if (err) {
            DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "pool %s worker=%u creation failed: %d", pool->name, i, err);
            crypto_pool_destroy(pool); return NULL;
        }
        pool->thread_count++;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "pool %s started: workers=%u pending_limit=%u bytes_limit=%zu", name, threads, pending, bytes);
    return pool;
}

int crypto_pool_submit(struct crypto_pool *pool, const struct crypto_task *task) {
    if (!pool || !task || !task->work || !task->done || pool->stopping) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid/stopped crypto submission"); return -1;
    }
    if (pool->pending >= pool->limit || task->bytes > pool->byte_limit - pool->bytes) {
        DEBUG_TRACE(DEBUG_CATEGORY_CRYPTO, "pool %s busy: pending=%u bytes=%zu request=%zu", pool->name, pool->pending, pool->bytes, task->bytes);
        return -2;
    }
    if (!pool->thread_count) {
        uint64_t start = get_time_us();
        task->work(task->arg);
        uint64_t elapsed = get_time_us() - start;
        pool->work_us += elapsed;
        if (elapsed > pool->work_max_us) pool->work_max_us = elapsed;
        pool->delivering++; task->done(task->arg); pool->delivering--;
        pool->completed++; pool->delivered++;
        return 0;
    }
    struct pool_job *job = u_calloc(1, sizeof(*job));
    struct posted_task *post = u_malloc(sizeof(*post));
    if (!job || !post) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "pool %s submission allocation failed", pool->name);
        u_free(job); u_free(post); return -1;
    }
    job->pool = pool; job->task = *task; job->post = post; job->queued_us = get_time_us();
    post->callback = complete; post->arg = job; post->next = NULL;
    job->all_next = pool->all;
    if (pool->all) pool->all->all_prev = job;
    pool->all = job; pool->pending++; pool->bytes += task->bytes;
    if (pool->pending > pool->pending_peak) pool->pending_peak = pool->pending;
    pthread_mutex_lock(&pool->mutex);
    if (pool->tail) pool->tail->next = job;
    else pool->head = job;
    pool->tail = job;
    pthread_cond_signal(&pool->ready);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

unsigned crypto_pool_pending(const struct crypto_pool *pool) { return pool ? pool->pending : 0; }

void crypto_pool_destroy(struct crypto_pool *pool) {
    if (!pool) return;
    if (pool->delivering) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "pool destroy from done callback rejected"); return; }
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->ready);
    pthread_mutex_unlock(&pool->mutex);
    for (unsigned i = 0; i < pool->thread_count; ++i) pthread_join(pool->threads[i], NULL);
    while (pool->all) {
        struct pool_job *job = pool->all;
        if (uasync_cancel_post(pool->owner, job->post) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "pool %s lost reserved completion", pool->name);
            return;
        }
        complete(job);
    }
    if (debug_should_output(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_CRYPTO)) report(pool, DEBUG_LEVEL_INFO);
    pthread_cond_destroy(&pool->ready); pthread_mutex_destroy(&pool->mutex);
    u_free(pool);
}
