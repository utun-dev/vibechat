/* Uasync-восстановление READY: один собственный READ/file/model continuation на instance.
 * schedule после включения publisher, stop до media/SQL drain; остановка не удаляет durable jobs.
 * Поздний callback владеет своим payload и не заимствует уже освобождённый runtime. */
#ifndef UTUN_ATTACHMENT_RECOVERY_H
#define UTUN_ATTACHMENT_RECOVERY_H
struct UTUN_INSTANCE;
void attachment_recovery_schedule(struct UTUN_INSTANCE* inst);
void attachment_recovery_stop(struct UTUN_INSTANCE* inst);
#endif
