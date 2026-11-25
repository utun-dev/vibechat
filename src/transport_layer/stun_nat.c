#include "stun_nat.h"
#include "etcp_connections.h"
#include "../utun_instance.h"
#include "../lib/async_dns.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "../ntp_time.h"
#include "crc32.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define STUN_COOKIE 0x2112a442u
#define STUN_FINGERPRINT_XOR 0x5354554eu
#define STUN_REQUEST 0x0001
#define STUN_SUCCESS 0x0101
#define STUN_ERROR 0x0111
#define STUN_MAPPED 0x0001
#define STUN_CHANGE 0x0003
#define STUN_ERROR_CODE 0x0009
#define STUN_XOR_MAPPED 0x0020
#define STUN_FINGERPRINT 0x8028
#define STUN_ORIGIN 0x802b
#define STUN_OTHER 0x802c
#define STUN_REQUESTS 7
#define STUN_SCAN_TB 30000
#define STUN_REFRESH_TB UINT64_C(1800000)
#define STUN_RETRY_TB UINT64_C(300000)

enum stun_stage { STUN_BASE, STUN_FILTER_BOTH, STUN_CONTROL_BOTH, STUN_FILTER_PORT,
                  STUN_CONTROL_PORT, STUN_CHECK_OTHER, STUN_CHECK_PORT, STUN_MAP_IP, STUN_MAP_PORT, STUN_VERIFY };

/* Снимок исходного сокета используется только для выбора того же сетевого пути. */
struct stun_source {
    struct stun_nat_key key;
    char name[MAX_CONN_NAME_LEN];
    struct sockaddr_in addr;
    uint32_t netif_index;
    int mark, fib, skipped;
    char device[64];
};

/* Порты не переиспользуются и после удаления/пересоздания группы в этом ядре. */
struct stun_ports { struct stun_ports* next; struct stun_nat_key key; uint8_t used[2048]; };

struct stun_history {
    struct stun_history* next;
    struct sockaddr_in addr;
};

struct stun_probe {
    struct stun_probe* next;
    struct STUN_NAT* owner;
    struct stun_source source;
    struct stun_nat_result result;
    struct stun_nat_result cached;
    struct stun_ports* ports;
    uint64_t generation, due;
    unsigned members;
    int changed, conflict, scan_conflict;
    socket_t fd;
    void* socket_handle;
    void* timer;
    struct adns_query* dns;
    const struct CFG_STUN_SERVER* cursor;
    uint16_t dns_port;
    struct stun_history* history;
    struct sockaddr_in primary, other, reference, base_mapping, ip_mapping, target, expected;
    enum stun_stage stage;
    uint8_t transaction[12];
    unsigned sent;
    uint64_t sent_at_tb;
    int seen, capability;
    enum stun_nat_filtering pending_filter;
};

struct STUN_NAT {
    struct UTUN_INSTANCE* inst;
    struct stun_probe* probes;
    struct stun_ports* ports;
    void* scan_timer;
    uint64_t next_transaction_tb;
};

struct stun_response {
    struct sockaddr_in mapped, other, origin;
    int have_mapped, have_other, have_origin, error;
};

static void next_server(struct stun_probe* p);
static void start_transaction(struct stun_probe* p, enum stun_stage stage, const struct sockaddr_in* target);
static void begin_mapping(struct stun_probe* p);
static void finish_server(struct stun_probe* p);
static void scan_sources(struct STUN_NAT* mod);

static int key_equal(const struct stun_nat_key* a, const struct stun_nat_key* b) {
    return a->family == b->family && a->netif_index == b->netif_index && !memcmp(a->address, b->address, 16);
}

static int policy_equal(const struct stun_source* a, const struct stun_source* b) {
    return a->mark == b->mark && a->fib == b->fib && a->skipped == b->skipped && !strcmp(a->device, b->device);
}

static int random_id(uint64_t* value) {
    do { if (random_bytes((uint8_t*)value, sizeof(*value))) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN group/evidence random generation failed"); return -1;
    } } while (!*value);
    return 0;
}

