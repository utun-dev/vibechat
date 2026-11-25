/* memory_pool — повторное использование блоков фиксированного размера в одном потоке.
 * init создаёт пустой пул; alloc выделяет блок по требованию или берёт свободный и обнуляет payload.
 * free проверяет защитный хвост и возвращает не более MEMORY_POOL_MAX_FREE блоков в кеш.
 * object_size должен вмещать void*: свободный блок хранит next в начале payload.
 * Перед destroy вернуть все выданные блоки: destroy освобождает только кеш и сам пул.
 * name заимствован до destroy; блок возвращать только своему пулу, внутренней синхронизации нет. */
/* Хвост блока хранит canary, места alloc/free и метку занятости. Повреждение или повторный возврат
 * кешированного блока логируется в SYS и навсегда останавливает текущий поток циклом диагностики.
 * Пример для очередей: init(sizeof(struct ll_entry) + inline_size, name), затем queue_entry_new_from_pool.
 * Объекты, уже переданные в u_free при переполнении кеша, больше нельзя проверять или возвращать. */
#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stddef.h>
#include "debug_config.h"

#define MEMORY_POOL_MAX_FREE 64

#define XSTR(s) STR(s)
#define STR(s) #s
#define PLOCATION __FILE__ ":" XSTR(__LINE__) "[POOL]"

struct memory_pool {
    void* free_head;            // Голова linked list свободных блоков (next хранится в начале каждого блока)
    int free_count;             // Количество свободных блоков
    size_t object_size;         // Размер объектов в пуле (пользовательские данные)
    size_t allocations;         // Попытки выделить новый блок, включая неудачные; выдачи из кеша не учитываются
    size_t reuse_count;         // Количество повторных использований из пула
    uint8_t alloc_tag_counter;  // Не используется; init устанавливает 1. Double-free проверяется по метке в хвосте блока
    const char* name;           // Имя пула для диагностики (напр. "data_pool", "pkt_pool")
};

// Создаёт пустой пул: object_size >= sizeof(void*), name живёт до destroy; NULL при OOM.
// Для ll_queue размер включает sizeof(struct ll_entry) и пользовательский data[].
struct memory_pool* memory_pool_init(size_t object_size, const char* name);
// Освобождает кеш и пул, но не выданные объекты; NULL допустим. Все владельцы должны предварительно вернуть блоки.
void memory_pool_destroy(struct memory_pool* pool);

// Выдаёт обнулённый payload из кеша или нового блока; NULL при NULL pool/OOM. location — место вызова в хвосте.
void* memory_pool_alloc_impl(struct memory_pool* pool, const char* location);
// Возвращает свой живой obj в кеш или u_free, если кеш заполнен; NULL pool/obj ничего не делает.
void memory_pool_free_impl(struct memory_pool* pool, void* obj, const char* location);
// Копирует счётчики попыток новых аллокаций и выдач из кеша; любой выход может быть NULL, NULL pool ничего не меняет.
void memory_pool_get_stats(struct memory_pool* pool, size_t* allocations, size_t* reuse_count);
// Общее число кешированных блоков всех пулов; глобальный счётчик атомарный, сами пулы однопоточные.
size_t memory_pool_get_total_free_blocks(void);

#define memory_pool_alloc(pool) memory_pool_alloc_impl(pool, PLOCATION)
#define memory_pool_free(pool, obj) memory_pool_free_impl(pool, obj, PLOCATION)

/* Проверяет присутствие в кеше свободных блоков; не определяет состояние уже освобождённого через u_free блока. */
int memory_pool_is_freed(struct memory_pool* pool, void* obj);


#ifdef __cplusplus
}
#endif
#endif // MEMORY_POOL_H
