/*
 * node_conn_direct (NCD) — прямой ETCP-транспорт к узлу по его ключу и сетевым адресам.
 * Порядок: open → дождаться UP → get_conn → close. Каждый владелец открывает свой handle.
 * Несколько handles делят один ETCP_CONN; close снимает только своё владение.
 * NCD создаёт и обновляет UDP/STCP-линки, но не проверяет членство и готовность topo-группы.
 * Для групповой сессии используйте topo_group_peer_open, для звонка/файла и выбора пути — conn_mgr_open.
 * Все операции — в потоке uasync. Диагностика — категория etcp, подробности на debug.
 */

/* Владение транспортом:
 * Группа держит NCD с начала CONNECTING; отдельные handles могут принадлежать CM и протоколу join.
 * Временный handle закрывают либо передают новому владельцу через transfer.
 * Потребность сервиса в доставке на всё время звонка/файла выражает CONN_MGR_HANDLE (conn_mgr.h).
 * Последний handle работающего транспорта начинает CLOSE/KEEP_ALIVE (5 с либо срок пробуждения пира);
 * Закрытие последнего handle до первого UP или после TIMEOUT удаляет транспорт сразу;
 * ранее работавший DOWN-транспорт остаётся до deadline ETCP.
 * Переходы UP/DOWN/TIMEOUT/CLOSED доставляются синхронно. Закрытие любого handle
 * из callback безопасно. Начальное UP при open готового conn откладывается через uasync_call_soon.
 * Вложенный переход отменяет оставшуюся рассылку прежнего события. После close указатель handle недействителен.
 * CLOSED отменяет таймеры и отсоединяет conn; handle всё равно закрывает владелец, в том числе после destroy ядра.
 */
#ifndef NODE_CONN_DIRECT_H
#define NODE_CONN_DIRECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
struct ETCP_CONN;
struct TOPO_NODE;
struct ETCP_SOCKET;

// Привязать CLOSE/KEEP_ALIVE при запуске ядра, в том числе без исходящих handles; 0 — успех, -1 — ошибка.
int node_conn_direct_init(struct UTUN_INSTANCE* inst);

// Число локальных владельцев конкретного транспорта; внешние ETCP refs сюда не входят.
int node_conn_direct_handle_count(const struct ETCP_CONN* conn);

// ─── Return codes ───
#define NCD_NEW        0  /* создано новое соединение */
#define NCD_REUSED     1  /* переиспользовано существующее */
#define NCD_ERR       -1  /* ошибка */

// ─── Opaque handle ───
struct NODE_CONN_DIRECT;

/* Рабочий ETCP, совместимая исходящая пара либо возможность reverse на наш опубликованный адрес.
 * Без линков/handles/таймеров/проб. Исходящие пары — тот же обход, что open; reverse — sock_match/публикуемые адреса.
 * Учитывает IPv6 и исходящий TCP fallback; 1 — кандидат есть, 0 — нет/ошибка. Обычный snapshot ищется только в registry.
 * Для подтверждённого LAN-кандидата проверяет сокеты по node_id без SQL и без регистрации.
 * Наличие/ключи идентичности проверяются при открытии транспорта, не в проверке возможности пары адресов.
 * LAN TCP соблюдает глобальную SOCKS-политику: при включённом SOCKS прямые LAN TCP-пробы не создаются.
 * Возможность попытки не доказывает достижимость и не подтверждает готовность группы. */
int node_conn_direct_compatible(struct UTUN_INSTANCE* inst, uint64_t node_id);
/* Та же проверка прямого пути для точного Ed25519. */
int node_conn_direct_compatible_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32]);

// ─── Callback events (единый callback на handle) ───
enum ncd_event {
    NCD_EVENT_UP      = 0,  /* соединение поднялось (линки работают) */
    NCD_EVENT_DOWN    = 1,  /* соединение упало                   */
    NCD_EVENT_CLOSED  = 3,  /* транспорт окончательно удалён; get_conn=NULL, владелец закрывает handle */
    NCD_EVENT_TIMEOUT = 2,  /* срок первой попытки истёк; удерживаемый handle может получить поздний UP */
};

typedef void (*ncd_callback)(struct NODE_CONN_DIRECT* h, enum ncd_event event, void* arg);

