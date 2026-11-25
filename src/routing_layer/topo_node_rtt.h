/* RTT узла и endpoints: сбор в uasync, среднее минимумов между WRITE-транзакциями.
 * Средний RTT выбранного пути для GUI обновляется по измерениям/изменениям пути, без таймера и зависимости от SQL.
 * link_sample вызывается после каждого TIMESTAMP, probe_sample — после PING/PONG.
 * Один таймер/WRITE на узел, интервал минимум 1с. Запись через RW worker либо RW SQLite синхронного ядра;
 * отдельный READONLY worker не принимает записи. stop отменяет таймеры и отсоединяет SQL callbacks. */
#ifndef TOPO_NODE_RTT_H
#define TOPO_NODE_RTT_H

#include <stdint.h>
#include "../lib/socket_compat.h"

struct UTUN_INSTANCE;
struct ETCP_LINK;
struct TOPO_GROUP;
struct TOPO_GROUP_NODE;

#define TOPO_RTT_AVERAGE_PERIOD_TB 3000u /* Окно отображаемого RTT: 300 мс в единицах timebase. */

/* Учесть выбранный путь group/node; measurement=1 — новый замер локального соединения, включая повтор того же RTT.
 * measurement=0 учитывает только новую цепочку/сессию/изменившийся cumulative RTT. Только uasync, без callbacks.
 * Возврат: 1 — опубликованное значение изменилось, 0 — прежнее/нет данных; неверные аргументы логируются. */
int topo_node_rtt_average_update(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, int measurement);

struct topo_rtt_endpoint {
    uint8_t address[16]; /* Сетевые байты IPv4/IPv6; оставшиеся байты IPv4 нулевые. */
    uint8_t family;      /* 4/6; не локальный интерфейс и не socket_id. */
    uint8_t protocol;    /* TOPO_PROTO_UDP/TCP или TCP|REALITY. */
    uint16_t port;       /* Host order; endpoint без порта не измеряется. */
};

/* Учесть текущий RTT линка и минимумы живых линков узла/endpoint. NULL игнорируется. */
void topo_node_rtt_link_sample(struct ETCP_LINK* link);
/* Учесть ненулевую UDP-пробу; x — X25519 узла, addr — фактический удалённый endpoint.
 * Для узла/endpoint с измеренными рабочими линками проба не заменяет их оценку. */
void topo_node_rtt_probe_sample(struct UTUN_INSTANCE* inst, const uint8_t x[32], uint16_t rtt,
    const struct sockaddr_storage* addr);
/* Остановить сбор перед закрытием DB worker. Повтор допустим; новые замеры при destroying игнорируются. */
void topo_node_rtt_stop(struct UTUN_INSTANCE* inst);

#endif
