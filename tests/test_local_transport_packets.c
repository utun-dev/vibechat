/* Повреждённые кодограммы и flood проходят штатный socket callback и chat-адаптер.
 * Подменён recvmsg boundary: воспроизводимый корпус без потерь UDP и без рассылки в пользовательскую LAN.
 * Реальные recvmsg, усечение UDP и IPv6 дополнительно проверяет test_local_transport. */
#include "../src/local_transport/local_transport.h"
#include "../src/local_transport/platform.h"
#include "../src/local_transport/protocol.h"
#include "../src/chat/chat_local.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/invite_link.h"
#include "../src/utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_clock.h"
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line=%d failed: %s\n", __LINE__, #x); exit(1); } } while (0)

struct test_socket { socket_t fd; socket_t_callback_t callback; void* arg; void* handle; };
static struct test_socket sockets[3];
static int socket_count, receiving, consumed, changes, invitations, log_lines, wire_packets;
static uint32_t interface_index, random_state = 0x938d26af;
static uint64_t own_epoch;
static uint8_t input[65507];
static size_t input_size;
static struct sockaddr_storage source;

int __wrap_local_interfaces(struct local_interface* interfaces, size_t capacity) {
    REQUIRE(capacity > 0);
    memset(interfaces, 0, sizeof(*interfaces));
    interfaces->index = interface_index; interfaces->mask4 = htonl(0xff000000);
    struct sockaddr_in* address = (struct sockaddr_in*)&interfaces->address;
    address->sin_family = AF_INET; address->sin_addr.s_addr = htonl(0x7f000001);
    struct sockaddr_in* broadcast = (struct sockaddr_in*)&interfaces->broadcast;
    broadcast->sin_family = AF_INET; broadcast->sin_addr.s_addr = htonl(0x7fffffff);
    broadcast->sin_port = htons(LOCAL_TRANSPORT_PORT);
    return 1;
}

void* __real_uasync_add_socket_t(struct UASYNC*, socket_t, socket_t_callback_t, socket_t_callback_t,
                               socket_t_callback_t, const char*, void*);
void* __wrap_uasync_add_socket_t(struct UASYNC* ua, socket_t fd, socket_t_callback_t read, socket_t_callback_t write,
                               socket_t_callback_t error, const char* name, void* arg) {
    void* handle = __real_uasync_add_socket_t(ua, fd, read, write, error, name, arg);
    if (handle && name && !strcmp(name, "local_transport")) {
        REQUIRE(socket_count < 3); sockets[socket_count++] = (struct test_socket){fd, read, arg, handle};
    }
    return handle;
}

ssize_t __real_local_receive(socket_t, void*, size_t, struct sockaddr_storage*, uint32_t*);
ssize_t __wrap_local_receive(socket_t fd, void* data, size_t capacity, struct sockaddr_storage* address, uint32_t* index) {
    if (!receiving) {
        ssize_t result = __real_local_receive(fd, data, capacity, address, index);
        if (result >= 0 || result == -2) wire_packets++;
        return result;
    }
    if (consumed) { errno = EAGAIN; return -1; }
    consumed = 1; *address = source; *index = interface_index;
    size_t size = input_size < capacity ? input_size : capacity;
    memcpy(data, input, size);
    return input_size > capacity ? -2 : (ssize_t)input_size;
}

ssize_t __wrap_socket_sendto(socket_t fd, const void* buffer, size_t len, const struct sockaddr* address, socklen_t address_size) {
    (void)fd; (void)address; (void)address_size;
    const uint8_t* data = buffer;
    REQUIRE(len >= LOCAL_HEADER_SIZE && len <= LOCAL_PACKET_MAX && !memcmp(data, LOCAL_MAGIC, 16));
    REQUIRE(!memcmp(data + LOCAL_SOURCE_OFF, (const uint8_t[32]){1}, 32));
    if (data[17] == LOCAL_ANNOUNCE) own_epoch = local_get64(data + LOCAL_SOURCE_EPOCH_OFF);
    return (ssize_t)len;
}

