/*
 * dm_core.h — прямой p2p чат между двумя пользователями (DM)
 *
 * Личная беседа не создаёт отдельную TOPO_GROUP и не синхронизируется через member_sync/Merkle.
 * Для доставки использует маршрут из общей CHAT-группы участников.
 * Router проверяет актуальный допуск self/peer именно в группе пакета и подпись DATA/controls.
 * Отзыв маршрута закрывает его очереди, сохраняя личную историю и durable outbox;
 * следующая попытка может выбрать другую допущенную общую группу с тем же телом/адресатом.
 *   - conv_id и content_key детерминированно выводятся обеими сторонами
 *     из своих ключей и pubkey пира (см. dm_crypto.h) — без переговоров.
 *   - сообщения идут через etcp_router (прямое соединение или релей),
 *     при недоступности пира — через dm_mailbox (суперузлы).
 *   - содержимое шифруется E2E (AES-256-CCM), запись подписывается Ed25519.
 *
 * Два независимых направленных потока (я→пир и пир→я) с монотонным исходящим seq.
 * Runtime API и трамплины выполняются в uasync с живым instance/router; SQL-only helpers находятся в dm_store.h.
 * Входные строки/тела заимствуются на время вызова; функции не забирают их владение.
 * Запросы трамплинов — один блок u_malloc/u_calloc с NUL-строками; обработчик освобождает блок.
 * Instance должен пережить запрос и его отложенное завершение; остановка сериализуется через uasync.
 */

#ifndef DM_CORE_H
#define DM_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "dm_media.h"
#include "dm_protocol.h"
#include "../chat/chat_member.h"

struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

/* ── Каноническое тело DM-сообщения (общий wire-формат dm_core и dm_mailbox) ──
 * [conv_id:8][seq:8][ts:8][author_ed:32][ct_len:1][ct:ct_len][data_len:2][data_enc:data_len][media?:56][sig:64]
 * media присутствует только для ct="file", подписан вместе с сообщением.
 * data_enc — AES-256-CCM (шифротекст + tag), sig — Ed25519 автора по всем полям до sig.
 * Числа native-endian; seq=1..INT64_MAX, ts — миллисекунды NTP-времени instance.
 * ETCP_RT_ID_DM=0x34: [MSG=1][тело] либо [ACK=2][квитанция:184]; router-пакет подписан. */
#define DM_MSG_SIG_SIZE   64
#define DM_DATA_MAX       1024
#define DM_PEER_NAME_MAX  63   /* UTF-8 байты без NUL; сохраняются только целые code points. */

/* Вычислить полную длину канонического тела из его заголовка.
 * Возвращает 1 и *out_len, либо 0 если буфер мал/повреждён (выход не меняется).
 * Допускает хвост после тела; для отдельного сообщения вызывающий требует *out_len==avail.
 * Многобайтные числа имеют порядок байтов хоста; подпись/seq/идентичность здесь не проверяются. */
static inline int dm_msg_len(const uint8_t* msg, size_t avail, size_t* out_len) {
    struct dm_message_view view;
    if (!out_len || dm_message_parse(msg, avail, &view)) return 0;
    *out_len = view.size;
    return 1;
}

/* Выбрать живую CHAT-группу с допуском self/peer и путём router, сначала preferred (0=без предпочтения).
 * 1 и *group_id при успехе; 0 при отсутствии пути/ошибке, выход не меняется.
 * Только uasync: перебирает подготовленные группы/подтверждённые права в памяти, не читает файловую БД.
 * group_id обязателен; наличие пути не гарантирует успешной последующей отправки. */
int dm_route_group(struct UTUN_INSTANCE* inst, uint64_t peer, uint64_t preferred, uint64_t* group_id);
/* 1 при доступном маршруте до полного Ed25519 в общей CHAT-группе; group_id меняется только при успехе. */
int dm_route_group_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], uint64_t preferred, uint64_t* group_id, uint8_t service);
/* Проверить role=yes у peer в любой живой общей CHAT-группе self/peer.
 * Группа объявления роли не ограничивает маршрут личной передачи. peer=self проверяет локальную роль.
 * Учитывает source=0, admitted=1, identity_verified=1, deleted=0 и действующий канал; путь не требуется.
 * 1=роль найдена, 0=нет роли/ошибка с диагностикой. role — supernode/storage, только uasync.
 * Использует подтверждённые права в памяти без файлового SQL; legacy DB handle не требуется. */
