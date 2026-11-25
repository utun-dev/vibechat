/* Привязка экземпляра пакетного драйвера к ETCP_LINK; все операции — его uasync-поток.
 * Линк владеет привязкой и ctx драйвера, кодирование пакетов задаётся отдельно.
 * Встроенные UDP/STCP-адаптеры используют тот же контракт bind/get_state/send/stop/destroy. */
#ifndef ETCP_LINK_IO_H
#define ETCP_LINK_IO_H
#include "../../lib/packet_link_backend.h"
#include "../../lib/socket_compat.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ETCP_DGRAM;
struct ETCP_LINK;
struct ETCP_SOCKET;
struct etcp_link_io;
#define ETCP_LINK_PROTOCOL_SIZE 32
#define ETCP_LINK_SOCKET_NAME_SIZE 64
/* Собственные значения для диагностики; адреса в сетевом порядке, порт — host order. */
struct etcp_link_address {
    uint8_t family; /* 0 — неизвестен, 4/6 — IP. */
    uint8_t bytes[16]; /* Для IPv4 используются первые 4 байта. */
    uint16_t port;
};
struct etcp_link_snapshot {
    uint8_t id, state, status; /* ID и фактическая готовность ETCP, не нижнего транспорта. */
    enum packet_link_direction direction; /* Физическое подключение, независимо от роли ETCP. */
    uint8_t has_ip; /* IP-путь: адрес может быть ещё неизвестен; у внешнего драйвера 0. */
    struct etcp_link_address local, remote;
    char protocol[ETCP_LINK_PROTOCOL_SIZE]; /* Подпись для GUI, не используется для выбора поведения. */
    char socket_name[ETCP_LINK_SOCKET_NAME_SIZE]; /* Пусто для внешнего драйвера. */
};
/* В uasync скопировать описание живого линка без callbacks. 0/-1 с логом; при ошибке out не изменён. */
int etcp_link_get_snapshot(const struct ETCP_LINK* link, struct etcp_link_snapshot* out);
/* Фабрика встроенного UDP/STCP-адаптера; 0/-1. Не начинает handshake. */
int etcp_link_io_create(struct ETCP_LINK* link);
/* Привязать произвольный драйвер; успех забирает ctx, отказ сохраняет у вызывающего. */
int etcp_link_io_bind(struct ETCP_LINK* link, const struct packet_link_driver* driver);
/* Кодирование протокола независимо от способа доставки байтов. receive забирает entry+dgram. */
struct etcp_link_codec {
    int ip_headers; /* Прежний MTU UDP/STCP включает IP; независимый пакетный канал — нет. */
    int (*send)(struct ETCP_DGRAM* packet);
    void (*receive)(struct ETCP_LINK* link, struct ll_entry* packet);
};
/* Снять события, освободить очереди; NULL допустим. */
void etcp_link_io_destroy(struct ETCP_LINK* link);
/* Обновить состояние после события native-транспорта. */
void etcp_link_io_update(struct ETCP_LINK* link);
/* Заимствованный общий интерфейс; действителен до закрытия/замены адаптера. Отдельно close не вызывать. */
struct packet_link* etcp_link_io_get(struct ETCP_LINK* link);
/* Готовность для ETCP. До привязки адаптера дополнительного ограничения нет; send создаёт его при необходимости. */
int etcp_link_io_ready(struct ETCP_LINK* link);
/* Копирует краткоживущий wire-буфер в пакет; >=0 — принято, -2 — занят, -1 — ошибка. */
int etcp_link_io_send(struct ETCP_LINK* link, const void* bytes, size_t size);
/* Входящий пакет во владении вызывающего; передаёт callback либо освобождает с диагностикой отказа. */
void etcp_link_io_receive(struct ETCP_LINK* link, struct ll_entry* packet);
/* Общий UDP-сокет получил writable; обслужить ожидающие датаграммы его адаптеров. */
void etcp_link_io_socket_writable(socket_t socket, void* arg);
/* Размер сетевых заголовков выбранного кодирования; не зависит от драйвера. */
int etcp_link_network_overhead(const struct ETCP_LINK* link);
#ifdef __cplusplus
}
#endif
#endif
