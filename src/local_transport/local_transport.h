/* UDP discovery и доставка небольших сообщений в LAN: create → set_active → peers/send → destroy.
 * ANNOUNCE/MESSAGE/ACK открыты и не аутентифицированы; публичный ключ/ник сами по себе не доказывают личность.
 * Не создаёт ETCP-сокетов и не подключает пиры; необязательный подписанный обмен даёт LAN-кандидаты через route().
 * Все вызовы и callbacks — в потоке ua. destroy/set_active — вне callbacks транспорта.
 * Callbacks могут выполняться синхронно из API; их аргументы-указатели заимствованы до возврата.
 * Discovery-порт 45731 принимает объявления; ответы, сообщения и ACK идут через отдельные ephemeral UDP-порты.
 * Объявления/сверка интерфейсов — каждые 10с, tick — 1с; только ANNOUNCE продлевает жизнь endpoint на 30с.
 * До 128 устройств, 8 endpoints на устройство и 8 ожидающих доставок на транспорт; порядок peers/routes нестабилен.
 * За tick: до 256 UDP, 8 вызовов received и общий бюджет 16 проверок подписей/ответов CHALLENGE.
 * При исчерпании UDP-бюджета чтение приостанавливается до tick; возможны потери, таймеры не гарантируют точное время.
 * Диагностика — категория discovery; причины отброса суммируются до следующего tick. */
#ifndef LOCAL_TRANSPORT_H
#define LOCAL_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include "../../lib/socket_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOCAL_TRANSPORT_PORT 45731
#define LOCAL_TRANSPORT_NAME_MAX 128
#define LOCAL_TRANSPORT_DATA_MAX 1100
#define LOCAL_TRANSPORT_PEERS_MAX 128
#define LOCAL_ROUTE_SOCKETS_MAX 32
#define LOCAL_ROUTE_UDP 1
#define LOCAL_ROUTE_TCP 2
#define LOCAL_ROUTE_REALITY 4

struct UASYNC;
struct local_transport;
/* Один байт в ACK и результате доставки; FORBIDDEN означает явный запрет приёма у получателя. */
enum local_delivery_status {
    LOCAL_DELIVERY_FAILED = 0,
    LOCAL_DELIVERY_RECEIVED = 1,
    LOCAL_DELIVERY_FORBIDDEN = 2
};
#define LOCAL_ADDRESS_KEY_SIZE 32

struct local_device_address {
    uint8_t ed[LOCAL_ADDRESS_KEY_SIZE]; // Ed25519 для LAN.
    uint8_t ble_x[LOCAL_ADDRESS_KEY_SIZE]; // X25519 устройства для Bluetooth.
};
struct local_peer {
    struct local_device_address address; // Ровно один адрес транспорта.
    char name[LOCAL_TRANSPORT_NAME_MAX];   // UTF-8 ник, завершается NUL
};
/* Рабочий endpoint, без ключей. Ненулевой generation отличает повторное использование socket_id.
 * В одном наборе для IP socket_id уникальны; protocol — UDP, TCP либо TCP|REALITY. */
struct local_route_socket {
    uint64_t generation;                // ненулевое поколение рабочего сокета, не эпоха discovery
    uint16_t port;                       // host order, ненулевой
    uint8_t socket_id, protocol;         // ID 0..255 и LOCAL_ROUTE_*, REALITY только вместе с TCP
};
/* Проверенный nonce-ответ; адрес принадлежит только интерфейсу, принявшему объявление. */
struct local_route {
    struct sockaddr_storage address;    // IP отправителя и порт рабочего транспорта
    struct sockaddr_storage local_address; // IP нашего интерфейса, порт 0
    uint32_t interface_index;            // локальный ifindex, также IPv6 scope
    struct local_route_socket socket;    // подписанные ID/протокол/порт/поколение рабочего сокета
};
struct local_transport_callbacks {
    /* Эти четыре callbacks необязательны. Изменения списка объединяются до tick; выключение уведомляет сразу. */
    void (*peers_changed)(void* arg);       // добавление/удаление устройства, смена ника/эпохи; копия через peers()
    /* data: 1..DATA_MAX произвольных байтов, без гарантии NUL. 0 = принято, FORBIDDEN = запрет, прочее = ошибка.
     * Без received отправляется FAILED. Только принятые сообщения попадают в кеш дедупликации. */
    int (*received)(void* arg, const struct local_peer* peer, const uint8_t* data, size_t len);
    /* Один результат на принятый send; RECEIVED означает приём приложением, FAILED не доказывает отсутствие приёма. */
    void (*delivery)(void* arg, uint64_t request_id, const uint8_t ed[LOCAL_ADDRESS_KEY_SIZE], int status); // enum local_delivery_status
    void (*activity_changed)(void* arg, int active); // до открытия/закрытия сокетов; также при отказе tick-таймера
    /* Необязательный LAN transport: задать все пять callbacks либо ни одного.
     * sockets возвращает 0..capacity или -1; sign — 0/-1; verify — 0 (верна), 1 (ключ неизвестен), -1 (неверна/ошибка).
     * route_sockets заполняет out endpoints для локального address/index; память заимствована до возврата.
     * port/generation ненулевые, socket_id уникальны, protocol — UDP/TCP/TCP|REALITY; отправитель их не валидирует.
     * Пустой набор не объявляется, но передаётся в PROOF и заменяет прежние кандидаты пустым набором.
     * Подпись Ed25519 ровно 64 байта.
     * Подписываются полный wire-заголовок и тело без подписи; verify использует только известный ключ peer.
     * offline возвращает 1 при отсутствии прямого UP и живого BGP-пути во всех группах.
     * Этот gate проверяется при ANNOUNCE/PROOF, а не при чтении route(); ответ на CHALLENGE от него не зависит.
     * routes_changed сообщает применение PROOF, истечение/потерю endpoint, смену эпохи или выключение.
     * Перечитать route(); при выключении он уже возвращает 0. Не останавливать транспорт из callback. */
    int (*route_sockets)(void* arg, const struct sockaddr_storage* address, uint32_t index,
                         struct local_route_socket* out, size_t capacity);
    int (*route_sign)(void* arg, const uint8_t* data, size_t len, uint8_t signature[64]);
    int (*route_verify)(void* arg, const uint8_t peer[LOCAL_ADDRESS_KEY_SIZE], const uint8_t* data, size_t len, const uint8_t signature[64]);
    int (*route_offline)(void* arg, const uint8_t peer[LOCAL_ADDRESS_KEY_SIZE]);
    void (*routes_changed)(void* arg, const uint8_t peer[LOCAL_ADDRESS_KEY_SIZE]);
};

