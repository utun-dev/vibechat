/* Политика роста периода desktop-аудио при повторных backend XRUN; только управляющий поток.
 * baseMs — исходный запрос (0, если не задан); после получения фактического периода вызвать begin.
 * observe получает ошибки за один опрос.
 * Increase означает запрос нового периода для внешнего reopen, сам объект backend не меняет.
 * Размер только растёт; для нового устройства/сессии сбросить объект, при reopen сохранить его. */
#pragma once

#include <algorithm>
#include <cstdint>

struct AudioBufferTuning {
    enum Result { Unchanged, Increase, Limit }; // Нет действия, обновлён requestedMs, дальнейший рост невозможен.
    unsigned requestedMs = 0, baseMs = 0, actualMs = 0, periods = 1; // Подобранный/исходный/фактический период в мс и число периодов.
    int64_t settleUntil = 0, changeAfter = 0, firstFaultAt = -1; // Конец прогрева/cooldown и первый сбой, монотонные мс; -1=нет сбоя.
    unsigned faultPolls = 0; // Число опросов с ошибками в текущем окне, ограничено двумя.

    /* now — монотонные мс; запоминает actualPeriodMs и max(1,periodCount), прогрев 1с, сброс серии сбоев.
     * Сохраняет requestedMs/baseMs и срок changeAfter от прежнего reopen. */
    void begin(int64_t now, unsigned actualPeriodMs, unsigned periodCount) {
        actualMs = actualPeriodMs; periods = std::max(1u, periodCount);
        settleUntil = now + 1000; firstFaultAt = -1; faultPolls = 0;
    }

    /* Нужны два опроса с ненулевым glitches за <=5с после прогрева; число ошибок внутри одного опроса неважно.
     * При достижении changeAfter повышает max(actual/requested/base) на 5мс до floor(100/periods), обновляя requestedMs.
     * Нулевой исходный период/достигнутый предел возвращают Limit; после Increase/Limit cooldown 5с.
     * Без нового сбоя один таймер/cooldown не вызывает действия; 100мс ограничивают запрос, не фактическую ёмкость backend. */
    Result observe(int64_t now, unsigned glitches) {
        if (!glitches || now < settleUntil) return Unchanged;
        if (firstFaultAt < 0 || now - firstFaultAt > 5000) { firstFaultAt = now; faultPolls = 0; }
        faultPolls = std::min(faultPolls + 1, 2u);
        if (faultPolls < 2 || now < changeAfter) return Unchanged;
        firstFaultAt = -1; faultPolls = 0; changeAfter = now + 5000;
        unsigned current = std::max({actualMs, requestedMs, baseMs}), maximum = 100 / periods;
        if (!current || current >= maximum) return Limit;
        requestedMs = std::min(current + 5, maximum);
        return Increase;
    }
};
