/* utun_instance — владелец ядра и сервисов одного узла.
 * Ядро содержит транспорт, маршрутизатор, идентичность и общую БД.
 * create → core_start; UTUN и чат запускаются отдельно через service_start/stop.
 * Все операции — в потоке переданного UASYNC; stop/reload/destroy вне callbacks останавливаемых модулей.
 * Цикл uasync_poll запускает вызывающий; один UASYNC можно разделять между несколькими instance.
 * destroy останавливает сервисы и освобождает ядро, но не UASYNC. Подробности: utun_instance_doc.md. */
#ifndef UTUN_INSTANCE_H
#define UTUN_INSTANCE_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../lib/memory_pool.h"
#include "../lib/sqlite3.h"
#include "../lib/db_async.h"
#include "secure_channel.h"
#include "etcp_padding.h"
#include "reality_io.h"
#include "etcp_api.h"
#include "transport_layer/etcp_tx_stats.h"
#include "config_parser.h"
#include "stcp_link.h"
#include "firewall.h"
#include "eim_nat.h"
#include "nat_transport.h"
#include "media_delivery/media_delivery.h"
#include "media_async/media_async.h"
#include "proxy/tcp_proxy_client.h"
#include "etcp_router.h"
#include "proxy/tcp_proxy_server.h"
#include "ntp_time.h"
#include "ntp_node_time.h"

#include "topo_group.h"
#include "chat/chat_setting.h"
#include "chat/chat_event.h"

// Forward declarations
struct utun_config;
struct chat_core_ctx;
struct chat_sync;
struct join_key_entry;   /* chat_join.c */
struct join_relay;
struct dm_state;
struct dm_mb_state;
struct dm_media_state;
struct file_transfer;
struct call_ctx;         /* call/call.c */
struct call_audio_ops;   /* Заимствованный frontend аудиозвонка; привязка живёт до detach ядра. */
struct radio_instance;   /* radio/radio.c */
struct radio_call_context; /* radio/radio_call.c */
struct radio_headless;   /* radio/radio_headless.c */
struct stream_service;   /* stream/stream.c — подписки и публикация данных */
struct multicast_ctx;   /* routing_layer/multicast.c — общие FIFO и отчёты потерь */
struct uasync_s;
struct ROUTE_TABLE;
struct ETCP_CONN;
typedef void (*etcp_new_conn_fn)(struct ETCP_CONN* conn, void* arg);
struct ETCP_SOCKET;
struct NODE_CONN_DIRECT;
struct tun_if;
struct ETCP_BINDINGS;
struct ETCP_ROUTER_BINDINGS;
struct TOPO_GROUP;
struct TOPO_GROUPS;
struct control_server;

struct PING_CONTEXT;
struct CONN_MGR;
struct DB_SYNC;
struct NAT_DETECTION;
struct STUN_NAT;
struct merkle_sync;
struct db_async;

struct NETWORK_ENTRY {
    uint8_t pubkey[SC_PUBKEY_SIZE];             // полный публичный адрес и ключ индекса сети
    uint8_t signing_key[SC_PRIVKEY_SIZE];       // бинарный ключ подписи сети
    char name[64];                             // имя [network:name], ASCIIZ
};

// Реестр прямого ETCP по полному X25519. До установки ключа запись доступна только при обходе.
struct conn_queue_entry {
    uint64_t peer_node_id;                     // числовой адрес ETCP для диагностики, 0 до определения пира
    struct ETCP_CONN* conn;                    // живой транспорт; удаляется при close
    uint8_t x25519[SC_PUBKEY_SIZE];            // ключ индекса
};

/* Постоянный транспорт ядра к пиру [client]; UTUN приобретает отдельный групповой запрос.
 * config/links принадлежат записи; привязка локального сокета хранится именем, без ссылок на старый конфиг. */
