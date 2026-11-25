#include <assert.h>
#include <string.h>
#include "utun_instance.h"
#include "routing_layer/topo_group.h"
#include "transport_layer/etcp.h"
#include "transport_layer/node_conn_direct.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

static void check_deliberate_leave(struct TOPO_GROUP* group, struct ETCP_CONN* conn, enum topo_group_remove_reason reason) {
    assert(topo_group_new_conn(group, conn) == 0);
    struct SC_MYKEYS keys; assert(sc_generate_keypair(&keys) == SC_OK);
    struct TOPO_NODE* node = u_calloc(1, sizeof(*node)); assert(node);
    memcpy(node->public_key, keys.public_key, SC_PUBKEY_SIZE);
    uint64_t id = node->node_id = sc_derive_node_id_from_pubkey(node->public_key);
    assert(topo_node_registry_store(group->instance->topo_groups, node));
    struct ll_entry* entry = queue_entry_new(sizeof(struct TOPO_GROUP_NODE) - sizeof(struct ll_entry)); assert(entry);
    struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)entry;
    nq->identity = node; nq->node_id = id;
    struct topo_hop hops[] = {{.id=id}, {.id=conn->peer_node_id}};
    assert(topo_group_add_path(nq, conn, hops, 2, 100) == 0);
    assert(queue_data_put_with_index(group->nodes, entry) == 0);
    topo_group_remove_conn(group, conn, reason);
    assert(!topo_node_find_by_id(group, id));
    assert(!group->recovery && !topo_node_registry_find(group->instance->topo_groups, id));
    assert(!conn->close_requested);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua,
        "[global]\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n");
    assert(inst && utun_instance_init(inst) == 0);
    struct TOPO_GROUP* a = topo_groups_create_group(inst->topo_groups, 42, TOPO_GROUP_TYPE_UTUN, NULL);
    struct TOPO_GROUP* b = topo_groups_create_group(inst->topo_groups, 43, TOPO_GROUP_TYPE_UTUN, NULL);
    assert(a && b);
    struct SC_MYKEYS keys; assert(sc_generate_keypair(&keys) == SC_OK);
    struct TOPO_ADDR4 addr = { .addr = {127, 0, 0, 1}, .port = 9, .protocol = TOPO_PROTO_UDP };
    struct TOPO_NODE node = { .v4_addrs = &addr };
    memcpy(node.public_key, keys.public_key, sizeof(node.public_key));
    node.node_id = sc_derive_node_id_from_pubkey(node.public_key);
    struct NODE_CONN_DIRECT* owner = NULL;
    assert(node_conn_direct_open_node(inst, node.node_id, NULL, NULL, &owner, &node, NULL) == NCD_NEW);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(owner); assert(conn);
    /* Создание группы при уже существующем транспорте читает payload записи conn. */
    struct TOPO_GROUP* c = topo_groups_create_group(inst->topo_groups, 44, TOPO_GROUP_TYPE_UTUN, NULL); assert(c);
    assert(queue_entry_count(c->senders_list) == 0);
    struct TOPO_PEER_REQUEST *first = NULL, *second = NULL;
    assert(topo_group_peer_open(c, node.node_id, &first) == 0);
    assert(topo_group_peer_open(c, node.node_id, &second) == 0);
    assert(topo_group_peer_phase(first) == TOPO_PEER_CONNECTING && topo_group_peer_phase(second) == TOPO_PEER_CONNECTING);
    assert(queue_entry_count(c->senders_list) == 1);
    topo_group_peer_close(first);
    assert(queue_entry_count(c->senders_list) == 1 && !conn->close_requested);
    topo_group_peer_close(second);
    assert(queue_entry_count(c->senders_list) == 0 && !conn->close_requested);
    check_deliberate_leave(c, conn, TOPO_REMOVE_LOCAL_LEAVE);
    check_deliberate_leave(c, conn, TOPO_REMOVE_REMOTE_LEAVE);
    check_deliberate_leave(c, conn, TOPO_REMOVE_MEMBER_INVALID);
    assert(topo_group_peer_open(c, node.node_id, &first) == 0);
    topo_groups_remove_group(inst->topo_groups, 44);
    assert(topo_group_peer_phase(first) == TOPO_PEER_FAILED);
    topo_group_peer_close(first);
    /* Проверяем владение независимо от доставки пакетов и ответа удалённого узла. */
    assert(topo_group_new_conn(a, conn) == 0);
    assert(topo_group_new_conn(a, conn) == 0 && queue_entry_count(a->senders_list) == 1);
    assert(topo_group_new_conn(b, conn) == 0);
    node_conn_direct_close(owner);
    assert(!conn->close_requested);
    topo_groups_remove_group(inst->topo_groups, 42);
    assert(!conn->close_requested && queue_entry_count(b->senders_list) == 1);
    topo_groups_remove_group(inst->topo_groups, 43);
    /* Без внешнего владельца обе уничтоженные группы обязаны отпустить транспорт. */
    assert(conn->close_requested && !inst->ncd_registry);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group ownership: duplicate attach, shared connection and final release passed");
    inst->running = 0; utun_instance_destroy(inst);
    uasync_poll(ua, 0); uasync_destroy(ua, 0);
    return 0;
}
