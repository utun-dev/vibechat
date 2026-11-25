/* Регрессии native bridge: config callbacks, сохранение идентичности и батчи настоящего Opus-декодера. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_event.h"

#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/db_async.h"
#include "../src/media_async/voice_file.h"
#include "../src/media_async/attachment_send.h"
#include "../src/config_parser.h"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_crypto.h"
#include "../src/routing_layer/devgroup.h"
#include "../src/transport_layer/secure_channel.h"
#include "../tools/chatgui-android/jni_bridge/android_jni_bridge.h"
#include "../tools/chatgui-android/libutun_lite/instance_lite.h"
#include "../tools/chatgui-android/libutun_lite/utun_config_api.h"
#include "test_utils.h"

#define REQUIRE(value) do { if (!(value)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Android bridge check failed line=%d: %s", __LINE__, #value); return 1; \
} } while (0)

static const char* config_string(const char* key, const char* def) {
    return !strcmp(key, CFG_KEY_NODE_NAME) ? "Android config" : def;
}

static int64_t config_int64(const char* key, int64_t def) {
    return !strcmp(key, "wide") ? INT64_C(0x123456789abcdef) : def;
}

static int config_int(const char* key, int def) { return !strcmp(key, CFG_KEY_CONTROL_PORT) ? 4321 : def; }
static int config_bool(const char* key, int def) { return !strcmp(key, "enabled") ? 3 : def; }

/* Таблица caller меняется после регистрации: сохранённые функции не должны зависеть от её памяти. */
static int check_provider_copy(void) {
    utun_config_provider_t provider = {config_string, config_int64, config_int, config_bool};
    utun_config_set_provider(&provider);
    memset(&provider, 0, sizeof(provider));
    REQUIRE(!strcmp(cfg_get_string(CFG_KEY_NODE_NAME, "fallback"), "Android config"));
    REQUIRE(cfg_get_int64("wide", -1) == INT64_C(0x123456789abcdef));
    REQUIRE(cfg_get_int(CFG_KEY_CONTROL_PORT, -1) == 4321);
    REQUIRE(cfg_get_bool("enabled", -1) == 3);
    REQUIRE(!strcmp(cfg_get_string("missing", "fallback"), "fallback"));
    REQUIRE(cfg_get_int64("missing", 123) == 123 && cfg_get_int("missing", 456) == 456);
    REQUIRE(cfg_get_bool("missing", 789) == 789);
    provider.get_int = config_int;
    utun_config_set_provider(&provider);
    REQUIRE(cfg_get_int(CFG_KEY_CONTROL_PORT, -1) == 4321);
    REQUIRE(!strcmp(cfg_get_string(CFG_KEY_NODE_NAME, "fallback"), "fallback"));
    REQUIRE(cfg_get_int64("wide", 123) == 123 && cfg_get_bool("enabled", 456) == 456);
    utun_config_set_provider(NULL);
    REQUIRE(cfg_get_string(CFG_KEY_NODE_NAME, NULL) == NULL);
    REQUIRE(cfg_get_int(CFG_KEY_CONTROL_PORT, 9999) == 9999);
    printf("[PASS] config provider: owned table, optional callbacks and reset\n");
    return 0;
}

static const char* bridge_string(const char* key) { return config_string(key, NULL); }
static int64_t bridge_int64(const char* key) { return config_int64(key, 0); }
static int bridge_int(const char* key) { return config_int(key, 0); }

/* Рабочая регистрация bridge переживает возврат init, повторная регистрация и destroy снимают прежние callbacks. */
static int check_bridge_config(void) {
    struct utun_bridge_config config = {bridge_string, bridge_int64, bridge_int};
    REQUIRE(utun_bridge_init(NULL, 0, NULL, NULL, &config));
    memset(&config, 0, sizeof(config));
    REQUIRE(!strcmp(cfg_get_string(CFG_KEY_NODE_NAME, NULL), "Android config"));
    REQUIRE(!strcmp(cfg_get_string("missing", "fallback"), "fallback"));
    REQUIRE(cfg_get_int64("wide", -1) == INT64_C(0x123456789abcdef));
    REQUIRE(cfg_get_int(CFG_KEY_CONTROL_PORT, -1) == 4321 && cfg_get_bool("enabled", 99) == 99);
    REQUIRE(utun_bridge_init(NULL, 0, NULL, NULL, NULL));
    REQUIRE(!strcmp(cfg_get_string(CFG_KEY_NODE_NAME, "fallback"), "fallback"));
    REQUIRE(cfg_get_int64("wide", 123) == 123 && cfg_get_int(CFG_KEY_CONTROL_PORT, 456) == 456);
    config.get_int = bridge_int;
    REQUIRE(utun_bridge_init(NULL, 0, NULL, NULL, &config));
    REQUIRE(cfg_get_int(CFG_KEY_CONTROL_PORT, -1) == 4321);
    REQUIRE(cfg_get_string(CFG_KEY_NODE_NAME, NULL) == NULL && cfg_get_int64("wide", 789) == 789);
    REQUIRE(!utun_bridge_destroy());
    REQUIRE(cfg_get_int(CFG_KEY_CONTROL_PORT, 9999) == 9999);
    REQUIRE(utun_bridge_init(NULL, 0, NULL, NULL, NULL));
    REQUIRE(cfg_get_int(CFG_KEY_CONTROL_PORT, 9998) == 9998);
    REQUIRE(!utun_bridge_destroy());
    printf("[PASS] bridge config: init, replacement, destroy and reinit\n");
    return 0;
}