static uint16_t read16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t read32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void write16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void write32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static int same_addr(const struct sockaddr_in* a, const struct sockaddr_in* b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

const char* stun_nat_mapping_name(enum stun_nat_mapping v) {
    static const char* names[] = { "UNKNOWN", "MATCH", "DEPENDENT", "EIM", "ADM", "APDM" };
    return (unsigned)v < sizeof(names) / sizeof(names[0]) ? names[v] : "INVALID";
}

const char* stun_nat_filtering_name(enum stun_nat_filtering v) {
    static const char* names[] = { "UNKNOWN", "EIF", "ADF", "APDF" };
    return (unsigned)v < sizeof(names) / sizeof(names[0]) ? names[v] : "INVALID";
}

const char* stun_nat_reason_name(enum stun_nat_reason v) {
    static const char* names[] = { "ok", "RFC5780 unsupported", "prior STUN traffic", "timeout", "server error",
        "unstable mapping", "no independent server pair", "DNS failure", "socket I/O failure", "unsupported socket", "out of memory",
        "incompatible routing policy", "no fresh test port" };
    return (unsigned)v < sizeof(names) / sizeof(names[0]) ? names[v] : "invalid reason";
}

static const char* stage_name(enum stun_stage stage) {
    static const char* names[] = { "binding", "change-ip-port", "control-ip-port", "change-port",
                                  "control-port", "check-alternate-ip", "check-alternate-port", "mapping-ip", "mapping-port", "verify" };
    return names[stage];
}

/* Завершает владение сетевыми ресурсами; сам результат остаётся доступен через API. */
static void close_probe_io(struct stun_probe* p) {
    struct UASYNC* ua = p->owner->inst->ua;
    if (p->timer) { uasync_cancel_timeout(ua, p->timer); p->timer = NULL; }
    if (p->dns) { adns_cancel(p->dns); p->dns = NULL; }
    if (p->socket_handle) { uasync_remove_socket_t(ua, p->fd); p->socket_handle = NULL; }
    if (p->fd != SOCKET_INVALID) { socket_close_wrapper(p->fd); p->fd = SOCKET_INVALID; }
    while (p->history) { struct stun_history* next = p->history->next; u_free(p->history); p->history = next; }
}

/* Публикует только локальный снимок диагностики и закрывает тестовый порт. */
static void finish_probe(struct stun_probe* p) {
    struct stun_nat_result* r = &p->result;
    r->measured_at_tb = get_time_tb();
    int64_t now_us = ntp_time_get_us(p->owner->inst);
    if (now_us > 0) r->measured_at_us = (uint64_t)now_us;
    else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN measurement wall time unavailable source=%s", p->source.name);
    if (r->status == STUN_NAT_PENDING) {
        if (!r->server_count) r->status = STUN_NAT_FAILED;
        else if (r->server_count < 2 || r->mapping == STUN_MAP_UNKNOWN || r->filtering == STUN_FILTER_UNKNOWN) r->status = STUN_NAT_PARTIAL;
        else r->status = STUN_NAT_COMPLETE;
    }
    if (r->mapping_reason == STUN_REASON_UNSTABLE || r->filtering_reason == STUN_REASON_UNSTABLE)
        memset(&p->cached, 0, sizeof(p->cached));
    if (r->status == STUN_NAT_COMPLETE || (r->status == STUN_NAT_PARTIAL &&
        (r->mapping != STUN_MAP_UNKNOWN || r->filtering != STUN_FILTER_UNKNOWN))) p->cached = *r;
    p->due = r->status == STUN_NAT_COMPLETE ? r->measured_at_tb + STUN_REFRESH_TB : r->measured_at_tb + STUN_RETRY_TB;
    if (r->status == STUN_NAT_SKIPPED) p->due = UINT64_MAX;
    DEBUG_INFO(DEBUG_CATEGORY_NAT,
        "STUN result source=%s local=%s servers=%u mapping=%s (%s) filtering=%s (%s) translated=%d RFC5780=%d status=%d",
        p->source.name, sockaddr_storage_to_str(&r->local).str, r->server_count,
        stun_nat_mapping_name(r->mapping), stun_nat_reason_name(r->mapping_reason),
        stun_nat_filtering_name(r->filtering), stun_nat_reason_name(r->filtering_reason), r->translated, r->rfc5780, r->status);
    close_probe_io(p);
}

/* Сохраняет отправки только собственного STUN: повторный тест не должен открыть фильтр заранее. */
static int remember_target(struct stun_probe* p) {
    for (struct stun_history* h = p->history; h; h = h->next) if (same_addr(&h->addr, &p->target)) return 0;
    struct stun_history* h = u_malloc(sizeof(*h));
    if (!h) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s cannot allocate destination history", p->source.name); return -1; }
    h->addr = p->target;
    h->next = p->history;
    p->history = h;
    return 0;
}

/* Проверяет, что собственные предыдущие тесты не разрешили проверяемый источник. */
static int filter_is_clean(const struct stun_probe* p, int port_only) {
    for (const struct stun_history* h = p->history; h; h = h->next) {
        if (!port_only && h->addr.sin_addr.s_addr == p->other.sin_addr.s_addr) return 0;
        if (port_only && h->addr.sin_addr.s_addr == p->primary.sin_addr.s_addr && h->addr.sin_port == p->other.sin_port) return 0;
    }
    return 1;
}

/* После таймаута CHANGE проверяет обычный Binding, прежде чем судить о фильтрации. */
static void transaction_failed(struct stun_probe* p, enum stun_nat_reason reason) {
    DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s stage=%s failed: %s target=%s:%u attempts=%u",
        p->source.name, stage_name(p->stage), stun_nat_reason_name(reason),
        ip_to_str(&p->target.sin_addr, AF_INET).str, ntohs(p->target.sin_port), p->sent);
    switch (p->stage) {
        case STUN_BASE:
            p->result.mapping_reason = reason;
            next_server(p);
            break;
        case STUN_FILTER_BOTH:
        case STUN_FILTER_PORT:
            if (reason == STUN_REASON_TIMEOUT) {
                enum stun_stage next = p->stage == STUN_FILTER_BOTH ? STUN_CONTROL_BOTH : STUN_CONTROL_PORT;
                start_transaction(p, next, &p->primary);
            } else { p->result.filtering_reason = reason; begin_mapping(p); }
            break;
        case STUN_CONTROL_BOTH:
        case STUN_CONTROL_PORT:
        case STUN_CHECK_OTHER:
        case STUN_CHECK_PORT:
            p->result.filtering_reason = reason;
            begin_mapping(p);
            break;
        case STUN_MAP_IP:
        case STUN_MAP_PORT:
            p->result.mapping_reason = reason;
            finish_server(p);
            break;
        case STUN_VERIFY:
            p->result.mapping = STUN_MAP_UNKNOWN;
            p->result.mapping_reason = reason;
            p->result.filtering = STUN_FILTER_UNKNOWN;
            p->result.filtering_reason = STUN_REASON_UNSTABLE;
            finish_probe(p);
            break;
    }
}

