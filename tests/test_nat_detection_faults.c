/* Настоящие UDP/router-пакеты; сетевые потери и задержка вводятся на границе sendto.
 * Подмена наблюдения отдельно проверяет применение подписанных mapping-отчётов. */
/* Инъекция sendto вызывает ETCP API и меняет фикстуру: этот тест намеренно использует один uasync-поток. */
#define NAT_FIXTURE_GLOBAL_OPTIONS "crypto_tx_threads=0\ncrypto_rx_threads=0\n"
#include "nat_fixture.h"
#include "transport_layer/packet_transport.h"
#include "test_clock.h"
#include "routing_layer/etcp_router.h"

enum fault_mode { FAULT_NONE, FAULT_NO_PONG, FAULT_FILTERED, FAULT_LATE_PRE, FAULT_CONTACT,
                  FAULT_DEPENDENT, FAULT_UNSTABLE, FAULT_NO_REPORT, FAULT_INGRESS_BLOCKED, FAULT_NO_ANCHOR,
                  FAULT_LOCAL_INGRESS, FAULT_LOCAL_OBSERVER, FAULT_LOCAL_HELPER_MAPPING, FAULT_CONTROL_STALL, FAULT_EIM_UNVERIFIED,
                  FAULT_KNOWN_ALIAS, FAULT_BUDGET, FAULT_RESUME_TIMEOUT,
                  FAULT_SHARED_UNREACHABLE, FAULT_SHARED_WRONG_SOURCE, FAULT_SHARED_REMOVED };

static struct {
    struct nat_fixture *fixture;
    struct UTUN_INSTANCE *owner, *observer, *helper, *alias;
    struct ETCP_SOCKET *socket, *alias_source;
    enum fault_mode mode;
    unsigned dropped, altered, direct_received, observations, preserved;
    int contacted, injected;
    socket_t delayed_fd;
    uint8_t delayed[1600];
    size_t delayed_length;
    struct sockaddr_storage delayed_target;
    socklen_t delayed_target_length;
} fault;

ssize_t __real_socket_sendto(socket_t, const void *, size_t, const struct sockaddr *, socklen_t);
void __real_nat_detection_record_observation(struct NAT_DETECTION *, struct ETCP_SOCKET *, const uint8_t *,
                                            const struct sockaddr_storage *, const uint8_t *, size_t);
void __real_nat_detection_receive_ingress(struct NAT_DETECTION *, struct ETCP_SOCKET *, const uint8_t *,
                                         const struct sockaddr_storage *, const uint8_t *, size_t);
int __real_etcp_route_send_key(struct UTUN_INSTANCE *, uint64_t, const uint8_t *, struct ll_entry *, int, int);

/* Помощник отправляет пробы, но его управляющие результаты до наблюдателя не доходят. */
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE *instance, uint64_t group, const uint8_t destination[32],
                           struct ll_entry *entry, int force, int mode) {
    if (fault.fixture && ((fault.mode == FAULT_CONTROL_STALL && instance == fault.helper) ||
        (fault.mode == FAULT_BUDGET && instance != fault.owner && instance != fault.observer)) &&
        !memcmp(destination, fault.observer->my_ed25519_pubkey, 32) && entry->len && entry->dgram[0] == ETCP_RT_ID_NAT_DETECTION) {
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "fault fixture: helper control result dropped before observer delivery");
        fault.dropped++; queue_dgram_free(entry); queue_entry_free(entry); return 0;
    }
    return __real_etcp_route_send_key(instance, group, destination, entry, force, mode);
}

static void ignored_ping(int success, uint16_t rtt, void *arg, uint64_t nonce, const uint8_t *data, size_t length) {
    (void)success; (void)rtt; (void)arg; (void)nonce; (void)data; (void)length;
}

