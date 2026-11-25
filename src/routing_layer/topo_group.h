/**
 * @file topo_group.h
 * @brief Групповые сессии и обмен маршрутами поверх общего транспорта NCD.
 *
 * У каждой группы своя топология; идентичности и адреса узлов общие (topo_node.h).
 * UTUN обменивается также подсетями, CHAT допускает только полных неудалённых участников из локальной БД.
 * Для подключения используйте peer_open/close: группа владеет NCD с начала попытки.
 * JOIN/ACCEPT проверяет точное совпадение protocol_version и согласует сессию, NODEINFO/WITHDRAW поддерживают её маршруты.
 * Потерю путей обрабатывает topo_recovery, выбор CHAT-пиров — topo_group_connect.
 * На старте NTP транспорт подключается сразу, а BGP ждёт успеха NTP либо 4 с; затем повторяет JOIN/RESYNC.
 * Все операции — в потоке uasync. Сервисы создают и удаляют свои группы явно.
 *
 * Построение маршрутов:
 * - Собственный NODEINFO имеет пустую цепочку. Получатель добавляет ID соседа
 *   в конец: цепочка хранится от назначения к нам, без нашего собственного ID.
 * - У назначения не более одного пути от каждого соседа. NODEINFO атомарно
 *   заменяет его путь, WITHDRAW отзывает только его путь, DOWN удаляет все
 *   пути этого соединения. Чужой WITHDRAW не пересылается как глобальное удаление.
 * - Timestamp версионирует подписанную запись узла, а не путь. Даже объявление
 *   со старой записью обновляет путь; сама запись при этом не откатывается.
 * - После изменения выбираем живой путь с минимумом переходов; при равенстве
 *   побеждает меньший ID соседа. DOWN/closing-пути не используются как запасной результат.
 * - Данные и NODEINFO используют один выбор пути. Выбранный путь объявляется
 *   всем соседям вне его цепочки. Если прежний анонс стал недопустим, шлём WITHDRAW.
 *   Цепочки с повторами или собственным ID на приёме отклоняются.
 * - Каждая сессия хранит хеш последнего отправленного анонса назначения.
 *   Очередь хранит node_id; пакет строится из актуального состояния при отправке.
 *   Поэтому изменение пути распространяется без изменения timestamp и без эха дублей.
 *   RTT передаётся как оценка на момент анонса, но исключён из хеша: колебания
 *   задержки сами по себе не меняют маршрут и не вызывают повторных объявлений.
 * - Альтернатива сохраняет доступность узла. После потери последнего пути
 *   удаляется только маршрут группы: членство и сохранённая запись узла остаются.
 *
 * После стабилизации связей обмен сходится в пределах связного графа группы
 * и MAX_HOPS. READY относится к обмену с одним соседом. Временные петли данных
 * при перестройке отсекает цепочка пройденных узлов пакета ETCP-router.
 */
#ifndef TOPO_GROUP_H
#define TOPO_GROUP_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>
#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "../lib/sqlite3.h"
#include "route_lib.h"
#include "topo_node.h"
#include "secure_channel.h"

struct UTUN_INSTANCE;
/* После снятия стартового ожидания NTP повторить удержанные JOIN/RESYNC на уже поднятом транспорте. */
void topo_groups_time_ready(struct UTUN_INSTANCE* instance);
struct NAT_DETECTION;
struct CONN_MGR;
struct TOPO_RECOVERY_CTX;
struct TOPO_GROUP_CONNECT;
struct TOPO_PEER_REQUEST;
struct topo_tx_item;
struct topo_advert;
struct broadcast_ctx;
struct radio_ctx;

/** Callback when a node's info (pubkeys, addresses) is persisted in the DB */
typedef void (*topo_node_updated_fn)(struct UTUN_INSTANCE* inst, uint64_t node_id,
                                      const uint8_t* x25519_pubkey,
                                      const uint8_t* ed25519_pubkey);

/* ── BGP node event callbacks (appearance/update/removal) ── */

#define TOPO_NODE_EVENT_NEW    0
#define TOPO_NODE_EVENT_UPDATE 1
#define TOPO_NODE_EVENT_REMOVE 2

typedef void (*topo_node_event_fn)(struct TOPO_GROUP* group, const uint8_t ed[32],
                                   int event, void* arg);

/* Подписки принадлежат группе/instance; fn и arg заимствованы до remove/destroy.
 * Вызов синхронный, в uasync. Callback не удаляет группу/узел и не меняет список
 * подписчиков во время обхода; такие действия нужно отложить через uasync_call_soon.
 * Повторная регистрация создаёт ещё одну подписку; remove снимает первое совпадение. */