int dm_peer_role(struct UTUN_INSTANCE* inst, uint64_t peer, const char* role);
/* 1, если ключ устройства имеет указанную роль хотя бы в одной общей допущенной группе. */
int dm_peer_role_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], const char* role);
/* Проверить точную длину msg/len, conv/author/recipient/seq, descriptor и подпись указанного Ed25519 автора.
 * 0=корректно, -1=отказ. Нужна SQLite, владелец dm_core не требуется.
 * Не расшифровывает тело и не проверяет членство/роль router-отправителя: это обязанность вызывающего. */
int dm_verify_message(struct UTUN_INSTANCE* inst, uint64_t recipient, const uint8_t* msg, size_t len);
/* Проверить и расшифровать входящее тело для локального узла, атомарно сохранить историю/беседу/media.
 * Вызывающий проверяет сетевого отправителя/посредника. Общая группа автора и адресата не требуется;
 * личность автора, адресат и закреплённые стороны определяются подписанным телом и проверенными ключами.
 * 0 и receipt после commit либо точного повтора; -1 при отказе. Для file нужен живой dm_media.
 * Точный удалённый повтор подтверждается по отпечатку без восстановления истории; конфликт отклоняется.
 * receipt — обязательные 184 байт; при -1 буфер не меняется. Синхронный RAM/legacy вход; RW требует dm_receive_submit. */
int dm_accept_message(struct UTUN_INSTANCE* inst, const uint8_t* msg, size_t len, uint8_t receipt[DM_RECEIPT_SIZE]);
struct dm_receive_query {
    const uint8_t* body;       /* Полное каноническое тело, копируется до возврата. */
    size_t size;             /* 1..DM_BODY_MAX байт без cmd/router-заголовка. */
    int (*valid)(void*);     /* Необязательный uasync guard arg перед SQL и публикацией; сетевой вход проверяет допуск здесь. */
    const struct dm_file_offer* file_offer; /* Необязательный OFFER: копируется, источник сохраняется в той же WRITE. */
};
struct dm_receive_result {
    int status, sqlite_code; /* 0/SQLITE_OK после commit; иначе -1 и причина отказа. */
    uint64_t conversation, author, seq, generation; /* Подписанные ID; generation=0 у удалённого повтора, всё0 при отказе. */
    int inserted, deleted;  /* Новое сообщение либо точный удалённый повтор,0 при отказе. */
    const uint8_t* receipt; /* 184 байт ACK после commit, NULL при отказе; заимствованы до возврата callback. */
    const uint8_t* media_receipt; /* 200 байт сохранённого FILE ACK либо нули; NULL при отказе. */
};
/* Ровно один вызов в uasync, включая синхронный отказ аргументов/OOM/FULL/CLOSED.
 * Result/квитанции заимствованы до возврата; arg принадлежит caller до done. */
typedef void (*dm_receive_done_fn)(void* arg, const struct dm_receive_result* result);
/* Uasync: скопировать тело/ключи и одной WRITE проверить, расшифровать и принять сообщение.
 * Runtime/сеть не используются worker; событие и receipt доступны только после commit.
 * Caller проверяет живого сетевого отправителя через valid, особенно для mailbox-посредника.
 * Stop отменяет ожидающий запрос и позднюю публикацию; начатая WRITE может завершиться без ACK.
 * inst/arg живут до done/DB drain. Legacy/RAM исполняют тот же SQL; file требует media-пути,
 * кроме точного удалённого повтора, которому достаточно сохранённого отпечатка. */
void dm_receive_submit(struct UTUN_INSTANCE* inst, const struct dm_receive_query* query, dm_receive_done_fn done, void* arg);
/* receipt — ровно 184 доступных байт: ["DMACK002":8][conv:8][seq:8][author_ed:32][recipient_ed:32][SHA256(msg):32][sig:64].
 * Проверяет идентичность и подпись B по первым 72 байтам; при msg!=NULL также точное тело msg/len.
 * 0=корректно, -1=отказ. Не проверяет наличие outbox и ничего не записывает. */
