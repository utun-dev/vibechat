/* dm_identity — стороны личной беседы и снимок известных устройств.
 * DEVICE сохраняет пару узлов; GROUPS разрешает каждую сторону в подгруппу
 * либо узел при её отсутствии. Ошибка проверки не означает отсутствие группы.
 * Instance-операции вызываются в uasync; варианты *_read работают с db/копией identity в SQL worker.
 * Выходные структуры меняются только при успехе.
 * Стороны сохраняются в беседе: повторное разрешение при отправке запрещено. */
#ifndef UTUN_DM_IDENTITY_H
#define UTUN_DM_IDENTITY_H

#include <stdint.h>
#include <stddef.h>

struct UTUN_INSTANCE;
struct sqlite3;
struct dm_store_identity;

#ifdef __cplusplus
extern "C" {
#endif

enum dm_mode { DM_MODE_DEVICE = 0, DM_MODE_GROUPS = 1 };
enum dm_party_kind { DM_PARTY_DEVICE = 0, DM_PARTY_GROUP = 1 };
#define DM_PARTY_WIRE_SIZE 65
#define DM_TARGET_MAX 128

struct dm_party {
    uint8_t kind;       /* dm_party_kind, независимо от режима беседы. */
    uint64_t node;      /* DEVICE: ID узла; GROUP: строго 0. */
    uint8_t ed[32];     /* Публичный ключ подписи узла или ID подгруппы. */
    uint8_t x[32];      /* Проверенный публичный X25519 ключ той же стороны. */
};

struct dm_target {
    uint64_t node;      /* Числовая проекция для локального индекса истории. */
    uint8_t side;       /* Индекс стороны в переданном массиве parties[2]. */
    uint8_t ed[32];     /* Полный проверенный адрес устройства, в том числе офлайн. */
};

/* 0/-1: разрешить известный node для mode, включая проверку локальных секретов
 * для собственной подгруппы. Отсутствующий/повреждённый узел — ошибка. */
int dm_party_resolve(struct UTUN_INSTANCE* inst, uint64_t node, enum dm_mode mode, struct dm_party* out);
/* SQL/worker: та же проверка стороны по db и копии ключей identity, без runtime.
 * Caller задаёт согласованную транзакцию. SQLITE_OK/положительный SQLite-код; out меняется только при успехе. */
int dm_party_resolve_read(struct sqlite3* db, const struct dm_store_identity* identity,
    uint64_t node, enum dm_mode mode, struct dm_party* out);
/* SQL/worker: определить сторону по паре ключей устройства; для GROUPS проверить членство по Ed25519. */
int dm_party_resolve_key_read(struct sqlite3* db, const struct dm_store_identity* identity,
    const uint8_t x[32], const uint8_t ed[32], enum dm_mode mode, struct dm_party* out);

/* Проверить текущую принадлежность узла закреплённой стороне: 1=да,0=нет,-1=ошибка.
 * Не использовать текущую принадлежность вместо подписанного исторического snapshot. */
int dm_party_member(struct UTUN_INSTANCE* inst, const struct dm_party* party, uint64_t node);
/* SQL/worker: та же проверка по db и копии identity, без runtime.
 * SQLITE_OK и member=0/1 либо положительный SQLite-код. member обязателен и обнуляется при ошибке. */
int dm_party_member_read(struct sqlite3* db, const struct dm_store_identity* identity,
    const struct dm_party* party, uint64_t node, int* member);

/* 0/-1: получить собственный X25519 секрет строго указанной стороны.
 * out — 32 байта; вызывающий очищает секрет после использования. */
int dm_party_private(struct UTUN_INSTANCE* inst, const struct dm_party* party, uint8_t out[32]);
/* SQL/worker: секрет локальной закреплённой стороны из identity либо проверенного devgroup_local.
 * SQLITE_OK/положительный SQLite-код; out[32] меняется только при успехе, caller очищает секрет после использования. */
int dm_party_private_read(struct sqlite3* db, const struct dm_store_identity* identity,
    const struct dm_party* party, uint8_t out[32]);

/* Канонический codec: kind:u8,ed:32,x:32; номер DEVICE вычисляется локально из связанной пары.
 * 0/-1, размеры буферов ровно DM_PARTY_WIRE_SIZE; decode проверяет форму и ID узла. */
int dm_party_encode(const struct dm_party* party, uint8_t out[DM_PARTY_WIRE_SIZE]);
int dm_party_decode(const uint8_t data[DM_PARTY_WIRE_SIZE], struct dm_party* out);

/* 1 при совпадении всех полей (без padding), иначе 0. Аргументы обязательны. */
int dm_party_equal(const struct dm_party* a, const struct dm_party* b);

/* 0 при ошибке, иначе положительный 63-битный ID. DEVICE выводится из двух Ed25519 через
 * dm_derive_conv_id, GROUPS — домен и отсортированные полные стороны.
 * Одинаковые стороны отклоняются; ID не заменяет проверку полной пары. */
uint64_t dm_identity_conv_id(enum dm_mode mode, const struct dm_party parties[2]);

/* 0/-1: собрать уникальные известные устройства обеих закреплённых сторон,
 * включая источник и офлайн. Сортировка по node; источник — обычная запись.
 * Переполнение capacity или DM_TARGET_MAX отклоняет весь snapshot. Каждая
 * сторона должна иметь хотя бы один узел; ошибки SQL/подписей не пропускаются. */
int dm_identity_targets(struct UTUN_INSTANCE* inst, const struct dm_party parties[2],
    struct dm_target* out, size_t capacity, size_t* count);
/* SQL/worker: проверенный снимок обеих сторон по db/identity; каждая сторона должна иметь устройство.
 * SQLITE_OK/положительный код, out/count меняются только при успехе. Caller задаёт согласованную транзакцию. */
int dm_identity_targets_read(struct sqlite3* db, const struct dm_store_identity* identity, const struct dm_party parties[2],
    struct dm_target* out, size_t capacity, size_t* count);

/* Текущий состав для панели участников: тот же проверенный каталог и ограничения,
 * но подгруппа без известных устройств допустима. Список доставки не изменяется.
 * 0/-1; out/count меняются только при успехе. Устройства отсортированы по node_id. */
int dm_identity_members(struct UTUN_INSTANCE* inst, const struct dm_party parties[2],
    struct dm_target* out, size_t capacity, size_t* count);
/* SQL/worker: снимок для панели участников, пустая подгруппа допустима. Остальной контракт как у targets_read. */
int dm_identity_members_read(struct sqlite3* db, const struct dm_store_identity* identity, const struct dm_party parties[2],
    struct dm_target* out, size_t capacity, size_t* count);

#ifdef __cplusplus
}
#endif
#endif
