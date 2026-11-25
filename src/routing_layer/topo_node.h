/**
 * @file topo_node.h
 * @brief Модель данных узла сети — структуры, сериализация, wire-формат.
 *
 * TOPO_NODE — общие ключи, имя и адреса; реестр хранит указатель с подсчётом ссылок.
 * Подписанная запись заменяется целиком при более свежем timestamp, вместе с подписью.
 * TOPO_GROUP_NODE — пути, подсети и состояние узла в конкретной группе.
 * TOPOMSG_NODE — сетевой формат. Модуль сериализует, подписывает и проверяет записи.
 * Работа с реестром и группами выполняется в потоке uasync экземпляра.
 */
#ifndef TOPO_NODE_H
#define TOPO_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "../lib/ll_queue.h"
#include "secure_channel.h"
#include "address_profile.h"
#include "devgroup.h"

struct TOPO_GROUP;
struct TOPO_GROUPS;
struct ETCP_SOCKET;
struct ETCP_LINK;
struct NODE_CONN_DIRECT;
struct ETCP_CONN;
struct UTUN_INSTANCE;
struct reality_client_config;

#define MAX_HOPS 16 /* Максимальная длина цепочки BGP-пути. */

#define TOPO_ADDR_INTERFACE  0
#define TOPO_ADDR_NAT        1
#define TOPO_ADDR_REAL       2

#define TOPO_PROTO_UDP     0x01
#define TOPO_PROTO_TCP     0x02
#define TOPO_PROTO_REALITY 0x04   /* флаг reality-камуфляжа поверх TCP (reality-TCP = 0x06) */

#define ADDR_TYPE_NETIF       0
#define ADDR_TYPE_DIRECT      1
#define ADDR_TYPE_NAT_EIM     2
#define ADDR_TYPE_NAT_STRICT  3
#define ADDR_TYPE_NAT         4   /* за NAT, тип неизвестен */
#define ADDR_TYPE_CONTACT     5   /* адрес не изменён, вход требует предварительного контакта */
#define ADDR_TYPE_NAT_EIM_UNVERIFIED 6 /* EIM наблюдается, незнакомый вход не проверен: EIM? */

#define PROBE_STATUS_NONE        0
#define PROBE_STATUS_IN_PROGRESS 1
#define PROBE_STATUS_DONE        2
#define PROBE_STATUS_DEFERRED    3   /* отложена до следующего burst (standby_wait) */

#define PROBE_RESULT_UNKNOWN     0
#define PROBE_RESULT_REACHABLE   1
#define PROBE_RESULT_UNREACHABLE 2

#define CONN_PROBE_COUNT       3
#define CONN_PROBE_TIMEOUT_MS  1000

#define CONN_TYPE_NONE     0
#define CONN_TYPE_DIRECT   1
#define CONN_TYPE_REVERSE  2
#define CONN_TYPE_INDIRECT 3

#define CLIENT_TYPE_SERVER   0
#define CLIENT_TYPE_DESKTOP  1
#define CLIENT_TYPE_MOBILE   2

#define CLIENT_ACTIVITY_STANDBY 0
#define CLIENT_ACTIVITY_ACTIVE  1
#define CONN_MGR_MAX_INTERMEDIARIES 3

#define NCONN_DIRECT   (1 << 0)  // прямое ETCP подключение
#define NCONN_INDIRECT (1 << 1)  // через посредника
#define NCONN_BGP      (1 << 2)  // узел виден в топологии (BGP)

#define TOPO_FLAG_SEND_SUBNETS  0x01
#define TOPO_FLAG_RADIO         0x02
#define TOPO_FLAG_PHONE_KNOWN   0x04 /* состояние телефона опубликовано */
#define TOPO_FLAG_PHONE_BUSY    0x08 /* занят телефонным либо обычным uTun-вызовом */
#define TOPO_GROUP_UTUN  0x8000000000000000ULL
#define TOPO_GROUP_TYPE_UTUN 1
#define TOPO_GROUP_TYPE_CHAT 2

/* Результаты проб адресов. *_time — timebase (0.1 ms), *_rtt — те же единицы;
 * *_status — PROBE_RESULT_*, probe_status — PROBE_STATUS_*. */
