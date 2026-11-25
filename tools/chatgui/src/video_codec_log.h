/* Диагностика codec: VideoCodecProbe задаёт вложенную scoped-пробу в текущем потоке.
 * Создать/уничтожить в одном потоке; копирование и перенос запрещены. Конструктор возвращает
 * общий FFmpeg callback после возможной замены Qt Multimedia; деструктор завершает одну вложенность.
 * CODEC_LOG внутри пробы переводит error/warn/info в debug; debug/trace не меняются.
 * Подробности FFmpeg-пробы и сводка повторов идут на trace категории video, вне пробы — error/warn.
 * Глобальные уровни не меняются, ошибки рабочего codec в соседнем потоке сохраняют свой уровень. */
#pragma once
#include "../../../src/video/video.h"
#include "debug_config.h"

class VideoCodecProbe {
public:
    VideoCodecProbe() { video_codec_probe_begin(); }
    ~VideoCodecProbe() { video_codec_probe_end(); }
    VideoCodecProbe(const VideoCodecProbe&) = delete;
    VideoCodecProbe& operator=(const VideoCodecProbe&) = delete;
};

/* Сохраняет сообщения FFmpeg error/fatal текущей попытки независимо от фильтров логирования.
 * Создать/уничтожить в одном потоке; text заимствован до уничтожения объекта. */
class VideoCodecErrors {
public:
    VideoCodecErrors() { video_ffmpeg_errors_begin(&m_errors); }
    ~VideoCodecErrors() { video_ffmpeg_errors_end(&m_errors); }
    VideoCodecErrors(const VideoCodecErrors&) = delete;
    VideoCodecErrors& operator=(const VideoCodecErrors&) = delete;
    const char* text() const { return m_errors.text; }
private:
    video_ffmpeg_errors m_errors;
};

// level/category — DEBUG_LEVEL_*/DEBUG_CATEGORY_*, далее printf-формат и аргументы;
// аргументы форматирования вычисляются только после фильтрации по итоговому уровню.
#define CODEC_LOG(level, category, ...) do { \
    const auto codecLevel = static_cast<debug_level_t>(video_codec_log_level(level)); \
    if (debug_should_output(codecLevel, category)) \
        debug_output(codecLevel, category, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__); \
} while (0)
