/* Реальные UDP-сокеты и локальный STUN-стенд: транзакции, RFC5780, потери и независимость от ETCP. */
#include "../src/transport_layer/stun_nat.h"
#include "../src/transport_layer/etcp_connections.h"
#include "../src/transport_layer/crc32.h"
#include "../lib/debug_config.h"
#include "test_clock.h"
/* В конце fallback закрываем приём ETCP точно на границе между измерением и проверкой срока. */
#define NAT_FIXTURE_GLOBAL_OPTIONS "crypto_tx_threads=0\ncrypto_rx_threads=0\n"
#include "nat_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COOKIE 0x2112a442u

static unsigned udp_creations;
extern socket_t __real_socket_create_udp(int family);
socket_t __wrap_socket_create_udp(int family) { udp_creations++; return __real_socket_create_udp(family); }

struct fixture;
struct endpoint {
    struct fixture* fixture;
    socket_t fd;
    struct sockaddr_in addr;
    unsigned index, requests;
};

struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE inst;
    struct ETCP_SOCKET source, original;
    struct endpoint endpoints[6];
    enum stun_nat_mapping mapping;
    enum stun_nat_filtering filtering;
    int rfc, ignored_change, error_change, drop_alternates, drop_primary, drop_first, tamper, unstable, direct, multiple_clients;
    uint8_t first_id[12], late[80];
    size_t late_len;
    struct sockaddr_in test_client;
    unsigned bad_sent, retries, primary_bindings;
};

static uint16_t get16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Независимое кодирование атрибутов тестового сервера. */
static size_t add_address(uint8_t* out, size_t pos, uint16_t type, const struct sockaddr_in* addr) {
    put16(out + pos, type);
    put16(out + pos + 2, 8);
    out[pos + 4] = 0;
    out[pos + 5] = 1;
    put16(out + pos + 6, ntohs(addr->sin_port) ^ (type == 0x0020 ? COOKIE >> 16 : 0));
    put32(out + pos + 8, ntohl(addr->sin_addr.s_addr) ^ (type == 0x0020 ? COOKIE : 0));
    return pos + 12;
}

static size_t add_fingerprint(uint8_t* out, size_t pos) {
    put16(out + 2, (uint16_t)(pos + 8 - 20));
    put16(out + pos, 0x8028);
    put16(out + pos + 2, 4);
    put32(out + pos + 4, crc32_calc(out, pos) ^ 0x5354554eu);
    return pos + 8;
}

static void send_reply(struct endpoint* e, const uint8_t* data, size_t len, const struct sockaddr_in* client) {
    assert(socket_sendto(e->fd, data, len, (const struct sockaddr*)client, sizeof(*client)) == (ssize_t)len);
}

