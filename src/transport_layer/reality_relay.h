// reality_relay.h — живой релей неавторизованных клиентов на реальный HTTPS-сайт
//
// Используется сервером STCP при включённом reality: если авторизация ClientHello
// не прошла (не utun-клиент — DPI-проба, случайный посетитель), соединение
// прозрачно проксируется на реальный сайт (dest), чтобы проба видела настоящий
// HTTPS-сеанс, а не ошибку/разрыв.
// Все операции — в uasync-потоке owner; owner изначально обнулён и живёт до shutdown.
// DNS выполняется асинхронно (только A); подключение использует первый IPv4-адрес.
#ifndef REALITY_RELAY_H
#define REALITY_RELAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "../lib/socket_compat.h"

struct UASYNC;
struct reality_owner;

// Начать релей. Забирает владение client_sock (non-blocking, зарегистрирован
// в uasync либо ещё не зарегистрирован) и проксирует его на dest (IPv4/hostname[:port], по умолчанию 443).
// IPv6 dest не поддерживается. Явный порт — десятичный 1..65535, вся строка dest короче 256 байт.
// initial_data/initial_len — уже прочитанные байты от клиента (ClientHello),
// отправляются на dest первыми (буфер КОПИРУЕТСЯ — владение остаётся у вызывающего).
// idle_timeout_sec — время простоя (без трафика в обе стороны), после которого
// релей принудительно закрывается; 0 = таймер выключен, максимум INT_MAX/10000 секунд.
// initial_len <= UINT32_MAX; NULL допустим только при initial_len=0.
// Возвращает 0 = запуск принят, -1 = синхронная ошибка (client_sock уже закрыт внутри).
// Ошибки DNS/connect после принятия закрывают relay и логируются; отдельного callback нет.
int reality_relay_start(struct reality_owner *owner, struct UASYNC *ua, socket_t client_sock,
                        const char *dest,
                        const uint8_t *initial_data, size_t initial_len,
                        int idle_timeout_sec);

// Синхронно завершает активные и освобождает закрытые relay, отменяя их call_soon.
// Вызывать до уничтожения UASYNC и вне callbacks relay; повторный вызов/NULL безопасны.
void reality_relay_shutdown(struct reality_owner *owner);

#ifdef __cplusplus
}
#endif
#endif // REALITY_RELAY_H
