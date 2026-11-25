/* topo_node_sqlite — хранение подписанных снимков узлов, адресов, каналов и блоков мемберов в общей SQLite.
 * Модель/проверка подписей/registry — topo_node и member_sync; чистые SQL-операции выполняет поток владельца db.
 * record_read/free работают в потоке владельца db без runtime; record_build создаёт узел только в uasync.
 * Остальные функции с живыми TOPO_NODE/TOPO_GROUPS вызываются в uasync; worker не получает runtime/его пулы.
 * Только snapshot_put/load проверяют подпись; остальные записи требуют проверенных вызывающим данных.
 * snapshot_put сохраняет более свежий timestamp; node_load предпочитает snapshot.
 * db/groups заимствованы. Возвращённый TOPO_NODE без registry ref: передать registry либо уничтожить через topo_node_destroy.
 * Массивы out_ids/out_ch_ids/out_peers принадлежат вызывающему и освобождаются u_free. */
#ifndef TOPO_NODE_SQLITE_H
#define TOPO_NODE_SQLITE_H

#include <sqlite3.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "topo_node.h"
#include "topo_node_rtt.h"

#define PEERS_JOIN_SIG_SIZE   64
#define PEERS_JOIN_TS_SIZE     8
#define PEERS_UPDATE_SIG_SIZE  64

/* Версии в одной peers_<channel>. Все обычные операции читают/меняют только CURRENT.
 * BACKUP существует только внутри транзакции проверки. Подписанный отзыв — обычная
 * проверенная запись CURRENT с запретом допуска, а не причина удалить весь состав. */
enum peer_record_state { PEER_CURRENT = 0, PEER_BACKUP = 2 };

/* Результаты channel_get/get_ed25519_pubkey: отсутствие зависимости и отказ чтения различаются. */
#define TOPO_SQLITE_LOOKUP_MISSING (-1)
#define TOPO_SQLITE_LOOKUP_ERROR   (-2)

/* flags byte in wire format */
#define PEERS_FLAG_HAS_JOIN  0x01

/* Проверить общую схему до запуска служб; пустую БД создать по db_schema. Миграций нет.
 * 0/-1; несовместимость не меняет БД, причину получает GUI/CLI через db_schema_last_error().
 * db открывает и закрывает вызывающий. */
int topo_node_sqlite_init(sqlite3* db);
/* Проверить и сохранить полный подписанный NODEINFO без групповых полей;старый/равный timestamp даёт0 без замены.
 * Возврат0/-1,свой savepoint проверяет bind/step/finalize/RELEASE;отказ откатывает запись.
 * При неудачном rollback открытая транзакция требует rollback/закрытия БД владельцем. */
int topo_node_sqlite_snapshot_put(sqlite3* db, struct TOPO_NODE* ni);
/* SQL/worker: варианты записи с точным положительным SQLite-кодом вместо legacy0/-1.
 * Сохраняют savepoint/проверки базовых функций; caller worker открывает общую WRITE-транзакцию.
 * node должен быть собственным замороженным снимком, без ссылок на runtime/пулы uasync. */
int topo_node_sqlite_snapshot_write(sqlite3* db, struct TOPO_NODE* node);
int topo_node_sqlite_addrs_write(sqlite3* db, uint64_t node_id, struct TOPO_NODE* node);
/* SQL/worker: записать проверенную идентичность; ключи/имя заимствованы только до возврата.
 * update_ts — версия, created_at — Unix seconds; SQLITE_OK/положительная ошибка, без собственного COMMIT. */
int topo_node_sqlite_node_write(sqlite3* db, uint64_t node_id, const char* name,
    const uint8_t* x25519, const uint8_t* ed25519, uint64_t update_ts, time_t created_at);
/* SQL/worker: атомарная проекция node_type/storage/deleted из адресов и adm_tags.
 * Только SQLite, без runtime; SQLITE_OK/положительный код, caller управляет внешним commit. */
int topo_node_sqlite_nodeinfo_write(sqlite3* db, const uint8_t x[32]);
/* SQL/worker: собственный список групп, в которых есть CURRENT-запись указанного Ed25519.
 * out/count обязательны; нет групп => NULL/0, ошибка => NULL/0. Массив освобождается u_free.
 * Возвращает SQLITE_OK/положительный SQLite-код; для допуска нужен отдельный policy cache. */