/* Расшифровываем только PING/PONG с публичным хвостом, используя настоящие ключи участников стенда. */
static size_t decode_ping(struct ETCP_SOCKET *socket, const uint8_t *packet, size_t length, uint8_t plain[1600]) {
    if (length <= SC_PUBKEY_ENC_SIZE || length > 1600) return 0;
    const uint8_t *salt = packet + length - SC_PUBKEY_ENC_SIZE;
    for (unsigned i = 0; i < fault.fixture->node_count; i++) {
        struct UTUN_INSTANCE *peer = fault.fixture->nodes[i];
        uint8_t obfuscated[SC_PUBKEY_SIZE];
        sc_obfuscate_pubkey(salt, peer->my_keys.public_key, socket->instance->my_keys.public_key, obfuscated);
        if (memcmp(obfuscated, salt + SC_PUBKEY_ENC_SALT_SIZE, sizeof(obfuscated))) continue;
        sc_context_t crypto; assert(sc_init_ctx(&crypto, &socket->instance->my_keys) == SC_OK);
        assert(sc_set_peer_public_key(&crypto, peer->my_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
        size_t size = 0; assert(sc_decrypt(&crypto, packet, length - SC_PUBKEY_ENC_SIZE, plain, &size) == SC_OK);
        return size >= ETCP_ENCRYPTED_HDR_SIZE + sizeof(struct ETCP_PING_HDR) && (plain[3] == ETCP_PING || plain[3] == ETCP_PONG) ? size : 0;
    }
    return 0;
}

ssize_t __wrap_socket_sendto(socket_t fd, const void *packet, size_t length, const struct sockaddr *target, socklen_t target_length) {
    if (!fault.fixture) return __real_socket_sendto(fd, packet, length, target, target_length);
    struct ETCP_SOCKET *source = NULL;
    for (unsigned i = 0; i < fault.fixture->node_count && !source; i++)
        for (struct ETCP_SOCKET *socket = fault.fixture->nodes[i]->etcp_sockets; socket; socket = socket->next)
            if (socket->fd == fd) { source = socket; break; }
    uint8_t plain[1600];
    size_t size = source ? decode_ping(source, packet, length, plain) : 0;
    if (!size) return __real_socket_sendto(fd, packet, length, target, target_length);
    struct sockaddr_storage destination = {0}; assert(target_length <= sizeof(destination));
    memcpy(&destination, target, target_length);
    int to_owner = route_ping_addr_equal(&destination, &fault.socket->local_addr, 1);
    if (fault.alias_source && fault.mode == FAULT_SHARED_UNREACHABLE && source == fault.socket && plain[3] == ETCP_PING &&
        route_ping_addr_equal(&destination, &fault.alias_source->local_addr, 1)) {
        fault.dropped++; return length;
    }
    if (fault.alias_source && source == fault.alias_source && to_owner && plain[3] == ETCP_PING &&
        (plain[4] & ETCP_PING_FLAG_ADDRESS_CHECK)) {
        if (fault.mode == FAULT_SHARED_WRONG_SOURCE) {
            fault.altered++;
            struct ETCP_SOCKET *third = test_etcp_bound_socket(fault.observer, "third");
            return __real_socket_sendto(third->fd, packet, length, target, target_length);
        }
        if (fault.mode == FAULT_SHARED_UNREACHABLE || fault.mode == FAULT_SHARED_REMOVED) {
            fault.dropped++; return length;
        }
    }
    if (to_owner && source->instance == fault.alias && plain[3] == ETCP_PING &&
        (plain[4] & ETCP_PING_FLAG_ADDRESS_CHECK)) {
        if (fault.mode == FAULT_LOCAL_HELPER_MAPPING) { fault.dropped++; return length; }
        fault.altered++;
        return __real_socket_sendto(fault.alias_source->fd, packet, length, target, target_length);
    }
    if ((fault.mode == FAULT_NO_PONG || fault.mode == FAULT_NO_ANCHOR || fault.mode == FAULT_EIM_UNVERIFIED) &&
        to_owner && plain[3] == ETCP_PONG) {
        fault.dropped++; return length;
    }
    if (source == fault.socket && plain[3] == ETCP_PING && (plain[4] & ETCP_PING_FLAG_OBSERVE) &&
        !route_ping_addr_equal(&destination, &test_etcp_bound_socket(fault.observer, "udp")->local_addr, 0)) {
        fault.contacted = 1;
        ssize_t result = __real_socket_sendto(fd, packet, length, target, target_length);
        if (fault.delayed_length) {
            assert(__real_socket_sendto(fault.delayed_fd, fault.delayed, fault.delayed_length,
                (const struct sockaddr *)&fault.delayed_target, fault.delayed_target_length) == (ssize_t)fault.delayed_length);
            DEBUG_INFO(DEBUG_CATEGORY_NAT, "fault fixture: delayed PRE delivered after outgoing contact");
            fault.delayed_length = 0; fault.injected = 1;
        }
        return result;
    }
    if (to_owner && plain[3] == ETCP_PING && (plain[4] & ETCP_PING_FLAG_ADDRESS_CHECK)) {
        if (fault.mode == FAULT_CONTACT && !fault.injected) {
            fault.injected = 1;
            assert(!etcp_send_ping_to_socket(fault.owner, fault.socket, source->instance->my_keys.public_key,
                &source->local_addr, 500, ignored_ping, NULL, NULL, 0, 0));
            DEBUG_INFO(DEBUG_CATEGORY_NAT, "fault fixture: real outgoing PING contaminates PRE source IP");
        }
        if (fault.mode == FAULT_INGRESS_BLOCKED || fault.mode == FAULT_CONTROL_STALL || fault.mode == FAULT_BUDGET ||
            ((fault.mode == FAULT_FILTERED || fault.mode == FAULT_LATE_PRE || fault.mode == FAULT_NO_ANCHOR) && !fault.contacted)) {
            if (fault.mode == FAULT_LATE_PRE && !fault.delayed_length && !fault.injected) {
                memcpy(fault.delayed, packet, length); fault.delayed_length = length; fault.delayed_fd = fd;
                fault.delayed_target = destination; fault.delayed_target_length = target_length;
            }
            if (fault.mode == FAULT_BUDGET) {
                memcpy(fault.delayed, packet, length); fault.delayed_length = length; fault.delayed_fd = fd;
                fault.delayed_target = destination; fault.delayed_target_length = target_length;
            }
            fault.dropped++; return length;
        }
        fault.direct_received++;
    }
    return __real_socket_sendto(fd, packet, length, target, target_length);
}

void __wrap_nat_detection_record_observation(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
    const uint8_t *key, const struct sockaddr_storage *source, const uint8_t *tag, size_t length) {
    if (fault.fixture && fault.mode == FAULT_RESUME_TIMEOUT && socket->instance == fault.observer &&
        length == ROUTE_OBSERVE_TAG_SIZE + 9 && !memcmp(key, fault.owner->my_keys.public_key, 32)) {
        fault.dropped++; return;
    }
    if (fault.fixture && fault.mode == FAULT_EIM_UNVERIFIED && length == ROUTE_OBSERVE_TAG_SIZE + 9 &&
        !memcmp(key, fault.owner->my_keys.public_key, 32)) {
        /* Два направления видят один переведённый адрес. История помощника уже загрязнена настоящим PING. */
        struct sockaddr_storage mapped = *source;
        assert(inet_pton(AF_INET, "127.0.0.9", &((struct sockaddr_in *)&mapped)->sin_addr) == 1);
        fault.observations++;
        __real_nat_detection_record_observation(detector, socket, key, &mapped, tag, length); return;
    }
    if (fault.fixture && socket->instance == fault.observer && length == ROUTE_OBSERVE_TAG_SIZE + 9 &&
        ((fault.mode == FAULT_LOCAL_OBSERVER && !memcmp(key, fault.owner->my_keys.public_key, 32)) ||
         (fault.mode == FAULT_LOCAL_HELPER_MAPPING && !memcmp(key, fault.alias->my_keys.public_key, 32)))) {
        /* Настоящий source — loopback; в этом callback включаем промышленную проверку публичности. */
        fault.injected++;
        nat_detection_set_allow_local(detector, 0);
        __real_nat_detection_record_observation(detector, socket, key, source, tag, length);
        nat_detection_set_allow_local(detector, 1); return;
    }
    if (!fault.fixture || socket->instance != fault.helper || memcmp(key, fault.owner->my_keys.public_key, 32) ||
        length != ROUTE_OBSERVE_TAG_SIZE + 9) {
        __real_nat_detection_record_observation(detector, socket, key, source, tag, length); return;
    }
    fault.observations++;
    if (fault.mode == FAULT_NO_REPORT) { fault.dropped++; return; }
    struct sockaddr_storage observed = *source;
    if (fault.mode == FAULT_DEPENDENT || (fault.mode == FAULT_UNSTABLE && fault.observations == 1)) {
        struct sockaddr_in *address = (struct sockaddr_in *)&observed;
        address->sin_port = htons(ntohs(address->sin_port) == 65535 ? 65534 : ntohs(address->sin_port) + 1);
        fault.altered++;
        DEBUG_INFO(DEBUG_CATEGORY_NAT, "fault fixture: observed mapping changed from=%s to=%s",
                   sockaddr_storage_to_str(source).str, sockaddr_storage_to_str(&observed).str);
    }
    __real_nat_detection_record_observation(detector, socket, key, &observed, tag, length);
}

/* Подлинная проба приходит с локального IP, несмотря на пригодный предварительный адрес помощника. */
void __wrap_nat_detection_receive_ingress(struct NAT_DETECTION *detector, struct ETCP_SOCKET *socket,
    const uint8_t *key, const struct sockaddr_storage *source, const uint8_t *tag, size_t length) {
    if (fault.fixture && fault.mode == FAULT_KNOWN_ALIAS && detector == fault.owner->nat_det &&
        !memcmp(key, fault.alias->my_keys.public_key, 32) && socket->address_profile.inbound == ADDRESS_IN_OPEN)
        fault.preserved++;
    if (fault.fixture && fault.mode == FAULT_LOCAL_INGRESS && detector == fault.owner->nat_det &&
        !memcmp(key, fault.alias->my_keys.public_key, 32)) {
        struct nat_check_status before, after;
        assert(!nat_detection_get_status(detector, socket, &before));
        nat_detection_set_allow_local(detector, 0);
        /* Неверный token не должен позволять постороннему пакету сменить кандидата. */
        uint8_t stale[ROUTE_OBSERVE_TAG_SIZE + 9]; assert(length == sizeof(stale));
        memcpy(stale, tag, length); stale[length - 1] ^= 1;
        __real_nat_detection_receive_ingress(detector, socket, key, source, stale, length);
        assert(!nat_detection_get_status(detector, socket, &after));
        assert(after.phase == before.phase && after.helper_reselections == before.helper_reselections);
        __real_nat_detection_receive_ingress(detector, socket, key, source, tag, length);
        assert(!nat_detection_get_status(detector, socket, &after));
        if (before.phase && !memcmp(before.helper, fault.alias->my_ed25519_pubkey, 32)) {
            assert(!after.phase && after.helper_reselections == before.helper_reselections + 1);
            assert(after.reason == NAT_REASON_NO_HELPER); fault.injected++;
        }
        nat_detection_set_allow_local(detector, 1); return;
    }
    __real_nat_detection_receive_ingress(detector, socket, key, source, tag, length);
}

static void run_case(enum fault_mode mode, uint8_t mapping, uint8_t inbound) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL, NULL);
    struct UTUN_INSTANCE *observer = nat_fixture_node(&fixture, "127.0.0.3", NULL, NULL);
    struct UTUN_INSTANCE *helper = nat_fixture_node(&fixture, "127.0.0.4", NULL, NULL);
    nat_detection_set_automatic(observer->nat_det, 0); nat_detection_set_automatic(helper->nat_det, 0);
    nat_fixture_connect(&fixture, observer, "udp", helper);
    nat_fixture_connect(&fixture, owner, "udp", observer);
    if (mode != FAULT_CONTACT) nat_detection_set_automatic(owner->nat_det, 0);
    memset(&fault, 0, sizeof(fault)); fault.fixture = &fixture; fault.owner = owner;
    fault.observer = observer; fault.helper = helper; fault.socket = test_etcp_bound_socket(owner, "udp"); fault.mode = mode;
    if (mode == FAULT_EIM_UNVERIFIED)
        assert(!etcp_send_ping_to_socket(owner, fault.socket, helper->my_keys.public_key,
            &test_etcp_bound_socket(helper, "udp")->local_addr, 500, ignored_ping, NULL, NULL, 0, 0));
    struct nat_check_status status = {0};
    nat_detection_get_status(owner->nat_det, fault.socket, &status); uint32_t rounds = status.rounds;
    assert(!nat_detection_request_check_socket(owner->nat_det, fault.socket, observer->my_ed25519_pubkey));
    uint64_t end = get_time_tb() + 400000;
    do { test_clock_poll(fixture.ua, 1000); assert(!nat_detection_get_status(owner->nat_det, fault.socket, &status)); }
    while (get_time_tb() < end && (status.rounds == rounds || status.phase || status.cycling));
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "fault fixture result mode=%d mapping=%s inbound=%s phase=%u cycling=%u reason=%s dropped=%u observations=%u",
        mode, address_mapping_name(fault.socket->address_profile.mapping), address_inbound_name(fault.socket->address_profile.inbound),
        status.phase, status.cycling, nat_check_reason_name(status.reason), fault.dropped, fault.observations);
    assert(!status.phase && !status.cycling);
    assert(fault.socket->address_profile.mapping == mapping && fault.socket->address_profile.inbound == inbound);
    if (mode == FAULT_NO_PONG || mode == FAULT_FILTERED || mode == FAULT_LATE_PRE || mode == FAULT_NO_REPORT) assert(fault.dropped);
    if (mode == FAULT_LATE_PRE || mode == FAULT_CONTACT) assert(fault.injected);
    if (mode == FAULT_CONTACT) {
        assert(status.rounds == rounds + 1 && status.helper_reselections == 1);
        struct sockaddr_storage fresh = test_etcp_bound_socket(helper, "udp")->local_addr;
        assert(inet_pton(AF_INET, "127.0.0.9", &((struct sockaddr_in *)&fresh)->sin_addr) == 1);
        assert(nat_detection_probe_delay(owner->nat_det) > 0);
        assert(!nat_detection_ping_allowed(owner->nat_det, fault.socket, helper->my_keys.public_key, &fresh, 0, NULL, 0));
    }
    if (mode == FAULT_DEPENDENT || mode == FAULT_UNSTABLE) assert(fault.altered);
    if (mode == FAULT_LOCAL_OBSERVER) assert(fault.injected && status.helper_reselections && !fault.observations);
    if (mode == FAULT_CONTROL_STALL) assert(fault.dropped && fault.observations >= 2 && status.rounds == rounds + 1);
    if (mode == FAULT_EIM_UNVERIFIED) {
        assert(fault.observations >= 4 && !fault.direct_received && fault.socket->nat_addr.ss_family == AF_INET);
        sqlite3_stmt *statement = NULL;
        assert(sqlite3_prepare_v2(owner->topo_sqlite_db,
            "SELECT addr_type FROM node_addresses WHERE node_x=? AND socket_id=? AND address_kind=?", -1, &statement, NULL) == SQLITE_OK);
        sqlite3_bind_blob(statement, 1, owner->my_keys.public_key, 32, SQLITE_STATIC);
        sqlite3_bind_int(statement, 2, fault.socket->sock_id); sqlite3_bind_int(statement, 3, TOPO_ADDR_NAT);
        assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == ADDR_TYPE_NAT_EIM_UNVERIFIED);
        sqlite3_finalize(statement);
    }
    fault.fixture = NULL; nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT fault mode=%d", mode);
}

