/* Capture worker переводит непрерывную позицию PCM 48кГц в монотонное время, плавно уточняя привязку по backend.
 * sequence — индекс PCM-фрейма на канал, observedUs — наблюдаемое время первого фрейма в микросекундах.
 * Сам не обнаруживает разрывы: владелец вызывает reset при потере sequence, смене route или скачке времени. */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

struct VoiceCaptureClock {
    bool initialized = false; // Получена первая пара sequence/observedUs.
    uint64_t originSequence = 0, previousSequence = 0; // Начальная и последняя позиции в непрерывном отрезке PCM.
    int64_t originUs = 0, errorUs = 0, adjustmentUs = 0; // Начальное время, ошибка перед коррекцией и последняя коррекция, мкс.
    double phaseUs = 0; // Накопленная плавная поправка к номинальной шкале 48кГц, мкс.

    /* Начинает новый независимый отрезок, очищая привязку и диагностические метрики. */
    void reset() { *this = {}; }

    /* sequence должен не убывать и не переполняться до следующего reset. Первый вызов возвращает observedUs.
     * Далее возвращает originUs + номинальное время PCM + фазовую поправку: фильтр с постоянной 2с,
     * изменение поправки не более 0.5% прошедшего PCM-времени (100мкс на 20мс); errorUs/adjustmentUs обновляются. */
    int64_t map(uint64_t sequence, int64_t observedUs) {
        if (!initialized) {
            initialized = true; originSequence = previousSequence = sequence; originUs = observedUs;
            return observedUs;
        }
        const double nominal = (sequence - originSequence) * (1000000.0 / 48000.0);
        const double elapsed = (sequence - previousSequence) * (1000000.0 / 48000.0);
        const double error = double(observedUs - originUs) - nominal - phaseUs;
        // Постоянная времени 2с; предел 0.5% (не более 100мкс коррекции на 20мс PCM).
        const double adjustment = std::clamp(error * (1.0 - std::exp(-elapsed / 2000000.0)), -elapsed / 200, elapsed / 200);
        phaseUs += adjustment;
        previousSequence = sequence;
        errorUs = std::llround(error); adjustmentUs = std::llround(adjustment);
        return originUs + std::llround(nominal + phaseUs);
    }
};
