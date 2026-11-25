/* Калибровка каналов: BWlim смешанных размеров, задержка, ограниченный HOLD, DROP и teardown с таймерами. */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include "dummynet.h"
#include "test_clock.h"
#include "etcp.h"
#include "../lib/platform_compat.h"
#include "../lib/mem.h"
#include "../lib/memory_pool.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL %s:%d: %s", __func__, __LINE__, #x); exit(1); } } while (0)
static struct UASYNC* ua;
static struct dummynet* dn;
static socket_t peers[2];
static struct sockaddr_in proxy;
static unsigned received[2];
static uint64_t bytes[2], last_rx[2];
static uint8_t seen[2][512];
static int fail_delay, fail_shaper, fail_packet, fail_delay_arg, fail_send;
static socket_t fail_fd = SOCKET_INVALID;
static int fail_context, fail_pool, fail_queue, fail_registration;
static int fail_freeze, measure_freezes;
static uint64_t freeze_tx_clock[2], freeze_tx_count[2], freeze_first_us, freeze_last_paused, freeze_last_send;

void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* location) {
    if (fail_context && strstr(location, "dummynet.c")) return NULL;
    return __real_u_calloc_impl(count, size, location);
}

struct memory_pool* __real_memory_pool_init(size_t, const char*);
struct memory_pool* __wrap_memory_pool_init(size_t size, const char* name) {
    if (fail_pool && !strcmp(name, "pkt_pool")) return NULL;
    return __real_memory_pool_init(size, name);
}

struct ll_queue* __real_queue_new(struct UASYNC*, size_t, uint16_t, uint16_t, char*);
struct ll_queue* __wrap_queue_new(struct UASYNC* loop, size_t hash, uint16_t offset, uint16_t size, char* name) {
    if (fail_queue && !strncmp(name, "dummynet", 8) && !--fail_queue) return NULL;
    return __real_queue_new(loop, hash, offset, size, name);
}

void* __real_uasync_add_socket_t(struct UASYNC*, socket_t, socket_t_callback_t, socket_t_callback_t,
                                socket_t_callback_t, const char*, void*);
void* __wrap_uasync_add_socket_t(struct UASYNC* loop, socket_t fd, socket_t_callback_t read, socket_t_callback_t write,
                                socket_t_callback_t error, const char* name, void* arg) {
    if (fail_registration && !strcmp(name, "dummynet")) return NULL;
    return __real_uasync_add_socket_t(loop, fd, read, write, error, name, arg);
}

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* loop, int delay, void* arg, timeout_callback_t fn, const char* name) {
    if ((fail_delay && !strcmp(name, "dummynet_delay")) || (fail_shaper && !strcmp(name, "dummynet_shaper")) ||
        (fail_freeze && !strcmp(name, "dummynet_freeze"))) return NULL;
    return __real_uasync_set_timeout(loop, delay, arg, fn, name);
}

struct ll_entry* __real_queue_entry_new_from_pool(struct memory_pool*);
struct ll_entry* __wrap_queue_entry_new_from_pool(struct memory_pool* pool) {
    if (fail_packet) return NULL;
    return __real_queue_entry_new_from_pool(pool);
}

void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* location) {
    if (fail_delay_arg && strstr(location, "dummynet.c")) return NULL;
    return __real_u_malloc_impl(size, location);
}

ssize_t __real_socket_sendto(socket_t, const void*, size_t, const struct sockaddr*, socklen_t);
ssize_t __wrap_socket_sendto(socket_t fd, const void* data, size_t size, const struct sockaddr* addr, socklen_t len) {
    if (fail_send && ((dn && fd == dummynet_get_socket(dn)) || fd == fail_fd)) { errno = EIO; return -1; }
    if (measure_freezes && dn && fd == dummynet_get_socket(dn)) {
        struct dummynet_freeze_stats s; CHECK(!dummynet_get_freeze_stats(dn, &s));
        CHECK(!s.active_since_us && size == 1000);
        unsigned side = ((const struct sockaddr_in*)addr)->sin_port == htons(socket_get_bound_port(peers[1])) ? 0 : 1;
        freeze_last_send = get_time_us();
        uint64_t service = freeze_last_send - s.paused_us;
        // При 80 кбит/с каждый пакет требует 100 мс активной работы; допускаем 2 мс точности таймеров.
        CHECK(service + 2000 >= freeze_tx_clock[side] + 100000);
        freeze_tx_clock[side] = service; freeze_tx_count[side]++; freeze_last_paused = s.paused_us;
    }
    return __real_socket_sendto(fd, data, size, addr, len);
}

