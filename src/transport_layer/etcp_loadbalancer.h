/* etcp_loadbalancer — выбор доступного линка одного ETCP_CONN с учётом cwnd, inflight и pacing/shaper.
 * select_link возвращает заимствованный готовый линк или NULL. send шифрует/отправляет пакет и учитывает shaper,
 * link_ready вызывает link_ready_for_send_fn при готовности физического линка/shaper;
 * окно пробуждает etcp_conn_wake_sender без изменения UP/DOWN. Все вызовы — uasync-поток соединения.
 * Pacer допускает положительный долг строго меньше 1мс; неиспользованный кредит также ограничен 1мс.
 * Таймер ждёт первого допустимого TB: shaper_load_time_tb - SHAPER_BURST_DELAY_TB + 1, а не погашения всего долга.
 * Дробь меньше 1TB не меняет эту строгую границу; callback повторно проверяет готовность перед уведомлением. */
#ifndef ETCP_LOADBALANCER_H
#define ETCP_LOADBALANCER_H

#include "etcp.h"

#ifdef __cplusplus
extern "C" {
#endif

// Constants (moved here for visibility)
#define TIMEBASE_NS 100000ULL  // 0.1ms base
#define SUB_MODULO 1000000ULL  // Sub-nanotime modulo
#define DELTA_TIME_NS 10000ULL // Delta for inactivity
#define SHAPER_BURST_DELAY_TB 10ULL  // 1мс: строгая граница положительного долга и предел накопленного кредита

/* Применить pacing в B/s, пересчитать оставшийся долг/таймер по новой ставке и разбудить готовый отправитель.
 * 0 явно отключает pacing; положительная скорость не округляется до Kbit/s. При OOM таймера закрытие отложено. */
void etcp_loadbalancer_set_pacing_rate(struct ETCP_LINK* link, uint32_t bytes_per_second);

/* Заимствованный рабочий линк с минимальным inflight, round-robin при равенстве; NULL — нет подходящего.
 * data_len > 0 требует места для целого DATA и готовности shaper; 0 выбирает ACK без DATA-cwnd/pacing.
 * ACK уже ограничен входящим потоком/ACK_DELAY; исходящая оценка DATA не описывает полосу обратных подтверждений.
 * Burst обходит только pacing, индивидуальное окно DATA сохраняется. Поиск не меняет историю/RR и не резервирует окно.
 * Обнаруженное заполнение окна ставит cwnd_limited для последующего ACK-sample BBR. */
/* retry != NULL: сначала неиспользованные пути текущего обхода; занятый неиспользованный путь ждём,
 * пропуская пакет при сканировании send_q. Если все допустимые пути использованы, разрешён следующий обход.
 * При наличии initialized+UP без tx_stalled застрявшие исключены; если застряли все — доступны все UP-пути.
 * Прежний путь разрешён лишь при отсутствии других допустимых; иначе ждём альтернативу по cwnd/pacing.
 * Обычные DATA/ACK передают NULL. Успешный DATA-send фиксирует историю и RR; поиск не создаёт попытку или RTO. */
struct ETCP_LINK* etcp_loadbalancer_select_link(struct ETCP_CONN* etcp, uint32_t data_len, const struct INFLIGHT_PACKET* retry);

// Передать dgram с установленным link: после отправки (включая ошибку сокета) возвращает dgram в pkt_pool.
int etcp_loadbalancer_send(struct ETCP_DGRAM* dgram); // >=0 принят транспортом; <0 ошибка, резерв попытки снят

// Готовность физического линка/его shaper; для ACK/изменения окна использовать etcp_conn_wake_sender.
void loadbalancer_link_ready(struct ETCP_LINK* link);

// 1 — shaper готов или активен burst, 0 — NULL/ожидание таймера; физический статус и cwnd проверяет select_link.
int loadbalancer_link_can_send(struct ETCP_LINK* link);

// 1 — хотя бы один link_status=UP; 0 — NULL conn, нет линков или все DOWN. Готовность сессии не проверяется.
int etcp_loadbalancer_get_link_status(struct ETCP_CONN* etcp);

#ifdef __cplusplus
}
#endif

#endif // ETCP_LOADBALANCER_H
