/* Файловые вложения личных сообщений: подготовка, шифрование, скачивание и подтверждение открытия.
 * На устройствах беседы модуль хранит локальные файлы и ищет доступные копии.
 * На узле-хранилище M он принимает зашифрованные копии с ограничением квоты и удаляет их по сроку/подтверждениям.
 * Размещением копий на хранилищах управляет отдельный координатор dm_mailbox_media.
 *
 * dm_send_file проверяет беседу и удерживает пути одной WRITE, затем шифрует файл в media_async.
 * Готовое сообщение сохраняется отдельной WRITE с повторной проверкой поколения; holds снимаются после её результата.
 * Загрузка endpoint/M также проверяет SQL-снимок до сети и после I/O; ready/квитанция меняются одной WRITE.
 * Завершение удаления custody сохраняет tombstone и снимок координаторов до отправки DELETE_ACK/EXPIRED.
 * dm_media_user_action обрабатывает ручное скачивание/открытие. Подтверждение файла независимо от доставки текста.
 * Сетевые команды и состояние приложения обслуживаются в uasync; тяжёлая работа с файлами — в media_async.
 * Функции с аргументом sqlite3* выполняются в потоке владельца соединения и используют только БД/входные данные.
 *
 * Порядок остановки: quiesce, завершение media_async, destroy. SQL-задачи отсоединяются и завершаются без поздних событий.
 * Ядро живёт до DB drain. Временный отказ освобождения holds повторяет media-таймер; после stop их сбрасывает history startup.
 * Входные строки/буферы можно освободить после возврата, кроме явно переданного временного файла.
 * Из C++ включать внутри extern "C", как в клиентах Qt/JNI.
 */
#ifndef DM_MEDIA_H
#define DM_MEDIA_H

#include <stdint.h>
#include <stddef.h>
#include <sqlite3.h>
#include "../media_async/attachment.h"
#include "dm_protocol.h"

struct UTUN_INSTANCE;

#define DM_MEDIA_DESC_SIZE 56 /* UUID:16, plaintext_size:8 native-endian, SHA256(ciphertext):32. */
#define DM_MEDIA_ASSIGNMENT_SIZE 16 /* Постоянный ID запроса S->M; повтор/смена маршрута сохраняют его. */
#define DM_MEDIA_MAX_SIZE (1024ULL * 1024 * 1024)
#define DM_MEDIA_KEYS_HEADER_SIZE 232 /* "DMKEY001" и DM_FILE_KEYS_SIZE байт. */
#define DM_MEDIA_ROOT_SIZE 768 /* Включая NUL; размер frozen пути каталога endpoint. */

/* Разобрать расшифрованное описание вложения data/size: заголовок, две упаковки ключа и attachment_info.
 * Возврат0=успех,-1=неверный формат; info/keys обязательны. *keys указывает внутрь data либо NULL для DEVICE прежнего формата.
 * Буфер data должен жить всё время использования *keys. SQL/сети нет, допустим вызов из worker. */
int dm_media_metadata_decode(const uint8_t* data, size_t size, struct attachment_info* info, const uint8_t** keys);

/* ETCP_RT_ID_DM_MEDIA=0x38: [cmd:1][body], router-подпись обязательна; числа native-endian.
 * OFFER: [recipient_ed25519:32][holder_ed25519:32][expires:8][каноническое file-сообщение]. A предлагает себя с expires=0.
 * STORE: [assignment:16][recipient_ed25519:32][file-сообщение] от S к M.
 * STORED/RESERVED: [assignment:16][UUID:16][expires:8][delete_on_open:1].
 * RECEIPT: [domain:8][conv:8][seq:8][author_ed:32][recipient_ed:32][UUID:16][SHA256(message):32][sig:64].
 * DELETE: [assignment:16][RECEIPT:200]; назначение не заменяет подпись факта получателя.
 * FILE_RECEIVED: DMFILE02/DMFILEG2; TAKEN: DMTAKE02/DMTAKEG2 для DEVICE/GROUPS.
 * Устройство подписывает первые 136 байт; DELETE переносит проверенный факт, M применяет свою сохранённую политику.
 * DELETE_ACK/EXPIRED: [assignment:16][UUID:16]; REJECT: [assignment:16][UUID:16][reason:1], reason=1 — квота.
 * SOURCE_GONE: [UUID:16][SHA256(message):32][holder_ed25519:32] от издателя OFFER или самого holder.
 * Предложения хранятся по (UUID,holder,issuer): S отзывает только своё, holder — все предложения своей копии.
 * Пока остаётся другое действующее предложение, копия доступна; оригинал автора не отзывается этим control.
 * EXPIRED относится только к назначению S/M. Получателю не объявляется удаление личного файла. */
