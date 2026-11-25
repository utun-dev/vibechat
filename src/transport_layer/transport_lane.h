/* Исполнитель направления транспорта: собственный uasync + пул криптографии либо текущий uasync при threads=0.
 * post передаёт собственное сообщение потоку направления; call синхронно выполняет управляющую операцию.
 * Callbacks направления не обращаются к ETCP. Сокеты заимствованы, после снятия регистрации их закрывает ядро. */
#ifndef UTUN_TRANSPORT_LANE_H
#define UTUN_TRANSPORT_LANE_H
#include "crypto_pool.h"
struct transport_lane;
struct transport_waiter {
    struct transport_waiter *next;
    void (*ready)(void *arg); // Поток направления: повторить RX после возврата кредита.
    void *arg;
};
struct transport_message {
    struct transport_message *next; // Внутренняя очередь; после post сообщение принадлежит fn.
    void (*fn)(void *arg);           // Выполняется в потоке направления, может освободить сообщение.
    void *arg;
};
/* 0..64 криптоворкеров; 0 не создаёт ни потока I/O, ни пула фоновых потоков. NULL при ошибке. */
struct transport_lane *transport_lane_create(struct UASYNC *main, unsigned threads, const char *name);
/* Без аллокаций и отказов: message остаётся жив до вызова fn. При 0 выполняется синхронно. */
void transport_lane_post(struct transport_lane *lane, struct transport_message *message);
/* Только управляющие операции; ждёт fn без poll основного uasync. arg заимствован до возврата. */
void transport_lane_call(struct transport_lane *lane, void (*fn)(void *), void *arg);
/* Только поток направления: владельцы сокетов и криптографических заданий. */
struct UASYNC *transport_lane_uasync(struct transport_lane *lane);
struct crypto_pool *transport_lane_pool(struct transport_lane *lane);
/* Общий кредит UDP/TCP: reserve/release потокобезопасны; память включает результат до потребления ядром. */
int transport_lane_reserve(struct transport_lane *lane, size_t bytes);
int transport_lane_available(struct transport_lane *lane, size_t bytes);
void transport_lane_release(struct transport_lane *lane, size_t bytes);
/* Только поток направления: регистрация владельца UDP/TCP, ожидающего свободный кредит. */
void transport_lane_watch(struct transport_lane *lane, struct transport_waiter *waiter, int enable);
/* Только после снятия всех сокетов/таймеров владельцами и прекращения producers. Не из fn/done. */
void transport_lane_destroy(struct transport_lane *lane);
#endif
