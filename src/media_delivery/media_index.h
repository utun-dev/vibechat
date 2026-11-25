/* media_index — регистрация файлов канала и их блоков в SQLite для последующей раздачи и сообщений чата.
 * Порядок: init → register_db_async → callback с UUID, хешем и подписями блоков; затем можно анонсировать файл.
 * Track WRITE фиксирует назначение до файлового I/O; после I/O другая WRITE фиксирует все блоки.
 * worker/ma/ua заимствованы до результата, callback приходит в uasync. Legacy register_async оставлен для RAM/SQL owner.
 * Назначения учитываются в media_preparations до запуска worker; неуспешная подготовка оставляет их для удаления канала.
 * Перед delete_channel вызывающий должен дождаться всех своих workers/completions; функция их сама не отменяет.
 * Результат callback заимствован только до возврата. result_free освобождает block_ids/block_sigs, но не саму структуру. */
#ifndef MEDIA_INDEX_H
#define MEDIA_INDEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <sqlite3.h>
#include "../../lib/db_async.h"

struct media_async;
struct UASYNC;
struct media_index_result;

#define MEDIA_ID_SIZE   16
#define MEDIA_HASH_SIZE 32
#define MEDIA_PATH_MAX 1024
#define MEDIA_BASE_MAX 512

/* Пути UTF-8, пределы включают NUL. location — непустой относительный путь с '/'; '.'/'..' запрещены.
 * Преобразования нормализуют разделители/компоненты '.' полного пути, отклоняют '..' и обрезание.
 * Проверяется строковая принадлежность базе с тем же написанием, а не переходы через ссылки файловой системы. 0/-1. */
int media_index_location(const char* path, const char* base, char location[MEDIA_PATH_MAX]);
int media_index_path(const char* base, const char* location, char path[MEDIA_PATH_MAX]);
/* Общая база БД в ui_state: set создаёт отсутствующую запись, существующая должна совпадать после нормализации.
 * get возвращает проверенную базу; 0/-1. Смена базы при работающем ядре не поддерживается. */
int media_index_set_base(sqlite3* db, const char* base);
int media_index_get_base(sqlite3* db, char base[MEDIA_BASE_MAX]);
/* SQL-only caller snapshot: прочитать и нормализовать media_base, без runtime/I/O/границ транзакции.
 * db/base обязательны; base вмещает MEDIA_BASE_MAX байт, при любой ошибке пуст.
 * SQLITE_OK либо точный положительный prepare/step/finalize код; отсутствующая запись — NOTFOUND,
 * неверный TEXT/путь/встроенный NUL — MISMATCH, превышение буфера — TOOBIG. */
int media_index_base_read(sqlite3* db, char base[MEDIA_BASE_MAX]);
/* SQL-only: создать ui_state/отсутствующую media_base либо проверить неизменность нормализованного base.
 * Не выполняет runtime/I/O/BEGIN/COMMIT; caller завершает свою WRITE. Строка заимствована до возврата.
 * SQLITE_OK либо точный положительный schema/prepare/bind/step/finalize/readback код;
 * другой сохранённый base — CONSTRAINT, аргументы/синтаксис — MISUSE/MISMATCH, длина — TOOBIG. */
int media_index_base_set(sqlite3* db, const char* base);
/* Учесть назначение до файловой подготовки. Ошибка подготовки оставляет запись для delete_channel;
 * успешная регистрация файла удаляет её в той же транзакции. Сам файл здесь не меняется. 0/-1. */
int media_index_track_preparation(sqlite3* db, const char* chat_id, const char* path, const char* base);
/* SQL-owner: тот же INSERT preparation с положительным SQLite-кодом; не открывает/закрывает транзакцию.
 * Использовать внутри WRITE перед запуском файлового worker; строки заимствованы до возврата. */
int media_index_track_preparation_write(sqlite3* db, const char* chat_id, const char* path, const char* base);
/* Uasync: копирует chat/path/base, WRITE затем один done(arg,result), включая FULL/OOM/close.
 * RW owner не использует legacy (можно NULL); readonly owner/RAM сохраняют общий legacy writer.
 * Caller проверяет свой lifetime в done до запуска I/O. При legacy rollback с открытой транзакцией приходит IOERR,
 * caller останавливает legacy database owner. worker/legacy и arg живут до callback. */
void media_index_track_preparation_async(struct db_async* worker, sqlite3* legacy, const char* chat,
    const char* path, const char* base, db_async_done_fn done, void* arg);
/* READ перед записью: SQLITE_CONSTRAINT для пути с другими байтами. file=NULL требует свободный путь. */
int media_index_path_access_read(sqlite3* db, const char* path, const char* base, const struct media_index_result* file);

struct media_index_result {
    uint8_t  media_id[16];    // случайный идентификатор целого файла
    uint8_t  content_hash[32]; // SHA256 полного содержимого
    int64_t  file_size;      // полный размер файла, байты
    int64_t  block_size;     // размер обычного блока, байты; последний может быть короче
    int      num_blocks;    // число блоков и элементов block_ids/block_sigs
    uint8_t* block_ids;      // num_blocks * 16
    uint8_t* block_sigs;     // num_blocks * 64
};

// ── Инициализация таблицы ──

// Создать media_files, media_preparations и индексы, сохранив записи; ошибка любого DDL даёт -1, успех 0.
int media_index_init(sqlite3* db);

