/* INI-конфигурация ядра: слушающие сокеты, исходящие пиры, сети/подсети и настройки сервисов.
 * parse_config читает файл, parse_config_from_buf — текст; filename задаёт источник в диагностике.
 * Успех возвращает выделенную utun_config со списками; освобождение — только free_config.
 * Файл и буфер проходят один разбор. Пробелы вокруг секций, ключей и значений удаляются;
 * имена секций нечувствительны к регистру, имена ключей чувствительны. Комментарий — #.
 * Ошибка обработчика известной опции отклоняет весь конфиг с освобождением частичной модели.
 * HEX проверяется целиком; network требует оба ключа. Неизвестные секции игнорируются с WARN;
 * [gui] обрабатывает только utun_enabled. Числа проверяются до сужения, bool — yes/no/true/false/1/0.
 * Повторные значения применяются по порядку; ошибочный ранний дубль также отклоняется.
 * Встроенный NUL и строки >1022 байтов до LF запрещены (CR входит в этот предел).
 * Парсер не открывает сетевые сокеты, но синхронно разрешает addr/control через getaddrinfo;
 * на Windows вызывающий заранее инициализирует socket subsystem. Сервисы запускает utun_instance. */
#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "secure_channel.h"
#include "transport_layer/reality.h"
#include "chat/chat_setting.h"
#include "transport_layer/connection_order.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CONN_NAME_LEN 64
#define MAX_KEY_LEN 256
#define MAX_ADDR_LEN 64
#define PROXY_DEFAULT_MAX_FLOWS 1024
#define PROXY_DEFAULT_MAX_FLOWS_PER_NODE 256

struct IP {
    sa_family_t family; // AF_INET/AF_INET6, 0 — адрес не задан; байты addr в сетевом порядке
    union {
        struct in_addr  v4;
        struct in6_addr v6;
    } addr;
};

#define CFG_SERVER_TYPE_UNKNOWN 0
#define CFG_SERVER_TYPE_PUBLIC  1
#define CFG_SERVER_TYPE_NAT     2
#define CFG_SERVER_TYPE_PRIVATE 3
#define CFG_SERVER_TYPE_LOCAL   4

/* Режимы auto_sockets: OFF — ручные [server]; SYSTEM — авто-сокеты из системных
 * интерфейсов; ANDROID — авто-сокеты из [server] секций конфига (генерирует Kotlin). */
#define CFG_AUTO_SOCKETS_OFF      0
#define CFG_AUTO_SOCKETS_SYSTEM   1
#define CFG_AUTO_SOCKETS_ANDROID  2

/* Маска протоколов автоматически создаваемых сокетов; по умолчанию TCP+UDP. */
#define CFG_AUTO_SOCKET_UDP       1
#define CFG_AUTO_SOCKET_TCP       2
#define CFG_AUTO_SOCKET_BOTH      (CFG_AUTO_SOCKET_UDP | CFG_AUTO_SOCKET_TCP)

#define CFG_IPV6_MODE_NONE      0 // normal explicit address
#define CFG_IPV6_MODE_TEMPORARY 1 // temporary IPv6 from interface
#define CFG_IPV6_MODE_PERMANENT 2 // permanent IPv6 from interface

struct CFG_SERVER {
    struct CFG_SERVER* next; // собственная цепочка конфигурации; список в обратном порядке секций
    char name[MAX_CONN_NAME_LEN]; // имя [server:name], до 63 байт
    struct sockaddr_storage ip; // ip:port
    uint32_t netif_index;// if_nameindex, 0 - no interface specified
    int so_mark;
    int fib;       // FreeBSD FIB (routing table), -1 = don't set
    uint8_t type;  // CFG_SERVER_TYPE_*; 0 если явно не задан
    uint8_t transport;  // 0=udp (default), 1=tcp
    uint8_t reality_enabled;  // 1 = TCP-порт с REALITY-камуфляжем (только transport=1)
    uint8_t socks_enabled;  // 1 = UDP-сокет туннелировать через SOCKS5 (только transport=0)
    uint8_t ipv6_mode; // CFG_IPV6_MODE_*
    int mtu; // 0 — не задан, при явной опции 0..65535; ресурс проверяет применимость
    uint8_t only_local;  // 1 = only local connections, no forwarding
};

struct CFG_CLIENT_LINK {
    struct CFG_CLIENT_LINK *next;                // Next link in linked list
    struct CFG_SERVER* local_srv; // заимствован у того же utun_config; NULL — remote-only
    char server_name[MAX_CONN_NAME_LEN];         // копия имени local_srv; пуста для remote-only
    struct sockaddr_storage remote_addr; // ip:port
};