/* Отправляет/повторяет одну транзакцию с тем же случайным ID и экспоненциальным RTO. */
static void send_request(void* arg) {
    struct stun_probe* p = arg;
    p->timer = NULL;
    if (p->sent == STUN_REQUESTS) { transaction_failed(p, STUN_REASON_TIMEOUT); return; }
    if (remember_target(p) < 0) {
        p->result.status = STUN_NAT_FAILED;
        p->result.mapping_reason = STUN_REASON_NOMEM;
        finish_probe(p);
        return;
    }
    uint8_t packet[36] = {0};
    unsigned flags = p->stage == STUN_FILTER_BOTH ? 6 : p->stage == STUN_FILTER_PORT ? 2 : 0;
    size_t fp_offset = flags ? 28 : 20;
    size_t len = fp_offset + 8;
    write16(packet, STUN_REQUEST);
    write16(packet + 2, (uint16_t)(len - 20));
    write32(packet + 4, STUN_COOKIE);
    memcpy(packet + 8, p->transaction, 12);
    if (flags) { write16(packet + 20, STUN_CHANGE); write16(packet + 22, 4); write32(packet + 24, flags); }
    write16(packet + fp_offset, STUN_FINGERPRINT);
    write16(packet + fp_offset + 2, 4);
    write32(packet + fp_offset + 4, crc32_calc(packet, fp_offset) ^ STUN_FINGERPRINT_XOR);
    ssize_t sent = socket_sendto(p->fd, packet, len, (const struct sockaddr*)&p->target, sizeof(p->target));
    if (sent != (ssize_t)len) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s send failed: sent=%zd expected=%zu error=%s",
            p->source.name, sent, len, socket_strerror(socket_get_error()));
        transaction_failed(p, STUN_REASON_IO);
        return;
    }
    p->sent_at_tb = get_time_tb();
    p->sent++;
    int delay_ms = p->sent == STUN_REQUESTS ? 8000 : 500 << (p->sent - 1);
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "STUN TX source=%s stage=%s target=%s:%u attempt=%u id=%08x%08x%08x RTO=%dms",
        p->source.name, stage_name(p->stage), ip_to_str(&p->target.sin_addr, AF_INET).str, ntohs(p->target.sin_port),
        p->sent, read32(p->transaction), read32(p->transaction + 4), read32(p->transaction + 8), delay_ms);
    p->timer = uasync_set_timeout(p->owner->inst->ua, delay_ms * 10, p, send_request, "stun_retry");
    if (!p->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s retry timer allocation failed", p->source.name);
        p->result.status = STUN_NAT_FAILED;
        p->result.mapping_reason = STUN_REASON_NOMEM;
        finish_probe(p);
    }
}

/* Новые транзакции всех проб ограничены десятью в секунду; event loop не блокируется. */
static void start_transaction(struct stun_probe* p, enum stun_stage stage, const struct sockaddr_in* target) {
    p->stage = stage;
    p->target = *target;
    p->expected = *target;
    if (stage == STUN_FILTER_BOTH) p->expected = p->other;
    if (stage == STUN_FILTER_PORT) p->expected.sin_port = p->other.sin_port;
    p->sent = 0;
    if (random_bytes(p->transaction, sizeof(p->transaction)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s transaction random generation failed", p->source.name);
        p->result.status = STUN_NAT_FAILED;
        p->result.mapping_reason = STUN_REASON_IO;
        finish_probe(p);
        return;
    }
    uint64_t now = get_time_tb();
    if (p->owner->next_transaction_tb < now) p->owner->next_transaction_tb = now;
    uint64_t delay = p->owner->next_transaction_tb - now;
    p->owner->next_transaction_tb += 1000;
    if (delay > INT_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s transaction pacing delay overflow", p->source.name);
        p->result.status = STUN_NAT_FAILED;
        p->result.mapping_reason = STUN_REASON_IO;
        finish_probe(p);
        return;
    }
    p->timer = uasync_set_timeout(p->owner->inst->ua, (int)delay, p, send_request, "stun_start");
    if (!p->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s start timer allocation failed", p->source.name);
        p->result.status = STUN_NAT_FAILED;
        p->result.mapping_reason = STUN_REASON_NOMEM;
        finish_probe(p);
    }
}

/* После filtering проверяет изменение IP, затем изменение порта альтернативного IP. */
static void begin_mapping(struct stun_probe* p) {
    if (p->capability && p->result.mapping < STUN_MAP_EIM) {
        struct sockaddr_in target = p->other;
        target.sin_port = p->primary.sin_port;
        start_transaction(p, STUN_MAP_IP, &target);
    } else finish_server(p);
}

/* Выбирает второй независимый сервер или повторно проверяет первоначальную трансляцию. */
static void finish_server(struct stun_probe* p) {
    if (p->result.server_count < 2) next_server(p);
    else start_transaction(p, STUN_VERIFY, &p->reference);
}

/* Начинает обычный Binding выбранного независимого сервера. */
static void use_server(struct stun_probe* p, const struct sockaddr_in* addr) {
    p->primary = *addr;
    p->capability = 0;
    start_transaction(p, STUN_BASE, &p->primary);
}

/* DNS-ответ принадлежит только живой пробе; destroy отменяет запрос без callback. */
static void dns_done(const struct adns_result* res, void* arg) {
    struct stun_probe* p = arg;
    p->dns = NULL;
    if (res->status != ADNS_OK || !res->count) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s DNS failed status=%d", p->source.name, res->status);
        p->result.mapping_reason = STUN_REASON_DNS;
        next_server(p);
        return;
    }
    for (int i = 0; i < res->count; i++) {
        struct sockaddr_in addr = res->addrs[i];
        addr.sin_port = htons(p->dns_port);
        uint32_t ip = ntohl(addr.sin_addr.s_addr);
        if (ip && ip < 0xe0000000u && (!p->result.server_count || addr.sin_addr.s_addr != p->reference.sin_addr.s_addr)) {
            use_server(p, &addr);
            return;
        }
    }
    DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s DNS supplied no independent server IP", p->source.name);
    next_server(p);
}

