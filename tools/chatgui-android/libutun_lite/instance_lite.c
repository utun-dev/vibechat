#include <openssl/crypto.h>
/*
 * instance_lite.c — lightweight uTun instance lifecycle for Android
 *
 * Kotlin sends INI config text → parse_config_from_buf →
 * utun_instance_create_from_config → utun_core_start → chat_service_start →
 * pthread with uasync_poll loop.
 *
 * UTUN выключен по умолчанию; [gui] utun_enabled=yes явно включает сервис при старте и рестарте.
 */
#include "instance_lite.h"
#include "../../../src/db_schema.h"
#include "utun_instance.h"
#include "config_parser.h"
#include "config_updater.h"
#include "file_io.h"
#include "secure_channel.h"
#include "topo_node_sqlite.h"
#include "chat/chat_core.h"
#include "kanban/kanban.h"
#include "chat/chat_sync.h"
#include "chat/chat_join_port.h"
#include "chat/chat_event.h"
#include "chat/chat_setting.h"
#include "chat/db_sync.h"
#include "u_async.h"
#include "sqlite3.h"
#include "mem.h"
#include "debug_config.h"
#include "transport_layer/etcp_connections.h"
#include "transport_layer/etcp.h"
#include "transport_layer/auto_socket.h"
#include "../jni_bridge/android_udp_log.h"
#include "../jni_bridge/android_jni_bridge.h"
#include "standby.h"
#include "stream_bridge.h"
#include "call/call.h"
#include "call/call_video_port.h"
#ifdef UTUN_WITH_VOICE
#include "call/call_audio.h"
#endif
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#ifdef __ANDROID__
#include <android/log.h>
#define  IL_LOGV(fmt, ...)  __android_log_print(ANDROID_LOG_VERBOSE, "utun-inst", fmt, ##__VA_ARGS__)
#define  IL_LOGI(fmt, ...)  __android_log_print(ANDROID_LOG_INFO,    "utun-inst", fmt, ##__VA_ARGS__)
#define  IL_LOGE(fmt, ...)  __android_log_print(ANDROID_LOG_ERROR,   "utun-inst", fmt, ##__VA_ARGS__)
#else
#define  IL_LOGV(fmt, ...)  fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define  IL_LOGI(fmt, ...)  fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define  IL_LOGE(fmt, ...)  fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#endif

/* ── Static state ──
 *
 * g_control_mutex сериализует start/stop/restart и владение joinable pthread.
 * Worker единолично создаёт/освобождает instance/ua. g_stop_mutex защищает
 * публикацию ua для wakeup, подтверждение готовности и условие завершения.
 * Сырые getters не удерживают ресурсы; их callers по-прежнему исключают teardown.
 */

static struct UASYNC*        g_ua = NULL;           /* C-write, Kotlin-read via __atomic */
static struct UTUN_INSTANCE* g_inst = NULL;         /* C-write, Kotlin-read via __atomic */
static pthread_t             g_thread;
static __thread int          g_is_worker;           /* Проверка controller API до захвата блокировки start/stop. */
static pthread_mutex_t join_port_lock = PTHREAD_MUTEX_INITIALIZER;
static struct chat_join_port* join_port;
static void bind_join_port(struct UTUN_INSTANCE* inst) {
    struct chat_join_port* next = inst ? chat_sync_join_port(inst) : NULL;
    pthread_mutex_lock(&join_port_lock);
    struct chat_join_port* old = join_port; join_port = next;
    pthread_mutex_unlock(&join_port_lock);
    chat_join_port_release(old);
}
int instance_lite_join_link(uint64_t request, const char* link, size_t len) {
    pthread_mutex_lock(&join_port_lock);
    int result = chat_join_port_submit(join_port, request, link, len);
    pthread_mutex_unlock(&join_port_lock); return result;
}

int instance_lite_add_server(uint64_t request, const char* channel, const char* link, size_t len) {
    pthread_mutex_lock(&join_port_lock);
    int result = chat_join_port_offer(join_port, request, channel, link, len);
    pthread_mutex_unlock(&join_port_lock); return result;
}

static volatile int          g_stop = 0;            /* Kotlin-write, C-read (poll loop) */
struct restart_request {
    char* config_text;              /* Копия INI, принадлежит запросу до завершения подготовки. */
    int regenerate_keys;            /* Новая пара сохраняется до уничтожения прежнего ядра. */
};
static struct restart_request* g_restart_config; /* Kotlin atomic-xchg → worker atomic-xchg (ownership transfer) */
static volatile int          g_running = 0;         /* C-write, Kotlin-read */
static int                   g_thread_joinable;    /* Только controller под g_control_mutex, до успешного join. */
static int                   g_thread_exited;      /* Worker/controller под g_stop_mutex, после освобождения ресурсов. */
static instance_lite_event_fn g_event_handler = NULL; /* Kotlin-write, C-read+call */
static pthread_mutex_t g_control_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_stop_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_stop_cond  = PTHREAD_COND_INITIALIZER;

/* Отдельная запись переживает отмену posted_task при destroy; arg освобождает callback либо discard. */
struct frontend_request {
    struct frontend_request* next;
    void (*callback)(void*);
    void (*discard)(void*);
    void* arg;
    struct UTUN_INSTANCE* instance; /* Привязка к публикации; pending restart закрывает ready, но ещё не уничтожает instance. */
};
static struct frontend_request* g_frontend_requests; /* Список под g_stop_mutex. */

static void frontend_request_run(void* arg) {
    struct frontend_request* request = arg;
    pthread_mutex_lock(&g_stop_mutex);
    struct frontend_request** entry = &g_frontend_requests;
    while (*entry && *entry != request) entry = &(*entry)->next;
    if (*entry) *entry = request->next;
    int cancelled = __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_inst, __ATOMIC_ACQUIRE) != request->instance;
    pthread_mutex_unlock(&g_stop_mutex);
    if (!cancelled) request->callback(request->arg);
    else if (request->discard) request->discard(request->arg);
    else u_free(request->arg);
    u_free(request);
}

/* Только после destroy uasync: ни один callback из снятого списка уже не может начаться. */
static void cancel_frontend_requests(void) {
    pthread_mutex_lock(&g_stop_mutex);
    struct frontend_request* request = g_frontend_requests;
    g_frontend_requests = NULL;
    pthread_mutex_unlock(&g_stop_mutex);
    unsigned cancelled = 0;
    while (request) {
        struct frontend_request* next = request->next;
        if (request->discard) request->discard(request->arg); else u_free(request->arg);
        u_free(request); request = next; cancelled++;
    }
    if (cancelled) DEBUG_INFO(DEBUG_CATEGORY_SYS, "Android pending frontend requests cancelled count=%u", cancelled);
}

