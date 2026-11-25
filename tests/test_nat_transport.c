/**
 * @file test_nat_transport.c
 * @brief Интеграционный тест NAT transport layer (nat_transport.c/h)
 *
 * Создаёт два UTUN_INSTANCE (provider + client), инициализирует NAT транспорт
 * и тестирует полный цикл: client → provider (egress) → provider TUN (internet)
 * → provider (ingress) → client (response).
 *
 * Тестовый socketpair сохраняет направления TUN: ответы читаются со стороны ОС.
 */

#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_utils.h"
#include "test_net.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "topo_group.h"
#include "../src/tun_if.h"
#include "../src/nat_transport.h"
#include "../src/eim_nat.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/ll_queue.h"

#define TEST_TIMEOUT_MS 15000
static uint64_t g_provider_node_id = 0;
static uint64_t g_client_node_id = 0;
#define NAT_SVC_HDR_SIZE 1  // send: svc_id(1) + ip_data (src/dst добавляет роутер)

static struct UTUN_INSTANCE* inst_provider = NULL;
static struct UTUN_INSTANCE* inst_client = NULL;
static struct UASYNC* ua = NULL;
static int test_timed_out = 0;
static void* test_timeout_id = NULL;

#ifdef __linux__
/* Отказ с реальным освобождением в router API и успешный приём с тем же контрактом владения. */
static __thread int send_test_mode, send_test_calls, send_test_valid, send_test_dgrams_freed, send_test_entries_freed;
static struct UTUN_INSTANCE* send_test_instance;
static const uint8_t* send_test_destination;
static size_t send_test_ip_len;
static __thread struct ll_entry* original_tun_packet;
static struct tun_if* write_blocked_tun;
int __real_queue_data_put(struct ll_queue*, struct ll_entry*);
int __wrap_queue_data_put(struct ll_queue* q, struct ll_entry* entry) {
    if (send_test_mode && send_test_instance && q == send_test_instance->nat_tr.nat_tun->output_queue) original_tun_packet = entry;
    return __real_queue_data_put(q, entry);
}
void __real_queue_dgram_free(struct ll_entry*);
void __wrap_queue_dgram_free(struct ll_entry* entry) {
    if (entry && entry == original_tun_packet && entry->dgram) send_test_dgrams_freed++;
    __real_queue_dgram_free(entry);
}
void __real_queue_entry_free(struct ll_entry*);
void __wrap_queue_entry_free(struct ll_entry* entry) {
    if (entry && entry == original_tun_packet) {
        send_test_entries_freed++; original_tun_packet = NULL;
    }
    __real_queue_entry_free(entry);
}
ssize_t __real_tun_write(struct tun_if*, const uint8_t*, size_t);
ssize_t __wrap_tun_write(struct tun_if* tun, const uint8_t* data, size_t len) {
    if (tun == write_blocked_tun) { errno = EAGAIN; return -1; }
    return __real_tun_write(tun, data, len);
}

int __real_etcp_route_send_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t*, struct ll_entry*, int, int);
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t* destination,
                           struct ll_entry* entry, int force, int mode) {
    if (!send_test_mode) return __real_etcp_route_send_key(inst, group, destination, entry, force, mode);
    send_test_calls++;
    if (inst != send_test_instance || group != TOPO_GROUP_UTUN || memcmp(destination, send_test_destination, 32) ||
        !entry || entry != original_tun_packet || !entry->dgram || entry->len != send_test_ip_len + 1 ||
        entry->dgram[0] != ETCP_RT_ID_NAT || force || mode) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Unexpected NAT send in ownership test call=%d", send_test_calls);
        send_test_valid = 0;
    }
    if (send_test_mode == 1) {
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "Inject NAT router refusal: packet ownership transfers on error");
        // NULL instance вызывает отказ в реальном router API; переданный entry он освобождает сам.
        return __real_etcp_route_send_key(NULL, group, destination, entry, force, mode);
    }
    queue_dgram_free(entry); queue_entry_free(entry);
    return 0;
}
#endif

static char temp_dir[] = "/tmp/utun_nat_transport_XXXXXX";
static char config_provider[256];
static char config_client[256];

/* Test result tracking */
static struct {
    int done;
    uint8_t captured[1500];
    size_t  captured_len;
    int     capture_count;

    int     provider_egress_entry;
    uint32_t egress_internal_ip;
    uint16_t egress_internal_port_net;
    uint16_t egress_external_port;
    uint8_t  egress_proto;
    int     provider_ingress_done;
    int     client_response_done;
} test_state;

static int write_config(const char* path, const char* content) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%s", content);
    fclose(f);
    return 0;
}

