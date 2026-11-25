/* Firewall: строгий INI, атомарная загрузка, разбор пакетов и обе границы основного TUN. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utun_instance.h"
#include "routing_layer/routing.h"
#include "routing_layer/route_lib.h"
#include "routing_layer/topo_node.h"
#include "transport_layer/etcp.h"
#include "tun_if.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "test_utils.h"

static int fail_firewall_alloc, fail_parser_alloc, check_sort, sorts_checked;
static unsigned sent_packets;
static int send_result;

void *__real_u_calloc_impl(uint32_t, uint32_t, const char *);
void *__wrap_u_calloc_impl(uint32_t count, uint32_t size, const char *where) {
    if (fail_firewall_alloc && strstr(where, "firewall.c:")) {
        fail_firewall_alloc = 0;
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "[fault] firewall allocation rejected");
        return NULL;
    }
    return __real_u_calloc_impl(count, size, where);
}

void *__real_u_realloc_impl(void *, uint32_t, const char *);
void *__wrap_u_realloc_impl(void *ptr, uint32_t size, const char *where) {
    if (fail_parser_alloc && strstr(where, "config_parser.c:") && !--fail_parser_alloc) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "[fault] firewall parser allocation rejected");
        return NULL;
    }
    return __real_u_realloc_impl(ptr, size, where);
}

void __real_qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void __wrap_qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *)) {
    if (check_sort) {
        for (size_t i = 0; i < count; i++) {
            const char *a = (const char *)base + i * size;
            assert(compare(a, a) == 0);
            for (size_t j = 0; j < count; j++) {
                const char *b = (const char *)base + j * size;
                assert(compare(a, b) == -compare(b, a));
            }
        }
        sorts_checked++;
    }
    __real_qsort(base, count, size, compare);
}

/* Только транспорт подменён: маршрутизация, очереди, TUN socketpair и владение пакетами настоящие. */
static const uint8_t remote_ed[32] = {[31]=42};
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE *inst, uint64_t group, const uint8_t peer[32],
                          struct ll_entry *entry, int force, int mode) {
    (void)inst; (void)force; (void)mode;
    assert(group == TOPO_GROUP_UTUN && !memcmp(peer, remote_ed, 32) && entry->len > 20);
    sent_packets++;
    queue_dgram_free(entry);
    queue_entry_free(entry);
    return send_result;
}

/* Собрать IPv4 с options и полноценным минимальным TCP/UDP-заголовком. */
static size_t packet(uint8_t *data, uint32_t ip, uint16_t port, uint8_t protocol, size_t ihl) {
    size_t payload = protocol == 6 ? 20 : 8;
    size_t len = ihl + payload;
    memset(data, 0, 128);
    data[0] = 0x40 | (ihl / 4);
    data[2] = len >> 8; data[3] = len;
    data[8] = 64; data[9] = protocol;
    data[12] = 192; data[13] = 0; data[14] = 2; data[15] = 10;
    data[16] = ip >> 24; data[17] = ip >> 16; data[18] = ip >> 8; data[19] = ip;
    data[ihl] = 0xc0; data[ihl + 1] = 0x01;
    data[ihl + 2] = port >> 8; data[ihl + 3] = port;
    if (protocol == 6) data[ihl + 12] = 0x50;
    if (protocol == 17) data[ihl + 5] = payload;
    return len;
}