int topo_node_sqlite_member_channels_read(sqlite3* db, const uint8_t ed[32], uint8_t (**out)[32], int* count);
/* SQL/worker: добавить неподписанный BGP-плейсхолдер, не заменяя уже известного мембера.
 * channel_id — существующая группа; ключи32 байта, caller проверил NODEINFO; SQLITE_OK/положительный код. */
int topo_node_sqlite_member_placeholder_write(sqlite3* db, const char* channel_id, uint64_t node_id,
    const uint8_t* x25519, const uint8_t* ed25519);
/* Сохранённый timestamp (Unix microseconds),в том числе после инвалидирования record;отсутствие даёт0.
 * Возвращает0/-1;при SQL/форматной ошибке *out не меняется. out обязателен. */
int topo_node_sqlite_snapshot_timestamp(sqlite3* db, uint64_t node_id, uint64_t* out);
/* SQL/worker: тот же timestamp с точным положительным SQLite-кодом;out меняется только при успехе.
 * Отсутствие =>0; читать до назначения следующего собственного timestamp внутри caller WRITE. */
int topo_node_sqlite_snapshot_time_read(sqlite3* db, uint64_t node_id, const uint8_t* x, uint64_t* out);
/* Прочитать connection_order из сохранённого подписанного снимка для карточки узла.
 * out — буфер 256 байт. 0 — успех (пусто, если снимка/политики ещё нет), -1 — ошибка с логом.
 * Не подставляет локальную/default политику вместо неизвестной политики пира. */
int topo_node_sqlite_get_connection_order(sqlite3* db, const uint8_t x[32], char out[256]);
/* SQL/worker: тот же снимок порядка соединений, 0/положительный SQLite-код без потери IOERR/CORRUPT.
 * out вмещает256 байт, пустая строка означает неизвестный порядок; при ошибке также очищается. */
int topo_node_sqlite_connection_order_read(sqlite3* db, const uint8_t x[32], char out[256]);

/* Данные для передачи worker -> uasync. Снимок владеет копиями строк/BLOB/адресов,
 * не содержит sqlite3/statement, TOPO_NODE, ссылок на instance/groups или memory_pool. */
struct topo_node_record;
enum topo_node_record_kind {
    TOPO_NODE_RECORD_SNAPSHOT,  /* Подписанный wire; проверка подписи выполняется при build. */
    TOPO_NODE_RECORD_IDENTITY,  /* Только ключи/имя/тип устройства; для подтверждённого LAN-адреса. */
    TOPO_NODE_RECORD_BOOTSTRAP, /* Ключи/имя и обязательные адреса SQL-проекции; без доверия её NAT-профилю. */
    TOPO_NODE_RECORD_CONNECT    /* Та же проекция, но адреса необязательны: можно ждать LAN/входящий транспорт. */
};
/* Поток владельца db: скопировать выбранные сведения в *out; out обязателен, изначально NULL.
 * SQLITE_OK, в том числе при отсутствии (*out=NULL); иначе код SQLite, *out=NULL, statements закрыты.
 * Нет обращений к uasync/runtime. Для согласованного чтения нескольких таблиц caller открывает READ-транзакцию.
 * Результат передаётся в done; освобождать record_free после build или при отмене запроса. */
/* Загрузить запись для точного X25519. При key=NULL числовой адрес должен быть однозначным. */
int topo_node_sqlite_record_lookup(sqlite3* db, uint64_t node_id, const uint8_t* key, enum topo_node_record_kind kind, struct topo_node_record** out);
int topo_node_sqlite_record_read(sqlite3* db, uint64_t node_id, enum topo_node_record_kind kind, struct topo_node_record** out);
/* Uasync: построить незарегистрированный узел из самостоятельного снимка, не обращаясь к SQLite.
 * record заимствован и не меняется; groups обязателен для всех видов, кроме IDENTITY; пулы живы до возврата.
 * Для IDENTITY groups необязателен. NULL — нет/невалидные данные либо OOM; отказ освобождает частичный узел.
 * Успешный узел передать registry либо уничтожить topo_node_destroy(groups,node). */
struct TOPO_NODE* topo_node_sqlite_record_build(struct TOPO_GROUPS* groups, const struct topo_node_record* record);
/* Любой поток после передачи владения: освободить только самостоятельный снимок, NULL допустим.
 * Не вызывать одновременно с read/build или другим free того же снимка. */