struct TOPO_CONNECTIVITY {
    uint8_t  probe_status;
    uint8_t  pending_count;         // незавершённые пробы
    uint64_t probe_start_time;
    uint16_t interface_min_rtt;
    uint16_t nat_min_rtt;
    uint16_t real_min_rtt;
    uint8_t  interface_status;
    uint8_t  nat_status;
    uint8_t  real_status;
    uint8_t  reserved;
    uint64_t interface_probe_time;
    uint64_t nat_probe_time;
    uint64_t real_probe_time;
    uint64_t ping_req_time;
    uint64_t last_ping_time;
    void*    probe_list;            // принадлежащие узлу контексты текущих проб
    void*    probe_deferred_wait;   /* контекст отложенной пробы (standby_wait, Android) */
    void*    nat_probe_wait;        /* контекст таймера: NAT сохраняет холодный ingress перед фоновыми ping */
};

#define PING_TTL_TB (86400ULL * 10000ULL)

struct TOPOMSG_SOCKMETA4 {
    uint8_t id, config_type, mapping, inbound, source;
} __attribute__((packed));
struct TOPOMSG_ADDR4     { uint8_t addr[4]; uint16_t port; uint8_t type, socket_id, protocol; } __attribute__((packed));
struct TOPOMSG_SOCKMETA6 {
    uint8_t id, config_type, mapping, inbound, source;
} __attribute__((packed));
struct TOPOMSG_ADDR6     { uint8_t addr[16]; uint16_t port; uint8_t type, socket_id, protocol; } __attribute__((packed));
struct TOPOMSG_SUBNET4   { uint8_t addr[4]; uint8_t prefix_length; } __attribute__((packed));
struct TOPOMSG_SUBNET6   { uint8_t addr[16]; uint8_t prefix_length; } __attribute__((packed));

/* Reality-камуфляж сокета: что нужно клиенту для подключения к reality TCP-сокету.
 * short_id(8) + server_pubkey(32) + version(3) + server_name_len(1), затем server_name без NUL (до 255 байт). */
#define TOPO_REALITY_SNAME_MAX 256
struct TOPOMSG_REALITY_SOCK {
    uint8_t socket_id;
    uint8_t short_id[8];
    uint8_t server_pubkey[32];
    uint8_t version[3];
    uint8_t server_name_len;
} __attribute__((packed));

/* NODEINFO: этот заголовок, имя без NUL, connection_order без NUL, devgroup[devgroup_size], SOCKMETA4/ADDR4/SOCKMETA6/ADDR6/REALITY,
 * опциональные SUBNET4/SUBNET6, затем hop_count ID. Счётчики — число элементов, имя до 63 байт.
 * Многобайтные числа wire копируются в нативном порядке; IP — сетевые байты, port в памяти — host order. */
struct TOPOMSG_NODE {
    uint8_t connection_order_len; /* Число байтов политики после имени, без NUL. */
    uint16_t reality_size; /* Суммарные байты переменных REALITY-записей. */
    uint16_t devgroup_size; /* Блок devgroup после имени, 0 — отсутствует. */
    uint8_t flags; uint64_t reserved; uint8_t ver; /* В общей записи узла поле группы зарезервировано нулём. */
    uint64_t timestamp;                                             // версия всей подписанной записи, Unix microseconds
    uint8_t  public_key[SC_PUBKEY_SIZE], ed25519_public_key[SC_PUBKEY_SIZE]; /* Полная пара, локальный ID выводится из X25519. */
    uint8_t  x25519_self_sig[64];
    uint8_t  node_name_len, local_v4_sockets, local_v4_addrs, local_v6_sockets, local_v6_addrs;
    uint8_t  local_v4_subnets, local_v6_subnets, hop_count;
    uint8_t  local_reality_socks;                                     // кол-во reality-сокетов
    uint8_t  client_type;                                             // CLIENT_TYPE_SERVER/DESKTOP/MOBILE
    uint8_t  client_activity;                                         // CLIENT_ACTIVITY_STANDBY/ACTIVE
    uint16_t cumulative_rtt;
} __attribute__((packed));

#define TOPOMSG_NODE_HDR_SIZE sizeof(struct TOPOMSG_NODE)

