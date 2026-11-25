/* Контракт кодека: полные roundtrip, точные буферы и атомарные выходы при отказе. */
#include "chat/invite_link.h"
#include "transport_layer/secure_channel.h"
#include "../lib/debug_config.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "codec line=%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned cases;
static void rejected(const char* link, size_t len) {
    struct InviteData out, before; memset(&out, 0xa5, sizeof(out)); before = out;
    char error[3] = {1, 1, 1};
    CHECK(invite_link_decode(link, len, &out, error, sizeof(error)) < 0);
    CHECK(!memcmp(&out, &before, sizeof(out)) && !error[2]); cases++;
}
static void rejected_raw(const uint8_t* raw, size_t size) {
    char link[INVITE_LINK_MAX]; memcpy(link, "utun://", 7);
    int len = EVP_EncodeBlock((unsigned char*)link + 7, raw, (int)size);
    CHECK(len > 0); rejected(link, (size_t)len + 7);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    struct InviteData input = { .join_key = UINT64_MAX };
    struct SC_MYKEYS keys; CHECK(sc_generate_keypair(&keys) == SC_OK);
    memcpy(input.pubkey, keys.public_key, 32);
    uint8_t inviter_x[32], inviter_private[32], inviter_seed[32];
    CHECK(sc_identity_generate(inviter_x, inviter_private, input.inviter_pubkey, inviter_seed) == SC_OK);
    /* Полный нулевой ключ запрещён; нулевой префикс допустим. */
    input.pubkey[0] = 0;
    for (unsigned count = 1; count <= INVITE_ADDR_MAX; count++) for (unsigned mode = 0; mode < 5; mode++) {
        input.addrCount = count; memset(&input.reality, 0, sizeof(input.reality));
        for (unsigned i = 0; i < count; i++) {
            struct InviteAddr* a = &input.addrs[i]; memset(a, 0, sizeof(*a));
            a->family = mode == 0 ? 4 : (mode == 1 || mode == 4) ? 6 : (i & 1 ? 4 : 6);
            a->port = (uint16_t)(1 + i * 1000); a->socketId = (uint8_t)(255 - i);
            a->proto = mode >= 3 ? 6 : i & 1 ? 1 : 2;
            for (unsigned j = 0; j < (a->family == 6 ? 16u : 4u); j++) a->address[j] = (uint8_t)(i + j + 1);
        }
        if (mode >= 3) {
            input.reality.has = 1; input.reality.server_pubkey[0] = 7;
            memset(input.reality.server_name, 'a', 63); input.reality.server_name[63] = 0;
        }
        char link[INVITE_LINK_MAX], exact[INVITE_LINK_MAX], small[INVITE_LINK_MAX];
        CHECK(invite_link_encode(&input, link, sizeof(link)) > 0);
        size_t length = strlen(link);
        if (mode == 4 && count == 32) CHECK(length + 1 == INVITE_LINK_MAX);
        CHECK(length < INVITE_LINK_MAX && invite_link_encode(&input, exact, length + 1) > 0);
        CHECK(!strcmp(exact, link)); memset(small, 0xa5, sizeof(small));
        CHECK(invite_link_encode(&input, small, length) < 0);
        for (unsigned i = 0; i < sizeof(small); i++) CHECK((unsigned char)small[i] == 0xa5);
        struct InviteData out; CHECK(!invite_link_decode(link, length, &out, NULL, 0));
        CHECK(out.join_key == input.join_key && out.addrCount == count);
        CHECK(!memcmp(out.pubkey, input.pubkey, 32));
        CHECK(!memcmp(out.inviter_pubkey, input.inviter_pubkey, 32));
        CHECK(!memcmp(out.addrs, input.addrs, count * sizeof(input.addrs[0])));
        CHECK(!memcmp(&out.reality, &input.reality, sizeof(out.reality)));
        uint8_t a[1024], b[1024]; int bytes = invite_serialize_addrs(&out, a, sizeof(a));
        CHECK(bytes > 0); memset(b, 0xa5, sizeof(b));
        CHECK(invite_serialize_addrs(&out, b, (size_t)bytes - 1) < 0);
        for (unsigned i = 0; i < sizeof(b); i++) CHECK(b[i] == 0xa5);
        CHECK(invite_serialize_addrs(&out, b, (size_t)bytes) == bytes && !memcmp(a, b, (size_t)bytes));
        /* Отказ при каждом усечении, кроме границ полных самостоятельных блоков. */
        for (size_t cut = 0; cut < length; cut++) {
            struct InviteData partial;
            if (invite_link_decode(link, cut, &partial, NULL, 0) == 0) {
                CHECK(partial.addrCount > 0 && partial.addrCount < count); continue;
            }
            rejected(link, cut);
        }
        cases++;
    }
    for (unsigned i = 0; i < 256; i++) {
        char link[INVITE_LINK_MAX]; CHECK(invite_link_encode(&input, link, sizeof(link)) > 0);
        if (!((i >= 'A' && i <= 'Z') || (i >= 'a' && i <= 'z') || (i >= '0' && i <= '9') || i == '+' || i == '/')) {
            link[12] = (char)i; rejected(link, strlen(link + 13) + 13);
        }
    }
    char link[INVITE_LINK_MAX]; CHECK(invite_link_encode(&input, link, sizeof(link)) > 0);
    uint8_t raw[1200]; int raw_len = EVP_DecodeBlock(raw, (const unsigned char*)link + 7, (int)strlen(link) - 7);
    raw_len -= link[strlen(link) - 1] == '='; raw_len -= link[strlen(link) - 2] == '=';
    uint8_t saved[1200]; memcpy(saved, raw, (size_t)raw_len);
    CHECK(raw[0] == INVITE_LINK_VERSION && raw[1] == INVITE_KIND_CHANNEL);
    raw[0] = 0x06; rejected_raw(raw, (size_t)raw_len); memcpy(raw, saved, (size_t)raw_len);
    memset(raw + 10, 0, 32); rejected_raw(raw, (size_t)raw_len); memcpy(raw, saved, (size_t)raw_len);
    raw[42] = 2; rejected_raw(raw, (size_t)raw_len); memcpy(raw, saved, (size_t)raw_len);
    raw[86] = 64; rejected_raw(raw, (size_t)raw_len); memcpy(raw, saved, (size_t)raw_len);
    raw[87] = 0; rejected_raw(raw, (size_t)raw_len); memcpy(raw, saved, (size_t)raw_len);
    raw[150] |= 0x80; rejected_raw(raw, (size_t)raw_len); memcpy(raw, saved, (size_t)raw_len);
    size_t second = 150 + 33 + 4 * 20; raw[second + 1] ^= 1;
    rejected_raw(raw, (size_t)raw_len); memcpy(raw, saved, (size_t)raw_len);
    rejected_raw(raw, 20); /* обязательный узел отсутствует */
    memset(raw + 2, 0, 8); rejected_raw(raw, (size_t)raw_len);
    struct InviteData bad = input; bad.addrCount = 0; CHECK(invite_link_validate(&bad) < 0);
    bad = input; bad.join_key = 0; CHECK(invite_link_validate(&bad) < 0);
    bad = input; bad.addrCount = 33; CHECK(invite_link_validate(&bad) < 0);
    bad = input; memset(bad.pubkey, 0, 32); CHECK(invite_link_validate(&bad) < 0);
    bad = input; memset(bad.inviter_pubkey, 0, 32); CHECK(invite_link_validate(&bad) < 0);
    bad = input; bad.addrs[0].family = 5; CHECK(invite_link_validate(&bad) < 0);
    bad = input; bad.addrs[0].port = 0; CHECK(invite_link_validate(&bad) < 0);
    bad = input; bad.addrs[0].proto = 3; CHECK(invite_link_validate(&bad) < 0);
    bad = input; bad.reality.has = 0; CHECK(invite_link_validate(&bad) < 0);
    struct InviteData server = input, decoded_server;
    server.kind = INVITE_KIND_SERVER; server.join_key = 0;
    memset(server.inviter_pubkey, 0, sizeof(server.inviter_pubkey));
    memset(server.add_key, 0x57, sizeof(server.add_key));
    CHECK(invite_link_encode(&server, link, sizeof(link)) > 0);
    CHECK(!invite_link_decode(link, strlen(link), &decoded_server, NULL, 0));
    CHECK(decoded_server.kind == INVITE_KIND_SERVER && !decoded_server.join_key);
    CHECK(!memcmp(server.add_key, decoded_server.add_key, sizeof(server.add_key)) && decoded_server.addrCount == server.addrCount);
    for (size_t cut = 1; cut < 34; cut++) {
        uint8_t server_raw[34] = {INVITE_LINK_VERSION, INVITE_KIND_SERVER};
        rejected_raw(server_raw, cut);
    }
    server.join_key = 1; CHECK(invite_link_validate(&server) < 0); server.join_key = 0;
    memset(server.add_key, 0, sizeof(server.add_key)); CHECK(invite_link_validate(&server) < 0);
    bad = input; bad.kind = 2; CHECK(invite_link_validate(&bad) < 0);
    rejected("utun://", 7); rejected(NULL, 1);
    printf("invite codec: %u contract cases PASS\n", cases); return 0;
}
