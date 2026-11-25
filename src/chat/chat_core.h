/*
 * chat_core — API каналов, сообщений, профиля и настроек одного узла.
 * chat_service_start/stop управляют всем чат-сервисом поверх работающего ядра.
 * Данные хранятся в общей SQLite; события для интерфейса доставляет chat_event.
 * Порядок: utun_core_start → chat_service_start → команды чата → chat_service_stop.
 * Сообщения отправляются через submit_message или submit_media_message; результат GUI — CHAT_EVT_MSG_SUBMITTED.
 *
 * Runtime API и сетевые функции вызываются в uasync; SQL асинхронных команд выполняет DB worker.
 * Все функции per-instance: первым аргументом struct UTUN_INSTANCE*.
 * GUI вызывает через gui_bridge_post_uasync() trampoline-функции,
 * чей arg-структура несёт inst.
 */

#ifndef CHAT_CORE_H
#define CHAT_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <sqlite3.h>
#include "../../lib/db_async.h"
#include "db_sync.h"

struct sqlite3;
struct UTUN_INSTANCE;
struct member_sync_write_context;
struct group_policy_update;

#ifdef __cplusplus
extern "C" {
#endif

// ── Жизненный цикл ──

// Запустить чат поверх core_start; UTUN не требуется. Повтор допустим; 0/-1.
int chat_service_start(struct UTUN_INSTANCE* inst);

/* Остановить чат, сохранив ядро, UTUN и БД. Повтор допустим; вызывать вне callbacks чата.
 * Отменяет доставку и ждёт native media workers; длительная задача может задержать stop. */
void chat_service_stop(struct UTUN_INSTANCE* inst);
/* Uasync: номер текущего initialized chat_core,0 при отсутствии/остановке. Номер меняется при каждом init,
 * включая reuse адреса cc; подходит для guard/publish отложенных операций. inst живёт до DB drain. */
uint64_t chat_core_generation(struct UTUN_INSTANCE* inst);
/* Uasync в начале общего group stop, до media drain: запретить новые команды/публикацию, generation getter вернёт0.
 * SQL/ожиданий нет, ресурсы сохраняются для завершения callbacks. Повтор безопасен; затем вызвать destroy. */
void chat_core_quiesce(struct UTUN_INSTANCE* inst);

/* Внутренняя часть запуска:заимствовать БД работающего ядра,сохранить метаданные и подготовить каналы. 0/-1.
 * Собственная транзакция включает schema/identity/media_base и установку обязательных подписок;чужая запрещена.
 * db_path<=511 байт задаёт default media base,отдельная БД не открывается. Готовность публикуется после commit/activation.
 * Activation error сохраняет committed метаданные;caller откатывает runtime через chat_service_stop и может повторить старт.
 * Для запуска всего чат-сервиса использовать chat_service_start(). */
int  chat_core_init(struct UTUN_INSTANCE* inst, const char* db_path);

/* Замороженные публичные поля локальной идентичности для SQL bootstrap; секретных ключей здесь нет.
 * name<=63 байт (пустая ->Me), db_path<=511; строки принадлежат caller и живут до возврата prepare. */
struct chat_core_bootstrap_input {
    uint64_t node_id, now_sec;
    uint8_t x25519[32], ed25519[32];
    const char *name, *db_path;
};
struct chat_core_bootstrap;
/* SQL-owner caller WRITE: schema/identity/media_base и проверенный каталог, без runtime/BEGIN/COMMIT.
 * SQLITE_OK/positive SQLite code; при успехе out получает собственный snapshot, при отказе NULL. */
int chat_core_prepare(struct sqlite3* db, const struct chat_core_bootstrap_input* input, struct chat_core_bootstrap** out);
void chat_core_bootstrap_free(struct chat_core_bootstrap* prepared);
/* Uasync после commit: создать cc/подписки/кеши настроек и media_base без SQL; prepared заимствован до возврата.
 * Каналы/остальные модули активирует startup chain; готовность ещё не публикуется.0/-1, retry создаёт новый generation. */
int chat_core_activate(struct UTUN_INSTANCE* inst, const struct chat_core_bootstrap* prepared);
/* Uasync/noSQL: однократно DB_READY после завершения всех startup activations. */
void chat_core_publish_ready(struct UTUN_INSTANCE* inst);
/* Uasync/noSQL: immutable проверенная база медиа, заимствована до core destroy; NULL до activation.
 * Для асинхронной задачи скопировать строку до submit. */
const char* chat_core_media_base(struct UTUN_INSTANCE* inst);

// Освободить chat_core после остановки остальных частей чата; общая БД остаётся у ядра.
void chat_core_destroy(struct UTUN_INSTANCE* inst);

/* Startup: восстановить производный attachment-индекс из подписанной истории и зарегистрировать сохранённые медиа.
 * FILE/RW ставит owned WRITE snapshot; повторные запросы объединяются. Результаты публикуются после commit.
 * Вызывать после attachment/startup_backfill; SQL-отказ ставит полный повтор. Ключи из истории не восстанавливаются. */
void chat_media_backfill(struct UTUN_INSTANCE* inst);

/* Докачка недостающего медиа за последние storage_backfill_days дней и применение квоты.
 * Также восстанавливает attachment-индекс/доступность сохранённых контейнеров; SQL-отказ ставит повтор.
 * Перед удалением по квоте/очисткой сирот требуется полный снимок ссылок;ошибка SQL/памяти отменяет удаление.
 * Один путь учитывается один раз,с самой новой ссылкой;derived plaintext входит в общий кеш.
 * FILE/RW SQL/проверку путей выполняет worker; uasync применяет готовый список с проверкой запуска и namespace. */
void chat_media_autodownload_backfill(struct UTUN_INSTANCE* inst);

/* Подписать завершения Merkle: первый обмен запускает общий scan после 2с тишины; отдельные каналы — batch до16 через2с.
 * Повтор индекса не отменяет MSG_RECEIVED committed-истории; следующий batch может быть поставлен во время текущего.
 * При переполнении очереди и для восстановления допускаются последующие общие scan.
 * init:0 — обе подписки установлены/повтор,-1 — отказ с отменой частичной установки.
 * Destroy снимает подписки/таймеры и отсоединяет активный scan; late done только освобождает собственные данные.
 * Вызывать до media drain/освобождения chat_core, также при content stop с живым общим сервисом групп. */
int chat_media_startup_backfill_init(struct UTUN_INSTANCE* inst);

void chat_media_startup_backfill_destroy(struct UTUN_INSTANCE* inst);

// Заимствованная legacy БД; FILE/RW возвращает NULL — соединением пользуется только DB worker.
struct sqlite3* chat_core_get_db(struct UTUN_INSTANCE* inst);

// 1 — chat_core инициализирован, 0 — нет; не проверяет доступность сети.
int chat_core_is_initialized(struct UTUN_INSTANCE* inst);

// ── Отправка сообщения (GUI → uasync) ──

struct chat_msg_submit {
    struct UTUN_INSTANCE* inst;
    char     channel_id[65];    /* публичный Ed25519 группы (64 HEX) */
    char     content_type[32];  /* тип содержимого сообщения */
    char     media_src[1024];   /* исходный файл вложения */
    char     media_dest[1024];  /* путь подготовленного локального файла */
    uint8_t  media_copy;        /* 1 = скопировать исходный файл при регистрации */
    uint8_t  media_video;        /* 1 = подготовить как видео (пробинг+транскод) */
    int      duration_ms;        /* видео: длительность (0 = неизвестно) */
    int      width;              /* видео: ширина */
    int      height;             /* видео: высота */
    uint8_t* data;              /* содержимое сообщения, заимствовано обычным submit */
    uint32_t data_len;          /* размер data в байтах */
    uint64_t timestamp;         /* успешная отправка возвращает назначенное время */
    uint8_t  message_id[32];    /* успешная обычная отправка возвращает общий ID */
    uint64_t expected_ver;     /* >0: trampoline редактирует message_id вместо новой отправки. */
    uint8_t delete_message;    /* 1: expected_ver/message_id задают удаление собственного сообщения с tombstone. */
    uint8_t  reply_to[32];      /* общий ID оригинала в этом канале; нули = не ответ */
    char reply_quote[DB_SYNC_MESSAGE_MAX + 1]; /* Снимок выделенного UTF-8 текста; пусто — обычный ответ. */
    uint64_t request_id;        /* ненулевой ID: trampoline/media отправляет CHAT_EVT_MSG_SUBMITTED */
    uint8_t attachment_job[16], attachment_object[16]; /* Внутренний READY intent общего сервиса; нули для обычного текста. */
};

/* Подписать,сохранить и разослать:0/-1,req/data остаются у caller.
 * channel/content_type — NUL-строки в пределах массивов,канал канонический;data<=4000 байт без NUL.
 * Итоговый экранированный JSON также должен помещаться в4000 байт. SQL JSON/bind/finalize проверяются до подписи/записи. */
int chat_core_submit_message(struct UTUN_INSTANCE* inst, struct chat_msg_submit* req);

struct chat_message_result {
    int status, sqlite_code; /* 0/SQLITE_OK после commit; -1=ошибка/отмена,-2=устаревшая версия редактора. */
    int committed;          /* Успешный commit, даже если остановка запретила позднее уведомление. */
    uint64_t timestamp, ver; /* Сохранённые время/версия;0 при отказе до commit. */
    uint8_t message_id[32]; /* Постоянный ID сохранённого сообщения;не новый ID при редактировании. */
};
/* Uasync, результат заимствован до возврата. Callback обязателен и приходит ровно один раз,
 * включая синхронный отказ аргументов/OOM/FULL/CLOSED; arg живёт до done/DB drain. */
typedef void (*chat_message_done_fn)(void* arg, const struct chat_message_result* result);
/* Uasync: скопировать req/data/ключи и сохранить сообщение одной WRITE; expected_ver>0 редактирует message_id.
 * Worker проверяет допуск/поколение и подписывает запись; события/рассылка следуют после commit.
 * Обычный текст/описание вложения, без копирования media_src. Query/data после возврата не нужны.
 * При committed=1 либо неоднозначном IOERR подготовленные файлы сохраняют. Stop отменяет queued работу
 * и позднюю публикацию; уже начатая WRITE может завершиться. READONLY/RAM используют общий legacy writer. */
void chat_core_submit_message_async(struct UTUN_INSTANCE* inst, const struct chat_msg_submit* req,
    chat_message_done_fn done, void* arg);

/* Правка собственного text: ожидаемая версия защищает открытый редактор от устаревшего сохранения.
 * 0 — сохранено/текст прежний, -1 — ошибка, -2 — версия изменилась. ID и порядок сообщения сохраняются. */
int chat_core_edit_message(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message_id[32],
        uint64_t expected_ver, const char* text, size_t len);

/* Подготовить файл в worker, затем сохранить и разослать сообщение; результат — CHAT_EVT_MSG_SUBMITTED при request_id!=0.
 * req и data копируются до возврата и остаются у вызывающего; файлы должны жить до завершения подготовки.
 * media_copy копирует src в dest, media_video запускает транскодирование. Ошибка также приходит событием.
 * Необязательный complete(arg,success) вызывается ровно один раз после сохранения/отказа и освобождения задачи,
 * в том числе синхронно при отказе запуска;arg принадлежит caller и живёт до callback. */
typedef void (*chat_media_submitted_fn)(void* arg, int success);
/* success1=сохранено и опубликовано в текущем запуске;0 также включает commit после stop без публикации.
 * Переданные файлы при success0 нельзя удалять без проверки сохранённого message binding. */
void chat_core_submit_media_message(struct UTUN_INSTANCE* inst, struct chat_msg_submit* req,
    chat_media_submitted_fn complete, void* arg);

/* Ручная загрузка по локальному номеру строки; внешняя SQL-транзакция запрещена.
 * Manifest ограничен общим DB_SYNC_MESSAGE_MAX. Зашифрованный индекс восстанавливается из истории до wanted/загрузки;
 * отсутствие plaintext сбрасывает ready с сохранением ключа. Отказ SQL логируется, состояние модуля атомарно сохраняется. */
void chat_core_attachment_download(struct UTUN_INSTANCE* inst, const char* channel_id, int64_t msg_id);

// ── DB-операции (оставлены для интроспекции) ──

// Число локальных сообщений; 0 также при отсутствии БД или ошибке запроса.
uint32_t chat_core_count(struct UTUN_INSTANCE* inst, const char* ch_id);

/* Список ID: count:u16, затем [длина:u8, строка]; buf_size >= 2. 0/-1.
 * Списки ограничены буфером/count<=65535;out_len — записанные байты,числа native.
 * SQL читается целиком даже после заполнения буфера;отказ сохраняет buf и обнуляет допустимый out_len.
 * Каналы идут по created_at,channel_id. */
int      chat_core_list_channels(struct UTUN_INSTANCE* inst, uint8_t* buf, size_t buf_size, size_t* out_len);

// Неудалённые участники:count:u16, затем ключи Ed25519 по 32 байта в лексикографическом порядке;канал канонический,остальной контракт как у list_channels.
int      chat_core_list_peers(struct UTUN_INSTANCE* inst, const char* ch_id, uint8_t* buf, size_t buf_size, size_t* out_len);

/* Ключи X25519/Ed25519 (по32 байта,NULL в БД→нули),count:u8,затем адреса:
 * family:u8,protocol:u8,address_len:u8,address[4/16],port:u16,rtt:i16;числа native,RTT насыщается.
 * buf_size>=65;не более255 целых первых адресов в детерминированном порядке БД.
 * 0 — прочитано,-1 — нет узла/ошибка,buf сохраняется,out_len=0. Проверяется весь SQL-снимок. */
int      chat_core_load_nodeinfo(struct UTUN_INSTANCE* inst, const uint8_t ed[32], uint8_t* buf, size_t buf_size, size_t* out_len);

// ── Создание канала (GUI → uasync) ──

struct chat_channel_create {
    struct UTUN_INSTANCE* inst; /* Заимствован trampoline; прямой вызов использует свой аргумент inst. */
    char     channel_id[65];    /* Публичный Ed25519 группы, 64 строчных HEX-символа. */
    char     group_type[8];    /* chat/kanban; пусто выбирает chat при создании. */
    char     name[128];         /* NUL-строка до127 байт, без молчаливого обрезания. */
    uint8_t owner_node_id[32];  /* Ed25519 устройства, создавшего группу. */
    uint8_t  x25519_pubkey[32]; /* Канальный ключ шифрования; должен соответствовать privkey. */
    uint8_t  x25519_privkey[32];/* Скаляр из ed25519_privkey. При создании проверяются связь секретов и обе публичные части. */
    uint8_t  ed25519_pubkey[32];/* Публичный ключ из raw seed. Для CHAT-группы бит знака равен нулю. */
    uint8_t  ed25519_privkey[32];/* 32-байтный seed Ed25519 для подписи. */
    uint8_t  signature[64];    /* Подпись JSON {name,ver:1} по group_info_message с привязкой к владельцу и ключам группы. */
};

struct chat_create_auto_arg { struct UTUN_INSTANCE* inst; char name[128]; char group_type[8]; };

/* Подготовить таблицы, группу и синхронизацию; 0/-1. Ошибка удаляет новые runtime-ресурсы,
 * проверенные данные БД сохраняются. Членства на других узлах не даёт. */
int chat_core_ensure_channel_ready(struct UTUN_INSTANCE* inst, const char* ch_id);

/* Создать новый канал:проверить пары ключей/подпись,одним commit сохранить описание,root JOIN/профиль/приглашение и Merkle.
 * 0 — сохранено,-1 — отказ без публикации;req заимствован и не меняется. Владелец — этот instance.
 * Внешняя транзакция и занятый ID запрещены. CHANNEL_UPDATED — после commit и попытки runtime-подготовки.
 * Отказ подготовки не отменяет созданный канал:можно повторить ensure_channel_ready. Неудачный rollback
 * с открытой транзакцией запрашивает stop/закрытие БД владельцем вне callback. */
int chat_core_create_channel(struct UTUN_INSTANCE* inst, const struct chat_channel_create* req);

// Забирает u_malloc-структуру chat_channel_create, очищает ключи и освобождает её после вызова.
void chat_core_create_channel_trampoline(void* arg);

/* Сгенерировать идентичность группы и создать канал. Возвращает локальный маршрутный индекс активной группы.
 * Ноль означает, что активного маршрута нет. Для результата сохранения и получения ключа используйте chat_channel_create_auto_submit().
 * name — NUL-строка<=127байт,не обрезается. Проверяет RNG/генерацию/извлечение/подписи;
 * временные приватные ключи очищаются. Внешняя транзакция отклоняется до генерации. */
uint64_t chat_core_create_channel_auto(struct UTUN_INSTANCE* inst, const char* name);
/* Создать группу протокола chat или kanban. Результат и условия те же. */
uint64_t group_core_create_auto(struct UTUN_INSTANCE* inst, const char* name, const char* type);

// Забирает и освобождает u_malloc-структуру chat_create_auto_arg; instance должен дожить до выполнения.
void chat_core_create_channel_auto_trampoline(void* arg);

// ── Локальное удаление канала (GUI → uasync) ──

struct chat_delete_channel_req {
    struct UTUN_INSTANCE* inst;
    char ch_id[65];
};

/* Удалить канал с этого устройства: отмена медиа-загрузок, удаление группы
 * (соединения/синк), db_sync инстанса, таблиц msg_ и peers_, медиафайлов и
 * merkle-хешей. Работает в uasync-потоке. Другие узлы канал не теряют.
 * Исходящие video/register задачи сначала завершаются; удаление продолжит последний completion.
 * Новые исходящие медиа этого канала в это время отклоняются. Внешняя SQL-транзакция запрещена до отмены ресурсов.
 * Ключи/запросы/outbox удаляются триггером атомарно с метаданными; при SQL-ошибке данные и файлы сохраняются,
 * сетевые ресурсы уже могут быть отменены — требуется повтор. Открытая после ошибки транзакция требует stop владельцем.
 * CHANNEL_DELETED приходит после commit метаданных и попытки файловой очистки. Её I/O-ошибка логируется отдельно;
 * остатки media_files сохраняются, собственный каталог plaintext можно очистить повторно после commit. */
void chat_core_delete_channel(struct UTUN_INSTANCE* inst, const char* ch_id);

void chat_core_delete_channel_trampoline(void* arg);

// ── Утилиты ──

/* Проверка канонического Ed25519 группы в HEX. Возвращает 1 для допустимого ключа, иначе 0. */
int chat_channel_valid(const char* text);
/* Десятичный ID личной беседы в диапазоне 1..INT64_MAX. Возвращает ID либо 0 при ошибке формата. */
uint64_t chat_numeric_id(const char* text);

/* Имя с NUL в пределах inst->name (пустое допустимо). RW:0 — запрос скопирован/принят,-1 — немедленный отказ;
 * поздний SQL отказ логируется/уведомляется. Для durable результата используйте chat_profile_name_submit.
 * Одна WRITE сохраняет local_identity,nodes,NODEINFO и полные member-блоки; только после commit меняются inst/LAN и сеть.
 * RAM/legacy сохраняются синхронно:0=committed/no-op,чужая транзакция запрещена, ошибочный rollback запрашивает stop.
 * Ожидающий JOIN без полного member пропускается. No-op сохраняет версии. Конфиг не меняется. */
int chat_core_update_my_name(struct UTUN_INSTANCE* inst, const char* name);

struct update_my_name_arg { struct UTUN_INSTANCE* inst; char name[128]; };
void chat_core_update_my_name_trampoline(void* arg);

// Обновить адреса topo_node и собственные member-записи каналов.
void chat_core_sync_my_addresses(struct UTUN_INSTANCE* inst);

/* При изменении своего userinfo в каналах увеличить update_ts, подписать и запустить sync.
 * Адреса обновляются отдельно через topo_node; подпись member-блока их не содержит. */
void chat_core_update_my_member(struct UTUN_INSTANCE* inst);

/* Синхронный legacy/RAM путь:0 — committed/no-op,отрицательная SQLite-ошибка — отказ.
 * Для работающего RW worker запрещён; сервисные вызовы используют chat_state_save.
 * key непустой,value допускает пустую строку.
 * Собственная транзакция проверяет все SQL-этапы;внешняя запрещена,no-op не вызывает commit hook.
 * Ошибка сохраняет прежнее значение;unrecovered rollback запрашивает owner stop/DB close. */
int chat_core_save_ui_state(struct UTUN_INSTANCE* inst, const char* key, const char* value);

/* Per-channel «play sound on new message» (ui_state: ch_<id>_sound_on_message, default 1=on).
 * get возвращает0/1,-1 при ошибке;отсутствие записи —1. set возвращает0/-1,
 * после собственного commit/no-op рассылает желаемое значение через chat_sync best effort.
 * SQL-отказ не рассылается,идентичный повтор повторяет доставку. ch_id — канонический ID канала.
 * Синхронный legacy/RAM API; сервисный READ/WRITE worker использует chat_sound_read/save из chat_state.h. */
int  chat_core_get_sound_on_message(struct UTUN_INSTANCE* inst, const char* ch_id);

int chat_core_set_sound_on_message(struct UTUN_INSTANCE* inst, const char* ch_id, int on);

struct chat_sound_arg { struct UTUN_INSTANCE* inst; char ch_id[65]; int on; };
void chat_core_set_sound_on_message_trampoline(void* arg);

/* Полный request принадлежит trampoline/u_free;data содержит key+NUL,value+NUL в пределах массива. */
struct save_ui_state_arg { struct UTUN_INSTANCE* inst; char data[1024]; };
void chat_core_save_ui_state_trampoline(void* arg);

/* Установить через chat_setting:0 — успех,-1 — неизвестное имя,-2 — валидация,-3 — LAN apply pending.
 * Только успешный group_autoconnect запускает пересмотр групп;остальные эффекты — в chat_setting.h. */
int chat_core_set_setting(struct UTUN_INSTANCE* inst, const char* name, const char* value);

/* Trampoline владеет полным request/u_free;data — name+NUL,value+NUL,обе строки проверяются в границах. */
struct chat_setting_arg { struct UTUN_INSTANCE* inst; char data[256]; };
void chat_core_set_setting_trampoline(void* arg);

// Запуск подключения к узлам выбранного канала (GUI → uasync)
void chat_core_connect_channel(struct UTUN_INSTANCE* inst, const char* ch_id);

void chat_core_connect_channel_trampoline(void* arg);

// Однократная попытка подключения к участнику по Ed25519 (GUI → uasync).
void chat_core_connect_node(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32]);

