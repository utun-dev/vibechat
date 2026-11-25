/* Именованные сетевые endpoints тестов: сначала открыть сервер на порту 0, затем подставить его адрес в CFG_CLIENT.
 * Сокет остаётся открыт: helper не резервирует порт через временный fd и не меняет ключи пира.
 * Вызовы выполнять в потоке владельца instance, вне параллельных изменений сокетов/конфига.
 * Неверная конфигурация завершает тест через abort; возвращённые sockets/instances остаются у ядра. */
#ifndef TEST_NET_H
#define TEST_NET_H

#include "../src/utun_instance.h"
#include "../src/transport_layer/etcp_connections.h"
#include "../lib/debug_config.h"
#include "../lib/socket_compat.h"
#include <stdlib.h>
#include <string.h>

/* Производный идентификатор для тестов числового API транспорта. */
static inline uint64_t test_transport_id(const struct utun_config* cfg) {
    uint8_t x[32];
    if (!cfg || sc_hex_to_binary(cfg->global.my_public_key_hex, x, sizeof(x))) abort();
    return sc_derive_node_id_from_pubkey(x);
}

// Порт уже bound fd в host byte order; fd не закрывается. Нулевой/недоступный порт завершает тест через abort.
static inline uint16_t test_bound_port(socket_t fd) {
    uint16_t port = socket_get_bound_port(fd);
    if (!port) abort();
    return port;
}

// Найти listener по NUL-имени: local_addr/interface_addr должны содержать одинаковый ненулевой порт; иначе abort.
static inline struct ETCP_SOCKET* test_etcp_bound_socket(struct UTUN_INSTANCE* inst, const char* name) {
    for (struct ETCP_SOCKET* s = inst ? inst->etcp_sockets : NULL; s; s = s->next) {
        if (strcmp(s->name, name)) continue;
        if (ss_get_port(&s->local_addr) && ss_get_port(&s->local_addr) == ss_get_port(&s->interface_addr)) return s;
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "test fixture: socket %s has no valid bound endpoint", name); abort();
    }
    DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "test fixture: missing listener %s", name); abort();
}

/* Обновить remote_addr всех links первого CFG_CLIENT с именем client_name до запуска UTUN-сервиса (!utun_started).
 * peer/socket_name выбирают уже bound listener с конкретным адресом; family/transport должны совпадать.
 * Копируется только endpoint; peer key и настройки транспорта клиентской секции не меняются. */
static inline void test_etcp_set_peer(struct UTUN_INSTANCE* client, const char* client_name,
                                      struct UTUN_INSTANCE* peer, const char* socket_name) {
    struct ETCP_SOCKET* target = test_etcp_bound_socket(peer, socket_name);
    if (!client || !client->config || client->utun_started) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "test fixture: client %s must be configured before start", client_name); abort();
    }
    for (struct CFG_CLIENT* c = client->config->clients; c; c = c->next) {
        if (strcmp(c->name, client_name)) continue;
        if (!c->links) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "test fixture: client %s has no links", client_name); abort(); }
        for (struct CFG_CLIENT_LINK* link = c->links; link; link = link->next) {
            if (link->remote_addr.ss_family != target->interface_addr.ss_family ||
                (link->local_srv && link->local_srv->transport != target->is_tcp)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "test fixture: incompatible endpoints client=%s listener=%s", client_name, socket_name);
                abort();
            }
            link->remote_addr = target->interface_addr;
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "test fixture: client=%s listener=%s peer=%016llx port=%u",
                       client_name, socket_name, (unsigned long long)peer->node_id, ss_get_port(&link->remote_addr));
        }
        return;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "test fixture: missing client %s", client_name); abort();
}

#endif