struct topo_node_cbk_entry {
    topo_node_event_fn fn;
    void*              arg;
    struct topo_node_cbk_entry* next;
};

/* Переход готовности конкретной групповой сессии, независимо от общего транспорта.
 * Callback выполняется в uasync; не должен удалять группу или её подписчиков. */
typedef void (*topo_peer_ready_fn)(struct TOPO_GROUP* group, const uint8_t ed[32], int ready, void* arg);
struct topo_peer_ready_cbk {
    topo_peer_ready_fn fn;
    void* arg;
    struct topo_peer_ready_cbk* next;
};
int topo_group_add_peer_ready_cbk(struct TOPO_GROUP* group, topo_peer_ready_fn fn, void* arg);
void topo_group_remove_peer_ready_cbk(struct TOPO_GROUP* group, topo_peer_ready_fn fn, void* arg);

/* ── Global nodeinfo callbacks (one per instance, any node change) ── */

typedef void (*nodeinfo_cbk_fn)(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, void* arg);

struct nodeinfo_cbk_entry {
    nodeinfo_cbk_fn fn;
    void*           arg;
    struct nodeinfo_cbk_entry* next;
};

/* Зарегистрировать0/-1;fn/arg принадлежат caller до remove/уничтожения instance. */
int utun_add_nodeinfo_cbk(struct UTUN_INSTANCE* instance, nodeinfo_cbk_fn fn, void* arg);
void utun_remove_nodeinfo_cbk(struct UTUN_INSTANCE* instance, nodeinfo_cbk_fn fn, void* arg);
void topo_fire_nodeinfo_cbk(struct UTUN_INSTANCE* instance, struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node);
/* Уведомить подписчиков о RTT/метриках без события изменения топологии для NAT-детектора. */
void topo_fire_node_metrics_cbk(struct UTUN_INSTANCE* instance, struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node);


// ETCP ID для пакетов топологии: ETCP_ID_TOPO_ENTRY (см. etcp_api.h)

/* Групповая сессия принадлежит паре (group, peer), транспорт разделяется через NCD.
 * JOIN_GROUP не добавляет мембера: CHAT требует source=0 и deleted=0 в локальной таблице участников
 * (chat_join.h / member_sync). Поздний commit мембера повторяет JOIN на готовом NCD.
 *
 * Каждая сторона создаёт случайный ненулевой local_epoch. JOIN несёт src_epoch,
 * dst_epoch=0 и тип группы. Получатель проверяет группу/членство/тип, посылает свой
 * JOIN и JOIN_ACCEPT(src=local_epoch, dst=полученный src). JOIN_REJECT возвращает
 * dst исходного запроса и причину; чужой/старый ответ не меняет текущую сессию.
 * Первый JOIN нового поколения пира повторяет наш JOIN: начальный мог попасть
 * в ожидание NTP. Повтор того же JOIN отвечает только ACCEPT, не сбрасывая состояние.
 * LEAVE до получения поколения пира имеет dst=0 и отменяет только предложение
 * с совпадающим src у ещё не согласованного получателя. После ACCEPT нужны оба id.
 * После ACCEPT стороны запрашивают таблицы. Все остальные сообщения содержат
 * оба поколения и принимаются только в согласованной сессии. TABLE_COMPLETE
 * отправляется после передачи транспорту всей таблицы отправителя.
 * READY = живой транспорт + ACCEPT + table_sent + table_received.
 * Исходящие команды одной сессии сохраняют FIFO-порядок. Снимок таблицы хранит
 * полные Ed25519; NODEINFO сериализуется при освобождении send_input_q. Удалённые
 * узлы пропускаются, изменения после запроса идут за снимком. Повторный запрос
 * не создаёт вторую таблицу. REINIT/leave/stop отменяют остаток и waiter; уже
 * переданные транспорту пакеты защищены поколениями на принимающей стороне.
 *
 * Новое присоединение и REINIT создают новое поколение; REINIT временно сохраняет
 * пути. Каждый NODEINFO помечает путь текущим local_epoch. На TABLE_COMPLETE
 * удаляются неподтверждённые пути этого соседа; альтернативы остаются.
 * Если обмен не продвигается TOPO_GROUP_SYNC_TIMEOUT_MS, все пути этого соседа
 * удаляются и JOIN начинается заново. Ошибка применения/отправки запрещает READY.
 * Поколения защищают сообщения, а доступность существующего пути определяется
 * его соединением. Recovery дополнительно ожидает READY группового пира.
 * JOIN_READY чата означает добавление мембера и не заменяет этот обмен.
 * Формат протокола изменяется без обратной совместимости.
 */

