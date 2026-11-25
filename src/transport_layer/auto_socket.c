/*
 * auto_socket.c — автоматическое управление сокетами (UDP+TCP) при изменении сетевых интерфейсов
 *
 * === Зачем ===
 * При auto_sockets=yes модуль создаёт выбранные auto_socket_protocols сокеты на всех не-loopback
 * интерфейсах с IP-адресами. Не нужно вручную описывать серверы в конфиге.
 *
 * === Что создаёт ===
 * Для каждого интерфейса с v4/v6 адресами:
 *   1. UDP-сокет (etcp_socket_add) — bind на 0.0.0.0/[::] + SO_BINDTODEVICE, случайный порт
 *   2. TCP-сокет (tcp_socket_add + stcp_server) — слушает входящие STCP-подключения
 *   3. Для нового сокета NCD добавляет все разрешённые пары адрес/сокет к своим
 *      активным транспортам (ncd_registry), кроме FIN_WAIT. Правила общие с NCD open/update.
 *
 * === Тип сокета ===
 * PUBLIC — если на интерфейсе есть глобальный (не RFC1918) адрес; иначе NAT.
 * При смене типа (например VPN включился/выключился) сокет пересоздаётся.
 *
 * === Persistence портов ===
 * Порт сохраняется в SQLite-таблицу auto_socket_ports и переиспользуется
 * при перезапуске utun. Если сохранённый порт занят — генерируется новый случайный.
 *
 * === Стратегия reconcile ===
 * При любом изменении адресов/линков сканируем фактические IP интерфейса,
 * сравниваем с состоянием сокетов и устраняем расхождения:
 *   - появился адрес нужного family → создать сокет + линки
 *   - исчезли все адреса family → удалить сокет + линки
 *   - изменился тип (NAT↔PUBLIC) → пересоздать сокет с новым типом + линки
 *   - изменился сам адрес → обновить interface_addr, оповестить подписчиков
 *   - интерфейс стал DOWN → удалить все сокеты
 *
 * === Линки ===
 * Каждый созданный сокет получает линки к активным NCD-соединениям через
 * ncd_add_socket_links(): все разрешённые адреса того же family и протокола, с проверкой политики и REALITY.
 * На линке сохраняется local_bound_addr (interface_addr в момент создания) для
 * последующей проверки принадлежности.
 *
 * === Platform monitoring ===
 * Встроенные обработчики: netlink (Linux), route socket (BSD), IP Helper (Windows).
 * При отсутствии поддержки платформы мониторинг неактивен — можно вызывать
 * auto_socket_add_interface / auto_socket_on_network_change извне (JNI и т.д.).
 */

#include "auto_socket.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "node_conn_direct.h"
#include "socket_monitor.h"
#include "stcp_link.h"
#include "utun_instance.h"
#include "config_parser.h"
#include "../chat/chat_event.h"
#include "../chat/chat_sync.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/conn_mgr.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../lib/db_sql.h"
#include "../lib/socket_compat.h"
#include "../lib/platform_compat.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../../lib/sqlite3.h"

#if !defined(_WIN32)
#include <net/if.h>
#include <ifaddrs.h>
#endif

#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <net/route.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#elif defined(_WIN32)
#include <iphlpapi.h>
#include <netioapi.h>
#include <ws2ipdef.h>
#endif

#define DEBUG_CATEGORY_AS DEBUG_CATEGORY_SOCKET

#define AS_PROTO_UDP  0
#define AS_PROTO_TCP  1
#ifdef _WIN32
#define AS_ADDR_IN_USE WSAEADDRINUSE
#define AS_LOCK(as) EnterCriticalSection(&(as)->notify_lock)
#define AS_UNLOCK(as) LeaveCriticalSection(&(as)->notify_lock)
#else
#define AS_ADDR_IN_USE EADDRINUSE
#define AS_LOCK(as) pthread_mutex_lock(&(as)->notify_lock)
#define AS_UNLOCK(as) pthread_mutex_unlock(&(as)->notify_lock)
#endif

/* Классифицирует IPv6-адрес для определения типа сокета:
 *   AS_V6_LL  = link-local   (fe80::) — создаём NAT-сокет
 *   AS_V6_LOC = ULA          (fc00::/fd00::) — локальный, NAT-сокет
 *   AS_V6_DIR = глобальный   — создаём PUBLIC-сокет
 *   AS_V6_OTH = loopback, multicast, :: — пропускаем */
#define AS_V6_LL  0
#define AS_V6_LOC 1
#define AS_V6_DIR 2
#define AS_V6_OTH 3

static int v6_classify(const uint8_t addr[16]) {
    if (addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80) return AS_V6_LL;
    if (addr[0] == 0xfc || addr[0] == 0xfd) return AS_V6_LOC;
    if (addr[0] == 0xff) return AS_V6_OTH;
    { static const uint8_t z[16]; if (!memcmp(addr, z, 16)) return AS_V6_OTH; }
    { static const uint8_t lb[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}; if (!memcmp(addr, lb, 16)) return AS_V6_OTH; }
    return (addr[0] & 0xe0) == 0x20 ? AS_V6_DIR : AS_V6_OTH;
}

/* Один отслеживаемый интерфейс: хранит указатели на созданные сокеты (UDP — ETCP_SOCKET,
 * TCP — stcp_server) и их актуальный тип (NAT/PUBLIC). По типу определяется когда
 * нужно пересоздать сокет при изменении адресов. */
struct auto_sock_iface {
    struct auto_sock_iface* next;
    uint32_t netif_index;           // индекс интерфейса (if_nametoindex)
    char ifname[IF_NAMESIZE];       // нужен и после удаления интерфейса из ОС
    struct ETCP_SOCKET* v4_udp;     // UDP-сокет для IPv4 (NULL если не создан)
    struct ETCP_SOCKET* v6_udp;     // UDP-сокет для IPv6
    struct stcp_server* v4_tcp;     // TCP STCP-сервер для IPv4
    struct stcp_server* v6_tcp;     // TCP STCP-сервер для IPv6
    struct ETCP_SOCKET* v4_tcp_sock; // модель сокета принадлежит тому же интерфейсу
    struct ETCP_SOCKET* v6_tcp_sock;
    struct sockaddr_storage v4_tcp_addr; // объявленный адрес в момент создания wildcard TCP listener
    struct sockaddr_storage v6_tcp_addr;
    uint8_t v4_type;                // CFG_SERVER_TYPE_PUBLIC или CFG_SERVER_TYPE_NAT
    uint8_t v6_type;                // CFG_SERVER_TYPE_PUBLIC или CFG_SERVER_TYPE_NAT
};

struct auto_saved_port {
    char ifname[IF_NAMESIZE];
    int family, protocol;
    uint16_t port;
};
struct auto_port_load;

/* Внутреннее состояние модуля auto_socket: хранит связный список отслеживаемых
 * интерфейсов и платформенные handle'ы для мониторинга изменений сети.
 * Создаётся в auto_socket_init, уничтожается в auto_socket_destroy.
 * Хранится в inst->auto_socket_state. */
struct AUTO_SOCKET {
    struct UTUN_INSTANCE* instance;         // родительский utun instance
    struct auto_sock_iface* ifaces;         // связный список интерфейсов
    struct auto_saved_port* ports;         // копия сохранённых портов, только uasync
    size_t port_count, port_capacity;       // заполненные элементы и ёмкость ports
    struct auto_port_load* ports_load;      // ожидающий query; уникальная метка защищает от повторного адреса as
    uint8_t ports_ready;                    // до чтения кэша системные сокеты ещё не создаются
    uint8_t mode;                           // CFG_AUTO_SOCKETS_SYSTEM или CFG_AUTO_SOCKETS_ANDROID
    uint8_t protocols;                      // маска CFG_AUTO_SOCKET_UDP/TCP для системных интерфейсов
    uint8_t v4_usable;                      // есть ли сейчас рабочий v4-сокет с реальным IP
    uint8_t v6_usable;                      // есть ли сейчас рабочий v6-сокет с реальным IP
    uint8_t v4_addr_changed;                // v4-адрес или тип изменился с прошлой проверки
    uint8_t v6_addr_changed;                // v6-адрес или тип изменился с прошлой проверки
    struct posted_task* pending;             // отменяемое полное сканирование
    int stopping;                            // запрещены новые задачи и уведомления при teardown
    int retain_context;                      // ОС не смогла отменить native callback: контекст остаётся отключённым
#ifdef _WIN32
    CRITICAL_SECTION notify_lock;
#else
    pthread_mutex_t notify_lock;
#endif
#ifdef __linux__
    socket_t nl_sock;                       // netlink socket fd для RTMGRP_LINK/IPV4/IPV6
    void* uasync_handle;                    // handle в uasync для netlink fd
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    socket_t route_sock;                    // route socket fd (PF_ROUTE)
    void* uasync_handle;                    // handle в uasync для route fd
#elif defined(_WIN32)
    HANDLE addr_notify_handle;              // NotifyUnicastIpAddressChange handle
    HANDLE route_notify_handle;             // NotifyRouteChange2 handle
#endif
};

/* ─── forward declarations ─── */
static struct ETCP_SOCKET* create_iface_udp_socket(struct AUTO_SOCKET* as, uint32_t ifindex, const char* ifname, int family, uint8_t type);
static struct stcp_server* create_iface_tcp_socket(struct AUTO_SOCKET* as, uint32_t ifindex, const char* ifname,
                                                  int family, uint8_t type, struct ETCP_SOCKET** out_sock);
static void remove_iface_sockets(struct AUTO_SOCKET* as, uint32_t ifindex);
static int  scan_and_classify_iface(uint32_t ifindex, const char* ifname, int* out_has_v4, int* out_has_v6, uint8_t* out_v4_type, uint8_t* out_v6_type);
static int reconcile_iface(struct AUTO_SOCKET* as, uint32_t ifindex, const char* ifname);
static int auto_socket_scan_all(struct AUTO_SOCKET* as);
static int auto_socket_reconcile_config(struct AUTO_SOCKET* as);
static void auto_socket_scan_all_trampoline(void* arg);

/* Производители завершаются до destroy; сама очередь отменяется владельцем в uasync. */
static void auto_socket_post_scan(struct AUTO_SOCKET* as) {
    AS_LOCK(as);
    if (!as->stopping && !as->pending) {
        struct posted_task* task = u_calloc(1, sizeof(*task));
        if (task) {
            task->callback = auto_socket_scan_all_trampoline; task->arg = as;
            as->pending = task; uasync_post_reserved(as->instance->ua, task);
        } else DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] cannot post network scan");
    }
    AS_UNLOCK(as);
}

/* ─── DB helpers (port persistence) ───
 * Сохраняют порт сокета в SQLite чтобы переиспользовать его при перезапуске utun.
 * Это сохраняет NAT-привязку (endpoint mapping) на роутере и позволяет пирам
 * продолжать достукиваться на тот же порт. */

