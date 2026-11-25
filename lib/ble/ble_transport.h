/* BLE transport без Qt: create → start → advertise/connect → tx_put/rx_get → destroy.
 * Все API и callbacks принадлежат переданному uasync-потоку; callbacks не вызывают stop/destroy/disconnect.
 * UUID сервиса содержит только короткий селектор обнаружения. Из GATT читаются полный X25519 и имя.
 * HELLO/ACCEPT сверяют полные адреса. Владение ключом проверяют ETCP и E2E JOIN.
 * DATA рукопожатия: [flow_seq:2 BE][тип:1][offset:1][часть X_src || X_dst]. Общий размер адресов 64 байта.
 * Один сервис обслуживает независимые соединения; непрозрачные кадры: DATA 1..1500, INVITE 1..16384 байт.
 * Целые кадры находятся в ll_queue. DATA: Write Without Response/Notification; INVITE: Request/Indication.
 * DATA имеет накопительный ACK GATT-фрагментов: ограничивает очередь ОС, без собственных ретрансляций.
 * ACK: Without Response/Notification; последний ID повторяется, чтобы потеря ACK не запирала окно.
 * Доставку целых DATA подтверждает потребитель (ETCP); ACK не является подтверждением приложения.
 * Диагностика: discovery (BLE-префикс). NULL validate разрешает запрос, как и начальная политика GUI. */
#ifndef VIBECHAT_BLE_TRANSPORT_H
#define VIBECHAT_BLE_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#include "../ll_queue.h"
#include "ble_identity.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BLE_FRAME_MAX 1500
#define BLE_INVITE_FRAME_MAX (16u * 1024u) /* JOIN_INFO_RESP с JSON группы, профилем и аутентифицированным envelope, отдельно от DATA. */
#define BLE_BROADCAST_MAX 24
#define BLE_ENDPOINT_MAX 256
#define BLE_PUBLIC_KEY_SIZE 32
#define BLE_LINKS_MAX 16
#define BLE_QUEUE_MAX 64
#define BLE_TX_WINDOW_FRAMES 3u /* Целые DATA от tx_put до ACK последнего фрагмента; баланс скорости/RTT на A52/BlueZ. */
#define BLE_TX_WINDOW_BYTES 12500u /* Дополнительный предел GATT-байтов; не оценка скорости/задержки. */
#define BLE_ACK_INTERVAL_US 15000u
#define BLE_ACK_REPEAT_US 100000u /* Повтор последнего ACK при отсутствии нового DATA. */
struct ble_transport;
struct UASYNC;
typedef uint64_t ble_link_id;
enum ble_result { BLE_OK = 0, BLE_ERROR = -1, BLE_WOULD_BLOCK = -2, BLE_NOT_READY = -3 };
enum ble_channel { BLE_CHANNEL_DATA, BLE_CHANNEL_INVITE, BLE_CHANNEL_COUNT };
enum ble_state { BLE_STARTING, BLE_READY, BLE_CLOSED };
struct ble_endpoint { char id[BLE_ENDPOINT_MAX]; }; /* Непрозрачный адрес backend, копируется целиком. */
struct ble_advertising_power {
    int max_dbm; /* Предел advertising адаптера, не текущая мощность ACL; 127 = неизвестен. */
    int can_set; /* Поддержка запроса мощности advertising: -1 = неизвестна, 0 = нет, 1 = есть. */
};
/* Копия возможностей активного радио, без I/O; NULL/не готово возвращает неизвестные значения. Только uasync. */
struct ble_advertising_power ble_get_advertising_power(const struct ble_transport* transport);
struct ble_callbacks {
    /* Node UUID из объявления сервиса: общий ble_identity bytes; буфер заимствован. RSSI=127 — неизвестный. */
    void (*broadcast)(void* arg, const struct ble_endpoint* endpoint, const uint8_t* data, size_t len, int rssi);
    void (*status)(void* arg, enum ble_state state, const char* reason); /* Состояние радио, reason заимствован. */
    void (*advertising)(void* arg, enum ble_state state, const char* reason); /* Публикация bytes: READY не подтверждает приём. */
    int (*validate)(void* arg, ble_link_id link, uint64_t node_id); /* Ненулевой результат разрешает HELLO. */
    void (*connection)(void* arg, ble_link_id link, uint64_t node_id, enum ble_state state, const char* reason);
    void (*received)(void* arg, ble_link_id link); /* rx_get до NULL; допустим синхронный drain. */
    void (*writable)(void* arg, ble_link_id link); /* После заполнения TX снова можно вызвать tx_put. */
    void (*invite_received)(void* arg, ble_link_id link); /* drain INVITE через channel_rx_get. */
    void (*invite_writable)(void* arg, ble_link_id link); /* Освободилось место в отдельной TX очереди. */
    /* Прочитанный профиль до HELLO. Имя заимствовано, ключ ещё не аутентифицирован. */
    void (*profile)(void* arg, const struct ble_endpoint* endpoint, const uint8_t key[BLE_PUBLIC_KEY_SIZE], const char* name);
};
struct ble_config {
    uint64_t node_id;             /* Короткий селектор UUID обнаружения, выводится из public_key. */
    uint8_t public_key[BLE_PUBLIC_KEY_SIZE]; /* Полный открытый X25519 этого устройства. */
    uint64_t (*discovery_id)(const uint8_t key[BLE_PUBLIC_KEY_SIZE]); /* Вычислить селектор UUID для ключа. */
    char name[BLE_NAME_MAX + 1];  /* Полное UTF-8 имя для GATT NODE_NAME; копируется, независимо от видимости. */
    int discover_names;          /* Frontend: ble_etcp кратко читает имена обнаруженных соседей, по одному запросу. */
    const char* adapter;          /* Linux: hci0 либо NULL (первый адаптер); Windows: NULL. Копируется. */
    struct ble_callbacks callbacks;
    void* arg;                   /* Заимствованный контекст callbacks, живёт до destroy. */
    unsigned tx_limit;           /* Целых TX-пакетов, включая фрагментируемый: 0=64, 1..64. */
    unsigned tx_window;          /* DATA в очереди/отправке/ожидании удалённого ACK: 0=BLE_TX_WINDOW_FRAMES, 1..64. */
    uint8_t initial_advertisement[BLE_BROADCAST_MAX]; /* Начальное объявление; копируется вместе с config. */
    size_t initial_advertisement_size; /* 0: скрыть идентичность в advertising; 1..24: объявлять node UUID сервиса. */
};
/* Создать остановленный transport. ua/config заимствованы только ua; возвращает handle или NULL с логом. */
struct ble_transport* ble_create(struct UASYNC* ua, const struct ble_config* config);
/* Начать регистрацию сервиса/scan; 0 означает запуск, готовность сообщает status. Повторный start — ошибка. */
int ble_start(struct ble_transport* transport);
/* Остановить свои объявления/scan/сессии, завершить pending и освободить очереди. Идемпотентно, вне callbacks. */
void ble_stop(struct ble_transport* transport);
/* Разрешить/запретить наши новые входящие сессии; запрет закрывает текущие входящие. Общий ACL ОС этим не управляется. */
void ble_set_incoming(struct ble_transport* transport, int allowed);
void ble_destroy(struct ble_transport* transport); /* stop + free; NULL допустим. */
/* Обновить полное UTF-8 имя характеристик без перезапуска сервиса/сессий; 0 либо отрицательная ошибка. */
int ble_set_name(struct ble_transport* transport, const char* name);
/* Включить публикацию node UUID сервиса; наличие data включает видимость, содержимое не передаётся. NULL,0 скрывает UUID.
 * Сервис остаётся зарегистрированным для известных адресов; допуск определяет ble_set_incoming, не видимость.
 * Регистрация не гарантирует выход в эфир/получение. Имя независимо задаётся через ble_set_name. */