/* Эмулирует mapping по адресу назначения, filtering — выборочным пропуском CHANGE-ответов. */
static void server_read(socket_t fd, void* arg) {
    struct endpoint* e = arg;
    struct fixture* f = e->fixture;
    uint8_t request[128];
    struct sockaddr_in client = {0};
    socklen_t size = sizeof(client);
    ssize_t len = socket_recvfrom(fd, request, sizeof(request), (struct sockaddr*)&client, &size);
    assert(len == 28 || len == 36);
    assert(get16(request) == 1 && get32(request + 4) == COOKIE);
    assert(get32(request + len - 4) == (crc32_calc(request, (size_t)len - 8) ^ 0x5354554eu));
    unsigned flags = len == 36 ? get32(request + 24) : 0;
    e->requests++;
    if (!f->multiple_clients) {
        assert(client.sin_addr.s_addr == ((struct sockaddr_in*)&f->source.local_addr)->sin_addr.s_addr);
        assert(client.sin_port != ((struct sockaddr_in*)&f->source.local_addr)->sin_port);
        if (!f->test_client.sin_family) f->test_client = client;
        assert(client.sin_port == f->test_client.sin_port);
    }
    if (!e->index && !flags) f->primary_bindings++;
    if (f->drop_primary && !e->index) return;
    if (f->drop_alternates && e->index >= 1 && e->index <= 3) return;
    if (f->drop_first && !e->index && e->requests == 1) { memcpy(f->first_id, request + 8, 12); return; }
    if (f->drop_first && !e->index && e->requests == 2) { assert(!memcmp(f->first_id, request + 8, 12)); f->retries++; }
    if (flags && !f->ignored_change && !f->error_change) {
        if (f->filtering == STUN_FILTER_APDF || (flags == 6 && f->filtering == STUN_FILTER_ADF)) return;
    }
    unsigned reply_index = e->index;
    if (flags && !f->ignored_change && !f->error_change) reply_index = flags == 6 ? 3 : 1;
    struct endpoint* sender = &f->endpoints[reply_index];
    uint8_t response[80] = {0};
    put16(response, flags && f->error_change ? 0x0111 : 0x0101);
    put32(response + 4, COOKIE);
    memcpy(response + 8, request + 8, 12);
    size_t pos = 20;
    if (flags && f->error_change) {
        put16(response + pos, 0x0009);
        put16(response + pos + 2, 4);
        response[pos + 6] = f->error_change / 100;
        response[pos + 7] = f->error_change % 100;
        pos += 8;
    } else {
        struct sockaddr_in mapped = {0};
        mapped.sin_family = AF_INET;
        mapped.sin_addr.s_addr = htonl(0xcb007107u);
        unsigned port = 40000;
        if (f->mapping == STUN_MAP_ADM) port += (e->index / 2) * 10;
        if (f->mapping == STUN_MAP_APDM) port += e->index * 10;
        if (f->unstable && !e->index && f->primary_bindings > 1) port++;
        mapped.sin_port = htons(port);
        if (f->direct) mapped = client;
        pos = add_address(response, pos, 0x0020, &mapped);
        if (f->rfc && e->index < 4) {
            struct sockaddr_in other = f->endpoints[e->index ^ 3].addr;
            pos = add_address(response, pos, 0x802c, &other);
            pos = add_address(response, pos, 0x802b, &sender->addr);
        }
    }
    size_t response_len = add_fingerprint(response, pos);
    if (f->tamper && !e->index && e->requests == 1) {
        uint8_t bad[80];
        memcpy(bad, response, response_len);
        bad[response_len - 1] ^= 1;
        send_reply(sender, bad, response_len, &client);
        memcpy(bad, response, response_len);
        put16(bad + 22, 1000);
        send_reply(sender, bad, response_len, &client);
        memcpy(bad, response, response_len);
        bad[8] ^= 1;
        add_fingerprint(bad, pos);
        send_reply(sender, bad, response_len, &client);
        memcpy(bad, response, response_len);
        put16(bad + 26, 49999 ^ (COOKIE >> 16));
        add_fingerprint(bad, pos);
        send_reply(&f->endpoints[2], bad, response_len, &client);
        f->bad_sent = 4;
        memcpy(f->late, response, response_len);
        f->late_len = response_len;
    }
    if (f->late_len && e->index == 4) send_reply(&f->endpoints[0], f->late, f->late_len, &client);
    send_reply(sender, response, response_len, &client);
}

/* Создаёт четыре RFC5780 endpoint и два независимых обычных сервера в том же event loop. */
static void fixture_init(struct fixture* f) {
    if (!f->ua) f->ua = uasync_create();
    assert(f->ua);
    f->inst.ua = f->ua;
    f->source.fd = socket_create_udp(AF_INET);
    assert(f->source.fd != SOCKET_INVALID);
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "127.0.0.1", &local.sin_addr) == 1);
    assert(!bind(f->source.fd, (struct sockaddr*)&local, sizeof(local)));
    socklen_t size = sizeof(f->source.local_addr);
    assert(!getsockname(f->source.fd, (struct sockaddr*)&f->source.local_addr, &size));
    f->source.interface_addr = f->source.local_addr;
    f->source.instance = &f->inst;
    f->source.address_profile.mapping = ADDRESS_MAP_DEPENDENT;
    f->source.address_profile.inbound = ADDRESS_IN_CONTACT;
    f->source.address_profile.source = ADDRESS_SOURCE_ETCP;
    f->source.nat_addr = f->source.local_addr;
    snprintf(f->source.name, sizeof(f->source.name), "source");
    f->inst.etcp_sockets = &f->source;
    f->original = f->source;
    for (unsigned i = 0; i < 6; i++) {
        struct endpoint* e = &f->endpoints[i];
        e->fixture = f;
        e->index = i;
        e->fd = socket_create_udp(AF_INET);
        assert(e->fd != SOCKET_INVALID);
        assert(!socket_set_reuseaddr(e->fd, 0));
        assert(!socket_set_nonblocking(e->fd));
        e->addr.sin_family = AF_INET;
        e->addr.sin_addr.s_addr = htonl(0x7f000001u + i / 2);
        if (i == 2 || i == 3) e->addr.sin_port = f->endpoints[i - 2].addr.sin_port;
        if (i == 5) e->addr.sin_addr.s_addr = htonl(0x7f000004u);
        assert(!bind(e->fd, (struct sockaddr*)&e->addr, sizeof(e->addr)));
        size = sizeof(e->addr);
        assert(!getsockname(e->fd, (struct sockaddr*)&e->addr, &size));
        assert(uasync_add_socket_t(f->ua, e->fd, server_read, NULL, NULL, "fake_stun", e));
    }
}