static int port_cache_reserve(struct AUTO_SOCKET* as, size_t needed) {
    if (needed <= as->port_capacity) return 0;
    size_t capacity = as->port_capacity ? as->port_capacity * 2 : 16;
    if (capacity < needed || capacity > UINT32_MAX / sizeof(*as->ports)) capacity = needed;
    if (capacity > UINT32_MAX / sizeof(*as->ports)) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] port cache too large rows=%zu", needed); return -1;
    }
    struct auto_saved_port* ports = u_realloc(as->ports, capacity * sizeof(*ports));
    if (!ports) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] port cache allocation failed rows=%zu", capacity); return -1; }
    as->ports = ports; as->port_capacity = capacity; return 0;
}
static size_t port_cache_find(struct AUTO_SOCKET* as, const char* ifname, int family, int protocol) {
    size_t i = 0;
    while (i < as->port_count && (as->ports[i].family != family || as->ports[i].protocol != protocol ||
        strcmp(as->ports[i].ifname, ifname))) i++;
    return i;
}
static void port_cache_save(struct AUTO_SOCKET* as, const char* ifname, int family, int protocol, uint16_t port) {
    size_t i = port_cache_find(as, ifname, family, protocol);
    if (i == as->port_count) {
        if (port_cache_reserve(as, i + 1)) return;
        as->port_count++;
    }
    snprintf(as->ports[i].ifname, sizeof(as->ports[i].ifname), "%s", ifname);
    as->ports[i].family = family; as->ports[i].protocol = protocol; as->ports[i].port = port;
}
/* Проверка до преобразования типов; нулевой порт не заменяет ошибочную строку. */
static int port_cache_decode(const struct db_value* values, struct auto_saved_port* port) {
    const struct db_value* name = values;
    int embedded_nul = name->type == SQLITE_TEXT && name->data && memchr(name->data, 0, name->bytes) != NULL;
    if (name->type != SQLITE_TEXT || !name->data || !name->bytes || name->bytes >= IF_NAMESIZE ||
        embedded_nul || values[1].type != SQLITE_INTEGER ||
        (values[1].i64 != AF_INET && values[1].i64 != AF_INET6) || values[2].type != SQLITE_INTEGER ||
        (values[2].i64 != AS_PROTO_UDP && values[2].i64 != AS_PROTO_TCP) || values[3].type != SQLITE_INTEGER ||
        values[3].i64 < 1024 || values[3].i64 > 65535) {
        DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] invalid saved port types=%d/%d/%d/%d name_bytes=%zu name_nul=%d"
            " family=%lld protocol=%lld port=%lld", name->type, values[1].type, values[2].type, values[3].type,
            name->bytes, embedded_nul, (long long)values[1].i64, (long long)values[2].i64, (long long)values[3].i64); return 0;
    }
    memcpy(port->ifname, name->data, name->bytes); port->ifname[name->bytes] = 0;
    port->family = (int)values[1].i64; port->protocol = (int)values[2].i64; port->port = (uint16_t)values[3].i64; return 1;
}
struct auto_port_load { struct UTUN_INSTANCE* instance; struct AUTO_SOCKET* owner; };
static const char port_snapshot_sql[] = "SELECT if_name,family,protocol,port FROM auto_socket_ports ORDER BY if_name,family,protocol";
static void ports_loaded(void* arg, const struct db_sql_result* result) {
    struct auto_port_load* request = arg; struct AUTO_SOCKET* as = request->instance->auto_socket_state;
    int current = as && as == request->owner && as->ports_load == request;
    if (current) as->ports_load = NULL;
    if (!current || as->stopping || request->instance->destroying) {
        DEBUG_DEBUG(DEBUG_CATEGORY_AS, "[as] port snapshot ignored after stop/restart"); u_free(request); return;
    }
    as->port_count = 0;
    if (result->outcome.status || result->columns != 4) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] port snapshot failed status=%d rc=%d columns=%u; using new ports: %s",
            result->outcome.status, result->outcome.sqlite_code, result->columns, result->outcome.message);
    } else if (!port_cache_reserve(as, result->rows)) {
        for (size_t i = 0; i < result->rows; i++) {
            struct auto_saved_port port;
            if (port_cache_decode(result->values + i * 4, &port)) as->ports[as->port_count++] = port;
        }
    }
    as->ports_ready = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_AS, "[as] saved ports loaded rows=%zu cached=%zu", result->rows, as->port_count);
    u_free(request);
    if (as->mode == CFG_AUTO_SOCKETS_SYSTEM) auto_socket_post_scan(as);
}
/* Файловый снимок — один query; после него создание/пересоздание endpoint читает только RAM. */
static int restore_ports(struct AUTO_SOCKET* as) {
    struct UTUN_INSTANCE* inst = as->instance;
    if (inst->db_worker) {
        struct auto_port_load* request = u_malloc(sizeof(*request));
        if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] port snapshot request allocation failed"); return -1; }
        *request = (struct auto_port_load){inst, as}; as->ports_load = request; as->ports_ready = 0;
        db_query(inst->db_worker, port_snapshot_sql, DB_NO_ARGS, ports_loaded, request); return 0;
    }
    as->ports_ready = 1; sqlite3* db = inst->topo_sqlite_db;
    const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    if (!db) return 0;
    if (!file || *file) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] no asynchronous port reader; using new ports"); return 0; }
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, port_snapshot_sql, -1, &st, NULL);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct db_value values[4] = {{0}};
        for (int col = 0; col < 4; col++) values[col].type = sqlite3_column_type(st, col);
        if (values[0].type == SQLITE_TEXT) {
            values[0].data = sqlite3_column_text(st, 0);
            if (!values[0].data) { rc = SQLITE_NOMEM; break; }
            values[0].bytes = (size_t)sqlite3_column_bytes(st, 0);
        }
        for (int col = 1; col < 4; col++) if (values[col].type == SQLITE_INTEGER) values[col].i64 = sqlite3_column_int64(st, col);
        struct auto_saved_port port;
        if (port_cache_decode(values, &port)) {
            if (port_cache_reserve(as, as->port_count + 1)) { rc = SQLITE_NOMEM; break; }
            as->ports[as->port_count++] = port;
        }
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        as->port_count = 0;
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] RAM port snapshot failed rc=%d finalize=%d; using new ports", rc, final);
    }
    return 0;
}
/* Возвращает сохранённый порт без SQL, 0 — запись отсутствует. */
static uint16_t load_saved_port(struct AUTO_SOCKET* as, const char* ifname, int family, int protocol) {
    size_t i = port_cache_find(as, ifname, family, protocol); return i < as->port_count ? as->ports[i].port : 0;
}

struct auto_port_write { char ifname[IF_NAMESIZE]; int family, protocol, removing; uint16_t port; };
static void port_write_done(void* arg, const struct db_sql_result* result) {
    struct auto_port_write* request = arg;
    if (result->outcome.status) DEBUG_ERROR(DEBUG_CATEGORY_AS,
        "[as] port %s failed iface=%s family=%d protocol=%d port=%u status=%d rc=%d: %s",
        request->removing ? "delete" : "save", request->ifname, request->family, request->protocol, request->port,
        result->outcome.status, result->outcome.sqlite_code, result->outcome.message);
    else DEBUG_DEBUG(DEBUG_CATEGORY_AS, "[as] port %s committed iface=%s family=%d protocol=%d port=%u",
        request->removing ? "delete" : "save", request->ifname, request->family, request->protocol, request->port);
    u_free(request);
}
static void queue_port_write(struct AUTO_SOCKET* as, const char* ifname, int family, int protocol, uint16_t port, int removing) {
    struct auto_port_write* request = u_malloc(sizeof(*request));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] port persistence allocation failed iface=%s", ifname); return; }
    snprintf(request->ifname, sizeof(request->ifname), "%s", ifname);
    request->family = family; request->protocol = protocol; request->port = port; request->removing = removing;
    const char* sql = removing ? "DELETE FROM auto_socket_ports WHERE if_name=? AND family=? AND protocol=?"
        : "INSERT OR REPLACE INTO auto_socket_ports(if_name,family,protocol,port,updated_at) VALUES(?,?,?,?,unixepoch())";
    struct db_args args = removing ? DB_ARGS(DB_TEXT(ifname), DB_I64(family), DB_I64(protocol))
        : DB_ARGS(DB_TEXT(ifname), DB_I64(family), DB_I64(protocol), DB_I64(port));
    db_exec(as->instance->db_worker, sql, args, port_write_done, request);
}
/* До общего RW-перехода допускается прежний писатель; закрытый RW owner сюда не попадает. */
static sqlite3* port_legacy_writer(struct AUTO_SOCKET* as) {
    sqlite3* db = as->instance->topo_sqlite_db;
    if (db && !as->instance->db_worker) {
        const char* file = sqlite3_db_filename(db, "main");
        if (!file || *file) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] no asynchronous port writer"); return NULL; }
    }
    return db;
}

/* Сохраняет порт в БД для переиспользования при следующем запуске. */
static void save_port_to_db(struct AUTO_SOCKET* as, const char* ifname, int family, int protocol, uint16_t port) {
    port_cache_save(as, ifname, family, protocol, port);
    if (db_async_can_write(as->instance->db_worker)) { queue_port_write(as, ifname, family, protocol, port, 0); return; }
    sqlite3* db = port_legacy_writer(as);
    if (!db) return;
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO auto_socket_ports(if_name, family, protocol, port, updated_at) VALUES(?,?,?,?,unixepoch())",
            -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, ifname, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, family);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 3, protocol);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, port);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int final = sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_AS,
        "[as] save port failed iface=%s rc=%d finalize=%d: %s", ifname, rc, final, sqlite3_errmsg(db));
}

/* Удаляет запись порта из БД после закрытия сокета. */
static void delete_port_from_db(struct AUTO_SOCKET* as, const char* ifname, int family, int protocol) {
    if (as->stopping) return;
    size_t i = port_cache_find(as, ifname, family, protocol);
    if (i < as->port_count) as->ports[i] = as->ports[--as->port_count];
    if (db_async_can_write(as->instance->db_worker)) { queue_port_write(as, ifname, family, protocol, 0, 1); return; }
    sqlite3* db = port_legacy_writer(as); if (!db) return;
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
            "DELETE FROM auto_socket_ports WHERE if_name=? AND family=? AND protocol=?",
            -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, ifname, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 2, family);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 3, protocol);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int final = sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_AS,
        "[as] delete port failed iface=%s rc=%d finalize=%d: %s", ifname, rc, final, sqlite3_errmsg(db));
}

/* Отправляет событие CHAT_EVT_LOCAL_SOCKETS в GUI/headless — оповещает что
 * список локальных сокетов изменился (добавился/удалился интерфейс). */
static int as_check_usable(struct AUTO_SOCKET* as, int family) {
    struct ETCP_SOCKET* s = as->instance->etcp_sockets;
    while (s) {
        if (!s->probe_only && s->local_addr.ss_family == family && s->interface_addr.ss_family == family) {
            if (family == AF_INET && ((struct sockaddr_in*)&s->interface_addr)->sin_addr.s_addr) return 1;
            if (family == AF_INET6 && !IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6*)&s->interface_addr)->sin6_addr)) return 1;
        }
        s = s->next;
    }
    return 0;
}