static void test_rules(void) {
    struct CFG_FIREWALL_RULE rules[] = {{0x0a000002, 443}, {0x0a000001, 80}, {0x0a000001, 80}, {0x0a000002, 0}};
    struct global_config cfg = {.firewall_rules = rules, .firewall_rule_count = 4};
    struct firewall_ctx fw;
    assert(!fw_init(&fw) && !fw_check(&fw, 0x0a000001, 80));
    check_sort = 1;
    assert(!fw_load_rules(&fw, &cfg) && sorts_checked == 1);
    check_sort = 0;
    assert(fw.rules != rules && fw.count == 4 && !fw.bypass_all);
    assert(fw_check(&fw, 0x0a000001, 80) && !fw_check(&fw, 0x0a000001, 443));
    assert(fw_check(&fw, 0x0a000002, 12345) && !fw_check(&fw, 0x0a000003, 80));
    assert(!fw_check(&fw, 0x0a000001, 0));
    struct CFG_FIREWALL_RULE *previous = fw.rules;
    cfg.firewall_bypass_all = 1;
    fail_firewall_alloc = 1;
    assert(fw_load_rules(&fw, &cfg) < 0 && !fail_firewall_alloc);
    assert(fw.rules == previous && fw.count == 4 && !fw.bypass_all && fw_check(&fw, 0x0a000001, 80));
    cfg.firewall_rule_count = -1;
    assert(fw_load_rules(&fw, &cfg) < 0 && fw.rules == previous && !fw.bypass_all);
    cfg.firewall_rule_count = 4; cfg.firewall_rules = NULL;
    assert(fw_load_rules(&fw, &cfg) < 0 && fw.rules == previous);
    cfg.firewall_rules = rules;
    assert(!fw_load_rules(&fw, &cfg) && fw_check(&fw, 0, 0));
    cfg.firewall_rule_count = 0; cfg.firewall_bypass_all = 0;
    assert(!fw_load_rules(&fw, &cfg) && !fw.rules && !fw_check(&fw, 0x0a000001, 80));
    fw_free(&fw); fw_free(&fw);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "[PASS] firewall sort contract, duplicate rules and atomic policy replacement");
}

static void test_parser(void) {
    const char *valid = "[firewall]\nallow=10.0.0.1\nallow=10.0.0.2:0\nallow=10.0.0.3:65535\nallow=10.0.0.4:1\n";
    struct utun_config *cfg = parse_config_from_buf(valid, strlen(valid), "firewall-valid");
    assert(cfg && cfg->global.firewall_rule_count == 4 && !cfg->global.firewall_bypass_all);
    assert(cfg->global.firewall_rules[0].ip == 0x0a000001 && cfg->global.firewall_rules[0].port == 0);
    assert(cfg->global.firewall_rules[1].port == 0 && cfg->global.firewall_rules[2].port == 65535);
    free_config(cfg);
    const char *bad[] = {"10.0.0.1:bad", "10.0.0.1:65536", "10.0.0.1:-1", "10.0.0.1:+80", "10.0.0.1:",
        "10.0.0.1:80x", "10.0.0.1:80:90", "10.0.0.1:999999999999999999999999999999", "10.0.0.0/24", "", "::1"};
    char text[2048];
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        snprintf(text, sizeof(text), "[firewall]\nallow=all\nallow=%s\n", bad[i]);
        assert(!parse_config_from_buf(text, strlen(text), "firewall-invalid"));
    }
    const char *options[] = {"rule=10.0.0.1 80", "bypass_all=1", "allow"};
    for (size_t i = 0; i < sizeof(options) / sizeof(*options); i++) {
        snprintf(text, sizeof(text), "[firewall]\n%s\n", options[i]);
        assert(!parse_config_from_buf(text, strlen(text), "firewall-option"));
    }
    strcpy(text, "[firewall]\nallow=10.0.0.1:");
    memset(text + strlen(text), '0', 100);
    text[strlen("[firewall]\nallow=10.0.0.1:") + 100] = '\0';
    assert(!parse_config_from_buf(text, strlen(text), "firewall-long"));
    strcpy(text, "[firewall]\n");
    for (int i = 0; i < 9; i++) strcat(text, "allow=10.0.0.1:80\n");
    for (int i = 1; i <= 2; i++) {
        fail_parser_alloc = i;
        assert(!parse_config_from_buf(text, strlen(text), "firewall-oom") && !fail_parser_alloc);
    }
    cfg = parse_config_from_buf("[firewall]\nallow=all\n", 21, "firewall-all");
    assert(cfg && cfg->global.firewall_bypass_all && !cfg->global.firewall_rule_count);
    free_config(cfg);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "[PASS] firewall INI boundaries, invalid ports/options and parser allocation failures");
}

