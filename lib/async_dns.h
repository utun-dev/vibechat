/*
 * async_dns.h — асинхронный (неблокирующий) DNS-резолвер поверх uasync.
 *
 * Тонкий адаптер над libdns (lib/dns.c, MIT). Выполняет A-запросы без
 * блокировки event loop: резолвер libdns управляется через uasync-сокеты и
 * таймеры (интерфейс pollfd/events/timeout + submit/check/fetch).
 *
 * Поддерживает только A-записи (IPv4) — этого достаточно для NTP и текущего
 * стека. TCP-fallback при truncation и компрессия имён — внутри libdns.
 */
#ifndef ASYNC_DNS_H
#define ASYNC_DNS_H

#include "u_async.h"
#include "socket_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADNS_OK            0
#define ADNS_ERR_NOMEM    -1
#define ADNS_ERR_PARAM    -2
#define ADNS_ERR_NOSERV   -3   /* нет ни одного DNS-сервера */
#define ADNS_ERR_NODATA   -4   /* NXDOMAIN или нет A-записей */
#define ADNS_ERR_TIMEOUT  -5
#define ADNS_ERR_RESPONSE -6   /* malformed / SERVFAIL / прочая ошибка ответа */

#define ADNS_MAX_ADDRS 8

struct adns_result {
    int status;
    int count;
    struct sockaddr_in addrs[ADNS_MAX_ADDRS];  /* AF_INET, sin_port = 0 */
};

/* res заимствован до возврата callback; handle запроса к этому моменту уже освобождён. */
typedef void (*adns_done_cb)(const struct adns_result* res, void* arg);

struct adns_opts {
    struct sockaddr_in server;   /* sin_family != 0 → использовать этот сервер (иначе системные) */
    int timeout_ms;              /* Таймаут на сервер в мс; <=0 → 2000, округляется вверх до целых секунд. */
    int max_attempts;            /* попыток на сервер, по умолчанию 2 */
};

struct adns_query;  /* opaque */

/* Запускает асинхронный A-запрос. Коллбэк вызывается в потоке event loop.
 * Возвращает handle, автоматически освобождаемый до callback либо при отмене.
 * NULL: неверные аргументы, нехватка памяти, отсутствие DNS-серверов, ошибка создания
 * резолвера, постановки запроса или регистрации отложенного запуска; callback не вызывается. */
struct adns_query* adns_resolve(struct UASYNC* ua, const char* name,
                                const struct adns_opts* opts,
                                adns_done_cb cb, void* arg);

/* Отмена ожидающего запроса: освобождает handle, коллбэк НЕ вызывается. Только из потока loop.
 * После завершения/отмены handle недействителен; NULL допустим. */
void adns_cancel(struct adns_query* q);

/* Системные DNS-серверы (IPv4). Возвращает количество; out[].sin_port = htons(53).
   Локальное чтение (resolv.conf / GetNetworkParams), без сети. */
int adns_system_servers(struct sockaddr_in* out, int max);

/* Чистый парсер текста resolv.conf ("nameserver <ip>") — для юнит-тестов. */
int adns_parse_resolv_conf(const char* text, struct sockaddr_in* out, int max);

#ifdef __cplusplus
}
#endif

#endif /* ASYNC_DNS_H */