static char* get_pubkey_from_config(const char* path) {
    struct utun_config* cfg = parse_config(path);
    if (!cfg) return NULL;
    char* pub = strdup(cfg->global.my_public_key_hex);
    free_config(cfg);
    return pub;
}

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return -1;
    }
    snprintf(config_provider, sizeof(config_provider), "%s/provider.conf", temp_dir);
    snprintf(config_client, sizeof(config_client), "%s/client.conf", temp_dir);

    const char* tpl_provider =
        "[global]\n"
        "tun_ip=10.100.0.1/24\n"
        "tun_ifname=tun_provider\n"
        "tun_test_mode=1\n"
        "\n"
        "[server:prov]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[allowed_keys]\n"
        "allow_all=1\n"
        "\n"
        "[nat]\n"
        "tun_ifname=tun_nat\n"
        "tun_ip=100.64.0.1/24\n"
        "port_start=20000\n"
        "port_end=20099\n";
    if (write_config(config_provider, tpl_provider) != 0) return -1;
    if (config_ensure_keys(config_provider) != 0) return -1;
    struct utun_config* cfg = parse_config(config_provider);
    if (!cfg) return -1;
    struct SC_MYKEYS keys; uint8_t ed[32], seed[32];
    int identity = sc_identity_load(cfg->global.my_public_key_hex, cfg->global.my_private_key_hex, &keys, ed, seed);
    OPENSSL_cleanse(&keys, sizeof(keys)); OPENSSL_cleanse(seed, sizeof(seed));
    if (identity != SC_OK) { free_config(cfg); return -1; }
    char provider_ed[65]; bytes_to_hex(ed, sizeof(ed), provider_ed, sizeof(provider_ed));
    free_config(cfg);
    char* pub_prov = get_pubkey_from_config(config_provider);
    if (!pub_prov) return -1;

    char tpl_client_full[4096];
    snprintf(tpl_client_full, sizeof(tpl_client_full),
        "[global]\n"
        "tun_ip=10.200.0.1/24\n"
        "tun_ifname=tun_client\n"
        "tun_test_mode=1\n"
        "\n"
        "[server:cl]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[client:to_prov]\n"
        "keepalive=1\n"
        "peer_public_key=%s\n"
        "link=cl:127.0.0.1:0\n"
        "\n"
        "[nat]\n"
        "tun_ifname=tun_nat_client\n"
        "tun_ip=100.64.1.1/24\n"
        "nat_via=%s\n",
        pub_prov, provider_ed);
    free(pub_prov);
    if (write_config(config_client, tpl_client_full) != 0) return -1;
    if (config_ensure_keys(config_client) != 0) return -1;

    return 0;
}

static void cleanup_temp_configs(void) {
    test_unlink(config_provider);
    test_unlink(config_client);
    test_rmdir(temp_dir);
}

static void test_timeout_cb(void* arg) {
    (void)arg;
    test_timed_out = 1;
    DEBUG_ERROR(DEBUG_CATEGORY_NAT, "test_nat_transport: timeout");
}

static struct ETCP_LINK* first_initialized_link(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return NULL;
    struct ll_entry* entry = inst->connections->head;
    while (entry) { struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* conn = ce->conn;
        struct ETCP_LINK* link = conn->links;
        while (link) { if (link->initialized) return link; link = link->next; }
        entry = entry->next; }
    return NULL;
}

// Получить ответ со стороны ОС, сохраняя рабочий output callback NAT-клиента.
static void capture_tun_read(struct tun_if* tun) {
    ssize_t n = tun_read_packet(tun, test_state.captured, sizeof(test_state.captured));
    if (n > 0) {
        test_state.captured_len = (size_t)n;
        test_state.capture_count++;
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "Client OS received %zd IP bytes", n);
    } else if (n < 0) DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Client test TUN read failed");
}

// ==================== Tests ====================

/* Build raw UDP/IP packet helper */
static uint8_t* build_udp_pkt(uint32_t src_host, uint16_t src_port_host,
                               uint32_t dst_host, uint16_t dst_port_host,
                               size_t* out_len) {
    const size_t len = 20 + 8 + 14;
    uint8_t* pkt = calloc(1, len);

    pkt[0] = 0x45; pkt[1] = 0x00;
    uint16_t tot = htobe16((uint16_t)len);
    memcpy(pkt + 2, &tot, 2);
    pkt[4] = 0x12; pkt[5] = 0x34;
    memset(pkt + 6, 0, 2);
    pkt[8] = 64;
    pkt[9] = IPPROTO_UDP_UINT8;
    memset(pkt + 10, 0, 2);
    uint32_t sn = htobe32(src_host), dn = htobe32(dst_host);
    memcpy(pkt + 12, &sn, 4);
    memcpy(pkt + 16, &dn, 4);

    uint16_t sp = htobe16(src_port_host), dp = htobe16(dst_port_host);
    memcpy(pkt + 20, &sp, 2);
    memcpy(pkt + 22, &dp, 2);
    uint16_t ul = htobe16(8 + 14);
    memcpy(pkt + 24, &ul, 2);
    memset(pkt + 26, 0, 2);
    memset(pkt + 28, 0xAB, 14);

    // Compute IP checksum
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) { uint16_t w; memcpy(&w, pkt + i*2, 2); sum += w; }
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum += (sum >> 16);
    uint16_t cs = (uint16_t)(~sum);
    memcpy(pkt + 10, &cs, 2);

    *out_len = len;
    return pkt;
}

/* Verify IP checksum */
static int verify_ip_checksum(const uint8_t* ip) {
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) { uint16_t w; memcpy(&w, ip + i*2, 2); sum += w; }
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum += (sum >> 16);
    return (uint16_t)(~sum) == 0;
}

static int test_init_destroy(void) {
    /* Already done in main: provider and client NAT transport initialized.
       Here we just verify that ctx fields are populated. */
    int ok = 1;

    // Check provider
    if (!inst_provider->nat_tr.initialized) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Provider NAT transport not initialized");
        ok = 0;
    }
    if (!inst_provider->nat.initialized) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Provider NAT engine not initialized");
        ok = 0;
    }
    if (inst_provider->nat.gateway_ip == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Provider gateway_ip = 0");
        ok = 0;
    }
    if (!inst_provider->nat_tr.nat_tun) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Provider NAT TUN not created");
        ok = 0;
    }
    if (!inst_provider->nat.table) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Provider unexpectedly has an upstream address");
        ok = 0;
    }

    // Check client
    if (!inst_client->nat_tr.initialized) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Client NAT transport not initialized");
        ok = 0;
    }
    if (inst_client->nat.table || memcmp(inst_client->config->global.nat_via_ed, inst_provider->my_ed25519_pubkey, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Client has wrong NAT provider address");
        ok = 0;
    }

    return ok;
}