/* Запускает настоящий модуль с конфигом; hostname-путь проверяется через /etc/hosts. */
static void fixture_start(struct fixture* f, int hostname, int reserve, int duplicate_ip) {
    char config[512];
    unsigned second = duplicate_ip ? 1 : 4;
    snprintf(config, sizeof(config), "[stun]\nenabled=yes\nserver=%s:%u\nserver=%s:%u\n",
        hostname ? "localhost" : "127.0.0.1", ntohs(f->endpoints[0].addr.sin_port),
        duplicate_ip ? "127.0.0.1" : "127.0.0.3", ntohs(f->endpoints[second].addr.sin_port));
    if (reserve) {
        size_t pos = strlen(config);
        snprintf(config + pos, sizeof(config) - pos, "server=127.0.0.4:%u\n", ntohs(f->endpoints[5].addr.sin_port));
    }
    f->inst.config = parse_config_from_buf(config, strlen(config), "test_stun_nat");
    assert(f->inst.config);
    assert(!stun_nat_init(&f->inst));
}

static struct stun_nat_result wait_result(struct fixture* f) {
    struct stun_nat_result result;
    uint64_t deadline = get_time_tb() + 3000000;
    do {
        assert(!stun_nat_get_result(&f->inst, "source", &result));
        if (result.status != STUN_NAT_PENDING) return result;
        test_clock_poll(f->ua, 10000);
    } while (get_time_tb() < deadline);
    DEBUG_ERROR(DEBUG_CATEGORY_NAT, "test STUN measurement exceeded virtual deadline");
    assert(0);
    return result;
}

/* Проверяет неизменность исходного fd, sockaddr, NAT-полей и отсутствие таймеров после destroy. */
static void fixture_destroy(struct fixture* f) {
    stun_nat_destroy(&f->inst);
    stun_nat_forget_ports(&f->inst);
    assert(!memcmp(&f->source, &f->original, sizeof(f->source)));
    struct sockaddr_storage addr = {0};
    socklen_t size = sizeof(addr);
    assert(!getsockname(f->source.fd, (struct sockaddr*)&addr, &size));
    assert(!memcmp(&addr, &f->source.local_addr, sizeof(addr)));
    assert(f->inst.nat_det == NULL);
    for (unsigned i = 0; i < 6; i++) {
        assert(!uasync_remove_socket_t(f->ua, f->endpoints[i].fd));
        socket_close_wrapper(f->endpoints[i].fd);
    }
    socket_close_wrapper(f->source.fd);
    free_config(f->inst.config);
    uasync_poll(f->ua, 0);
    assert(!f->ua->immediate_queue_head);
    assert(!timeout_heap_get_size(f->ua->timeout_heap));
    uasync_destroy(f->ua, 0);
}

static void test_config(void) {
    const char* good = "[stun]\nenabled=yes\nserver=one.example\nserver=two.example:1234\n";
    struct utun_config* cfg = parse_config_from_buf(good, strlen(good), "stun-config");
    assert(cfg && cfg->global.stun_enabled && cfg->global.stun_server_count == 2);
    assert(!strcmp(cfg->global.stun_servers->host, "one.example") && cfg->global.stun_servers->port == 3478);
    assert(cfg->global.stun_servers->next->port == 1234);
    free_config(cfg);
    char path[] = "/tmp/utun-stun-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && write(fd, good, strlen(good)) == (ssize_t)strlen(good));
    close(fd);
    cfg = parse_config(path);
    unlink(path);
    assert(cfg && cfg->global.stun_server_count == 2 && cfg->global.stun_servers->port == 3478);
    free_config(cfg);
    const char* bad[] = {
        "[stun]\nenabled=yes\nserver=one\n",
        "[stun]\nserver=one:0\n", "[stun]\nserver=one:65536\n", "[stun]\nserver=one:12x\n",
        "[stun]\nserver=one:1\nserver=ONE:1\n", "[stun]\nenabled=maybe\n", "[stun]\nwrong=yes\n"
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) assert(!parse_config_from_buf(bad[i], strlen(bad[i]), "bad-stun"));
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS STUN config: buffer/file, order, defaults, validation");
}

static void run_case(const char* name, struct fixture* f, enum stun_nat_mapping mapping, enum stun_nat_filtering filtering) {
    fixture_init(f);
    fixture_start(f, f->tamper, f->drop_primary, 0);
    struct stun_nat_result r = wait_result(f);
    assert(r.server_count == 2 && r.mapping == mapping && r.filtering == filtering);
    assert(r.translated == !f->direct);
    assert(r.local.ss_family == AF_INET && r.measured_at_tb);
    if (f->tamper) assert(f->bad_sent == 4);
    if (f->drop_first) assert(f->retries == 1);
    if (f->ignored_change || f->error_change) assert(r.filtering_reason == STUN_REASON_UNSUPPORTED);
    if (f->drop_alternates) assert(r.filtering_reason == STUN_REASON_TIMEOUT);
    if (f->unstable) assert(r.mapping_reason == STUN_REASON_UNSTABLE && r.filtering_reason == STUN_REASON_UNSTABLE);
    if (filtering != STUN_FILTER_UNKNOWN) assert(r.status == STUN_NAT_COMPLETE);
    else assert(r.status == STUN_NAT_PARTIAL);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS STUN %s mapping=%s filtering=%s", name,
        stun_nat_mapping_name(r.mapping), stun_nat_filtering_name(r.filtering));
    fixture_destroy(f);
}