struct TOPO_SOCKMETA4 { struct TOPO_SOCKMETA4* next; uint8_t id, config_type; struct address_profile address_profile; };
/* port=0 — известен только внешний TCP IP; адрес хранится, но не участвует в подключении. */
struct TOPO_ADDR4     { struct TOPO_ADDR4* next; uint8_t addr[4]; uint16_t port; uint8_t type, socket_id, protocol; };
struct TOPO_SOCKMETA6 { struct TOPO_SOCKMETA6* next; uint8_t id, config_type; struct address_profile address_profile; };
struct TOPO_ADDR6     { struct TOPO_ADDR6* next; uint8_t addr[16]; uint16_t port; uint8_t type, socket_id, protocol; };
struct TOPO_SUBNET4   { struct TOPO_SUBNET4* next; uint8_t addr[4]; uint8_t prefix_length; };
struct TOPO_SUBNET6   { struct TOPO_SUBNET6* next; uint8_t addr[16]; uint8_t prefix_length; };

/* Reality-камуфляж сокета (in-memory): per-socket список, значения на узел одинаковы. */
struct TOPO_REALITY_SOCK {
    struct TOPO_REALITY_SOCK* next;
    uint8_t socket_id;
    uint8_t short_id[8];
    uint8_t server_pubkey[32];
    uint8_t version[3];
    char    server_name[TOPO_REALITY_SNAME_MAX];
};

/* Reality-параметры адреса для персистенции в node_addresses.options (BLOB, фиксированный размер). */
#define TOPO_ADDR_REALITY_OPTS_SIZE (8 + 32 + 3 + TOPO_REALITY_SNAME_MAX)
struct TOPO_ADDR_REALITY_OPTS {
    uint8_t short_id[8];
    uint8_t server_pubkey[32];
    uint8_t version[3];
    char    server_name[TOPO_REALITY_SNAME_MAX];
};

struct _topo_head { struct _topo_head* next; };
static inline int topo_list_count(struct _topo_head* head) { int n = 0; while (head) { n++; head = head->next; } return n; }

/** Общая запись узла; node_registry хранит указатель на неё в отдельном ll_entry. */
struct TOPO_NODE {
    struct ll_entry ll;
    struct devgroup_record devgroup; /* Общая подписанная принадлежность, владеет всеми байтами. */
    uint32_t group_ref_count;                                    // ссылки групп, ядра и временных пользователей
    uint64_t node_id; uint8_t ver;
    uint64_t timestamp;                                          // 0 = неподписанные bootstrap-сведения
    uint8_t  public_key[SC_PUBKEY_SIZE], ed25519_public_key[SC_PUBKEY_SIZE];
    uint8_t  x25519_self_sig[64];                                 // Ed25519-подпись общей записи
    uint8_t  client_type;                                          // CLIENT_TYPE_SERVER/DESKTOP/MOBILE
    uint8_t  client_activity;                                      // CLIENT_ACTIVITY_STANDBY/ACTIVE
    char*    node_name;                                          // собственная строка; освобождается с записью
    char connection_order[256]; /* Подписанная строка переменной wire-длины; пустая только у bootstrap. */
    struct TOPO_SOCKMETA4* v4_sock_meta;
    struct TOPO_ADDR4*     v4_addrs;
    struct TOPO_SOCKMETA6* v6_sock_meta;
    struct TOPO_ADDR6*     v6_addrs;
    struct TOPO_REALITY_SOCK* reality_socks;   // reality-камуфляж сокетов (per-socket, значения на узел)
};

struct TOPO_NODESUBNETS {
    struct TOPO_SUBNET4* v4_subnets;
    struct TOPO_SUBNET6* v6_subnets;
};

/* Идентичность перехода BGP. На проводе только Ed25519; id вычислен из X25519 для интерфейса и метрик. */
struct topo_hop {
    uint64_t id;
    uint8_t ed[SC_PUBKEY_SIZE];
};

struct TOPO_NODEPATH {
    struct ll_entry ll;
    struct ETCP_CONN* conn;       // заимствованный next-hop; транспорт удерживает сессия
    uint8_t  hop_count;           // число элементов hops
    uint16_t cumulative_rtt;     // RTT оставшейся цепочки, без локального линка (0.1 ms)
    uint64_t exchange;           // local_epoch сессии, в которой путь подтверждён NODEINFO
    struct topo_hop hops[];      // от назначения до непосредственного отправителя
};

