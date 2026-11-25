/* Расчёт времени и отказы NTP: реальные модули/API, управляемые часы, DNS, UDP и таймеры без сети/ожиданий. */
#define _GNU_SOURCE
#include "../src/utun_instance.h"
#include "../src/ntp_time.h"
#include "../src/ntp_node_time.h"
#include "../src/db_schema.h"
#include "../src/transport_layer/etcp_api.h"
#include "../src/transport_layer/etcp.h"
#include "../lib/async_dns.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

#define FAKE_SOCKET 12345678
#define WALL_START_US 1700000000000000LL

struct timer {
    int active, delay_tb;
    timeout_callback_t callback;
    void* arg;
};
static struct timer timers[4];
static int64_t wall_us;
static int socket_open, socket_registered, creates, sends, peer_sends, address_updates, dns_calls, dns_cancels;
static uint8_t peer_flags;
static int fail_create, fail_nonblock, fail_send, fail_register, fail_dns;
static const char* fail_timer;
static int fail_alloc_at, alloc_attempts;
static socket_t_callback_t read_callback;
static void* read_arg;
static adns_done_cb dns_callback;
static void* dns_arg;
static uint8_t request[48], reply[48];
static int reply_length;

extern int __real_socket_close_wrapper(socket_t);
extern void* __real_u_malloc_impl(uint32_t, const char*);
extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern char* __real_u_strdup_impl(const char*, const char*);

static int allocation_fault(void) { return fail_alloc_at && ++alloc_attempts == fail_alloc_at; }
void* __wrap_u_malloc_impl(uint32_t size, const char* where) {
    return allocation_fault() ? NULL : __real_u_malloc_impl(size, where);
}
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    return allocation_fault() ? NULL : __real_u_calloc_impl(count, size, where);
}
char* __wrap_u_strdup_impl(const char* value, const char* where) {
    return allocation_fault() ? NULL : __real_u_strdup_impl(value, where);
}

int __wrap_gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    tv->tv_sec = wall_us / 1000000;
    tv->tv_usec = wall_us % 1000000;
    return 0;
}

void* __wrap_uasync_set_timeout(struct UASYNC* ua, int delay, void* arg, timeout_callback_t cb, const char* name) {
    assert(ua && cb && delay >= 0);
    assert(!strcmp(name, "ntp_sync") || !strcmp(name, "ntp_q") || !strcmp(name, "ntp_startup"));
    if (fail_timer && !strcmp(fail_timer, name)) { fail_timer = NULL; return NULL; }
    for (size_t i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
        if (timers[i].active) continue;
        timers[i] = (struct timer){.active = 1, .delay_tb = delay, .callback = cb, .arg = arg};
        return &timers[i];
    }
    assert(!"too many live timers");
    return NULL;
}

err_t __wrap_uasync_cancel_timeout(struct UASYNC* ua, void* handle) {
    assert(ua && handle);
    struct timer* t = handle;
    assert(t >= timers && t < timers + 4 && t->active);
    t->active = 0;
    return ERR_OK;
}

static void fire_timer(void* handle) {
    struct timer* t = handle;
    assert(t && t->active);
    struct timer saved = *t;
    t->active = 0;
    wall_us += (int64_t)saved.delay_tb * 100;
    saved.callback(saved.arg);
}

static void fire_query_timer(void) {
    for (int i = 0; i < 4; i++) {
        if (timers[i].active && timers[i].delay_tb == 20000) { fire_timer(&timers[i]); return; }
    }
    assert(!"no query timer");
}