int instance_lite_post(void (*callback)(void*), void* arg, struct UTUN_INSTANCE** instance_field, void (*discard)(void*)) {
    if (!callback) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Android frontend request has no callback"); return -1; }
    struct frontend_request* request = u_malloc(sizeof(*request));
    struct posted_task* task = u_malloc(sizeof(*task));
    if (!request || !task) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Android frontend request allocation failed"); u_free(request); u_free(task); return -1;
    }
    request->callback = callback; request->discard = discard; request->arg = arg;
    task->callback = frontend_request_run; task->arg = request;
    pthread_mutex_lock(&g_stop_mutex);
    struct UASYNC* ua = __atomic_load_n(&g_ua, __ATOMIC_ACQUIRE);
    struct UTUN_INSTANCE* inst = __atomic_load_n(&g_inst, __ATOMIC_ACQUIRE);
    if (!ua || !inst || !__atomic_load_n(&g_running, __ATOMIC_ACQUIRE) || __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&g_stop_mutex);
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "Android frontend request refused: worker not ready");
        u_free(task); u_free(request); return -1;
    }
    if (instance_field) *instance_field = inst;
    request->instance = inst;
    request->next = g_frontend_requests; g_frontend_requests = request;
    uasync_post_reserved(ua, task);
    pthread_mutex_unlock(&g_stop_mutex);
    return 0;
}

struct frontend_call {
    pthread_mutex_t mutex;
    pthread_cond_t completed;
    void (*callback)(void*);
    instance_lite_deferred_fn deferred;
    void* arg;
    int result; /* 1 — ожидание, 0 — выполнено, -1 — отменено. */
};

static void frontend_call_complete(struct frontend_call* call, int result) {
    pthread_mutex_lock(&call->mutex);
    call->result = result;
    pthread_cond_signal(&call->completed);
    pthread_mutex_unlock(&call->mutex);
}
static void frontend_call_reply(void* arg) { frontend_call_complete(arg, 0); }

static void frontend_call_run(void* arg) {
    struct frontend_call* call = arg;
    if (call->deferred) { call->deferred(call->arg, frontend_call_reply, call); return; }
    call->callback(call->arg);
    frontend_call_complete(call, 0);
}

static void frontend_call_cancel(void* arg) { frontend_call_complete(arg, -1); }

/* Единственное ожидание находится у frontend; mutex не удерживается во время работы uasync/DB. */
static int frontend_call_wait(struct frontend_call* call, int result) {
    if (!result) {
        pthread_mutex_lock(&call->mutex);
        while (call->result == 1) pthread_cond_wait(&call->completed, &call->mutex);
        result = call->result;
        pthread_mutex_unlock(&call->mutex);
    }
    pthread_cond_destroy(&call->completed);
    pthread_mutex_destroy(&call->mutex);
    return result;
}
int instance_lite_call(void (*callback)(void*), void* arg) {
    if (!callback) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Android frontend call has no callback"); return -1; }
    if (g_is_worker) { callback(arg); return 0; }
    struct frontend_call call = {.mutex=PTHREAD_MUTEX_INITIALIZER, .completed=PTHREAD_COND_INITIALIZER,
        .callback=callback, .arg=arg, .result=1};
    return frontend_call_wait(&call, instance_lite_post(frontend_call_run, &call, NULL, frontend_call_cancel));
}
int instance_lite_call_deferred(instance_lite_deferred_fn begin, void* arg) {
    if (!begin || g_is_worker) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Android deferred call requires callback and a non-worker caller"); return -1;
    }
    struct frontend_call call = {.mutex=PTHREAD_MUTEX_INITIALIZER, .completed=PTHREAD_COND_INITIALIZER,
        .deferred=begin, .arg=arg, .result=1};
    return frontend_call_wait(&call, instance_lite_post(frontend_call_run, &call, NULL, frontend_call_cancel));
}

/* Генерируем отдельно от config: не публикуем в памяти ключи, которые не удалось сохранить. */
static int generate_keys(const char* directory, struct global_config* config) {
    if (!directory || !*directory || strlen(directory) >= sizeof(config->db_path)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity generation requires a database directory"); return -1;
    }
    uint8_t public_x[32], seed[32];
    if (sc_identity_generate_config(public_x, seed) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Android identity key generation failed"); return -1;
    }
    char pub[65], priv[65];
    bytes_to_hex(public_x, sizeof(public_x), pub, sizeof(pub));
    bytes_to_hex(seed, sizeof(seed), priv, sizeof(priv));
    char filename[sizeof(config->db_path) + sizeof("/keys.conf")];
    snprintf(filename, sizeof(filename), "%s/keys.conf", directory);
    OPENSSL_cleanse(seed, sizeof(seed));
    struct file_io_temp output = {0};
    int published = 0;
    const char* phase = "open";
    if (file_io_temp_open(&output, filename, FILE_IO_DURABLE)) goto failed;
    phase = "write";
    if (fprintf(output.file, "public_key=%s\nprivate_key=%s\n", pub, priv) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity write failed path=%s errno=%d: %s", filename, errno, strerror(errno));
        goto failed;
    }
    phase = "finish";
    if (file_io_temp_finish(&output)) goto failed;
    phase = "publish";
    if (file_io_temp_publish(&output, output.temporary, filename)) goto failed;
    file_io_temp_discard(&output);
    if (config) {
        memcpy(config->my_public_key_hex, pub, sizeof(pub));
        memcpy(config->my_private_key_hex, priv, sizeof(priv));
    }
    OPENSSL_cleanse(priv, sizeof(priv));
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Android device identity saved path=%s", filename);
    return 0;
failed:
    OPENSSL_cleanse(priv, sizeof(priv));
    published = !strcmp(phase, "publish") && !output.temporary[0];
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity save failed path=%s phase=%s published=%d",
                filename, phase, published);
    file_io_temp_discard(&output);
    return published ? 1 : -1;
}

int instance_lite_generate_keys(struct utun_config* config) {
    if (!config) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity generation requires config"); return -1; }
    return generate_keys(config->global.db_path, &config->global) ? -1 : 0;
}

