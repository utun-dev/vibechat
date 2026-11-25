package com.utun.chat.data

/**
 * Адаптация размера аппаратного буфера, отдельно от сетевого JB; не выполняет I/O и не логирует.
 * Владелец вызывает reset после открытия выхода, затем sample примерно раз в 200 мс;
 * предложенный размер применяет к устройству и подтверждает фактическое значение через applied.
 * Все вызовы сериализованы одним lock. nowMs — монотонные мс, размеры/played — PCM-сэмплы mono.
 * После 2 с стабилизации underrun увеличивает размер на blockFrames до capacity; устойчивый
 * ход head позволяет пробовать уменьшение через 30 с. Каждый underrun удваивает интервал до 5 мин.
 */
internal class CallPlaybackBuffer(val blockFrames: Int = 960) {
    /** Запрошенный размер, причина для лога и прирост underruns на текущем наблюдении. */
    data class Change(val frames: Int, val reason: String, val underruns: Int = 0)

    private var actual = 0
    private var capacity = 0
    private var floor = blockFrames
    private var previousUnderruns = 0
    private var previousPlayed = -1L
    private var previousMs = 0L
    private var stableSinceMs = 0L
    private var settleUntilMs = 0L
    private var probeIntervalMs = 30_000L

    /** Новое устройство: frames > 0, capacityFrames >= frames, underruns >= 0; начать 2 с стабилизации. */
    fun reset(nowMs: Long, frames: Int, capacityFrames: Int, underruns: Int) {
        require(frames > 0 && capacityFrames >= frames && underruns >= 0)
        actual = frames
        capacity = capacityFrames
        floor = blockFrames
        previousUnderruns = underruns
        previousPlayed = -1
        previousMs = nowMs
        probeIntervalMs = 30_000
        settle(nowMs)
    }

    /** На 2 с исключить старт/смену маршрута из обучения, затем заново отсчитывать устойчивый интервал. */
    fun settle(nowMs: Long) {
        settleUntilMs = nowMs + 2_000
        stableSinceMs = settleUntilMs
    }

    /** underruns — накопительный счётчик устройства, played — проверенный head либо -1.
     * null — менять размер не нужно; Change может повторять текущий размер (reset счётчика/предел capacity).
     * Уменьшение требует роста head с интервалами наблюдений 1..1000 мс; сброс underruns начинает стабилизацию. */
    fun sample(nowMs: Long, underruns: Int, played: Long): Change? {
        val delta = underruns - previousUnderruns
        val progress = played >= 0 && previousPlayed >= 0 && played > previousPlayed
        val timely = nowMs - previousMs in 1..1_000
        previousUnderruns = underruns
        previousPlayed = played
        previousMs = nowMs
        if (nowMs < settleUntilMs) return null
        if (delta < 0) {
            settle(nowMs)
            return Change(actual, "counter-reset")
        }
        if (delta > 0) {
            probeIntervalMs = (probeIntervalMs * 2).coerceAtMost(300_000)
            settle(nowMs)
            val next = (actual + blockFrames).coerceAtMost(capacity)
            if (next == actual) settleUntilMs = nowMs + 10_000
            return Change(next, if (next > actual) "underrun" else "capacity-limit", delta)
        }
        if (!progress || !timely) stableSinceMs = nowMs
        if (actual <= floor || nowMs - stableSinceMs < probeIntervalMs) return null
        stableSinceMs = nowMs
        return Change((actual - blockFrames).coerceAtLeast(floor), "stable-probe")
    }

    /** Подтвердить принятые frames/capacityFrames после Change, включая округление/ограничение устройства.
     * Отказ уменьшения запоминается как нижний предел; успешная проба не скрывает следующий underrun стабилизацией. */
    fun applied(nowMs: Long, change: Change, frames: Int, capacityFrames: Int) {
        require(frames > 0 && capacityFrames >= frames)
        if (change.frames < actual && frames >= actual) floor = frames
        actual = frames
        capacity = capacityFrames
        // Уменьшение не останавливает устройство: первый underrun сразу опровергает пробу.
        stableSinceMs = maxOf(nowMs, settleUntilMs)
    }
}