socket_t __wrap_socket_create_udp(int family) {
    assert(family == AF_INET && !socket_open);
    creates++;
    if (fail_create) { errno = EMFILE; return SOCKET_INVALID; }
    socket_open = 1;
    return FAKE_SOCKET;
}
int __wrap_socket_set_nonblocking(socket_t sock) {
    assert(sock == FAKE_SOCKET && socket_open);
    errno = EIO;
    return fail_nonblock ? -1 : 0;
}
int __wrap_socket_close_wrapper(socket_t sock) {
    if (sock != FAKE_SOCKET) return __real_socket_close_wrapper(sock);
    assert(socket_open && !socket_registered);
    socket_open = 0;
    return 0;
}
ssize_t __wrap_socket_sendto(socket_t sock, const void* data, size_t len, const struct sockaddr* addr, socklen_t addr_len) {
    assert(sock == FAKE_SOCKET && socket_open && len == sizeof(request));
    assert(addr && addr_len == sizeof(struct sockaddr_in));
    sends++;
    memcpy(request, data, len);
    errno = EIO;
    return fail_send ? -1 : (ssize_t)len;
}
ssize_t __wrap_socket_recvfrom(socket_t sock, void* data, size_t len, struct sockaddr* addr, socklen_t* addr_len) {
    assert(sock == FAKE_SOCKET && socket_open && len >= sizeof(reply));
    (void)addr; (void)addr_len;
    memcpy(data, reply, sizeof(reply));
    return reply_length;
}
void* __wrap_uasync_add_socket_t(struct UASYNC* ua, socket_t sock, socket_t_callback_t r,
                                socket_t_callback_t w, socket_t_callback_t e, const char* name, void* arg) {
    assert(ua && sock == FAKE_SOCKET && socket_open && !socket_registered && r && !w && !e);
    assert(!strcmp(name, "ntp_query"));
    if (fail_register) return NULL;
    socket_registered = 1;
    read_callback = r; read_arg = arg;
    return &socket_registered;
}
err_t __wrap_uasync_remove_socket_t(struct UASYNC* ua, socket_t sock) {
    assert(ua && sock == FAKE_SOCKET && socket_registered);
    socket_registered = 0; read_callback = NULL; read_arg = NULL;
    return ERR_OK;
}

struct adns_query* __wrap_adns_resolve(struct UASYNC* ua, const char* name, const struct adns_opts* opts,
                                      adns_done_cb cb, void* arg) {
    assert(ua && name && cb && !dns_callback);
    (void)opts;
    dns_calls++;
    if (fail_dns) return NULL;
    dns_callback = cb; dns_arg = arg;
    return (struct adns_query*)&dns_callback;
}
void __wrap_adns_cancel(struct adns_query* query) {
    assert(query == (struct adns_query*)&dns_callback && dns_callback);
    dns_callback = NULL; dns_arg = NULL; dns_cancels++;
}
static void finish_dns(int status) {
    assert(dns_callback);
    adns_done_cb cb = dns_callback;
    void* arg = dns_arg;
    dns_callback = NULL; dns_arg = NULL;
    struct adns_result result = {.status = status, .count = status == ADNS_OK ? 1 : 0};
    result.addrs[0].sin_family = AF_INET;
    result.addrs[0].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cb(&result, arg);
}

void __wrap_topo_node_update_my_addresses(struct UTUN_INSTANCE* inst) { assert(inst); address_updates++; }
int __wrap_etcp_send(struct ETCP_CONN* conn, struct ll_entry* e) {
    assert(conn && e && e->len == 10 && e->dgram[0] == ETCP_ID_NTP_TIME);
    peer_sends++; peer_flags = e->dgram[1];
    queue_dgram_free(e); queue_entry_free(e);
    return 0;
}

struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE inst;
    struct utun_config config;
    struct CFG_NTP_SERVER servers[2];
    struct ETCP_CONN conns[3];
};

static void setup(struct fixture* f) {
    memset(f, 0, sizeof(*f));
    memset(timers, 0, sizeof(timers));
    wall_us = WALL_START_US;
    creates = sends = peer_sends = address_updates = dns_calls = dns_cancels = 0;
    peer_flags = 0;
    fail_create = fail_nonblock = fail_send = fail_register = fail_dns = 0;
    fail_alloc_at = alloc_attempts = 0; fail_timer = NULL;
    f->ua = uasync_create();
    assert(f->ua);
    f->inst.ua = f->ua;
    f->inst.config = &f->config;
    f->inst.connections = queue_new(f->ua, 0, 0, 0, "ntp_test_connections");
    assert(f->inst.connections);
    f->config.global.ntp_enabled = 1;
    f->config.global.ntp_resync_interval = 3600;
    f->config.global.ntp_server_count = 2;
    f->config.global.ntp_servers = &f->servers[0];
    strcpy(f->servers[0].name, "a.invalid");
    strcpy(f->servers[1].name, "b.invalid");
    f->servers[0].next = &f->servers[1];
    for (int i = 0; i < 3; i++) {
        f->conns[i].instance = &f->inst;
        f->conns[i].peer_ed25519_pubkey[31] = i + 1;
    }
}