/* Разные ключи не означают разные IP: неподходящего помощника отклоняет получатель. */
static void run_alias(const char *alias_ip, enum fault_mode mode) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct nat_device_keys first, second;
    uint8_t first_ed[32], second_ed[32], private_x[32];
    assert(sc_identity_generate(first.public_key, private_x, first_ed, first.seed) == SC_OK);
    assert(sc_identity_generate(second.public_key, private_x, second_ed, second.seed) == SC_OK);
    if (memcmp(first_ed, second_ed, 32) > 0) {
        struct nat_device_keys swap = first; first = second; second = swap;
    }
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL, NULL);
    struct UTUN_INSTANCE *observer = nat_fixture_node(&fixture, "127.0.0.3", NULL, NULL);
    char extra[160]; snprintf(extra, sizeof(extra), "[server:alias]\naddr=%s:0\ntype=private\n", alias_ip);
    struct UTUN_INSTANCE *alias = nat_fixture_node(&fixture, "127.0.0.8", &first, extra);
    struct UTUN_INSTANCE *helper = nat_fixture_node(&fixture, "127.0.0.4", &second, NULL);
    nat_detection_set_automatic(observer->nat_det, 0); nat_detection_set_automatic(alias->nat_det, 0);
    nat_detection_set_automatic(helper->nat_det, 0);
    nat_fixture_connect(&fixture, observer, "udp", alias); nat_fixture_connect(&fixture, observer, "udp", helper);
    nat_fixture_connect(&fixture, owner, "udp", observer); nat_detection_set_automatic(owner->nat_det, 0);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "udp");
    memset(&fault, 0, sizeof(fault)); fault.fixture = &fixture; fault.owner = owner; fault.socket = socket;
    fault.observer = observer; fault.helper = helper; fault.alias = alias; fault.alias_source = test_etcp_bound_socket(alias, "alias");
    fault.mode = mode;
    if (mode == FAULT_KNOWN_ALIAS)
        assert(!etcp_send_ping_to_socket(owner, socket, alias->my_keys.public_key,
            &fault.alias_source->local_addr, 500, ignored_ping, NULL, NULL, 0, 0));
    struct nat_check_status status = {0}; uint64_t end = get_time_tb() + 400000;
    nat_detection_get_status(owner->nat_det, socket, &status); uint32_t rounds = status.rounds;
    assert(!nat_detection_request_check_socket(owner->nat_det, socket, observer->my_ed25519_pubkey));
    int partial_seen = 0;
    do {
        test_clock_poll(fixture.ua, 1000); assert(!nat_detection_get_status(owner->nat_det, socket, &status));
        if (status.cycling && socket->address_profile.mapping == ADDRESS_MAP_REAL_IP &&
            socket->address_profile.inbound == ADDRESS_IN_UNKNOWN) partial_seen = 1;
    }
    while (get_time_tb() < end && (status.rounds == rounds || status.phase || status.cycling));
    assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_OPEN);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "alias fixture result source=%s helper=%s expected=%s reselections=%u remapped=%u",
               alias_ip, sc_public_key_hex(status.helper, (char[65]){0}), sc_public_key_hex(helper->my_ed25519_pubkey, (char[65]){0}),
               status.helper_reselections, fault.altered);
    assert(!memcmp(status.helper, helper->my_ed25519_pubkey, 32) && status.helper_reselections);
    if (mode == FAULT_LOCAL_HELPER_MAPPING) assert(fault.injected && fault.dropped);
    else assert(fault.altered);
    if (mode == FAULT_LOCAL_INGRESS) assert(fault.injected);
    if (mode == FAULT_KNOWN_ALIAS) {
        assert(partial_seen);
        /* Сохранённый OPEN не завершает повторную проверку на знакомом фактическом IP alias. */
        struct nat_device_keys next;
        uint8_t next_ed[32];
        do { assert(sc_identity_generate(next.public_key, private_x, next_ed, next.seed) == SC_OK); }
        while (memcmp(next_ed, alias->my_ed25519_pubkey, 32) <= 0);
        struct UTUN_INSTANCE *fresh = nat_fixture_node(&fixture, "127.0.0.6", &next, NULL);
        nat_detection_set_automatic(fresh->nat_det, 0);
        fault.helper = fresh; rounds = status.rounds;
        assert(!nat_detection_request_check_socket(owner->nat_det, socket, observer->my_ed25519_pubkey));
        /* Резерв нового измерения включён до появления нового IP в топологии. */
        nat_fixture_connect(&fixture, observer, "udp", fresh);
        end = get_time_tb() + 400000;
        do {
            test_clock_poll(fixture.ua, 1000); assert(!nat_detection_get_status(owner->nat_det, socket, &status));
        } while (get_time_tb() < end && (status.rounds == rounds || status.phase || status.cycling));
        assert(fault.preserved && !status.phase && !status.cycling && !memcmp(status.helper, fresh->my_ed25519_pubkey, 32) && !status.reason);
        assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_OPEN);
    }
    fault.fixture = NULL; nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT receiver rejects helper source=%s", alias_ip);
}