void chat_core_connect_node_trampoline(void* arg);

struct chat_connect_node_req {
    struct UTUN_INSTANCE* inst;
    char     ch_id[65];
    uint8_t ed[32];
};

/* Передать в uasync блок chat_msg_submit, выделенный u_calloc/u_malloc; trampoline освобождает его.
 * data отдельно не освобождается: удобно разместить содержимое в хвосте того же блока.
 * Непустой media_src выбирает подготовку вложения, иначе — обычную отправку. Результат — по request_id. */
void chat_core_submit_trampoline(void* arg);

// Трамплин для ручного запуска загрузки медиа (JNI → uasync)
struct attachment_dl_req {
    struct UTUN_INSTANCE* inst;
    char    channel_id[65];
    int64_t msg_id;
};
void chat_core_attachment_download_trampoline(void* arg);

/* Заменить local_attrs подписи/timestamp: FILE/RW0=принята copied WRITE,-1=синхронный отказ;
 * поздние SQL/COMMIT ошибки логируются, данные при отказе не меняются. RAM0=committed,-1=ошибка.
 * Канал канонический,timestamp1..INT64_MAX,подпись64 байта, JSON-object. Legacy чужая транзакция запрещена. */
int chat_core_update_local_attrs(struct UTUN_INSTANCE* inst, const char* ch_id, uint64_t ts,
                                  const uint8_t* author_sig,
                                  const char* local_attrs_json);

