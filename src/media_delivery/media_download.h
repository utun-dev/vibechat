/* media_download — загрузка канального файла по media_index_result: QUERY держателей → блоки → проверка/сборка.
 * Состояние хранит inst->md. При открытии подключения через conn_mgr загрузчик получает CM handle.
 * Найденный ETCP_CONN используется без нового handle; при отсутствии conn_mgr запрос отправляется best-effort без handle.
 * Старт копирует метаданные result; callbacks, обработчики и файловые операции выполняются в uasync-потоке.
 * FILE/RW: перед сетью worker фиксирует preparation, после durable файла — индекс. SQL не использует legacy.
 * В SQL-фазе dl удерживается ссылкой до callback; cancel снимает active, поздний результат не возобновляет загрузку.
 * QUERY: одна пачка до 32 блоков в полёте, отдельный UUID/дедлайн, источники только своей группы и автор.
 * До четырёх кандидатов в ответе QUERY; до десяти одновременно используемых узлов, слоты переиспользуются.
 * Кандидаты копятся из локального каталога и сети; OVERLOADED откладывает конкретного держателя.
 * Попытка имеет UUID и отдельный temp; принимается только непрерывный префикс от текущего узла своей группы.
 * Прогресс обновляется после успешного close; DONE проверяет размер и подпись по сохранённому ключу автора.
 * Проверенный temp независимо удерживают relay-выдачи. Итоговый файл публикуется после полного SHA-256.
 * done сообщает результат, а не гарантирует доставку другим участникам. */
#ifndef MEDIA_DOWNLOAD_H
#define MEDIA_DOWNLOAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "../lib/ll_queue.h"
#include "media_delivery_proto.h"

struct UTUN_INSTANCE;
struct media_index_result;
struct CONN_MGR_HANDLE;
struct media_block_source;
struct media_destination;

/* максимальное число узлов-держателей одного блока (кандидатов на фейловер) */
#define MD_MAX_HOLDERS_PER_BLOCK 64
/* Максимальное число одновременно используемых держателей; свободные слоты переиспользуются. */
#define MD_MAX_PEERS 10

enum md_wait_reason { MD_WAIT_NONE, MD_WAIT_SEARCH, MD_WAIT_SLOT, MD_WAIT_RETRY };

/* ── состояние блока в конечном автомате загрузки ── */
enum md_block_state {
    MD_BLK_IDLE = 0,   /* ещё не запрошен */
    MD_BLK_CONN,       /* conn_mgr_open в процессе, BLOCK_REQ ещё не отправлен */
    MD_BLK_REQ,        /* BLOCK_REQ отправлен, ждём первый чанк */
    MD_BLK_RECV,       /* чанки идут */
    MD_BLK_WAIT,       /* Причина в wait_reason: поиск, занятые слоты или retry_at_tb. */
    MD_BLK_DONE,       /* BLOCK_DONE + сигнатура валидна */
    MD_BLK_FAILED      /* попытки исчерпаны */
};

/* ── состояние одного блока ── */
struct media_download_block {
    uint8_t  block_id[16];
    uint8_t  transfer_id[16];    /* Текущая попытка, меняется до отправки BLOCK_REQ. */
    struct media_block_source* source; // Собственная producer-ссылка на файл этой попытки.
    uint8_t holders[MD_MAX_HOLDERS_PER_BLOCK][32];  /* Ed25519 держателей блока. */
    int      num_holders;
    int      holder_idx;        /* текущий держатель (-1 = нет) */
    uint64_t failed_holders;    /* Кандидаты, уже исчерпанные этой загрузкой. */
    uint64_t holder_retry[MD_MAX_HOLDERS_PER_BLOCK]; /* Дедлайн OVERLOADED отдельно для каждого держателя. */
    uint8_t  holder_busy[MD_MAX_HOLDERS_PER_BLOCK]; /* Ограниченные повторы от перегруженного держателя. */
    int      attempts;          /* 0..max (фейловеры/провалы блока) */
    uint32_t bytes_received;    /* Непрерывный успешно записанный и закрытый префикс. */
    uint32_t expected_size;     /* реальный размер блока (последний короче) */
    uint8_t  state;             /* enum md_block_state */
    uint64_t last_progress_tb;  /* get_time_tb() последнего чанка/прогресса */
    uint64_t retry_at_tb;       /* Не запрашивать блок до этого срока после OVERLOADED. */
    int      query_next;        /* Следующий источник поиска; QUERY не сбрасывает attempts. */
    uint8_t  author_tried;      /* Прямой fallback автора уже запускался. */
    uint8_t  wait_reason;       /* enum md_wait_reason; нехватка слота не расходует attempts. */
};