/* LAN INIT_RESPONSE описывает свой путь и не инвалидирует уже известный интернет-профиль. */
static void run_lan_observation(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL, NULL);
    struct UTUN_INSTANCE *peer = nat_fixture_node(&fixture, "127.0.0.3", NULL, NULL);
    nat_detection_set_automatic(owner->nat_det, 0); nat_detection_set_automatic(peer->nat_det, 0);
    struct ETCP_CONN *connection = nat_fixture_connect(&fixture, owner, "udp", peer);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "udp");
    struct address_profile profile = {ADDRESS_MAP_EIM, ADDRESS_IN_OPEN, ADDRESS_SOURCE_ETCP};
    struct sockaddr_storage external = socket->local_addr;
    assert(inet_pton(AF_INET, "8.8.8.8", &((struct sockaddr_in *)&external)->sin_addr) == 1);
    socket->address_profile = profile; socket->nat_addr = external;
    nat_detection_set_allow_local(owner->nat_det, 0);
    nat_detection_observed_by_peer(owner->nat_det, connection->links, &socket->local_addr);
    assert(!memcmp(&socket->address_profile, &profile, sizeof(profile)));
    assert(route_ping_addr_equal(&socket->nat_addr, &external, 1));
    nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS LAN observation preserves internet mapping and inbound profile");
}