static void setup(unsigned bw, unsigned delay, unsigned capacity) {
    ua = uasync_create(); CHECK(ua);
    dn = dummynet_create(ua, "127.0.0.1", 0); CHECK(dn);
    memset(received, 0, sizeof(received)); memset(bytes, 0, sizeof(bytes)); memset(seen, 0, sizeof(seen));
    memset(&proxy, 0, sizeof(proxy)); proxy.sin_family = AF_INET;
    proxy.sin_addr.s_addr = htonl(INADDR_LOOPBACK); proxy.sin_port = htons(dummynet_get_listen_port(dn));
    for (int i = 0; i < 2; i++) {
        peers[i] = socket_create_udp(AF_INET); CHECK(peers[i] != SOCKET_INVALID);
        struct sockaddr_in address = proxy; address.sin_port = 0;
        CHECK(bind(peers[i], (struct sockaddr*)&address, sizeof(address)) == 0);
        CHECK(socket_set_nonblocking(peers[i]) == 0);
        int size = 4 * 1024 * 1024;
        CHECK(setsockopt(peers[i], SOL_SOCKET, SO_RCVBUF, (const char*)&size, sizeof(size)) == 0);
        CHECK(dummynet_set_direction(dn, 1-i, delay, 0, bw, capacity, 0,
            "127.0.0.1", socket_get_bound_port(peers[i])) == 0);
    }
}

/* Проверяем каждый байт и уникальность независимо от статистики эмулятора. */
static void poll_channels(int timeout) {
    test_clock_poll(ua, timeout);
    if (measure_freezes) {
        struct dummynet_freeze_stats s; CHECK(!dummynet_get_freeze_stats(dn, &s));
        // Конец и следующее начало в одном callback не должны задним числом включать отправку в паузу.
        CHECK(!s.active_since_us || freeze_last_send <= s.active_since_us);
    }
    for (int i = 0; i < 2; i++) {
        uint8_t data[1500]; ssize_t n;
        while ((n = socket_recvfrom(peers[i], data, sizeof(data), NULL, NULL)) >= 0) {
            CHECK(n >= 4);
            unsigned seq = data[0] | (data[1] << 8); CHECK(seq < 512 && !seen[i][seq]);
            seen[i][seq] = 1;
            for (ssize_t p = 2; p < n; p++) CHECK(data[p] == (uint8_t)(seq + p));
            received[i]++; bytes[i] += n; last_rx[i] = get_time_us();
        }
        CHECK(socket_get_error() == ERR_WOULDBLOCK || socket_get_error() == ERR_AGAIN);
    }
}

static void send_packet(int from, unsigned seq, unsigned size) {
    uint8_t data[1500]; CHECK(size >= 4 && size <= sizeof(data));
    data[0] = seq; data[1] = seq >> 8;
    for (unsigned i = 2; i < size; i++) data[i] = seq + i;
    CHECK(socket_sendto(peers[from], data, size, (struct sockaddr*)&proxy, sizeof(proxy)) == (ssize_t)size);
    poll_channels(0);
}

static void run_for(unsigned ms) {
    uint64_t end = get_time_us() + ms * 1000ULL;
    while (get_time_us() < end) poll_channels(10);
}

static void teardown(void) {
    dummynet_destroy(dn); dn = NULL;
    for (int i = 0; i < 2; i++) socket_close_wrapper(peers[i]);
    uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
}

