/* Настоящий UDP на loopback, ускоренные монотонные часы и потеря ACK.
 * Подменяется только снимок интерфейсов: тест не объявляет тестовые узлы в пользовательской LAN. */
#include "../src/local_transport/local_transport.h"
#include "../src/local_transport/platform.h"
#include "../src/local_transport/protocol.h"
#include "../src/chat/chat_local.h"
#include "../src/chat/invite_link.h"
#include "../src/chat/chat_setting.h"
#include "../src/utun_instance.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_clock.h"
#include <net/if.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "line=%d failed: %s", __LINE__, #x); exit(1); } } while (0)

static uint32_t loopback_index;
static uint64_t suppress_announcements;
static int drop_ack, announce_count;
static int announcements[5];
static int broadcasts[5], forbidden_acks;
static int ipv6_only, ipv6_messages;
static uint8_t announcement[LOCAL_PACKET_MAX];
static size_t announcement_size;
static struct sockaddr_storage endpoint_a;

int __real_local_interfaces(struct local_interface*, size_t);
int __wrap_local_interfaces(struct local_interface* interfaces, size_t capacity) {
    if (ipv6_only) {
        int count = __real_local_interfaces(interfaces, capacity), used = 0;
        for (int i = 0; i < count; i++) if (interfaces[i].address.ss_family == AF_INET6) interfaces[used++] = interfaces[i];
        return count < 0 ? count : used;
    }
    REQUIRE(capacity > 0);
    memset(interfaces, 0, sizeof(*interfaces));
    interfaces[0].index = loopback_index; interfaces[0].mask4 = htonl(0xff000000);
    struct sockaddr_in* source = (struct sockaddr_in*)&interfaces[0].address;
    source->sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &source->sin_addr);
    struct sockaddr_in* destination = (struct sockaddr_in*)&interfaces[0].broadcast;
    destination->sin_family = AF_INET; destination->sin_port = htons(LOCAL_TRANSPORT_PORT);
    inet_pton(AF_INET, "127.255.255.255", &destination->sin_addr);
    return 1;
}

ssize_t __real_socket_sendto(socket_t, const void*, size_t, const struct sockaddr*, socklen_t);
ssize_t __wrap_socket_sendto(socket_t fd, const void* buffer, size_t len, const struct sockaddr* destination, socklen_t size) {
    const uint8_t* data = buffer;
    if (len >= LOCAL_HEADER_SIZE && !memcmp(data, LOCAL_MAGIC, 16)) {
        uint64_t sender = data[LOCAL_SOURCE_OFF];
        if (data[17] == LOCAL_MESSAGE && destination->sa_family == AF_INET6) ipv6_messages++;
        if (data[17] == LOCAL_ANNOUNCE) {
            announce_count++;
            if (sender < 5) announcements[sender]++;
            unsigned port = destination->sa_family == AF_INET ? ntohs(((const struct sockaddr_in*)destination)->sin_port) :
                ntohs(((const struct sockaddr_in6*)destination)->sin6_port);
            if (sender < 5 && port == LOCAL_TRANSPORT_PORT) broadcasts[sender]++;
            if (sender == 1) {
                memcpy(announcement, data, len); announcement_size = len;
                socklen_t address_len = sizeof(endpoint_a);
                REQUIRE(!getsockname(fd, (struct sockaddr*)&endpoint_a, &address_len));
            }
            if (sender == suppress_announcements) return (ssize_t)len;
        }
        if (data[17] == LOCAL_ACK && len == LOCAL_HEADER_SIZE + 1 && data[LOCAL_HEADER_SIZE] == LOCAL_DELIVERY_FORBIDDEN)
            forbidden_acks++;
        if (data[17] == LOCAL_ACK && drop_ack) { if (drop_ack > 0) drop_ack--; return (ssize_t)len; }
    }
    return __real_socket_sendto(fd, buffer, len, destination, size);
}

struct result { int changes, received, delivered, failed, active; uint64_t request; };
static void changed(void* arg) { ((struct result*)arg)->changes++; }
static int received(void* arg, const struct local_peer* peer, const uint8_t* data, size_t len) {
    struct result* r = arg;
    REQUIRE(!local_address_empty(peer->address.ed) && peer->name[0]);
    REQUIRE(len == 10 && !memcmp(data, "invitation", len)); r->received++;
    return 0;
}
static void delivered(void* arg, uint64_t request, const uint8_t peer[32], int success) {
    struct result* r = arg;
    REQUIRE(!local_address_empty(peer) && request); r->request = request;
    if (success) r->delivered++; else r->failed++;
}
static void activity(void* arg, int active) { ((struct result*)arg)->active = active; }