#define DEVICE_HEADER (CHAT_LOCAL_ADDRESS_SIZE + 1)

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    REQUIRE(data && len > 0);
    if (type == CHAT_EVT_LOCAL_DEVICES) {
        REQUIRE(len >= 2 && len <= 2 + LOCAL_TRANSPORT_PEERS_MAX * (DEVICE_HEADER + LOCAL_TRANSPORT_NAME_MAX - 1));
        size_t count = (size_t)data[0] << 8 | data[1], offset = 2;
        REQUIRE(count <= LOCAL_TRANSPORT_PEERS_MAX);
        for (size_t i = 0; i < count; i++) {
            REQUIRE(offset + DEVICE_HEADER <= (size_t)len);
            size_t size = data[offset + CHAT_LOCAL_ADDRESS_SIZE]; offset += DEVICE_HEADER;
            REQUIRE(offset + size <= (size_t)len && local_name_valid(data + offset, size)); offset += size;
        }
        REQUIRE(offset == (size_t)len); changes++;
    }
    if (type == CHAT_EVT_LOCAL_INVITE) {
        REQUIRE(len > DEVICE_HEADER + data[CHAT_LOCAL_ADDRESS_SIZE] && len <= DEVICE_HEADER + LOCAL_TRANSPORT_NAME_MAX + LOCAL_TRANSPORT_DATA_MAX);
        struct InviteData invite = {0};
        REQUIRE(!invite_link_decode((const char*)data + DEVICE_HEADER + data[CHAT_LOCAL_ADDRESS_SIZE], (size_t)len - DEVICE_HEADER - data[CHAT_LOCAL_ADDRESS_SIZE], &invite, NULL, 0));
        REQUIRE(invite.join_key && invite.addrCount); invitations++;
    }
}

static void log_hook(int level, const char* category, const char* message) {
    (void)level; (void)category; (void)message; log_lines++;
}

static uint32_t random32(void) {
    uint32_t value = random_state;
    value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    return random_state = value;
}

static void packet(uint8_t type, uint64_t peer, uint64_t request, const void* payload, size_t size) {
    REQUIRE(size <= sizeof(input) - LOCAL_HEADER_SIZE);
    memset(input, 0, LOCAL_HEADER_SIZE); memcpy(input, LOCAL_MAGIC, 16);
    input[16] = LOCAL_VERSION; input[17] = type; input[18] = (uint8_t)(size >> 8); input[19] = (uint8_t)size;
    local_put64(input + LOCAL_SOURCE_OFF, peer); local_put64(input + LOCAL_SOURCE_EPOCH_OFF, 100);
    if (type != LOCAL_ANNOUNCE) {
        input[LOCAL_DESTINATION_OFF] = 1; local_put64(input + LOCAL_DESTINATION_EPOCH_OFF, own_epoch); local_put64(input + LOCAL_REQUEST_OFF, request);
    }
    if (size) memcpy(input + LOCAL_HEADER_SIZE, payload, size);
    input_size = LOCAL_HEADER_SIZE + size;
}

static int inject(struct test_socket* socket) {
    receiving = 1; consumed = 0;
    socket->callback(socket->fd, socket->arg);
    receiving = 0;
    return consumed;
}

static void next_window(struct UASYNC* ua) { test_clock_next_timer(ua); }
static void heartbeat(void* arg) { (*(int*)arg)++; }