/* Создать выключенный транспорт: ua заимствован, name/callbacks копируются, arg передаётся callbacks.
 * ed отличен от нуля, callbacks != NULL, name != NULL — NUL-строка, 0..127 байтов UTF-8 без управляющих ASCII.
 * Пустой ник заменяется 64 hex-цифрами Ed25519. Случайная ненулевая эпоха создаётся один раз на handle.
 * Возвращает владельческий handle либо NULL с ошибкой в логе. */
struct local_transport* local_transport_create(struct UASYNC* ua, const uint8_t ed[LOCAL_ADDRESS_KEY_SIZE], const char* name,
                                               const struct local_transport_callbacks* callbacks, void* arg);
/* Закрыть сокеты, отменить таймеры, завершить pending через delivery(FAILED) и освободить handle.
 * Может синхронно вызывать callbacks выключения; NULL допустим. ua/arg не освобождаются. */
void local_transport_destroy(struct local_transport* transport);
/* Ненулевой active включает сокеты и tick; выключение завершает доставки FAILED, очищает peers/routes.
 * Повтор допустим; эпоха и кеш принятых сообщений сохраняются до destroy, настройки имени/broadcasts тоже.
 * Возвращает 0/-1. Отказы интерфейсов/сокетов логируются и повторяются при сверке, но не отменяют активацию;
 * отказ tick-таймера выключает транспорт и даёт -1. NULL transport — -1. */
int local_transport_set_active(struct local_transport* transport, int active);
/* Разрешить периодические broadcast/multicast-объявления (по умолчанию включены).
 * Unicast-объявления, приём, сверка интерфейсов и TTL сохраняются; это не запрет видимости устройства.
 * При смене на enabled != 0 активный транспорт сразу объявляется. Возвращает 0; NULL transport — -1. */
int local_transport_set_broadcasts(struct local_transport* transport, int enabled);
/* Обновить ник (ограничения create); при изменении объявить, если active и broadcasts включены.
 * При том же имени ничего не отправляет. Возвращает 0/-1; ошибка оставляет прежний ник. */
int local_transport_set_name(struct local_transport* transport, const char* name);
/* Скопировать не более capacity живых устройств в out; возвращает количество скопированных, без общего размера.
 * Перед копированием удаляет просроченные endpoints/routes: возможен синхронный routes_changed.
 * peers_changed откладывается до tick. NULL transport/out — 0, без очистки; out обязан вмещать capacity элементов. */
size_t local_transport_peers(struct local_transport* transport, struct local_peer* out, size_t capacity);
/* Скопировать 1..DATA_MAX байтов data для ed из текущей таблицы. request_id != 0, уникален среди pending.
 * До 8 pending всего. 0 = запрос принят, -1 = callback для него не будет; нужна активность и известный живой peer.
 * Первая попытка сразу, до 2 повторов по tick на всех endpoints; первый ACK, потеря peer/эпохи, timeout или stop завершают запрос.
 * Ошибка отправки UDP не меняет 0 на -1. FAILED/потерянный ACK не гарантируют, что приложение не приняло данные.
 * Кеш получателя: последние 128 принятых (ed, epoch, request_id), каждый до 30с, без продления повтором.
 * Только в этом окне дубли дают RECEIVED без received; отказ приложения не кешируется. Учитывать это при выборе request_id.
 * Может вызвать routes_changed при очистке TTL. */
int local_transport_send(struct local_transport* transport, const uint8_t ed[LOCAL_ADDRESS_KEY_SIZE], uint64_t request_id,
                         const void* data, size_t len);
/* Скопировать проверенный живой LAN-кандидат с порядковым номером index (с 0); 1/0.
 * Кандидат требует nonce-PROOF за 3с и действует до 30с, пока живы endpoint и локальный интерфейс.
 * Без callbacks/продления TTL и без повторного offline-gate; NULL/неактивный транспорт/нет адреса дают 0, out не меняется. */
int local_transport_route(struct local_transport* transport, const uint8_t ed[LOCAL_ADDRESS_KEY_SIZE], size_t index, struct local_route* out);

/* Подтверждённые кандидаты для полного ключа ETCP. */
int local_transport_route_x(struct local_transport* transport, const uint8_t x[32], size_t index, struct local_route* out);
/* Разрешить числовой запрос старого API NCD в единственный X25519 с живым LAN-адресом. */
int local_transport_route_x_lookup(struct local_transport* transport, uint64_t node_id, uint8_t x[32]);

#ifdef __cplusplus
}
#endif
#endif
