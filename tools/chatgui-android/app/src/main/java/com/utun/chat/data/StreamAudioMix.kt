package com.utun.chat.data

import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** Worker-локальное начало источника на шкале AudioTrack, позиции/count — PCM frames, не stereo samples.
 * append требует непрерывные порции, включая тишину источника; первый append фиксирует origin.
 * played переводит общий head в локальные [0, frames]. Новая шкала требует нового экземпляра, PTS здесь не сравниваются. */
internal class StreamMixTimeline {
    var origin = -1L
        private set
    var frames = 0L
        private set

    fun append(firstOutputFrame: Long, count: Int) {
        require(firstOutputFrame >= 0 && count > 0)
        if (origin < 0) origin = firstOutputFrame
        check(firstOutputFrame == origin + frames) { "Non-contiguous mixer output $firstOutputFrame != ${origin + frames}" }
        frames += count
    }

    fun played(outputHead: Long): Long = if (origin < 0) 0 else (outputHead - origin).coerceIn(0, frames)
}

/** Одна порция stereo 48k. Тихий новый участник не снижает громкость остальных.
 * frames=480 по умолчанию (10 мс), count у add — только подготовленный prefix PCM.
 * fadeFrames=0..480 задаёт фазу 10 мс fade-in; add возвращает обновлённую фазу.
 * clear очищает сумму, сохраняя состояние stereo limiter; finish возвращает переиспользуемый буфер,
 * который разрешено менять только после полной записи предыдущей порции устройством. */
internal class StreamMixBuffer(val frames: Int = 480) {
    private val sum = FloatArray(frames * 2)
    private val output = ShortArray(frames * 2)
    private var gain = 1f
    fun clear() = sum.fill(0f)

    fun add(pcm: ShortArray, fadeFrames: Int = 480, count: Int = pcm.size / 2): Int {
        require(pcm.size % 2 == 0 && count in 0..min(frames, pcm.size / 2))
        for (i in 0 until count) {
            val volume = min(1f, (fadeFrames + i + 1) / 480f) / 32768f
            sum[2 * i] += pcm[2 * i] * volume
            sum[2 * i + 1] += pcm[2 * i + 1] * volume
        }
        return min(480, fadeFrames + count)
    }

    fun finish(): ShortArray {
        for (i in 0 until frames) {
            val peak = max(abs(sum[i * 2]), abs(sum[i * 2 + 1]))
            val target = if (peak > 1f) 1f / peak else 1f
            gain = min(target, gain + (1f - gain) / 4800f)
            output[i * 2] = (sum[i * 2] * gain * 32768f).roundToInt().coerceIn(-32768, 32767).toShort()
            output[i * 2 + 1] = (sum[i * 2 + 1] * gain * 32768f).roundToInt().coerceIn(-32768, 32767).toShort()
        }
        return output
    }
}