/* Настоящий unlink: журналы, повтор очистки и отказ до удаления основной БД при заблокированном WAL. */
static int check_database_cleanup(const char* directory) {
    REQUIRE(utun_bridge_clear_database(NULL) < 0);
    REQUIRE(utun_bridge_clear_database("") < 0);
    REQUIRE(!utun_bridge_clear_database(directory));
    const char* names[] = {"chats.db", "chats.db-wal", "chats.db-shm"};
    char path[1024];
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        FILE* file = fopen(path, "wb"); REQUIRE(file);
        int written = fputs("fixture", file);
        int closed = fclose(file);
        REQUIRE(written >= 0 && !closed);
    }
    REQUIRE(!utun_bridge_clear_database(directory));
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        REQUIRE(test_unlink(path) == -1 && errno == ENOENT);
    }
    REQUIRE(!utun_bridge_clear_database(directory));
    snprintf(path, sizeof(path), "%s/chats.db-wal", directory);
    REQUIRE(!utun_mkdir(path, 0700));
    for (unsigned i = 0; i < 3; i += 2) {
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        FILE* file = fopen(path, "wb"); REQUIRE(file);
        int written = fputs("fixture", file);
        int closed = fclose(file);
        REQUIRE(written >= 0 && !closed);
    }
    REQUIRE(utun_bridge_clear_database(directory) < 0);
    for (unsigned i = 0; i < 3; i += 2) {
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        FILE* file = fopen(path, "rb"); REQUIRE(file && !fclose(file));
    }
    snprintf(path, sizeof(path), "%s/chats.db-wal", directory);
    REQUIRE(!test_rmdir(path));
    REQUIRE(!utun_bridge_clear_database(directory));
    printf("[PASS] database cleanup: exact files, missing files, real unlink failure and retry\n");
    return 0;
}

/* Только полностью записанная пара, права 0600, согласованность X25519 и отсутствие оставшихся temp. */
static int check_identity_file(const char* directory, const struct global_config* identity) {
    char path[1024], text[256], expected[256];
    snprintf(path, sizeof(path), "%s/keys.conf", directory);
    FILE* file = fopen(path, "rb"); REQUIRE(file);
    size_t length = fread(text, 1, sizeof(text), file);
    int read_error = ferror(file), closed = fclose(file);
    int expected_length = snprintf(expected, sizeof(expected), "public_key=%s\nprivate_key=%s\n",
                                   identity->my_public_key_hex, identity->my_private_key_hex);
    REQUIRE(!read_error && !closed && length == (size_t)expected_length && !memcmp(text, expected, length));
    struct stat info;
    REQUIRE(!stat(path, &info) && (info.st_mode & 0777) == 0600);
    struct SC_MYKEYS keys; uint8_t ed[32], seed[32];
    REQUIRE(sc_identity_load(identity->my_public_key_hex, identity->my_private_key_hex, &keys, ed, seed) == SC_OK);
    REQUIRE(sc_identity_check("Android identity", keys.public_key, ed) == SC_OK);
    DIR* entries = opendir(directory); REQUIRE(entries);
    int unexpected = 0;
    struct dirent* entry;
    while ((entry = readdir(entries)))
        unexpected += strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") && strcmp(entry->d_name, "keys.conf");
    REQUIRE(!closedir(entries) && !unexpected);
    return 0;
}