struct CONFIG_CONN_HANDLE {
    uint64_t node_id;
    uint8_t x25519[SC_PUBKEY_SIZE]; // проверенный публичный ключ секции client
    struct UTUN_INSTANCE* instance; // заимствован до закрытия записи
    struct CFG_CLIENT config; // собственная копия параметров и списка links
    struct NODE_CONN_DIRECT* transport; // принадлежит ядру, сохраняется при остановке UTUN
    struct TOPO_PEER_REQUEST* request; // принадлежит UTUN; NULL, когда сервис выключен
    void* retry_timer; // повтор после окончательного CLOSED; отменяется перед освобождением записи
    struct CONFIG_CONN_HANDLE* next;
};

// Подписка на смену active mode (client_activity). Событие рассылается
// подписчикам (topo_group, keepalive и т.д.) при каждом изменении активности.
typedef void (*utun_activity_cbk_fn)(struct UTUN_INSTANCE* instance, int active, void* arg);
struct utun_activity_cbk_entry {
    utun_activity_cbk_fn fn; // синхронный обработчик эффективной активности
    void*                arg; // заимствованный контекст обработчика
    struct utun_activity_cbk_entry* next; // следующая подписка
    uint8_t removed; // tombstone до выхода из внешней рассылки
};

// Подписка на смену спячки пира (keepalive-протокол). Сервер (или любой узел)
// получает от мобильного пира sleep-анонс и уведомляет подписчиков:
// sleeping=1 — пир ушёл в спячку (в фон), 0 — проснулся (дедлайн истёк / обычный keepalive).
typedef void (*peer_sleep_cbk_fn)(struct ETCP_CONN* conn, int sleeping, void* arg);
struct peer_sleep_cbk_entry {
    peer_sleep_cbk_fn fn; // синхронный обработчик sleep/wake пира
    void*             arg; // заимствованный контекст обработчика
    struct peer_sleep_cbk_entry* next; // следующая подписка
    uint8_t removed; // снятая подписка не вызывается, память удерживает текущая рассылка
};

// Состояние одного узла. Вложенные модули освобождаются через их lifecycle API.
struct UTUN_INSTANCE {
    struct etcp_tx_history tx_history; // секундное окно ACK всех транспортов ядра
    struct reality_owner reality_owner; // relay и replay-кеш REALITY данного ядра
    uint8_t core_started, utun_started, chat_started; // Успешный запуск ядра / UTUN / полного чат-сервиса.
    uint8_t group_started, chat_content_started, kanban_started; // Общий runtime групп / подготовка истории чата / канбан.
    uint64_t kanban_generation; // Uasync: номер запуска канбана; ожидающие DB-команды прежнего запуска больше не применяются.
    uint64_t group_generation; // Uasync: номер создания общего контекста групп; поздние команды не переходят в новый runtime.
    uint8_t destroying; // запрещает повторный запуск ядра/повторное destroy во время очистки
    uint8_t database_starting, database_ready, database_incompatible; // Асинхронный bootstrap БД / готовность / подтверждённое отличие схемы.
    char database_error[2048]; // Причина отказа worker bootstrap для frontend, без секретов; пусто при успехе.
    uint8_t chat_starting; // Ожидается SQL-подготовка чат-сервиса; повторный start отклоняется до done.
    uint64_t chat_start_generation; // Stop отменяет применение результата прежнего async start.
    uint8_t reload_pending; // Новый FILE instance восстанавливает прежние сервисы; running держит внешний poll до результата.

    // Identification
    char name[MAX_CONN_NAME_LEN];    // Instance name from config

    // Конфигурация принадлежит экземпляру после успешного create.
    struct utun_config *config;
    
    // TUN interface
    struct tun_if* tun;
    
    // Принадлежащие UTUN системные маршруты; независимы от изменений config.
    struct tun_route* tun_routes;       // только успешно установленные системные маршруты UTUN

