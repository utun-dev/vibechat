// Проверяем пакеты на границе proxy → TUN: длины, адреса, payload и checksum.
#include <stdio.h>
#include <string.h>
#include "utun_instance.h"
#include "tun_if.h"
#include "proxy/udp_proxy.h"
#include "proxy/icmp_proxy.h"
#include "debug_config.h"
#include "mem.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static unsigned checksum_sum(const uint8_t* p, size_t n) {
    unsigned sum = 0;
    while (n >= 2) { sum += ((unsigned)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) sum += (unsigned)p[0] << 8;
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return sum;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    struct UASYNC* ua = uasync_create();
    struct UTUN_INSTANCE* inst = u_calloc(1, sizeof(*inst));
    struct tcp_proxy_client client = {0};
    struct tun_if tun = {0};
    CHECK(ua && inst);
    inst->tcp_proxy_client = &client; client.tun = &tun;
    tun.input_queue = queue_new(ua, 0, 0, 0, "packet_test");
    CHECK(tun.input_queue);
    uint8_t payload[1501];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 17 + 3);
    const uint8_t src[4] = {192, 0, 2, 1}, dst[4] = {10, 0, 0, 2};
    uint32_t src_ip, dst_ip; memcpy(&src_ip, src, 4); memcpy(&dst_ip, dst, 4);
    for (size_t len = 0; len <= sizeof(payload); len++) {
        CHECK(udp_proxy_deliver_reply(inst, src_ip, htons(53), dst_ip, htons(23456), payload, len) == 0);
        struct ll_entry* e = queue_data_get(tun.input_queue);
        CHECK(e && e->len == 29 + len);
        const uint8_t* ip = e->dgram + 1;
        CHECK(e->dgram[0] == 4 && ip[9] == 17 && checksum_sum(ip, 20) == 65535);
        CHECK(memcmp(ip + 12, src, 4) == 0 && memcmp(ip + 16, dst, 4) == 0);
        CHECK(ip[26] == 0 && ip[27] == 0);
        CHECK(memcmp(ip + 28, payload, len) == 0);
        queue_dgram_free(e); queue_entry_free(e);
        CHECK(icmp_proxy_deliver_reply(inst, dst_ip, src_ip, htons(123), htons(5), payload, len) == 0);
        e = queue_data_get(tun.input_queue);
        CHECK(e && e->len == 29 + len);
        ip = e->dgram + 1;
        CHECK(ip[9] == 1 && checksum_sum(ip, 20) == 65535 && checksum_sum(ip + 20, 8 + len) == 65535);
        CHECK(ip[20] == 0 && ip[24] == 0 && ip[25] == 123 && ip[27] == 5);
        CHECK(memcmp(ip + 28, payload, len) == 0);
        queue_dgram_free(e); queue_entry_free(e);
    }
    queue_free(tun.input_queue); u_free(inst); uasync_destroy(ua, 0);
    CHECK(u_get_allocated_count() == 0);
    puts("[PASS] proxy UDP/ICMP packets: payload lengths 0..1501");
    return 0;
}