/* Пачка уже лежит в kernel socket до poll: приём не должен ограничиваться одним пакетом за событие. */
static void receive_batch(void) {
    setup(8000, 0, 64);
    CHECK(!dummynet_set_mode(dn, 0, DUMMYNET_HOLD));
    for (unsigned seq = 0; seq < 32; seq++) {
        uint8_t data[128]; data[0] = seq; data[1] = 0;
        for (unsigned i = 2; i < sizeof(data); i++) data[i] = seq + i;
        CHECK(socket_sendto(peers[0], data, sizeof(data), (struct sockaddr*)&proxy, sizeof(proxy)) == sizeof(data));
    }
    uasync_poll(ua, 0);
    const struct dummynet_stats* stats = dummynet_get_stats(dn, 0);
    CHECK(stats->recv == 32 && stats->queue_size == 32 && !stats->dropped);
    CHECK(!dummynet_set_mode(dn, 0, DUMMYNET_RUN));
    run_for(50);
    CHECK(received[1] == 32 && bytes[1] == 32 * 128);
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS UDP receive batch: 32 queued datagrams in one poll, complete delivery");
}

static void bandwidth(unsigned bw) {
    setup(bw, 20, 512);
    uint64_t start = get_time_us(), total = 0;
    for (unsigned seq = 0; seq < 256; seq++) {
        unsigned len = seq & 1 ? 1500 : 100;
        total += len;
        send_packet(0, seq, len); send_packet(1, seq, len);
    }
    uint64_t expected = total * 8000 / bw + 20000, end = start + expected + 2000000;
    while ((received[0] < 256 || received[1] < 256) && get_time_us() < end) poll_channels(10);
    for (int i = 0; i < 2; i++) {
        uint64_t elapsed = last_rx[i] - start;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BW calibration: kbps=%u dir=%d bytes=%llu elapsed_us=%llu expected_us=%llu", bw, i,
            (unsigned long long)bytes[i], (unsigned long long)elapsed, (unsigned long long)expected);
        CHECK(received[i] == 256 && bytes[i] == total);
        CHECK(elapsed * 100 >= expected * 95 && elapsed * 100 <= expected * 105 + 5000);
        const struct dummynet_stats* stats = dummynet_get_stats(dn, 1-i);
        CHECK(stats->sent_bytes == total && !stats->queue_size && !stats->lost && !stats->dropped);
        CHECK(stats->max_queue_delay_us <= elapsed && stats->max_queue_delay_us >= expected - 40000);
    }
    teardown();
}

static void hold_drop(void) {
    setup(80, 20, 8);
    CHECK(dummynet_set_mode(dn, 0, DUMMYNET_HOLD) == 0);
    for (unsigned seq = 0; seq < 12; seq++) send_packet(0, seq, 1000);
    run_for(60);
    const struct dummynet_stats* stats = dummynet_get_stats(dn, 0);
    CHECK(!received[1] && stats->queue_size == 8 && stats->dropped == 4);
    dummynet_reset_stats(dn, 0); CHECK(stats->queue_size == 8 && stats->queue_max == 8);
    uint64_t resumed = get_time_us();
    CHECK(dummynet_set_mode(dn, 0, DUMMYNET_RUN) == 0);
    run_for(50); CHECK(!received[1]);
    run_for(850); CHECK(received[1] == 8 && last_rx[1] - resumed >= 795000);
    // DROP удаляет как delay, так и сериализуемую голову; RUN не возвращает потерянное.
    send_packet(0, 20, 1000); run_for(5);
    CHECK(dummynet_set_mode(dn, 0, DUMMYNET_DROP) == 0);
    send_packet(0, 21, 1000); run_for(50);
    CHECK(stats->blocked == 2 && !stats->queue_size && received[1] == 8);
    CHECK(dummynet_set_mode(dn, 0, DUMMYNET_RUN) == 0);
    send_packet(0, 22, 1000); run_for(25);
    CHECK(dummynet_set_mode(dn, 0, DUMMYNET_DROP) == 0);
    CHECK(stats->blocked == 3 && !stats->queue_size);
    CHECK(dummynet_set_mode(dn, 0, DUMMYNET_RUN) == 0);
    send_packet(0, 23, 1000); run_for(150); CHECK(received[1] == 9);
    // Уничтожение одновременно с delay и shaper: после destroycallbacks не должны обращаться к dn.
    send_packet(0, 24, 1000); run_for(25); send_packet(1, 25, 1000);
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS bounded HOLD, paced resume, DROP of pending packets and active teardown");
}