    struct ROUTE_TABLE* rt;             // таблица маршрутов подсетей
    struct TOPO_GROUPS* topo_groups;    // Groups module for topology exchange
    sqlite3* topo_sqlite_db;            // собственная общая SQLite; открывается при create с топологией
    struct topo_rtt_pending* rtt_pending; /* Накопители RTT узлов/endpoints; только uasync, stop до DB teardown. */
    struct db_async* db_worker;        // Общая очередь SQL; при переносе чтений открывается строго READONLY, без второго writer.
    struct packet_transport* packet_transport; // Автономные TX/RX и общие криптопулы; создаётся при первом сетевом endpoint.
    struct group_policy_cache* group_access; // Подтверждённые права/ключи групп; только uasync, заменяются после commit.
    void* node_loads;                  // struct topo_node_load*: только uasync, запросы отменяются до закрытия worker.
    uint8_t node_load_stopping;        // Запрещает новые node-load во время постоянной остановки/уничтожения.
    unsigned node_load_callbacks;     // Вложенность node-load done; destroy переносится за пределы callback.
    struct NAT_DETECTION* nat_det;      // NAT detection module
    struct STUN_NAT* stun_nat;          // независимая локальная STUN-диагностика UDP
    struct stun_ports* stun_port_history; // ранее использованные STUN-порты; переживает reload config
    struct merkle_sync* msync;         // Merkle tree sync module (per-instance)
    
    // Identification
    uint64_t node_id;

    struct SC_MYKEYS my_keys;

    uint8_t my_ed25519_pubkey[SC_PUBKEY_SIZE];  // свой Ed25519 pubkey (для подписи метрик)
    uint8_t my_ed25519_privkey[SC_PRIVKEY_SIZE]; // seed Ed25519 устройства, из него выведен my_keys
    struct devgroup_local_identity local_devgroup; // Проверенные собственные ключи группы устройств; только uasync, очищаются при destroy.
    uint64_t local_snapshot_reserved;  // Последний выданный timestamp своего NODEINFO; больше не переиспользуется при ошибке записи.
    uint64_t local_snapshot_committed; // Последний durable NODEINFO; более новый снимок нельзя отправлять в сеть до commit.
    
    uint8_t next_socket_id;  // начало поиска свободного ID 0..255; поколение различает повторное использование

    // Заимствованный цикл событий: создаёт и уничтожает вызывающий.
    struct UASYNC* ua;
    
    // State
    int running;                    // продолжать цикл приложения; готовность задают отдельные *_started

    // Connections (очередь всех подключений для instance)
    struct ll_queue* connections;        // indexed by peer_node_id (0=pending), data=conn_queue_entry
    
    // Callback chain for new ETCP connections
    struct etcp_inst_cbk_entry* new_conn_cbks;
    struct etcp_link_status_cbk_entry* link_status_cbks;
    struct etcp_status_cbk_entry* conn_status_cbks;  // instance-level: NEW/UP/DOWN/DELETE
    struct etcp_socket_cbk_entry* socket_cbks;       // instance-level: socket ADDR/STATUS changes
    unsigned etcp_callbacks_depth;                 // Вложенность ETCP-рассылок: снятые подписки освобождаются при выходе из внешней
    struct nodeinfo_cbk_entry* nodeinfo_cbks;         // глобальная подписка на изменения nodeinfo
    void* test_user_ptr;             // Generic user pointer (used by tests)

    struct memory_pool* data_pool;// для входных-выходных данных пакета
    struct memory_pool* pkt_pool;    // ETCP_DGRAM и буферы сетевых пакетов
    struct memory_pool* ack_pool;    // ACK_PACKET
    
    // Active sockets (UDP + TCP, is_tcp=1 flag)
    struct ETCP_SOCKET* etcp_sockets;// linked-list (UDP + TCP)
    struct stcp_server *stcp_servers; // TCP servers linked list (via stcp_link.c)
    void* socket_monitor;            // SOCKET_MONITOR* (opaque, transport_layer/socket_monitor.c)
    void* auto_socket_state;         // AUTO_SOCKET* (opaque, transport_layer/auto_socket.c)
    uint8_t auto_socket_enabled;     // 1 = auto_sockets enabled from config
    
    // Pending one-shot pings (for callback on PONG or timeout)
    struct PING_CONTEXT* pending_pings;
    struct ETCP_PADDING padding; // диапазон и таймер PING/PONG без ETCP_CONN
    
    // Routing statistics
    uint64_t routed_packets;
    uint64_t dropped_packets;
    