struct CFG_CLIENT {
    char name[MAX_CONN_NAME_LEN];
    char peer_public_key_hex[MAX_KEY_LEN];
    int keepalive; // необязательная клиентская настройка; 0 по умолчанию, неотрицательный int
    struct CFG_CLIENT_LINK *links;           // Linked list of links
    struct CFG_CLIENT *next;              // Next client in linked list
    uint8_t reality_enabled;                 // 1 = TCP-линк с REALITY-камуфляжем
    struct reality_client_config reality;    // short_id, server_static_pubkey, version, server_name, fingerprint
};

struct CFG_NETWORK {
    struct CFG_NETWORK* next;
    char name[64];
    char pubkey_hex[SC_PUBKEY_SIZE * 2 + 1];        // 64 hex chars
    char signing_key_hex[SC_PRIVKEY_SIZE * 2 + 1];  // 64 hex chars
};

struct CFG_ROUTE_ENTRY {
    struct CFG_ROUTE_ENTRY* next;
    struct IP ip; // исходный адрес подсети (host-биты парсер не нормализует)
    uint8_t netmask; // префикс 0..32/0..128; без / по умолчанию 32/128 по семейству
};

struct CFG_FIREWALL_RULE {
    uint32_t ip;      // IPv4 назначения в host order.
    uint16_t port;    // TCP/UDP-порт назначения в host order; 0 разрешает весь IP.
};

struct CFG_CONTROL_ALLOW {
    uint32_t network;  // IPv4 network in host byte order
    uint32_t netmask;  // netmask in host byte order (e.g. 0xffffff00 for /24)
};

struct CFG_ALLOWED_KEY {
    uint8_t key_bin[SC_PUBKEY_SIZE];  // public key in binary (32 bytes, X25519)
    struct CFG_ALLOWED_KEY *next;
};

struct CFG_NTP_SERVER {
    char name[256]; // NUL-terminated hostname/IP; парсер не разрешает его, список идёт в обратном порядке
    struct CFG_NTP_SERVER *next;
};

/* STUN-серверы обходятся в порядке конфигурации; DNS выполняет модуль stun_nat. */
struct CFG_STUN_SERVER {
    struct CFG_STUN_SERVER* next;
    char host[256]; // IPv4/имя A-сервера; DNS выполняет stun_nat
    uint16_t port; // 1..65535, по умолчанию 3478
};

#define MAX_TCP_PROXY_CLIENT_MAPPINGS 32
struct tcp_proxy_client_mapping_config {
    uint16_t local_port;
    char     remote_ip[64];
    uint16_t remote_port;
};

struct global_config {
    int crypto_tx_threads;    // Шифровальщики: 0 — I/O и криптография в uasync, 1..64 — отдельный TX и пул.
    int crypto_rx_threads;    // Дешифровальщики: независимо от TX, по умолчанию 4.
    char name[MAX_CONN_NAME_LEN];     // Instance name
    char my_private_key_hex[MAX_KEY_LEN]; /* Seed Ed25519 устройства, 32 байта HEX. my_public_key_hex — производный X25519. */
    char my_public_key_hex[MAX_KEY_LEN];
    uint8_t  client_type;              // 0=server, 1=desktop, 2=mobile
    int  tun_enabled;          // 1 = запрошен TUN, 0 = выключен (значение по умолчанию)
    int  gui_utun_enabled;     // [gui] utun_enabled=yes: явно запустить UTUN в desktop/Android; по умолчанию 0
    int  gui_bluetooth_flags;  // [gui] bluetooth_flags: 1=BLE, 2=объявление узла, 4=входящие; по умолчанию 0
    char tun_ifname[16];       // TUN interface name (e.g., "tun12")
    struct IP tun_ip;
    int mtu;
    struct sockaddr_storage control_sock;
    char control_ip[MAX_ADDR_LEN];  // Control server IP address
    // Debug and logging configuration
    char log_file[256];        // Path to log file (empty = stdout)
    char db_path[256];         // Каталог chats.db; пусто — ядро использует SQLite :memory:
    int  db_sync_enabled;      // 1 = enable distributed DB sync (default: 0)
    int  chatserver_enabled;   // 1 = chat server mode (set by [chatserver] section presence)
    uint32_t db_sync_ttl;      // TTL for unsent records in seconds (default: 86400)
    // UDP log target
    char     log_udp_ip[64];
    int      log_udp_port;
    char debug_level[16];      // debug level: error, warn, info, debug, trace
    int enable_timestamp;      // enable timestamps in logs
    int enable_function_names; // enable function names in logs
    int enable_file_lines;     // enable file:line in logs
    int enable_colors;         // enable ANSI colors in logs
    
