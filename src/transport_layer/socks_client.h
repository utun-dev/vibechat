// socks_client.h — SOCKS5 клиент (для исходящих ETCP-линков через SOCKS-прокси)
//
// Модуль позволяет подключаться к пиру не напрямую, а через SOCKS5-прокси:
//   - SOCKS5 CONNECT (TCP/STCP-линки) — socks_dial();
//   - SOCKS5 UDP ASSOCIATE (UDP-линки) — socks_udp_associate() и далее.
//
// Модуль не знает про ETCP — только транспорт. API/callbacks — uasync-поток.
// cfg копируется, cb/arg заимствованы. Диагностика — категория proxy, пароли в лог не попадают.
#ifndef SOCKS_CLIENT_H
#define SOCKS_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "../lib/socket_compat.h"

struct UASYNC;

// Все строки NUL-терминированы, максимум 63 байта. host — IP/домен, port ненулевой.
// Пустой user предлагает только no-auth; непустой предлагает no-auth и username/password.
struct socks_cfg {
    char     host[64];
    uint16_t port;
    char     user[64];
    char     pass[64];
};

// ── SOCKS5 CONNECT (TCP) ──────────────────────────────────────────────

// Коллбэк завершения подключения:
//   sock — готовый сокет (на нём можно делать STCP-хендшейк), err == 0;
//   sock == SOCKET_INVALID при ошибке, err — errno/WSA-код или протокольная ошибка.
// Состояние dial освобождено до callback, handle больше недействителен; отменять его из callback нельзя.
typedef void (*socks_dial_cb)(socket_t sock, int err, void *arg);

// Асинхронно подключиться к прокси и выполнить SOCKS5 CONNECT к target_host:target_port.
// target_host может быть числовым IP или доменом (домен резолвит сам прокси).
// Возвращает handle (NULL при немедленной ошибке — тогда cb НЕ вызывается, ошибка залогирована).
// target_host — 1..255 байт, target_port ненулевой; длина не обрезается.
// Разрешение host самого прокси синхронно через getaddrinfo, используется первый IPv4/IPv6 адрес;
// connect/handshake и callback — после возврата. timeout_tb в 0.1 мс, <=0 означает 10 секунд.
// При успехе удаляет свою uasync-регистрацию и передаёт nonblocking socket владельцу.
// Первые байты TCP-потока после SOCKS-ответа остаются в сокете, control-буфер их не поглощает.
void *socks_dial(struct UASYNC *ua, const struct socks_cfg *cfg,
                 const char *target_host, uint16_t target_port,
                 socks_dial_cb cb, void *arg, int timeout_tb);

// Отменить незавершённый dial: закрывает сокет, освобождает состояние, cb НЕ вызывается.
void socks_dial_cancel(void *h);

// ── SOCKS5 UDP ASSOCIATE (UDP) ────────────────────────────────────────

struct socks_udp;

// err==0: relay готов (повторяется после reconnect), можно слать через socks_udp_sendto.
// ENOMEM: после запуска не удалось поставить retry, handle остаётся unready до destroy/recreate.
// В callback допускается socks_udp_destroy(s); после этого s/arg больше не используются модулем.
typedef void (*socks_udp_ready_cb)(struct socks_udp *s, int err, void *arg);

// Открыть UDP ASSOCIATE к прокси (persistent TCP-control + relay). local_udp_fd — уже
// забинденный локальный UDP-сокет (reserved port), через него шлём на relay.
// При обрыве control-канала сессия автоматически пересоздаёт ассоциацию (backoff).
// Возвращает сессию; NULL при неверных аргументах/ресурсах без callback.
// Начальная ошибка connect может вернуть сессию с ожидающим reconnect; 10с на попытку,
// backoff 1/2/4/.../30с. При отказе обоих начальных таймеров возвращает NULL.
// local_udp_fd заимствован до destroy, callback необязателен. Релей только IPv4/IPv6;
// wildcard BND.ADDR заменяется IP TCP-прокси с сохранением BND.PORT, который должен быть ненулевым.
struct socks_udp *socks_udp_associate(struct UASYNC *ua, const struct socks_cfg *cfg,
                                      socket_t local_udp_fd, socks_udp_ready_cb cb, void *arg);

// Отправить датаграмму пиру dst: обёртка в SOCKS-UDP заголовок + sendto(relay).
// len<=2048, data может быть NULL при len=0. Успех — len без SOCKS-заголовка, включая 0;
// -1 пока ассоциация не готова или при ошибке. Удалённая доставка не гарантируется.
ssize_t socks_udp_sendto(struct socks_udp *s, const uint8_t *data, size_t len,
                         const struct sockaddr_storage *dst);
/* Готовность relay; NULL — не готов. После reconnect ready_cb уведомляет о восстановлении. */
int socks_udp_is_ready(const struct socks_udp *s);
/* Скопировать готовый relay endpoint без заимствования состояния SOCKS; 1 — готов, 0 — нет. Только uasync. */
int socks_udp_get_relay(const struct socks_udp *s, struct sockaddr_storage *relay);

// Снять SOCKS-UDP заголовок с входящей датаграммы: вернуть payload и реальный src пира.
// s не используется, может быть NULL; raw/src обязательны. Поддержаны IPv4/IPv6, FRAG=0.
// Возвращает длину payload (0 допустим), -1 при отказе; при ошибке src/payload не меняются.
// payload может быть NULL для пустых данных, иначе cap должен вмещать их целиком.
// Допускается снятие заголовка в том же буфере; src должен быть отдельным от raw/payload.
ssize_t socks_udp_unwrap(const struct socks_udp *s, const uint8_t *raw, size_t raw_len,
                         uint8_t *payload, size_t cap, struct sockaddr_storage *src);

// Закрыть сессию: control-TCP, таймеры, память. Локальный UDP-сокет НЕ закрывается (чужой).
void socks_udp_destroy(struct socks_udp *s);

#ifdef __cplusplus
}
#endif
#endif // SOCKS_CLIENT_H
