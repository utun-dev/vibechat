/* Подписанный профиль участника: worker читает/проверяет и меняет JSON в транзакции,
 * uasync после commit публикует права и уведомляет MEMBERS. Имена/опции/аватар копируются
 * до submit; живой instance, TOPO_GROUP и их пулы не передаются SQL-работе.
 * Для составного bootstrap/JOIN используйте SQL read/update в транзакции caller. */
#ifndef UTUN_CHAT_PROFILE_H
#define UTUN_CHAT_PROFILE_H
#include "chat_core.h"
#include "member_sync.h"
#include "../../lib/db_async.h"
#ifdef __cplusplus
extern "C" {
#endif
struct group_policy_cache;
struct chat_profile_identity {
    uint64_t node; /* Локальный ID, привязанный к публичному X25519. */
    uint8_t x[32], ed[32]; /* Только публичные ключи; READ не требует секретов. */
};
struct chat_profile_record {
    char json[MS_USERINFO_MAX + 1]; /* Собственные проверенные байты userinfo. */
    uint8_t signature[64]; /* Подпись JSON/версии в конкретном JOIN. */
    uint64_t version; /* Положительная версия профиля; результат только при полном успехе. */
};
/* SQL/worker: проверить JOIN/подпись своего профиля в снимке caller; без runtime.
 * SQLITE_OK/положительный SQLite-код, out меняется только при успехе. Отсутствие =>SQLITE_NOTFOUND. */
int chat_profile_read(sqlite3* db, const struct chat_profile_identity* identity, const char* channel,
    struct chat_profile_record* out);
struct chat_profile_change {
    struct chat_profile_identity identity;
    uint8_t private_key[32]; /* Ed25519 seed caller; очистить после завершения работы. */
    uint64_t now; /* Замороженное Unix-время в микросекундах. */
    const char *channel, *name, *file; /* Заимствованы на время SQL update; file нужен только для avatar. */
    const struct chat_profile_option* options; /* Частичная замена: NULL value удаляет ключ. */
    size_t count;
    int avatar; /* 1 — заменить avatar_server/avatar_file; нулевой ключ server удаляет оба. */
    uint8_t server[32];
    unsigned changes; /* Выход MS_APPLY_*; ещё не committed. */
    struct group_policy_cache* cache; /* Выход caller: publish после commit либо free после rollback/stop. */
};
/* SQL/worker, caller WRITE: свежая проверка member, patch JSON, подпись, MEMBERS/derived index.
 * SQLITE_OK/положительный код; отсутствующий полный member =>SQLITE_NOTFOUND. Без BEGIN/COMMIT/runtime.
 * changes/cache обнуляются до работы; no-op даёт0/NULL. Caller освобождает cache на любом исходе. */
int chat_profile_update_write(sqlite3* db, struct chat_profile_change* change);
struct chat_profile_batch;
/* SQL/worker, caller WRITE: применить change ко всем своим полным member-профилям.
 * channel/changes/cache в template игнорируются; отсутствие собственного JOIN пропускается.
 * SQLITE_OK/положительный код. out принадлежит caller даже при ошибке, освободить batch_free.
 * После commit activate заменяет все снимки без callbacks, notify отдельно рассылает изменения. */
int chat_profile_batch_write(sqlite3* db, const struct chat_profile_change* change, struct chat_profile_batch** out);
void chat_profile_batch_activate(struct UTUN_INSTANCE* inst, struct chat_profile_batch* batch);
void chat_profile_batch_notify(struct UTUN_INSTANCE* inst, const struct chat_profile_batch* batch);
/* Любой поток: освободить ещё не опубликованные снимки/массив; NULL допустим. */
void chat_profile_batch_free(struct chat_profile_batch* batch);
/* Uasync: атомарно сменить identity/NODEINFO и собственные профили во всех группах.
 * name копируется; NULL означает только обновить member-профили текущим именем.
 * 0=принято,-1=синхронный отказ; необязательный done ровно один раз, успех только после commit. */
int chat_profile_name_submit(struct UTUN_INSTANCE* inst, const char* name, db_async_done_fn done, void* arg);
/* Uasync: скопировать options и сохранить одной WRITE.0=принято,-1=отказ до принятия с логом.
 * done(arg,result) необязателен, при наличии вызывается ровно один раз, включая синхронный отказ.
 * Успех done означает durable commit; принятие ещё не означает сохранение. count>0, имена без reserved ключей.
 * NULL value удаляет ключ. inst/arg живут до done/drain, stop/rejoin отменяет ожидающую работу. */
int chat_profile_options_submit(struct UTUN_INSTANCE* inst, const char* channel,
    const struct chat_profile_option* options, size_t count, db_async_done_fn done, void* arg);
#ifdef __cplusplus
}
#endif
#endif