/* Средний RTT выбранного пути для интерфейсов; обновляется событиями, без таймера и SQL. */
struct topo_rtt_average {
    uint64_t sum, count;          /* Сумма RTT в 0.1 мс и число измерений текущего окна. count=0 — нет состояния. */
    uint64_t start_tb, last_tb;   /* Начало окна и последнее измерение, монотонные часы 0.1 мс. */
    struct ETCP_CONN* conn;      /* Заимствованная идентичность соединения; не разыменовывается. */
    uint64_t conn_id, local_epoch, peer_epoch; /* Различают новый объект/сессию при прежней цепочке. */
    struct topo_hop hops[MAX_HOPS];      /* Копия выбранной цепочки: смена маршрута сбрасывает накопление. */
    uint8_t hop_count;           /* Число элементов hops. */
    uint16_t cumulative_rtt;     /* Последняя учтённая оценка оставшейся цепочки, 0.1 мс. */
    uint16_t published;          /* Показанное среднее, 0.1 мс; до завершения первого окна — первый замер. */
};

/** Узел группы: удалённый хранится в TOPO_GROUP->nodes (ll_queue), локальный — отдельно в local_node. */
struct TOPO_GROUP_NODE {
    struct ll_entry ll;                                       // queue entry, хеш-ключ 8 байт → node_id
    uint64_t node_id;                                         // идентификатор узла в этой группе
    uint8_t ed25519[SC_PUBKEY_SIZE];                          // полный адрес, полученный из проверенной идентичности
    struct TOPO_NODE* identity;                               // владеющая ссылка на точную запись общего реестра
    uint32_t short31;                                         // младшие 31 бита первых 4 байтов SHA256(ed25519) в big-endian
    uint8_t address_indexed, short_ambiguous;                 // в индексе группы; short31 занят несколькими ключами
    struct TOPO_GROUP_NODE* address_next;                     // следующий узел в корзине индекса short31
    struct TOPO_NODESUBNETS* subnets;                         // подсети узла (v4/v6), для route_insert()
    struct ll_queue* paths;                                   // TOPO_NODEPATH{conn,hop_count,rtt} — маршруты
    struct topo_rtt_average rtt_average;                      // средний RTT пути за 300 мс для desktop/Android
    uint8_t last_ver;                                         // диагностический циклический ver; свежесть определяется timestamp
    uint64_t last_timestamp;                                  // последняя принятая версия записи в группе
    uint8_t conn_mgr_type;                                    // CONN_TYPE_DIRECT/REVERSE/INDIRECT/NONE
    uint8_t conn_mgr_intermediaries[CONN_MGR_MAX_INTERMEDIARIES][SC_PUBKEY_SIZE]; // Ed25519 посредников для INDIRECT
    uint8_t conn_mgr_intermediariy_count;                     // количество посредников
    struct TOPO_CONNECTIVITY connectivity;                    // результаты ping-проб (interface/nat/real)
    uint8_t conn_presence;                                    // маска NCONN_*: известные виды подключения
    uint8_t conn_up;                                          // маска NCONN_*: работающие виды подключения
    uint8_t radio;                                            // 1 = узел слушает рацию канала (TOPO_FLAG_RADIO)
    uint8_t phone_status;                                     // TOPO_FLAG_PHONE_KNOWN/BUSY; подсказка интерфейсу, не основание для отказа
    struct NODE_CONN_DIRECT* handle;                          // не владеет транспортом; NCD принадлежит сессии группы
};

/* Вычислить short31 для 32 байт Ed25519 в сетевом порядке. 0/-1; ноль допустим как адрес. */
int topo_address_short(const uint8_t ed[SC_PUBKEY_SIZE], uint32_t* short31);
/* Добавить проверенную идентичность узла в индекс группы; short31 получен topo_address_short().
 * Повторный анонс того же узла идемпотентен. Удалить из индекса перед освобождением узла. */
void topo_address_attach(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, const uint8_t ed[SC_PUBKEY_SIZE], uint32_t short31);
void topo_address_detach(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node);
/* Найти активный узел группы по short31; ed=NULL требует однозначного адреса.
 * При заданном ed сверяет все 32 байта. Результат заимствован, неизвестный/неоднозначный адрес даёт NULL. */
