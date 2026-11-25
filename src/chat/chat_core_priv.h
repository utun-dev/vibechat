/*
 * chat_core_priv.h — внутренний заголовок для под-модулей chat_core
 *
 * Предоставляет per-instance chat_core_ctx через CC(inst), общие SQL/сериализационные хелперы
 * и внутренние операции частей chat_core/member_sync. Не является frontend API.
 * Runtime — в uasync, явно помеченные SQL-only helpers допустимы в DB worker без контекста.
 * Контекст создаёт/освобождает chat_core. Сначала закрываются
 * приём команд, callbacks и media workers, затем освобождается контекст; БД остаётся ядру.
 */

#ifndef CHAT_CORE_PRIV_H
#define CHAT_CORE_PRIV_H

#include "chat_core.h"

#include "db_sync.h"
#include "../attachments/manifest.h"
#include "../../lib/debug_config.h"

#include <sqlite3.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define CC_ID "chat_core"
#define CHAT_MEDIA_BACKFILL_CHANNELS 16
#define CHAT_CHANNEL_DESCRIPTION_MAX (1024 + 128)

/* Base64url для префикса поля d:заимствованные непересекающиеся буферы.
 * encode возвращает записанные байты,дописывает '=' для неполной тройки,без NUL;
 * при нехватке места останавливается перед очередной четверкой.
 * decode пропускает чужие символы,останавливается на '=',принимает хвост из2/3 символов;
 * возвращает ограниченный dst_cap префикс,не валидирует каноничность или весь вход.
 * Строгие потребители отдельно сверяют длину и повторное кодирование (chat_attachment_identity). */

static const char B64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static inline int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static inline size_t b64_encode(const uint8_t* src, size_t src_len, char* dst, size_t dst_cap) {
    size_t out = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        if (out + 4 > dst_cap) break;
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < src_len) v |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < src_len) v |= (uint32_t)src[i + 2];
        dst[out++] = B64_ALPHABET[(v >> 18) & 0x3F];
        dst[out++] = B64_ALPHABET[(v >> 12) & 0x3F];
        dst[out++] = (i + 1 < src_len) ? B64_ALPHABET[(v >> 6) & 0x3F] : '=';
        dst[out++] = (i + 2 < src_len) ? B64_ALPHABET[v & 0x3F] : '=';
    }
    return out;
}