// ── Регистрация файлов и блоков ──

/* Атомарно записать все блоки файла; 0/-1, result остаётся у вызывающего, block_sigs здесь не используются.
 * holder — полный публичный Ed25519 локального держателя, входит в подписываемые метаданные.
 * ed25519_privkey — 32 байта для авторской node_sign; NULL — скачанный файл, node_sign новой записи нулевая.
 * chat_id — десятичный ID канала; dest_path — готовый файл, media_base — база для относительного location.
 * Все блоки записываются через SAVEPOINT: ошибка откатывает регистрацию; внешнюю транзакцию завершает вызывающий.
 * SQLite может сама откатить всю транзакцию при тяжёлой ошибке. Уже выполненные файловые операции не откатываются.
 * Повтор с теми же метаданными/путём допустим, конфликт block_id даёт ошибку; прежняя подпись сохраняется.
 * Выполняется в uasync, сама функция не копирует/не хеширует файл и не анонсирует блоки. */
int media_index_commit(
    sqlite3* db, const struct media_index_result* result,
    const uint8_t holder[32], const uint8_t* ed25519_privkey,
    const char* chat_id, const char* dest_path, const char* media_base);

/* Зарегистрировать один скачанный блок: location — канонический путь относительно общей базы; 0/-1.
 * file_size/chunk_size >0, chunk/offset >=0, блок целиком помещается в файл.
 * node_sign новой записи — нули (при отдаче не проверяется); существующая подпись при совпадающем повторе сохраняется.
 * Для целого файла использовать commit с ключом NULL; одиночные вызовы не образуют общую транзакцию. */
int media_index_register_downloaded(sqlite3* db, const uint8_t* media_id, const uint8_t* block_id,
                                    const uint8_t* content_hash, const char* chat_id,
                                    const char* location, const uint8_t holder[32],
                                    int64_t file_size, int64_t chunk_size, int chunk, int64_t offset);
/* Подготовить и зарегистрировать файл; пути/ключ/chat_id копируются до возврата, cb_arg заимствован до callback.
 * При copy_file!=0 копирует src_path в dest_path; при 0 dest_path уже должен содержать готовый файл.
 * src/dest < MEDIA_PATH_MAX, media_base < MEDIA_BASE_MAX, chat_id <64 байт; dest проверяется до запуска worker.
 * Файлы и db/ma/ua должны жить до callback. Callback может быть синхронным при отказе запуска.
 * err=0 даёт result только на время callback; иначе result=NULL. Сохранить нужные поля/массивы отдельной копией. */
void media_index_register_async(
    struct media_async* ma, struct UASYNC* ua, sqlite3* db,
    const uint8_t holder[32], const uint8_t* ed25519_privkey,
    const char* chat_id,
    const char* src_path, const char* dest_path, int copy_file,
    const char* media_base,
    void (*cb)(void* arg, int err, const struct media_index_result* result),
    void* cb_arg);

/* Uasync: copied inputs, WRITE track → file worker → WRITE commit → один callback с прежним result-форматом.
 * valid(arg) опционален, вызывается только в uasync до SQL dispatch/I/O и после них; stop возвращает0.
 * RW legacy=NULL допустим; failed/closed RW не переключается на legacy. При valid=NULL ma живёт до callback.
 * err=0 только после commit и живого valid; иначе err<0, result=NULL. Файлы при отказе/неоднозначном IOERR
 * сохраняются с durable preparation/index для последующей уборки. Секрет копируется и очищается при завершении. */
void media_index_register_db_async(struct media_async* ma, struct UASYNC* ua, struct db_async* worker, sqlite3* legacy,
    const uint8_t holder[32], const uint8_t* key, const char* chat, const char* source, const char* destination, int copy,
    const char* base, int (*valid)(void*), void (*done)(void*, int, const struct media_index_result*), void* arg);

/* SQL-owner/WRITE: все блоки и снятие preparation атомарно в транзакции caller.
 * Аргументы как commit; result/строки/ключ заимствованы до возврата. SQLITE_OK/положительный код, без runtime/I/O/BEGIN/COMMIT. */
int media_index_commit_write(sqlite3* db, const struct media_index_result* result,
    const uint8_t holder[32], const uint8_t* key, const char* chat, const char* destination, const char* base);

// ── Вспомогательные операции ──

// Заполнить 16 байт идентификатора; 0/-1. При ошибке RNG выход обнулён, использовать его нельзя.
int  media_index_generate_uuid(uint8_t uuid_out[16]);

// Освободить только массивы собственного result и обнулить их указатели; callback-result освобождает сам модуль.
void media_index_result_free(struct media_index_result* result);

// Выбрать размер блока в байтах: 10..25 МиБ для непустого файла, 0 для file_size=0.
int  media_calc_block_size(uint64_t file_size);

// ── Локальное удаление канала ──

/* Удалить медиа канала (локальное удаление канала): стереть файлы на диске
 * (media_base + location) и строки media_files с chat_id. Вызывать вне внешней транзакции и после остановки writers.
 * Общий файл другого канала сохраняется; ENOENT — успех. При ошибке сохраняются записи для повторной очистки.
 * Удаление файлов необратимо: возможен частичный результат. 0 — всё очищено, -1 — ошибка/неполная очистка. */
int  media_index_delete_channel(sqlite3* db, const char* chat_id, const char* media_base);

#ifdef __cplusplus
}
#endif
#endif