void topo_node_sqlite_record_free(struct topo_node_record* record);

/* Синхронный SQL+build в uasync; для worker использовать раздельные record_read/record_build.
 * Незарегистрированный проверенный снимок, включая снимок без адресов; NULL — нет/невалиден/ошибка.
 * groups и groups->instance->topo_groups должны обозначать один контейнер с пулами. */
struct TOPO_NODE* topo_node_sqlite_snapshot_load(sqlite3* db, struct TOPO_GROUPS* groups, uint64_t node_id);

/* Синхронно загрузить подписанный снимок по полному X25519, при его отсутствии прочитать fallback.
 * x=NULL разрешён только для однозначного id. Возвращает собственный узел либо NULL. */
struct TOPO_NODE* topo_node_sqlite_load_key(sqlite3* db, struct TOPO_GROUPS* groups, uint64_t id,
                                           const uint8_t* x, enum topo_node_record_kind fallback);

/* Персистентная запись адресов узла в node_addresses по X25519 узла (источник истины — BGP/topo_group).
 * DELETE + INSERT из ni->v4_addrs/v6_addrs под savepoint, включая пустой набор; 0/-1, отказ откатывает замену.
 * mapping/inbound/source берутся из SOCKMETA; проверку ni выполняет вызывающий. RTT при замене не сохраняется. */
int topo_node_sqlite_addrs_put(sqlite3* db, uint64_t node_id, struct TOPO_NODE* ni);

/* Есть ли строка хотя бы в одной peers_*: включая source=1 и deleted=1; 1/0, при ошибке также 0. */
int topo_node_sqlite_member_exists(sqlite3* db, const uint8_t ed[32]);

/* Числовые ненулевые channel_id таблиц peers_*, содержащих строку узла; включает placeholders/deleted.
 * 0/-1; пустой результат NULL/0, при ошибке частичный массив не выдаётся. */
int topo_node_sqlite_get_member_channels(sqlite3* db, const uint8_t ed[32],
                                         uint8_t (**out_ch_ids)[32], int* out_count);

/* Создать peers_<id> и сохранить канал; 0/-1. Ключи — по 32, signature — 64 байта, privkey необязательны.
 * Создаёт JSON {name,ver:1}. При обновлении идентичность обязана совпасть; более новое описание сохраняется.
 * Каждый публичный ключ X25519 и каждый публичный ключ Ed25519 принадлежит одному channel_id в этой БД.
 * Повтор любого из них в другом канале отклоняет запись. NULL privkey сохраняет прежний.
 * Свой savepoint включает peers/индексы/описание; проверяет DDL,все bind/step/changes1/finalize/release.
 * Ошибка сохраняет прежние данные/схему. Внешняя транзакция допустима и не завершается.
 * Отказ rollback не выполняет release:caller должен откатить/закрыть оставшуюся транзакцию.
 * Подпись/пары ключей не проверяет. Табличный суффикс: первые 63 символа, вне [A-Za-z0-9_] заменены на '_'. */
int topo_node_sqlite_channel_put(sqlite3* db, const char* channel_id,
    const char* name, const uint8_t owner_node_id[32],
    const uint8_t* x25519_pub, const uint8_t* x25519_priv,
    const uint8_t* ed25519_pub, const uint8_t* ed25519_priv,
    const uint8_t* signature);

/* Сохранить полный проверенный caller JSON вместо исходного имени;0/-1. Сравнение (ver,JSON).
 * Ключи/владелец неизменны, подпись и JSON заменяются вместе. Savepoint включает peers/индексы/описание. */
int topo_node_sqlite_channel_put_description(sqlite3* db, const char* channel_id,
    const char* json, const uint8_t owner_node_id[32],
    const uint8_t* x25519_pub, const uint8_t* x25519_priv,
    const uint8_t* ed25519_pub, const uint8_t* ed25519_priv,
    const uint8_t* signature);
/* SQL/worker: тот же UPSERT в WRITE caller с точным положительным SQLite-кодом до cleanup/savepoint rollback.
 * Проверяет JSON, immutable type и схему peers; подпись/идентичность проверяет caller до записи.
 * SAVEPOINT не завершает внешнюю транзакцию. При любом отказе caller откатывает WRITE. */