/* Настоящие ошибки open, flush и publish не меняют config и не разрушают прежний keys.conf. */
static int check_identity_persistence(const char* directory) {
    struct utun_config* config = u_calloc(1, sizeof(*config)); REQUIRE(config);
    REQUIRE(instance_lite_generate_keys(NULL) == -1);
    REQUIRE(instance_lite_generate_keys(config) == -1);
    /* Выбранный общим INI-парсером каталог: пробелы и последнее значение должны сохранять смысл. */
    char ini[1024];
    int length = snprintf(ini, sizeof(ini), "[global]\ndb_path=/nonexistent/utun-identity\n  db_path = %s  \n", directory);
    struct utun_config* parsed = parse_config_from_buf(ini, (size_t)length, "android-identity-test"); REQUIRE(parsed);
    config->global = parsed->global;
    free_config(parsed);
    REQUIRE(!instance_lite_generate_keys(config));
    REQUIRE(!check_identity_file(directory, &config->global));
    struct global_config previous = config->global;
    REQUIRE(!instance_lite_generate_keys(config));
    REQUIRE(strcmp(previous.my_public_key_hex, config->global.my_public_key_hex));
    REQUIRE(!check_identity_file(directory, &config->global));
    previous = config->global;

    struct rlimit original, limited;
    REQUIRE(!getrlimit(RLIMIT_FSIZE, &original));
    limited = original; limited.rlim_cur = 64;
    struct sigaction ignore = {0}, old_signal;
    ignore.sa_handler = SIG_IGN;
    REQUIRE(!sigemptyset(&ignore.sa_mask) && !sigaction(SIGXFSZ, &ignore, &old_signal));
    int limit_result = setrlimit(RLIMIT_FSIZE, &limited);
    int result = limit_result ? 0 : instance_lite_generate_keys(config);
    int restored = setrlimit(RLIMIT_FSIZE, &original);
    int signal_restored = sigaction(SIGXFSZ, &old_signal, NULL);
    REQUIRE(!limit_result && !restored && !signal_restored && result == -1);
    REQUIRE(!memcmp(&previous, &config->global, sizeof(previous)));
    REQUIRE(!check_identity_file(directory, &previous));

    char path[1024], backup[1024];
    snprintf(path, sizeof(path), "%s/keys.conf", directory);
    snprintf(backup, sizeof(backup), "%s/identity-backup", directory);
    REQUIRE(!rename(path, backup) && !utun_mkdir(path, 0700));
    REQUIRE(instance_lite_generate_keys(config) == -1);
    REQUIRE(!memcmp(&previous, &config->global, sizeof(previous)));
    REQUIRE(!test_rmdir(path) && !rename(backup, path));
    REQUIRE(!check_identity_file(directory, &previous));
    snprintf(config->global.db_path, sizeof(config->global.db_path), "%s/missing", directory);
    struct global_config missing = config->global;
    REQUIRE(instance_lite_generate_keys(config) == -1);
    REQUIRE(!memcmp(&missing, &config->global, sizeof(missing)));
    REQUIRE(!check_identity_file(directory, &previous));
    config->global = previous;
    REQUIRE(!instance_lite_generate_keys(config) && !check_identity_file(directory, &config->global));
    REQUIRE(!test_unlink(path));
    u_free(config);
    printf("[PASS] identity persistence: coherent keys, private mode, parsed path, flush/open/publish failures and retry\n");
    return 0;
}

/* Один и тот же файл декодируется покадрово и батчами: PCM и количество отсчётов должны совпасть. */
static int check_voice_batches(const char* path, int rate, int channels) {
    int frame = rate / 50 * channels, count = frame * 27;
    int16_t* input = u_calloc((size_t)count, sizeof(*input));
    int16_t* reference = u_calloc((size_t)count, sizeof(*reference));
    int16_t* output = u_calloc((size_t)(count + 8 * frame), sizeof(*output));
    REQUIRE(input && reference && output);
    for (int i = 0; i < count; i++) input[i] = (int16_t)((i % 97 - 48) * 200);
    struct attachment_info info = {0};
    REQUIRE(!voice_file_encode(input, (size_t)count, rate, channels, 1, path, &info));

    int decoded_rate, decoded_channels, total;
    void* decoder = utun_bridge_voice_decode_open(path, &decoded_rate, &decoded_channels, &total);
    REQUIRE(decoder && decoded_rate == rate && decoded_channels == channels && total == count);
    for (int position = 0; position < count; position += frame)
        REQUIRE(utun_bridge_voice_decode_read(decoder, reference + position, frame) == frame);
    REQUIRE(!utun_bridge_voice_decode_read(decoder, output, frame));
    utun_bridge_voice_decode_close(decoder);

    decoder = utun_bridge_voice_decode_open(path, &decoded_rate, &decoded_channels, &total); REQUIRE(decoder);
    REQUIRE(!utun_bridge_voice_decode_read(decoder, output, frame - 1));
    int position = 0;
    while (position < count) {
        int expected = count - position < 7 * frame ? count - position : 7 * frame;
        output[position + expected] = 12345;
        int read = utun_bridge_voice_decode_read(decoder, output + position, 7 * frame + frame / 2);
        REQUIRE(read == expected && output[position + expected] == 12345); position += read;
    }
    REQUIRE(position == count && !memcmp(output, reference, (size_t)count * sizeof(*output)));
    REQUIRE(!utun_bridge_voice_decode_read(decoder, output, count + frame));
    REQUIRE(!utun_bridge_voice_decode_read(decoder, output, count + frame));
    utun_bridge_voice_decode_close(decoder);

    decoder = utun_bridge_voice_decode_open(path, &decoded_rate, &decoded_channels, &total); REQUIRE(decoder);
    output[count] = 12345;
    REQUIRE(utun_bridge_voice_decode_read(decoder, output, count + frame) == count);
    REQUIRE(output[count] == 12345 && !memcmp(output, reference, (size_t)count * sizeof(*output)));
    REQUIRE(!utun_bridge_voice_decode_read(decoder, output, count + frame));
    utun_bridge_voice_decode_close(decoder);
    REQUIRE(!test_unlink(path));
    u_free(input); u_free(reference); u_free(output);
    printf("[PASS] voice decode: rate=%d channels=%d, partial batch, oversized read, exact PCM and EOF\n", rate, channels);
    return 0;
}

