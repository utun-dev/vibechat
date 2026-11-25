/* Общий владелец BLE для desktop/Android: create → ble_start(radio) → destroy.
 * Регистрирует заимствованный chat_local_provider: общий список LAN/BT, приглашения через отдельную характеристику.
 * bluetooth_invites_enabled (по умолчанию 0) проверяется при каждой доставке; ACK не запускает JOIN.
 * Перехватывает discovery/GATT и подключает packet_link_driver к ETCP; platform callbacks — наблюдатели.
 * Таблица соседей принадлежит этому объекту, объявления не удостоверяют личность.
 * Все операции в ua, destroy до instance и вне callbacks. При instance=NULL — только discovery/сырой BLE (пробы).
 * CHAT fallback: обнаруженный член группы без живого BGP-пути; UTUN запрещает ETCP поверх BLE.
 * Дубликаты одного ETCP_CONN: остаётся UP-линк с большим node_id GATT central; одиночный путь не заменяется.
 */
#ifndef CHAT_BLE_ETCP_H
#define CHAT_BLE_ETCP_H
#include "../../lib/ble/ble_transport.h"
#include "../../lib/ble/ble_identity.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BLE_NEIGHBORS_MAX 64
#define BLE_NEIGHBOR_TTL_US 30000000ULL
struct UTUN_INSTANCE;
struct ble_etcp;
struct ble_neighbor {
    struct ble_identity identity; /* ID/имя из последнего объявления, не аутентифицированы. */
    uint8_t public_key[BLE_PUBLIC_KEY_SIZE]; /* Полный адрес после чтения GATT-профиля. */
    char name[BLE_NAME_MAX + 1];  /* Полное имя GATT при profile_read, иначе доступное короткое имя. */
    int profile_read;            /* Полное имя прочитано; пустое значение также является именем. */
    uint64_t profile_at_us;      /* Последнее успешное чтение имени; не продлевает свежесть discovery. */
    uint64_t profile_retry_us;   /* Следующая допустимая фоновая попытка чтения. */
    struct ble_endpoint endpoint; /* Адрес для исходящего GATT; обновляется каждым объявлением. */
    int rssi;                    /* dBm, 127 — неизвестно. */
    uint64_t seen_us;             /* Монотонное время последнего объявления. */
    uint64_t broadcast_received;  /* Принятые объявления за жизнь записи, включая одинаковые повторы. */
};
/* config копируется, instance заимствован. NULL при ошибке. TX для ETCP ограничен одним пакетом. */
/* require_known_node=1 для штатного приложения, 0 только для изолированных аппаратных проб без базы/групп. */
struct ble_etcp* ble_etcp_create(struct UASYNC* ua, struct UTUN_INSTANCE* instance, const struct ble_config* config, int require_known_node);
/* Заимствованный транспорт для start/advertise/policy; пакеты ETCP обслуживает драйвер. */
struct ble_transport* ble_etcp_radio(struct ble_etcp* driver);
/* Копировать до capacity свежих соседей в output; возвращает число скопированных записей. */
size_t ble_etcp_neighbors(struct ble_etcp* driver, struct ble_neighbor* output, size_t capacity);
/* Начать GATT к свежему соседу по ID; 0=начато/уже существует, -1=отказ с логом. */
int ble_etcp_connect(struct ble_etcp* driver, uint64_t node_id);
/* Отменить попытки групп, закрыть свои ETCP-линки и радио. NULL допустим. */
void ble_etcp_destroy(struct ble_etcp* driver);
#ifdef __cplusplus
}
#endif
#endif
