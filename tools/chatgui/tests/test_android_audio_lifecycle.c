/* Реальный instance_lite worker и voice; wrappers наблюдают потоки/порядок и отказы attach. */
#include "utun_instance.h"
#include "call/call.h"
#include "call/call_audio.h"
#include "chat/chat_event.h"
#include "db_schema.h"
#include "chat/chat_setting.h"
#include "transport_layer/secure_channel.h"
#include "debug_config.h"
#include "mem.h"
#include "u_async.h"
#include "../../chatgui-android/libutun_lite/instance_lite.h"
#include "../../chatgui-android/jni_bridge/android_jni_bridge.h"
#include "../../chatgui-android/libutun_lite/utun_config_api.h"
#include "../../../tests/test_utils.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <openssl/sha.h>

static pthread_mutex_t observation_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t ui, worker;
static struct UASYNC* worker_loop; /* Цикл instance_lite, без циклов транспортных потоков. */
static unsigned attached, detached, destroyed, loops_destroyed, started, errors, checked;
static unsigned identity_failures;
static unsigned startup_failures;
static char startup_reason[2048];
static char startup_directory[256];
static int startup_can_recreate;
static unsigned activity_checked;
static unsigned phone_checked, phone_applied, phone_post_entered, release_phone_post;
static int pause_poll, pause_phone_post, phone_post_result;
static __thread int phone_producer, fail_phone_alloc;
static int radio_mode, radio_post_result;
static unsigned radio_acked, radio_inspected, radio_post_entered, release_radio_post;
static __thread int radio_producer;
static int64_t radio_request = INT64_C(0x1234567800000000);
static int radio_success = 1;
static int radio_expected[INSTANCE_LITE_RADIO_SETTINGS_COUNT];
static const int radio_valid[] = {1, 2, -20, -60, 400, 65, 20};
static const int radio_defaults[] = {0, 1, -35, -45, 200, 50, 10};
static uint8_t first_node[32], current_node[32];
static struct UTUN_INSTANCE* last_detached;
static int stream_failure, voice_failure;
static const char* lifecycle_string(const char* key, const char* fallback) { return !strcmp(key, "lifecycle") ? "owned" : fallback; }
static int pause_loop, pause_instance, pause_attach;
static unsigned paused, release_worker, stop_requested, stop_entered, stop_done, joins, self_detaches, finished;

#define CHECK_ANDROID(value) do { if (!(value)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Android audio lifecycle line=%d: %s", __LINE__, #value); return 1; \
} } while (0)

static void observe(int condition, const char* message) {
    pthread_mutex_lock(&observation_lock);
    if (!condition) { ++errors; DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Android audio lifecycle: %s", message); }
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&observation_lock);
}

static void pause_worker(int enabled) {
    if (!enabled) return;
    pthread_mutex_lock(&observation_lock);
    ++paused; pthread_cond_broadcast(&changed);
    while (!release_worker) pthread_cond_wait(&changed, &observation_lock);
    pthread_mutex_unlock(&observation_lock);
}

struct UASYNC* __real_uasync_create(void);
struct UASYNC* __wrap_uasync_create(void) {
    pthread_mutex_lock(&observation_lock);
    int main_loop = !worker_loop;
    if (main_loop) worker = pthread_self();
    pthread_mutex_unlock(&observation_lock);
    if (main_loop) pause_worker(pause_loop);
    struct UASYNC* ua = __real_uasync_create();
    if (main_loop) {
        pthread_mutex_lock(&observation_lock); worker_loop = ua; pthread_mutex_unlock(&observation_lock);
    }
    return ua;
}

void __real_uasync_poll(struct UASYNC*, int);
void __wrap_uasync_poll(struct UASYNC* ua, int timeout) {
    pthread_mutex_lock(&observation_lock);
    int pause = timeout < 0 && ua == worker_loop && pause_poll;
    if (pause) pause_poll = 0;
    pthread_mutex_unlock(&observation_lock);
    if (pause) {
        pause_worker(1); // Сервис готов, но очередь ещё не взята в исполняемую пачку.
        return;
    }
    __real_uasync_poll(ua, timeout);
}

void __real_uasync_post_reserved(struct UASYNC*, struct posted_task*);
void __wrap_uasync_post_reserved(struct UASYNC* ua, struct posted_task* task) {
    if (radio_producer && radio_mode == 4) {
        pthread_mutex_lock(&observation_lock);
        ++radio_post_entered; pthread_cond_broadcast(&changed);
        while (!release_radio_post) pthread_cond_wait(&changed, &observation_lock);
        pthread_mutex_unlock(&observation_lock);
    }
    if (phone_producer && pause_phone_post) {
        pthread_mutex_lock(&observation_lock);
        ++phone_post_entered; pthread_cond_broadcast(&changed);
        while (!release_phone_post) pthread_cond_wait(&changed, &observation_lock);
        pthread_mutex_unlock(&observation_lock);
    }
    __real_uasync_post_reserved(ua, task);
}