static void auto_socket_post_sockets_changed(struct AUTO_SOCKET* as) {
    if (as->stopping) return;
    int old_v4 = as->v4_usable, old_v6 = as->v6_usable;
    as->v4_usable = as_check_usable(as, AF_INET);
    as->v6_usable = as_check_usable(as, AF_INET6);
    if ((old_v4 || old_v6) && !as->v4_usable && !as->v6_usable)
        DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] no usable sockets; waiting for network changes");

    int need_reconnect = 0;
    if ((!old_v4 && as->v4_usable) || (!old_v6 && as->v6_usable))
        need_reconnect = 1;
    if (as->v4_addr_changed || as->v6_addr_changed)
        need_reconnect = 1;
    as->v4_addr_changed = 0; as->v6_addr_changed = 0;

    client_connections_network_changed(as->instance);
    struct TOPO_GROUPS* groups = as->instance->topo_groups;
    for (struct ll_entry* e = groups && groups->group_list ? groups->group_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (!group->stopping) conn_mgr_network_changed(group->conn_mgr);
    }
    chat_event_post(as->instance, CHAT_EVT_LOCAL_SOCKETS, NULL, 0);

    if (need_reconnect) {
        DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] connectivity change: v4 %d→%d v6 %d→%d — triggering reconnect",
                   old_v4, as->v4_usable, old_v6, as->v6_usable);
        chat_sync_retry_channels_on_socket_change(as->instance);
    }
}

/* ═══════════ Platform-agnostic core ═══════════ */

/* Генерирует случайный порт в диапазоне 1024..65535 для нового сокета. */
static uint16_t random_port(void) {
    uint32_t r;
    if (random_bytes((uint8_t*)&r, sizeof(r)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] cannot generate socket port"); return 0;
    }
    return (uint16_t)((r % (65536 - 1024)) + 1024);
}

/*
 * Создаёт UDP-сокет привязанный к конкретному интерфейсу и добавляет к нему
 * линки к активным NCD-соединениям.
 *
 * Алгоритм:
 *   1. Пытается переиспользовать сохранённый порт из БД (на случай перезапуска)
 *   2. Если занят — генерирует случайный (до 10 попыток)
 *   3. Вызывает etcp_socket_add — создаёт ETCP_SOCKET с bind на интерфейс
 *   4. Обновляет interface_addr через socket_monitor_update_if_addr
 *   5. Отбрасывает сокет если адрес нулевой (INADDR_ANY / ::) — интерфейс без реального IP
 *   6. Сохраняет порт в БД для будущих перезапусков
 *   7. Вызывает ncd_add_socket_links — добавляет разрешённые UDP-пары этого family
 *      к активным NCD-соединениям, кроме FIN_WAIT
 *
 * Возвращает созданный endpoint или NULL при ошибке. */
static struct ETCP_SOCKET* create_iface_udp_socket(struct AUTO_SOCKET* as, uint32_t ifindex, const char* ifname, int family, uint8_t type) {
    struct UTUN_INSTANCE* inst = as->instance;
    struct CFG_SERVER server;
    memset(&server, 0, sizeof(server));
    snprintf(server.name, sizeof(server.name), "as_%s_%s_udp", ifname, family == AF_INET6 ? "v6" : "v4");
    server.type = type;
    server.netif_index = ifindex;
    server.transport = 0;
    server.mtu = inst->config ? inst->config->global.mtu : 1280;

    if (family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&server.ip;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = INADDR_ANY;
    } else {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&server.ip;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_addr = in6addr_any;
    }

    uint16_t saved_port = load_saved_port(as, ifname, family, AS_PROTO_UDP);
    int max_att = saved_port ? 11 : 10;

    for (int attempt = 0; attempt < max_att; attempt++) {
        uint16_t port = (attempt == 0 && saved_port) ? saved_port : random_port();
        if (!port) return NULL;
        if (family == AF_INET)
            ((struct sockaddr_in*)&server.ip)->sin_port = htons(port);
        else
            ((struct sockaddr_in6*)&server.ip)->sin6_port = htons(port);

        struct ETCP_SOCKET* es = etcp_socket_add(inst, &server);
        if (!es) {
            int error = socket_get_error();
            DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] UDP socket create failed iface=%s ifidx=%u family=%s port=%u attempt=%d reused=%d err=%d",
                        ifname, ifindex, family == AF_INET6 ? "IPv6" : "IPv4", port, attempt + 1,
                        attempt == 0 && saved_port != 0, error);
            if (error != AS_ADDR_IN_USE) return NULL;
            continue;
        }

        socket_monitor_update_if_addr(es);

        {   const struct sockaddr_storage* ia = es->interface_addr.ss_family ? &es->interface_addr : &es->local_addr;
            if (ia->ss_family == AF_INET && ((struct sockaddr_in*)ia)->sin_addr.s_addr == INADDR_ANY) {
                DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] UDP socket %s has INADDR_ANY — removing", es->name);
                ncd_remove_socket_links(inst, es); etcp_socket_remove(es); return NULL;
            }
            if (ia->ss_family == AF_INET6 && IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6*)ia)->sin6_addr)) {
                DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] UDP socket %s has :: — removing", es->name);
                ncd_remove_socket_links(inst, es); etcp_socket_remove(es); return NULL;
            }
        }

        save_port_to_db(as, ifname, family, AS_PROTO_UDP, port);

        DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] UDP socket created: name=%s fd=%d if=%s(%u) type=%d addr=%s",
                   es->name, (int)es->fd, ifname, ifindex, type,
                   sockaddr_storage_to_str(&es->local_addr).str);

        int links = ncd_add_socket_links(inst, es);
        if (links > 0)
            DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] added %d links to UDP socket %s", links, es->name);
        else if (links < 0) DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] incomplete UDP link update iface=%s", ifname);
        return es;
    }

    DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] failed to create UDP socket after %d attempts if=%s fam=%d", max_att, ifname, family);
    return NULL;
}

/*
 * Создаёт TCP-сокет и STCP-сервер на интерфейсе, добавляет исходящие TCP-линки
 * к активным NCD-соединениям.
 *
 * Алгоритм аналогичен create_iface_udp_socket:
 *   1. Переиспользует сохранённый порт или генерирует случайный
 *   2. Создаёт ETCP_SOCKET через tcp_socket_add
 *   3. Отбрасывает если адрес нулевой
 *   4. Создаёт stcp_server (слушает входящие STCP-подключения)
 *   5. Регистрирует сервер через stcp_server_list_add
 *   6. Вызывает ncd_add_socket_links — добавляет исходящие TCP-линки
 *      с проверкой политики и REALITY к активным NCD-соединениям, кроме FIN_WAIT
 *
 * Возвращает stcp_server* при успехе, NULL при ошибке. */
static struct stcp_server* create_iface_tcp_socket(struct AUTO_SOCKET* as, uint32_t ifindex, const char* ifname,
                                                  int family, uint8_t type, struct ETCP_SOCKET** out_sock) {
    struct UTUN_INSTANCE* inst = as->instance;
    *out_sock = NULL;

    uint16_t saved_port = load_saved_port(as, ifname, family, AS_PROTO_TCP);
    int max_att = saved_port ? 11 : 10;

    struct stcp_link_config scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.inst = inst;
    scfg.listen_family = family;

    for (int attempt = 0; attempt < max_att; attempt++) {
        uint16_t port = (attempt == 0 && saved_port) ? saved_port : random_port();
        if (!port) return NULL;

        struct CFG_SERVER server;
        memset(&server, 0, sizeof(server));
        snprintf(server.name, sizeof(server.name), "as_%s_%s_tcp", ifname, family == AF_INET6 ? "v6" : "v4");
        server.type = type;
        server.netif_index = ifindex;
        server.transport = 1;
        if (family == AF_INET) {
            struct sockaddr_in* sin = (struct sockaddr_in*)&server.ip;
            sin->sin_family = AF_INET; sin->sin_addr.s_addr = INADDR_ANY; sin->sin_port = htons(port);
        } else {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&server.ip;
            sin6->sin6_family = AF_INET6; sin6->sin6_addr = in6addr_any; sin6->sin6_port = htons(port);
        }

        struct ETCP_SOCKET* ts = tcp_socket_add(inst, &server);
        if (!ts) { DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] TCP tcp_socket_add failed if=%s fam=%d port=%u", ifname, family, port); return NULL; }

        {   const struct sockaddr_storage* ia = (ts->interface_addr.ss_family) ? &ts->interface_addr : &ts->local_addr;
            if ((ia->ss_family == AF_INET && ((const struct sockaddr_in*)ia)->sin_addr.s_addr == INADDR_ANY) ||
                (ia->ss_family == AF_INET6 && IN6_IS_ADDR_UNSPECIFIED(&((const struct sockaddr_in6*)ia)->sin6_addr))) {
                DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] TCP socket %s has null addr — removing", server.name);
                tcp_socket_remove(ts); return NULL;
            }
        }

        struct stcp_server* tsrv = stcp_server_listen(&scfg, port, tcp_server_on_link, ts);
        if (!tsrv) {
            int error = socket_get_error();
            DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] TCP socket create failed iface=%s ifidx=%u family=%s port=%u attempt=%d reused=%d err=%d",
                        ifname, ifindex, family == AF_INET6 ? "IPv6" : "IPv4", port, attempt + 1,
                        attempt == 0 && saved_port != 0, error);
            tcp_socket_remove(ts);
            if (error != AS_ADDR_IN_USE) return NULL;
            continue;
        }

        stcp_server_list_add(inst, tsrv);
        *out_sock = ts;

        save_port_to_db(as, ifname, family, AS_PROTO_TCP, port);

        {   int tcp_links = ncd_add_socket_links(inst, ts);
            if (tcp_links > 0)
                DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] added %d TCP links on if=%s", tcp_links, ifname);
            else if (tcp_links < 0) DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] incomplete TCP link update iface=%s", ifname);
        }

        DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] TCP socket created: if=%s(%u) type=%d port=%u", ifname, ifindex, type, port);
        return tsrv;
    }

        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] failed to create TCP socket after %d attempts if=%s fam=%d", max_att, ifname, family);
    return NULL;
}

int auto_socket_add_interface(struct UTUN_INSTANCE* inst, uint32_t ifindex,
                               const char* ifname, int has_v4, int has_v6,
                               uint8_t v4_type, uint8_t v6_type) {
    if (!inst || !inst->auto_socket_state || !ifname || !ifname[0]) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] add_interface: invalid arguments"); return 0;
    }
    struct AUTO_SOCKET* as = (struct AUTO_SOCKET*)inst->auto_socket_state;
    if (!as->ports_ready) { DEBUG_TRACE(DEBUG_CATEGORY_AS, "[as] interface %s waits for saved ports", ifname); return 0; }
#if !defined(_WIN32)
    if (ifindex == 0) ifindex = if_nametoindex(ifname);
