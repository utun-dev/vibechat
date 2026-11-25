/* Ограниченный аутентифицированный JOIN поверх BLE INVITE, без ETCP до членства.
 * create регистрирует provider ядра; connection/receive/run вызываются владельцем радио
 * только в uasync. destroy снимает provider и отменяет JOIN до освобождения instance/radio.
 * Внешний заголовок несёт X25519 отправителя для ECDH, шифрованное тело — X25519 получателя.
 * Сессии ограничены BLE_LINKS_MAX, очереди — четырьмя пакетами; секреты не логируются. */
#ifndef CHAT_BLE_JOIN_H
#define CHAT_BLE_JOIN_H
#include "../../lib/ble/ble_transport.h"
struct UTUN_INSTANCE;
struct ble_join;
/* Заимствует inst/radio/arg; connect запускает GATT по node_id, wake планирует будущий run.
 * NULL при ошибке; provider уже зарегистрирован при успехе. */
struct ble_join* ble_join_create(struct UTUN_INSTANCE* inst, struct ble_transport* radio,
                                ble_link_id (*connect)(void*, const uint8_t key[32]), void (*wake)(void*), void* arg);
void ble_join_destroy(struct ble_join* join); /* Снять provider, очистить ограниченные очереди; NULL допустим. */
/* Состояние GATT; CLOSED отменяет только соответствующую криптографическую сессию. */
void ble_join_connection(struct ble_join* join, ble_link_id link, uint64_t peer, enum ble_state state);
/* 1=bootstrap-пакет обработан/отклонён, 0=обычный пакет приглашения. bytes заимствованы. */
int ble_join_receive(struct ble_join* join, ble_link_id link, const uint8_t* bytes, size_t len);
void ble_join_run(struct ble_join* join); /* Продвинуть handshake/TX, deadlines и deferred callbacks ядра. */
int ble_join_holds(struct ble_join* join, ble_link_id link); /* Ненулевой ответ удерживает повышение GATT до ETCP. */
#endif
