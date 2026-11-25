/* Строковые опции подписанного блока мембера, без отдельного протокола объявлений.
 * stream_source = epoch(16 hex):n:r:max-RS-size:audio(0/1); отсутствие/пустое значение — не публикует.
 * stream_subscriptions = source(64 hex):epoch(16 hex)[:r],...; :r — только помощь, без локального зрителя.
 * Указанная в stream_source собственная трансляция также должна быть в subscriptions.
 * load читает подтверждённый подписанный профиль из общего снимка прав без SQL/повторного crypto;
 * подписи и принадлежность ключа проверяет общая проекция до commit. BGP-доступность — отдельно.
 * parse/format не меняют выход при ошибке. Все вызовы — один uasync-поток. */
#ifndef STREAM_MEMBER_H
#define STREAM_MEMBER_H

#include "stream_proto.h"
#include "../chat/member_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_MEMBER_SOURCE "stream_source"
#define STREAM_MEMBER_SUBSCRIPTIONS "stream_subscriptions"
#define STREAM_MEMBER_MAX_SUBSCRIPTIONS 30

struct UTUN_INSTANCE;

struct stream_subscription_key {
    struct stream_address source; // Ed25519 источника в данной CHAT-группе
    uint64_t source_epoch;   // Ненулевая эпоха конкретного запуска
    int relay_only;          // 1 — помощь без локального зрителя; не влияет на has_subscription
};

struct stream_member_state {
    struct stream_descriptor source; // group/member заданы всегда, FEC/epoch значимы при published
    struct stream_subscription_key subscriptions[STREAM_MEMBER_MAX_SUBSCRIPTIONS]; // Строго по (source,epoch), без повторов
    size_t count;                    // Число заполненных subscriptions
    uint64_t version;                // Подписанный update_ts из load; после parse равен 0
    int published;                   // Непустая корректная опция stream_source
    int audio_enabled;               // Переключатель звука источника в профиле, может меняться внутри эпохи
};

/* Разобрать плоский JSON userinfo (до MS_USERINFO_MAX), неизвестные опции пропустить.
 * IDs/epoch — ровно 16 строчных hex, числа без ведущих нулей; stream-ключи не повторяются.
 * Подписи не проверяются; отсутствие обеих опций допустимо. OK/ERR_ARGUMENT/ERR_FORMAT. */
int stream_member_parse(uint64_t group_id, struct stream_address member, const char *userinfo, struct stream_member_state *state);
/* Сформировать два значения опций, без JSON; вызывающий затем обновляет подписанный профиль.
 * Буферы не перекрываются, каждый MS_USERINFO_MAX+1; OK/ERR_ARGUMENT, выходы меняются только вместе. */
int stream_member_format(const struct stream_member_state *state, char source[MS_USERINFO_MAX + 1],
                         char subscriptions[MS_USERINFO_MAX + 1]);
/* 1 — найдена пара source/epoch любого relay_only; 0 — отсутствует либо state недопустим. */
int stream_member_has_subscription(const struct stream_member_state *state, struct stream_address source, uint64_t epoch);

/* WAIT — снимок/полный подписанный профиль ещё не готов, включая placeholder/JOIN-only; ошибка формата <0.
 * NOT_MEMBER — строка отсутствует либо принятый блок владельца задаёт deleted=1; запрет проверяется до crypto.
 * Выходы меняются только при STREAM_OK; public_key — Ed25519 ключ источника, необязателен; state обязателен.
 * Online/BGP здесь не проверяются. deleted и профиль принадлежат одному опубликованному снимку;
 * незавершённые SQL-изменения не видны. */
int stream_member_load(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address member,
                       struct stream_member_state *state, uint8_t public_key[SC_PUBKEY_SIZE]);
/* 1 — у удалённого узла есть живой BGP-путь этой CHAT-группы; для себя нужны core_started и local_node.
 * 0 — пути/работающей группы нет. Подпись профиля и deleted эта функция не проверяет. */
int stream_member_online(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address member);
/* supernode=yes из подтверждённого снимка прав группы: только допущенный проверенный участник.
 * Подпись проверена до публикации; файлового SQL здесь нет. 0 — нет роли/CHAT-группы, 1 — роль задана,
 * -1 — неверный ID/namespace либо ошибка автономного RAM fallback. */
int stream_member_supernode(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address member);

#ifdef __cplusplus
}
#endif
#endif
