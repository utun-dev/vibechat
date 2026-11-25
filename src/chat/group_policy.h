/* Общие права приглашения CHAT-группы. Политика подписана ключом группы и входит в
 * MEMBERS Merkle вместе с удостоверениями участников. После сверки проверяем дерево
 * приглашений до ключа группы: identity_verified подтверждает ключи участника,
 * admitted дополнительно учитывает текущие роли, политику и исключение предков.
 * Оба результата сохраняются с составом одной транзакцией. После commit публикуется
 * снимок прав/ключей в памяти; сетевые проверки обходятся без SQL. NODEINFO даёт
 * маршруты, но не доверенные ключи. Снимок резервируется до commit, rollback его не меняет.
 * CHAT-контент удостоверяет member_log: отзыв закрывает поколение контрольной точкой,
 * подписанной владельцем. Префикс остаётся общей историей; хвост вне среза неприменим.
 * Поколение — JOIN + подписанный владельцем счётчик _message_epoch в adm_tags.
 * Административный отзыв увеличивает счётчик всех потерявших допуск; восстановление сохраняет его.
 * Owner-блоки сравниваются по (счётчик,ver,JSON), поэтому поздняя правка не отменяет известный отзыв.
 * Runtime-операции выполняются в uasync; функции только с sqlite3 допускают поток владельца SQL.
 * Строки channel — публичный Ed25519 группы в строчном HEX. */