void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* location) {
    if (fail_phone_alloc) { fail_phone_alloc = 0; return NULL; }
    return __real_u_malloc_impl(size, location);
}

void __real_call_set_phone_state(struct UTUN_INSTANCE*, int, unsigned);
void __wrap_call_set_phone_state(struct UTUN_INSTANCE* inst, int state, unsigned policy) {
    observe(pthread_equal(pthread_self(), worker), "phone state applied outside core worker");
    __real_call_set_phone_state(inst, state, policy);
    pthread_mutex_lock(&observation_lock); ++phone_applied; pthread_mutex_unlock(&observation_lock);
}

struct UTUN_INSTANCE* __real_utun_instance_create_from_config_async(struct UASYNC*, struct utun_config*, utun_instance_ready_fn, void*);
struct UTUN_INSTANCE* __wrap_utun_instance_create_from_config_async(struct UASYNC* ua, struct utun_config* config,
    utun_instance_ready_fn done, void* arg) {
    struct UTUN_INSTANCE* inst = __real_utun_instance_create_from_config_async(ua, config, done, arg);
    pause_worker(pause_instance);
    return inst;
}

int __real_android_stream_attach(struct UTUN_INSTANCE*);
int __wrap_android_stream_attach(struct UTUN_INSTANCE* inst) {
    worker = pthread_self();
    debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_INFO);
    observe(!pthread_equal(worker, ui), "media attach executed on UI thread");
    pause_worker(pause_attach);
    if (stream_failure) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "Android audio lifecycle: injected stream attach failure"); return -1;
    }
    return __real_android_stream_attach(inst);
}

int __real_call_audio_init(struct UTUN_INSTANCE*);
int __wrap_call_audio_init(struct UTUN_INSTANCE* inst) {
    observe(pthread_equal(pthread_self(), worker) && !pthread_equal(pthread_self(), ui), "voice init outside core worker");
    int result = __real_call_audio_init(inst);
    observe(!result && inst->call_audio_ops, "voice registration missing");
    pthread_mutex_lock(&observation_lock); ++attached; pthread_mutex_unlock(&observation_lock);
    if (voice_failure) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "Android audio lifecycle: injected failure after voice registration"); return -1;
    }
    return result;
}

void __real_call_audio_destroy(struct UTUN_INSTANCE*);
void __wrap_call_audio_destroy(struct UTUN_INSTANCE* inst) {
    observe(!pthread_equal(pthread_self(), ui), "voice detach executed on UI thread");
    __real_call_audio_destroy(inst);
    observe(!inst->call_audio_ops, "voice registration retained after detach");
    pthread_mutex_lock(&observation_lock); ++detached; last_detached = inst; pthread_mutex_unlock(&observation_lock);
}

void __real_utun_instance_destroy(struct UTUN_INSTANCE*);
void __wrap_utun_instance_destroy(struct UTUN_INSTANCE* inst) {
    observe(inst == last_detached && !inst->call_audio_ops, "core destroyed before voice detach");
    __real_utun_instance_destroy(inst);
    pthread_mutex_lock(&observation_lock); ++destroyed; pthread_mutex_unlock(&observation_lock);
}

void __real_uasync_destroy(struct UASYNC*, int);
void __wrap_uasync_destroy(struct UASYNC* ua, int flags) {
    pthread_mutex_lock(&observation_lock); int main_loop = ua == worker_loop; pthread_mutex_unlock(&observation_lock);
    if (main_loop) observe(detached >= destroyed, "loop destroyed before core detach/destroy");
    __real_uasync_destroy(ua, flags);
    if (main_loop) {
        pthread_mutex_lock(&observation_lock);
        worker_loop = NULL; ++loops_destroyed; pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&observation_lock);
    }
}

int __real_pthread_detach(pthread_t);
int __wrap_pthread_detach(pthread_t thread) {
    int result = __real_pthread_detach(thread);
    if (pthread_equal(thread, worker)) {
        pthread_mutex_lock(&observation_lock); ++self_detaches; pthread_mutex_unlock(&observation_lock);
    }
    return result;
}

int __real_pthread_join(pthread_t, void**);
int __wrap_pthread_join(pthread_t thread, void** result) {
    int status = __real_pthread_join(thread, result);
    if (pthread_equal(thread, worker)) {
        pthread_mutex_lock(&observation_lock); ++joins; pthread_mutex_unlock(&observation_lock);
    }
    return status;
}

struct UTUN_INSTANCE* __real_instance_lite_get_instance(void);
struct UTUN_INSTANCE* __wrap_instance_lite_get_instance(void) {
    observe(!pthread_equal(pthread_self(), ui), "UI bridge reads raw instance");
    return __real_instance_lite_get_instance();
}

char* __wrap_utun_bridge_get_local_sockets_json(void) { return u_strdup("[]"); }