/* Ошибка настройки не меняет действующий канал; повторное изменение BW допустимо с очередью. */
static void configuration(void) {
    setup(0, 0, 16);
    uint16_t port = socket_get_bound_port(peers[1]);
    CHECK(dummynet_set_direction(dn, 0, 123, 456, 1, 0, 1000, "bad address", port) == -1);
    send_packet(0, 0, 100); send_packet(1, 1, 100); run_for(5);
    CHECK(received[0] == 1 && received[1] == 1);
    CHECK(dummynet_set_direction(dn, 0, UINT32_MAX, UINT32_MAX, 0, 16, 0, "127.0.0.1", port) == -1);
    CHECK(dummynet_set_direction(dn, 0, 10000, 1, 0, 16, 0, "127.0.0.1", port) == -1);
    CHECK(dummynet_set_direction(dn, 0, 0, 0, 0, UINT32_MAX, 0, "127.0.0.1", port) == -1);
    CHECK(dummynet_set_direction(dn, 0, 0, 0, 0, 16, 0, "127.0.0.1", socket_get_bound_port(peers[0])) == -1);
    CHECK(dummynet_set_direction(dn, 0, 0, 0, 0, 16, 0, "0.0.0.0", port) == -1);
    CHECK(dummynet_set_direction(dn, 0, 20, 0, 80, 16, 0, "127.0.0.1", port) == 0);
    send_packet(0, 2, 1000); run_for(25);
    CHECK(dummynet_set_direction(dn, 0, 0, 0, 8000, 16, 0, "127.0.0.1", port) == 0);
    CHECK(dummynet_set_direction(dn, 0, 0, 0, 0, 16, 0, "127.0.0.2", port) == -1);
    run_for(150); CHECK(received[1] == 2 && !dummynet_get_stats(dn, 0)->queue_size);
    uint64_t count = UINT64_MAX;
    dummynet_get_debug_counters(NULL, -1, NULL, &count, NULL, NULL); CHECK(!count);
    dummynet_get_debug_counters(dn, 0, NULL, NULL, NULL, NULL);
    dummynet_reset_stats(dn, -2); CHECK(dummynet_get_stats(dn, 0)->recv == 2);
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS atomic channel configuration, bounds, distinct peers and queued BW changes");
}

/* Нельзя пересылать усечённую UDP-датаграмму. Пустая датаграмма — допустимый пакет. */
static void packet_bounds(void) {
    setup(0, 0, 16);
    uint8_t large[65507];
    for (size_t i = 0; i < sizeof(large); i++) large[i] = (uint8_t)i;
    for (int i = 0; i < 2; i++) {
        size_t size = i ? sizeof(large) : DUMMYNET_MAX_PKT_SIZE + 1;
        CHECK(socket_sendto(peers[0], large, size, (struct sockaddr*)&proxy, sizeof(proxy)) == (ssize_t)size);
        test_clock_run_for(ua, 50);
        uint8_t reply[1600];
        CHECK(socket_recvfrom(peers[1], reply, sizeof(reply), NULL, NULL) < 0);
    }
    const struct dummynet_stats* stats = dummynet_get_stats(dn, 0);
    CHECK(stats->recv == 2 && stats->dropped == 2 && !stats->sent && !stats->queue_size);
    CHECK(stats->recv_bytes == sizeof(large) + DUMMYNET_MAX_PKT_SIZE + 1);
    CHECK(socket_sendto(peers[0], large, 0, (struct sockaddr*)&proxy, sizeof(proxy)) == 0);
    test_clock_run_for(ua, 50);
    CHECK(socket_recvfrom(peers[1], large, sizeof(large), NULL, NULL) == 0);
    CHECK(stats->recv == 3 && stats->sent == 1 && !stats->sent_bytes);
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS UDP boundaries: oversized packets discarded intact, zero-byte packet delivered");
}

/* Каждый отказ теряет один пакет и оставляет направление пригодным для следующего. */
static void failures(void) {
    setup(80, 20, 16);
    int* faults[] = {&fail_packet, &fail_delay_arg, &fail_delay, &fail_shaper, &fail_send};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        *faults[i] = 1;
        send_packet(0, i * 2, 1000); run_for(150);
        *faults[i] = 0;
        const struct dummynet_stats* stats = dummynet_get_stats(dn, 0);
        CHECK(stats->dropped == i + 1 && !stats->queue_size && received[1] == i);
        send_packet(0, i * 2 + 1, 1000); run_for(150);
        CHECK(received[1] == i + 1 && !stats->queue_size);
    }
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS packet allocation, delay arg/timer, shaper timer and UDP send failures with recovery");
}

