/* Снимок настроек группы для UI: только SQL и собственный JSON, без runtime и секретных ключей.
 * Worker вызывает функцию внутри READ; caller освобождает результат через u_free.
 * Вся проверка JSON и построение ответа относятся к одному SQL-снимку caller. */
#ifndef UTUN_GROUP_INFO_READ_H
#define UTUN_GROUP_INFO_READ_H
#include <stdint.h>
struct sqlite3;
#ifdef __cplusplus
extern "C" {
#endif

/* channel — публичный Ed25519 группы в строчном HEX, self_ed — локальный Ed25519.
 * Читать описание, признак права редактирования и SHA256-token исходного JSON.
 * SQLITE_OK/код SQLite; *result=NULL при любом отказе, включая отсутствие группы.
 * JSON {ok,can_edit,token,description} самостоятельный; приватный ключ в результат не попадает. */
int group_info_snapshot_read(struct sqlite3* db, const char* channel, const uint8_t self_ed[32], char** result);

#ifdef __cplusplus
}
#endif
#endif