static void event(int type, const uint8_t* data, int length) {
    if (type == CHAT_EVT_STARTUP_FAILED) {
        observe(!pthread_equal(pthread_self(), ui) && !instance_lite_is_running(), "schema error requires unpublished worker");
        observe(data && length >= 4, "schema error payload invalid");
        if (!data || length < 4) return;
        size_t n = (size_t)data[1] | ((size_t)data[2] << 8);
        observe(data[0] <= 1 && n < sizeof(startup_directory) && n + 3 < (size_t)length, "schema failure directory invalid");
        if (data[0] > 1 || n >= sizeof(startup_directory) || n + 3 >= (size_t)length) return;
        size_t message_size = (size_t)length - n - 3;
        observe(message_size < sizeof(startup_reason), "schema failure reason too long");
        if (message_size >= sizeof(startup_reason)) return;
        pthread_mutex_lock(&observation_lock);
        startup_can_recreate = data[0];
        memcpy(startup_directory, data + 3, n); startup_directory[n] = 0;
        memcpy(startup_reason, data + 3 + n, message_size); startup_reason[message_size] = 0;
        ++startup_failures; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
        return;
    }
    if (type != CHAT_EVT_SERVICE_STARTED) return;
    observe(pthread_equal(pthread_self(), worker), "service ready event outside core worker");
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    pthread_mutex_lock(&observation_lock);
    memcpy(current_node, inst->my_ed25519_pubkey, sizeof(current_node));
    if (!started) memcpy(first_node, current_node, sizeof(first_node));
    ++started; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&observation_lock);
}

static void log_event(int level, const char* category, const char* message) {
    (void)level; (void)category;
    if (strstr(message, "Android worker stop requested")) {
        pthread_mutex_lock(&observation_lock); ++stop_requested; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
    }
    if (strstr(message, "Android worker finished")) {
        worker = pthread_self();
        pthread_mutex_lock(&observation_lock); ++finished; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
    }
    if (!strstr(message, "Android identity save failed") && !strstr(message, "Android identity key pair is invalid")) return;
    pthread_mutex_lock(&observation_lock); ++identity_failures; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
}

static void* stop_worker(void* arg) {
    (void)arg;
    pthread_mutex_lock(&observation_lock); ++stop_entered; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
    instance_lite_stop();
    pthread_mutex_lock(&observation_lock); ++stop_done; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
    return NULL;
}

static void* post_phone_state(void* arg) {
    (void)arg;
    phone_producer = 1;
    phone_post_result = utun_bridge_set_phone_state(2, 31);
    return NULL;
}

static int wait_for(const unsigned* counter, unsigned expected) {
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 10;
    pthread_mutex_lock(&observation_lock);
    int result = 0;
    while (*counter < expected && !result) result = pthread_cond_timedwait(&changed, &observation_lock, &deadline);
    int ready = *counter >= expected;
    pthread_mutex_unlock(&observation_lock);
    if (!ready) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Android audio lifecycle timed out expected=%u", expected);
    return ready;
}

static void inspect_radio(void* arg) {
    (void)arg;
    static const char* keys[] = {"radio_auto_enabled", "radio_auto_mode", "radio_level_on_dbfs", "radio_level_off_dbfs",
                               "radio_auto_hangover_ms", "radio_vad_threshold", "radio_vad_compressor_max_gain_db"};
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    observe(inst && pthread_equal(pthread_self(), worker), "radio settings inspected outside core worker");
    if (inst) for (unsigned i = 0; i < INSTANCE_LITE_RADIO_SETTINGS_COUNT; ++i)
        observe(chat_setting_get_int(inst, keys[i], radio_defaults[i]) == radio_expected[i], "radio snapshot partially changed or misordered");
    pthread_mutex_lock(&observation_lock); ++radio_inspected; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
}

static void radio_configured(int64_t request, int success) {
    observe(request == radio_request && success == radio_success, "radio settings acknowledgement differs from request/result");
    inspect_radio(NULL);
    pthread_mutex_lock(&observation_lock); ++radio_acked; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
}

static void* post_radio_config(void* arg) {
    (void)arg;
    radio_producer = 1;
    radio_post_result = instance_lite_configure_radio(radio_request, radio_valid, radio_configured);
    return NULL;
}

static int submit_radio(const int* values, int success) {
    unsigned target = radio_acked + 1;
    ++radio_request; radio_success = success;
    if (success) memcpy(radio_expected, values, sizeof(radio_expected));
    CHECK_ANDROID(!instance_lite_configure_radio(radio_request, values, radio_configured));
    CHECK_ANDROID(wait_for(&radio_acked, target));
    return 0;
}

