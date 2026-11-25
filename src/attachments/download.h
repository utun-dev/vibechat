/* Загрузка группового объекта по проверенной ссылке сообщения без обращения к таблицам chat/Kanban.
 * Uasync: copied ref → READ допуска/manifest/index → проверка локального файла worker → общая media_delivery.
 * Переданный callback вызывается ровно один раз, путь заимствован до возврата; inst/arg живут до SQL/media drain.
 * Ключевые контейнеры обрабатывает отдельный group-key backend; этот API выдаёт только открытые файлы. */
#ifndef UTUN_ATTACHMENT_DOWNLOAD_H
#define UTUN_ATTACHMENT_DOWNLOAD_H
#include "store.h"
struct UTUN_INSTANCE;
#ifdef __cplusplus
extern "C" {
#endif
void attachment_group_download(struct UTUN_INSTANCE* inst, const struct attachment_ref* reference,
    void (*done)(void*, int, const char*), void* arg);
/* Каноническое имя копии в media/<group>; для ciphertext используется UUID контейнера. */
int attachment_group_filename(const uint8_t media[16], const char* body, const char* type, char name[256]);
/* Загрузка объекта чата с прогрессом. Для шифрованного файла возвращает проверенный ciphertext. */
void attachment_chat_download(struct UTUN_INSTANCE* inst, const struct attachment_ref* reference,
    void (*done)(void*, int, const char*), void (*progress)(void*, int, int), void* arg);
/* Снять запросы удалённого сообщения. Загрузки остальных ссылок продолжаются. */
void attachment_chat_download_cancel(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32]);
/* Отсоединить запросы до остановки workers и транспорта. Поздние callbacks ещё владеют своими данными. */
void attachment_group_download_detach(struct UTUN_INSTANCE* inst);
/* Uasync: путь участвует в проверке, подготовке или выдаче. Очистка кеша пропускает такую копию. */
int attachment_group_path_busy(struct UTUN_INSTANCE* inst, const char* path);
#ifdef __cplusplus
}
#endif
#endif
