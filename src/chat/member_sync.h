/*
 * member_sync — подписанные записи участников и состав CHAT-группы.
 * JOIN и профиль подписывает участник, права и отзыв — ключ группы.
 * Для каждой пары Ed25519 участника и пригласившего хранится свой сертификат.
 * Адаптер читает типизированные записи из Merkle-индекса и временно сохраняет страницы
 * входящего прохода в SQLite. После проверки корня одним WRITE объединяет подписанные
 * данные, обновляет дерево и пересчитывает допуск. Уведомления отправляются после COMMIT.
 *
 * Допуск вычисляется по достижимым цепочкам приглашений и действующей политике.
 * identity_verified удостоверяет ключи для истории, admitted разрешает текущие операции.
 * Таблица nodes и NODEINFO содержат информацию для маршрутизации. Доверие к участнику
 * строится по его канонической паре Ed25519/X25519 и подписанным данным в группе.
 * При запуске проверяются и восстанавливаются независимые части локальных записей.
 *
 * SQL-функции вызываются потоком владельца SQLite, runtime и callbacks — потоком uasync.
 * Формат записей и порядок проверки описаны в member_sync_doc.md.
 */

#ifndef MEMBER_SYNC_H
#define MEMBER_SYNC_H

#include "merkle_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

struct UTUN_INSTANCE;
struct TOPO_GROUP;
struct sqlite3;

/* Допуск автора отделён от ожидания ключей и отказа самой проверки. */
enum ms_author_status {
    MS_AUTHOR_ERROR = -1,     /* SQL/OpenSSL/аргументы: проверка не выполнена */
    MS_AUTHOR_INACTIVE = 0,   /* участник отсутствует, source!=0 или deleted=1 */
    MS_AUTHOR_ACTIVE = 1,     /* удостоверение полностью проверено */
    MS_AUTHOR_PENDING = 2,    /* ещё нет ключа или подтверждения приглашения */
    MS_AUTHOR_INVALID = 3     /* неверная подпись, идентичность или формат удостоверения */
};

/* Проверить текущий допуск; возвращает MS_AUTHOR_*, key[32] меняется только при ACTIVE.
 * Неактивность и ожидание зависимости не являются ошибками БД или доказательством порчи.
 * db/channel/key обязательны; channel — публичный Ed25519 группы в строчном HEX.
 * Только SQL/crypto: допускается DB worker в согласованной транзакции caller, runtime не используется. */
enum ms_author_status member_sync_author_key(struct sqlite3* db, const char* channel,
        uint64_t author, uint8_t key[32]);
/* Та же полная crypto-проверка; error получает точный положительный SQLite-код для MS_AUTHOR_ERROR,
 * включая prepare/bind/step/finalize когда connection errcode уже сброшен. Для остальных статусов error=SQLITE_OK. */
enum ms_author_status member_sync_author_key_ex(struct sqlite3* db, const char* channel,
    uint64_t author, uint8_t key[32], int* error);
/* Проверить актуальный допуск автора по полному Ed25519. error получает SQLite-код при MS_AUTHOR_ERROR. */
enum ms_author_status member_sync_verify_author(struct sqlite3* db, const char* channel, const uint8_t ed[32], int* error);
/* SQL/worker: проверить основание для входящего MEMBERS от закреплённой пары ed/x.
 * Требуется подписанное удостоверение JOIN и криптографически действительный сертификат приглашения.
 * valid получает 0 или 1. Возврат — SQLITE_OK или положительный SQLite-код. */
int member_sync_recovery_source_read(struct sqlite3* db, const char* channel, const uint8_t ed[32], const uint8_t x[32], int* valid);
/* SQL: прочитать публичные ключи группы. db/channel/found обязательны, x и ed опциональны.
 * SQLITE_OK и found=1 — ключи скопированы, found=0 — отсутствуют. Иначе код SQLite. */
int member_sync_channel_keys_read(struct sqlite3* db, const char* channel, uint8_t* x, uint8_t* ed, int* found);

/* SQL/worker: добавить канонические записи листа MEMBERS в начатый SHA256, не завершать hash.
 * channel — канонический ID, level=MT_MAX_LEVEL, prefix нормализован по level.
 * Включает описание/политику в нулевом листе. Число записей либо отрицательный код SQLite.
 * Runtime, сеть и регистрация Merkle-модели не нужны; снимок/транзакция принадлежат caller.
 * При ошибке промежуточный hash отбросить: частичный digest не является результатом. */