static pthread_mutex_t worker_test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_test_cond = PTHREAD_COND_INITIALIZER;
static int worker_entered, worker_release;
static void worker_block(void* unused) {
    (void)unused;
    pthread_mutex_lock(&worker_test_mutex);
    worker_entered = 1; pthread_cond_broadcast(&worker_test_cond);
    while (!worker_release) pthread_cond_wait(&worker_test_cond, &worker_test_mutex);
    pthread_mutex_unlock(&worker_test_mutex);
}
static void worker_no_discard(void* unused) { (void)unused; }
static void worker_increment(void* arg) { (*(int*)arg)++; }
static void worker_inline(void* arg, instance_lite_complete_fn complete, void* completion_arg) {
    worker_increment(arg); complete(completion_arg);
}
struct worker_query { pthread_t thread; uint64_t node; uint8_t x[32], ed[32]; int nested, nested_deferred; uint64_t nested_dm; };
static void worker_read(void* arg) {
    struct worker_query* query = arg;
    query->thread = pthread_self(); query->node = instance_lite_get_instance()->node_id;
    memcpy(query->x, instance_lite_get_instance()->my_keys.public_key, 32);
    memcpy(query->ed, instance_lite_get_instance()->my_ed25519_pubkey, 32);
    if (instance_lite_call(worker_increment, &query->nested)) query->nested = -1;
    query->nested_deferred = instance_lite_call_deferred(worker_inline, &query->nested);
    query->nested_dm = utun_bridge_dm_start_wait(query->ed, NULL, NULL, 0);
}
static void worker_payload_discard(void* arg) { __atomic_store_n((int*)arg, -1, __ATOMIC_RELEASE); }
static void worker_payload_run(void* arg) { __atomic_store_n((int*)arg, 1, __ATOMIC_RELEASE); }
static void* worker_stop(void* arg) { *(int*)arg = instance_lite_stop(); return NULL; }
static void* worker_wait_call(void* arg) {
    int callback_count = 0;
    *(int*)arg = instance_lite_call(worker_increment, &callback_count);
    if (callback_count) *(int*)arg = 2;
    return NULL;
}
struct worker_pcm_owner { int16_t* samples; int released; };
static void worker_pcm_release(void* arg) {
    struct worker_pcm_owner* owner = arg;
    u_free(owner->samples); owner->samples = NULL;
    __atomic_add_fetch(&owner->released, 1, __ATOMIC_ACQ_REL);
}