int dm_verify_receipt(struct UTUN_INSTANCE* inst, const uint8_t receipt[DM_RECEIPT_SIZE], const uint8_t* msg, size_t len);
/* Для локального автора проверить receipt и удалить только точный outbox; история/file receipt сохраняются.
 * 0=удалено либо outbox уже отсутствует, -1=ошибка/конфликт. Требуется запущенный dm_core.
 * Синхронный RAM/диагностический вход и переходный legacy writer; при RW worker использовать submit. */
int dm_accept_receipt(struct UTUN_INSTANCE* inst, const uint8_t receipt[DM_RECEIPT_SIZE]);
/* Необязательный результат в uasync:0 и заимствованная проверенная receipt до возврата либо -1,NULL.
 * При заданном done он вызывается ровно один раз, включая FULL/OOM/CLOSED/stop. arg живёт до done.
 * Callback может прийти до возврата submit; отказ не повторять напрямую из done. */
typedef void (*dm_receipt_done_fn)(void* arg, int status, const uint8_t* receipt);
/* Uasync: копирует184 байт, в RW worker проверяет/применяет receipt одной WRITE-транзакцией.
 * Событие обновления истории и done следуют за commit; поздний результат остановленного DM отклоняется.
 * READONLY backend пока сохраняет прежнего писателя; workerless разрешён только для RAM. */
void dm_receipt_submit(struct UTUN_INSTANCE* inst, const uint8_t receipt[DM_RECEIPT_SIZE], dm_receipt_done_fn done, void* arg);

/* Жизненный цикл в uasync: init после chat_core_init с SQLite/router/ua ядра; 0=успех, -1=ошибка.
 * На RW worker общая схема уже готова; init и таймер outbox не используют legacy SQLite.
 * Outbox одной WRITE выбирает до32 адресатов и фиксирует сроки; только затем uasync отправляет копии тел.
 * Одновременно допускается одна пачка; остановка отменяет ожидающую и запрещает позднюю отправку.
 * Повторный init запущенного владельца возвращает 0. Для file-send/accept нужен также dm_media_init.
 * quiesce вызывается до media drain: снимает сеть/таймер/invite и отсоединяет DB owners, не ждёт worker.
 * Поздние API callbacks получают ошибку; UI replies старого запуска подавляются. После quiesce init запрещён до destroy.
 * destroy освобождает quiesced owner; БД/outbox сохраняются. Вызывать до chat_core_destroy. */
int  dm_core_init(struct UTUN_INSTANCE* inst);
/* Worker в общей startup WRITE: схема, sequence автора self и reset локальных holds.
 * Не открывает транзакцию/не читает runtime; SQLITE_OK/положительный SQLite code. Затем uasync dm_core_init. */
int dm_core_prepare_write(sqlite3* db, const uint8_t self[32]);
void dm_core_quiesce(struct UTUN_INSTANCE* inst);
void dm_core_destroy(struct UTUN_INSTANCE* inst);

/* Начать беседу с пользователем (peer). source_ch_id — канал, где нашли target
 * (общая группа; нужен для маршрутизации и подключения). Создаёт беседу если её
 * ещё нет, выводит conv_id/ключи. Подключение для передачи медиа принадлежит media.
 * Снимает hidden без смены поколения; публикует CONV_UPDATED с текущим поколением.
 * Ключи обязательны, peer_name/source_ch_id могут быть NULL; переданные ключи пира проверяет вызывающий.
 * Имя ограничено DM_PEER_NAME_MAX байтами по границе UTF-8; некорректный суффикс отбрасывается с WARN.
 * Синхронный legacy/RAM вход: собственная транзакция, чужая транзакция отклоняется.
 * На RW worker использовать dm_start_submit. Возвращает0 после commit либо <0 при ошибке.
 * Отказ чтения/проверки ключей не создаёт беседу, не меняет её поля/hidden и не выдаёт CONV_UPDATED.
 * Создание разрешено только после успешного SELECT без строки; существующие seq сохраняются. */
int dm_start(struct UTUN_INSTANCE* inst, uint64_t peer_node_id,
             const uint8_t peer_x25519[32], const uint8_t peer_ed25519[32],
             const char* peer_name, const char* source_ch_id, enum dm_mode mode, uint64_t* result_conv);
