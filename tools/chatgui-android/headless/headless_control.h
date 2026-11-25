/* Каркас TCP-управления Android headless: init → регулярный poll → destroy в одном потоке.
 * ua сейчас не используется; основной рабочий API чата — src/chat/chat_headless_control.h.
 * JSON line: {"id":N,"cmd":"..."} → {"id":N,"ok":true,"data":...} либо error.
 * ping/status/connections/debug_level возвращают заглушки; subscribe/quit управляют клиентом.
 * join/chat_setting обращаются к запущенному instance_lite; прочие команды дают not implemented.
 * event/broadcast_log отправляют JSON подписанным клиентам. Этот модуль сам ядро не запускает. */
#ifndef HEADLESS_CONTROL_H
#define HEADLESS_CONTROL_H

#include <stdint.h>
#include <stddef.h>

struct UASYNC;

/* ── Lifecycle ── */

/* Открыть один IPv4 listener: bind_ip=NULL → 127.0.0.1, port=1..65535; ua игнорируется.
 * 0 — слушает, -1 — ошибка. Перед повторным init вызвать destroy. */
int  headless_control_init(struct UASYNC* ua, const char* bind_ip, int port);
/* Обработать подключения/строки; select может ждать до 100мс. Все API модуля — из одного потока. */
void headless_control_poll(void);
/* Закрыть listener и всех клиентов, повторный вызов допустим. */
void headless_control_destroy(void);

/* ── Send event to all subscribed clients ── */

/* json_event — готовая NUL-терминированная JSON-строка; заимствуется на вызов, добавляется LF. */
void headless_control_event(const char* json_event);
/* Формирует событие log; строки должны быть уже безопасны для включения внутрь JSON-кавычек. */
void headless_control_broadcast_log(const char* level, const char* category, const char* msg);

#endif /* HEADLESS_CONTROL_H */