static size_t peers(struct local_transport* transport, uint64_t id, const char* name) {
    struct local_peer list[LOCAL_TRANSPORT_PEERS_MAX];
    size_t count = local_transport_peers(transport, list, LOCAL_TRANSPORT_PEERS_MAX);
    if (id) {
        int found = 0;
        for (size_t i = 0; i < count; i++) if (!memcmp(list[i].address.ed, (const uint8_t[32]){id}, 32)) { REQUIRE(!strcmp(list[i].name, name)); found = 1; }
        REQUIRE(found);
    }
    return count;
}

static void raw_send(socket_t fd, struct UASYNC* ua, const void* data, size_t len) {
    REQUIRE(__real_socket_sendto(fd, data, len, (struct sockaddr*)&endpoint_a, sizeof(struct sockaddr_in)) == (ssize_t)len);
    test_clock_run_for(ua, 1000);
}

struct chat_result { int invites, delivery, success; uint64_t request; char link[LOCAL_TRANSPORT_DATA_MAX + 1]; };
static void chat_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    struct chat_result* result = inst->test_user_ptr;
    if (type == CHAT_EVT_LOCAL_INVITE) {
        const unsigned header = CHAT_LOCAL_ADDRESS_SIZE + 1;
        REQUIRE(len > header + data[CHAT_LOCAL_ADDRESS_SIZE]); result->invites++;
        size_t size = (size_t)len - header - data[CHAT_LOCAL_ADDRESS_SIZE];
        memcpy(result->link, data + header + data[CHAT_LOCAL_ADDRESS_SIZE], size); result->link[size] = 0;
    }
    if (type == CHAT_EVT_LOCAL_DELIVERY) {
        REQUIRE(len == 8 + CHAT_LOCAL_ADDRESS_SIZE + 1); result->delivery++; result->success = data[8 + CHAT_LOCAL_ADDRESS_SIZE]; result->request = local_get64(data);
    }
}

static void send_invite(struct UTUN_INSTANCE* inst, uint64_t request, const char* link) {
    struct chat_local_send_request* req = u_calloc(1, sizeof(*req)); REQUIRE(req);
    req->inst = inst; req->address.ed[0] = 4; req->request_id = request; strcpy(req->link, link);
    chat_local_send_trampoline(req);
}

