/* Единственный дисковый поток модуля. submit/done выполняются в uasync, work —
 * в pthread. Очередь ограничена; completion резервируется до передачи работы.
 * Для обязательной очистки reserve выполняется до открытия файлов. destroy в uasync
 * ждёт работ и доставляет done, включая поставленные ими cleanup jobs; допустим из внешней команды.
 * Отмена completion удаляет task и из общей очереди, и из ещё не исполненной части batch uasync.
 * data/arg заимствованы до done; worker освобождает только собственную оболочку работы.
 * ua/work/done и worker обязательны; ua живёт до конца destroy. Останавливать worker из done нельзя. */
#ifndef MEMBER_FILES_WORKER_H
#define MEMBER_FILES_WORKER_H
struct UASYNC;
struct mf_worker;
struct mf_work;
typedef void (*mf_work_fn)(void* arg);
/* error=0 — work завершён, результат находится в data; -1 — отказ submit (синхронно);
 * MF_WORK_CANCELLED — ожидающая работа отменена, work не вызывался; done отложен и приходит ровно один раз. */
#define MF_WORK_CANCELLED (-2)
typedef void (*mf_done_fn)(void* arg, int error);
/* Создать pthread/mutex/condition; возвращает owner либо NULL, без дискового I/O. */
struct mf_worker* mf_worker_create(struct UASYNC* ua);
/* Резерв оболочки и completion, NULL при OOM. Сам по себе не занимает очередь/не задаёт worker. */
struct mf_work* mf_worker_reserve(void);
/* Освободить только ещё не переданный в submit_reserved резерв; NULL допустим. */
void mf_worker_unreserve(struct mf_work* work);
/* До64 обычных задач, включая ожидающие done. При отказе done(-1) синхронно, work не вызывается. */
void mf_worker_submit(struct mf_worker* worker, mf_work_fn work, void* data, mf_done_fn done, void* arg);
/* Uasync: отменить ещё не начатые обычные задачи с этим data, вернуть их число. data живёт до done.
 * Отмена и начало work сериализованы mutex. Начатый work завершается; reserved cleanup не отменяется. */
unsigned mf_worker_cancel(struct mf_worker* worker, void* data);
/* Передать резерв worker без проверки лимита64: только обязательные cleanup continuations.
 * После вызова резерв нельзя освобождать/использовать повторно. Work выполняется FIFO, done ровно один раз. */
void mf_worker_submit_reserved(struct mf_worker* worker, struct mf_work* reserved,
                               mf_work_fn work, void* data, mf_done_fn done, void* arg);
// После quiesce владельца в uasync, вне собственных done: дождаться работ и cleanup continuations, не останавливая pthread.
// Не запускает uasync рекурсивно; порядок done при drain не обещается. NULL допустим.
void mf_worker_drain(struct mf_worker* worker);
/* Drain, затем join и освобождение. Работы не прерываются и получают обычный done(0); NULL допустим. */
void mf_worker_destroy(struct mf_worker* worker);
#endif
