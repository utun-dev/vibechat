/* Подписанное описание группы: JSON {"name":"...","ver":1,...} до 1024 байт.
 * Неизвестные поля могут содержать любые JSON-значения; clipboard_members — boolean, по умолчанию false.
 * group_type — chat/kanban; отсутствие означает неизвестный тип. Принятый тип неизменяем.
 * Исходные байты сохраняются и подписываются целиком.
 * name — UTF-8 до127 байт, ver — целое1..INT64_MAX. Среди действительных записей выбирается большая версия, затем меньшая подпись и меньшие байты JSON.
 * Вызовы с instance — в uasync; чистые SQL-функции — в единственном потоке владельца db.
 * apply работает в транзакции caller, publish — только после commit. */
#ifndef CHAT_GROUP_INFO_H
#define CHAT_GROUP_INFO_H
#include <stdint.h>
#include <stddef.h>
struct sqlite3;
struct UTUN_INSTANCE;
struct group_policy_cache;
#ifdef __cplusplus
extern "C" {
#endif
#define GROUP_INFO_MAX 1024
#define GROUP_INFO_WIRE_MAX (2 + GROUP_INFO_MAX + 64)
#define GROUP_INFO_MESSAGE_MAX (GROUP_INFO_MAX + 128)
struct group_info_record {
    char json[GROUP_INFO_MAX + 1]; /* Полный JSON, включая неизвестные опции; NUL не входит в подпись. */
    uint8_t signature[64]; /* Ed25519 ключом группы, с привязкой к владельцу и обоим публичным ключам. */
};
struct group_info_profile {
    char name[128]; /* Отображаемое UTF-8 имя, без скрытого NUL; спецсимволы экранируются в JSON/логах. */
    uint64_t version; /* Логический счётчик изменений; не зависит от времени устройства. */
    char group_type[8]; /* Пусто — неизвестен; chat/kanban фиксируется при первом принятии. */
    int clipboard_members; /* Разрешена адресная отправка clipboard участникам; отсутствие поля означает0. */
};
/* Построить исходный JSON chat-группы с ver=1, экранируя name;0/-1, out не меняется при отказе. */
int group_info_initial(const char* name, char out[GROUP_INFO_MAX + 1]);
/* Начальное описание выбранного протокола (chat/kanban); 0/-1, out не меняется при отказе. */
int group_info_initial_typed(const char* name, const char* type, char out[GROUP_INFO_MAX + 1]);
/* Проверить неизменяемость типа относительно сохранённого описания; 0/-1.
 * Новая группа и отсутствие типа разрешены; известный тип менять/удалять нельзя.
 * Вызывать в транзакции записи перед сравнением версий; проверку подписи выполняет caller. */
int group_info_check_type(struct sqlite3* db, const char* channel, const char* json);
/* SQL/worker: та же неизменность типа с точной положительной SQLite-причиной.
 * SQLITE_CONSTRAINT=попытка сменить уже принятый тип,0=разрешено; транзакция caller. */
/* Проверить тип импортируемой группы и определить победителя среди двух описаний. */
int group_info_import_order(struct sqlite3* db, const char* channel, const char* json, const uint8_t* signature, int* newer);
int group_info_check_type_ex(struct sqlite3* db, const char* channel, const char* json);
/* Проверить строгий JSON, уникальность ключей, типы известных полей и UTF-8.
 * 0 либо отрицательный код SQLite, out меняется только при успехе; первая SQL/validation-ошибка сохраняется для worker. */
int group_info_parse(struct sqlite3* db, const char* json, struct group_info_profile* out);
/* Прочитать разрешение из подписанного описания в SQL-снимке caller. Отсутствие ключей или неверная подпись дают allowed=0.
 * Возврат SQLITE_OK либо положительный код SQLite. При ошибке чтения или проверки значение allowed сохраняется. */
/* ed[32] необязателен; подтверждённый ключ подписанта описания, нули при отсутствии проверки. */
int group_info_access_read(struct sqlite3* db, const char* channel, int* allowed, uint8_t ed[32]);
/* Прямая проверка доступа для RAM: 1 — подписанное описание разрешает clipboard, 0 — выключено/нет группы/ошибка с логом. */
int group_info_clipboard_allowed(struct sqlite3* db, const char* channel);
/* Подписываются домен "utun.group.info.v3" с NUL, Ed25519 владельца, X25519 и Ed25519 группы и исходный JSON без NUL.
 * Без обращения к БД;длина/-1, out не меняется при отказе. JSON проверяет caller. */
/* Каноническое подписанное описание, независимо от локальной регистрации группы. */
int group_info_signature_body(const char* json, const uint8_t owner[32], const uint8_t x[32], const uint8_t ed[32],
    uint8_t* out, size_t capacity);
int group_info_message(const char* channel, const char* json, const uint8_t owner[32], const uint8_t x[32], const uint8_t ed[32],
    uint8_t* out, size_t capacity);
/* SQL/worker: прочитать JSON/подпись и проверить формат;0/отрицательная SQLite-причина.
 * out сохраняется при отказе prepare/bind/step/finalize/формата/JSON. */
int group_info_get(struct sqlite3* db, const char* channel, struct group_info_record* out);
/* Порядок проверенных описаний по большей версии, меньшей подписи, затем меньшим байтам JSON. */
int group_info_order(uint64_t a_version, const struct group_info_record* a, uint64_t b_version, const struct group_info_record* b);
enum group_info_decision { GROUP_INFO_REJECTED, GROUP_INFO_UNCHANGED, GROUP_INFO_NEWER };
/* Проверить входную запись относительно текущих ключей и типа. При SQLITE_OK decision содержит результат,
 * Действительное описание восстанавливает повреждённую запись независимо от её версии и типа.
 * version (если запрошена) заполняется для принятой записи. При локальном отказе возвращается код SQLite. */
int group_info_check(struct sqlite3* db, const char* channel, const struct group_info_record* record,
    enum group_info_decision* decision, uint64_t* version);
/* В WRITE caller повторить проверку и сохранить более новое описание вместе с MEMBERS-хешем. Тот же контракт результата. */
int group_info_apply_checked(struct sqlite3* db, const char* channel, const struct group_info_record* record, enum group_info_decision* decision);
/* Проверить подпись по сохранённой идентичности группы;0/-1. */
int group_info_verify(struct sqlite3* db, const char* channel, const struct group_info_record* record);
/* SQL/worker в транзакции caller: проверить и применить более новое описание.
 * 1=записано,0=старое/no-op,отрицательный код SQLite=ошибка.
 * Пересчитывает MEMBERS Merkle атомарно с JSON, runtime/регистрация модели не нужны.
 * Caller откатывает ошибку; событий до commit нет. */
int group_info_apply(struct sqlite3* db, const char* channel, const struct group_info_record* record);
/* После commit уведомить Merkle и интерфейсы; callback может удалить группу. */
void group_info_publish(struct UTUN_INSTANCE* inst, const char* channel);
/* notify_sync=0 передаёт уведомление MEMBERS вызывающему batch publisher. */
void group_info_publish_changes(struct UTUN_INSTANCE* inst, const char* channel, int notify_sync);
/* Wire: длина JSON:u16BE, JSON, подпись64; encode — длина/-1, decode0/-1 с точной проверкой границ. */
int group_info_encode(const struct group_info_record* record, uint8_t* out, size_t capacity);
int group_info_decode(struct group_info_record* out, const uint8_t* data, size_t size);
/* Команда ядра: name!=NULL — rename с сохранением опций; json!=NULL — полная замена JSON с большей ver.
 * clipboard_members:-1 не менять,0/1 изменить с новой ver; совместим с name, исключает json.
 * name/json NULL и clipboard_members=-1 — снимок. expected — SHA256 прежнего JSON (64 lowercase hex) либо NULL.
 * Результат {ok,can_edit,description/error} принадлежит caller/u_free;0/-1/-2(conflict). */
int group_info_command(struct UTUN_INSTANCE* inst, const char* channel, const char* name, const char* json,
    int clipboard_members, const char* expected, char** result);
struct group_info_write {
    const char *channel, *name, *json, *expected; /* Неизменяемые строки до завершения worker; NULL — не менять поле. */
    uint8_t self_ed[32]; /* Ed25519 автора запроса. */
    int clipboard_members; /* -1 не менять, 0/1 новое значение. */
    int status, sqlite_code, changed; /* Semantic0/-1/-2 conflict; SQL reason; staged описание изменилось. */
    char* result; /* Собственный JSON, caller/u_free; при SQL commit failure успешный ответ отбросить. */
    struct group_policy_cache* access; /* Snapshot готов до commit: publish после успеха, free после rollback. */
};
/* SQL/worker, WRITE caller: fresh ACL/description, подпись и MEMBERS index одной транзакцией.
 * SQLITE_OK/положительный SQLite-код; status/result также описывают semantic refusal.
 * Ни runtime, ни BEGIN/COMMIT; при любом SQL отказе caller откатывает всю WRITE. */
int group_info_command_write(struct sqlite3* db, struct group_info_write* query);
struct group_info_request {
    struct UTUN_INSTANCE* inst; /* Устанавливается мостом при enqueue; живёт до drain DB worker. */
    uint64_t id; /* Корреляция команды интерфейса. */
    char group[65]; /* Публичный Ed25519 группы в HEX. */
    char name[128], expected[65]; /* Пустое name — не менять имя; expected пустой — без проверки конфликта. */
    int clipboard_members; /* 0 не менять,1 запретить,2 разрешить; совместимо с нулевой инициализацией запроса. */
};
/* Uasync: забирает u_malloc-request; файловый снимок читает общий READ worker, done возвращает
 * CHAT_EVT_GROUP_INFO_RESULT(request:8LE, group Ed25519:32 + JSON), включая ошибку, и освобождает request.
 * RW изменение выполняет одной worker WRITE и публикует snapshot/события после commit.
 * Stop/start общего сервиса отвергает старые команды, даже с прежним SQLite handle. */
void group_info_gui_command(void* request);
#ifdef __cplusplus
}
#endif
#endif
