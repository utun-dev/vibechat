/* Восстановление после несовместимой схемы. Только по выбору пользователя после закрытия всех SQLite handles.
 * Удаляются chats.db и журналы. Обычный запуск создаёт пустую БД актуального формата. */
#ifndef UTUN_DB_STORAGE_H
#define UTUN_DB_STORAGE_H
#ifdef __cplusplus
extern "C" {
#endif
/* directory — каталог БД. 0 — очищен, отрицательный errno — отказ с подробным логом.
 * Отсутствующие файлы допустимы. При ошибке запуск новой базы запрещён до успешного повтора. */
int db_storage_clear(const char* directory);
#ifdef __cplusplus
}
#endif
#endif
