/* chat_sync_sql — SQL-снимки и подготовка JOIN без instance, транспорта и runtime-кешей.
 * Worker вызывает helpers в транзакции caller; возвращённые поля собственные, пригодны для отправки после commit.
 * Проверка подписи группы/своего профиля выполняется в том же снимке, что и чтение JOIN. */
#ifndef UTUN_CHAT_SYNC_SQL_H
#define UTUN_CHAT_SYNC_SQL_H
#include "chat_profile.h"
#include "group_info.h"
#include "group_policy.h"
#include "../group/group_protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
struct chat_sync_group_snapshot {
    uint8_t signed_by[32]; /* Полный Ed25519 пригласившего либо нули для группы. */
    uint8_t owner[32]; /* Ed25519 владельца из подписанного описания. */
    uint64_t join_ts; /* Время приглашения участника. */
    uint8_t x[32], ed[32]; /* Публичная идентичность группы. */
    uint8_t join_signature[64], invitation_signature[64]; /* Собственный JOIN и дерево его приглашения. */
    struct group_info_record description; /* Подписанные исходные байты описания. */
    struct chat_profile_record profile; /* Проверенный собственный профиль, привязанный к тому же JOIN. */
};
/* SQL/worker, READ caller: согласованно прочитать группу и полный свой member для INFO_RESP/CHANNEL_INVITE.
 * identity содержит только скопированные публичные ключи; channel — публичный Ed25519 группы в строчном HEX.
 * SQLITE_OK/положительный код (NOTFOUND — нет группы/полного своего участника); out меняется только при успехе. */
int chat_sync_group_read(sqlite3* db, const struct chat_profile_identity* identity, const char* channel,
    struct chat_sync_group_snapshot* out);
struct chat_sync_join_input {
    struct member_sync_write_context identity; /* Собственная frozen подпись/время; секрет очистить после SQL done. */
    uint8_t x[32], peer_x[32], peer_ed[32]; /* Свой X25519 и подтверждённые ключи пригласившего узла. */
    uint8_t group_keys[64]; /* X25519 || Ed25519 из проверенного E2E ответа пригласившего. */
    uint64_t peer;
    const char *channel, *name; /* Canonical ID и собственное имя, заимствованы только на время helper. */
    const uint8_t* data; /* Описание и запись A из расшифрованного INFO_RESP. Caller хранит до возврата worker. */
    size_t size;
    int chat_content; /* Frozen наличие сервиса сообщений для подготовки его схемы. */
};
struct chat_sync_join_prepared {
    struct group_protocol_prepared protocol; /* SQL schema/history, активировать после commit. */
    struct group_policy_update* admission; /* Caller publish после commit либо discard при ошибке/stop. */
    uint64_t join_ts, update_ts; /* Время своего приглашения и версия профиля. */
    uint8_t inviter_ed[32]; /* Ed25519 сохранённого участника A. */
    unsigned changes; /* MS_APPLY_* для публикации записи A после commit. */
    uint8_t join_signature[64], update_signature[64]; /* Собственные подписи для следующего JOIN_REQUEST. */
    char userinfo[MS_USERINFO_MAX + 1];
};
/* Перед импортом проверить связь X||Ed из E2E-ответа, JSON и подпись описания.
 * db нужен парсеру. SQLITE_OK при успехе, SQLITE_AUTH при неверных ключах/подписи, иначе код ошибки. */
int chat_sync_join_description_check(sqlite3* db, const char* channel, const char* json, const uint8_t owner[32],
    const uint8_t keys[64], const uint8_t signature[64]);
/* Worker/caller WRITE: проверить INFO_RESP, атомарно сохранить описание, запись A и индекс,
 * подготовить admission/runtime и подписать собственный JOIN_REQUEST. Без instance/сети/BEGIN/COMMIT.
 * SQLITE_OK/положительная причина. out обнулить до вызова; admission discard нужен также при ошибке. */
int chat_sync_join_accept_write(sqlite3* db, struct chat_sync_join_input* input, struct chat_sync_join_prepared* out);
/* Worker/READ: соответствует ли сохранённый JOIN ожидаемому pending attempt; отсутствие даёт OK/matches=0.
 * channel/node/signature — frozen подтверждённые входы; положительная SQLite-причина при отказе. */
int chat_sync_join_matches_read(sqlite3* db, const char* channel, const uint8_t peer_x[32],
    const uint8_t signature[64], int* matches);
#ifdef __cplusplus
}
#endif
#endif
