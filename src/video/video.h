/* Подготовка локальных видеофайлов через FFmpeg: метаданные, JPEG и MP4 H.264/AAC.
 * probe/get_info/thumbnail выполняют синхронное чтение; запускать вне GUI/uasync при медленном I/O.
 * transcode_start передаёт работу media_async; публикация готового файла и done — в потоке uasync.
 * Исходник должен оставаться неизменным до done. Назначение — новое имя или обычный файл, без ссылок.
 * Один исходный и выходной файл, в том числе через hardlink, запрещены. Ошибка/отмена сохраняет старый dst.
 * Подготовка пишет закрытый проверенный временный файл рядом с dst, затем заменяет конечное имя.
 * В процессе допускается одна задача до её completion; thumbnail/probe независимы от этого лимита. */
#ifndef VIDEO_H
#define VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct UASYNC;

/* Инициализирует FFmpeg и подключает общий лог; повторный вызов восстанавливает callback после загрузки Qt backend.
 * video=trace включает AMF и заголовки HEVC. В сборке без FFmpeg ничего не делает. */
void video_ffmpeg_init(void);

/* Сбор ошибок FFmpeg текущего потока для отображения причины отказа codec.
 * begin/end парные, допускают вложенность; сообщения получает только внутренний сборщик.
 * Уровни и обычный вывод логов не меняются; без FFmpeg текст остаётся пустым. */
struct video_ffmpeg_errors {
    char text[4096];                       /* Первые сообщения уровня error/fatal, разделённые переводами строк. */
    struct video_ffmpeg_errors* previous;   /* Предыдущий сборщик текущего потока; управляется begin/end. */
};
/* errors обязателен и живёт до end; begin очищает text, end оставляет его доступным вызывающему. */
void video_ffmpeg_errors_begin(struct video_ffmpeg_errors* errors);
void video_ffmpeg_errors_end(struct video_ffmpeg_errors* errors);

/* Проба доступности codec: begin/end парные, вложенные, только для текущего потока.
 * Ожидаемые отказы FFmpeg и сводка повторов выводятся на trace категории video;
 * рабочие потоки сохраняют error/warn. begin/end должны выполняться в одном потоке. */
void video_codec_probe_begin(void);
void video_codec_probe_end(void);
/* Возвращает debug вместо error/warn/info внутри пробы; level — DEBUG_LEVEL_* из debug_config.h. */
int video_codec_log_level(int level);

/* Целевой профиль: H.264/YUV420P в MP4, <=1280x720; video VBV=2 Mbit, AAC=128 kbit.
 * Bitrate — настройка кодера, не ограничение мгновенного размера отдельного кадра. */

#define VIDEO_TARGET_MAX_WIDTH   1280
#define VIDEO_TARGET_MAX_HEIGHT  720
#define VIDEO_TARGET_MAX_BITRATE 2000000      // 2 Mbit (только видео)
#define VIDEO_TARGET_AUDIO_BITRATE 128000     // AAC 128 kbit
#define VIDEO_PATH_MAX 1024                  // Размер буфера пути с завершающим NUL; длинные пути отклоняются.

/* ── информация о видеофайле ── */

struct video_info {
    int      is_video;       // 1 = открыт и есть видеопоток
    int      width;          // Ширина выбранного видеопотока в пикселях.
    int      height;         // Высота выбранного видеопотока в пикселях.
    int64_t  bitrate;        // Средний bitrate выбранного видеопотока, бит/с; 0 — неизвестен.
    int64_t  duration_ms;    // Длительность контейнера/видеопотока, мс; 0 — неизвестна.
    int      has_audio;      // Есть выбранный аудиопоток; отсутствие декодера не означает отсутствие аудио.
    int      is_yuv420p;     // Видео имеет 8-битный YUV420P; иначе требуется перекодирование.
    int64_t  audio_bitrate;  // Средний bitrate выбранного аудиопотока; 0 — неизвестен/нет аудио.
    char     audio_codec[32]; // Имя аудиокодека, например "aac"; пусто без аудио.
    char     codec[32];      // напр. "h264", "hevc", "mpeg4"
    char     container[32];  // короткое имя контейнера (ifmt->name)
};

/*
 * Синхронный пробинг: FFmpeg может читать/анализировать пакеты, ограничения времени нет.
 * Возвращает 1 при наличии видеопотока, кроме attached picture; иначе 0, включая сборку без FFmpeg.
 */
int video_probe(const char* path);

/* Синхронно прочитать метаданные выбранных потоков. path/out обязательны; 0 — успех, -1 — ошибка.
 * out обнуляется до работы, в том числе без FFmpeg; при ошибке содержимое не использовать. */
int video_get_info(const char* path, struct video_info* out);

/*
 * 0 — достаточно remux MOV/MP4/Matroska в MP4; 1 — нужно перекодирование выбранных потоков.
 * Проверяет известные bitrate, размеры, H.264/YUV420P и AAC при наличии аудио.
 * NULL, неизвестный профиль или сборка без FFmpeg — 1. Выход start всегда формируется как MP4.
 */
int video_needs_transcode(const struct video_info* info);

/*
 * Запуск транскодирования. Вызывается из uasync-потока.
 * owner/ua/src/dst/done обязательны; owner обслуживается только в этом uasync-потоке.
 * Worker либо перекодирует видео/аудио, либо выполняет remux без изменения сжатых кадров.
 * Прогресс обновляется атомарно, читается через video_transcode_get_progress().
 * При возврате 0 done(arg, err) вызывается ровно один раз в uasync-потоке,
 * в том числе при отмене owner. Callback может выполниться внутри start.
 * При возврате -1 callback не вызывается, arg остаётся у вызывающего.
 * done: err=0 — dst опубликован, info заимствован только на время callback; при ошибке info=NULL.
 * err=-1 — подготовка/запуск/публикация не удалась, MEDIA_ASYNC_CANCELLED (-2) — отмена owner.
 * destroy owner ждёт worker, затем отменяет публикацию и вызывает done; мгновенного прерывания кодека нет.
 * Выбирается по одному video/audio-потоку; остальные не переносятся. Существенные разрывы audio PTS
 * завершают подготовку ошибкой, без скрытого удаления/добавления звука. На POSIX новый файл имеет mode 0600.
 */
struct media_async;
int video_transcode_start(struct media_async* owner, struct UASYNC* ua,
                          const char* src, const char* dst,
                          void (*done)(void* arg, int err, const struct video_info* info), void* arg);

/* Глобальный прогресс 0..1000; 1000 — временный файл подготовлен, публикация ещё может ожидаться.
 * -1 — активной задачи нет. Читать можно из любого потока; active/progress не являются единым снимком. */
int video_transcode_get_progress(void);

/* 1 — задача занята работой/ожиданием completion; 0 — можно запускать следующую. */
int video_transcode_is_active(void);

/*
 * Извлечь превью: декодирует первый видеокадр, масштабирует до max_w по ширине
 * и сохраняет JPEG в jpg_out. Синхронно; параллельные вызовы используют разные назначения.
 * max_w<=0 означает 320; допустимый явный предел >=2, размеры округляются до чётных.
 * Правила файлов/длины путей — как у start; 0 — опубликован JPEG, -1 — ошибка/нет FFmpeg.
 */
int video_extract_thumbnail(const char* src, const char* jpg_out, int max_w);

#ifdef __cplusplus
}
#endif
#endif /* VIDEO_H */