int instance_lite_replace_stopped_keys(const char* directory) {
    if (g_is_worker) { IL_LOGE("key replacement refused from native worker"); return -1; }
    pthread_mutex_lock(&g_control_mutex);
    int result = -1;
    if (g_thread_joinable) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Android identity replacement requires joined worker");
    else result = generate_keys(directory, NULL);
    pthread_mutex_unlock(&g_control_mutex);
    return result;
}

/* ── Ensure valid keys in config, generate if missing ── */

static int ensure_keys(struct utun_config* config, const char** reason) {
    *reason = "Cannot read device identity";
    char filename[sizeof(config->global.db_path) + sizeof("/keys.conf")];
    snprintf(filename, sizeof(filename), "%s/keys.conf", config->global.db_path);
    struct file_io_info info;
    int status = config->global.db_path[0] ? file_io_inspect(filename, 0, &info) : 1;
    if (status < 0) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity inspection failed path=%s", filename); return CHAT_STARTUP_KEY_ACCESS; }
    if (!status) {
        if (info.type != FILE_IO_REGULAR) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity is not a regular file path=%s type=%d", filename, info.type); return CHAT_STARTUP_KEY_ACCESS;
        }
        FILE* file = file_io_open_read(filename);
        if (!file) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity read failed path=%s", filename); return CHAT_STARTUP_KEY_ACCESS; }
        char public_line[sizeof("public_key=") + SC_PUBKEY_SIZE * 2 + 1] = {0};
        char private_line[sizeof("private_key=") + SC_PRIVKEY_SIZE * 2 + 1] = {0};
        int complete = fgets(public_line, sizeof(public_line), file) && fgets(private_line, sizeof(private_line), file) && fgetc(file) == EOF;
        int read_error = ferror(file), closed = fclose(file);
        if (read_error || closed) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity read failed path=%s read_error=%d close_error=%d", filename, read_error, closed);
            return CHAT_STARTUP_KEY_ACCESS;
        }
        if (!complete || strlen(public_line) != sizeof(public_line) - 1 ||
            strlen(private_line) != sizeof(private_line) - 1 || public_line[sizeof(public_line) - 2] != '\n' ||
            private_line[sizeof(private_line) - 2] != '\n' ||
            strncmp(public_line, "public_key=", sizeof("public_key=") - 1) ||
            strncmp(private_line, "private_key=", sizeof("private_key=") - 1)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity read/format failed path=%s complete=%d read_error=%d close_error=%d",
                        filename, complete, read_error, closed);
            *reason = "Invalid keys.conf format"; return CHAT_STARTUP_INVALID_KEY;
        }
        public_line[sizeof(public_line) - 2] = private_line[sizeof(private_line) - 2] = '\0';
        memcpy(config->global.my_public_key_hex, public_line + sizeof("public_key=") - 1, SC_PUBKEY_SIZE * 2 + 1);
        memcpy(config->global.my_private_key_hex, private_line + sizeof("private_key=") - 1, SC_PRIVKEY_SIZE * 2 + 1);
    }
    if (!config->global.my_private_key_hex[0] && !config->global.my_public_key_hex[0]) {
        IL_LOGI("config identity absent, generating device Ed25519 seed");
        *reason = "Cannot create device identity";
        return instance_lite_generate_keys(config) ? CHAT_STARTUP_KEY_ACCESS : 0;
    }
    struct SC_MYKEYS keys;
    uint8_t ed[32], seed[32];
    int rc = sc_identity_load(config->global.my_public_key_hex, config->global.my_private_key_hex, &keys, ed, seed);
    OPENSSL_cleanse(&keys, sizeof(keys)); OPENSSL_cleanse(seed, sizeof(seed));
    if (rc != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Android identity validation failed path=%s persisted=%d rc=%d", filename, !status, rc);
        if (rc == SC_ERR_CRYPTO) { *reason = "Cannot initialize device cryptography"; return -1; }
        *reason = "Invalid device identity"; return CHAT_STARTUP_INVALID_KEY;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Android identity ready path=%s persisted=%d", filename, !status);
    return 0;
}

/* ── Chat event forward → Kotlin callback ── */

static void chat_event_forward(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    instance_lite_event_fn handler = __atomic_load_n(&g_event_handler, __ATOMIC_ACQUIRE);
    if (handler) handler(type, data, len);
}

/* Путь передаётся отдельно от текста: GUI очищает только каталог отказавшего запуска. */
static void report_startup_failure(int kind, const char* directory, const char* reason) {
    size_t dir_len = strlen(directory), reason_len = strlen(reason);
    uint8_t payload[3 + dir_len + reason_len]; // INI ограничивает каталог 255 байтами, текст ошибки — 2047.
    payload[0] = kind;
    payload[1] = (uint8_t)dir_len; payload[2] = (uint8_t)(dir_len >> 8);
    memcpy(payload + 3, directory, dir_len); memcpy(payload + 3 + dir_len, reason, reason_len);
    chat_event_forward(NULL, CHAT_EVT_STARTUP_FAILED, payload, (int)sizeof(payload));
}

static void schema_startup_failure(const struct utun_config* config, const struct UTUN_INSTANCE* inst) {
    const char* reason = inst ? inst->database_error : db_schema_last_error();
    int incompatible = inst ? inst->database_incompatible : db_schema_is_incompatible();
    report_startup_failure(incompatible ? CHAT_STARTUP_DATABASE : CHAT_STARTUP_ERROR, config->global.db_path,
                          *reason ? reason : "Core initialization failed");
}

struct lite_startup_result { int ready; struct db_async_result result; };
static void lite_startup_done(void* arg, const struct db_async_result* result) {
    struct lite_startup_result* state = arg; state->result = *result; state->ready = 1;
}
static void lite_core_ready(void* arg, struct UTUN_INSTANCE* inst, const struct db_async_result* result) {
    (void)inst; lite_startup_done(arg, result);
}
/* Только внешний цикл worker_main до публикации ready; SQL в отдельном потоке, uasync продолжает работать.
 * Дожидаемся done также при stop: callback хранит указатель на локальный state до завершения DB. */
static struct UTUN_INSTANCE* lite_create_core(struct UASYNC* ua, struct utun_config* config) {
    struct lite_startup_result state = {0};
    struct UTUN_INSTANCE* inst = utun_instance_create_from_config_async(ua, config, lite_core_ready, &state);
    if (!inst) return NULL;
    while (!state.ready) uasync_poll(ua, 50);
    if (state.result.status) IL_LOGE("database bootstrap failed status=%d rc=%d: %s", state.result.status,
        state.result.sqlite_code, state.result.message);
    return inst;
}
static int lite_start_chat(struct UTUN_INSTANCE* inst) {
    struct lite_startup_result state = {0}; chat_service_start_async(inst, lite_startup_done, &state);
    while (!state.ready) uasync_poll(inst->ua, 50);
    if (state.result.status) IL_LOGE("chat startup failed status=%d rc=%d: %s", state.result.status,
        state.result.sqlite_code, state.result.message);
    return state.result.status ? -1 : 0;
}

