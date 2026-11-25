#include "../src/group/group_protocol.h"
/* Реальный UDP/JOIN: авторизация сервера, несколько каналов и подтверждение именно сервером. */
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_channel.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/chat_sync.h"
#include "../src/chat/chat_setting.h"
#include "../src/chat/invite_build.h"
#include "../src/chat/invite_link.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/etcp_api.h"
#include "test_clock.h"
#include "test_join_wire.h"
#include "test_net.h"
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "server join test line=%d: %s", __LINE__, #x); abort(); } } while (0)
static struct UTUN_INSTANCE *client, *server;
static unsigned events[32], info_requests, server_requests;
static int results[32], hold_ready, hold_result, hold_request, hold_offer;
static uint8_t ready_wire[512], result_wire[64], request_wire[INVITE_LINK_MAX + 64];
static size_t ready_size, result_size, request_size, offer_size;
static uint8_t offer_wire[CHAT_JOIN_WIRE_MAX];

int __real_etcp_send(struct ETCP_CONN*, struct ll_entry*);
int __wrap_etcp_send(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (entry->len >= 18 && entry->dgram[0] == ETCP_ID_CHAT_SYNC) {
        unsigned type = entry->dgram[CHAT_SYNC_TYPE_OFF];
        if (type == CS_MSG_JOIN_RELAY) type = test_join_open(conn->instance, entry->dgram, entry->len, NULL, NULL);
        if (type == CS_MSG_JOIN_INFO_REQ && conn->instance == server) info_requests++;
        uint8_t* buffer = NULL;
        if (type == CS_MSG_SERVER_JOIN_REQ) {
            server_requests++;
            CHECK(entry->len <= sizeof(request_wire)); request_size = entry->len;
            memcpy(request_wire, entry->dgram, request_size);
            if (hold_request) buffer = request_wire;
        } else if (type == CS_MSG_CHANNEL_INVITE && conn->instance == client && hold_offer) {
            CHECK(entry->len <= sizeof(offer_wire)); offer_size = entry->len; buffer = offer_wire;
        } else if (type == CS_MSG_JOIN_READY && hold_ready && conn->instance == client) {
            CHECK(entry->len <= sizeof(ready_wire)); ready_size = entry->len; buffer = ready_wire;
        } else if (type == CS_MSG_SERVER_JOIN_RESULT && hold_result) {
            CHECK(entry->len <= sizeof(result_wire)); result_size = entry->len; buffer = result_wire;
        }
        if (buffer) {
            memcpy(buffer, entry->dgram, entry->len);
            queue_dgram_free(entry); queue_entry_free(entry); return 0;
        }
    }
    return __real_etcp_send(conn, entry);
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (inst != client || type != CHAT_EVT_CONNECT_RESULT) return;
    CHECK(len == CHAT_CONNECT_RESULT_SIZE);
    uint64_t request; int32_t result;
    memcpy(&request, data + CHAT_CONNECT_RESULT_REQUEST_OFF, 8); memcpy(&result, data + CHAT_CONNECT_RESULT_CODE_OFF, 4);
    CHECK(request < 32); events[request]++; results[request] = result;
}

static void deliver(struct UTUN_INSTANCE* target, struct UTUN_INSTANCE* source, const uint8_t* bytes, size_t size) {
    struct ETCP_CONN* conn = instance_find_conn(target, source->node_id); CHECK(conn);
    struct ll_entry* entry = ll_alloc_lldgram(size); CHECK(entry);
    memcpy(entry->dgram, bytes, size); entry->len = (uint16_t)size;
    target->api_bindings.callbacks[ETCP_ID_CHAT_SYNC](conn, entry);
}

