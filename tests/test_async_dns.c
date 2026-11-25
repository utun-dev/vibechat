/**
 * @file test_async_dns.c
 * @brief Юнит-тесты async_dns (асинхронный DNS-резолвер поверх libdns).
 *
 * 1. Чистый парсер resolv.conf (adns_parse_resolv_conf).
 * 2. Сквозные тесты с фейковым DNS-сервером в отдельном потоке:
 *    - A-запись (с компрессией имени);
 *    - два A;
 *    - CNAME + A;
 *    - NXDOMAIN;
 *    - таймаут (сервер молчит);
 *    - отмена (коллбэк не вызывается).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include "../lib/async_dns.h"

#define SERVER_WAIT_MS 5000
#define RESOLVE_TIMEOUT_TB 5000   /* 500 ms для быстрых ответов */
#define TIMEOUT_TEST_TB    20000  /* 2 s (timeout_ms=1000, attempts=1 → ждём > 1 s) */

static volatile int g_server_running = 1;
static volatile uint16_t g_server_port = 0;

static void platform_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

/* собрать qname (завершается точкой) из query по смещению 12 */
static void extract_qname(const uint8_t* q, int n, char* out, int cap) {
    int p = 12, l = 0;
    while (p < n && q[p] != 0 && l < cap - 1) {
        if ((q[p] & 0xC0) == 0xC0) { p += 2; break; }
        int lab = q[p++];
        for (int j = 0; j < lab && p < n && l < cap - 1; j++) out[l++] = (char)q[p++];
        out[l++] = '.';
    }
    out[l] = '\0';
}

/* конец question-секции (после qname + qtype + qclass) */
static int question_end(const uint8_t* q, int n) {
    int p = 12;
    while (p < n && q[p] != 0) {
        if ((q[p] & 0xC0) == 0xC0) { return p + 2 + 4; }
        p += 1 + q[p];
    }
    return p + 1 + 4;
}

/* собрать DNS-ответ: qid + вопрос (из query) + ответы. rcode и ancount задаются. */
static int build_response(const uint8_t* q, int n, uint8_t* out, int cap,
                          int rcode, int ancount,
                          const uint16_t* atype, const uint8_t** adata, const uint16_t* alen) {
    int qend = question_end(q, n);
    if (qend > n) return -1;
    int o = 0;

    out[o++] = q[0]; out[o++] = q[1];                 /* qid */
    uint16_t flags = (uint16_t)(0x8180 | rcode);      /* QR + RD + RA + rcode */
    out[o++] = (uint8_t)(flags >> 8); out[o++] = (uint8_t)(flags & 0xFF);
    out[o++] = 0; out[o++] = 1;                        /* qdcount */
    out[o++] = 0; out[o++] = (uint8_t)ancount;         /* ancount */
    out[o++] = 0; out[o++] = 0;                        /* nscount */
    out[o++] = 0; out[o++] = 0;                        /* arcount */

    memcpy(out + o, q + 12, (size_t)(qend - 12));      /* question */
    o += qend - 12;

    for (int i = 0; i < ancount && o + 12 + alen[i] <= cap; i++) {
        out[o++] = 0xC0; out[o++] = 0x0C;              /* имя = указатель на вопрос */
        out[o++] = (uint8_t)(atype[i] >> 8); out[o++] = (uint8_t)(atype[i] & 0xFF);
        out[o++] = 0; out[o++] = 1;                    /* class IN */
        out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 60; /* ttl */
        out[o++] = (uint8_t)(alen[i] >> 8); out[o++] = (uint8_t)(alen[i] & 0xFF);
        memcpy(out + o, adata[i], alen[i]); o += alen[i];
    }
    return o;
}