static int test_provider_egress(void) {
    /* Inject a raw IP/UDP packet directly into provider via ETCP */
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "=== test_provider_egress ===");

    // Get the connection from client to provider (for debug info only)
    struct ETCP_CONN* client_conn = (inst_client->connections && inst_client->connections->head) ? ((struct conn_queue_entry*)inst_client->connections->head->data)->conn : NULL;
    if (!client_conn) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "No client connections");
        return 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "Client conn peer_node_id=0x%016llx", (unsigned long long)client_conn->peer_node_id);

    // Build ETCP_RT_ID_NAT packet via etcp_route_send (src/dst node_id добавляет роутер)
    size_t ip_len;
    uint8_t* raw_ip = build_udp_pkt(0x0A0000FE, 40000, 0x08080808, 53, &ip_len);
    size_t total = NAT_SVC_HDR_SIZE + ip_len;
    uint8_t* dgram = u_malloc(total);
    dgram[0] = ETCP_RT_ID_NAT;
    memcpy(dgram + 1, raw_ip, ip_len);
    free(raw_ip);

    struct ll_entry* entry = queue_entry_new(0);
    entry->dgram = dgram;
    entry->len = total;

    int ret = etcp_route_send(inst_client, TOPO_GROUP_UTUN, g_provider_node_id, entry, 0, 0);
    if (ret != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "etcp_route_send failed");
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "ETCP_RT_ID_NAT sent from client to provider via etcp_router");

    // Poll to let provider process
    int cycles = 0;
    while (cycles < 200 && !test_timed_out) {
        uasync_poll(ua, 5);
        cycles++;
        // Stop when we see a NAT entry
        if (inst_provider->nat.table[inst_provider->nat.port_start].state != EIM_NAT_ENTRY_FREE)
            break;
    }

    // Verify NAT table on provider
    struct eim_nat_entry* e = &inst_provider->nat.table[inst_provider->nat.port_start];
    if (e->state != EIM_NAT_ENTRY_ACTIVE) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Provider: no NAT entry after egress (state=%d)", (int)e->state);
        return 0;
    }

    test_state.provider_egress_entry = 1;
    test_state.egress_internal_ip = e->internal_ip;
    test_state.egress_internal_port_net = e->internal_port;
    test_state.egress_external_port = inst_provider->nat.port_start;
    test_state.egress_proto = e->proto;

    DEBUG_INFO(DEBUG_CATEGORY_NAT, "Provider egress: internal=%08x:%u proto=%u ext_port=%u ed=%s",
               e->internal_ip, be16toh(e->internal_port), e->proto,
               inst_provider->nat.port_start, sc_public_key_hex(e->src_ed, (char[65]){0}));

    if (memcmp(e->src_ed, inst_client->my_ed25519_pubkey, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT mapping has wrong Ed25519 owner");
        return 0;
    }
    if (e->internal_ip != 0x0A0000FE) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "internal_ip mismatch: %08x", e->internal_ip);
        return 0;
    }
    if (e->internal_port != htobe16(40000)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "internal_port mismatch: %u vs 40000", be16toh(e->internal_port));
        return 0;
    }
    if (e->proto != IPPROTO_UDP_UINT8) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "proto mismatch: %u", e->proto);
        return 0;
    }

    return 1;
}

static int test_provider_ingress_response(void) {
    /* Inject internet response into provider's TUN output_queue.
       The provider's TUN output callback will do ingress NAT and send to client. */
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "=== test_provider_ingress_response ===");

    if (!test_state.provider_egress_entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Skip: no egress entry from previous test");
        return 0;
    }

    // Use actual gateway IP from provider's NAT engine
    uint32_t gw_ip_host = inst_provider->nat.gateway_ip;
    size_t ip_len;
    uint8_t* resp_ip = build_udp_pkt(0x08080808, 53,
                                      gw_ip_host, test_state.egress_external_port,
                                      &ip_len);

    // Inject into provider's NAT TUN output_queue (simulating internet response)
    struct tun_if* tun = inst_provider->nat_tr.nat_tun;
    if (!tun) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "No NAT TUN"); free(resp_ip); return 0; }

    struct tun_if* client_tun = inst_client->nat_tr.nat_tun;
    memset(&test_state.captured, 0, sizeof(test_state.captured));
    test_state.captured_len = 0;
    test_state.capture_count = 0;

    int r = tun_inject_packet(tun, resp_ip, ip_len);
    free(resp_ip);
    if (r != 0) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "tun_inject_packet failed"); return 0; }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "Injected response into provider TUN output_queue");

    // Poll extensively to let the full chain complete
    int cycles = 0;
    while (cycles < 500 && !test_timed_out && test_state.capture_count == 0) {
        uasync_poll(ua, 10);
        capture_tun_read(client_tun);
        cycles++;
    }

    DEBUG_INFO(DEBUG_CATEGORY_NAT, "After poll: capture_count=%d, captured_len=%zu",
               test_state.capture_count, test_state.captured_len);

    if (test_state.capture_count == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "No response captured on client after %d cycles", cycles);
        return 0;
    }

    // Verify the captured response
    if (test_state.captured_len < 20) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Captured packet too short: %zu", test_state.captured_len);
        return 0;
    }
    if (!verify_ip_checksum(test_state.captured)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Captured IP checksum invalid");
        return 0;
    }
    // Check dst IP = original internal IP
    uint32_t dst_net; memcpy(&dst_net, test_state.captured + 16, 4);
    uint32_t expected_dst = htobe32(0x0A0000FE); // 10.0.0.254
    if (dst_net != expected_dst) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Response dst IP: %08x, expected %08x", be32toh(dst_net), be32toh(expected_dst));
        return 0;
    }
    // Check dst port = original internal port
    uint16_t dst_port_net; memcpy(&dst_port_net, test_state.captured + 22, 2);
    if (dst_port_net != htobe16(40000)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Response dst port: %u, expected 40000", be16toh(dst_port_net));
        return 0;
    }

    test_state.client_response_done = 1;
    return 1;
}