#ifndef GROUP_POLICY_H
#define GROUP_POLICY_H
#include <stddef.h>
#include <stdint.h>
#include "merkle_sync.h"
struct UTUN_INSTANCE;
struct member_sync_write_context;
struct sqlite3;
struct group_policy_update;
struct group_policy_cache;
#define GROUP_MESSAGE_EPOCH_TAG "_message_epoch"
#ifdef __cplusplus
extern "C" {
#endif

#define GROUP_POLICY_WIRE_SIZE 73
struct group_policy {
    uint64_t version; /* 0 — исходное admins-only; остальные версии подписаны. */
    uint8_t all_members; /* 0 — владелец/администраторы, 1 — все действующие участники. */
    uint8_t signature[64]; /* Ed25519 ключом группы: domain, channel, version, mode. */
};

/* Схема общей таблицы политик; 0/-1, допускает внешнюю транзакцию. */
int group_policy_schema(struct sqlite3* db);
/* Прочитать и проверить политику существующей группы; 0/отрицательная ошибка SQLite, out меняется только при успехе. */
int group_policy_get(struct sqlite3* db, const char* channel, struct group_policy* out);
/* Кодирование фиксированной wire-записи. Размер буфера проверяет вызывающий код. */
void group_policy_encode(const struct group_policy* policy, uint8_t out[GROUP_POLICY_WIRE_SIZE]);
void group_policy_decode(const uint8_t wire[GROUP_POLICY_WIRE_SIZE], struct group_policy* out);
/* Проверить формат и подпись. SQLITE_OK с valid=0 означает неверные данные. Положительная ошибка SQLite означает локальный отказ. */
int group_policy_check(struct sqlite3* db, const char* channel, const struct group_policy* policy, int* valid);
/* При равной версии ограничивающая политика побеждает. Возвращает 1, если candidate выигрывает. */
int group_policy_newer(const struct group_policy* candidate, const struct group_policy* current);
/* Применить политику в транзакции caller; 1=изменена,0=прежняя,отрицательный SQLite-код=ошибка. */
int group_policy_apply(struct sqlite3* db, const char* channel, const struct group_policy* policy);
/* Legacy RAM/RO writer: пересчитать admitted и снять недопущенные сессии;0/-1, RW отклоняется.
 * Worker использует project в своей WRITE, затем publish после commit. */
int group_policy_refresh(struct UTUN_INSTANCE* inst, const char* channel);
/* Локальная административная команда: внутри своей транзакции сменить подписанное поколение всех
 * участников, теряющих допуск. Вызывать после изменения ролей/политики, до project/commit.
 * Получатель MEMBERS эту функцию не вызывает. 0/-1; ключ группы берётся из общей БД. */
int group_policy_seal_revocations(struct UTUN_INSTANCE* inst, const char* channel);
/* SQL/worker, открытая WRITE caller: тот же отзыв без runtime; context содержит frozen self/время/ключи.
 * 0/-1; при отказе откатить WRITE. Секреты context остаются caller, runtime публикуется после commit/project. */
int group_policy_seal_revocations_write(struct sqlite3* db, struct member_sync_write_context* context, const char* channel);
/* Получить поколение из JOIN и подписанной служебной метки владельца; текущий допуск проверяет caller.
 * 0 — получено, 1 — удостоверение отсутствует, -1 — SQL/формат/crypto ошибка; out[32] меняется только при успехе. */
int group_policy_message_generation(struct sqlite3* db, const char* channel, uint64_t node, uint8_t out[32]);
/* Та же версия прав для подписанного Ed25519 автора. Возврат 0/1/-1 как у числового SQL-вызова. */
int group_policy_key_generation(struct sqlite3* db, const char* channel, const uint8_t ed[32], uint8_t out[32]);
/* SQL/worker: рассчитать и записать допуск в открытой транзакции caller, без runtime/сети.
 * self — локальный ID для архивных снимков mine (0 допустим). 0 либо отрицательный SQLite-код;
 * out принадлежит caller: после COMMIT вызвать publish в uasync, после ROLLBACK — discard. */
int group_policy_project(struct sqlite3* db, uint64_t self, const char* channel, struct group_policy_update** out);
/* Подписанный сертификат приглашения. wins заполняет только group_policy_choose_invitations. */
struct group_invitation_candidate {
    uint8_t subject[32], signer[32]; /* Ed25519 адресата и пригласившего. Нули в signer обозначают ключ группы. */
    uint64_t stamp; /* Подписанный join_ts. */
    uint8_t signature[64]; /* Подпись пригласившего. */
    int wins;
};
/* SQL: проверить кандидатов и выбрать пути по всему снимку сразу. SQLITE_AUTH — неверная подпись, иначе SQLite-код. */
int group_policy_choose_invitations(struct sqlite3* db, const char* channel, struct group_invitation_candidate* candidates, size_t count);

/* Сначала заменить снимок прав/ключей, затем уведомить сервисы после commit и освободить update.
 * Новых аллокаций для публикации снимка нет; callbacks могут удалять группу. */
void group_policy_publish(struct UTUN_INSTANCE* inst, struct group_policy_update* update);
/* notify_sync=0 передаёт групповые уведомления вызывающему batch publisher; допуск публикуется сразу. */
void group_policy_publish_changes(struct UTUN_INSTANCE* inst, struct group_policy_update* update, int notify_sync);
/* Освободить расчёт без уведомлений (rollback/stop); NULL допустим. */
void group_policy_discard(struct group_policy_update* update);
/* SQL bootstrap: забрать готовый снимок без уведомлений; update остаётся caller/discard.
 * Возвращает принадлежащий caller cache либо NULL, если уже забран; после commit publish, после rollback free. */
struct group_policy_cache* group_policy_update_take_snapshot(struct group_policy_update* update);
/* Текущий допуск из подтверждённого снимка: transport=0 требует admitted, transport=1
 * разрешает только обмен MEMBERS с полным неудалённым участником. 1/0, ошибка запрещает доступ.
 * Вызывать в uasync вне SQL-транзакции; внутри SQL использовать topo_node_sqlite_member_*.
 * Для изолированных RAM-тестов без снимка допускается чтение :memory:, файлового fallback нет. */
int group_policy_member_access(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, int transport);
/* Скопировать проверенные ключи из снимка; active=1 дополнительно требует текущий допуск.
 * 0=успех, TOPO_SQLITE_LOOKUP_MISSING/ERROR; x/ed (хотя бы один) меняются только при успехе. */
int group_policy_member_keys(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, int active,
    uint8_t x[32], uint8_t ed[32]);
/* Проверить допуск точной пары X25519/Ed25519; transport=1 разрешает старт MEMBERS для неудалённой строки.
 * При transport=0 требуется допуск. ed=NULL допустим только для поиска транспорта по X25519. 1/0, uasync. */
int group_policy_member_key_allowed(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t key[32], const uint8_t ed[32], int transport);
/* Uasync: подписанная идентичность по Ed25519, независимо от текущего допуска. 1=подтверждена, 0=отсутствует. */
/* Найти Ed25519 допущенного участника по полному X25519 в готовом снимке. 0 — найдено, иначе LOOKUP_MISSING/ERROR. */
int group_policy_member_ed_by_x(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t x[32], uint8_t ed[32]);
int group_policy_member_verified(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32]);
/* Проверить пару ключей по committed-снимку независимо от допуска. Только uasync, 1/0. */
int group_policy_member_verified_key(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t x[32], const uint8_t ed[32]);
/* Uasync/noSQL: ключ пира совпадает с последним committed основанием получить MEMBERS;1/0.
 * Это разрешение только на собственный receive-only recovery, без передачи страниц пиру. */
