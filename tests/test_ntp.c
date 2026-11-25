/**
 * @file test_ntp.c
 * @brief NTP client test: direct address + hostname (async DNS) paths
 *
 * Сценарий 1 (direct): фейковый NTP-сервер, синхронизация по test_addr.
 * Сценарий 2 (hostname): фейковый DNS-сервер резолвит hostname → 127.0.0.1,
 *   затем NTP опрашивает фейковый NTP-сервер (async_dns → неблокирующий запрос).
 * Оба сценария проверяют флаг synced, знак offset и итоговое скорректированное время getters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include "../src/utun_instance.h"
#include "../src/ntp_time.h"
#include "../src/config_parser.h"

#define TEST_KNOWN_OFFSET_SEC 5
#define TEST_KNOWN_OFFSET_US  (TEST_KNOWN_OFFSET_SEC * 1000000LL)
#define TEST_TIMEOUT_SEC      10
#define OFFSET_TOLERANCE_US   500000   // 500ms — generous for scheduling jitter on localhost
#define SYNC_WAIT_POLL_MS     10

#define NTP_DELTA 2208988800ULL

#pragma pack(push, 1)
struct ntp_packet {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    uint8_t  precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint64_t ref_ts;
    uint64_t orig_ts;
    uint64_t recv_ts;
    uint64_t xmit_ts;
};
#pragma pack(pop)
_Static_assert(sizeof(struct ntp_packet) == 48, "bad ntp_packet size");

static volatile int g_server_running = 1;
static volatile uint16_t g_server_port = 0;

static volatile int g_dns_running = 1;
static volatile uint16_t g_dns_port = 0;

static uint64_t timeval_to_ntp(struct timeval *tv) {
    uint64_t sec  = (uint64_t)(tv->tv_sec + NTP_DELTA);
    uint64_t frac = ((uint64_t)tv->tv_usec << 32) / 1000000ULL;
    return (sec << 32) | frac;
}

static void platform_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void* test_server_thread(void* arg) {
    (void)arg;
    socket_t sock = socket_create_udp(AF_INET);
    if (sock == SOCKET_INVALID) { g_server_running = 0; return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { socket_close_wrapper(sock); g_server_running = 0; return NULL; }

    socklen_t addr_len = sizeof(addr);
    getsockname(sock, (struct sockaddr*)&addr, &addr_len);
    g_server_port = ntohs(addr.sin_port);

    struct timeval tv = {1, 0};
    while (g_server_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        // Таймаут select нужен только для быстрой остановки тестового сервера.
        tv.tv_sec = 0; tv.tv_usec = 10000;
        int r = select((int)(sock + 1), &fds, NULL, NULL, &tv);
        if (r <= 0) continue;

        struct ntp_packet request;
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        ssize_t n = recvfrom(sock, (char*)&request, sizeof(request), 0, (struct sockaddr*)&client, &client_len);
        if (n < (ssize_t)sizeof(request)) continue;

        uint8_t mode = request.li_vn_mode & 0x07;
        if (mode != 3) continue; // only respond to client-mode

        struct timeval now;
        utun_gettimeofday(&now, NULL);
        now.tv_sec += TEST_KNOWN_OFFSET_SEC;
        uint64_t fake_ntp = timeval_to_ntp(&now);

        struct ntp_packet reply;
        memset(&reply, 0, sizeof(reply));
        reply.li_vn_mode = (0 << 6) | (4 << 3) | 4; // stratum 0=unspec, v4, server
        reply.stratum    = 1;
        reply.ref_id     = htonl(0x7F000001);         // 127.0.0.1
        reply.orig_ts    = request.xmit_ts;           // echo client's transmit
        reply.recv_ts    = htobe64(fake_ntp);
        reply.xmit_ts    = htobe64(fake_ntp);

        sendto(sock, (const char*)&reply, sizeof(reply), 0, (struct sockaddr*)&client, sizeof(client));
    }
    socket_close_wrapper(sock);
    return NULL;
}

/* ─── фейковый DNS-сервер: любой A-запрос → 127.0.0.1 ─── */

static int dns_question_end(const uint8_t* q, int n) {
    int p = 12;
    while (p < n && q[p] != 0) {
        if ((q[p] & 0xC0) == 0xC0) return p + 2 + 4;
        p += 1 + q[p];
    }
    return p + 1 + 4;
}

