/* route_lib — локальная IPv4-таблица подсетей UTUN, отсортированный массив без пересечения маршрутов.
 * create → insert → lookup → delete/destroy; все операции в одном uasync-потоке.
 * Пересечения и дубликаты запрещены, в том числе внутри одного набора подсетей; /0 занимает всю таблицу.
 * Вставка нормализует адреса по маске; при отказе таблица и её статистика не меняются.
 * network/dest_ip/parse_subnet — host order; только is_local_subnet принимает network order.
 * TOPO_GROUP_NODE заимствован: удалить его маршруты до освобождения узла.
 * lookup возвращает элемент массива, действительный до следующего изменения таблицы. */
#ifndef ROUTE_LIB_H
#define ROUTE_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
// задача модуля: поиск по таблице маршрутизации нужной записи, вставка-удаление маршрутов


// Forward declarations
struct ETCP_CONNECTIONS;
struct ROUTE_TABLE;
struct ROUTE_ENTRY;
struct TOPO_GROUP_NODE;

/**
 * @brief Флаги узла
 */
typedef enum {
    ROUTE_FLAG_ACTIVE     = (1 << 0),  /**< Маршрут активен */
    ROUTE_FLAG_LEARNED    = (1 << 1)   /**< Маршрут изучен */
} route_flags_t;


/**
 * @brief Расширенная запись маршрута
 * 
 * Структура представляет собой отдельную запись в таблице маршрутизации с детальной информацией о маршруте.
 */
struct ROUTE_ENTRY {
    uint32_t network;          // Сетевой адрес в host order
    bool     local;            // Подсеть добавлена владельцем локального узла UTUN
    uint8_t  prefix_length;    // Длина префикса подсети
    struct TOPO_GROUP_NODE* v_node_info; // заимствованный владелец; локальный маршрут тоже содержит узел
};

/**
 * @brief Таблица маршрутизации
 * 
 * Структура представляет собой таблицу маршрутизации, содержащую записи маршрутов, подсети и статистику.
 * Подсети не пересекаются: например, 192.168.1.0/24 исключает 192.168.1.0/30 и 192.168.1.2/32.
 */
struct ROUTE_TABLE {
    struct ROUTE_ENTRY *entries;         /**< Массив записей маршрутов */
    size_t count;                        /**< Текущее количество записей */
    size_t capacity;                     /**< Максимальная емкость таблицы */
    uint32_t *dynamic_subnets;           /**< Не используется текущей реализацией; destroy освобождает память */
    size_t dynamic_subnet_count;         /**< Не используется текущей реализацией */
    uint32_t *local_subnets;             /**< Не используется текущей реализацией; destroy освобождает память */
    size_t local_subnet_count;           /**< Не используется текущей реализацией */
    struct {
        uint64_t local_routes;           /**< Количество локальных маршрутов */
        uint64_t learned_routes;         /**< Количество изученных маршрутов */
        uint64_t routes_lookup_hits;     /**< Количество попаданий в поиск маршрутов */
        uint64_t routes_lookup_misses;   /**< Количество промахов в поиск маршрутов */
    } stats;                             /**< Статистика таблицы маршрутизации */
};

/**
 * @brief Создает новую таблицу маршрутизации
 * 
 * @return Указатель на созданную таблицу или NULL в случае ошибки
 */
struct ROUTE_TABLE *route_table_create(void);

/**
 * @brief Уничтожает таблицу маршрутизации и освобождает ресурсы
 *
 * @param table Указатель на таблицу маршрутизации
 */
void route_table_destroy(struct ROUTE_TABLE *table);

/**
 * @brief Атомарно вставляет все IPv4-подсети узла; /0..32, адреса нормализуются по маске
 * @param table Таблица без пересечений; перед заменой маршрутов узла вызвать route_delete
 * @param local true для собственных подсетей UTUN, false для маршрутов от соседей
 * @param node узел, все маршруты которого нужно добавить
 * 
 * @return true если весь набор добавлен; false при неверных аргументах/префиксе, отсутствии подсетей,
 * пересечении с таблицей/внутри набора или нехватке памяти. При false таблица не меняется.
 */
bool route_insert(struct ROUTE_TABLE *table, struct TOPO_GROUP_NODE *node, bool local);

/**
 * @brief Удаляет все записи из таблицы маршрутизации для указанного узла
 * 
 * @param table Указатель на таблицу маршрутизации
 * @param node узел, все маршруты которого нужно удалить
 */
void route_delete(struct ROUTE_TABLE *table, struct TOPO_GROUP_NODE *node);

/**
 * @brief Выполняет поиск маршрута для заданного IP-адреса
 * 
 * @param table Указатель на таблицу маршрутизации
 * @param dest_ip Целевой IPv4-адрес в host order
 * @return найденный маршрут или NULL
 */
struct ROUTE_ENTRY* route_lookup(struct ROUTE_TABLE *table, uint32_t dest_ip);

/**
 * @brief Печатает содержимое таблицы маршрутизации
 * 
 * @param table Указатель на таблицу маршрутизации
 */
void route_table_print(const struct ROUTE_TABLE *table);

/**
 * @brief Разбирает IPv4/префикс; host-биты адреса сохраняются, префикс читается через atoi
 * 
 * @param subnet_str Строка подсети (например, "192.168.1.0/24")
 * @param network Указатель для сохранения адреса в host order
 * @param prefix_length Указатель для сохранения длины префикса
 * @return 0 при успехе, -1 при ошибке
 */
int parse_subnet(const char *subnet_str, uint32_t *network, uint8_t *prefix_length);

/**
 * @brief Проверяет что IP адрес НЕ является публичным (роутится в интернете)
 *
 * @param ip IP адрес в сетевом порядке (big-endian)
 * @return 0 - публичный (роутится в интернете), 1 - локальный/CGNAT/мультикастовый/зарезервированный
 */
int is_local_subnet(uint32_t ip);


#ifdef __cplusplus
}
#endif
#endif // ROUTE_LIB_H