/* Настоящий дополнительный рабочий порт; ссылка source остаётся собственностью теста. */
static struct ETCP_SOCKET extra_socket(struct fixture* f, const char* name, const char* ip) {
    struct ETCP_SOCKET socket = f->source;
    socket.next = NULL; socket.generation++;
    snprintf(socket.name, sizeof(socket.name), "%s", name);
    socket.fd = socket_create_udp(AF_INET); assert(socket.fd != SOCKET_INVALID);
    struct sockaddr_in local = {0}; local.sin_family = AF_INET;
    assert(inet_pton(AF_INET, ip, &local.sin_addr) == 1);
    assert(!bind(socket.fd, (struct sockaddr*)&local, sizeof(local)));
    socklen_t length = sizeof(socket.local_addr);
    assert(!getsockname(socket.fd, (struct sockaddr*)&socket.local_addr, &length));
    socket.interface_addr = socket.local_addr;
    return socket;
}

/* Дедупликация по IP/netif, смена представителя, независимые пути и неоднозначная политика. */
static void test_groups(void) {
    struct fixture f = {.mapping = STUN_MAP_EIM}; fixture_init(&f);
    struct ETCP_SOCKET duplicate = extra_socket(&f, "duplicate", "127.0.0.1");
    f.source.next = &duplicate; f.original = f.source;
    fixture_start(&f, 0, 0, 0);
    struct stun_nat_result first = wait_result(&f), second;
    assert(!stun_nat_get_socket_result(&f.inst, &duplicate, &second));
    assert(first.members == 2 && first.evidence_id == second.evidence_id && first.generation == second.generation);
    assert(first.measured_at_us == second.measured_at_us && first.local.ss_family == AF_INET);
    unsigned requests = f.endpoints[0].requests;
    f.inst.etcp_sockets = &duplicate; test_clock_run_for(f.ua, 30001);
    assert(!stun_nat_get_socket_result(&f.inst, &duplicate, &second));
    assert(second.members == 1 && second.evidence_id == first.evidence_id && f.endpoints[0].requests == requests);
    f.inst.etcp_sockets = &f.source; f.multiple_clients = 1;
    duplicate.netif_index = 1; test_clock_run_for(f.ua, 30001);
    uint64_t end = get_time_tb() + 3000000;
    do { assert(!stun_nat_get_socket_result(&f.inst, &duplicate, &second));
        if (second.status != STUN_NAT_PENDING) break;
        test_clock_poll(f.ua, 10000);
    } while (get_time_tb() < end);
    assert(second.key.netif_index == 1 && second.evidence_id != first.evidence_id);
    duplicate.netif_index = 0;
    if (!socket_bind_to_device(duplicate.fd, "lo")) {
        test_clock_run_for(f.ua, 30001);
        assert(!stun_nat_get_socket_result(&f.inst, &f.source, &second));
        assert(second.status == STUN_NAT_SKIPPED && second.mapping_reason == STUN_REASON_ROUTE);
        assert(!stun_nat_get_socket_result(&f.inst, &duplicate, &second));
        assert(second.mapping_reason == STUN_REASON_ROUTE);
    } else DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN routing conflict subtest unavailable: bind-to-device denied");
    socket_close_wrapper(duplicate.fd); duplicate = extra_socket(&f, "another-ip", "127.0.0.5");
    test_clock_run_for(f.ua, 30001); end = get_time_tb() + 3000000;
    do { assert(!stun_nat_get_socket_result(&f.inst, &duplicate, &second));
        if (second.status != STUN_NAT_PENDING) break;
        test_clock_poll(f.ua, 10000);
    } while (get_time_tb() < end);
    assert(!stun_nat_get_socket_result(&f.inst, &f.source, &first));
    assert(second.key.address[3] == 5 && first.key.address[3] == 1 && second.evidence_id != first.evidence_id);
    socket_close_wrapper(duplicate.fd); f.source.next = NULL; f.original = f.source;
    first = wait_result(&f); stun_nat_destroy(&f.inst);
    assert(f.inst.stun_port_history && !stun_nat_init(&f.inst));
    second = wait_result(&f);
    assert(second.generation != first.generation);
    assert(((const struct sockaddr_in*)&second.local)->sin_port != ((const struct sockaddr_in*)&first.local)->sin_port);
    fixture_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS STUN groups: duplicate ports, representative removal, separate netif, policy conflict");
}