    // ETCP API bindings (per-instance)
    struct ETCP_BINDINGS api_bindings;
    
    // Control server for monitoring
    struct control_server* control_srv;
    
    // Firewall
    struct firewall_ctx fw;
    
    // EIM NAT
    struct eim_nat_ctx nat;
    struct nat_transport_ctx nat_tr;

    // Media delivery
    struct media_delivery_ctx md;

    // Media async engine (thread-per-task for crypto/file ops)
    struct media_async* media_async;
    struct attachment_service* attachments; /* Общий сервис файлов; живёт до остановки групп, независимо от chat_service. */
    
    // Последний результат init/reload endpoint: 0=успех, -1=ошибка хотя бы одного обязательного endpoint.
    int socket_init_status;

    // TCP proxy client (optional, NULL if not enabled)
    struct tcp_proxy_client* tcp_proxy_client;

    // etcp_router bindings и seq-connections (per-instance service routing)
    struct ETCP_ROUTER_BINDINGS router_bindings;
    struct ll_queue* router_conns;
    struct multicast_ctx* multicast; // принадлежит ядру, общий для всех topo-групп
    uint64_t router_loop_drops;     // пакеты, вернувшиеся в уже посещённый узел
    uint64_t router_path_errors;    // некорректный или переполненный список пройденных узлов
    struct CONN_MGR* conn_mgr;       // заимствован у UTUN-группы; NULL при остановленном UTUN
    struct DB_SYNC* db_sync;         // Distributed DB sync (может быть NULL)
    const struct call_audio_ops* call_audio_ops; // Регистрация frontend, только uasync; сохраняется при stop/start чата.

    // Контексты чат-сервиса; освобождаются при chat_service_stop.
    struct chat_core_ctx* chat_core;      // каналы, сообщения и доступ к общей БД
    const struct chat_local_provider* bluetooth_discovery; // заимствованный API BLE владельца GUI; снять до destroy instance
    const struct chat_join_carrier* join_carrier; // заимствованный транспорт JOIN до допуска ETCP; снять до destroy provider
    struct local_transport* local_transport; // отдельный открытый LAN UDP, принадлежит чат-сервису
    struct chat_sync*     chat_sync;      // join и синхронизация каналов
    struct join_key_entry* join_keys;     // зарегистрированные ключи приглашений
    struct join_relay* join_relays;        // обратные маршруты сквозных кадров JOIN
    int join_stopping;                   // запрещает JOIN при остановке сервиса
    struct chat_invite_build_req* invite_gui_request; // текущее построение ссылки для GUI
    struct dm_state*      dm;             // личные сообщения
    struct dm_mb_state*   dm_mailbox;     // хранилище личных сообщений для получателей
    struct dm_media_state* dm_media;      // собственные файлы и промежуточное ciphertext-хранилище DM
    struct file_transfer* file_transfer; // DM-экземпляр передачи файлов с CM-владением.
    struct file_transfer* file_transfer_services; // Реестр независимых файловых сервисов, каждый со своим допуском/ID router.
    struct member_files* member_files;   // файловый сервис участников CHAT-групп
    struct call_ctx*      call;           // call/call.c — P2P audio call
    struct radio_call_context* radio_call; // адресные приглашения в рацию, принадлежит чат-сервису
    void*                 call_audio;     // call/call_headless.c — audio stream socket
    struct radio_instance* radio;         // radio/radio.c — PTT walkie-talkie (frame cb + counters)
    struct radio_headless* radio_headless; // radio/radio_headless.c — audio stream socket
    struct stream_service* stream;        // живёт в чат-сервисе, независимо от UTUN
    struct chat_setting_state chat_settings;  // текущие настройки чата
    chat_event_handler_fn chat_event_handler;  // получатель событий чата
    void*                 headless;       // контекст TCP-управления headless-чатом

    // E2E encryption cache — per-peer sc_context_t with derived session key
#define E2E_CTX_CACHE_SIZE 8
    struct { struct secure_channel ctx; uint8_t valid; } e2e_ctx_cache[E2E_CTX_CACHE_SIZE];