static void cleanup(struct fixture* f) {
    fail_alloc_at = 0;
    ntp_time_destroy(&f->inst);
    ntp_node_time_destroy(&f->inst);
    assert(!socket_open && !socket_registered && !dns_callback && !read_callback);
    for (int i = 0; i < 4; i++) assert(!timers[i].active);
    assert(!f->inst.new_conn_cbks && !f->inst.api_bindings.callbacks[ETCP_ID_NTP_TIME]);
    for (int i = 0; i < 3; i++) assert(!f->conns[i].cbks);
    struct ll_entry* e;
    while ((e = queue_data_get(f->inst.connections))) queue_entry_free(e);
    queue_free(f->inst.connections);
    uasync_destroy(f->ua, 0);
}

static void start(struct fixture* f, int direct) {
    assert(!ntp_time_init(&f->inst));
    if (direct) ntp_time_set_test_addr(&f->inst.ntp, "127.0.0.1", 11123);
    assert(!f->inst.ntp.sync && f->inst.ntp.timer);
    fire_timer(f->inst.ntp.timer);
}

static uint64_t ntp_stamp(int64_t us) {
    return ((uint64_t)(us / 1000000 + 2208988800LL) << 32) | ((uint64_t)(us % 1000000) << 32) / 1000000;
}
static uint64_t request_stamp(void) {
    uint64_t value;
    memcpy(&value, request + 40, sizeof(value));
    return be64toh(value);
}
static void deliver_reply(uint64_t origin, int64_t server_us) {
    memset(reply, 0, sizeof(reply));
    reply[0] = (4 << 3) | 4; reply[1] = 1;
    uint64_t value = htobe64(origin);
    memcpy(reply + 24, &value, sizeof(value));
    value = htobe64(ntp_stamp(server_us));
    memcpy(reply + 32, &value, sizeof(value));
    memcpy(reply + 40, &value, sizeof(value));
    reply_length = sizeof(reply);
    assert(read_callback);
    read_callback(FAKE_SOCKET, read_arg);
}
static void check_time(struct fixture* f, int64_t expected_us) {
    assert(ntp_time_is_synced(&f->inst));
    int64_t error = ntp_time_get_us(&f->inst) - expected_us;
    assert(error >= -2 && error <= 2);
    assert(ntp_time_get_seconds(&f->inst) == expected_us / 1000000);
}

static void test_offsets(struct fixture* f) {
    for (int sign = -1; sign <= 1; sign++) {
        start(f, 1);
        deliver_reply(request_stamp(), wall_us + sign * 5000000LL);
        assert(f->inst.ntp.offset_us == -sign * 5000000LL);
        check_time(f, wall_us + sign * 5000000LL);
        assert(f->inst.ntp.reachable && !f->inst.ntp.sync && f->inst.ntp.timer);
        assert(((struct timer*)f->inst.ntp.timer)->delay_tb == 36000000);
        ntp_time_destroy(&f->inst);
        check_time(f, wall_us + sign * 5000000LL);
    }
    assert(!dns_calls);
}

static void test_retry_timestamp(struct fixture* f) {
    start(f, 1);
    uint64_t first = request_stamp();
    fire_query_timer();
    uint64_t second = request_stamp();
    assert(first != second && sends == 2);
    deliver_reply(first, wall_us + 5000000);
    assert(!f->inst.ntp.synced && f->inst.ntp.sync);
    deliver_reply(second, wall_us + 5000000);
    check_time(f, wall_us + 5000000);
}

