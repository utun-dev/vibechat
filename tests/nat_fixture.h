/* NAT-стенд с настоящими ядрами/UDP/NCD и одной общей uasync: init → node/connect → cleanup.
 * До 8 узлов и 24 NCD handles; фикстура владеет ими, caller получает заимствованные указатели.
 * Запускает core и создаёт UTUN topo-группу, но не UTUN/chat сервисы; TUR3 позволяет независимо выбирать UDP/TCP.
 * Управление только в общем uasync-потоке.
 * Проверки и обязательные вызовы находятся в assert: собирать без NDEBUG. Отказы завершают тест.
 * Cleanup вызывается один раз после завершения внешних операций; раннее уничтожение узлов/handles вручную запрещено. */
#ifndef NAT_FIXTURE_H
#define NAT_FIXTURE_H

#ifndef NAT_FIXTURE_GLOBAL_OPTIONS
#define NAT_FIXTURE_GLOBAL_OPTIONS ""
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "test_net.h"
#include "utun_instance.h"
#include "config_parser.h"
#include "transport_layer/etcp.h"
#include "transport_layer/node_conn_direct.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/nat_detection.h"
#include "routing_layer/route_ping.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"

struct nat_fixture {
    struct UASYNC *ua;                    // Собственный event loop для всех узлов.
    struct UTUN_INSTANCE *nodes[8];       // Собственные ядра в порядке создания.
    struct NODE_CONN_DIRECT *handles[24]; // Собственные NCD handles явных соединений.
    unsigned node_count, handle_count;    // Число занятых элементов соответствующих массивов.
    int transport_only;                  // Не запускать UTUN/BGP после UP: проверка только явного подключения.
    size_t allocations;                  // Снимок u_* allocations до init для проверки полного cleanup.
};

// Обнулить свежую фикстуру, сохранить allocation baseline и создать uasync; повторно поверх живого стенда не вызывать.
static inline void nat_fixture_init(struct nat_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture)); fixture->allocations = u_get_allocated_count();
    fixture->ua = uasync_create(); assert(fixture->ua);
}

/* Создать/запустить узел: ip — локальный bind IP, порт UDP выбирает ОС; keys=NULL генерирует новую X25519-пару.
 * extra — добавляемый INI-текст либо NULL, может задавать дополнительные серверы/настройки. auto_sockets=no, NTP выключен.
 * Для NAT разрешаются локальные адреса, создаётся UTUN-группа; возвращённый instance принадлежит fixture. */
struct nat_device_keys { uint8_t public_key[32], seed[32]; };
static inline struct UTUN_INSTANCE *nat_fixture_node(struct nat_fixture *fixture, const char *ip,
                                                     const struct nat_device_keys *keys, const char *extra) {
    struct nat_device_keys generated;
    if (!keys) { assert(sc_identity_generate_config(generated.public_key, generated.seed) == SC_OK); keys = &generated; }
    char private_key[65], public_key[65], config[4096];
    for (unsigned i = 0; i < 32; i++) {
        snprintf(private_key + i * 2, 3, "%02x", keys->seed[i]);
        snprintf(public_key + i * 2, 3, "%02x", keys->public_key[i]);
    }
    snprintf(config, sizeof(config), "[global]\nmy_private_key=%s\nmy_public_key=%s\nauto_sockets=no\nconnection_order=TUR3\n"
             NAT_FIXTURE_GLOBAL_OPTIONS
             "[ntp]\nenabled=no\n[server:udp]\naddr=%s:0\ntype=public\n[allowed_keys]\nallow_all=1\n%s",
             private_key, public_key, ip, extra ? extra : "");
    struct UTUN_INSTANCE *node = utun_instance_create_from_str(fixture->ua, config);
    assert(node && fixture->node_count < 8 && utun_core_start(node) == 0);
    nat_detection_set_allow_local(node->nat_det, 1);
    assert(topo_groups_create_group(node->topo_groups, TOPO_GROUP_UTUN, TOPO_GROUP_TYPE_UTUN, ""));
    fixture->nodes[fixture->node_count++] = node; return node;
}

/* Добавляет привязанный UDP-сокет; вызывающий публикует новый NODEINFO, когда адреса готовы. */
static inline struct ETCP_SOCKET *nat_fixture_add_public_socket(struct UTUN_INSTANCE *node, const char *name, const char *ip) {
    struct CFG_SERVER server = {.type = CFG_SERVER_TYPE_PUBLIC, .mtu = 1500, .fib = -1};
    struct sockaddr_in *address = (struct sockaddr_in *)&server.ip;
    address->sin_family = AF_INET;
    assert(inet_pton(AF_INET, ip, &address->sin_addr) == 1);
    snprintf(server.name, sizeof(server.name), "%s", name);
    struct ETCP_SOCKET *socket = etcp_socket_add(node, &server);
    assert(socket);
    return socket;
}

