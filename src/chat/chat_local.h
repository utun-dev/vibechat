/* Общая обработка LAN/Bluetooth discovery: готовые invite-ссылки и подписанные адреса известных узлов, без запуска JOIN.
 * LAN-адреса подтверждаются nonce-ответом только при отсутствии прямого UP и BGP-путей во всех группах.
 * Ключи берутся из registry/подтверждённого состава; неизвестный ключ дозагружает READ worker.
 * До16 coalesced запросов/коротких cache slots; неизвестное объявление пропускается до следующей рассылки.
 * Открытые объявления содержат ключ отправителя; NODEINFO не изменяется, stop отсоединяет ожидающие READ.
 * Весь API и callbacks работают в uasync-потоке живого instance; stop — до освобождения ядра.
 * LOCAL_INVITE передаёт проверенную ссылку обработчику chat_event; получение не запускает JOIN.
 * LOCAL_DELIVERY подтверждает доставку ссылки устройству, а не вступление в канал. */
#ifndef CHAT_LOCAL_H
#define CHAT_LOCAL_H

#include "../local_transport/local_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
/* Дополнительный транспорт ближайших устройств; API заимствован до снятия через set_bluetooth(NULL).
 * peers копирует до capacity записей. send копирует ссылку: 0 => один будущий delivery, -1 => callback не будет.
 * Установка и callbacks — uasync; таблицей соседей и pending владеет драйвер, ядро их не дублирует. */
struct chat_local_provider {
    void* ctx;
    size_t (*peers)(void* ctx, struct local_peer* output, size_t capacity);
    int (*send)(void* ctx, const uint8_t key[32], uint64_t request, const void* data, size_t len);
};
void chat_local_set_bluetooth(struct UTUN_INSTANCE* inst, const struct chat_local_provider* provider);
/* Проверить готовую join-ссылку; 1=валидна, 0=ошибка с логом. */
int chat_local_valid_link(const char* link, size_t len);
/* Проверить политику LAN/BT и передать приглашение UI; 0=доставлено, FORBIDDEN либо -1=отказ. JOIN не запускается. */
int chat_local_receive(struct UTUN_INSTANCE* inst, const struct local_peer* peer, const uint8_t* link, size_t len, int bluetooth);
/* Конечный статус доставки, использует тот же UI event для LAN и BT; содержимое ссылки не логируется. */
void chat_local_delivered(void* inst, uint64_t request, const struct local_device_address* address, int status);
/* Адрес событий UI: ключ(32), вид транспорта(1). 0 — Ed25519 LAN, 1 — X25519 Bluetooth. */
#define CHAT_LOCAL_ADDRESS_SIZE (LOCAL_ADDRESS_KEY_SIZE + 1)
int chat_local_address_decode(struct local_device_address* out, const uint8_t data[CHAT_LOCAL_ADDRESS_SIZE]);
/* Создать транспорт для desktop/mobile, подписать активность. Повтор и server — 0; ошибка — -1. */
int chat_local_start(struct UTUN_INSTANCE* inst);
/* Снять подписку и уничтожить транспорт чат-сервиса; NULL/repeat допустимы. */
void chat_local_stop(struct UTUN_INSTANCE* inst);
/* Применить local_invites_enabled/local_broadcasts_enabled из chat_setting без рестарта.
 * Receive проверяется при каждой доставке, broadcasts применяется к транспорту.
 * При отсутствии транспорта ничего не делает. Возвращает 0/-1; сохранённые настройки не откатывает. */
int chat_local_apply_settings(struct UTUN_INSTANCE* inst);
/* Трамплины uasync. peers получает заимствованный inst; send освобождает u_malloc request. */
void chat_local_peers_trampoline(void* inst);
struct chat_local_send_request {
    struct UTUN_INSTANCE* inst;            // заимствованный instance, которому передаётся команда
    struct local_device_address address;   // адрес устройства из CHAT_EVT_LOCAL_DEVICES
    uint64_t request_id;                   // ненулевой ID UI, возвращается LOCAL_DELIVERY
    char link[LOCAL_TRANSPORT_DATA_MAX + 1]; // готовая зарегистрированная ссылка, завершается NUL
};
void chat_local_send_trampoline(void* request);

#ifdef __cplusplus
}
#endif
#endif
