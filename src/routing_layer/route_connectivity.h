/* route_connectivity — прямые UDP/TCP-пробы адресов узла для оценки достижимости и RTT.
 * probe_node запускает серии из подходящих локальных сокетов и обновляет nq->connectivity;
 * результат используется топологией, сама проба не создаёт READY-сессию группы.
 * Состояние привязано к TOPO_GROUP_NODE: отменить пробы до удаления узла/группы. Все операции — uasync. */
#ifndef ROUTE_CONNECTIVITY_H
#define ROUTE_CONNECTIVITY_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include "topo_node.h"

struct UTUN_INSTANCE;
struct TOPO_GROUP;

/* Запустить асинхронные пробы известных адресов nq из registry; group — его группа.
 * UDP: до 8 сокетов, серия CONN_PROBE_COUNT с каждого до первого успешного сокета;
 * TCP: один STCP ping с REALITY-параметрами при наличии. RTT — 0.1 мс, UINT16_MAX неизвестен.
 * До 16 уникальных IP:port на семейство. Собственный/спящий узел и текущие пробы пропускаются;
 * NAT/standby могут отложить старт. UDP-кандидат хранится как ID+поколение: исчезновение endpoint
 * отменяет его серию без отрицательного результата; новые сокеты попадут в следующий запуск.
 * Результат — в nq->connectivity, завершение уведомляет topo callback (может удалить nq). */
void route_connectivity_probe_node(struct UTUN_INSTANCE* instance, struct TOPO_GROUP* group,
                                   struct TOPO_GROUP_NODE* nq);

/* Отменить UDP/TCP-пробы и отложенный старт, освободить контексты; NULL допустим.
 * Сбрасывает probe_status/pending_count, сохранённые RTT/status остаются. */
void route_connectivity_cancel_node(struct TOPO_GROUP_NODE* nq);

// Отменяет pending пробы всех узлов во всех группах instance (при destroy)
void route_connectivity_cancel_all(struct UTUN_INSTANCE* instance);

// Устанавливает таймаут одного probe-пинга (по умолчанию CONN_PROBE_TIMEOUT_MS).
// Используется тестами для ускорения probe-циклов; настройка общая для процесса, 0 игнорируется.
void route_connectivity_set_probe_timeout_ms(uint16_t ms);


#ifdef __cplusplus
}
#endif
#endif // ROUTE_CONNECTIVITY_H
