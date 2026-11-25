/* Проверки ограничений входа и диагностического вывода настоящего IPv4-форматтера. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/transport_layer/packet_dump.h"

#define CHECK(expr) do { \
    if (!(expr)) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet dump test line=%d: %s", __LINE__, #expr); exit(1); } \
} while (0)

static void expect_contains(const char* text, const char* part) {
    if (!strstr(text, part)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "packet dump: missing '%s' in '%s'", part, text);
        exit(1);
    }
}

static void init_packet(uint8_t* packet, size_t total, unsigned ihl, uint8_t protocol) {
    memset(packet, 0, total);
    packet[0] = 0x40 | (ihl / 4);
    packet[2] = (uint8_t)(total >> 8); packet[3] = (uint8_t)total;
    packet[9] = protocol;
    packet[12] = 192; packet[13] = 168; packet[14] = 1; packet[15] = 2;
    packet[16] = 10; packet[17] = 20; packet[18] = 30; packet[19] = 40;
}

static void test_protocols(void) {
    uint8_t packet[60];
    init_packet(packet, 40, 20, 6);
    packet[22] = 0xff; packet[23] = 0xff;
    packet[24] = 0x89; packet[25] = 0xab; packet[26] = 0xcd; packet[27] = 0xef;
    packet[28] = 0xff; packet[29] = 0xff; packet[30] = 0xff; packet[31] = 0xff;
    packet[32] = 0x50; packet[33] = 0xff; packet[34] = 0x80; packet[35] = 0x01;
    const char* out = dump_ip_packet_to_buffer(packet, 40);
    CHECK(strcmp(out, "IPv4 TCP 192.168.1.2:0 -> 10.20.30.40:65535 (40 bytes) "
                      "[FIN,SYN,RST,PSH,ACK,URG,ECE,CWR] seq=2309737967 ack=4294967295 win=32769 "
                      "data: 0000ffff89abcdef ffffffff50ff8001 00000000") == 0);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "%s", out);
    packet[33] = 0;
    expect_contains(dump_ip_packet_to_buffer(packet, 40), "[none]");

    init_packet(packet, 28, 20, 17);
    packet[22] = 0x01; packet[23] = 0xbb; packet[25] = 8;
    CHECK(strcmp(dump_ip_packet_to_buffer(packet, 28),
                 "IPv4 UDP 192.168.1.2:0 -> 10.20.30.40:443 (28 bytes) len=8 data: 000001bb00080000") == 0);

    init_packet(packet, 28, 20, 1);
    packet[20] = 8; packet[24] = 0x12; packet[25] = 0x34; packet[26] = 0xab; packet[27] = 0xcd;
    expect_contains(dump_ip_packet_to_buffer(packet, 28), "echo_request seq=43981 id=4660 data: 080000001234abcd");
    packet[20] = 0;
    expect_contains(dump_ip_packet_to_buffer(packet, 28), "echo_reply seq=43981 id=4660");
    packet[20] = 3; packet[21] = 1;
    expect_contains(dump_ip_packet_to_buffer(packet, 28), "type=3 code=1");

    init_packet(packet, 20, 20, 99);
    CHECK(strcmp(dump_ip_packet_to_buffer(packet, 20),
                 "IPv4 UNKNOWN 192.168.1.2 -> 10.20.30.40 (20 bytes) data: ") == 0);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS protocols: TCP flags/none, zero/max ports, seq/ack/window, UDP, ICMP, unknown/empty");
}

static void test_input_diagnostics(void) {
    uint8_t packet[60];
    expect_contains(dump_ip_packet_to_buffer(NULL, 0), "Missing IP packet");
    expect_contains(dump_ip_packet_to_buffer(NULL, 100), "Missing IP packet");
    init_packet(packet, 20, 20, 99);
    packet[0] = 0x65;
    expect_contains(dump_ip_packet_to_buffer(packet, 20), "Non-IPv4 packet: version=6");
    for (unsigned ihl = 0; ihl < 5; ihl++) {
        packet[0] = 0x40 | ihl;
        expect_contains(dump_ip_packet_to_buffer(packet, 20), "Invalid IP header length");
    }
    init_packet(packet, 20, 20, 99);
    packet[3] = 19;
    expect_contains(dump_ip_packet_to_buffer(packet, 20), "Invalid IP total length");
    packet[3] = 0;
    expect_contains(dump_ip_packet_to_buffer(packet, 20), "Invalid IP total length");
    packet[0] = 0x4f;
    expect_contains(dump_ip_packet_to_buffer(packet, 20), "Truncated IP header");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS input diagnostics: NULL, version, invalid IHL, total length, missing options");
}

static void test_lengths_and_alignment(void) {
    uint8_t packet[76];
    init_packet(packet, sizeof(packet), 60, 17);
    packet[65] = 16;
    debug_set_category_level(DEBUG_CATEGORY_DUMP, DEBUG_LEVEL_DISABLED);
    for (size_t len = 0; len <= sizeof(packet); len++) {
        uint8_t* exact = u_malloc(len + 1);
        CHECK(exact != NULL);
        memcpy(exact + 1, packet, len);
        const char* out = dump_ip_packet_to_buffer(exact + 1, len);
        if (len < 20) expect_contains(out, "Packet too small");
        else if (len < 60) expect_contains(out, "Truncated IP header");
        else if (len < sizeof(packet)) expect_contains(out, "Truncated IPv4 packet");
        else expect_contains(out, "UDP 192.168.1.2:0 -> 10.20.30.40:0 (76 bytes) len=16");
        CHECK(memcmp(exact + 1, packet, len) == 0);
        u_free(exact);
    }
    debug_set_category_level(DEBUG_CATEGORY_DUMP, DEBUG_LEVEL_WARN);

    init_packet(packet, 28, 20, 17);
    packet[25] = 8;
    memset(packet + 28, 0xab, sizeof(packet) - 28);
    const char* out = dump_ip_packet_to_buffer(packet, sizeof(packet));
    expect_contains(out, "(28 bytes) len=8 data: 0000000000080000");
    CHECK(!strstr(out, "abab"));
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS lengths: 77 capture lengths, IP options, unaligned input, ignored capture suffix, unchanged input");
}

static void test_fragments(void) {
    uint8_t packet[40];
    const uint8_t protocols[] = {6, 17, 1};
    for (size_t i = 0; i < sizeof(protocols); i++) {
        init_packet(packet, sizeof(packet), 20, protocols[i]);
        packet[7] = 1;
        const char* out = dump_ip_packet_to_buffer(packet, sizeof(packet));
        expect_contains(out, "192.168.1.2 -> 10.20.30.40 (40 bytes) fragment offset=8 MF=0");
        CHECK(!strstr(out, "seq=") && !strstr(out, "len=") && !strstr(out, "type=") && !strstr(out, "header:"));
        packet[6] = 0x3f; packet[7] = 0xff;
        expect_contains(dump_ip_packet_to_buffer(packet, sizeof(packet)), "fragment offset=65528 MF=1");
    }
    init_packet(packet, sizeof(packet), 20, 6);
    packet[6] = 0x20; packet[32] = 0x50;
    expect_contains(dump_ip_packet_to_buffer(packet, sizeof(packet)), "[none] seq=0 ack=0 win=0 fragment offset=0 MF=1");
    packet[3] = 28;
    expect_contains(dump_ip_packet_to_buffer(packet, 28), "partial header: available=8 required=20 fragment offset=0 MF=1");
    packet[6] = 0;
    expect_contains(dump_ip_packet_to_buffer(packet, 28), "truncated header: available=8 required=20");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS fragments: subsequent/first, MF, maximum offset, partial transport header");
}

static void test_hex_bounds(void) {
    const uint8_t protocols[] = {6, 17, 1, 99};
    size_t cases = 0;
    debug_set_category_level(DEBUG_CATEGORY_DUMP, DEBUG_LEVEL_DISABLED);
    for (size_t p = 0; p < sizeof(protocols); p++) {
        for (unsigned ihl = 20; ihl <= 60; ihl += 4) {
            for (size_t count = 0; count <= 65; count++) {
                size_t total = ihl + count;
                uint8_t* exact = u_malloc(total + 1);
                CHECK(exact != NULL);
                uint8_t* packet = exact + 1;
                init_packet(packet, total, ihl, protocols[p]);
                memset(packet + ihl, 0xa5, count);
                const char* out = dump_ip_packet_to_buffer(packet, total);
                const char* hex = strstr(out, " data: ");
                CHECK(hex != NULL && strlen(out) < 1024);
                hex += strlen(" data: ");
                size_t displayed = count < 32 ? count : 32;
                CHECK(strlen(hex) == displayed * 2 + (displayed ? (displayed - 1) / 8 : 0));
                for (size_t i = 0; i < displayed; i++) {
                    if (i && i % 8 == 0) CHECK(*hex++ == ' ');
                    CHECK(*hex++ == 'a'); CHECK(*hex++ == '5');
                }
                CHECK(*hex == '\0');
                u_free(exact);
                cases++;
            }
        }
    }
    debug_set_category_level(DEBUG_CATEGORY_DUMP, DEBUG_LEVEL_WARN);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS hex bounds: %zu cases, four protocols, 11 IHL values, 0..65 payload bytes", cases);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_DUMP, DEBUG_LEVEL_WARN);
    test_protocols();
    test_input_diagnostics();
    test_lengths_and_alignment();
    test_fragments();
    test_hex_bounds();
    uint8_t packet[20];
    init_packet(packet, sizeof(packet), 20, 99);
    char* first = dump_ip_packet_to_buffer(packet, sizeof(packet));
    CHECK(first == dump_ip_packet_to_buffer(NULL, 0));
    expect_contains(first, "Missing IP packet");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS packet dump: 5 groups and static output lifetime");
    return 0;
}