static void nodeinfo_event_cb(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* nq, void* arg) {
    (void)group; (void)arg;
    instance_lite_event_fn handler = __atomic_load_n(&g_event_handler, __ATOMIC_ACQUIRE);
    if (!nq || !handler) return;
    handler(CHAT_EVT_NODEINFO_UPDATED, NULL, 0);
}

static void fire_local_sockets_event(void) {
    instance_lite_event_fn handler = __atomic_load_n(&g_event_handler, __ATOMIC_ACQUIRE);
    if (!handler) return;
    char* json = utun_bridge_get_local_sockets_json();
    if (!json) return;
    handler(CHAT_EVT_LOCAL_SOCKETS, (const uint8_t*)json, (int)strlen(json));
    u_free(json);
}

static void on_link_status_changed(struct ETCP_CONN* conn, struct ETCP_LINK* link,
                                    int old_state, int old_status, void* arg) {
    (void)old_state; (void)old_status; (void)arg;
    instance_lite_event_fn handler = __atomic_load_n(&g_event_handler, __ATOMIC_ACQUIRE);
    if (!handler || !conn || !link) return;
    if (!memcmp(conn->peer_ed25519_pubkey, (uint8_t[32]){0}, 32)) { fire_local_sockets_event(); return; }
    uint8_t data[37];
    memcpy(data, conn->peer_ed25519_pubkey, 32);
    data[32] = link->local_link_id;
    data[33] = link->link_state;
    data[34] = link->link_status;
    data[35] = conn->links_up;
    data[36] = conn->initialized ? 1 : 0;
    handler(CHAT_EVT_LINK_STATUS_CHANGED, data, sizeof(data));
    fire_local_sockets_event();
}

/* ── Thread function ── */
static void log_active_sockets(struct UTUN_INSTANCE* inst, const char* label);

/* Только uasync; частичный attach всегда закрывается общим detach до уничтожения ядра. */
static int attach_media(struct UTUN_INSTANCE* inst) {
    if (android_stream_attach(inst) < 0 || call_video_port_attach(inst) < 0) return -1;
#ifdef UTUN_WITH_VOICE
    if (call_audio_init(inst) < 0) return -1;
#endif
    return 0;
}

#ifdef __ANDROID__
#include "../jni_bridge/android_ble.h"
#endif
static void detach_media(struct UTUN_INSTANCE* inst) {
#ifdef __ANDROID__
    android_ble_detach();
#endif
#ifdef UTUN_WITH_VOICE
    call_audio_destroy(inst);
#endif
    call_video_port_detach(inst);
    android_stream_detach(inst);
}

static void* instance_thread(void* arg) {
    g_is_worker = 1;
    char* config_text = (char*)arg;
    struct UASYNC* my_ua = NULL;
    struct UTUN_INSTANCE* my_inst = NULL;

    struct utun_config* config = parse_config_from_buf(config_text, strlen(config_text), "android");
    u_free(config_text);
    if (!config) {
        IL_LOGE("parse_config_from_buf failed");
        report_startup_failure(CHAT_STARTUP_ERROR, "", "Invalid core configuration"); goto cleanup;
    }
    config->global.client_type = CLIENT_TYPE_MOBILE;   /* Android — всегда мобильный узел (keepalive standby/normal) */

    const char* key_error;
    int key_status = ensure_keys(config, &key_error);
    if (key_status) {
        IL_LOGE("startup refused: %s", key_error);
        report_startup_failure(key_status < 0 ? CHAT_STARTUP_ERROR : key_status, config->global.db_path, key_error);
        free_config(config);
        goto cleanup;
    }
    if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) { free_config(config); goto cleanup; }

    my_ua = uasync_create();
    if (!my_ua) {
        IL_LOGE("uasync_create failed"); report_startup_failure(CHAT_STARTUP_ERROR, config->global.db_path, "Core event loop unavailable");
        free_config(config); goto cleanup;
    }
    pthread_mutex_lock(&g_stop_mutex);
    __atomic_store_n(&g_ua, my_ua, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_stop_mutex);
    standby_init(g_ua);
    if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) { free_config(config); goto cleanup; }

    udp_log_set_target(config->global.log_udp_ip, config->global.log_udp_port);
    udp_log_attach(g_ua);

    my_inst = lite_create_core(g_ua, config);
    __atomic_store_n(&g_inst, my_inst, __ATOMIC_RELEASE);
    if (!my_inst) {
        IL_LOGE("utun_instance_create_from_config failed");
        schema_startup_failure(config, NULL);
        free_config(config);
        goto cleanup;
    }
    if (!my_inst->database_ready) { schema_startup_failure(config, my_inst); goto cleanup; }
    chat_event_set_handler(g_inst, chat_event_forward);
    if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) goto cleanup;

    IL_LOGI("instance created, node_id=0x%016llx", (unsigned long long)g_inst->node_id);

    IL_LOGI("service policy: utun_enabled=%d tun_enabled=%d", config->global.gui_utun_enabled, config->global.tun_enabled);
    if (utun_core_start(g_inst) != 0 || (config->global.gui_utun_enabled && utun_service_start(g_inst) != 0) ||
        lite_start_chat(g_inst) != 0 || kanban_service_start(g_inst) != 0 || attach_media(g_inst) < 0) {
        IL_LOGE("core/optional UTUN/chat start failed");
        report_startup_failure(CHAT_STARTUP_ERROR, config->global.db_path, "Could not start core services");
        goto cleanup;
    }

    bind_join_port(g_inst);
    /* sockets are now created — sync my addresses to DB */
    chat_core_sync_my_addresses(g_inst);
    fire_local_sockets_event();

    utun_add_nodeinfo_cbk(g_inst, nodeinfo_event_cb, NULL);
    etcp_add_link_status_cbk(g_inst, on_link_status_changed, NULL);

    /* Set my_name from config */
    if (g_inst->config->global.name[0]) {
        chat_core_update_my_name(g_inst, g_inst->config->global.name);
        IL_LOGI("my_name set to '%s'", g_inst->config->global.name);
    }

    IL_LOGI("all init done, entering poll loop");

    pthread_mutex_lock(&g_stop_mutex);
    if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) { pthread_mutex_unlock(&g_stop_mutex); goto cleanup; }
    /* Notify GUI of current public key (initial generation or existing). Stop не может пересечь публикацию ready. */
    if (strlen(config->global.my_public_key_hex) == 64) {
        chat_event_post(g_inst, CHAT_EVT_KEYS_GENERATED,
                        (const uint8_t*)config->global.my_public_key_hex, 64);
    }

    uasync_mark_running(g_ua);
    if (!__atomic_load_n(&g_restart_config, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_running, 1, __ATOMIC_RELEASE);
        chat_event_post(g_inst, CHAT_EVT_SERVICE_STARTED, NULL, 0);
    }
    pthread_mutex_unlock(&g_stop_mutex);