/* ── соединение с узлом-держателем ── */
struct media_download_peer {
    uint8_t ed[32];
    uint8_t connected;
    struct CONN_MGR_HANDLE* cm_handle; /* Один запрос на держателя; закрывается после DOWN/TIMEOUT или завершения загрузки. */
    uint8_t  pending_event;     /* CM event + 1; обработка отложена, чтобы open не вызывал рекурсивный pump. */
};

/* Запрос к общей загрузке. Отмена отмечается до завершения текущего callback прогресса. */
struct md_download_listener {
    struct md_download_listener* next;
    void (*done)(void*, int);
    void* arg;
    void (*progress)(void*, int, int);
    void* progress_arg;
    int cancelled;
};

#define MEDIA_DOWNLOAD_KEY_SIZE (sizeof(uint64_t) + 16)
/* ── внутреннее состояние загрузки (доступно для тестов) ── */
struct media_download {
    struct ll_entry* entry;     /* Владеющая запись; после удаления из индекса живёт до последнего refs. */
    unsigned refs;             /* Активная загрузка и обработчики, вызывающие пользовательские callbacks. */
    struct media_destination* destination; // Исключительное право записи dest_path до terminal.
    uint8_t author_pubkey[32];  /* Точный ключ автора из БД, скопирован до начала загрузки. */
    uint64_t group_id;
    uint8_t  media_id[16];      /* Сразу после group_id, вместе образуют ключ индекса. */
    char     dest_path[1024];
    char     media_base[512];
    int      num_blocks;
    uint8_t* block_sigs;        /* num_blocks * 64 (Ed25519 подписи блоков) */
    uint8_t  content_hash[32];
    int64_t  file_size;
    int64_t  block_size;
    struct   media_download_block* blocks;  /* num_blocks */
    int      blocks_validated;
    int      inflight;          /* блоков в CONN/REQ/RECV */
    uint8_t  active;
    uint8_t  assembled;
    uint64_t db_generation;     /* Captured runtime групп; SQL refs удерживают dl до callback, даже после cancel/destroy. */
    int      err;
    uint8_t  super_nodes[10][32];
    int      super_count;
    uint8_t  announce_peer[32];     /* Адресат HAVE_BLOCK, независимый от текущего QUERY. */
    uint8_t  query_id[16];      /* UUID ожидающей попытки; поздние ответы игнорируются. */
    uint8_t  query_peer[32];
    uint64_t query_deadline_tb; /* 0 — нет ожидающего QUERY; чанки не меняют дедлайн. */
    int      query_count;
    int      query_blocks[MEDIA_QUERY_MAX_BLOCKS]; /* Индексы пачки в blocks, в порядке QUERY. */
    void*    pump_timer;        /* Отложенная обработка CM; отменяется до освобождения dl. */
    void*    watchdog_timer;    /* Ближайший QUERY/простой блока/retry_at; отменяется до done_cb. */
    int      num_peers;
    struct   media_download_peer peers[MD_MAX_PEERS];
    struct md_download_listener* listeners; /* Запросы, ожидающие эту загрузку. */
    int notifying;              /* Callback может отменить запрос или выгрузить всю загрузку. */
    struct UTUN_INSTANCE* inst;
};

/* Запустить непустой файл с корректной геометрией и массивами block_ids/block_sigs; блок до MEDIA_BLOCK_SIZE_MAX.
 * author_pubkey — 32 байта подтверждённого Ed25519 участника этой группы.
 * dest_path <1024, media_base <512 байт (UTF-8); назначение принадлежит общей базе БД, смена базы отклоняется.
 * Проверки идут до создания каталогов. 0 — принят: done_cb ровно один раз, в том числе синхронно при отсутствии суперузлов;
 * Совпадающий объект, манифест и путь присоединяют callback к текущей загрузке.
 * -1 — отказ без done_cb. Аргументы callbacks заимствованы до done_cb.
 * Успех включает атомарную регистрацию блоков опубликованного файла в SQLite; общей транзакции ФС/БД нет.
 * Ошибка регистрации не удаляет опубликованный файл;
 * путь остаётся в media_preparations для удаления канала. Остальные поля result копируются до возврата.
 * progress_cb может отменить загрузку и запустить новую с тем же media_id; done_cb старой загрузки
 * вызывается после возврата из progress_cb, когда её последний обработчик освободил контекст. */