int ble_advertise(struct ble_transport* transport, const void* data, size_t len);
/* Самостоятельно выполнить connect/discovery/subscribe/HELLO. expected_node задаёт UUID сервиса.
 * Handle либо 0 при отказе. Готовность/ошибка — connection; физическое ACL может обслуживать другие сервисы. */
ble_link_id ble_connect(struct ble_transport* transport, const struct ble_endpoint* endpoint, uint64_t expected_node);
/* Подключиться к выбранному X25519. Прочитанный GATT-профиль обязан совпасть с ним побайтно. */
ble_link_id ble_connect_key(struct ble_transport* transport, const struct ble_endpoint* endpoint, const uint8_t key[BLE_PUBLIC_KEY_SIZE]);
/* Кратко прочитать NODE_ID/NODE_NAME без CCC/HELLO/ETCP. Один запрос одновременно, profile callback при успехе.
 * 0=начато, WOULD_BLOCK=занято/уже есть outgoing link к узлу, прочие отрицательные значения — отказ. */
int ble_read_profile(struct ble_transport* transport, const struct ble_endpoint* endpoint, uint64_t expected_node);
/* Копировать направление существующего GATT в outgoing (0/1). 0 — успех, -1 — неверный handle/аргументы с логом. */
int ble_get_outgoing(struct ble_transport* transport, ble_link_id link, int* outgoing);
/* Копировать заявленный полный X25519 после HELLO/ACCEPT. 0 либо BLE_NOT_READY. */
int ble_get_peer_key(struct ble_transport* transport, ble_link_id link, uint8_t key[BLE_PUBLIC_KEY_SIZE]);
void ble_disconnect(struct ble_transport* transport, ble_link_id link);
/* Принять целый ll_entry в TX. При 0 владение передано; при любом отказе entry остаётся вызывающему.
 * entry должен быть вне очереди, dgram!=NULL, len=1..1500. Заполнение: WOULD_BLOCK, затем writable. */
int ble_tx_put(struct ble_transport* transport, ble_link_id link, struct ll_entry* entry);
/* Снимок готовности принять пакет; учитывает очередь и кадры до ACK последнего фрагмента.
 * После заполнения writable вызывается при освобождении места, без обязательного отказа tx_put. */
int ble_tx_ready(struct ble_transport* transport, ble_link_id link);
/* Извлечь целый кадр из RX или NULL; вызывающий освобождает queue_dgram_free + queue_entry_free. */
struct ll_entry* ble_rx_get(struct ble_transport* transport, ble_link_id link);
/* Каналы имеют независимые очереди/сборку, но один общий GATT send. INVITE: до 16384 байт, TX ограничен одним пакетом.
 * Контракт владения и возвратов как у DATA API выше. Планировщик чередует фрагменты каналов. */
int ble_channel_tx_ready(struct ble_transport* transport, ble_link_id link, enum ble_channel channel);
int ble_channel_tx_put(struct ble_transport* transport, ble_link_id link, enum ble_channel channel, struct ll_entry* entry);
struct ll_entry* ble_channel_rx_get(struct ble_transport* transport, ble_link_id link, enum ble_channel channel);
#ifdef __cplusplus
}
#endif
#endif