/* Добавить/заменить pl=1 через JSON,сохранив остальные local_attrs любой допустимой длины.
 * NULL/пустые attrs становятся объектом;неверный объект не перезаписывается. Строка определяется по message_id; FILE/RW copied WRITE с тем же lifecycle, что update_local_attrs. Ошибки логируются. */
void chat_core_mark_voice_played(struct UTUN_INSTANCE* inst, const char* channel_id, const uint8_t message_id[32]);

void chat_core_mark_voice_played_trampoline(void* arg);

struct voice_played_req {
    struct UTUN_INSTANCE* inst;
    char     channel_id[65];
    uint8_t message_id[32];
};

/* Сбор статуса (NTP + connections) в uasync. arg — заимствованный inst.
 * Ответ CHAT_EVT_STATUS_REFRESH: UTF-8 без завершающего NUL в длине; размер растёт по данным.
 * Текст заимствован только на время синхронного event callback; отказ памяти/форматирования логируется без события. */
void chat_core_collect_status_trampoline(void* arg);

// Лёгкий запрос нагрузки uasync для GUI. arg = inst, ответ CHAT_EVT_CPU_LOAD.
void chat_core_collect_cpu_load_trampoline(void* arg);

/* Список до250 ETCP-соединений:arg — заимствованный inst,ответ CONN_LIST [count:u16 native][entry:46]*.
 * Entry:peer:u64,flags:u8,links:u8,rtt:u16,inflight_KiB:u16,name:32;числа native,links/inflight насыщаются. */