poll_current:
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&g_restart_config, __ATOMIC_ACQUIRE) && g_inst->running) {
        uasync_poll(g_ua, -1);
    }
    if (!g_inst->running) IL_LOGE("poll exit: core requested stop; cleanup runs outside callbacks");

    struct restart_request* request;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) &&
           (request = __atomic_exchange_n(&g_restart_config, NULL, __ATOMIC_ACQUIRE))) {
        int regenerate_keys = request->regenerate_keys;
        struct utun_config* config = parse_config_from_buf(request->config_text, strlen(request->config_text), "android-restart");
        u_free(request->config_text);
        u_free(request);
        if (!config) { IL_LOGE("restart refused: invalid config, current core retained"); goto restart_refused; }
        if (regenerate_keys ? instance_lite_generate_keys(config) : ensure_keys(config, &key_error)) {
            IL_LOGE("restart refused: Android identity is not saved, current core retained");
            free_config(config);
            goto restart_refused;
        }
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) { free_config(config); break; }
        config->global.client_type = CLIENT_TYPE_MOBILE;
        log_active_sockets(g_inst, "restart OLD sockets");
        bind_join_port(NULL);
        IL_LOGI("poll exit: restart requested, destroying old instance");
        IL_LOGI("pre-destroy: g_inst=%p node_id=0x%016llx ua=%p running=%d sockets=%p conns=%p",
                g_inst, g_inst ? (unsigned long long)g_inst->node_id : 0ULL,
                g_ua, g_running,
                g_inst ? g_inst->etcp_sockets : NULL,
                g_inst ? g_inst->connections : NULL);
        uasync_mark_stopped(g_ua);
        pthread_mutex_lock(&g_stop_mutex);
        __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&g_stop_mutex);
        chat_event_post(g_inst, CHAT_EVT_SERVICE_STOPPED, NULL, 0);
        struct UTUN_INSTANCE* old_inst = g_inst;
        __atomic_store_n(&g_inst, NULL, __ATOMIC_RELEASE);
        detach_media(old_inst);
        utun_instance_destroy(old_inst);
        my_inst = NULL;
        standby_deinit();
        udp_log_detach(g_ua);
        pthread_mutex_lock(&g_stop_mutex);
        __atomic_store_n(&g_ua, NULL, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&g_stop_mutex);
        uasync_destroy(my_ua, 0);
        my_ua = NULL;
        cancel_frontend_requests();
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) { free_config(config); break; }
        my_ua = uasync_create();
        if (!my_ua) { IL_LOGE("poll exit: uasync_create failed"); free_config(config); break; }
        pthread_mutex_lock(&g_stop_mutex);
        __atomic_store_n(&g_ua, my_ua, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&g_stop_mutex);
        standby_init(g_ua);
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) { free_config(config); break; }
        IL_LOGI("poll exit: creating new instance from restart config");
        udp_log_set_target(config->global.log_udp_ip, config->global.log_udp_port);
        udp_log_attach(g_ua);

        my_inst = lite_create_core(g_ua, config);
        __atomic_store_n(&g_inst, my_inst, __ATOMIC_RELEASE);
        if (!my_inst) {
            IL_LOGE("poll exit: create_from_config failed");
            schema_startup_failure(config, NULL);
            free_config(config); break;
        }
        if (!my_inst->database_ready) { schema_startup_failure(config, my_inst); break; }
        chat_event_set_handler(g_inst, chat_event_forward);
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) break;

        IL_LOGI("restart service policy: utun_enabled=%d tun_enabled=%d", config->global.gui_utun_enabled, config->global.tun_enabled);
        if (utun_core_start(g_inst) != 0 || (config->global.gui_utun_enabled && utun_service_start(g_inst) != 0) ||
            lite_start_chat(g_inst) != 0 || kanban_service_start(g_inst) != 0 || attach_media(g_inst) < 0) {
            IL_LOGE("poll exit: core/chat/stream bridge start failed"); break;
        }
        bind_join_port(g_inst);
        chat_core_sync_my_addresses(g_inst);
        fire_local_sockets_event();
        if (g_inst->config->global.name[0]) chat_core_update_my_name(g_inst, g_inst->config->global.name);
        utun_add_nodeinfo_cbk(g_inst, nodeinfo_event_cb, NULL);
        etcp_add_link_status_cbk(g_inst, on_link_status_changed, NULL);
        pthread_mutex_lock(&g_stop_mutex);
        if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) { pthread_mutex_unlock(&g_stop_mutex); break; }
        chat_event_post(g_inst, CHAT_EVT_KEYS_GENERATED, (const uint8_t*)config->global.my_public_key_hex, 64);
        uasync_mark_running(g_ua);
        if (!__atomic_load_n(&g_restart_config, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_running, 1, __ATOMIC_RELEASE);
            chat_event_post(g_inst, CHAT_EVT_SERVICE_STARTED, NULL, 0);
        }
        pthread_mutex_unlock(&g_stop_mutex);

        IL_LOGI("poll exit: restart done, entering new poll loop");
        while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) &&
               !__atomic_load_n(&g_restart_config, __ATOMIC_ACQUIRE) && g_inst->running) uasync_poll(g_ua, -1);
        if (!g_inst->running) IL_LOGE("poll exit: restarted core requested stop; cleanup runs outside callbacks");
    }

