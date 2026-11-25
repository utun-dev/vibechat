/* dm_mailbox_store — SQL непрозрачной очереди S без instance, сети и uasync.
 * Caller открывает WRITE, передаёт скопированный пакет и публикует результат после commit.
 * SQLite-указатели не выходят из функций; ошибка не оставляет частичный результат. */
#ifndef UTUN_DM_MAILBOX_STORE_H
#define UTUN_DM_MAILBOX_STORE_H
#include <sqlite3.h>
#include "dm_core.h"
#ifdef __cplusplus
extern "C" {
#endif

struct dm_mail_receipt_update {
    uint64_t author, recipient, seq; /* Проверенные подписанные ID для ответа после commit. */
    int removed;                    /* Удалена сохранённая pending-копия; повтор оставляет0. */
};
/* В открытом WRITE проверить подписанный ACK, сохранить точную tombstone и удалить совпавшую pending-копию.
 * Receipt без pending допустима; конфликт с прежней receipt отклоняется, первая запись сохраняется.
 * SQLITE_OK/AUTH либо точный положительный SQL-код. out обязателен и пуст при отказе; commit у caller. */
int dm_mail_receipt_apply(sqlite3* db, const uint8_t receipt[DM_RECEIPT_SIZE], struct dm_mail_receipt_update* out);

#define DM_MAIL_BATCH_MAX 32
struct dm_mail_delivery {
    uint8_t recipient[32], author[32]; /* Полные публичные ключи Ed25519 из сохранённого PUT. */
    uint64_t seq; /* Первичный ключ неизменяемой pending-копии. */
    uint8_t* body;                   /* Собственная копия тела, освобождается dm_mail_batch_clear. */
    size_t size;                    /* Размер body, не больше DM_BODY_MAX. */
};
struct dm_mail_batch {
    int64_t now;                    /* Frozen время отправки; следующая попытка через5 секунд. */
    unsigned count;                 /* Число собственных записей, до32; перед claim должно быть0. */
    struct dm_mail_delivery items[DM_MAIL_BATCH_MAX];
};
/* В открытом WRITE скопировать до32 готовых сообщений и перенести retry_at на now+5.
 * batch обязателен, count=0; now=0..INT64_MAX-5. Отправлять items только после commit.
 * SQLITE_OK либо точный SQL-код; при ошибке items очищены, rollback выполняет caller. */
int dm_mail_batch_claim(sqlite3* db, struct dm_mail_batch* batch);
/* Освободить все скопированные тела и обнулить count/items; now сохраняется. NULL допустим.
 * Вызвать после результата commit, в том числе при отказе commit/остановке; без SQL. */
void dm_mail_batch_clear(struct dm_mail_batch* batch);
#ifdef __cplusplus
}
#endif
#endif