#ifdef __linux__
static int test_send_failure_ownership(int provider) {
    struct UTUN_INSTANCE* sender = provider ? inst_provider : inst_client;
    struct tun_if* tun = sender->nat_tr.nat_tun;
    uint16_t external_port = inst_provider->nat.port_start;
    size_t ip_len;
    uint8_t* ip = provider ? build_udp_pkt(0x08080808, 80, inst_provider->nat.gateway_ip, external_port, &ip_len) :
        build_udp_pkt(0x0a0000cd, 44444, 0x08080808, 80, &ip_len);
    send_test_instance = sender;
    send_test_destination = provider ? inst_client->my_ed25519_pubkey : inst_provider->my_ed25519_pubkey;
    send_test_ip_len = ip_len;
    send_test_calls = send_test_dgrams_freed = send_test_entries_freed = 0; send_test_valid = 1;
    int ok = 1;
    for (int call = 1; call <= 2; call++) {
        send_test_mode = call; /* Сначала отказ роутера, затем успешная передача следующего пакета. */
        if (tun_inject_packet(tun, ip, ip_len) || send_test_calls != call || send_test_dgrams_freed != call ||
            send_test_entries_freed != call || !send_test_valid || queue_entry_count(tun->output_queue)) {
            DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT %s ownership step=%d sends=%d valid=%d freed_data=%d freed_entries=%d queued=%d",
                provider ? "provider" : "client", call, send_test_calls, send_test_valid, send_test_dgrams_freed,
                send_test_entries_freed, (int)queue_entry_count(tun->output_queue));
            ok = 0; break;
        }
    }
    send_test_mode = 0; send_test_instance = NULL; original_tun_packet = NULL;
    free(ip);
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT %s send failure: no double free/leak, next packet accepted",
                       provider ? "provider" : "client");
    return ok;
}
#endif

/* Loopback проходит реальный router API: клиент не принимает IP от постороннего узла/группы. */
static int test_unexpected_nat_sender(int wrong_group) {
    size_t len;
    uint8_t* packet = build_udp_pkt(0x08080808, 53, 0x0a0000cd, 44444, &len);
    struct ll_entry* entry = ll_alloc_lldgram((uint16_t)(len + 1));
    if (!entry) { free(packet); DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT sender test allocation failed"); return 0; }
    entry->dgram[0] = ETCP_RT_ID_NAT; memcpy(entry->dgram + 1, packet, len); entry->len = len + 1;
    free(packet);
    uint8_t saved_provider[32]; memcpy(saved_provider, inst_client->config->global.nat_via_ed, 32);
    if (wrong_group) memcpy(inst_client->config->global.nat_via_ed, inst_client->my_ed25519_pubkey, 32);
    int r = etcp_route_send(inst_client, wrong_group ? 1234 : TOPO_GROUP_UTUN, g_client_node_id, entry, 0, 0);
    memcpy(inst_client->config->global.nat_via_ed, saved_provider, 32);
    test_state.captured_len = 0; test_state.capture_count = 0;
    capture_tun_read(inst_client->nat_tr.nat_tun);
    int ok = r == 0 && !test_state.capture_count;
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT client rejects unexpected %s", wrong_group ? "group" : "sender");
    else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL NAT client accepted unexpected %s: result=%d captured=%d",
                     wrong_group ? "group" : "sender", r, test_state.capture_count);
    return ok;
}

/* Первый входящий пакет по статическому пробросу ниже динамического диапазона доходит до заданного узла. */
static int test_static_forward_delivery(void) {
    if (inst_provider->nat.table[8080].state == EIM_NAT_ENTRY_FREE &&
        eim_nat_add_forward(&inst_provider->nat, IPPROTO_UDP_UINT8, 0x0a0000ce, htobe16(8081), 8080, inst_client->my_ed25519_pubkey)) return 0;
    size_t len;
    uint8_t* packet = build_udp_pkt(0x08080808, 53, inst_provider->nat.gateway_ip, 8080, &len);
    test_state.capture_count = 0; test_state.captured_len = 0;
    int r = tun_inject_packet(inst_provider->nat_tr.nat_tun, packet, len);
    free(packet);
    if (r) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Static forward injection failed"); return 0; }
    for (int i = 0; i < 500 && !test_timed_out && !test_state.capture_count; i++) {
        uasync_poll(ua, 10); capture_tun_read(inst_client->nat_tr.nat_tun);
    }
    uint32_t dst = 0; uint16_t port = 0;
    if (test_state.captured_len >= 28) {
        memcpy(&dst, test_state.captured + 16, 4); memcpy(&port, test_state.captured + 22, 2);
    }
    int ok = test_state.captured_len == len && dst == htobe32(0x0a0000ce) && port == htobe16(8081) &&
        verify_ip_checksum(test_state.captured);
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS static forward: :8080 -> node=%016llx 10.0.0.206:8081", (unsigned long long)g_client_node_id);
    else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL static forward: bytes=%zu destination=%08x port=%u",
                     test_state.captured_len, be32toh(dst), be16toh(port));
    return ok;
}

