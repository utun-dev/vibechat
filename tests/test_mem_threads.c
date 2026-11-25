/* Конкурентные аллокации, передача владения и снимки при живых писателях. */
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct block { unsigned length, marker; uint8_t bytes[]; };
struct shared {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct block* queue[32];
    unsigned head, count, producers, transferred;
    int started;
};
static struct shared state = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {0}, 0, 0, 2, 0, 0};
static void start(void) {
    pthread_mutex_lock(&state.mutex);
    while (!state.started) pthread_cond_wait(&state.condition, &state.mutex);
    pthread_mutex_unlock(&state.mutex);
}
static void* produce(void* arg) {
    start(); unsigned marker = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < 8000; i++) {
        unsigned n = 1 + i % 257;
        struct block* p = u_calloc(1, sizeof(*p) + n); assert(p);
        p->length = n; p->marker = marker; memset(p->bytes, (int)marker, n);
        p = u_realloc(p, sizeof(*p) + n + 17); assert(p);
        u_check(p, "producer", LOCATION);
        pthread_mutex_lock(&state.mutex);
        while (state.count == 32) pthread_cond_wait(&state.condition, &state.mutex);
        state.queue[(state.head + state.count) % 32] = p; state.count++;
        pthread_cond_broadcast(&state.condition); pthread_mutex_unlock(&state.mutex);
    }
    pthread_mutex_lock(&state.mutex); state.producers--; pthread_cond_broadcast(&state.condition); pthread_mutex_unlock(&state.mutex);
    return NULL;
}
static void* consume(void* arg) {
    (void)arg; start();
    for (;;) {
        pthread_mutex_lock(&state.mutex);
        while (!state.count && state.producers) pthread_cond_wait(&state.condition, &state.mutex);
        if (!state.count) { pthread_mutex_unlock(&state.mutex); break; }
        struct block* p = state.queue[state.head]; state.head = (state.head + 1) % 32; state.count--; state.transferred++;
        pthread_cond_broadcast(&state.condition); pthread_mutex_unlock(&state.mutex);
        for (unsigned i = 0; i < p->length; i++) assert(p->bytes[i] == p->marker);
        u_check(p, "consumer", LOCATION); u_free(p);
    }
    return NULL;
}
static void* scratch(void* arg) {
    (void)arg; start();
    for (unsigned i = 0; i < 12000; i++) {
        char* p = u_strdup("thread owned block"); assert(p);
        p = u_realloc(p, 80 + i % 31); assert(p && !strcmp(p, "thread owned block"));
        assert(u_get_allocated_count() > 0); u_free(p);
    }
    return NULL;
}
static void* report(void* arg) {
    (void)arg; start();
    for (unsigned i = 0; i < 1000; i++) { (void)u_get_allocated_count(); u_report_unfreed_blocks(); }
    return NULL;
}
static unsigned hook_calls;
static void log_hook(int level, const char* category, const char* message) {
    (void)level; (void)category; (void)message;
    void* p = u_malloc(3); assert(p); u_free(p); hook_calls++;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_NONE); debug_enable_console(0);
    pthread_t threads[7];
    assert(!pthread_create(&threads[0], NULL, produce, (void*)(uintptr_t)11));
    assert(!pthread_create(&threads[1], NULL, produce, (void*)(uintptr_t)17));
    assert(!pthread_create(&threads[2], NULL, consume, NULL));
    assert(!pthread_create(&threads[3], NULL, consume, NULL));
    assert(!pthread_create(&threads[4], NULL, scratch, NULL));
    assert(!pthread_create(&threads[5], NULL, scratch, NULL));
    assert(!pthread_create(&threads[6], NULL, report, NULL));
    pthread_mutex_lock(&state.mutex); state.started = 1; pthread_cond_broadcast(&state.condition); pthread_mutex_unlock(&state.mutex);
    for (unsigned i = 0; i < 7; i++) assert(!pthread_join(threads[i], NULL));
    assert(state.transferred == 16000 && !state.count && !u_get_allocated_count());
    /* Hook аллоцирует: отчёт не имеет права держать alloc_mutex во время DEBUG. */
    void* p = u_malloc(17); assert(p); debug_set_log_hook(log_hook); debug_set_level(DEBUG_LEVEL_ERROR);
    u_report_unfreed_blocks(); debug_set_log_hook(NULL); assert(hook_calls >= 2 && u_get_allocated_count() == 1); u_free(p);
    assert(!u_get_allocated_count());
    pthread_cond_destroy(&state.condition); pthread_mutex_destroy(&state.mutex);
    puts("PASS mem threads: concurrent malloc/calloc/realloc/free/count, 16000 ownership transfers, 1000 live reports, allocating log hook");
    return 0;
}
