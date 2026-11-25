/* chat_join — ключи приглашений и сквозной обмен A ↔ J.
 * A хранит ключ локально, читает описание и подтверждает JOIN после COMMIT.
 * Exit пересылает кадры по полному ключу A через BGP и доставляет ответы J по прямому ETCP/BLE.
 * Открытый заголовок передаёт направление:u8, A Ed25519:32, J Ed25519:32 и attempt:u64BE.
 * AES-CCM защищает домен "utun-join-relay" (16 байт с нулями), X25519 exit (32),
 * копию открытого заголовка, внутреннюю команду (1) и её тело.
 * chat_sync сохраняет маршрут до JOIN_REQUEST и запускает первый MEMBERS от A по READY либо при восстановлении.
 * Клиент завершает JOIN после публикации собственного допуска. BGP проверяет членство отдельно.
 * Все операции выполняются в uasync. Объекты маршрута копируются для задач SQLite. */
#ifndef CHAT_JOIN_H
#define CHAT_JOIN_H

#include <stdint.h>
#include <stddef.h>
#include "member_sync.h"
#include "../transport_layer/secure_channel.h"

struct UTUN_INSTANCE;
struct chat_join_route;

#ifdef __cplusplus
extern "C" {
#endif

/* etcp_router service id (connection ↔ инвайтер): ETCP_RT_ID_JOIN (см. etcp_api.h) */

#define JOIN_SUBCMD_RELAY 0x07
#define CHAT_JOIN_BODY_MAX (12u * 1024u) /* Вмещает максимальную запись MEMBERS вместе с кадром BGP и BLE. */

/* Маршрут сквозного JOIN. group=0 означает прямой ETCP/BLE; иначе hop_ed — адрес BGP. */
struct chat_join_path {
    uint64_t attempt; /* Случайный ненулевой ID сетевой попытки. */
    uint64_t group, generation; /* Локальная группа маршрута и её поколение. */
    uint64_t handle; /* Сессия BLE либо 0 для ETCP. */
    uint8_t inviter[32], guest[32]; /* Полные Ed25519 конечных сторон. */
    uint8_t exit_x[32]; /* Подтверждённый X25519 узла подключения. */
    uint8_t hop_x[32], hop_ed[32]; /* Ключ следующего узла по прямому транспорту или BGP. */
};

/* Отправить кадр второго участника: type и data шифруются, связанные ключи и attempt проверяются на приёме. */
int chat_join_send(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, uint8_t type,
    const uint8_t* data, size_t size);
/* Разобрать заголовок от аутентифицированного ETCP/BLE и принять либо переслать кадр. */
void chat_join_receive_direct(struct UTUN_INSTANCE* inst, const struct chat_join_route* route,
    const uint8_t* frame, size_t size);
/* Удалить обратные маршруты указанного транспорта; NULL и 0 снимают все BLE-маршруты. */
void chat_join_relay_closed(struct UTUN_INSTANCE* inst, const uint8_t* x, uint64_t handle);
#define JOIN_MEMBER_FIXED_SIZE (32 + 32 + 64 + 8 + 64 + 8 + 2) /* Поля member до байтов userinfo. */
#define JOIN_REQUEST_MAX_SIZE (JOIN_MEMBER_FIXED_SIZE + MS_USERINFO_MAX) /* Полный запрос помещается в CHAT_JOIN_WIRE_MAX. */

#define JOIN_KEY_TTL_SECONDS 600   /* Время жизни ключей, созданных вне окна приглашения. */
#define JOIN_KEYS_PER_INVITER 4u    /* Лимит локальных ключей по каналам, включая до одного ключа окна. */

enum chat_join_result { CHAT_JOIN_OK = 0, CHAT_JOIN_ERROR = -1, CHAT_JOIN_TIMEOUT = -2, CHAT_JOIN_CANCELLED = -3 };

/* Полный набор мембера джойнера (без подписи дерева — её ставит инвайтер). */
struct join_member_data {
    const uint8_t* x25519;      /* 32 */
    const uint8_t* ed25519;     /* 32 */
    const uint8_t* join_sig;    /* 64, может быть NULL */
    uint64_t      join_ts;      /* время приглашения (секунды Unix), подписывается пригласившим */
    const uint8_t* update_sig;  /* 64, может быть NULL */
    uint64_t      update_ts;    /* версия подписанного блока участника */
    char          userinfo[MS_USERINFO_MAX + 1]; /* JSON, null-terminated */
};

/* Сериализовать member-блок (без join_key). 0 и *out_len,-1 при неверных аргументах/размере.
 * На отказе out/out_len сохраняются; подписи NULL кодируются 64 нулями,ключи обязательны.
 * join_ts/update_ts: u64BE, userinfo_len: u16BE; userinfo до MS_USERINFO_MAX байт без NUL на проводе. */
int  chat_join_serialize_member(const struct join_member_data* m,
                                uint8_t* out, size_t out_sz, size_t* out_len);

/* Разобрать весь member-блок:проверить длину/отсутствие скрытого NUL. 0/-1,на отказе m сохраняется.
 * Ключи/подписи заимствованы у data,userinfo копируется;криптография здесь не проверяется. */
int  chat_join_parse_member(const uint8_t* data, size_t len, struct join_member_data* m);

/* Инициализация JOIN с обязательной подписью/шифрованием и допуском self/peer к CHAT-группе. 0/-1. */
int  chat_join_init(struct UTUN_INSTANCE* inst);

/* Снять обработчик, освободить локальные ключи и обратные маршруты exit. */
void chat_join_destroy(struct UTUN_INSTANCE* inst);

/* После commit допуска/политики удалить недействительные локальные ключи. group=0 проверяет все каналы. */
void chat_join_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group);

