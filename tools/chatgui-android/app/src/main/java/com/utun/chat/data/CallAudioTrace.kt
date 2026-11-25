package com.utun.chat.data

/**
 * Ограниченный трейс одного звонка: playback копирует числовые строки WIDTH в record,
 * единственный reporter вызывает flush и emit(level, text). Форматирования/логов под lock нет.
 * При переполнении кольца теряются старейшие строки, число потерь попадает в WARN при flush.
 * Сводки формируются примерно за секунду; старт/смена маршрута/аномалии собирают окно -2..+3 с,
 * новые окна ограничены паузой 10 с после предыдущего dump. Неполное окно сохраняет flush(true).
 * Владелец включает enabled по native-фильтру call=debug и публикует device отдельно от record;
 * flush при выключении очищает историю. Сам класс не управляет устройством или размером его буфера.
 */
internal class CallAudioTrace(private val id: Long, private val emit: (String, String) -> Unit) {
    companion object {
        const val WIDTH = 37
        const val CAPACITY = 512
        // monoUs, routeId, routeType, written, played, pullUs, writeCallUs, pullGapExcessUs, zeroWrites,
        // deviceUnderruns, bufferFrames, startFrames, stampFrame, stampNs, stampReadUs, stampOk,
        // encodedMs, processingMs, reserveMs, spreadMs, pcmSamples, tempo100, jbUnderruns, waiting, accepted
        // headCallUs, progressCallUs, writeSpanUs, jbSnapshotUs, blockUs,
        // deviceBeginUs, deviceEndUs, stampQueryUs, routeQueryUs, propertiesQueryUs, outputUs, performanceMode
        private const val PRE_US = 2_000_000L
        private const val POST_US = 3_000_000L
    }
    @Volatile var enabled = false
    /** Неизменяемый снимок reporter; playback читает ссылку без Android API.
     * Позиции — общие PCM-сэмплы, begin/end/query — монотонные мкс, stampNs — CLOCK_MONOTONIC в нс.
     * Неизвестные поля равны -1 (длительности запросов по умолчанию 0); generation связывает снимок с выходом. */
    data class DeviceSample(
        val routeId: Long = -1, val routeType: Long = -1, val underruns: Long = -1,
        val bufferFrames: Long = -1, val startFrames: Long = -1,
        val stampFrame: Long = -1, val stampNs: Long = -1, val stampOk: Boolean = false,
        val beginUs: Long = -1, val endUs: Long = -1,
        val stampQueryUs: Long = 0, val routeQueryUs: Long = 0, val propertiesQueryUs: Long = 0,
        val generation: Long = -1, val performanceMode: Int = -1
    ) {
        /** Заполнить только колонки устройства в строке WIDTH; измерения playback сохраняются. */
        fun copyTo(row: LongArray) {
            row[1] = routeId; row[2] = routeType; row[9] = underruns
            row[10] = bufferFrames; row[11] = startFrames
            row[12] = stampFrame; row[13] = stampNs; row[14] = beginUs; row[15] = if (stampOk) 1 else 0
            row[30] = beginUs; row[31] = endUs
            row[32] = stampQueryUs; row[33] = routeQueryUs; row[34] = propertiesQueryUs
            row[36] = performanceMode.toLong()
        }
    }
    @Volatile var device = DeviceSample()
    private val ring = Array(CAPACITY) { LongArray(WIDTH) }
    private var head = 0
    private var count = 0
    private var lost = 0L
    private val history = ArrayDeque<LongArray>() // Только reporter.
    private val summary = ArrayList<LongArray>(64)
    private var previous: LongArray? = null
    private var summaryAt = 0L
    private var eventAt = -1L
    private var eventBits = 0
    private var eventCount = 0
    private var lastDump = -10_000_000L
    private var suppressed = 0
    private var highSince = -1L
    private var stoppedSince = -1L
    private var underrunAt = -10_000_000L
    private var faults = 0
    private var reportedDeviceUnderruns = 0L
    private var reportedJbUnderruns = 0L

    /** Playback: скопировать строку WIDTH; вызывающий может сразу переиспользовать свой массив. При !enabled — no-op. */
    @Synchronized fun record(row: LongArray) {
        if (!enabled) return
        if (count == CAPACITY) { head = (head + 1) % CAPACITY; count--; lost++ }
        row.copyInto(ring[(head + count) % CAPACITY])
        count++
    }

    // Копирование ограничено 512 короткими строками; никаких логов под lock.
    @Synchronized private fun take(): Pair<List<LongArray>, Long> {
        val rows = List(count) { ring[(head + it) % CAPACITY].copyOf() }
        val overflow = lost
        head = 0; count = 0; lost = 0
        return rows to overflow
    }

