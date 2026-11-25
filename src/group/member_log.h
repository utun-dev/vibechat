/* Личные линейные журналы мемберов группы. Единственный писатель создаёт следующую
 * запись через make; принимающие узлы дописывают непрерывный хвост через accept.
 * SQLite принадлежит caller, вызовы сериализованы его SQL-потоком, включая DB worker. make/accept
 * требуют транзакцию; отрицательный результат accept требует rollback. Изменения
 * Merkle сохраняются в той же транзакции; рассылка/события — только после commit.
 * Для исходящих записей commit ниже проверяет файловый WAL/FULL и ждёт sync диска.
 * Ключ проверки предоставляет вызывающий из проверенного удостоверения мембера.
 * Подпись подтверждает цепочку; предметные права проверяет callback apply. */
#ifndef UTUN_MEMBER_LOG_H
#define UTUN_MEMBER_LOG_H
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MEMBER_LOG_BODY_MAX 7860 /* Два максимальных блока развилки помещаются в кодограмму 16 КиБ. */
#define MEMBER_LOG_GROUP_OFFSET 16
#define MEMBER_LOG_MEMBER_OFFSET 48
#define MEMBER_LOG_SEQUENCE_OFFSET 80
#define MEMBER_LOG_SCOPE_OFFSET 128
#define MEMBER_LOG_HEADER_SIZE 196
#define MEMBER_LOG_WIRE_MIN (MEMBER_LOG_HEADER_SIZE + 1 + 64)
#define MEMBER_LOG_WIRE_MAX (MEMBER_LOG_HEADER_SIZE + MEMBER_LOG_BODY_MAX + 64)
/* Аудит полученного приглашения: тип, Ed25519 пригласившего, подпись, канонический JSON. */
#define MEMBER_LOG_INVITATION 5
#define MEMBER_LOG_INVITATION_PREFIX 97
#define MEMBER_LOG_VERIFY_LAST 100
#define MEMBER_LOG_ERROR (-1)
#define MEMBER_LOG_DEFERRED (-2)
#define MEMBER_LOG_DIVERGED (-3) /* Подписанный следующий блок ссылается на другую голову. */
/* Положительный терминальный результат: цепочка заблокирована, новая запись НЕ добавлена.
 * Caller делает COMMIT для сохранения доказательства и уведомляет после commit. */
#define MEMBER_LOG_BLOCKED 2

struct member_log_head {
    uint64_t seq;                 /* 0 — пустая цепочка. */
    uint8_t hash[32];             /* Нули при seq=0. */
    int blocked;                 /* Две проверенные подписи автора на разные блоки одного номера. */
};
struct member_log_record {
    char channel[65];            /* Ключ строки группы, устанавливается SQL-вызовами. */
    uint64_t seq;                 /* Номер записи 1..INT64_MAX. */
    uint8_t group_ed[32];         /* Ed25519 группы, входит в подписанный блок. */
    uint8_t member[32];           /* Ed25519 единственного автора, входит в подписанный блок. */
    uint64_t time_us;             /* Время для UI/LWW; порядок цепочки задаёт seq. */
    uint8_t previous[32], hash[32]; /* Хеш предшественника и SHA256 подписанного тела. */
    uint8_t scope[32], grant[32]; /* Объект операции и версия основания прав. */
    uint8_t signature[64];        /* Ed25519 автора над hash. */
    const uint8_t* body;          /* Заимствованные точные байты операции, не NUL-строка. */
    size_t body_size;
};
/* 1 — изменения применены; 0 — права отказаны, журнал продолжается без изменения данных;
 * -2 — ещё нет зависимости; -1 — ошибка. После отрицательного результата caller делает rollback.
 * Запись уже вставлена внутри транзакции: callback может ссылаться на неё из текущих таблиц. */
typedef int (*member_log_apply_fn)(void* context, const struct member_log_record* record);

/* Прочитать голову; отсутствующая цепочка даёт seq=0/hash=0. 0/-1, out сбрасывается при ошибке. */
int member_log_head(sqlite3* db, const char* channel, const uint8_t member[32], struct member_log_head* out);
/* Прочитать time_us блока головы в транзакции caller; 0 для пустой цепочки, результат — код SQLite. */
int member_log_head_time(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t* time_us);
/* Перед запуском сетевых сервисов: проверить до100 последних записей своих цепочек
 * во всех группах, подписи/номера/хеши и связь с предшественником. Остальная история
 * не перечитывается. Требует autocommit, использует собственный read snapshot.
 * 0 — проверено; -1 — порча/ошибка, продолжать собственные цепочки запрещено.
 * Ничего не удаляет и не помечает автора скомпрометированным из-за порчи диска. */
int member_log_verify_own(sqlite3* db, const uint8_t member[32]);
/* SQL/worker: та же проверка в уже открытой READ/WRITE caller; не меняет границы транзакции.
 * SQLITE_OK/положительный SQLite-код; порча/неверная подпись =>SQLITE_CORRUPT.
 * Не публикует runtime и не удаляет записи; ошибку caller обязан откатить. */
int member_log_verify_own_read(sqlite3* db, const uint8_t member[32]);
/* Завершить транзакцию перед публикацией: только файловая БД, WAL и synchronous>=FULL.
 * Синхронно ждёт SQLite xSync; 0 — можно публиковать, -1 — публикация запрещена.
 * При ошибке caller откатывает оставшуюся транзакцию; при I/O ошибке останавливает
 * запись до переоткрытия/проверки БД. :memory: допускается make/accept для тестов,
 * но не даёт подтверждения сохранения и здесь отвергается. */
