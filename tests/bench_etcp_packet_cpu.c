/* CPU UDP/ETCP: реальные loopback-сокеты, обычные очереди/normalizer, отдельный wall time.
 * Запуск: bench_etcp_packet_cpu [packets=10000] [window=1] [mem=checked|libc] [logs=off|info|debug|trace].
 * UTUN_PACKET_CPU_SIZE выбирает один размер, UTUN_PACKET_CPU_MTU задаёт MTU стенда (по умолчанию 1280).
 * --wrap переключает ВСЕ внешние точки mem.c до первого выделения; режим нельзя менять при живых блоках. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <sys/utsname.h>
#include <openssl/crypto.h>
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_connections.h"
#include "etcp_profile.h"
#include "pkt_normalizer.h"
#include "test_net.h"
#include "../lib/mem.h"

#define REQUIRE(x) do { if (!(x)) { \
    debug_output(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_DEBUG, __func__, __FILE__, __LINE__, "FAIL: %s", #x); exit(1); \
} } while (0)
#define REPORT(...) debug_output(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_DEBUG, __func__, __FILE__, __LINE__, __VA_ARGS__)
#ifdef UTUN_DISABLE_LOGS
#define COMPILED_LOGS "removed"
#else
#define COMPILED_LOGS "present"
#endif

struct metric { uint64_t ns, calls; };
static struct metric metrics[ETCP_PROF_STAGES];
static enum etcp_profile_stage current_stage;
static uint64_t last_cpu;
static int measuring, raw_mem;
static struct UASYNC* ua;
static struct UTUN_INSTANCE* instances[2];
static struct ETCP_SOCKET* sockets[2];
static struct ETCP_CONN* connections[2];
static unsigned sent, received, packet_size, in_flight;
static unsigned benchmark_mtu;
static uint64_t submit_wall, norm_wall, encrypt_wall, delivered_wall;
static uint64_t latency_ns, norm_latency_ns, encrypt_latency_ns;
static unsigned latency_count;
static uint64_t init_packets;

// Часы потока включают user+system CPU, исключая вытеснение и ожидание в epoll/очередях.
static uint64_t now_ns(clockid_t clock) {
    struct timespec ts;
    REQUIRE(clock_gettime(clock, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// Необязательная числовая настройка стенда; неверное значение завершается с диагностикой до создания ядер.
static unsigned env_uint(const char* name, unsigned fallback, unsigned minimum, unsigned maximum) {
    const char* value = getenv(name);
    if (!value) return fallback;
    char* end = NULL;
    unsigned long number = strtoul(value, &end, 10);
    if (!*value || *end || number < minimum || number > maximum) {
        debug_output(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_DEBUG, __func__, __FILE__, __LINE__,
            "Invalid %s=%s: expected integer %u..%u", name, value, minimum, maximum);
        exit(1);
    }
    return (unsigned)number;
}

void etcp_profile_set(enum etcp_profile_stage stage) {
    if (!measuring || current_stage == stage) return;
    uint64_t now = now_ns(CLOCK_THREAD_CPUTIME_ID);
    metrics[current_stage].ns += now - last_cpu;
    metrics[stage].calls++;
    last_cpu = now; current_stage = stage;
}

struct etcp_profile_scope etcp_profile_enter(enum etcp_profile_stage stage) {
    struct etcp_profile_scope scope = {current_stage};
    etcp_profile_set(stage);
    return scope;
}

void etcp_profile_leave(struct etcp_profile_scope* scope) { etcp_profile_set(scope->previous); }

void etcp_profile_set_init(enum etcp_profile_stage stage) {
    if (current_stage == ETCP_PROF_INIT ||
        (current_stage >= ETCP_PROF_INIT_CONTEXT && current_stage <= ETCP_PROF_INIT_CRC)) etcp_profile_set(stage);
}

struct etcp_profile_scope etcp_profile_enter_init(enum etcp_profile_stage stage) {
    struct etcp_profile_scope scope = {current_stage};
    etcp_profile_set_init(stage);
    return scope;
}

// Wall time привязываем только к одиночной кодограмме: batching не даёт соответствия 1:1 с UDP.
void etcp_profile_mark(enum etcp_profile_event event, const void* owner) {
    if (!measuring) return;
    if (event == ETCP_PROF_INIT_BEGIN) { init_packets++; return; }
    if (in_flight != 1 || owner != connections[0]) return;
    if (event == ETCP_PROF_SUBMIT) {
        submit_wall = now_ns(CLOCK_MONOTONIC); norm_wall = encrypt_wall = delivered_wall = 0;
    } else if (event == ETCP_PROF_NORMALIZED && !norm_wall) norm_wall = now_ns(CLOCK_MONOTONIC);
    else if (event == ETCP_PROF_ENCRYPT_BEGIN && !encrypt_wall) encrypt_wall = now_ns(CLOCK_MONOTONIC);
}

void* __real_u_malloc_impl(uint32_t, const char*);
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void __real_u_free_impl(void*, const char*);
char* __real_u_strdup_impl(const char*, const char*);
void __real_u_check(void*, const char*, const char*);
size_t __real_u_get_allocated_count(void);
void __real_u_report_unfreed_blocks(void);

void* __wrap_u_malloc_impl(uint32_t n, const char* loc) {
    return raw_mem ? malloc(n) : __real_u_malloc_impl(n, loc);
}
void* __wrap_u_calloc_impl(uint32_t n, uint32_t size, const char* loc) {
    return raw_mem ? calloc(n, size) : __real_u_calloc_impl(n, size, loc);
}
void* __wrap_u_realloc_impl(void* p, uint32_t n, const char* loc) {
    return raw_mem ? realloc(p, n) : __real_u_realloc_impl(p, n, loc);
}
void __wrap_u_free_impl(void* p, const char* loc) {
    if (raw_mem) free(p); else __real_u_free_impl(p, loc);
}
char* __wrap_u_strdup_impl(const char* s, const char* loc) {
    return raw_mem ? (s ? strdup(s) : NULL) : __real_u_strdup_impl(s, loc);
}
void __wrap_u_check(void* p, const char* text, const char* loc) {
    if (!raw_mem) __real_u_check(p, text, loc);
}
size_t __wrap_u_get_allocated_count(void) { return raw_mem ? 0 : __real_u_get_allocated_count(); }
void __wrap_u_report_unfreed_blocks(void) { if (!raw_mem) __real_u_report_unfreed_blocks(); }

ssize_t __real_socket_sendto(socket_t, const void*, size_t, const struct sockaddr*, socklen_t);
ssize_t __wrap_socket_sendto(socket_t fd, const void* bytes, size_t len, const struct sockaddr* addr, socklen_t alen) {
    enum etcp_profile_stage previous = current_stage;
    etcp_profile_set(ETCP_PROF_SOCKET);
    ssize_t result = __real_socket_sendto(fd, bytes, len, addr, alen);
    etcp_profile_set(previous == ETCP_PROF_TX_POST ? ETCP_PROF_TX_COMPLETE : previous);
    REQUIRE(result == (ssize_t)len);
    return result;
}

// Данные проверяются вне измеряемого RX-стека; callback приложения не включается в стоимость транспорта.
static void receive_packet(struct ETCP_CONN* conn, struct ll_entry* entry) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_APP);
    REQUIRE(conn == connections[1] && entry->len == packet_size && entry->dgram[0] == 253);
    uint32_t sequence;
    memcpy(&sequence, entry->dgram + 1, sizeof(sequence));
    REQUIRE(sequence == received);
    for (unsigned i = 5; i < entry->len; i++) REQUIRE(entry->dgram[i] == (uint8_t)i);
    received++;
    if (measuring && in_flight == 1) {
        delivered_wall = now_ns(CLOCK_MONOTONIC);
        REQUIRE(norm_wall >= submit_wall && encrypt_wall >= norm_wall && delivered_wall >= encrypt_wall);
        latency_ns += delivered_wall - submit_wall;
        norm_latency_ns += norm_wall - submit_wall;
        encrypt_latency_ns += encrypt_wall - submit_wall;
        latency_count++;
    }
    queue_dgram_free(entry); queue_entry_free(entry);
}

// Два независимых ядра, один uasync-поток и один UDP-путь; сервисы UTUN/CHAT не запускаются.
static void setup(void) {
    static const char* pub[] = {
        "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f",
        "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d"
    };
    static const char* priv[] = {
        "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f",
        "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68"
    };
    ua = uasync_create(); REQUIRE(ua);
    for (unsigned i = 0; i < 2; i++) {
        char config[1024];
        snprintf(config, sizeof(config), "[global]\nmy_public_key=%s\nmy_private_key=%s\n"
            "crypto_tx_threads=0\ncrypto_rx_threads=0\n" // Профилировщик измеряет ровно один uasync-поток.
            "mtu=%u\nauto_sockets=no\nkeepalive_interval=100\nkeepalive_timeout=1000\nkeepalive_adaptive=0\n"
            "[server:bench]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n", pub[i], priv[i], benchmark_mtu);
        instances[i] = utun_instance_create_from_str(ua, config); REQUIRE(instances[i]);
        REQUIRE(utun_core_start(instances[i]) == 0);
        sockets[i] = test_etcp_bound_socket(instances[i], "bench");
        REQUIRE(etcp_bind(instances[i], 253, receive_packet) == 0);
    }
    connections[0] = etcp_connection_create(instances[0], "cpu_bench"); REQUIRE(connections[0]);
    REQUIRE(etcp_connection_set_peer(connections[0], instances[1]->my_keys.public_key) == 0);
    REQUIRE(etcp_link_new(connections[0], sockets[0], &sockets[1]->interface_addr, 0));
    uint64_t deadline = now_ns(CLOCK_MONOTONIC) + 5000000000;
    for (;;) {
        uasync_poll(ua, 0);
        connections[1] = instance_find_conn(instances[1], instances[0]->node_id);
        if (connections[1] && connections[0]->initialized && connections[1]->initialized &&
            !connections[0]->session_required && !connections[1]->session_required &&
            !connections[0]->reinit_pending && !connections[1]->reinit_pending &&
            connections[0]->links_up && connections[1]->links_up) break;
        REQUIRE(now_ns(CLOCK_MONOTONIC) < deadline);
    }
    for (unsigned i = 0; i < 2; i++) REQUIRE(etcp_conn_ref_take(connections[i]) == 0);
}

static void teardown(void) {
    for (unsigned i = 0; i < 2; i++) etcp_conn_ref_free(connections[i]);
    for (unsigned i = 0; i < 2; i++) utun_instance_destroy(instances[i]);
    uasync_poll(ua, 0);
    REQUIRE(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    memset(connections, 0, sizeof(connections));
    REQUIRE(u_get_allocated_count() == 0);
}

// Дренируем также ACK: стоимость обратного направления входит в амортизированную цену доставки.
static int drained(void) {
    for (unsigned i = 0; i < 2; i++) {
        struct ETCP_CONN* c = connections[i];
        if (c->unacked_bytes || c->input_send_q->count || c->input_queue->count || c->send_input_q->count ||
            c->normalizer->data_ptr || c->tx_token || c->ack_resp_timer || c->ack_till_deadline_us) return 0;
        for (struct ETCP_LINK* l = c->links; l; l = l->next) if (l->ack_q->count || l->ack_reply_pending) return 0;
    }
    return 1;
}

static void start_measurement(void) {
    REQUIRE(current_stage == ETCP_PROF_IDLE);
    memset(metrics, 0, sizeof(metrics));
    latency_ns = norm_latency_ns = encrypt_latency_ns = latency_count = 0;
    init_packets = 0;
    last_cpu = now_ns(CLOCK_THREAD_CPUTIME_ID); measuring = 1;
}

static void stop_measurement(void) {
    REQUIRE(current_stage == ETCP_PROF_IDLE);
    metrics[ETCP_PROF_IDLE].ns += now_ns(CLOCK_THREAD_CPUTIME_ID) - last_cpu;
    measuring = 0;
}

static const char* stage_names[] = {
    "driver_event_loop", "tx_stack", "encrypt", "tx_after_encrypt", "socket_sendto", "tx_completion",
    "rx_before_decrypt", "decrypt_normal", "decrypt_init", "rx_stack", "app_verify_free",
    "init_context", "init_obfuscate", "init_validate", "init_private_key", "init_public_key",
    "init_derive_context", "init_derive_setup", "init_set_peer", "init_x25519", "init_kdf", "init_free", "init_aes", "init_crc"
};

// Печатаем после замера, в обход выбранного фильтра логов приложения.
static void report_metrics(const char* name, unsigned packets, uint64_t total_cpu, uint64_t wall) {
    uint64_t accounted = 0;
    for (unsigned i = 0; i < ETCP_PROF_STAGES; i++) accounted += metrics[i].ns;
    REPORT("BENCH name=%s size=%u window=%u mem=%s packets=%u encrypt_packets=%" PRIu64
        " decrypt_packets=%" PRIu64 " total_cpu_us_per_message=%.3f wall_ms=%.3f accounted_cpu_ms=%.3f",
        name, packet_size, in_flight, raw_mem ? "libc" : "checked", packets,
        metrics[ETCP_PROF_ENCRYPT].calls, metrics[ETCP_PROF_DECRYPT].calls,
        (double)total_cpu / packets / 1000, (double)wall / 1000000, (double)accounted / 1000000);
    for (unsigned i = 0; i < ETCP_PROF_STAGES; i++) {
        if (!metrics[i].calls && !metrics[i].ns) continue;
        struct metric metric = metrics[i];
        uint64_t intervals = metric.calls;
        if (i == ETCP_PROF_INIT) {
            // Родитель INIT включает подэтапы; их интервалы нужны для поправки на все чтения часов.
            for (unsigned child = ETCP_PROF_INIT_CONTEXT; child <= ETCP_PROF_INIT_CRC; child++) {
                metric.ns += metrics[child].ns; intervals += metrics[child].calls;
            }
            metric.calls = init_packets;
        }
        REPORT("STAGE name=%s stage=%s cpu_us_per_message=%.3f calls=%" PRIu64 " intervals=%" PRIu64 " cpu_us_per_call=%.3f",
            name, stage_names[i], (double)metric.ns / packets / 1000,
            metric.calls, intervals, metric.calls ? (double)metric.ns / metric.calls / 1000 : 0);
    }
    if (latency_count) REPORT("LATENCY samples=%u submit_to_normalizer_us=%.3f submit_to_encrypt_us=%.3f submit_to_receive_us=%.3f",
        latency_count, (double)norm_latency_ns / latency_count / 1000,
        (double)encrypt_latency_ns / latency_count / 1000, (double)latency_ns / latency_count / 1000);
}

// Производитель держит максимум одну кодограмму в input; окно ограничивает ещё не доставленные сообщения.
static void transfer(unsigned count, int profile) {
    sent = received = 0;
    uint32_t first_seq = connections[0]->next_tx_id, reset = connections[0]->reset_count;
    uint64_t deadline = now_ns(CLOCK_MONOTONIC) + 30000000000ULL;
    uint64_t cpu_begin = now_ns(CLOCK_THREAD_CPUTIME_ID), wall_begin = now_ns(CLOCK_MONOTONIC);
    if (profile) start_measurement();
    while (received < count || !drained()) {
        while (sent < count && sent - received < in_flight && !connections[0]->send_input_q->count) {
            // Подготовка caller-owned entry и payload учитывается только в driver, до входа в etcp_send.
            struct ll_entry* p = ll_alloc_lldgram(packet_size); REQUIRE(p);
            p->len = packet_size; p->dgram[0] = 253;
            uint32_t sequence = sent;
            memcpy(p->dgram + 1, &sequence, sizeof(sequence));
            for (unsigned i = 5; i < packet_size; i++) p->dgram[i] = (uint8_t)i;
            sent++;
            REQUIRE(etcp_send(connections[0], p) == 0);
        }
        uasync_poll(ua, 10);
        REQUIRE(now_ns(CLOCK_MONOTONIC) < deadline);
    }
    if (profile) stop_measurement();
    uint64_t cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_begin, wall = now_ns(CLOCK_MONOTONIC) - wall_begin;
    uint32_t fragments = connections[0]->next_tx_id - first_seq;
    REQUIRE(connections[0]->reset_count == reset);
    REPORT("FRAGMENTS size=%u messages=%u tx_fragments=%u capacity=%u window=%u",
        packet_size, count, fragments, connections[0]->normalizer->frag_size, in_flight);
    if (in_flight == 1 && packet_size + 2 <= connections[0]->normalizer->frag_size) REQUIRE(fragments == count);
    if (profile) {
        REQUIRE(!metrics[ETCP_PROF_INIT].calls);
        report_metrics("data", count, cpu, wall);
    } else REPORT("CONTROL size=%u window=%u mem=%s packets=%u total_cpu_us_per_message=%.3f wall_ms=%.3f",
        packet_size, in_flight, raw_mem ? "libc" : "checked", count, (double)cpu / count / 1000, (double)wall / 1000000);
}

int main(int argc, char** argv) {
    unsigned count = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 10000;
    in_flight = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1;
    raw_mem = argc > 3 && !strcmp(argv[3], "libc");
    const char* logs = argc > 4 ? argv[4] : "off";
    debug_config_init();
    REQUIRE(sizeof(stage_names) / sizeof(stage_names[0]) == ETCP_PROF_STAGES);
    unsigned selected_size = env_uint("UTUN_PACKET_CPU_SIZE", 0, 5, PKTNORM_MAX_DGRAM_SIZE);
    benchmark_mtu = env_uint("UTUN_PACKET_CPU_MTU", 1280, ETCP_RFC791_MIN_MTU, PACKET_DATA_MAX_MTU);
    REQUIRE(count && in_flight && (argc <= 3 || raw_mem || !strcmp(argv[3], "checked")));
    REQUIRE(!strcmp(logs, "off") || !strcmp(logs, "info") || !strcmp(logs, "debug") || !strcmp(logs, "trace"));
    debug_level_t level = !strcmp(logs, "off") ? DEBUG_LEVEL_DISABLED : !strcmp(logs, "info") ? DEBUG_LEVEL_INFO :
        !strcmp(logs, "debug") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_TRACE;
    debug_set_level(level == DEBUG_LEVEL_DEBUG ? DEBUG_LEVEL_INFO : level);
    for (int i = 0; i < DEBUG_CATEGORY_COUNT; i++) debug_set_category_level(i, DEBUG_LEVEL_NONE);
    if (level == DEBUG_LEVEL_DEBUG) debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
    // При включённых логах отчёт тоже находится в этом файле; каждый вывод делает обычный fflush.
    if (level > DEBUG_LEVEL_DISABLED) {
        const char* path = getenv("UTUN_PACKET_CPU_LOG");
        debug_enable_file_output(path ? path : "/tmp/utun-packet-cpu-debug.log", 1);
        REQUIRE(g_debug_config.file_output);
    }
    struct utsname system; REQUIRE(uname(&system) == 0);
    REPORT("ENV system=%s release=%s arch=%s openssl=%s compiler=%s logs=%s mem=%s",
        system.sysname, system.release, system.machine, OpenSSL_version(OPENSSL_VERSION), __VERSION__, logs,
        raw_mem ? "libc" : "checked");
    uint64_t clock_start = now_ns(CLOCK_THREAD_CPUTIME_ID);
    for (unsigned i = 0; i < 100000; i++) (void)now_ns(CLOCK_THREAD_CPUTIME_ID);
    REPORT("CLOCK thread_cpu_read_ns=%.1f", (double)(now_ns(CLOCK_THREAD_CPUTIME_ID) - clock_start) / 100001);
    // Прогрев OpenSSL, аллокатора и ядра перед выборкой независимых INIT-handshake.
    setup(); teardown();
    start_measurement();
    uint64_t cpu_begin = now_ns(CLOCK_THREAD_CPUTIME_ID), wall_begin = now_ns(CLOCK_MONOTONIC);
    for (unsigned i = 0; i < 64; i++) { setup(); teardown(); }
    stop_measurement();
    report_metrics("handshake", 64, now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_begin, now_ns(CLOCK_MONOTONIC) - wall_begin);
    setup();
    REPORT("PATH transport=UDP mtu=%u normalizer_fragment=%u compiled_logs=%s",
        connections[0]->mtu, connections[0]->normalizer->frag_size, COMPILED_LOGS);
    const unsigned sizes[] = {64, 1200, 8192};
    unsigned size_count = selected_size ? 1 : sizeof(sizes) / sizeof(sizes[0]);
    for (unsigned i = 0; i < size_count; i++) {
        packet_size = selected_size ? selected_size : sizes[i];
        transfer(100, 0);
        transfer(count, 0);
        transfer(count, 1);
    }
    teardown();
    REPORT("PASS: all payloads delivered in order, ACK drained, timers and mem.c allocations released");
    debug_disable_file_output();
    return 0;
}
