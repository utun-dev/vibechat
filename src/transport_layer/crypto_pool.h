/* Ограниченный пул независимых задач криптографии. Все API и done — поток owner uasync;
 * work — любой свободный worker, без доступа к owner/ETCP и чужим изменяемым данным.
 * create → submit → done → destroy. При threads=0 work/done вызываются прямо из submit.
 * destroy завершает принятые задачи и вызывает оставшиеся done до возврата; из done destroy запрещён. */
#ifndef UTUN_CRYPTO_POOL_H
#define UTUN_CRYPTO_POOL_H
#include <stddef.h>
#include <stdint.h>
struct UASYNC;
struct crypto_pool;
struct crypto_task {
    void (*work)(void *arg); // Единственный владелец payload на время вычисления.
    void (*done)(void *arg); // Возврат владения owner; допускает освобождение task/arg.
    void *arg;              // Собственные данные задачи, живут до done.
    size_t bytes;           // Резерв всех буферов задачи, включая результат.
};
/* threads=0..64; pending/bytes ограничивают все незавершённые задачи, включая готовые результаты.
 * name копируется для логов. NULL с диагностикой при ошибке. owner живёт до destroy. */
struct crypto_pool *crypto_pool_create(struct UASYNC *owner, unsigned threads, unsigned pending, size_t bytes, const char *name);
/* 0 — принято (при threads=0 уже выполнено); -2 — нет ёмкости; -1 — ошибка. Отказ сохраняет владение caller. */
int crypto_pool_submit(struct crypto_pool *pool, const struct crypto_task *task);
/* Количество принятых незавершённых задач, только owner. */
unsigned crypto_pool_pending(const struct crypto_pool *pool);
/* Дождаться workers без poll(owner), доставить завершения, освободить пул. NULL допустим. */
void crypto_pool_destroy(struct crypto_pool *pool);
#endif
