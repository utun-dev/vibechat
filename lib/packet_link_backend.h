/* Экземпляр пакетного драйвера: создать → передать packet_link_bind → stop/destroy.
 * Один ctx — один канал к пиру; общий сокет/радио принадлежит платформенному владельцу.
 * Все API и события выполняются в ua. До bind RX выключен. */
#ifndef PACKET_LINK_BACKEND_H
#define PACKET_LINK_BACKEND_H
#include "packet_link.h"
#ifdef __cplusplus
extern "C" {
#endif
struct UASYNC;
struct packet_link_events {
    void (*state_changed)(void* arg); /* Читать актуальный get_state; событие обязательно при изменении готовности. */
    void (*received)(void* arg, struct ll_entry* packet); /* Передаёт владение entry+dgram получателю. */
};
struct packet_link_driver {
    void* ctx;                      /* Управляющая структура одного канала. */
    struct UASYNC* ua;               /* Тот же event loop, что у потребителя. */
    const char* name;                /* Непустое диагностическое имя; оболочка копирует строку. */
    /* Копирует events, заимствует arg. 0 — успех, отказ оставляет ctx непривязанным. Без синхронных событий. */
    int (*bind)(void* ctx, const struct packet_link_events* events, void* arg);
    struct packet_link_state (*get_state)(void* ctx); /* Снимок без побочных действий. */
    int (*send)(void* ctx, struct ll_entry* packet); /* OK передаёт владение; любой отказ сохраняет его. Без RX callbacks. */
    void (*set_rx_enabled)(void* ctx, int enabled); /* Разрешить/приостановить RX; накоплением управляет драйвер. */
    void (*stop)(void* ctx);          /* Сразу запретить события, остановить канал; не освобождать ctx. */
    void (*destroy)(void* ctx);       /* Освободить остановленный ctx после выхода обработчиков. */
    enum packet_link_direction direction; /* Диагностика подключения; копируется при bind, 0 — неизвестно. */
};
/* Копирует API/name/callbacks. Успех передаёт ctx оболочке; отказ сохраняет его вызывающему.
 * arg живёт до close. Первое state_changed отложено. TERMINATED необратим, max_packet не меняется. */
struct packet_link* packet_link_bind(const struct packet_link_driver* driver,
    const struct packet_link_callbacks* callbacks, void* arg);
/* Заимствованное диагностическое имя до close. */
const char* packet_link_name(const struct packet_link* link);
#ifdef __cplusplus
}
#endif
#endif
