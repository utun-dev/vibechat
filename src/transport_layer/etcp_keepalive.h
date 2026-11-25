#ifndef ETCP_KEEPALIVE_H
#define ETCP_KEEPALIVE_H

#ifdef __cplusplus
extern "C" {
#endif

// подмодуль ETCP: keepalive-пакеты, таймер, адаптивный период и протокол
// спячки (sleep) на флагах самого keepalive-пакета.
// RX-timeout удерживает conn/link на время уведомлений. Close/удаление из callback прекращает обработку старого линка.

#include "etcp_connections.h"

/* ─── Протокол спячки поверх keepalive-пакета (code ETCP_KEEPALIVE = 0x08) ───
 *
 * Обычный keepalive (normal-режим) и спячка (standby) — раздельные механизмы
 * (разные таймеры, разный код):
 *
 *   normal: периодический flagless-keepalive (адаптивный период).
 *           Отправка активна только когда бодрствуют обе стороны.
 *
 *   standby (спящий узел): периодический flagless НЕ шлётся вовсе. На фазовых
 *           переходах duty-cycle (SLEEP 120с / ACTIVE 2с) шлётся анонс
 *           KA_FLAG_SLEEP с таймингом. В ACTIVE-окне анонс ретраится каждые
 *           keepalive_interval до ACK (лимит: сколько влезет в окно, но ≥
 *           KA_SLEEP_MIN_ATTEMPTS). Кумулятивные провалы (сброс по ACK) — при
 *           KA_SLEEP_MAX_FAILS → link down (детект мёртвого пира в спячке).
 *
 *   RX watchdog: отдельный конечный deadline работает в обоих режимах. Учитывает
 *           объявленный ACTIVE+SLEEP и keepalive_timeout; фазы и отправки его
 *           не продлевают. Пробуждение сразу сокращает срок до normal timeout.
 *
 * Формат пакета:
 *   data[0]     = ETCP_KEEPALIVE (0x08)
 *   data[1]     = flags (bit0 = KA_FLAG_SLEEP — «я сплю», bit1 = KA_FLAG_SLEEP_ACK — ответ)
 *   data[2..3]  = ka_period_ms (LE 16-bit) — база dead-link таймаута, мс
 *   data[4..5]  = active_remaining (LE 16-bit, x0.1с) — до SLEEP-фазы (0 = уже спим)
 *   data[6..7]  = sleep_duration (LE 16-bit, x0.1с) — длительность SLEEP-фазы
 *   далее      = зашифрованный нулевой padding; тело без SLEEP занимает 4 байта, с SLEEP — 8
 *
 * Протокол полностью симметричный (без привязки к STCP-роли is_server):
 * спящий шлёт анонс KA_FLAG_SLEEP, пир «затыкается» (гасит свой normal-keepalive)
 * и отвечает KA_FLAG_SLEEP_ACK. Спящий, получив ACK, снимает ретраи. В standby
 * обе стороны НЕ шлют периодические flagless-keepalive.
 *
 * По таймингу анонса пир ведёт фазу троттлинга duty-cycle (SLEEP-фаза → тормозим
 * протоколы, ACTIVE-окно → обмен).
 *
 * Пробуждение (форграунд): спящий шлёт flagless-keepalive — пир снимает standby
 * и фазу троттлинга, возвращается в normal-режим.
 */

// Флаги keepalive-пакета (data[1])
#define KA_FLAG_SLEEP                  0x01   // «я сплю» (анонс, с таймингом)
#define KA_FLAG_SLEEP_ACK              0x02   // «подтверждаю твой сон» (ответ на анонс)

// Параметры анонса спячки
#define KA_SLEEP_MIN_ATTEMPTS          5      // минимум попыток анонса в ACTIVE-окне
#define KA_SLEEP_MAX_FAILS             10     // провалов анонса подряд → link down

// --- перенесены из etcp_connections.c (имена сохранены) ---
uint16_t negotiate_keepalive(uint8_t my_type, uint16_t my_ka,
                             uint8_t peer_type, uint16_t peer_ka);
void etcp_link_send_keepalive(struct ETCP_LINK* link);
void start_keepalive_timer(struct ETCP_LINK* link);

// Обновить срок ожидания RX после принятого пакета или изменения режима сна.
void etcp_keepalive_watch_rx(struct ETCP_LINK* link);

// --- новое ---
// Обработка keepalive-пакетов (ETCP_KEEPALIVE) из etcp_packet_decrypted.
// Возвращает 1 если пакет обработан (и освобождён), 0 — если это не keepalive.
int etcp_keepalive_on_recv(struct ETCP_DGRAM* pkt);

// Подписка на фактический режим/фазу standby; без duty-cycle — на активность клиента.
// Вызывается из init_connections.
void etcp_keepalive_register(struct UTUN_INSTANCE* inst);

// После INIT/INIT_RESPONSE, TCP handshake и reconnect сразу анонсирует текущий режим/фазу.
// В standby запускает sleep-анонс с ретраями UDP; в normal — обычный keepalive.
void etcp_keepalive_link_ready(struct ETCP_LINK* link);

#ifdef __cplusplus
}
#endif
#endif // ETCP_KEEPALIVE_H
