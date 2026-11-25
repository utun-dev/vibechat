package com.utun.chat.data

/** Остаток захвата PCM 48 кГц в сэмплах: read — уже прочитанные, frame/stampNs — аппаратный timestamp.
 * nowNs и stampNs используют CLOCK_MONOTONIC; учитывается возраст timestamp, результат неотрицателен.
 * null — отрицательная позиция, неположительная метка или возраст вне -20..500 мс. */
internal fun callCapturePendingFrames(read: Long, frame: Long, stampNs: Long, nowNs: Long): Long? {
    if (frame < 0 || stampNs <= 0 || nowNs - stampNs !in -20_000_000..500_000_000) return null
    return (frame - read + (nowNs - stampNs) * 48_000 / 1_000_000_000).coerceAtLeast(0)
}

/** Шкала одного AudioRecord после startRecording, 48 кГц; владелец — capture worker.
 * Валидный timestamp обновляет начало шкалы. Без него продолжается прежний якорь по числу сэмплов;
 * первый fallback считает концом прочитанного блока nowUs. При замене устройства нужен reset. */
internal class CallCaptureClock {
    private var originUs = Long.MIN_VALUE
    /** Хотя бы один аппаратный якорь принят после reset; не означает успешный замер текущего блока. */
    var measured = false
        private set

    fun reset() { originUs = Long.MIN_VALUE; measured = false }

    /** first — индекс начала блока от старта записи, count — его длина; возвращает PTS первого сэмпла в мкс.
     * nowUs/stampUs — CLOCK_MONOTONIC; nullable stampFrame/stampUs должны описывать одну аппаратную точку. */
    fun firstSampleUs(first: Long, count: Int, nowUs: Long, stampFrame: Long?, stampUs: Long?): Long {
        if (stampFrame != null && stampUs != null && stampFrame >= 0 && stampUs > 0 && stampUs <= nowUs + 20_000) {
            val candidate = stampUs - stampFrame * 1_000_000 / 48_000
            val start = candidate + first * 1_000_000 / 48_000
            if (start >= 0 && start <= nowUs) {
                originUs = candidate
                measured = true
            }
        }
        if (originUs == Long.MIN_VALUE) originUs = nowUs - (first + count) * 1_000_000 / 48_000
        return originUs + first * 1_000_000 / 48_000
    }
}

/** Разворачивает unsigned 32-bit head в число сэмплов одного выхода; владелец — playback worker.
 * Регрессия (дельта > INT32_MAX) или выход за written навсегда запрещает эту шкалу: нужен новый объект. */
internal class CallPlaybackHead {
    private var raw = 0L
    private var position = 0L
    var failed = false
        private set

    /** head — сырые младшие 32 бита, written — принятые этим выходом сэмплы; null при недостоверной позиции. */
    fun update(head: Int, written: Long): Long? {
        if (failed) return null
        val next = head.toLong() and 0xffffffffL
        val delta = (next - raw) and 0xffffffffL
        if (delta > 0x7fffffffL || position + delta > written) { failed = true; return null }
        raw = next
        position += delta
        return position
    }
}

/** Время вывода конца записанного PCM 48 кГц; владелец — playback worker одного запуска звонка.
 * Все позиции относятся к общей шкале принятого PCM, монотонный written ведёт вызывающий код.
 * Смена routeGeneration сбрасывает аппаратный якорь. После первого принятого timestamp за всё время
 * жизни объекта возврат к оценке по head запрещён, в том числе при замене выхода. */
internal class CallPlaybackClock {
    /** position=written; atUs — ожидаемое время его вывода CLOCK_MONOTONIC, measured — аппаратный якорь. */
    data class Anchor(val position: Long, val atUs: Long, val measured: Boolean)
    private var generation = -1L
    private var stampFrame = -1L
    private var stampUs = -1L
    private var measuredOnce = false

    /** head — проверенная общая позиция, frame/timeUs — снимок устройства поколения sampleGeneration.
     * Время в мкс CLOCK_MONOTONIC; timestamp принимается только для текущего поколения и близко к nowUs.
     * Возвращает аппаратный/начальный оценочный якорь либо null; младшие 32 бита frame разворачиваются по written. */
    fun update(written: Long, head: Long, nowUs: Long, routeGeneration: Long,
               sampleGeneration: Long, frame: Long, timeUs: Long): Anchor? {
        if (generation != routeGeneration) {
            generation = routeGeneration
            stampFrame = -1; stampUs = -1
        }
        if (head < 0 || head > written) return null
        // AudioTimestamp может содержать только младшие 32 бита; разворачиваем относительно written.
        var extended = frame
        if (frame in 0..0xffffffffL) {
            extended = (written and -0x100000000L) or frame
            if (extended > written) extended -= 0x100000000L
        }
        if (sampleGeneration == generation && extended in 0..written && timeUs > 0 &&
            timeUs in (nowUs - 500_000)..(nowUs + 200_000) && written - extended <= 96_000) {
            stampFrame = extended; stampUs = timeUs; measuredOnce = true
        }
        if (stampFrame >= 0 && stampUs in (nowUs - 500_000)..(nowUs + 200_000)) {
            val at = stampUs + (written - stampFrame) * 1_000_000 / 48_000
            if (at in (nowUs - 20_000)..(nowUs + 2_000_000)) return Anchor(written, at, true)
        }
        // Исчезнувший аппаратный timestamp не заменяем head: это сдвинуло бы видео вперёд на задержку HAL.
        if (measuredOnce) return null
        return Anchor(written, nowUs + (written - head) * 1_000_000 / 48_000, false)
    }
}