static void* dns_server_thread(void* arg) {
    (void)arg;
    socket_t sock = socket_create_udp(AF_INET);
    if (sock == SOCKET_INVALID) { g_server_running = 0; return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { socket_close_wrapper(sock); g_server_running = 0; return NULL; }
    socklen_t al = sizeof(addr);
    getsockname(sock, (struct sockaddr*)&addr, &al);
    g_server_port = ntohs(addr.sin_port);

    uint8_t buf[512];
    while (g_server_running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        // Таймаут DNS-запроса задаёт opts; select ограничивает только ожидание остановки сервера.
        struct timeval tv = {0, 10000};
        int r = select((int)(sock + 1), &fds, NULL, NULL, &tv);
        if (r <= 0) continue;

        struct sockaddr_in client; socklen_t cl = sizeof(client);
        ssize_t n = recvfrom(sock, (char*)buf, sizeof(buf), 0, (struct sockaddr*)&client, &cl);
        if (n < 12) continue;

        char qname[256];
        extract_qname(buf, (int)n, qname, sizeof(qname));

        uint8_t resp[512]; int rl = 0;

        if (strcmp(qname, "a.test.") == 0) {
            uint8_t ip[4]; inet_pton(AF_INET, "192.168.1.42", ip);
            const uint8_t* d = ip; uint16_t l = 4;
            rl = build_response(buf, (int)n, resp, sizeof(resp), 0, 1, (const uint16_t[]){1}, &d, &l);
        } else if (strcmp(qname, "multi.test.") == 0) {
            uint8_t ip1[4]; inet_pton(AF_INET, "10.0.0.1", ip1);
            uint8_t ip2[4]; inet_pton(AF_INET, "10.0.0.2", ip2);
            const uint8_t* d[2] = { ip1, ip2 }; uint16_t l[2] = {4, 4};
            rl = build_response(buf, (int)n, resp, sizeof(resp), 0, 2, (const uint16_t[]){1, 1}, d, l);
        } else if (strcmp(qname, "cname.test.") == 0) {
            /* CNAME → real.test., затем A */
            uint8_t cname[16] = { 4, 'r','e','a','l', 4, 't','e','s','t', 0 };
            uint8_t ip[4]; inet_pton(AF_INET, "192.168.1.99", ip);
            const uint8_t* d[2] = { cname, ip }; uint16_t l[2] = {11, 4};
            rl = build_response(buf, (int)n, resp, sizeof(resp), 0, 2, (const uint16_t[]){5, 1}, d, l);
        } else if (strcmp(qname, "nx.test.") == 0) {
            rl = build_response(buf, (int)n, resp, sizeof(resp), 3, 0, NULL, NULL, NULL);
        } else if (strcmp(qname, "silent.test.") == 0) {
            continue; /* молчим — таймаут */
        } else {
            continue; /* неизвестное имя — молчим */
        }

        if (rl > 0)
            sendto(sock, (const char*)resp, rl, 0, (struct sockaddr*)&client, sizeof(client));
    }
    socket_close_wrapper(sock);
    return NULL;
}

struct test_ctx {
    int done;
    struct adns_result res;
};

static void on_done(const struct adns_result* res, void* arg) {
    struct test_ctx* t = (struct test_ctx*)arg;
    t->res = *res;
    t->done = 1;
}

/* запустить резолв и дождаться коллбэка или таймаута. Возвращает 0=успех. */
static int run_resolve(struct UASYNC* ua, const char* name, struct adns_opts* opts,
                       struct test_ctx* t, uint64_t timeout_tb) {
    memset(t, 0, sizeof(*t));
    struct adns_query* q = adns_resolve(ua, name, opts, on_done, t);
    if (!q) return -1;
    uint64_t start = get_time_tb();
    while (!t->done && get_time_tb() - start < timeout_tb)
        uasync_poll(ua, 10);
    if (!t->done) { adns_cancel(q); return -2; }
    return 0;
}

#define ASSERT(c, msg) do { if (!(c)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL: %s", msg); goto fail; } } while (0)

int main(void) {
    int result = 1;
    struct UASYNC* ua = NULL;
    pthread_t srv;
    int srv_started = 0;

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_SOCKET, DEBUG_LEVEL_INFO);

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== async_dns test ===");

    /* ── 1. чистый парсер resolv.conf ── */
    {
        struct sockaddr_in out[4];
        const char* text = "# comment\nnameserver 8.8.8.8\nnameserver 1.1.1.1 # inline\nsearch foo\n";
        int n = adns_parse_resolv_conf(text, out, 4);
        ASSERT(n == 2, "parse_resolv_conf count");
        char ip[16];
        inet_ntop(AF_INET, &out[0].sin_addr, ip, sizeof(ip));
        ASSERT(strcmp(ip, "8.8.8.8") == 0, "parse_resolv_conf[0] == 8.8.8.8");
        inet_ntop(AF_INET, &out[1].sin_addr, ip, sizeof(ip));
        ASSERT(strcmp(ip, "1.1.1.1") == 0, "parse_resolv_conf[1] == 1.1.1.1");
        ASSERT(ntohs(out[0].sin_port) == 53, "parse_resolv_conf port 53");
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: parse_resolv_conf");

    /* ── 2. фейковый DNS-сервер ── */
    if (pthread_create(&srv, NULL, dns_server_thread, NULL) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "pthread_create failed");
        return 1;
    }
    srv_started = 1;
    uint64_t wstart = get_time_tb();
    while (g_server_port == 0 && get_time_tb() - wstart < (uint64_t)(SERVER_WAIT_MS * 10))
        platform_sleep_ms(5);
    if (g_server_port == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "fake DNS server did not bind");
        goto cleanup;
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "fake DNS server on 127.0.0.1:%u", g_server_port);

    ua = uasync_create();
    if (!ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "uasync_create failed");
        goto cleanup;
    }

    struct adns_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.server.sin_family = AF_INET;
    opts.server.sin_addr.s_addr = inet_addr("127.0.0.1");
    opts.server.sin_port = htons(g_server_port);
    opts.timeout_ms = 1000;
    opts.max_attempts = 1;

    struct test_ctx t;
    char ip[16];

    /* A-запись */
    ASSERT(run_resolve(ua, "a.test", &opts, &t, RESOLVE_TIMEOUT_TB) == 0, "a.test resolve completes");
    ASSERT(t.res.status == ADNS_OK, "a.test status OK");
    ASSERT(t.res.count == 1, "a.test count == 1");
    inet_ntop(AF_INET, &t.res.addrs[0].sin_addr, ip, sizeof(ip));
    ASSERT(strcmp(ip, "192.168.1.42") == 0, "a.test addr == 192.168.1.42");
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: A record (compression)");

    /* два A */
    ASSERT(run_resolve(ua, "multi.test", &opts, &t, RESOLVE_TIMEOUT_TB) == 0, "multi resolve");
    ASSERT(t.res.status == ADNS_OK && t.res.count == 2, "multi count == 2");
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: multiple A");

    /* CNAME + A */
    ASSERT(run_resolve(ua, "cname.test", &opts, &t, RESOLVE_TIMEOUT_TB) == 0, "cname resolve");
    ASSERT(t.res.status == ADNS_OK && t.res.count >= 1, "cname status OK");
    inet_ntop(AF_INET, &t.res.addrs[0].sin_addr, ip, sizeof(ip));
    ASSERT(strcmp(ip, "192.168.1.99") == 0, "cname addr == 192.168.1.99");
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: CNAME + A");

    /* NXDOMAIN */
    ASSERT(run_resolve(ua, "nx.test", &opts, &t, RESOLVE_TIMEOUT_TB) == 0, "nx resolve");
    ASSERT(t.res.status == ADNS_ERR_NODATA, "nx status NODATA");
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: NXDOMAIN");

    /* таймаут: сервер молчит */
    ASSERT(run_resolve(ua, "silent.test", &opts, &t, TIMEOUT_TEST_TB) == 0, "silent resolve");
    ASSERT(t.res.status == ADNS_ERR_TIMEOUT, "silent status TIMEOUT");
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: timeout");

    /* отмена: коллбэк не вызывается */
    {
        memset(&t, 0, sizeof(t));
        struct adns_query* q = adns_resolve(ua, "silent.test", &opts, on_done, &t);
        ASSERT(q != NULL, "cancel: resolve created");
        for (int i = 0; i < 50 && !t.done; i++) uasync_poll(ua, 10); /* немного покрутим */
        adns_cancel(q);
        for (int i = 0; i < 50; i++) uasync_poll(ua, 10);
        ASSERT(t.done == 0, "cancel: callback NOT invoked");
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: cancel");
    }

    result = 0;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== async_dns test PASSED ===");
    goto cleanup;

fail:
    result = 1;

cleanup:
    if (ua) uasync_destroy(ua, 0);
    uint64_t stop_started = get_time_us();
    if (srv_started) { g_server_running = 0; pthread_join(srv, NULL); }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "test DNS server stopped: started=%d elapsed_us=%llu",
               srv_started, (unsigned long long)(get_time_us() - stop_started));
    return result;
}