struct TOPO_GROUP_NODE* topo_address_find(const struct TOPO_GROUP* group, uint32_t short31, const uint8_t* ed);
/* Найти маршрут к точному Ed25519 среди узлов группы. NULL, если запись отсутствует. */
struct TOPO_GROUP_NODE* topo_node_find_by_ed(const struct TOPO_GROUP* group, const uint8_t ed[SC_PUBKEY_SIZE]);

/* Служебные поля имеют фиксированные 32 байта. Переменное поле содержит short31
 * (4 байта big-endian) либо ровно 0x80 и полный Ed25519. Полный адрес обязателен
 * для открытых src/dst. Сжатие разрешено только для однозначного ключа в своей группе. */
enum topo_address_format { TOPO_ADDRESS_FIXED, TOPO_ADDRESS_FULL, TOPO_ADDRESS_COMPACT };
struct topo_wire_address {
    const uint8_t* ed; /* Заимствован из входного буфера для полного адреса, NULL для short31. */
    uint32_t short31;  /* Для обеих форм; ноль допустим. */
};
/* Записать проверенный ed в buf указанной ёмкости. group нужен для сжатия, NULL даёт полный адрес.
 * Возвращает число записанных байтов или -1, при ошибке буфер сохраняется. */
int topo_address_write(uint8_t* buf, size_t size, const struct TOPO_GROUP* group, const uint8_t ed[SC_PUBKEY_SIZE],
                       enum topo_address_format format);
/* Прочитать одно поле из buf, проверить маркер, длину и каноничность полного ключа.
 * Число потреблённых байтов либо -1. При ошибке out сохраняется. */
int topo_address_read(const uint8_t* buf, size_t size, enum topo_address_format format, struct topo_wire_address* out);

// API — глобальный реестр TOPO_NODE
/* Заимствованный указатель; ref нужен, если запись должна пережить удаление из группы. */
struct TOPO_NODE* topo_node_registry_find(struct TOPO_GROUPS* groups, uint64_t node_id);
/* Найти заимствованную запись по полному X25519 и его node_id. При x=NULL требуется однозначный ID. */
struct TOPO_NODE* topo_node_registry_lookup(struct TOPO_GROUPS* groups, uint64_t node_id, const uint8_t* x);
/* Удержать уже найденную запись реестра. NULL допустим. */
void topo_node_ref(struct TOPO_NODE* node);
/* Снять ссылку на конкретную запись, полученную из реестра. NULL допустим. */
void topo_node_registry_release(struct TOPO_GROUPS* groups, struct TOPO_NODE* node);
/* Принять ni и вернуть запись с одной новой ссылкой. При NULL входной ni остаётся у вызывающего.
 * Более свежий подписанный снимок заменяет содержимое целиком, сохраняя адрес существующего объекта.
 * Старый/равный вход поглощается, возвращается прежняя запись; при двух timestamp=0 сливаются непустые bootstrap-списки.
 * Новая запись может быть заменена более свежим снимком SQLite перед регистрацией. */
struct TOPO_NODE* topo_node_registry_store(struct TOPO_GROUPS* groups, struct TOPO_NODE* ni);
/* Uasync, без SQL: принять уже сверенный с сохранённым снимком узел и вернуть одну registry ref.
 * На успехе ni потребляется (может заменить/дополнить существующий объект); на отказе остаётся caller.
 * Для результата async-загрузки использовать put; обычный store пока синхронно читает прежний snapshot. */
struct TOPO_NODE* topo_node_registry_put(struct TOPO_GROUPS* groups, struct TOPO_NODE* ni);
/* Добавить/снять ссылку; последняя unref удаляет запись из реестра и освобождает её. */
void topo_node_registry_ref(struct TOPO_GROUPS* groups, uint64_t node_id);
void topo_node_registry_unref(struct TOPO_GROUPS* groups, uint64_t node_id);
/* Освободить отдельную запись и её списки; зарегистрированную запись отпускать через unref. */
void topo_node_destroy(struct TOPO_GROUPS* groups, struct TOPO_NODE* ni);

