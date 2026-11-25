#include <assert.h>
#include <string.h>
#include <openssl/evp.h>
#include "utun_instance.h"
#include "routing_layer/topo_group.h"
#include "transport_layer/etcp.h"
#include "transport_layer/etcp_api.h"
#include "transport_layer/node_conn_direct.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"

#ifdef __linux__
static int fail_path;
struct ll_entry* __real_queue_entry_new(size_t);
struct ll_entry* __wrap_queue_entry_new(size_t size) {
    if (fail_path && size == sizeof(struct TOPO_NODEPATH) - sizeof(struct ll_entry) + sizeof(struct topo_hop)) {
        fail_path = 0;
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "[fault] reject incoming BGP path allocation"); return NULL;
    }
    return __real_queue_entry_new(size);
}
#endif

static struct TOPO_GROUP_CONN_ITEM* peer(struct TOPO_GROUP* group) {
    assert(group->senders_list->head && !group->senders_list->head->next);
    return (struct TOPO_GROUP_CONN_ITEM*)group->senders_list->head->data;
}

/* Deterministic transport consumer: consume one packet, then let the next producer run. */
static unsigned drain(struct ETCP_CONN* conn) {
    unsigned joins = 0;
    for (int i = 0; i < 128; i++) {
        uasync_poll(conn->instance->ua, 0);
        assert(queue_entry_count(conn->send_input_q) <= 1);
        struct ll_entry* e = queue_data_get(conn->send_input_q);
        if (e) {
            if (e->len >= 2 && e->dgram[0] == ETCP_ID_TOPO_ENTRY && e->dgram[1] == TOPO_SUBCMD_JOIN_GROUP) joins++;
            queue_dgram_free(e); queue_entry_free(e);
        }
        queue_resume_callback(conn->send_input_q);
    }
    return joins;
}

static void packet(struct ETCP_CONN* conn, const void* data, size_t size) {
    struct ll_entry* e = ll_alloc_lldgram(size); assert(e);
    memcpy(e->dgram, data, size); e->len = size;
    conn->instance->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY](conn, e);
}

static void nodeinfo(struct TOPO_GROUP* group, struct ETCP_CONN* conn, struct TOPO_NODE* node,
                     const uint8_t seed[32], uint64_t generation, uint64_t timestamp) {
    node->timestamp = timestamp;
    uint8_t signed_data[TOPO_SIG_MSG_MAX_SIZE];
    int length = topo_node_build_sig_msg(node, signed_data, sizeof(signed_data)); assert(length > 0);
    assert(sc_ed25519_sign(seed, signed_data, length, node->x25519_self_sig) == SC_OK);
    struct TOPOMSG_HEADER h = { .cmd = ETCP_ID_TOPO_ENTRY, .subcmd = TOPO_SUBCMD_NODEINFO,
        .src_epoch = group->group_id + 1000, .dst_epoch = generation };
    memcpy(h.group_key, topo_group_wire_id(group), 32);
    uint8_t wire[TOPO_NODE_WIRE_MAX_SIZE + sizeof(struct TOPOMSG_NODEINFO_PKT)];
    memcpy(wire, &h, sizeof(h));
    struct TOPO_GROUP_NODE nq = {0};
    length = topo_node_serialize(node, &nq, TOPO_FLAG_SEND_SUBNETS,
                                wire + sizeof(h), sizeof(wire) - sizeof(h), 0); assert(length > 0);
    packet(conn, wire, sizeof(h) + length);
}

static void receive(struct ETCP_CONN* conn, uint64_t group_id, uint8_t cmd, uint64_t local_epoch, size_t length) {
    struct TOPOMSG_JOIN_GROUP msg = { .protocol_version = htons(TOPO_GROUP_PROTOCOL_VERSION), .h = { .cmd = ETCP_ID_TOPO_ENTRY, .subcmd = cmd,
        .src_epoch = group_id + 1000, .dst_epoch = local_epoch }, .group_type = TOPO_GROUP_TYPE_UTUN };
    topo_group_utun_wire_id(msg.h.group_key, group_id);
    assert(length <= sizeof(msg));
    struct ll_entry* packet = ll_alloc_lldgram(sizeof(msg)); assert(packet);
    memcpy(packet->dgram, &msg, sizeof(msg)); packet->len = length;
    conn->instance->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY](conn, packet);
}