static void inspect(void* arg) {
    (void)arg;
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    observe(inst && inst->call_audio_ops && inst->call_audio_ops->on_media, "ready core without audio provider");
    observe(instance_lite_set_active(1) == -1, "activity posting accepted from native callback");
    observe(utun_bridge_set_phone_state(1, 31) == -1, "phone state posting accepted from native callback");
    observe(instance_lite_configure_radio(42, radio_valid, radio_configured) == -1, "radio config posting accepted from native callback");
    observe(!strcmp(cfg_get_string("lifecycle", "missing"), "owned"), "config provider unavailable before callback stop");
    observe(utun_bridge_destroy() == -1 && instance_lite_is_running(), "bridge destroyed resources from core callback");
    observe(!strcmp(cfg_get_string("lifecycle", "missing"), "owned"), "failed stop cleared config callbacks");
    uint8_t public_key[SC_PUBKEY_SIZE], ed_public[SC_PUBKEY_SIZE], private_x[32];
    observe(sc_compute_public_key_from_private(inst->my_keys.private_key, public_key) == SC_OK &&
        !memcmp(public_key, inst->my_keys.public_key, sizeof(public_key)) &&
        inst->node_id == sc_derive_node_id_from_pubkey(public_key),
        "X25519/node_id identity mismatch");
    observe(sc_identity_from_seed(inst->my_ed25519_privkey, public_key, private_x, ed_public) == SC_OK &&
        !memcmp(ed_public, inst->my_ed25519_pubkey, 32) && !memcmp(private_x, inst->my_keys.private_key, 32),
        "device signing seed and transport key disagree");
    pthread_mutex_lock(&observation_lock); ++checked; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
}

static void inspect_activity(void* arg) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    int expected = (int)(intptr_t)arg;
    observe(inst && inst->client_activity_requested == expected && inst->client_activity == expected,
            "activity command did not run before subsequent worker callback");
    pthread_mutex_lock(&observation_lock); ++activity_checked; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
}

struct phone_expectation { int state; unsigned policy; };
static void inspect_phone(void* arg) {
    const struct phone_expectation* expected = arg;
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    observe(inst && inst->phone_state == expected->state && inst->incoming_call_policy == expected->policy,
            "phone state/policy differs from queued request");
    pthread_mutex_lock(&observation_lock); ++phone_checked; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
}

/* Держим готовый worker перед poll: stop/restart должны отменить ожидающие команды без их применения и утечки. */
static int check_phone_shutdown(const char* config, int restart, int publication, size_t allocated) {
    CHECK_ANDROID(wait_for(&started, 1) && wait_for(&paused, 1));
    CHECK_ANDROID(utun_bridge_set_phone_state(-1, 0) == -1 && utun_bridge_set_phone_state(3, 0) == -1);
    CHECK_ANDROID(utun_bridge_set_phone_state(0, 32) == -1 && utun_bridge_set_phone_state(0, (unsigned)-1) == -1);
    pthread_t stopper, poster;
    unsigned stopped_before_publication = 0;
    if (publication) {
        CHECK_ANDROID(!pthread_create(&poster, NULL, post_phone_state, NULL) && wait_for(&phone_post_entered, 1));
        CHECK_ANDROID(!pthread_create(&stopper, NULL, stop_worker, NULL) && wait_for(&stop_entered, 1));
        struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_nsec += 100000000;
        if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
        pthread_mutex_lock(&observation_lock);
        while (!stop_requested) if (pthread_cond_timedwait(&changed, &observation_lock, &deadline)) break;
        stopped_before_publication = stop_requested;
        release_phone_post = 1; pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&observation_lock);
        CHECK_ANDROID(!pthread_join(poster, NULL) && !phone_post_result && wait_for(&stop_requested, 1));
    } else {
        for (unsigned i = 0; i < 16; ++i) CHECK_ANDROID(!utun_bridge_set_phone_state(i % 3, i));
        if (restart) instance_lite_restart(config);
        else CHECK_ANDROID(!pthread_create(&stopper, NULL, stop_worker, NULL) && wait_for(&stop_requested, 1));
    }
    pthread_mutex_lock(&observation_lock); release_worker = 1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
    if (restart) {
        CHECK_ANDROID(wait_for(&started, 2) && !phone_applied);
        struct phone_expectation expected = { .state = 0, .policy = CALL_POLICY_PUBLISH_PHONE };
        uasync_post(instance_lite_get_uasync(), inspect_phone, &expected);
        CHECK_ANDROID(wait_for(&phone_checked, 1));
        fail_phone_alloc = 1;
        CHECK_ANDROID(utun_bridge_set_phone_state(2, 31) == -1 && !fail_phone_alloc);
        for (unsigned i = 0; i < 6; ++i) {
            expected.state = i % 3; expected.policy = i < 3 ? 31 : 1u << (i - 3);
            CHECK_ANDROID(!utun_bridge_set_phone_state(expected.state, expected.policy));
            uasync_post(instance_lite_get_uasync(), inspect_phone, &expected);
            CHECK_ANDROID(wait_for(&phone_checked, i + 2));
        }
        CHECK_ANDROID(phone_applied == 6);
        instance_lite_stop();
    } else CHECK_ANDROID(!pthread_join(stopper, NULL) && !phone_applied);
    CHECK_ANDROID(utun_bridge_set_phone_state(2, 31) == -1 && !instance_lite_is_running());
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "Android phone shutdown restart=%d publication=%d early_stop=%u applied=%u errors=%u allocations=%zu->%zu",
        restart, publication, stopped_before_publication, phone_applied, errors, allocated, u_get_allocated_count());
    CHECK_ANDROID(!stopped_before_publication && !errors && u_get_allocated_count() == allocated);
    return 0;
}