static int build_dns_a_response(const uint8_t* q, int n, uint8_t* out, int cap) {
    int qend = dns_question_end(q, n);
    if (qend > n) return -1;
    uint8_t ip[4];
    inet_pton(AF_INET, "127.0.0.1", ip);

    int o = 0;
    out[o++] = q[0]; out[o++] = q[1];                 /* qid */
    out[o++] = 0x81; out[o++] = 0x80;                 /* QR+RD+RA, rcode 0 */
    out[o++] = 0; out[o++] = 1;                       /* qdcount */
    out[o++] = 0; out[o++] = 1;                       /* ancount */
    out[o++] = 0; out[o++] = 0;                       /* nscount */
    out[o++] = 0; out[o++] = 0;                       /* arcount */
    memcpy(out + o, q + 12, (size_t)(qend - 12));     /* question */
    o += qend - 12;
    out[o++] = 0xC0; out[o++] = 0x0C;                 /* имя = указатель на вопрос */
    out[o++] = 0; out[o++] = 1;                       /* type A */
    out[o++] = 0; out[o++] = 1;                       /* class IN */
    out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 60; /* ttl */
    out[o++] = 0; out[o++] = 4;                       /* rdlength */
    memcpy(out + o, ip, 4); o += 4;
    return o;
}

static void* dns_server_thread(void* arg) {
    (void)arg;
    socket_t sock = socket_create_udp(AF_INET);
    if (sock == SOCKET_INVALID) { g_dns_running = 0; return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { socket_close_wrapper(sock); g_dns_running = 0; return NULL; }
    socklen_t al = sizeof(addr);
    getsockname(sock, (struct sockaddr*)&addr, &al);
    g_dns_port = ntohs(addr.sin_port);

    uint8_t buf[512];
    while (g_dns_running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {0, 10000};
        int r = select((int)(sock + 1), &fds, NULL, NULL, &tv);
        if (r <= 0) continue;
        struct sockaddr_in client; socklen_t cl = sizeof(client);
        ssize_t n = recvfrom(sock, (char*)buf, sizeof(buf), 0, (struct sockaddr*)&client, &cl);
        if (n < 12) continue;
        uint8_t resp[512];
        int rl = build_dns_a_response(buf, (int)n, resp, sizeof(resp));
        if (rl > 0) sendto(sock, (const char*)resp, rl, 0, (struct sockaddr*)&client, sizeof(client));
    }
    socket_close_wrapper(sock);
    return NULL;
}

/* ─── helpers ─── */

static struct UTUN_INSTANCE* make_instance(struct UASYNC* ua, const char* server_name) {
    char config_text[512];
    snprintf(config_text, sizeof(config_text),
        "[global]\n"
        "my_node_name=ntp_test\n"
        "my_private_key=e8d4d14943ca3732d7802d4010ea4f6f8bdb71e2304f78d68f4f3dc31e760c79\n"
        "my_public_key=64af656aa6b0130170a140c005a77531d8fd31459a49a59960840e7fe97aee78\n"

        "\n"
        "[ntp]\n"
        "enabled=yes\n"
        "server=%s\n"
        "interval=3600\n",
        server_name);

    struct utun_config* cfg = parse_config_from_buf(config_text, strlen(config_text), "inline");
    if (!cfg) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "parse config failed"); return NULL; }
    struct UTUN_INSTANCE* inst = utun_instance_create_from_config(ua, cfg);
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "create instance failed"); free_config(cfg); return NULL; }
    utun_instance_init(inst);
    return inst;
}