int topo_node_sqlite_channel_put_description_write(sqlite3* db, const char* channel_id,
    const char* json, const uint8_t owner_node_id[32], const uint8_t* x25519_pub, const uint8_t* x25519_priv,
    const uint8_t* ed25519_pub, const uint8_t* ed25519_priv, const uint8_t* signature);

/* Проверить формат версионной peers_<id> и локальные поля; 0/-1, идемпотентно.
 * Старый формат с UNIQUE(node_id) не поддерживается; ошибка не изменяет его записи. */
int topo_node_sqlite_member_local_init(sqlite3* db, const char* channel_id);
/* SQL/worker, WRITE caller: тот же schema check/create, SQLITE_OK/точный положительный SQLite-код. */
int topo_node_sqlite_member_local_init_write(sqlite3* db, const char* channel_id);

/* Полное локальное удаление канала: DROP peers_<id>/msg_<id>, DELETE channels,
 * Удаляются feedback_<id> и merkle_tree_hash всех datasets канала (если таблица есть).
 * Проверяются finalize и отсутствие канала/хешей после DELETE, включая RAISE(IGNORE).
 * Локальные каскадные triggers (в том числе attachment) входят в ту же транзакцию.
 * 0/-1; savepoint откатывает в том числе DROP TABLE, внешняя транзакция сохраняется.
 * При ошибке rollback вызывающий проверяет autocommit; открытая транзакция требует остановки/закрытия БД владельцем. */
int topo_node_sqlite_channel_delete(sqlite3* db, const char* channel_id);
/* SQL/worker в WRITE caller: удалить metadata/динамические таблицы/Merkle группы.
 * SQLITE_OK/положительный SQLite-код, включая prepare/bind/step/finalize. Собственных BEGIN/savepoint нет;
 * при любой ошибке caller обязан откатить всю WRITE. Runtime/файлы/события отдельно после commit. */
int topo_node_sqlite_channel_delete_write(sqlite3* db, const char* channel_id);

/* Вставить source=1 с нулевой join_sig; существующую строку не менять. 0/-1; ключи по 32 байта.
 * Плейсхолдер не участвует в Merkle-хеше; канал/peers_<id> должны уже существовать. */
int topo_node_sqlite_member_placeholder_put(sqlite3* db, const char* channel_id, uint64_t node_id,
    const uint8_t* x25519_pubkey, const uint8_t* ed25519_pubkey);

/* Обновляет только блок мембера (keys/join/update_sig/update_ts/userinfo), не трогая блок владельца.
 * signed_by/signature — дерево приглашений: кто подписал pubkey узла (0 = корень, ключом группы).
 * 0/-1; подписи по 64, ключи по 32 байта. NULL join_sig даёт нулевую подпись, userinfo=NULL даёт ''.
 * Сбрасывает source=0; свежесть/подписи проверяет вызывающий, owner/local-поля сохраняются. */
/* Проверяет все bind,step,changes1 и finalize;при отказе caller откатывает свою транзакцию записи/индекса. */
int topo_node_sqlite_member_block_put(sqlite3* db, const char* channel_id, uint64_t node_id,
    const uint8_t* join_sig, uint64_t join_ts,
    const uint8_t* update_sig, uint64_t update_ts,
    const uint8_t* x25519_pubkey, const uint8_t* ed25519_pubkey, const char* userinfo, const uint8_t* signed_by, const uint8_t* signature);

/* Скопировать необязательный публичный ключ из SQLite; SQL NULL даёт 32 нуля. Возвращает SQLite-код. */
int topo_node_sqlite_read_optional_key(sqlite3_stmt* st, int column, uint8_t out[32]);

/* Прочитать дерево приглашений мембера (signed_by + signature). 0=ок, -1=нет записи/полей. */
int topo_node_sqlite_member_get_sign(sqlite3* db, const char* channel_id, const uint8_t ed[32],
    uint8_t signed_by_out[32], uint8_t signature_out[64]);

/* Есть ли строка в peers_<id>, включая placeholders/deleted. 1/0; ошибка также даёт 0. */
int topo_node_sqlite_member_in_channel(sqlite3* db, const char* channel_id, uint64_t node_id);