/* Синхронный legacy/RAM вариант: читает ключи node из SQLite в той же транзакции создания.
 * Фактический ID выдаётся после commit;0/-1. Сервисы/UI/headless используют dm_start_submit. */
int dm_start_node(struct UTUN_INSTANCE* inst, uint64_t peer, const char* name, const char* source,
    enum dm_mode mode, uint64_t* result_conv);

struct dm_start_query {
    uint64_t peer;              /* Производный номер для синхронных вызовов с ключами из SQLite. */
    enum dm_mode mode;          /* DEVICE/GROUPS для обеих сторон. */
    const char* name;           /* Имя новой беседы;NULL=взять из nodes. Копируется/нормализуется при вызове. */
    const char* source;         /* Десятичный ID предложенной общей группы либо NULL;копируется при вызове. */
    const uint8_t* peer_x;      /* Допустим с peer_ed; NULL при выводе X25519 из peer_ed. */
    const uint8_t* peer_ed;     /* Ed25519 адресата (32 байта). Если оба ключа NULL, используется peer. */
    int (*valid)(void*);        /* Необязательный uasync guard для arg:0 отменяет до SQL/публикации. */
};
struct dm_start_result {
    int status;                /* 0=committed беседа; -1=ошибка/отмена. */
    int sqlite_code;           /* Точный код SQLite при ошибке;0 при успехе. */
    uint64_t conversation;     /* Фактический ID DEVICE/GROUPS;0 при отказе. */
    uint64_t generation;       /* Сохранённое поколение после снятия hidden;0 при отказе. */
};
/* Ровно один результат в uasync, включая синхронный отказ FULL/OOM/аргументов.
 * result заимствован до возврата; arg принадлежит caller до done. */
typedef void (*dm_start_done_fn)(void* arg, const struct dm_start_result* result);
/* Uasync: скопировать query/ключи и создать/открыть беседу одной WRITE-транзакцией DB worker.
 * CONV_UPDATED и done следуют за commit; повтор сохраняет ключи/имя/счётчики и поколение.
 * При stop ожидающая работа отменяется; уже начатая может committed, но поздний успех не публикуется.
 * inst/arg живут до done и DB drain; копия локального секрета очищается на любом исходе.
 * На переходном READONLY backend используется legacy writer; синхронные start/start_node — только для него/RAM. */
void dm_start_submit(struct UTUN_INSTANCE* inst, const struct dm_start_query* query, dm_start_done_fn done, void* arg);

/* Снимок действующей беседы для файлового модуля. parties: local/peer, key: content key.
 * NULL-выходы допустимы; 0=успех, -1=нет беседы/вышли из подгруппы/ошибка. Только uasync.
 * Секрет key вызывающий очищает после использования. Архив этим API не открывается. */
int dm_conversation_access(struct UTUN_INSTANCE* inst, const char* conv, struct dm_party parties[2], uint8_t key[32]);
/* Внутри транзакции выхода из подгруппы закрыть её GROUPS-беседы и исходящие очереди.
 * group_ed — прежний публичный ID подгруппы. 0/-1; без запущенного DM пустая операция.
 * Ключи истории/готовые файлы сохраняются. После commit вызвать dm_media_close_committed. */
int dm_group_leave_prepare(struct UTUN_INSTANCE* inst, const uint8_t group_ed[32]);
/* SQL-owner/WRITE: закрыть GROUPS-беседы прежней стороны group_ed[32] и удалить их исходящие очереди.
 * db и таблицы DM обязательны; runtime не нужен, BEGIN/COMMIT не выполняются. SQLITE_OK/положительный код.
 * Готовые файлы/история сохраняются; в той же транзакции вызвать dm_media_close_write, после commit — close_committed. */
int dm_group_leave_write(struct sqlite3* db, const uint8_t group_ed[32]);