/* Одно соединение даёт NAT-кандидат с типом UNKNOWN; он подписывается и распространяется отдельно от адреса интерфейса. */
static void run_unknown_address(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL, NULL);
    struct UTUN_INSTANCE *peer = nat_fixture_node(&fixture, "127.0.0.3", NULL, NULL);
    nat_detection_set_automatic(owner->nat_det, 0); nat_detection_set_automatic(peer->nat_det, 0);
    struct ETCP_CONN *connection = nat_fixture_connect(&fixture, owner, "udp", peer);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "udp");
    struct sockaddr_storage observed = socket->local_addr;
    assert(inet_pton(AF_INET, "127.0.0.9", &((struct sockaddr_in *)&observed)->sin_addr) == 1);
    nat_detection_observed_by_peer(owner->nat_det, connection->links, &observed);
    assert(socket->address_profile.mapping == ADDRESS_MAP_UNKNOWN && socket->address_profile.inbound == ADDRESS_IN_UNKNOWN);
    assert(socket->address_profile.source == ADDRESS_SOURCE_ETCP && route_ping_addr_equal(&socket->nat_addr, &observed, 1));
    struct TOPO_NODE *node = topo_node_registry_find(owner->topo_groups, owner->node_id);
    assert(node && !topo_node_verify(node));
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(owner->topo_sqlite_db,
        "SELECT addr_type,nat_mapping,nat_inbound,nat_source,address FROM node_addresses"
        " WHERE node_x=? AND socket_id=? AND address_kind=?", -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_blob(statement, 1, owner->my_keys.public_key, 32, SQLITE_STATIC);
    sqlite3_bind_int(statement, 2, socket->sock_id); sqlite3_bind_int(statement, 3, TOPO_ADDR_NAT);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == ADDR_TYPE_NAT);
    assert(!sqlite3_column_int(statement, 1) && !sqlite3_column_int(statement, 2));
    assert(sqlite3_column_int(statement, 3) == ADDRESS_SOURCE_ETCP);
    assert(sqlite3_column_bytes(statement, 4) == 4);
    assert(!memcmp(sqlite3_column_blob(statement, 4), &((struct sockaddr_in *)&observed)->sin_addr, 4));
    sqlite3_finalize(statement);
    uint64_t end = get_time_tb() + 30000; int received = 0;
    while (get_time_tb() < end && !received) {
        test_clock_poll(fixture.ua, 1000);
        node = topo_node_registry_find(peer->topo_groups, owner->node_id);
        for (struct TOPO_ADDR4 *a = node ? node->v4_addrs : NULL; a; a = a->next)
            if (a->type == TOPO_ADDR_NAT && a->socket_id == socket->sock_id &&
                !memcmp(a->addr, &((struct sockaddr_in *)&observed)->sin_addr, 4)) received = 1;
    }
    assert(received && !topo_node_verify(node));
    struct nat_check_status status;
    assert(!nat_detection_get_status(owner->nat_det, socket, &status)); uint32_t rounds = status.rounds;
    assert(!nat_detection_request_check_socket(owner->nat_det, socket, peer->my_ed25519_pubkey));
    end = get_time_tb() + 400000;
    do { test_clock_poll(fixture.ua, 1000); assert(!nat_detection_get_status(owner->nat_det, socket, &status)); }
    while (get_time_tb() < end && (status.rounds == rounds || status.phase || status.cycling));
    assert(!status.phase && !status.cycling && status.reason == NAT_REASON_NO_HELPER);
    assert(socket->address_profile.mapping == ADDRESS_MAP_UNKNOWN && route_ping_addr_equal(&socket->nat_addr, &observed, 1));
    nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT type=UNKNOWN keeps one observed candidate in signed NODEINFO and SQLite");
}

