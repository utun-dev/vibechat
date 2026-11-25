/* Односторонняя потеря прямых UDP-пакетов к выбранному peer для BGP-тестов, включая новые линки.
 * Обнулённый context → init → set(blocked, only, spared) → destroy до уничтожения instance.
 * Один владелец send_hook для пары (instance, peer); старый hook не сохраняется и не восстанавливается.
 * Использовать только прямые UDP-линки: разрешённые пакеты уходят в socket_sendto, STCP/SOCKS/эмуляторы не обслуживаются.
 * Все вызовы/счётчики в uasync-потоке. Это потеря исходящих пакетов, а не отключение fd/входящей стороны. */
#ifndef TEST_CONNECTION_LOSS_H
#define TEST_CONNECTION_LOSS_H

#include "etcp_api.h"
#include "etcp_connections.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"

/* Контекст заимствуется подпиской/hook и живёт до test_loss_destroy; init не обнуляет состояние/статистику. */
struct test_connection_loss {
    struct UTUN_INSTANCE* instance; // Заимствованный локальный instance, NULL после destroy.
    uint64_t peer_id;              // ID удалённого узла, чьи линки перехватываются.
    struct ETCP_LINK* only;        // NULL — все линки, иначе только этот; указатель не удерживает link.
    struct ETCP_LINK* spared;      // Этот линк пропускается даже при совпадении only; NULL — без исключения.
    int blocked;                  // Ненулевое значение включает заданную потерю.
    unsigned links;               // Накопленное число установок hook, не текущее количество живых линков.
    unsigned long long dropped;   // Число отброшенных отправок, не байты; сохраняется между set.
};

static ssize_t test_loss_send(socket_t fd, const void* buf, size_t len, const struct sockaddr* addr,
                              socklen_t addr_len, struct ETCP_LINK* link, void* arg) {
    struct test_connection_loss* loss = arg;
    if (loss->blocked && (!loss->only || loss->only == link) && loss->spared != link) {
        loss->dropped++;
        return (ssize_t)len;
    }
    return socket_sendto(fd, buf, len, addr, addr_len);
}

static void test_loss_link(struct ETCP_CONN* conn, struct ETCP_LINK* link, int old_state, int old_status, void* arg) {
    (void)old_state; (void)old_status;
    struct test_connection_loss* loss = arg;
    if (conn->peer_node_id != loss->peer_id || link->send_hook == test_loss_send) return;
    link->send_hook = test_loss_send; link->send_hook_ctx = loss; loss->links++;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "test partition: peer=%016llx attached link=%p total=%u blocked=%d",
               (unsigned long long)loss->peer_id, (void*)link, loss->links, loss->blocked);
}

// Прикрепить hook к существующим линкам и подписаться на новые; instance и loss обязаны жить до destroy.
static void test_loss_init(struct test_connection_loss* loss, struct UTUN_INSTANCE* instance, uint64_t peer_id) {
    loss->instance = instance; loss->peer_id = peer_id;
    etcp_add_link_status_cbk(instance, test_loss_link, loss);
    struct ETCP_CONN* conn = instance_find_conn(instance, peer_id);
    for (struct ETCP_LINK* link = conn ? conn->links : NULL; link; link = link->next)
        test_loss_link(conn, link, 0, 0, loss);
}

// Изменить фильтр без сброса статистики. Заимствованные only/spared сбрасывать до освобождения соответствующих линков.
static void test_loss_set(struct test_connection_loss* loss, int blocked, struct ETCP_LINK* only, struct ETCP_LINK* spared) {
    loss->blocked = blocked; loss->only = only; loss->spared = spared;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "test partition: peer=%016llx blocked=%d only=%p spared=%p links=%u dropped=%llu",
               (unsigned long long)loss->peer_id, blocked, (void*)only, (void*)spared, loss->links, loss->dropped);
}

// Снять свою подписку и свои hooks, instance оставить живым; повторный destroy допустим, loss не освобождается.
static void test_loss_destroy(struct test_connection_loss* loss) {
    if (!loss->instance) return;
    etcp_remove_link_status_cbk(loss->instance, test_loss_link, loss);
    struct ETCP_CONN* conn = instance_find_conn(loss->instance, loss->peer_id);
    for (struct ETCP_LINK* link = conn ? conn->links : NULL; link; link = link->next) {
        if (link->send_hook_ctx != loss) continue;
        link->send_hook = NULL; link->send_hook_ctx = NULL;
    }
    loss->instance = NULL;
}
#endif
