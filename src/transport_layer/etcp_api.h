/**
 * @file etcp_api.h
 * @brief Прямой обмен кодограммами ETCP и подписки на события транспорта.
 *
 * bind выбирает обработчик по первому байту cmd, send передаёт кодограмму соседу.
 * Для доставки через промежуточные узлы используется etcp_router.h.
 * Все вызовы — в потоке uasync. Перед отправкой ждать UP и свободной send_input_q.
 * Владение entry различается: send забирает его только при успехе, recv — всегда.
 * Принятие send и транспортный ACK не гарантируют завершение операции сервиса через REINIT/DELETE:
 * сброс отбрасывает старый поток, владелец завершает/повторяет свои неподтверждённые операции.
 */

#ifndef ETCP_API_H
#define ETCP_API_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include "../lib/ll_queue.h"

#define ETCP_MAX_BINDINGS 256

// ═══════════════════════════════════════════════════════════════════════════
// ЕДИНЫЙ РЕЕСТР ETCP-идентификаторов. Единственное место определения.
//
// Два независимых пространства имён (отдельные таблицы биндинга):
//   • ETCP_ID_*     — raw ETCP:       etcp_bind() / etcp_send()
//   • ETCP_RT_ID_*  — router-сервисы: etcp_router_bind() / etcp_route_send()
//
// Значения 0..255 уникальны внутри своей таблицы. Между таблицами совпадения
// допустимы: например, ETCP_ID_DATA и ETCP_RT_ID_DATA имеют значение 0x00.
// Префикс соответствует таблице без псевдонимов и исключений.
// tests/test_service_ids.py проверяет объявления, уникальность и использование
// при make check. Подкоманды отдельных протоколов объявляются в их заголовках.
// ═══════════════════════════════════════════════════════════════════════════

// === Raw ETCP packet IDs (etcp_bind/etcp_send) ===
#define ETCP_ID_DATA            0x00  // обработчик прямых данных и fallback для неизвестного cmd
#define ETCP_ID_TOPO_ENTRY      0x01  // BGP-топология (topo_group)
#define ETCP_ID_SVC_ROUTE       0x03  // транспорт роутера (etcp_router)
#define ETCP_ID_NTP_TIME        0x04  // NTP time sync P2P
#define ETCP_ID_BROADCAST       0x05  // broadcast-сообщения между пирами
#define ETCP_ID_RADIO           0x06  // рация (PTT мультикаст голоса) между пирами
#define ETCP_ID_STREAM          0x07  // прямой stream INFO/выдачи/DATA
#define ETCP_ID_MULTICAST       0x08  // адресный мультикаст с разделением по маршрутам группы
#define ETCP_ID_NCD_CONTROL     0x12  // node_conn_direct CLOSE/KEEP_ALIVE
#define ETCP_ID_GROUP_INVITE    0x13  // зарезервирован: старый INVITE_INFO удалён; вступление только через CHAT_SYNC JOIN
#define ETCP_ID_DB_SYNC         0x20  // db_sync
#define ETCP_ID_CHAT_SYNC       0x30  // chat_sync
#define ETCP_ID_MEMBER_SYNC     0x31  // member_sync/merkle_sync
#define ETCP_ID_BLE_PROBE       0xFD  // только явно включённая instrumentation-проба BLE ETCP echo