/* Обходит список конфига: недоступные серверы заменяются резервными. */
static void next_server(struct stun_probe* p) {
    while (p->cursor) {
        const struct CFG_STUN_SERVER* server = p->cursor;
        p->cursor = server->next;
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server->port);
        if (inet_pton(AF_INET, server->host, &addr.sin_addr) == 1) {
            uint32_t ip = ntohl(addr.sin_addr.s_addr);
            if (!ip || ip >= 0xe0000000u || (p->result.server_count && addr.sin_addr.s_addr == p->reference.sin_addr.s_addr)) {
                DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s skipping invalid/duplicate server IP=%s", p->source.name, server->host);
                continue;
            }
            use_server(p, &addr);
            return;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "STUN source=%s resolving %s:%u", p->source.name, server->host, server->port);
        p->dns_port = server->port;
        p->dns = adns_resolve(p->owner->inst->ua, server->host, NULL, dns_done, p);
        if (p->dns) return;
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s cannot start DNS for %s", p->source.name, server->host);
        p->result.mapping_reason = STUN_REASON_DNS;
    }
    if (p->result.server_count) {
        if (p->result.mapping == STUN_MAP_UNKNOWN) p->result.mapping_reason = STUN_REASON_NO_PAIR;
        start_transaction(p, STUN_VERIFY, &p->reference);
    } else finish_probe(p);
}

/* Декодирует адрес без unaligned-доступов; IPv6 не смешивается с IPv4 измерением. */
static int parse_addr(const uint8_t* data, size_t len, int xored, struct sockaddr_in* out) {
    if (len != 8 || data[0] || data[1] != 1) return -1;
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(read16(data + 2) ^ (xored ? STUN_COOKIE >> 16 : 0));
    out->sin_addr.s_addr = htonl(read32(data + 4) ^ (xored ? STUN_COOKIE : 0));
    uint32_t ip = ntohl(out->sin_addr.s_addr);
    return !out->sin_port || !ip || ip >= 0xe0000000u ? -1 : 0;
}

/* Проверяет TLV, выравнивание, fingerprint и необходимые поля ответа STUN. */
static int parse_response(const uint8_t* data, size_t len, struct stun_response* r) {
    if (len < 20 || read32(data + 4) != STUN_COOKIE || (read16(data + 2) & 3) || len != 20u + read16(data + 2)) return -1;
    uint16_t type = read16(data);
    if (type != STUN_SUCCESS && type != STUN_ERROR) return -1;
    memset(r, 0, sizeof(*r));
    for (size_t pos = 20; pos < len;) {
        if (len - pos < 4) return -1;
        uint16_t attr = read16(data + pos), size = read16(data + pos + 2);
        size_t padded = ((size_t)size + 3) & ~(size_t)3;
        if (padded > len - pos - 4) return -1;
        const uint8_t* value = data + pos + 4;
        if (attr == STUN_XOR_MAPPED && !r->have_mapped) {
            if (parse_addr(value, size, 1, &r->mapped) < 0) return -1;
            r->have_mapped = 1;
        } else if (attr == STUN_OTHER && !r->have_other) {
            if (parse_addr(value, size, 0, &r->other) < 0) return -1;
            r->have_other = 1;
        } else if (attr == STUN_ORIGIN && !r->have_origin) {
            if (parse_addr(value, size, 0, &r->origin) < 0) return -1;
            r->have_origin = 1;
        } else if (attr == STUN_ERROR_CODE) {
            if (size < 4 || value[0] || value[1] || value[2] < 3 || value[2] > 6 || value[3] > 99) return -1;
            r->error = value[2] * 100 + value[3];
        } else if (attr == STUN_FINGERPRINT) {
            if (size != 4 || pos + 8 != len || read32(value) != (crc32_calc(data, pos) ^ STUN_FINGERPRINT_XOR)) return -1;
        } else if (attr < 0x8000 && attr != STUN_MAPPED && attr != 0x000a) return -1;
        pos += 4 + padded;
    }
    if (type == STUN_ERROR) return r->error ? 0 : -1;
    return r->have_mapped && !r->error ? 0 : -1;
}