    struct broadcast_ctx* broadcast;   // Broadcast protocol instance handler (ETCP_ID_BROADCAST dispatcher)

    uint8_t  client_type;              // CLIENT_TYPE_SERVER/DESKTOP/MOBILE (from config)
    uint16_t keepalive_interval;        // желаемый keepalive (ms), из конфига. для handshake
    uint8_t  client_activity;          // CLIENT_ACTIVITY_STANDBY/ACTIVE
    uint8_t  client_activity_requested; // режим frontend; stream owners могут временно удерживать эффективный ACTIVE
    uint8_t published_phone_status;    // Флаги телефона для отправки с подтверждённой версией NODEINFO.
    uint8_t status_publish_pending;     // Идёт запись версии NODEINFO для статуса телефона или рации.
    uint8_t  phone_state;              // 0=неизвестно, 1=свободен, 2=занят; живое состояние ОС, не сохраняется
    uint8_t  incoming_call_policy;     // CALL_POLICY_*: независимые настройки публикации и допуска входящих
    struct utun_activity_cbk_entry* activity_cbks;  // подписки на смену client_activity
    struct peer_sleep_cbk_entry*    peer_sleep_cbks; // подписки на смену спячки пира (keepalive)
    unsigned lifecycle_callbacks_depth; // общая вложенность рассылок activity/peer_sleep
    uint64_t activity_version; // смена эффективной активности отменяет остаток более старой рассылки

    // TCP proxy server (exit node)
    struct tcp_proxy_server tcp_proxy_server;
    struct udp_proxy_ctx* udp_proxy;
    struct icmp_proxy_ctx* icmp_proxy;

    // Networks (queue of NETWORK_ENTRY, indexed by 56-bit id)
    struct ll_queue* networks;

    uint32_t etcp_connect_timeout_tb;  // Initial timeout in 0.1ms units, default 20000 (2s)

    // NTP time synchronization
    struct NTP_TIME ntp;
    struct NTP_NODE_TIME ntp_node;

    // Постоянные транспорты [client] ядра и независимые запросы участия в UTUN.
    struct CONFIG_CONN_HANDLE* config_conn_handles;

    // Per-instance NCD state (replaces global statics in node_conn_direct.c)
    void*  ncd_registry;        // struct ncd_entry* linked list
    uint8_t ncd_control_bound;  // 1 = etcp_bind(ETCP_ID_NCD_CONTROL) сделан
    uint64_t chat_admin_generation; // Uasync: регистрация сервиса ключей; queued jobs предыдущей регистрации отменяются.
};

/* Синхронный bootstrap для offline/RAM fixtures: выполнять до запуска event loop.
 * Production frontend использует create_from_config_async, чтобы файловый SQL не задерживал uasync.
 * ua заимствован, config_file — обязательный путь. Файл может создаваться/нормализоваться updater.
 * Уже открывает listeners, SQLite/топологию, router/proxy и мониторинг; core/UTUN/chat ещё не запущены.
 * Возвращает instance либо NULL с освобождением частично созданных ресурсов. */
struct UTUN_INSTANCE* utun_instance_create(struct UASYNC* ua, const char* config_file);
/* Синхронный offline bootstrap как create, без updater/файлового parse: config содержит корректные ключи.
 * Успех передаёт владение config ядру; при NULL config остаётся caller (node_id мог быть исправлен).
 * debug меняет общий logger процесса даже при отказе; log_file выбирается только до открытия файлового logger. */
struct UTUN_INSTANCE* utun_instance_create_from_config(struct UASYNC* ua, struct utun_config* config);
/* Создать ядро с единственным RW worker: open/schema/проверка своего журнала/self snapshot выполняются в worker.
 * Возвращает pending instance, передавая ему config; NULL оставляет config caller и не вызывает done.
 * done(arg,instance,result) ровно один раз в uasync, в том числе при ошибке/stop. До него сервисы не запускать.
 * При ошибке instance остаётся caller: уничтожить вне callback. database_error/incompatible сохраняют причину схемы.
 * :memory: — синхронный RAM bootstrap; done может прийти до возврата. ua/arg живут до done/destroy.
 * Callback не уничтожает instance/uasync; внешний цикл продолжает poll, ожидания БД в uasync нет. */