static void check_failed_cycle(struct fixture* f) {
    assert(!f->inst.ntp.sync && !f->inst.ntp.reachable && f->inst.ntp.timer);
    assert(!socket_open && !socket_registered && !dns_callback && address_updates == 1);
    int active = 0;
    for (int i = 0; i < 4; i++) active += timers[i].active;
    assert(active == 1 + f->inst.ntp.startup_wait && ((struct timer*)f->inst.ntp.timer)->delay_tb == 36000000);
}
static void test_async_dns_failure(struct fixture* f) {
    start(f, 0);
    finish_dns(ADNS_ERR_TIMEOUT);
    assert(dns_calls == 2 && f->inst.ntp.sync);
    finish_dns(ADNS_ERR_NODATA);
    assert(dns_calls == 2);
    check_failed_cycle(f);
    // Следующий цикл действительно запускается и может восстановить reachable.
    fire_timer(f->inst.ntp.timer);
    finish_dns(ADNS_OK);
    deliver_reply(request_stamp(), wall_us - 5000000);
    check_time(f, wall_us - 5000000);
    assert(f->inst.ntp.reachable && address_updates == 2 && f->inst.ntp.server_current == 1);
}
static void test_immediate_dns_failure(struct fixture* f) {
    fail_dns = 1;
    start(f, 0);
    assert(dns_calls == 2);
    check_failed_cycle(f);
}
static void test_udp_exhaustion(struct fixture* f) {
    start(f, 0);
    for (int server = 0; server < 2; server++) {
        finish_dns(ADNS_OK);
        for (int i = 0; i < 3; i++) fire_query_timer();
    }
    assert(dns_calls == 2 && creates == 2 && sends == 6);
    check_failed_cycle(f);
}
static void test_immediate_udp_failures(struct fixture* f) {
    strcpy(f->servers[0].name, "127.0.0.1");
    strcpy(f->servers[1].name, "127.0.0.2");
    int* faults[] = {&fail_create, &fail_nonblock, &fail_send, &fail_register};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        creates = sends = address_updates = 0;
        *faults[i] = 1;
        start(f, 0);
        assert(creates == 2 && !dns_calls);
        check_failed_cycle(f);
        ntp_time_destroy(&f->inst);
        *faults[i] = 0;
    }
}
static void test_timer_failures(struct fixture* f) {
    fail_timer = "ntp_startup";
    assert(ntp_time_init(&f->inst) == -1 && !f->inst.ntp.startup_wait && !f->inst.ntp.startup_timer);
    fail_timer = "ntp_sync";
    assert(ntp_time_init(&f->inst) == -1);
    assert(!f->inst.ntp.servers && !f->inst.ntp.timer && !f->inst.ntp.enabled);
    assert(!ntp_time_init(&f->inst));
    ntp_time_set_test_addr(&f->inst.ntp, "127.0.0.1", 11123);
    fail_timer = "ntp_q";
    fire_timer(f->inst.ntp.timer);
    check_failed_cycle(f);
    ntp_time_destroy(&f->inst);
    start(f, 1);
    fail_timer = "ntp_sync";
    deliver_reply(request_stamp(), wall_us + 5000000);
    check_time(f, wall_us + 5000000);
    assert(!f->inst.ntp.sync && !f->inst.ntp.timer && !socket_open);
}
static void test_cancel(struct fixture* f) {
    start(f, 0);
    assert(dns_callback);
    ntp_time_destroy(&f->inst);
    assert(!dns_callback && dns_cancels == 1 && !f->inst.ntp.sync);
    start(f, 1);
    assert(socket_registered);
    ntp_time_destroy(&f->inst);
    assert(!read_callback && !socket_open && !f->inst.ntp.sync);
}
static void test_init_failures(struct fixture* f) {
    size_t baseline = u_get_allocated_count();
    for (int i = 1; i <= 3; i++) {
        alloc_attempts = 0; fail_alloc_at = i;
        assert(ntp_time_init(&f->inst) == -1);
        fail_alloc_at = 0;
        assert(!f->inst.ntp.servers && !f->inst.ntp.timer && u_get_allocated_count() == baseline);
    }
    f->config.global.ntp_server_count = 3;
    assert(ntp_time_init(&f->inst) == -1 && u_get_allocated_count() == baseline);
    f->config.global.ntp_server_count = 2;
    f->config.global.ntp_resync_interval = INT_MAX;
    assert(ntp_time_init(&f->inst) == -1);
    f->config.global.ntp_resync_interval = 3600;
    assert(!ntp_time_init(&f->inst));
    void* timer = f->inst.ntp.timer;
    assert(ntp_time_init(&f->inst) == -1 && timer == f->inst.ntp.timer);
    alloc_attempts = 0; fail_alloc_at = 1;
    fire_timer(timer);
    fail_alloc_at = 0;
    assert(!f->inst.ntp.sync && f->inst.ntp.timer);
}

