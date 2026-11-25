/* Создание таблиц личных бесед по общему каталогу db_schema, без миграций.
 * Несовпадение существующей структуры — ошибка; данные не преобразуются.
 * Savepoint в uasync-потоке откатывает создание таблиц при ошибке. */
#ifndef UTUN_DM_SCHEMA_H
#define UTUN_DM_SCHEMA_H
#include <stdint.h>
struct UTUN_INSTANCE;

/* 0/-1: проверить/создать таблицы и начальный счётчик автора. */
int dm_schema_init(struct UTUN_INSTANCE* inst);
/* 0/-1: создать mailbox pending/receipts без привязки к маршруту; сохранить имеющиеся записи.
 * Вызывается dm_mailbox_init независимо от dm_core_init. */
int dm_mailbox_schema_init(struct UTUN_INSTANCE* inst);
/* Таблицы файлов endpoint/S/M и квитанций устройств; до запуска media owners. 0/-1. */
int dm_media_schema_init(struct UTUN_INSTANCE* inst);
/* 0/-1: прочитать следующий seq автора. Резервирование выполняет take в той же
 * транзакции, что запись сообщения; до commit тело нельзя передавать в сеть. */
int dm_sequence_next(struct UTUN_INSTANCE* inst, uint64_t* seq);
/* 0/-1: увеличить счётчик с seq-1 до seq. Вызывать внутри транзакции отправки. */
int dm_sequence_take(struct UTUN_INSTANCE* inst, uint64_t seq);
#endif