/* Допуск участника: source=0, deleted=0 и admitted=1 последней контрольной точки политики/цепочек.
 * 1/0, ошибка логируется и даёт 0. BGP-placeholder и неподходящая цепочка доступа не дают. */
int topo_node_sqlite_member_allowed(sqlite3* db, const char* channel_id, uint64_t node_id);

/* Ключи только из проверенного дерева данной группы; NODEINFO/nodes не используются.
 * active=1 требует текущего допуска; active=0 разрешает удостоверения исключённых для проверки истории.
 * x/ed — необязательные выходы по 32 байта (хотя бы один нужен), меняются только при успехе.
 * 0 — найдено, LOOKUP_MISSING — удостоверение ещё не проверено/нет допуска, LOOKUP_ERROR — ошибка. */
int topo_node_sqlite_member_keys(sqlite3* db, const char* channel_id, uint64_t node_id, int active,
    uint8_t x[32], uint8_t ed[32]);
/* Найти Ed25519 допущенного участника по полному X25519 в данной группе. Возврат как у member_keys. */
int topo_node_sqlite_member_ed_by_x(sqlite3* db, const char* channel, const uint8_t peer_x[32], uint8_t ed[32]);
/* Тот же lookup для worker: error получает точный SQLite код при LOOKUP_ERROR, иначе SQLITE_OK.
 * error необязателен; сохраняет ошибки prepare/bind/step/finalize и формата даже когда connection errcode уже OK. */
int topo_node_sqlite_member_keys_ex(sqlite3* db, const char* channel_id, uint64_t node_id, int active,
    uint8_t x[32], uint8_t ed[32], int* error);
/* Найти проверенного участника по Ed25519. id — числовое поле локальной проекции, статусы TOPO_SQLITE_LOOKUP_*. */
int topo_node_sqlite_member_local_id(sqlite3* db, const char* channel, const uint8_t author[32], int active, uint64_t* id, int* error);
/* Транспорт для первой MEMBERS сверки: полная неудалённая запись, ещё без admitted. 1/0; ошибка запрещает. */
int topo_node_sqlite_member_transport_allowed(sqlite3* db, const char* channel_id, uint64_t node_id);
/* Проверить пару ключей: transport=1 требует полную неудалённую запись; 0 — также verified и admitted.
 * При transport=0 разрешён поиск только по X25519 (ed=NULL). 1 — разрешено, 0 — нет или ошибка. */
int topo_node_sqlite_member_key_allowed(sqlite3* db, const char* channel_id, const uint8_t key[32], const uint8_t ed[32], int transport);

/* Обновить owner-блок и deleted из adm_tags в одной существующей строке;0/-1,отсутствующая строка — ошибка.
 * adm_tags/adm_tags_sig могут быть NULL;ненулевая подпись64 байта,проверяется вызывающим.
 * Проверяет все bind/step/changes=1/finalize.
 * Работает в транзакции caller;при отказе caller откатывает её до любых runtime-уведомлений. */
int topo_node_sqlite_member_owner_put(sqlite3* db, const char* channel_id, const uint8_t ed[32],
    const char* adm_tags, const uint8_t* adm_tags_sig, int storage);

/* Прочитать опубликованный JOIN с выбранным приглашением. SQLITE_OK и found=0 означают, что одного из них пока нет.
 * Подпись и время приглашения выдаёт после успешного finalize при found=1. При сбое возвращает точную SQLite-причину. */
int topo_node_sqlite_member_join_read(sqlite3* db, const char* channel_id, const uint8_t ed[32],
    uint8_t signature[64], uint64_t* epoch, int* found);
/* Прочитать обязательный JOIN. Возвращает 0 или отрицательную SQLite-причину, включая -SQLITE_NOTFOUND. */
int topo_node_sqlite_member_get_join(sqlite3* db, const char* channel_id, const uint8_t ed[32],
    uint8_t* join_sig_out, uint64_t* join_ts_out);

/* Физически удалить строку участника; 0/-1. Отсутствующая строка даёт 0; tombstone/Merkle не создаёт. */
int topo_node_sqlite_member_del(sqlite3* db, const char* channel_id, uint64_t node_id);

/* Записать проверенные вызывающим ключи/имя в nodes; 0/-1. Пустое имя сохраняет прежнее при UPDATE.
 * Ключи заменяются независимо от update_ts; update_ts хранит максимум, created_at сохраняется, если не NULL.
 * Новая строка online=1, при UPDATE online сохраняется. Подпись и свежесть здесь не проверяются.
 * Проверяет changes=1:подавленная trigger запись не считается успехом. Транзакцией управляет caller. */