static void wire_boundaries(struct UASYNC* ua, struct test_socket* unicast) {
    socket_t raw = socket_create_udp(AF_INET); REQUIRE(raw != SOCKET_INVALID);
    int enabled = 1;
    REQUIRE(!setsockopt(raw, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)));
    struct sockaddr_in broadcast = { .sin_family = AF_INET, .sin_port = htons(LOCAL_TRANSPORT_PORT),
        .sin_addr.s_addr = htonl(0x7fffffff) };
    struct sockaddr_storage endpoint; socklen_t size = sizeof(endpoint);
    REQUIRE(!getsockname(unicast->fd, (struct sockaddr*)&endpoint, &size));
    const size_t sizes[] = {LOCAL_PACKET_MAX, LOCAL_PACKET_MAX + 1, sizeof(input)};
    packet(LOCAL_ANNOUNCE, 2, 0, "Peer", 4);
    int before = invitations;
    /* Необёрнутый sendto проверяет настоящий recvmsg, нулевые/короткие UDP и MSG_TRUNC, включая 65507 байт. */
    for (size_t i = 0; i < LOCAL_HEADER_SIZE + 1 + sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t len = i <= LOCAL_HEADER_SIZE ? i : sizes[i - LOCAL_HEADER_SIZE - 1];
        REQUIRE(sendto(raw, input, len, 0, (struct sockaddr*)&broadcast, sizeof(broadcast)) == (ssize_t)len);
        REQUIRE(sendto(raw, input, len, 0, (struct sockaddr*)&endpoint, size) == (ssize_t)len);
        uasync_poll(ua, 0);
    }
    REQUIRE(invitations == before);
    next_window(ua);
    int beat = 0, packets_before = wire_packets;
    REQUIRE(uasync_set_timeout(ua, 1, &beat, heartbeat, "hostile_udp_heartbeat"));
    uint64_t start = get_time_tb();
    for (int i = 0; i < 10000; i++) {
        REQUIRE(sendto(raw, input, 16, 0, (struct sockaddr*)&broadcast, sizeof(broadcast)) == 16);
        if (!(i % 32)) uasync_poll(ua, 0);
    }
    test_clock_run_for(ua, 100);
    REQUIRE(beat == 1 && invitations == before);
    REQUIRE((uint64_t)(wire_packets - packets_before) <= 256 * ((get_time_tb() - start) / 10000 + 2));
    socket_close_wrapper(raw); next_window(ua);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    debug_enable_console(0); debug_set_log_hook(log_hook);
    test_clock_init(10000);
    interface_index = if_nametoindex("lo"); REQUIRE(interface_index);
    struct sockaddr_in* address = (struct sockaddr_in*)&source;
    address->sin_family = AF_INET; address->sin_addr.s_addr = htonl(0x7f000002); address->sin_port = htons(55000);
    struct UASYNC* ua = uasync_create(); REQUIRE(ua);
    struct UTUN_INSTANCE inst = { .ua = ua, .node_id = 1, .my_ed25519_pubkey = {1}, .name = "", .client_type = CLIENT_TYPE_DESKTOP,
        .chat_event_handler = event };
    REQUIRE(!chat_local_start(&inst) && socket_count == 3 && own_epoch);
    struct test_socket *unicast = NULL, *discovery = NULL;
    for (int i = 0; i < socket_count; i++) {
        struct sockaddr_storage bound; socklen_t size = sizeof(bound);
        REQUIRE(!getsockname(sockets[i].fd, (struct sockaddr*)&bound, &size));
        if (bound.ss_family != AF_INET) continue;
        if (ss_get_port(&bound) == LOCAL_TRANSPORT_PORT) discovery = &sockets[i]; else unicast = &sockets[i];
    }
    REQUIRE(unicast && discovery);
    struct InviteData invite = { .join_key = 456, .pubkey = {1}, .addrCount = 1 };
    uint8_t invite_x[32], invite_xpriv[32], invite_seed[32];
    REQUIRE(!sc_identity_generate(invite_x, invite_xpriv, invite.inviter_pubkey, invite_seed));
    invite.addrs[0] = (struct InviteAddr){ .family = 4, .address = {127, 0, 0, 1}, .proto = INVITE_PROTO_UDP, .port = 12345 };
    char link[LOCAL_TRANSPORT_DATA_MAX + 1]; REQUIRE(invite_link_encode(&invite, link, sizeof(link)) > 0);

    /* На discovery-порт нельзя доставить сообщение/ACK даже от ранее объявленного устройства. */
    packet(LOCAL_ANNOUNCE, 2, 0, "Peer", 4); REQUIRE(inject(discovery));
    packet(LOCAL_MESSAGE, 2, 1, link, strlen(link)); REQUIRE(inject(discovery)); REQUIRE(!invitations);
    packet(LOCAL_MESSAGE, 2, 1, link, strlen(link)); REQUIRE(inject(unicast)); REQUIRE(invitations == 1);
    packet(LOCAL_MESSAGE, 2, 1, link, strlen(link)); REQUIRE(inject(unicast)); REQUIRE(invitations == 1);
    /* Один endpoint и request/epoch, два ключа с одинаковыми первыми 8 байтами. */
    packet(LOCAL_ANNOUNCE, 2, 0, "Second", 6); input[LOCAL_SOURCE_OFF + 31] = 1; REQUIRE(inject(discovery));
    packet(LOCAL_MESSAGE, 2, 1, link, strlen(link)); input[LOCAL_SOURCE_OFF + 31] = 1;
    REQUIRE(inject(unicast)); REQUIRE(invitations == 2);
    REQUIRE(inject(unicast)); REQUIRE(invitations == 2);
    next_window(ua);

    int before = invitations;
    for (int i = 0; i < 64; i++) {
        packet(LOCAL_MESSAGE, 2, 100 + i, link, strlen(link)); REQUIRE(inject(unicast));
    }
    REQUIRE(invitations - before == 8);
    next_window(ua);

    /* Flood не вызывает ни busy-loop, ни отдельного события/лога на каждое объявление. */
    int processed = 0, changes_before = changes, logs_before = log_lines;
    for (int i = 0; i < 10000; i++) {
        packet(LOCAL_ANNOUNCE, 2, 0, i & 1 ? "Peer A" : "Peer B", 6);
        processed += inject(discovery);
    }
    if (processed != 256 || changes != changes_before || log_lines != logs_before)
        fprintf(stderr, "local flood: processed=%d/256 changes=%d logs=%d real_udp=%d\n",
                processed, changes - changes_before, log_lines - logs_before, wire_packets);
    REQUIRE(processed == 256 && changes == changes_before && log_lines == logs_before);
    next_window(ua); REQUIRE(changes == changes_before + 1 && log_lines - logs_before <= 10);
    packet(LOCAL_MESSAGE, 2, 999, link, strlen(link)); REQUIRE(inject(unicast)); REQUIRE(invitations == before + 9);

    /* Исчерпание таблиц узлов и endpoints остаётся внутри предвыделенной памяти. */
    next_window(ua);
    for (unsigned i = 0; i < 160; i++) {
        packet(LOCAL_ANNOUNCE, i + 2, 0, "Peer", 4); REQUIRE(inject(discovery));
    }
    struct local_peer peers[LOCAL_TRANSPORT_PEERS_MAX];
    REQUIRE(local_transport_peers(inst.local_transport, peers, LOCAL_TRANSPORT_PEERS_MAX) == LOCAL_TRANSPORT_PEERS_MAX);
    for (int i = 0; i < 16; i++) {
        address->sin_port = htons(55000 + i);
        packet(LOCAL_ANNOUNCE, 2, 0, "Peer", 4); REQUIRE(inject(discovery));
    }
    address->sin_port = htons(55000); next_window(ua);
    wire_boundaries(ua, unicast);
    /* Дальше корпус подаётся вручную: настоящие UDP параллельных тестов не должны тратить его receive budget. */
    for (int i = 0; i < socket_count; i++) REQUIRE(!uasync_set_socket_read(ua, sockets[i].handle, 0));

    size_t allocated = u_get_allocated_count();
    unsigned long cases = 0;
    debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_NONE);
    uint8_t payload[LOCAL_TRANSPORT_DATA_MAX];
    /* Миллион воспроизводимых мутаций: все длины заголовка, версии/типы/ID, UTF-8, тела invite и ACK. */
    for (unsigned i = 0; i < 1000000; i++) {
        if (!(i % 128)) test_clock_next_timer_local_io(ua);
        uint32_t mode = random32() % 8;
        size_t size = random32() % (sizeof(payload) + 1);
        for (size_t j = 0; j < size; j++) payload[j] = (uint8_t)random32();
        packet(mode < 3 ? LOCAL_ANNOUNCE : mode < 6 ? LOCAL_MESSAGE : LOCAL_ACK, 2, i + 1000, payload, size);
        if (mode == 0) input_size = i % (LOCAL_HEADER_SIZE + 1);
        if (mode == 1) {
            for (unsigned j = 0, n = random32() % 8 + 1; j < n; j++) input[random32() % input_size] ^= (uint8_t)random32();
        }
        if (mode == 2) { local_put64(input + LOCAL_SOURCE_OFF, random32() % 160 + 2); local_put64(input + LOCAL_SOURCE_EPOCH_OFF, random32()); }
        if (mode == 3 && size >= 7) memcpy(input + LOCAL_HEADER_SIZE, "utun://", 7);
        if (mode == 4) {
            packet(LOCAL_MESSAGE, 2, i + 1000, link, strlen(link));
            for (unsigned j = 0, n = random32() % 8 + 1; j < n; j++)
                input[LOCAL_HEADER_SIZE + random32() % strlen(link)] ^= (uint8_t)random32();
        }
        if (mode == 5) { local_put64(input + LOCAL_DESTINATION_OFF, random32()); local_put64(input + LOCAL_DESTINATION_EPOCH_OFF, random32()); }
        if (mode == 6) { input_size = random32() % sizeof(input); }
        if (mode == 7) { input[18] = (uint8_t)random32(); input[19] = (uint8_t)random32(); }
        REQUIRE(inject(i & 1 ? unicast : discovery)); cases++;
        /* Сохраняем достижимость MESSAGE/ACK после мутаций epoch/TTL и смены ника. */
        if (!(i % 32)) { packet(LOCAL_ANNOUNCE, 2, 0, "Peer", 4); REQUIRE(inject(discovery)); }
    }
    test_clock_next_timer_local_io(ua);
    REQUIRE(u_get_allocated_count() <= allocated + 2);
    debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_WARN);
    packet(LOCAL_ANNOUNCE, 2, 0, "Peer", 4); REQUIRE(inject(discovery));
    before = invitations;
    packet(LOCAL_MESSAGE, 2, 2000000, link, strlen(link)); REQUIRE(inject(unicast)); REQUIRE(invitations == before + 1);
    chat_local_stop(&inst); uasync_poll(ua, 0);
    size_t timers, timers_freed, registered, removed;
    uasync_get_stats(ua, &timers, &timers_freed, &registered, &removed);
    REQUIRE(timers == timers_freed && registered == removed);
    uasync_destroy(ua, 0);
    debug_set_log_hook(NULL); debug_enable_console(1);
    test_clock_finish();
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: %lu hostile packets, bounded flood/events/memory, discovery isolation and recovery", cases);
    return 0;
}