static pthread_mutex_t deferred_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t deferred_cond = PTHREAD_COND_INITIALIZER;
static int deferred_entered, deferred_release;
struct deferred_query { int hold, fail, value, result, returned, beat; pthread_t begin_thread, done_thread, sql_thread; };
struct deferred_job { struct deferred_query* query; instance_lite_complete_fn complete; void* completion_arg; };
/* SQL намеренно задержан: frontend ждёт, но вызовы/таймеры uasync должны продолжаться. */
static int deferred_work(sqlite3* db, void* arg) {
    struct deferred_query* query = ((struct deferred_job*)arg)->query; query->sql_thread = pthread_self();
    if (query->hold) {
        pthread_mutex_lock(&deferred_mutex); deferred_entered = 1; pthread_cond_broadcast(&deferred_cond);
        while (!deferred_release) pthread_cond_wait(&deferred_cond, &deferred_mutex);
        pthread_mutex_unlock(&deferred_mutex);
    }
    if (query->fail) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "deferred bridge fixture: injected SQL failure"); return SQLITE_BUSY; }
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT 1", -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); if (rc == SQLITE_ROW) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "deferred bridge SQL rc=%d finalize=%d", rc, final);
    return rc;
}
static void deferred_done(void* arg, const struct db_async_result* result) {
    struct deferred_job* job = arg; job->query->done_thread = pthread_self(); job->query->value = result->status ? -1 : 1;
    instance_lite_complete_fn complete = job->complete; void* completion_arg = job->completion_arg;
    u_free(job); complete(completion_arg);
}
static void deferred_begin(void* arg, instance_lite_complete_fn complete, void* completion_arg) {
    struct deferred_query* query = arg; query->begin_thread = pthread_self(); struct deferred_job* job = u_malloc(sizeof(*job));
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "deferred bridge request allocation failed"); query->value = -1; complete(completion_arg); return;
    }
    job->query = query; job->complete = complete; job->completion_arg = completion_arg;
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    db_async_run(inst ? inst->db_worker : NULL, &(struct db_async_task){.name="android-deferred-test", .mode=DB_ASYNC_READ,
        .work=deferred_work, .data=job, .done=deferred_done});
}
static void* worker_wait_deferred(void* arg) {
    struct deferred_query* query = arg; query->result = instance_lite_call_deferred(deferred_begin, query);
    __atomic_store_n(&query->returned, 1, __ATOMIC_RELEASE); return NULL;
}
static void deferred_pause(void) {
    pthread_mutex_lock(&deferred_mutex); deferred_entered = deferred_release = 0; pthread_mutex_unlock(&deferred_mutex);
}
static void deferred_paused(void) {
    pthread_mutex_lock(&deferred_mutex); while (!deferred_entered) pthread_cond_wait(&deferred_cond, &deferred_mutex);
    pthread_mutex_unlock(&deferred_mutex);
}
static void deferred_unpause(void) {
    pthread_mutex_lock(&deferred_mutex); deferred_release = 1; pthread_cond_broadcast(&deferred_cond); pthread_mutex_unlock(&deferred_mutex);
}
static void deferred_beat(void* arg) { __atomic_store_n((int*)arg, 1, __ATOMIC_RELEASE); }
static void deferred_timer(void* arg) {
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!uasync_set_timeout(inst->ua, 10, arg, deferred_beat, "android-deferred-alive"))
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "deferred bridge timer allocation failed");
}
struct bridge_peer {
    uint64_t node; uint8_t x[32], ed[32]; int result;
    instance_lite_complete_fn complete; void* completion_arg;
};
/* Только фикстура: штатный API затем обязан вернуть фактические DEVICE/GROUPS ID после commit. */
static int worker_seed_peer_sql(sqlite3* db, void* arg) {
    struct bridge_peer* peer = arg; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey,name) VALUES(?,?,?,'Bridge peer')", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)peer->node);
    if (!rc) rc = sqlite3_bind_blob(st, 2, peer->x, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, peer->ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); peer->result = rc == SQLITE_DONE ? final : rc;
    if (peer->result) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Android DM fixture rc=%d finalize=%d: %s", rc, final, sqlite3_errmsg(db));
    return peer->result;
}
static void worker_seed_peer_done(void* arg, const struct db_async_result* result) {
    struct bridge_peer* peer = arg; peer->result = result->status ? result->sqlite_code : SQLITE_OK;
    peer->complete(peer->completion_arg);
}
static void worker_seed_peer(void* arg, instance_lite_complete_fn complete, void* completion_arg) {
    struct bridge_peer* peer = arg; peer->complete = complete; peer->completion_arg = completion_arg;
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    db_async_run(inst ? inst->db_worker : NULL, &(struct db_async_task){.name="Android peer fixture", .mode=DB_ASYNC_WRITE,
        .data=peer, .work=worker_seed_peer_sql, .done=worker_seed_peer_done});
}

static int startup_issue;
static void observe_startup_error(int type, const uint8_t* data, int size) {
    if (type == CHAT_EVT_STARTUP_FAILED && size > 3) __atomic_store_n(&startup_issue, data[0], __ATOMIC_RELEASE);
}