int topo_node_sqlite_node_update_verified(sqlite3* db, uint64_t node_id,
    const char* name, const uint8_t* x25519, const uint8_t* ed25519, uint64_t update_ts, time_t created_at);

/* Метаданные канала: 0 — успех, LOOKUP_MISSING — нет строки/запрошенного ключа,
 * LOOKUP_ERROR — аргументы/SQL/повреждённый размер. Выходы действительны только при 0.
 * Все выходы необязательны; name_sz включает NUL, ключи по 32, подпись 64 байта. */
int topo_node_sqlite_channel_get(sqlite3* db, const char* channel_id,
    char* name_out, int name_sz, uint8_t owner_node_id[32],
    uint8_t* x25519_pub, uint8_t* ed25519_pub, uint8_t* signature);

/* Прочитать Ed25519 privkey ровно32 байта BLOB; 0/-1, отсутствие/повреждение/SQL-ошибка даёт -1.
 * Проверяются bind/step/finalize; при отказе выход не меняется. */
int topo_node_sqlite_channel_get_priv(sqlite3* db, const char* channel_id,
    uint8_t* ed25519_priv_out);

/* Прочитать оба канальных privkey (x25519 + ed25519), каждый ровно32 байта BLOB.
 * 0 — оба прочитаны, -1 — отказ с сохранением обоих выходов. Временные ключи очищаются. */
int topo_node_sqlite_channel_get_privs(sqlite3* db, const char* channel_id,
    uint8_t* x25519_priv_out, uint8_t* ed25519_priv_out);

/* Атомарно записать оба канальных privkey[32] в одну существующую строку; 0/-1.
 * Savepoint проверяет все SQL-операции и откатывает запись при отказе; отсутствие строки — ошибка.
 * При внешней транзакции её commit остаётся вызывающему; событие успеха до него запрещено.
 * Соответствие пары/подпись проверяет вызывающий. Аргументы обязательны; rollback-отказ логируется. */
int topo_node_sqlite_channel_set_privs(sqlite3* db, const char* channel_id,
    const uint8_t* x25519_priv, const uint8_t* ed25519_priv);

/* Установить online=0/1 существующей строки; 0/-1. Отсутствующая строка даёт 0 без вставки. */
int topo_node_sqlite_node_set_online(sqlite3* db, uint64_t node_id, int online);
/* online из nodes; 0, если записи нет или запрос не выполнен. */
int topo_node_sqlite_node_get_online(sqlite3* db, uint64_t node_id);
/* Скопировать ровно 32 байта Ed25519 из nodes: 0 — найден, LOOKUP_MISSING — нет строки/ключа,
 * LOOKUP_ERROR — аргументы/SQL/повреждённый размер. При отказе выход не меняется.
 * Значение байтов не определяет наличие ключа; криптографическую пригодность проверяет операция verify. */
int topo_node_sqlite_get_ed25519_pubkey(sqlite3* db, uint64_t node_id, uint8_t pubkey_out[32]);

/* Атомарно пересчитать node_type/storage/deleted во всех peers_* по адресам и adm_tags; 0/-1.
 * node_type: 0 нет адресов, 1 прямой/локальный/strict, 2 NAT/contact, 4 supernode=yes.
 * storage=yes действует только при наличии ADDR_TYPE_DIRECT, deleted берётся из adm_tags.deleted=yes. */
int topo_node_sqlite_nodeinfo_updated(sqlite3* db, const uint8_t x[32]);

/* SQL/worker: обновить nodes.rtt, единицы 0.1 ms. Открытая WRITE caller обязательна.
 * SQLITE_OK/код SQLite; отсутствующий узел допустим, новая идентичность не создаётся. */
int topo_node_sqlite_update_rtt(sqlite3* db, const uint8_t x[32], uint16_t rtt);
/* SQL/worker: обновить только совпадающий endpoint; алиасы socket_id/address_kind получают ту же оценку.
 * endpoint копируется caller; rtt>0. Транзакция/результат как у update_rtt, отсутствие endpoint допустимо. */