/* Пакет проверок одной транзакции: копия входа, атомарный отказ, отмена и удержание loop во время публикации. */
static int check_radio_config(const char* config, size_t allocated) {
    CHECK_ANDROID(wait_for(&started, 1) && wait_for(&paused, 1));
    CHECK_ANDROID(instance_lite_configure_radio(42, NULL, radio_configured) == -1);
    CHECK_ANDROID(instance_lite_configure_radio(42, radio_valid, NULL) == -1);
    memcpy(radio_expected, radio_valid, sizeof(radio_expected));
    pthread_t stopper, poster;
    unsigned early_stop = 0;
    if (radio_mode == 1) {
        int input[INSTANCE_LITE_RADIO_SETTINGS_COUNT]; memcpy(input, radio_valid, sizeof(input));
        CHECK_ANDROID(!instance_lite_configure_radio(radio_request, input, radio_configured));
        memset(input, 0, sizeof(input)); // Задача обязана владеть копией до начала poll.
    } else if (radio_mode == 4) {
        CHECK_ANDROID(!pthread_create(&poster, NULL, post_radio_config, NULL) && wait_for(&radio_post_entered, 1));
        CHECK_ANDROID(!pthread_create(&stopper, NULL, stop_worker, NULL) && wait_for(&stop_entered, 1));
        struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_nsec += 100000000;
        if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
        pthread_mutex_lock(&observation_lock);
        while (!stop_requested) if (pthread_cond_timedwait(&changed, &observation_lock, &deadline)) break;
        early_stop = stop_requested;
        release_radio_post = 1; pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&observation_lock);
        CHECK_ANDROID(!pthread_join(poster, NULL) && !radio_post_result && wait_for(&stop_requested, 1));
    } else {
        for (unsigned i = 0; i < 16; ++i)
            CHECK_ANDROID(!instance_lite_configure_radio(radio_request, radio_valid, radio_configured));
        if (radio_mode == 3) instance_lite_restart(config);
        else CHECK_ANDROID(!pthread_create(&stopper, NULL, stop_worker, NULL) && wait_for(&stop_requested, 1));
    }
    pthread_mutex_lock(&observation_lock); release_worker = 1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
    if (radio_mode == 1) {
        CHECK_ANDROID(wait_for(&radio_acked, 1));
        const int limits[][2] = {{-1, 2}, {0, 4}, {-91, 1}, {-91, 1}, {49, 2001}, {9, 96}, {0, 61}};
        for (unsigned field = 0; field < INSTANCE_LITE_RADIO_SETTINGS_COUNT; ++field) for (unsigned bound = 0; bound < 2; ++bound) {
            int invalid[] = {0, 1, -30, -70, 600, 75, 30}; invalid[field] = limits[field][bound];
            CHECK_ANDROID(!submit_radio(invalid, 0));
        }
        int invalid_pair[] = {0, 1, -30, -30, 600, 75, 30};
        CHECK_ANDROID(!submit_radio(invalid_pair, 0));
        invalid_pair[3] = -20;
        CHECK_ANDROID(!submit_radio(invalid_pair, 0));
        const int extremes[] = {0, 1, 0, -90, 2000, 95, 60};
        CHECK_ANDROID(!submit_radio(extremes, 1));
        fail_phone_alloc = 1;
        CHECK_ANDROID(instance_lite_configure_radio(42, radio_valid, radio_configured) == -1 && !fail_phone_alloc);
        CHECK_ANDROID(!submit_radio(radio_valid, 1));
        uasync_post(instance_lite_get_uasync(), inspect, NULL);
        CHECK_ANDROID(wait_for(&checked, 1));
        instance_lite_stop();
    } else if (radio_mode == 3) {
        CHECK_ANDROID(wait_for(&started, 2) && !radio_acked);
        memcpy(radio_expected, radio_defaults, sizeof(radio_expected));
        uasync_post(instance_lite_get_uasync(), inspect_radio, NULL);
        CHECK_ANDROID(wait_for(&radio_inspected, 1));
        CHECK_ANDROID(!submit_radio(radio_valid, 1));
        instance_lite_stop();
    } else CHECK_ANDROID(!pthread_join(stopper, NULL) && !radio_acked);
    CHECK_ANDROID(instance_lite_configure_radio(42, radio_valid, radio_configured) == -1);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "Android radio config mode=%d early_stop=%u ack=%u errors=%u allocations=%zu->%zu",
        radio_mode, early_stop, radio_acked, errors, allocated, u_get_allocated_count());
    CHECK_ANDROID(!early_stop && !errors && u_get_allocated_count() == allocated);
    return 0;
}