/* Тестовый seed из байтов 02 даёт Ed25519 со знаком 1, как на SM. До подтверждения worker сохраняет файл. */
static int check_stopped_key_recovery(const char* directory, const char* config) {
    char path[1100], saved[180];
    snprintf(path, sizeof(path), "%s/keys.conf", directory);
    const char fixture[] = "public_key=0000000000000000000000000000000000000000000000000000000000000000\n"
                           "private_key=0202020202020202020202020202020202020202020202020202020202020202\n";
    instance_lite_set_event_handler(observe_startup_error);
    REQUIRE(!utun_mkdir(path, 0700) && !instance_lite_start(config));
    for (int i = 0; i < 200 && !__atomic_load_n(&startup_issue, __ATOMIC_ACQUIRE); i++) usleep(10000);
    REQUIRE(__atomic_load_n(&startup_issue, __ATOMIC_ACQUIRE) == CHAT_STARTUP_KEY_ACCESS);
    REQUIRE(!instance_lite_stop() && !test_rmdir(path));
    __atomic_store_n(&startup_issue, 0, __ATOMIC_RELEASE);
    FILE* file = fopen(path, "wb"); REQUIRE(file);
    REQUIRE(fwrite(fixture, 1, sizeof(fixture) - 1, file) == sizeof(fixture) - 1 && !fclose(file));
    REQUIRE(!instance_lite_start(config));
    for (int i = 0; i < 200 && !__atomic_load_n(&startup_issue, __ATOMIC_ACQUIRE); i++) usleep(10000);
    REQUIRE(__atomic_load_n(&startup_issue, __ATOMIC_ACQUIRE) == CHAT_STARTUP_INVALID_KEY);
    REQUIRE(!instance_lite_is_running() && !instance_lite_stop());
    file = fopen(path, "rb"); REQUIRE(file);
    REQUIRE(fread(saved, 1, sizeof(saved), file) == sizeof(fixture) - 1 && !fclose(file));
    REQUIRE(!memcmp(fixture, saved, sizeof(fixture) - 1));
    REQUIRE(!instance_lite_replace_stopped_keys(directory));
    file = fopen(path, "rb"); REQUIRE(file);
    size_t length = fread(saved, 1, sizeof(saved), file); REQUIRE(!fclose(file));
    REQUIRE(length == sizeof(fixture) - 1 && memcmp(fixture, saved, length));
    /* Несовместимая база требует отдельного выбора даже после подтверждения нового ключа. */
    char database[1100]; snprintf(database, sizeof(database), "%s/chats.db", directory);
    sqlite3* db = NULL; REQUIRE(sqlite3_open(database, &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "CREATE TABLE incompatible(value TEXT);INSERT INTO incompatible VALUES('retained')", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_close(db) == SQLITE_OK);
    __atomic_store_n(&startup_issue, 0, __ATOMIC_RELEASE);
    REQUIRE(!instance_lite_start(config));
    for (int i = 0; i < 200 && !__atomic_load_n(&startup_issue, __ATOMIC_ACQUIRE); i++) usleep(10000);
    REQUIRE(__atomic_load_n(&startup_issue, __ATOMIC_ACQUIRE) == CHAT_STARTUP_DATABASE && !instance_lite_stop());
    REQUIRE(sqlite3_open_v2(database, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_stmt* row = NULL; REQUIRE(sqlite3_prepare_v2(db, "SELECT value FROM incompatible", -1, &row, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(row) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(row, 0), "retained"));
    REQUIRE(sqlite3_finalize(row) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK);
    REQUIRE(!utun_bridge_clear_database(directory));
    REQUIRE(!instance_lite_start(config));
    for (int i = 0; i < 1000 && !instance_lite_is_running(); i++) usleep(10000);
    REQUIRE(instance_lite_is_running() && instance_lite_replace_stopped_keys(directory) == -1);
    REQUIRE(!instance_lite_stop());
    char again[sizeof(saved)];
    file = fopen(path, "rb"); REQUIRE(file);
    REQUIRE(fread(again, 1, sizeof(again), file) == length && !fclose(file) && !memcmp(again, saved, length));
    instance_lite_set_event_handler(NULL);
    printf("[PASS] invalid saved identity reported, preserved, replaced after join and reused on startup\n");
    return 0;
}

/* Настоящий pthread: чтение и вложенный call идут в worker, stop отменяет очередь и будит ожидающего caller. */
static int check_worker_requests(const char* directory) {
    char path[1024], config[2048];
    snprintf(path, sizeof(path), "%s/worker", directory); REQUIRE(!utun_mkdir(path, 0700));
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\nauto_sockets=no\nchatserver_enabled=yes\n"
        "db_sync_enabled=yes\n[ntp]\nenabled=no\n[chat]\ngroup_autoconnect=0\n", path);
    REQUIRE(!check_stopped_key_recovery(path, config));
    int refused = 0;
    REQUIRE(instance_lite_call(worker_increment, &refused) == -1 && !refused);
    REQUIRE(instance_lite_call_deferred(worker_inline, &refused) == -1 && !refused);
    REQUIRE(!instance_lite_start(config));
    for (int i = 0; i < 1000 && !instance_lite_is_running(); i++) usleep(10000);
    REQUIRE(instance_lite_is_running());
    struct worker_query query = {0};
    REQUIRE(!instance_lite_call(worker_read, &query));
    REQUIRE(query.node && query.nested == 1 && query.nested_deferred == -1 && !query.nested_dm && !pthread_equal(query.thread, pthread_self()));
    struct bridge_peer peer = {0}; uint8_t seed[32], secret[32];
    REQUIRE(!sc_identity_generate(peer.x, secret, peer.ed, seed)); peer.node = sc_derive_node_id_from_pubkey(peer.x);
    REQUIRE(!instance_lite_call_deferred(worker_seed_peer, &peer) && !peer.result);
    uint64_t device = utun_bridge_dm_start_wait(peer.ed, "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951", "Bridge peer", 0);
    REQUIRE(device == dm_derive_conv_id(query.ed, peer.ed));
    struct dm_party parties[2] = {{.node=query.node}, {.node=peer.node}};
    memcpy(parties[0].x, query.x, 32); memcpy(parties[0].ed, query.ed, 32);
    memcpy(parties[1].x, peer.x, 32); memcpy(parties[1].ed, peer.ed, 32);
    uint64_t grouped = utun_bridge_dm_start_wait(peer.ed, "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951", "Bridge devices", 1);
    REQUIRE(grouped && grouped != device && grouped == dm_identity_conv_id(DM_MODE_GROUPS, parties));
    REQUIRE(!utun_bridge_dm_start_wait(query.ed, NULL, NULL, 0) && !utun_bridge_dm_start_wait(peer.ed, NULL, NULL, 2));
    REQUIRE(utun_bridge_dm_start_wait(peer.ed, "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16", "Changed", 0) == device);
    int immediate = 0; REQUIRE(!instance_lite_call_deferred(worker_inline, &immediate) && immediate == 1);
    struct deferred_query delayed = {.hold=1}; pthread_t deferred_waiter; deferred_pause();
    REQUIRE(!pthread_create(&deferred_waiter, NULL, worker_wait_deferred, &delayed)); deferred_paused();
    struct worker_query live = {0}; REQUIRE(!instance_lite_call(worker_read, &live) && live.node == query.node);
    REQUIRE(!instance_lite_call(deferred_timer, &delayed.beat));
    for (int i = 0; i < 1000 && !__atomic_load_n(&delayed.beat, __ATOMIC_ACQUIRE); i++) usleep(1000);
    REQUIRE(__atomic_load_n(&delayed.beat, __ATOMIC_ACQUIRE) && !__atomic_load_n(&delayed.returned, __ATOMIC_ACQUIRE));
    deferred_unpause(); REQUIRE(!pthread_join(deferred_waiter, NULL));
    REQUIRE(!delayed.result && delayed.value == 1 && pthread_equal(delayed.begin_thread, query.thread) &&
        pthread_equal(delayed.done_thread, query.thread) && !pthread_equal(delayed.sql_thread, query.thread));
    struct deferred_query failed = {.fail=1}; REQUIRE(!instance_lite_call_deferred(deferred_begin, &failed) && failed.value == -1);
    REQUIRE(!instance_lite_post(worker_block, NULL, NULL, worker_no_discard));
    pthread_mutex_lock(&worker_test_mutex);
    while (!worker_entered) pthread_cond_wait(&worker_test_cond, &worker_test_mutex);
    pthread_mutex_unlock(&worker_test_mutex);
    int retained_payload = 0;
    REQUIRE(!instance_lite_post(worker_payload_run, &retained_payload, NULL, worker_payload_discard));
    REQUIRE(!instance_lite_restart("[global]\ndb_path=/nonexistent/utun-restart-identity/missing\n"));
    REQUIRE(!instance_lite_is_running());
    pthread_mutex_lock(&worker_test_mutex);
    worker_release = 1; pthread_cond_broadcast(&worker_test_cond);
    pthread_mutex_unlock(&worker_test_mutex);
    for (int i = 0; i < 1000 && !instance_lite_is_running(); i++) usleep(10000);
    REQUIRE(instance_lite_is_running() && __atomic_load_n(&retained_payload, __ATOMIC_ACQUIRE) == 1);
    struct worker_query retained = {0}; REQUIRE(!instance_lite_call(worker_read, &retained) && retained.node == query.node);
    struct deferred_query draining = {.hold=1}; deferred_pause();
    REQUIRE(!pthread_create(&deferred_waiter, NULL, worker_wait_deferred, &draining)); deferred_paused();
    pthread_mutex_lock(&worker_test_mutex);
    worker_entered = 0; worker_release = 0;
    pthread_mutex_unlock(&worker_test_mutex);
    REQUIRE(!instance_lite_post(worker_block, NULL, NULL, worker_no_discard));
    pthread_mutex_lock(&worker_test_mutex);
    while (!worker_entered) pthread_cond_wait(&worker_test_cond, &worker_test_mutex);
    pthread_mutex_unlock(&worker_test_mutex);
    int payload = 0, stopped = -2, waited = -2;
    REQUIRE(!instance_lite_post(worker_payload_run, &payload, NULL, worker_payload_discard));
    char cancelled_file[1100]; snprintf(cancelled_file, sizeof(cancelled_file), "%s/cancelled-source", path);
    FILE* source = fopen(cancelled_file, "wb"); REQUIRE(source && fputs("owned cache", source) >= 0 && !fclose(source));
    struct worker_pcm_owner pcm = {u_calloc(960, sizeof(int16_t)), 0}; REQUIRE(pcm.samples);
    struct attachment_send_req* media = u_calloc(1, sizeof(*media)); REQUIRE(media);
    snprintf(media->source, sizeof(media->source), "%s", cancelled_file); media->temporary_source = 1;
    media->pcm = pcm.samples; media->pcm_count = 960; media->pcm_owner = &pcm; media->pcm_release = worker_pcm_release;
    REQUIRE(!instance_lite_post(attachment_send_trampoline, media, &media->inst, attachment_send_discard));
    pthread_t waiter, stopper;
    REQUIRE(!pthread_create(&waiter, NULL, worker_wait_call, &waited));
    REQUIRE(!pthread_create(&stopper, NULL, worker_stop, &stopped));
    for (int i = 0; i < 500 && instance_lite_is_running(); i++) usleep(10000);
    REQUIRE(!instance_lite_is_running());
    REQUIRE(!__atomic_load_n(&draining.returned, __ATOMIC_ACQUIRE));
    pthread_mutex_lock(&worker_test_mutex);
    worker_release = 1; pthread_cond_broadcast(&worker_test_cond);
    pthread_mutex_unlock(&worker_test_mutex);
    deferred_unpause(); REQUIRE(!pthread_join(deferred_waiter, NULL));
    REQUIRE(!draining.result && (draining.value == 1 || draining.value == -1) && pthread_equal(draining.done_thread, query.thread));
    REQUIRE(!pthread_join(waiter, NULL) && !pthread_join(stopper, NULL));
    REQUIRE(!stopped && waited == -1 && __atomic_load_n(&payload, __ATOMIC_ACQUIRE) == -1);
    REQUIRE(__atomic_load_n(&pcm.released, __ATOMIC_ACQUIRE) == 1 && !pcm.samples);
    REQUIRE(test_unlink(cancelled_file) == -1 && errno == ENOENT);
    REQUIRE(instance_lite_post(worker_payload_run, &payload, NULL, worker_payload_discard) == -1 && payload == -1);
    REQUIRE(utun_bridge_attachment_download("1", 1) == -1);
    REQUIRE(utun_bridge_request_invite_candidates("1") == -1 && utun_bridge_request_invite_link("1", (const uint8_t[32]){1}, 1) == -1);
    REQUIRE(utun_bridge_dm_start(peer.ed, "1", "peer") == -1);
    REQUIRE(!utun_bridge_dm_start_wait(peer.ed, "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951", "peer", 0));
    REQUIRE(!utun_bridge_clear_database(path));
    char keys[1100]; snprintf(keys, sizeof(keys), "%s/keys.conf", path); REQUIRE(!test_unlink(keys));
    const char* folders[] = {"member_files/.uploads", "member_files", "dm_pending", "dm_media"};
    for (unsigned i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        snprintf(keys, sizeof(keys), "%s/%s", path, folders[i]); REQUIRE(!test_rmdir(keys));
    }
    REQUIRE(!test_rmdir(path));
    printf("[PASS] worker requests: real worker read, nested call, stopped queue discard and waiter cancellation\n");
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG);
    int failed = check_provider_copy();
    /* Проверки bridge требуют исправного владения таблицей провайдера. */
    if (!failed) failed += check_bridge_config();
    utun_config_set_provider(NULL);
    char directory[512], path[1024];
    const char* temporary = getenv("TMPDIR");
    int length = snprintf(directory, sizeof(directory), "%s/utun_android_bridge_XXXXXX", temporary ? temporary : "/tmp");
    REQUIRE(length > 0 && length < (int)sizeof(directory));
    REQUIRE(!test_mkdtemp(directory));
    failed += check_database_cleanup(directory);
    failed += check_identity_persistence(directory);
    snprintf(path, sizeof(path), "%s/voice.opus", directory);
    const int rates[] = {8000, 12000, 16000, 24000, 48000};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        for (int channels = 1; channels <= 2; channels++) failed += check_voice_batches(path, rates[i], channels);
    failed += check_worker_requests(directory);
    if (failed) return 1;
    REQUIRE(!test_rmdir(directory));
    printf("test_android_bridge: PASS\n");
    return 0;
}