/* Отправить сообщение в беседу. Шифрует, подписывает, сохраняет локально и
 * доставляет (прямое соединение при доступности пира, иначе mailbox).
 * conv_id — строковое представление (как в dm_conversations.conv_id).
 * Для ct="file" data=[descriptor:56][attachment_info в формате attachment_encode()], после подготовки dm_media.
 * Descriptor подписывается открыто, метаданные вложения шифруются; формат — media_async/attachment.h.
 * Для ct="invite" data=[group_name:utf8][NUL][полная ссылка:utf8], без завершающего NUL.
 * Имя и ссылка шифруются и подписываются вместе; GUI получает data=ссылка, tags.group_name=имя.
 * 0 после атомарного commit истории/seq/outbox, -1 при ошибке; доставка подтверждается отдельно.
 * content_type до255 байт,data_len до1024 (для file вместе с descriptor); скрытая беседа отклоняется.
 * Синхронный RAM/legacy вход; RW worker требует dm_send_submit. */
int dm_send(struct UTUN_INSTANCE* inst, const char* conv_id, const char* content_type,
            const uint8_t* data, uint32_t data_len);

struct dm_send_query {
    const char* conv;          /* Канонический десятичный ID беседы, копируется до возврата. */
    const char* type;          /* NUL-строка content type до255 байт, включая text/invite/file. */
    const uint8_t* data;       /* Plaintext/descriptor; копируется, NULL допустим при size=0. */
    uint32_t size;             /* До DM_DATA_MAX байт. */
    uint64_t generation;       /* 0=прочитать текущее видимое поколение; иначе требуется точное совпадение. */
    int (*valid)(void*);       /* Необязательная uasync-проверка актуальности arg перед SQL/публикацией. */
};
struct dm_send_result {
    int status, sqlite_code;   /* 0 и SQLITE_OK после commit; -1 и точный код при отказе/отмене. */
    int committed;            /* WRITE успешно committed, даже если stop уже запретил уведомление/доставку. */
    uint64_t seq, generation; /* Фактический номер автора и поколение;0 при отказе. */
};
/* Результат заимствован до возврата, arg принадлежит caller до done. Callback обязателен,
 * приходит ровно один раз, включая синхронный отказ аргументов/OOM/FULL/CLOSED. */
typedef void (*dm_send_done_fn)(void* arg, const struct dm_send_result* result);
/* Uasync: скопировать данные/ключи и одной WRITE сохранить seq, историю, адресатов, file и outbox.
 * Шифрование/подпись и проверка поколения происходят worker в том же снимке. Сеть и событие — после commit.
 * File требует подготовленного media-пути; владелец сохраняет файл до done и после committed=1.
 * IOERR при commit неоднозначен: файл сохраняют для проверки после открытия БД. Query/строки после возврата не нужны.
 * Stop отменяет ожидающую работу и запрещает позднюю публикацию; начатая WRITE может завершиться.
 * inst/arg живут до done/DB drain. READONLY backend использует тот же SQL через единственный legacy writer. */
void dm_send_submit(struct UTUN_INSTANCE* inst, const struct dm_send_query* query, dm_send_done_fn done, void* arg);

/* Локальное удаление по dm_history_delete; generation защищает повторно открытую беседу.
 * CHAT_EVT_DM_DELETE_RESULT: [request_id:8][conv:u64][generation:8][success:u8], всего 25 байт native-endian. */
struct dm_delete_req {
    struct UTUN_INSTANCE* inst; /* Заимствованное ядро до завершения запроса. */
    char conv_id[64];           /* Десятичный ID беседы с NUL. */
    uint64_t generation,      /* Ненулевое ожидаемое поколение. */
             request_id;      /* Корреляция результата с запросом UI. */
};
struct dm_delete_query {
    const char* conv;          /* ID беседы; копируется до возврата. */
    uint64_t generation;      /* Ожидаемое ненулевое видимое поколение. */
    int (*valid)(void*);       /* Необязательная проверка актуальности arg в uasync, без SQL. */
};
struct dm_delete_result {
    int status, sqlite_code;  /* 0 после commit/актуальной публикации, иначе-1/положительный SQLite-код. */
    int committed;            /* SQL удаление committed даже при остановке владельца до уведомления. */
    uint64_t generation;      /* Новое поколение при успехе;0 при отказе/остановке. */
};
typedef void (*dm_delete_done_fn)(void* arg, const struct dm_delete_result* result);
/* Uasync: удалить историю одной WRITE, скопировав ID/generation/root до возврата. Callback обязателен и ровно один,
 * в том числе при FULL/OOM/stop; результат заимствован. После commit отменяет старые передачи/снимки и запускает cleanup.
 * inst/arg живут до done/DB drain. На RAM/переходном RO используется тот же SQL через единственный legacy writer. */