static void stop_core(void* arg) {
    (void)arg;
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    observe(inst && inst->running && inst->call_audio_ops, "core stop without live voice owner");
    utun_instance_stop(inst);
}

int main(int argc, char** argv) {
    CHECK_ANDROID(argc == 2);
    int core_stop = !strcmp(argv[1], "core_stop") || !strcmp(argv[1], "restart_core_stop");
    int restart = !strcmp(argv[1], "restart") || !strcmp(argv[1], "restart_core_stop"), normal = !strcmp(argv[1], "normal");
    stream_failure = !strcmp(argv[1], "stream_failure"); voice_failure = !strcmp(argv[1], "voice_failure");
    int cancel_restart = !strcmp(argv[1], "cancel_restart");
    int slow = !strcmp(argv[1], "cancel_slow_attach");
    int parse_failure = !strcmp(argv[1], "parse_failure");
    int schema_failure = !strcmp(argv[1], "schema_failure");
    int phone_restart = !strcmp(argv[1], "phone_restart"), phone_publication = !strcmp(argv[1], "phone_publication_stop");
    int phone = phone_restart || phone_publication || !strcmp(argv[1], "phone_stop");
    radio_mode = !strcmp(argv[1], "radio_config") ? 1 : !strcmp(argv[1], "radio_config_stop") ? 2 :
        !strcmp(argv[1], "radio_config_restart") ? 3 : !strcmp(argv[1], "radio_config_publication_stop") ? 4 : 0;
    pause_poll = phone || radio_mode; pause_phone_post = phone_publication;
    pause_loop = !strcmp(argv[1], "cancel_loop_create");
    pause_instance = !strcmp(argv[1], "cancel_instance_create");
    pause_attach = !strcmp(argv[1], "cancel_attach") || slow;
    int cancel = pause_loop || pause_instance || pause_attach || cancel_restart;
    CHECK_ANDROID(restart || normal || core_stop || stream_failure || voice_failure || cancel || parse_failure || schema_failure ||
        phone || radio_mode);
    ui = pthread_self(); worker = ui;
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0);
    size_t allocated = u_get_allocated_count();
    utun_config_provider_t provider = { .get_string = lifecycle_string };
    utun_config_set_provider(&provider);
    CHECK_ANDROID(utun_bridge_call_audio_start(42) == -1);
    char directory[512], config[1200], public_key[65], private_key[65];
    const char* temporary = getenv("TMPDIR");
    int length = snprintf(directory, sizeof(directory), "%s/utun_android_audio_XXXXXX", temporary ? temporary : "/tmp");
    CHECK_ANDROID(length > 0 && length < (int)sizeof(directory));
    CHECK_ANDROID(!test_mkdtemp(directory));
    struct SC_MYKEYS keys;
    uint8_t seed[32]; CHECK_ANDROID(sc_identity_generate_config(keys.public_key, seed) == SC_OK);
    for (unsigned i = 0; i < SC_PUBKEY_SIZE; ++i) {
        snprintf(public_key + i * 2, 3, "%02x", keys.public_key[i]);
        snprintf(private_key + i * 2, 3, "%02x", seed[i]);
    }
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\nauto_sockets=no\nmy_public_key=%s\nmy_private_key=%s\n"
        "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[chatserver]\nstorage_autoload=0\n", directory, public_key, private_key);
    instance_lite_set_event_handler(event);
    debug_set_log_hook(log_event);
    if (schema_failure) {
        char path[1024]; snprintf(path, sizeof(path), "%s/chats.db", directory);
        sqlite3* db = NULL; CHECK_ANDROID(sqlite3_open(path, &db) == SQLITE_OK);
        CHECK_ANDROID(sqlite3_exec(db, "CREATE TABLE peers_42(node_id INTEGER PRIMARY KEY,userinfo TEXT);"
            "INSERT INTO peers_42 VALUES(7,'retained')", NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_int64 size; unsigned char* before = sqlite3_serialize(db, "main", &size, 0); CHECK_ANDROID(before);
        CHECK_ANDROID(sqlite3_close(db) == SQLITE_OK);
        for (unsigned i = 1; i <= 2; i++) {
            CHECK_ANDROID(!instance_lite_start(config) && wait_for(&finished, i) && wait_for(&startup_failures, i));
            CHECK_ANDROID(!instance_lite_stop() && !started && !attached && !errors && !self_detaches);
            CHECK_ANDROID(strstr(startup_reason, path) && strstr(startup_reason, "peers_42"));
            CHECK_ANDROID(startup_can_recreate && !strcmp(startup_directory, directory));
            CHECK_ANDROID(u_get_allocated_count() == allocated);
        }
        CHECK_ANDROID(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
        sqlite3_int64 after_size; unsigned char* after = sqlite3_serialize(db, "main", &after_size, 0);
        CHECK_ANDROID(after && after_size == size && !memcmp(before, after, (size_t)size));
        sqlite3_free(before); sqlite3_free(after); CHECK_ANDROID(sqlite3_close(db) == SQLITE_OK);
        // Выбор «Создать заново»: очистить всю БД и запустить тот же сервис обычным путём.
        CHECK_ANDROID(!utun_bridge_clear_database(directory));
        CHECK_ANDROID(!instance_lite_start(config) && wait_for(&started, 1));
        CHECK_ANDROID(!instance_lite_stop() && !errors && u_get_allocated_count() == allocated);
        CHECK_ANDROID(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && !db_schema_check(db));
        CHECK_ANDROID(sqlite3_close(db) == SQLITE_OK);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "Android schema rejection: exit preserves database; explicit clear and service restart PASS");
        return 0;
    }
    if (parse_failure) {
        for (unsigned i = 1; i <= 2; ++i) {
            CHECK_ANDROID(!instance_lite_start("[global]\nmy_private_key=invalid\n"));
            CHECK_ANDROID(wait_for(&finished, i));
        }
        instance_lite_stop();
        CHECK_ANDROID(joins == 2 && !self_detaches && !started && u_get_allocated_count() == allocated);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "Android worker repeated parse failure: joins=%u detach=%u PASS", joins, self_detaches);
        return 0;
    }
    CHECK_ANDROID(!instance_lite_start(config));
    if (phone) return check_phone_shutdown(config, phone_restart, phone_publication, allocated);
    if (radio_mode) return check_radio_config(config, allocated);
    if (cancel) {
        if (cancel_restart) {
            CHECK_ANDROID(wait_for(&started, 1));
            pause_instance = 1;
            instance_lite_restart(config);
        }
        CHECK_ANDROID(wait_for(&paused, 1));
        CHECK_ANDROID(instance_lite_set_active(1) == -1);
        CHECK_ANDROID(utun_bridge_set_phone_state(2, 31) == -1);
        CHECK_ANDROID(instance_lite_configure_radio(42, radio_valid, radio_configured) == -1);
        pthread_t stopper;
        CHECK_ANDROID(!pthread_create(&stopper, NULL, stop_worker, NULL));
        CHECK_ANDROID(wait_for(&stop_entered, 1));
        struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 3;
        pthread_mutex_lock(&observation_lock);
        while (!stop_requested && !stop_done) {
            if (pthread_cond_timedwait(&changed, &observation_lock, &deadline)) break;
        }
        unsigned stopped_early = stop_done;
        pthread_mutex_unlock(&observation_lock);
        CHECK_ANDROID(stop_requested && !stopped_early);
        CHECK_ANDROID(instance_lite_set_active(1) == -1);
        CHECK_ANDROID(utun_bridge_set_phone_state(2, 31) == -1);
        CHECK_ANDROID(instance_lite_configure_radio(42, radio_valid, radio_configured) == -1);
        if (slow) {
            clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 2; deadline.tv_nsec += 200000000;
            if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
            pthread_mutex_lock(&observation_lock);
            while (!stop_done) {
                if (pthread_cond_timedwait(&changed, &observation_lock, &deadline)) break;
            }
            stopped_early = stop_done;
            pthread_mutex_unlock(&observation_lock);
            CHECK_ANDROID(!stopped_early && !self_detaches);
        }
        pthread_mutex_lock(&observation_lock); release_worker = 1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&observation_lock);
        CHECK_ANDROID(!pthread_join(stopper, NULL));
        CHECK_ANDROID(stop_done == 1 && joins == 1 && !self_detaches && started == (unsigned)cancel_restart);
        CHECK_ANDROID(!instance_lite_is_running() && !__real_instance_lite_get_instance() && !instance_lite_get_uasync());
        CHECK_ANDROID(instance_lite_set_active(1) == -1);
        CHECK_ANDROID(utun_bridge_set_phone_state(2, 31) == -1);
        CHECK_ANDROID(instance_lite_configure_radio(42, radio_valid, radio_configured) == -1);
        CHECK_ANDROID(!errors && u_get_allocated_count() == allocated);
        /* Тот же singleton должен запуститься снова только после полного join. */
        pause_loop = pause_instance = pause_attach = 0;
        CHECK_ANDROID(!instance_lite_start(config) && wait_for(&started, 1u + cancel_restart));
        instance_lite_stop();
        CHECK_ANDROID(joins == 2 && !errors && u_get_allocated_count() == allocated);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "Android worker cancellation mode=%s joins=%u ready=%u detach=%u PASS",
            argv[1], joins, started, self_detaches);
        return 0;
    }
    if (stream_failure || voice_failure) {
        CHECK_ANDROID(wait_for(&loops_destroyed, 1));
        instance_lite_stop();
        CHECK_ANDROID(!started && attached == (unsigned)voice_failure && detached == 1 && destroyed == 1 && loops_destroyed == 1);
    } else {
        CHECK_ANDROID(wait_for(&started, 1) && instance_lite_is_running());
        CHECK_ANDROID(!instance_lite_set_active(1));
        uasync_post(instance_lite_get_uasync(), inspect_activity, (void*)(intptr_t)1);
        CHECK_ANDROID(wait_for(&activity_checked, 1));
        CHECK_ANDROID(!instance_lite_set_active(0));
        uasync_post(instance_lite_get_uasync(), inspect_activity, NULL);
        CHECK_ANDROID(wait_for(&activity_checked, 2));
        CHECK_ANDROID(!utun_bridge_call_audio_start(42));
        uasync_post(instance_lite_get_uasync(), inspect, NULL);
        CHECK_ANDROID(wait_for(&checked, 1));
        utun_bridge_call_audio_stop();
        if (restart) {
            instance_lite_restart(config);
            CHECK_ANDROID(wait_for(&started, 2) && !utun_bridge_call_audio_start(43));
            uasync_post(instance_lite_get_uasync(), inspect, NULL);
            CHECK_ANDROID(wait_for(&checked, 2));
            utun_bridge_call_audio_stop();
            char path[1024]; snprintf(path, sizeof(path), "%s/keys.conf", directory);
            CHECK_ANDROID(!utun_mkdir(path, 0700));
            CHECK_ANDROID(!instance_lite_regenerate_keys(config));
            CHECK_ANDROID(wait_for(&identity_failures, 1) && wait_for(&started, 3));
            CHECK_ANDROID(instance_lite_is_running() && started == 3 && attached == 2 && detached == 1 && !memcmp(current_node, first_node, sizeof(current_node)));
            uasync_post(instance_lite_get_uasync(), inspect, NULL);
            CHECK_ANDROID(wait_for(&checked, 3));
            CHECK_ANDROID(!test_rmdir(path));
            CHECK_ANDROID(!instance_lite_regenerate_keys(config));
            CHECK_ANDROID(wait_for(&started, 4) && memcmp(current_node, first_node, sizeof(current_node)) && !utun_bridge_call_audio_start(44));
            uasync_post(instance_lite_get_uasync(), inspect, NULL);
            CHECK_ANDROID(wait_for(&checked, 4));
            utun_bridge_call_audio_stop();
            uint8_t regenerated_node[32]; memcpy(regenerated_node, current_node, sizeof(regenerated_node));
            /* Снимок INI был снят до смены ключей: обычный restart не должен возвращать прежнюю идентичность. */
            instance_lite_restart(config);
            CHECK_ANDROID(wait_for(&started, 5) && !memcmp(current_node, regenerated_node, sizeof(current_node)));
            uasync_post(instance_lite_get_uasync(), inspect, NULL);
            CHECK_ANDROID(wait_for(&checked, 5));
            char backup[1024]; snprintf(backup, sizeof(backup), "%s/keys.backup", directory);
            CHECK_ANDROID(!rename(path, backup));
            FILE* invalid = fopen(path, "wb"); CHECK_ANDROID(invalid);
            int written = fprintf(invalid, "public_key=%s\nprivate_key=%064d\n", public_key, 0);
            int closed = fclose(invalid);
            CHECK_ANDROID(written > 0 && !closed);
            instance_lite_restart(config);
            CHECK_ANDROID(wait_for(&identity_failures, 2) && wait_for(&started, 6));
            CHECK_ANDROID(instance_lite_is_running() && started == 6 && !memcmp(current_node, regenerated_node, sizeof(current_node)) && detached == 3);
            uasync_post(instance_lite_get_uasync(), inspect, NULL);
            CHECK_ANDROID(wait_for(&checked, 6));
            CHECK_ANDROID(!test_unlink(path) && !rename(backup, path));
        }
        if (core_stop) {
            uasync_post(instance_lite_get_uasync(), stop_core, NULL);
            CHECK_ANDROID(wait_for(&loops_destroyed, 1u + 3u * restart));
        }
        instance_lite_stop();
        CHECK_ANDROID(attached == 1u + 3u * restart && detached == attached && destroyed == attached && loops_destroyed == attached);
    }
    CHECK_ANDROID(!errors && !instance_lite_is_running() && utun_bridge_call_audio_start(44) == -1);
    CHECK_ANDROID(u_get_allocated_count() == allocated);
    debug_set_log_hook(NULL);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "Android audio lifecycle mode=%s attach=%u detach=%u destroyed=%u loops=%u allocations=%zu->%zu PASS",
        argv[1], attached, detached, destroyed, loops_destroyed, allocated, u_get_allocated_count());
    return 0;
}