static void wait_value(struct UASYNC* ua, const unsigned* value) {
    uint64_t deadline = get_time_tb() + 400000;
    while (!*value && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(*value);
}

static int has_channel(uint64_t channel) {
    const char* name = topo_groups_channel(client->topo_groups, channel);
    return topo_node_sqlite_channel_get(server->topo_sqlite_db, name, NULL, 0, NULL, NULL, NULL, NULL) == 0;
}

static void wait_packet(struct UASYNC* ua, const size_t* size) {
    uint64_t deadline = get_time_tb() + 100000;
    while (!*size && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(*size);
}

/* Новый клиент известен отправителю только по Ed25519 и переданному UDP-адресу. */
static void test_address_invite(struct UASYNC* ua, uint64_t group) {
    struct UTUN_INSTANCE* guest = utun_instance_create_from_str(ua, "[global]\nmy_node_name=address invite guest\n[ntp]\nenabled=no\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=yes\n");
    CHECK(guest && !utun_core_start(guest) && !chat_service_start(guest));
    CHECK(!chat_setting_set(guest, "join_policy", "0"));
    CHECK(!instance_find_conn_by_key(client, guest->my_keys.public_key));
    struct InviteAddr address = {.family = 4, .address = {127, 0, 0, 1}, .proto = INVITE_PROTO_UDP,
        .port = ss_get_port(&test_etcp_bound_socket(guest, "udp")->local_addr)};
    CHECK(!chat_sync_invite_to_channel(client, group, guest->my_ed25519_pubkey, &address));
    const char* channel = topo_groups_channel(client->topo_groups, group);
    uint64_t deadline = get_time_tb() + 100000;
    uint64_t slot = 0;
    while (get_time_tb() < deadline) {
        slot = topo_groups_channel_slot(guest->topo_groups, channel);
        if (slot && topo_group_chat_key_allowed(guest, slot, guest->my_ed25519_pubkey)) break;
        test_clock_poll(ua, 100);
    }
    CHECK(slot && topo_group_chat_key_allowed(guest, slot, guest->my_ed25519_pubkey));
    CHECK(topo_group_chat_key_allowed(client, group, guest->my_ed25519_pubkey));
    utun_instance_destroy(guest);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS address invitation connected to a fresh peer and published admission on both nodes");
}

int main(void) {
    debug_config_init(); debug_set_level(getenv("UTUN_TEST_DEBUG") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
    test_clock_init(1000);
    size_t baseline = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); CHECK(ua); utun_instance_set_tun_init_enabled(0);
    const char* base = "[global]\nmy_node_name=server-join-client\n[ntp]\nenabled=no\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=yes\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n";
    client = utun_instance_create_from_str(ua, base);
    char config[1024]; snprintf(config, sizeof(config), "%sadd_key=%s\n", base,
        "5757575757575757575757575757575757575757575757575757575757575757");
    server = utun_instance_create_from_str(ua, config); CHECK(client && server);
    CHECK(!utun_core_start(client) && !chat_service_start(client));
    CHECK(!utun_core_start(server) && !chat_service_start(server));
    chat_event_set_handler(client, event);
    uint64_t first = chat_core_create_channel_auto(client, "first"); CHECK(first);
    uint64_t second = chat_core_create_channel_auto(client, "second"); CHECK(second && second != first);
    uint64_t third = chat_core_create_channel_auto(client, "third"); CHECK(third);
    char first_key[65]; strcpy(first_key, topo_groups_channel(client->topo_groups, first));
    char link[INVITE_LINK_MAX], wrong[INVITE_LINK_MAX]; struct InviteData target;
    CHECK(!chat_server_link_build(server, link, sizeof(link)));
    CHECK(!invite_link_decode(link, strlen(link), &target, NULL, 0));
    CHECK(target.kind == INVITE_KIND_SERVER && target.addrCount && target.addrs[0].port);
    CHECK(chat_sync_join_link(client, 1, link, strlen(link)) == CHAT_CONNECT_INVALID && !events[1]);
    target.add_key[0] ^= 1; CHECK(invite_link_encode(&target, wrong, sizeof(wrong)) > 0);
    CHECK(!chat_sync_offer_link(client, 2, first, wrong, strlen(wrong))); wait_value(ua, &events[2]);
    CHECK(events[2] == 1 && results[2] == CHAT_CONNECT_SERVER_KEY && !has_channel(first) && !info_requests);

    hold_ready = hold_result = 1;
    CHECK(!chat_sync_offer_link(client, 3, first, link, strlen(link)));
    wait_packet(ua, &ready_size); CHECK(!events[3] && !result_size && has_channel(first) && info_requests);
    /* Инвайтер уже сохранил S, но S ещё не получил JOIN_READY. */
    hold_ready = 0; deliver(server, client, ready_wire, ready_size);
    wait_packet(ua, &result_size); CHECK(!events[3]);
    uint8_t stale[64]; memcpy(stale, result_wire, result_size); stale[CHAT_SYNC_SCOPE_OFF] ^= 1;
    deliver(client, server, stale, result_size); CHECK(!events[3]);
    memcpy(stale, result_wire, result_size); stale[CHAT_SYNC_GROUP_SIZE] ^= 1;
    deliver(client, server, stale, result_size); CHECK(!events[3]);
    hold_result = 0; deliver(client, server, result_wire, result_size);
    CHECK(events[3] == 1 && !results[3]);
    deliver(client, server, result_wire, result_size); CHECK(events[3] == 1);

    /* Обычный участник получает локальный отказ в праве приглашать, а не ошибку серверного ключа. */
    char client_link[INVITE_LINK_MAX]; CHECK(!chat_server_link_build(client, client_link, sizeof(client_link)));
    unsigned before_denied = server_requests;
    CHECK(chat_sync_offer_link(server, 16, topo_groups_channel_slot(server->topo_groups, first_key), client_link, strlen(client_link)) == CHAT_CONNECT_INVITE_DENIED);
    CHECK(server_requests == before_denied);
    CHECK(chat_sync_offer_link(server, 17, INT64_MAX, client_link, strlen(client_link)) == CHAT_CONNECT_ERROR);
    CHECK(server_requests == before_denied);

    CHECK(!chat_sync_offer_link(client, 4, second, link, strlen(link))); wait_value(ua, &events[4]);
    CHECK(events[4] == 1 && !results[4] && has_channel(first) && has_channel(second));
    CHECK(!chat_sync_offer_link(client, 5, first, link, strlen(link))); wait_value(ua, &events[5]);
    CHECK(events[5] == 1 && !results[5]);

    /* Ротация: старая ссылка не добавляет новый канал; существующие сохраняются. */
    server->config->global.chatserver_add_key[0] ^= 1;
    CHECK(!chat_sync_offer_link(client, 6, third, link, strlen(link))); wait_value(ua, &events[6]);
    CHECK(results[6] == CHAT_CONNECT_SERVER_KEY && !has_channel(third) && has_channel(first) && has_channel(second));
    CHECK(!chat_server_link_build(server, link, sizeof(link)));
    hold_request = 1; request_size = 0;
    CHECK(!chat_sync_offer_link(client, 7, third, link, strlen(link))); wait_packet(ua, &request_size);
    /* Каждый усечённый запрос должен закончиться отказом до создания канала. */
    for (size_t cut = CHAT_SYNC_BOOTSTRAP_SIZE; cut < request_size; cut++) deliver(server, client, request_wire, cut);
    CHECK(!has_channel(third));
    hold_request = 0; deliver(server, client, request_wire, request_size); wait_value(ua, &events[7]);
    /* Первый отказ от усечённого пакета завершил исходный request ровно один раз. */
    CHECK(events[7] == 1 && results[7] == CHAT_CONNECT_INVALID);
    test_clock_run_for(ua, 20000);
    CHECK(!chat_sync_offer_link(client, 8, third, link, strlen(link))); wait_value(ua, &events[8]);
    CHECK(events[8] == 1 && !results[8] && has_channel(third));

    /* Отмена ожидания после отправки не откатывает уже выполненный JOIN. */
    hold_request = 1; request_size = 0;
    CHECK(!chat_sync_offer_link(client, 9, first, link, strlen(link))); wait_packet(ua, &request_size);
    chat_sync_cancel_join(client, 9); CHECK(events[9] == 1 && results[9] == CHAT_CONNECT_CANCELLED);
    hold_request = 0;
    CHECK(!chat_sync_offer_link(client, 10, first, link, strlen(link))); wait_value(ua, &events[10]);
    CHECK(events[10] == 1 && !results[10]);

    /* Отмена до callback регистрации освобождает builder; следующий запрос работает. */
    CHECK(!chat_sync_offer_link(client, 11, first, link, strlen(link)));
    chat_sync_cancel_join(client, 11); CHECK(events[11] == 1 && results[11] == CHAT_CONNECT_CANCELLED);
    test_clock_run_for(ua, 1000); CHECK(events[11] == 1);

    /* Молчащий сервер ограничен общим deadline; повтор может использовать тот же транспорт. */
    hold_request = 1; request_size = 0;
    CHECK(!chat_sync_offer_link(client, 12, first, link, strlen(link))); wait_packet(ua, &request_size);
    test_clock_run_for(ua, 610000); CHECK(events[12] == 1 && results[12] == CHAT_CONNECT_TIMEOUT);
    hold_request = 0;
    CHECK(!chat_sync_offer_link(client, 13, first, link, strlen(link))); wait_value(ua, &events[13]);
    CHECK(events[13] == 1 && !results[13]);

    /* Остановка сервиса отменяет ожидающую команду ровно один раз. */
    hold_request = 1; request_size = 0;
    CHECK(!chat_sync_offer_link(client, 14, first, link, strlen(link))); wait_packet(ua, &request_size);
    group_service_stop(client); CHECK(events[14] == 1 && results[14] == CHAT_CONNECT_CANCELLED);
    hold_request = 0;
    CHECK(!chat_service_start(client));
    first = topo_groups_channel_slot(client->topo_groups, first_key); CHECK(first);
    CHECK(!chat_sync_offer_link(client, 15, first, link, strlen(link))); wait_value(ua, &events[15]);
    CHECK(events[15] == 1 && !results[15]);
    test_address_invite(ua, first);

    /* Push в ещё неизвестную группу. Подмена Ed25519 в заголовке отклоняется до JOIN. */
    server->config->global.chatserver_enabled = 0;
    CHECK(!chat_setting_set(server, "join_policy", "0"));
    struct chat_channel_create offer = {.name="push"}, occupied = {.name="unrelated local group"};
    CHECK(!chat_channel_generate_keys(offer.x25519_pubkey, offer.x25519_privkey, offer.ed25519_pubkey, offer.ed25519_privkey));
    sc_public_key_hex(offer.ed25519_pubkey, offer.channel_id);
    const char* push_channel = offer.channel_id;
    struct chat_channel_create* groups[] = {&offer, &occupied};
    struct UTUN_INSTANCE* owners[] = {client, server};
    CHECK(!chat_channel_generate_keys(occupied.x25519_pubkey, occupied.x25519_privkey, occupied.ed25519_pubkey, occupied.ed25519_privkey));
    sc_public_key_hex(occupied.ed25519_pubkey, occupied.channel_id);
    uint8_t descriptor[CHAT_CHANNEL_DESCRIPTION_MAX], local_key[32];
    for (size_t i = 0; i < sizeof(groups) / sizeof(*groups); i++) {
        struct chat_channel_create* group = groups[i];
        memcpy(group->owner_node_id, owners[i]->my_ed25519_pubkey, 32);
        int size = chat_channel_description_data(group->channel_id, group->name, group->owner_node_id,
            group->x25519_pubkey, group->ed25519_pubkey, descriptor, sizeof(descriptor));
        CHECK(size > 0 && !sc_ed25519_sign(group->ed25519_privkey, descriptor, (size_t)size, group->signature));
        CHECK(!chat_core_create_channel(owners[i], group));
    }
    CHECK(topo_node_sqlite_channel_get(server->topo_sqlite_db, push_channel, NULL, 0, NULL, NULL, local_key, NULL));
    uint64_t push = topo_groups_channel_slot(client->topo_groups, push_channel); CHECK(push);
    hold_offer = 1;
    CHECK(!chat_sync_invite_to_channel(client, push, server->my_ed25519_pubkey, NULL));
    wait_packet(ua, &offer_size);
    uint8_t push_key[32];
    CHECK(!topo_node_sqlite_channel_get(client->topo_sqlite_db, push_channel, NULL, 0, NULL, NULL, push_key, NULL));
    CHECK(offer_size > CHAT_SYNC_GROUP_SIZE && !memcmp(offer_wire + CHAT_SYNC_SCOPE_OFF, push_key, sizeof(push_key)));
    unsigned before_push = info_requests;
    offer_wire[CHAT_SYNC_SCOPE_OFF] ^= 1;
    deliver(server, client, offer_wire, offer_size); test_clock_run_for(ua, 5000);
    CHECK(!topo_groups_find_wire(server->topo_groups, push_key) && info_requests == before_push);
    offer_wire[CHAT_SYNC_SCOPE_OFF] ^= 1;
    deliver(server, client, offer_wire, offer_size);
    uint64_t push_deadline = get_time_tb() + 100000;
    struct TOPO_GROUP* imported = NULL;
    while (get_time_tb() < push_deadline) {
        imported = topo_groups_find_wire(server->topo_groups, push_key);
        if (imported && topo_node_sqlite_member_key_allowed(server->topo_sqlite_db, imported->channel_id,
            server->my_keys.public_key, server->my_ed25519_pubkey, 0)) break;
        test_clock_poll(ua, 100);
    }
    CHECK(imported && imported->group_id != push && info_requests > before_push);
    CHECK(topo_node_sqlite_member_key_allowed(server->topo_sqlite_db, imported->channel_id,
        server->my_keys.public_key, server->my_ed25519_pubkey, 0));
    uint8_t stored[32];
    CHECK(!topo_node_sqlite_channel_get(server->topo_sqlite_db, push_channel, NULL, 0, NULL, NULL, stored, NULL));
    CHECK(!memcmp(push_key, stored, 32));
    CHECK(!strcmp(imported->channel_id, push_channel));
    CHECK(!topo_node_sqlite_channel_get(server->topo_sqlite_db, occupied.channel_id, NULL, 0, NULL, NULL, local_key, NULL));
    CHECK(!memcmp(local_key, occupied.ed25519_pubkey, 32));

    utun_instance_destroy(server); utun_instance_destroy(client); uasync_poll(ua, 0); uasync_destroy(ua, 0);
    CHECK(u_get_allocated_count() == baseline); test_clock_finish();
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS server join: auth, ordinary JOIN, confirmation, channels, rotation, framing, full group keys across independent route indices, replay, cancel, timeout, restart");
    return 0;
}
