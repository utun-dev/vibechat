/* Linux: два процесса с независимыми uasync, настоящий UDP loopback, проверка каждого байта и порядка.
 * bench_etcp_throughput [TX=4] [bytes=1200] [window=1024] [seconds=5] [RX=TX].
 * Одна секунда прогрева, затем goodput по доставленным сообщениям; ACK дренируются отдельно.
 * CPU охватывает прогрев+замер+drain, все потоки процесса. crypto=info даёт профиль пула при stop. */
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "../lib/platform_compat.h"
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include "test_net.h"
#include "etcp.h"
#include "etcp_api.h"
#include "pkt_normalizer.h"
#include "../lib/mem.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL pid=%d line=%d: %s\n", getpid(), __LINE__, #x); abort(); } } while (0)

static const char *pub[2] = {
    "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f",
    "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d"
};
static const char *priv[2] = {
    "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f",
    "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68"
};
struct shared {
    atomic_uint received, stop, ready; // Общая память: приёмник публикует только полностью проверенные сообщения.
    double cpu[2];                   // Каждый процесс пишет свою запись, родитель читает после waitpid.
};
struct endpoint { struct sockaddr_storage addr; uint64_t id; };
static struct shared *shared;
static unsigned packet_size;
static uint8_t reference[8192];

static uint64_t now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static unsigned number(const char *value, unsigned minimum, unsigned maximum) {
    char *end;
    unsigned long n = strtoul(value, &end, 10);
    CHECK(*value && !*end && n >= minimum && n <= maximum);
    return (unsigned)n;
}
static void receive(struct ETCP_CONN *conn, struct ll_entry *entry) {
    (void)conn;
    CHECK(entry->len == packet_size && entry->dgram[0] == 253);
    uint32_t seq;
    memcpy(&seq, entry->dgram + 1, sizeof(seq));
    CHECK(seq == atomic_load_explicit(&shared->received, memory_order_relaxed));
    CHECK(!memcmp(entry->dgram + 5, reference + 5, packet_size - 5));
    atomic_store_explicit(&shared->received, seq + 1, memory_order_release);
    queue_dgram_free(entry); queue_entry_free(entry);
}
static int drained(struct ETCP_CONN *conn) {
    if (conn->unacked_bytes || conn->input_send_q->count || conn->input_queue->count || conn->send_input_q->count ||
        conn->normalizer->data_ptr || conn->tx_token || conn->ack_resp_timer || conn->ack_till_deadline_us) return 0;
    for (struct ETCP_LINK *link = conn->links; link; link = link->next)
        if (link->ack_q->count || link->ack_reply_pending) return 0;
    return 1;
}
static double cpu_seconds(const struct rusage *u) {
    return u->ru_utime.tv_sec + u->ru_utime.tv_usec / 1e6 + u->ru_stime.tv_sec + u->ru_stime.tv_usec / 1e6;
}