int topo_node_sqlite_update_address_rtt(sqlite3* db, const uint8_t x[32], const struct topo_rtt_endpoint* endpoint, uint16_t rtt);

/**
 * @brief Загружает TOPO_NODE из SQLite (ref_count=0).
 * @param groups нужен для выделения адресов из memory_pool
 * @return TOPO_NODE* или NULL, если нет пригодной записи/адресов bootstrap либо при ошибке
 *
 * Синхронно читает record и создаёт узел в uasync; worker использует раздельный record_read/record_build.
 * Сначала загружает и проверяет подписанный snapshot; при его отсутствии собирает сведения nodes/node_addresses.
 * Snapshot может быть без адресов. Bootstrap требует адресов и получает timestamp=0, профиль UNKNOWN.
 * Отказ SQL/копирования snapshot прекращает загрузку; ошибка не подменяется отсутствием записи.
 * При отказе SQL/памяти частичная запись освобождается. groups->instance->topo_groups должен совпадать с groups.
 * Вызывающий передаёт владение через topo_node_registry_store() либо вызывает topo_node_destroy().
 */
struct TOPO_NODE* topo_node_sqlite_node_load(sqlite3* db, struct TOPO_GROUPS* groups, uint64_t node_id);
/* Синхронно загрузить только известную идентичность nodes, без адресов/подписи/timestamp; вызывать в uasync.
 * Для NCD с независимо подтверждённым LAN-адресом. Проверяет размеры ключей и node_id=derive(X25519).
 * Владельческий TOPO_NODE или NULL; уничтожить topo_node_destroy либо передать registry. */
struct TOPO_NODE* topo_node_sqlite_identity_load(sqlite3* db, uint64_t node_id);

/* Записать connected=0/1 существующего участника; 0/-1, без вставки отсутствующей строки. */
int topo_node_sqlite_set_connected(sqlite3* db, const char* channel_id, const uint8_t ed[32], int connected);

/* Кандидат CHAT-подключения: роль канальная, тип устройства из подписанного snapshot.
 * UINT8_MAX означает отсутствие сведений о типе; адреса не ограничивают выборку. */
struct topo_connect_peer {
    uint64_t node_id;
    uint8_t ed[32]; /* Ed25519 участника из peers. */
    uint8_t client_type, supernode;
};

/* Допущенные полные неудалённые участники, включая self; сначала connected, затем RTT и Ed25519.
 * 0/отрицательный код SQLite (в том числе NOMEM/CORRUPT/finalize); ошибка/пусто дают NULL/0.
 * Массив содержит только собственные значения, принадлежит caller/u_free; можно читать в worker. */
int topo_node_sqlite_get_connect_peers(sqlite3* db, const char* channel_id,
                                     struct topo_connect_peer** out, size_t* out_count);
/* Нижеследующие выборки: 0/-1, пусто/ошибка — NULL/0, частичный массив не выдаётся; освобождение u_free.
 * connected/supernode ограничены участниками канала, self не исключают, deleted=1 исключают.
 * public/local — глобальные адреса с записью nodes, исключены self и deleted=1 в указанном канале;
 * наличие членства в канале не требуется. public: addr_type 1/2, local: 0/3; фильтра port!=0 нет. */
int topo_node_sqlite_get_connected_peers(sqlite3* db, const char* channel_id, uint64_t** out_ids, int* out_count);
/* Лучший подтверждённый тип публикуемых endpoint: DIRECT(1), EIM(2), STRICT(3), иначе0.
 * Исключает адреса без порта, LOCAL/PRIVATE; использует ту же классификацию, что проекция SQLite. */
unsigned topo_node_best_confirmed_type(const struct TOPO_NODE *node);
int topo_node_sqlite_get_public_peers(sqlite3* db, const char* channel_id, uint64_t** out_ids, int* out_count, uint64_t self_node_id);
int topo_node_sqlite_get_local_peers(sqlite3* db, const char* channel_id, uint64_t** out_ids, int* out_count, uint64_t self_node_id);
int topo_node_sqlite_get_supernode_peers(sqlite3* db, const char* channel_id, uint64_t** out_ids, int* out_count);

/* node_type узла в канале (peers_<ch>.node_type); -1 если нет записи/ошибка. */
int topo_node_sqlite_get_node_type(sqlite3* db, const char* channel_id, uint64_t node_id);

#endif