int group_policy_recovery_source(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32], const uint8_t x[32]);
/* Uasync/noSQL: подписанная опция clipboard_members из последнего committed group snapshot;1/0 fail-closed. */
int group_policy_clipboard_members(struct UTUN_INSTANCE* inst, const char* channel);
/* Заимствовать подтверждённый Ed25519 группы до замены снимка; NULL при отсутствии. Только uasync. */
const uint8_t* group_policy_public_key(struct UTUN_INSTANCE* inst, const char* channel);
enum group_member_role { GROUP_MEMBER_SUPERNODE = 1, GROUP_MEMBER_STORAGE = 2 };
/* Uasync: supernode=yes либо storage=yes из подтверждённого блока владельца; требуется admitted и verified.
 * role — ровно одна константа group_member_role.
 * 1=роль есть,0=нет роли/снимка/допуска,-1=неверные аргументы или ошибка изолированного RAM fallback.
 * Файловая БД не читается; роль подготовлена snapshot_read и меняется вместе с публикацией прав. */
int group_policy_member_role(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, enum group_member_role role);
/* Та же проверка роли по полному Ed25519, включая узлы с одинаковым числовым адресом. */
int group_policy_member_role_key(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32], enum group_member_role role);
/* Uasync, без SQL/выделений: следующий допущенный verified участник с role и ID строго больше after.
 * after=0 начинает обход; вернуть node_id,0 — конец/нет снимка/ошибка аргументов с логом.
 * ID упорядочены как uint64; снимок может заменяться между вызовами, заимствованных указателей нет. */
uint64_t group_policy_role_next(struct UTUN_INSTANCE* inst, const char* channel, enum group_member_role role, uint64_t after);
/* Перебрать полные Ed25519 допущенных участников с ролью. Начать с cursor=0; 1 — получен ключ, 0 — конец. */
int group_policy_role_key_next(struct UTUN_INSTANCE* inst, const char* channel, enum group_member_role role,
                               size_t* cursor, uint8_t ed[32]);
/* Uasync, без SQL: подписанный userinfo допущенного участника из подтверждённого снимка.
 * MS_AUTHOR_ACTIVE/INACTIVE/PENDING/ERROR (member_sync.h): placeholder/JOIN-only ещё PENDING,
 * отсутствующий/исключённый/недопущенный — INACTIVE; без снимка — PENDING.
 * userinfo/version обязательны, ed[32] необязателен; выходы меняются только при ACTIVE.
 * userinfo заимствован до следующего publish/forget: разобрать сразу либо скопировать до выхода в event loop. */
int group_policy_member_profile(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node,
    const char** userinfo, uint64_t* version, uint8_t ed[32]);
/* Тот же проверенный снимок профиля по полному Ed25519. */
int group_policy_member_profile_key(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32],
        const char** userinfo, uint64_t* version);
/* Удалить снимок после commit удаления группы; channel=NULL освобождает все снимки при stop/destroy.
 * Вызывать до callbacks, которые могли бы воспользоваться прежними правами. */
void group_policy_forget(struct UTUN_INSTANCE* inst, const char* channel);
/* Worker/SQL: скопировать готовые admitted/identity_verified, роли и собственный userinfo из согласованного снимка.
 * До первого чтения при старте удостоверения должны быть проверены через project.
 * out=NULL при ошибке; SQLITE_OK/код SQLite. Указателей SQLite/runtime в результате нет.
 * Используется для передачи подтверждённого состояния через границу потоков. */
int group_policy_snapshot_read(struct sqlite3* db, const char* channel, struct group_policy_cache** out);
/* Uasync: проверяет наличие опубликованного снимка для обновления отдельных профилей. */
int group_policy_snapshot_present(struct UTUN_INSTANCE* inst, const char* channel);
/* SQL: готовит профили по keys из текущего снимка, когда ключи и допуск не менялись.
 * Возвращает код SQLite. Результат принадлежит вызывающему до publish либо snapshot_free. */
int group_policy_profiles_read(struct sqlite3* db, const char* channel, const merkle_key_t* keys, size_t count,
    struct group_policy_cache** out);