// API — per-group
/* Заимствованный узел группы, NULL если не найден. */
struct TOPO_GROUP_NODE* topo_node_find_by_id(struct TOPO_GROUP* group, uint64_t node_id);
/* Найти узел по полному X25519, включая временную запись до индексации short31. */
struct TOPO_GROUP_NODE* topo_node_find_by_transport(struct TOPO_GROUP* group, uint64_t id, const uint8_t x[32]);
/* Отпустить ссылку реестра и подсети; nq и paths не освобождает. */
void topo_nodeq_free_group_fields(struct TOPO_GROUPS* groups, struct TOPO_GROUP_NODE* nq);
/* Освободить принадлежащие узлу записи путей и их контейнер; ETCP-соединения заимствованы. */
void topo_nodeq_clear_paths(struct TOPO_GROUP_NODE* nq);
/* Полное удаление узла из group->nodes: отменяет пробы, освобождает paths/subnets,
 * убирает из очереди и освобождает nq. local_node освобождает владелец группы отдельно. */
void topo_nodeq_remove_node(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* nq);

/* Размер переменной части NODEINFO или -1 при NULL/счётчиках подсетей без TOPO_FLAG_SEND_SUBNETS. */
int topo_node_dyn_size(const struct TOPOMSG_NODE* msg);
/* Собрать NODEINFO в out[out_max]; длина или -1. Входные структуры остаются у вызывающего.
 * Без TOPO_FLAG_SEND_SUBNETS обе секции подсетей отсутствуют, их счётчики равны 0. */
int topo_node_serialize(struct TOPO_NODE* ni, struct TOPO_GROUP_NODE* nq, uint8_t flags,
                        uint8_t* out, size_t out_max, uint16_t cumulative_rtt);
/* Разобрать ровно len байт NODEINFO; 0/-1. group->instance->topo_groups содержит пулы списков.
 * out_ni обязателен, остальные выходы независимы и необязательны. Отсутствие выхода не меняет обход/проверки секций.
 * Перед разбором все переданные выходы обнуляются, прежние объекты не освобождаются.
 * При успехе node уничтожается topo_node_destroy, подсети возвращаются в пулы groups, hops освобождается u_free.
 * Пустые секции дают NULL/0; при ошибке выходы остаются NULL/0, частичные объекты освобождены.
 * Проверяет размер, профили и /0..32, /0..128; подпись и допустимость BGP-пути проверяет вызывающий. */
int topo_node_deserialize(struct TOPO_GROUP* group, const uint8_t* data, size_t len, struct TOPO_NODE** out_ni, struct TOPO_NODESUBNETS** out_subnets,
                          struct topo_hop** out_hop_list, uint8_t* out_hop_count,
                          uint16_t* out_cumulative_rtt);
/* Заимствованный список hops выбранного живого пути nq: минимум переходов, затем ID соседа.
 * out_count обязателен; out_rtt необязателен и получает RTT цепочки без локального линка (0.1 ms).
 * Если живого пути нет, возвращает NULL и обнуляет выходные значения. Память списка не освобождать. */
const struct topo_hop* topo_node_best_hop_list(struct TOPO_GROUP_NODE* nq, uint8_t* out_count, uint16_t* out_rtt);
/* Лучший живой путь: минимум переходов, затем ID соседа. Указатель заимствованный. */
struct TOPO_NODEPATH* topo_node_best_path(const struct TOPO_GROUP_NODE* nq);

/* Подготовить и опубликовать local_node с актуальными подсетями/состоянием; число IPv4-подсетей или -1.
 * Учитывает адреса и префиксы, а не только счётчики; CHAT не анонсирует подсети и возвращает 0.
 * При отказе подготовки/подписи или конфликте IPv4 сохраняет прежние local_node и маршруты.
 * Вызывает обновление общей подписанной записи; рассылку NODEINFO выполняет вызывающий. */
int topo_group_update_my_nodeinfo(struct UTUN_INSTANCE* instance, struct TOPO_GROUP* group);
/* Создать подписанную локальную идентичность с отдельной ссылкой ядра. 0/-1. */
int topo_node_init_self(struct UTUN_INSTANCE* instance);
/* Пересобрать общие адреса из рабочих UDP/TCP-сокетов; 1=изменились, 0=прежние, -1=ошибка.
 * Новая запись подписывается/сохраняется перед заменой, затем рассылается во все группы.
 * Ошибка SQL-проекции даёт -1 и после применённого изменения; при неизменных адресах проекция также обновляется. */
