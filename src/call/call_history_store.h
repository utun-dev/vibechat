/* Ограниченная история диагностики; владелец сериализует все обращения своим mutex.
 * В hot path нет аллокаций. Гистограмма учитывает длительность каждого выданного
 * блока, а не число pull/пакетов. Возраст укрупняется до 50мс; окна интерфейса не владеют историей. */
#pragma once
#include "call_audio_history.h"
#include "../../lib/debug_config.h"
#include <algorithm>
#include <cmath>

class CallHistoryStore {
    struct RxBucket { call_history_rx point{}; unsigned bins[CALL_HISTORY_BINS]{}; };
    RxBucket rx[CALL_HISTORY_POINTS]{};
    call_history_tx tx[CALL_HISTORY_POINTS]{};
    unsigned rxNext = 0, txNext = 0, rxUsed = 0, txUsed = 0;
    call_history_interval interval{};
public:
    // samples/missing — PCM на выходной шкале; до первого звука samples=0. headroom — доступный запас, мс; при паузе 0.
    void receive(uint64_t now, int depth, int reserve, int tempo, unsigned samples, unsigned missing, int headroom) {
        unsigned index = (rxNext + CALL_HISTORY_POINTS - 1) % CALL_HISTORY_POINTS;
        if (rxUsed && now < rx[index].point.at_ms) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio history RX clock regressed"); return;
        }
        if (!rxUsed || now / CALL_HISTORY_STEP_MS != rx[index].point.at_ms / CALL_HISTORY_STEP_MS) {
            index = rxNext; rxNext = (rxNext + 1) % CALL_HISTORY_POINTS;
            rxUsed = std::min(rxUsed + 1, (unsigned)CALL_HISTORY_POINTS); rx[index] = {};
            rx[index].point.low_ms = rx[index].point.high_ms = depth;
            rx[index].point.headroom_low_ms = rx[index].point.headroom_high_ms = headroom;
        }
        auto& b = rx[index]; auto& p = b.point;
        if (!interval.pulls) {
            interval.low_ms = interval.high_ms = depth;
            interval.headroom_low_ms = interval.headroom_high_ms = headroom;
        }
        ++interval.pulls; interval.samples += samples; interval.missing_samples += std::min(samples, missing);
        interval.last_pull_ms = now;
        interval.low_ms = std::min(interval.low_ms, depth); interval.high_ms = std::max(interval.high_ms, depth);
        interval.headroom_low_ms = std::min(interval.headroom_low_ms, headroom);
        interval.headroom_high_ms = std::max(interval.headroom_high_ms, headroom);
        p.at_ms = now; p.depth_ms = depth; p.reserve_ms = reserve; p.tempo_x100 = tempo;
        p.low_ms = std::min(p.low_ms, depth); p.high_ms = std::max(p.high_ms, depth);
        p.headroom_low_ms = std::min(p.headroom_low_ms, headroom); p.headroom_high_ms = std::max(p.headroom_high_ms, headroom);
        p.rendered_samples += samples; p.missing_samples += std::min(samples, missing);
        b.bins[std::min(CALL_HISTORY_BINS - 1, std::max(0, depth) / CALL_HISTORY_BIN_MS)] += samples;
    }
    // Все значения принадлежат одному снимку uasync. Скорость фиксируется здесь: уход старых
    // точек из видимого окна не должен менять секундное среднее уже записанных измерений.
    void transmit(const call_history_tx& p) {
        unsigned index = (txNext + CALL_HISTORY_POINTS - 1) % CALL_HISTORY_POINTS;
        if (txUsed && p.at_ms < tx[index].at_ms) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio history TX clock regressed"); return;
        }
        if (!txUsed || p.at_ms / CALL_HISTORY_STEP_MS != tx[index].at_ms / CALL_HISTORY_STEP_MS) {
            index = txNext; txNext = (txNext + 1) % CALL_HISTORY_POINTS;
            txUsed = std::min(txUsed + 1, (unsigned)CALL_HISTORY_POINTS);
        }
        tx[index] = p;
        auto& current = tx[index]; current.wire_bytes_per_sec = -1;
        unsigned first = index;
        for (unsigned i = 1; i < txUsed && current.peer_id; ++i) {
            unsigned previous = (first + CALL_HISTORY_POINTS - 1) % CALL_HISTORY_POINTS;
            if (tx[previous].peer_id != current.peer_id || tx[previous].tx_epoch != current.tx_epoch
                || current.at_ms - tx[previous].at_ms > 1000 || tx[first].at_ms - tx[previous].at_ms > 150
                || tx[first].wire_tx_bytes < tx[previous].wire_tx_bytes) break;
            first = previous;
        }
        if (first != index && current.at_ms > tx[first].at_ms)
            current.wire_bytes_per_sec = (current.wire_tx_bytes - tx[first].wire_tx_bytes) * 1000.0 / (current.at_ms - tx[first].at_ms);
    }
    // Сброс только интервала логгера; кольца UI и их гистограмма остаются нетронутыми.
    void take_log(call_history_interval& out, call_history_tx& latest) {
        out = interval; interval = {}; interval.last_pull_ms = out.last_pull_ms;
        latest = txUsed ? tx[(txNext + CALL_HISTORY_POINTS - 1) % CALL_HISTORY_POINTS] : call_history_tx{};
    }
    // out — независимая копия. Пустые/старые участки не интерполируются и не становятся нулевыми измерениями.
    void snapshot(uint64_t now, call_audio_history& out) const {
        out = {}; out.now_ms = now;
        for (unsigned i = 0; i < rxUsed; ++i) {
            const auto& b = rx[(rxNext + CALL_HISTORY_POINTS - rxUsed + i) % CALL_HISTORY_POINTS];
            if (b.point.at_ms > now || now - b.point.at_ms >= CALL_HISTORY_MS) continue;
            out.rx[out.rx_count++] = b.point;
            double weight = std::exp2(-(double)(now - b.point.at_ms) / 10000.0) / 48.0;
            out.observed_ms += b.point.rendered_samples * weight;
            out.missing_ms += b.point.missing_samples * weight;
            for (unsigned k = 0; k < CALL_HISTORY_BINS; ++k) out.histogram[k] += b.bins[k] * weight;
        }
        if (out.observed_ms > 0) for (auto& value : out.histogram) value /= out.observed_ms;
        for (unsigned i = 0; i < txUsed; ++i) {
            const auto& p = tx[(txNext + CALL_HISTORY_POINTS - txUsed + i) % CALL_HISTORY_POINTS];
            if (p.at_ms <= now && now - p.at_ms < CALL_HISTORY_MS) out.tx[out.tx_count++] = p;
        }
    }
};