static void register_connection(struct fixture* f, int index) {
    struct ll_entry* e = queue_entry_new(sizeof(struct conn_queue_entry));
    assert(e);
    ((struct conn_queue_entry*)e->data)->conn = &f->conns[index];
    assert(!queue_data_put(f->inst.connections, e));
    for (struct etcp_inst_cbk_entry* cb = f->inst.new_conn_cbks; cb; cb = cb->next) cb->fn(&f->conns[index], cb->arg);
}
static void receive_peer(struct fixture* f, struct ETCP_CONN* conn, int synced, int64_t time_us) {
    struct ll_entry* e = queue_entry_new(0);
    assert(e);
    e->dgram = u_malloc(10);
    assert(e->dgram);
    e->len = 10; e->dgram[0] = ETCP_ID_NTP_TIME; e->dgram[1] = synced;
    memcpy(e->dgram + 2, &time_us, sizeof(time_us));
    assert(f->inst.api_bindings.callbacks[ETCP_ID_NTP_TIME]);
    f->inst.api_bindings.callbacks[ETCP_ID_NTP_TIME](conn, e);
}
static void test_peer_cache(struct fixture* f) {
    assert(!ntp_node_time_init(&f->inst));
    struct ETCP_CONN* conn = &f->conns[0];
    size_t baseline = u_get_allocated_count();
    for (int i = 0; i < NTP_NODE_MAX_PEERS; i++) {
        conn->peer_ed25519_pubkey[31] = i + 1;
        receive_peer(f, conn, 0, wall_us);
        f->inst.ntp_node.peers[i].last_seen_tb = i + 100;
    }
    conn->peer_ed25519_pubkey[31] = 1;
    receive_peer(f, conn, 0, wall_us); // обновление сохраняет запись 1; вытеснить надо запись 2
    conn->peer_ed25519_pubkey[31] = 100;
    struct ETCP_LINK link = {.rtt_last = 200}; // 20 мс, половина — 10000 мкс
    conn->links = &link;
    receive_peer(f, conn, 1, wall_us + 4990000);
    conn->links = NULL;
    check_time(f, wall_us + 5000000);
    assert(!f->inst.ntp.enabled && f->inst.ntp_node.peer_count == NTP_NODE_MAX_PEERS);
    assert(f->inst.ntp_node.peers[0].ed25519[31] == 1 && f->inst.ntp_node.peers[1].ed25519[31] == 100);
    // Ещё один новый пир получает ответ и не меняет уже принятую поправку.
    conn->peer_ed25519_pubkey[31] = 101;
    receive_peer(f, conn, 0, wall_us - 5000000);
    check_time(f, wall_us + 5000000);
    assert(peer_sends == 1 && u_get_allocated_count() == baseline);
    ntp_node_time_destroy(&f->inst);
    assert(!f->inst.ntp_node.peer_count);
    check_time(f, wall_us + 5000000);
}