void chat_core_collect_conn_list_trampoline(void* arg);

struct chat_node_arg { struct UTUN_INSTANCE* inst; uint64_t node_id; };

/* Полные метрики одного соединения, вызывается в uasync. arg — u_malloc struct chat_node_arg,
 * trampoline освобождает arg; inst остаётся заимствованным. CHAT_EVT_CONN_METRICS содержит UTF-8
 * без завершающего NUL в длине; включает очереди Router для пира/TX/RX-пути и transit, очереди normalizer.
 * Буфер растёт по числу линков и router-сессий, при OOM нет частичного события.
 * Данные ответа заимствованы только на время синхронного event callback. */
void chat_core_collect_conn_metrics_trampoline(void* arg);

/* Диагностика карточки участника: запрос/codec находятся в chat_member.h. */

// ── Headless CLI API ──

/* JSON-списки:0 — полный JSON с NUL,-1 — ошибка;out_len без NUL,при отказе0,допустимый buf очищается.
 * Синхронные helpers в потоке ядра; файловый headless использует SQL-снимки в READ worker.
 * GUI со своим SQLite reader не должен обращаться к runtime instance из UI-потока.
 * buf/out_len обязательны,buf_size>=3. SQL/память/переполнение не дают успешный префикс;
 * Строки экранируются целиком;channels/members отклоняют скрытый NUL в канальных/профильных SQLite TEXT.
 * Каналы:[{id,name,owner_id,owner_name,peers,msgs,online}];отложенная history-таблица означает msgs=0. */
