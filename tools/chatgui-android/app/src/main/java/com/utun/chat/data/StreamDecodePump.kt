package com.utun.chat.data

import java.util.ArrayDeque

/** Один рабочий вход и один готовый выход; PTS/position/now/lead/period — микросекунды.
 * Codec владеет внутренним конвейером;
 * число поданных кадров и часы показа не ограничивают заполнение этого конвейера.
 * Все вызовы принадлежат одному player worker. Ошибки логирует владелец вместе с session ID. */
internal class StreamDecodePump<F : StreamDecodePump.Frame>(
    private val submit: (F) -> Boolean,
    private val receive: () -> Output?,
    private val release: (Output, Boolean) -> Unit
) {
    interface Frame { val pts: Long }
    data class Output(val index: Int, val pts: Long, val eos: Boolean = false, val empty: Boolean = false)
    private val submitted = ArrayDeque<Long>()
    var input: F? = null
        private set
    var output: Output? = null
        private set
    var firstOutputPts: Long? = null
        private set
    var lastInputPts = -1L
        private set
    var inputCount = 0L
        private set
    var outputCount = 0L
        private set
    var shown = 0L
        private set
    var discarded = 0L
        private set
    var decoderLeadUs = 0L
        private set
    var framePeriodUs = 33333L
        private set
    var ended = false
        private set
    private var waitingSince = -1L
    private var lastInputAt = -1L
    val wantsInput: Boolean get() = input == null && output == null && !ended
    val oldestSubmittedPts: Long get() = submitted.firstOrNull() ?: -1L
    val inflight: Int get() = submitted.size

    fun offer(frame: F, now: Long) {
        // Уже запрошенный ответ может прийти после появления будущего output.
        check(input == null && !ended) {
            "Decoder already owns a working frame: incomingPTS=${frame.pts} inputPTS=${input?.pts} outputPTS=${output?.pts} " +
                "ended=$ended inputs=$inputCount decoded=$outputCount shown=$shown"
        }
        // Новые данные после сетевой паузы получают собственное окно ожидания decoder progress.
        if (lastInputAt < 0 || now - lastInputAt > 1_000_000) waitingSince = now
        input = frame
    }

    /** Выход проверяется перед каждым входом. Свободный input buffer берут лишь для готового кадра. */
    fun step(position: Long, now: Long) {
        drain(position, now)
        if (output != null || ended) return
        val frame = input
        if (frame != null) {
            check(lastInputPts < frame.pts) { "Decoder input PTS did not advance" }
            if (!submit(frame)) return
            if (lastInputPts >= 0) framePeriodUs = (frame.pts - lastInputPts).coerceIn(1, 1_000_000)
            lastInputPts = frame.pts; lastInputAt = now; inputCount++; submitted.addLast(frame.pts); input = null
            drain(position, now)
        }
    }

    private fun drain(position: Long, now: Long) {
        repeat(8) {
            if (ended) return
            if (output == null) {
                val next = receive() ?: return
                output = next
                if (!next.empty) {
                    if (firstOutputPts == null) firstOutputPts = next.pts
                    // Опустевший хвост не уменьшает измеренный защитный запас текущего decoder.
                    outputCount++; decoderLeadUs = maxOf(decoderLeadUs, lastInputPts - next.pts)
                    while (submitted.isNotEmpty() && submitted.first <= next.pts) submitted.removeFirst()
                }
                waitingSince = now
            }
            val next = output!!
            if (!next.empty && next.pts > position) return
            val render = !next.empty && next.pts >= position - 100000
            release(next, render); output = null
            if (!next.empty) { if (render) shown++ else discarded++ }
            if (next.eos) { ended = true; submitted.clear() }
        }
    }

    /** Нехватка сети и ожидание PTS допустимы; зависший decoder при наличии данных — ошибка. */
    fun checkProgress(now: Long, finishing: Boolean) {
        val hasWork = input != null || (lastInputAt >= 0 && now - lastInputAt < 1_000_000) || finishing
        check(ended || output != null || !hasWork || waitingSince < 0 || now - waitingSince <= 10_000_000) {
            "Decoder made no output progress: inputs=$inputCount outputs=$outputCount inflight=$inflight lastPTS=$lastInputPts"
        }
    }
}
