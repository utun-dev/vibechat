/* topo_recovery — восстановление путей группы после потери транспорта.
 * Собирает потерянные узлы, пробует кандидатов через групповые запросы и ждёт
 * нужных маршрутов через READY-пиров. Контекст принадлежит группе; работа — в uasync. */
#ifndef TOPO_RECOVERY_H
#define TOPO_RECOVERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct TOPO_GROUP;
struct TOPO_NODE;
struct TOPO_RECOVERY_CTX;

#define TOPO_RECOVERY_CONNECT_TIMEOUT_MS 2000
#define TOPO_RECOVERY_SYNC_TIMEOUT_MS 5000

/* Один recovery-контекст на группу, одна текущая попытка присоединения.
 * add_node сохраняет цель и кандидатуру до удаления registry ref;
 * start вызывается после удаления всех путей через потерянного пира.
 * Приоритет: бывшие downstream next-hop, затем минимальный RTT, затем полный Ed25519.
 * Кандидат проверяется не более одного раза за цикл. Транспорт принадлежит
 * групповой сессии: recovery владеет только отменяемым TOPO_PEER_REQUEST.
 *
 * Успех — все потерянные узлы снова имеют живой путь через READY-пира этой группы.
 * UP и даже READY без нужных маршрутов не завершают recovery. Частичный результат
 * сохраняет полезное присоединение и продолжает оставшиеся цели. Маршрут через
 * другого READY-пира также закрывает цель. При исчерпании кандидатов оставшиеся
 * цели логируются, контекст завершается без скрытого повторного цикла.
 *
 * CONNECTING ограничен отдельным таймаутом; SYNCING — временем без прогресса.
 * NODEINFO/смена состояния лишь планируют проверку через call_soon: текущий
 * BGP callback завершается до изменения попытки или освобождения контекста.
 * Остановка группы отменяет recovery до освобождения сессий и таблицы узлов. */
/* Удержать полную запись потерянного узла. downstream — непосредственный кандидат; rtt в единицах 0.1 ms. */
void topo_recovery_add_node(struct TOPO_GROUP* group, struct TOPO_NODE* identity, int downstream, uint16_t rtt);
/* Запустить собранные попытки после удаления нерабочих путей. */
void topo_recovery_start(struct TOPO_GROUP* group);
/* Отложить проверку прогресса; повторные уведомления объединяются. */
void topo_recovery_changed(struct TOPO_GROUP* group);
/* Отменить попытку и освободить контекст; готовые сессии остаются у группы. */
void topo_recovery_cancel_all(struct TOPO_GROUP* group);

#ifdef __cplusplus
}
#endif

#endif
