/* Пакетный канал одного uasync-потока: state → send/received → close.
 * Каждый модуль сообщает готовность только собственных ресурсов. Формат байтов задаёт пользователь.
 * send не вызывает верхние callbacks; received передаёт владение entry и dgram получателю.
 * close допустим из callback, отменяет следующие события; после close handle недействителен. */
#ifndef PACKET_LINK_H
#define PACKET_LINK_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct packet_link;
struct ll_entry;
enum packet_link_result {
    PACKET_LINK_OK = 0, PACKET_LINK_WOULD_BLOCK = -1, PACKET_LINK_CLOSED = -2,
    PACKET_LINK_TOO_LARGE = -3, PACKET_LINK_INVALID = -4, PACKET_LINK_ERROR = -5
};
enum packet_link_phase { PACKET_LINK_CONNECTING, PACKET_LINK_OPEN, PACKET_LINK_TERMINATED };
/* Направление физического подключения; не роль протокола, передаваемого по каналу. */
enum packet_link_direction { PACKET_LINK_DIRECTION_UNKNOWN, PACKET_LINK_OUTGOING, PACKET_LINK_INCOMING };
struct packet_link_state {
    enum packet_link_phase phase; /* OPEN — готов транспорт, не ETCP-сессия. */
    size_t max_packet;            /* Максимальный пакет, включая байты формата пользователя. */
    int tx_ready;                /* Есть место для одного пакета до max_packet. */
    int error;                   /* 0 — штатно; иначе packet_link_result. Подробности — в логе реализации. */
};
struct packet_link_callbacks {
    void (*state_changed)(struct packet_link* link, void* arg); /* События объединяются; читать актуальный get_state. */
    void (*received)(struct packet_link* link, struct ll_entry* packet, void* arg); /* Получатель владеет пакетом. */
};
/* Снимок без callbacks/изменения очередей; NULL возвращает TERMINATED/CLOSED. */
struct packet_link_state packet_link_get_state(const struct packet_link* link);
/* Неизменное направление из скопированного описания драйвера; NULL возвращает UNKNOWN. */
enum packet_link_direction packet_link_get_direction(const struct packet_link* link);
/* Целый извлечённый из очереди entry: OK передаёт владение, любой отказ оставляет его вызывающему. */
int packet_link_send(struct packet_link* link, struct ll_entry* packet);
/* Включить/приостановить received; нижний модуль сам ограничивает накопление. Изначально RX выключен. */
void packet_link_set_rx_enabled(struct packet_link* link, int enabled);
/* Отменить события и остановить реализацию; освобождение ждёт активные обработчики. NULL допустим. */
void packet_link_close(struct packet_link* link);
#ifdef __cplusplus
}
#endif
#endif