/* Завершает очередной этап, сохраняя mapping и filtering как независимые свойства. */
static void handle_response(struct stun_probe* p, const struct stun_response* r) {
    struct stun_nat_result* result = &p->result;
    if (r->error) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s stage=%s server error=%d", p->source.name, stage_name(p->stage), r->error);
        transaction_failed(p, r->error == 420 ? STUN_REASON_UNSUPPORTED : STUN_REASON_SERVER_ERROR);
        return;
    }
    switch (p->stage) {
        case STUN_BASE: {
            unsigned idx = result->server_count++;
            memcpy(&result->servers[idx], &p->primary, sizeof(p->primary));
            memcpy(&result->mapped[idx], &r->mapped, sizeof(r->mapped));
            p->base_mapping = r->mapped;
            if (idx == 0) p->reference = p->primary;
            if (!same_addr((const struct sockaddr_in*)&result->local, &r->mapped)) result->translated = 1;
            else if (result->translated < 0) result->translated = 0;
            p->capability = r->have_other && r->have_origin && r->other.sin_addr.s_addr != p->primary.sin_addr.s_addr &&
                            r->other.sin_port != p->primary.sin_port;
            if (p->capability) { p->other = r->other; result->rfc5780 = 1; }
            else DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "STUN source=%s server lacks usable RFC5780 OTHER-ADDRESS/RESPONSE-ORIGIN", p->source.name);
            if (p->capability && result->filtering == STUN_FILTER_UNKNOWN && filter_is_clean(p, 0))
                start_transaction(p, STUN_FILTER_BOTH, &p->primary);
            else {
                if (p->capability && result->filtering == STUN_FILTER_UNKNOWN) result->filtering_reason = STUN_REASON_TRAFFIC;
                begin_mapping(p);
            }
            break;
        }
        case STUN_FILTER_BOTH:
        case STUN_FILTER_PORT:
            if (!same_addr(&p->base_mapping, &r->mapped)) {
                result->filtering_reason = STUN_REASON_UNSTABLE;
            } else {
                if (p->stage == STUN_FILTER_PORT) {
                    p->pending_filter = STUN_FILTER_ADF;
                    start_transaction(p, STUN_CHECK_OTHER, &p->other);
                    break;
                }
                result->filtering = STUN_FILTER_EIF;
                result->filtering_reason = STUN_REASON_NONE;
            }
            begin_mapping(p);
            break;
        case STUN_CONTROL_BOTH:
        case STUN_CONTROL_PORT:
            if (!same_addr(&p->base_mapping, &r->mapped) || !r->have_other || !same_addr(&p->other, &r->other)) {
                result->filtering_reason = STUN_REASON_UNSTABLE;
                begin_mapping(p);
            } else if (p->stage == STUN_CONTROL_BOTH) {
                if (filter_is_clean(p, 1)) start_transaction(p, STUN_FILTER_PORT, &p->primary);
                else { result->filtering_reason = STUN_REASON_TRAFFIC; begin_mapping(p); }
            } else {
                p->pending_filter = STUN_FILTER_APDF;
                start_transaction(p, STUN_CHECK_OTHER, &p->other);
            }
            break;
        case STUN_CHECK_OTHER:
            if (p->pending_filter == STUN_FILTER_APDF) {
                struct sockaddr_in target = p->primary;
                target.sin_port = p->other.sin_port;
                start_transaction(p, STUN_CHECK_PORT, &target);
                break;
            }
            result->filtering = p->pending_filter;
            result->filtering_reason = STUN_REASON_NONE;
            begin_mapping(p);
            break;
        case STUN_CHECK_PORT:
            result->filtering = p->pending_filter;
            result->filtering_reason = STUN_REASON_NONE;
            begin_mapping(p);
            break;
        case STUN_MAP_IP:
            p->ip_mapping = r->mapped;
            if (same_addr(&p->base_mapping, &r->mapped)) {
                result->mapping = STUN_MAP_EIM;
                result->mapping_reason = STUN_REASON_NONE;
                finish_server(p);
            } else start_transaction(p, STUN_MAP_PORT, &p->other);
            break;
        case STUN_MAP_PORT:
            result->mapping = same_addr(&p->ip_mapping, &r->mapped) ? STUN_MAP_ADM : STUN_MAP_APDM;
            result->mapping_reason = STUN_REASON_NONE;
            finish_server(p);
            break;
        case STUN_VERIFY:
            if (!same_addr((const struct sockaddr_in*)&result->mapped[0], &r->mapped)) {
                DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s first server mapping changed during measurement", p->source.name);
                result->mapping = STUN_MAP_UNKNOWN;
                result->filtering = STUN_FILTER_UNKNOWN;
                result->mapping_reason = result->filtering_reason = STUN_REASON_UNSTABLE;
            } else if (result->server_count == 2) {
                int match = same_addr((const struct sockaddr_in*)&result->mapped[0], (const struct sockaddr_in*)&result->mapped[1]);
                if (result->mapping < STUN_MAP_EIM) {
                    result->mapping = match ? STUN_MAP_MATCH : STUN_MAP_DEPENDENT;
                    result->mapping_reason = STUN_REASON_NONE;
                } else if (result->mapping == STUN_MAP_EIM && !match) {
                    result->mapping = STUN_MAP_DEPENDENT;
                    DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s second server contradicts EIM; retaining observed dependent mapping",
                        p->source.name);
                }
            }
            finish_probe(p);
            break;
    }
}