/* Первый UDP listener наблюдателя недоступен; рабочий второй IP либо принятый вход всё равно дают результат. */
static void run_endpoint_retry(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL,
        "[server:tcp]\naddr=127.0.0.2:0\ntransport=tcp\ntype=public\n");
    struct UTUN_INSTANCE *observer = nat_fixture_node(&fixture, "127.0.0.3", NULL,
        "[server:other]\naddr=127.0.0.8:0\ntype=public\n[server:tcp]\naddr=127.0.0.3:0\ntransport=tcp\ntype=public\n");
    struct UTUN_INSTANCE *helper = nat_fixture_node(&fixture, "127.0.0.4", NULL,
        "[server:tcp]\naddr=127.0.0.4:0\ntransport=tcp\ntype=public\n");
    nat_detection_set_automatic(observer->nat_det, 0); nat_detection_set_automatic(helper->nat_det, 0);
    struct ETCP_SOCKET *bad = test_etcp_bound_socket(observer, "udp");
    if (bad->transport) packet_transport_receive(bad->transport, 0);
    else assert(!uasync_set_socket_read(fixture.ua, bad->socket_id, 0));
    nat_fixture_connect(&fixture, observer, "tcp", helper); nat_fixture_connect(&fixture, owner, "tcp", observer);
    nat_detection_set_automatic(owner->nat_det, 0);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "udp");
    struct nat_check_status status = {0}; uint64_t end = get_time_tb() + 400000;
    nat_detection_get_status(owner->nat_det, socket, &status); uint32_t rounds = status.rounds;
    assert(!nat_detection_request_check_socket(owner->nat_det, socket, observer->my_ed25519_pubkey));
    do { test_clock_poll(fixture.ua, 1000); assert(!nat_detection_get_status(owner->nat_det, socket, &status)); }
    while (get_time_tb() < end && (status.phase || status.cycling ||
        (status.rounds == rounds && socket->address_profile.inbound != ADDRESS_IN_OPEN)));
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "endpoint fixture result mapping=%s inbound=%s rounds=%u previous=%u reason=%s",
               address_mapping_name(socket->address_profile.mapping), address_inbound_name(socket->address_profile.inbound),
               status.rounds, rounds, nat_check_reason_name(status.reason));
    assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_OPEN);
    if (!status.helper_reselections) {
        struct ETCP_LINK *accepted = NULL;
        for (struct ETCP_LINK *link = instance_find_conn(owner, observer->node_id)->links; link; link = link->next)
            if (link->conn == socket && link->is_server && link->initialized) accepted = link;
        assert(accepted && !status.reason && !status.phase && !status.cycling);
    } else assert(!memcmp(status.observer, observer->my_ed25519_pubkey, 32));
    nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT unavailable observer IP: alternative endpoint or accepted incoming connection confirms OPEN");
}