static int test_full_roundtrip(void) {
    /* Полный путь через client socketpair → ETCP → provider TUN и обратно. */
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "=== test_full_roundtrip ===");

    struct tun_if* client_tun = inst_client->nat_tr.nat_tun;
    if (!client_tun) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "No client TUN"); return 0; }

    // Создать новый mapping без перезапуска/обнуления внутренних индексов после проверок отказов.
    uint16_t external_port = inst_provider->nat.next_port;
    if (!external_port) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "No free port for roundtrip"); return 0; }

    // === Step 1: ОС клиента отправляет пакет в NAT TUN ===
    size_t ip_len;
    uint8_t* raw_ip = build_udp_pkt(0x0A0000CD, 44444, 0x08080808, 80, &ip_len);
    ssize_t sent = send(tun_get_test_fd(client_tun), raw_ip, ip_len, 0);
    free(raw_ip);
    if (sent != (ssize_t)ip_len) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Client test TUN send failed"); return 0; }

    // Poll for provider to process egress
    int cycles = 0;
    while (cycles < 200 && !test_timed_out) {
        uasync_poll(ua, 5); cycles++;
        if (inst_provider->nat.table[external_port].state != EIM_NAT_ENTRY_FREE) break;
    }

    // Verify NAT entry
    struct eim_nat_entry* e = &inst_provider->nat.table[external_port];
    if (e->state != EIM_NAT_ENTRY_ACTIVE || e->internal_ip != 0x0A0000CD || e->internal_port != htobe16(44444)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Egress entry not found: ip=%08x port=%u state=%d",
                    e->internal_ip, be16toh(e->internal_port), e->state);
        return 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "Egress entry ok: %08x:%u ext=%u proto=%u",
               e->internal_ip, be16toh(e->internal_port), external_port, e->proto);

    // === Step 2: Manually inject internet response into provider's TUN ===
    memset(&test_state.captured, 0, sizeof(test_state.captured));
    test_state.captured_len = 0;
    test_state.capture_count = 0;

    uint32_t gw_ip_host = inst_provider->nat.gateway_ip;
    size_t rip_len;
    uint8_t* resp_ip = build_udp_pkt(0x08080808, 80, gw_ip_host, external_port, &rip_len);
    if (tun_inject_packet(inst_provider->nat_tr.nat_tun, resp_ip, rip_len) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "tun_inject response failed");
        free(resp_ip); return 0;
    }
    free(resp_ip);

    cycles = 0;
    while (cycles < 500 && !test_timed_out && test_state.capture_count == 0) {
        uasync_poll(ua, 10); capture_tun_read(client_tun); cycles++;
    }


    if (test_state.capture_count == 0) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "No response in roundtrip"); return 0; }

    // Verify captured response
    if (!verify_ip_checksum(test_state.captured)) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Captured IP checksum invalid"); return 0; }
    uint32_t dst_net; memcpy(&dst_net, test_state.captured + 16, 4);
    if (dst_net != htobe32(0x0A0000CD)) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Roundtrip dst IP mismatch"); return 0; }
    uint16_t dp_net; memcpy(&dp_net, test_state.captured + 22, 2);
    if (dp_net != htobe16(44444)) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Roundtrip dst port mismatch"); return 0; }

    return 1;
}

/* Заполнить router send_q без потерь; затем проверить порядок. У провайдера mapping переиспользуется во время ожидания. */
static int test_router_backpressure(int provider) {
    struct UTUN_INSTANCE* sender = provider ? inst_provider : inst_client;
    struct tun_if* sink = provider ? inst_client->nat_tr.nat_tun : inst_provider->nat_tr.nat_tun;
    uint64_t destination = provider ? g_client_node_id : g_provider_node_id;
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(sender, TOPO_GROUP_UTUN, destination, ETCP_RT_ID_NAT);
    if (!route || queue_entry_count(route->send_q)) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Backpressure fixture requires empty router queue"); return 0; }
    size_t len;
    uint8_t* packet;
    uint16_t external_port = 0;
    if (provider) {
        packet = build_udp_pkt(0x0a0000dd, 45678, 0x08080808, 4242, &len);
        if (eim_nat_egress(&inst_provider->nat, packet, len, inst_client->my_ed25519_pubkey, NULL)) { free(packet); return 0; }
        memcpy(&external_port, packet + 20, 2); external_port = be16toh(external_port);
        free(packet);
        packet = build_udp_pkt(0x08080808, 4242, inst_provider->nat.gateway_ip, external_port, &len);
    } else packet = build_udp_pkt(0x0a0000ee, 40001, 0x08080808, 4242, &len);
    route->peer_receive_blocked = 1;
    uint64_t dropped = sender->nat_tr.dropped;
    int ok = 1;
    for (uint32_t i = 0; i < 66; i++) {
        memcpy(packet + 28, &i, 4);
        if (tun_inject_packet(sender->nat_tr.nat_tun, packet, len)) ok = 0;
    }
    free(packet);
    if (queue_entry_count(route->send_q) != ROUTER_MAX_SEND_Q_PACKETS || sender->nat_tr.pending_count != 2 ||
        sender->nat_tr.dropped != dropped) ok = 0;
    if (provider) {
        if (eim_nat_remove(&inst_provider->nat, external_port) != 1 ||
            eim_nat_add_forward(&inst_provider->nat, IPPROTO_UDP_UINT8, 0x0a000077, htobe16(55000), external_port, inst_provider->my_ed25519_pubkey)) ok = 0;
    }
    route->peer_receive_blocked = 0;
    uint16_t previous_limit = route->max_inflight;
    etcp_router_set_max_inflight(route, previous_limit + 1); // Возобновить drain без добавления фиктивного IP-пакета.
    uint32_t received = 0;
    for (int cycles = 0; cycles < 1000 && !test_timed_out && received < 66; cycles++) {
        uasync_poll(ua, 5);
        uint8_t data[1500]; ssize_t n;
        while ((n = tun_read_packet(sink, data, sizeof(data))) > 0) {
            uint16_t port = 0;
            if (n >= 28) memcpy(&port, data + 22, 2);
            if (port != htobe16(provider ? 45678 : 4242)) continue;
            uint32_t serial; memcpy(&serial, data + 28, 4);
            if (n != (ssize_t)len || serial != received || !verify_ip_checksum(data)) ok = 0;
            received++;
        }
    }
    etcp_router_set_max_inflight(route, previous_limit);
    if (received != 66 || sender->nat_tr.pending_count || sender->nat_tr.dropped != dropped) ok = 0;
    if (provider) eim_nat_remove(&inst_provider->nat, external_port);
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS %s router backpressure: 66/66 ordered, no drops, mapping reuse safe", provider ? "provider" : "client");
    else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL %s router backpressure: received=%u queued=%d pending=%zu dropped=%llu",
                     provider ? "provider" : "client", received, queue_entry_count(route->send_q), sender->nat_tr.pending_count,
                     (unsigned long long)(sender->nat_tr.dropped - dropped));
    return ok;
}