// === Router-сервисы (etcp_router_bind/etcp_route_send) ===
#define ETCP_RT_ID_DATA            0x00  // routing.c — маршрутизация данных
#define ETCP_RT_ID_NAT             0x02  // NAT-трафик между узлами
#define ETCP_RT_ID_TCP_PROXY_SERVER 0x04 // TCP proxy server (CONNECT/DATA от клиентов)
#define ETCP_RT_ID_UDP_PROXY       0x05  // UDP datagram прокси (client ↔ exit)
#define ETCP_RT_ID_ICMP_PROXY      0x06  // ICMP echo прокси (ping через exit)
#define ETCP_RT_ID_MEDIA_DELIVERY  0x07  // распространение медиа (аудио/видео)
#define ETCP_RT_ID_TCP_PROXY_CLIENT 0x08 // TCP proxy client (DATA/CLOSE/ERROR/FIN)
#define ETCP_RT_ID_CONN_MGR        0x11  // Connection Manager — management connections
#define ETCP_RT_ID_ADMIN_KEY       0x32  // chat_admin — канальный приватный ключ
#define ETCP_RT_ID_JOIN            0x33  // chat_join — join-протокол
#define ETCP_RT_ID_DM              0x34  // dm_core — прямой p2p чат
#define ETCP_RT_ID_DM_MAILBOX      0x35  // dm_mailbox — offline-хранение
#define ETCP_RT_ID_CALL            0x36  // call — P2P аудио-звонок
#define ETCP_RT_ID_FILE_TRANSFER   0x37  // file_transfer — передача неизменяемых файлов
#define ETCP_RT_ID_DM_MEDIA        0x38  // dm_media — custody, location и подтверждения медиа
#define ETCP_RT_ID_MEMBER_FILES    0x39  // member_files — каталоги и файлы участников группы
#define ETCP_RT_ID_GROUP_AVATAR    0x3A  // подписанная аватарка группы, блоки до 4 КиБ
#define ETCP_RT_ID_NAT_DETECTION   0x3B  // управление NAT-пробами через маршруты выбранной группы
#define ETCP_RT_ID_STREAM          0x3C  // stream REVERSE_REQ/CLOSED и помощь суперузлов через группу
#define ETCP_RT_ID_CALL_VIDEO_CONTROL 0x3D // возможности и восстановление видеотрека
#define ETCP_RT_ID_CALL_VIDEO      0x3E  // видео, отдельный ACK/debt/backpressure
#define ETCP_RT_ID_CALL_AUDIO      0x3F  // аудио, независимо от сигналинга/видео
#define ETCP_RT_ID_RADIO_CALL      0x40  // адресное приглашение участника в рацию CHAT-канала
#define ETCP_RT_ID_MULTICAST_REPORT 0x41 // отчёты о переполнении multicast FIFO
#define ETCP_RT_ID_DEVGROUP         0x43 // объединение устройств, E2E приглашения
#define ETCP_RT_ID_CLIPBOARD        0x44 // буфер обмена своих устройств через общую CHAT-группу
#define ETCP_RT_ID_CLIPBOARD_FILES  0x45 // файлы буфера по запросу, только свои устройства, E2E router
#define ETCP_RT_ID_ATTACHMENT_KEYS 0x42 // запросы и ручная E2E-выдача ключей групповых вложений

// Connection status events (instance-level callback).
// NEW начинает жизнь объекта, UP/DOWN отражают доступность, DELETE завершает её.
//   NEW    — один раз после new-connection callbacks, если они не отменили создание.
//   UP     — переход links_up 0→1 (UP после UP не вызывается).
//   DOWN   — переход links_up 1→0 (DOWN после DOWN не вызывается).
//   DELETE — один раз при закрытии, финальный; единственный статус, допустимый при state==2.
//   REINIT — новая транспортная сессия; не означает потерю физического пути.
#define ETCP_CONN_STATUS_NEW    0  // соединение создано
#define ETCP_CONN_STATUS_UP     1  // соединение поднялось
#define ETCP_CONN_STATUS_DOWN   2  // соединение упало
#define ETCP_CONN_STATUS_REINIT 4  // новая сессия на существующем соединении
#define ETCP_CONN_STATUS_DELETE 3  // соединение удалено

// Forward declarations
struct ETCP_CONN;
struct ETCP_LINK;
struct ETCP_SOCKET;
struct UTUN_INSTANCE;

typedef void (*etcp_conn_status_fn)(struct ETCP_CONN* conn, int status, void* arg);
struct etcp_status_cbk_entry {
    etcp_conn_status_fn fn;
    void*               arg;
    struct etcp_status_cbk_entry* next;
};

/**
 * @brief Тип коллбэка для приёма пакетов
 * @param conn ETCP соединение от которого получен пакет
 * @param entry Элемент очереди с данными пакета
 * 
 * @note После обработки освободить сначала dgram через queue_dgram_free(),
 *       затем entry через queue_entry_free(). conn заимствован на время вызова.
 * @note Raw bind получает cmd || payload. Router bind получает заголовок
 *       ROUTER_SVC_* и payload со смещения ROUTER_SVC_PAYLOAD_OFF (etcp_router.h).
 */