/* Uasync: переносит подготовленные профили в опубликованный снимок и освобождает пакет. */
void group_policy_profiles_publish(struct UTUN_INSTANCE* inst, struct group_policy_cache* profiles);
/* Uasync: заменить снимок без аллокаций и callbacks, принять владение cache (обязателен).
 * Вызывать после успешного SQL/commit до уведомлений; после rollback/cancel вызвать free. */
void group_policy_snapshot_publish(struct UTUN_INSTANCE* inst, struct group_policy_cache* cache);
/* Любой поток, только принадлежащий caller ещё не опубликованный снимок; NULL допустим. */
void group_policy_snapshot_free(struct group_policy_cache* cache);
/* Uasync без файлового SQL: право приглашать из committed снимка owner/policy/admitted/подписанной admin-роли.
 * 1=разрешено,0=отказ/снимок ещё не готов,-1=неверные аргументы. Без снимка SQL допустим только для RAM fixtures.
 * Profile-only refresh заново вычисляет capability; старое право не переносится после смены policy/owner/ключей. */
int group_policy_can_invite(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t inviter[32]);
/* Worker/SQL: заново проверить приглашение по текущему снимку в транзакции caller.
 * 1=разрешено,0=отказ,отрицательный SQLite-код=операционный отказ; runtime не используется. */
int group_policy_can_invite_read(struct sqlite3* db, const char* channel, const uint8_t inviter[32]);
/* Worker/SQL: согласованный предпросмотр из транзакции READ caller, без runtime и приватных ключей.
 * all_members=0/1, keep_json=NULL означает []; 64 HEX Ed25519 сохраняемых участников.
 * Самостоятельный JSON принадлежит caller (u_free). SQLITE_OK/положительная ошибка SQLite;
 * SQLITE_MISMATCH/CONSTRAINT — неправильный выбор сохранения. При ошибке *result=NULL. */
int group_policy_preview_read(struct sqlite3* db, const char* channel, int all_members, const char* keep_json, char** result);
struct group_policy_mutation {
    const char *channel, *token, *keep; /* Frozen строки caller; token64 из preview, keep JSON. */
    int all_members; struct member_sync_write_context* context; /* Полная frozen идентичность/время автора WRITE. */
    int status; uint64_t version; /* 0=успех,-1=семантический отказ,-2=устарел preview; staged версия. */
    char* json; struct group_policy_update* admission; /* Owned outputs: publish после commit, иначе discard/free. */
};
/* Worker: свежий preview/права, подпись политики/сохранений/отзывов и индекс одной WRITE caller.
 * SQLITE_OK/положительная SQL-ошибка; status/json описывают семантический отказ без записей.
 * На SQL/COMMIT отказе json успеха не публиковать; память и admission всегда освобождает caller. */
int group_policy_command_write(struct sqlite3* db, struct group_policy_mutation* mutation);
/* Предпросмотр/применение: token=NULL — preview; иначе проверка Merkle token и commit.
 * keep_json — массив Ed25519 в строках по 64 HEX, NULL означает []. all_members=0/1.
 * Результат JSON принадлежит caller (u_free), включая ошибку/conflict. 0/-1/-2(conflict).
 * Чужая транзакция запрещена. Переподписывает только текущих проверенных участников. */
int group_policy_command(struct UTUN_INSTANCE* inst, const char* channel, int all_members,
    const char* token, const char* keep_json, char** result);

/* Один u_calloc-блок, включая NUL-terminated keep[]; пустой token означает preview.
 * UI получает CHAT_EVT_GROUP_POLICY_RESULT: request:8LE, group Ed25519:32 + JSON. */
struct group_policy_request {
    struct UTUN_INSTANCE* inst; /* Заполняется мостом при постановке в uasync. */
    uint64_t id; /* Корреляция команды интерфейса. */
    char group[65]; /* Публичный Ed25519 группы в HEX. */
    int all_members; /* Предлагаемый режим 0/1. */
    char token[65]; /* Merkle root из preview; пусто для чтения. */
    char keep[]; /* JSON-массив строковых node ID. */
};
/* Uasync: поставить READ-preview либо полную WRITE в общий DB worker, затем доставить событие и освободить request.
 * Публикация прав/события после commit; после stop/restart старый запрос отменяется.
 * inst должен жить до drain DB worker. При отказе очереди ответ может быть синхронным.
 * Если после commit не выделен большой event, короткий ответ подтверждает applied с details_unavailable. */
void group_policy_gui_command(void* request);

#ifdef __cplusplus
}
#endif
#endif
