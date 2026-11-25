/* devgroup — общая подписанная принадлежность устройства и профиль группы.
 * Публичная запись хранится по значению в TOPO_NODE; нулевая запись означает отсутствие группы.
 * JSON пересылается без пересериализации, ключи группы независимы от ключей устройства.
 * Instance-операции вызываются в uasync; функции только с sqlite3 допускают worker владельца БД.
 * Codec не обращается к instance. Для согласованных составных чтений caller открывает READ-транзакцию. */
#ifndef UTUN_DEVGROUP_H
#define UTUN_DEVGROUP_H
#include <stdint.h>
#include <stddef.h>
struct sqlite3;
struct UTUN_INSTANCE;
struct TOPO_NODE;
#ifdef __cplusplus
extern "C" {
#endif
#define DEVGROUP_PROFILE_MAX 768
#define DEVGROUP_WIRE_MAX (192 + DEVGROUP_PROFILE_MAX)
struct devgroup_record {
    uint8_t ed[32], x[32]; /* Общие публичные ключи подписи и шифрования. */
    uint8_t member_sig[64]; /* Подпись идентичности конкретного устройства ключом группы. */
    uint8_t profile_sig[64]; /* Подпись исходных байтов JSON ключом группы. */
    char json[DEVGROUP_PROFILE_MAX + 1]; /* Полный строковый JSON; пустой — группы нет. */
};
/* Committed локальная идентичность для runtime; SQL bootstrap заполняет через devgroup_local_read.
 * Это собственные значения, не SQLite-указатели; секреты очистить при замене/уничтожении instance. */
struct devgroup_local_identity {
    struct devgroup_record record;
    uint8_t ed_private[32], x_private[32];
    int ready; /* 1 — завершён SQL bootstrap, включая отсутствие группы;0 — ещё не загружено. */
    int present; /* Результат local_read:0=нет группы,1=есть проверенные запись и оба секрета. */
};
/* Uasync после SQL commit: скопировать prepared identity в embedded local_devgroup instance без SQL.
 * 0/-1, caller очищает свою копию секретов после возврата. */
int devgroup_local_activate(struct UTUN_INSTANCE* inst, const struct devgroup_local_identity* prepared);
/* Uasync/noSQL: собственная committed identity либо публичная подписанная запись живого node_registry.
 * 0=группа неизвестна/отсутствует,1=готова,-1=аргументы/непроверенная запись; out очищается при0,
 * не меняется при ошибке. Это принадлежность устройства, право CHAT-группы caller проверяет отдельно. */
int devgroup_runtime_member(struct UTUN_INSTANCE* inst, uint64_t node, struct devgroup_record* out);
/* Получить удостоверение конкретного устройства по его Ed25519. 1 найдено, 0 отсутствует, <0 ошибка. */
int devgroup_member_by_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], struct devgroup_record* out);
struct devgroup_profile {
    uint64_t version, timestamp; /* Порядок выбора профиля, независимо от timestamp ноды. */
    char name[64]; /* UTF-8 ник, максимум 63 байта. */
    uint8_t hash[32]; /* Последний критерий при равенстве version/timestamp. */
};
/* 0/-1: проверить весь JSON и подпись; out меняется только при успехе. Запись обязательна и непустая. */
int devgroup_profile_read(const struct devgroup_record* record, struct devgroup_profile* out);
/* 0/-1: проверить пустую запись либо обе подписи и привязку к ключам устройства. */
int devgroup_verify(const struct devgroup_record* record, const uint8_t x[32], const uint8_t ed[32]);
/* 0/-1: проверить только удостоверение узла по record.ed/x/member_sig, без профиля.
 * Для закреплённого снимка DM ключи группы уже переданы отдельно; JSON не нужен.
 * Не подтверждает актуальное членство: для него используется devgroup_read_member. */