/* Неверный raw-конверт не меняет часы/список пиров; корректное время не требует CHAT-членства. */
static void test_peer_wire(struct fixture* f) {
    assert(!ntp_node_time_init(&f->inst));
    for (unsigned length = 0; length <= 11; length++) {
        if (length == 10) continue;
        struct ll_entry* packet = ll_alloc_lldgram(11); assert(packet);
        packet->len = length; packet->dgram[0] = ETCP_ID_NTP_TIME; packet->dgram[1] = 3;
        memcpy(packet->dgram + 2, &wall_us, sizeof(wall_us));
        f->inst.api_bindings.callbacks[ETCP_ID_NTP_TIME](&f->conns[0], packet);
        assert(!f->inst.ntp.synced && !f->inst.ntp_node.peer_count && !peer_sends);
    }
    struct ll_entry* packet = ll_alloc_lldgram(10); assert(packet);
    packet->len = 10; packet->dgram[0] = ETCP_ID_NCD_CONTROL; packet->dgram[1] = 3;
    memcpy(packet->dgram + 2, &wall_us, sizeof(wall_us));
    f->inst.api_bindings.callbacks[ETCP_ID_NTP_TIME](&f->conns[0], packet);
    assert(!f->inst.ntp.synced && !f->inst.ntp_node.peer_count && !peer_sends);
    assert(!f->inst.topo_groups && !f->inst.topo_sqlite_db);
    receive_peer(f, &f->conns[0], 3, wall_us + 5000000);
    assert(f->inst.ntp.source == NTP_TIME_PEER && f->inst.ntp_node.peer_count == 1);
    assert(!memcmp(f->inst.ntp_node.peers[0].ed25519, f->conns[0].peer_ed25519_pubkey, 32));
    check_time(f, wall_us + 5000000);
}
static void test_sources(struct fixture* f) {
    start(f, 1);
    assert(!ntp_node_time_init(&f->inst));
    receive_peer(f, &f->conns[0], 3, wall_us + 7000000);
    check_time(f, wall_us + 7000000);
    assert(f->inst.ntp.source == NTP_TIME_PEER && f->inst.ntp.startup_wait);
    deliver_reply(request_stamp(), wall_us + 5000000);
    check_time(f, wall_us + 5000000); // Свой NTP заменяет свежего пира и открывает обмен групп.
    assert(f->inst.ntp.source == NTP_TIME_SERVER && !f->inst.ntp.startup_wait && !f->inst.ntp.startup_timer);
    fire_timer(f->inst.ntp.timer);
    for (int i = 0; i < 3; i++) fire_query_timer();
    check_failed_cycle(f);
    check_time(f, wall_us + 5000000); // отказ следующего цикла не уничтожает принятую поправку
    receive_peer(f, &f->conns[0], 3, wall_us - 5000000);
    check_time(f, wall_us + 5000000); // уже синхронизированный узел не принимает другую поправку пира
    assert(f->inst.ntp.offsets[NTP_TIME_PEER].offset_us == 5000000); // резерв всё равно обновился
}

static void test_peer_fallback(struct fixture* f) {
    register_connection(f, 0); f->conns[0].initialized = f->conns[0].links_up = 1;
    start(f, 1); assert(!ntp_node_time_init(&f->inst));
    assert(f->inst.ntp.startup_wait && !peer_flags);
    fire_timer(f->inst.ntp.startup_timer);
    assert(!f->inst.ntp.startup_wait && f->inst.ntp.sync); // 4 секунды, не ждём конца опроса
    int before = peer_sends;
    for (int i = 0; i < 3; i++) fire_query_timer();
    check_failed_cycle(f);
    assert(peer_sends == before + 1 && !peer_flags); // Не ждём нового подключения: сами просим время.
    receive_peer(f, &f->conns[0], 1, wall_us + 7000000);
    check_time(f, wall_us + 7000000); assert(peer_flags == 1);
    assert(f->inst.ntp.source == NTP_TIME_SAVED); // чужой кэш не становится свежей синхронизацией
    fire_timer(f->inst.ntp.timer);
    receive_peer(f, &f->conns[0], 3, wall_us - 5000000);
    check_time(f, wall_us - 5000000); assert(peer_flags == 3);
    receive_peer(f, &f->conns[0], 1, wall_us + 9000000);
    check_time(f, wall_us - 5000000); // кэш слабее свежей поправки
    receive_peer(f, &f->conns[0], 3, wall_us - 6000000);
    check_time(f, wall_us - 6000000); // новое измерение того же уровня обновляет часы
    deliver_reply(request_stamp(), wall_us + 2000000);
    check_time(f, wall_us + 2000000); assert(f->inst.ntp.reachable);
    receive_peer(f, &f->conns[0], 3, wall_us + 9000000);
    check_time(f, wall_us + 2000000); // Ответ NTP заменил резерв и не заменяется пиром.
}

static void test_peer_without_ntp(struct fixture* f) {
    assert(!ntp_node_time_init(&f->inst));
    for (int empty = 0; empty <= 1; empty++) {
        f->config.global.ntp_enabled = empty;
        f->config.global.ntp_server_count = empty ? 0 : 2;
        assert(!ntp_time_init(&f->inst));
        assert(!f->inst.ntp.timer && !f->inst.ntp.sync);
        receive_peer(f, &f->conns[0], 1, wall_us + 5000000);
        check_time(f, wall_us + 5000000);
        ntp_time_destroy(&f->inst);
    }
    assert(!sends && !dns_calls);
}

