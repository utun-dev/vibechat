/* Адаптер подготовленного группового файла в подписанное сообщение карточки.
 * Uasync, строки/метаданные копируются до возврата; done(arg,success) ровно один раз, включая отказ.
 * Файл собственный, под media_base; caller сохраняет его до done. success=0 может включать commit после stop.
 * Регистрация/доставка — attachments/group_file, здесь только контекст карточки, права и публикация после commit. */
#ifndef UTUN_KANBAN_ATTACHMENT_H
#define UTUN_KANBAN_ATTACHMENT_H
#include "../media_async/attachment.h"
struct UTUN_INSTANCE;
#ifdef __cplusplus
extern "C" {
#endif
void kanban_attachment_send(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* path,
    const struct attachment_info* info, const char* type, uint64_t request, void (*done)(void*, int), void* arg);
/* После восстановления READY: без повторного индексирования, все входы копируются; те же правила done/commit. */
void kanban_attachment_publish_ready(struct UTUN_INSTANCE* inst, const char* group, const char* card, const char* path,
    const char* type, const char* body, const uint8_t job[16], void (*done)(void*, int), void* arg);
#ifdef __cplusplus
}
#endif
#endif