int member_log_commit(sqlite3* db);
/* Подготовить следующую запись без записи в БД. Требует транзакцию; body заимствуется.
 * time_us автоматически повышается относительно предыдущего блока. 0/-1. */
int member_log_make(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t time_us,
    const uint8_t scope[32], const uint8_t grant[32], const uint8_t* body, size_t body_size,
    const uint8_t private_key[32], struct member_log_record* out);
/* Проверить подпись/хеш/продолжение и атомарно с callback продвинуть голову.
 * 1 — добавлено, 0 — идентичный повтор, 2 — цепочка заблокирована (требует commit),
 * -3 — другая голова, -2 — зависимость, -1 — ошибка. Доказательство хранится вне линейного журнала. */
int member_log_accept(sqlite3* db, const struct member_log_record* record,
    const uint8_t public_key[32], member_log_apply_fn apply, void* context);
/* Сетевой путь: проверяет wire и ожидаемые group/member, затем принимает запись.
 * Подпись новой записи проверяется один раз. Коды/транзакция как у accept. */
int member_log_accept_wire(sqlite3* db, const char* channel, const uint8_t member[32], const uint8_t* wire, size_t size,
    const uint8_t public_key[32], member_log_apply_fn apply, void* context);
/* Wire содержит Ed25519 группы, BE-заголовок, body и signature.
 * encode: size ёмкость->длина, 0/-1. decode проверяет hash/signature, body заимствует wire.
 * Поле channel устанавливает decode_group после проверки Ed25519 по SQLite. */
int member_log_encode(const struct member_log_record* record, uint8_t* wire, size_t* size);
int member_log_decode(const uint8_t* wire, size_t size, const uint8_t public_key[32], struct member_log_record* out);
int member_log_decode_group(sqlite3* db, const char* channel, const uint8_t* wire, size_t size,
    const uint8_t public_key[32], struct member_log_record* out);
/* Страница хвоста с from_seq включительно: [len:2BE,wire]... до ёмкости *size.
 * next_seq — первый ещё не выданный номер; more — есть продолжение. 0/-1.
 * Номер после текущей головы возвращает пустой хвост, запрос с пропуском запрещён принимающим accept. */
int member_log_tail(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t from_seq,
    uint8_t* out, size_t* size, uint64_t* next_seq, int* more);
/* Проверить точную контрольную точку;1 — есть,0 — нет,отрицательный SQLite-код — ошибка.
 * seq=0 допускает только нулевой hash. Сравнение по времени не используется. */
int member_log_checkpoint(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t seq, const uint8_t hash[32]);
/* Прочитать один точный блок для редкой диагностики расхождения. size ёмкость->длина.
 * 1 — блок найден,0 — отсутствует,отрицательный SQLite-код — ошибка/порча. */
int member_log_get(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t seq, uint8_t* wire, size_t* size);

/* Принять два разных подписанных блока одного номера, в том числе до получения префикса.
 * Приоритет у меньшего seq. При равном seq храним два разных блока с наименьшими SHA256.
 * Caller WRITE; 1=доказательство изменилось, 0=уже есть равное или более раннее, -1=ошибка. */
int member_log_prove(sqlite3* db, const char* channel,const uint8_t public_key[32],const uint8_t* first,size_t first_size,const uint8_t* second,size_t second_size);
/* Прочитать доказательство; выходные wire необязательны, size=ёмкость->длина. 1/0/-1; seq=0 если доказательства нет. */
int member_log_fault(sqlite3* db,const char* channel,const uint8_t member[32],uint64_t* seq,
    uint8_t* first,size_t* first_size,uint8_t* second,size_t* second_size);
/* Прочитать seq и два проверенных хеша в порядке возрастания. 1/0/-1; без доказательства выходы нулевые. */
int member_log_fault_hashes(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t* seq, uint8_t hashes[64]);

/* Бинарная сверка двух непрерывных цепочек. Только один незавершённый запрос.
 * Контекст заимствован caller и привязан к выбранному пиру до завершения проверки. */
struct member_log_probe {
    char channel[65];
    uint8_t member[32];           /* Ed25519 автора проверяемой цепочки. */
    uint64_t same, different;     /* Совпавший префикс и правая граница. */
    uint64_t query;               /* Номер блока для следующего запроса этому пиру. */
    int mismatch;                /* Правая граница уже подтверждена разными подписанными блоками. */
};
/* 1 — запросить out->query у пира; 0 — общая длина нулевая, сравнивать нечего; -1 — ошибка. */
int member_log_probe_begin(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t peer_seq, struct member_log_probe* out);
/* Проверить ответ на query. Требует транзакцию: подтверждённое расхождение блокирует цепочку.
 * 1 — запросить следующий query; 0 — префиксы совпали; 2 — первый разный блок = different;
 * -1 — ошибочный ответ, caller делает rollback. Положительный результат требует commit.
 * При ошибке commit caller перезапускает диагностику, не продолжает изменённый out. */
int member_log_probe_reply(sqlite3* db, struct member_log_probe* probe, const uint8_t public_key[32],
    const uint8_t* wire, size_t size);
#ifdef __cplusplus
}
#endif
#endif