/* Штатный refresh сохраняет прежний результат до TTL, не продлевая его возраст. */
static void test_cache(void) {
    struct fixture f = {.rfc = 1, .mapping = STUN_MAP_EIM, .filtering = STUN_FILTER_EIF,
        .multiple_clients = 1}; fixture_init(&f); fixture_start(&f, 0, 0, 0);
    struct stun_nat_result first = wait_result(&f), current;
    assert(first.status == STUN_NAT_COMPLETE);
    f.drop_primary = 1;
    test_clock_run_for(f.ua, 1830000);
    assert(!stun_nat_get_socket_result(&f.inst, &f.source, &current));
    assert(current.evidence_id == first.evidence_id && current.measured_at_us == first.measured_at_us && current.refreshing);
    test_clock_run_for(f.ua, 1300000);
    assert(!stun_nat_get_socket_result(&f.inst, &f.source, &current));
    assert(current.evidence_id != first.evidence_id && current.mapping == STUN_MAP_UNKNOWN && current.filtering == STUN_FILTER_UNKNOWN);
    fixture_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS STUN cache: automatic refresh, preserved age, failed refresh and expiry");
}

/* Recheck одновременно видит старый и новый путь: каждому нужен только один свежий тестовый fd. */
static void test_recheck_new_path(void) {
    struct fixture f = {.mapping = STUN_MAP_EIM, .multiple_clients = 1}; fixture_init(&f); fixture_start(&f, 0, 0, 0);
    struct stun_nat_result prior = wait_result(&f), current;
    struct ETCP_SOCKET added = extra_socket(&f, "added", "127.0.0.5");
    f.source.next = &added;
    unsigned before = udp_creations;
    stun_nat_recheck(&f.inst);
    assert(udp_creations == before + 2);
    current = wait_result(&f);
    assert(current.generation != prior.generation && current.evidence_id != prior.evidence_id);
    uint64_t deadline = get_time_tb() + 3000000;
    do {
        assert(!stun_nat_get_socket_result(&f.inst, &added, &current));
        if (current.status != STUN_NAT_PENDING) break;
        test_clock_poll(f.ua, 10000);
    } while (get_time_tb() < deadline);
    assert(current.mapping == STUN_MAP_MATCH);
    f.source.next = NULL; socket_close_wrapper(added.fd); fixture_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS STUN recheck creates one fresh probe per existing/new path");
}

static void wait_source(struct nat_fixture* nf, struct ETCP_SOCKET* socket, uint8_t source) {
    uint64_t end = get_time_tb() + 3000000;
    while (get_time_tb() < end) {
        if (socket->address_profile.source == source) return;
        test_clock_poll(nf->ua, 10000);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT source wait failed socket=%s source=%s expected=%s", socket->name,
        address_profile_source_name(socket->address_profile.source), address_profile_source_name(source));
    abort();
}

