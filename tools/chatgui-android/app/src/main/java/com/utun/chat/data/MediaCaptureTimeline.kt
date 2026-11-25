package com.utun.chat.data

import java.util.ArrayDeque
import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/**
 * Непрерывная сетка PCM16 48 кГц stereo для InputBridge общего media-входа.
 * originNs и timestamps используют System.nanoTime; один pull выдаёт 960 stereo-фреймов (1920 Short).
 * Владелец сериализует все обращения своим lock и ждёт deadlineNs перед pull: конец блока плюс резерв 200 мс.
 * reset переключает поколение аппаратного входа, сохраняя общую выходную позицию/время.
 * До привязки, перед началом входа и при пропусках выдаётся тишина; поздний PCM не заменяет уже выданную тишину.
 * Внутри поколения sequence определяет непрерывность, шумные timestamps не переставляют блоки.
 * Медленный дрейф компенсируется интерполяцией до ±2000 ppm; скачок времени либо компенсируется bias,
 * либо заново привязывает свежий звук после остановки reader. Здесь нет I/O, ожиданий или фонового worker.
 */
internal class MediaCaptureTimeline(val originNs: Long, private val log: (String, String) -> Unit = { _, _ -> }) {
    companion object {
        const val FRAMES = 960
        const val PERIOD_NS = 20_000_000L
        const val RESERVE_NS = 200_000_000L
        private const val NS_PER_FRAME = 1_000_000_000.0 / 48000
    }
    private val pending = ArrayDeque<MicrophoneCapture.Frame>()
    private var position = 0L
    private var lastInputNs = -1L
    private var lastSequence = -1L
    private var lastCreatedNs = 0L
    private var anchorNs = 0L
    private var anchorSequence = 0L
    private var startPosition = 0L
    private var anchored = false
    private var cursor = Double.NaN
    private var clockOffset = 0.0
    private var timestampBiasNs = 0L
    var generation = 0L
        private set
    /** Число отвергнутых 20мс блоков: поздние или очередь уже содержит 24 блока; накопление за жизнь объекта. */
    var lateFrames = 0L
        private set
    /** Число выданных stereo-фреймов тишины, включая ожидание привязки; reset не обнуляет. */
    var silentFrames = 0L
        private set
    /** Часть silentFrames после начала привязанного входа, когда PCM отсутствует по нужному sequence. */
    var missingFrames = 0L
        private set
    var timestampJitterMaxNs = 0L
        private set
    var timestampJumps = 0L
        private set
    var ratePpm = 0
        private set
    val nextTimeNs: Long get() = originNs + position * 1_000_000_000 / 48000
    val deadlineNs: Long get() = nextTimeNs + PERIOD_NS + RESERVE_NS

    /** Очистить очередь, привязку и оценку дрейфа/jitter для epoch; выходная position и накопленные счётчики PCM сохраняются. */
    fun reset(epoch: Long) {
        generation = epoch; pending.clear(); lastInputNs = -1; lastSequence = -1; lastCreatedNs = 0
        anchored = false; cursor = Double.NaN; clockOffset = 0.0; ratePpm = 0; timestampJitterMaxNs = 0; timestampBiasNs = 0; timestampJumps = 0
    }