static int verify_synced(struct UTUN_INSTANCE* inst, struct UASYNC* ua, const char* label) {
    uint64_t start_tb = get_time_tb();
    while (!inst->ntp.synced && get_time_tb() - start_tb < (uint64_t)(TEST_TIMEOUT_SEC * 10000))
        uasync_poll(ua, SYNC_WAIT_POLL_MS);

    if (!inst->ntp.synced) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL: %s did not sync within %ds", label, TEST_TIMEOUT_SEC);
        return -1;
    }
    int64_t diff = inst->ntp.offset_us + TEST_KNOWN_OFFSET_US;
    if (diff < 0) diff = -diff;
    if (diff > OFFSET_TOLERANCE_US) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL: %s offset %lldus too far from expected %lldus (diff=%lldus)",
                    label, (long long)inst->ntp.offset_us, (long long)-TEST_KNOWN_OFFSET_US, (long long)diff);
        return -1;
    }
    if (!ntp_time_is_synced(inst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL: %s ntp_time_is_synced returned 0", label);
        return -1;
    }
    struct timeval before, after;
    utun_gettimeofday(&before, NULL);
    int64_t corrected_us = ntp_time_get_us(inst);
    time_t corrected_sec = ntp_time_get_seconds(inst);
    utun_gettimeofday(&after, NULL);
    int64_t lower = (int64_t)before.tv_sec * 1000000 + before.tv_usec + TEST_KNOWN_OFFSET_US - OFFSET_TOLERANCE_US;
    int64_t upper = (int64_t)after.tv_sec * 1000000 + after.tv_usec + TEST_KNOWN_OFFSET_US + OFFSET_TOLERANCE_US;
    if (corrected_us < lower || corrected_us > upper || corrected_sec < lower / 1000000 || corrected_sec > upper / 1000000) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL: %s corrected time outside server clock bounds: us=%lld sec=%lld range=%lld..%lld",
                    label, (long long)corrected_us, (long long)corrected_sec, (long long)lower, (long long)upper);
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: %s offset=%lldus", label, (long long)inst->ntp.offset_us);
    return 0;
}

int main(void) {
    int result = 1;
    pthread_t server_thread, dns_thread;
    int server_started = 0, dns_started = 0;

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_categories(DEBUG_CATEGORY_GENERAL);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== NTP Test ===");

    utun_instance_set_tun_init_enabled(0);

    /* fake NTP server */
    if (pthread_create(&server_thread, NULL, test_server_thread, NULL) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Failed to create server thread");
        return 1;
    }
    server_started = 1;
    { uint64_t w = get_time_tb(); while (g_server_port == 0 && get_time_tb() - w < 50000) platform_sleep_ms(5); }
    if (g_server_port == 0) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "fake NTP server did not bind"); goto cleanup; }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Fake NTP server on 127.0.0.1:%u, offset=+%ds", g_server_port, TEST_KNOWN_OFFSET_SEC);

    /* ── scenario 1: direct address (test_addr) ── */
    {
        struct UASYNC* ua = uasync_create();
        if (!ua) goto cleanup;
        struct UTUN_INSTANCE* inst = make_instance(ua, "placeholder");
        if (!inst) { uasync_destroy(ua, 0); goto cleanup; }
        ntp_time_set_test_addr(&inst->ntp, "127.0.0.1", g_server_port);
        int ok = verify_synced(inst, ua, "direct (test_addr)");
        utun_instance_destroy(inst);
        uasync_destroy(ua, 0);
        if (ok != 0) goto cleanup;
    }

    /* ── scenario 2: hostname resolved via fake DNS (async_dns) ── */
    if (pthread_create(&dns_thread, NULL, dns_server_thread, NULL) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Failed to create DNS server thread");
        goto cleanup;
    }
    dns_started = 1;
    { uint64_t w = get_time_tb(); while (g_dns_port == 0 && get_time_tb() - w < 50000) platform_sleep_ms(5); }
    if (g_dns_port == 0) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "fake DNS server did not bind"); goto cleanup; }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Fake DNS server on 127.0.0.1:%u", g_dns_port);

    {
        struct UASYNC* ua = uasync_create();
        if (!ua) goto cleanup;
        struct UTUN_INSTANCE* inst = make_instance(ua, "ntp.test.local");
        if (!inst) { uasync_destroy(ua, 0); goto cleanup; }
        ntp_time_set_test_dns(&inst->ntp, "127.0.0.1", g_dns_port, g_server_port);
        int ok = verify_synced(inst, ua, "hostname (async DNS)");
        utun_instance_destroy(inst);
        uasync_destroy(ua, 0);
        if (ok != 0) goto cleanup;
    }

    result = 0;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP Test PASSED");

cleanup:
    g_server_running = g_dns_running = 0;
    uint64_t stop_started = get_time_us();
    if (server_started) pthread_join(server_thread, NULL);
    if (dns_started) pthread_join(dns_thread, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "test NTP servers stopped: ntp=%d dns=%d elapsed_us=%llu",
               server_started, dns_started, (unsigned long long)(get_time_us() - stop_started));
    return result;
}