/* Единственный читатель собственного fd: проверяет источник, ID и формат до изменения состояния. */
static void read_response(socket_t fd, void* arg) {
    struct stun_probe* p = arg;
    uint8_t packet[2048];
    struct sockaddr_storage from = {0};
    socklen_t from_len = sizeof(from);
    ssize_t len = socket_recvfrom(fd, packet, sizeof(packet), (struct sockaddr*)&from, &from_len);
    if (len < 0) {
        int err = socket_get_error();
        if (err == ERR_AGAIN || err == ERR_WOULDBLOCK || err == ERR_INTR) return;
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s receive failed: %s", p->source.name, socket_strerror(err));
        if (p->timer) { uasync_cancel_timeout(p->owner->inst->ua, p->timer); p->timer = NULL; }
        transaction_failed(p, STUN_REASON_IO);
        return;
    }
    if (len < 20 || from.ss_family != AF_INET || memcmp(packet + 8, p->transaction, 12) || !p->sent) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "STUN source=%s ignoring unrelated/late datagram from=%s len=%zd", p->source.name,
            sockaddr_storage_to_str(&from).str, len);
        return;
    }
    const struct sockaddr_in* src = (const struct sockaddr_in*)&from;
    int changed = p->stage == STUN_FILTER_BOTH || p->stage == STUN_FILTER_PORT;
    if (!same_addr(src, &p->expected) && !(changed && same_addr(src, &p->primary))) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s matching ID from unexpected source=%s", p->source.name, sockaddr_storage_to_str(&from).str);
        return;
    }
    struct stun_response response;
    if (parse_response(packet, (size_t)len, &response) < 0 || (response.have_origin && !same_addr(src, &response.origin))) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s malformed response stage=%s from=%s len=%zd", p->source.name,
            stage_name(p->stage), sockaddr_storage_to_str(&from).str, len);
        return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "STUN RX source=%s stage=%s from=%s mapped=%s:%u RTT=%llums error=%d",
        p->source.name, stage_name(p->stage), sockaddr_storage_to_str(&from).str,
        ip_to_str(&response.mapped.sin_addr, AF_INET).str, ntohs(response.mapped.sin_port),
        (unsigned long long)((get_time_tb() - p->sent_at_tb) / 10), response.error);
    if (p->timer) { uasync_cancel_timeout(p->owner->inst->ua, p->timer); p->timer = NULL; }
    if (changed && !response.error && !same_addr(src, &p->expected)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN source=%s server ignored CHANGE-REQUEST", p->source.name);
        transaction_failed(p, STUN_REASON_UNSUPPORTED);
    } else handle_response(p, &response);
}

/* Читает фактический путь исходного UDP без изменения его состояния. */
static int source_snapshot(const struct ETCP_SOCKET* sock, struct stun_source* source) {
    memset(source, 0, sizeof(*source));
    snprintf(source->name, sizeof(source->name), "%s", sock->name);
    source->netif_index = sock->netif_index;
    source->key.netif_index = sock->netif_index;
    source->skipped = sock->is_tcp || sock->probe_only || sock->socks_udp || sock->only_local;
    const struct sockaddr_storage* addr = &sock->local_addr;
    if ((addr->ss_family == AF_INET && !((const struct sockaddr_in*)addr)->sin_addr.s_addr) ||
        (addr->ss_family == AF_INET6 && !memcmp(&((const struct sockaddr_in6*)addr)->sin6_addr, &in6addr_any, 16)) ||
        (addr->ss_family != AF_INET && addr->ss_family != AF_INET6)) addr = &sock->interface_addr;
    if (addr->ss_family == AF_INET6) {
        source->key.family = 6;
        memcpy(source->key.address, &((const struct sockaddr_in6*)addr)->sin6_addr, 16);
    } else if (addr->ss_family == AF_INET) {
        source->key.family = 4;
        memcpy(source->key.address, &((const struct sockaddr_in*)addr)->sin_addr, 4);
    }
    if (addr->ss_family != AF_INET || !((const struct sockaddr_in*)addr)->sin_addr.s_addr) source->skipped = 1;
    else { source->addr = *(const struct sockaddr_in*)addr; source->addr.sin_port = 0; }
    if (source->skipped) return 0;
#ifdef __linux__
    socklen_t size = sizeof(source->mark);
    if (getsockopt(sock->fd, SOL_SOCKET, SO_MARK, &source->mark, &size) < 0) goto error;
    size = sizeof(source->device);
    if (getsockopt(sock->fd, SOL_SOCKET, SO_BINDTODEVICE, source->device, &size) < 0) goto error;
#endif
#ifdef __FreeBSD__
    socklen_t size = sizeof(source->fib);
    if (getsockopt(sock->fd, SOL_SOCKET, SO_SETFIB, &source->fib, &size) < 0) goto error;
#endif
    return 0;
#if defined(__linux__) || defined(__FreeBSD__)
error:
    DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s cannot read routing options: %s", sock->name, socket_strerror(socket_get_error()));
    return -1;
#endif
}