cleanup:
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Android worker cleanup: instance=%p loop=%p", (void*)my_inst, (void*)my_ua);
    pthread_mutex_lock(&g_stop_mutex);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_stop_mutex);
    bind_join_port(NULL);
    if (my_inst) {
        chat_event_post(my_inst, CHAT_EVT_SERVICE_STOPPED, NULL, 0);
        __atomic_store_n(&g_inst, NULL, __ATOMIC_RELEASE);
        detach_media(my_inst);
        utun_instance_destroy(my_inst);
    }
    if (my_ua) {
        standby_deinit();
        udp_log_detach(my_ua);
        pthread_mutex_lock(&g_stop_mutex);
        __atomic_store_n(&g_ua, NULL, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&g_stop_mutex);
        uasync_destroy(my_ua, 0);
    }
    cancel_frontend_requests();
    struct restart_request* stale = __atomic_exchange_n(&g_restart_config, NULL, __ATOMIC_ACQUIRE);
    if (stale) { u_free(stale->config_text); u_free(stale); }
    pthread_mutex_lock(&g_stop_mutex);
    g_thread_exited = 1;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Android worker finished: all native resources released");
    pthread_cond_signal(&g_stop_cond);
    pthread_mutex_unlock(&g_stop_mutex);
    return NULL;

restart_refused:
    pthread_mutex_lock(&g_stop_mutex);
    if (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) && !__atomic_load_n(&g_restart_config, __ATOMIC_ACQUIRE) && g_inst->running) {
        __atomic_store_n(&g_running, 1, __ATOMIC_RELEASE);
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "Android retained core ready after rejected restart");
        chat_event_post(g_inst, CHAT_EVT_SERVICE_STARTED, NULL, 0);
    }
    pthread_mutex_unlock(&g_stop_mutex);
    goto poll_current;
}

/* ── Public API ── */

/* g_control_mutex удерживается caller: завершившийся pthread надо собрать до следующего create. */
static int start_locked(const char* config_text) {
    if (!config_text) { IL_LOGE("start refused: config is NULL"); return -1; }
    if (g_thread_joinable) {
        pthread_mutex_lock(&g_stop_mutex);
        int exited = g_thread_exited;
        pthread_mutex_unlock(&g_stop_mutex);
        if (!exited) {
            if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) { IL_LOGE("start refused: previous worker is stopping"); return -1; }
            return 0;
        }
        int result = pthread_join(g_thread, NULL);
        if (result) { IL_LOGE("start refused: previous worker join failed rc=%d", result); return -1; }
        g_thread_joinable = 0;
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "Android previous worker joined before start");
    }

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_DUMP, DEBUG_LEVEL_WARN);
    debug_enable_function_name(0);

    char* config_copy = u_strdup(config_text);
    if (!config_copy) { IL_LOGE("start refused: config allocation failed"); return -1; }

    pthread_mutex_lock(&g_stop_mutex);
    g_thread_exited = 0;
    pthread_mutex_unlock(&g_stop_mutex);
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
    int rc = pthread_create(&g_thread, NULL, instance_thread, config_copy);
    if (rc != 0) {
        u_free(config_copy);
        IL_LOGE("pthread_create failed rc=%d", rc);
        return -1;
    }
    g_thread_joinable = 1;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Android worker created");
    return 0;
}

int instance_lite_start(const char* config_text) {
    if (g_is_worker) { IL_LOGE("start refused: must run outside the native worker"); return -1; }
    pthread_mutex_lock(&g_control_mutex);
    int result = start_locked(config_text);
    pthread_mutex_unlock(&g_control_mutex);
    return result;
}

int instance_lite_stop(void) {
    if (g_is_worker) { IL_LOGE("stop refused: must run outside the native worker"); return -1; }
    pthread_mutex_lock(&g_control_mutex);
    if (!g_thread_joinable) { pthread_mutex_unlock(&g_control_mutex); return 0; }
    bind_join_port(NULL);
    pthread_mutex_lock(&g_stop_mutex);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
    struct UASYNC* ua = (struct UASYNC*)__atomic_load_n(&g_ua, __ATOMIC_ACQUIRE);
    if (ua) uasync_wakeup(ua);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Android worker stop requested: exited=%d loop=%p", g_thread_exited, (void*)ua);
    while (!g_thread_exited) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        int rc = pthread_cond_timedwait(&g_stop_cond, &g_stop_mutex, &ts);
        if (rc == ETIMEDOUT) {
            DEBUG_WARN(DEBUG_CATEGORY_SYS, "Android worker still stopping after 2s; resources retained until join");
        } else if (rc) {
            IL_LOGE("worker condition wait failed rc=%d; waiting with pthread_join", rc);
            break;
        }
    }
    pthread_mutex_unlock(&g_stop_mutex);

    int result = pthread_join(g_thread, NULL);
    if (result) IL_LOGE("worker join failed rc=%d", result);
    else {
        g_thread_joinable = 0;
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "Android worker joined; stop completed");
    }
    pthread_mutex_unlock(&g_control_mutex);
    return result ? -1 : 0;
}

/* Логирует текущие активные сокеты (имя=ip:port(протокол)) — состояние ДО изменения. */
static void log_active_sockets(struct UTUN_INSTANCE* inst, const char* label) {
    if (!inst) { DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "%s: (no instance)", label); return; }
    char buf[768] = "";
    int off = 0;
    for (struct ETCP_SOCKET* s = inst->etcp_sockets; s; s = s->next) {
        const struct sockaddr_storage* a = s->interface_addr.ss_family ? &s->interface_addr : &s->local_addr;
        const char* addr = sockaddr_storage_to_str(a).str;
        if (off < (int)sizeof(buf) - 96)
            off += snprintf(buf + off, sizeof(buf) - (size_t)off, " %s=%s(%s)",
                            s->name, addr, s->is_tcp ? "TCP" : "UDP");
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "%s:%s", label, buf[0] ? buf : " (none)");
}