typedef void (*utun_instance_ready_fn)(void* arg, struct UTUN_INSTANCE* instance, const struct db_async_result* result);
struct UTUN_INSTANCE* utun_instance_create_from_config_async(struct UASYNC* ua, struct utun_config* config,
    utun_instance_ready_fn done, void* arg);
/* Как create из ASCIIZ config_text; строка заимствована лишь на вызов. Использует mkstemp в /tmp,
 * updater и файловый parse, затем unlink. Нужна доступная ФС; ошибки дают NULL. */
struct UTUN_INSTANCE* utun_instance_create_from_str(struct UASYNC* ua, const char* config_text);
/* Остановить сервисы/транспорт и освободить instance/config; NULL допустим. UASYNC остаётся caller.
 * Выполняет drain отложенных callbacks и join media workers; вызывать вне callbacks и без параллельных обращений.
 * Заимствованные SQLite statements/backup должны быть закрыты caller; BUSY логируется и переводится в close_v2. */
void utun_instance_destroy(struct UTUN_INSTANCE* instance);
/* Запустить репликацию, NCD/keepalive, [client], control и время; без вступления в группы. Первый успех: running=core_started=1.
 * При core_started возвращает 0 без изменения running. Ошибка — -1; подготовленные ресурсы остаются для retry/destroy.
 * Отказ NTP/STUN не фатален. NULL/разрушаемый instance отклоняется. */
int utun_core_start(struct UTUN_INSTANCE* instance);
/* Uasync: запустить чат через WRITE prepare → commit → noSQL runtime activation.
 * Нужен core_start и готовая БД; SQLite остаётся только у RW worker. done(arg,result) обязателен, ровно один раз.
 * Отказ/OOM/FULL может завершиться синхронно; во время принятой работы stop отменяет позднюю activation.
 * Ошибка SQL сохраняет прежний runtime; ошибка activation очищает частичный новый сервис.
 * inst/arg живут до done/drain. Не вызывать destroy из done; RAM допускает синхронный старт. */
void chat_service_start_async(struct UTUN_INSTANCE* instance, db_async_done_fn done, void* arg);
/* Тот же WRITE prepare/activation только для общей службы групп: MEMBERS/JOIN/роли/профили,
 * без запуска истории чата, DM и медиа. done/lifetime/отмена как у chat_service_start_async.
 * Нужен для независимого канбана и восстановления ранее запущенных сервисов. */
void group_service_start_async(struct UTUN_INSTANCE* instance, db_async_done_fn done, void* arg);
/* Запустить UTUN-группу, TUN/NAT по конфигу и отдельные запросы к [client]. Нужны core_start и топология.
 * 0 — успех/уже запущен; -1 — откат ресурсов UTUN, ядро и чат сохраняются. */
int utun_service_start(struct UTUN_INSTANCE* instance);
/* Остановить UTUN, сохранив ядро и чат. Повтор допустим. */
void utun_service_stop(struct UTUN_INSTANCE* instance);
/* Последовательно core_start → UTUN (при наличии топологии) → чат (chatserver_enabled).
 * 0/-1; ошибка позднего сервиса сохраняет ранее запущенные сервисы. */
int utun_instance_init(struct UTUN_INSTANCE *instance);
/* Перечитать config_file через updater; ua должен совпадать с instance->ua. Всегда использовать результат.
 * Ошибка подготовки сохраняет прежний указатель. Смена негорячих global/подсетей либо automatic servers
 * пересоздаёт ядро с прежними core/UTUN/chat/running; повторные server-имена также требуют restart.
 * Ошибка после destroy возвращает NULL. RW FILE restart возвращает pending instance: внешний poll
 * продолжает bootstrap/восстановление сервисов; reload_pending снимается после готовности, ошибка снимает running.
 * Ручные endpoints обновляются по имени и всем параметрам, неизменённые сохраняются (включая port=0).
 * Частичный reload не транзакционный: отказ endpoint логируется, новый config сохраняется для следующего retry. */
