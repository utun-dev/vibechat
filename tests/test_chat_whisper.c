/* Настоящие media_async/uasync/Opus;управляемый backend проверяет retry,busy,изоляцию и освобождение после join. */
#define _GNU_SOURCE
#include "../src/chat/chat_whisper.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_setting.h"
#include "../src/utun_instance.h"
#include "../src/media_async/media_async.h"
#include "../src/media_async/voice_file.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include <whisper.h>
#include <assert.h>
#include <pthread.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

struct whisper_context { unsigned id; };
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned loads, frees, alive, entered;
static int fail_load, block, fail_full, sample_count, no_segments;
static uint8_t reply[32] = {42};
static unsigned callbacks;
static const char* fail_site;
static unsigned fail_bytes;
static char directory[512], wav[600], opus[600];
static struct UASYNC* ua;

extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* site) {
    if (fail_site && strstr(site, fail_site)) {
        fail_site = NULL; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "whisper test: allocation refused at %s", site); return NULL;
    }
    return __real_u_calloc_impl(count, bytes, site);
}
extern void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* site) {
    if (fail_bytes == bytes && strstr(site, "chat_whisper.c")) {
        fail_bytes = 0; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "whisper test: text allocation refused at %s", site); return NULL;
    }
    return __real_u_malloc_impl(bytes, site);
}

struct whisper_context_params whisper_context_default_params(void) { return (struct whisper_context_params){0}; }
struct whisper_full_params whisper_full_default_params(int strategy) { (void)strategy; return (struct whisper_full_params){0}; }
struct whisper_context* whisper_init_from_file_with_params(const char* path, struct whisper_context_params params) {
    assert(!params.use_gpu && *path);
    pthread_mutex_lock(&mutex); loads++;
    if (fail_load) { fail_load--; pthread_mutex_unlock(&mutex); return NULL; }
    struct whisper_context* ctx = u_calloc(1, sizeof(*ctx)); assert(ctx); ctx->id = loads; alive++;
    pthread_mutex_unlock(&mutex); return ctx;
}
void whisper_free(struct whisper_context* ctx) {
    assert(ctx); pthread_mutex_lock(&mutex); assert(alive); alive--; frees++; pthread_mutex_unlock(&mutex); u_free(ctx);
}
int whisper_full(struct whisper_context* ctx, struct whisper_full_params params, const float* samples, int count) {
    assert(ctx && !strcmp(params.language, "ru") && count > 0);
    for (int i = 0; i < count; i++) {
        assert(isfinite(samples[i]) && fabsf(samples[i]) <= 1.0f);
        if (count == 4000) assert(fabsf(samples[i] - .25f) < .0001f);
    }
    pthread_mutex_lock(&mutex); sample_count = count; entered++; pthread_cond_broadcast(&changed);
    while (block) pthread_cond_wait(&changed, &mutex);
    int rc = fail_full; pthread_mutex_unlock(&mutex); return rc;
}
int whisper_full_n_segments(struct whisper_context* ctx) { assert(ctx); return no_segments ? 0 : 2; }
const char* whisper_full_get_segment_text(struct whisper_context* ctx, int i) { assert(ctx); return i ? " world" : "hello"; }