#endif
    if (ifindex == 0) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] ifindex=0 for %s", ifname); return 0; }

    struct auto_sock_iface* ifa = as->ifaces;
    while (ifa) { if (ifa->netif_index == ifindex) return 0; ifa = ifa->next; }

    ifa = u_calloc(1, sizeof(*ifa));
    if (!ifa) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] alloc failed"); return 0; }
    ifa->netif_index = ifindex;
    snprintf(ifa->ifname, sizeof(ifa->ifname), "%s", ifname);

    int added = 0;
    if (has_v4) {
        if (as->protocols & CFG_AUTO_SOCKET_UDP) {
            ifa->v4_udp = create_iface_udp_socket(as, ifindex, ifname, AF_INET, v4_type);
            if (ifa->v4_udp) { added++; ifa->v4_type = v4_type; }
        }
        if (as->protocols & CFG_AUTO_SOCKET_TCP) {
            struct stcp_server* tsrv = create_iface_tcp_socket(as, ifindex, ifname, AF_INET, v4_type, &ifa->v4_tcp_sock);
            if (tsrv) { added++; ifa->v4_tcp = tsrv; ifa->v4_tcp_addr = ifa->v4_tcp_sock->interface_addr; }
        }
    }
    if (has_v6) {
        if (as->protocols & CFG_AUTO_SOCKET_UDP) {
            ifa->v6_udp = create_iface_udp_socket(as, ifindex, ifname, AF_INET6, v6_type);
            if (ifa->v6_udp) { added++; ifa->v6_type = v6_type; }
        }
        if (as->protocols & CFG_AUTO_SOCKET_TCP) {
            struct stcp_server* tsrv = create_iface_tcp_socket(as, ifindex, ifname, AF_INET6, v6_type, &ifa->v6_tcp_sock);
            if (tsrv) { added++; ifa->v6_tcp = tsrv; ifa->v6_tcp_addr = ifa->v6_tcp_sock->interface_addr; }
        }
    }

    if (added > 0) {
        ifa->next = as->ifaces; as->ifaces = ifa;
        DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] added iface %s idx=%u socks=%d v4=%d v6=%d v4t=%d v6t=%d",
                   ifname, ifindex, added, has_v4, has_v6, v4_type, v6_type);
        auto_socket_post_sockets_changed(as);
    } else {
        u_free(ifa);
    }

    return added;
}

void auto_socket_remove_interface(struct UTUN_INSTANCE* inst, uint32_t ifindex) {
    if (!inst || !inst->auto_socket_state || ifindex == 0) return;
    struct AUTO_SOCKET* as = (struct AUTO_SOCKET*)inst->auto_socket_state;
    remove_iface_sockets(as, ifindex);
}

/*
 * Удаляет все сокеты и линки для указанного интерфейса.
 * Для каждого из 4 сокетов (v4 UDP/TCP, v6 UDP/TCP):
 *   1. Закрывает ETCP-линки через ncd_remove_socket_links
 *   2. Удаляет ETCP_SOCKET (etcp_socket_remove / tcp_socket_remove)
 *   3. Для TCP — дополнительно останавливает stcp_server
 *   4. Удаляет запись порта из БД
 * Затем освобождает запись auto_sock_iface и оповещает GUI. */
static void remove_iface_tcp(struct AUTO_SOCKET* as, struct stcp_server** server, struct ETCP_SOCKET** socket) {
    /* Сначала отменяем все callbacks/линки, затем освобождаем их socket context. */
    if (*server) {
        stcp_server_list_remove(as->instance, *server);
        stcp_link_server_destroy(*server);
        *server = NULL;
    }
    if (*socket) { tcp_socket_remove(*socket); *socket = NULL; }
}

static void remove_iface_sockets(struct AUTO_SOCKET* as, uint32_t ifindex) {
    struct auto_sock_iface** pp = &as->ifaces;
    while (*pp) {
        if ((*pp)->netif_index == ifindex) {
            struct auto_sock_iface* ifa = *pp;
            *pp = ifa->next;
            const char* ifname = ifa->ifname;

            if (ifa->v4_udp) {
                DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] removing v4 UDP socket %s ifidx=%u", ifa->v4_udp->name, ifindex);
                ncd_remove_socket_links(as->instance, ifa->v4_udp);
                etcp_socket_remove(ifa->v4_udp);
                delete_port_from_db(as, ifname, AF_INET, AS_PROTO_UDP);
            }
            if (ifa->v6_udp) {
                DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] removing v6 UDP socket %s ifidx=%u", ifa->v6_udp->name, ifindex);
                ncd_remove_socket_links(as->instance, ifa->v6_udp);
                etcp_socket_remove(ifa->v6_udp);
                delete_port_from_db(as, ifname, AF_INET6, AS_PROTO_UDP);
            }
            if (ifa->v4_tcp) {
                DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] removing v4 TCP socket ifidx=%u", ifindex);
                remove_iface_tcp(as, &ifa->v4_tcp, &ifa->v4_tcp_sock);
                delete_port_from_db(as, ifname, AF_INET, AS_PROTO_TCP);
            }
            if (ifa->v6_tcp) {
                DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] removing v6 TCP socket ifidx=%u", ifindex);
                remove_iface_tcp(as, &ifa->v6_tcp, &ifa->v6_tcp_sock);
                delete_port_from_db(as, ifname, AF_INET6, AS_PROTO_TCP);
            }

            u_free(ifa);
            auto_socket_post_sockets_changed(as);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ─── Сканирование адресов интерфейса и определение типа ───
 *
 * Эти функции анализируют реальное состояние интерфейса: есть ли v4/v6 адреса,
 * являются ли они публичными или приватными. Результат используется reconcile_iface
 * для принятия решений о создании/удалении/пересоздании сокетов. */

#if !defined(_WIN32)

#include <stdio.h>

/* Проверяет через /proc/net/if_inet6: есть ли на интерфейсе постоянные (не temporary)
 * IPv6-адреса. Временные адреса (privacy extensions) пропускаются — они нестабильны
 * и не подходят для долгоживущих P2P-соединений. */
/* /proc/net/if_inet6 format: hex_addr(32) ifindex(2) prefixlen(2) scope(2) flags(2) ifname
   flags bit 0x20 = IFA_F_TEMPORARY */
static int iface_has_permanent_v6(const char* ifname) {
    FILE* f = fopen("/proc/net/if_inet6", "r");
    if (!f) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char addr[33], name[IFNAMSIZ];
        unsigned int flags;
        if (sscanf(line, "%32s %*x %*x %*x %x %15s", addr, &flags, name) == 3) {
            if (strcmp(name, ifname) != 0) continue;
            if (strncmp(addr, "fe80", 4) == 0) continue; /* link-local */
            if (!(flags & 0x20)) { found = 1; break; }
        }
    }
    fclose(f);
    return found;
}

/* Проверяет является ли конкретный IPv6-адрес временным (IFA_F_TEMPORARY,
 * privacy extensions). Такие адреса меняются и не годятся для P2P. */
static int is_v6_addr_temporary(const uint8_t addr[16], const char* ifname) {
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", addr[i]);
    FILE* f = fopen("/proc/net/if_inet6", "r");
    if (!f) return 0;
    char line[256];
    int tmp = 0;
    while (fgets(line, sizeof(line), f)) {
        char a[33], name[IFNAMSIZ];
        unsigned int flags;
        if (sscanf(line, "%32s %*x %*x %*x %x %15s", a, &flags, name) == 3) {
            if (strcmp(name, ifname) != 0) continue;
            if (strcmp(a, hex) == 0) { tmp = (flags & 0x20) != 0; break; }
        }
    }
    fclose(f);
    return tmp;
}

/*
 * Сканирует все IP-адреса на интерфейсе и определяет:
 *   - есть ли IPv4/IPv6 адреса (out_has_v4, out_has_v6)
 *   - тип адресов: PUBLIC (глобальный) или NAT (приватный/link-local)
 * Для IPv6 временные адреса (privacy extensions) пропускаются.
 * Если интерфейс не UP — все out-параметры сбрасываются в 0/NAT.
 * Используется reconcile_iface для сравнения ожидаемого и фактического состояния. */
static int scan_and_classify_iface(uint32_t ifindex, const char* ifname,
                                    int* out_has_v4, int* out_has_v6,
                                    uint8_t* out_v4_type, uint8_t* out_v6_type) {
    (void)ifindex;
    *out_has_v4 = 0; *out_has_v6 = 0;
    *out_v4_type = CFG_SERVER_TYPE_NAT;
    *out_v6_type = CFG_SERVER_TYPE_NAT;

    int has_perm_v6 = iface_has_permanent_v6(ifname);

    struct ifaddrs* ifa_list = NULL;
    if (getifaddrs(&ifa_list) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] getifaddrs: %s", strerror(errno)); return -1; }

    int is_up = 0;
    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (strcmp(ifa->ifa_name, ifname) != 0) continue;
        if (ifa->ifa_flags & IFF_UP) is_up = 1;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
            if (sin->sin_addr.s_addr == INADDR_ANY) continue;
            *out_has_v4 = 1;
            if (ip_is_public(sin->sin_addr.s_addr))
                *out_v4_type = CFG_SERVER_TYPE_PUBLIC;
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)ifa->ifa_addr;
            int cls = v6_classify(sin6->sin6_addr.s6_addr);
            if (cls == AS_V6_OTH || cls == AS_V6_LL) continue;
            if (has_perm_v6 && is_v6_addr_temporary(sin6->sin6_addr.s6_addr, ifname)) continue;
            *out_has_v6 = 1;
            if (cls == AS_V6_DIR)
                *out_v6_type = CFG_SERVER_TYPE_PUBLIC;
        }
    }
    freeifaddrs(ifa_list);

    /* интерфейс не UP — адреса не релевантны */
    if (!is_up) { *out_has_v4 = 0; *out_has_v6 = 0; }

    return 0;
}

/*
 * Полное сканирование: обходит все UP-интерфейсы (не loopback) и для каждого
 * вызывает reconcile_iface. Используется при старте и при ручном запросе
 * пересканирования (auto_socket_on_network_change). */
static int auto_socket_scan_all(struct AUTO_SOCKET* as) {
    if (!as->ports_ready) return 0;
    struct ifaddrs* ifa_list = NULL;
    if (getifaddrs(&ifa_list) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] getifaddrs: %s", strerror(errno)); return -1; }

    int up_count = 0;
    char up_names[64][IFNAMSIZ];

    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        int found = 0;
        for (int i = 0; i < up_count; i++) { if (strcmp(up_names[i], ifa->ifa_name) == 0) { found = 1; break; } }
        if (found) continue;

        if (up_count >= 64) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] too many interfaces, cap at 64"); freeifaddrs(ifa_list); return -1; }
        strncpy(up_names[up_count], ifa->ifa_name, IFNAMSIZ - 1);
        up_names[up_count][IFNAMSIZ - 1] = 0;
        up_count++;
    }
    freeifaddrs(ifa_list);

    int result = 0;
    for (int i = 0; i < up_count; i++)
        if (reconcile_iface(as, if_nametoindex(up_names[i]), up_names[i]) < 0) result = -1;
    struct auto_sock_iface** pp = &as->ifaces;
    while (*pp) {
        struct auto_sock_iface* ifa = *pp;
        int found = 0;
        for (int i = 0; i < up_count; i++) {
            if (!strcmp(up_names[i], ifa->ifname) && if_nametoindex(up_names[i]) == ifa->netif_index) { found = 1; break; }
        }
        if (!found) { remove_iface_sockets(as, ifa->netif_index); continue; }
        pp = &ifa->next;
    }
    return result;
}