int main(int argc, char **argv) {
    if (argc > 6) {
        fprintf(stderr, "Usage: %s [TX=4] [bytes=1200] [window=1024] [seconds=5] [RX=TX]\n", argv[0]); return 1;
    }
    unsigned tx = argc > 1 ? number(argv[1], 0, 64) : 4;
    packet_size = argc > 2 ? number(argv[2], 5, sizeof(reference)) : 1200;
    unsigned window = argc > 3 ? number(argv[3], 1, 65536) : 1024;
    unsigned duration = argc > 4 ? number(argv[4], 1, 3600) : 5;
    unsigned rx = argc > 5 ? number(argv[5], 0, 64) : tx;
    for (unsigned i = 0; i < packet_size; i++) reference[i] = (uint8_t)i;
    reference[0] = 253;
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED);
    atomic_init(&shared->received, 0); atomic_init(&shared->stop, 0); atomic_init(&shared->ready, 0);
    CHECK(atomic_is_lock_free(&shared->received));
    int control[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, control));
    pid_t child = fork(); CHECK(child >= 0);
    unsigned side = child == 0;
    close(control[1 - side]);
    int fd = control[side];
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_CRYPTO, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_SOCKET, DEBUG_LEVEL_INFO);
    char config[1024];
    snprintf(config, sizeof(config), "[global]\nmy_public_key=%s\nmy_private_key=%s\n"
        "crypto_tx_threads=%u\ncrypto_rx_threads=%u\nmtu=1400\nauto_sockets=no\n"
        "keepalive_interval=100\nkeepalive_timeout=1000\nkeepalive_adaptive=0\n"
        "[server:bench]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n", pub[side], priv[side], tx, rx);
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE *inst = utun_instance_create_from_str(ua, config);
    CHECK(inst && !utun_core_start(inst));
    struct ETCP_SOCKET *sock = test_etcp_bound_socket(inst, "bench");
    CHECK(!etcp_bind(inst, 253, receive));
    struct endpoint local = {sock->interface_addr, inst->node_id}, peer;
    CHECK(write(fd, &local, sizeof(local)) == sizeof(local));
    CHECK(recv(fd, &peer, sizeof(peer), MSG_WAITALL) == sizeof(peer)); close(fd);
    struct ETCP_CONN *conn = NULL;
    if (!side) {
        uint8_t peer_key[SC_PUBKEY_SIZE];
        CHECK(!sc_hex_to_binary(pub[1], peer_key, sizeof(peer_key)));
        conn = etcp_connection_create(inst, "throughput");
        CHECK(conn && !etcp_connection_set_peer(conn, peer_key));
        CHECK(etcp_link_new(conn, sock, &peer.addr, 0));
    }
    uint64_t deadline = now() + 10000000000ULL;
    for (;;) {
        uasync_poll(ua, 10);
        if (side) conn = instance_find_conn(inst, peer.id);
        if (conn && conn->initialized && !conn->session_required && !conn->reinit_pending && conn->links_up) break;
        CHECK(now() < deadline);
    }
    CHECK(!etcp_conn_ref_take(conn));
    uint32_t resets = conn->reset_count;
    struct rusage before;
    CHECK(!getrusage(RUSAGE_SELF, &before));
    unsigned sent = 0;
    if (side) {
        atomic_store(&shared->ready, 1);
        deadline = now() + ((uint64_t)duration + 45) * 1000000000;
        while (!atomic_load(&shared->stop)) { uasync_poll(ua, 10); CHECK(now() < deadline); }
    } else {
        while (!atomic_load(&shared->ready)) { uasync_poll(ua, 10); CHECK(now() < deadline); }
        unsigned first = 0;
        uint64_t start = now(), sample_start = 0;
        while (now() - start < ((uint64_t)duration + 1) * 1000000000) {
            uint64_t t = now();
            if (!sample_start && t - start >= 1000000000) {
                sample_start = t; first = atomic_load(&shared->received);
            }
            for (unsigned batch = 0; batch < 64 && sent - atomic_load_explicit(&shared->received, memory_order_acquire) < window &&
                 !conn->send_input_q->count; batch++) {
                struct ll_entry *packet = ll_alloc_lldgram(packet_size); CHECK(packet);
                packet->len = packet_size; memcpy(packet->dgram, reference, packet_size);
                memcpy(packet->dgram + 1, &sent, sizeof(uint32_t)); sent++;
                CHECK(!etcp_send(conn, packet));
            }
            uasync_poll(ua, 10);
        }
        uint64_t sample_end = now();
        unsigned last = atomic_load(&shared->received);
        deadline = now() + 30000000000ULL;
        while (atomic_load(&shared->received) != sent || !drained(conn)) { uasync_poll(ua, 10); CHECK(now() < deadline); }
        double seconds = (sample_end - sample_start) / 1e9, bytes = (double)(last - first) * packet_size;
        printf("RESULT tx=%u rx=%u size=%u window=%u seconds=%.6f measured_messages=%u Mbps=%.3f MBps=%.3f "
            "total_sent=%u total_received=%u retrans=%u resets=%u\n", tx, rx, packet_size, window, seconds, last - first,
            bytes * 8 / seconds / 1e6, bytes / seconds / 1e6, sent, atomic_load(&shared->received),
            conn->retransmissions_count, conn->reset_count - resets);
        fflush(stdout); atomic_store(&shared->stop, 1);
    }
    struct rusage usage;
    CHECK(!getrusage(RUSAGE_SELF, &usage));
    double cpu = cpu_seconds(&usage) - cpu_seconds(&before);
    printf("CPU side=%u seconds=%.6f voluntary=%ld involuntary=%ld (warmup+measurement+drain, all threads)\n",
        side, cpu, usage.ru_nvcsw - before.ru_nvcsw, usage.ru_nivcsw - before.ru_nivcsw);
    shared->cpu[side] = cpu;
    CHECK(conn->reset_count == resets);
    etcp_conn_ref_free(conn); utun_instance_destroy(inst); uasync_poll(ua, 0); uasync_destroy(ua, 0);
    CHECK(!u_get_allocated_count());
    if (!side) {
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
        double total_cpu = shared->cpu[0] + shared->cpu[1];
        printf("CPU_TOTAL seconds=%.6f us_per_message=%.3f bytes=%llu\n", total_cpu, total_cpu * 1e6 / sent,
            (unsigned long long)sent * packet_size);
        puts("PASS: all payloads verified in order; ACK drained; no resets or outstanding allocations");
    }
    CHECK(!munmap(shared, sizeof(*shared)));
    return 0;
}
