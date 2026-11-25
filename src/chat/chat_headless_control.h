/*
 * chat_headless_control.h — TCP control socket for headless chat CLI
 *
 * JSON line protocol:
 *   Request:  {"id":N,"cmd":"<command>",...params}
 *   Response: {"id":N,"ok":true,"data":<JSON>} | {"id":N,"ok":false,"error":"..."}
 *   Event:    {"event":"<type>",...}
 *
 * Команды каналов, DM и взаимодействий — в tools/chatcli_commands.txt;
 * диспетчер реализации — chat_headless_control.c. Аудио PCM использует отдельные call/radio_headless сокеты.
 * init требует готовый chat_core и ua==inst->ua. Listener IPv4,до64 клиентов,строка до16383байт без LF.
 * До команды проверяется весь строгий JSON-object:до32 уникальных ключей длиной<=31байта,
 * id — int32,строковые параметры не обрезаются,Unicode escapes декодируются,скрытый NUL запрещён.
 * Массивы опросов передаются общему feedback API;ошибка разбора/типов получает id=0,ошибка команды сохраняет id.
 * Файловый разбор запроса и чтение channels/members/messages/dm_list/dm_messages выполняет READ worker.
 * На клиента одна операция: TCP read и следующая команда ждут completion, порядок ответов сохраняется.
 * Online/connected добавляются в uasync из текущих маршрутов/соединений после SQL-снимка.
 * Следующая строка запускается отдельным posted callback: отказ очереди не приводит к рекурсивному повтору.
 * Исходящая очередь до32768байт:короткие записи/EAGAIN сохраняют порядок,переполнение закрывает клиента с WARN.
 * quit дописывает bye перед закрытием. attach отвечает после сохранения/отказа публикации,не после доставки пирам;
 * его completion освобождается также после disconnect. connect/call подтверждают передачу запроса ядру;
 * подписчикам приходят connect_result/call_error и состояния звонка,а не гарантия готовности связи из ответа команды.
 * Прежний chat_event handler вызывается после headless;destroy восстанавливает его,если handler всё ещё принадлежит headless.
 * Parser/DB callbacks удерживают client/context до completion: destroy сразу закрывает сокеты и не ждёт worker.
 * Все вызовы/команды/completion — в uasync;stop/destroy вне callbacks этой службы. Диагностика — категория control.
 */
#ifndef CHAT_HEADLESS_CONTROL_H
#define CHAT_HEADLESS_CONTROL_H

#include <stdint.h>
#include <stddef.h>

struct UASYNC;
struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

/* 0 — запущен/повтор с тем же bind,-1 — отказ с полной очисткой. port=0 выбирает ОС. */
int  chat_headless_control_init(struct UASYNC* ua, struct UTUN_INSTANCE* inst,
                                 const char* bind_ip, int port);
void chat_headless_control_destroy(struct UTUN_INSTANCE* inst);
// port=0 при init выбирает ОС; getter возвращает 0 для остановленной службы или при ошибке.
uint16_t chat_headless_control_get_port(struct UTUN_INSTANCE* inst);

#ifdef __cplusplus
}
#endif

#endif /* CHAT_HEADLESS_CONTROL_H */