    /** Только reporter, обычно раз в 200 мс; finished сохраняет неполную сводку и окно события при stop.
     * emit выполняется синхронно в этом потоке после снятия lock кольца; при !enabled накопленные данные отбрасываются. */
    fun flush(finished: Boolean = false) {
        val (rows, overflow) = take()
        if (!enabled) {
            history.clear(); summary.clear(); previous = null; eventAt = -1
            faults = 0; highSince = -1; stoppedSince = -1; underrunAt = -10_000_000
            lastDump = -10_000_000; suppressed = 0
            reportedDeviceUnderruns = 0; reportedJbUnderruns = 0
            return
        }
        if (overflow > 0) emit("WARN", "audio trace call=$id reporter overrun lost=$overflow")
        for (row in rows) {
            val now = row[0]
            val old = previous
            // Сначала завершаем старое окно, чтобы оно не поглотило новое событие после задержки reporter.
            if (eventAt >= 0 && now > eventAt + POST_US) dump(false)
            history.addLast(row)
            while (history.size > 400 || (eventAt < 0 && history.first()[0] < now - PRE_US)) history.removeFirst()
            var events = if (old == null) 1 else 0
            if (old != null && old[1] >= 0 && row[1] >= 0 && (row[1] != old[1] || row[2] != old[2])) events = events or 2
            val queueUs = queued(row)
            if (queueUs > 100_000) { if (highSince < 0) highSince = now } else highSince = -1
            if (old != null && row[4] == old[4] && queueUs > 0) {
                if (stoppedSince < 0) stoppedSince = old[0]
            } else stoppedSince = -1
            if (old != null && old[9] >= 0 && row[9] > old[9]) underrunAt = now
            val current = (if (highSince >= 0 && now - highSince >= 1_000_000) 4 else 0) or
                (if (stoppedSince >= 0 && now - stoppedSince >= 100_000) 8 else 0) or
                (if (now - underrunAt < 3_000_000) 16 else 0)
            val added = current and faults.inv()
            if (added != 0) {
                emit("WARN", "audio trace call=$id fault=$added queueUs=$queueUs route=${row[1]}/${row[2]} monoUs=$now")
                events = events or added
            }
            if (faults != 0 && current == 0) emit("INFO", "audio trace call=$id recovered monoUs=$now")
            faults = current
            if (events != 0) {
                if (eventAt >= 0) { eventBits = eventBits or events; eventCount++ }
                else if (now - lastDump >= 10_000_000) { eventAt = now; eventBits = events; eventCount = 1 }
                else suppressed++
            }
            if (summary.isEmpty()) summaryAt = now
            summary.add(row)
            if (now - summaryAt >= 1_000_000) summarize()
            previous = row
        }
        if (finished) {
            if (summary.isNotEmpty()) summarize()
            if (eventAt >= 0) dump(true)
        }
    }

    private fun queued(r: LongArray): Long = if (r[4] < 0) -1 else (r[3] - r[4]) * 1_000_000 / 48000

    private fun range(value: (LongArray) -> Long): String {
        val values = summary.map(value)
        return "${values.minOrNull()}/${values.sum() / values.size}/${values.maxOrNull()}"
    }

    private fun summarize() {
        val first = summary.first(); val last = summary.last()
        emit("DEBUG", "audio summary call=$id monoUs=${last[0]} spanUs=${last[0] - first[0]} n=${summary.size} " +
            "route=${last[1]}/${last[2]} min/avg/max queueUs=${range(::queued)} " +
            "jbMs=${range { it[16] + it[17] }} reserveMs=${range { it[18] }} spreadMs=${range { it[19] }} " +
            "pullUs=${range { it[5] }} writeCallUs=${range { it[6] }} headCallUs=${range { it[25] }} " +
            "progressCallUs=${range { it[26] }} writeSpanUs=${range { it[27] }} jbSnapshotUs=${range { it[28] }} " +
            "blockUs=${range { it[29] }} pullGapExcessUs=${range { it[7] }} " +
            "tempo=${last[21]} deviceUnderruns=${last[9]} deviceUnderDelta=${last[9] - reportedDeviceUnderruns} " +
            "jbUnderruns=${last[22]} jbUnderDelta=${last[22] - reportedJbUnderruns} " +
            "zeroWrites=${summary.sumOf { it[8] }} missingPcm=${summary.sumOf { (960 - it[20]).coerceAtLeast(0) }} " +
            "stamp=${last[12]}@${last[13]} readUs=${last[14]} ok=${last[15]} " +
            "deviceAgeUs=${if (last[31] >= 0) last[0] - last[31] else -1} " +
            "stampQueryUs=${range { it[32] }} routeQueryUs=${range { it[33] }} propertiesQueryUs=${range { it[34] }} " +
            "buffer=${last[10]} start=${last[11]} outputUs=${range { it[35] }} performanceMode=${last[36]} " +
            "outputIncludesQueue=1 fault=$faults suppressed=$suppressed")
        reportedDeviceUnderruns = last[9]; reportedJbUnderruns = last[22]
        summary.clear()
    }

    private fun dump(partial: Boolean) {
        emit("DEBUG", "audio detail call=$id triggerUs=$eventAt reasons=$eventBits events=$eventCount partial=$partial " +
            "bits=start:1,route:2,queue:4,stalled:8,underrun:16 " +
            "columns=monoUs,routeId,routeType,written,played,pullUs,writeCallUs,pullGapExcessUs,zeroWrites,deviceUnderruns," +
            "bufferFrames,startFrames,stampFrame,stampNs,stampReadUs,stampOk," +
            "encodedMs,processingMs,reserveMs,spreadMs,pcmSamples,tempo100,jbUnderruns,waiting,accepted," +
            "headCallUs,progressCallUs,writeSpanUs,jbSnapshotUs,blockUs," +
            "deviceBeginUs,deviceEndUs,stampQueryUs,routeQueryUs,propertiesQueryUs,outputUs,performanceMode")
        // До пяти секунд, по четыре снимка в строке, с явным временем каждого.
        history.filter { it[0] in (eventAt - PRE_US)..(eventAt + POST_US) }.chunked(4).forEach { group ->
            emit("DEBUG", "audio detail call=$id rows=" + group.joinToString(";") { it.joinToString(",") })
        }
        lastDump = history.lastOrNull()?.get(0) ?: eventAt
        eventAt = -1
    }
}