int  media_download_start(struct UTUN_INSTANCE* inst, uint64_t group_id,
                          const struct media_index_result* result,
                          const char* dest_path, const char* media_base,
                          const uint8_t author_pubkey[32],
                          void (*done_cb)(void* arg, int err), void* done_arg,
                          void (*progress_cb)(void* arg, int blocks_done, int num_blocks), void* progress_arg);

/* Отменить всю загрузку media_id: block_id/chunk не используются. Отдельный CANCEL отправляется для каждой текущей попытки.
 * 0 — отменена, -1 — не найдена. done_cb(-2) после последнего удерживающего обработчика; повторной отмены/done нет. */
int  media_download_cancel(struct UTUN_INSTANCE* inst,
                           const uint8_t* media_id, const uint8_t* block_id, uint32_t chunk);
/* Отмена всей загрузки в конкретной группе либо одного запроса по done_arg.
 * Для отдельного запроса done_cb получает -2; общий transfer живёт, пока есть запросы. */
int media_download_cancel_group_object(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t media_id[16]);
int media_download_unsubscribe(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t media_id[16], void* done_arg);

/* Router закрыл group/peer: отменить его CM, отложить failover активных попыток; callbacks загрузки здесь не вызываются. */
void media_download_peer_closed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]);

/* ── handlers for incoming data, called from md_etcp_recv dispatch ── */
/* Полный ответ пачки. group_id/from_ed — из router; несовпадающие/истёкшие попытки игнорируются.
 * Повреждённый пакет не меняет состояние; ошибка поиска продолжает обход источников.
 * Может завершить загрузку и вызвать done_cb; после возврата прежний dl может быть освобождён. */
void media_download_handle_query_resp(struct UTUN_INSTANCE* inst,
                                      uint64_t group_id, const uint8_t from_ed[32], const uint8_t* data, size_t len);
/* group_id/from_ed берутся из router. Длина/состояние/блок/chunk/UUID проверяются до записи и прогресса.
 * Повреждение текущей попытки меняет держателя, локальная ошибка завершает загрузку.
 * Чужие и устаревшие пакеты игнорируются; обработчики могут завершить загрузку синхронно. */
void media_download_handle_chunk(struct UTUN_INSTANCE* inst,
                                 uint64_t group_id, const uint8_t from_ed[32], const uint8_t* data, size_t len);
void media_download_handle_done(struct UTUN_INSTANCE* inst,
                                uint64_t group_id, const uint8_t from_ed[32], const uint8_t* data, size_t len);
void media_download_handle_overloaded(struct UTUN_INSTANCE* inst,
                                      uint64_t group_id, const uint8_t from_ed[32], const uint8_t* data, size_t len);
void media_download_handle_relay_full(struct UTUN_INSTANCE* inst,
                                      uint64_t group_id, const uint8_t from_ed[32], const uint8_t* data, size_t len);

/* ── conn_mgr callback (non-static for test visibility) ── */
struct CONN_MGR_HANDLE;
enum conn_mgr_event;
void md_dl_conn_cb(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t group_id,
                    enum conn_mgr_event event, void* arg);

/* ── тестовые хуки (для детерминированных юнит-тестов с «фейковым» временем) ──
 *  md_dl_check_stall  — обработать QUERY/простой/ожидание, запустить готовые блоки; 0 / -1 (нужен finish).
 *  md_dl_failover_block — переход блока к следующему держателю: 0, включая DONE/FAILED и ожидание новых держателей;
 *                         -1 — исчерпаны попытки или локальная ошибка; занятые peer-слоты возвращают 0.
 *  md_dl_dump         — дамп состояния загрузки в лог (DEBUG_CATEGORY_MEDIA). */
int  md_dl_check_stall(struct media_download* dl, uint64_t now_tb);
int  md_dl_failover_block(struct media_download* dl, int bi, uint64_t now_tb);
void md_dl_dump(struct media_download* dl);

#ifdef __cplusplus
}
#endif
#endif