void dm_delete_submit(struct UTUN_INSTANCE* inst, const struct dm_delete_query* query, dm_delete_done_fn done, void* arg);
void dm_delete_trampoline(void* arg);
/* После commit удаления отменить ожидающий invite, READ этой истории и старых списков бесед.
 * Данные/квитанции других бесед не меняет; следующее чтение видит committed удаление. */
void dm_cancel_pending_history(struct UTUN_INSTANCE* inst, const char* conv);

/* До 200 видимых бесед по last_ts DESC: [{conv_id,peer_ed25519,peer_name,last_ts,out_seq,in_seq,generation}].
 * Синхронный helper в uasync для RAM/диагностики; файловые сервисы используют dm_json_read ниже.
 * buf вмещает JSON и NUL; 0 и *out_len без NUL при успехе, -1 при ошибке; при отказе выход пуст.
 * Имя из БД либо nodes нормализуется по правилам dm_start; снимок не меняет сохранённые строки. */
int dm_list_conversations_json(struct UTUN_INSTANCE* inst, char* buf, size_t buf_size, size_t* out_len);
struct dm_json_query {
    const char* conv;             /* NULL — список видимых бесед, иначе канонический ID истории; копируется. */
    uint64_t generation;          /* История:0 — текущая, иначе требуется совпадение. */
    int count, offset;            /* История:count вне1..200 означает200, offset>=0. */
    int include_hidden;          /* Разрешить архив скрытой беседы; по умолчанию запрещён. */
    size_t capacity;              /* JSON вместе с NUL:3..1 MiB. */
    int (*valid)(void*);          /* Необязательный uasync guard:0 отменяет запрос до SQL. */
};
struct dm_json_result {
    int status;                  /* 0 — успех, -1 — отказ/ошибка/отмена. */
    const char* json;            /* Заимствованный JSON до возврата callback, NULL при ошибке. */
    size_t length;               /* Байты без NUL, 0 при ошибке. */
    uint64_t generation;         /* Текущее поколение истории, 0 для списка/ошибки. */
};
/* Ровно один результат в uasync, включая синхронный отказ до очереди. Скопировать данные до возврата;
 * arg принадлежит caller до callback; result также заимствован до возврата. */
typedef void (*dm_json_done_fn)(void* arg, const struct dm_json_result* result);
/* Uasync: список бесед либо история в одном READ worker; query копируется до возврата, done обязателен.
 * valid(arg) выполняется в uasync без SQL/ожиданий.
 * Stop DM отменяет ожидающие запросы; поздний результат старого запуска получает ошибку.
 * inst/arg живут до done и DB drain; workerless RAM разрешён, файлового legacy fallback нет. */
void dm_json_read(struct UTUN_INSTANCE* inst, const struct dm_json_query* query, dm_json_done_fn done, void* arg);

#define DM_MEMBER_WIRE_SIZE (1 + CHAT_MEMBER_DISPLAY_SIZE)
/* Синхронный snapshot для GUI/RAM. Текущие известные устройства закреплённых сторон GROUPS, включая offline.
 * [conv_len:1][conv:UTF8][generation:LE64][count:LE16][side:1,member:CHAT_MEMBER_DISPLAY_SIZE]*count.
 * side=0 — свои,1 — собеседника. DEVICE/закрытая беседа возвращают пустой список.
 * Требуется текущее ненулевое generation; при ошибке out=NULL,size=0; успех0, ошибка-1.
 * Caller освобождает *out через u_free. Чтение не меняет снимки доставки сообщений. */
int dm_members_snapshot(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation, uint8_t** out, size_t* size);
struct dm_members_query {
    const char* conv;             /* Канонический десятичный ID беседы, копируется до возврата submit. */
    uint64_t generation;          /* Ненулевое видимое поколение; проверяется до чтения и после выбора маршрутов. */
};
struct dm_members_result {
    int status, sqlite_code;     /* status=0 успех,-1 отказ/ошибка/stop; sqlite_code уточняет причину. */
    const uint8_t* data;          /* Тот же бинарный формат snapshot; заимствован только до возврата callback. */
    size_t size;                 /* Размер data; при ошибке data=NULL,size=0. */
};
typedef void (*dm_members_done_fn)(void* arg, const struct dm_members_result* result);
/* Uasync: READ участников → выбор живых маршрутов → READ подписей/имён → live-поля и один callback.
 * query копируется; done обязателен, arg/inst живут до результата. FULL/OOM/close тоже завершают callback.
 * Stop/restart DM и удаление истории отменяют старый результат. Файлового legacy fallback нет; RAM разрешён. */