#ifdef __linux__
/* EAGAIN удерживает первый IP-пакет; второй остаётся в router recv_q. После возобновления оба доставляются по порядку. */
static int test_tun_write_backpressure(int provider) {
    struct UTUN_INSTANCE* receiver = provider ? inst_provider : inst_client;
    struct UTUN_INSTANCE* sender = provider ? inst_client : inst_provider;
    write_blocked_tun = receiver->nat_tr.nat_tun;
    for (uint32_t i = 1; i <= 2; i++) {
        size_t len;
        uint8_t* ip = provider ? build_udp_pkt(0x0a0000ab, 40101, 0x08080808, 44444, &len) :
            build_udp_pkt(0x08080808, 53, 0x0a0000cd, 44444, &len);
        memcpy(ip + 28, &i, 4);
        struct ll_entry* packet = ll_alloc_lldgram(len + 1);
        if (!packet) { free(ip); write_blocked_tun = NULL; return 0; }
        packet->len = len + 1; packet->dgram[0] = ETCP_RT_ID_NAT;
        memcpy(packet->dgram + 1, ip, len); free(ip);
        if (etcp_route_send(sender, TOPO_GROUP_UTUN, receiver->node_id, packet, 0, 0)) { write_blocked_tun = NULL; return 0; }
    }
    struct ETCP_ROUTER_CONN* incoming = etcp_router_conn_get(receiver, TOPO_GROUP_UTUN, sender->node_id, ETCP_RT_ID_NAT);
    for (int cycles = 0; cycles < 500 && !test_timed_out && !incoming->receive_blocked; cycles++) uasync_poll(ua, 5);
    int ok = receiver->nat_tr.tun_pending && incoming->receive_blocked;
    if (provider && receiver->nat_tr.tun_pending_port) {
        uint16_t port = receiver->nat_tr.tun_pending_port;
        uint64_t deadline = receiver->nat.table[port].last_seen + EIM_NAT_UDP_IDLE_TB;
        eim_nat_expire(&receiver->nat, deadline, 65535);
        if (!receiver->nat.table[port].holds || receiver->nat.table[port].state != EIM_NAT_ENTRY_ACTIVE) ok = 0;
    } else if (provider) ok = 0;
    write_blocked_tun = NULL;
    uint32_t received = 0;
    for (int cycles = 0; cycles < 500 && !test_timed_out && received < 2; cycles++) {
        uasync_poll(ua, 5);
        uint8_t data[1500]; ssize_t n;
        while ((n = tun_read_packet(receiver->nat_tr.nat_tun, data, sizeof(data))) > 0) {
            uint32_t serial = 0;
            if (n >= 32) memcpy(&serial, data + 28, 4);
            if (n != 42 || serial != received + 1) ok = 0;
            received++;
        }
    }
    if (received != 2 || receiver->nat_tr.tun_pending || receiver->nat_tr.tun_pending_port || incoming->receive_blocked) ok = 0;
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS %s TUN write backpressure: retained packet, held mapping, router pause and ordered resume",
                       provider ? "provider" : "client");
    else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL TUN write backpressure: received=%u pending=%p blocked=%u", received,
                     (void*)receiver->nat_tr.tun_pending, incoming->receive_blocked);
    return ok;
}
#endif

/* Один недоступный адресат исчерпывает только свой буфер. Stop отменяет pending/waiters до уничтожения TUN. */
static int test_slow_peer_and_shutdown(void) {
    uint8_t x[32], secret[32], ed[32], seed[32];
    if (sc_identity_generate(x, secret, ed, seed)) return 0;
    if (eim_nat_add_forward(&inst_provider->nat, IPPROTO_UDP_UINT8, 0x0a000099, htobe16(9091), 9090, ed)) return 0;
    size_t len;
    uint8_t* packet = build_udp_pkt(0x08080808, 53, inst_provider->nat.gateway_ip, 9090, &len);
    uint64_t before = inst_provider->nat_tr.dropped;
    for (int i = 0; i < ROUTER_MAX_SEND_Q_PACKETS + NAT_PENDING_PER_PEER + 1; i++)
        if (tun_inject_packet(inst_provider->nat_tr.nat_tun, packet, len)) { free(packet); return 0; }
    free(packet);
    int ok = inst_provider->nat_tr.pending_count == NAT_PENDING_PER_PEER && inst_provider->nat_tr.dropped == before + 1;
    if (!test_static_forward_delivery()) ok = 0; // Другой адресат продолжает получать пакеты.
    nat_transport_destroy(inst_provider);
    uasync_poll(ua, 0);
    if (inst_provider->nat_tr.initialized || inst_provider->nat_tr.pending_count || inst_provider->nat.initialized) ok = 0;
    for (struct ll_entry* e = inst_provider->router_conns->head; e; e = e->next)
        if (((struct ETCP_ROUTER_CONN*)e)->svc_id == ETCP_RT_ID_NAT) ok = 0;
    if (nat_transport_init(inst_provider)) ok = 0;
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS slow-peer isolation and stop/restart with pending TUN-pool packets");
    else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL slow-peer isolation/shutdown");
    return ok;
}