int  topo_node_update_my_addresses(struct UTUN_INSTANCE* instance);
/* Обработчик изменений локального сокета: обновляет адреса узла. */
void topo_node_on_socket_changed(struct ETCP_SOCKET* sock, int event, void* arg);
/* Вывести group->nodes в лог категории BGP на уровне INFO; отдельный local_node не добавляется. */
void topo_node_dump_all(struct TOPO_GROUP* group);
/* Записать дамп в buf: длина без NUL или -1 при ошибке форматирования. Ёмкость ограничена INT_MAX.
 * При усечении завершает запись сразу, заменяет хвост доступной частью "\n[TRUNCATED]\n" и возвращает реальную длину.
 * При buf != NULL и buf_size > 0 строка всегда завершена NUL. Некорректные аргументы дают 0 и пустую строку, если есть буфер. */
int  topo_node_format_all(struct TOPO_GROUP* group, char* buf, size_t buf_size);
/* 1, если хотя бы в одной группе пора повторить ping этого узла; иначе 0. */
int  topo_node_ping_request_cbk(struct TOPO_GROUPS* groups, uint64_t node_id);
/* Uasync: обновить время ping/минимальные RTT проб во всех группах и уведомить подписчиков.
 * Только runtime, без SQL; rtt в единицах 0.1 ms. Сохранение выполняет topo_node_rtt. */
void topo_node_ping_update_rtt(struct TOPO_GROUPS* groups, const uint8_t x[32], uint16_t rtt);
/* Суммарный RTT выбранного живого пути nq: RTT локального соединения + RTT оставшейся цепочки (0.1 ms).
 * Путь выбирается по числу переходов и ID соседа. UINT16_MAX — нет пути, неизвестен локальный RTT или сумма насыщена. */
uint16_t topo_get_chain_rtt(struct TOPO_GROUP_NODE* nq);
/* Опубликованное среднее RTT выбранного пути за 300 мс, 0.1 мс. Только uasync, чтение не добавляет замеров.
 * Без накопления/после смены пути возвращает первый текущий RTT; неизвестность — UINT16_MAX, как у мгновенного RTT. */
uint16_t topo_get_chain_rtt_avg(struct TOPO_GROUP_NODE* nq);

/* Лимиты буфера подписываемых данных и полного сетевого NODEINFO. */
#define TOPO_SIG_MSG_MAX_SIZE 8192
#define TOPO_NODE_WIRE_MAX_SIZE 16384
/* Канонические байты: node_id(native), ver, timestamp(BE), X25519-ключ, длина/строка connection_order, имя до 63 байт + NUL,
 * тип/активность, пять счётчиков, SOCKMETA4/ADDR4/SOCKMETA6/ADDR6/REALITY. Порты здесь LE, SNI с NUL.
 * Групповые flags/id, подсети, hops и RTT не подписываются. Длина или -1, буфер при ошибке может быть заполнен частично. */
int topo_node_build_sig_msg(struct TOPO_NODE* ni, uint8_t* buf, size_t buf_size);

/* Подготовить подпись локального NODEINFO; RAM сохраняет синхронно, RW ставит frozen snapshot в очередь.
 * 0=принято/no-op,-1=отказ с восстановлением timestamp/подписи. RW registry обновляется только после durable commit;
 * guard может освежить timestamp и committed name/devgroup перед dispatch. Кандидат не публиковать вручную до done.
 * Остальные изменённые caller поля при ошибке не откатываются. */
int topo_node_sign_self(struct UTUN_INSTANCE* instance, struct TOPO_NODE* ni);
/* Uasync: опубликовать статус телефона и подписку рации каждой CHAT-группы с новой версией NODEINFO.
 * При RW SQLite рассылка следует за commit; повторный вызов при прежнем статусе бесплатен. */
void topo_node_update_chat_status(struct UTUN_INSTANCE* instance);
/* Скопировать только самостоятельные данные node (без pool/ref/runtime связей), в том числе списки.
 * Результат пригоден для SQL worker; NULL при аргументах/OOM. Не регистрировать в runtime/pool.
 * node_name у копии — собственный буфер64 байта. Передавать владение между потоками, не использовать одновременно. */