void dm_members_submit(struct UTUN_INSTANCE* inst, const struct dm_members_query* query, dm_members_done_fn done, void* arg);
/* Забирает u_calloc(dm_messages_req), отправляет CHAT_EVT_DM_MEMBERS только при успехе. */
void dm_members_trampoline(void* arg);

/* Сообщения беседы в JSON: расшифрованное содержимое, статусы доставки и медиа.
 * count=1..200 задаёт предел; count<=0 или count>200 означает 200.
 * offset>=0 отсчитывается от новых сообщений; offset=0 выбирает последние count.
 * Выбранная страница возвращается по возрастанию ts, seq, author.
 * buf — выходной буфер с NUL; 0 и *out_len байт без NUL при успехе, -1 при ошибке.
 * pending относится к квитанции сообщения. media_status: delivered/ready/downloading/expired/pending.
 * hidden здесь не фильтруется. Синхронный helper для RAM/диагностики; файловые сервисы используют dm_json_read. */
int dm_list_messages_json(struct UTUN_INSTANCE* inst, const char* conv_id, int count, int offset,
                          char* buf, size_t buf_size, size_t* out_len);

/* ── Трамплины для gui_bridge (GUI → uasync) ── */

/* Асинхронно начать беседу через dm_start_submit: ключи/имя/строки копируются до освобождения запроса.
 * После commit выдаётся CONV_UPDATED; request_id также получает START_RESULT с фактическим ID/поколением. */
struct dm_start_req {
    struct UTUN_INSTANCE* inst; /* Заимствованное ядро. */
    uint8_t  peer_ed[32];       /* Другой участник общей группы, полный Ed25519. */
    char     source_ch_id[65];  /* Десятичный ID общей группы либо пустая строка. */
    char     peer_name[128];    /* UTF-8 имя с NUL; dm_start ограничит до 63 байт. */
    enum dm_mode mode;          /* Один переключатель DEVICE/GROUPS для обеих сторон. */
    uint64_t request_id;        /* Ненулевой запрос получает CHAT_EVT_DM_START_RESULT после commit/ошибки. */
};
void dm_start_trampoline(void* arg);

/* Передать блок в dm_send_trampoline через uasync; он освобождается после результата записи.
 * RW worker сохраняет сообщение асинхронно. CHAT_EVT_MSG_SUBMITTED подтверждает commit либо ошибку. */
struct dm_send_req {
    struct UTUN_INSTANCE* inst; /* Заимствованное ядро. */
    char     conv_id[64];       /* Десятичный ID беседы с NUL. */
    uint64_t generation; /* Ожидаемая видимая версия; 0 — взять текущую внутри транзакции отправки. */
    uint64_t request_id; /* Ненулевой ID composer: результат локальной записи через CHAT_EVT_MSG_SUBMITTED. */
    uint32_t data_len;          /* Число байт текста, до DM_DATA_MAX. */
    uint8_t* data;              /* Указывает на data_len байт в том же блоке после структуры. */
};
void dm_send_trampoline(void* arg);

/* Приглашение в выбранный канал через PM: свежая ссылка после ACK регистрации,
 * Затем worker читает название группы и dm_send_submit фиксирует invite-сообщение.
 * Подготовка/проверка pinned PM и снятие hidden — одна WRITE; ACK регистрации предшествует отправке.
 * Получатель фиксирован независимо от текущего UI, SQL/ключи/пакеты не заимствуются между callbacks.
 * Один ожидающий запрос на instance; результат CHAT_EVT_DM_INVITE_RESULT.
 * Результат: [request_id:8][result:4][peer Ed25519:32][conv:8][generation:8], всего 60 байт native-endian.
 * cancel=1 отменяет ожидающий request_id на любой фазе; начатая WRITE завершается фактическим commit/rollback.
 * Уже committed PM не отзывается. Stop/restart отсоединяет старые callback-контексты без публикации в новый DM.
 * arg=u_calloc(dm_invite_req); обработчик/финальный callback освобождает arg. */
