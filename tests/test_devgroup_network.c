/* Настоящий router/UDP A ↔ B ↔ C: devgroup E2E, повтор PIN, потеря ACK, NODEINFO и выход. */
#include "test_join_id.h"
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_devgroup.h"
#include "chat/chat_clipboard.h"
#include "chat/group_info.h"
#include "chat/chat_event.h"
#include "chat/chat_member.h"
#include "routing_layer/route_crypto.h"

#include "chat/member_sync.h"
#include "chat/merkle_sync.h"
#include "chat/db_sync.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "transport_layer/node_conn_direct.h"
#include "transport_layer/etcp_connections.h"
#include "transport_layer/etcp.h"
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "../lib/debug_config.h"
#include "../lib/file_io.h"
#include "test_net.h"
#include "test_clock.h"
#include "router_wire_observer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct UASYNC* ua;
static struct UTUN_INSTANCE* nodes[4];
static char channel[65];
static etcp_recv_fn relay_receiver;
static unsigned relay_checked;
static unsigned clipboard_relay, clipboard_received, clipboard_finished;
static uint8_t clipboard_expected[20000];
static int clipboard_apply = 1;
static unsigned clipboard_offers, clipboard_file_relay;
static etcp_recv_fn clipboard_wire_receiver[3];
static int clipboard_pull_finished, clipboard_pull_error;
static int clipboard_scope;
static void clipboard_pulled(void* arg, int error) { (void)arg; clipboard_pull_error = error; clipboard_pull_finished = 1; }
static void clipboard_wire_receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct ll_entry* canonical = test_router_observe(entry, conn->instance->topo_groups, NULL, NULL);
    const struct SVC_ROUTE_HDR* h = (const struct SVC_ROUTE_HDR*)canonical->dgram;
    if (h->svc_id == ETCP_RT_ID_CLIPBOARD_FILES) {
        assert((h->flags & (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) == (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
        size_t body = canonical->len;
        unsigned author = h->src_node_id == nodes[0]->node_id ? 0 : 2;
        uint8_t* plain = NULL, flags; size_t length;
        assert(route_crypto_decode(nodes[1], ROUTE_CRYPTO_CHAT, nodes[author]->my_keys.public_key,
            nodes[author]->my_ed25519_pubkey, canonical->dgram, body, &plain, &length, &flags) < 0 && !plain);
        clipboard_file_relay++;
    }
    unsigned receiver = conn->instance == nodes[0] ? 0 : 2;
    queue_dgram_free(canonical); queue_entry_free(canonical);
    clipboard_wire_receiver[receiver](conn, entry);
}
static void clipboard_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (type == CHAT_EVT_CLIPBOARD_RECEIVED) {
        assert(inst == nodes[2]);
        assert(data[CHAT_CLIPBOARD_EVENT_SCOPE_OFF] == clipboard_scope);
        if (data[CHAT_CLIPBOARD_EVENT_TYPE_OFF] == CHAT_CLIPBOARD_TEXT) {
            assert(len == CHAT_CLIPBOARD_EVENT_HEADER + sizeof(clipboard_expected));
            assert(!memcmp(data + CHAT_CLIPBOARD_EVENT_HEADER, clipboard_expected, sizeof(clipboard_expected))); clipboard_received++;
        } else { assert(data[CHAT_CLIPBOARD_EVENT_TYPE_OFF] == CHAT_CLIPBOARD_FILES); clipboard_offers++; }
        struct TOPO_GROUP* group = topo_groups_find_wire(inst->topo_groups, data + CHAT_CLIPBOARD_EVENT_GROUP_OFF);
        assert(group);
        assert(!memcmp(data + CHAT_CLIPBOARD_EVENT_PEER_ED_OFF, nodes[0]->my_ed25519_pubkey, 32));
        chat_clipboard_applied(inst, group->group_id, data + CHAT_CLIPBOARD_EVENT_PEER_ED_OFF, data, clipboard_apply);
    } else if (type == CHAT_EVT_CLIPBOARD_RESULT && len == 25 && data[8] == CHAT_CLIPBOARD_FINISHED) {
        assert(inst == nodes[0] && data[9] == 1 && data[13] == clipboard_apply && data[17] == 0 && data[21] == !clipboard_apply);
        clipboard_finished++;
    }
}

/* На настоящем BGP-транзите B видит только заголовок и не раскрывает даже handshake/ACK. */
static void relay_receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct ll_entry* canonical = test_router_observe(entry, conn->instance->topo_groups, NULL, NULL);
    const struct SVC_ROUTE_HDR* h = (const struct SVC_ROUTE_HDR*)canonical->dgram;
    if ((h->svc_id == ETCP_RT_ID_CLIPBOARD || h->svc_id == ETCP_RT_ID_CLIPBOARD_FILES) &&
        h->dst_node_id != nodes[1]->node_id) {
        assert((h->flags & (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) == (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
        size_t body = canonical->len;
        unsigned author = h->src_node_id == nodes[0]->node_id ? 0 : 2;
        uint8_t* plain = NULL, flags; size_t length;
        assert(route_crypto_decode(nodes[1], ROUTE_CRYPTO_CHAT, nodes[author]->my_keys.public_key,
            nodes[author]->my_ed25519_pubkey, canonical->dgram, body, &plain, &length, &flags) < 0 && !plain);
        if (h->svc_id == ETCP_RT_ID_CLIPBOARD_FILES) clipboard_file_relay++; else clipboard_relay++;
    }
    if (h->svc_id == ETCP_RT_ID_DEVGROUP && h->dst_node_id != nodes[1]->node_id) {
        assert((h->flags & (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) == (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
        size_t body = canonical->len;
        unsigned author = h->src_node_id == nodes[0]->node_id ? 0 : 2;
        unsigned bit = 1u << (author + (body > SVC_ROUTE_HDR_SIZE + SC_SIGN_SIZE + SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE));
        if (!(relay_checked & bit)) {
            uint8_t* plain = NULL, flags; size_t length;
            assert(route_crypto_decode(nodes[1], ROUTE_CRYPTO_CHAT, nodes[author]->my_keys.public_key,
                nodes[author]->my_ed25519_pubkey, canonical->dgram, body, &plain, &length, &flags) < 0 && !plain);
            relay_checked |= bit;
        }
    }
    queue_dgram_free(canonical); queue_entry_free(canonical);
    relay_receiver(conn, entry);
}

static void hex(const uint8_t* data, size_t len, char* out) {
    for (size_t i = 0; i < len; i++) snprintf(out + i * 2, 3, "%02x", data[i]);
}

static struct UTUN_INSTANCE* create_node(unsigned port) {
    uint8_t device_x[32], device_seed[32]; assert(sc_identity_generate_config(device_x, device_seed) == SC_OK);
    char private_key[65], public_key[65], config[1024]; hex(device_seed, 32, private_key); hex(device_x, 32, public_key);
    snprintf(config, sizeof(config),
        "[global]\nmy_private_key=%s\nmy_public_key=%s\nkeepalive_adaptive=0\n"
        "[server: udp]\naddr=127.0.0.1:%u\ntype=public\n[allowed_keys]\nallow_all=1\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", private_key, public_key, port);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && utun_core_start(inst) == 0 && chat_service_start(inst) == 0); return inst;
}

/* Все три заранее принятых участника имеют настоящие подписи root/join. */
static void seed_members(uint8_t channel_x[32], uint8_t channel_ed[32], uint8_t owner_private[32], int count) {
    for (int dst = 0; dst < count; dst++) for (int author = 0; author < count; author++) {
        uint8_t body[MEMBER_JOIN_MSG_SIZE], join[64], tree[64];
        assert(member_sync_build_join_msg(channel_x, channel_ed, nodes[author]->my_ed25519_pubkey,
            nodes[author]->my_keys.public_key, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
        assert(sc_ed25519_sign(nodes[author]->my_ed25519_privkey, body, sizeof(body), join) == SC_OK);
        assert(member_sync_sign_join(owner_private, channel, NULL,
            nodes[author]->my_keys.public_key, nodes[author]->my_ed25519_pubkey,
                TEST_JOIN_ID(channel_x, channel_ed, nodes[author]->my_ed25519_pubkey, nodes[author]->my_keys.public_key), 1, tree) == 0);
        assert(member_sync_put(nodes[dst], channel, nodes[author]->my_keys.public_key,
            nodes[author]->my_ed25519_pubkey, join, 1, NULL, 0, "", NULL, NULL, 0, 0, tree) >= 0);
    }
}

/* Поднимаем транспорт только соседних пар; C получает данные A через B. */
static struct NODE_CONN_DIRECT* connect_node(int from, int to, unsigned port) {
    struct TOPO_ADDR4 address = { .addr = {127, 0, 0, 1}, .port = port, .protocol = TOPO_PROTO_UDP };
    struct TOPO_NODE node = { .node_id = nodes[to]->node_id, .v4_addrs = &address };
    memcpy(node.public_key, nodes[to]->my_keys.public_key, 32); struct NODE_CONN_DIRECT* handle = NULL;
    assert(node_conn_direct_open_node(nodes[from], node.node_id, NULL, NULL, &handle, &node, NULL) != NCD_ERR); return handle;
}

static int wait_ready(struct TOPO_PEER_REQUEST* handle) {
    uint64_t deadline = get_time_tb() + 15u * 10000u;
    while (get_time_tb() < deadline) {
        if (topo_group_peer_phase(handle) == TOPO_PEER_READY) return 1;
        test_clock_poll(ua, 5);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "devgroup network: group readiness timeout phase=%d", topo_group_peer_phase(handle)); return 0;
}

/* C открывает групповой запрос после входящего UP; до него адрес B ещё не получен. */
static int wait_transport(struct NODE_CONN_DIRECT* ab, struct NODE_CONN_DIRECT* bc) {
    struct ETCP_CONN *a = NULL, *b = NULL;
    uint64_t deadline = get_time_tb() + 15u * 10000u;
    while (get_time_tb() < deadline) {
        a = node_conn_direct_get_conn(ab); b = node_conn_direct_get_conn(bc);
        struct ETCP_CONN* c = instance_find_conn(nodes[2], nodes[1]->node_id);
        if (a && b && c && a->initialized && b->initialized && c->initialized
            && a->links_up && b->links_up && c->links_up) return 1;
        test_clock_poll(ua, 5);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "devgroup network: bootstrap timeout ab=%p bc=%p", (void*)a, (void*)b); return 0;
}


static struct ll_entry* held_keys;
static int hold_keys;
static int drop_pin_result = 1, drop_key_ack = 1, duplicate_pin, key_sends;
static int drop_first_ok = 1, drop_return_ok = 1, return_sends, return_acks;
int __real_etcp_route_send_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t*, struct ll_entry*, int, int);
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], struct ll_entry* entry, int ttl, int crypto) {
    if (entry->len >= 18 && entry->dgram[0] == ETCP_RT_ID_DEVGROUP) {
        assert(crypto == (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
        if (entry->dgram[1] == 2) duplicate_pin++;
        if (entry->dgram[1] == 5) return_sends++;
        if (entry->dgram[1] == 4 && entry->len == 21 && entry->dgram[18] == 2 && entry->dgram[20] == 0) return_acks++;
        if (entry->dgram[1] == 3) {
            key_sends++;
            if (hold_keys && !held_keys) { held_keys = entry; return 0; }
        }
        if (entry->dgram[1] == 4 && entry->len == 21 &&
            ((entry->dgram[18] == 1 && entry->dgram[20] == 1 && drop_pin_result-- > 0) ||
            (entry->dgram[18] == 1 && entry->dgram[20] == 0 && drop_first_ok-- > 0) ||
            (entry->dgram[18] == 2 && entry->dgram[20] == 0 && drop_return_ok-- > 0) ||
            (entry->dgram[18] == 3 && entry->dgram[20] == 0 && drop_key_ack-- > 0))) {
            queue_dgram_free(entry); queue_entry_free(entry); return 0;
        }
    }
    return __real_etcp_route_send_key(inst, group, peer, entry, ttl, crypto);
}
static int state(int node, const char* id) {
    char* json = chat_devgroup_status(nodes[node]); assert(json);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(nodes[node]->topo_sqlite_db,
        "SELECT json_extract(value,'$.state') FROM json_each(?,'$.invitations') WHERE json_extract(value,'$.id')=?", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
    int result = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st); u_free(json); return result;
}
static void wait_state(int node, const char* id, int expected) {
    uint64_t until = get_time_tb() + 150000;
    while (get_time_tb() < until) { if (state(node, id) == expected) return; test_clock_poll(ua, 5); }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup test: timeout node=%d state=%d expected=%d", node, state(node, id), expected); abort();
}
static void return_pin(const char* id, char pin[5]) {
    char* json = chat_devgroup_status(nodes[2]); assert(json); sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(nodes[2]->topo_sqlite_db,
        "SELECT json_extract(value,'$.pin') FROM json_each(?,'$.invitations') WHERE json_extract(value,'$.id')=?",
        -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 4);
    memcpy(pin, sqlite3_column_text(st, 0), 4); pin[4] = 0; sqlite3_finalize(st); u_free(json);
}
static void confirm_both(const char* id, const char* pin) {
    assert(!chat_devgroup_accept(nodes[2], id, pin)); wait_state(2, id, DG_WAIT_RETURN); wait_state(0, id, DG_WAIT_RETURN);
    char reply[5]; return_pin(id, reply); assert(strcmp(reply, pin));
    assert(!chat_devgroup_accept(nodes[0], id, reply));
}
static void wait_profile(int node, const uint8_t peer_ed[32], const char* name) {
    uint64_t until = get_time_tb() + 150000;
    while (get_time_tb() < until) {
        uint8_t id[32]; char actual[64];
        int rc = devgroup_member(nodes[node]->topo_sqlite_db, peer_ed, id, actual);
        assert(rc >= 0); if (!strcmp(actual, name)) return; test_clock_poll(ua, 5);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "devgroup test: profile timeout node=%d peer=%s", node, sc_public_key_hex(peer_ed, (char[65]){0})); abort();
}
static void clipboard_permission(int enabled) {
    char* result = NULL;
    assert(!group_info_command(nodes[0], channel, NULL, NULL, enabled, NULL, &result)); u_free(result);
    struct group_info_record record; assert(!group_info_get(nodes[0]->topo_sqlite_db, channel, &record));
    for (unsigned i = 1; i < 3; i++) {
        assert(sqlite3_exec(nodes[i]->topo_sqlite_db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
        assert(group_info_apply(nodes[i]->topo_sqlite_db, channel, &record) >= 0);
        assert(sqlite3_exec(nodes[i]->topo_sqlite_db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
        group_info_publish(nodes[i], channel);
    }
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_INFO);
    test_clock_init(1000);
    ua = uasync_create(); assert(ua); unsigned ports[4];
    for (int i = 0; i < 3; i++) {
        nodes[i] = create_node(0);
        ports[i] = ss_get_port(&test_etcp_bound_socket(nodes[i], "udp")->local_addr);
    }
    chat_core_create_channel_auto(nodes[0], "Devices");
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(nodes[0]->topo_sqlite_db, "SELECT channel_id FROM channels", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(st, 0)); sqlite3_finalize(st);
    uint8_t x[32], ed[32], signature[64], owner_private[32];
    assert(topo_node_sqlite_channel_get(nodes[0]->topo_sqlite_db, channel, NULL, 0, NULL, x, ed, signature) == 0);
    assert(topo_node_sqlite_channel_get_priv(nodes[0]->topo_sqlite_db, channel, owner_private) == 0);
    for (int i = 1; i < 3; i++) {
        assert(topo_node_sqlite_channel_put(nodes[i]->topo_sqlite_db, channel, "Devices", nodes[0]->my_ed25519_pubkey,
            x, NULL, ed, NULL, signature) == 0); chat_core_ensure_channel_ready(nodes[i], channel);
    }
    seed_members(x, ed, owner_private, 3);
    struct NODE_CONN_DIRECT* ab = connect_node(0, 1, ports[1]), *bc = connect_node(1, 2, ports[2]);
    assert(wait_transport(ab, bc));
    struct TOPO_PEER_REQUEST *groups[5] = {0}; uint64_t gid = topo_groups_channel_slot(nodes[0]->topo_groups, channel);
    assert(topo_group_peer_open(topo_groups_find(nodes[0]->topo_groups, gid), nodes[1]->node_id, &groups[0]) == 0);
    assert(topo_group_peer_open(topo_groups_find(nodes[1]->topo_groups, gid), nodes[2]->node_id, &groups[1]) == 0);
    assert(topo_group_peer_open(topo_groups_find(nodes[2]->topo_groups, gid), nodes[1]->node_id, &groups[2]) == 0);
    for (int i = 0; i < 3; i++) assert(wait_ready(groups[i]));
    relay_receiver = nodes[1]->api_bindings.callbacks[ETCP_ID_SVC_ROUTE]; assert(relay_receiver);
    nodes[1]->api_bindings.callbacks[ETCP_ID_SVC_ROUTE] = relay_receive;
    for (unsigned i = 0; i < 3; i += 2) {
        clipboard_wire_receiver[i] = nodes[i]->api_bindings.callbacks[ETCP_ID_SVC_ROUTE]; assert(clipboard_wire_receiver[i]);
        nodes[i]->api_bindings.callbacks[ETCP_ID_SVC_ROUTE] = clipboard_wire_receive;
    }
    /* Ждём снимок C через B до выбора C как известного участника. */
    uint64_t until = get_time_tb() + 150000;
    while (!topo_node_registry_find(nodes[0]->topo_groups, nodes[2]->node_id) && get_time_tb() < until) test_clock_poll(ua, 5);
    char id[33], pin[5], wrong[5];
    struct devgroup_record before; uint8_t before_ed[32], before_x[32];
    assert(!chat_devgroup_invite(nodes[0], gid, nodes[2]->my_ed25519_pubkey, id, pin)); wait_state(2, id, 1);
    assert(devgroup_load_local(nodes[0], &before, before_ed, before_x) == 0);
    strcpy(wrong, pin); wrong[0] = pin[0] == '0' ? '1' : '0';
    assert(!chat_devgroup_accept(nodes[2], id, wrong)); wait_state(2, id, 1);
    assert(duplicate_pin >= 2 && state(0, id) == 1);
    assert(devgroup_load_local(nodes[0], &before, before_ed, before_x) == 0);
    int previous_keys = key_sends;
    assert(!chat_devgroup_accept(nodes[2], id, pin)); wait_state(2, id, DG_WAIT_RETURN); wait_state(0, id, DG_WAIT_RETURN);
    assert(key_sends == previous_keys && devgroup_load_local(nodes[2], &before, before_ed, before_x) == 0);
    assert(devgroup_load_local(nodes[0], &before, before_ed, before_x) == 0);
    char reply[5]; return_pin(id, reply); assert(strcmp(reply, pin));
    strcpy(wrong, reply); wrong[0] = reply[0] == '0' ? '1' : '0';
    assert(!chat_devgroup_accept(nodes[0], id, wrong)); wait_state(0, id, DG_WAIT_RETURN);
    assert(state(2, id) == DG_WAIT_RETURN && key_sends == previous_keys);
    assert(devgroup_load_local(nodes[2], &before, before_ed, before_x) == 0);
    assert(!chat_devgroup_accept(nodes[0], id, reply)); wait_state(2, id, 4); wait_state(0, id, 4);
    /* После потери ACK первым может сработать таймер повторного ACK, а не запроса PIN. */
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "devgroup retries: keys=%d return_pin=%d return_ack=%d drop_ack=%d/%d",
        key_sends, return_sends, return_acks, drop_return_ok, drop_key_ack);
    assert(key_sends >= 2 && return_sends >= 2 && return_acks >= 2 && drop_return_ok < 0 && drop_key_ack < 0);
    assert(relay_checked == 15); /* Данные и control обоих отправителей защищены от промежуточного B. */
    struct devgroup_record r[3]; uint8_t ep[3][32], xp[3][32];
    assert(devgroup_load_local(nodes[0], &r[0], ep[0], xp[0]) == 1);
    assert(devgroup_load_local(nodes[1], &r[1], ep[1], xp[1]) == 0);
    assert(devgroup_load_local(nodes[2], &r[2], ep[2], xp[2]) == 1);
    assert(!memcmp(ep[0], ep[2], 32) && !memcmp(xp[0], xp[2], 32));
    assert(!chat_devgroup_rename(nodes[2], "New device group"));
    wait_profile(0, nodes[2]->my_ed25519_pubkey, "New device group");
    until = get_time_tb() + 150000;
    do {
        assert(devgroup_load_local(nodes[0], &r[0], ep[0], xp[0]) == 1);
        if (!strcmp(r[0].json, topo_node_registry_find(nodes[2]->topo_groups, nodes[2]->node_id)->devgroup.json)) break;
        test_clock_poll(ua, 5);
    } while (get_time_tb() < until);
    assert(!strcmp(r[0].json, topo_node_registry_find(nodes[2]->topo_groups, nodes[2]->node_id)->devgroup.json));
    wait_profile(1, nodes[0]->my_ed25519_pubkey, "New device group");
    chat_event_set_handler(nodes[0], clipboard_event); chat_event_set_handler(nodes[2], clipboard_event);
    for (unsigned i = 0; i < sizeof(clipboard_expected); i++) clipboard_expected[i] = (uint8_t)('a' + i % 26);
    for (unsigned pass = 0; pass < 2; pass++) {
        clipboard_apply = !pass;
        assert(!chat_clipboard_send(nodes[0], pass + 1, 0, 0, CHAT_CLIPBOARD_TEXT, clipboard_expected, sizeof(clipboard_expected)));
        until = get_time_tb() + 150000;
        while (clipboard_finished <= pass && get_time_tb() < until) test_clock_poll(ua, 5);
        assert(clipboard_finished == pass + 1 && clipboard_received == pass + 1);
    }
    assert(clipboard_relay >= 6);
    /* Offer не передаёт содержимое. Отдельный pull идёт тем же E2E router через B. */
    struct file_io_temp clipboard_source = {0}, clipboard_destination = {0};
    assert(!file_io_temp_open(&clipboard_source, "utun-clipboard-source", 0));
    assert(!file_io_temp_open(&clipboard_destination, "utun-clipboard-dest", 0));
    assert(fwrite(clipboard_expected, 1, sizeof(clipboard_expected), clipboard_source.file) == sizeof(clipboard_expected));
    assert(!file_io_temp_finish(&clipboard_source) && !file_io_temp_finish(&clipboard_destination));
    const char manifest[] = "{\"version\":1}";
    struct chat_clipboard_request* offer = u_calloc(1, sizeof(*offer) + sizeof(struct chat_clipboard_source) + sizeof(manifest) - 1);
    assert(offer); offer->inst = nodes[0]; offer->request = 3; offer->action = 2; offer->source_count = 1; offer->size = sizeof(manifest) - 1;
    struct chat_clipboard_source* source = (struct chat_clipboard_source*)(offer + 1);
    memset(source->object, 9, 16); source->size = sizeof(clipboard_expected);
    assert(strlen(clipboard_source.temporary) < sizeof(source->path)); strcpy(source->path, clipboard_source.temporary);
    memcpy(source + 1, manifest, sizeof(manifest) - 1); clipboard_apply = 1; chat_clipboard_trampoline(offer);
    until = get_time_tb() + 150000;
    while (clipboard_finished < 3 && get_time_tb() < until) test_clock_poll(ua, 5);
    assert(clipboard_offers == 1 && clipboard_finished == 3 && !clipboard_file_relay);
    struct chat_clipboard_pull_request* pull = u_calloc(1, sizeof(*pull)); assert(pull);
    pull->inst = nodes[2]; memcpy(pull->group, topo_group_wire_id(topo_groups_find(nodes[2]->topo_groups, gid)), 32); memcpy(pull->peer_ed, nodes[0]->my_ed25519_pubkey, 32); pull->size = sizeof(clipboard_expected);
    memset(pull->object, 9, 16); strcpy(pull->destination, clipboard_destination.temporary); pull->done = clipboard_pulled;
    chat_clipboard_pull_trampoline(pull); until = get_time_tb() + 150000;
    while (!clipboard_pull_finished && get_time_tb() < until) test_clock_poll(ua, 5);
    assert(clipboard_pull_finished && !clipboard_pull_error && clipboard_file_relay);
    FILE* received = file_io_open_read(clipboard_destination.temporary); uint8_t bytes[sizeof(clipboard_expected)];
    assert(received && fread(bytes, 1, sizeof(bytes), received) == sizeof(bytes) && fgetc(received) == EOF);
    assert(!memcmp(bytes, clipboard_expected, sizeof(bytes))); fclose(received);
    /* Чужой участник общей CHAT-группы не может открыть clipboard-сервис. */
    assert(!etcp_router_conn_get(nodes[1], gid, nodes[2]->node_id, ETCP_RT_ID_CLIPBOARD));
    assert(!etcp_router_conn_get(nodes[1], gid, nodes[2]->node_id, ETCP_RT_ID_CLIPBOARD_FILES));
    chat_event_set_handler(nodes[0], NULL); chat_event_set_handler(nodes[2], NULL);
    assert(!chat_devgroup_leave(nodes[2])); wait_profile(0, nodes[2]->my_ed25519_pubkey, "");
    chat_event_set_handler(nodes[0], clipboard_event); chat_event_set_handler(nodes[2], clipboard_event);
    clipboard_scope = CHAT_CLIPBOARD_MEMBER;
    assert(chat_clipboard_send(nodes[0], 4, gid, nodes[2]->my_ed25519_pubkey, CHAT_CLIPBOARD_TEXT, clipboard_expected, sizeof(clipboard_expected)) < 0);
    clipboard_permission(1);
    assert(!chat_clipboard_send(nodes[0], 4, gid, nodes[2]->my_ed25519_pubkey, CHAT_CLIPBOARD_TEXT, clipboard_expected, sizeof(clipboard_expected)));
    until = get_time_tb() + 150000; while (clipboard_finished < 4 && get_time_tb() < until) test_clock_poll(ua, 5);
    assert(clipboard_finished == 4 && clipboard_received == 3);
    offer = u_calloc(1, sizeof(*offer) + sizeof(struct chat_clipboard_source) + sizeof(manifest) - 1); assert(offer);
    offer->inst = nodes[0]; offer->request = 5; offer->action = 2; offer->source_count = 1; offer->size = sizeof(manifest) - 1;
    memcpy(offer->group, topo_group_wire_id(topo_groups_find(offer->inst->topo_groups, gid)), 32); memcpy(offer->peer_ed, nodes[2]->my_ed25519_pubkey, 32);
    source = (struct chat_clipboard_source*)(offer + 1); memset(source->object, 10, 16); source->size = sizeof(clipboard_expected);
    strcpy(source->path, clipboard_source.temporary); memcpy(source + 1, manifest, sizeof(manifest) - 1);
    chat_clipboard_trampoline(offer);
    until = get_time_tb() + 150000; while (clipboard_finished < 5 && get_time_tb() < until) test_clock_poll(ua, 5);
    assert(clipboard_finished == 5 && clipboard_offers == 2);
    for (unsigned attacker = 0; attacker < 2; attacker++) {
        clipboard_pull_finished = 0;
        pull = u_calloc(1, sizeof(*pull)); assert(pull); pull->inst = nodes[1]; memcpy(pull->group, topo_group_wire_id(topo_groups_find(pull->inst->topo_groups, gid)), 32); memcpy(pull->peer_ed, nodes[0]->my_ed25519_pubkey, 32);
        pull->size = sizeof(clipboard_expected); memset(pull->object, attacker ? 10 : 9, 16);
        strcpy(pull->destination, clipboard_destination.temporary); pull->done = clipboard_pulled; chat_clipboard_pull_trampoline(pull);
        until = get_time_tb() + 150000; while (!clipboard_pull_finished && get_time_tb() < until) test_clock_poll(ua, 5);
        assert(clipboard_pull_finished && clipboard_pull_error); // Ни own offer, ни offer другому участнику не доступны B.
    }
    clipboard_pull_finished = 0;
    pull = u_calloc(1, sizeof(*pull)); assert(pull); pull->inst = nodes[2]; memcpy(pull->group, topo_group_wire_id(topo_groups_find(pull->inst->topo_groups, gid)), 32); memcpy(pull->peer_ed, nodes[0]->my_ed25519_pubkey, 32);
    pull->size = sizeof(clipboard_expected); memset(pull->object, 10, 16); strcpy(pull->destination, clipboard_destination.temporary);
    pull->done = clipboard_pulled; chat_clipboard_pull_trampoline(pull);
    until = get_time_tb() + 150000; while (!clipboard_pull_finished && get_time_tb() < until) test_clock_poll(ua, 5);
    assert(clipboard_pull_finished && !clipboard_pull_error);
    clipboard_permission(0); clipboard_pull_finished = 0;
    pull = u_calloc(1, sizeof(*pull)); assert(pull); pull->inst = nodes[2]; memcpy(pull->group, topo_group_wire_id(topo_groups_find(pull->inst->topo_groups, gid)), 32); memcpy(pull->peer_ed, nodes[0]->my_ed25519_pubkey, 32);
    pull->size = sizeof(clipboard_expected); memset(pull->object, 10, 16); strcpy(pull->destination, clipboard_destination.temporary);
    pull->done = clipboard_pulled; chat_clipboard_pull_trampoline(pull);
    until = get_time_tb() + 150000; while (!clipboard_pull_finished && get_time_tb() < until) test_clock_poll(ua, 5);
    assert(clipboard_pull_finished && clipboard_pull_error);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "PASS member clipboard: signed permission, E2E, per-recipient object ACL and revocation");
    chat_event_set_handler(nodes[0], NULL); chat_event_set_handler(nodes[2], NULL);
    /* После успешного импорта новый ACK может потеряться прямо перед выходом. */
    drop_key_ack = 1;
    assert(!chat_devgroup_invite(nodes[0], gid, nodes[2]->my_ed25519_pubkey, id, pin)); wait_state(2, id, 1);
    confirm_both(id, pin); wait_state(2, id, 4);
    assert(devgroup_load_local(nodes[2], &r[2], ep[2], xp[2]) == 1);
    assert(!memcmp(ep[0], ep[2], 32) && !memcmp(xp[0], xp[2], 32));
    assert(!chat_devgroup_leave(nodes[2])); wait_state(0, id, 4);
    assert(devgroup_load_local(nodes[2], &r[2], ep[2], xp[2]) == 0);
    /* Три разные неверные попытки завершают операцию. */
    assert(!chat_devgroup_invite(nodes[0], gid, nodes[2]->my_ed25519_pubkey, id, pin)); wait_state(2, id, 1);
    strcpy(wrong, pin); wrong[0] = pin[0] == '0' ? '1' : '0';
    for (int i = 0; i < 3; i++) { assert(!chat_devgroup_accept(nodes[2], id, wrong)); wait_state(2, id, i == 2 ? 5 : 1); }
    assert(state(0, id) == 5 && chat_devgroup_accept(nodes[2], id, pin) < 0);
    /* Три неверных ответных PIN тоже завершают операцию без импорта ключей. */
    assert(!chat_devgroup_invite(nodes[0], gid, nodes[2]->my_ed25519_pubkey, id, pin)); wait_state(2, id, 1);
    assert(!chat_devgroup_accept(nodes[2], id, pin)); wait_state(2, id, DG_WAIT_RETURN); wait_state(0, id, DG_WAIT_RETURN);
    /* Даже настоящий подписанный пакет ключей от A не принимается до ручного ответного PIN. */
    uint8_t early[64 + DEVGROUP_WIRE_MAX], binary_id[16];
    assert(devgroup_load_local(nodes[0], &before, early, early + 32) == 1);
    int encoded = devgroup_encode(&before, early + 64, DEVGROUP_WIRE_MAX); assert(encoded > 0);
    assert(!sc_hex_to_binary(id, binary_id, 16));
    struct ll_entry* packet = ll_alloc_lldgram(18 + 64 + encoded); assert(packet);
    packet->dgram[0] = ETCP_RT_ID_DEVGROUP; packet->dgram[1] = 3; memcpy(packet->dgram + 2, binary_id, 16);
    memcpy(packet->dgram + 18, early, 64 + encoded); packet->len = 18 + 64 + encoded;
    assert(!__real_etcp_route_send_key(nodes[0], gid, nodes[2]->my_ed25519_pubkey, packet, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    until = get_time_tb() + 5000; while (get_time_tb() < until) test_clock_poll(ua, 5);
    assert(state(2, id) == DG_WAIT_RETURN && devgroup_load_local(nodes[2], &before, before_ed, before_x) == 0);
    return_pin(id, reply); strcpy(wrong, reply); wrong[0] = reply[0] == '0' ? '1' : '0'; previous_keys = key_sends;
    for (int i = 0; i < 3; i++) {
        assert(!chat_devgroup_accept(nodes[0], id, wrong)); wait_state(0, id, i == 2 ? DG_REJECTED : DG_WAIT_RETURN);
    }
    assert(state(2, id) == DG_REJECTED && key_sends == previous_keys);
    assert(devgroup_load_local(nodes[2], &before, before_ed, before_x) == 0);
    /* Выход до первого commit тоже отменяет согласие: поздние ключи не должны присоединить C. */
    hold_keys = 1;
    assert(!chat_devgroup_invite(nodes[0], gid, nodes[2]->my_ed25519_pubkey, id, pin)); wait_state(2, id, 1);
    confirm_both(id, pin); wait_state(0, id, 3); assert(held_keys);
    assert(!chat_devgroup_leave(nodes[2]) && state(2, id) == 5); hold_keys = 0;
    assert(!__real_etcp_route_send_key(nodes[0], gid, nodes[2]->my_ed25519_pubkey, held_keys, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    held_keys = NULL;
    until = get_time_tb() + 5000; while (get_time_tb() < until) test_clock_poll(ua, 5);
    assert(devgroup_load_local(nodes[2], &r[2], ep[2], xp[2]) == 0);
    assert(!chat_devgroup_leave(nodes[0]) && state(0, id) == 7);
    for (int i = 0; i < 3; i++) topo_group_peer_close(groups[i]);
    node_conn_direct_close(ab); node_conn_direct_close(bc);
    for (int i = 0; i < 3; i++) utun_instance_destroy(nodes[i]);
    file_io_temp_discard(&clipboard_source); file_io_temp_discard(&clipboard_destination);
    uasync_poll(ua, 0); uasync_destroy(ua, 0);
    test_clock_finish();
    puts("PASS devgroup: two manual PINs, early keys rejected, retries/ACK loss in both phases, NODEINFO, leave/replay, three attempts");
    return 0;
}