/* Два источника одновременно, оба направления: ни отправок в паузе, ни компенсации потерянной полосы. */
static void microfreezes(void) {
    setup(80, 0, 256);
    struct dummynet_freeze_spec specs[] = {{10, 30}, {0.5, 200}};
    CHECK(!dummynet_set_microfreezes(dn, specs, 2, 113));
    struct dummynet_freeze_spec bad = {NAN, 30};
    CHECK(dummynet_set_microfreezes(dn, &bad, 1, 1) == -1);
    bad = (struct dummynet_freeze_spec){-1, 30}; CHECK(dummynet_set_microfreezes(dn, &bad, 1, 1) == -1);
    bad = (struct dummynet_freeze_spec){1001, 30}; CHECK(dummynet_set_microfreezes(dn, &bad, 1, 1) == -1);
    bad = (struct dummynet_freeze_spec){1, 10001}; CHECK(dummynet_set_microfreezes(dn, &bad, 1, 1) == -1);
    CHECK(dummynet_set_microfreezes(dn, NULL, 1, 1) == -1);
    CHECK(dummynet_set_microfreezes(dn, specs, DUMMYNET_MAX_FREEZE_TYPES + 1, 1) == -1);
    fail_freeze = 1; CHECK(dummynet_set_microfreezes(dn, specs, 1, 1) == -1); fail_freeze = 0;
    struct dummynet_freeze_stats s; CHECK(!dummynet_get_freeze_stats(dn, &s) && s.type_count == 2);
    freeze_first_us = get_time_us(); freeze_tx_clock[0] = freeze_tx_clock[1] = freeze_first_us;
    freeze_tx_count[0] = freeze_tx_count[1] = 0; freeze_last_send = 0; measure_freezes = 1;
    for (unsigned seq = 0; seq < 128; seq++) { send_packet(0, seq, 1000); send_packet(1, seq, 1000); }
    uint64_t end = freeze_first_us + 30000000;
    while ((received[0] < 128 || received[1] < 128) && get_time_us() < end) poll_channels(10);
    CHECK(received[0] == 128 && received[1] == 128 && freeze_tx_count[0] == 128 && freeze_tx_count[1] == 128);
    uint64_t elapsed = get_time_us() - freeze_first_us;
    CHECK(elapsed >= 12800000 + freeze_last_paused - 2000 && elapsed <= 12800000 + freeze_last_paused + 50000);
    measure_freezes = 0;
    CHECK(!dummynet_get_freeze_stats(dn, &s) && s.events[0] > 50 && s.events[1] && s.overlaps && s.paused_us > 500000);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "FREEZE calibration elapsed_us=%llu work_us=12800000 paused_us=%llu events=%llu/%llu overlaps=%llu",
               (unsigned long long)elapsed, (unsigned long long)freeze_last_paused, (unsigned long long)s.events[0],
               (unsigned long long)s.events[1], (unsigned long long)s.overlaps);
    // Ожидаем действующую паузу: отключение должно выпустить очередь, не оставить отменённые callbacks.
    end = get_time_us() + 2000000;
    do { poll_channels(10); CHECK(!dummynet_get_freeze_stats(dn, &s)); } while (!s.active_since_us && get_time_us() < end);
    CHECK(s.active_since_us);
    send_packet(0, 128, 1000);
    CHECK(!dummynet_set_microfreezes(dn, NULL, 0, 0));
    run_for(150); CHECK(received[1] == 129);
    CHECK(!dummynet_get_freeze_stats(dn, &s) && !s.type_count && !s.active_since_us);
    // Отказ следующего таймера тоже обязан завершить паузу и не заблокировать передачу.
    CHECK(!dummynet_set_microfreezes(dn, specs, 2, 113));
    end = get_time_us() + 2000000;
    do { poll_channels(10); CHECK(!dummynet_get_freeze_stats(dn, &s)); } while (!s.active_since_us && get_time_us() < end);
    CHECK(s.active_since_us); fail_freeze = 1;
    send_packet(0, 129, 1000); run_for(350); fail_freeze = 0;
    CHECK(received[1] == 130 && !dummynet_get_freeze_stats(dn, &s) && !s.type_count && !s.active_since_us);
    CHECK(!dummynet_set_microfreezes(dn, specs, 2, 113));
    end = get_time_us() + 2000000;
    do { poll_channels(10); CHECK(!dummynet_get_freeze_stats(dn, &s)); } while (!s.active_since_us && get_time_us() < end);
    CHECK(s.active_since_us); send_packet(0, 130, 1000);
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS concurrent microfreezes, overlap union, duplex service clock, disable, timer failure and active teardown");
}

