/* Обнаружение VibeChat: VC, версия 6, селектор BE64, имя UTF-8 до 13 байт.
 * Обнаружение/выбор endpoint идёт по UUID подключаемого GATT-сервиса; bytes — общий формат события/метаданных.
 * encode возвращает размер 11..24 либо -1; decode возвращает ble_identity_result.
 * Имя сокращается по целому символу; decode отклоняет неверный UTF-8 и нулевой ID. */
#ifndef VIBECHAT_BLE_IDENTITY_H
#define VIBECHAT_BLE_IDENTITY_H
#include <stdint.h>
#include <stddef.h>
#define BLE_IDENTITY_VERSION 6 /* HELLO/ACCEPT переносят полные X25519, DATA/ACK сохраняют формат. */
#define BLE_NAME_MAX 63 /* Полное имя узла в UTF-8; без NUL, соответствует конфигурации ядра. */
#define BLE_SERVICE_UUID_FILTER "f61a9020-0000-4506-98e1-000000000000"
#define BLE_SERVICE_UUID_MASK "ffffffff-0000-ffff-ffff-000000000000"
#ifdef __cplusplus
extern "C" {
#endif
struct ble_identity { uint64_t node_id; char name[14]; };
/* Причины отказа: вызывающий код логирует их с endpoint/RSSI и ограничивает повторы. */
enum ble_identity_result {
    BLE_IDENTITY_OK = 0,
    BLE_IDENTITY_ERR_ARGUMENT = -1,
    BLE_IDENTITY_ERR_LENGTH = -2,
    BLE_IDENTITY_ERR_MAGIC = -3,
    BLE_IDENTITY_ERR_VERSION = -4,
    BLE_IDENTITY_ERR_NODE = -5,
    BLE_IDENTITY_ERR_UTF8 = -6
};
int ble_identity_encode(uint8_t output[24], uint64_t node_id, const char* name);
/* UUID: f61a9020-<selector upper16>-4506-98e1-<selector lower48>. 37 байт с NUL, 0/-1, селектор ненулевой. */
int ble_service_uuid_encode(char output[37], uint64_t node_id);
/* Проверить семейство/версию/ненулевой ID. Регистр hex произвольный, node_id меняется лишь при успехе;0/-1. */
int ble_service_uuid_decode(const char* uuid, uint64_t* node_id);
/* Разбирает data[size]; output меняется только при успехе. Ошибка — отрицательный код. */
int ble_identity_decode(struct ble_identity* output, const uint8_t* data, size_t size);
/* Короткое имя результата decode для диагностики; возвращает статическую строку. */
const char* ble_identity_result_name(int result);
/* Проверить полное UTF-8 имя длиной 0..BLE_NAME_MAX; 0 при успехе, -1 с диагностикой при ошибке. */
int ble_name_validate(const char* name);
#ifdef __cplusplus
}
#endif
#endif
