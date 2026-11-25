/* Настоящие UDP-наблюдения и отклонение PONG чужого ключа/поколения. */
#include "nat_fixture.h"

struct result { int done, success; uint8_t data[128]; size_t length; };

static void ping_done(int success, uint16_t rtt, void *arg, uint64_t nonce, const uint8_t *data, size_t length) {
    (void)rtt; (void)nonce;
    struct result *result = arg; result->done++; result->success = success; result->length = length;
    assert(length <= sizeof(result->data)); if (length) memcpy(result->data, data, length);
}

static void wait_result(struct nat_fixture *fixture, struct result *result) {
    uint64_t end = get_time_tb() + 30000;
    while (!result->done && get_time_tb() < end) uasync_poll(fixture->ua, 10);
    assert(result->done == 1);
}

static const struct PING_CONTEXT *pending_probe(const struct UTUN_INSTANCE *owner, const struct result *result) {
    for (const struct PING_CONTEXT *ping = owner->pending_pings; ping; ping = ping->next)
        if (ping->cb == ping_done && ping->arg == result) return ping;
    return NULL;
}

/* Аутентичный PONG от другого узла с известным nonce. Его source совпадает с
 * target ожидающего запроса, поэтому ключ пира обязан проверяться отдельно. */
static void send_foreign_pong(struct UTUN_INSTANCE *sender, struct ETCP_SOCKET *destination, uint64_t nonce) {
    uint8_t plain[ETCP_ENCRYPTED_HDR_SIZE + sizeof(struct ETCP_PING_HDR)] = {0}, encrypted[256], salt[8], obfuscated[32];
    plain[2] = 1; plain[3] = ETCP_PONG;
    struct ETCP_PING_HDR* pong = (struct ETCP_PING_HDR*)(plain + ETCP_ENCRYPTED_HDR_SIZE);
    memcpy(pong->ed25519, sender->my_ed25519_pubkey, sizeof(pong->ed25519));
    for (unsigned i = 0; i < 8; i++) pong->nonce[i] = (uint8_t)(nonce >> (56 - i * 8));
    sc_context_t crypto; assert(sc_init_ctx(&crypto, &sender->my_keys) == SC_OK);
    assert(sc_set_peer_public_key(&crypto, destination->instance->my_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
    size_t length = 0; assert(sc_encrypt(&crypto, plain, sizeof(plain), encrypted, &length) == SC_OK);
    assert(random_bytes(salt, sizeof(salt)) == 0);
    sc_obfuscate_pubkey(salt, destination->instance->my_keys.public_key, sender->my_keys.public_key, obfuscated);
    memcpy(encrypted + length, salt, sizeof(salt)); memcpy(encrypted + length + sizeof(salt), obfuscated, sizeof(obfuscated));
    length += sizeof(salt) + sizeof(obfuscated);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(sender, "udp");
    assert(socket_sendto(socket->fd, encrypted, length, (const struct sockaddr *)&destination->local_addr,
                         sizeof(struct sockaddr_in)) == (ssize_t)length);
}

static void address_encoding(void) {
    struct sockaddr_storage address = {0}, decoded;
    struct sockaddr_in *v4 = (struct sockaddr_in *)&address;
    v4->sin_family = AF_INET; assert(inet_pton(AF_INET, "198.51.100.7", &v4->sin_addr) == 1); v4->sin_port = htons(1300);
    uint8_t wire[ROUTE_ADDR_WIRE_SIZE];
    static const uint8_t expected[19] = {4,198,51,100,7,0,0,0,0,0,0,0,0,0,0,0,0,5,20};
    assert(route_ping_addr_encode(wire, &address) == 0 && !memcmp(wire, expected, sizeof(wire)));
    assert(route_ping_addr_decode(wire, &decoded) == 0 && route_ping_addr_equal(&address, &decoded, 1));
    wire[5] = 1; assert(route_ping_addr_decode(wire, &decoded) < 0); /* скрытые байты IPv4 запрещены */
    memset(&address, 0, sizeof(address));
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address;
    v6->sin6_family = AF_INET6; v6->sin6_port = htons(0xabcd);
    assert(inet_pton(AF_INET6, "2001:db8::1234", &v6->sin6_addr) == 1);
    assert(route_ping_addr_encode(wire, &address) == 0 && wire[0] == 6 && wire[17] == 0xab && wire[18] == 0xcd);
    assert(route_ping_addr_decode(wire, &decoded) == 0 && route_ping_addr_equal(&address, &decoded, 1));
    wire[17] = wire[18] = 0; assert(route_ping_addr_decode(wire, &decoded) < 0);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_NAT, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_SOCKET, DEBUG_LEVEL_DEBUG);
    address_encoding();
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL, NULL);
    struct UTUN_INSTANCE *observer = nat_fixture_node(&fixture, "127.0.0.3", NULL, NULL);
    struct UTUN_INSTANCE *foreign = nat_fixture_node(&fixture, "127.0.0.4", NULL, NULL);
    nat_fixture_connect(&fixture, owner, "udp", observer);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "udp");
    struct ETCP_SOCKET *peer = test_etcp_bound_socket(observer, "udp");
    const uint64_t round = UINT64_C(0x0102030405060708);
    struct result result = {0};
    assert(route_ping_observe(socket, observer->my_keys.public_key, &peer->local_addr, TOPO_GROUP_UTUN, round, 500, ping_done, &result) == 0);
    wait_result(&fixture, &result);
    struct sockaddr_storage observed;
    assert(result.success && route_ping_observation(result.data, result.length, socket, TOPO_GROUP_UTUN, round, &observed) == 0);
    assert(route_ping_addr_equal(&observed, &socket->local_addr, 1));
    assert(route_ping_observation(result.data, result.length, socket, TOPO_GROUP_UTUN, round + 1, &observed) < 0);
    assert(route_ping_observation(result.data, result.length, socket, TOPO_GROUP_UTUN + 1, round, &observed) < 0);
    assert(socket->address_profile.mapping == ADDRESS_MAP_UNKNOWN); /* один ответ не назначает NAT-тип */

    memset(&result, 0, sizeof(result));
    struct ETCP_SOCKET *untrusted = test_etcp_bound_socket(foreign, "udp");
    assert(route_ping_observe(socket, observer->my_keys.public_key, &untrusted->local_addr,
                              TOPO_GROUP_UTUN, round + 1, 300, ping_done, &result) == 0);
    const struct PING_CONTEXT *probe = pending_probe(owner, &result);
    assert(probe);
    send_foreign_pong(foreign, socket, probe->nonce);
    wait_result(&fixture, &result);
    assert(!result.success && !pending_probe(owner, &result));

    memset(&result, 0, sizeof(result));
    assert(route_ping_observe(socket, observer->my_keys.public_key, &peer->local_addr, TOPO_GROUP_UTUN, round + 2, 300, ping_done, &result) == 0);
    uint64_t generation = socket->generation; socket->generation = generation == UINT64_MAX ? generation - 1 : generation + 1;
    wait_result(&fixture, &result); assert(!result.success); socket->generation = generation;

    memset(&result, 0, sizeof(result));
    assert(route_ping_observe(socket, observer->my_keys.public_key, &peer->local_addr, TOPO_GROUP_UTUN, round + 3, 500, ping_done, &result) == 0);
    etcp_cancel_pings(owner, ping_done, &result); assert(!pending_probe(owner, &result));
    uint64_t end = get_time_tb() + 6000;
    while (get_time_tb() < end) uasync_poll(fixture.ua, 10);
    assert(!result.done);
    nat_fixture_cleanup(&fixture);
    return 0;
}
