/* RGB888 для sws_scale: FFmpeg выделяет выровненные строки и запас для SIMD.
 * Доступен только при HAVE_FFMPEG. width/height — положительные размеры в пикселях;
 * ошибка allocation/геометрии даёт пустой QImage с ERROR(video), ресурсов у вызывающего нет.
 * Успех — QImage::Format_RGB888 с AVFrame stride (выравнивание 32), память не инициализирована:
 * перед показом заполнить видимые пиксели. Для sws_scale брать bits()/bytesPerLine()
 * свежего, ещё не разделяемого image. Qt detach создаёт свой буфер без этих гарантий SIMD.
 * Копии QImage разделяют буфер без копирования; последний владелец исходного буфера
 * освобождает AVFrame через cleanup callback. Освобождать bits() вручную нельзя.
 * Отдельные изображения можно создавать в разных потоках; совместную запись исключает вызывающий. */
#pragma once

#ifdef HAVE_FFMPEG
#include <QImage>
#include "debug_config.h"
extern "C" {
#include <libavutil/frame.h>
}

inline QImage video_frame_image(int width, int height) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video RGB frame allocation failed size=%dx%d", width, height); return {};
    }
    frame->format = AV_PIX_FMT_RGB24; frame->width = width; frame->height = height;
    int result = av_frame_get_buffer(frame, 32);
    if (result < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video RGB buffer allocation failed size=%dx%d result=%d", width, height, result);
        av_frame_free(&frame); return {};
    }
    QImage image(frame->data[0], width, height, frame->linesize[0], QImage::Format_RGB888, [](void* owner) {
        auto* frame = static_cast<AVFrame*>(owner); av_frame_free(&frame);
    }, frame);
    if (image.isNull()) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video RGB image initialization failed size=%dx%d stride=%d", width, height, frame->linesize[0]);
        av_frame_free(&frame); return {};
    }
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "video RGB buffer allocated size=%dx%d stride=%d alignment=32", width, height, frame->linesize[0]);
    return image;
}
#endif