// Sub-команды
#define TOPO_SUBCMD_NODEINFO       0x04  // полная информация об узле + подсети
#define TOPO_SUBCMD_REQUEST_TABLE  0x05  // запрос полной таблицы
#define TOPO_SUBCMD_WITHDRAW       0x06  // узел стал недоступен
#define TOPO_SUBCMD_TABLE_COMPLETE     0x0B  // завершение начальной синхронизации таблицы
#define TOPO_SUBCMD_ERR_GROUP_MISMATCH 0x0C  // ошибка несоответствия типа группы
#define TOPO_SUBCMD_JOIN_GROUP     0x0D  // согласование участия в группе
#define TOPO_SUBCMD_RESYNC         0x0E  // общий запрос: «я переподключился, заново анонсируй свои группы»
#define TOPO_SUBCMD_LEAVE_GROUP    0x0F  // завершить участие, сохранив общий транспорт

#define TOPO_SUBCMD_JOIN_ACCEPT    0x10
#define TOPO_SUBCMD_JOIN_REJECT    0x11

/* Версия всего протокола группы; менять при несовместимых изменениях. */
#define TOPO_GROUP_PROTOCOL_VERSION 5

enum topo_join_reject { TOPO_JOIN_NO_GROUP = 1, TOPO_JOIN_NOT_MEMBER, TOPO_JOIN_WRONG_TYPE, TOPO_JOIN_UNAVAILABLE, TOPO_JOIN_PROTOCOL_MISMATCH };

#define BGP_NODES_HASH_SIZE 256

/* Заголовок всех групповых сообщений. RESYNC — единственное исключение. */
struct TOPOMSG_HEADER {
    uint8_t cmd, subcmd;
    uint8_t group_key[32]; /* Ed25519 CHAT или адрес маршрутизирующей группы UTUN. */
    uint64_t src_epoch, dst_epoch;
} __attribute__((packed));

struct TOPOMSG_NODEINFO_PKT {
    struct TOPOMSG_HEADER h;
    struct TOPOMSG_NODE node;
} __attribute__((packed));

struct TOPOMSG_WITHDRAW_PKT {
    struct TOPOMSG_HEADER h;
    uint8_t ed25519[SC_PUBKEY_SIZE]; /* Полный ключ назначения отзываемого пути. */
} __attribute__((packed));

struct topo_peer_protocol {
    struct topo_peer_protocol* next; /* владеет группа, до её уничтожения */
    uint8_t ed[32]; /* Ed25519 проверенного прямого пира. */
    uint16_t version; /* версия пира; 0 — поле отсутствовало */
    uint8_t state; /* 1 — совпадает, 2 — несовместима */
};

/* Только uasync: заимствованный результат проверки либо NULL, если проверки не было. */
const struct topo_peer_protocol* topo_group_peer_protocol(const struct TOPO_GROUP* group, const uint8_t ed[32]);

struct TOPOMSG_JOIN_GROUP {
    struct TOPOMSG_HEADER h;
    uint8_t group_type; /* JOIN/ACCEPT: тип группы; REJECT: topo_join_reject */
    uint16_t protocol_version; /* network byte order */
} __attribute__((packed));

struct TOPOMSG_RESYNC {
    uint8_t cmd, subcmd;
} __attribute__((packed));

struct TOPOMSG_ERR_GROUP_MISMATCH {
    struct TOPOMSG_HEADER h;
    uint8_t expected_type, received_flags;
} __attribute__((packed));