#else /* _WIN32 */

/* Собирает IfIndex всех UP не-loopback адаптеров. Возвращает число найденных (до max_count). */
static int win_collect_up_ifaces(uint32_t* out_ifindex, int max_count) {
    PIP_ADAPTER_ADDRESSES adapters = NULL;
    ULONG size = 0;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD rv = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &size);
    if (rv == ERROR_NO_DATA) return 0;
    if (rv != ERROR_BUFFER_OVERFLOW) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] GetAdaptersAddresses size failed: %lu", rv); return -1; }
    adapters = (PIP_ADAPTER_ADDRESSES)u_malloc(size);
    if (!adapters) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] adapter list allocation failed"); return -1; }
    rv = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, adapters, &size);
    if (rv != NO_ERROR) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] GetAdaptersAddresses failed: %lu", rv); u_free(adapters); return -1; }

    int count = 0;
    for (PIP_ADAPTER_ADDRESSES a = adapters; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (count == max_count) {
            DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] too many UP adapters: capacity=%d", max_count); u_free(adapters); return -1;
        }
        out_ifindex[count++] = a->IfIndex ? a->IfIndex : a->Ipv6IfIndex;
    }
    u_free(adapters);
    return count;
}

static int scan_and_classify_iface(uint32_t ifindex, const char* ifname,
                                    int* out_has_v4, int* out_has_v6,
                                    uint8_t* out_v4_type, uint8_t* out_v6_type) {
    (void)ifname;
    *out_has_v4 = 0; *out_has_v6 = 0;
    *out_v4_type = CFG_SERVER_TYPE_NAT;
    *out_v6_type = CFG_SERVER_TYPE_NAT;

    PIP_ADAPTER_ADDRESSES adapters = NULL;
    ULONG size = 0;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD rv = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &size);
    if (rv == ERROR_NO_DATA) return 0;
    if (rv != ERROR_BUFFER_OVERFLOW) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] adapter sizing failed: %lu", rv); return -1; }
    adapters = (PIP_ADAPTER_ADDRESSES)u_malloc(size);
    if (!adapters) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] adapter allocation failed size=%lu", size); return -1; }
    rv = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, adapters, &size);
    if (rv != NO_ERROR) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] adapter lookup failed: %lu", rv); u_free(adapters); return -1; }

    PIP_ADAPTER_ADDRESSES a = adapters;
    while (a && a->IfIndex != ifindex && a->Ipv6IfIndex != ifindex) a = a->Next;
    if (!a || a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
        u_free(adapters);
        return 0;
    }

    for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
        if (!u->Address.lpSockaddr) continue;
        if (u->Address.lpSockaddr->sa_family == AF_INET) {
            const struct sockaddr_in* sin = (const struct sockaddr_in*)u->Address.lpSockaddr;
            if (sin->sin_addr.s_addr == INADDR_ANY) continue;
            *out_has_v4 = 1;
            if (ip_is_public(sin->sin_addr.s_addr)) *out_v4_type = CFG_SERVER_TYPE_PUBLIC;
        } else if (u->Address.lpSockaddr->sa_family == AF_INET6) {
            const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)u->Address.lpSockaddr;
            int cls = v6_classify(sin6->sin6_addr.s6_addr);
            if (cls == AS_V6_OTH || cls == AS_V6_LL) continue;
            *out_has_v6 = 1;
            if (cls == AS_V6_DIR) *out_v6_type = CFG_SERVER_TYPE_PUBLIC;
        }
    }
    u_free(adapters);
    return 0;
}

static int auto_socket_scan_all(struct AUTO_SOCKET* as) {
    if (!as->ports_ready) return 0;
    uint32_t up[64];
    int up_count = win_collect_up_ifaces(up, 64);

    if (up_count < 0) return -1;
    int result = 0;
    for (int i = 0; i < up_count; i++) {
        char ifname[IF_NAMESIZE];
        if (!if_indextoname(up[i], ifname)) snprintf(ifname, sizeof(ifname), "if%u", up[i]);
        if (reconcile_iface(as, up[i], ifname) < 0) result = -1;
    }

    /* prune: удаляем сокеты адаптеров, которых больше нет в UP-списке (или они DOWN) */
    struct auto_sock_iface** pp = &as->ifaces;
    while (*pp) {
        struct auto_sock_iface* ifa = *pp;
        int found = 0;
        for (int i = 0; i < up_count; i++) if (up[i] == ifa->netif_index) { found = 1; break; }
        if (!found) { remove_iface_sockets(as, ifa->netif_index); continue; }
        pp = &ifa->next;
    }
    return result;
}

#endif /* _WIN32 */

/* ═══════════ default-маршрут: проверка возможности выхода в интернет ═══════════
 *
 * Фильтр для авто-режима: интерфейс без default-маршрута (шлюза) не может
 * достучаться до публичных пиров и даёт только sendto ENETUNREACH (например
 * VirtualBox host-only, изолированные VPN). Такие интерфейсы пропускаем.
 * Возвращают 1 если default-маршрут есть, 0 если нет. При невозможности
 * определить — 1 (не пропускаем, чтобы не сломать существующие сетапы). */

#if defined(__linux__)

static int iface_has_default_v4(uint32_t ifindex, const char* ifname) {
    (void)ifindex;
    FILE* f = fopen("/proc/net/route", "r");
    if (!f) return 1;
    char line[512];
    int found = 0;
    if (fgets(line, sizeof(line), f)) {
        while (fgets(line, sizeof(line), f)) {
            char iface[IFNAMSIZ];
            unsigned long dest;
            if (sscanf(line, "%15s %lx %*x %*x", iface, &dest) == 2) {
                if (dest == 0 && strcmp(iface, ifname) == 0) { found = 1; break; }
            }
        }
    }
    fclose(f);
    return found;
}

static int iface_has_default_v6(uint32_t ifindex, const char* ifname) {
    (void)ifindex;
    FILE* f = fopen("/proc/net/ipv6_route", "r");
    if (!f) return 1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char dest[33], iface[IFNAMSIZ];
        unsigned int prefixlen;
        if (sscanf(line, "%32s %2x %*32s %*2x %*32s %*8x %*8x %*8x %*8x %15s", dest, &prefixlen, iface) == 3) {
            if (prefixlen == 0 && strcmp(iface, ifname) == 0) { found = 1; break; }
        }
    }
    fclose(f);
    return found;
}

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)

/* Native sockaddr-вектор FreeBSD; короткая маска допускает sa_len=0. */
static struct sockaddr* rta_get(struct rt_msghdr* rtm, int rtax) {
    if (!(rtm->rtm_addrs & (1 << rtax))) return NULL;
    char* p = (char*)(rtm + 1);
    size_t remaining = rtm->rtm_msglen - sizeof(*rtm);
    for (int i = 0; i <= rtax; i++) {
        if (rtm->rtm_addrs & (1 << i)) {
            if (remaining < 2) goto malformed;
            struct sockaddr* sa = (struct sockaddr*)p;
            size_t step = sa->sa_len ? (sa->sa_len + sizeof(long) - 1) & ~(sizeof(long) - 1) : sizeof(long);
            if ((sa->sa_len && sa->sa_len < 2) || step > remaining) goto malformed;
            if (i == rtax) return sa;
            p += step; remaining -= step;
        }
    }
malformed:
    DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] malformed FreeBSD route attributes index=%u len=%u", rtm->rtm_index, rtm->rtm_msglen);
    return NULL;
}

static int freebsd_iface_has_default(uint32_t ifindex, int family) {
    int mib[6] = { CTL_NET, PF_ROUTE, 0, family, NET_RT_DUMP, 0 };
    size_t need = 0;
    if (sysctl(mib, 6, NULL, &need, NULL, 0) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] route dump size failed: %s", strerror(errno)); return 1;
    }
    if (!need) return 0;
    if (need > UINT32_MAX) { DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] route dump too large: %zu", need); return 1; }
    char* buf = u_malloc(need);
    if (!buf) return 1;
    if (sysctl(mib, 6, buf, &need, NULL, 0) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] route dump failed: %s", strerror(errno)); u_free(buf); return 1;
    }

    int found = 0;
    char* p = buf;
    char* end = buf + need;
    while ((size_t)(end - p) >= sizeof(struct rt_msghdr)) {
        struct rt_msghdr* rtm = (struct rt_msghdr*)p;
        if (rtm->rtm_msglen < sizeof(*rtm) || rtm->rtm_msglen > (size_t)(end - p)) {
            DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] short route dump message len=%u remaining=%zu", rtm->rtm_msglen, (size_t)(end - p));
            found = 1; break; /* Ошибка снимка не запрещает интерфейс. */
        }
        if (rtm->rtm_index == ifindex && (rtm->rtm_flags & RTF_UP) && !(rtm->rtm_flags & RTF_HOST)) {
            struct sockaddr* dst = rta_get(rtm, RTAX_DST);
            struct sockaddr* mask = rta_get(rtm, RTAX_NETMASK);
            size_t offset = family == AF_INET ? offsetof(struct sockaddr_in, sin_addr) : offsetof(struct sockaddr_in6, sin6_addr);
            size_t size = family == AF_INET ? 4 : 16;
            int zero = dst && mask && dst->sa_family == family && dst->sa_len >= offset + size;
            for (size_t i = 0; zero && i < size; i++) if (((const uint8_t*)dst)[offset + i]) zero = 0;
            for (size_t i = 2; zero && i < mask->sa_len; i++) if (((const uint8_t*)mask)[i]) zero = 0;
            if (zero) { found = 1; break; }
        }
        p += rtm->rtm_msglen;
    }
    u_free(buf);
    return found;
}

static int iface_has_default_v4(uint32_t ifindex, const char* ifname) {
    (void)ifname;
    return freebsd_iface_has_default(ifindex, AF_INET);
}

static int iface_has_default_v6(uint32_t ifindex, const char* ifname) {
    (void)ifname;
    return freebsd_iface_has_default(ifindex, AF_INET6);
}

#elif defined(_WIN32)

static int iface_has_default_v4(uint32_t ifindex, const char* ifname) {
    (void)ifname;
    MIB_IPFORWARD_TABLE2* table = NULL;
    if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR || !table) return 1;
    int found = 0;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        if (table->Table[i].InterfaceIndex == ifindex &&
            table->Table[i].DestinationPrefix.PrefixLength == 0) { found = 1; break; }
    }
    FreeMibTable(table);
    return found;
}