/* Логирует [server:...]/addr= секции нового конфига — состояние ПОСЛЕ изменения. */
static void log_config_servers(const char* cfg, const char* label) {
    char buf[768] = "";
    int off = 0;
    char srvname[64] = "";
    const char* p = cfg;
    while (p && *p) {
        if (strncmp(p, "[server:", 8) == 0) {
            const char* end = strchr(p, ']');
            if (end && end > p + 8) {
                int n = (int)(end - p) - 8; if (n > 63) n = 63;
                memcpy(srvname, p + 8, (size_t)n); srvname[n] = '\0';
            } else srvname[0] = '\0';
        } else if (strncmp(p, "addr=", 5) == 0 && srvname[0]) {
            const char* eol = p; while (*eol && *eol != '\n' && *eol != '\r') eol++;
            int alen = (int)(eol - p) - 5; char addr[128];
            if (alen < 1) alen = 0; if (alen > 127) alen = 127;
            memcpy(addr, p + 5, (size_t)alen); addr[alen] = '\0';
            if (off < (int)sizeof(buf) - 96)
                off += snprintf(buf + off, sizeof(buf) - (size_t)off, " %s=%s", srvname, addr);
            srvname[0] = '\0';
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "%s:%s", label, buf[0] ? buf : " (none)");
}

static int request_restart(const char* new_config_text, int regenerate_keys) {
    if (g_is_worker) { IL_LOGE("restart refused: must run outside the native worker"); return -1; }
    if (!new_config_text) { IL_LOGE("restart refused: config is NULL"); return -1; }
    pthread_mutex_lock(&g_control_mutex);
    pthread_mutex_lock(&g_stop_mutex);
    int alive = g_thread_joinable && !g_thread_exited;
    pthread_mutex_unlock(&g_stop_mutex);
    int running = __atomic_load_n(&g_running, __ATOMIC_ACQUIRE);
    log_config_servers(new_config_text, "restart NEW config");
    if (regenerate_keys && !running) {
        IL_LOGE("key regeneration refused: core is not ready"); pthread_mutex_unlock(&g_control_mutex); return -1;
    }
    if (!alive) {
        int result = start_locked(new_config_text);
        pthread_mutex_unlock(&g_control_mutex);
        return result;
    }
    struct restart_request* next = u_calloc(1, sizeof(*next));
    if (!next) { IL_LOGE("restart refused: request allocation failed"); pthread_mutex_unlock(&g_control_mutex); return -1; }
    next->config_text = u_strdup(new_config_text);
    if (!next->config_text) {
        IL_LOGE("restart refused: config allocation failed"); u_free(next); pthread_mutex_unlock(&g_control_mutex); return -1;
    }
    next->regenerate_keys = regenerate_keys;
    pthread_mutex_lock(&g_stop_mutex);
    if (__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) || g_thread_exited ||
        (regenerate_keys && !__atomic_load_n(&g_running, __ATOMIC_ACQUIRE))) {
        pthread_mutex_unlock(&g_stop_mutex);
        IL_LOGE("restart refused: worker is stopping or not ready");
        u_free(next->config_text); u_free(next);
        pthread_mutex_unlock(&g_control_mutex);
        return -1;
    }
    struct restart_request* old = __atomic_exchange_n(&g_restart_config, next, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
    if (old) { IL_LOGI("restart: replacing pending request regenerate_keys=%d", old->regenerate_keys); u_free(old->config_text); u_free(old); }
    IL_LOGI("restart: request queued regenerate_keys=%d", regenerate_keys);
    struct UASYNC* ua = __atomic_load_n(&g_ua, __ATOMIC_ACQUIRE);
    if (ua) uasync_wakeup(ua);
    pthread_mutex_unlock(&g_stop_mutex);
    pthread_mutex_unlock(&g_control_mutex);
    return 0;
}

int instance_lite_restart(const char* new_config_text) {
    return request_restart(new_config_text, 0);
}

/* Принимает владение ненулевым task при любом результате. Все данные команды находятся в том же выделении
 * (task — первое поле) либо являются скалярами: uasync/free не должны отдельно освобождать arg.
 * Не заимствует instance; trampoline берёт его только в worker, пока тот исполняет свою очередь. */
int instance_lite_post_frontend(struct posted_task* task, debug_category_t category, const char* name) {
    if (g_is_worker) {
        u_free(task);
        DEBUG_ERROR(category, "Android %s request refused: frontend thread required", name); return -1;
    }
    pthread_mutex_lock(&g_stop_mutex);
    struct UASYNC* ua = __atomic_load_n(&g_ua, __ATOMIC_ACQUIRE);
    if (!ua || !__atomic_load_n(&g_running, __ATOMIC_ACQUIRE) || __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&g_stop_mutex);
        u_free(task);
        DEBUG_DEBUG(category, "Android %s request rejected: core is not ready", name); return -1;
    }
    uasync_post_reserved(ua, task);
    pthread_mutex_unlock(&g_stop_mutex);
    return 0;
}

static void set_active_trampoline(void* arg) {
    struct UTUN_INSTANCE* inst = __atomic_load_n(&g_inst, __ATOMIC_ACQUIRE);
    if (!inst || __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "Android activity command cancelled: worker is stopping"); return;
    }
    int active = (int)(intptr_t)arg;
    if (utun_set_client_activity(inst, active) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Android activity transition failed requested=%d", active); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "Android activity applied requested=%d effective=%u", active, (unsigned)inst->client_activity);
}

int instance_lite_set_active(int active) {
    struct posted_task* task = u_malloc(sizeof(*task));
    if (!task) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Android activity request allocation failed"); return -1;
    }
    task->callback = set_active_trampoline;
    task->arg = (void*)(intptr_t)!!active;
    return instance_lite_post_frontend(task, DEBUG_CATEGORY_SYS, "activity");
}

/* posted — первое поле: uasync освобождает всё выделение и после callback, и при отмене очереди. */
struct phone_state_task {
    struct posted_task posted;
    int state;
    unsigned policy;
};

/* Только worker: request заимствован у исполняемой задачи, instance берём в момент применения. */
static void set_phone_state_trampoline(void* arg) {
    const struct phone_state_task* request = arg;
    struct UTUN_INSTANCE* inst = __atomic_load_n(&g_inst, __ATOMIC_ACQUIRE);
    if (!inst || __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "Android phone state command cancelled: worker is stopping"); return;
    }
    call_set_phone_state(inst, request->state, request->policy);
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "Android phone state applied state=%d policy=%u", request->state, request->policy);
}

int instance_lite_set_phone_state(int state, unsigned policy) {
    if (state < 0 || state > 2 || policy > 31) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Android phone state request invalid state=%d policy=%u", state, policy); return -1;
    }
    struct phone_state_task* request = u_malloc(sizeof(*request));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Android phone state request allocation failed"); return -1;
    }
    request->state = state; request->policy = policy;
    request->posted.callback = set_phone_state_trampoline; request->posted.arg = request;
    return instance_lite_post_frontend(&request->posted, DEBUG_CATEGORY_CALL, "phone state");
}

struct radio_config_request {
    struct posted_task task;                  /* Первое поле: очередь освобождает весь снимок, включая отмену. */
    int64_t request_id;
    int values[INSTANCE_LITE_RADIO_SETTINGS_COUNT];
    instance_lite_radio_configured_fn configured;
};