struct topo_node_load;
struct TOPO_GROUP_CONN_ITEM {
    struct TOPO_GROUP* group;          // владелец сессии
    uint64_t node_id;                  // идентичность пира, включая CONNECTING
    struct ETCP_CONN* conn;            // NULL до UP, в состоянии CONNECTING
    struct NODE_CONN_DIRECT* handle;   // владение BGP этим conn (NCD-handle)
    struct topo_node_load* rx_load;    // Сверка первого NODEINFO с сохранённой identity; отменяется при смене сессии.
    struct topo_rx_item *rx_head, *rx_tail; // Собственные пакеты BGP: сохраняют порядок до завершения rx_load.
    size_t rx_bytes;                  // Ограничение памяти очереди при медленном SQL.
    unsigned rx_count;                // Число отложенных входящих пакетов.
    uint8_t rx_checked[32];           // Ed25519 единственного пакета после READ, до завершения его replay.
    struct TOPO_PEER_REQUEST* requests; // запросы инициаторов; каждый закрывает свой
    uint64_t progress;                // время последнего прогресса обмена (timebase)
    void* exchange_timer;             // watchdog незавершённого обмена; отменяется при READY/DOWN/free
    uint8_t retained;                 // участие запрошено постоянным владельцем или достигло READY
    uint64_t local_epoch, peer_epoch;  // поколения сторон текущего присоединения
    uint8_t accepted;                 // JOIN_ACCEPT подтвердил local_epoch
    uint8_t tx_failed, transport_down; // ошибка отправки / потеря транспорта
    struct topo_tx_item *tx_head, *tx_tail; // FIFO команд, ещё не переданных транспорту
    struct queue_waiter_handle tx_waiter; // ожидание свободной send_input_q
    void* tx_wake;                    // отложенный запуск отправки
    struct ll_queue* advertised;      // Ed25519 -> хеш отправленного NODEINFO без RTT; владеет сессия
    uint8_t table_received;            // TABLE_COMPLETE текущей сессии принят
    uint8_t table_sent;                // ответ на запрос пира целиком передан транспорту
    uint8_t ready_notified;            // последнее опубликованное состояние READY
};

#define TOPO_GROUP_SYNC_TIMEOUT_MS 5000 /* отсутствие прогресса: удалить пути пира и начать новый JOIN */

struct ETCP_LINK;

#define TOPO_NODE_REGISTRY_HASH_SIZE 256

// node_registry entries: queue_entry_new(8 + sizeof(void*))
//   data[0..7]  = node_id (hash key, index_size=8)
//   data[8..15] = struct TOPO_NODE* pointer

/**
 * @brief Группа топологии (одна на каждый group_id: utun, чат-группа, …)
 */
struct TOPO_GROUP {
    struct ll_entry ll;
    uint64_t group_id;                // локальный индекс группы
    uint8_t wire_id[32];              // кеш неизменяемого адреса группы, нули до получения ключа CHAT
    uint64_t generation;              // Uasync: уникальный номер регистрации в этом контейнере; защита jobs от delete/rejoin того же ID.
    uint8_t  group_type;              // TOPO_GROUP_TYPE_*
    uint8_t content_protocol;         // Подготовленный протокол содержимого GROUP_PROTOCOL_*; только uasync, не wire.
    struct UTUN_INSTANCE* instance;
    struct ll_queue* senders_list;       // ll_entry.data = TOPO_GROUP_CONN_ITEM; включает CONNECTING с conn=NULL
    struct ll_queue* nodes;              // TOPO_GROUP_NODE{ll_entry,node_id,paths,...} — per-group узлы, хеш-индекс по node_id(8B)
    struct TOPO_GROUP_NODE* local_node;  // свой узел в этой группе
    struct TOPO_GROUP_NODE* address_buckets[BGP_NODES_HASH_SIZE]; // self и узлы с путём BGP, заимствованные ссылки
    uint8_t ed25519_public_key[SC_PUBKEY_SIZE]; // публичный ключ локального узла
    char channel_id[65];              // channel_id для групп типа CHAT
    struct CONN_MGR* conn_mgr;        // менеджер соединений для этой группы
    struct TOPO_RECOVERY_CTX* recovery; // один последовательный recovery на группу
    struct TOPO_GROUP_CONNECT* connect; // авто-подключение к узлам группы (CHAT)
    struct broadcast_ctx* broadcast;      // broadcast protocol per-group context
    struct radio_ctx* radio;              // radio (PTT walkie-talkie) per-group context
    uint8_t radio_active;                 // 1 = я слушаю рацию этого канала (TOPO_FLAG_RADIO в NODEINFO)
    struct topo_node_cbk_entry* node_cbks; // цепочка подписчиков на события узлов
    struct topo_peer_ready_cbk* peer_ready_cbks;
    struct topo_peer_protocol* peer_protocols; /* результаты проверок, не зависят от сессий */
    uint8_t stopping;                 // teardown начат; новые попытки запрещены
};

/**
 * @brief Контейнер всех групп топологии экземпляра
 */
struct TOPO_GROUPS {
    struct UTUN_INSTANCE* instance;
    uint64_t next_group_generation;   // Последний выданный registration token; увеличивается даже при неудачной подготовке группы.
    struct ll_queue* group_list;          // ll_queue of TOPO_GROUP entries
    struct ll_queue* node_registry;       // реестр TOPO_NODE*: корзина по node_id, совпадение по полному X25519
    struct memory_pool* v4_sock_meta_pool;
    struct memory_pool* v4_addr_pool;
    struct memory_pool* v6_sock_meta_pool;
    struct memory_pool* v6_addr_pool;
    struct memory_pool* v4_subnet_pool;
    struct memory_pool* v6_subnet_pool;
    topo_node_updated_fn node_updated_cb; /* optional — set by chatgui's member_sync */
};