int member_sync_leaf_hash(struct sqlite3* db, const char* channel, uint8_t level,
    uint64_t prefix, EVP_MD_CTX* hash);
/* SQL/worker: обновить записи JOIN и прав владельца для ed и пересчитать их пути в текущей WRITE.
 * ed=NULL обновляет лист описания и политики. 1=изменилось,0=прежний индекс,отрицательный код SQLite=ошибка.
 * Без runtime/событий/собственного commit; caller откатывает ошибку и уведомляет только после commit. */
int member_sync_reindex(struct sqlite3* db, const char* channel, const uint8_t ed[32]);
/* Префикс листа записи JOIN для Ed25519 участника. */
uint64_t member_sync_leaf_prefix(const uint8_t ed[32]);

/* Проверить source=0/deleted=0/admitted=1, проверенную цепочку группы и привязку node_id/X25519,
 * join_sig и подпись непосредственного пригласившего (либо ключом канала для signed_by=0).
 * Не обходит цепочку предков. inst/channel/key обязательны, channel — публичный Ed25519 группы в строчном HEX.
 * 0 — ключ записан в key[32], -1 — отказ, key не меняется. */
int member_sync_verified_ed25519(struct UTUN_INSTANCE* inst, const char* channel, uint64_t author, uint8_t key[32]);

/* Результат применения рекорда мембера (compare+update по версиям двух блоков). */
#define MS_APPLY_CHANGED 0x01  /* запись и дерево обновлены */
#define MS_APPLY_STALE   0x02  /* локальная версия новее; обратный проход доставит её пиру */
#define MS_VERIFY_DEFERRED (-1) /* зависимость подписи ещё не получена */
#define MS_VERIFY_ERROR    (-2) /* SQL/crypto/аргументы: проверка не выполнена */

/* UTF-8 JSON bytes, excluding NUL. A complete record fits in one Merkle page. */
#define MS_ADM_TAGS_MAX 8192
#define MS_USERINFO_MAX 4096
/* Проверить весь строковый JSON owner-блока,уникальность ключей и ver1..INT_MAX без ведущих нулей.
 * Возвращает версию,0 для NULL/пустого блока,-1 при ошибке формата/SQL. Подпись здесь не проверяется. */
int member_sync_owner_version(struct sqlite3* db, const char* tags);
/* Подписанный счётчик отзыва сообщений: отсутствующий тег=0; 0/-1, out меняется при успехе. */
int member_sync_owner_epoch(const char* tags, uint64_t* epoch);
/* Полный порядок owner-блоков: epoch, ver, удаление, меньшая подпись, меньшие байты JSON. -1/0/1; -2 при неверном формате. */
int member_sync_owner_compare(struct sqlite3* db, const char* left, const uint8_t* left_sig, const char* right, const uint8_t* right_sig);

#define MS_OWNER_DOMAIN "utun-member-rights-v3"
#define MS_OWNER_PREFIX_SIZE (sizeof(MS_OWNER_DOMAIN) - 1 + 32)
#define MS_OWNER_MSG_MAX (MS_OWNER_PREFIX_SIZE + MS_ADM_TAGS_MAX)
/* Подписываются домен без NUL, Ed25519 участника и полный JSON tags. Подпись проверяется ключом группы.
 * Права принадлежат участнику группы и переживают смену его транспортного адреса.
 * Записывает сообщение в out и возвращает длину; -1 при неверных аргументах или коротком буфере. */
int member_sync_build_owner_msg(const char* channel, const uint8_t ed[32], const char* tags, uint8_t* out, size_t capacity);

/* Поле подписанта в кодограмме содержит 32 нуля для подписи ключом группы.
 * Возвращает NULL для группы, иначе заимствованный указатель на Ed25519 участника. */
const uint8_t* member_sync_signer(const uint8_t key[32]);

