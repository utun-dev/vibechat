/* Производные реакции/ответы CHAT, feedback_<channel>.
 * У тройки (message_id,node_id,kind) хранится последнее разрешённое состояние; пустое data — отмена.
 * Единственная подпись и версия находятся в member_log. SQL helpers работают в caller WRITE/worker;
 * runtime публикуется в uasync после durable commit, отдельного сетевого набора FEEDBACK нет. */
#ifndef CHAT_FEEDBACK_H
#define CHAT_FEEDBACK_H

#include <stdint.h>
#include <stddef.h>
#include <sqlite3.h>
#include <openssl/evp.h>

struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

#define CHAT_FEEDBACK_REACTION 1
#define CHAT_FEEDBACK_ANSWER 2
#define CHAT_FEEDBACK_DATA_MAX 2048
#define CHAT_MESSAGE_ID_SIZE 32

struct chat_feedback_record {
    uint8_t message_id[CHAT_MESSAGE_ID_SIZE]; /* Неизменяемый ID исходного сообщения. */
    uint8_t author_ed[32];                    /* Ed25519 автора, идентичность записи. */
    uint64_t ver;                             /* Номер удостоверяющего блока участника,1..INT64_MAX. */
    uint8_t kind;                             /* REACTION либо ANSWER. */
    uint16_t data_len;                        /* Длина полного состояния,0 отменяет. */
    uint8_t data[CHAT_FEEDBACK_DATA_MAX];     /* Канонические байты codec из chat_poll.h. */
    uint8_t signature[64];                    /* Подпись внешнего блока; проверяется по ссылке author_ed/ver. */
};

/* Синхронная подготовка SQL/модели CHAT вне внешней транзакции;0/-1, файловый worker использует prepare/activate. */
int chat_feedback_init(struct UTUN_INSTANCE* inst);
/* SQL-owner bootstrap: создать таблицы CHAT и восстановить активный индекс в caller WRITE; positive SQLite code.
 * Не регистрирует модель и не обращается к runtime/сети; применяется до activate и при полной перестройке. */
int chat_feedback_prepare(sqlite3* db);
/* Uasync после committed prepare: зарегистрировать журнал CHAT без SQL;0/-1. Общий Merkle уже создан. */
int chat_feedback_activate(struct UTUN_INSTANCE* inst);
/* Создать/проверить таблицу текущей проекции;0/отрицательный SQLite-код, без транзакции/публикации. */
int chat_feedback_ensure_channel(sqlite3* db, const char* channel);
/* Прочитать сохранённое состояние, включая неактивное:1 найдено,0 отсутствует,отрицательный SQLite-код при ошибке.
 * out меняется только при успехе1;ошибка bind/step/finalize не считается отсутствием. */
int chat_feedback_get(sqlite3* db, const char* channel, const uint8_t message_id[32], const uint8_t author_ed[32], uint8_t kind,
                      struct chat_feedback_record* out);
/* Только из проверенного журнала в caller WRITE: проверить ссылку и записать проекцию.
 * 1=изменилось,0=повтор/старая версия,отрицательный SQLite-код требует rollback. */
int chat_feedback_project_write(sqlite3* db, const char* channel, const struct chat_feedback_record* record);
/* Локально изменить состояние; транзакция и durable commit принадлежат вызову. 1/0/-1. */
int chat_feedback_set(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message_id[32], uint8_t kind, const uint8_t* data, size_t len,
                      uint64_t* out_ver);
struct member_sync_write_context;
/* Worker/caller WRITE: свежий допуск, следующий блок, проекция/индекс без runtime/BEGIN/COMMIT.
 * self/Ed seed берутся из frozen context; data уже проверены codec. SQLITE_OK/положительная причина.
 * changed=0 при no-op/архиве. history_changed (или NULL) указывает на подтверждение среза.
 * После COMMIT публикует caller по готовому record и history_changed. */
int chat_feedback_set_write(sqlite3* db, const struct member_sync_write_context* context, const char* channel, const uint8_t message_id[32],
                            uint8_t kind, const uint8_t* data, size_t size, struct chat_feedback_record* out, int* changed, int* history_changed);
/* После committed приёма: только уведомление UI/интерпретация, без повторной подписи/рассылки. */
void chat_feedback_received(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_feedback_record* record);
/* После локального durable commit: live блок и уведомление; callback может удалить группу. */
void chat_feedback_publish(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_feedback_record* record, int history_changed);

#ifdef __cplusplus
}
#endif
#endif