static void accept_join(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    receive(conn, group->group_id, TOPO_SUBCMD_JOIN_GROUP, 0, sizeof(struct TOPOMSG_JOIN_GROUP));
    receive(conn, group->group_id, TOPO_SUBCMD_JOIN_ACCEPT, peer(group)->local_epoch, sizeof(struct TOPOMSG_JOIN_GROUP));
    assert(peer(group)->accepted);
}

static void complete(struct TOPO_GROUP* group, struct ETCP_CONN* conn, uint64_t local_epoch) {
    receive(conn, group->group_id, TOPO_SUBCMD_TABLE_COMPLETE, local_epoch, sizeof(struct TOPOMSG_HEADER));
}

static void request(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    receive(conn, group->group_id, TOPO_SUBCMD_REQUEST_TABLE, peer(group)->local_epoch, sizeof(struct TOPOMSG_HEADER));
    drain(conn);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua,
        "[global]\n"
        "my_private_key=7fcd000000000000000000000000000000000000000000000000000000000000\n"
        "my_public_key=54c93c4dbd458d70a0e113341df80b18569f798665298422a6f87fb758e3fb0c\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n");
    assert(inst && utun_instance_init(inst) == 0);
    struct TOPO_GROUP* a = topo_groups_create_group(inst->topo_groups, 42, TOPO_GROUP_TYPE_UTUN, NULL);
    struct TOPO_GROUP* b = topo_groups_create_group(inst->topo_groups, 43, TOPO_GROUP_TYPE_UTUN, NULL);
    assert(a && b);
    uint8_t x_private[32], seed[32] = {0xcb, 0x66, 0x03};
    struct TOPO_ADDR4 addr = { .addr = {127, 0, 0, 1}, .port = 9, .protocol = TOPO_PROTO_UDP };
    struct TOPO_NODE node = { .v4_addrs = &addr };
    assert(sc_identity_from_seed(seed, node.public_key, x_private, node.ed25519_public_key) == SC_OK);
    node.node_id = sc_derive_node_id_from_pubkey(node.public_key);
    /* Первая пара из generate_short31_collisions.py, ключи с разными ETCP ID. */
    uint32_t short31;
    assert(!topo_address_short(node.ed25519_public_key, &short31) && short31 == 0x37ff42e8);
    assert(node.node_id != inst->node_id && topo_address_find(a, short31, NULL) == a->local_node);
    uint8_t field[33];
    struct topo_wire_address parsed;
    assert(topo_address_write(field, sizeof(field), a, inst->my_ed25519_pubkey, TOPO_ADDRESS_COMPACT) == 4);
    assert(!memcmp(field, (uint8_t[]){0x37, 0xff, 0x42, 0xe8}, 4));
    assert(topo_address_read(field, 4, TOPO_ADDRESS_COMPACT, &parsed) == 4 && !parsed.ed && parsed.short31 == short31);
    assert(topo_address_read(field, 3, TOPO_ADDRESS_COMPACT, &parsed) < 0 && parsed.short31 == short31);
    assert(topo_address_read(field, sizeof(field), TOPO_ADDRESS_FULL, &parsed) < 0);
    assert(topo_address_write(field, sizeof(field), a, node.ed25519_public_key, TOPO_ADDRESS_COMPACT) == 33);
    assert(field[0] == 0x80 && !memcmp(field + 1, node.ed25519_public_key, 32));
    assert(topo_address_read(field, 33, TOPO_ADDRESS_FULL, &parsed) == 33 && parsed.ed == field + 1 && parsed.short31 == short31);
    assert(topo_address_read(field, 32, TOPO_ADDRESS_COMPACT, &parsed) < 0);
    field[0] = 0x81;
    assert(topo_address_read(field, sizeof(field), TOPO_ADDRESS_COMPACT, &parsed) < 0);
    struct NODE_CONN_DIRECT* owner = NULL;
    assert(node_conn_direct_open_node(inst, node.node_id, NULL, NULL, &owner, &node, NULL) == NCD_NEW);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(owner); assert(conn);
    /* Детерминированная доставка управляющих пакетов через штатный BGP receiver.
     * Event loop не запускаем: транспортный handshake проверяют интеграционные тесты. */
    queue_set_callback(conn->send_input_q, NULL, NULL);
    conn->links_up = 1;
    inst->ntp.startup_wait = 1;
    int queued = queue_entry_count(conn->send_input_q);
    assert(topo_group_new_conn(a, conn) == 0 && topo_group_new_conn(b, conn) == 0);
    assert(queue_entry_count(conn->send_input_q) == queued);
    assert(!peer(a)->tx_head && !peer(b)->tx_head);
    receive(conn, a->group_id, TOPO_SUBCMD_JOIN_GROUP, 0, sizeof(struct TOPOMSG_JOIN_GROUP));
    assert(!peer(a)->accepted && !peer(a)->peer_epoch);
    inst->ntp.startup_wait = 0;
    topo_groups_time_ready(inst);
    assert(peer(a)->tx_head && peer(b)->tx_head); // общий FIFO передаст пакеты при следующем poll
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "regression: initial time wait holds BGP and resumes on existing transport");
    uint64_t a_id = peer(a)->local_epoch, b_id = peer(b)->local_epoch;
    assert(a_id && b_id && a_id != b_id);
    assert(drain(conn) == 2); /* Начальные JOIN могли быть отброшены ожидающим NTP пиром. */
    receive(conn, a->group_id, TOPO_SUBCMD_JOIN_GROUP, 0, sizeof(struct TOPOMSG_JOIN_GROUP));
    assert(drain(conn) == 1 && peer(a)->local_epoch == a_id && !peer(a)->accepted);
    receive(conn, a->group_id, TOPO_SUBCMD_JOIN_GROUP, 0, sizeof(struct TOPOMSG_JOIN_GROUP));
    assert(drain(conn) == 0 && peer(a)->local_epoch == a_id && !peer(a)->accepted);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "regression: lost initial JOIN retries once on peer JOIN, duplicates preserve exchange");
    assert(!topo_group_peer_ready(a, node.node_id) && !topo_group_peer_ready(b, node.node_id));
    complete(a, conn, a_id);
    assert(!peer(a)->table_received); /* no topology before ACCEPT */
    accept_join(a, conn); accept_join(b, conn);
    complete(a, conn, b_id);
    receive(conn, a->group_id, TOPO_SUBCMD_TABLE_COMPLETE, a_id, 2);
    assert(!peer(a)->table_received);
    complete(a, conn, a_id);
    assert(peer(a)->table_received && !topo_group_peer_ready(a, node.node_id));
    request(a, conn);
    assert(topo_group_peer_ready(a, node.node_id) && !topo_group_peer_ready(b, node.node_id));
    assert(topo_group_new_conn(a, conn) == 0 && peer(a)->local_epoch == a_id);
    receive(conn, a->group_id, TOPO_SUBCMD_JOIN_GROUP, 0, sizeof(struct TOPOMSG_JOIN_GROUP));
    assert(peer(a)->local_epoch == a_id);
    assert(queue_entry_count(a->senders_list) == 1 && topo_group_peer_ready(a, node.node_id));
    /* Другой порядок доставки: сначала наш снимок, потом подтверждение чужого. */
    request(b, conn);
    assert(!topo_group_peer_ready(b, node.node_id));
    complete(b, conn, b_id);
    assert(topo_group_peer_ready(b, node.node_id));

    nodeinfo(a, conn, &node, seed, a_id, 1);
    assert(topo_node_find_by_id(a, node.node_id)->last_timestamp == 1);
    /* Отзыв альтернативы не должен уничтожать сохранившийся прямой путь. */
    struct ETCP_CONN relay = { .instance = inst, .peer_node_id = 0x1234, .links_up = 1 };
    struct topo_hop relay_hops[] = {{.id=node.node_id}, {.id=relay.peer_node_id}};
    struct TOPO_GROUP_NODE* routed = topo_node_find_by_id(a, node.node_id);
    assert(topo_address_write(field, sizeof(field), a, inst->my_ed25519_pubkey, TOPO_ADDRESS_COMPACT) == 33);
    assert(topo_address_write(field, sizeof(field), a, node.ed25519_public_key, TOPO_ADDRESS_COMPACT) == 33);
    assert(routed->short_ambiguous && a->local_node->short_ambiguous && !topo_address_find(a, short31, NULL));
    assert(topo_address_find(a, short31, node.ed25519_public_key) == routed);
    assert(topo_address_find(a, short31, inst->my_ed25519_pubkey) == a->local_node);
    assert(topo_address_find(b, short31, NULL) == b->local_node);
    assert(topo_group_add_path(routed, &relay, relay_hops, 2, 10) == 0);
    struct TOPOMSG_WITHDRAW_PKT alternate_wd = {0};
    assert(topo_group_process_withdraw(a, &relay, (const uint8_t*)&alternate_wd, sizeof(alternate_wd)) < 0);
    memcpy(alternate_wd.ed25519, inst->my_ed25519_pubkey, sizeof(alternate_wd.ed25519));
    assert(!topo_group_process_withdraw(a, &relay, (const uint8_t*)&alternate_wd, sizeof(alternate_wd)));
    assert(queue_entry_count(routed->paths) == 2);
    memcpy(alternate_wd.ed25519, node.ed25519_public_key, sizeof(alternate_wd.ed25519));
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "regression: withdraw one of two paths node=%016llx paths=%d",
               (unsigned long long)node.node_id, queue_entry_count(routed->paths));
    assert(topo_group_process_withdraw(a, &relay, (const uint8_t*)&alternate_wd, sizeof(alternate_wd)) == 0);
    assert(topo_node_find_by_id(a, node.node_id) == routed);
    assert(queue_entry_count(routed->paths) == 1 && topo_group_find_conn_for_node(a, node.node_id) == conn);
    assert(topo_group_process_withdraw(a, &relay, (const uint8_t*)&alternate_wd, sizeof(alternate_wd)) == 0);
    assert(topo_node_find_by_id(a, node.node_id) == routed && queue_entry_count(routed->paths) == 1);