enum dm_invite_result {
    DM_INVITE_OK = 0, DM_INVITE_START_ERROR = -1, DM_INVITE_BUILD_ERROR = -2,
    DM_INVITE_SEND_ERROR = -3, DM_INVITE_CANCELLED = -4, DM_INVITE_BUSY = -5
};
struct dm_invite_req {
    struct UTUN_INSTANCE* inst; /* Заимствованное ядро до итогового callback. */
    uint64_t request_id,       /* Ненулевой ID; cancel использует тот же ID. */
             conv_id,          /* Заполняет uasync: выбранная PM либо новая DEVICE. */
             generation;       /* Заполняет uasync до регистрации; проверяет callback. */
    char channel_id[65];     /* Выбранная CHAT-группа отправителя, Ed25519 в HEX. */
    uint8_t peer_ed[32];      /* Фиксированный получатель, полный Ed25519. */
    char source_ch_id[65];    /* Общая CHAT-группа либо dm:<conv> из открытой PM; не менять режим этой беседы. */
    char peer_name[128];       /* UTF-8 имя для новой беседы с NUL. */
    int cancel;               /* 0=создать, 1=отменить ожидающий request_id. */
    struct chat_invite_build* pending; /* Внутренний handle; запрос создаётся обнулённым. */
};
void dm_invite_trampoline(void* arg);

/* Запросить последние 200 сообщений со статусами. CHAT_EVT_DM_MESSAGES:
 * [conv_id_len:1][conv_id:var][generation:8][JSON:utf8]. Все чтения в uasync.
 * Ненулевое generation проверяется до чтения; 0 захватывает текущее поколение. GUI отбрасывает старый снимок. */
struct dm_messages_req {
    struct UTUN_INSTANCE* inst; /* Заимствованное ядро. */
    char conv_id[64];           /* Десятичный ID с NUL, также для запроса маршрута звонка. */
    uint64_t generation; /* Поколение выбранного PM; для запроса маршрута не используется. */
};
void dm_messages_trampoline(void* arg);
/* 1 при живом BGP-маршруте общей группы, 0 при недоступности/ошибке. peer/group_id обязательны.
 * При корректных аргументах выходы сначала обнуляются; peer может быть известен и без маршрута.
 * Трамплин выдаёт CHAT_EVT_DM_CALL_ROUTE: [conv_len:1][conv][peer Ed25519:32][group Ed25519:32]; нулевой ключ без маршрута. */
struct dm_call_route_result {
    int status, sqlite_code; /* 0=проверенный снимок, -1=ошибка/отмена; точная SQL-причина. */
    int cancelled; /* Stop/restart/старое поколение: не публиковать UI даже при sqlite_code=BUSY/IOERR. */
    int available, peer_group; /* Есть живой маршрут; удалённая сторона закреплена как группа устройств. */
    uint64_t group, generation; /* Общая группа и поколение истории. */
    uint8_t peer_ed[32]; /* Точный адрес устройства на выбранном маршруте. */
};
typedef void (*dm_call_route_done_fn)(void* arg, const struct dm_call_route_result* result);
/* Uasync: скопировать conv, одной READ проверить pinned стороны/состав устройств и видимое поколение,
 * затем выбрать живой BGP-маршрут в uasync без SQL. generation=0 захватывает текущее поколение.
 * done обязателен, ровно один раз, включая OOM/FULL/stop; result заимствован до возврата.
 * arg/inst живут до done/drain. Stop/restart и удаление истории отменяют старый результат.
 * RAM может завершить callback синхронно. */
void dm_call_route_submit(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation, dm_call_route_done_fn done, void* arg);
void dm_call_route_trampoline(void* arg); /* аргумент: dm_messages_req */
/* Снимок бесед публикуется как CHAT_EVT_DM_CONVERSATIONS с JSON без NUL. */
struct dm_conversations_req { struct UTUN_INSTANCE* inst; /* Заимствованное ядро. */ };
void dm_conversations_trampoline(void* arg);

#ifdef __cplusplus
}
#endif
#endif /* DM_CORE_H */