/* Представление записи; все указатели заимствованы на время вызова. */
struct ms_member_rec {
    uint64_t       node_id;      /* derive(x25519), индекс локального кеша маршрутов */
    const uint8_t* x25519;       /* 32 байта, обязательно */
    const uint8_t* ed25519;      /* Полный публичный ключ участника списка группы, 32 байта, обязательно */
    const uint8_t* join_sig;     /* 64 байта, может быть NULL */
    uint64_t       join_ts;      /* Время приглашения под подписью пригласившего в секундах Unix, 1..INT64_MAX. */
    const uint8_t* update_sig;   /* 64 байта, может быть NULL */
    uint64_t       update_ts;    /* ver блока мембера */
    const char*    userinfo;     /* JSON {"name":...}, может быть NULL */
    const char*    adm_tags;     /* JSON владельца с "ver", может быть NULL */
    const uint8_t* adm_tags_sig; /* 64 байта, может быть NULL */
    int            storage;     /* вход не используется: сохраняемое значение выводится из adm_tags.storage="yes" */
    const uint8_t* signed_by;    /* дерево приглашений: 0 = корень (подписан ключом группы), иначе полный Ed25519 пригласившего */
    const uint8_t* signature;    /* Подпись пригласившего (или группы) по формату member_sync_build_sign_msg. */
};

struct group_invitation_candidate;
/* Uasync после COMMIT: сохранить приглашение себе отдельной записью личного журнала.
 * NULL выбирает текущее приглашение. Повтор не добавляет блок, сбой журнала не отменяет допуск. */
void member_sync_audit_invitation(struct UTUN_INSTANCE* inst, const char* channel, const struct group_invitation_candidate* candidate);

/* Собственная копия uasync-идентичности для SQL WRITE; секреты очистить после done.
 * Ключи требуются только при подписании записи отзыва канбана. */
struct member_sync_write_context {
    uint64_t self; /* Производный индекс локального узла для операций с журналом. */
    uint64_t now; /* Согласованное Unix-время в микросекундах; копируется до dispatch. */
    uint8_t public_key[32], private_key[32]; /* Собственная идентичность подписанта. */
    int sqlite_code; /* Выход: положительная SQLite-причина при MT_ERR_IO, иначе SQLITE_OK. */
};
/* SQL/worker в открытой WRITE caller: проверить/обновить одну запись, derived rows и MEMBERS-индекс.
 * source_ed указывает на Ed25519 отправителя сетевой записи. NULL передаёт только локальный писатель.
 * При несовпадении ключей пира с подтверждённой парой участника возвращает MT_ERR_CONFLICT.
 * Не начинает транзакцию, не меняет admission и не публикует runtime. Все входы заимствованы до возврата.
 * Возвращает MS_APPLY_* либо MT_ERR_*; при IO использовать context.sqlite_code, затем откатить всю WRITE.
 * После успеха caller вызывает group_policy_project в той же WRITE, затем publish/record_committed после COMMIT. */
int member_sync_record_write(struct sqlite3* db, struct member_sync_write_context* context,
    const char* channel, const uint8_t source_ed[32], const struct ms_member_rec* record);

/* ── Дерево приглашений (подтверждение конкретного JOIN) ── */

/* Участник определяется публичным Ed25519. Каждый узел проверяет его каноническое
 * преобразование в X25519 до принятия удостоверения. Два разных X25519 одного
 * Ed25519 не могут пройти эту проверку даже при действительных подписях участника.
 * Права владельца привязаны к полному Ed25519. Порядок и курсор Merkle задаёт
 * типизированный ключ записи. Контекст временного приёма отделён от опубликованного состава. */
#define MEMBER_JOIN_MSG_SIZE 136
/* SHA-256 тела JOIN. Вариант read восстанавливает тело из ключей группы и адреса участника. Возврат — SQLite-код. */
int member_sync_join_id(const uint8_t body[MEMBER_JOIN_MSG_SIZE], uint8_t id[32]);
int member_sync_join_id_read(struct sqlite3* db, const char* channel, const uint8_t node_ed[32], const uint8_t x[32], uint8_t id[32]);
#define MEMBER_SIGN_MSG_SIZE 448 /* Ёмкость канонического JSON приглашения, включая завершающий NUL. */

/* Построить канонический JSON приглашения в out ёмкостью capacity. Возвращает число подписываемых
 * байтов без NUL или -1. signer_ed=NULL кодирует "group" для ключа группы, иначе signer содержит 64 HEX публичного Ed25519; join_ts — Unix-время приглашения. */
int member_sync_build_sign_msg(const char* channel, const uint8_t signer_ed[32], const uint8_t x25519[32], const uint8_t ed25519[32],
    const uint8_t join_id[32], uint64_t join_ts, uint8_t* out, size_t capacity);