#define DM_MEDIA_OFFER_HOLDER_OFF 32
#define DM_MEDIA_OFFER_EXPIRES_OFF 64
#define DM_MEDIA_OFFER_HEADER 72
#define DM_MEDIA_STORE_RECIPIENT_OFF 16
#define DM_MEDIA_STORE_HEADER 48
#define DM_MEDIA_GONE_HOLDER_OFF 48
#define DM_MEDIA_GONE_SIZE 80

#define DM_MEDIA_OFFER      1
#define DM_MEDIA_STORE      2
#define DM_MEDIA_STORED     3
#define DM_MEDIA_RECEIPT    4
#define DM_MEDIA_DELETE     5
#define DM_MEDIA_DELETE_ACK 6
#define DM_MEDIA_EXPIRED    7
#define DM_MEDIA_REJECT     8
#define DM_MEDIA_RESERVED   9 /* assignment:16, UUID:16, expires:8, delete_on_open:1; ciphertext ещё не подтверждён. */
#define DM_MEDIA_HAVE      10 /* UUID:16, SHA256:32, probe_id:8, cipher_size:8. */
#define DM_MEDIA_HAVE_ACK  11 /* Точное эхо HAVE; RTT измеряет запросившее устройство. */
#define DM_MEDIA_SOURCE_GONE 12

/* В uasync после dm_core_init создать файловый сервис inst: таблицы, каталоги, передачу файлов, сетевой обработчик и таймер.
 * Требуются работающие БД/config/router/uasync/media_async. 0=успех,-1=ошибка,включая повторный запуск работающего сервиса.
 * При отказе уже созданные таблицы/каталоги остаются.
 * Корень: db_path/dm_media и db_path/dm_pending; без db_path используется текущий каталог.
 * После перезапуска возобновляются загрузки/повторы; история, копии на хранении и квитанции сохраняются. */
int dm_media_init(struct UTUN_INSTANCE* inst);
/* Worker в общей startup WRITE: схемы media/mailbox, reset transient retry/download и restore targets/receipts.
 * Без runtime/собственной транзакции, SQLITE_OK/положительный код. После commit RW dm_media_init не выполняет SQL. */
int dm_media_prepare_write(sqlite3* db);
/* В uasync запретить новые задачи и отменить таймер, сетевой обработчик и передачи файлов inst.
 * Повторный вызов допустим. Вызывать до media_async_destroy: обработчики отмены ещё используют живые БД/группы. NULL допустим. */
void dm_media_quiesce(struct UTUN_INSTANCE* inst);
/* После завершения media_async освободить файловый сервис inst, сохранив его данные в БД.
 * Вызывать до удаления групп/ядра/mailbox. При незавершённых задачах логирует ошибку и ждёт повторного destroy. NULL допустим. */
void dm_media_destroy(struct UTUN_INSTANCE* inst);
/* После изменения допуска группы отменить её ожидающие HAVE-пробы; поздний ответ их не оживляет.
 * Файлы/назначения остаются; следующий поиск выберет свежий маршрут. FILE_TRANSFER отменяется своим owner. */
void dm_media_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group);