static void test_packets(void) {
    debug_set_category_level(DEBUG_CATEGORY_ROUTING, DEBUG_LEVEL_INFO);
    struct CFG_FIREWALL_RULE rule = {0x0a000001, 443};
    struct global_config cfg = {.firewall_rules = &rule, .firewall_rule_count = 1};
    struct firewall_ctx fw = {0};
    uint8_t data[128];
    assert(!fw_load_rules(&fw, &cfg));
    for (unsigned proto = 6; proto <= 17; proto += 11) {
        for (size_t ihl = 20; ihl <= 60; ihl += 4) {
            size_t len = packet(data, rule.ip, 443, proto, ihl);
            assert(fw_check_packet(&fw, data, len) == 1);
            data[ihl + 2] = 0; data[ihl + 3] = 80;
            assert(fw_check_packet(&fw, data, len) == 0);
            data[ihl] = 1; data[ihl + 1] = 187;
            assert(fw_check_packet(&fw, data, len) == 0); // source port 443 не разрешает destination 80.
        }
    }
    size_t len = packet(data, rule.ip, 443, 1, 20);
    assert(!fw_check_packet(&fw, data, len));
    len = packet(data, rule.ip, 443, 17, 20);
    data[6] = 0x20;
    assert(!fw_check_packet(&fw, data, len)); // Первый фрагмент тоже не проходит правилом порта.
    data[6] = 0; data[7] = 1;
    assert(!fw_check_packet(&fw, data, len));
    rule.port = 0;
    assert(!fw_load_rules(&fw, &cfg) && fw_check_packet(&fw, data, len) == 1);
    len = packet(data, rule.ip, 0, 1, 20);
    assert(fw_check_packet(&fw, data, len) == 1);
    cfg.firewall_bypass_all = 1;
    assert(!fw_load_rules(&fw, &cfg));
    assert(fw_check_packet(&fw, data, 19) == -1);
    data[0] = 0x65; assert(fw_check_packet(&fw, data, len) == -1);
    data[0] = 0x44; assert(fw_check_packet(&fw, data, len) == -1);
    len = packet(data, rule.ip, 443, 6, 20);
    data[3]--; assert(fw_check_packet(&fw, data, len) == -1);
    len = packet(data, rule.ip, 443, 6, 20);
    data[0] = 0x4f; assert(fw_check_packet(&fw, data, len) == -1);
    len = packet(data, rule.ip, 443, 6, 20);
    data[32] = 0x40; assert(fw_check_packet(&fw, data, len) == -1);
    data[32] = 0xf0; assert(fw_check_packet(&fw, data, len) == -1);
    len = packet(data, rule.ip, 443, 17, 20);
    data[25] = 7; assert(fw_check_packet(&fw, data, len) == -1);
    data[25] = 9; assert(fw_check_packet(&fw, data, len) == -1);
    data[3] = 24; assert(fw_check_packet(&fw, data, 24) == -1);
    len = packet(data, rule.ip, 443, 17, 20);
    data[6] = 0x60; assert(fw_check_packet(&fw, data, len) == -1);
    data[6] = 0x80; assert(fw_check_packet(&fw, data, len) == -1);
    data[6] = 0x20; data[3]--; assert(fw_check_packet(&fw, data, len - 1) == -1);
    fw_free(&fw);
    debug_set_category_level(DEBUG_CATEGORY_ROUTING, DEBUG_LEVEL_DEBUG);
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "[PASS] firewall IPv4 options, TCP/UDP destination, ICMP, fragments and malformed lengths");
}