int chat_core_get_channels_json(struct UTUN_INSTANCE* inst, char* buf, size_t buf_size, size_t* out_len);

/* Сообщения канала в JSON: [{id,ts,author_id,author_name,content_type,data,local_attrs}].
 * От новых к старым;count<=0 — до100 записей,offset>=0 — сколько пропустить.
 * Канал — канонический ID1..INT64_MAX;общий контракт буфера как у channels_json. */
int chat_core_get_messages_json(struct UTUN_INSTANCE* inst, const char* ch_id, int count, int offset,
                                 char* buf, size_t buf_size, size_t* out_len);

/* Мемберы в JSON: [{node_id,name,online,connected,adm_tags,x25519,ed25519,addrs}].
 * connected означает наличие транспорта в реестре,а не его UP/готовность группы;deleted показывается offline.
 * addrs.proto=UDP/TCP/TCP+REALITY,IPv4/IPv6 текстовые;канал/буфер как у messages/channels_json. */
int chat_core_get_members_json(struct UTUN_INSTANCE* inst, const char* ch_id, char* buf, size_t buf_size, size_t* out_len);

/* Имя с NUL,при необходимости обрезанное до sz-1 байт:0 — прочитано/узел отсутствует,-1 — ошибка.
 * FILE/RW читает только общий runtime node_registry (неизвестное имя пусто), RAM/READONLY — таблицу nodes.
 * out/sz>0 обязательны;отсутствие узла и ошибка оставляют пустую строку (если буфер допустим). */
