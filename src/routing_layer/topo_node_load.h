/* Асинхронная загрузка узла: SQL-снимок читает DB worker, registry/пулы меняются в uasync.
 * load_async -> done либо cancel. Принятый запрос вызывает done один раз, после возврата load_async;
 * cancel снимает callback, а начатый SQL безопасно освобождает свои данные после завершения.
 * Все вызовы API — в uasync; stop выполняется после остановки потребителей, до закрытия БД и пулов. */
#ifndef UTUN_TOPO_NODE_LOAD_H
#define UTUN_TOPO_NODE_LOAD_H
#include "topo_node_sqlite.h"
#include "../../lib/db_async.h"
#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;
struct topo_node_load;
/* node заимствован до возврата done; при необходимости удержать registry ref.
 * status=OK,node=NULL означает отсутствие сведений. При ошибке node=NULL.
 * result заимствован до возврата; запрос после done недействителен, caller сбрасывает свой указатель.
 * Instance/groups не уничтожать из done; запрос остановки ядра разрешён, destroy выполняется после callback. */
typedef void (*topo_node_load_done)(void* data, struct TOPO_NODE* node, const struct db_async_result* result);
/* Читать сохранённый snapshot; при отсутствии fallback выбирает IDENTITY, BOOTSTRAP либо CONNECT.
 * fallback=SNAPSHOT запрещает вторую выборку. Сначала проверяется уже готовый registry.
 * NULL — отказ аргументов/резерва памяти, done не вызывается; иначе ровно один done, кроме cancel.
 * data заимствованы до done/cancel. Результат регистрируется без повторного SQL; ошибки не публикуют узел.
 * Файловая БД требует inst->db_worker; только :memory: допускает чтение RAM в uasync. */
struct topo_node_load* topo_node_load_async(struct UTUN_INSTANCE* inst, uint64_t node_id,
    enum topo_node_record_kind fallback, topo_node_load_done done, void* data);
/* Тот же запрос с копией ожидаемого X25519. Реестр и SQL проверяют точное совпадение ключа. */
struct topo_node_load* topo_node_load_key_async(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x,
    enum topo_node_record_kind fallback, topo_node_load_done done, void* data);
/* Отменить callback и забыть запрос; NULL допустим. Начатый SQL не прерывается и не читает data/runtime.
 * После cancel указатель недействителен. Вызов из собственного done безопасен и ничего не меняет. */
void topo_node_load_cancel(struct topo_node_load* request);
/* Завершить все оставшиеся запросы done(CLOSED), не строя узлы; число завершённых запросов.
 * Сначала потребители отменяют свои запросы; ua/inst живы до последующего db_async_destroy.
 * Callback при stop может быть синхронным. Stop постоянный: новые load_async отвергаются. */
unsigned topo_node_load_stop(struct UTUN_INSTANCE* inst);
#ifdef __cplusplus
}
#endif
#endif