    // Per-category debug levels (loaded from [debug] section)
    struct {
        char category[DEBUG_CATEGORY_COUNT][16];  // category name
        char level[DEBUG_CATEGORY_COUNT][16];    // level string
        int count;
    } debug_levels;
    
    int tun_test_mode;         // test mode: 1 = don't open real TUN, queues only
    int keepalive_timeout;     // keepalive timeout in ms (default: 2000)
    int keepalive_interval;    // keepalive interval in ms (default: 200)
    int keepalive_adaptive;    // 1 = adaptive period (default), 0 = fixed interval
    int bbr_max_cwnd;          // лимит DATA-окна одного линка, 8192..INT_MAX байтов; по умолчанию 1048576

    // Firewall configuration
    struct CFG_FIREWALL_RULE *firewall_rules;
    int firewall_rule_count;
    int firewall_bypass_all;

    // Control server configuration ([control] section)
    struct CFG_CONTROL_ALLOW *control_allows;
    int control_allow_count;

    // Allowed keys configuration ([allowed_keys] section)
    struct CFG_ALLOWED_KEY *allowed_keys;
    int allowed_keys_count;
    int allowed_keys_allow_all;

    // Опции [nat] включают NAT; forward=proto:Ed25519_64hex:IPv4:internal_port:external_port.
    // Ошибки parse_nat отклоняют весь конфиг; диапазон портов дополнительно проверяет eim_nat_init_ctx.
    int nat_enabled;
    char nat_tun_ifname[16];
    struct IP nat_tun_ip;
    uint8_t nat_via_ed[32];       // Канонический Ed25519 провайдера.
    uint16_t nat_port_start;
    uint16_t nat_port_end;
    #define MAX_NAT_FORWARDS 64
    struct {
        uint8_t ed[32];             // Полный адрес получателя.
        uint8_t  proto;
        uint32_t internal_ip_host;
        uint16_t internal_port_net;
        uint16_t external_port;
    } nat_forwards[MAX_NAT_FORWARDS];
    int nat_forward_count;

    // TCP proxy client configuration ([tcp_proxy_client] section)
    int      tcp_proxy_client_enabled;
    char     tcp_proxy_client_tun_name[16];
    char     tcp_proxy_client_tun_ip[64];
    int      tcp_proxy_client_mtu;
    uint8_t tcp_proxy_client_via_ed[32]; // Адрес exit в настройках клиента.
    struct   tcp_proxy_client_mapping_config tcp_proxy_client_mappings[MAX_TCP_PROXY_CLIENT_MAPPINGS];
    int      tcp_proxy_client_mapping_count;
    int      tcp_proxy_client_socks_enabled;
    char     tcp_proxy_client_socks_addr[64];       // e.g. "127.0.0.1:1080"
    int      tcp_proxy_client_http_proxy_enabled;
    char     tcp_proxy_client_http_proxy_addr[64];  // e.g. "127.0.0.1:8080"

    // TCP proxy server (exit node) configuration ([tcp_proxy_server] section)
    int      tcp_proxy_server_enabled;
    int      tcp_proxy_max_flows;          // Общий предел для активных TCP, UDP и ожидающих ICMP.
    int      tcp_proxy_max_flows_per_node; // Предел по конечному node_id во всех разрешённых группах.
    int hide_my_sockets;             // Принудительный PRIVATE для всех транспортных сокетов; адреса не публикуются.
    int auto_sockets;                // CFG_AUTO_SOCKETS_*: OFF/SYSTEM/ANDROID (см. константы ниже)
    char connection_order[CONNECTION_ORDER_MAX]; // Подписанная политика подключений к нашему узлу.
    int auto_socket_protocols;       // маска CFG_AUTO_SOCKET_UDP/TCP; Android выбирает протоколы при генерации [server]
    int auto_socket_skip_no_default_route;  // 1 = в авто-режиме не создавать сокеты на интерфейсах без default-маршрута
    int tcp_recv_buf;                // SO_RCVBUF для TCP сокетов (0=default ОС)
    int      tcp_proxy_server_fib;          // FreeBSD SO_SETFIB, 0=не менять
    uint32_t tcp_proxy_server_so_mark;      // Linux SO_MARK, 0=не менять
    char     tcp_proxy_server_netif[16];    // Имя устройства Linux для SO_BINDTODEVICE, пустое = маршрут ОС.
    uint8_t (*tcp_proxy_groups)[32];       // Адреса групп exit по возрастанию. Нулевой ключ означает UTUN; пустой список — только UTUN.
    size_t    tcp_proxy_group_count;       // Число элементов tcp_proxy_groups.
    char     tcp_proxy_server_bind_ip[64];  // IP для bind() перед connect()
    int      tcp_proxy_server_connect_timeout_ms;  // таймаут connect(), мс (0 = без таймаута)

