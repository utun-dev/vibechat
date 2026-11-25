/* Изолированная проба Windows GATT + VC-маяка. Не использует ключи, БД или настройки приложения.
 * Windows отвечает только тестовому клиенту; observer соединяет объявления по SERVER_ID, не по BLE-адресу.
 * Клиент сверяет echo разного размера не менее 30 секунд; ник должен продолжать приходить в это время. */
#ifndef VIBECHAT_BLE_DUAL_PROBE_H
#define VIBECHAT_BLE_DUAL_PROBE_H
#include <stdint.h>
#define BLE_DUAL_SERVER_ID UINT64_C(0x56434455414c0001)
#define BLE_DUAL_CLIENT_ID UINT64_C(0x56434455414c0002)
#define BLE_DUAL_NAME "VC-dual-probe"
#define BLE_DUAL_HOLD_US UINT64_C(30000000)
#endif