/* Явно соединить from→to через локальный listener from с именем source (UDP либо TCP).
 * Удерживает собственный NCD handle, ждёт готовый линк выбранного сокета до 5с, затем READY UTUN-группы ещё до 5с.
 * transport_only пропускает UTUN/BGP; работающий линк другого сокета не завершает ожидание выбранного транспорта.
 * Возвращает заимствованный ETCP_CONN; ожидания обслуживают общий uasync и не подменяют часы. */
static inline struct ETCP_CONN *nat_fixture_connect(struct nat_fixture *fixture, struct UTUN_INSTANCE *from,
                                                    const char *source, struct UTUN_INSTANCE *to) {
    assert(fixture->handle_count < 24);
    struct TOPO_NODE *peer = topo_node_registry_lookup(to->topo_groups, to->node_id, to->my_keys.public_key); assert(peer);
    struct ETCP_SOCKET *source_socket = test_etcp_bound_socket(from, source);
    struct NODE_CONN_DIRECT **handle = &fixture->handles[fixture->handle_count++];
    assert(node_conn_direct_open_node(from, to->node_id, NULL, NULL, handle, peer, source_socket) != NCD_ERR);
    uint64_t end = get_time_tb() + 50000;
    struct ETCP_CONN *connection = NULL;
    struct ETCP_LINK *selected = NULL;
    while (get_time_tb() < end) {
        connection = node_conn_direct_get_conn(*handle);
        for (selected = connection ? connection->links : NULL; selected; selected = selected->next)
            if (selected->conn == source_socket && selected->initialized && selected->link_status && !selected->closing) break;
        if (selected) break;
        uasync_poll(fixture->ua, 10);
    }
    if (!selected) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "fixture transport timed out from=%016llx to=%016llx socket=%s protocol=%s "
                    "state=%d links_up=%u policy=%s", (unsigned long long)from->node_id, (unsigned long long)to->node_id,
                    source, source_socket->is_tcp ? "TCP" : "UDP", connection ? connection->state : -1,
                    connection ? connection->links_up : 0, to->config->global.connection_order);
        for (struct ETCP_LINK *link = connection ? connection->links : NULL; link; link = link->next)
            DEBUG_ERROR(DEBUG_CATEGORY_NAT, "fixture link socket=%s protocol=%s initialized=%u up=%u closing=%u",
                        link->conn ? link->conn->name : "none", link->is_tcp ? "TCP" : "UDP",
                        link->initialized, link->link_status, link->closing);
        abort();
    }
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "fixture transport ready from=%016llx to=%016llx socket=%s protocol=%s link=%u",
               (unsigned long long)from->node_id, (unsigned long long)to->node_id, source,
               source_socket->is_tcp ? "TCP" : "UDP", selected->local_link_id);
    if (fixture->transport_only) return connection;
    struct TOPO_GROUP *group = topo_groups_find(from->topo_groups, TOPO_GROUP_UTUN);
    struct TOPO_GROUP *remote = topo_groups_find(to->topo_groups, TOPO_GROUP_UTUN);
    assert(group && remote && !topo_group_new_conn(group, connection));
    end = get_time_tb() + 50000;
    while (get_time_tb() < end) {
        if (topo_group_peer_ready(group, to->node_id) && topo_group_peer_ready(remote, from->node_id)) return connection;
        uasync_poll(fixture->ua, 10);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_NAT, "fixture group negotiation timed out from=%016llx to=%016llx",
                (unsigned long long)from->node_id, (unsigned long long)to->node_id); abort();
}

/* Force-close handles → destroy всех узлов → poll(0) → проверка timer/socket balance → destroy uasync.
 * Сравнить u_* allocations с baseline, при расхождении вывести утечки и завершить assert. Не обнуляет fixture. */
static inline void nat_fixture_cleanup(struct nat_fixture *fixture) {
    for (unsigned i = 0; i < fixture->handle_count; i++) node_conn_direct_force_close(fixture->handles[i]);
    for (unsigned i = 0; i < fixture->node_count; i++) utun_instance_destroy(fixture->nodes[i]);
    uasync_poll(fixture->ua, 0);
    assert(fixture->ua->timer_alloc_count == fixture->ua->timer_free_count);
    assert(fixture->ua->socket_alloc_count == fixture->ua->socket_free_count);
    uasync_destroy(fixture->ua, 0);
    if (u_get_allocated_count() != fixture->allocations) u_report_unfreed_blocks();
    assert(u_get_allocated_count() == fixture->allocations);
}

#endif
