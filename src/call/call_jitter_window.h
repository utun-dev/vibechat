#pragma once
#include "call_jitter.h"
#include <algorithm>
#include <cstdint>

// Десять секундных интервалов (текущий и девять предыдущих), без внутренней синхронизации.
// Владелец передаёт неубывающие неотрицательные now/received_ms. Отдельные экземпляры
// для глубины очереди и задержки доставки: возраст PCM сам по себе не меняет сетевой reserve.
struct call_jitter_window {
    struct bucket { int64_t second = -1, low = 0, high = 0; } buckets[10]; // -1 — интервал ещё не наблюдался.
    int64_t low = 0, high = 0; // Экстремумы depth/transit в действующих интервалах, мс.
    int reserve = CALL_JITTER_MIN_RESERVE_MS; // clamp(high-low, 20, max_reserve), мс.
    int max_reserve = 1000; // Владелец задаёт предел не ниже CALL_JITTER_MIN_RESERVE_MS.
    int64_t last_received_ms = -1, transit_ms = 0; // Последний приём и накопленная вариация задержки, мс.
    uint32_t last_sent_ms = 0; // Предыдущий wire timestamp; соседние интервалы должны укладываться в int32.
    // Добавить depth (мс, допускается отрицательный transit); true — начался другой секундный интервал.
    bool observe(int64_t now_ms, int64_t depth) {
        int64_t second = now_ms / 1000;
        auto& b = buckets[second % 10];
        bool changed_second = b.second != second;
        if (changed_second) { b.second = second; b.low = b.high = depth; }
        else { b.low = std::min(b.low, depth); b.high = std::max(b.high, depth); }
        low = high = depth;
        for (const auto& item : buckets) {
            if (item.second < 0 || item.second > second || second - item.second >= 10) continue;
            low = std::min(low, item.low); high = std::max(high, item.high);
        }
        reserve = (int)std::max<int64_t>(CALL_JITTER_MIN_RESERVE_MS, std::min<int64_t>(max_reserve, high - low));
        return changed_second;
    }

    // Разность интервалов прихода и отправки: смещение часов исключается, uint32 timestamp разворачивается.
    // Первый приём задаёт transit=0; возврат и обновлённые поля — как у observe().
    bool observe_arrival(int64_t received_ms, uint32_t sent_ms) {
        if (last_received_ms >= 0) transit_ms += received_ms - last_received_ms - (int32_t)(sent_ms - last_sent_ms);
        last_received_ms = received_ms; last_sent_ms = sent_ms;
        return observe(received_ms, transit_ms);
    }
};