/* Общий предел останавливает TUN. После освобождения места адресат и уже преобразованный IP сохраняются. */
static int test_global_backpressure(void) {
    uint8_t first_ed[32];
    size_t len;
    for (uint32_t peer = 0; peer <= NAT_PENDING_TOTAL / NAT_PENDING_PER_PEER; peer++) {
        uint16_t external = 1000 + peer;
        uint8_t x[32], secret[32], ed[32], seed[32];
        if (sc_identity_generate(x, secret, ed, seed)) return 0;
        if (!peer) memcpy(first_ed, ed, 32);
        if (eim_nat_add_forward(&inst_provider->nat, IPPROTO_UDP_UINT8, 0x0a000100 + peer, htobe16(5000), external, ed)) return 0;
        uint8_t* packet = build_udp_pkt(0x08080808, 53, inst_provider->nat.gateway_ip, external, &len);
        unsigned int count = peer < NAT_PENDING_TOTAL / NAT_PENDING_PER_PEER ? ROUTER_MAX_SEND_Q_PACKETS + NAT_PENDING_PER_PEER : 1;
        for (unsigned int i = 0; i < count; i++) {
            if (tun_inject_packet(inst_provider->nat_tr.nat_tun, packet, len)) { free(packet); return 0; }
        }
        free(packet);
    }
    struct nat_transport_ctx* tr = &inst_provider->nat_tr;
    int ok = tr->pending_count == NAT_PENDING_TOTAL && tr->blocked_packet && queue_entry_count(tr->nat_tun->output_queue) == 1;
    uint8_t saved_ed[32]; memcpy(saved_ed, tr->blocked_ed, 32);
    uint16_t external = 1000 + NAT_PENDING_TOTAL / NAT_PENDING_PER_PEER;
    if (eim_nat_remove(&inst_provider->nat, external) != 1 ||
        eim_nat_add_forward(&inst_provider->nat, IPPROTO_UDP_UINT8, 0x0a000099, htobe16(9999), external, inst_provider->my_ed25519_pubkey)) ok = 0;
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get_key(inst_provider, TOPO_GROUP_UTUN, first_ed, ETCP_RT_ID_NAT);
    if (!route) return 0;
    struct ll_entry* consumed = queue_data_get(route->send_q); // Освободить один слот, вызвав реальный producer waiter.
    if (!consumed) ok = 0;
    else { queue_dgram_free(consumed); queue_entry_free(consumed); }
    queue_resume_callback(route->send_q);
    for (int cycles = 0; cycles < 20 && tr->blocked_packet; cycles++) uasync_poll(ua, 0);
    if (tr->blocked_packet || queue_entry_count(tr->nat_tun->output_queue) || tr->pending_count >= NAT_PENDING_TOTAL) ok = 0;
    struct ETCP_ROUTER_CONN* resumed = etcp_router_conn_get_key(inst_provider, TOPO_GROUP_UTUN, saved_ed, ETCP_RT_ID_NAT);
    if (!resumed || !resumed->send_q->head || resumed->send_q->head->len != 42) ok = 0;
    else {
        uint32_t destination; memcpy(&destination, resumed->send_q->head->dgram + 16, 4);
        if (destination != htobe32(0x0a000100 + NAT_PENDING_TOTAL / NAT_PENDING_PER_PEER)) ok = 0;
    }
    nat_transport_destroy(inst_provider);
    uasync_poll(ua, 0);
    if (inst_provider->nat_tr.initialized || inst_provider->nat_tr.pending_count) ok = 0;
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS global NAT backpressure: bounded at 1024, TUN pause/resume, mapping reuse and pending shutdown");
    else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL global NAT backpressure");
    return ok;
}

#ifdef __linux__
/* Stop обязан отменить retry и освободить удержанный mapping до уничтожения TUN/таблицы. */
static int test_shutdown_pending_write(void) {
    if (nat_transport_init(inst_provider)) return 0;
    write_blocked_tun = inst_provider->nat_tr.nat_tun;
    size_t len;
    uint8_t* ip = build_udp_pkt(0x0a000002, 40000, 0x08080808, 53, &len);
    struct ll_entry* packet = ll_alloc_lldgram(len + 1);
    if (!packet) { free(ip); write_blocked_tun = NULL; return 0; }
    packet->len = len + 1; packet->dgram[0] = ETCP_RT_ID_NAT; memcpy(packet->dgram + 1, ip, len); free(ip);
    int result = etcp_route_send(inst_provider, TOPO_GROUP_UTUN, inst_provider->node_id, packet, 0, 0);
    int ok = !result && inst_provider->nat_tr.tun_pending && inst_provider->nat_tr.tun_pending_port;
    nat_transport_destroy(inst_provider); write_blocked_tun = NULL;
    uasync_poll(ua, 0);
    if (inst_provider->nat_tr.tun_pending || inst_provider->nat_tr.tun_retry_timer || inst_provider->nat.table) ok = 0;
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS shutdown with held outgoing mapping and active TUN retry");
    else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL shutdown with pending TUN write");
    return ok;
}
#endif

