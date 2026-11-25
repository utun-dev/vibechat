/* Индекс медиапотока поверх общего RS-кольца: хранит только позиции/PTS, без копии кодированного потока.
 * Один uasync-поток; store принадлежит stream-session, cursor — конкретному локальному reader.
 * feed получает уже проверенные последовательные пачки; gap обозначает окончательно потерянный диапазон.
 * Независимые audio/video позиции читают оригинал из кольца. Рабочая память cursor — один видеокадр,
 * один Opus-пакет и два scratch медиаблока. Курсоры ничего не закрепляют против вытеснения.
 * read возвращает заимствованный packet до следующего read/destroy; frontend копирует только ближайшие записи.
 * Каждый cursor допускает новую video revision только с полным KEY; готовый KEY выдаётся без дополнительного ожидания.
 * Смена video revision не сбрасывает аудио и не запрещает другим readers дочитать прежнюю revision.
 * WAIT — материал ещё не готов/дальше audio lookahead. GAP/EXPIRED требуют новой точки KEY; END — весь материал выдан. */
#ifndef STREAM_MEDIA_BUFFER_H
#define STREAM_MEDIA_BUFFER_H

#include "stream_buffer.h"
#include "stream_media.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_MEDIA_INDEX_BYTES (4u * 1024u * 1024u)
#define STREAM_MEDIA_READ_AHEAD_US 80000
#define STREAM_MEDIA_PREROLL_US 500000
#define STREAM_MEDIA_TRACK_VIDEO 1
#define STREAM_MEDIA_TRACK_AUDIO 2

struct stream_media_store;
struct stream_media_cursor;
enum stream_media_read_kind { STREAM_MEDIA_READ_VIDEO, STREAM_MEDIA_READ_AUDIO, STREAM_MEDIA_READ_END };
struct stream_media_packet {
    enum stream_media_read_kind kind;  // Заполнен только соответствующий video/audio; END не несёт данных
    struct stream_media_format format; // Формат пачки выданного трека, включая его video revision
    struct stream_media_video video;   // Полный access unit, offset=0; data принадлежит cursor
    struct stream_media_audio audio;   // Один исходный Opus-пакет; data принадлежит cursor
};
struct stream_media_status {
    int64_t ready_us,                   // Общая готовая граница треков; на END учитывается ещё не дочитанный трек
        video_us,                      // PTS последнего полного видео; -1, если ещё нет
        audio_us,                      // Конец последнего аудио (PTS+20 мс); -1, если ещё нет
        start_us;                      // PTS стартового KEY данного cursor; -1 до инициализации
    uint64_t generation;               // Непрерывный участок cursor, отделённый GAP/разрывом; до старта 0
    int initialized,                   // Выбран полный сохранённый KEY
        audio_enabled,                 // В формате есть аудио и cursor запросил трек; stream_media_get_status также учитывает звук источника
        finished,                      // Store получил finish; ещё остаются непрочитанные данные
        video_ended, audio_ended;      // Соответствующий трек дочитан; отключённый аудиотрек уже ended
};
/* Снимок индекса одной эпохи: last_seq — последняя проиндексированная пачка,
 * [key_seq, key_last_seq] — последний полный сохранённый KEY. Нет KEY: key_valid=0, позиции UINT64_MAX. */
struct stream_media_recovery {
    uint64_t last_seq, key_seq, key_last_seq; // Все позиции UINT64_MAX, пока соответствующих данных нет
    int64_t video_us, key_us;                 // PTS последнего полного видео/сохранённого полного KEY; -1 до появления.
    int key_valid;                           // Обе границы полного KEY ещё представлены в индексе
};

/* out=NULL; buffer заимствован, descriptor скопирован. Индекс ограничен геометрией кольца и 4 МиБ.
 * Возвращает OK/ERR_*; при ошибке out не меняется. Buffer должен жить до прекращения обращений к store/cursors. */
int stream_media_store_create(struct stream_media_store **out, struct stream_buffer *buffer, const struct stream_descriptor *descriptor);
/* Освободить индекс после прекращения обращений cursors; их destroy не разыменовывает store. NULL допустим. */
void stream_media_store_destroy(struct stream_media_store *store);
/* Проиндексировать уже проверенную пачку seq != UINT64_MAX; данные не сохраняются.
 * Владелец подаёт пачки по порядку и отдельно обозначает пропуски. OK/DUPLICATE/ERR_*;
 * ошибка формата переводит store в failed, частично изменённый индекс дальше не используется. */
int stream_media_store_feed(struct stream_media_store *store, uint64_t seq, const uint8_t *data, size_t length);
/* Зафиксировать окончательный пропуск [first,last], last != UINT64_MAX, и начать новый участок с KEY. */
void stream_media_store_gap(struct stream_media_store *store, uint64_t first, uint64_t last);
/* Обозначить конец ввода; уже сохранённые полные кадры остаются доступны для чтения. */
void stream_media_store_finish(struct stream_media_store *store);
/* Удалить метаданные пачек ниже границы вытеснения RS; автоматически вызывается при feed/start/recovery. */
void stream_media_store_trim(struct stream_media_store *store);
/* Снимок не читает/не копирует кадры и не закрепляет кольцо; STREAM_OK либо ошибка аргументов/store. */
int stream_media_store_recovery(struct stream_media_store *store, struct stream_media_recovery *position);
/* out=NULL; tracks=VIDEO либо VIDEO|AUDIO. OK/ERR_*; store заимствован, независимые cursor не удерживают окно. */
int stream_media_cursor_create(struct stream_media_cursor **out, struct stream_media_store *store, unsigned tracks);
/* Освободить рабочие буферы; NULL допустим, store/buffer не освобождаются. */
void stream_media_cursor_destroy(struct stream_media_cursor *cursor);
/* Сбросить позиции и scratch после GAP/EXPIRED; следующий start выберет последний полный KEY. */
void stream_media_cursor_reset(struct stream_media_cursor *cursor);
/* Скопировать границы участка cursor; при необходимости выбрать стартовый KEY. OK даже до появления KEY,
 * тогда initialized=0 и времена=-1. При ERR_* выход не меняется. */
int stream_media_cursor_status(struct stream_media_cursor *cursor, struct stream_media_status *status);
/* want_video=1 запрашивает следующий кадр независимо от часов показа; 0 оставляет видео в кольце.
 * position_us<0 прогревает аудио перед KEY; затем звук выдаётся до position_us+audio_ahead_us, -1 отключает звук.
 * Если звук начинается позже KEY, lookahead отсчитывается минимум от первого аудиопакета:
 * decoder получает стартовый PCM при неподвижных часах; размер разрешённого окна остаётся ограниченным.
 * При GAP/EXPIRED cursor сбрасывают отдельно: это позволяет frontend сначала обработать разрыв.
 * audio_ahead_us допустим в [-1,60000000]. Бюджет одного вызова ограничен; MORE требует отложенного продолжения,
 * WAIT — новых данных/позиции. OK выдаёт один packet, включая END; остальные результаты не меняют packet. */
int stream_media_cursor_read(struct stream_media_cursor *cursor, int64_t position_us, int want_video, int64_t audio_ahead_us,
                             struct stream_media_packet *packet);

#ifdef __cplusplus
}
#endif
#endif