#ifdef __linux__
    /* Поздний OOM не меняет подписанную запись, per-group поля и предыдущий путь. */
    struct ll_queue* previous_paths = routed->paths;
    struct TOPO_NODE* previous_record = topo_node_registry_find(inst->topo_groups, node.node_id);
    uint32_t previous_refs = previous_record->group_ref_count;
    fail_path = 1;
    nodeinfo(a, conn, &node, seed, a_id, 2);
    assert(!fail_path && peer(a)->tx_failed && !topo_group_peer_ready(a, node.node_id));
    assert(topo_node_find_by_id(a, node.node_id) == routed && routed->paths == previous_paths);
    assert(routed->last_timestamp == 1 && previous_record->timestamp == 1 && previous_record->group_ref_count == previous_refs);
    assert(queue_entry_count(routed->paths) == 1 && topo_group_find_conn_for_node(a, node.node_id) == conn);
#endif
    etcp_fire_conn_status(conn, ETCP_CONN_STATUS_REINIT);
    assert(topo_group_find_conn_for_node(a, node.node_id) == conn); /* REINIT preserves known paths */
    assert(peer(a)->local_epoch != a_id && peer(b)->local_epoch != b_id);
    assert(!topo_group_peer_ready(a, node.node_id) && !topo_group_peer_ready(b, node.node_id));
    complete(a, conn, a_id); complete(b, conn, b_id);
    assert(!peer(a)->table_received && !peer(b)->table_received);
    accept_join(a, conn);
    /* Ошибка отправки снимка не разрешает TABLE_COMPLETE и READY. */
    int saved_limit = conn->send_input_q->size_limit;
    conn->send_input_q->size_limit = 0;
    request(a, conn);
    assert(!peer(a)->table_sent);
    assert(peer(a)->tx_failed);
    conn->send_input_q->size_limit = saved_limit;
    /* Watchdog must recover a failed sender without a transport status callback. */
    uint64_t failed_epoch = peer(a)->local_epoch;
    peer(a)->progress = get_time_tb() - TOPO_GROUP_SYNC_TIMEOUT_MS * 10ULL - 1;
    uint64_t deadline = get_time_tb() + 10000;
    while (peer(a)->local_epoch == failed_epoch && get_time_tb() < deadline) uasync_poll(ua, 10);
    assert(peer(a)->local_epoch != failed_epoch && !peer(a)->tx_failed);
    assert(!topo_node_find_by_id(a, node.node_id)); /* incomplete snapshot expired */
    accept_join(a, conn);
    nodeinfo(a, conn, &node, seed, peer(a)->local_epoch, 1);
    complete(a, conn, peer(a)->local_epoch); request(a, conn);
    assert(topo_group_peer_ready(a, node.node_id));

    nodeinfo(a, conn, &node, seed, a_id, 2);
    assert(topo_node_find_by_id(a, node.node_id)->last_timestamp == 1);
    nodeinfo(a, conn, &node, seed, peer(a)->local_epoch, 2);
    assert(topo_node_find_by_id(a, node.node_id)->last_timestamp == 2);
    struct TOPOMSG_WITHDRAW_PKT wd = { .h = { .cmd = ETCP_ID_TOPO_ENTRY, .subcmd = TOPO_SUBCMD_WITHDRAW,
        .src_epoch = a->group_id + 1000, .dst_epoch = a_id } };
    memcpy(wd.ed25519, node.ed25519_public_key, sizeof(wd.ed25519));
    memcpy(wd.h.group_key, topo_group_wire_id(a), 32);
    packet(conn, &wd, sizeof(wd));
    assert(topo_node_find_by_id(a, node.node_id));
    wd.h.dst_epoch = peer(a)->local_epoch; packet(conn, &wd, sizeof(wd));
    assert(!topo_node_find_by_id(a, node.node_id));
    assert(topo_address_find(a, short31, NULL) == a->local_node && !a->local_node->short_ambiguous);
    /* Same identity in a new exchange confirms a path; omission removes only that peer's path. */
    nodeinfo(a, conn, &node, seed, peer(a)->local_epoch, 2);
    etcp_fire_conn_status(conn, ETCP_CONN_STATUS_REINIT); accept_join(a, conn);
    nodeinfo(a, conn, &node, seed, peer(a)->local_epoch, 2);
    complete(a, conn, peer(a)->local_epoch); request(a, conn);
    assert(topo_group_find_conn_for_node(a, node.node_id) == conn && !peer(a)->exchange_timer);
    routed = topo_node_find_by_id(a, node.node_id);
    assert(topo_group_add_path(routed, &relay, relay_hops, 2, 10) == 0);
    etcp_fire_conn_status(conn, ETCP_CONN_STATUS_REINIT); accept_join(a, conn);
    complete(a, conn, peer(a)->local_epoch); request(a, conn);
    assert(topo_group_find_conn_for_node(a, node.node_id) == &relay && queue_entry_count(routed->paths) == 1);
    assert(topo_group_process_withdraw(a, &relay, (const uint8_t*)&alternate_wd, sizeof(alternate_wd)) == 0);
    assert(!topo_node_find_by_id(a, node.node_id));
    a_id = peer(a)->local_epoch;
    topo_group_remove_conn(a, conn, TOPO_REMOVE_LOCAL_LEAVE);
    complete(a, conn, a_id);
    assert(!topo_group_peer_ready(a, node.node_id));
    assert(topo_group_new_conn(a, conn) == 0 && peer(a)->local_epoch != a_id);
    receive(conn, a->group_id, TOPO_SUBCMD_JOIN_ACCEPT, a_id, sizeof(struct TOPOMSG_JOIN_GROUP));
    assert(!peer(a)->accepted);
    accept_join(a, conn);
    receive(conn, a->group_id, TOPO_SUBCMD_LEAVE_GROUP, a_id, sizeof(struct TOPOMSG_HEADER));
    assert(peer(a)->accepted); /* old LEAVE cannot detach the new session */
    complete(a, conn, a_id); request(a, conn);
    assert(!topo_group_peer_ready(a, node.node_id));
    complete(a, conn, peer(a)->local_epoch);
    assert(topo_group_peer_ready(a, node.node_id) && !topo_group_peer_ready(b, node.node_id));
    receive(conn, b->group_id, TOPO_SUBCMD_JOIN_REJECT, b_id, sizeof(struct TOPOMSG_JOIN_GROUP));
    assert(b->senders_list->head);
    receive(conn, b->group_id, TOPO_SUBCMD_JOIN_REJECT, peer(b)->local_epoch, sizeof(struct TOPOMSG_JOIN_GROUP));
    assert(!b->senders_list->head && !conn->close_requested);
    /* Cancellation while only JOIN has been received must not leave a half-open peer. */
    receive(conn, b->group_id, TOPO_SUBCMD_JOIN_GROUP, 0, sizeof(struct TOPOMSG_JOIN_GROUP));
    assert(peer(b)->peer_epoch && !peer(b)->accepted);
    receive(conn, b->group_id, TOPO_SUBCMD_LEAVE_GROUP, 0, sizeof(struct TOPOMSG_HEADER));
    assert(!b->senders_list->head);
    /* Ошибка версии блокирует только b, сохраняет результат после удаления peer. */
    struct TOPOMSG_JOIN_GROUP incompatible = { .h = { .cmd = ETCP_ID_TOPO_ENTRY,
        .subcmd = TOPO_SUBCMD_JOIN_GROUP, .src_epoch = 9000 },
        .group_type = TOPO_GROUP_TYPE_UTUN, .protocol_version = htons(TOPO_GROUP_PROTOCOL_VERSION + 1) };
    memcpy(incompatible.h.group_key, topo_group_wire_id(b), 32);
    packet(conn, &incompatible, sizeof(incompatible));
    const struct topo_peer_protocol* protocol = topo_group_peer_protocol(b, conn->peer_ed25519_pubkey);
    assert(protocol && protocol->state == 2 && protocol->version == TOPO_GROUP_PROTOCOL_VERSION + 1);
    assert(!b->senders_list->head && topo_group_peer_ready(a, conn->peer_node_id) && !conn->close_requested);
    /* Усечённый JOIN не трогает последнее состояние версии. */
    packet(conn, &incompatible, sizeof(incompatible) - sizeof(uint16_t));
    assert(protocol->state == 2 && protocol->version == TOPO_GROUP_PROTOCOL_VERSION + 1 && !b->senders_list->head);
    assert(topo_group_new_conn(b, conn) == 0);
    /* Устаревший ACCEPT с другой версией не меняет предупреждение. */
    incompatible.h.subcmd = TOPO_SUBCMD_JOIN_ACCEPT; incompatible.h.dst_epoch = peer(b)->local_epoch + 1;
    packet(conn, &incompatible, sizeof(incompatible));
    assert(protocol->version == TOPO_GROUP_PROTOCOL_VERSION + 1 && peer(b));
    accept_join(b, conn);
    assert(protocol->state == 1 && protocol->version == TOPO_GROUP_PROTOCOL_VERSION);
    incompatible.h.dst_epoch = peer(b)->local_epoch;
    incompatible.h.src_epoch = peer(b)->peer_epoch;
    packet(conn, &incompatible, sizeof(incompatible));
    assert(protocol->state == 2 && !b->senders_list->head && !conn->close_requested);
    conn->links_up = 0;
    assert(!topo_group_peer_ready(a, node.node_id));
    node_conn_direct_close(owner);
    inst->running = 0; utun_instance_destroy(inst);
    uasync_poll(ua, 0); uasync_destroy(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group exchange: isolation, duplicate attach, stale replies, send failure and REINIT passed");
    return 0;
}