/* Сохранить ключ A в локальной памяти. Возврат 0/-1. */
int chat_join_store_local_key(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint64_t join_key);
/* Uasync: опубликовать единственный ключ окна, принадлежащий ненулевому request. 0/-1. */
int chat_join_store_window_key(struct UTUN_INSTANCE* inst, uint64_t channel, uint64_t key, uint64_t request);
/* Uasync: отозвать ключ окна с указанным request; 0 означает любой ключ окна. */
void chat_join_revoke_window_key(struct UTUN_INSTANCE* inst, uint64_t request);

/* Вернуть локальный Ed25519 для действующего ключа в указанном канале либо NULL. Буфер принадлежит inst. */
const uint8_t* chat_join_lookup_inviter(struct UTUN_INSTANCE* inst, uint64_t channel_id, uint64_t join_key);
/* Найти локальный namespace по действующему уникальному ключу приглашения. 0 при отсутствии. */
uint64_t chat_join_key_group(struct UTUN_INSTANCE* inst, uint64_t join_key);

/* 1, если ключ сохранён в указанном канале и ed совпадает с локальным Ed25519. */
int chat_join_key_matches(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t key, const uint8_t ed[32]);

/* A: проверить подписанную запись J, полученную по аутентифицированному маршруту, и сохранить членство.
 * guest — Ed25519 из сквозного кадра. Буфер заимствован до возврата. 0/CHAT_CONNECT_* (RAM). */
int chat_join_process_request(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t guest[32],
    uint64_t key, uint64_t attempt, const uint8_t* request, size_t request_len);

struct chat_join_request_result {
    int status, sqlite_code; /* 0/CHAT_CONNECT_*; SQLite уточняет SQL/очередь/отмену. */
    int committed; /* WRITE подтверждена даже при позднем stop, запрещающем runtime публикацию. */
    uint8_t node[32]; /* Подтверждённый сквозным транспортом Ed25519 J. */
};
typedef void (*chat_join_done_fn)(void* arg, const struct chat_join_request_result* result);
/* Uasync: скопировать запись участника, свежие права/проверку/подпись/MEMBERS выполнить одной worker WRITE.
 * done ровно один раз, включая синхронный OOM/FULL/отказ; результат заимствован до возврата callback.
 * inst/arg живут до done и drain, ключи очищаются в cleanup, stop/destroy вне callbacks. */
void chat_join_process_submit(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t guest[32], uint64_t key,
    uint64_t attempt, const uint8_t* request, size_t size, chat_join_done_fn done, void* arg);

#ifdef __cplusplus
}
#endif
#endif /* CHAT_JOIN_H */
