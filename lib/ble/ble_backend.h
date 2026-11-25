/* Внутренний платформенный контракт. Все вызовы/события в uasync, send/close не вызывают события синхронно.
 * ACCEPTED send копирует значение и даёт ровно один sent, либо closed отменяет все операции соединения.
 * DATA/ACK sent означает локальное завершение Without Response/Notify; только INVITE использует Request/Indication.
 * close/destroy отменяют callbacks; transport создаёт идентификаторы, backend не хранит указатели core-link.
 * Реализации не создают неограниченных очередей: максимум одна операция send на link. */
#ifndef VIBECHAT_BLE_BACKEND_H
#define VIBECHAT_BLE_BACKEND_H
#include "ble_transport.h"
#include "ble_identity.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BLE_RX_UUID "f61a9021-4abc-45ac-98e1-761df4c13811"
#define BLE_TX_UUID "f61a9022-4abc-45ac-98e1-761df4c13811"
#define BLE_INVITE_UUID "f61a9023-4abc-45ac-98e1-761df4c13811"
#define BLE_ACK_UUID "f61a9024-4abc-45ac-98e1-761df4c13811"
#define BLE_NAME_UUID "f61a9025-4abc-45ac-98e1-761df4c13811" /* Read: полное UTF-8 имя, без NUL. */
#define BLE_NODE_UUID "f61a9026-4abc-45ac-98e1-761df4c13811" /* Read: 32-байтовый X25519. */
#define BLE_VALUE_MAX 512
/* Wire-каналы backend; ACK не является прикладным каналом ble_channel. */
enum ble_wire_channel { BLE_WIRE_DATA, BLE_WIRE_INVITE, BLE_WIRE_ACK, BLE_WIRE_COUNT };
struct ble_backend;
struct ble_backend* ble_backend_create(struct ble_transport* owner, struct UASYNC* ua, const struct ble_config* config);
void ble_backend_destroy(struct ble_backend* backend);
int ble_backend_advertise(struct ble_backend* backend, const uint8_t* data, size_t len);
/* Изменение NODE_NAME. Backend не хранит заимствованное имя; 0 либо отрицательная ошибка. */
int ble_backend_set_name(struct ble_backend* backend, const char* name);
/* Локальные значения характеристик; только uasync, имя заимствовано до следующего set_name/destroy. */
const char* ble_backend_name(struct ble_transport* owner);
const uint8_t* ble_backend_public_key(struct ble_transport* owner);
/* Запрос только метаданных: не создавать подписки или HELLO после чтения профиля. */
int ble_backend_profile_only(struct ble_transport* owner, ble_link_id link);
/* Сохранить удалённый X25519 и проверить имя до opened. 0=продолжить GATT; 1=profile-only закончен; -1=link закрыт. */
int ble_backend_profile(struct ble_transport* owner, ble_link_id link, const uint8_t* node, size_t node_size,
                        const uint8_t* name, size_t name_size);
int ble_backend_connect(struct ble_backend* backend, ble_link_id id, const struct ble_endpoint* endpoint);
void ble_backend_close(struct ble_backend* backend, ble_link_id id);
int ble_backend_send(struct ble_backend* backend, ble_link_id id, enum ble_wire_channel channel, const uint8_t* data, size_t len);
/* События backend. incoming вызывается до любых других событий нового входящего link; 0 = лимит/отказ. */
ble_link_id ble_backend_incoming(struct ble_transport* owner, const struct ble_endpoint* endpoint);
/* Ожидаемый/согласованный node_id link; 0 для неизвестного/закрытого. Только адресация, не аутентификация. */
uint64_t ble_backend_peer(struct ble_transport* owner, ble_link_id link);
void ble_backend_status(struct ble_transport* owner, int ready, const char* reason);
void ble_backend_advertising(struct ble_transport* owner, enum ble_state state, const char* reason);
/* Снимок возможностей advertising адаптера, не измерение мощности соединения. Копируется. */
void ble_backend_advertising_power(struct ble_transport* owner, struct ble_advertising_power power);
void ble_backend_broadcast(struct ble_transport* owner, const struct ble_endpoint* endpoint,
                           const uint8_t* data, size_t len, int rssi);
void ble_backend_opened(struct ble_transport* owner, ble_link_id id, unsigned int mtu);
void ble_backend_received(struct ble_transport* owner, ble_link_id id, enum ble_wire_channel channel, const uint8_t* data, size_t len);
void ble_backend_sent(struct ble_transport* owner, ble_link_id id, int success);
void ble_backend_closed(struct ble_transport* owner, ble_link_id id, const char* reason);
#ifdef __cplusplus
}
#endif
#endif