/* Оценка STUN относится к IP/netif, сохраняет рабочий порт и уступает измерению ETCP. */
static void test_fallback(int rfc) {
    struct nat_fixture nf; nat_fixture_init(&nf);
    struct fixture server = {.ua = nf.ua, .rfc = rfc, .mapping = STUN_MAP_EIM,
        .filtering = STUN_FILTER_EIF, .multiple_clients = 1}; fixture_init(&server);
    char config[512];
    snprintf(config, sizeof(config), "[stun]\nenabled=yes\nserver=127.0.0.1:%u\nserver=127.0.0.3:%u\n"
        "[server:duplicate]\naddr=127.0.0.1:0\ntype=public\n", ntohs(server.endpoints[0].addr.sin_port),
        ntohs(server.endpoints[4].addr.sin_port));
    struct UTUN_INSTANCE* owner = nat_fixture_node(&nf, "127.0.0.1", NULL, config);
    struct ETCP_SOCKET* socket = test_etcp_bound_socket(owner, "udp");
    struct ETCP_SOCKET* duplicate = test_etcp_bound_socket(owner, "duplicate");
    struct stun_nat_result diagnostic;
    uint64_t end = get_time_tb() + 3000000;
    do { test_clock_poll(nf.ua, 10000); assert(!stun_nat_get_socket_result(owner, socket, &diagnostic)); }
    while (get_time_tb() < end && diagnostic.status == STUN_NAT_PENDING);
    assert(diagnostic.mapping == (rfc ? STUN_MAP_EIM : STUN_MAP_MATCH));
    assert(diagnostic.filtering == (rfc ? STUN_FILTER_EIF : STUN_FILTER_UNKNOWN));
    wait_source(&nf, socket, ADDRESS_SOURCE_STUN); wait_source(&nf, duplicate, ADDRESS_SOURCE_STUN);
    assert(socket->address_profile.mapping == ADDRESS_MAP_EIM && socket->address_profile.inbound == ADDRESS_IN_UNKNOWN);
    assert(!memcmp(&socket->address_profile, &duplicate->address_profile, sizeof(socket->address_profile)));
    assert(!socket->nat_addr.ss_family && !duplicate->nat_addr.ss_family);
    nat_detection_set_allow_local(owner->nat_det, 0);
    wait_source(&nf, socket, ADDRESS_SOURCE_NONE);
    nat_detection_set_allow_local(owner->nat_det, 1);
    wait_source(&nf, socket, ADDRESS_SOURCE_STUN);
    struct nat_check_status status;
    end = get_time_tb() + 30000;
    do { test_clock_poll(nf.ua, 1000); assert(!nat_detection_get_status(owner->nat_det, socket, &status)); }
    while (get_time_tb() < end && status.reason != NAT_REASON_NO_OBSERVER);
    assert(status.reason == NAT_REASON_NO_OBSERVER);
    struct TOPO_NODE* snapshot = topo_node_registry_find(owner->topo_groups, owner->node_id);
    assert(snapshot && !topo_node_verify(snapshot));
    for (const struct TOPO_ADDR4* address = snapshot->v4_addrs; address; address = address->next)
        assert(address->type != TOPO_ADDR_NAT); /* тестовый внешний порт не опубликован */
    struct ETCP_SOCKET other = *socket;
    other.netif_index++;
    assert(stun_nat_get_socket_result(owner, &other, &diagnostic) < 0);
    other = *socket;
    ((struct sockaddr_in*)&other.local_addr)->sin_addr.s_addr = htonl(0x7f000009u);
    assert(stun_nat_get_socket_result(owner, &other, &diagnostic) < 0);
    struct ETCP_CONN peer = {.peer_node_id = 1};
    struct ETCP_LINK link = {.conn = socket, .etcp = &peer};
    struct sockaddr_storage observed = {0};
    struct sockaddr_in* external = (struct sockaddr_in*)&observed;
    external->sin_family = AF_INET; external->sin_addr.s_addr = htonl(0xcb007107u); external->sin_port = htons(45678);
    nat_detection_observed_by_peer(owner->nat_det, &link, &observed);
    assert(socket->address_profile.source == ADDRESS_SOURCE_STUN);
    assert(route_ping_addr_equal(&socket->nat_addr, &observed, 1));
    test_clock_run_for(nf.ua, 10001);
    int found = 0;
    for (const struct TOPO_ADDR4* address = snapshot->v4_addrs; address; address = address->next) {
        if (address->type != TOPO_ADDR_NAT) continue;
        assert(address->port == 45678 && !memcmp(address->addr, &external->sin_addr, 4)); found++;
    }
    assert(found == 1);
    /* Противоречащий внешний IP исключает STUN-оценку, даже при том же локальном пути. */
    external->sin_addr.s_addr = htonl(0xcb007108u);
    nat_detection_observed_by_peer(owner->nat_det, &link, &observed);
    assert(socket->address_profile.source == ADDRESS_SOURCE_ETCP && socket->address_profile.mapping == ADDRESS_MAP_UNKNOWN);
    external->sin_addr.s_addr = htonl(0xcb007107u);
    nat_detection_observed_by_peer(owner->nat_det, &link, &observed);
    assert(socket->address_profile.source == ADDRESS_SOURCE_STUN);
    /* Неудачный refresh не омолаживает кэш: EIM? исчезает после TTL. */
    server.drop_primary = 1;
    test_clock_run_for_local_io(nf.ua, STUN_NAT_TTL_TB + 30000);
    assert(socket->address_profile.source == ADDRESS_SOURCE_ETCP && socket->address_profile.mapping == ADDRESS_MAP_UNKNOWN);
    assert(route_ping_addr_equal(&socket->nat_addr, &observed, 1));
    server.drop_primary = 0; stun_nat_recheck(owner);
    wait_source(&nf, socket, ADDRESS_SOURCE_STUN);
    struct UTUN_INSTANCE* observer = nat_fixture_node(&nf, "127.0.0.5", NULL, NULL);
    nat_fixture_connect(&nf, owner, "udp", observer); nat_detection_request_check_all(owner->nat_det);
    end = get_time_tb() + 300000;
    do { test_clock_poll(nf.ua, 10000); nat_detection_get_status(owner->nat_det, socket, &status); }
    while (get_time_tb() < end && status.reason != NAT_REASON_NO_HELPER);
    assert(status.reason == NAT_REASON_NO_HELPER && socket->address_profile.source == ADDRESS_SOURCE_STUN);
    struct UTUN_INSTANCE* helper = nat_fixture_node(&nf, "127.0.0.6", NULL, NULL);
    nat_fixture_connect(&nf, observer, "udp", helper); nat_detection_request_check_all(owner->nat_det);
    wait_source(&nf, socket, ADDRESS_SOURCE_ETCP); wait_source(&nf, duplicate, ADDRESS_SOURCE_ETCP);
    /* Источник ETCP публикуется до конца обхода. Оба публичных сокета участвуют в фоне:
     * ответ/подключение к IP помощника до измерения оставляет UNKNOWN с точной причиной contact_history. */
    struct ETCP_SOCKET *native[] = {socket, duplicate};
    for (unsigned i = 0; i < 2; i++) {
        end = get_time_tb() + 400000;
        do {
            test_clock_poll(nf.ua, 1000);
            assert(!nat_detection_get_status(owner->nat_det, native[i], &status));
        } while (get_time_tb() < end && (status.phase || status.cycling));
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "STUN fallback native evidence: socket=%s mapping=%s inbound=%s reason=%s"
            " phase=%u cycling=%u history_complete=%u rounds=%u reselections=%u",
            native[i]->name, address_mapping_name(native[i]->address_profile.mapping),
            address_inbound_name(native[i]->address_profile.inbound), nat_check_reason_name(status.reason),
            status.phase, status.cycling, status.history_complete, status.rounds, status.helper_reselections);
        assert(!status.phase && !status.cycling && status.history_complete);
        assert(native[i]->address_profile.source == ADDRESS_SOURCE_ETCP && native[i]->address_profile.mapping == ADDRESS_MAP_REAL_IP);
        assert(native[i]->address_profile.inbound == ADDRESS_IN_OPEN ||
            (native[i]->address_profile.inbound == ADDRESS_IN_UNKNOWN && status.reason == NAT_REASON_CONTACT_HISTORY));
    }
    assert(!nat_detection_get_status(owner->nat_det, socket, &status));
    uint64_t round = status.round;
    struct address_profile measured = socket->address_profile;
    struct stun_nat_result prior, next;
    assert(!stun_nat_get_socket_result(owner, socket, &prior)); stun_nat_recheck(owner);
    end = get_time_tb() + 3000000;
    do { test_clock_poll(nf.ua, 10000); assert(!stun_nat_get_socket_result(owner, socket, &next)); }
    while (get_time_tb() < end && next.status == STUN_NAT_PENDING);
    assert(next.mapping == (rfc ? STUN_MAP_EIM : STUN_MAP_MATCH) && next.evidence_id != prior.evidence_id);
    assert(!memcmp(&socket->address_profile, &measured, sizeof(measured)));
    nat_detection_request_check_all(owner->nat_det); end = get_time_tb() + 300000;
    do { test_clock_poll(nf.ua, 10000); assert(!nat_detection_get_status(owner->nat_det, socket, &status)); }
    while (get_time_tb() < end && (status.round == round || status.phase || status.cycling));
    assert(status.round != round && !status.phase && !status.cycling);
    assert(!memcmp(&socket->address_profile, &measured, sizeof(measured)));
    /* Проверяем срок последнего ETCP без новых входящих соединений, которые могут подтвердить OPEN.
     * STUN на отдельном probe-сокете продолжает обновляться. */
    for (unsigned i = 0; i < 2; i++) assert(!uasync_set_socket_read(nf.ua, native[i]->socket_id, 0));
    unsigned bindings = server.primary_bindings;
    uint64_t timestamp = snapshot->timestamp;
    test_clock_run_for_local_io(nf.ua, 3000001);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "STUN fallback aged evidence: bindings=%u->%u inbound=%s->%s timestamp=%llu->%llu",
        bindings, server.primary_bindings, address_inbound_name(measured.inbound), address_inbound_name(socket->address_profile.inbound),
        (unsigned long long)timestamp, (unsigned long long)snapshot->timestamp);
    assert(server.primary_bindings > bindings);
    assert(!memcmp(&socket->address_profile, &measured, sizeof(measured)) && snapshot->timestamp == timestamp);
    for (unsigned i = 0; i < 6; i++) {
        assert(!uasync_remove_socket_t(nf.ua, server.endpoints[i].fd)); socket_close_wrapper(server.endpoints[i].fd);
    }
    socket_close_wrapper(server.source.fd); nat_fixture_cleanup(&nf);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS STUN EIM? RFC5780=%d: IP/netif, observed port, expiry and native priority", rfc);
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_NAT, DEBUG_LEVEL_INFO);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_NAT, DEBUG_LEVEL_DEBUG);
    test_clock_init(10000);
    test_config();
    test_groups();
    test_cache();
    test_recheck_new_path();
    struct fixture ordinary = { .mapping = STUN_MAP_EIM };
    run_case("ordinary equal", &ordinary, STUN_MAP_MATCH, STUN_FILTER_UNKNOWN);
    struct fixture dependent = { .mapping = STUN_MAP_ADM };
    run_case("ordinary dependent", &dependent, STUN_MAP_DEPENDENT, STUN_FILTER_UNKNOWN);
    struct fixture eif = { .rfc = 1, .mapping = STUN_MAP_EIM, .filtering = STUN_FILTER_EIF };
    run_case("RFC5780 EIM/EIF", &eif, STUN_MAP_EIM, STUN_FILTER_EIF);
    struct fixture adf = { .rfc = 1, .mapping = STUN_MAP_ADM, .filtering = STUN_FILTER_ADF };
    run_case("RFC5780 ADM/ADF", &adf, STUN_MAP_ADM, STUN_FILTER_ADF);
    struct fixture apdf = { .rfc = 1, .mapping = STUN_MAP_APDM, .filtering = STUN_FILTER_APDF };
    run_case("RFC5780 APDM/APDF", &apdf, STUN_MAP_APDM, STUN_FILTER_APDF);
    struct fixture ignored = { .rfc = 1, .ignored_change = 1, .mapping = STUN_MAP_EIM };
    run_case("ignored CHANGE", &ignored, STUN_MAP_EIM, STUN_FILTER_UNKNOWN);
    struct fixture error = { .rfc = 1, .error_change = 420, .mapping = STUN_MAP_EIM };
    run_case("CHANGE error 420", &error, STUN_MAP_EIM, STUN_FILTER_UNKNOWN);
    struct fixture dead = { .rfc = 1, .drop_alternates = 1, .mapping = STUN_MAP_EIM, .filtering = STUN_FILTER_APDF };
    run_case("dead alternate endpoints", &dead, STUN_MAP_MATCH, STUN_FILTER_UNKNOWN);
    struct fixture tamper = { .tamper = 1, .mapping = STUN_MAP_EIM };
    run_case("hostname, malformed/spoofed/late responses", &tamper, STUN_MAP_MATCH, STUN_FILTER_UNKNOWN);
    struct fixture loss = { .drop_first = 1, .mapping = STUN_MAP_EIM };
    run_case("packet loss/retransmission", &loss, STUN_MAP_MATCH, STUN_FILTER_UNKNOWN);
    struct fixture reserve = { .drop_primary = 1, .mapping = STUN_MAP_EIM };
    run_case("reserve server", &reserve, STUN_MAP_MATCH, STUN_FILTER_UNKNOWN);
    struct fixture unstable = { .unstable = 1, .mapping = STUN_MAP_EIM };
    run_case("unstable mapping", &unstable, STUN_MAP_UNKNOWN, STUN_FILTER_UNKNOWN);
    struct fixture direct = { .rfc = 1, .direct = 1, .mapping = STUN_MAP_EIM, .filtering = STUN_FILTER_EIF };
    run_case("no translation", &direct, STUN_MAP_EIM, STUN_FILTER_EIF);
    struct fixture helpers = { .mapping = STUN_MAP_EIM };
    fixture_init(&helpers);
    struct ETCP_SOCKET auxiliary = helpers.source;
    auxiliary.probe_only = 1;
    snprintf(auxiliary.name, sizeof(auxiliary.name), "nat-helper");
    auxiliary.next = &helpers.source;
    helpers.inst.etcp_sockets = &auxiliary;
    fixture_start(&helpers, 0, 0, 0);
    struct stun_nat_result helper_result = wait_result(&helpers);
    assert(helper_result.mapping == STUN_MAP_MATCH);
    assert(stun_nat_get_result(&helpers.inst, "nat-helper", &helper_result) < 0);
    fixture_destroy(&helpers);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS STUN skips auxiliary NAT helper sockets");
    struct fixture duplicate = { .mapping = STUN_MAP_EIM };
    fixture_init(&duplicate);
    fixture_start(&duplicate, 0, 0, 1);
    struct stun_nat_result result = wait_result(&duplicate);
    assert(result.server_count == 1 && result.mapping == STUN_MAP_UNKNOWN && result.mapping_reason == STUN_REASON_NO_PAIR);
    fixture_destroy(&duplicate);
    struct fixture cancelled = { .mapping = STUN_MAP_EIM };
    fixture_init(&cancelled);
    fixture_start(&cancelled, 1, 0, 0);
    fixture_destroy(&cancelled);
    struct fixture changed = { .mapping = STUN_MAP_EIM };
    fixture_init(&changed);
    fixture_start(&changed, 0, 0, 0);
    result = wait_result(&changed);
    stun_nat_recheck(&changed.inst);
    changed.test_client.sin_family = 0;
    struct stun_nat_result rechecked = wait_result(&changed);
    assert(rechecked.mapping == STUN_MAP_MATCH && rechecked.server_count == 2);
    assert(((struct sockaddr_in*)&rechecked.local)->sin_port != ((struct sockaddr_in*)&result.local)->sin_port);
    changed.inst.etcp_sockets = NULL;
    test_clock_run_for(changed.ua, 30001);
    assert(stun_nat_get_result(&changed.inst, "source", &result) < 0);
    changed.inst.etcp_sockets = &changed.source;
    changed.test_client.sin_family = 0;
    test_clock_run_for(changed.ua, 30001);
    result = wait_result(&changed);
    assert(result.mapping == STUN_MAP_MATCH);
    fixture_destroy(&changed);
    test_fallback(0); test_fallback(1);
    test_clock_finish();
    puts("PASS STUN independent sockets, RFC5780, fallback, lifecycle and ETCP isolation");
    return 0;
}