/*
 * Открыть handle к узлу по node_id.
 * Node info ищется через node_registry, fallback — SQLite.
 * Файловый fallback выполняется worker: handle уже принят, get_conn может быть NULL до завершения SQL.
 * Close отменяет ожидание; ошибка SQL/несовместимый сохранённый снимок завершаются CLOSED после возврата open.
 * cb/arg — callback и его контекст (cb=NULL допустим). Новый handle получает события только после возврата open.
 * Если conn уже готов — cb(NCD_EVENT_UP) через uasync_call_soon (не синхронно).
 * До первой отправки дождаться UP. Результат: NCD_NEW/NCD_REUSED или NCD_ERR.
 * При успехе вызывающий обязан закрыть *out_handle, в том числе после CLOSED.
 * out_handle обязателен; при ошибке *out_handle=NULL, старые handles и FIN_WAIT сохраняются.
 * Принятый snapshot и уже добавленные линки могут сохраниться при частичной ошибке обновления.
 * specific_sock=NULL выбирает все разрешённые пары; иначе нужен обычный сокет этого instance.
 * Явный сокет задаёт локальную политику, но сохраняются проверки family, протокола, target и REALITY.
 */
int node_conn_direct_open(struct UTUN_INSTANCE* inst, uint64_t node_id,
                          ncd_callback cb, void* cb_arg,
                          struct NODE_CONN_DIRECT** out_handle,
                          struct ETCP_SOCKET* specific_sock); /* NULL — авто-подбор всех совместимых сокетов для линков */

/* Подготовить владельца [client] по ключу identity. Линки создаются после публикации конфига.
 * identity заимствован до возврата. Возвращает NCD_NEW/NCD_REUSED/NCD_ERR как open. */
int node_conn_direct_open_config(struct UTUN_INSTANCE* inst, struct TOPO_NODE* identity, ncd_callback cb, void* cb_arg,
                                 struct NODE_CONN_DIRECT** out_handle);

/* 1, если для пира есть явные link= или подготовлен их владелец. UTUN может сразу получить групповой handle. */
int node_conn_direct_configured(struct UTUN_INSTANCE* inst, uint64_t node_id);

/* Применить опубликованные link= к существующему транспорту пира, закрыв устаревшие исходящие линки. */
void node_conn_direct_config_changed(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t key[32]);

/* Открыть прямой транспорт для точного X25519, включая асинхронную загрузку адресов.
 * Аргументы и результат как у node_conn_direct_open; key=NULL требует однозначного node_id. */
int node_conn_direct_open_key(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* key, ncd_callback cb, void* cb_arg,
                              struct NODE_CONN_DIRECT** out_handle, struct ETCP_SOCKET* specific_sock);

/* Взять handle конкретного текущего транспорта ETCP, включая CONNECTING.
 * Возврат 0/-1, при ошибке *out_handle=NULL. Не создаёт соединение. Освободить через node_conn_direct_close. */
int node_conn_direct_retain(struct ETCP_CONN* conn, struct NODE_CONN_DIRECT** out_handle);

/* Удержать уже аутентифицированный UP conn отдельным владельцем; 0/-1, при ошибке *out_handle=NULL.
 * Не ищет адреса, не создаёт линки и не вызывает callbacks владельцев. Нужен текущий conn из индекса instance.
 * Освободить через node_conn_direct_close; владение не даёт прав CHAT и не мешает физическому DOWN/DELETE. */
int node_conn_direct_hold(struct ETCP_CONN* conn, struct NODE_CONN_DIRECT** out_handle);

/*
 * То же, но с уже загруженным TOPO_NODE (ni не владеем — можно передать временный на стеке).
 * ni заимствован до возврата; node_id должен совпадать с ni->node_id и значением, производным из public_key.
 * При холодной файловой БД входной снимок копируется до возврата; сохранённая политика проверяется асинхронно.
 * timestamp>0 требует верной подписи: более свежая запись принимается целиком, старая уступает известной,
 * одинаковый timestamp с другой подписью отклоняется. timestamp=0 — bootstrap, он не заменяет подписанную запись.
 * Адреса могут отсутствовать: handle ожидает также входящего подключения. Обход адресов использует отдельную копию.
 */
int node_conn_direct_open_node(struct UTUN_INSTANCE* inst, uint64_t node_id,
                               ncd_callback cb, void* cb_arg,
                               struct NODE_CONN_DIRECT** out_handle,
                               struct TOPO_NODE* ni,                  /* публичный ключ + адреса пира (не владеем) */
                               struct ETCP_SOCKET* specific_sock);    /* NULL — авто-подбор                     */

// Освободить handle и отменить его callbacks; другие владельцы сохраняют транспорт.
void node_conn_direct_close(struct NODE_CONN_DIRECT* h);