struct TOPO_NODE* topo_node_freeze(const struct TOPO_NODE* node);
/* Освободить только результат freeze; NULL допустим. Сигнатура пригодна для db_async cleanup. */
void topo_node_frozen_free(void* node);
/* Uasync: заранее создать проверенную pool-копию signed node для публикации после commit.
 * Возврат незарегистрированного узла либо NULL; освобождать topo_node_destroy(groups,node).
 * Не выполняет SQL; заранее резервирует все списки/имя, worker не получает эту копию. */
struct TOPO_NODE* topo_node_clone(struct TOPO_GROUPS* groups, const struct TOPO_NODE* node);
/* Uasync после durable commit: установить заранее подготовленный self snapshot и разослать NODEINFO.
 * SQL/аллокаций нет; own registry уже должен существовать.0 принимает владение prepared,
 * -1 оставляет его caller. Обновляет committed/reserved timestamp и local_node всех групп. */
int topo_node_publish_self(struct UTUN_INSTANCE* instance, struct TOPO_NODE* prepared);
/* Uasync: заморозить подписанный узел и сохранить snapshot/адреса/классификацию одной WRITE-работой.
 * channel_id необязателен: для CHAT добавляет только неподписанный BGP-плейсхолдер.
 * from — необязательное соединение, передавшее NODEINFO. Для диагностики копируются его публичный ключ и node_id.
 * Входы копируются до возврата;0=принято,-1=отказ с логом. Успех очереди ещё не означает commit.
 * Собственный NODEINFO разрешается рассылать только после local_snapshot_committed; done запускает рассылку.
 * RAM-fixtures без worker используют синхронную запись; файловая БД требует RW worker. */
int topo_node_persist(struct UTUN_INSTANCE* instance, const struct TOPO_NODE* node, const char* channel_id, const struct ETCP_CONN* from);
/* Uasync: асинхронно пересчитать SQL-классификацию node_id после смены связи/результатов проб.
 * Runtime не передаётся worker;0=принято,-1=отказ с логом. Нет frontend-события до durable commit. */
int topo_node_mark_dirty(struct UTUN_INSTANCE* instance, const uint8_t x[32]);
/* Проверить 0 < timestamp <= INT64_MAX, node_id=derive(X25519) и подпись предоставленным Ed25519-ключом. 0/-1. */
int topo_node_verify(const struct TOPO_NODE* ni);

/* Заимствованные списки: ni обязателен; подсетевые аксессоры допускают r=NULL. */
static inline const struct TOPO_SOCKMETA4* topo_v4_sock_meta(const struct TOPO_NODE* ni) { return ni->v4_sock_meta; }
static inline const struct TOPO_ADDR4*     topo_v4_addrs(const struct TOPO_NODE* ni)      { return ni->v4_addrs; }
static inline const struct TOPO_SOCKMETA6* topo_v6_sock_meta(const struct TOPO_NODE* ni) { return ni->v6_sock_meta; }
static inline const struct TOPO_ADDR6*     topo_v6_addrs(const struct TOPO_NODE* ni)      { return ni->v6_addrs; }
static inline const struct TOPO_SUBNET4*   topo_v4_subnets(const struct TOPO_NODESUBNETS* r) { return r ? r->v4_subnets : NULL; }
static inline const struct TOPO_SUBNET6*   topo_v6_subnets(const struct TOPO_NODESUBNETS* r) { return r ? r->v6_subnets : NULL; }

/* Найти reality-инфо сокета по socket_id (NULL, если сокет без reality). */
const struct TOPO_REALITY_SOCK* topo_node_find_reality_sock(const struct TOPO_NODE* ni, uint8_t socket_id);

/* Скопировать REALITY-параметры пира rs в cfg и задать fingerprint OPENSSL; оба указателя обязательны. Возврата нет. */
void topo_node_reality_config(struct reality_client_config *cfg, const struct TOPO_REALITY_SOCK *rs);
/* Заполнить link->reality из rs и установить link->reality_set=1; при NULL аргументе ничего не делает. Возврата нет. */
void topo_node_apply_reality(struct ETCP_LINK* link, const struct TOPO_REALITY_SOCK* rs);

#ifdef __cplusplus
}
#endif
#endif
