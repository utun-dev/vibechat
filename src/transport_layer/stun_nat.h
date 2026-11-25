/* Определяет поведение NAT через внешние STUN-серверы: совпадает ли внешний IP:порт
 * при обращении к разным IP и доступны ли входящие без предварительного контакта.
 * mapping: MATCH — два сервера видят одинаковый адрес; DEPENDENT — разные;
 * RFC 5780 уточняет зависимость mapping: EIM — не зависит от назначения, ADM — от IP, APDM — от IP:порта.
 * filtering: EIF — незнакомый вход разрешён, ADF — нужен контакт с IP, APDF — с IP:портом. UNKNOWN — данных мало.
 * Для работы: [stun] enabled=yes и минимум две строки server=host[:port] с разными IP.
 * Ядро вызывает init(instance)/destroy(instance), модуль сам измеряет и обновляет результат.
 * get_socket_result(instance, socket, &result) возвращает снимок; recheck(instance) повторяет проверку.
 * nat_detection использует совпавший mapping как предварительный EIM? для рабочих UDP-сокетов
 * с теми же IP, интерфейсом и routing options; измерение рабочего сокета имеет приоритет.
 * Проверка идёт с отдельного тестового порта: его внешний IP:порт не публикуется как адрес рабочего сокета,
 * а filtering не переносится на рабочий сокет. Без RFC 5780 входящие остаются UNKNOWN.
 * Все вызовы — в uasync-потоке ядра; destroy отменяет DNS/таймеры до освобождения config.
 * TCP/probe_only/SOCKS/only_local исключаются из API; IPv6 и отсутствие IPv4 дают снимок SKIPPED. */
#ifndef STUN_NAT_H
#define STUN_NAT_H

#include "../lib/socket_compat.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
struct STUN_NAT;
struct ETCP_SOCKET;

#define STUN_NAT_TTL_TB UINT64_C(3000000) /* Результат действителен 300 секунд; единица — 100 мкс. */

/* Ключ пути не содержит порт/имя рабочего сокета. family — 4 или 6. */
struct stun_nat_key {
    uint8_t family; // 4/6, 0 если адрес пути ещё неизвестен
    uint8_t address[16]; // сетевой порядок; у IPv4 первые 4 байта, остальное 0
    uint32_t netif_index; // индекс исходного интерфейса, 0 если не задан
};

enum stun_nat_status { STUN_NAT_PENDING, STUN_NAT_COMPLETE, STUN_NAT_PARTIAL, STUN_NAT_FAILED, STUN_NAT_SKIPPED };
enum stun_nat_mapping { STUN_MAP_UNKNOWN, STUN_MAP_MATCH, STUN_MAP_DEPENDENT, STUN_MAP_EIM, STUN_MAP_ADM, STUN_MAP_APDM };
enum stun_nat_filtering { STUN_FILTER_UNKNOWN, STUN_FILTER_EIF, STUN_FILTER_ADF, STUN_FILTER_APDF };
enum stun_nat_reason {
    STUN_REASON_NONE, STUN_REASON_UNSUPPORTED, STUN_REASON_TRAFFIC, STUN_REASON_TIMEOUT, STUN_REASON_SERVER_ERROR,
    STUN_REASON_UNSTABLE, STUN_REASON_NO_PAIR, STUN_REASON_DNS, STUN_REASON_IO, STUN_REASON_SOCKET, STUN_REASON_NOMEM,
    STUN_REASON_ROUTE, STUN_REASON_PORTS
};

/* Снимок одного пути IP/netif; несколько рабочих портов используют общее измерение. */
struct stun_nat_result {
    struct stun_nat_key key;                    // локальный IP и интерфейс измерения
    uint64_t generation, evidence_id, measured_at_us; // случайные ненулевые ID пути/измерения и Unix-время результата (мкс)
    unsigned members;                          // число рабочих сокетов этого пути
    int refreshing;                            // идёт обновление, пока возвращается действующий кэш
    enum stun_nat_status status;                // этап/полнота результата; COMPLETE требует пары и обоих свойств
    enum stun_nat_mapping mapping;              // наблюдавшаяся зависимость внешнего endpoint от назначения
    enum stun_nat_filtering filtering;          // входящая доступность независимого тестового порта
    enum stun_nat_reason mapping_reason, filtering_reason; // причины неполного результата по каждому свойству
    struct sockaddr_storage local;               // адрес независимого тестового UDP-сокета
    struct sockaddr_storage servers[2], mapped[2]; // реальные серверы и наблюдаемые внешние адреса
    unsigned server_count;
    int translated;                             // -1 = неизвестно, 0 = нет трансляции, 1 = есть
    int rfc5780;                                // сервер объявил корректный OTHER-ADDRESS
    uint64_t measured_at_tb;                     // локальное монотонное время, 0.1мс
};

/* init требует instance/ua/config даже при выключенном STUN; 0 — запущен/уже запущен/выключен, -1 — ошибка с логом.
 * При включении нужны >=2 server; init создаёт пробные сокеты, результаты поступают асинхронно.
 * destroy отменяет DNS/таймеры и закрывает только собственные UDP-сокеты; NULL/повтор безопасны. */
int stun_nat_init(struct UTUN_INSTANCE* inst);
void stun_nat_destroy(struct UTUN_INSTANCE* inst);
/* История портов живёт до destroy ядра, переживает reload STUN. Вызывать после stun_nat_destroy; NULL безопасен. */
void stun_nat_forget_ports(struct UTUN_INSTANCE* inst);
/* Сверяет состав путей и повторяет измерения с новым generation/портом без прежнего кэша; выключенный модуль/NULL — no-op. */
void stun_nat_recheck(struct UTUN_INSTANCE* inst);
/* Как get_socket_result, но ищет рабочий сокет по NUL-terminated имени. 0 — снимок, -1 — нет/некорректный аргумент. */
int stun_nat_get_result(const struct UTUN_INSTANCE* inst, const char* socket_name, struct stun_nat_result* result);
/* Записывает в result снимок пути socket из instance; 0 — готово, -1 — подходящего пути нет.
 * Проверяет IP, интерфейс и routing options. Во время refresh возвращает действующий кэш с прежним временем.
 * 0 допускает PENDING/FAILED/SKIPPED и устаревший снимок: пригодность/возраст проверяет вызывающий по status/TTL.
 * -1 не меняет result; TCP/probe_only/SOCKS/only_local не имеют результата. Входной socket принадлежит instance. */
int stun_nat_get_socket_result(const struct UTUN_INSTANCE* inst, const struct ETCP_SOCKET* socket, struct stun_nat_result* result);
/* Статические строки диагностики, неизвестное значение даёт INVALID/invalid reason; освобождать не нужно. */
const char* stun_nat_mapping_name(enum stun_nat_mapping value);
const char* stun_nat_filtering_name(enum stun_nat_filtering value);
const char* stun_nat_reason_name(enum stun_nat_reason value);

#ifdef __cplusplus
}
#endif
#endif