int devgroup_verify_binding(const struct devgroup_record* record, const uint8_t x[32], const uint8_t ed[32]);
/* Codec: encode возвращает число байтов (0 для пустой записи), decode0/-1. Буферы caller, без struct-padding на проводе. */
int devgroup_encode(const struct devgroup_record* record, uint8_t* out, size_t capacity);
int devgroup_decode(struct devgroup_record* out, const uint8_t* data, size_t size);
/* Сравнение проверенных профилей: отрицательное/0/положительное, a относительно b. */
int devgroup_profile_compare(const struct devgroup_profile* a, const struct devgroup_profile* b);
/* Подписать только удостоверение устройства, сохраняя исходные JSON/подпись профиля; 0/-1. */
int devgroup_bind(struct devgroup_record* record, const uint8_t priv[32], const uint8_t x[32], const uint8_t ed[32]);
/* Создать/переименовать профиль и удостоверение. priv — Ed25519 seed; версии положительные <=INT64_MAX; 0/-1. */
int devgroup_sign(struct devgroup_record* record, const uint8_t priv[32], const char* name,
    uint64_t version, uint64_t timestamp, const uint8_t x[32], const uint8_t ed[32]);
/* Обновить принадлежность при совпадении обоих ключей с проверенной идентичностью nodes;0/отрицательный SQLite-код.
 * Вызывающий проверяет NODEINFO. Неизвестная/иная идентичность пропускается, ключи nodes не меняются.
 * В транзакции caller; MEMBERS повторит проекцию после проверки ещё неизвестного участника. */
int devgroup_project(struct sqlite3* db, const struct TOPO_NODE* node);
/* Загрузить собственную запись и проверить обе пары секретов. 0=нет группы,1=загружена,-1=ошибка.
 * RW использует только committed local_devgroup; до activate возвращает ошибку, выходы не меняет. */
int devgroup_load_local(struct UTUN_INSTANCE* inst, struct devgroup_record* out, uint8_t ed_priv[32], uint8_t x_priv[32]);
/* SQL/worker: загрузить группу локального устройства self с публичными ключами device_x/device_ed.
 * Проверяет формат, подписи и соответствие обоих секретов публичным ключам группы. Caller задаёт READ-снимок.
 * 0=группы нет,1=найдена,отрицательный SQLite-код=ошибка. При отсутствии выходы обнуляются,
 * при ошибке не меняются. Секреты ed_priv/x_priv вызывающий очищает после использования. */
int devgroup_local_read(struct sqlite3* db, const uint8_t device_x[32], const uint8_t device_ed[32],
    struct devgroup_record* out, uint8_t ed_priv[32], uint8_t x_priv[32]);
/* SQL/worker: максимальное проверенное объявление среди nodes;0=нет,1=найдено,отрицательный SQLite-код=ошибка.
 * Caller задаёт READ-снимок. out/profile обнуляются при отсутствии, не меняются при ошибке. */
int devgroup_latest(struct sqlite3* db, const uint8_t id[32], struct devgroup_record* out, struct devgroup_profile* profile);
/* Проверенная запись одного узла без поиска профилей других устройств; 0=нет,1=найдено,отрицательный SQLite-код=ошибка.
 * out обнуляется при отсутствии группы, при ошибке не меняется. Для свежего имени — devgroup_member. */
int devgroup_read_member(struct sqlite3* db, uint64_t node, struct devgroup_record* out);
/* Та же проверка по Ed25519 устройства, SQL/worker. */
int devgroup_read_member_key(struct sqlite3* db, const uint8_t ed[32], struct devgroup_record* out);
/* SQL/worker: самостоятельный список проверенных устройств группы anchor, включая offline/self.
 * Читает через индекс nodes_devgroup; caller фильтрует живой маршрут/допуск после READ.
 * SQLITE_OK/положительный SQLite-код; отсутствие группы — пустой успех. До65535 устройств,
 * SQL-порядок Ed25519, без усечения/частичного результата. out/count обязательны; массив caller освобождает u_free. */
int devgroup_nodes_read(struct sqlite3* db, const uint8_t anchor[32], uint8_t (**out)[32], size_t* count);
/* SQL/worker: проверенная принадлежность узла и самое свежее имя;0=нет,1=найдено,отрицательный SQLite-код=ошибка.
 * Буферы32/64 байта; caller задаёт READ-снимок. При отсутствии обнуляются, при ошибке не меняются. */
int devgroup_member(struct sqlite3* db, const uint8_t ed[32], uint8_t id[32], char name[64]);
/* Независимые публичные ключи из seed, 0/-1. */
int devgroup_public_keys(const uint8_t ed_priv[32], const uint8_t x_priv[32], uint8_t ed[32], uint8_t x[32]);
#ifdef __cplusplus
}
#endif
#endif