int chat_core_get_node_name(struct UTUN_INSTANCE* inst, const uint8_t x[32], char* out, size_t sz);

/* Ссылка avatars/<sha256>.json внутри своего дерева;server=NULL или нулевой ключ удаляет ссылку,file — обязательная NUL-строка.
 * RW:0=скопировано/принято,-1=немедленный отказ; SQL/роль/подпись проверяет worker, поздний отказ идёт в лог/событие.
 * RAM/legacy:0=committed/no-op,-1=отказ без изменений,чужая транзакция запрещена. Storage-роль проверяется в WRITE. */
int chat_core_set_my_avatar(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t server[32], const char* file);

/* Построить {name:string},включая Unicode/control escaping;0/-1,при ошибке out не меняется. */
int chat_core_name_userinfo(const char* name, char* out, size_t capacity);

/* Прочитать собственный подписанный профиль:0/-1,при отказе все выходы сохраняются.
 * Проверяет SQL,размеры,JOIN/update-подписи и целый плоский JSON с уникальными строковыми полями.
 * out требует capacity>=длина+1,signature64,timestamp — версия update_ts в секундах/монотонных шагах. */
int chat_core_get_my_profile(struct UTUN_INSTANCE* inst, const char* channel, char* out, size_t capacity,
                              uint8_t signature[64], uint64_t* timestamp);