/**
 * @brief Создаёт реестр и локальную идентичность; сервисных групп пока нет.
 * Открывает общую SQLite: db_path/chats.db, без db_path — :memory:.
 *
 * При ошибке выделения пулов, открытия/инициализации SQLite или локальной записи
 * освобождает созданные ресурсы; instance->topo_groups/topo_sqlite_db остаются NULL.
 * Повторная инициализация уже имеющихся topology/SQLite запрещена.
 *
 * @param instance экземпляр utun с uasync, node_id и конфигом; topology/SQLite пока NULL
 * @return TOPO_GROUPS или NULL при ошибке
 */
struct TOPO_GROUPS* topo_groups_init(struct UTUN_INSTANCE* instance);
/* Uasync: создать только контейнер/пулы/подписки, без открытия SQLite и собственного NODEINFO.
 * Для асинхронного bootstrap: caller сохраняет и проверяет self в worker, затем устанавливает registry.
 * Возврат контейнер/NULL; destroy через topo_groups_destroy, instance должен быть новым. */
struct TOPO_GROUPS* topo_groups_init_runtime(struct UTUN_INSTANCE* instance);
/* Только uasync. 1 — точный X25519-ключ действующего участника хотя бы одной своей CHAT-группы.
 * Без БД/групп, при SQL-ошибке, заглушке, удалённом мембере — 0. Объявления/реестр nodes не дают допуска. */
int topo_groups_peer_key_known(struct UTUN_INSTANCE* instance, const uint8_t key[32]);

/**
 * @brief Обработчик события смены active mode (подписчик utun_add_activity_cbk).
 *
 * Обновляет свой nodeinfo и рассылает его по всем группам и активным BGP-пирам.
 */
void topo_group_on_activity_change(struct UTUN_INSTANCE* instance, int active, void* arg);

/**
 * @brief Освобождает все группы и контейнер.
 *
 * @param instance экземпляр utun
 */
void topo_groups_destroy(struct UTUN_INSTANCE* instance);

/**
 * @brief Возвращает UTUN-группу или NULL, если сервис не запущен. Указатель заимствован.
 */
struct TOPO_GROUP* topo_groups_get_default(struct TOPO_GROUPS* g);

/**
 * @brief Заимствованный указатель на группу по id; NULL, если её нет.
 */
struct TOPO_GROUP* topo_groups_find(struct TOPO_GROUPS* g, uint64_t group_id);

/* Адрес CHAT — подтверждённый Ed25519. Адрес UTUN — BE64 в первых восьми байтах с нулевым хвостом.
 * Только uasync. Нули означают отсутствие известного адреса. Полученный указатель принадлежит group. */
const uint8_t* topo_group_wire_id(struct TOPO_GROUP* group);
/* Найти активную группу по всем 32 байтам. Неизвестный или неоднозначный адрес даёт NULL. */
/* CHAT-группа, в том числе удерживающая ключ во время удаления. */
struct TOPO_GROUP* topo_groups_find_channel(struct TOPO_GROUPS* groups, const char* channel);

/* Индекс активной группы для локальной маршрутизации, 0 если группы нет. */
uint64_t topo_groups_channel_slot(struct TOPO_GROUPS* groups, const char* channel);

/* Заимствованный HEX-адрес CHAT по локальному индексу, пустая строка при отсутствии. */
const char* topo_groups_channel(struct TOPO_GROUPS* groups, uint64_t index);

/* Регистрирует полный адрес CHAT и выделяет свободный локальный индекс маршрутизации. */
struct TOPO_GROUP* topo_groups_create_channel(struct TOPO_GROUPS* groups, const char* channel);

struct TOPO_GROUP* topo_groups_find_wire(struct TOPO_GROUPS* groups, const uint8_t key[32]);
/* Сформировать зарезервированный адрес UTUN. id — номер маршрутизирующей группы. */
void topo_group_utun_wire_id(uint8_t key[32], uint64_t id);