/* Начать подготовку файла path и создание сообщения-вложения в беседе conv с метаданными info.
 * Строки conv/path и info копируются при вызове; сам файл должен быть доступен worker до завершения.
 * 0=запрос принят в SQL/файловый цикл,-1=отказ до принятия. Результат приходит через CHAT_EVT_ATTACHMENT_PREPARATION;
 * при немедленном отказе DB/worker это событие может появиться ещё до возврата0.
 * Беседа должна быть видимой; до 4 одновременных подготовок, plaintext до DM_MEDIA_MAX_SIZE.
 * remove_source=1 передаёт worker временный файл приложения для удаления; при -1 его убирает вызывающий.
 * Успех подготовки означает commit file-сообщения, не доставку. Пользовательский исходник: remove_source=0.
 * request_id!=0 связывает результат с CHAT_EVT_MSG_SUBMITTED после commit/ошибки; при возврате-1 событие посылает вызывающий. */
int dm_send_file(struct UTUN_INSTANCE* inst, const char* conv, const char* path, const struct attachment_info* info,
                 int remove_source, uint64_t request_id);
struct dm_file_req {
    struct UTUN_INSTANCE* inst;  /* Ядро должно жить до выполнения запроса в uasync. */
    char conv[64];               /* Десятичный ID беседы с NUL. */
    uint64_t generation;        /* Ожидаемое поколение; 0=захватить в первой WRITE перед шифрованием. */
    uint64_t request_id;        /* ID для сопоставления результата записи;0=без отдельного подтверждения. */
    char path[1024];             /* Путь исходника с NUL; копируется при принятии. */
    struct attachment_info info; /* Проверенные метаданные вложения. */
    int remove_source; /* Только принадлежащий приложению временный файл. */
};
/* Передать в uasync один выделенный dm_file_req через arg; функция проверяет поколение истории и освобождает запрос.
 * При отказе переданный remove_source удаляется через worker. */
void dm_send_file_trampoline(void* arg);

/* Действие пользователя над сохранённым file-сообщением (conv,author,seq).
 * opened=0 разрешает ручную загрузку; opened=1 фиксирует успешное открытие уже
 * готового файла. Вызывать opened только после реального действия UI, не из preview.
 * После выхода из беседы готовый файл открывается локально без отправки TAKEN.
 * FILE/RW копирует запрос,проверяет поколение/членство и сохраняет факт в worker WRITE;уведомление после commit.
 * 0=принято,-1=ошибка аргументов/памяти;только uasync. В RAM SQL выполняется до возврата. */
int dm_media_user_action(struct UTUN_INSTANCE* inst, const char* conv, const uint8_t author[32], uint64_t seq, int opened);
struct dm_media_action_req {
    struct UTUN_INSTANCE* inst; /* Ядро живо до выполнения uasync callback. */
    char conv[64];              /* Десятичный ID беседы. */
    uint8_t author[32];      /* Ed25519 автора подписанного сообщения. */
    uint64_t seq, generation; /* Точная запись; generation защищает удаление истории. */
    int opened;                /* 0=скачать,1=пользователь успешно открыл. */
};
/* Один u_calloc-блок; проверяет generation и освобождает запрос. */
void dm_media_action_trampoline(void* arg);

/* Legacy RAM/READONLY: внутри транзакции dm_core после проверки msg/len; dir:0=входящее,1=исходящее.
 * 0=записано/точный повтор UUID и тела, -1=ошибка/конфликт; rollback делает вызывающий.
 * Для не-file — пустая операция. Исходящий file требует уже подготовленного .file нужного размера.
 * FILE/RW отвергает этот вызов; использовать dm_media_track_write в worker. */
int dm_media_track(struct UTUN_INSTANCE* inst, int dir, const uint8_t* msg, size_t len);
/* Uasync: скопировать путь приложения для id[16], без SQL/дискового I/O. Живой media обязателен.
 * out вмещает1024 байта;0=путь готов,-1=отказ. Путь не удерживает файл: владелец сохраняет свои holds. */
int dm_media_path_snapshot(struct UTUN_INSTANCE* inst, const uint8_t id[16], char out[1024]);
/* Uasync: скопировать корень файлов endpoint в out[DM_MEDIA_ROOT_SIZE], без SQL/I/O. 0/-1; media должен быть жив. */
int dm_media_root_snapshot(struct UTUN_INSTANCE* inst, char out[DM_MEDIA_ROOT_SIZE]);
/* Worker/WRITE: зарегистрировать file и снимок адресатов по db, self и собственной копии path.
 * Для исходящего проверяет stat подготовленного .file; чтения runtime/уведомлений нет.
 * SQLITE_OK либо положительный SQLite-код; commit/rollback у caller. Для не-file пустая операция. */