struct UTUN_INSTANCE *utun_instance_reload(struct UTUN_INSTANCE *instance, struct UASYNC *ua, const char *config_file);
/* Снять running и разбудить цикл; ресурсы освобождает destroy(), а не stop(). */
void utun_instance_stop(struct UTUN_INSTANCE *instance);
/* Общий переключатель TUN (по умолчанию 0): влияет на последующий UTUN start и proxy test mode при create. */
void utun_instance_set_tun_init_enabled(int enabled);
/* Общий переключатель топологии (по умолчанию 1): влияет на последующие create, включая открытие SQLite. */
void utun_instance_set_topo_group_enabled(int enabled);
/* Сохраняет желаемый режим frontend; stream owners удерживают ACTIVE до последнего close.
 * Применяет эффективную активность и мобильный standby до уведомления подписчиков; выполнять в uasync-потоке.
 * Повторная команда согласует фактический режим без повторного BGP-события. SERVER — успешный no-op.
 * 0 — успех, -1 — NULL/ошибка standby с откатом режима. phone_state не меняется. */
int utun_set_client_activity(struct UTUN_INSTANCE* instance, int active);
/* Подписки уникальны по fn+arg, новые идут в голову; arg заимствован. NULL instance/fn — no-op,
 * OOM логируется без добавления. Remove из callback безопасен; память удерживается до конца внешней рассылки.
 * Новая подписка не получает уже начатое событие, но участвует во вложенном. Destroy внутри callback запрещён. */
void utun_add_activity_cbk(struct UTUN_INSTANCE* instance, utun_activity_cbk_fn fn, void* arg);
/* Удалить подписку fn+arg; отсутствие — no-op. Вложенная новая активность отменяет остаток старого события. */
void utun_remove_activity_cbk(struct UTUN_INSTANCE* instance, utun_activity_cbk_fn fn, void* arg);
/* Подписка на смену состояния конкретного транспорта; nesting исполняется синхронно до продолжения внешней рассылки. */
void utun_add_peer_sleep_cbk(struct UTUN_INSTANCE* instance, peer_sleep_cbk_fn fn, void* arg);
/* Удалить peer-sleep подписку fn+arg; отсутствие/NULL — no-op. */
void utun_remove_peer_sleep_cbk(struct UTUN_INSTANCE* instance, peer_sleep_cbk_fn fn, void* arg);
/* Синхронно передать заимствованный conn и sleeping (1 сон, 0 пробуждение); NULL допустим. */
void utun_fire_peer_sleep_cbk(struct ETCP_CONN* conn, int sleeping);

/* Вывести sys/info счётчики ресурсов и простые sanity-проверки списков с меткой phase.
 * Живые ресурсы сами по себе не означают утечку; функция ничего не освобождает. */
void utun_instance_diagnose_leaks(struct UTUN_INSTANCE* instance, const char* phase);

/* Заимствованное соединение по полному проверенному X25519, NULL при отсутствии. */
struct ETCP_CONN* instance_find_conn_by_key(struct UTUN_INSTANCE* inst, const uint8_t x[SC_PUBKEY_SIZE]);
/* Найти подтверждённое соединение участника по каноническому Ed25519. NULL, если handshake ещё не завершил проверку ключа. */
struct ETCP_CONN* instance_find_conn_by_ed(struct UTUN_INSTANCE* inst, const uint8_t ed[SC_PUBKEY_SIZE]);
/* По числовому адресу ETCP возвращает только однозначное соединение, иначе NULL.
 * Оба поиска выполняются в потоке ядра; наличие записи не гарантирует UP. */
int instance_lookup_conn(struct UTUN_INSTANCE* inst, uint64_t node_id, struct ETCP_CONN** result); /* 1 найден, 0 нет, -1 пустая запись, -2 коллизия; result обязателен. */
static inline struct ETCP_CONN* instance_find_conn(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    struct ETCP_CONN* conn;
    instance_lookup_conn(inst, node_id, &conn);
    return conn;
}

#ifdef __cplusplus
}
#endif
#endif // UTUN_INSTANCE_H