/* Создаёт независимый сокет с теми же IP/device/mark/FIB и случайным свободным портом. */
static int open_probe_socket(struct stun_probe* p) {
    p->fd = socket_create_udp(AF_INET);
    if (p->fd == SOCKET_INVALID) goto error;
    if (socket_set_nonblocking(p->fd) < 0 || socket_set_reuseaddr(p->fd, 0) < 0) goto error;
    if (p->source.mark && socket_set_mark(p->fd, p->source.mark) < 0) goto error;
    if (p->source.device[0] && socket_bind_to_device(p->fd, p->source.device) < 0) goto error;
#ifdef __FreeBSD__
    if (setsockopt(p->fd, SOL_SOCKET, SO_SETFIB, &p->source.fib, sizeof(p->source.fib)) < 0) goto error;
#endif
    struct sockaddr_in local = p->source.addr;
    int bound = 0;
    for (int i = 0; i < 32; i++) {
        uint16_t random_port;
        if (random_bytes((uint8_t*)&random_port, sizeof(random_port)) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s random port generation failed", p->source.name);
            return -1;
        }
        unsigned port = random_port & 16383;
        local.sin_port = htons(49152 + port);
        if (p->ports->used[port / 8] & (1u << (port % 8))) continue;
        int working = 0;
        for (const struct ETCP_SOCKET* socket = p->owner->inst->etcp_sockets; socket; socket = socket->next) {
            if (socket->local_addr.ss_family != AF_INET) continue;
            const struct sockaddr_in* address = (const struct sockaddr_in*)&socket->local_addr;
            if (address->sin_addr.s_addr == local.sin_addr.s_addr && address->sin_port == local.sin_port) working = 1;
        }
        if (working) continue;
        if (bind(p->fd, (const struct sockaddr*)&local, sizeof(local)) == 0) {
            p->ports->used[port / 8] |= (uint8_t)(1u << (port % 8)); bound = 1; break;
        }
#ifdef _WIN32
        if (socket_get_error() != WSAEADDRINUSE) goto error;
#else
        if (socket_get_error() != EADDRINUSE) goto error;
#endif
    }
    if (!bound) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s no fresh free test port after 32 attempts", p->source.name);
        p->result.mapping_reason = p->result.filtering_reason = STUN_REASON_PORTS;
        return -1;
    }
    memcpy(&p->result.local, &local, sizeof(local));
    p->socket_handle = uasync_add_socket_t(p->owner->inst->ua, p->fd, read_response, NULL, NULL, "stun_probe", p);
    if (!p->socket_handle) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s cannot register test socket", p->source.name); return -1; }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "STUN start source=%s test=%s netif=%u device=%s mark=%d fib=%d fd=%d",
        p->source.name, sockaddr_storage_to_str(&p->result.local).str, p->source.netif_index,
        p->source.device, p->source.mark, p->source.fib, (int)p->fd);
    return 0;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s cannot create socket IP=%s device=%s mark=%d fib=%d: %s",
        p->source.name, ip_to_str(&p->source.addr.sin_addr, AF_INET).str, p->source.device,
        p->source.mark, p->source.fib, socket_strerror(socket_get_error()));
    return -1;
}

/* Освобождает пробу; никогда не закрывает исходный ETCP fd. */
static void destroy_probe(struct stun_probe* p) {
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "STUN cleanup source=%s status=%d test_fd=%d DNS=%d timer=%d",
        p->source.name, p->result.status, (int)p->fd, p->dns != NULL, p->timer != NULL);
    close_probe_io(p);
    u_free(p);
}

/* Начинает новый раунд; действующий кэш сохраняется до TTL при штатном refresh. */
static void restart_probe(struct stun_probe* p) {
    close_probe_io(p);
    memset(&p->result, 0, sizeof(p->result));
    p->result.key = p->source.key; p->result.generation = p->generation; p->result.members = p->members;
    p->result.translated = -1;
    p->result.mapping_reason = STUN_REASON_NO_PAIR;
    p->result.filtering_reason = STUN_REASON_UNSUPPORTED;
    p->cursor = p->owner->inst->config->global.stun_servers;
    p->capability = 0;
    p->pending_filter = STUN_FILTER_UNKNOWN;
    p->sent = 0;
    if (random_id(&p->result.evidence_id)) {
        p->result.status = STUN_NAT_FAILED; p->result.mapping_reason = STUN_REASON_IO; finish_probe(p); return;
    }
    if (p->source.skipped || p->conflict) {
        p->result.status = STUN_NAT_SKIPPED;
        p->result.mapping_reason = p->result.filtering_reason = p->conflict ? STUN_REASON_ROUTE : STUN_REASON_SOCKET;
        finish_probe(p);
    } else if (open_probe_socket(p) < 0) {
        if (p->result.mapping_reason != STUN_REASON_PORTS) p->result.mapping_reason = STUN_REASON_IO;
        finish_probe(p);
    } else next_server(p);
}

/* Сверяет снимки: новый/изменённый интерфейс получает свежую пробу, исчезнувший удаляется. */
static void scan_sources(struct STUN_NAT* mod) {
    for (struct stun_probe* p = mod->probes; p; p = p->next) {
        p->seen = 0; p->members = 0; p->scan_conflict = 0; p->changed = 0;
    }
    for (struct ETCP_SOCKET* sock = mod->inst->etcp_sockets; sock; sock = sock->next) {
        if (sock->is_tcp || sock->probe_only || sock->socks_udp || sock->only_local) continue;
        struct stun_source source;
        if (source_snapshot(sock, &source) < 0) continue;
        struct stun_probe* p = mod->probes;
        while (p && !key_equal(&p->source.key, &source.key)) p = p->next;
        if (p) {
            if (!p->seen) {
                p->changed = !policy_equal(&p->source, &source) || (!p->generation && get_time_tb() >= p->due);
                p->source = source;
            } else if (!policy_equal(&p->source, &source)) p->scan_conflict = 1;
            p->seen = 1; p->members++;
            continue;
        }
        p = u_calloc(1, sizeof(*p));
        if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN source=%s probe allocation failed", source.name); continue; }
        p->owner = mod;
        p->source = source;
        p->seen = 1; p->members = 1; p->changed = 1;
        p->ports = mod->ports;
        while (p->ports && !key_equal(&p->ports->key, &source.key)) p->ports = p->ports->next;
        if (!p->ports) {
            p->ports = u_calloc(1, sizeof(*p->ports));
            if (!p->ports) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN port history allocation failed"); u_free(p); continue; }
            p->ports->key = source.key; p->ports->next = mod->ports; mod->ports = p->ports;
            mod->inst->stun_port_history = mod->ports;
        }
        p->fd = SOCKET_INVALID;
        p->next = mod->probes;
        mod->probes = p;
    }
    struct stun_probe** slot = &mod->probes;
    while (*slot) {
        if ((*slot)->seen) {
            struct stun_probe* p = *slot;
            if (p->changed || p->conflict != p->scan_conflict) {
                close_probe_io(p); memset(&p->cached, 0, sizeof(p->cached));
                p->conflict = p->scan_conflict;
                if (random_id(&p->generation)) {
                    p->generation = 0; memset(&p->result, 0, sizeof(p->result)); p->result.key = p->source.key;
                    p->result.status = STUN_NAT_FAILED; p->result.mapping_reason = STUN_REASON_IO; finish_probe(p);
                }
                else restart_probe(p);
                DEBUG_INFO(DEBUG_CATEGORY_NAT, "STUN group source=%s IP=%s netif=%u members=%u generation=%016llx conflict=%d",
                    p->source.name, ip_to_str(&p->source.addr.sin_addr, AF_INET).str, p->source.netif_index,
                    p->members, (unsigned long long)p->generation, p->conflict);
            } else if (p->result.status != STUN_NAT_PENDING && get_time_tb() >= p->due) restart_probe(p);
            slot = &p->next; continue;
        }
        struct stun_probe* p = *slot;
        *slot = p->next;
        destroy_probe(p);
    }
}