/* Подписать эти же поля личным Ed25519-ключом пригласившего или ключом группы. sig — выход 64 байта, результат 0/-1. */
int member_sync_sign_join(const uint8_t signer_ed_priv[32], const char* channel, const uint8_t signer_ed[32],
    const uint8_t x25519[32], const uint8_t ed25519[32], const uint8_t join_id[32], uint64_t join_ts, uint8_t sig[64]);

/* Проверить подтверждение пары ключей участника и конкретного JOIN в ch_id.
 * signed_by==0 → ключ канала (ch_ed25519), иначе ed25519 pubkey узла signed_by.
 * Возвращает 1 если подпись валидна, 0 если невалидна, MS_VERIFY_DEFERRED если нет ключа,
 * MS_VERIFY_ERROR при отказе SQL/crypto. Отсутствие ключа не доказывает порчу записи. */
int  member_sync_verify_signature(struct sqlite3* db, const char* ch_id, const struct ms_member_rec* m);

/* Найти полный X25519 в опубликованных строках peers_<ch_id>. Возвращает 1/0 (ошибки тоже дают 0).
 * Проверяет присутствие независимо от source, deleted и подписей.
 * Для допуска в CHAT-группу используется отдельный topo_node_sqlite_member_allowed. */
int  member_sync_validate_pubkey(struct UTUN_INSTANCE* inst, const char* ch_id,
                                 const uint8_t* x25519_pubkey);

/*
 * Инициализировать модуль: создаёт merkle_sync с коллбэками для мемберов
 * (ETCP_ID_MEMBER_SYNC), проверяет локальные записи, восстанавливает деревья
 * и подписывается на события CHAT-групп. 0/-1.
 */
/* Legacy RAM/RO writer; RW использует prepare/activate, init отклоняется до SQL. */
int  member_sync_init(struct UTUN_INSTANCE* inst);
struct member_sync_prepared;
/* SQL/worker, WRITE caller: проверить сохранённые записи, удалить незавершённый приём,
 * пересчитать MEMBERS индекс и committed-снимки допуска всех групп. Без runtime/сети/своего BEGIN.
 * SQLITE_OK/положительный SQLite-код; out=NULL при ошибке. self — локальный node_id.
 * После commit вызвать activate; после rollback только prepared_free. */
int member_sync_prepare(struct sqlite3* db, uint64_t self, struct member_sync_prepared** out);
/* Uasync после commit: создать Merkle runtime и опубликовать подготовленные снимки без SQL/callbacks.
 * 0/-1; prepared остаётся caller, успешно опубликованные снимки передаются instance.
 * Вызывающий всегда завершает prepared_free; повторная activation одного prepared запрещена. */
int member_sync_activate(struct UTUN_INSTANCE* inst, struct member_sync_prepared* prepared);
/* Только память, любой поток до публикации либо uasync после activate; NULL допустим. */
void member_sync_prepared_free(struct member_sync_prepared* prepared);

/* Завершить модуль. */
void member_sync_destroy(struct UTUN_INSTANCE* inst);

/* Подписаться на BGP node-события chat-группы (BGP → member_sync → node_props_changed).
   Вызывается при создании канала (chat_core_ensure_channel_ready). */
/* Идемпотентно подписать CHAT-группу; 0/-1, отказ регистрации явно возвращается. */
int member_sync_subscribe_group(struct UTUN_INSTANCE* inst, struct TOPO_GROUP* group);

/*
 * Запустить синхронизацию мемберов канала ch_id с пиром peer.
 * Проверяет право обмена/запроса, затем вызывает merkle_sync_start(MEMBERS).
 * 0 — запрос принят; <0 — не принят, done_cb не вызывается (см. merkle_sync.h).
 * Прежние записи сохраняются для проверки и fallback после получения различий.
 */
int  member_sync_start(struct UTUN_INSTANCE* inst, struct ETCP_CONN* conn,
         const char* ch_id, merkle_sync_done_cb done_cb, void* arg);

/* Допуск MEMBER_ADMIN по локально проверенным JOIN обеих сторон и ключу живого ETCP. */
int member_sync_admin_allowed(struct UTUN_INSTANCE* inst, const char* channel, struct ETCP_CONN* conn);

/*
 * Отменить синхронизацию. Делегирует в merkle_sync_cancel().
 */