int dm_media_track_write(sqlite3* db, const uint8_t self[32], const uint8_t* msg, size_t len, const char* path);
struct dm_file_offer {
    uint8_t holder[32];         /* Ed25519 устройства, предлагающего копию file-сообщения. */
    uint8_t issuer[32];         /* Ed25519 подписавшего OFFER: автор либо проверенный суперузел. */
    uint64_t expires;           /* Срок custody в секундах; для оригинала автора нормализуется в0. */
};
/* SQL/WRITE: сохранить предложение для проверенного file-тела body/len и обновить незавершённую загрузку.
 * Вызывать после регистрации file в той же транзакции. Допуск/роль issuer проверяет caller в uasync.
 * Точный повтор допустим; отозванное/изменённое предложение не оживает.
 * SQLITE_OK также при устаревшем предложении, иначе положительный код; commit/rollback у caller, runtime не используется. */
int dm_media_source_offer_write(sqlite3* db, const uint8_t* body, size_t len, const struct dm_file_offer* offer);
struct dm_file_revoke {
    uint8_t id[16], body_hash[32]; /* UUID и SHA256 полного канонического file-сообщения. */
    uint8_t holder[32], issuer[32]; /* Ed25519 держателя копии и подписавшего отзыв. */
    uint64_t now;                 /* Frozen NTP-время для выбора оставшихся действующих предложений. */
    int holder_issued; /* Подпись router сделана самим держателем копии. */
};
/* SQL/WRITE: сверить точное тело и отозвать предложение issuer; сам holder отзывает все предложения своей копии.
 * Оригинал автора и готовые локальные файлы сохраняются. Неизвестный/конфликтный файл даёт NOTFOUND/AUTH.
 * SQLITE_OK либо положительный SQLite-код; caller проверяет сетевой допуск и завершает транзакцию. */
int dm_media_source_revoke_write(sqlite3* db, const struct dm_file_revoke* revoke);
/* Заимствованный descriptor внутри точного канонического file-тела либо NULL.
 * Проверяет только формат/длину, не подпись и не размер/хеш файла; указатель живёт вместе с msg. */
const uint8_t* dm_message_media(const uint8_t* msg, size_t len);
/* Ciphertext bytes для plain_size<=DM_MEDIA_MAX_SIZE: plaintext плюс 16 байт на порцию 32768 (для пустого — 16). */
uint64_t dm_media_cipher_size(uint64_t plain_size);
/* В переданной db проверить200-байтовую квитанцию receipt: подпись устройства и при msg!=NULL соответствие сообщению msg/len.
 * Проверяются UUID и SHA256; для GROUPS ключ берётся из закреплённого списка устройств сообщения,иначе из nodes.
 * SQLITE_OK=верна,AUTH=отказ/ключ ещё неизвестен,иначе код SQL-ошибки. В БД ничего не меняет,транзакция у вызывающего. */
int dm_media_verify_receipt(sqlite3* db, const uint8_t* receipt, const uint8_t* msg, size_t len);
/* Для локального A проверить 200-байтовую квитанцию и сохранить её в точном исходящем dm_files.
 * FILE/RW: копирует receipt и ставит WRITE,0=принят,-1=неверный запрос/нет памяти; проверка/сохранение асинхронны.
 * RAM:0=сохранена/повтор либо точное подтверждение уже удалённого файла;-1=ошибка/неизвестное тело.
 * История, собственный файл A и очередь отправки текста сохраняются. */
int dm_media_accept_receipt(struct UTUN_INSTANCE* inst, const uint8_t* receipt);

/* Legacy RAM/READONLY внутри dm_history_delete: сохранить пути в манифесте поколения и удалить dm_files беседы.
 * 0=успех,-1=ошибка/FILE-RW; требует живого dm_media,commit/rollback делает caller. Для RW — dm_media_forget_write. */