static void chat_adapter(struct UASYNC* ua) {
    struct chat_result rc = {0}, rd = {0};
    struct UTUN_INSTANCE c = { .ua = ua, .node_id = 3, .my_ed25519_pubkey = {3}, .name = "Desktop", .client_type = CLIENT_TYPE_DESKTOP,
        .chat_event_handler = chat_event, .test_user_ptr = &rc };
    struct UTUN_INSTANCE d = { .ua = ua, .node_id = 4, .my_ed25519_pubkey = {4}, .name = "Phone", .client_type = CLIENT_TYPE_MOBILE,
        .client_activity = CLIENT_ACTIVITY_ACTIVE,
        .chat_event_handler = chat_event, .test_user_ptr = &rd };
    REQUIRE(chat_setting_get_int(&c, "local_invites_enabled", 1) == 1);
    REQUIRE(chat_setting_get_int(&c, "local_broadcasts_enabled", 1) == 1);
    REQUIRE(!chat_setting_set(&c, "local_broadcasts_enabled", "0"));
    int before_broadcast = broadcasts[3];
    REQUIRE(!chat_local_start(&c) && !chat_local_start(&d));
    test_clock_run_for(ua, 10000);
    REQUIRE(broadcasts[3] == before_broadcast);
    REQUIRE(peers(c.local_transport, 4, "Phone") == 1);
    REQUIRE(!local_transport_set_name(c.local_transport, "Desktop renamed"));
    test_clock_run_for(ua, 110000); REQUIRE(broadcasts[3] == before_broadcast);
    struct InviteData invite = { .join_key = 456, .pubkey = {1}, .addrCount = 1 };
    uint8_t invite_x[32], invite_xpriv[32], invite_seed[32];
    REQUIRE(!sc_identity_generate(invite_x, invite_xpriv, invite.inviter_pubkey, invite_seed));
    invite.addrs[0] = (struct InviteAddr){ .family = 4, .address = {127, 0, 0, 1}, .proto = INVITE_PROTO_UDP, .port = 12345 };
    char link[LOCAL_TRANSPORT_DATA_MAX + 1]; REQUIRE(invite_link_encode(&invite, link, sizeof(link)) > 0);
    send_invite(&c, 501, link); test_clock_run_for(ua, 10000);
    REQUIRE(rd.invites == 1 && !strcmp(rd.link, link) && rc.delivery == 1 && rc.success && rc.request == 501);
    REQUIRE(!c.etcp_sockets && !d.etcp_sockets && !c.connections && !d.connections);
    send_invite(&c, 502, "invalid"); REQUIRE(rc.delivery == 2 && !rc.success && rc.request == 502);
    d.chat_event_handler = NULL;
    send_invite(&c, 503, link); test_clock_run_for(ua, 10000);
    REQUIRE(rc.delivery == 3 && !rc.success && rd.invites == 1);
    d.chat_event_handler = chat_event;
    REQUIRE(!utun_set_client_activity(&d, 0));
    REQUIRE(!peers(d.local_transport, 0, NULL));
    int before = announcements[4]; test_clock_run_for(ua, 110000); REQUIRE(announcements[4] == before);
    REQUIRE(!utun_set_client_activity(&d, 1)); REQUIRE(announcements[4] > before);
    test_clock_run_for(ua, 10000);
    send_invite(&c, 504, link); test_clock_run_for(ua, 10000);
    REQUIRE(rc.delivery == 4 && rc.success && rd.invites == 2);
    REQUIRE(!chat_setting_set(&d, "local_invites_enabled", "0"));
    send_invite(&c, 505, link); test_clock_run_for(ua, 40000);
    REQUIRE(rc.delivery == 5 && rc.success == LOCAL_DELIVERY_FORBIDDEN && rc.request == 505 && rd.invites == 2);
    REQUIRE(forbidden_acks > 0);
    REQUIRE(!chat_setting_set(&d, "local_invites_enabled", "1"));
    REQUIRE(chat_setting_set(&d, "local_invites_enabled", "invalid") < 0);
    send_invite(&c, 506, link); test_clock_run_for(ua, 10000);
    REQUIRE(rc.delivery == 6 && rc.success == LOCAL_DELIVERY_RECEIVED && rd.invites == 3);
    REQUIRE(!chat_setting_set(&c, "local_invites_enabled", "0"));
    send_invite(&c, 507, link); test_clock_run_for(ua, 10000);
    REQUIRE(rc.delivery == 7 && rc.success == LOCAL_DELIVERY_RECEIVED && rd.invites == 4);
    REQUIRE(broadcasts[3] == before_broadcast);
    REQUIRE(!local_transport_send(d.local_transport, (const uint8_t[32]){3}, 508, link, strlen(link)));
    test_clock_run_for(ua, 10000);
    REQUIRE(rd.delivery == 1 && rd.success == LOCAL_DELIVERY_FORBIDDEN && rc.invites == 0);
    REQUIRE(!chat_setting_set(&c, "local_broadcasts_enabled", "1"));
    REQUIRE(broadcasts[3] > before_broadcast);
    chat_local_stop(&c); chat_local_stop(&d);
    REQUIRE(!c.local_transport && !d.local_transport && !c.activity_cbks && !d.activity_cbks);
}

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    test_clock_init(10000);
    const char config_text[] = "[chatserver]\nlocal_invites_enabled=no\nlocal_broadcasts_enabled=false\n";
    struct utun_config* config = parse_config_from_buf(config_text, sizeof(config_text) - 1, "local-permissions-test");
    REQUIRE(config);
    REQUIRE(!chat_setting_get_int_state(&config->global.chat_settings, "local_invites_enabled", 1));
    REQUIRE(!chat_setting_get_int_state(&config->global.chat_settings, "local_broadcasts_enabled", 1));
    free_config(config);
    ipv6_only = argc == 2 && !strcmp(argv[1], "--ipv6");
    loopback_index = if_nametoindex("lo"); REQUIRE(loopback_index);
    struct UASYNC* ua = uasync_create(); REQUIRE(ua);
    struct result ra = {0}, rb = {0};
    struct local_transport_callbacks callbacks = { changed, received, delivered, activity };
    struct local_transport* a = local_transport_create(ua, (const uint8_t[32]){1}, "Same nick", &callbacks, &ra);
    struct local_transport* b = local_transport_create(ua, (const uint8_t[32]){2}, "Same nick", &callbacks, &rb);
    REQUIRE(a && b);
    REQUIRE(!local_transport_set_active(a, 1) && !local_transport_set_active(b, 1));
    test_clock_run_for(ua, 10000);
    REQUIRE(peers(a, 2, "Same nick") == 1 && peers(b, 1, "Same nick") == 1);
    REQUIRE(ra.active && rb.active && announce_count >= 4);
    REQUIRE(!local_transport_set_name(b, "Телефон"));
    test_clock_run_for(ua, 10000);
    REQUIRE(peers(a, 2, "Телефон") == 1);

    /* Потеря ACK приводит к повтору UDP, но callback получателя выполняется один раз. */
    drop_ack = 1;
    REQUIRE(!local_transport_send(a, (const uint8_t[32]){2}, 101, "invitation", 10));
    REQUIRE(local_transport_send(a, (const uint8_t[32]){2}, 101, "invitation", 10) < 0);
    test_clock_run_for(ua, 30000);
    REQUIRE(rb.received == 1 && ra.delivered == 1 && !ra.failed && ra.request == 101);
    REQUIRE(local_transport_send(a, (const uint8_t[32]){99}, 102, "invitation", 10) < 0);
    REQUIRE(local_transport_send(a, (const uint8_t[32]){2}, 0, "invitation", 10) < 0);
    REQUIRE(local_transport_send(a, (const uint8_t[32]){2}, 102, "invitation", LOCAL_TRANSPORT_DATA_MAX + 1) < 0);

    /* Чужой трафик, усечённые заголовки, длины, версии, UTF-8 и собственные объявления. */
    socket_t raw = SOCKET_INVALID;
    if (!ipv6_only) {
        raw = socket_create_udp(AF_INET); REQUIRE(raw != SOCKET_INVALID);
        uint8_t bad[LOCAL_PACKET_MAX + 1];
        REQUIRE(announcement_size > LOCAL_HEADER_SIZE);
        memcpy(bad, announcement, announcement_size); bad[LOCAL_SOURCE_OFF] = 3;
        size_t size = announcement_size;
        raw_send(raw, ua, "foreign UDP", 11);
        raw_send(raw, ua, bad, 16);
        bad[16] = 1; raw_send(raw, ua, bad, size); bad[16] = LOCAL_VERSION;
        raw_send(raw, ua, bad, size - 1);
        bad[LOCAL_HEADER_SIZE] = 0; raw_send(raw, ua, bad, size);
        bad[LOCAL_HEADER_SIZE] = 0xff; raw_send(raw, ua, bad, size);
        memset(bad, 0, sizeof(bad)); memcpy(bad, LOCAL_MAGIC, 16); raw_send(raw, ua, bad, sizeof(bad));
        REQUIRE(peers(a, 2, "Телефон") == 1 && !ra.received);
    }
    REQUIRE(!local_name_valid((const uint8_t*)"\xc0\x80", 2));
    REQUIRE(!local_name_valid((const uint8_t*)"\xed\xa0\x80", 3));
    REQUIRE(!local_name_valid((const uint8_t*)"\xf4\x90\x80\x80", 4));
    REQUIRE(!local_name_valid((const uint8_t*)"name\n", 5));
    REQUIRE(local_name_valid((const uint8_t*)"Телефон", strlen("Телефон")));

    /* Только ANNOUNCE продлевает TTL: последний ACK не удерживает устройство в таблице. */
    REQUIRE(!local_transport_set_name(b, "TTL"));
    test_clock_run_for(ua, 1000);
    suppress_announcements = 2;
    test_clock_run_for(ua, 260000);
    REQUIRE(!local_transport_send(a, (const uint8_t[32]){2}, 103, "invitation", 10));
    test_clock_run_for(ua, 10000);
    REQUIRE(ra.delivered == 2 && rb.received == 2);
    test_clock_run_for(ua, 50000);
    REQUIRE(peers(a, 0, NULL) == 0);
    suppress_announcements = 0;
    REQUIRE(!local_transport_set_name(b, "Back"));
    test_clock_run_for(ua, 10000); REQUIRE(peers(a, 2, "Back") == 1);

    /* Недоступный ACK даёт один failure; остановка отменяет запрос и удаляет все ресурсы. */
    drop_ack = -1;
    REQUIRE(!local_transport_send(a, (const uint8_t[32]){2}, 104, "invitation", 10));
    test_clock_run_for(ua, 50000);
    REQUIRE(ra.failed == 1 && ra.request == 104 && rb.received == 3);
    REQUIRE(!local_transport_send(a, (const uint8_t[32]){2}, 105, "invitation", 10));
    REQUIRE(!local_transport_set_active(a, 0));
    REQUIRE(!ra.active && ra.failed == 2 && ra.request == 105 && !peers(a, 0, NULL));
    REQUIRE(!local_transport_set_active(b, 0));
    uasync_poll(ua, 0); // uasync освобождает отменённые heap nodes на следующем проходе
    size_t alloc, freed, sockets, closed;
    uasync_get_stats(ua, &alloc, &freed, &sockets, &closed);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "after stop: timers=%zu/%zu sockets=%zu/%zu", alloc, freed, sockets, closed);
    REQUIRE(alloc == freed && sockets == closed);
    int before = announce_count; test_clock_run_for(ua, 110000); REQUIRE(announce_count == before);
    REQUIRE(!local_transport_set_active(a, 1)); REQUIRE(announce_count > before);
    local_transport_destroy(a); local_transport_destroy(b);
    if (raw != SOCKET_INVALID) socket_close_wrapper(raw);
    drop_ack = 0;
    chat_adapter(ua);
    REQUIRE(!ipv6_only || ipv6_messages > 0);
    uasync_destroy(ua, 0); test_clock_finish();
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: UDP discovery/delivery, ACK retry/dedup, TTL, malformed packets, activity and teardown");
    return 0;
}