/**
 * @brief Создаёт и регистрирует новую группу; существующий group_id — ошибка.
 * После регистрации присоединяет уже поднятые транспорты: CHAT — локальных мемберов,
 * UTUN — пиров, которыми владеет конфигурация. Для CHAT по group_autoconnect
 * (по умолчанию 1) включает подбор пиров. Ошибки этих попыток логируются отдельно.
 *
 * @param g контейнер групп
 * @param group_id уникальный идентификатор
 * @param group_type TOPO_GROUP_TYPE_UTUN или TOPO_GROUP_TYPE_CHAT
 * @param channel_id локальный ключ таблиц CHAT; копируется до 63 байт, NULL оставляет пустой ключ
 * @return TOPO_GROUP с local_node, conn_mgr, broadcast и radio или NULL с полным откатом создания
 */
struct TOPO_GROUP* topo_groups_create_group(struct TOPO_GROUPS* g, uint64_t group_id, uint8_t group_type, const char* channel_id);

/**
 * @brief Удаляет одну группу по id (разрушает conn_mgr, connect-цикл, broadcast,
 *        recovery, узлы и подписчиков node_cbks) и извлекает её из group_list.
 *
 * Используется при удалении канала и остановке сервиса. UTUN-группой
 * управляет utun_service_start/stop.
 */
void topo_groups_remove_group(struct TOPO_GROUPS* g, uint64_t group_id);

/**
 * @brief Присоединяет открытый транспорт того же instance к группе и начинает JOIN/ACCEPT.
 * Допускает постановку обмена до UP; READY требует UP и завершённой синхронизации.
 * Группа приобретает собственный NCD handle; повтор не сбрасывает сессию. 0/-1.
 * При активной задержке CHAT-планировщика возвращает 0 без нового JOIN;
 * повтор запустит планировщик по retry_at. Входящий JOIN принимается независимо.
 * Для обычного инициатора используйте отменяемый topo_group_peer_open().
 */
int topo_group_new_conn(struct TOPO_GROUP* group, struct ETCP_CONN* conn);

/* Готовность относится только к паре (group, peer): собственный снимок отправлен,
 * снимок пира принят до TABLE_COMPLETE текущего REQUEST_TABLE. Транспортный UP
 * этого не гарантирует. Повторное new_conn не сбрасывает готовность; REINIT
 * начинает новый обмен с новым поколением. Старый TABLE_COMPLETE игнорируется.
 * Это состояние обмена таблицами, а не добавление мембера (см. chat_join.h). */
int topo_group_peer_ready(const struct TOPO_GROUP* group, uint64_t peer_id);
/* READY именно этого прямого соединения группы. 1/0, NULL допустим. */
int topo_group_connection_ready(const struct TOPO_GROUP* group, const struct ETCP_CONN* conn);

enum topo_peer_phase { TOPO_PEER_FAILED, TOPO_PEER_CONNECTING, TOPO_PEER_SYNCING, TOPO_PEER_READY };
/* Запрос присоединения делит попытку с другими запросами этой пары (группа, пир).
 * Единственный NCD handle принадлежит группе с начала CONNECTING. REQUEST не
 * владеет транспортом. Проверка CHAT-членства общая с входящим JOIN_GROUP.
 * close отменяет только свой запрос; последний запрос отменяет незавершённую
 * попытку, если её не удерживает другой инициатор. READY сохраняется в группе.
 * DOWN/TIMEOUT сообщает FAILED, но живой запрос сохраняет интерес к NCD: поздний
 * UP возобновляет сессию. Закрытие последнего запроса отменяет неудачную попытку.
 * Активная задержка CHAT-планировщика отклоняет новый запрос с -1.
 * CLOSED/leave/удаление группы отсоединяет запрос окончательно.
 * Запрос остаётся валиден после удаления группы (phase=FAILED), caller обязан
 * закрыть его. Все операции выполняются в потоке единственного uasync.
 * progress — timebase последнего принятого NODEINFO/шага обмена, не polling time.
 * open: node_id ненулевой и не self; 0 = запрос создан в *out, -1 = ошибка (*out=NULL). */
/* Известный CHAT-мембер может удерживать транспорт для начальной MEMBERS сверки;
 * групповая сессия остаётся закрытой, пока собственный узел и пир не admitted.
 * При холодном файловом snapshot CONNECTING включает ожидание SQL; до NCD peer_conn возвращает NULL.
 * Последний close и удаление группы отменяют SQL callback, поздний результат не создаёт транспорт. */