static inline size_t b64_decode(const char* src, size_t src_len, uint8_t* dst, size_t dst_cap) {
    size_t out = 0;
    int vals[4]; size_t vi = 0;
    for (size_t i = 0; i < src_len && out < dst_cap; i++) {
        if (src[i] == '=') break;
        int v = b64_decode_char(src[i]);
        if (v < 0) continue;
        vals[vi++] = v;
        if (vi == 4) {
            dst[out++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
            if (out < dst_cap) dst[out++] = (uint8_t)((vals[1] << 4) | (vals[2] >> 2));
            if (out < dst_cap)
                dst[out++] = (uint8_t)((vals[2] << 6) | vals[3]);
            vi = 0;
        }
    }
    if (vi == 3 && out < dst_cap) {
        dst[out++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
        if (out < dst_cap) dst[out++] = (uint8_t)((vals[1] << 4) | (vals[2] >> 2));
    } else if (vi == 2 && out < dst_cap) {
        dst[out++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
    }
    return out;
}

/* ── Состояние одного экземпляра (создаётся в chat_core.c) ── */

struct ms_props_cbk;    /* member_sync.c */
struct media_index_result;
struct group_avatar;
struct media_submit_ctx;
struct ak_file;
struct chat_whisper;
struct member_route_event;
struct member_read_job;

struct chat_devgroup;
struct chat_clipboard;
struct chat_invite_build;
struct chat_local_keys;
struct chat_core_ctx {
    struct chat_invite_build* invite_builds; /* Незавершённые приглашения: chat_join_destroy отменяет/отсоединяет до free cc. */
    uint64_t sync_generation; /* Новая регистрация chat_sync внутри cc; READ callbacks защищены от stop/reinit ABA. */
    char media_base[512]; /* Проверенный immutable media_base из SQL bootstrap; runtime путям не нужен SQL. */
    int readiness_published; /* DB_READY уже отправлено для этой регистрации cc. */
    struct chat_clipboard* clipboard; /* Текущая рассылка/приём буфера своих устройств через E2E router. */
    struct chat_devgroup* devgroup; /* Приглашения, retry и периодическое принятие профиля из nodes. */
    struct chat_local_keys* local_keys; /* До16 LAN key READ/cache slots; stop отсоединяет SQL jobs до free cc. */
    struct UTUN_INSTANCE* inst;         /* Заимствованный владелец;не уничтожается из callbacks чата. */
    sqlite3*              db;          /* Заимствована у ядра;chat_core её не открывает/не закрывает. */
    char                  db_path[512]; /* Копия пути init для идентичности запуска/default media base;не отдельное соединение БД. */
    uint8_t       initialized;         /* Метаданные committed,подписки установлены;не означает готовность всего chat_service. */
    struct chat_whisper* whisper;     /* Ленивый владелец модели/одной задачи; stop отсоединяет до ожидания workers. */
    struct member_route_event* member_route_events; /* Отложенные UI-снимки топологии по (канал,узел), без TOPO_* указателей. */
    struct member_read_job* member_reads; /* Принятые READ-снимки: SQL-данные собственные, stop отсоединяет runtime до free контекста. */
    struct ms_props_cbk* props_cbks;   /* Владение списком подписчиков свойств member_sync. */
    unsigned props_dispatch_depth;    /* снятие props-подписки откладывает free до выхода из вложенных callbacks */
    struct group_avatar* group_avatar; /* Контекст запросов/передач аватарки группы;освобождает group_avatar_destroy. */
    uint64_t              media_cache_bytes;   /* Производный размер уникальных файлов/расшифрованных копий;сверяется полным scan. */
    void*                 media_scan; /* Единственный owned SQL snapshot; completion очищает marker без borrowed cc. */
    unsigned              media_scan_pending; /* Объединённые флаги ещё одного полного scan, без неограниченной очереди. */
    uint64_t              media_scan_epoch; /* Меняется при content backfill init/destroy, включая restart при живом общем CC. */
    struct media_submit_ctx* media_submissions; /* исходящие задачи до последнего callback; удаление канала ждёт их */
    void*                 media_backfill_timer;/* Общий scan после debounce истории либо повтор при отказе/переполнении batch. */
    int                   media_backfill_ran;  /* Первый общий scan уже запущен;последующие завершения не переносят его. */
    struct UTUN_INSTANCE* media_backfill_inst; /* inst при установленных done-подписках,NULL после destroy;заимствован. */
    void*                 media_channel_backfill_timer; /* ограниченный batch докачки/восстановления индекса через 2с от первой заявки */
    char                  media_channel_backfill[CHAT_MEDIA_BACKFILL_CHANNELS][65]; /* pending ch_id */
    int                   media_channel_backfill_pending; /* Число занятых строк batch,0..16. */
    void*                 journal_effect_timer; /* Повтор долговечных заданий; отменяется до уничтожения cc. */
    int                   journal_effect_running; /* Один SQL batch эффектов на экземпляр. */
};

/* Заимствованный контекст или NULL;inst обязан быть ненулевым,макрос не проверяет lifecycle. */
#define CC(inst) ((inst)->chat_core)

/* ── Хелперы ── */

/* Внутренние имена таблиц:не валидатор ID. Ненулевые строки/буферы,out_sz>0;
 * sanitize сохраняет ASCII alnum/_,остальное заменяет '_',обрезает до out_sz-1 и ставит NUL.
 * Имена ограничивают очищенный ID 63 байтами; msg_table_name даёт локальное представление history_msg_, peers — peers_. */
void sanitize_ch_id(const char* ch_id, char* out, size_t out_sz);
void msg_table_name(const char* ch_id, char* buf, size_t sz);
void peers_table_name(const char* ch_id, char* buf, size_t sz);

/* SQL-снимок JSON: fields — собственные свойства объекта без скобок/online/connected.
 * Для канала channel заполнен, members содержит допущенные node_id; для участника — node/deleted.
 * READ заполняет только эти данные, renderer добавляет живые статусы в uasync. */
struct chat_json_row {
    char* fields;                /* Собственный JSON-фрагмент, освобождается rows_free. */
    char channel[65];             /* Канонический ID канала; пусто для строки участника. */
    uint8_t node[32];            /* Ed25519 участника; не используется для строки канала. */
    int deleted;                 /* Удалённый участник всегда offline. */
    uint8_t (*members)[32];      /* Собственные Ed25519 для подсчёта online в канале. */
    size_t member_count;
};
struct chat_json_rows {
    struct chat_json_row* rows;  /* Собственные строки; начать с {0}, освободить после done. */
    size_t count, allocated;    /* Заполненные строки и ёмкость массива. */
    size_t bytes;               /* Учёт сохранённых фрагментов/ID, предел 8 MiB на снимок. */
};
/* SQL-only, внутри READ: capacity=3..8 MiB ограничивает JSON; out изначально {0}.
 * SQLITE_OK/точный код ошибки; при ошибке весь out очищен, частичного результата нет. */
int chat_json_channels_read(sqlite3* db, size_t capacity, struct chat_json_rows* out);
int chat_json_members_read(sqlite3* db, const char* channel, size_t capacity, struct chat_json_rows* out);
int chat_json_messages_read(sqlite3* db, const char* channel, int count, int offset, char* buf, size_t capacity, size_t* length);
/* Только uasync: дополнить снимок текущими RAM-статусами; 0/-1, при ошибке buf/length пусты.
 * channel=NULL для списка каналов, канонический ID — для участников; снимок заимствован. */
int chat_json_rows_render(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_json_rows* rows,
    char* buf, size_t capacity, size_t* length);
/* Освободить весь SQL-снимок, в том числе незавершённый, и обнулить его. */
void chat_json_rows_free(struct chat_json_rows* rows);

/* Проверить весь JSON-object<=limit:уникальные ключи,только строковые значения,без скрытого NUL.
 * SQLite заимствована;0/-1 с логом,callback может быть NULL. Callback получает строки до следующего step,
 * строит только временный результат и возвращает0/-1;при поздней ошибке caller отбрасывает весь результат. */
int chat_json_strings(sqlite3* db, const char* text, size_t limit,
    int (*visit)(const char* key, const char* value, void* arg), void* arg);
/* Проверить NUL-терминированный текст на корректный UTF-8. Возвращает 1/0. */
int chat_utf8_valid(const char* text);

/* SQL/worker: тот же parser с точным положительным SQLite-кодом, сохранённым до finalize cleanup.
 * Callback отказ =>SQLITE_CONSTRAINT; промежуточные visitor данные отбрасываются caller при любом отказе. */
int chat_json_strings_read(sqlite3* db, const char* text, size_t limit,
    int (*visit)(const char* key, const char* value, void* arg), void* arg);

/* Построить исходный JSON {name,ver:1} и байты его подписи group_info_message.
 * Возвращает длину/-1;аргументы обязательны,при отказе out не меняется. Для создания канала по имени. */
int chat_channel_description_data(const char* channel, const char* name, const uint8_t owner[32],
    const uint8_t x25519[32], const uint8_t ed25519[32], uint8_t* out, size_t capacity);

/* ── db_sync registry (chat_core.c) ── */

/* Заимствованный зарегистрированный history instance канонического канала;NULL при отсутствии/неверном ID. */
struct DB_SYNC_INSTANCE* si_find(struct UTUN_INSTANCE* inst, const char* ch_id);

/* Uasync после регистрации файла: скопировать envelope request и filename, сохранить вместе с local_attrs.
 * crypto_id[16]/hash[32] оба NULL для обычного файла; для encrypted ключ автора привязывается в той же WRITE.
 * Callback как submit_message_async; файлы остаются при отказе/неоднозначном commit и чистятся отдельным sweep. */
void chat_core_submit_registered_media_async(struct UTUN_INSTANCE* inst, const struct chat_msg_submit* request,
    const char* filename, const uint8_t* crypto_id, const uint8_t* hash, chat_message_done_fn done, void* arg);

/* ── db_sync callback (chat_msg.c) ── */

/* db_sync callback после commit:производный attachment-индекс,feedback/UI,live download и last_msg_at.
 * data/message_id заимствованы до возврата;updated даёт MSG_UPDATED без звука/unread/повторной загрузки. */
void on_msg_inserted(struct DB_SYNC_INSTANCE* si, const uint8_t message_id[32], uint64_t record_ts,
        const char* data, size_t len, uint64_t author, const uint8_t author_ed[32], int initial_sync, int updated, void* arg);
/* Собственный результат обработки одной committed версии; SQL не использует runtime. */
struct chat_content_effect {
    struct db_sync_record record; /* Committed версия; data указывает на собственный json ниже. */
    char channel[65],json[DB_SYNC_MESSAGE_MAX+1],content_type[32],filename[256]; /* SQL-вход и результат разбора manifest. */
    char* body; /* Собственная копия manifest; освобождает cleanup batch. */
    int initial,updated,notify,fields,stale,error; /* Режим UI, состояние версии и точная SQLite-причина отложенного индекса. */
    int deleted; /* В committed проекции сохранён tombstone сообщения. */
};
/* Внутри WRITE: обработать точную версию и удалить задание при успехе; SQLITE_OK/положительный код. */
int chat_content_effect_write(sqlite3* db,struct chat_content_effect* effect);
/* После commit: UI/загрузка с проверкой поколения канала; память результата остаётся caller. */
void chat_content_effect_publish(struct UTUN_INSTANCE* inst,struct chat_content_effect* effect);
/* Uasync перед teardown cc: отменить таймеры, поздние SQL completion отсеивает поколение. */
void chat_log_effects_stop(struct UTUN_INSTANCE* inst);
/* Учесть также расшифрованные производные файлы и применить общую квоту кеша. */
void chat_media_cache_enforce(struct UTUN_INSTANCE* inst);
/* Пометить текущие исходящие задачи канала; 1 — удаление продолжит последний completion, 0 — задач нет. */
int chat_media_defer_channel_delete(struct UTUN_INSTANCE* inst, const char* channel);

/* ── применить настройку group_autoconnect ко всем CHAT-группам (chat_channel.c) ── */

void chat_core_apply_group_autoconnect(struct UTUN_INSTANCE* inst);

#endif /* CHAT_CORE_PRIV_H */
