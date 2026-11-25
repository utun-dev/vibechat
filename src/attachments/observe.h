/* SQL-адаптер существующего группового manifest {ct,d} в общий каталог вложений.
 * Вызывать после проверки подписанного сообщения, в его WRITE. Обычный текст — no-op.
 * CHAT/KANBAN используют одинаковый descriptor/доставку; DM регистрирует E2E descriptor своим адаптером. */
#ifndef UTUN_ATTACHMENT_OBSERVE_H
#define UTUN_ATTACHMENT_OBSERVE_H
#include "store.h"
enum attachment_observe_skip { ATTACHMENT_OBSERVE_NONE, ATTACHMENT_OBSERVE_INVALID, ATTACHMENT_OBSERVE_CONFLICT };
/* json/length — точное тело проверенного сообщения; message — постоянный ID, author — подписант.
 * Если skip задан, неверный манифест и конфликт UUID возвращают SQLITE_OK с причиной пропуска.
 * При skip=NULL прежний строгий режим. Ошибки SQLite требуют отката caller WRITE в обоих режимах. */
int attachment_manifest_observe_write(sqlite3* db, enum attachment_owner owner, const char* space,
    const uint8_t message[32], const uint8_t author[32], const char* json, size_t length, enum attachment_observe_skip* skip);
#endif