/* Применить принятую запись из общего реестра к уже существующему соединению.
 * Не создаёт соединение и не приобретает handle. Число новых линков, 0 при отсутствии записи/транспорта, -1 при ошибке.
 * Добавляет недостающие пары и обновляет TCP/REALITY; работающие линки по исчезнувшим адресам сохраняются. */
int node_conn_direct_update_node(struct UTUN_INSTANCE* inst, uint64_t node_id);
/* Точный вариант для NODEINFO/CONN_MGR с проверенным публичным X25519. */
int node_conn_direct_update_node_key(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x);
/* Применить текущие nonce-подтверждённые LAN-адреса к существующему NCD вне FIN_WAIT.
 * Не создаёт транспорт/handle, не меняет snapshot. Число новых линков либо -1; без транспорта — 0.
 * Неподтверждённые ETCP-пробы удаляются после истечения LAN-кандидата; работающие линки сохраняются. */
int node_conn_direct_update_local(struct UTUN_INSTANCE* inst, const uint8_t x[32]);

/* Как close, но последний handle закрывает транспорт сразу, без fin_wait.
 * При наличии других handle общий транспорт и их callbacks сохраняются. */
void node_conn_direct_force_close(struct NODE_CONN_DIRECT* h);

/* Сменить или сбросить (cb=NULL) callback на уже открытом handle.
 * Не влияет на refcounting и состояние conn, не повторяет уже доставленное событие.
 * Ещё не доставленный начальный UP использует новый callback/arg. */
void node_conn_direct_set_callback(struct NODE_CONN_DIRECT* h, ncd_callback cb, void* cb_arg);

/* Передать владение handle новому владельцу (смена callback + arg).
 * После вызова handle принадлежит new_cb_arg, прежний владелец
 * должен забыть указатель. Соединение не трогается. */
void node_conn_direct_transfer(struct NODE_CONN_DIRECT* h, ncd_callback new_cb, void* new_cb_arg);

// Идентификатор узла; 0 для NULL.
uint64_t node_conn_direct_node_id(const struct NODE_CONN_DIRECT* h);

// Заимствованный транспорт, в том числе до UP; NULL после CLOSED. Не закрывать напрямую.
struct ETCP_CONN* node_conn_direct_get_conn(struct NODE_CONN_DIRECT* h);
/* Публичный X25519 ожидаемого или подключённого пира, заимствован до close; NULL пока ключ неизвестен. */
const uint8_t* node_conn_direct_get_key(const struct NODE_CONN_DIRECT* h);

// Прямой ETCP control: ровно 34 байта: cmd=0x12, subcmd, полный публичный Ed25519 отправителя из INIT.

#define NCD_SUBCMD_CLOSE      0x01
#define NCD_SUBCMD_KEEP_ALIVE 0x02

#pragma pack(push, 1)
struct ncd_control_msg {
    uint8_t  cmd;       /* ETCP_ID_NCD_CONTROL */
    uint8_t  subcmd;    /* NCD_SUBCMD_CLOSE / KEEP_ALIVE */
    uint8_t ed25519[32]; /* Сверяется с идентичностью из INIT текущего прямого транспорта. */
};
#pragma pack(pop)

#define NCD_CONTROL_MSG_SIZE sizeof(struct ncd_control_msg)

// ─── Управление линками на конкретном сокете ───
/*
 * ncd_add_socket_links  — итерирует все активные NCD-соединения (ncd_registry),
 * кроме FIN_WAIT; загружает TOPO_NODE и добавляет все разрешённые пары на новом сокете.
 * Использует правила open(NULL): family/UDP/TCP, локальная политика, NAT-профиль, REALITY, IPv6 scope.
 * sock — обычный зарегистрированный сокет inst, заимствован до возврата; callbacks не должны удалять его.
 * Возвращает число новых линков, 0 если нечего добавить, -1 при ошибке (частичные добавления сохраняются).
 */
int ncd_add_socket_links(struct UTUN_INSTANCE* inst, struct ETCP_SOCKET* sock);

/*
 * ncd_remove_socket_links — итерирует все активные NCD-соединения,
 * для каждого закрывает линки (etcp_link_close) привязанные к переданному сокету.
 * Вызывается перед удалением сокета; sock заимствован до возврата. Сам сокет не удаляет.
 * Возвращает число линков, закрытых этим вызовом, или -1 при отказе памяти. NULL inst/sock — 0.
 * Вложенные callbacks могут закрывать другие handles/линки. Транспорт вне NCD удаляет сам etcp_socket_remove.
 */
int ncd_remove_socket_links(struct UTUN_INSTANCE* inst, struct ETCP_SOCKET* sock);

#ifdef __cplusplus
}
#endif

#endif
