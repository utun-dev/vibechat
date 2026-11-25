package com.utun.chat.data

import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRouting
import android.media.AudioTimestamp
import android.media.AudioTrack
import android.os.Build
import android.os.Handler

/**
 * Выход PCM 48 кГц mono PCM16 без дополнительной очереди: AAudio либо AudioTrack.
 * Playback worker создаёт выход и пишет PCM; reporter читает timestamp и настраивает буфер.
 * Замена выхода сериализована владельцем через trackLock, release при stop — после join обоих workers.
 * Методы I/O и release используют монитор этого объекта; доступ к свойствам/слушателям владелец
 * согласует отдельно. AAudio-свойства буфера/маршрута/underruns берутся из последнего sample,
 * обновляемого при открытии, getTimestamp и setBufferSizeInFrames.
 */
internal class CallAudioOutput private constructor(
    val track: AudioTrack?, private var handle: Long, private val manager: AudioManager
) {
    val fast = handle != 0L
    val name = if (fast) "AAudio" else "AudioTrack"
    private val sample = LongArray(10)
    private var accepted = 0L
    private var closed = false
    val playState: Int get() = track?.playState ?: if (closed) AudioTrack.PLAYSTATE_STOPPED else AudioTrack.PLAYSTATE_PLAYING
    val bufferSizeInFrames: Int get() = track?.bufferSizeInFrames ?: sample[4].toInt()
    val bufferCapacityInFrames: Int get() = track?.bufferCapacityInFrames ?: sample[5].toInt()
    val underrunCount: Int get() = track?.underrunCount ?: sample[3].toInt()
    val performanceMode: Int get() = track?.performanceMode ?: AudioTrack.PERFORMANCE_MODE_LOW_LATENCY
    val startThresholdInFrames: Int get() = if (track != null && Build.VERSION.SDK_INT >= 31) track.startThresholdInFrames else bufferSizeInFrames
    val routedDevice: AudioDeviceInfo? get() = track?.routedDevice ?:
        manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull { it.id.toLong() == sample[6] }

    // AAudio уже запущен native open; play/pause управляют только AudioTrack.
    @Synchronized fun play() { check(!closed); track?.play() }
    @Synchronized fun pause() { if (!closed) track?.pause() }
    /** Число принятых сэмплов, 0 — повторить позже, отрицательное — ошибка backend.
     * AudioTrack — NON_BLOCKING; AAudio — порциями аппаратного burst с общим timeout 100 мс.
     * Диапазон pcm[offset..offset+count) должен существовать; для AAudio count=1..960. */
    @Synchronized fun write(pcm: ShortArray, offset: Int, count: Int): Int {
        check(!closed)
        val n = track?.write(pcm, offset, count, AudioTrack.WRITE_NON_BLOCKING) ?: CallOutputNative.write(handle, pcm, offset, count)
        if (n > 0) accepted += n
        return n
    }
    /** Младшие unsigned 32 бита позиции; AAudio ограничен числом сэмплов, принятых через write. */
    val playbackHeadPosition: Int @Synchronized get() = track?.playbackHeadPosition ?:
        CallOutputNative.head(handle).coerceIn(0, accepted).toInt()

    /** Аппаратная позиция локального выхода и CLOCK_MONOTONIC в ns; false — timestamp недоступен.
     * Для AAudio одновременно обновляет кеш свойств, даже если сам timestamp ещё не получен. */
    @Synchronized fun getTimestamp(stamp: AudioTimestamp): Boolean {
        if (closed) return false
        track?.let { return it.getTimestamp(stamp) }
        CallOutputNative.sample(handle, sample)
        if (sample[1] < 0 || sample[2] < 0) return false
        stamp.framePosition = sample[1]; stamp.nanoTime = sample[2]
        return true
    }
    @Synchronized fun setBufferSizeInFrames(frames: Int): Int {
        check(!closed)
        track?.let { return it.setBufferSizeInFrames(frames) }
        val actual = CallOutputNative.buffer(handle, frames)
        CallOutputNative.sample(handle, sample)
        return actual
    }
    /** Реальный start threshold задаётся только AudioTrack на API 31+; иначе возвращает заявку без изменения. */
    @Synchronized fun setStartThresholdInFrames(frames: Int): Int =
        if (track != null && Build.VERSION.SDK_INT >= 31) track.setStartThresholdInFrames(frames) else frames
    /** AAudio привязан к deviceId при open: здесь true без смены маршрута, слушатели ниже также только AudioTrack. */
    @Synchronized fun setPreferredDevice(device: AudioDeviceInfo?): Boolean = track?.setPreferredDevice(device) ?: true
    fun addOnRoutingChangedListener(listener: AudioRouting.OnRoutingChangedListener, handler: Handler) {
        track?.addOnRoutingChangedListener(listener, handler)
    }
    fun removeOnRoutingChangedListener(listener: AudioRouting.OnRoutingChangedListener) {
        track?.removeOnRoutingChangedListener(listener)
    }
    /** Идемпотентно закрыть выход; ошибка release сохраняет владельца для повторной попытки.
     * После подтверждённого освобождения write/play/настройка буфера недопустимы. */
    @Synchronized fun release() {
        if (closed) return
        if (handle != 0L) { CallOutputNative.close(handle); handle = 0 }
        track?.release()
        closed = true
    }

    companion object {
        /** AAudio пробуется лишь при tryFast и deviceId встроенного speaker из communication devices API 31+.
         * Native проверяет формат/latency/burst/device; отказ возвращает AudioTrack, сбой fallback бросает исключение.
         * Владелец сохраняет возвращённый выход до проверки track.state и дальнейшей настройки, включая отказ release. */
        fun open(manager: AudioManager, deviceId: Int, tryFast: Boolean): CallAudioOutput {
            val type = if (Build.VERSION.SDK_INT >= 31)
                manager.availableCommunicationDevices.firstOrNull { it.id == deviceId }?.type else null
            // Samsung MMAP подтверждает wired headset в HAL, но звук остаётся в speaker.
            // Быстрый backend включаем только для проверенного физического выхода; гарнитуры ведёт AudioTrack.
            val fastRoute = type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
            if (tryFast && fastRoute && Build.VERSION.SDK_INT >= 31) {
                val handle = CallOutputNative.open(deviceId)
                if (handle != 0L) return CallAudioOutput(null, handle, manager).also { CallOutputNative.sample(handle, it.sample) }
            }
            val min = AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
            check(min > 0) { "AudioTrack minimum=$min" }
            val track = AudioTrack.Builder()
                .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                .setAudioFormat(AudioFormat.Builder().setSampleRate(48000).setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
                .setBufferSizeInBytes((min * 2).coerceAtLeast(3840)).setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY).build()
            LogManager.addLog("INFO", "CallAudio", "output fallback=AudioTrack device=$deviceId type=$type tryFast=$tryFast fastRoute=$fastRoute")
            return CallAudioOutput(track, 0, manager)
        }
    }
}

/** Прямой JNI AAudio: handle != 0 до единственного close, все вызовы с ним сериализует CallAudioOutput.
 * sample требует LongArray(10): head, stampFrame, stampNs, underruns, buffer, capacity, deviceId,
 * performance, error, written. suspendClock снимает аппаратный якорь native-звонка id/epoch, сохраняя PCM/JB. */
internal object CallOutputNative {
    init { System.loadLibrary("utun") }
    external fun open(deviceId: Int): Long
    external fun write(handle: Long, pcm: ShortArray, offset: Int, count: Int): Int
    external fun head(handle: Long): Long
    external fun sample(handle: Long, out: LongArray)
    external fun buffer(handle: Long, frames: Int): Int
    external fun close(handle: Long)
    external fun suspendClock(id: Long, epoch: Long)
}