static int iface_has_default_v6(uint32_t ifindex, const char* ifname) {
    (void)ifname;
    MIB_IPFORWARD_TABLE2* table = NULL;
    if (GetIpForwardTable2(AF_INET6, &table) != NO_ERROR || !table) return 1;
    int found = 0;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        if (table->Table[i].InterfaceIndex == ifindex &&
            table->Table[i].DestinationPrefix.PrefixLength == 0) { found = 1; break; }
    }
    FreeMibTable(table);
    return found;
}

#else

static int iface_has_default_v4(uint32_t ifindex, const char* ifname) { (void)ifindex; (void)ifname; return 1; }
static int iface_has_default_v6(uint32_t ifindex, const char* ifname) { (void)ifindex; (void)ifname; return 1; }

#endif

/* ═══════════ reconcile: сверка и исправление состояния сокетов ═══════════
 *
 * reconcile — ключевой механизм модуля. При любом изменении сети сравнивает
 * фактическое состояние интерфейса (IP-адреса, их тип) с текущим набором сокетов
 * и устраняет расхождения: создаёт недостающие сокеты, удаляет лишние,
 * пересоздаёт при смене типа (NAT↔PUBLIC), обновляет interface_addr. */

/* Находит или создаёт запись auto_sock_iface для интерфейса.
 * Нужна потому что reconcile_iface может вызываться до auto_socket_add_interface
 * (например при старте через auto_socket_scan_all). */
static struct auto_sock_iface* reconcile_get_ifa(struct AUTO_SOCKET* as, uint32_t ifindex, const char* ifname) {
    struct auto_sock_iface* ifa = as->ifaces;
    while (ifa) { if (ifa->netif_index == ifindex) return ifa; ifa = ifa->next; }
    ifa = u_calloc(1, sizeof(*ifa));
    if (!ifa) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] interface record allocation failed: %s", ifname); return NULL; }
    ifa->netif_index = ifindex;
    snprintf(ifa->ifname, sizeof(ifa->ifname), "%s", ifname);
    ifa->next = as->ifaces;
    as->ifaces = ifa;
    return ifa;
}

/* Удаляет запись auto_sock_iface если все 4 сокета (v4/v6 UDP+TCP) отсутствуют.
 * Вызывается после reconcile чтобы подчистить записи для интерфейсов без сокетов. */
static void reconcile_prune_ifa(struct AUTO_SOCKET* as, struct auto_sock_iface* ifa, uint32_t ifindex, const char* ifname) {
    if (!ifa) return;
    if (ifa->v4_udp || ifa->v6_udp || ifa->v4_tcp || ifa->v6_tcp) return;
    struct auto_sock_iface** pp = &as->ifaces;
    while (*pp && *pp != ifa) pp = &(*pp)->next;
    if (*pp) *pp = ifa->next;
    DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] iface record removed: %s idx=%u", ifname, ifindex);
    u_free(ifa);
}

/*
 * Сверяет состояние интерфейса с набором сокетов и устраняет расхождения.
 * Это основная логика модуля — вызывается при ЛЮБОМ изменении сети.
 *
 * Для каждого из 4 сокетов (v4/v6 UDP/TCP) проверяет 4 сценария:
 *   1. Адрес есть а сокета нет → создать сокет + линки
 *   2. Адреса нет а сокет есть → удалить сокет + линки
 *   3. Тип изменился (NAT↔PUBLIC) → пересоздать сокет с новым типом
 *   4. Адрес изменился (тот же семейство/тип, другой IP) → обновить
 *      interface_addr, оповестить через ETCP_SOCKET_EVENT_ADDR_CHANGED
 *
 * В конце удаляет запись auto_sock_iface если все сокеты отсутствуют,
 * и оповещает GUI если были изменения. */
static int reconcile_iface(struct AUTO_SOCKET* as, uint32_t ifindex, const char* ifname) {
    if (!ifindex) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] invalid interface index: %s", ifname); return -1; }
    int has_v4 = 0, has_v6 = 0, changed = 0;
    uint8_t v4_type = CFG_SERVER_TYPE_NAT, v6_type = CFG_SERVER_TYPE_NAT;
    if (scan_and_classify_iface(ifindex, ifname, &has_v4, &has_v6, &v4_type, &v6_type) < 0) return -1;
    if (as->instance->config->global.hide_my_sockets) v4_type = v6_type = CFG_SERVER_TYPE_PRIVATE;
    if (as->instance->config->global.auto_socket_skip_no_default_route) {
        if (has_v4 && !iface_has_default_v4(ifindex, ifname)) has_v4 = 0;
        if (has_v6 && !iface_has_default_v6(ifindex, ifname)) has_v6 = 0;
    }
    struct auto_sock_iface* ifa = as->ifaces;
    while (ifa && ifa->netif_index != ifindex) ifa = ifa->next;
    if (ifa && strcmp(ifa->ifname, ifname)) { remove_iface_sockets(as, ifindex); ifa = NULL; }
    if (!ifa && (has_v4 || has_v6)) ifa = reconcile_get_ifa(as, ifindex, ifname);
    if (!ifa) return (has_v4 || has_v6) ? -1 : 0;

    for (int v6 = 0; v6 < 2; v6++) {
        int family = v6 ? AF_INET6 : AF_INET, has_address = v6 ? has_v6 : has_v4;
        uint8_t type = v6 ? v6_type : v4_type;
        for (int tcp = 0; tcp < 2; tcp++) {
            int wanted = has_address && (as->protocols & (tcp ? CFG_AUTO_SOCKET_TCP : CFG_AUTO_SOCKET_UDP));
            struct ETCP_SOCKET** slot = tcp ? (v6 ? &ifa->v6_tcp_sock : &ifa->v4_tcp_sock) :
                                                    (v6 ? &ifa->v6_udp : &ifa->v4_udp);
            struct stcp_server** server = v6 ? &ifa->v6_tcp : &ifa->v4_tcp;
            struct ETCP_SOCKET* sock = *slot;
            struct sockaddr_storage* tcp_addr = v6 ? &ifa->v6_tcp_addr : &ifa->v4_tcp_addr;
            int rebuild = 0;
            if (sock && wanted) {
                socket_monitor_update_if_addr(sock);
                const struct sockaddr_storage* original = tcp ? tcp_addr : &sock->local_addr;
                if (family == AF_INET) {
                    const struct sockaddr_in* bound = (const struct sockaddr_in*)original;
                    const struct sockaddr_in* current = (const struct sockaddr_in*)&sock->interface_addr;
                    rebuild = bound->sin_addr.s_addr != current->sin_addr.s_addr;
                } else {
                    const struct sockaddr_in6* bound = (const struct sockaddr_in6*)original;
                    const struct sockaddr_in6* current = (const struct sockaddr_in6*)&sock->interface_addr;
                    rebuild = memcmp(&bound->sin6_addr, &current->sin6_addr, 16) ||
                              bound->sin6_scope_id != current->sin6_scope_id;
                }
                rebuild |= sock->type != type;
            }
            if (sock && (!wanted || rebuild)) {
                DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] %s endpoint %s iface=%s family=%d",
                           rebuild ? "rebuilding" : "removing", tcp ? "TCP" : "UDP", ifname, family);
                if (tcp) remove_iface_tcp(as, server, slot);
                else {
                    *slot = NULL;
                    ncd_remove_socket_links(as->instance, sock); etcp_socket_remove(sock);
                }
                if (!wanted) delete_port_from_db(as, ifname, family, tcp ? AS_PROTO_TCP : AS_PROTO_UDP);
                if (v6) as->v6_addr_changed = 1; else as->v4_addr_changed = 1;
                changed = 1;
            }
            if (wanted && !*slot) {
                if (tcp) *server = create_iface_tcp_socket(as, ifindex, ifname, family, type, slot);
                else *slot = create_iface_udp_socket(as, ifindex, ifname, family, type);
                if (*slot) { if (tcp) *tcp_addr = (*slot)->interface_addr; changed = 1; }
                else {
                    DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] skipping unavailable socket iface=%s ifidx=%u family=%s protocol=%s; retry on next scan",
                                ifname, ifindex, v6 ? "IPv6" : "IPv4", tcp ? "TCP" : "UDP");
                }
            }
        }
    }
    ifa->v4_type = v4_type; ifa->v6_type = v6_type;
    reconcile_prune_ifa(as, ifa, ifindex, ifname);
    if (changed) auto_socket_post_sockets_changed(as);
    return 0;
}

/* ═══════════ Platform monitoring ═══════════ */

/* ─── Linux: netlink ─── */
#ifdef __linux__

/* При изменении IP-адреса (RTM_NEWADDR / RTM_DELADDR) — запускаем reconcile для интерфейса. */
static void handle_nl_addr(struct AUTO_SOCKET* as, struct ifaddrmsg* ifa, int msg_type) {
    (void)msg_type;
    char ifname[IFNAMSIZ];
    if (!if_indextoname(ifa->ifa_index, ifname)) return;
    reconcile_iface(as, ifa->ifa_index, ifname);
}

/* При изменении состояния линка (RTM_NEWLINK / RTM_DELLINK):
 *   - DOWN или удаление → удаляем все сокеты интерфейса
 *   - UP (новый или поднялся) → запускаем reconcile */
static void handle_nl_link(struct AUTO_SOCKET* as, struct ifinfomsg* ifi, int msg_type) {
    char ifname[IFNAMSIZ];
    if (!if_indextoname(ifi->ifi_index, ifname)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_AS, "[as] handle_nl_link: if_indextoname(%u) failed, msg=%d — removing by index",
                   ifi->ifi_index, msg_type);
        remove_iface_sockets(as, ifi->ifi_index);
        return;
    }
    if (ifi->ifi_flags & IFF_LOOPBACK) return;

    int is_up = (ifi->ifi_flags & IFF_UP) != 0;

    if (msg_type == RTM_DELLINK || !is_up) {
        remove_iface_sockets(as, ifi->ifi_index);
    } else if (msg_type == RTM_NEWLINK && is_up) {
        /* интерфейс появился или стал UP — сверяем (адреса могут уже быть) */
        reconcile_iface(as, ifi->ifi_index, ifname);
    }
}

/* При изменении маршрута (RTM_NEWROUTE / RTM_DELROUTE): default-маршрут
 * (dst_len==0) мог появиться/исчезнуть на интерфейсе — пересканируем все. */
static void handle_nl_route(struct AUTO_SOCKET* as, struct rtmsg* rtm) {
    if (rtm->rtm_dst_len == 0) auto_socket_scan_all(as);
}

/* Коллбэк uasync для netlink-сокета. Принимает все сообщения netlink,
 * разбирает их и направляет в handle_nl_addr / handle_nl_link. */