void member_sync_cancel(struct UTUN_INSTANCE* inst, const uint8_t peer[32], const char* ch_id);

/*
 * Отменить все merkle-сессии канала (локальное удаление канала).
 * Делегирует в merkle_sync_cancel_ns().
 */
void member_sync_cancel_channel(struct UTUN_INSTANCE* inst, const char* ch_id);

/*
 * Добавить/обновить мембера (локальная запись, доверенная) и пересчитать дерево.
 * Делегирует в member_sync_apply_record() с source_ed = NULL.
 *
 * ch_id       — ID канала
 * x25519      — X25519 публичный ключ (32 байта)
 * ed25519     — Ed25519 публичный ключ (32 байта)
 * join_sig    — Ed25519 подпись join-сообщения (64 байта)
 *               сообщение: "utunJOIN" || ch_x25519(32) || ch_ed25519(32) || node_ed25519(32) || x25519(32).
 * join_ts     — Unix-время приглашения (секунды); включено в JSON под подписью пригласившего.
 * update_sig  — подпись блока A (64 байта), update_ts — версия блока A
 * userinfo    — метаданные узла (имя) в канале
 * adm_tags    — атрибуты владельца (JSON с ver/storage), adm_tags_sig — подпись канальным ключом
 * storage     — производная от adm_tags.storage
 *
 * Пишет блоки через topo_node_sqlite_member_block_put/member_owner_put, узел —
 * через topo_node_sqlite_node_update_verified, затем merkle_sync_recompute_path().
 *
 * Возвращает маску MS_APPLY_* (0 = no-op) или -1 при ошибке.
 */
int  member_sync_put(struct UTUN_INSTANCE* inst, const char* ch_id,
         const uint8_t* x25519,
         const uint8_t* ed25519,
         const uint8_t* join_sig, uint64_t join_ts,
         const uint8_t* update_sig, uint64_t update_ts,
         const char* userinfo,
         const char* adm_tags, const uint8_t* adm_tags_sig, int storage,
         const uint8_t* signed_by, const uint8_t* signature);

/*
 * Строит сообщение для update_sig блока A (мембер):
 *   join_id(32) || update_ts(8BE) || userinfo(с NUL)
 * Используется и для подписи, и для верификации — байты обязаны совпадать.
 * Возвращает длину сообщения или -1.
 */
int  member_sync_build_update_msg(const uint8_t* join_id, uint64_t update_ts,
         const char* userinfo,
         uint8_t* out, int out_sz);

/*
 * Строит сообщение для join_sig (каноническая раскладка 136 байт):
 *   "utunJOIN"(8) || ch_x25519(32) || ch_ed25519(32) || node_ed25519(32) || node_x25519(32)
 * Время приглашения подписывает пригласивший в JSON.
 * Единственный источник истины для подписи и верификации join_sig во всех модулях.
 * Возвращает 136 при успехе или -1 при ошибке.
 */
int  member_sync_build_join_msg(const uint8_t* ch_x25519, const uint8_t* ch_ed25519,
         const uint8_t* node_ed25519, const uint8_t* node_x25519,
         uint8_t* out, int out_sz);

/*
 * Применить рекорд мембера с по-блочным сравнением версий:
 *  - блок A (мембер): ver = update_ts (для постоянного join_id), подписан ключом мембера;
 *  - блок B (владелец): ver = adm_tags.ver, подписан канальным ключом (adm_tags_sig).
 * Блок A сравнивает update_ts. При равной версии выигрывает меньшая подпись update_sig.
 * Блок B сравнивается по эпохе, затем по adm_tags.ver, при равенстве выигрывает удаление, затем меньшая подпись.
 * Отсутствующее подтверждение приглашения может дополниться без увеличения версий.
 * source_ed задаёт полный адрес отправителя. Только локальный писатель передаёт NULL для уже проверенных данных;
 * подпись непустого блока B проверяется также для локальной записи.
 * Если ver пришедшего блока меньше локального — блок игнорируется и выставляется
 * MS_APPLY_STALE (локальная версия новее; её доставит обратный проход).
 * При внешней транзакции caller после commit вызывает member_sync_record_committed
 * для изменённой записи; до commit автоматических уведомлений нет.
 * Отказ ROLLBACK TO не выполняет RELEASE частичных изменений. Владелец внешней транзакции
 * при ошибке откатывает её целиком;собственная транзакция пытается полный ROLLBACK,а при его отказе запрашивает stop/DB close.
 * Возвращает битовую маску MS_APPLY_* (0 — без изменения индекса), <0 — ошибка.
 */
