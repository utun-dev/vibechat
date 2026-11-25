/* Согласованный снимок всех ETCP-подключений для GUI; сбор только в uasync-потоке.
 * CHAT_EVT_NETWORK_STATS содержит native POD header, затем count пар conn + link[link_count].
 * Это внутреннее событие одного процесса, не сетевой протокол; структуры копируются через memcpy.
 * Все строки завершаются NUL, padding обнуляется. Получатель проверяет весь снимок до применения. */
#ifndef CHAT_NETWORK_STATS_H
#define CHAT_NETWORK_STATS_H

#include "../transport_layer/etcp_link_io.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct chat_network_stats_header {
    uint64_t request_id, at_ms; /* Корреляция запроса и монотонное время общего снимка. */
    uint32_t count, failed; /* Число подключений; failed=1 возвращает только header при отказе. */
};
struct chat_network_stats_conn {
    uint64_t id, peer_id; /* Lifetime ID объекта и node_id; peer_id может быть ещё неизвестен. */
    uint64_t tx_bytes, rx_bytes; /* Накопительные ETCP-байты всех линков, включая уже удалённые. */
    uint32_t link_count, up; /* Число следующих link-записей и наличие работающих линков. */
    char name[64]; /* Имя узла либо имя подключения до определения peer_id. */
};
struct chat_network_stats_link {
    uint64_t id, tx_bytes, rx_bytes, inflight; /* Поколение линка, lifetime трафик и физические DATA-байты в полёте. */
    struct etcp_link_snapshot description; /* Номер, транспорт, состояние и адреса для легенды. */
};
struct chat_network_stats_request {
    struct UTUN_INSTANCE* inst; /* Заимствованное ядро, привязывается мостом под mutex. */
    uint64_t request_id; /* Ненулевой ID; возвращается также при ошибке сбора. */
};
/* arg — один u_malloc-блок chat_network_stats_request; освобождается здесь, при отмене — мостом.
 * Результат — CHAT_EVT_NETWORK_STATS; при OOM/ошибке возвращается failed header без частичных данных. */
void chat_core_collect_network_stats_trampoline(void* arg);

#ifdef __cplusplus
}
#endif
#endif