/* Проверяет фактическую доставку inline-фильтра, независимо от его счётчиков. */
static void inline_receive(const uint8_t* expected, ssize_t length) {
    uint8_t data[128];
    ssize_t n = socket_recvfrom(peers[1], data, sizeof(data), NULL, NULL);
    if (length < 0) CHECK(n < 0 && (socket_get_error() == ERR_WOULDBLOCK || socket_get_error() == ERR_AGAIN));
    else CHECK(n == length && !memcmp(data, expected, (size_t)length));
}

/* Настоящий UDP ETCP_LINK: закрытие линка и ядра отвязывает живой фильтр, чужой hook не заменяется. */
static void inline_filter(void) {
    setup(0, 0, 16);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua,
        "[global]\nauto_sockets=no\n[ntp]\nenabled=no\n[server:udp]\naddr=127.0.0.1:0\ntype=public\n");
    CHECK(inst && inst->etcp_sockets);
    struct ETCP_CONN* connection = etcp_connection_create(inst, "inline filter test"); CHECK(connection);
    struct sockaddr_storage target = {0};
    struct sockaddr_in* address = (struct sockaddr_in*)&target;
    *address = proxy; address->sin_port = htons(socket_get_bound_port(peers[1]));
    struct ETCP_LINK* link = etcp_link_new(connection, inst->etcp_sockets, &target, 1); CHECK(link);
    CHECK(!dummynet_filter_create(NULL));
    struct dummynet_filter* filter = dummynet_filter_create(ua); CHECK(filter);
    struct dummynet_filter* second = dummynet_filter_create(ua); CHECK(second);
    CHECK(!dummynet_filter_attach(filter, link) && !dummynet_filter_attach(filter, link));
    CHECK(dummynet_filter_attach(second, link) == -1 && link->send_hook_ctx == filter);
    struct ETCP_LINK* tcp = etcp_link_new(connection, NULL, NULL, 0); CHECK(tcp);
    CHECK(dummynet_filter_attach(filter, tcp) == -1 && link->send_hook_ctx == filter);
    socket_t fd = inst->etcp_sockets->fd;
    uint8_t data[64]; memset(data, 0x5a, sizeof(data));
    CHECK(etcp_udp_send(link, fd, data, sizeof(data), (struct sockaddr*)address, sizeof(*address)) == sizeof(data));
    inline_receive(data, sizeof(data));
    dummynet_filter_set_loss(filter, UINT32_MAX);
    CHECK(etcp_udp_send(link, fd, data, sizeof(data), (struct sockaddr*)address, sizeof(*address)) == sizeof(data));
    inline_receive(NULL, -1);
    dummynet_filter_set_loss(filter, 0);
    for (unsigned i = 0; i < 4; i++)
        CHECK(!dummynet_filter_block_addr(filter, htonl(INADDR_LOOPBACK + i), address->sin_port));
    CHECK(!dummynet_filter_block_addr(filter, address->sin_addr.s_addr, address->sin_port));
    CHECK(dummynet_filter_block_addr(filter, htonl(INADDR_LOOPBACK + 4), address->sin_port) == -1);
    CHECK(etcp_udp_send(link, fd, data, sizeof(data), (struct sockaddr*)address, sizeof(*address)) == sizeof(data));
    inline_receive(NULL, -1);
    CHECK(!dummynet_filter_unblock_addr(filter, address->sin_addr.s_addr, address->sin_port));
    CHECK(!dummynet_filter_unblock_addr(filter, address->sin_addr.s_addr, address->sin_port));
    CHECK(etcp_udp_send(link, fd, data, sizeof(data), (struct sockaddr*)address, sizeof(*address)) == sizeof(data));
    inline_receive(data, sizeof(data));
    CHECK(etcp_udp_send(link, fd, data, 0, (struct sockaddr*)address, sizeof(*address)) == 0);
    inline_receive(data, 0);
    fail_send = 1; fail_fd = fd;
    CHECK(etcp_udp_send(link, fd, data, sizeof(data), (struct sockaddr*)address, sizeof(*address)) < 0);
    fail_send = 0; fail_fd = SOCKET_INVALID;
    inline_receive(NULL, -1);
    CHECK(etcp_udp_send(link, fd, data, sizeof(data), (struct sockaddr*)address, sizeof(*address)) == sizeof(data));
    inline_receive(data, sizeof(data));
    const struct dummynet_stats* stats = dummynet_filter_get_stats(filter);
    CHECK(stats->recv == 7 && stats->sent == 4 && stats->lost == 2 && stats->blocked == 1 && stats->dropped == 1);
    CHECK(stats->recv_bytes == 6 * sizeof(data) && stats->sent_bytes == 3 * sizeof(data));
    dummynet_filter_detach(filter);
    CHECK(!link->send_hook && !link->send_hook_ctx && !link->send_hook_release);
    CHECK(!dummynet_filter_attach(filter, link));
    link->active_users++; /* Удержать закрытую память для проверки отказа повторного attach. */
    etcp_link_close(link);
    CHECK(dummynet_filter_attach(filter, link) == -1);
    etcp_link_release(link);
    dummynet_filter_destroy(filter); /* link уже освобождён. */
    link = etcp_link_new(connection, inst->etcp_sockets, &target, 1); CHECK(link);
    CHECK(!dummynet_filter_attach(second, link));
    utun_instance_destroy(inst);
    dummynet_filter_destroy(second); /* Ядро уже освободило link. */
    teardown();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS inline UDP loss/block/zero/send failure, exclusive hook and independent link/core lifetime");
}