static void auto_socket_netlink_cb(int fd, void* arg) {
    struct AUTO_SOCKET* as = (struct AUTO_SOCKET*)arg;
    char buf[8192];
    struct sockaddr_nl nladdr;
    DEBUG_DEBUG(DEBUG_CATEGORY_AS, "[as] netlink cb fd=%d", fd);
    for (;;) {
        socklen_t nladdr_len = sizeof(nladdr);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT | MSG_TRUNC, (struct sockaddr*)&nladdr, &nladdr_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == ENOBUFS) {
                DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] netlink overflow: rescan"); auto_socket_scan_all(as); continue;
            }
            DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] netlink recv: %s", strerror(errno)); break;
        }
        if (n == 0) break;
        if (nladdr_len < sizeof(nladdr) || nladdr.nl_pid) {
            DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] ignoring non-kernel netlink message"); continue;
        }
        if ((size_t)n > sizeof(buf)) {
            DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] netlink message truncated len=%zd: rescan", n); auto_socket_scan_all(as); continue;
        }
        int remaining = (int)n;
        struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
        while (NLMSG_OK(nlh, remaining)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_AS, "[as] netlink msg type=%u len=%u", nlh->nlmsg_type, nlh->nlmsg_len);
            if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR) {
                if (NLMSG_PAYLOAD(nlh, 0) < sizeof(struct ifaddrmsg)) goto short_message;
                handle_nl_addr(as, (struct ifaddrmsg*)NLMSG_DATA(nlh), nlh->nlmsg_type);
            } else if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK) {
                if (NLMSG_PAYLOAD(nlh, 0) < sizeof(struct ifinfomsg)) goto short_message;
                handle_nl_link(as, (struct ifinfomsg*)NLMSG_DATA(nlh), nlh->nlmsg_type);
            } else if (nlh->nlmsg_type == RTM_NEWROUTE || nlh->nlmsg_type == RTM_DELROUTE) {
                if (NLMSG_PAYLOAD(nlh, 0) < sizeof(struct rtmsg)) goto short_message;
                handle_nl_route(as, (struct rtmsg*)NLMSG_DATA(nlh));
            } else if (nlh->nlmsg_type == NLMSG_ERROR || nlh->nlmsg_type == NLMSG_OVERRUN) {
                DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] netlink error type=%u: rescan", nlh->nlmsg_type); auto_socket_scan_all(as);
            }
            if (nlh->nlmsg_type == NLMSG_DONE) break;
            nlh = NLMSG_NEXT(nlh, remaining);
            continue;
short_message:
            DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] short netlink payload type=%u len=%u", nlh->nlmsg_type, nlh->nlmsg_len);
            nlh = NLMSG_NEXT(nlh, remaining);
        }
    }
}

static int auto_socket_init_monitor(struct AUTO_SOCKET* as) {
    as->nl_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (as->nl_sock < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] netlink socket failed: %s", strerror(errno)); return -1;
    }

    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;
    nladdr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
    if (bind(as->nl_sock, (struct sockaddr*)&nladdr, sizeof(nladdr)) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] netlink bind failed: %s", strerror(errno));
        close(as->nl_sock); as->nl_sock = -1; return -1;
    }
    if (socket_set_nonblocking(as->nl_sock) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] netlink nonblocking mode failed: %s", strerror(errno));
        close(as->nl_sock); as->nl_sock = -1; return -1;
    }
    as->uasync_handle = uasync_add_socket(as->instance->ua, as->nl_sock, auto_socket_netlink_cb, NULL, NULL, "netlink_autosock", as);
    if (!as->uasync_handle) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] uasync_add_socket failed"); close(as->nl_sock); as->nl_sock = -1; return -1; }
    DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] netlink monitor started: fd=%d", (int)as->nl_sock);
    return 0;
}

/* Отключает netlink-мониторинг: убирает fd из uasync, закрывает сокет. */
static void auto_socket_destroy_monitor(struct AUTO_SOCKET* as) {
    if (as->uasync_handle) { uasync_remove_socket(as->instance->ua, as->uasync_handle); as->uasync_handle = NULL; }
    if (as->nl_sock >= 0) { close(as->nl_sock); as->nl_sock = -1; }
}

/* ─── BSD: route socket ───
 * На FreeBSD мониторим изменения через PF_ROUTE сокет. При любых изменениях
 * адресов/интерфейсов запускаем полное пересканирование. */
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)

static void auto_socket_bsd_cb(int fd, void* arg) {
    struct AUTO_SOCKET* as = (struct AUTO_SOCKET*)arg;
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] route sock read: %s", strerror(errno)); break;
        }
        if (!n) break;
        if (n < 4) { DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] short route prefix len=%zd", n); continue; }
        uint16_t length; memcpy(&length, buf, sizeof(length));
        if (length < 4 || length > n || (uint8_t)buf[2] != RTM_VERSION) {
            DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] invalid route prefix len=%u available=%zd version=%u", length, n, (uint8_t)buf[2]);
            continue;
        }
        unsigned type = (uint8_t)buf[3];
        if (type == RTM_IFINFO || type == RTM_IFANNOUNCE || type == RTM_NEWADDR || type == RTM_DELADDR ||
            type == RTM_ADD || type == RTM_DELETE || type == RTM_CHANGE) auto_socket_scan_all(as);
    }
}

static int auto_socket_init_monitor(struct AUTO_SOCKET* as) {
    as->route_sock = socket(PF_ROUTE, SOCK_RAW, AF_UNSPEC);
    if (as->route_sock < 0) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] route socket: %s", strerror(errno)); return -1; }
    if (socket_set_nonblocking(as->route_sock) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] route socket nonblocking failed: %s", strerror(errno));
        close(as->route_sock); as->route_sock = -1; return -1;
    }
    as->uasync_handle = uasync_add_socket(as->instance->ua, as->route_sock, auto_socket_bsd_cb, NULL, NULL, "route_autosock", as);
    if (!as->uasync_handle) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] uasync_add_socket failed"); close(as->route_sock); as->route_sock = -1; return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] route socket monitor started: fd=%d", (int)as->route_sock);
    return 0;
}

static void auto_socket_destroy_monitor(struct AUTO_SOCKET* as) {
    if (as->uasync_handle) { uasync_remove_socket(as->instance->ua, as->uasync_handle); as->uasync_handle = NULL; }
    if (as->route_sock >= 0) { close(as->route_sock); as->route_sock = -1; }
}

/* ─── Windows: IP Helper ───
 * Используем NotifyUnicastIpAddressChange + NotifyRouteChange2.
 * Коллбэки приходят из системных потоков → uasync_post для обработки в главном потоке. */
#elif defined(_WIN32)

static void WINAPI win_addr_change_cb(PVOID ctx, PMIB_UNICASTIPADDRESS_ROW row, MIB_NOTIFICATION_TYPE type) {
    (void)row; (void)type;
    struct AUTO_SOCKET* as = (struct AUTO_SOCKET*)ctx;
    auto_socket_post_scan(as);
}

static void WINAPI win_route_change_cb(PVOID ctx, PMIB_IPFORWARD_ROW2 row, MIB_NOTIFICATION_TYPE type) {
    (void)row; (void)type;
    struct AUTO_SOCKET* as = (struct AUTO_SOCKET*)ctx;
    auto_socket_post_scan(as);
}

static int auto_socket_init_monitor(struct AUTO_SOCKET* as) {
    DWORD ret = NotifyUnicastIpAddressChange(AF_UNSPEC, win_addr_change_cb, as, FALSE, &as->addr_notify_handle);
    if (ret != NO_ERROR) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] NotifyUnicastIpAddressChange: %lu", (unsigned long)ret); return -1; }
    ret = NotifyRouteChange2(AF_UNSPEC, win_route_change_cb, as, FALSE, &as->route_notify_handle);
    if (ret != NO_ERROR) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] NotifyRouteChange2: %lu", (unsigned long)ret); return -1; }
    DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] Windows IP Helper monitor started");
    return 0;
}

static void auto_socket_destroy_monitor(struct AUTO_SOCKET* as) {
    DWORD addr_result = as->addr_notify_handle ? CancelMibChangeNotify2(as->addr_notify_handle) : NO_ERROR;
    DWORD route_result = as->route_notify_handle ? CancelMibChangeNotify2(as->route_notify_handle) : NO_ERROR;
    if (addr_result != NO_ERROR || route_result != NO_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] native cancel failed addr=%lu route=%lu; retaining detached context",
                    (unsigned long)addr_result, (unsigned long)route_result);
        as->retain_context = 1;
    }
}

#else
/* Платформа без встроенного мониторинга — только явные вызовы API (JNI и т.д.). */
static int auto_socket_init_monitor(struct AUTO_SOCKET* as) { (void)as; return 0; }
static void auto_socket_destroy_monitor(struct AUTO_SOCKET* as) { (void)as; }
#endif

/* ═══════════ Public API ═══════════ */

/*
 * Инициализирует модуль auto_socket. Если auto_sockets=no в конфиге — сразу
 * возвращает 0 без действий. Иначе создаёт таблицу портов в БД, запускает
 * платформенный мониторинг и сканирует все текущие UP-интерфейсы.
 * Состояние хранится в inst->auto_socket_state.
 */
int auto_socket_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->ua) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] instance/uasync is NULL"); return -1; }
    if (!inst->config || inst->config->global.auto_sockets == CFG_AUTO_SOCKETS_OFF) return 0;
    if (inst->auto_socket_state) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] already initialized"); return -1; }

    struct AUTO_SOCKET* as = u_calloc(1, sizeof(*as));
    if (!as) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] alloc failed"); return -1; }
#ifdef _WIN32
    InitializeCriticalSection(&as->notify_lock);
#else
    int mutex_error = pthread_mutex_init(&as->notify_lock, NULL);
    if (mutex_error) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] mutex init failed: %s", strerror(mutex_error)); u_free(as); return -1; }
#endif
#ifdef __linux__
    as->nl_sock = -1;
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    as->route_sock = -1;
#endif
    as->instance = inst;
    as->mode = (uint8_t)inst->config->global.auto_sockets;
    as->protocols = (uint8_t)inst->config->global.auto_socket_protocols;
    inst->auto_socket_state = as;

    /* Android получает адреса из конфига; системный режим — от монитора интерфейсов. */
    if (as->mode == CFG_AUTO_SOCKETS_ANDROID) {
        as->ports_ready = 1;
        if (auto_socket_reconcile_config(as) < 0) { auto_socket_destroy(inst); return -1; }
    } else {
        if (restore_ports(as)) { auto_socket_destroy(inst); return -1; }
        if (auto_socket_init_monitor(as) < 0) { auto_socket_destroy(inst); return -1; }
        if (auto_socket_scan_all(as) < 0) { auto_socket_destroy(inst); return -1; }
    }
    int v4 = as_check_usable(as, AF_INET), v6 = as_check_usable(as, AF_INET6);
    if (!as->ports_ready) DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] waiting for saved ports before opening system sockets");
    else if (!v4 && !v6) DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] initialized without usable sockets; waiting for network/config changes");
    DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] initialized mode=%s usable_v4=%d usable_v6=%d protocols=%s",
               as->mode == CFG_AUTO_SOCKETS_ANDROID ? "android" : "system", v4, v6,
               as->protocols == CFG_AUTO_SOCKET_BOTH ? "tcp+udp" : as->protocols == CFG_AUTO_SOCKET_TCP ? "tcp" : "udp");
    return 0;
}

