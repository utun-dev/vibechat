/* Копирование одной плоскости YUV с поворотом по часовой стрелке, без выделений.
 * Вызывается отдельно для Y/U/V; размеры chroma уже уменьшены вызывающим.
 * Буферы источника и назначения не должны пересекаться. */
#ifndef VIDEO_PLANE_H
#define VIDEO_PLANE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* data — первый пиксель, size — доступные байты от него.
 * row_stride — шаг строки 1..16777216 байт, pixel_stride — шаг пикселя 1..4096 байт. */
struct video_plane {
    uint8_t *data;
    size_t size, row_stride, pixel_stride;
};

/* src_width/height — размеры источника, каждый 1..4096; rotation — 0/90/180/270 градусов.
 * При 90/270 размеры назначения переставлены. Возвращает 0 либо -1 с диагностикой;
 * все границы проверяются до первой записи. */
int video_plane_rotate(const struct video_plane *src, const struct video_plane *dst,
                       unsigned src_width, unsigned src_height, unsigned rotation);
#ifdef __cplusplus
}
#endif
#endif