typedef void (*etcp_recv_fn)(struct ETCP_CONN* conn, struct ll_entry* entry);

// Forward declaration
struct ETCP_CONN;
struct UTUN_INSTANCE;
struct TOPO_GROUP_NODE;

// Per-connection callback event types (bitmask).
// При state==2 допустим только терминальный ETCP_CBK_EVENT_DELETE.
// UP/DOWN зеркалят conn-status UP/DOWN; при закрытии (если соединение было UP)
// отправляется один финальный DOWN до state=2.
#define ETCP_CBK_EVENT_INIT         (1 << 0)
#define ETCP_CBK_EVENT_REINIT       (1 << 1)
#define ETCP_CBK_EVENT_UP           (1 << 2)
#define ETCP_CBK_EVENT_DOWN         (1 << 3)
#define ETCP_CBK_EVENT_NODE_CHANGED (1 << 4)
#define ETCP_CBK_EVENT_DELETE       (1 << 5) // терминальное событие, state==2

typedef void (*etcp_cbk_fn)(struct ETCP_CONN* conn, int event, void* arg);
struct etcp_cbk_entry {
    etcp_cbk_fn fn;
    void*       arg;
    int         event_mask;
    struct etcp_cbk_entry* next;
};

/**
 * @brief Рассылка per-connection события
 * 
 * @param conn ETCP соединение
 * @param event Битовая маска события (ETCP_CBK_EVENT_*)
 * 
 * @note Вызываются все коллбэки соединения, у которых event_mask
 *       пересекается с переданным event.
 * @note При state==2 допускается только DELETE; прочие события игнорируются.
 */
void etcp_cbk_fire(struct ETCP_CONN* conn, int event);

// Instance-level callback types (separate from per-connection etcp_cbk_fn in etcp.h)
typedef void (*etcp_inst_cbk_fn)(struct ETCP_CONN* conn, void* arg);
struct etcp_inst_cbk_entry {
    etcp_inst_cbk_fn fn;
    void*            arg;
    struct etcp_inst_cbk_entry* next;
};

/* Внутренняя очистка снятых instance-подписок после рассылки; при depth>0 ничего не освобождает.
 * Во время callbacks remove сразу отключает запись, память сохраняется до выхода из внешней рассылки.
 * Новые подписки добавляются в голову и не участвуют в уже начатом обходе. */
void etcp_instance_callbacks_collect(struct UTUN_INSTANCE* inst);

// ---- Per-connection unified callback API ----
/**
 * @brief Зарегистрировать per-connection коллбэк с маской событий
 * 
 * @param conn ETCP соединение
 * @param fn Коллбэк (сигнатура etcp_cbk_fn)
 * @param arg Пользовательский аргумент, передаваемый в коллбэк
 * @param event_mask Битовая маска интересующих событий (ETCP_CBK_EVENT_*)
 */
void etcp_conn_add_cbk(struct ETCP_CONN* conn, etcp_cbk_fn fn, void* arg, int event_mask);

/**
 * @brief Удалить per-connection коллбэк
 * 
 * @param conn ETCP соединение
 * @param fn Коллбэк
 * @param arg Пользовательский аргумент (для идентификации записи)
 */
void etcp_conn_remove_cbk(struct ETCP_CONN* conn, etcp_cbk_fn fn, void* arg);

/**
 * @brief Обновить маску событий у уже зарегистрированного коллбэка
 * 
 * @param conn ETCP соединение
 * @param fn Коллбэк
 * @param arg Пользовательский аргумент
 * @param event_mask Новая битовая маска событий (ETCP_CBK_EVENT_*)
 */
void etcp_conn_update_cbk_mask(struct ETCP_CONN* conn, etcp_cbk_fn fn, void* arg, int event_mask);

/* Instance-подписки new/conn-status/link-status/socket можно снимать из callback, включая себя и следующий.
 * Снятие немедленно отключает запись, память освобождается после внешней рассылки. Добавленные записи
 * не участвуют в уже начатом обходе, но видны отдельной вложенной рассылке. Destroy instance внутри callback запрещён. */
