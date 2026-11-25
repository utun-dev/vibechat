/* eim_nat — таблица Endpoint-Independent Mapping для IPv4 TCP/UDP и ICMP echo.
 * Ключ mapping: (Ed25519, протокол, внутренний IP, внутренний порт/echo ID), без внешнего адресата.
 * Исходящий поиск — хеш, в среднем O(1); входящий — table[port], O(1); свободные порты — двусвязный FIFO, O(1).
 * Таблица и 65536 uint16_t-корзин выделяются только при init; обработка/создание mapping не выделяют память.
 * init_ctx создаёт состояние из global_config; при отключённом NAT оставляет initialized=0.
 * egress/ingress меняют IP/порты и checksums на месте, не отправляют пакеты и не владеют TUN/ETCP.
 * IPv4 UDP: отключённая checksum=0 сохраняется; вычисленный ноль записывается как 0xffff.
 * Перед обработкой нужен инициализированный ctx; вызовы последовательны в uasync-потоке.
 * gateway/internal IP — host order, internal_port — network order, внешний порт — host order.
 * Динамические записи истекают по idle-timeout; владелец регулярно вызывает expire с ограниченным budget.
 * Исходящие/входящие пакеты обновляют last_seen. STATIC и временно удержанные записи не истекают.
 * Фрагменты и неподдерживаемые протоколы возвращаются без изменений с EIM_NAT_SKIP.
 * src_conn только сохраняется при создании динамической записи, для отправки не используется. */
#ifndef EIM_NAT_H
#define EIM_NAT_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>

struct ETCP_CONN;

#define EIM_NAT_TABLE_SIZE   65536
#define EIM_NAT_OK            0  // Пакет преобразован.
#define EIM_NAT_SKIP          1  // Фрагмент, неподдерживаемый протокол или отсутствует mapping; пакет не изменён.
#define EIM_NAT_ERROR        -1  // Некорректный пакет/контекст или исчерпан пул; пакет не изменён.
#define EIM_NAT_TCP_IDLE_TB  (7440ULL * 10000) // Консервативный TCP idle timeout без отслеживания TCP-состояния (RFC 5382).
#define EIM_NAT_UDP_IDLE_TB  (120ULL * 10000)  // RFC 4787: не менее двух минут.
#define EIM_NAT_ICMP_IDLE_TB (60ULL * 10000)   // RFC 5508: не менее минуты.

#define EIM_NAT_ENTRY_FREE   0
#define EIM_NAT_ENTRY_ACTIVE 1
#define EIM_NAT_ENTRY_STATIC 2

#define IPPROTO_TCP_UINT8    6
#define IPPROTO_UDP_UINT8    17
#define IPPROTO_ICMP_UINT8   1

struct eim_nat_entry {
    uint32_t internal_ip;       // Внутренний IPv4, host order.
    uint16_t internal_port;     // Внутренний TCP/UDP-порт или ICMP echo ID, network order.
    uint8_t  proto;             // IPPROTO_*_UINT8.
    uint8_t  state;             // EIM_NAT_ENTRY_FREE / ACTIVE / STATIC.
    uint8_t src_ed[32];         // Полный адрес владельца mapping для обратной доставки.
    uint64_t last_seen;         // Монотонное время последнего пакета в любом направлении, единицы 0.1 мс.
    struct ETCP_CONN* src_conn; // Заимствованная метка исходного conn, может быть NULL; модуль её не разыменовывает.
    uint16_t hash_next;         // Внутренняя цепочка корзины; 0 — конец. Ключи и состояние извне не менять.
    uint16_t free_prev, free_next; // Внутренний список свободных динамических портов; 0 — конец.
    uint16_t holds;             // Временные удержания уже преобразованных исходящих пакетов до записи в TUN.
};

// Состояние преобразования; TUN и транспортные ресурсы принадлежат nat_transport.
struct eim_nat_ctx {
    uint32_t gateway_ip; // Внешний IPv4 NAT, host order; nat_tun_ip, иначе tun_ip.
    uint16_t port_start; // Начало динамического диапазона включительно; 1 <= port_start < port_end.
    uint16_t port_end;   // Конец диапазона включительно; допускается 65535.
    uint16_t next_port; // Голова списка свободных портов; 0 — пул исчерпан.
    uint16_t free_tail; // Хвост: освобождённые порты возвращаются сюда.
    uint32_t free_count; // Число свободных динамических портов.
    uint32_t gc_cursor; // Следующий слот порционной очистки, 1..65535.
    struct eim_nat_entry* table; // Владеет table_size записями; индекс — внешний порт/echo ID для всех протоколов вместе.
    uint16_t* buckets;  // 65536 голов хеш-цепочек полного внутреннего ключа, 0 — пустая корзина.
    size_t table_size;  // EIM_NAT_TABLE_SIZE при успешной инициализации.
    int initialized;   // 1 — таблица создана; 0 — NAT отключён, не создан или уничтожен.
};

