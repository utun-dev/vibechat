#include "video_plane.h"
#include "debug_config.h"
#include <string.h>

static int plane_valid(const struct video_plane *p, unsigned width, unsigned height) {
    if (!p || !p->data || !p->pixel_stride || p->pixel_stride > 4096 || !p->row_stride || p->row_stride > 16777216) return 0;
    size_t row = (width - 1u) * p->pixel_stride + 1u;
    return row <= p->row_stride && row <= p->size && height - 1u <= (p->size - row) / p->row_stride;
}

int video_plane_rotate(const struct video_plane *src, const struct video_plane *dst,
                       unsigned width, unsigned height, unsigned rotation) {
    unsigned dw = rotation % 180 ? height : width, dh = rotation % 180 ? width : height;
    if (!width || !height || width > 4096 || height > 4096 || rotation > 270 || rotation % 90
        || !plane_valid(src, width, height) || !plane_valid(dst, dw, dh)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "YUV plane invalid size=%ux%u rotation=%u src=%zu/%zu/%zu dst=%zu/%zu/%zu",
            width, height, rotation, src ? src->size : 0, src ? src->row_stride : 0, src ? src->pixel_stride : 0,
            dst ? dst->size : 0, dst ? dst->row_stride : 0, dst ? dst->pixel_stride : 0);
        return -1;
    }
    for (unsigned y = 0; y < dh; y++) {
        uint8_t *out = dst->data + y * dst->row_stride;
        if (!rotation && src->pixel_stride == 1 && dst->pixel_stride == 1) {
            memcpy(out, src->data + y * src->row_stride, width);
            continue;
        }
        size_t start;
        ptrdiff_t step;
        switch (rotation) {
            case 90: start = (height - 1u) * src->row_stride + y * src->pixel_stride; step = -(ptrdiff_t)src->row_stride; break;
            case 180: start = (height - 1u - y) * src->row_stride + (width - 1u) * src->pixel_stride;
                step = -(ptrdiff_t)src->pixel_stride; break;
            case 270: start = (width - 1u - y) * src->pixel_stride; step = (ptrdiff_t)src->row_stride; break;
            default: start = y * src->row_stride; step = (ptrdiff_t)src->pixel_stride; break;
        }
        const uint8_t *in = src->data + start;
        for (unsigned x = 0; x < dw; x++) out[x * dst->pixel_stride] = in[(ptrdiff_t)x * step];
    }
    return 0;
}