static int64_t saved_offset(struct fixture* f, int source) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->inst.topo_sqlite_db, "SELECT offset_us FROM clock_offsets WHERE source=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int(st, 1, source) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int64_t value = sqlite3_column_int64(st, 0);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    return value;
}
static void test_saved_priorities(struct fixture* f) {
    assert(sqlite3_open(":memory:", &f->inst.topo_sqlite_db) == SQLITE_OK);
    assert(!db_schema_table(f->inst.topo_sqlite_db, "clock_offsets", NULL));
    register_connection(f, 0); f->conns[0].initialized = f->conns[0].links_up = 1;
    f->config.global.ntp_enabled = 0;
    assert(!ntp_time_init(&f->inst) && !ntp_node_time_init(&f->inst));
    receive_peer(f, &f->conns[0], 2, wall_us); // fresh без synced запрещён
    receive_peer(f, &f->conns[0], 7, wall_us); // неизвестные флаги
    receive_peer(f, &f->conns[0], 3, INT64_MAX);
    assert(ntp_time_record_offset(&f->inst, NTP_TIME_SERVER, INT64_MIN) == -1);
    assert(!f->inst.ntp.synced && !f->inst.ntp_node.peer_count);
    receive_peer(f, &f->conns[0], 1, wall_us + 1000000);
    receive_peer(f, &f->conns[0], 3, wall_us + 2000000);
    assert(ntp_time_record_offset(&f->inst, NTP_TIME_SERVER, -3000000) == 1);
    receive_peer(f, &f->conns[0], 1, wall_us + 4000000);
    receive_peer(f, &f->conns[0], 3, wall_us + 5000000);
    check_time(f, wall_us + 3000000);
    assert(saved_offset(f, NTP_TIME_SAVED) == -4000000 && saved_offset(f, NTP_TIME_PEER) == -5000000);
    assert(saved_offset(f, NTP_TIME_SERVER) == -3000000);
    ntp_time_destroy(&f->inst);
    assert(!ntp_time_init(&f->inst));
    check_time(f, wall_us + 3000000);
    assert(f->inst.ntp.source == NTP_TIME_SAVED && !f->inst.ntp.offsets[NTP_TIME_SERVER].fresh);
    ntp_node_sync_peers(&f->inst); assert(peer_flags == 1); // кэш после перезапуска не объявляется свежим
    receive_peer(f, &f->conns[0], 3, wall_us + 6000000);
    check_time(f, wall_us + 6000000); assert(peer_flags == 3);
    assert(ntp_time_record_offset(&f->inst, NTP_TIME_SERVER, 0) == 1);
    check_time(f, wall_us); // нулевая поправка — полноценное измерение
    ntp_time_destroy(&f->inst); assert(!ntp_time_init(&f->inst));
    check_time(f, wall_us); assert(f->inst.ntp.source == NTP_TIME_SAVED);
    // Отказ записи не теряет живое измерение и не вмешивается в чужую транзакцию.
    assert(sqlite3_exec(f->inst.topo_sqlite_db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    assert(ntp_time_record_offset(&f->inst, NTP_TIME_SERVER, -7000000) == 1);
    assert(!sqlite3_get_autocommit(f->inst.topo_sqlite_db) && saved_offset(f, NTP_TIME_SERVER) == 0);
    assert(sqlite3_exec(f->inst.topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    check_time(f, wall_us + 7000000);
    ntp_time_destroy(&f->inst);
    assert(sqlite3_exec(f->inst.topo_sqlite_db, "DELETE FROM clock_offsets WHERE source=3", NULL, NULL, NULL) == SQLITE_OK);
    assert(!ntp_time_init(&f->inst)); check_time(f, wall_us + 6000000); // нет своего NTP — берём сохранённого пира
    ntp_time_destroy(&f->inst);
    assert(sqlite3_exec(f->inst.topo_sqlite_db, "UPDATE clock_offsets SET offset_us=9223372036854775807", NULL, NULL, NULL) == SQLITE_OK);
    assert(!ntp_time_init(&f->inst) && !f->inst.ntp.synced); // повреждённый кэш не портит часы
    assert(sqlite3_close(f->inst.topo_sqlite_db) == SQLITE_OK); f->inst.topo_sqlite_db = NULL;
}
static void test_startup_deadline(struct fixture* f) {
    start(f, 0);
    assert(f->inst.ntp.startup_wait && dns_callback);
    assert(((struct timer*)f->inst.ntp.startup_timer)->delay_tb == 40000);
    fire_timer(f->inst.ntp.startup_timer);
    assert(wall_us == WALL_START_US + 4000000 && !f->inst.ntp.startup_wait && dns_callback);
    finish_dns(ADNS_OK); deliver_reply(request_stamp(), wall_us + 1000000);
    check_time(f, wall_us + 1000000); // продолжающийся опрос всё ещё может улучшить время
    ntp_time_destroy(&f->inst);
    fail_dns = 1; start(f, 0);
    assert(f->inst.ntp.startup_wait && !f->inst.ntp.sync); // быстрый отказ всех серверов не сокращает окно
    fire_timer(f->inst.ntp.startup_timer);
    assert(!f->inst.ntp.startup_wait);
}
static int foreign_calls;
static void foreign_callback(struct ETCP_CONN* conn, int event, void* arg) {
    (void)conn; (void)event; (void)arg;
    foreign_calls++;
}
static void test_node_lifecycle(struct fixture* f) {
    register_connection(f, 0); // pending до init
    register_connection(f, 1); // initialized до init
    f->conns[1].initialized = f->conns[1].links_up = 1;
    etcp_conn_add_cbk(&f->conns[0], foreign_callback, f, ETCP_CBK_EVENT_INIT);
    struct etcp_cbk_entry* foreign = f->conns[0].cbks;
    assert(foreign);
    for (int cycle = 0; cycle < 2; cycle++) {
        int before = peer_sends;
        assert(!ntp_node_time_init(&f->inst));
        assert(peer_sends == before + 1);
        assert(!ntp_node_time_init(&f->inst) && peer_sends == before + 1);
        if (!cycle) register_connection(f, 2); // новое pending после init
        etcp_cbk_fire(&f->conns[0], ETCP_CBK_EVENT_INIT);
        etcp_cbk_fire(&f->conns[2], ETCP_CBK_EVENT_INIT);
        assert(peer_sends == before + 3);
        ntp_node_time_destroy(&f->inst);
        ntp_node_time_destroy(&f->inst);
        assert(f->conns[0].cbks == foreign && !foreign->next);
        assert(!f->conns[1].cbks && !f->conns[2].cbks);
        etcp_cbk_fire(&f->conns[0], ETCP_CBK_EVENT_INIT);
        etcp_cbk_fire(&f->conns[2], ETCP_CBK_EVENT_INIT);
        assert(peer_sends == before + 3);
    }
    assert(foreign_calls == 4);
    etcp_conn_remove_cbk(&f->conns[0], foreign_callback, f);
    // Отказ регистрации на каждом шаге должен снять уже установленные подписки.
    for (int i = 1; i <= 4; i++) {
        alloc_attempts = 0; fail_alloc_at = i;
        assert(ntp_node_time_init(&f->inst) == -1);
        fail_alloc_at = 0;
        assert(!f->inst.ntp_node.active && !f->inst.new_conn_cbks);
        assert(!f->inst.api_bindings.callbacks[ETCP_ID_NTP_TIME]);
        for (int j = 0; j < 3; j++) assert(!f->conns[j].cbks);
    }
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    void (*tests[])(struct fixture*) = {test_offsets, test_retry_timestamp, test_async_dns_failure,
        test_immediate_dns_failure, test_udp_exhaustion, test_immediate_udp_failures, test_timer_failures,
        test_cancel, test_init_failures, test_peer_cache, test_sources, test_peer_fallback, test_peer_without_ntp, test_node_lifecycle,
        test_saved_priorities, test_startup_deadline, test_peer_wire};
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        size_t baseline = u_get_allocated_count();
        struct fixture f;
        setup(&f);
        tests[i](&f);
        cleanup(&f);
        assert(u_get_allocated_count() == baseline);
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP regression %zu passed, no live allocations", i + 1);
    }
    return 0;
}