int topo_group_peer_open(struct TOPO_GROUP* group, uint64_t node_id, struct TOPO_PEER_REQUEST** out);
/* Открыть запрос по полному X25519 из конфига или проверенной записи участника. */
int topo_group_peer_open_key(struct TOPO_GROUP* group, uint64_t node_id, const uint8_t* x, struct TOPO_PEER_REQUEST** out);
/* Открыть CHAT-запрос для полного Ed25519, полученного из списка участников. */
int topo_group_peer_open_ed(struct TOPO_GROUP* group, const uint8_t ed[32], struct TOPO_PEER_REQUEST** out);
/* Освободить свой запрос; после вызова request недействителен. NULL допустим. */
void topo_group_peer_close(struct TOPO_PEER_REQUEST* request);
struct ETCP_CONN* topo_group_peer_conn(const struct TOPO_PEER_REQUEST* request); /* borrowed, including CONNECTING */
/* Завершить всю сессию этой пары: LEAVE, удаление путей, отсоединение всех запросов. */
void topo_group_peer_leave(struct TOPO_GROUP* group, uint64_t node_id);
/* Текущая фаза; NULL или отсоединённый запрос дают FAILED. */
enum topo_peer_phase topo_group_peer_phase(const struct TOPO_PEER_REQUEST* request);
/* Время последнего прогресса в единицах 0.1 ms; 0, если сессии нет. */
uint64_t topo_group_peer_progress(const struct TOPO_PEER_REQUEST* request);

/* Причина завершения групповой сессии определяет необходимость recovery. */
enum topo_group_remove_reason {
    TOPO_REMOVE_TRANSPORT_DOWN,
    TOPO_REMOVE_LOCAL_LEAVE,
    TOPO_REMOVE_REMOTE_LEAVE,
    TOPO_REMOVE_MEMBER_INVALID,
    TOPO_REMOVE_REJECTED
};
/* Сбросить сессию и удалить пути через conn, разослать WITHDRAW для потерянных узлов.
 * При DOWN живые запросы сохраняют NCD для повторного UP. Только потеря транспорта
 * запускает recovery каскадно потерянных маршрутов. */
void topo_group_remove_conn(struct TOPO_GROUP* group, struct ETCP_CONN* conn, enum topo_group_remove_reason reason);
/* После пересчёта admitted: закрыть недопущенные сессии/запросы только этой группы. Общий транспорт сохраняется. */
void topo_group_revalidate_members(struct TOPO_GROUP* group);
/* Допуск сервиса CHAT: существующая работающая группа, self и peer имеют активное admitted-членство.
 * Готовность прямой сессии не требуется. Только чтение БД в uasync; 1 — разрешено, 0 — отказ/ошибка. */
int topo_group_chat_allowed(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t peer);
/* Проверить по MEMBERS полный ключ своего узла и этого прямого пира; 1 — есть допуск обоих к CHAT. */
int topo_group_chat_connection_allowed(const struct TOPO_GROUP* group, struct ETCP_CONN* conn);
/* Проверить допуск своего устройства и участника по полному Ed25519 в активной CHAT-группе. 1/0. */
int topo_group_chat_key_allowed(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]);

/* Допуск raw-данных CHAT: текущее членство self/соседа и принятая живая сессия на этом conn.
 * conn=NULL проверяет локального производителя. Без мутаций; 1/0, отказ с причиной в логе.
 * Для существующих UTUN-групп сохраняет прежний допуск. Не использовать для JOIN/MEMBERS bootstrap. */
int topo_group_data_allowed(struct TOPO_GROUP* group, struct ETCP_CONN* conn);

/**
 * @brief Обрабатывает пакет NODEINFO.
 *
 * data/len — полный TOPOMSG_NODEINFO_PKT с динамическими секциями; память заимствована.
 * Диспетчер заранее проверяет поколения/членство сессии. Здесь проверяются длина,
 * адрес группы в заголовке, резерв в теле, тип, подпись, timestamp и цепочка; добавляется ключ from.
 * FILE/RW: если identity ещё нет в памяти, пакет копируется и ждёт READ сохранённого snapshot.
 * 0 в этом случае означает принятие для replay; FIFO соседа удерживает последующие BGP пакеты.
 * Смена сессии/free отменяет READ и очередь; ошибки/переполнение запрещают READY и запускают recovery.
 * Старый timestamp обновляет только путь. Для новой записи путь готовится до commit:
 * отказ памяти сохраняет прежние запись/подсети/пути. Входная цепочка < MAX_HOPS,
 * сохранённая после добавления соседа — 1..MAX_HOPS.
 * Изменения распространяются из актуального состояния с backpressure.
 * Для UTUN прежние IPv4-маршруты узла удаляются перед установкой нового набора.
 * Пересечение/OOM маршрутов оставляет новый набор неустановленным, возвращает -1
 * и запрещает READY; подписанная запись и групповые пути уже могут быть приняты.
 *
 * @return 0 при применении/игнорировании собственного объявления, -1 при ошибке
 */