/* Редкая локальная сверка не отправляет повторных запросов на неизменившихся интерфейсах. */
static void scan_timer(void* arg) {
    struct STUN_NAT* mod = arg;
    mod->scan_timer = NULL;
    scan_sources(mod);
    mod->scan_timer = uasync_set_timeout(mod->inst->ua, STUN_SCAN_TB, mod, scan_timer, "stun_scan");
    if (!mod->scan_timer) DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN interface scan timer allocation failed");
}

int stun_nat_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->ua || !inst->config) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN init requires instance, uasync and config"); return -1; }
    if (inst->stun_nat) return 0;
    if (!inst->config->global.stun_enabled) return 0;
    if (inst->config->global.stun_server_count < 2) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN requires at least two configured servers"); return -1; }
    struct STUN_NAT* mod = u_calloc(1, sizeof(*mod));
    if (!mod) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN module allocation failed"); return -1; }
    mod->inst = inst;
    mod->ports = inst->stun_port_history;
    inst->stun_nat = mod;
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "STUN initialized with %d servers; independent test sockets", inst->config->global.stun_server_count);
    scan_timer(mod);
    if (!mod->scan_timer) { stun_nat_destroy(inst); return -1; }
    return 0;
}

void stun_nat_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->stun_nat) return;
    struct STUN_NAT* mod = inst->stun_nat;
    inst->stun_nat = NULL;
    if (mod->scan_timer) uasync_cancel_timeout(inst->ua, mod->scan_timer);
    while (mod->probes) { struct stun_probe* next = mod->probes->next; destroy_probe(mod->probes); mod->probes = next; }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "STUN destroyed; test sockets/DNS/timers released");
    u_free(mod);
}

void stun_nat_forget_ports(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    if (inst->stun_nat) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "STUN port history release requires stopped module"); return; }
    while (inst->stun_port_history) {
        struct stun_ports* next = inst->stun_port_history->next;
        u_free(inst->stun_port_history); inst->stun_port_history = next;
    }
}

void stun_nat_recheck(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->stun_nat) return;
    struct STUN_NAT* mod = inst->stun_nat;
    mod->next_transaction_tb = get_time_tb();
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "STUN rechecking interfaces using fresh test ports");
    for (struct stun_probe* p = mod->probes; p; p = p->next) {
        p->generation = 0; p->due = 0; // единый scan перезапустит существующие и добавит новые ровно один раз
    }
    scan_sources(mod);
}

int stun_nat_get_result(const struct UTUN_INSTANCE* inst, const char* socket_name, struct stun_nat_result* result) {
    if (!inst || !inst->stun_nat || !socket_name || !result) return -1;
    for (const struct ETCP_SOCKET* socket = inst->etcp_sockets; socket; socket = socket->next)
        if (!strcmp(socket->name, socket_name)) return stun_nat_get_socket_result(inst, socket, result);
    return -1;
}

int stun_nat_get_socket_result(const struct UTUN_INSTANCE* inst, const struct ETCP_SOCKET* socket, struct stun_nat_result* result) {
    if (!inst || !inst->stun_nat || !socket || !result || socket->instance != inst ||
        socket->is_tcp || socket->probe_only || socket->socks_udp || socket->only_local) return -1;
    struct stun_source source;
    if (source_snapshot(socket, &source)) return -1;
    for (const struct stun_probe* p = inst->stun_nat->probes; p; p = p->next) {
        if (!key_equal(&p->source.key, &source.key) || (!p->conflict && !policy_equal(&p->source, &source))) continue;
        uint64_t now = get_time_tb();
        int fresh = p->cached.measured_at_tb && now >= p->cached.measured_at_tb && now - p->cached.measured_at_tb < STUN_NAT_TTL_TB;
        *result = fresh ? p->cached : p->result;
        result->refreshing = fresh && p->result.status == STUN_NAT_PENDING;
        result->members = p->members;
        return 0;
    }
    return -1;
}
