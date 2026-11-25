/* Внутреннее состояние member_files/, все списки/таймеры принадлежат uasync.
 * Disk worker последовательно владеет storage и своими снимками I/O; completion возвращает управление uasync.
 * Start: worker/reserved cleanup → storage → router/tick → initialize. Stop: quiesce → drain → storage cleanup → join/free. */
#ifndef MEMBER_FILES_PRIVATE_H
#define MEMBER_FILES_PRIVATE_H
#include "member_files.h"
#include "storage.h"
#include "worker.h"
#include "../routing_layer/conn_mgr.h"
#include "../../lib/ll_queue.h"
struct ETCP_ROUTER_CONN;
struct mf_job;
struct mf_transfer;
struct mf_local;
struct member_files {
    struct UTUN_INSTANCE* inst; // Заимствованное ядро; ua/router/группы живут до destroy.
    struct mf_storage* storage; // Собственный storage; освобождается worker после всех stream cleanup.
    struct mf_worker* disk; // Единственный FIFO дискового I/O; uasync владеет handle.
    struct mf_work* cleanup; // Резерв освобождения storage, создан до начала I/O.
    struct mf_pending* pending; // Клиентские команды, до16; handle удаляется до callback.
    struct mf_job* jobs; // До32 серверных queued/worker/reply задач, включая sweep.
    struct mf_job* worker; // Текущая серверная задача; не включает startup/клиентские stream I/O.
    struct mf_transfer* transfers; // Собственные client/server streams и их отложенная очистка.
    struct mf_local* locals; // До32 сообщений self→self, общий FIFO запросов/ответов.
    void* tick; // Таймер1с: pending/stream timeout, повторы, sweep/resume.
    void* pump; // Отменяемый call_soon серверного pump, не worker.
    unsigned pending_count, job_count, transfer_count; // Числа pending/server jobs/client streams.
    unsigned server_transfer_count; // Серверные streams, независимо от client limit16.
    unsigned local_count; // Длина локального FIFO.
    int closing, used_storage, sweep_due; // Quiesce, необходимость периодического sweep и ожидающий sweep.
    int storage_status; // Uasync:BUSY до startup completion, затем MF_OK либо постоянная ошибка.
    int init_result; // Worker пишет результат initialize; читать только после completion/drain.
};
/* Запросить остановку streams группы (0 — всех), дождаться их I/O через worker drain отдельно. */
void mf_transfers_quiesce(struct member_files* files, uint64_t group);
/* 1 — DATA помещается в кольцо либо относится к уже неактивному потоку;0 — router должен удержать пакет. */
int mf_transfer_receive_ready(struct member_files* files, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], int kind, uint64_t id);
/* Обработать проверенный STREAM-пакет; group/peer — end-to-end identity router. */
void mf_transfer_dispatch(struct member_files* files, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* message);
/* Завершить потоки пары с IO_ERROR при reset/close router; прочие пары не затрагиваются. */
void mf_transfers_peer_down(struct member_files* files, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE]);
/* Отменить передачи группы с отозванным членством/ролью/owner; начатый I/O завершается перед обязательной очисткой. */
void mf_transfers_revalidate(struct member_files* files, uint64_t group);
/* Планировать готовые pumps / обработать прогресс и таймауты; все функции вызываются в uasync. */
void mf_transfers_wake(struct member_files* files);
void mf_transfers_tick(struct member_files* files, uint64_t now);
/* Отправить signed+encrypted либо поставить local:0 принято,1 backpressure,-1 ошибка. message копируется.
 * На удалённом backpressure blocked получает заимствованный канал для подписки на освобождение очереди. */
int mf_send(struct member_files* files, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* message, struct ETCP_ROUTER_CONN** blocked);
/* 1/0: enabled, storage-role либо собственные avatars, действующие peer/owner, запись только своего UID. Без I/O. */
/* Канонический запрос к собственным avatars:1/0; не проверяет членство. */
int mf_avatar_self_request(const uint8_t server[MF_ADDRESS_SIZE], const struct mf_message* message);
/* Uasync: членство сервера и storage-role либо исключение собственных avatars;1/0, без I/O. */
int mf_storage_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t server[MF_ADDRESS_SIZE], const struct mf_message* message);
int mf_server_allowed(struct member_files* files, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* message);
/* Запросить путь группы к точному ключу через CM, если числовой индекс однозначен. */
int mf_open_cm(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[MF_ADDRESS_SIZE],
               conn_mgr_cb_t cb, void* arg, struct CONN_MGR_HANDLE** handle);
/* Пробудить заблокированный router receive и голову local FIFO, если появились места. */
void mf_receive_resume(struct member_files* files);
/* Поставить копию ответа с указанным статусом в ограниченную очередь; отказ логируется. */
void mf_reply(struct member_files* files, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* request, int status);
#endif