int dm_media_forget_prepare(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation);
/* Worker/WRITE: записать манифест из frozen root и удалить file-метаданные conv текущего generation.
 * SQLITE_OK/положительный код; root — копия каталога endpoint, runtime/BEGIN/COMMIT не используются. */
int dm_media_forget_write(sqlite3* db, const char* root, const char* conv, uint64_t generation);
/* После commit удаления: READ-снимок UUID, отмена providers в uasync, затем WRITE-claim и файловый worker.
 * conv копируется до возврата; NULL повторяет отмену всех tombstones после OOM. SQL в uasync не выполняется.
 * Очистка ждёт holds; FULL/OOM/BUSY повторяются таймером. Stop оставляет durable manifest для startup recovery. */
void dm_media_forget_committed(struct UTUN_INSTANCE* inst, const char* conv);
/* Legacy RAM/READONLY в транзакции выхода: пометить пути незавершённых closed-бесед для очистки.
 * Готовые файлы сохраняются.0=успех,-1=ошибка/FILE-RW;без media — no-op. Для RW — dm_media_close_write. */
int dm_media_close_prepare(struct UTUN_INSTANCE* inst);
/* Worker/WRITE: пометить пути незавершённых closed-бесед для очистки. Готовые файлы сохраняются.
 * SQLITE_OK/положительный код, db/активная транзакция обязательны; runtime не используется. */
int dm_media_close_write(sqlite3* db);
/* После commit выхода: READ-снимок closed-бесед и отмена transfers в uasync, затем фоновая очистка.
 * Незавершённые workers теряют допуск публикации; готовые локальные файлы сохраняются. FULL/OOM повторяет таймер. */
void dm_media_close_committed(struct UTUN_INSTANCE* inst);
/* Legacy RAM/READONLY: зарегистрировать path и увеличить busy до worker/transfer.
 * conv/generation текущие и видимые;0=захват,-1=ошибка/FILE-RW. Для RW — dm_media_paths_write. */
int dm_media_file_hold(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation, const char* path);
/* Legacy RAM/READONLY: снять один hold после callback; SQL/счётчик логируются. FILE/RW вызов отвергается.
 * Уборка старого поколения ждёт всех holds;busy=-1 — захват уборщиком. Для RW — dm_media_paths_write. */
void dm_media_file_release(struct UTUN_INSTANCE* inst, const char* path);
/* Worker в caller WRITE: acquire (release=0) либо release=1 всего набора paths[0..count), count=0..4.
 * Строки заимствованы до возврата. Один элемент — один hold; повторы пути считаются отдельно. count=0 — no-op.
 * При acquire caller сначала проверяет текущую видимую history generation в той же транзакции (dm_history_state_read).
 * Аргумент generation>=1; release также снимает hold строки с generation=0 после выхода/удаления истории.
 * Не открывает транзакцию и не обращается к runtime/ФС. SQLITE_OK либо положительный код, включая CONSTRAINT при чужом пути/
 * неверном поколении/счётчике. При ошибке caller откатывает весь набор. Success действует только после общего commit. */
int dm_media_paths_write(sqlite3* db, const char* conv, uint64_t generation, const char* const* paths, unsigned count, int release);

/* Отправить cmd/body/len через подписанный router к peer; group — только предпочтительный маршрут (0=любой).
 * Выбирается текущая допущенная общая группа self/peer независимо от хранения и объявления роли.
 * 0=пакет принят транспортом,иначе ошибка/очередь заполнена. Постоянную очередь повторов создаёт вызывающий.
 * Локальный peer обрабатывается синхронно: результат хранения подтверждается отдельно.
 * body нужен только до возврата; len должен соответствовать формату команды. */
int dm_media_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32],
                      uint8_t cmd, const uint8_t* body, size_t len);
/* Обёртка для локальных задач с числовой проекцией адреса. */
/* Опубликовать CHAT_EVT_DM_MSG_RECEIVED [conv_len:1][conv:decimal][local Ed25519:32] для обновления снимка UI. */
void dm_media_notify(struct UTUN_INSTANCE* inst, uint64_t conv);

#endif
