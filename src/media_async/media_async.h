/* media_async — владелец фоновых задач обработки файлов и криптографии.
 * Каждая work выполняется в отдельном pthread; done — в потоке владельца uasync.
 * create/submit/destroy вызываются из этого потока; ua живёт до окончания destroy.
 * destroy ждёт workers и завершает ожидающие done с CANCELLED, пока их данные ещё существуют.
 * Отмена не прерывает work и не откатывает файловые изменения. Примитивы ниже синхронные, подходят для workers. */
#ifndef MEDIA_ASYNC_H
#define MEDIA_ASYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

struct UASYNC;

struct media_async;

typedef void (*ma_work_fn)(void* data);             // рабочий поток; не обращаться к состоянию uasync
typedef void (*ma_done_fn)(void* arg, int err);     // 0=work завершён, -1=отказ запуска/аргументы, -2=остановка владельца

/* Создать владельца задач; NULL при ошибке выделения памяти. */
struct media_async* media_async_create(void);
#define MEDIA_ASYNC_CANCELLED (-2)
#define MEDIA_ASYNC_MAX_TASKS 16
#define MA_WALK_MAX_DEPTH 128
/* Вызывать вне callbacks uasync. Ждёт работы, отменяет posted completion,
 * вызывает ожидающие done и освобождает ma; NULL допустим. Повторный вход во время destroy отклоняется.
 * После возврата ma недействителен. Не вызывать из work или callbacks uasync, в том числе обычного done. */
void media_async_destroy(struct media_async* ma);

/* Передать задачу: ma/ua/work/done обязательны, data/arg могут быть NULL и остаются у вызывающего до done.
 * Лимит 16 включает завершённые workers до обработки done. При отказе done вызывается синхронно ровно один раз;
 * при done=NULL только логируется ошибка. У closing-владельца возврат через done с CANCELLED.
 * err=0 означает завершение work, её собственную ошибку передают через data. После принятия done — ровно один раз. */
void media_async_submit(struct media_async* ma, struct UASYNC* ua,
                        ma_work_fn work, void* data,
                        ma_done_fn done, void* arg);

/* SHA256 обычного файла path (UTF-8); hash_out — 32 байта, обнуляется при ошибке. 0/-1. */
int  ma_sha256_file(const char* path, uint8_t hash_out[32]);
/* Подписать len байт Ed25519 ключом 32 байта; sig_out — 64 байта, обнуляется при ошибке.
 * data=NULL допустим только при len=0. 0/-1. */
int  ma_sign_block(const uint8_t* ed25519_privkey,
                   const uint8_t* data, size_t len, uint8_t sig_out[64]);
/* Копировать regular src в отсутствующий/regular dst через соседний temp; 0/-1. Пути UTF-8, <4096 байт,
 * причём dst вместе с суффиксом .tmp.<32 hex> и NUL должен помещаться в 4096 байт.
 * Один существующий файл (включая ./ и hardlink) — успешный no-op. Конечные ссылки назначения отклоняются.
 * Ошибка сохраняет src и прежний dst; src/каталоги не менять во время вызова. POSIX результат имеет права 0600.
 * Публикация происходит внутри вызова; последующая отмена media_async не откатывает копирование. */
int  ma_copy_file(const char* src, const char* dst);
/* Размер regular-файла path (UTF-8): 0..INT64_MAX байт; -1 при отсутствии/ошибке/другом типе. */
int64_t ma_file_size(const char* path);
/* Заполнить 16 криптографических случайных байт; 0/-1. При отказе выход обнуляется; NULL — ошибка. */
int ma_uuid(uint8_t uuid_out[16]);

/* Рекурсивный обход каталога: для каждого regular-файла вызывается cb(full_path, size).
 * Пути UTF-8 с '/', <4096 байт; глубина не более MA_WALK_MAX_DEPTH. Ссылки/reparse points пропускаются,
 * включая корень. 0 — полный обход, -1 — неверные аргументы, отсутствие корня, I/O или превышение лимита.
 * Callback синхронный, full_path заимствован до его возврата. При ошибке прежние callbacks не откатываются. */
typedef void (*ma_dir_walk_cb)(void* arg, const char* full_path, int64_t size);
int ma_dir_walk(const char* dir, ma_dir_walk_cb cb, void* arg);

#ifdef __cplusplus
}
#endif
#endif