int topo_group_process_nodeinfo(struct TOPO_GROUP* group, struct ETCP_CONN* from, const uint8_t* data, size_t len);
/* Присоединить UP-транспорт; переданный handle остаётся у вызывающего. 0/-1. */
int topo_group_join_peer(struct TOPO_GROUP* group, struct NODE_CONN_DIRECT* handle);
/* Отсоединить пира и запланировать LEAVE. Чужой handle не забирает. 0/-1. */
int topo_group_leave_peer(struct TOPO_GROUP* group, struct NODE_CONN_DIRECT* handle);

/**
 * @brief Обрабатывает WITHDRAW.
 *
 * data/len — заимствованный полный TOPOMSG_WITHDRAW_PKT; поколения/членство
 * проверяет диспетчер. Удаляет только путь sender; узел удаляется без оставшихся путей.
 * Неизвестный узел/отсутствующий путь sender — успешный no-op.
 *
 * @return 0 при успехе, -1 при неверных аргументах/длине
 */
int topo_group_process_withdraw(struct TOPO_GROUP* group, struct ETCP_CONN* sender, const uint8_t* data, size_t len);

/**
 * @brief Планирует NODEINFO узла одному пиру с backpressure.
 * group/node/conn заимствованы на время вызова; очередь хранит только node_id.
 * Запись и RTT читаются при отправке; повторные изменения узла объединяются.
 * 0 — запланировано либо сессия ещё не ACCEPT, -1 — ошибка. Ошибка самой отложенной
 * отправки сбрасывает READY и перезапускает обмен по watchdog.
 */
int topo_group_send_nodeinfo(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, struct ETCP_CONN* conn);

/**
 * @brief Поиск оптимального ETCP соединения для указанного node_id.
 *
 * Выбирает минимум hop_count среди UP-путей, при равенстве — минимум ID соседа.
 * При отсутствии живого пути возвращает NULL.
 *
 * @param group    указатель на TOPO_GROUP
 * @param node_id целевой узел
 * @return заимствованный ETCP_CONN* или NULL
 */
struct ETCP_CONN* topo_group_find_conn_for_node(struct TOPO_GROUP* group, uint64_t node_id);

/**
 * @brief Атомарно заменяет путь через conn; остальные соседи не затрагиваются.
 *
 * nq/conn/hop_list заимствованы; цепочка 1..MAX_HOPS копируется. Семантику и петли
 * проверяет вызывающий; функция проверяет размер и атомарно заменяет путь conn.
 * @return 0 при успехе, -1 при неверных аргументах/OOM
 */
int topo_group_add_path(struct TOPO_GROUP_NODE* nq, struct ETCP_CONN* conn, const struct topo_hop* hop_list, uint8_t hop_count, uint16_t cumulative_rtt);

/**
 * @brief Удаляет conn из paths узла.
 *
 * @return число удалённых путей; доступность проверяется отдельно
 */
int topo_group_remove_path(struct TOPO_GROUP_NODE* nq, struct ETCP_CONN* conn);

void topo_groups_set_node_updated_cb(struct TOPO_GROUPS* groups, topo_node_updated_fn fn);

/** Разрешить/запретить NAT check для локальных подсетей (тестовый хелпер, делегат в nat_detection) */
void topo_group_set_nat_check_local(struct TOPO_GROUP* group, int allow);

/* ── Рация (PTT walkie-talkie, per-CHAT-группа) ── */

/* Включить/выключить прослушивание рации канала: публикует local_node->radio и
 * рассылает NODEINFO (TOPO_FLAG_RADIO) через BGP. Только CHAT-группы.
 * RW меняет локальное прослушивание немедленно, объявляет флаг после commit новой версии;
 * при ошибке прежний анонс сохраняется, повтор команды снова пробует публикацию. */
void topo_group_set_radio(struct TOPO_GROUP* group, int on);

/* Число узлов группы, слушающих рацию (radio==1), без учёта себя. */
int topo_group_radio_subscribers(struct TOPO_GROUP* group);

/* BGP node event callbacks ── */
void topo_group_add_node_cbk(struct TOPO_GROUP* group, topo_node_event_fn fn, void* arg);
void topo_group_remove_node_cbk(struct TOPO_GROUP* group, topo_node_event_fn fn, void* arg);


#ifdef __cplusplus
}
#endif
#endif // TOPO_GROUP_H
