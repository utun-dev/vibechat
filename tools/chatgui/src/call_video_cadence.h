#pragma once
#include <cstdint>

// Выбор исходных кадров по фазе 1..60 fps, до encode. Дробный период не накапливает ошибку.
// Допуск четверти периода сохраняет кадры при дрожании PTS; опоздание сбрасывает фазу без burst.
class CallVideoCadence {
    int fps;
    int64_t lastPts = -1, next = -1;
public:
    explicit CallVideoCadence(int rate = 30) : fps(rate) {}
    bool accept(int64_t pts) {
        if (fps < 1 || fps > 60 || pts < 0 || pts <= lastPts || pts > (INT64_MAX - 1250000) / fps) return false;
        lastPts = pts;
        const int64_t scaled = pts * fps;
        if (next >= 0 && scaled < next - 250000) return false;
        next = next < 0 || scaled - next > 1000000 ? scaled + 1000000 : next + 1000000;
        return true;
    }
};
