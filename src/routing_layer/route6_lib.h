/* route6_lib — локальная IPv6-таблица подсетей на BSD radix с longest-prefix lookup.
 * Очередь индексирует записи по node_id для удаления всех подсетей узла; TOPO_GROUP_NODE заимствован.
 * create/insert/lookup/delete/destroy — в одном uasync-потоке; маршруты удалять до освобождения узла.
 * addr — 16 сетевых байтов. Результат lookup принадлежит таблице и живёт до удаления записи/destroy. */
#ifndef ROUTE6_LIB_H
#define ROUTE6_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../lib/radix.h"
#include "../lib/ll_queue.h"
#include "../lib/u_async.h"




// таблица маршрутизации IPv6 на radix tree + ll_queue (поиск по node_id для удаления)

struct ROUTE6_DATA {
    struct radix_node rn_nodes[2];   // scratch-буфер для rn_addroute
    struct TOPO_GROUP_NODE *v_node_info;  // узел-владелец
    uint64_t node_id;                // ключ для hash-поиска в ll_queue
    uint8_t  prefix_length;          // 0..128
    uint8_t  key[17];                // sockaddr-like: {len=17, addr[16]}
    uint8_t  mask[17];               // sockaddr-like: {len=17, mask[16]}
};

#define ROUTE6_DATA_SIZE   sizeof(struct ROUTE6_DATA)
#define ROUTE6_NODE_ID_OFF offsetof(struct ROUTE6_DATA, node_id)
#define ROUTE6_KEY_SIZE    17

struct ROUTE6_TABLE {
    struct radix_node_head *rnh;     // radix tree
    struct ll_queue *queue;          // indexed by node_id for delete
};

/* Создать таблицу в ua (его время жизни длиннее таблицы); NULL при ошибке. */
struct ROUTE6_TABLE *route6_table_create(struct UASYNC *ua);
/* Освободить таблицу и записи, сохраняя заимствованные узлы; NULL допустим. */
void route6_table_destroy(struct ROUTE6_TABLE *table);
/* Вставить IPv6-подсети node. Вложенные префиксы допустимы; дубликаты radix пропускает.
 * true означает завершённый обход, даже если ничего не добавлено. false — неверный вход/нет подсетей/OOM;
 * при OOM ранее вставленные подсети остаются. Адреса и /0..128 должен проверить вызывающий. */
bool route6_insert(struct ROUTE6_TABLE *table, struct TOPO_GROUP_NODE *node);
/* Удалить все записи с node->node_id (не по равенству указателей); NULL table/node допустимы. */
void route6_delete(struct ROUTE6_TABLE *table, struct TOPO_GROUP_NODE *node);
/* Найти наиболее длинный подходящий префикс; addr — ненулевой указатель на 16 сетевых байт. NULL — нет маршрута. */
const struct ROUTE6_DATA *route6_lookup(struct ROUTE6_TABLE *table, const uint8_t addr[16]);


#ifdef __cplusplus
}
#endif
#endif