/* Доставить пакет через зарегистрированный DATA callback с настоящим router-префиксом. */
static void incoming(struct UTUN_INSTANCE *inst, const uint8_t *data, size_t len) {
    struct ll_entry *entry = ll_alloc_lldgram(ROUTER_SVC_PAYLOAD_OFF + len);
    assert(entry);
    entry->len = ROUTER_SVC_PAYLOAD_OFF + len;
    entry->dgram[0] = ETCP_RT_ID_DATA;
    memcpy(entry->dgram + ROUTER_SVC_SRC_ED_OFF, remote_ed, 32);
    memcpy(entry->dgram + ROUTER_SVC_DST_ED_OFF, inst->my_ed25519_pubkey, 32);
    memcpy(entry->dgram + ROUTER_SVC_PAYLOAD_OFF, data, len);
    struct ETCP_CONN conn = {.instance = inst};
    inst->router_bindings.callbacks[ETCP_RT_ID_DATA](&conn, entry);
}

static void test_tun(void) {
    struct UASYNC *ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE inst = {.ua = ua, .node_id = 1};
    struct utun_config cfg = {.global = {.tun_test_mode = 1, .tun_ifname = "fw_test", .tun_ip = {.family = AF_INET}}};
    inst.tun = tun_init(ua, &cfg); assert(inst.tun);
    assert(!routing_create(&inst) && !routing_bind(&inst));
    routing_set_tun(&inst);
    struct TOPO_SUBNET4 local_subnet = {.addr = {10,0,0,0}, .prefix_length = 24};
    struct TOPO_SUBNET4 remote_subnet = {.addr = {10,0,1,0}, .prefix_length = 24};
    struct TOPO_NODESUBNETS local_subnets = {.v4_subnets = &local_subnet}, remote_subnets = {.v4_subnets = &remote_subnet};
    struct TOPO_GROUP_NODE local = {.node_id = 1, .subnets = &local_subnets}, peer = {.node_id = 1, .subnets = &remote_subnets};
    memcpy(peer.ed25519, remote_ed, 32);
    assert(route_insert(inst.rt, &local, true) && route_insert(inst.rt, &peer, false));
    struct CFG_FIREWALL_RULE rules[] = {{0x0a000001, 443}, {0x0a000101, 443}};
    cfg.global.firewall_rules = rules; cfg.global.firewall_rule_count = 2;
    assert(!fw_load_rules(&inst.fw, &cfg.global));
    uint8_t data[128], received[128];
    size_t len = packet(data, rules[1].ip, 443, 6, 24);
    assert(!tun_inject_packet(inst.tun, data, len) && sent_packets == 1);
    len = packet(data, rules[1].ip, 80, 6, 24);
    assert(!tun_inject_packet(inst.tun, data, len) && sent_packets == 1 && inst.dropped_packets == 1);
    len = packet(data, rules[0].ip, 443, 17, 24);
    incoming(&inst, data, len);
    int fd = tun_get_test_fd(inst.tun);
    assert(recv(fd, received, sizeof(received), MSG_DONTWAIT) == (ssize_t)len && !memcmp(data, received, len));
    len = packet(data, rules[0].ip, 80, 17, 24);
    incoming(&inst, data, len);
    assert(recv(fd, received, sizeof(received), MSG_DONTWAIT) < 0 && inst.dropped_packets == 2);
    // Ответ на разрешённое исходящее соединение требует разрешения собственного порта назначения.
    len = packet(data, rules[0].ip, 49153, 6, 20);
    data[20] = 1; data[21] = 187;
    incoming(&inst, data, len);
    assert(recv(fd, received, sizeof(received), MSG_DONTWAIT) < 0 && inst.dropped_packets == 3);
    queue_set_size_limit(inst.tun->input_queue, 0);
    len = packet(data, rules[0].ip, 443, 17, 20);
    incoming(&inst, data, len); // Отказ очереди уже освобождает пакет.
    assert(inst.dropped_packets == 4);
    queue_set_size_limit(inst.tun->input_queue, -1);
    cfg.global.firewall_rule_count = 0;
    assert(!fw_load_rules(&inst.fw, &cfg.global));
    len = packet(data, rules[1].ip, 80, 17, 20);
    incoming(&inst, data, len); // Транзит DATA не касается TUN и не фильтруется.
    assert(sent_packets == 2);
    assert(!tun_inject_packet(inst.tun, data, len) && sent_packets == 2 && inst.dropped_packets == 5);
    struct tun_if *nat = tun_init_nat(ua, "fw_nat", "10.2.0.1", 1500, 1); assert(nat);
    assert(!tun_inject_packet(nat, data, len) && queue_entry_count(nat->output_queue) == 1);
    tun_close(nat);
    cfg.global.firewall_bypass_all = 1;
    assert(!fw_load_rules(&inst.fw, &cfg.global));
    send_result = -1;
    assert(!tun_inject_packet(inst.tun, data, len) && sent_packets == 3 && inst.dropped_packets == 6);
    send_result = 0;
    struct ll_entry *invalid = ll_alloc_lldgram(1); assert(invalid);
    invalid->len = 1;
    assert(!queue_data_put(inst.tun->output_queue, invalid));
    struct ll_entry *short_entry = ll_alloc_lldgram(1); assert(short_entry);
    short_entry->len = 1;
    route_pkt(&inst, short_entry, remote_ed);
    struct ll_entry *no_instance = ll_alloc_lldgram(1); assert(no_instance);
    route_pkt(NULL, no_instance, remote_ed);
    routing_destroy(&inst);
    tun_close(inst.tun);
    fw_free(&inst.fw);
    uasync_destroy(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "[PASS] both main TUN directions, explicit reply rules, DATA transit, NAT isolation and packet ownership");
}