    // NTP configuration ([ntp] section)
    int ntp_enabled;
    struct CFG_NTP_SERVER *ntp_servers;   // linked list, unlimited
    int ntp_server_count;
    int ntp_resync_interval;              // seconds, default 3600

    int stun_enabled;                    // [stun] enabled=yes; по умолчанию выключено
    struct CFG_STUN_SERVER* stun_servers;
    int stun_server_count;
    
    // Chatserver configuration ([chatserver] section)
    uint8_t  chatserver_add_key[32];          // секрет разрешения серверного JOIN, не ключ канала
    int      chatserver_has_add_key;          // add_key разобран из INI; без него серверный JOIN запрещён
    char     chatserver_allowed_groups[1024]; // "all" или публичные Ed25519 групп через запятую
    uint64_t chatserver_storage_total_size;   // bytes, общий лимит хранилища (0 = безлимит)
    uint64_t chatserver_storage_unit_size;    // bytes, лимит автозагрузки одного файла (0 = отключено); отправку не ограничивает
    char     headless_control_bind[64];       // ip:port для headless chat CLI (пусто = отключено)
    char     call_audio_bind[64];             // ip:port для headless аудио-сокета звонков (пусто = отключено)
    char     radio_audio_bind[64];            // ip:port для headless аудио-сокета рации (пусто = отключено)
    struct   chat_setting_state chat_settings; // chat-настройки из [chatserver] (per-instance)

    // Reality-камуфляж ([reality] section)
    struct reality_config reality;

    // SOCKS5-прокси для исходящих ETCP-линков ([socks] section)
    int      socks_enabled;          // 1 = TCP/STCP-линки подключаются через SOCKS5
    char     socks_host[64];         // host прокси (ip или домен)
    uint16_t socks_port;             // порт прокси
    char     socks_username[64];     // пусто = no-auth
    char     socks_password[64];
};

struct utun_config {
    struct global_config global;
    struct CFG_SERVER* servers;
    struct CFG_CLIENT* clients;
    struct CFG_ROUTE_ENTRY* route_subnets;
    struct CFG_ROUTE_ENTRY* my_subnets;
    struct CFG_NETWORK* networks;
};

/* Непустой путь; результат освобождать free_config. NULL при ошибке открытия/чтения/закрытия,
 * памяти, формата или обработчика известной опции. Значения по умолчанию и полноту настройки
 * для конкретного сервиса дополнительно проверяет его start; ненулевой результат не гарантирует запуск.
 * Файл не изменяется, ключи не генерируются: для этого отдельно вызвать config_ensure_keys.
 * До вызова требуется socket_platform_init (для getaddrinfo на Windows).
 * Причины отказа доступны через config_error_text() в текущем потоке, независимо от фильтров журнала. */
struct utun_config* parse_config(const char *filename);
/* buf — len байтов без завершающего NUL (NULL допустим при len=0), буфер не меняется.
 * filename — источник для диагностики, NULL означает <buffer>. Возврат как у parse_config. */
struct utun_config* parse_config_from_buf(const char *buf, size_t len, const char *filename);
// Освобождает модель и все собственные списки/массивы; заимствованные ссылки становятся недействительны; NULL допустим.
void free_config(struct utun_config *config);
// Пишет диагностический снимок в config/INFO; private/signing keys не выводятся; NULL — no-op.
void print_config(const struct utun_config *config);
// Устаревшее добавление [global] с двумя 32-byte HEX ключами в конец файла; 0/-1 с логом.
// Не атомарно, частичная запись возможна. Для согласованной записи ключей использовать config_updater.
int update_config_keys(const char *filename, const char *priv_key, const char *pub_key);

/* Узел (по pubkey, 32 байта) присутствует в списке [client] конфига? 1=да, 0=нет. */
int config_peer_in_clients(const struct utun_config* config, const uint8_t* pubkey);

#ifdef __cplusplus
}
#endif

#endif