/* Несколько медленных помощников исчерпывают общий бюджет посередине следующей попытки. */
static void run_budget_resume(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL, NULL);
    struct UTUN_INSTANCE *observer = nat_fixture_node(&fixture, "127.0.0.3", NULL, NULL);
    nat_detection_set_automatic(owner->nat_det, 0); nat_detection_set_automatic(observer->nat_det, 0);
    for (unsigned i = 0; i < 4; i++) {
        char ip[32]; snprintf(ip, sizeof(ip), "127.0.0.%u", i + 4);
        struct UTUN_INSTANCE *helper = nat_fixture_node(&fixture, ip, NULL, NULL);
        nat_detection_set_automatic(helper->nat_det, 0); nat_fixture_connect(&fixture, observer, "udp", helper);
    }
    nat_fixture_connect(&fixture, owner, "udp", observer);
    memset(&fault, 0, sizeof(fault)); fault.fixture = &fixture; fault.owner = owner; fault.observer = observer;
    fault.socket = test_etcp_bound_socket(owner, "udp"); fault.mode = FAULT_BUDGET;
    for (unsigned i = 2; i < fixture.node_count; i++)
        assert(!etcp_send_ping_to_socket(owner, fault.socket, fixture.nodes[i]->my_keys.public_key,
            &test_etcp_bound_socket(fixture.nodes[i], "udp")->local_addr, 500, ignored_ping, NULL, NULL, 0, 0));
    assert(!nat_detection_request_check_socket(owner->nat_det, fault.socket, observer->my_ed25519_pubkey));
    struct nat_check_status status = {0}; uint64_t end = get_time_tb() + 500000;
    /* Все UDP-ответы формируются в общем uasync без crypto workers; потери задаёт фикстура. */
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "fault fixture: local I/O for budget and retry timers on the shared UDP loop");
    do { test_clock_poll_local_io(fixture.ua, 1000); assert(!nat_detection_get_status(owner->nat_det, fault.socket, &status)); }
    while (get_time_tb() < end && (status.phase || !status.cycling || status.reason != NAT_REASON_BUDGET));
    assert(!status.phase && status.cycling && status.reason == NAT_REASON_BUDGET);
    assert(status.helper_reselections >= 2 && fault.dropped && fault.delayed_length);
    assert(fault.socket->address_profile.mapping == ADDRESS_MAP_REAL_IP &&
           fault.socket->address_profile.inbound == ADDRESS_IN_UNKNOWN);
    uint64_t paused_round = status.round; uint8_t paused_helper[32];
    memcpy(paused_helper, status.helper, 32);
    uint32_t completed_pairs = status.helper_reselections;
    /* Поздняя подлинная проба не завершает уже отменённую попытку. */
    assert(__real_socket_sendto(fault.delayed_fd, fault.delayed, fault.delayed_length,
        (const struct sockaddr *)&fault.delayed_target, fault.delayed_target_length) == (ssize_t)fault.delayed_length);
    test_clock_run_for(fixture.ua, 2000);
    assert(!nat_detection_get_status(owner->nat_det, fault.socket, &status));
    assert(!status.phase && status.cycling && status.round == paused_round && status.helper_reselections == completed_pairs);
    assert(fault.socket->address_profile.inbound == ADDRESS_IN_UNKNOWN);
    fault.mode = FAULT_RESUME_TIMEOUT; fault.delayed_length = 0; end = get_time_tb() + 1300000;
    uint32_t resumed_rounds = 0;
    /* Большой RTT увеличивает ожидание UDP-отчёта за общий предел; отчёты B теряются в сети. */
    struct ETCP_CONN *connection = instance_find_conn(owner, observer->node_id); assert(connection);
    do {
        for (struct ETCP_LINK *link = connection->links; link; link = link->next) link->rtt_last = 60000;
        test_clock_poll_local_io(fixture.ua, 1000); assert(!nat_detection_get_status(owner->nat_det, fault.socket, &status));
        if (status.round != paused_round && !resumed_rounds) {
            assert(!memcmp(status.helper, paused_helper, 32) && status.helper_reselections == completed_pairs);
            resumed_rounds = status.rounds;
        }
    } while (get_time_tb() < end && status.cycling);
    assert(!status.phase && !status.cycling && status.round != paused_round && !memcmp(status.helper, paused_helper, 32));
    assert(status.rounds == resumed_rounds);
    assert(status.helper_reselections == completed_pairs + 1 && fault.socket->address_profile.mapping == ADDRESS_MAP_REAL_IP &&
           fault.socket->address_profile.inbound == ADDRESS_IN_UNKNOWN && status.reason == NAT_REASON_TIMEOUT);
    fault.fixture = NULL; nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT budget pause retains mapping, rejects late ingress and retries the unfinished pair");
}

