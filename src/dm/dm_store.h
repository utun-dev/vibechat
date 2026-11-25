/* Хранение личных бесед и проверка подписанных DM-сообщений в потоке БД.
 * Модуль создаёт/загружает беседы и историю, раскрывает ключ беседы, проверяет сообщения и сохраняет квитанции доставки.
 * Его используют задачи DB worker: им передают соединение SQLite, копию ключей устройства и входные данные.
 * Результаты содержат собственные копии данных; SQL-указатели и состояние сетевого ядра наружу не передаются.
 *
 * Вызывающий код открывает транзакцию чтения или записи, вызывает нужную функцию и завершает транзакцию.
 * Обновлять состояние приложения и отправлять пакеты следует после commit. Копии секретов identity/content_key
 * вызывающий код очищает после использования. Вспомогательные функции имени/формата ключа работают без БД. */
#ifndef UTUN_DM_STORE_H
#define UTUN_DM_STORE_H
#include "dm_core.h"
#include "dm_crypto.h"
#include <sqlite3.h>
#ifdef __cplusplus
extern "C" {
#endif

struct dm_store_identity {
    uint64_t node;              /* ID локального устройства, скопированный до передачи задачи в worker. */
    uint8_t x[32], ed[32];      /* Публичные ключи конкретного устройства. */
    uint8_t private_key[32];    /* X25519-секрет устройства для чтения локальной истории. */
};

struct dm_conv {
    char conv_id[64];           /* Канонический положительный 63-битный ID с NUL. */
    uint8_t peer_x25519[32], peer_ed25519[32]; /* Публичные ключи закреплённой удалённой стороны. */
    char peer_name[DM_PEER_NAME_MAX + 1]; /* Полный UTF-8 префикс до 63 байт. */
    char group_id[65];          /* Публичный Ed25519 предпочтительной группы, пусто без подсказки. */
    uint8_t content_key[DM_CONTENT_KEY_SIZE]; /* Выведенный/раскрытый секрет; не передавать в сеть. */
    uint64_t last_out_seq, last_in_seq, last_ts; /* Сохранённые счётчики и время беседы. */
    enum dm_mode mode;          /* DEVICE=два устройства,GROUPS=закреплённые стороны из групп устройств. */
    struct dm_party parties[2]; /* Локальная сторона и собеседник, закреплённые при создании. */
    uint8_t stored_key[DM_KEY_WRAP_SIZE]; /* GROUPS: content_key, упакованный к этому устройству. */
    int stored_key_valid, closed; /* Наличие упаковки; closed запрещает новые операции, архив читается. */
};

struct dm_start_write {
    struct dm_store_identity identity; /* Копия ключей локального устройства; caller очищает после выполнения. */
    uint64_t peer;              /* Выбранное удалённое устройство, не локальный узел. */
    char group[65], fallback_group[65]; /* Предложенная и известная общие группы. */
    uint64_t now; /* Время создания в секундах. */
    enum dm_mode mode;          /* DEVICE/GROUPS для обеих сторон новой беседы. */
    int supplied_keys;          /* 1=peer_x/peer_ed уже проверены caller;0=прочитать keys из nodes. */
    uint8_t peer_x[32], peer_ed[32]; /* Входные ключи устройства при supplied_keys=1. */
    int supplied_name;          /* 1=использовать name,0=прочитать и нормализовать имя из nodes. */
    char name[DM_PEER_NAME_MAX + 1]; /* Входное UTF-8 имя с NUL; используется только при создании. */
    uint64_t conversation, generation; /* Выход: ID/поколение после успешной работы,0 при ошибке. */
};
/* В транзакции записи db сохранить c; local_x — публичный ключ устройства для упаковки GROUPS content_key.
 * created_at — секунды; при обновлении сохраняется исходное время создания. Все bind/step/finalize проверяются.
 * SQLITE_OK либо положительный код SQLite/формата/crypto; commit/rollback делает caller. */
int dm_store_conv_save(sqlite3* db, const uint8_t local_x[32], const struct dm_conv* c, uint64_t created_at);
/* В одной транзакции записи создать/проверить беседу и снять hidden, не меняя её поколение.
 * write — копии входных данных, runtime не используется. GROUPS проверяет текущие стороны и локальные секреты.
 * Существующие имя/ключи/счётчики сохраняются; отсутствующая область может дополняться.
 * SQLITE_OK и заполненные conversation/generation либо точный положительный код; на ошибке выходы0.
 * Публиковать результат/события разрешено только после commit; identity и временные секреты очищает caller. */
int dm_store_start(sqlite3* db, struct dm_start_write* write);

/* Нормализовать len байт имени: полный UTF-8 префикс до 63 байт, обрыв/внутренний NUL отсекают суффикс.
 * out обязателен; name=NULL только при len=0, conv — метка для диагностики. SQL/выделений памяти нет. */
void dm_store_copy_name(char out[DM_PEER_NAME_MAX + 1], const char* name, size_t len, const char* conv);
/* Канонические info-байты HPKE локального ключа: домен/conv/обе стороны. Буфер192 байта;
 * вернуть длину, 0 при неверных аргументах/сторонах; SQL/секреты не использует. */
size_t dm_store_key_info(const struct dm_conv* conv, uint8_t info[192]);
/* В транзакции чтения db загрузить беседу conv и раскрыть её content_key с помощью копии ключей identity.
 * SQLITE_OK либо точный положительный SQLite-код; found=0 при отсутствии/ошибке,1 при успехе.
 * out/found обязательны; out меняется после завершения SQL и проверки ключа. Секреты в out очищает вызывающий. */
int dm_store_conv_read(sqlite3* db, const struct dm_store_identity* identity, const char* conv,
    struct dm_conv* out, int* found);
/* Собрать зашифрованное подписанное тело по копиям identity/ключа подписи и загруженной беседе.
 * GROUPS читает проверенный snapshot устройств из db в транзакции caller; DEVICE не использует db.
 * type до255 байт, data до DM_DATA_MAX, file включает descriptor. seq=1..INT64_MAX, ts — миллисекунды.
 * SQLITE_OK и собственные body/body_size либо положительный код; на ошибке выходы NULL/0.
 * Caller освобождает body через u_free. Runtime не используется, секреты остаются во входах caller. */
int dm_store_build_body(sqlite3* db, const struct dm_store_identity* identity, const uint8_t signing_key[32],
    const struct dm_conv* conv, uint64_t seq, uint64_t ts, const char* type, const uint8_t* data, uint32_t size,
    uint8_t** body, size_t* body_size);
/* В WRITE сохранить проверенное каноническое тело. Точный повтор допустим, конфликт одного ключа запрещён.
 * SQLITE_OK и inserted=0/1 либо положительный код; inserted обязателен и0 при ошибке.
 * Подпись проверяет caller; commit/rollback также у caller. Runtime не используется. */
int dm_store_message_save(sqlite3* db, const char* conv, int dir, const uint8_t* body, size_t size, int* inserted);

struct dm_send_write {
    struct dm_store_identity identity; /* Копии ключей устройства; caller очищает после завершения. */
    uint8_t signing_key[32];    /* Собственный Ed25519-секрет для подписи тела. */
    char conv[64], type[256];   /* Канонический ID беседы и NUL-строка content type. */
    uint8_t data[DM_DATA_MAX]; /* Собственная копия plaintext/descriptor; size задаёт используемые байты. */
    uint32_t size;             /* Используемые байты массива data,0..DM_DATA_MAX. */
    char file_path[1024];      /* File: уже подготовленный путь приложения; прочие типы не используют. */
    uint64_t generation, now_ms; /* Ожидаемое поколение (0=текущее) и frozen NTP-время в миллисекундах. */
    uint64_t seq, current_generation; /* Выходы после работы; публикуются только после commit. */
};
/* В одной WRITE проверить беседу/поколение, выделить seq, подписать и сохранить историю, адресатов, file и outbox.
 * SQLITE_OK либо положительный код; на ошибке выходы0, rollback делает caller.
 * Все входы принадлежат write, runtime/сеть не используются; копии секретов очищает caller. */
int dm_store_send(sqlite3* db, struct dm_send_write* write);
struct dm_receive_update {
    uint64_t conversation, author, seq, generation; /* Подписанные ID и поколение; у удалённого повтора generation=0. */
    int created, inserted, deleted; /* Новая беседа, новая запись истории либо точный удалённый повтор. */
    uint8_t receipt[DM_RECEIPT_SIZE]; /* Подготовленный ACK; публиковать только после commit. */
    uint8_t media_receipt[DM_MEDIA_RECEIPT_SIZE]; /* Сохранённая квитанция удалённого файла либо нули. */
};
struct dm_receive_write {
    struct dm_store_identity identity; /* Frozen ключи локального устройства; caller очищает после completion. */
    uint8_t signing_key[32];       /* Ed25519-секрет для подписи квитанции. */
    uint8_t body[DM_BODY_MAX];    /* Собственная копия полного канонического сообщения. */
    size_t size;                 /* Используемые байты body, не больше DM_BODY_MAX. */
    uint64_t now;                /* Frozen время создания беседы, секунды. */
    char file_path[1024];        /* File: локальный путь из media-owner; удалённому повтору не требуется. */
    struct dm_file_offer offer;  /* Нулевой issuer — обычный приём; иначе записать OFFER в той же транзакции. */
    struct dm_receive_update update; /* Только результат SQL; при ошибке пуст, публикация после commit. */
};
/* В одной WRITE проверить подпись/адресата/replay, расшифровать и сохранить беседу/историю/file.
 * Подписывает ACK до возврата; ошибка подписи также требует rollback. Новая запись снимает hidden.
 * SQLITE_OK либо положительный код; update пуст при ошибке. Точный удалённый повтор не восстанавливает историю.
 * Runtime/сеть не используются; транзакция и очистка копий ключей принадлежат caller. */
int dm_store_receive(sqlite3* db, struct dm_receive_write* write);
/* В транзакции чтения db сформировать JSON до200 видимых бесед: имена, поколения, последние проверенные имена групп устройств.
 * buf/length обязательны, capacity>=3. SQLITE_OK/положительный код; ошибка очищает весь результат. */
int dm_store_conversations_read(sqlite3* db, char* buf, size_t capacity, size_t* length);
/* В транзакции чтения db расшифровать историю беседы conv в JSON-буфер buf; include_hidden разрешает скрытый архив.
 * generation:0 — прочитать текущий, иначе требуется совпадение; на успехе получает фактический.
 * count вне1..200 означает200, offset>=0. SQLITE_OK/точный положительный код, выход пуст при ошибке.
 * identity/conv/buf/length/generation обязательны; транзакцией и очисткой копии секретов identity управляет вызывающий. */
int dm_store_messages_read(sqlite3* db, const struct dm_store_identity* identity, const char* conv,
    int count, int offset, int include_hidden, uint64_t* generation, char* buf, size_t capacity, size_t* length);
/* Прочитать из nodes оба публичных ключа устройства node; x/ed меняются только после завершения SQL-запроса.
 * db/node/x/ed обязательны. SQLITE_NOTFOUND — неизвестный node/ещё не полученный ключ (SQL NULL).
 * Иначе SQLITE_OK либо точный положительный код ошибки SQL/формата. */
int dm_store_node_keys(sqlite3* db, uint64_t node, uint8_t x[32], uint8_t ed[32]);
/* Получить точный Ed25519 адресата из подписанного списка либо каталога устройств. */
int dm_store_message_target_key(sqlite3* db, const struct dm_message_view* view, uint64_t node, uint8_t ed[32]);
/* В переданной db проверить подпись сообщения msg/len и адресата recipient; GROUPS использует сертификаты из самого сообщения.
 * SQLITE_OK — корректно, SQLITE_AUTH — неверный пакет/подпись, иначе положительный SQL-код.
 * recipient/msg обязательны, len — полная длина без лишнего хвоста. Вызывать внутри транзакции чтения или записи. */
/* Проверить тело для известного полного адресата; NULL ed разрешает числовой SQL-запрос. */
int dm_store_verify_message_for(sqlite3* db, uint64_t recipient, const uint8_t* recipient_ed, const uint8_t* msg, size_t len);
int dm_store_verify_message(sqlite3* db, uint64_t recipient, const uint8_t* msg, size_t len);
/* В переданной db проверить136-байтовую квитанцию receipt; при msg!=NULL также сверить её с сообщением msg/len.
 * SQLITE_OK=верна,AUTH=отказ,иначе положительный код SQL-ошибки. Вызывать в транзакции чтения или записи. */
int dm_store_verify_receipt(sqlite3* db, const uint8_t receipt[DM_RECEIPT_SIZE], const uint8_t* msg, size_t len);
struct dm_receipt_update {
    uint64_t conv, seq, recipient; /* Подписанные ID, пригодны для публикации после commit. */
    int present;                  /* Была точная запись outbox; только тогда требуется событие обновления. */
    int complete;                 /* Все адресаты подтвердили: строка outbox удалена. */
};
/* В транзакции записи db проверить квитанцию для локального self, сохранить подтверждение адресата
 * и убрать сообщение из очереди отправки, когда подтвердили все адресаты. История и файлы сохраняются.
 * out обязателен и пуст при отказе; повтор для уже отправленного сообщения успешен с present=0.
 * SQLITE_OK/AUTH/CONSTRAINT либо код SQL-ошибки. Commit и последующее уведомление выполняет вызывающий. */
int dm_store_receipt_apply(sqlite3* db, const uint8_t self_ed[32], const uint8_t receipt[DM_RECEIPT_SIZE], struct dm_receipt_update* out);

#define DM_OUTBOX_BATCH_MAX 32
struct dm_outbox_delivery {
    char conv[64];             /* Собственный десятичный ID беседы с NUL. */
    uint64_t seq;       /* Номер сообщения. */
    char group[65]; /* Публичный Ed25519 предпочтительной группы. */
    uint8_t recipient_ed[32];  /* Полный адрес из закреплённой беседы или подписанного снимка. */
    int64_t direct_until;      /* Сохранённый срок ожидания прямой квитанции; после него нужен также S. */
    uint8_t* body;             /* Собственная копия; освобождается dm_outbox_batch_clear. */
    size_t size;               /* Полное каноническое тело, до DM_BODY_MAX. */
};
struct dm_outbox_batch {
    uint8_t self_ed[32];       /* Полный Ed25519 локального автора. */
    int64_t now;               /* Время попытки в секундах; retry_at становится now+5. */
    unsigned count;            /* Число записей до32; перед claim должно быть0. */
    struct dm_outbox_delivery items[DM_OUTBOX_BATCH_MAX];
};
/* В открытой WRITE скопировать до32 готовых адресатов и записать retry_at/first_attempt/direct_until.
 * Закрытые/скрытые беседы и подтверждённые адресаты исключаются. Проверяет соответствие тела ключам строки.
 * now=0..INT64_MAX-5, self_ed обязателен, count=0. SQLITE_OK/положительный код; ошибка очищает копии, rollback делает caller.
 * Передавать items сети можно только после commit. Список адресатов/тела/квитанции не изменяются. */
int dm_outbox_batch_claim(sqlite3* db, struct dm_outbox_batch* batch);
/* Освободить собственные тела и обнулить items/count, без SQL. self_ed/now сохраняются, NULL допустим. */
void dm_outbox_batch_clear(struct dm_outbox_batch* batch);
#ifdef __cplusplus
}
#endif
#endif
