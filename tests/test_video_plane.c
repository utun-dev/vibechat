#include "../lib/video_plane.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const uint8_t expected[4][8] = {{1,2,3,4,5,6,7,8}, {5,1,6,2,7,3,8,4},
        {8,7,6,5,4,3,2,1}, {4,8,3,7,2,6,1,5}};
    for (unsigned rotation = 0; rotation < 360; rotation += 90) {
        unsigned w = rotation % 180 ? 2 : 4, h = rotation % 180 ? 4 : 2;
        for (unsigned sp = 1; sp <= 2; sp++) for (unsigned dp = 1; dp <= 2; dp++) {
            uint8_t source[128], output[128], before[128], touched[128] = {0};
            memset(source, 0xcc, sizeof(source)); memset(output, 0xee, sizeof(output));
            struct video_plane src = {source + 7, 0, 4 * sp + 3, sp};
            struct video_plane dst = {output + 9, 0, w * dp + 5, dp};
            src.size = src.row_stride + 3 * sp + 1;
            dst.size = (h - 1) * dst.row_stride + (w - 1) * dp + 1;
            for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 4; x++) src.data[y * src.row_stride + x * sp] = 1 + y * 4 + x;
            assert(!video_plane_rotate(&src, &dst, 4, 2, rotation));
            for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
                unsigned at = 9 + y * dst.row_stride + x * dp;
                assert(output[at] == expected[rotation / 90][y * w + x]); touched[at] = 1;
            }
            for (unsigned i = 0; i < sizeof(output); i++) if (!touched[i]) assert(output[i] == 0xee);
            memcpy(before, output, sizeof(output));
            dst.size--; assert(video_plane_rotate(&src, &dst, 4, 2, rotation) < 0);
            assert(!memcmp(before, output, sizeof(output)));
            dst.size++; src.size--; assert(video_plane_rotate(&src, &dst, 4, 2, rotation) < 0);
            assert(!memcmp(before, output, sizeof(output)));
        }
    }
    uint8_t uv[] = {1,101,2,102,3,103,4,104}, rotated[8] = {0};
    struct video_plane src = {uv, sizeof(uv), 4, 2}, dst = {rotated, sizeof(rotated), 4, 2};
    assert(!video_plane_rotate(&src, &dst, 2, 2, 90));
    src.data++; src.size--; dst.data++; dst.size--;
    assert(!video_plane_rotate(&src, &dst, 2, 2, 90));
    static const uint8_t expected_uv[] = {3,103,1,101,4,104,2,102};
    assert(!memcmp(rotated, expected_uv, sizeof(rotated)));
    puts("YUV: all rotations, planar/interleaved strides, cropped starts, exact bounds and guards PASS");
    return 0;
}