/* Одно подписанное обновление нескольких собственных строковых опций. NULL value удаляет ключ.
 * Остальные подписанные поля сохраняются;неподписанные старые атрибуты не авторизуются новым обновлением.
 * Имя/аватар меняются своими API. Чтение/проверка/версия/запись/Merkle защищены одним BEGIN IMMEDIATE.
 * Семантический no-op сохраняет подпись/версию независимо от порядка ключей/escaping;ошибка не меняет БД/Merkle.
 * Уведомления идут после commit,без SQLite cursor. Вызов в uasync вне чужой транзакции;
 * неудачный rollback с открытой транзакцией запрашивает stop/закрытие БД владельцем. */
struct chat_profile_option { const char* key; const char* value; };
int chat_core_set_my_profile_options(struct UTUN_INSTANCE* inst, const char* channel,
                                     const struct chat_profile_option* options, size_t count);

struct chat_avatar_req {
    struct UTUN_INSTANCE* inst;
    uint64_t id;
    uint8_t group[32]; /* Ed25519 группы. */
    uint8_t server[32]; /* Ed25519 сервера, нули для очистки ссылки. */
    char file[70];
};
void chat_core_set_my_avatar_trampoline(void* arg);

// ── Редактирование adm_tags (владелец канала) ──

#define CHAT_MEMBER_TAGS_MAX_KEYS 16
#define CHAT_MEMBER_TAGS_KEY_SZ   64

struct chat_member_tags;

/* Загрузить проверенный owner-снимок полного участника:NULL при отсутствии/placeholder/SQL/подписи/формате.
 * Объект принадлежит caller до free,используется в uasync и освобождается до остановки chat-сервиса. */
struct chat_member_tags* chat_member_tags_load(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t ed[32]);

/* Изменить желаемое поле:0/-1;отказ не меняет editor. val=NULL удаляет поле;admin/moder принимают yes/no.
 * Пустой ключ/ver запрещены,версию выбирает commit;место для ver резервируется в лимите16 ключей. */
int chat_member_tags_set(struct chat_member_tags* t, const char* key, const char* val);

// Получить значение ключа. Возвращает NULL если ключ не найден.
const char* chat_member_tags_get(struct chat_member_tags* t, const char* key);

/* Заменить желаемые теги:0/-1,при отказе editor не меняется. Весь объект<=8192 байта,уникальные
 * непустые ключи,только строки;admin/moder yes/no. Необязательный ver обязан совпадать с исходным снимком. */