struct global_config;

/* Проверить IPv4 и транспортные границы без изменения данных. header_len обязателен и получает IHL в байтах.
 * EIM_NAT_OK — поддерживаемый полный пакет; SKIP — фрагмент/неподдерживаемый протокол; ERROR — неверный формат.
 * Checksums не проверяются. Не требует созданного NAT ctx. */
int eim_nat_validate_packet(const uint8_t* ip, size_t len, uint16_t* header_len);

/* Создать ctx по g: IPv4 шлюза, 1 <= port_start < port_end <= 65535, статические пробросы.
 * ctx не должен владеть предыдущей таблицей: перед повторным init вызвать destroy_ctx.
 * 0 — успех (при !nat_enabled таблица не создаётся); -1 — ошибка, созданная таблица освобождена. */
int  eim_nat_init_ctx(struct eim_nat_ctx* ctx, const struct global_config* g);
/* Освободить таблицу и обнулить созданный ctx; NULL и повторное уничтожение допустимы. */
void eim_nat_destroy_ctx(struct eim_nat_ctx* ctx);

/* Изменить source IP и порт/echo ID в ip_data (ip_len байтов IPv4), выделив/переиспользовав mapping.
 * Ed25519 — владелец записи; src_conn — необязательная заимствованная метка только для новой ACTIVE-записи.
 * Контрольные суммы должны быть корректны до вызова: модуль их обновляет, но не проверяет.
 * ctx создан, ip_data — полный доступный для записи IPv4-пакет; буфер остаётся у вызывающего.
 * Возврат EIM_NAT_OK / SKIP / ERROR. Проверяются версия, IHL, total_length и границы транспортного заголовка. */
int  eim_nat_egress(struct eim_nat_ctx* ctx, uint8_t* ip_data, size_t ip_len,
                     const uint8_t ed[32], struct ETCP_CONN* src_conn);
/* Для пакета на gateway_ip найти запись по внешнему порту/echo ID и протоколу, восстановить destination.
 * ctx/ip_data/checksum — как у egress. out_entry необязателен, перед обработкой обнуляется.
 * Указатель заимствован до remove/expiry/destroy; для ожидания сохранить Ed25519/преобразованный пакет или вызвать hold.
 * При EIM_NAT_OK получает заимствованную запись; иначе остаётся NULL. Возврат EIM_NAT_OK / SKIP / ERROR. */
int  eim_nat_ingress(struct eim_nat_ctx* ctx, uint8_t* ip_data, size_t ip_len,
                      struct eim_nat_entry** out_entry);

/* Добавить TCP/UDP-проброс на ненулевой Ed25519: internal_ip_host — IPv4 host order,
 * internal_port_net — ненулевой порт network order; external_port — 1..65535 host order,
 * включая порты вне динамического диапазона. Ответ владельца использует тот же внешний порт.
 * 0 — добавлен; -1 — ctx не создан, неверные аргументы, занят внешний порт или уже есть такой внутренний ключ. */
int  eim_nat_add_forward(struct eim_nat_ctx* ctx, uint8_t proto,
                          uint32_t internal_ip_host, uint16_t internal_port_net,
                         uint16_t external_port, const uint8_t ed[32]);

/* Удалить запись по внешнему порту из всех индексов; порт из динамического диапазона вернуть в пул.
 * 1 — удалена, 0 — уже свободна, -1 — неверный ctx/порт, удержанная запись или нарушен индекс. Включая STATIC-записи.
 * Все ранее полученные указатели на эту запись теряют смысл: слот может быть переиспользован. */
int eim_nat_remove(struct eim_nat_ctx* ctx, uint16_t external_port);
/* Проверить не более budget слотов начиная с gc_cursor; now_tb — get_time_tb(), не календарное время.
 * Возврат — число удалённых ACTIVE-записей; STATIC не истекают. Очистка не выполняется из обработки пакета.
 * budget ограничивается 65535. NULL/неинициализированный ctx — 0. */
size_t eim_nat_expire(struct eim_nat_ctx* ctx, uint64_t now_tb, size_t budget);
/* Удержать существующий mapping на время асинхронной отправки; удаление/expiry запрещены до release.
 * release с refresh != 0 обновляет last_seen при фактической отправке; при отбрасывании refresh=0.
 * 0 — успех, -1 — неверная/свободная запись, переполнение holds или непарный release. */
int eim_nat_hold(struct eim_nat_ctx* ctx, uint16_t external_port);
int eim_nat_release(struct eim_nat_ctx* ctx, uint16_t external_port, int refresh);


#ifdef __cplusplus
}
#endif
#endif