static void run_single_peer_fallback(enum fault_mode mode) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL, NULL);
    struct UTUN_INSTANCE *peer = nat_fixture_node(&fixture, "127.0.0.3", NULL, NULL);
    nat_detection_set_automatic(peer->nat_det, 0);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "udp");
    nat_fixture_connect(&fixture, owner, "udp", peer);
    struct ETCP_SOCKET *alternate = nat_fixture_add_public_socket(peer, "alternate", "127.0.0.4");
    struct ETCP_SOCKET *third = nat_fixture_add_public_socket(peer, "third", "127.0.0.5");
    assert(topo_node_update_my_addresses(peer) >= 0);
    uint64_t end = get_time_tb() + 180000;
    int published = 0;
    do {
        test_clock_poll(fixture.ua, 10);
        const struct TOPO_NODE *record = topo_node_registry_find(owner->topo_groups, peer->node_id);
        for (const struct TOPO_ADDR4 *addr = record ? record->v4_addrs : NULL; addr; addr = addr->next)
            if (addr->socket_id == third->sock_id && addr->type == TOPO_ADDR_INTERFACE) published = 1;
    } while (!published && get_time_tb() < end);
    assert(published);

    memset(&fault, 0, sizeof(fault));
    fault.fixture = &fixture; fault.owner = owner; fault.observer = peer; fault.socket = socket;
    fault.alias_source = alternate; fault.mode = mode;
    struct nat_check_status status;
    assert(!nat_detection_get_status(owner->nat_det, socket, &status));
    uint32_t previous_rounds = status.rounds;
    assert(!nat_detection_request_check_socket(owner->nat_det, socket, peer->my_ed25519_pubkey));
    end = get_time_tb() + 280000;
    do {
        test_clock_poll(fixture.ua, 10);
        if (mode == FAULT_SHARED_REMOVED && fault.alias_source && fault.dropped) {
            fault.alias_source = NULL;
            etcp_socket_remove(alternate);
        }
        assert(!nat_detection_get_status(owner->nat_det, socket, &status));
    } while (get_time_tb() < end && (status.rounds <= previous_rounds || status.cycling));
    assert(!status.cycling && status.helper_reselections && status.reason == NAT_REASON_NONE);
    assert(!memcmp(status.observer, peer->my_ed25519_pubkey, 32) && !memcmp(status.helper, status.observer, 32));
    assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_OPEN);
    assert(mode == FAULT_SHARED_WRONG_SOURCE ? fault.altered : fault.dropped);
    assert(mode != FAULT_SHARED_REMOVED || !fault.alias_source);
    fault.fixture = NULL; nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT single peer: third IP succeeds after fault=%d", mode);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_NAT, DEBUG_LEVEL_DEBUG);
    test_clock_init(1000);
    run_case(FAULT_NO_PONG, ADDRESS_MAP_REAL_IP, ADDRESS_IN_OPEN);
    run_case(FAULT_FILTERED, ADDRESS_MAP_REAL_IP, ADDRESS_IN_CONTACT);
    run_case(FAULT_LATE_PRE, ADDRESS_MAP_REAL_IP, ADDRESS_IN_CONTACT);
    run_case(FAULT_CONTACT, ADDRESS_MAP_REAL_IP, ADDRESS_IN_UNKNOWN);
    run_case(FAULT_DEPENDENT, ADDRESS_MAP_DEPENDENT, ADDRESS_IN_OPEN);
    run_case(FAULT_UNSTABLE, ADDRESS_MAP_UNKNOWN, ADDRESS_IN_UNKNOWN);
    run_case(FAULT_NO_REPORT, ADDRESS_MAP_UNKNOWN, ADDRESS_IN_UNKNOWN);
    run_case(FAULT_INGRESS_BLOCKED, ADDRESS_MAP_REAL_IP, ADDRESS_IN_UNKNOWN);
    run_case(FAULT_NO_ANCHOR, ADDRESS_MAP_REAL_IP, ADDRESS_IN_UNKNOWN);
    run_case(FAULT_LOCAL_OBSERVER, ADDRESS_MAP_UNKNOWN, ADDRESS_IN_UNKNOWN);
    run_case(FAULT_CONTROL_STALL, ADDRESS_MAP_REAL_IP, ADDRESS_IN_UNKNOWN);
    run_case(FAULT_EIM_UNVERIFIED, ADDRESS_MAP_EIM, ADDRESS_IN_UNKNOWN);
    run_alias("127.0.0.3", FAULT_NONE); run_alias("127.0.0.2", FAULT_NONE);
    run_alias("127.0.0.9", FAULT_LOCAL_INGRESS); run_alias("127.0.0.9", FAULT_LOCAL_HELPER_MAPPING);
    run_alias("127.0.0.9", FAULT_KNOWN_ALIAS);
    run_lan_observation(); run_unknown_address(); run_endpoint_retry(); run_budget_resume();
    run_single_peer_fallback(FAULT_SHARED_UNREACHABLE);
    run_single_peer_fallback(FAULT_SHARED_WRONG_SOURCE);
    run_single_peer_fallback(FAULT_SHARED_REMOVED);
    test_clock_finish(); return 0;
}