int chat_member_tags_replace(struct chat_member_tags* t, const char* json);

/* Собственный BEGIN IMMEDIATE проверяет исходный owner-снимок и фиксирует новую подпись/версию/Merkle.
 * 0 — committed/no-op,-2 — конфликт с изменившимся owner-блоком,-1 — прочий отказ;чужая транзакция запрещена.
 * Отказ сохраняет editor/БД,успех обновляет editor для следующей правки. No-op сохраняет версию/подпись.
 * Версия1..INT_MAX;admin/moder сохраняют историю,отключение дописывает d<seconds>. Приватный ключ очищается.
 * Уведомления после commit без cursor;при неудачном rollback запрашивается stop/закрытие БД владельцем. */
int chat_member_tags_commit(struct UTUN_INSTANCE* inst, struct chat_member_tags* t);

/* SQL: обновить псевдоним текущей строки channel/Ed25519. ed — обязательный ключ участника.
 * nickname="" очищает ник. Возвращает SQLITE_OK, SQLITE_NOTFOUND при смене адресата либо ошибку SQLite. */
int chat_member_write_nickname(sqlite3* db, const char* channel, const uint8_t ed[32], const char* nickname);

/* uasync: атомарно сохранить локальный nickname и частичный owner_json для участника channel/Ed25519.
 * Строки заимствованы на время вызова; nickname="" снимает псевдоним, owner_json="" сохраняет только ник.
 * Непустой JSON использует правила tags_req ниже, включая проверку ver; нужен канальный приватный ключ для изменения прав.
 * 0 — commit и MEMBERS_CHANGED, -1 — отказ, -2 — конфликт owner-снимка. Чужая транзакция запрещена.
 * Ошибка не сохраняет частичный результат; SQL/подпись/rollback логируются, зависший rollback запрашивает stop. */
int chat_core_save_member(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32],
                          const char* nickname, const char* owner_json);

/* uasync: скопировать nickname и частичный owner_json, сохранить одной WRITE и вызвать done ровно один раз.
 * ed — обязательный Ed25519 участника. Отсутствующая строка отменяет всю WRITE.
 * nickname=NULL сохраняет псевдоним, "" удаляет; owner_json=NULL/"" не меняет права. Неуказанные JSON поля сохраняются.
 * done получает стандартный db_async_result: OK только после durable commit; stale ver отклоняется с SQLITE_BUSY.
 * Queued stop отменяет запись, поздний commit после stop не публикует runtime-события. arg заимствован до done. */
void chat_core_save_member_async(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32],
    const char* nickname, const char* owner_json, db_async_done_fn done, void* arg);
/* SQL/worker в WRITE caller: та же частичная правка без runtime/BEGIN/COMMIT.
 * context — frozen self/Ed25519/время, секреты очищает caller; nickname/owner_json заимствованы на вызов.
 * SQLITE_OK/положительный код, stale ver=>BUSY. changes — MS_APPLY flags только при успехе.
 * admission — собственный nullable delta: publish после COMMIT либо discard при любой ошибке/rollback.
 * Позволяет атомарно импортировать ключи группы и подписать свою роль той же WRITE. */
int chat_core_save_member_write(sqlite3* db, struct member_sync_write_context* context, const char* channel, const uint8_t ed[32],
    const char* nickname, const char* owner_json, struct group_policy_update** admission, int* changes);

// Освободить без сохранения.
void chat_member_tags_free(struct chat_member_tags* t);

/* Трамплин для gui_bridge:владеет req/u_free. json — частичный объект желаемых полей,неуказанные сохраняются;
 * необязательный ver проверяет снимок формы. Пустая строка — no-op;ошибки логируются без commit. */
struct chat_member_tags_req {
    struct UTUN_INSTANCE* inst;
    char     ch_id[65];
    uint8_t  ed[32];
    char     json[512];
};
void chat_member_tags_commit_trampoline(void* arg);

// ── Передача прав админа (владелец → другому узлу) ──

// Передать оба канальных privkey выбранному узлу (encrypted+signed через etcp_router).
// uasync-поток; NULL/неинициализированный inst отклоняется с ERROR. Постановка в транспорт не подтверждает импорт.
void chat_core_transfer_admin(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t target_ed[32]);

void chat_core_transfer_admin_trampoline(void* arg);

struct chat_transfer_admin_req {
    struct UTUN_INSTANCE* inst;
    char     ch_id[65];
    uint8_t target_ed[32]; /* Полный Ed25519 выбранного участника. */
};

#ifdef __cplusplus
}
#endif
#endif /* CHAT_CORE_H */
