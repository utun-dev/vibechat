/* Политика запуска/восстановления звонка. Используется call_jitter под его mutex.
 *
 * Владение: сеть добавляет Opus и измерения; единственный playback worker декодирует,
 * меняет состояние накопления и управляет CallDelay. Адаптер устройства независимо
 * поддерживает готовый PCM под размер callback. Callback только потребляет очередь.
 * TX-регулятор пачки независим от RX: отправка не ждёт готовности удалённого вывода.
 *
 * reserve_ms — защищённый нижний запас речи, без периода пачки и аппаратного FIFO.
 * Перед стартом нужен reserve + длительность пачки (не меньше запроса PCM): после
 * подготовки PCM остаётся запас. Период пачки входит в порог один раз; CallDelay
 * сравнивает с reserve нижнюю огибающую очереди, а не её пик после получения пачки.
 * Стартовые 200мс — исходная цель, не одноразовый порог и не максимальная задержка.
 * Цель растёт сразу, снижается по 20мс после подтверждённой устойчивости. Недостаток
 * измерений/остановленный выход не являются основанием снижать запас.
 * Измеренная вариация доставки получает один кадр (20мс) запаса на границу PCM.
 * CallDelay наблюдает минимум за период пачки + вариацию доставки, иначе пик
 * большой пачки ошибочно считался бы постоянным избытком. При дефиците цели
 * SoundTouch замедляет речь до0.98x, пополняя запас без изменения высоты голоса;
 * при избытке действует прежний догон. Изменение цели само по себе не создаёт PCM.
 *
 * PREBUFFER/RECOVER -> речевой PCM -> подготовленный FIFO адаптера -> реальный вывод.
 * До первого PCM ожидание отличается от underrun; тишина не считается готовой речью.
 * CallDelay исполняет reserve, не выбирает его. Задержки FIFO выводятся отдельно;
 * сумма задержек диагностическая, не общий регулятор всех очередей.
 * Константы ниже — параметры политики, проверяемые сценариями, не свойства протокола. */
#pragma once
#include "call_jitter.h"
#include <algorithm>
#include <cstdint>

struct call_playout_policy {
    int reserve_ms = CALL_JITTER_STARTUP_MS; // Нижний сетевой запас, уменьшается только при устойчивом воспроизведении.
    int64_t stable_since_ms = -1, last_lower_ms = -1;
    unsigned packets = 0; // Число новых пачек с последнего нарушения устойчивости.
    static constexpr int StableMs = 5000, LowerIntervalMs = 1000, StepMs = CALL_JITTER_FRAME_MS;
    static constexpr unsigned MinimumPackets = 20;

    // Одна оценка на пачку; observed_ms — размах изменения доставки, без постоянного RTT.
    void observe(int64_t now_ms, int observed_ms, int maximum_ms, bool playing) {
        int required = std::min(maximum_ms, std::max(CALL_JITTER_MIN_RESERVE_MS, observed_ms + StepMs));
        if (stable_since_ms < 0 || !playing || required > reserve_ms) {
            stable_since_ms = now_ms; packets = 0;
        }
        reserve_ms = std::max(reserve_ms, required);
        if (packets < MinimumPackets) ++packets;
        if (playing && packets >= MinimumPackets && now_ms - stable_since_ms >= StableMs
            && (last_lower_ms < 0 || now_ms - last_lower_ms >= LowerIntervalMs) && required < reserve_ms) {
            reserve_ms = std::max(required, reserve_ms - StepMs); last_lower_ms = now_ms;
        }
    }
    // Реальная нехватка/смена устройства отменяет уверенность; повторный старт не расходует старый малый резерв.
    void recover(int64_t now_ms, int maximum_ms) {
        reserve_ms = std::min(maximum_ms, std::max(CALL_JITTER_STARTUP_MS, reserve_ms + StepMs));
        stable_since_ms = now_ms; packets = 0; last_lower_ms = now_ms;
    }
};
