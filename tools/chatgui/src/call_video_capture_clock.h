#pragma once
#include <cstdint>

// Первый callback может опоздать при запуске камеры. Его задержка не должна навсегда сдвигать PTS в будущее.
struct CallVideoCaptureClock {
    int64_t source = -1, offset = 0, correction = 0;
    int64_t map(int64_t next, int64_t arrival) {
        if (next < 0) { source = -1; return arrival; }
        if (source < 0 || next < source) offset = arrival - next;
        source = next;
        int64_t pts = next + offset;
        if (pts > arrival) {
            correction += pts - arrival;
            offset -= pts - arrival;
            pts = arrival;
        }
        return pts;
    }
};
