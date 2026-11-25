/* Потокобезопасный вход команд JOIN для GUI/JNI. Контекст удерживается фронтендом
 * от bind до unbind; close в uasync отменяет команды и запрещает дальнейшие submit.
 * После close порт живёт до release последних ссылок, не содержит живого instance. */
#ifndef CHAT_JOIN_PORT_H
#define CHAT_JOIN_PORT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct chat_join_port;
/* Создать порт в uasync; одна ссылка принадлежит chat_sync, NULL при ошибке. */
struct chat_join_port* chat_join_port_create(struct UTUN_INSTANCE* inst);
/* Получить отдельную ссылку на порт текущего чата; только uasync, NULL до init. */
struct chat_join_port* chat_sync_join_port(struct UTUN_INSTANCE* inst);
/* Управление отдельной ссылкой. retain требует уже удерживаемой ссылки. */
void chat_join_port_retain(struct chat_join_port* port);
void chat_join_port_release(struct chat_join_port* port);
/* uasync: снять корреляцию завершённой активной команды до уведомления. */
void chat_join_port_complete(struct chat_join_port* port, uint64_t request);
/* uasync: закрыть приём и завершить ожидающие команды CANCELLED; повтор допустим. */
void chat_join_port_close(struct chat_join_port* port);
/* Любой поток: скопировать ссылку и принять команду. 0 — будет один результат с request;
 * <0 — команда не принята, результата не будет. request != 0; link заимствован до возврата. */
int chat_join_port_submit(struct chat_join_port* port, uint64_t request, const char* link, size_t len);
/* Те же гарантии для приглашения адресата ссылки в собственный channel. */
int chat_join_port_offer(struct chat_join_port* port, uint64_t request, const char* channel, const char* link, size_t len);
#ifdef __cplusplus
}
#endif
#endif