// ---- Instance-level callbacks ----
/**
 * @brief Зарегистрировать instance-level коллбэк «создано новое соединение»
 * 
 * @param inst UTUN instance
 * @param fn Коллбэк (сигнатура etcp_inst_cbk_fn)
 * @param arg Пользовательский аргумент
 */
void etcp_add_new_conn_cbk(struct UTUN_INSTANCE* inst, etcp_inst_cbk_fn fn, void* arg);

/**
 * @brief Удалить instance-level коллбэк «создано новое соединение»
 * 
 * @param inst UTUN instance
 * @param fn Коллбэк
 * @param arg Пользовательский аргумент
 */
void etcp_remove_new_conn_cbk(struct UTUN_INSTANCE* inst, etcp_inst_cbk_fn fn, void* arg);

// ---- Link status change callback (instance-level) ----
typedef void (*etcp_link_status_cbk_fn)(struct ETCP_CONN* conn, struct ETCP_LINK* link,
                                         int old_state, int old_status, void* arg);

struct etcp_link_status_cbk_entry {
    etcp_link_status_cbk_fn fn;
    void*                   arg;
    struct etcp_link_status_cbk_entry* next;
};

/**
 * @brief Зарегистрировать instance-level коллбэк смены состояния линка
 * 
 * @param inst UTUN instance
 * @param fn Коллбэк (сигнатура etcp_link_status_cbk_fn)
 * @param arg Пользовательский аргумент
 */
void etcp_add_link_status_cbk(struct UTUN_INSTANCE* inst, etcp_link_status_cbk_fn fn, void* arg);

/**
 * @brief Удалить instance-level коллбэк смены состояния линка
 * 
 * @param inst UTUN instance
 * @param fn Коллбэк
 * @param arg Пользовательский аргумент
 */
void etcp_remove_link_status_cbk(struct UTUN_INSTANCE* inst, etcp_link_status_cbk_fn fn, void* arg);

/**
 * @brief Внутренняя функция: рассылка события смены состояния линка
 * 
 * @param link Линк, состояние которого изменилось
 * @param old_state Предыдущее link_state (0-init, 1-handshake, 2-try_reconnect, 3-connected)
 * @param old_status Предыдущее link_status (1-up, 0-down)
 * 
 * @note Вызывается автоматически из etcp_connections.c при изменении
 *       link_state/link_status; не предназначен для вызова извне
 */
void etcp_fire_link_status_cbk(struct ETCP_LINK* link, int old_state, int old_status);

/**
 * @brief Структура bindings для ETCP API (per-instance)
 */
struct ETCP_BINDINGS {
    etcp_recv_fn callbacks[ETCP_MAX_BINDINGS];  /**< Массив callbacks, NULL = не установлен */
};

/**
 * @brief Отправить пакет в очередь normalizer. первый байт кодограммы - cmd (на приёмной стороне bind выберет обработчик по cmd).
 * 
 * @param conn ETCP соединение
 * @param entry Кодограмма вместе с cmd, от 1 до PKTNORM_MAX_DGRAM_SIZE байт (pkt_normalizer.h)
 * @return 0 при успехе, -1 при ошибке
 * 
 * @note Владение entry:
 *       - при успехе (0) entry передаётся в очередь normalizer'а; вызывающий
 *         НЕ должен ни освобождать, ни читать entry после вызова — если очередь
 *         пуста, entry обрабатывается и освобождается СИНХРОННО внутри вызова
 *         (queue_data_put → callback → queue_entry_free). Нужные поля entry надо
 *         сохранять до etcp_send().
 *       - при ошибке (-1) entry НЕ освобождается: вызывающий владеет entry и
 *         должен сам освободить его (queue_dgram_free(entry); queue_entry_free(entry)).
 */
int etcp_send(struct ETCP_CONN* conn, struct ll_entry* entry);

