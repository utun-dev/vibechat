#include "worker.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <pthread.h>

struct mf_work {
    struct mf_work* next;
    struct mf_work* queued_next;
    struct mf_worker* owner;
    struct posted_task* completion;
    mf_work_fn work;
    mf_done_fn done;
    void* data;
    void* arg;
    int completed, error, cancellable;
};
struct mf_worker {
    struct UASYNC* ua;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct mf_work* jobs;
    struct mf_work* queue;
    struct mf_work* tail;
    unsigned count;
    long creator_pid;
    int stopping;
};

static void work_finish(struct mf_work* j) {
    struct mf_worker* w = j->owner;
    struct mf_work** link = &w->jobs;
    while (*link && *link != j) link = &(*link)->next;
    if (*link) *link = j->next;
    w->count--;
    DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "member_files: worker callback owner=%p task=%p remaining=%u pid=%ld",
                (void*)w, (void*)j, w->count, (long)getpid());
    mf_done_fn done = j->done; void* arg = j->arg; int error = j->error;
    u_free(j); done(arg, error);
}

static void work_completed(void* arg) {
    struct mf_work* j = arg;
    pthread_mutex_lock(&j->owner->mutex);
    j->completion = NULL;
    pthread_mutex_unlock(&j->owner->mutex);
    work_finish(j);
}

static void* disk_thread(void* arg) {
    struct mf_worker* w = arg;
    pthread_mutex_lock(&w->mutex);
    for (;;) {
        while (!w->queue && !w->stopping) pthread_cond_wait(&w->condition, &w->mutex);
        if (!w->queue && w->stopping) break;
        struct mf_work* j = w->queue; w->queue = j->queued_next;
        if (!w->queue) w->tail = NULL;
        pthread_mutex_unlock(&w->mutex);
        DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "member_files: worker execute owner=%p task=%p pid=%ld", (void*)w, (void*)j, (long)getpid());
        j->work(j->data);
        pthread_mutex_lock(&w->mutex);
        j->completed = 1;
        DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "member_files: worker posting owner=%p task=%p pid=%ld", (void*)w, (void*)j, (long)getpid());
        uasync_post_reserved(w->ua, j->completion);
        pthread_cond_broadcast(&w->condition);
    }
    pthread_mutex_unlock(&w->mutex);
    return NULL;
}

struct mf_worker* mf_worker_create(struct UASYNC* ua) {
    struct mf_worker* w = u_calloc(1, sizeof(*w));
    if (!w) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: disk worker allocation failed"); return NULL; }
    w->ua = ua; w->creator_pid = (long)getpid();
    int rc = pthread_mutex_init(&w->mutex, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: worker mutex failed rc=%d", rc); u_free(w); return NULL; }
    rc = pthread_cond_init(&w->condition, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: worker condition failed rc=%d", rc);
        pthread_mutex_destroy(&w->mutex); u_free(w); return NULL;
    }
    rc = pthread_create(&w->thread, NULL, disk_thread, w);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: disk thread failed rc=%d", rc);
        pthread_cond_destroy(&w->condition); pthread_mutex_destroy(&w->mutex); u_free(w); return NULL;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: worker created owner=%p pid=%ld ua=%p limit=64",
                (void*)w, w->creator_pid, (void*)ua);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: disk thread started queue_limit=64"); return w;
}

struct mf_work* mf_worker_reserve(void) {
    struct mf_work* j = u_calloc(1, sizeof(*j));
    if (j) j->completion = u_calloc(1, sizeof(*j->completion));
    if (!j || !j->completion) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: work/completion reservation failed"); u_free(j); return NULL; }
    j->completion->callback = work_completed; j->completion->arg = j;
    return j;
}

void mf_worker_unreserve(struct mf_work* j) { if (j) { u_free(j->completion); u_free(j); } }

void mf_worker_submit_reserved(struct mf_worker* w, struct mf_work* j,
                               mf_work_fn work, void* data, mf_done_fn done, void* arg) {
    if (w->creator_pid != (long)getpid()) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA,
        "member_files: worker inherited after fork creator_pid=%ld current_pid=%ld; disk thread is missing",
        w->creator_pid, (long)getpid());
    j->owner = w; j->work = work; j->data = data; j->done = done; j->arg = arg;
    j->next = w->jobs; w->jobs = j; w->count++;
    DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "member_files: worker queued owner=%p task=%p current_pid=%ld creator_pid=%ld active=%u",
                (void*)w, (void*)j, (long)getpid(), w->creator_pid, w->count);
    pthread_mutex_lock(&w->mutex);
    if (w->tail) w->tail->queued_next = j;
    else w->queue = j;
    w->tail = j; pthread_cond_signal(&w->condition);
    pthread_mutex_unlock(&w->mutex);
}

void mf_worker_submit(struct mf_worker* w, mf_work_fn work, void* data, mf_done_fn done, void* arg) {
    struct mf_work* j = w->count < 64 ? mf_worker_reserve() : NULL;
    if (!j) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: disk queue rejected active=%u", w->count); done(arg, -1); return; }
    j->cancellable = 1;
    mf_worker_submit_reserved(w, j, work, data, done, arg);
}

/* Снятая работа сохраняет jobs/completion до done; callbacks здесь не запускаются. */
unsigned mf_worker_cancel(struct mf_worker* w, void* data) {
    unsigned cancelled = 0;
    pthread_mutex_lock(&w->mutex);
    struct mf_work** link = &w->queue;
    struct mf_work* previous = NULL;
    while (*link) {
        struct mf_work* j = *link;
        if (j->cancellable && j->data == data) {
            *link = j->queued_next;
            if (w->tail == j) w->tail = previous;
            j->queued_next = NULL; j->error = MF_WORK_CANCELLED; j->completed = 1;
            uasync_post_reserved(w->ua, j->completion);
            cancelled++;
        } else { previous = j; link = &j->queued_next; }
    }
    if (cancelled) pthread_cond_broadcast(&w->condition);
    pthread_mutex_unlock(&w->mutex);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: worker cancel owner=%p data=%p queued=%u", (void*)w, data, cancelled);
    return cancelled;
}

void mf_worker_drain(struct mf_worker* w) {
    if (!w) return;
    /* Не запускаем loop рекурсивно: завершаем только собственные callbacks. */
    while (w->jobs) {
        struct mf_work* j = w->jobs;
        pthread_mutex_lock(&w->mutex);
        while (!j->completed) pthread_cond_wait(&w->condition, &w->mutex);
        pthread_mutex_unlock(&w->mutex);
        uasync_cancel_post(w->ua, j->completion); j->completion = NULL;
        work_finish(j);
    }
}

void mf_worker_destroy(struct mf_worker* w) {
    if (!w) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: worker stop owner=%p current_pid=%ld creator_pid=%ld active=%u",
                (void*)w, (long)getpid(), w->creator_pid, w->count);
    mf_worker_drain(w);
    pthread_mutex_lock(&w->mutex); w->stopping = 1; pthread_cond_signal(&w->condition); pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, NULL);
    pthread_cond_destroy(&w->condition); pthread_mutex_destroy(&w->mutex);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: disk thread stopped"); u_free(w);
}
