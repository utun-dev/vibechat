/* attachment_send — подготовить вложение и передать его публикации в зафиксированный канал или DM.
 * Trampoline вызывается в uasync (обычно через uasync_post), забирает req и PCM/compressor, включая отказ запуска.
 * Если сам uasync_post не принят, request и его ресурсы остаются у вызывающего.
 * pcm_release освобождает pcm_owner, NULL означает PCM из u_malloc; release может быть в worker
 * или в uasync при отказе/отмене без work. Нельзя использовать GUI-объекты, требующие освобождения в UI-потоке.
 * Qt/Android используют общий результат подготовки, доставка остаётся в chat/dm.
 * req выделяется u_calloc; caller заполняет target/is_dm, info и source либо PCM/compressor, строки NUL-терминированы.
 * inst/uasync живут до SQL drain; config/media_async — до quiesce и завершения их workers.
 * После вызова req нельзя читать или освобождать.
 * Исходник не изменять во время подготовки; temporary_source разрешает удалить только собственный временный source.
 * Подготовка выполняется в media_async worker, публикация и attachment-события — в uasync.
 * Attachment-события 0/1/2 означают начало/готовность/ошибку подготовки, не подтверждение доставки сообщения.
 * Подготовленный output затем забирает chat/dm; собственные пути и PCM освобождаются также при отмене. */
#ifndef ATTACHMENT_SEND_H
#define ATTACHMENT_SEND_H
#include "attachment.h"
#include "../chat/db_sync.h"
struct UTUN_INSTANCE;
struct audio_compressor;
struct attachment_send_owned;
#ifdef __cplusplus
extern "C" {
#endif
struct attachment_send_req {
    struct UTUN_INSTANCE* inst;
    char target[65]; /* Публичный Ed25519 группы либо conv_id без префикса dm:, зафиксированный до подготовки. */
    int is_dm;
    char card[65]; /* Постоянный ID карточки KANBAN; пусто для CHAT/DM. Снимок цели до выбора/записи файла. */
    int encrypted; /* Отдельный случайный ключ группового вложения с ручной выдачей. */
    char content_type[32]; /* MIME для Kanban/зашифрованного канала; пусто → MIME по kind. */
    uint64_t dm_generation; /* Поколение беседы до подготовки; 0 → захват в trampoline. Удалённое поколение не публикуется. */
    unsigned dm_paths_held; /* Внутренние holds output/source; caller оставляет 0. */
    uint64_t publisher_epoch; /* Внутренний токен запуска CHAT/DM; заполняет trampoline до подготовки. */
    uint8_t reply_to[32]; /* Оригинал канального ответа, зафиксированный до подготовки. */
    char reply_quote[DB_SYNC_MESSAGE_MAX + 1]; /* Собственный UTF-8 снимок цитаты до подготовки. */
    uint64_t request_id;  /* CHAT_EVT_MSG_SUBMITTED после локального commit либо ошибки; 0=без подтверждения. */
    struct attachment_info info; /* kind обязателен; пустое name заменяется UUID.opus/mp4; для copy нужны готовые метаданные. */
    uint8_t id[16]; /* Внутренний UUID подготовки для attachment-событий; не UUID конечного media_index/DM. */
    char source[1024], output[1024]; /* source задаёт caller; output — внутреннее случайное назначение задачи. */
    int transcode, temporary_source; /* transcode только VIDEO: отдельный FFmpeg worker; иначе файл копируется. */
    const int16_t* pcm;
    size_t pcm_count; /* Для voice: int16, 48 кГц моно, число отсчётов. */
    void* pcm_owner; /* Аргумент собственного pcm_release. */
    void (*pcm_release)(void* owner); /* NULL → u_free(pcm); задача вызывает не более одного раза. */
    struct audio_compressor* compressor; /* Альтернатива PCM: задача flush/читает output/destroy, внешние обращения прекращены. */
    int preset, error; /* preset: 0..2 для voice; error заполняет worker. */
    struct attachment_send_owned* owned; /* Внутренний owner SQL/IO фаз; caller оставляет NULL. */
};
/* arg — собственный struct attachment_send_req; завершается событиями chat/dm и освобождением req. NULL — no-op. */
void attachment_send_trampoline(void* arg);
/* Отказ/отмена ДО trampoline: arg — собственный attachment_send_req; освободить PCM/compressor, temporary_source и req.
 * Не обращается к inst/БД; output/dm_paths_held ещё должны быть пусты. NULL допустим, события не отправляет. */
void attachment_send_discard(void* arg);
/* Uasync вне callbacks: запретить новые подготовки и снять retry timer до media/video drain.
 * SQL/IO запросы сохраняют собственный heap owner; late done не публикует GUI. Повтор допустим. */
void attachment_send_quiesce(struct UTUN_INSTANCE* inst);
/* После media/video workers, до CC free: отсоединить owner без ожидания общего DB worker.
 * Frozen release продолжает завершаться после chat stop; последний request освобождает owner.
 * После detach таймеров нет, fatal/refused release логируется и восстанавливается следующим startup prepare. */
void attachment_send_destroy(struct UTUN_INSTANCE* inst);
#ifdef __cplusplus
}
#endif
#endif
