/* Локальная история DM: поколения, скрытые беседы и точные отпечатки удалённых сообщений.
 * Instance-операции выполняются в uasync; *_read/write принимают SQLite worker без runtime.
 * init вызывается после создания DM-таблиц. Транзакцией SQL-helpers управляет вызывающий.
 * Удаление скрывает беседу и сохраняет её ключи/счётчики в dm_conversations; show открывает её снова.
 * Поколение передавать в отложенные операции и проверять current перед применением результата.
 * Беседой/seq владеет dm_core, файлами/загрузками — dm_media; этот модуль связывает их удаление.
 * Аргументы заимствованы на время вызова; самостоятельные транзакции выполняются без внешней транзакции. */
#ifndef DM_HISTORY_H
#define DM_HISTORY_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct sqlite3;
/* Создать dm_history, dm_deleted_messages и dm_local_files; сбросить busy после restart.
 * 0=успех, -1=ошибка. Вызывается из dm_core_init; повторять при живых workers нельзя: они ещё держат пути.
 * inst и его topo_sqlite_db обязательны; частично созданные таблицы при ошибке остаются. */
int dm_history_init(struct UTUN_INSTANCE* inst);
/* Поколение существующей беседы: 1 до первого удаления; 0 при отсутствии/ошибке.
 * Скрытая беседа также имеет поколение. conv — десятичный ID из dm_conversations. */
uint64_t dm_history_generation(struct UTUN_INSTANCE* inst, const char* conv);
/* SQL-owner: прочитать generation/hidden существующей беседы. SQLITE_OK, NOTFOUND либо положительный код ошибки.
 * Выходы обязательны, при ошибке0; statements закрываются, runtime/границы транзакции не используются. */
int dm_history_state_read(struct sqlite3* db, const char* conv, uint64_t* generation, int* hidden);
/* 1 только для существующей видимой беседы с указанным ненулевым поколением; иначе 0, включая ошибку SQL. */
int dm_history_current(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation);
/* Снять hidden, не меняя поколение/счётчики. 0=успех (также при отсутствии строки), -1=ошибка SQL.
 * inst/db/conv обязательны; запись dm_history до первого удаления не требуется. */
int dm_history_show(struct UTUN_INSTANCE* inst, const char* conv);
/* В WRITE снять hidden у conv, не меняя поколение. SQLITE_OK/положительный код SQLite;
 * отсутствие строки допустимо. db/conv обязательны; commit выполняет caller. */
int dm_history_show_write(struct sqlite3* db, const char* conv);
/* body/len — полное каноническое тело, dir: 0=входящее, 1=исходящее. Подпись проверяет вызывающий.
 * 1=точный удалённый повтор по SHA256 тела, 0=нет записи, -1=конфликт/ошибка.
 * media_receipt может быть NULL либо буфером DM_MEDIA_RECEIPT_SIZE: сначала обнуляется,
 * затем для точного повтора копируется сохранённая файловая квитанция, если она есть. inst/db обязательны. */
int dm_history_deleted(struct UTUN_INSTANCE* inst, const uint8_t* body, size_t len, int dir, uint8_t* media_receipt);
/* SQL/worker: проверить точный удалённый повтор полного body/len. Подпись проверяет caller.
 * SQLITE_OK и deleted=0/1, иначе положительный код; конфликт тела даёт SQLITE_CONSTRAINT.
 * deleted обязателен, media_receipt — NULL либо DM_MEDIA_RECEIPT_SIZE байт. При ошибке выходы пусты.
 * Вызывать в согласованной транзакции; runtime не используется, все statements закрываются. */
int dm_history_deleted_read(struct sqlite3* db, const uint8_t* body, size_t len, int* deleted, uint8_t* media_receipt);
/* Удалить видимую беседу текущего generation<INT64_MAX; DM/core/media должны быть запущены.
 * Самостоятельная транзакция: отпечатки, удаление истории/outbox/медиа, hidden=1, generation+1.
 * 0 после commit, отмены invite/transfer и запуска уборки; -1 при отказе с rollback SQL-изменений.
 * Это не ожидание workers: они сохраняют busy пути и отбрасывают результат старого поколения.
 * Файлы убираются отдельно по durable манифесту после release; ошибку удаления повторяет media.
 * Квитанция файла сохраняется только если уже была: удаление не подтверждает скачивание.
 * Встречный узел, его история и промежуточные S/M-копии этим вызовом не меняются. */
int dm_history_delete(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation);
/* Worker/WRITE: сохранить точные tombstones/файловый манифест, удалить историю/очереди, скрыть беседу и повысить поколение.
 * root — frozen каталог endpoint; ожидается точное текущее видимое generation. SQLITE_OK/положительный код.
 * Не использует runtime/I/O/BEGIN/COMMIT. После успешного commit caller отменяет старые workers/снимки и запускает cleanup. */
int dm_history_delete_write(struct sqlite3* db, const char* root, const char* conv, uint64_t generation);
#ifdef __cplusplus
}
#endif
#endif