/**
 * @brief Подписаться на все пакеты с указанным ID для данного instance
 * 
 * @param inst UTUN instance
 * @param id Идентификатор пакета (первый байт кодограммы, cmd)
 * @param callback Коллбэк для обработки пакетов с этим ID
 * @return 0 при успехе, -1 при ошибке
 * 
 * @note ETCP_ID_DATA также получает пакеты без отдельного обработчика
 * @note Можно зарегистрировать только один коллбэк на каждый ID в пределах instance
 */
int etcp_bind(struct UTUN_INSTANCE* inst, uint8_t id, etcp_recv_fn callback);

/**
 * @brief Отписаться от пакетов с указанным ID для данного instance
 * 
 * @param inst UTUN instance
 * @param id Идентификатор пакета
 * @return 0 при успехе (в том числе без прежнего binding), -1 при неверном instance
 */
int etcp_unbind(struct UTUN_INSTANCE* inst, uint8_t id);

/**
 * @brief Зарегистрировать instance-level коллбэк статуса соединения
 * 
 * @param inst UTUN instance
 * @param fn Коллбэк (сигнатура etcp_conn_status_fn)
 * @param arg Пользовательский аргумент
 * 
 * @note status = ETCP_CONN_STATUS_NEW / UP / DOWN / DELETE / REINIT.
 *       DELETE — единственный статус при state==2. Финальный DOWN при close
 *       доставляется per-connection подписчикам; instance-подписчики получают DELETE.
 */
void etcp_add_conn_status_cbk(struct UTUN_INSTANCE* inst, etcp_conn_status_fn fn, void* arg);

/**
 * @brief Удалить instance-level коллбэк статуса соединения
 * 
 * @param inst UTUN instance
 * @param fn Коллбэк
 * @param arg Пользовательский аргумент
 */
void etcp_remove_conn_status_cbk(struct UTUN_INSTANCE* inst, etcp_conn_status_fn fn, void* arg);

/* Socket property change events (instance-level) */
#define ETCP_SOCKET_EVENT_ADDR_CHANGED    (1 << 0)
#define ETCP_SOCKET_EVENT_STATUS_CHANGED  (1 << 1)

typedef void (*etcp_socket_cbk_fn)(struct ETCP_SOCKET* sock, int event, void* arg);
struct etcp_socket_cbk_entry {
    etcp_socket_cbk_fn fn;
    void*              arg;
    int                event_mask;
    struct etcp_socket_cbk_entry* next;
};

/**
 * @brief Зарегистрировать instance-level коллбэк событий сокета
 * 
 * @param inst UTUN instance
 * @param fn Коллбэк (сигнатура etcp_socket_cbk_fn)
 * @param arg Пользовательский аргумент
 * @param event_mask Битовая маска событий (ETCP_SOCKET_EVENT_ADDR_CHANGED / STATUS_CHANGED)
 */
void etcp_add_socket_cbk(struct UTUN_INSTANCE* inst, etcp_socket_cbk_fn fn, void* arg, int event_mask);

/**
 * @brief Удалить instance-level коллбэк событий сокета
 * 
 * @param inst UTUN instance
 * @param fn Коллбэк
 * @param arg Пользовательский аргумент
 */
void etcp_remove_socket_cbk(struct UTUN_INSTANCE* inst, etcp_socket_cbk_fn fn, void* arg);

/**
 * @brief Внутренняя функция: рассылка события сокета
 * 
 * @param sock Сокет, у которого произошло событие
 * @param event Битовая маска события (ETCP_SOCKET_EVENT_*)
 * 
 * @note Вызываются коллбэки, у которых event_mask пересекается с event
 */
void etcp_socket_cbk_fire(struct ETCP_SOCKET* sock, int event);


/**
 * @brief Внутренняя функция etcp: Коллбэк для очередей output ll_queue normalizer
 * 
 * Собирает пакеты из всех подключений и отправляет в bind->cbk
 * по ID (первый байт кодограммы).
 * 
 * @param queue Очередь из которой получен пакет
 * @param arg Аргумент - указатель на ETCP_CONN
 * 
 * @note Этот коллбэк устанавливается автоматически при создании
 *       ETCP подключения через pn_init()
 */
void etcp_int_recv(struct ll_queue* queue, void* arg);


#ifdef __cplusplus
}
#endif
#endif // ETCP_API_H