struct result { unsigned count; int error; };
static void completed(void* arg, const char* channel, const char* text, int error, const uint8_t id[32]) {
    struct result* result = arg;
    assert(channel && id && !memcmp(id, reply, 32));
    assert(error ? !text : text && !strcmp(text, no_segments ? "" : "hello world"));
    result->count++; result->error = error; callbacks++;
}
static void await(struct result* result, unsigned count) {
    for (int i = 0; result->count < count && i < 1000; i++) uasync_poll(ua, 10);
    assert(result->count == count);
}
static void submit(struct UTUN_INSTANCE* inst, const char* file, struct result* result) {
    chat_whisper_transcribe(inst, file, "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951", reply, completed, result);
}
static void instance(struct UTUN_INSTANCE* inst, struct chat_core_ctx* cc) {
    memset(inst, 0, sizeof(*inst)); memset(cc, 0, sizeof(*cc));
    inst->chat_core = cc; cc->inst = inst; cc->initialized = 1; inst->chat_started = 1; inst->ua = ua;
    inst->media_async = media_async_create(); assert(inst->media_async);
    assert(!chat_setting_set(inst, "whisper_enabled", "1"));
    assert(!chat_setting_set(inst, "whisper_model_path", "test-model"));
}
static void stop(struct UTUN_INSTANCE* inst) {
    inst->chat_started = 0; chat_whisper_destroy(inst);
    media_async_destroy(inst->media_async); inst->media_async = NULL;
    assert(!inst->chat_core->whisper);
}
static void fixture(void) {
    snprintf(directory, sizeof(directory), "/tmp/utun-whisper-XXXXXX"); assert(!test_mkdtemp(directory));
    snprintf(wav, sizeof(wav), "%s/input.wav", directory); snprintf(opus, sizeof(opus), "%s/input.opus", directory);
    FILE* file = fopen(wav, "wb"); assert(file);
    uint8_t header[44] = {'R','I','F','F',0xa4,0x3e,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
        0x80,0x3e,0,0,0,0xfa,0,0,4,0,16,0,'d','a','t','a',0x80,0x3e,0,0};
    /* 4000 стереокадров 16kHz:0.25сек,после downmix постоянный0.25. */
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    for (int i = 0; i < 4000; i++) { const uint8_t frame[] = {0,0x10,0,0x30}; assert(fwrite(frame, 1, 4, file) == 4); }
    assert(!fclose(file));
    int16_t* samples = u_malloc(48000 * sizeof(*samples)); assert(samples);
    for (int i = 0; i < 24000; i++) { samples[i * 2] = (int16_t)(sin(i * .03) * 1000); samples[i * 2 + 1] = samples[i * 2]; }
    struct attachment_info info = {0}; assert(!voice_file_encode(samples, 48000, 48000, 2, 1, opus, &info));
    u_free(samples);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_DEBUG);
    size_t baseline = u_get_allocated_count(); ua = uasync_create(); assert(ua); fixture();
    struct UTUN_INSTANCE a, b; struct chat_core_ctx ca, cb; struct result one = {0}, two = {0}, busy = {0};
    instance(&a, &ca); instance(&b, &cb);
    fail_load = 1; submit(&a, wav, &one); await(&one, 1); assert(one.error == -1 && !alive);
    submit(&a, wav, &one); await(&one, 2); assert(!one.error && sample_count == 4000 && alive == 1 && loads == 2);
    submit(&a, opus, &one); await(&one, 3); assert(!one.error && sample_count == 8000 && loads == 2);
    assert(!chat_setting_set(&a, "whisper_model_path", "replacement"));
    fail_load = 1; submit(&a, wav, &one); await(&one, 4); assert(one.error == -1 && alive == 1);
    submit(&a, wav, &one); await(&one, 5); assert(!one.error && alive == 1 && loads == 4 && frees == 1);
    block = 1; unsigned previous = entered; submit(&a, wav, &one); submit(&a, opus, &busy);
    assert(busy.count == 1 && busy.error == CHAT_WHISPER_BUSY);
    submit(&b, opus, &two);
    pthread_mutex_lock(&mutex);
    struct timespec deadline; assert(!clock_gettime(CLOCK_REALTIME, &deadline)); deadline.tv_sec += 10;
    while (entered < previous + 2) assert(pthread_cond_timedwait(&changed, &mutex, &deadline) != ETIMEDOUT);
    assert(alive == 2); block = 0; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&mutex);
    await(&one, 6); await(&two, 1); assert(!one.error && !two.error);
    no_segments = 1; submit(&a, wav, &one); await(&one, 7); assert(!one.error); no_segments = 0;
    fail_full = 1; submit(&a, wav, &one); await(&one, 8); assert(one.error == -1); fail_full = 0;
    /* Синхронный отказ media_async освобождает job/input и снимает busy для повтора. */
    fail_site = "media_async.c"; submit(&a, wav, &one); assert(!fail_site && one.count == 9 && one.error == -1);
    /* Отсоединение busy-владельца не освобождает модель до join и даёт CANCELLED. */
    previous = callbacks; submit(&a, wav, &one); a.chat_started = 0; chat_whisper_destroy(&a);
    assert(!ca.whisper && callbacks == previous);
    media_async_destroy(a.media_async); a.media_async = NULL; assert(one.count == 10 && one.error == MEDIA_ASYNC_CANCELLED);
    stop(&b); assert(!alive && frees == loads - 2);
    /* Новый сервис не наследует ready/closing старого владельца. */
    a.chat_started = 1; a.media_async = media_async_create(); submit(&a, opus, &one); await(&one, 11); assert(!one.error); stop(&a);
    instance(&a, &ca); struct result extra = {0};
    fail_site = "chat_whisper.c"; submit(&a, wav, &extra); assert(!fail_site && extra.count == 1 && extra.error == -1);
    fail_bytes = 12; submit(&a, wav, &extra); await(&extra, 2); assert(!fail_bytes && extra.error == -1);
    submit(&a, wav, &extra); await(&extra, 3); assert(!extra.error);
    /* Штатный writer и reader согласованы для всех поддерживаемых частот/каналов. */
    const int rates[] = {8000,12000,16000,24000,48000};
    for (unsigned r = 0; r < 5; r++) for (int channels = 1; channels <= 2; channels++) {
        size_t count = (size_t)rates[r] / 2 * channels;
        int16_t* samples = u_calloc((uint32_t)count, sizeof(*samples)); assert(samples);
        struct attachment_info info = {0}; assert(!voice_file_encode(samples, count, rates[r], channels, 1, opus, &info));
        u_free(samples); unsigned expected = extra.count + 1; submit(&a, opus, &extra); await(&extra, expected);
        assert(!extra.error && sample_count == 8000);
    }
    stop(&a);
    assert(!remove(wav) && !remove(opus) && !rmdir(directory));
    uasync_poll(ua, 0); uasync_destroy(ua, 0); assert(!alive && u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "PASS whisper:retry/cache/replace,busy,two instances,empty/error,cancel/restart,PCM/WAV/real Opus;callbacks=%u", callbacks);
    return 0;
}