static const char *instance_config =
    "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
    "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
    "debug_level=warn\n[debug]\nconfig=info\nrouting=debug\n";

static void test_reload(void) {
    char directory[] = "/tmp/utun-firewall-XXXXXX";
    assert(!test_mkdtemp(directory));
    char path[256], text[1024];
    snprintf(path, sizeof(path), "%s/reload.conf", directory);
    struct UASYNC *ua = uasync_create(); assert(ua);
    snprintf(text, sizeof(text), "%s[firewall]\nallow=10.0.0.1:80\n", instance_config);
    struct UTUN_INSTANCE *inst = utun_instance_create_from_str(ua, text); assert(inst);
    struct utun_config *old_config = inst->config;
    struct CFG_FIREWALL_RULE *old_rules = inst->fw.rules;
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fprintf(file, "%s[firewall]\nallow=all\nallow=10.0.0.2:443\n", instance_config) > 0 && !fclose(file));
    fail_firewall_alloc = 1;
    assert(utun_instance_reload(inst, ua, path) == inst && !fail_firewall_alloc);
    assert(inst->config == old_config && inst->fw.rules == old_rules && !inst->fw.bypass_all);
    assert(fw_check(&inst->fw, 0x0a000001, 80) && !fw_check(&inst->fw, 0x0a000002, 443));
    assert(utun_instance_reload(inst, ua, path) == inst && inst->fw.bypass_all && inst->fw.count == 1 && !inst->core_started);
    old_config = inst->config; old_rules = inst->fw.rules;
    file = fopen(path, "wb"); assert(file);
    assert(fprintf(file, "%s[firewall]\nallow=10.0.0.2:65536\n", instance_config) > 0 && !fclose(file));
    assert(utun_instance_reload(inst, ua, path) == inst && inst->config == old_config && inst->fw.rules == old_rules);
    file = fopen(path, "wb"); assert(file);
    assert(fprintf(file, "%s[firewall]\n", instance_config) > 0 && !fclose(file));
    assert(utun_instance_reload(inst, ua, path) == inst && !inst->fw.count && !inst->fw.bypass_all && !inst->core_started);
    utun_instance_destroy(inst);
    uasync_destroy(ua, 0);
    assert(!test_unlink(path) && !test_rmdir(directory));
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "[PASS] selective reload retains config/rules on OOM and parse failure; valid/empty policy applied");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_ROUTING, DEBUG_LEVEL_DEBUG);
    size_t baseline = u_get_allocated_count();
    test_rules(); test_parser(); test_packets(); test_tun(); test_reload();
    assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "[PASS] firewall regressions completed without live allocations");
    return 0;
}