    /** Принять 1920 Short с sequence в stereo-фреймах и временем первого фрейма; PCM заимствуется до pull/reset.
     * false — чужое поколение, неповышающийся sequence, поздний PCM или полная очередь.
     * Неверная длина PCM либо отрицательные sequence/timeNs бросают исключение.
     * Вызывающий не должен менять массив принятого Frame. Необычные пропуски/скачки логируются через log владельца. */
    fun offer(frame: MicrophoneCapture.Frame): Boolean {
        require(frame.pcm.size == FRAMES * 2 && frame.sequence >= 0 && frame.timeNs >= 0)
        if (frame.generation != generation) return false
        if (lastSequence >= 0) {
            if (frame.sequence <= lastSequence) {
                log("WARN", "microphone stale PCM generation=$generation sequence=${frame.sequence}/$lastSequence time=${frame.timeNs}/$lastInputNs")
                return false
            }
            val elapsed = (frame.sequence - lastSequence) * 1_000_000_000 / 48000
            val jitter = frame.timeNs - lastInputNs - elapsed
            timestampJitterMaxNs = maxOf(timestampJitterMaxNs, abs(jitter))
            // Отличаем перестановку timestamp от остановки аппаратного reader при запуске Bluetooth.
            // Свежий звук после реальной остановки привязываем заново, не воспроизводя старую очередь.
            if (abs(jitter) > PERIOD_NS * 2) {
                val deliveryGap = frame.createdNs - lastCreatedNs - elapsed
                val latePcm = !cursor.isNaN() && frame.sequence + FRAMES <= cursor
                val reanchor = frame.timeNs + PERIOD_NS > nextTimeNs && (deliveryGap > PERIOD_NS * 2 || latePcm)
                if (reanchor) { pending.clear(); anchored = false; cursor = Double.NaN; clockOffset = 0.0; timestampBiasNs = 0 }
                else timestampBiasNs += jitter
                timestampJumps++
                if (timestampJumps == 1L || timestampJumps % 50L == 0L)
                    log("WARN", "microphone timestamp jump generation=$generation deltaNs=$jitter deliveryGapNs=$deliveryGap " +
                        "late=$latePcm reanchor=$reanchor count=$timestampJumps")
            }
            if (frame.sequence != lastSequence + FRAMES)
                log("WARN", "microphone sequence gap generation=$generation expected=${lastSequence + FRAMES} actual=${frame.sequence}")
        }
        lastInputNs = frame.timeNs; lastSequence = frame.sequence; lastCreatedNs = frame.createdNs
        val late = if (anchored && !cursor.isNaN()) frame.sequence + FRAMES <= cursor else frame.timeNs + PERIOD_NS <= nextTimeNs
        if (late || pending.size >= 24) {
            lateFrames++
            if (lateFrames == 1L || lateFrames % 50L == 0L)
                log("WARN", "microphone PCM rejected generation=$generation late=$late queue=${pending.size} " +
                    "sequence=${frame.sequence} cursor=$cursor ageUs=${(frame.createdNs - frame.timeNs) / 1000} count=$lateFrames")
            return false
        }
        if (!anchored) {
            anchorNs = frame.timeNs; anchorSequence = frame.sequence
            startPosition = ((anchorNs - originNs) / NS_PER_FRAME).roundToLong()
            anchored = true
            log("INFO", "microphone clock anchored generation=$generation sequence=$anchorSequence position=$startPosition")
        }
        // Фильтр около 1с подавляет джиттер измерений; фазовая ошибка не двигает уже прочитанный PCM.
        val measured = (frame.timeNs - anchorNs - timestampBiasNs) / NS_PER_FRAME - (frame.sequence - anchorSequence)
        clockOffset += (measured - clockOffset) * 0.02
        pending.addLast(frame)
        return true
    }

    /** Требует nowNs >= deadlineNs. Вернуть новый Frame общей выходной сетки с текущим generation и продвинуть её на 20 мс.
     * timeNs/sequence относятся к сетке, createdNs=nowNs; массив принадлежит получателю. */
    fun pull(nowNs: Long): MicrophoneCapture.Frame {
        check(nowNs >= deadlineNs)
        val start = nextTimeNs
        val pcm = ShortArray(FRAMES * 2)
        var hasCapture = false
        val leading = if (anchored) (startPosition - position).coerceIn(0, FRAMES.toLong()).toInt() else FRAMES
        if (anchored && leading < FRAMES && cursor.isNaN()) cursor = anchorSequence + maxOf(0, position - startPosition).toDouble()
        var step = 1.0
        if (!cursor.isNaN()) {
            val error = anchorSequence + (position + leading - startPosition).toDouble() - clockOffset - cursor
            // 1мс мёртвой зоны: обычный джиттер не меняет PCM. Коррекция дрейфа ограничена ±0.2%.
            val correction = if (abs(error) <= 48) 0.0 else (error - if (error > 0) 48 else -48) / 96000
            step += correction.coerceIn(-0.002, 0.002)
        }
        ratePpm = ((step - 1) * 1_000_000).roundToInt()
        for (i in 0 until FRAMES) {
            if (i < leading) { silentFrames++; continue }
            val sample = floor(cursor).toLong()
            while (pending.isNotEmpty() && pending.first.sequence + FRAMES <= sample) pending.removeFirst()
            val frame = pending.peekFirst()
            val offset = if (frame == null) -1 else sample - frame.sequence
            if (frame == null || offset < 0 || offset >= FRAMES) {
                silentFrames++; missingFrames++
            } else {
                hasCapture = true
                val index = offset.toInt()
                val fraction = cursor - sample
                val next = if (index + 1 < FRAMES) frame else pending.elementAtOrNull(1)?.takeIf { it.sequence == frame.sequence + FRAMES }
                val nextIndex = if (index + 1 < FRAMES) index + 1 else 0
                for (channel in 0..1) {
                    val a = frame.pcm[index * 2 + channel].toInt()
                    val b = next?.pcm?.get(nextIndex * 2 + channel)?.toInt() ?: a
                    pcm[i * 2 + channel] = (a + (b - a) * fraction).roundToInt().toShort()
                }
            }
            cursor += step
        }
        return MicrophoneCapture.Frame(pcm, start, position, nowNs, generation, hasCapture).also { position += FRAMES }
    }
}