/* Ни повреждённый IPv4, ни фрагмент не попадает в TUN провайдера и не расходует порты. */
static int test_provider_rejects_invalid(void) {
    uint8_t drain[1500];
    while (tun_read_packet(inst_provider->nat_tr.nat_tun, drain, sizeof(drain)) > 0) {}
    uint32_t free_before = inst_provider->nat.free_count;
    uint64_t dropped_before = inst_provider->nat_tr.dropped;
    for (unsigned int i = 0; i < 3; i++) {
        size_t len;
        uint8_t* ip = build_udp_pkt(0x0a000002, 40000, 0x08080808, 53, &len);
        if (i == 0) ip[0] = 0x65;
        if (i == 1) { uint16_t total = htobe16(100); memcpy(ip + 2, &total, 2); }
        if (i == 2) ip[6] = 0x20;
        struct ll_entry* packet = ll_alloc_lldgram(len + 1);
        if (!packet) { free(ip); return 0; }
        packet->len = len + 1; packet->dgram[0] = ETCP_RT_ID_NAT; memcpy(packet->dgram + 1, ip, len); free(ip);
        if (etcp_route_send(inst_provider, TOPO_GROUP_UTUN, inst_provider->node_id, packet, 0, 0)) return 0;
    }
    int ok = tun_read_packet(inst_provider->nat_tr.nat_tun, drain, sizeof(drain)) == 0 &&
        inst_provider->nat.free_count == free_before && inst_provider->nat_tr.dropped == dropped_before + 3;
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS invalid/fragmented IP rejected before TUN delivery");
    else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL invalid provider input");
    return ok;
}

// ==================== Main ====================
int main(void) {
    int test_result = 1;
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_NAT | DEBUG_CATEGORY_ETCP | DEBUG_CATEGORY_BGP | DEBUG_CATEGORY_ROUTING);
    utun_instance_set_tun_init_enabled(0);

    if (create_temp_configs() != 0) {
        fprintf(stderr, "Failed to create temp configs\n");
        return 1;
    }

    ua = uasync_create();
    if (!ua) { cleanup_temp_configs(); return 1; }

    inst_provider = utun_instance_create(ua, config_provider);
    inst_client   = utun_instance_create(ua, config_client);
    if (!inst_provider || !inst_client) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Failed to create instances");
        goto cleanup;
    }

    test_etcp_set_peer(inst_client, "to_prov", inst_provider, "prov");
    if (utun_instance_init(inst_provider) != 0 || utun_instance_init(inst_client) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Failed to init instances");
        goto cleanup;
    }

    g_provider_node_id = inst_provider->node_id; g_client_node_id = inst_client->node_id;

    test_timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_MS * 10, NULL, test_timeout_cb, "test_nat_transport");

    // Wait for ETCP link between client and provider
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "Waiting for ETCP link...");
    while (!test_timed_out) {
        if (first_initialized_link(inst_client)) {
            DEBUG_INFO(DEBUG_CATEGORY_NAT, "Link initialized");
            break;
        }
        uasync_poll(ua, 10);
    }
    if (test_timed_out) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Link timeout"); goto cleanup; }

    // Wait for BGP exchange (provider learns about client)
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "Waiting for BGP...");
    int bgp_cycles = 0;
    while (!test_timed_out && bgp_cycles < 500) {
        if (topo_groups_get_default(inst_provider->topo_groups) && topo_node_find_by_id(topo_groups_get_default(inst_provider->topo_groups), g_client_node_id) &&
            topo_groups_get_default(inst_client->topo_groups)   && topo_node_find_by_id(topo_groups_get_default(inst_client->topo_groups), g_provider_node_id)) {
            DEBUG_INFO(DEBUG_CATEGORY_NAT, "BGP exchanged");
            break;
        }
        uasync_poll(ua, 10);
        bgp_cycles++;
    }
    if (test_timed_out) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "BGP timeout"); goto cleanup; }

    // Run tests
    int passed = 0, total = 0;

    total++; if (test_init_destroy()) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: init_destroy");
    total++; if (test_provider_egress()) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: provider_egress");
    total++; if (test_provider_ingress_response()) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: provider_ingress");
#ifdef __linux__
    total++; if (test_send_failure_ownership(0)) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: client send ownership");
    total++; if (test_send_failure_ownership(1)) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: provider send ownership");
#endif
    total++; if (test_unexpected_nat_sender(0)) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: unexpected_sender");
    total++; if (test_unexpected_nat_sender(1)) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: unexpected_group");
    // Реальная доставка после инъекций проверяет восстановление обеих веток без перезапуска NAT.
    total++; if (test_full_roundtrip()) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: full_roundtrip");
    total++; if (test_static_forward_delivery()) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: static_forward");
    total++; if (test_provider_rejects_invalid()) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: provider_invalid");
    total++; if (test_router_backpressure(0)) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: client_backpressure");
    total++; if (test_router_backpressure(1)) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: provider_backpressure");
#ifdef __linux__
    total++; if (test_tun_write_backpressure(0)) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: client_tun_backpressure");
    total++; if (test_tun_write_backpressure(1)) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: provider_tun_backpressure");
#endif
    total++; if (test_slow_peer_and_shutdown()) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: slow_peer_shutdown");
    total++; if (test_global_backpressure()) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: global_backpressure");
#ifdef __linux__
    total++; if (test_shutdown_pending_write()) passed++; else DEBUG_ERROR(DEBUG_CATEGORY_NAT, "FAIL: shutdown_pending_write");
#endif

    printf("\n=== NAT Transport Tests: %d/%d passed ===\n", passed, total);
    test_result = (passed == total) ? 0 : 1;

cleanup:
    if (test_timeout_id) uasync_cancel_timeout(ua, test_timeout_id);
    if (inst_provider) utun_instance_destroy(inst_provider);
    if (inst_client)   utun_instance_destroy(inst_client);
    if (ua) uasync_destroy(ua, 0);
    cleanup_temp_configs();
    return test_result;
}
