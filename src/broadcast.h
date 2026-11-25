/* broadcast — рассылка данных соседям topo-группы с пересылкой и дедупликацией UUID.
 * Контекст и кеш увиденных UUID принадлежат группе; TTL — срок хранения UUID, не счётчик сетевых хопов.
 * init_instance регистрирует общий ETCP-диспетчер, init(group) создаёт состояние отдельной группы.
 * Все вызовы/callbacks — uasync-поток; recv получает заимствованные uuid/data только на время callback.
 * send копирует данные и возвращает 0 / -1; 0 не подтверждает доставку каждому участнику.
 * Получение нового UUID вызывает подписчиков; локальный send их не вызывает. Кеш общий для send/recv группы.
 * Внутри recv-callback нельзя менять список подписчиков или уничтожать группу/контекст broadcast. */
#ifndef BROADCAST_H
#define BROADCAST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

struct TOPO_GROUP;
struct UTUN_INSTANCE;

#define BROADCAST_MAX_DATA 1600
#define BROADCAST_UUID_SIZE 16
#define BROADCAST_GROUP_OFF 1
#define BROADCAST_PAYLOAD_OFF (BROADCAST_GROUP_OFF + 32)
#define BROADCAST_HEADER_SIZE (BROADCAST_PAYLOAD_OFF + BROADCAST_UUID_SIZE + 2)
#define BROADCAST_CLEANUP_INTERVAL_TB 600000   // 1 минута
#define BROADCAST_TTL_TB           1200000      // 2 минуты; удаление при возрасте > TTL на периодической очистке

// uuid — 16 байт, data — data_len байт; заимствованы только на время вызова. arg — контекст из add_cbk.
typedef void (*broadcast_recv_fn)(const uint8_t* uuid, const uint8_t* data, uint16_t data_len, void* arg);

struct broadcast_cbk_entry {
    broadcast_recv_fn fn;               // Функция подписчика
    void*            arg;               // Заимствованный контекст, модуль его не освобождает
    struct broadcast_cbk_entry* next;   // Следующая регистрация (новые добавляются в начало)
};

struct broadcast_ctx;

// Создаёт кеш и таймер живой group с instance/ua; 0 — успех, -1 — NULL group, повторный init или ошибка ресурсов.
int  broadcast_init(struct TOPO_GROUP* group);
// Отменяет таймер/standby-ожидание, освобождает кеш и регистрации, но не arg подписчиков; NULL/нет контекста допустимы.
void broadcast_destroy(struct TOPO_GROUP* group);
// data не NULL даже при data_len=0; data_len <= BROADCAST_MAX_DATA, группа должна быть инициализирована.
// 0 — данные сохранены в кеше и рассылка предпринята; -1 — аргументы/контекст/отсутствие CHAT-допуска/OOM кеша.
// Ошибки выделения/отправки отдельным соседям не меняют 0; собственные recv-callbacks не вызываются.
int  broadcast_send(struct TOPO_GROUP* group, const uint8_t* data, uint16_t data_len);
// Добавляет fn(arg) в начало; дубликаты разрешены и вызываются отдельно. При NULL/нет контекста/OOM не добавляет.
// arg должен жить до удаления регистрации или destroy; список не менять из recv-callback.
void broadcast_add_cbk(struct TOPO_GROUP* group, broadcast_recv_fn fn, void* arg);
// Удаляет первое совпадение fn+arg (последнее добавленное); отсутствие совпадения ничего не меняет.
void broadcast_remove_cbk(struct TOPO_GROUP* group, broadcast_recv_fn fn, void* arg);

// Регистрирует диспетчер ETCP_ID_BROADCAST для instance; возвращает результат etcp_bind или -1 при NULL.
int  broadcast_init_instance(struct UTUN_INSTANCE* instance);
// Снимает общий диспетчер, но не уничтожает контексты отдельных групп; NULL допустим.
void broadcast_destroy_instance(struct UTUN_INSTANCE* instance);

#ifdef __cplusplus
}
#endif
#endif
