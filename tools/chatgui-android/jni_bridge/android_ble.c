/* Android backend общего BLE C API. JNI события копируются в ограниченный mailbox;
 * eventfd будит единственный uasync owner. Mutex защищает active/очередь/fd от teardown.
 * Java commands: 0 stop, 1 start (bytes=UTF8 filter|mask|name), 2 advertise, 3 connect, 4 close, 5 send, 6 incoming,
 * 7 status UI, 8 advertising UI, 10 update name. Events: 1 ready, 2 opened, 3 received, 4 sent,
 * 5 closed, 6 broadcast, 7 advertising, 8 failed, 10 profile (node BE8 + UTF8 name). token < 0 — входящий Java link.
 * Flags настроек: 1 enabled, 2 advertise, 4 incoming; 8 raw echo/16 dial/32 ETCP echo — instrumentation.
 * JVM class reference живёт до завершения процесса. Все transport handles — только worker. */
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <errno.h>
#include "platform_compat.h"
#include "ble/ble_backend.h"
#include "ble/ble_identity.h"
#include "u_async.h"
#include "mem.h"
#include "debug_config.h"
#include "instance_lite.h"
#include "utun_instance.h"
#include "config_parser.h"
#include "etcp.h"
#include "android_ble.h"
#include "../../cgathui_common/ble_etcp.h"
#define CAT DEBUG_CATEGORY_DISCOVERY
#define EVENTS 256
struct event { jlong token; int type, arg, len; char text[256]; uint8_t data[512]; };
struct android_link { ble_link_id id; jlong token; };
struct ble_backend {
    struct UASYNC* ua;
    struct ble_transport* owner;
    void* io;
    int wake_fd;
    jlong generation;
    struct android_link links[BLE_LINKS_MAX];
    struct event events[EVENTS];
    unsigned head, count;
    int overflow;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct ble_backend* active;
static jlong generation;
static JavaVM* vm;
static jclass radio_class;
static jmethodID command_method;
static struct ble_transport* transport; /* Только worker. */
static struct ble_etcp* driver; /* Общий владелец ETCP и таблицы соседей. */
static struct UTUN_INSTANCE* echo_instance; /* Временный bind только явно включённой instrumentation-пробы. */
static int policy;
static int incoming_policy;
static int radio_ready;
static int probe_trace;
static debug_level_t saved_trace, saved_discovery;
static uint8_t identity[24];
static int identity_len;
static int command(int op, jlong gen, jlong token, const char* text, const uint8_t* bytes, size_t len, int arg) {
    JNIEnv* env = NULL; int attached = 0, ok = 0;
    if (!vm || !radio_class) { DEBUG_ERROR(CAT, "BLE JNI is not installed"); return -1; }
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) { DEBUG_ERROR(CAT, "BLE JNI attach failed"); return -1; }
        attached = 1;
    }
    jstring string = (*env)->NewStringUTF(env, text ? text : "");
    jbyteArray data = string ? (*env)->NewByteArray(env, (jsize)len) : NULL;
    if (string && data) {
        if (len) (*env)->SetByteArrayRegion(env, data, 0, (jsize)len, (const jbyte*)bytes);
        if (!(*env)->ExceptionCheck(env))
            ok = (*env)->CallStaticBooleanMethod(env, radio_class, command_method, op, gen, token, string, data, arg) == JNI_TRUE;
        if (!ok && !(*env)->ExceptionCheck(env)) DEBUG_ERROR(CAT, "BLE radio rejected command op=%d", op);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); ok = 0;
        DEBUG_ERROR(CAT, "BLE JNI command failed op=%d", op);
    }
    if (string) (*env)->DeleteLocalRef(env, string);
    if (data) (*env)->DeleteLocalRef(env, data);
    if (attached && (*vm)->DetachCurrentThread(vm) != JNI_OK) DEBUG_ERROR(CAT, "BLE JNI detach failed op=%d", op);
    return ok ? 0 : -1;
}
static struct android_link* by_id(struct ble_backend* b, ble_link_id id) {
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (b->links[i].id == id) return &b->links[i];
    return NULL;
}
static struct android_link* by_token(struct ble_backend* b, jlong token) {
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (b->links[i].id && b->links[i].token == token) return &b->links[i];
    return NULL;
}
/* JNI держит lock до записи: detach не может закрыть/reuse fd под отправителем. */
static void wake(struct ble_backend* b) {
    uint64_t value = 1; ssize_t n;
    do { n = write(b->wake_fd, &value, sizeof(value)); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN) DEBUG_ERROR(CAT, "BLE eventfd write failed: %s", strerror(errno));
}
/* Одна ограниченная порция событий; eventfd будит uasync сразу, без задержки polling-таймера. */
static void drain(int fd, void* arg) {
    struct ble_backend* b = arg;
    uint64_t value; ssize_t n;
    do { n = read(fd, &value, sizeof(value)); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN) {
        DEBUG_ERROR(CAT, "BLE eventfd read failed: %s", strerror(errno));
        ble_backend_status(b->owner, 0, "eventfd read failed"); return;
    }
    for (int i = 0; i < EVENTS; i++) {
        struct event e; int available, overflow;
        pthread_mutex_lock(&lock);
        available = b->count != 0; overflow = b->overflow; b->overflow = 0;
        if (available) { e = b->events[b->head]; b->head = (b->head + 1) % EVENTS; b->count--; }
        pthread_mutex_unlock(&lock);
        if (overflow) {
            pthread_mutex_lock(&lock);
            unsigned discarded = b->count + available + overflow;
            b->head = b->count = 0;
            pthread_mutex_unlock(&lock);
            DEBUG_ERROR(CAT, "BLE Android event mailbox full generation=%lld capacity=%d discarded=%u",
                        (long long)b->generation, EVENTS, discarded);
            command(0, b->generation, 0, NULL, NULL, 0, 0);
            ble_backend_status(b->owner, 0, "Android event mailbox full"); break;
        }
        if (!available) break;
        struct android_link* l = by_token(b, e.token);
        if (!l && e.type >= 3 && e.type <= 5)
            DEBUG_TRACE(CAT, "BLE retired Android event token=%lld type=%d status=%d bytes=%d", (long long)e.token, e.type, e.arg, e.len);
        struct ble_endpoint ep; memcpy(ep.id, e.text, sizeof(ep.id));
        switch (e.type) {
        case 1: ble_backend_status(b->owner, 1, "Android scan/GATT ready"); break;
        case 9: /* Первая серверная CCC: создать link с общим handshake deadline, ещё без opened. */
            if (!l && e.token < 0) {
                l = by_id(b, 0);
                ble_link_id id = l ? ble_backend_incoming(b->owner, &ep) : 0;
                if (!id) { command(4, b->generation, e.token, NULL, NULL, 0, 0); break; }
                l->id = id; l->token = e.token;
            }
            break;
        case 2:
            if (l) ble_backend_opened(b->owner, l->id, e.arg);
            else command(4, b->generation, e.token, NULL, NULL, 0, 0);
            break;
        case 3: if (l) ble_backend_received(b->owner, l->id, (enum ble_wire_channel)e.arg, e.data, e.len); break;
        case 4: if (l) ble_backend_sent(b->owner, l->id, e.arg == 0); break;
        case 5: if (l) ble_backend_closed(b->owner, l->id, e.text); break;
        case 6: ble_backend_broadcast(b->owner, &ep, e.data, e.len, e.arg); break;
        case 10: if (l && e.len >= BLE_PUBLIC_KEY_SIZE) ble_backend_profile(b->owner, l->id, e.data, BLE_PUBLIC_KEY_SIZE,
                    e.data + BLE_PUBLIC_KEY_SIZE, e.len - BLE_PUBLIC_KEY_SIZE); break;
        case 7: ble_backend_advertising(b->owner, e.arg ? BLE_CLOSED : BLE_READY, e.text); break;
        case 8: ble_backend_status(b->owner, 0, e.text); break;
        default: DEBUG_WARN(CAT, "BLE unknown Android event=%d", e.type); break;
        }
    }
    pthread_mutex_lock(&lock);
    if (b->count || b->overflow) wake(b);
    pthread_mutex_unlock(&lock);
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_BleRadio_nativeInstall(JNIEnv* env, jclass cls) {
    if (radio_class) return; /* Application устанавливает один раз, до запуска worker. */
    if ((*env)->GetJavaVM(env, &vm) != JNI_OK) { DEBUG_ERROR(CAT, "BLE GetJavaVM failed"); return; }
    command_method = (*env)->GetStaticMethodID(env, cls, "command", "(IJJLjava/lang/String;[BI)Z");
    if (!command_method) { DEBUG_ERROR(CAT, "BLE command method unavailable"); return; }
    radio_class = (*env)->NewGlobalRef(env, cls);
    if (!radio_class) DEBUG_ERROR(CAT, "BLE global class allocation failed");
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_BleRadio_nativeEvent(JNIEnv* env, jclass cls, jlong gen,
        jlong token, jint type, jint arg, jstring text, jbyteArray bytes) {
    (void)cls;
    struct event e = {.token = token, .type = type, .arg = arg};
    e.len = bytes ? (*env)->GetArrayLength(env, bytes) : 0;
    if (e.len > 512) { DEBUG_ERROR(CAT, "BLE JNI oversized event=%d", e.len); return; }
    if (e.len) (*env)->GetByteArrayRegion(env, bytes, 0, e.len, (jbyte*)e.data);
    const char* s = text ? (*env)->GetStringUTFChars(env, text, NULL) : NULL;
    if (s) { snprintf(e.text, sizeof(e.text), "%s", s); (*env)->ReleaseStringUTFChars(env, text, s); }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); DEBUG_ERROR(CAT, "BLE JNI event copy failed"); return; }
    pthread_mutex_lock(&lock);
    if (active && active->generation == gen) {
        if (active->count == EVENTS) active->overflow++;
        else { active->events[(active->head + active->count) % EVENTS] = e; active->count++; }
        wake(active);
    }
    pthread_mutex_unlock(&lock);
}
struct ble_backend* ble_backend_create(struct ble_transport* owner, struct UASYNC* ua, const struct ble_config* cfg) {
    struct ble_backend* b = u_calloc(1, sizeof(*b));
    if (!b) { DEBUG_ERROR(CAT, "BLE Android allocation failed"); return NULL; }
    b->owner = owner; b->ua = ua;
    b->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (b->wake_fd < 0) { DEBUG_ERROR(CAT, "BLE eventfd create: %s", strerror(errno)); u_free(b); return NULL; }
    pthread_mutex_lock(&lock);
    if (active) { pthread_mutex_unlock(&lock); close(b->wake_fd); u_free(b); DEBUG_ERROR(CAT, "BLE Android already active"); return NULL; }
    b->generation = ++generation; active = b;
    pthread_mutex_unlock(&lock);
    b->io = uasync_add_socket(ua, b->wake_fd, drain, NULL, drain, "android_ble_events", b);
    if (!b->io) DEBUG_ERROR(CAT, "BLE eventfd registration failed");
    char uuid[37]; ble_service_uuid_encode(uuid, cfg->node_id);
    const char filter[] = BLE_SERVICE_UUID_FILTER "|" BLE_SERVICE_UUID_MASK;
    uint8_t setup[BLE_PUBLIC_KEY_SIZE + sizeof(filter) + BLE_NAME_MAX + 1];
    memcpy(setup, cfg->public_key, BLE_PUBLIC_KEY_SIZE);
    int setup_size = snprintf((char*)setup + BLE_PUBLIC_KEY_SIZE, sizeof(setup) - BLE_PUBLIC_KEY_SIZE, "%s|%s", filter, cfg->name);
    if (setup_size < 0 || (size_t)setup_size >= sizeof(setup) - BLE_PUBLIC_KEY_SIZE) {
        DEBUG_ERROR(CAT, "BLE Android setup metadata overflow bytes=%d", setup_size); ble_backend_destroy(b); return NULL;
    }
    if (!b->io || command(1, b->generation, 0, uuid, setup, BLE_PUBLIC_KEY_SIZE + (size_t)setup_size, 0) < 0) {
        ble_backend_destroy(b); return NULL;
    }
    return b;
}
void ble_backend_destroy(struct ble_backend* b) {
    if (!b) return;
    pthread_mutex_lock(&lock); if (active == b) active = NULL; pthread_mutex_unlock(&lock);
    command(0, b->generation, 0, NULL, NULL, 0, 0);
    if (b->io) uasync_remove_socket(b->ua, b->io);
    close(b->wake_fd);
    u_free(b);
}
int ble_backend_advertise(struct ble_backend* b, const uint8_t* data, size_t len) {
    return command(2, b->generation, 0, NULL, data, len, data != NULL);
}
int ble_backend_connect(struct ble_backend* b, ble_link_id id, const struct ble_endpoint* ep) {
    struct android_link* l = by_id(b, 0);
    if (!l) { DEBUG_WARN(CAT, "BLE Android link limit"); return BLE_ERROR; }
    l->id = id; l->token = (jlong)id;
    char uuid[37];
    if (ble_service_uuid_encode(uuid, ble_backend_peer(b->owner, id))) { memset(l, 0, sizeof(*l)); return BLE_ERROR; }
    return command(3, b->generation, l->token, ep->id, uuid, strlen(uuid), ble_backend_profile_only(b->owner, id));
}
/* Общий C codec UUID используется и Kotlin scanner; modified-UTF8 JNI не применяется к имени. */
JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_BleRadio_nativeDiscovery(JNIEnv* env, jclass cls,
        jstring service) {
    (void)cls;
    const char* uuid = (*env)->GetStringUTFChars(env, service, NULL); if (!uuid) return NULL;
    uint64_t node = 0; int result = ble_service_uuid_decode(uuid, &node);
    (*env)->ReleaseStringUTFChars(env, service, uuid); if (result) return NULL;
    uint8_t bytes[BLE_BROADCAST_MAX]; int len = ble_identity_encode(bytes, node, "");
    if ((*env)->ExceptionCheck(env)) return NULL;
    jbyteArray out = (*env)->NewByteArray(env, len);
    if (out) (*env)->SetByteArrayRegion(env, out, 0, len, (const jbyte*)bytes);
    return out;
}
void ble_backend_close(struct ble_backend* b, ble_link_id id) {
    struct android_link* l = by_id(b, id); if (!l) return;
    command(4, b->generation, l->token, NULL, NULL, 0, 0); memset(l, 0, sizeof(*l));
}
int ble_backend_send(struct ble_backend* b, ble_link_id id, enum ble_wire_channel channel, const uint8_t* data, size_t len) {
    struct android_link* l = by_id(b, id);
    if (!l) { DEBUG_ERROR(CAT, "BLE Android send unknown link"); return BLE_ERROR; }
    return command(5, b->generation, l->token, NULL, data, len, channel);
}
static void status(void* arg, enum ble_state state, const char* reason) {
    (void)arg;
    radio_ready = state == BLE_READY;
    DEBUG_INFO(CAT, "BLE Android status=%d reason=%s", state, reason);
    command(7, 0, 0, reason, NULL, 0, state);
    if (state == BLE_READY && (policy & 2)) ble_advertise(transport, identity, identity_len);
}
static void advertising(void* arg, enum ble_state state, const char* reason) {
    (void)arg; command(8, 0, 0, reason, NULL, 0, state);
}
static void broadcast(void* arg, const struct ble_endpoint* ep, const uint8_t* bytes, size_t len, int rssi) {
    (void)arg; (void)rssi;
    if (!(policy & 16)) return; /* Обычное discovery ведёт и логирует общий драйвер. */
    struct ble_identity peer;
    if (ble_identity_decode(&peer, bytes, len) < 0) return;
    /* Только явно запрошенный instrumentation test; обычный транспорт не выбирает пиров. */
    if ((policy & 16) && peer.node_id == 0x5643424c45544553ULL) {
        policy &= ~16;
        DEBUG_INFO(CAT, "BLE probe dial peer=%016llx endpoint=%s", (unsigned long long)peer.node_id, ep->id);
        ble_connect(transport, ep, peer.node_id);
    }
}
static int validate(void* arg, ble_link_id link, uint64_t peer) {
    (void)arg; DEBUG_INFO(CAT, "BLE GATT identity received link=%llu node=%016llx; ETCP admission pending",
        (unsigned long long)link, (unsigned long long)peer);
    return 1;
}
static void received(void* arg, ble_link_id link) {
    (void)arg;
    if (!(policy & 8)) return;
    struct ll_entry* e;
    while (ble_tx_ready(transport, link) && (e = ble_rx_get(transport, link))) {
        int result = ble_tx_put(transport, link, e);
        if (result != BLE_OK) {
            DEBUG_ERROR(CAT, "BLE test echo failed link=%llu result=%d", (unsigned long long)link, result);
            queue_dgram_free(e); queue_entry_free(e);
        }
    }
}
static void etcp_echo(struct ETCP_CONN* conn, struct ll_entry* e) {
    DEBUG_TRACE(CAT, "BLE ETCP probe echo peer=%016llx bytes=%u", (unsigned long long)conn->peer_node_id, e->len);
    if (etcp_send(conn, e)) {
        DEBUG_ERROR(CAT, "BLE ETCP probe echo refused bytes=%u", e->len);
        queue_dgram_free(e); queue_entry_free(e);
    }
}
struct ble_transport* android_ble_transport(void) { return transport; }
struct ble_etcp* android_ble_driver(void) { return driver; }
void android_ble_detach(void) {
    if (echo_instance) { etcp_unbind(echo_instance, ETCP_ID_BLE_PROBE); echo_instance = NULL; }
    ble_etcp_destroy(driver); driver = NULL; transport = NULL;
    if (probe_trace) {
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, saved_trace);
        debug_set_category_level(CAT, saved_discovery); probe_trace = 0;
    }
}
static void configure(void* arg) {
    int flags = (int)(intptr_t)arg;
    struct UTUN_INSTANCE* inst = instance_lite_get_instance();
    if (!inst) { DEBUG_WARN(CAT, "BLE configure cancelled: core stopped"); return; }
    /* Raw-проба намеренно публикует длинное имя: проверяем отсутствие прежнего ограничения13 байт. */
    const char* full_name = (flags & 32) ? "ETCP-probe" :
                            (flags & 8) ? "Android BLE: полное имя устройства" : inst->config->global.name;
    if ((flags & 1) && ble_name_validate(full_name)) return;
    int previous = policy;
    policy = flags; incoming_policy = !!(flags & 4);
    if (transport && radio_ready && (flags & 1) && !((previous ^ flags) & (8 | 32))) {
        ble_set_incoming(transport, incoming_policy);
        command(6, 0, 0, NULL, NULL, 0, incoming_policy);
        ble_set_name(transport, full_name);
        ble_advertise(transport, (flags & 2) ? identity : NULL, (flags & 2) ? identity_len : 0);
        return;
    }
    android_ble_detach();
    command(6, 0, 0, NULL, NULL, 0, incoming_policy);
    if (!(flags & 1)) return;
    struct ble_config cfg = {0}; cfg.node_id = inst->node_id;
    strcpy(cfg.name, full_name); cfg.discover_names = !(flags & (8 | 32));
    cfg.callbacks.advertising = advertising;
    cfg.callbacks.status = status; cfg.callbacks.broadcast = broadcast; cfg.callbacks.validate = validate; cfg.callbacks.received = received;
    cfg.callbacks.writable = received; /* Raw echo оставляет RX в очереди до освобождения TX-окна. */
    identity_len = ble_identity_encode(identity, cfg.node_id, (flags & 32) ? "ETCP-probe" : inst->config->global.name);
    if (identity_len < 0) { DEBUG_ERROR(CAT, "BLE identity encoding failed node=%016llx", (unsigned long long)cfg.node_id); return; }
    if (flags & 32) {
        if (inst->utun_started || inst->api_bindings.callbacks[ETCP_ID_BLE_PROBE] || etcp_bind(inst, ETCP_ID_BLE_PROBE, etcp_echo)) {
            DEBUG_ERROR(CAT, "BLE ETCP probe unavailable: UTUN active or test binding occupied"); return;
        }
        echo_instance = inst;
    }
    driver = ble_etcp_create(instance_lite_get_uasync(), (flags & 8) ? NULL : inst, &cfg, !(flags & (8 | 32)));
    transport = ble_etcp_radio(driver);
    if (!transport) { android_ble_detach(); return; }
    if (flags & (8 | 32)) {
        saved_trace = g_debug_config.category_levels[DEBUG_CATEGORY_DEBUG];
        saved_discovery = g_debug_config.category_levels[CAT]; probe_trace = 1;
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_TRACE);
        debug_set_category_level(CAT, DEBUG_LEVEL_DEBUG);
    }
    ble_set_incoming(transport, incoming_policy);
    if (ble_start(transport)) DEBUG_ERROR(CAT, "BLE Android driver start failed");
}
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_BleRadio_nativeConfigure(JNIEnv* env, jclass cls, jint flags) {
    (void)env; (void)cls;
    if (flags < 0 || flags > 63 || ((flags & 8) && (flags & 32))) { DEBUG_ERROR(CAT, "BLE invalid settings=%d", flags); return JNI_FALSE; }
    struct posted_task* task = u_calloc(1, sizeof(*task));
    if (!task) { DEBUG_ERROR(CAT, "BLE settings allocation failed"); return JNI_FALSE; }
    task->callback = configure; task->arg = (void*)(intptr_t)flags;
    return instance_lite_post_frontend(task, CAT, "BLE") == 0;
}

int ble_backend_set_name(struct ble_backend* b, const char* name) {
    return command(10, b->generation, 0, NULL, (const uint8_t*)name, strlen(name), 0);
}
