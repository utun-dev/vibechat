/* Общий SQL-каталог вложений: неизменяемые объекты, ссылки сообщений и задания отправки.
 * Вызывает владелец SQLite, включая DB worker; WRITE требует транзакцию caller.
 * Подписи/предметные права проверяет модель сообщения до observe/publish в той же WRITE.
 * Файлового I/O, runtime и собственных BEGIN/COMMIT здесь нет. Результаты — SQLite-коды с логом.
 * ID задания устойчив к повтору после неизвестного результата commit; сообщение связывается ровно один раз.
 * Manifest содержит только публичное описание передаваемых байтов; пути/ключи в него не включаются. */
#ifndef UTUN_ATTACHMENT_STORE_H
#define UTUN_ATTACHMENT_STORE_H
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>
#ifdef __cplusplus
extern "C" {
#endif
enum attachment_owner { ATTACHMENT_OWNER_CHAT = 1, ATTACHMENT_OWNER_KANBAN, ATTACHMENT_OWNER_DM };
enum attachment_scope { ATTACHMENT_SCOPE_GROUP = 1, ATTACHMENT_SCOPE_DM };
enum attachment_job_state { ATTACHMENT_JOB_PREPARING = 1, ATTACHMENT_JOB_READY, ATTACHMENT_JOB_PUBLISHED,
    ATTACHMENT_JOB_FAILED, ATTACHMENT_JOB_CANCELLED };
enum attachment_crypto { ATTACHMENT_CRYPTO_NONE, ATTACHMENT_CRYPTO_GROUP, ATTACHMENT_CRYPTO_DM };
#define ATTACHMENT_MANIFEST_MAX 16384
#define ATTACHMENT_REQUEST_MAX 16384
#define ATTACHMENT_PATH_MAX 1024
struct attachment_ref {
    enum attachment_owner owner; /* Модель сообщения; не вид передачи файла. */
    char space[65]; /* Полный ключ группы либо канонический ID беседы. */
    uint8_t message[32], object[16]; /* Постоянный ID сообщения и UUID вложения. */
};
struct attachment_object {
    enum attachment_scope scope; /* Общая группа или E2E-беседа. */
    char space[65]; /* Область доступа, определённая scope. */
    uint64_t size; /* Размер передаваемых байтов. */
    uint8_t author[32]; /* Полный Ed25519 автора, входит в неизменяемое описание. */
    uint8_t id[16], hash[32]; /* UUID объекта; SHA256 передаваемых байтов, включая ciphertext при шифровании. */
    enum attachment_crypto crypto; /* Политика существующего group/DM backend. */
    const void* manifest; size_t manifest_size; /* Заимствованное проверенное описание, без локальных путей/ключей. */
};
struct attachment_job {
    uint8_t id[16], namespace_token[32]; /* UUID задания и сохранённая идентичность/поколение адресата. */
    enum attachment_owner owner;
    char space[65]; /* Адресат, неизменен после приёма задания. */
    uint8_t author[32]; /* Полный Ed25519 подписанта задания. */
    const char* source; /* Заимствованный путь источника, NUL, менее ATTACHMENT_PATH_MAX. */
    const void* request; size_t request_size; /* Скопированный opaque intent публикации, интерпретирует владелец. */
};
/* Создать/проверить общие таблицы в caller WRITE; используется общим bootstrap. */
int attachment_store_prepare_write(sqlite3* db);
/* Зафиксировать PREPARING; точный повтор UUID/всех входов допустим, подмена — CONSTRAINT. */
int attachment_store_begin_write(sqlite3* db, const struct attachment_job* job);
/* После файлового I/O: зарегистрировать неизменяемый объект и PREPARING→READY одной WRITE.
 * path — готовый собственный файл, публикация/проверка файла уже выполнены caller. Повтор того же READY допустим. */
int attachment_store_ready_write(sqlite3* db, const uint8_t job[16], const struct attachment_object* object, const char* path);
/* В транзакции публикации сообщения: READY→PUBLISHED и ссылка атомарны с журналом модели.
 * Опубликованное задание разрешает только точный повтор того же сообщения/объекта. */
int attachment_store_publish_write(sqlite3* db, const uint8_t job[16], const struct attachment_ref* reference);
/* В caller WRITE зарегистрировать объект и его ссылку. Повтор не меняет локальный кеш.
 * Если conflict задан, при несовпадении существующего описания выставляет 1 и возвращает SQLITE_OK без записи ссылки.
 * При conflict=NULL несовпадение возвращает SQLITE_CONSTRAINT. Ошибки SQLite всегда возвращаются вызывающему. */
int attachment_store_observe_write(sqlite3* db, const struct attachment_object* object, const struct attachment_ref* reference,
    int* conflict);
/* Локальное освобождение ссылки; историю/файл не удаляет. GC отдельно учитывает остальные ссылки и active holds. */
int attachment_store_release_write(sqlite3* db, const struct attachment_ref* reference);
/* READ: SQLITE_OK для живой ссылки, SQLITE_NOTFOUND для удалённой/отсутствующей. */
int attachment_store_live_reference_read(sqlite3* db, const struct attachment_ref* reference);
/* READ: состояние задания и связанный ID сообщения. NOTFOUND при отсутствии, выходы обнулены при ошибке. */
int attachment_store_job_read(sqlite3* db, const uint8_t job[16], enum attachment_job_state* state, uint8_t message[32]);
/* READ перед новой публикацией: intent должен принадлежать автору/модели и текущему persistent-поколению адресата. */
int attachment_store_authorize_read(sqlite3* db, const uint8_t job[16], enum attachment_owner owner,
    const char* space, const uint8_t author[32], const uint8_t namespace_token[32]);
/* READ: GROUP-объект имеет committed ссылку проверенного сообщения —1, ещё не опубликован —0,
 * ошибка — отрицательный SQLite-код с логом. released не отзывает прежнюю сетевую публикацию. */
int attachment_store_public_read(sqlite3* db, const char* space, const uint8_t object[16]);
#ifdef __cplusplus
}
#endif
#endif