/*
 * Завершает работу модуля: останавливает платформенный мониторинг, удаляет
 * все сокеты и линки, освобождает память. Безопасно вызывать повторно. */
void auto_socket_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->auto_socket_state) return;
    struct AUTO_SOCKET* as = (struct AUTO_SOCKET*)inst->auto_socket_state;
    inst->auto_socket_state = NULL;
    AS_LOCK(as); as->stopping = 1; AS_UNLOCK(as);

    if (as->mode != CFG_AUTO_SOCKETS_ANDROID) auto_socket_destroy_monitor(as);
    if (as->pending) { uasync_cancel_post(inst->ua, as->pending); as->pending = NULL; }
    while (as->ifaces) remove_iface_sockets(as, as->ifaces->netif_index);
    u_free(as->ports); as->ports = NULL; as->port_count = as->port_capacity = 0; as->ports_load = NULL;

    DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] destroyed");
    if (as->retain_context) { as->instance = NULL; return; }
#ifdef _WIN32
    DeleteCriticalSection(&as->notify_lock);
#else
    pthread_mutex_destroy(&as->notify_lock);
#endif
    u_free(as);
}

static void auto_socket_scan_all_trampoline(void* arg) {
    struct AUTO_SOCKET* as = arg;
    AS_LOCK(as); as->pending = NULL; AS_UNLOCK(as);
    if (auto_socket_scan_all(as) < 0) DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] posted network scan incomplete");
}

void auto_socket_on_network_change(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->auto_socket_state) return;
    struct AUTO_SOCKET* as = (struct AUTO_SOCKET*)inst->auto_socket_state;
    /* Android-режим: обновление идёт только через auto_socket_reload_config (Kotlin → JNI). */
    if (as->mode == CFG_AUTO_SOCKETS_ANDROID) return;
    auto_socket_post_scan(as);
}

/* ═══════════ Android-режим: reconcile сокетов из [server] секций конфига ═══════════ */

/* Сравнивает CFG_SERVER с существующим ETCP_SOCKET: имя, адрес+порт, тип, транспорт. */
static int server_matches_socket(const struct CFG_SERVER* server, const struct ETCP_SOCKET* sock) {
    if (!server || !sock) return 0;
    if (strcmp(server->name, sock->name) != 0) return 0;
    if (server->transport != (uint8_t)sock->is_tcp) return 0;
    if (server->type != sock->type) return 0;
    if (server->netif_index && server->netif_index != sock->netif_index) return 0;
    if (server->reality_enabled != sock->reality_enabled || server->only_local != sock->only_local) return 0;
    const struct sockaddr_storage* a = &server->ip;
    const struct sockaddr_storage* b = sock->local_addr.ss_family ? &sock->local_addr : &sock->interface_addr;
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in* ia = (const struct sockaddr_in*)a;
        const struct sockaddr_in* ib = (const struct sockaddr_in*)b;
        return ia->sin_addr.s_addr == ib->sin_addr.s_addr && (!ia->sin_port || ia->sin_port == ib->sin_port);
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6* ia = (const struct sockaddr_in6*)a;
        const struct sockaddr_in6* ib = (const struct sockaddr_in6*)b;
        return !memcmp(&ia->sin6_addr, &ib->sin6_addr, 16) && ia->sin6_scope_id == ib->sin6_scope_id &&
               (!ia->sin6_port || ia->sin6_port == ib->sin6_port);
    }
    return 0;
}

/* Удаляет один сокет (UDP или TCP) и все его линки. */
static void remove_config_socket(struct AUTO_SOCKET* as, struct ETCP_SOCKET* sock) {
    if (!sock) return;
    ncd_remove_socket_links(as->instance, sock);
    if (sock->is_tcp) {
        struct stcp_server* tsrv = stcp_server_list_find_by_sock(as->instance, sock);
        if (tsrv) {
            stcp_server_list_remove(as->instance, tsrv);
            stcp_link_server_destroy(tsrv);
        }
        tcp_socket_remove(sock);
    } else {
        etcp_socket_remove(sock);
    }
}

/* Сверяет inst->config->servers с inst->etcp_sockets и устраняет расхождения:
 * удаляет сокеты, которых нет/которые изменились (проход 1), создаёт отсутствующие (проход 2). */
static int auto_socket_reconcile_config(struct AUTO_SOCKET* as) {
    struct UTUN_INSTANCE* inst = as->instance;
    if (!inst || !inst->config) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] missing config for reconcile"); return -1; }

    /* Тип сокета определяем по фактическому адресу (не доверяем type из конфига):
     * публичный адрес → PUBLIC, приватный/link-local → NAT. */
    for (struct CFG_SERVER* s = inst->config->servers; s; s = s->next) {
        uint8_t by_addr = CFG_SERVER_TYPE_NAT;
        if (s->ip.ss_family == AF_INET) {
            if (ip_is_public(((const struct sockaddr_in*)&s->ip)->sin_addr.s_addr))
                by_addr = CFG_SERVER_TYPE_PUBLIC;
        } else if (s->ip.ss_family == AF_INET6) {
            if (v6_classify(((const struct sockaddr_in6*)&s->ip)->sin6_addr.s6_addr) == AS_V6_DIR)
                by_addr = CFG_SERVER_TYPE_PUBLIC;
        }
        if (inst->config->global.hide_my_sockets) by_addr = CFG_SERVER_TYPE_PRIVATE;
        if (s->type != by_addr)
            DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] server %s type %d → %d (hide_my_sockets=%d)",
                       s->name, s->type, by_addr, inst->config->global.hide_my_sockets);
        s->type = by_addr;
    }

    /* Проход 1: удалить сокеты, которых нет в новом списке, либо которые изменились. */
    for (unsigned id = 0; id < 256; id++) {
        struct ETCP_SOCKET* sock = inst->etcp_sockets;
        while (sock && sock->sock_id != id) sock = sock->next;
        if (!sock || sock->probe_only) continue;
        const struct CFG_SERVER* srv = NULL;
        for (const struct CFG_SERVER* s = inst->config->servers; s; s = s->next)
            if (strcmp(s->name, sock->name) == 0) { srv = s; break; }
        ip_str_t old_addr = sockaddr_storage_to_str(sock->local_addr.ss_family ? &sock->local_addr : &sock->interface_addr);
        if (!srv) {
            DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] removing socket %s (%s) addr=%s (no longer in config)",
                       sock->name, sock->is_tcp ? "tcp" : "udp", old_addr.str);
            remove_config_socket(as, sock);
        } else if (!server_matches_socket(srv, sock)) {
            DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] socket %s changed: %s → %s — rebuild",
                       sock->name, old_addr.str, sockaddr_storage_to_str(&srv->ip).str);
            remove_config_socket(as, sock);
        }
    }

    /* Проход 2: создать сокеты, которых ещё нет. */
    for (struct CFG_SERVER* s = inst->config->servers; s; s = s->next) {
        int exists = 0;
        for (struct ETCP_SOCKET* es = inst->etcp_sockets; es; es = es->next)
            if (strcmp(es->name, s->name) == 0) { exists = 1; break; }
        if (exists) continue;
        struct ETCP_SOCKET* created = NULL;
        if (etcp_create_server_socket(inst, s, &created) == 0 && created) {
            DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] created socket %s (%s) addr=%s",
                       s->name, s->transport ? "tcp" : "udp", sockaddr_storage_to_str(&s->ip).str);
            ncd_add_socket_links(inst, created);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] skipping unavailable configured socket name=%s ifidx=%u protocol=%s addr=%s; retry on next reload",
                        s->name, s->netif_index, s->transport ? "TCP" : "UDP", sockaddr_storage_to_str(&s->ip).str);
        }
    }

    auto_socket_post_sockets_changed(as);
    return 0;
}

void auto_socket_reload_config(struct UTUN_INSTANCE* inst, struct utun_config* new_cfg) {
    if (!new_cfg) return;
    if (!inst || !inst->auto_socket_state) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] reload_config: invalid args"); free_config(new_cfg); return; }
    struct AUTO_SOCKET* as = (struct AUTO_SOCKET*)inst->auto_socket_state;
    if (new_cfg == inst->config) { DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] reload_config: current config passed as new"); return; }
    if (as->mode != CFG_AUTO_SOCKETS_ANDROID) { DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] reload_config: not in android mode"); free_config(new_cfg); return; }

    /* Сводка до/после: сколько серверов было → стало. */
    int old_count = 0; for (struct CFG_SERVER* s = inst->config->servers; s; s = s->next) old_count++;
    int new_count = 0; for (struct CFG_SERVER* s = new_cfg->servers; s; s = s->next) new_count++;

    /* Некоторые параметры bind не хранятся в ETCP_SOCKET; сравниваем прежний и новый CFG_SERVER. */
    for (unsigned id = 0; id < 256; id++) {
        struct ETCP_SOCKET* socket = inst->etcp_sockets;
        while (socket && socket->sock_id != id) socket = socket->next;
        if (!socket || socket->probe_only) continue;
        const struct CFG_SERVER* old = inst->config->servers;
        const struct CFG_SERVER* next = new_cfg->servers;
        while (old && strcmp(old->name, socket->name)) old = old->next;
        while (next && strcmp(next->name, socket->name)) next = next->next;
        if (old && next && (old->so_mark != next->so_mark || old->fib != next->fib || old->mtu != next->mtu ||
            old->ipv6_mode != next->ipv6_mode || old->socks_enabled != next->socks_enabled ||
            old->netif_index != next->netif_index || old->only_local != next->only_local || old->reality_enabled != next->reality_enabled)) {
            DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] listener options changed: %s", socket->name);
            remove_config_socket(as, socket);
        }
    }
    for (struct CFG_CLIENT* client = inst->config->clients; client; client = client->next) {
        for (struct CFG_CLIENT_LINK* link = client->links; link; link = link->next) {
            if (!link->local_srv) continue;
            snprintf(link->server_name, sizeof(link->server_name), "%s", link->local_srv->name);
            link->local_srv = new_cfg->servers;
            while (link->local_srv && strcmp(link->local_srv->name, link->server_name)) link->local_srv = link->local_srv->next;
            if (!link->local_srv) DEBUG_WARN(DEBUG_CATEGORY_AS, "[as] client %s bind server %s removed", client->name, link->server_name);
        }
    }

    /* Переносим новый список серверов в inst->config, старый освобождаем. */
    struct CFG_SERVER* old = inst->config->servers;
    inst->config->servers = new_cfg->servers;
    new_cfg->servers = NULL;
    while (old) { struct CFG_SERVER* n = old->next; u_free(old); old = n; }
    free_config(new_cfg);

    DEBUG_INFO(DEBUG_CATEGORY_AS, "[as] reload_config: %d → %d servers", old_count, new_count);

    if (auto_socket_reconcile_config(as) < 0) DEBUG_ERROR(DEBUG_CATEGORY_AS, "[as] reload incomplete; new server config retained");
}
