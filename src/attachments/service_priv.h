/* Только runtime общего сервиса и его group-key/send backend, все поля — uasync.
 * group core заимствован до stop; отдельный worker не принадлежит chat_service. */
#ifndef UTUN_ATTACHMENT_SERVICE_PRIV_H
#define UTUN_ATTACHMENT_SERVICE_PRIV_H
#include "service.h"
#include "../utun_instance.h"
struct ak_db_state;
struct ak_file;
struct attachment_send_state;
struct attachment_recovery;
struct group_download;
struct attachment_service {
    struct media_async* workers;
    unsigned owners; /* Биты enum attachment_owner, только включённые publisher. */
    uint64_t owner_epoch[4]; /* Меняется при enable/disable; 0 не выдаётся caller. */
    int closing;
    struct attachment_recovery* recovery; /* Собственный SQL/file/model continuation, stop отсоединяет до free runtime. */
    void* recovery_timer; /* Проверка READY после включения модели/повтор при временном отказе. */
    unsigned recovery_runs; /* Первый проход сразу после активации модели, последующие — с ограничением частоты. */
    void* attachment_timer;
    int attachment_closing, attachment_initialized;
    struct ak_db_state* attachment_db;
    struct attachment_send_state* attachment_sends;
    struct group_download* downloads; /* Запросы модели к общему загрузчику, только uasync. */
    struct ak_file* attachment_files;
    unsigned attachment_file_count;
};
#define ATTACHMENTS(inst) ((inst)->attachments)
#endif
