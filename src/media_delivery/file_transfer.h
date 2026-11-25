/* file_transfer — передача неизменяемого файла между явно заданными узлами.
 * Проверяет допуск self/peer к CHAT-группе; lookup отдельно авторизует объект
 * и возвращает путь. Ключи содержимого, custody и проверка результата принадлежат владельцу.
 * При доступной оптимизации пути операция владеет CM handle до завершения/отмены.
 * Все API/callbacks в uasync; destroy вызывается вне callbacks, до удаления групп.
 * До 16 входящих/исходящих операций суммарно. DATA до 8192 байт; router DATA и controls подписаны,
 * uint64 wire-поля имеют native byte order. Простой 30с завершает операцию ошибкой;
 * при восстановлении пути/простое приём повторяет REQUEST с непрерывного полученного offset.
 * Собственное временное назначение передаёт вызывающий: файл открывается с усечением,
 * после проверки допуска, но до возможного ресурсного отказа запуска. При ошибке/cancel файл не удаляется и отбрасывается caller.
 * Отзыв отменяет операции/таймеры/waiters данной группы; общий ETCP и другие группы сохраняются.
 */
#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stdint.h>
#include <stddef.h>

struct UTUN_INSTANCE;
struct file_transfer;
struct file_transfer_op;

/* Вызывается для каждого REQUEST, включая повторы: проверить права group/peer/object/size.
 * 0 — записан NUL-терминированный путь (<cap байт) неизменяемого файла; иначе отказ.
 * Файл сохранять доступным до завершения выдачи; перед отзывом отменить операции объекта. */
typedef int (*file_transfer_lookup_fn)(void* arg, uint64_t group, const uint8_t peer[32],
                                       const uint8_t object[16], uint64_t size, char* path, size_t cap);
/* Completion асинхронного lookup, только uasync и ровно один раз, включая ошибку/stop.
 * error=0: path — заимствованный NUL-путь <1024 байт до возврата callback; иначе path может быть NULL.
 * request принадлежит file_transfer, provider не читает/не освобождает его. Поздний callback после destroy разрешён:
 * registry уже отсоединён, он только освобождает request без открытия файла/доступа к старому owner. */
typedef void (*file_transfer_lookup_ready_fn)(void* request, int error, const char* path);
/* Uasync: скопировать group/peer/object[16]/size до возврата и запустить проверку владельца, обычно DB READ.
 * Даже отказ запуска вызывает ready(request,error,NULL), допустимо синхронно. Все пути заканчиваются ready ровно один раз.
 * Вызывать ready только в uasync; instance/ua живут до завершения provider DB drain. */
typedef void (*file_transfer_lookup_async_fn)(void* arg, uint64_t group, const uint8_t peer[32],
    const uint8_t object[16], uint64_t size, file_transfer_lookup_ready_fn ready, void* request);
typedef void (*file_transfer_done_fn)(void* arg, int error);

/* Стандартный DM file-transfer instance; inst/его ua/router/группы живут дольше ft. lookup обязателен, arg заимствован.
 * Создаёт router binding 0x37; возвращает owner либо NULL при ошибке. */
struct file_transfer* file_transfer_create(struct UTUN_INSTANCE* inst, file_transfer_lookup_fn lookup, void* arg);
/* Независимый файловый сервис с отдельным свободным router ID и чистой политикой допуска.
 * allowed получает Ed25519 пира; проверяется до открытия и при каждом продолжении I/O.
 * lookup/arg живут до destroy; потоки/владение/лимиты такие же, как у стандартного DM-сервиса. */
struct file_transfer* file_transfer_create_service(struct UTUN_INSTANCE* inst, uint8_t service,
    file_transfer_lookup_fn lookup, int (*allowed)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]), void* arg);
/* Те же владельцы/bindings: новые pending lookups резервируют место в общем лимите16 передач.
 * Проверка уже существующей выдачи не занимает второй transfer slot; одновременно не более16 pending проверок.
 * Повторы одного transfer ID объединяются; destroy/отзыв отсоединяет pending до позднего ready. NULL — отказ создания. */
struct file_transfer* file_transfer_create_async(struct UTUN_INSTANCE* inst, file_transfer_lookup_async_fn lookup, void* arg);
struct file_transfer* file_transfer_create_service_async(struct UTUN_INSTANCE* inst, uint8_t service,
    file_transfer_lookup_async_fn lookup, int (*allowed)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]), void* arg);
/* Снимает binding и синхронно завершает активные pull как cancel; NULL допустим. */
void file_transfer_destroy(struct file_transfer* ft);

/* 0 в done означает точный приём size байт и закрытие файла, а не custody ACK.
 * group ненулевой, ed — Ed25519 владельца, object — 16 байт, size=1..INT64_MAX; done обязателен.
 * destination — собственный временный путь; строка не сохраняется, файл открывается до возврата.
 * Возвращает handle операции; done/arg заимствованы до завершения/отмены.
 * Возврат NULL означает отказ запуска, done не вызывается.
 * Ошибка приёма/таймаут даёт done(-1); успешный приём не включает fsync или проверку хеша.
 * Handle перестаёт существовать перед вызовом done, его нужно обнулить в callback. */
struct file_transfer_op* file_transfer_pull(struct file_transfer* ft, uint64_t group, const uint8_t ed[32],
                                           const uint8_t object[16], uint64_t size, const char* path,
                                           file_transfer_done_fn done, void* arg);
/* Синхронный done(-2), ошибка fclose заменяет результат на -1. NULL допустим; прежний handle больше не использовать. */
void file_transfer_cancel(struct file_transfer_op* op);
/* Отозвать все операции объекта во всех группах (TTL/delete); ft=NULL допустим, object обязателен.
 * callbacks приёма вызываются синхронно с -2, при ошибке fclose — с -1. */
void file_transfer_cancel_object(struct file_transfer* ft, const uint8_t object[16]);
/* Отменить операции именно этого локального пути. Совмещённые endpoint/custody
 * одного UUID имеют разные файлы; отзыв одного не затрагивает другую копию. */
void file_transfer_cancel_path(struct file_transfer* ft, const char* path);

/* После commit изменения состава отменить операции group, для которых self/peer больше не допущены.
 * ft=NULL допустим; done вызывается синхронно после освобождения операции с -2 (либо -1 при ошибке fclose).
 * Новый pull через другую допущенную группу создаёт отдельную операцию; личные данные владельца не удаляются. */
void file_transfer_revalidate_members(struct file_transfer* ft, uint64_t group);

#endif