/* Все частично созданные ресурсы возвращаются до уничтожения общего event loop. */
static void startup_failures(void) {
    ua = uasync_create(); CHECK(ua);
    size_t baseline = u_get_allocated_count();
    CHECK(!dummynet_create(NULL, "127.0.0.1", 0));
    for (unsigned i = 0; i < 5; i++) {
        fail_context = i == 0; fail_pool = i == 1; fail_queue = i == 2 ? 1 : i == 3 ? 2 : 0; fail_registration = i == 4;
        CHECK(!dummynet_create(ua, "127.0.0.1", 0));
        fail_context = fail_pool = fail_queue = fail_registration = 0;
        uasync_poll(ua, 0);
        CHECK(u_get_allocated_count() == baseline);
        CHECK(ua->socket_alloc_count == ua->socket_free_count);
    }
    CHECK(!dummynet_create(ua, "invalid IP", 0));
    CHECK(u_get_allocated_count() == baseline);
    dn = dummynet_create(ua, "127.0.0.1", 0); CHECK(dn);
    size_t live = u_get_allocated_count();
    CHECK(!dummynet_create(ua, "127.0.0.1", dummynet_get_listen_port(dn)));
    CHECK(u_get_allocated_count() == live);
    dummynet_destroy(dn); dn = NULL;
    uasync_poll(ua, 0);
    CHECK(ua->socket_alloc_count == ua->socket_free_count && ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS partial startup rollback: context/pool/two queues/socket registration, invalid IP and occupied port");
}

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    test_clock_init(10);
    CHECK(socket_platform_init() == 0);
    size_t baseline = u_get_allocated_count();
    if (argc == 1) {
        bandwidth(2000); CHECK(u_get_allocated_count() == baseline);
        bandwidth(8000); CHECK(u_get_allocated_count() == baseline);
        hold_drop(); CHECK(u_get_allocated_count() == baseline);
    }
    struct { const char* name; void (*run)(void); } cases[] = {
        {"configuration", configuration}, {"packet_bounds", packet_bounds}, {"failures", failures}, {"receive_batch", receive_batch},
        {"inline", inline_filter}, {"startup", startup_failures}, {"microfreezes", microfreezes}
    };
    unsigned selected = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].name)) continue;
        selected++; cases[i].run(); CHECK(u_get_allocated_count() == baseline);
    }
    CHECK(selected && argc <= 2);
    test_clock_finish(); socket_platform_cleanup();
    return 0;
}