/* Снимок применяется целиком в потоке ядра, callback разрешает frontend новый capture. */
static void radio_config_apply(void* arg) {
    struct radio_config_request* req = arg;
    static const char* keys[] = {"radio_auto_enabled", "radio_auto_mode", "radio_level_on_dbfs", "radio_level_off_dbfs",
                                 "radio_auto_hangover_ms", "radio_vad_threshold", "radio_vad_compressor_max_gain_db"};
    struct UTUN_INSTANCE* inst = __atomic_load_n(&g_inst, __ATOMIC_ACQUIRE);
    if (!inst || __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "Android radio config cancelled request=%lld: worker is stopping", (long long)req->request_id);
        return;
    }
    int success = 1;
    struct chat_setting_state next = inst->chat_settings;
    for (int i = 0; i < INSTANCE_LITE_RADIO_SETTINGS_COUNT; ++i) {
        char value[16]; snprintf(value, sizeof(value), "%d", req->values[i]);
        if (chat_setting_set_state(&next, keys[i], value) != 0) success = 0;
    }
    if (req->values[3] >= req->values[2]) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config: invalid thresholds on=%d off=%d", req->values[2], req->values[3]);
        success = 0;
    }
    if (success) {
        inst->chat_settings = next;
        if (chat_setting_set(inst, "radio_vad_compressor_max_gain_db", chat_setting_get(inst, "radio_vad_compressor_max_gain_db"))) success = 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "Android radio config request=%lld success=%d enabled=%d mode=%d on=%d off=%d "
               "hangover=%dms threshold=%d%% vad_gain=%ddB", (long long)req->request_id, success, req->values[0], req->values[1],
               req->values[2], req->values[3], req->values[4], req->values[5], req->values[6]);
    req->configured(req->request_id, success);
}

int instance_lite_configure_radio(int64_t request_id, const int* values, instance_lite_radio_configured_fn configured) {
    if (!values || !configured) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config invalid arguments request=%lld", (long long)request_id); return -1;
    }
    struct radio_config_request* req = u_malloc(sizeof(*req));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Android radio config allocation failed"); return -1; }
    req->request_id = request_id; req->configured = configured;
    memcpy(req->values, values, sizeof(req->values));
    req->task.callback = radio_config_apply; req->task.arg = req;
    return instance_lite_post_frontend(&req->task, DEBUG_CATEGORY_RADIO, "radio config");
}

/* ── Лёгкое обновление сокетов (auto_sockets=android) ── */

static void update_sockets_trampoline(void* arg) {
    char* cfg_text = (char*)arg;
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)__atomic_load_n(&g_inst, __ATOMIC_ACQUIRE);
    if (!inst) {
        IL_LOGE("update_sockets: instance gone in trampoline — dropping update");
        u_free(cfg_text);
        return;
    }
    log_active_sockets(inst, "update OLD sockets");
    struct utun_config* cfg = parse_config_from_buf(cfg_text, strlen(cfg_text), "android-update");
    u_free(cfg_text);
    if (!cfg) { IL_LOGE("update_sockets: parse_config_from_buf failed"); return; }
    auto_socket_reload_config(inst, cfg);   /* cfg владеет auto_socket: servers переносятся, остальное освобождается */
}

int instance_lite_update_sockets(const char* config_text) {
    if (!config_text) { IL_LOGE("update_sockets: config is NULL"); return -1; }
    log_config_servers(config_text, "update NEW config");
    char* copy = u_strdup(config_text);
    if (!copy) { IL_LOGE("update_sockets: config allocation failed"); return -1; }
    if (instance_lite_post(update_sockets_trampoline, copy, NULL, NULL)) {
        IL_LOGE("update_sockets: core not ready, update rejected"); u_free(copy); return -1;
    }
    return 0;
}

/* ── Health check ping ── */

static uint64_t g_ping_id;
static uint64_t g_pong_id;

static void ping_trampoline(void* arg) {
    __atomic_store_n(&g_pong_id, *(uint64_t*)arg, __ATOMIC_RELEASE);
    u_free(arg);
}

void instance_lite_ping(void) {
    if (!instance_lite_is_running()) return;
    uint64_t* id = u_malloc(sizeof(*id));
    if (!id) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Android health ping allocation failed"); return; }
    *id = __atomic_add_fetch(&g_ping_id, 1, __ATOMIC_ACQ_REL);
    if (instance_lite_post(ping_trampoline, id, NULL, NULL)) u_free(id);
}

int instance_lite_is_responsive(void) {
    uint64_t ping = __atomic_load_n(&g_ping_id, __ATOMIC_ACQUIRE);
    return instance_lite_is_running() && ping && __atomic_load_n(&g_pong_id, __ATOMIC_ACQUIRE) == ping;
}

static void collect_conn_list_run(void* unused) {
    (void)unused;
    chat_core_collect_conn_list_trampoline(instance_lite_get_instance());
}

void instance_lite_collect_conn_list(void) {
    if (instance_lite_post(collect_conn_list_run, NULL, NULL, NULL))
        DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "Android connection list request rejected: core not ready");
}

void instance_lite_collect_conn_metrics(uint64_t peer_node_id) {
    struct chat_node_arg* arg = (struct chat_node_arg*)u_malloc(sizeof(struct chat_node_arg));
    if (!arg) { DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "Android connection metrics allocation failed"); return; }
    arg->node_id = peer_node_id;
    if (instance_lite_post(chat_core_collect_conn_metrics_trampoline, arg, &arg->inst, NULL)) u_free(arg);
}

int instance_lite_is_running(void) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)__atomic_load_n(&g_inst, __ATOMIC_ACQUIRE);
    return (inst != NULL && __atomic_load_n(&g_running, __ATOMIC_ACQUIRE)) ? 1 : 0;
}

struct UASYNC* instance_lite_get_uasync(void) {
    return (struct UASYNC*)__atomic_load_n(&g_ua, __ATOMIC_ACQUIRE);
}

struct UTUN_INSTANCE* instance_lite_get_instance(void) {
    return (struct UTUN_INSTANCE*)__atomic_load_n(&g_inst, __ATOMIC_ACQUIRE);
}

int instance_lite_regenerate_keys(const char* config_text) {
    return request_restart(config_text, 1);
}

void instance_lite_set_event_handler(instance_lite_event_fn handler) {
    __atomic_store_n(&g_event_handler, handler, __ATOMIC_RELEASE);
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "Android event handler enabled=%d", handler != NULL);
}