/* Legacy RAM/RO writer; RW отклоняется до SQL. Worker использует record_write и postcommit publication. */
int  member_sync_apply_record(struct UTUN_INSTANCE* inst, const char* ch_id,
         const uint8_t source_ed[32], const struct ms_member_rec* m);

/*
 * Верифицировать локальную запись мембера (чтение из БД). Единственный сценарий
 * удаления: подпись присутствует и невалидна, либо node_id != derive(x25519).
 * Отсутствие подписи = «не верифицировано» (self-add/плейсхолдер) — не битая.
 * Возвращает 1 если запись сохраняется (валидна/не подписана/ждёт зависимость),
 * 0 если доказанно битая, -1 при операционной ошибке. Только 0 разрешает удаление.
 */
int  member_sync_verify_local_record(struct sqlite3* db, const char* ch_id,
         const struct ms_member_rec* m);
/* Та же SQL/crypto проверка с точной операционной SQLite-причиной (0 при валидном/невалидном доказательстве).
 * sqlite_code обязателен, выход устанавливается до finalize; 1/0/-1 как у legacy wrapper. */
int member_sync_verify_local_record_ex(struct sqlite3* db, const char* channel, const struct ms_member_rec* record,
    int* sqlite_code);

/* Проверить и восстановить опубликованные записи канала и их индексы. Для повреждённого профиля
 * сохраняются действительный JOIN и подписанный блок владельца. Строка только с отзывом получает source=1.
 * Требуются initialized member_sync, общая БД в autocommit и публичный Ed25519 группы в строчном HEX ch_id.
 * Чужая транзакция отклоняется до изменений; недостающие ключи/зависимости сохраняются.
 * >=0 — число удалений после commit, -1 — отказ; до commit уведомления/mute/группа не меняются.
 * Props callbacks можно снимать/добавлять и вызывать очистку повторно; stop/destroy — вне callbacks. */
int  member_sync_verify_and_purge(struct UTUN_INSTANCE* inst, const char* ch_id);

/* Очистить все peers_*: 0/-1, остановиться на первом отказе. Атомарность — по каналу;
 * ранее завершённые каналы сохраняют результат. Не держит SQLite cursor через callbacks. */
int member_sync_verify_and_purge_all(struct UTUN_INSTANCE* inst);

/* Legacy RAM/RO writer: число строк включая placeholders/deleted; ошибка/RW дают0 с логом.
 * В production читать count внутри общего worker snapshot списка участников. */
int member_sync_count(struct UTUN_INSTANCE* inst, const char* ch_id);

/*
 * Коллбэк: изменились свойства/состояние любого узла (включая себя).
 * channel_id — канал (namespace), в котором произошло изменение.
 * Уведомляет об изменении свойств после commit и при обновлении состояния узла в BGP.
 * Передаёт идентификаторы без SQL-чтения; потребитель использует runtime либо собственный READ-снимок.
 * Синхронный, многоподписочный; channel_id заимствован только на время вызова.
 */
typedef void (*node_props_changed_fn)(const uint8_t ed[32], const char* channel_id, void* arg);
/* 0 — зарегистрирован, -1 — неверные аргументы/память; потребитель может откатить свой init. */
int member_sync_add_props_cbk(struct UTUN_INSTANCE* inst, node_props_changed_fn fn, void* arg);
/* Uasync, после commit допуска/свойств: уведомить потребителей по ID без обращения к БД. */
void member_sync_notify_props(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32]);
void member_sync_remove_props_cbk(struct UTUN_INSTANCE* inst, node_props_changed_fn fn, void* arg);

/* После внешнего commit уведомить MEMBERS, свойства и планировщик CHAT-группы;
 * также переоценить допуск MESSAGES и ожидание общих ключей исторических авторов.
 * changes — маска MS_APPLY_* из apply_record/put. ed — обязательный ключ изменённой записи.
 * Вызывать в uasync после завершения транзакции. */
void member_sync_record_committed(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32], unsigned changes);

#ifdef __cplusplus
}
#endif
#endif /* MEMBER_SYNC_H */
