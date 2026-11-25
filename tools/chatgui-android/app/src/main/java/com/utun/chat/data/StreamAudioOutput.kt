package com.utun.chat.data

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build

/** Один mixer worker владеет устройством и единственным недописанным PCM.
 * Native уже учёл выданные кадры: остаток нельзя терять или переставлять до сброса всей шкалы.
 * Sink сохраняется до initialize/play, close снимает ссылку только после успешного освобождения.
 */
internal class StreamAudioOutput(
    private val open: () -> Sink = { AndroidSink() },
    private val log: (String) -> Unit = {}
) {
    internal interface Sink {
        val routeChanging: Boolean get() = false
        val bufferFrames: Int
        val threshold: Int
        val head: Long
        val description: String
        fun initialize() {}
        fun write(pcm: ShortArray, offset: Int, count: Int): Int
        fun pause()
        fun play()
        fun close()
    }

    private class AndroidSink : Sink {
        override val routeChanging: Boolean get() = MediaAudioController.state.value.switching
        private var route: AutoCloseable? = null
        private val track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
            .setAudioFormat(AudioFormat.Builder().setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setBufferSizeInBytes(AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT).coerceAtLeast(3840)).setTransferMode(AudioTrack.MODE_STREAM).build()
        override fun initialize() {
            check(track.state == AudioTrack.STATE_INITIALIZED) { "AudioTrack initialization failed" }
            // API 26..30 не умеет уменьшать порог отдельно от аппаратного буфера.
            if (Build.VERSION.SDK_INT >= 31) track.setStartThresholdInFrames(1)
            route = MediaAudioController.attachOutput(track)
        }
        override val bufferFrames: Int get() = track.bufferSizeInFrames
        override val threshold: Int get() = if (Build.VERSION.SDK_INT >= 31)
            minOf(track.startThresholdInFrames, track.bufferSizeInFrames) else track.bufferSizeInFrames
        override val head: Long get() = track.playbackHeadPosition.toLong() and 0xffffffffL
        override val description: String get() = "state=${track.state} playState=${track.playState} " +
            "buffer=${track.bufferSizeInFrames} threshold=$threshold underruns=${track.underrunCount}"
        override fun write(pcm: ShortArray, offset: Int, count: Int) = track.write(pcm, offset, count, AudioTrack.WRITE_NON_BLOCKING)
        override fun pause() = track.pause()
        override fun play() = track.play()
        override fun close() { route?.close(); route = null; track.release() }
    }

    private var sink: Sink? = null
    private var closing = false
    private var pending: ShortArray? = null
    private var offset = 0
    private var padding = false
    private var padded = 0L
    private var previousHead = 0L
    private var headEpoch = 0L
    private var deviceHead = 0L
    private var stalledSince = -1L
    private var openedAt = 0L
    private var paused = false
    private val silence = ShortArray(960)
    var written = 0L
        private set
    var played = 0L
        private set
    val pendingFrames: Int get() = if (padding) 0 else ((pending?.size ?: 0) - offset) / 2
    val leadUs: Long get() = (written - played + pendingFrames) * 1_000_000 / 48000
    val drained: Boolean get() = pendingFrames == 0 && played == written
    val description: String get() = "written=$written played=$played pending=$pendingFrames padded=$padded " +
        "head=$deviceHead paused=$paused ${sink?.description ?: "device=closed"}"

    /** Не экстраполируем старый timestamp: при остановке устройства media clock также стоит. */
    private fun poll(now: Long) {
        val s = sink ?: return
        val raw = s.head
        if (raw < previousHead) {
            check(previousHead - raw > 0x80000000L) { "AudioTrack head regressed $previousHead->$raw; $description" }
            headEpoch += 0x100000000L
        }
        previousHead = raw
        val head = headEpoch + raw
        val first = deviceHead == 0L && head > 0
        if (head > deviceHead) {
            stalledSince = -1
        }
        deviceHead = head
        played = head.coerceAtMost(written)
        if (first) log("audio started delayMs=${(now - openedAt) / 1_000_000} $description")
    }

    /** Ровно одна неблокирующая запись за итерацию; ожидание устройства не задерживает video pump. */
    fun step(now: Long, hold: Boolean, pull: () -> ShortArray?, finished: () -> Boolean) {
        check(!closing) { "Stream audio output already closing" }
        poll(now)
        if (paused != hold) {
            if (hold) sink?.pause() else sink?.play()
            paused = hold; stalledSince = -1
            log("audio ${if (hold) "paused" else "resumed"} $description")
        }
        if (hold) return
        // Размер устройства уже учитывает его период обслуживания; 20ms могут быть меньше одного периода.
        if (pending == null && written - played < (sink?.bufferFrames ?: 1)) {
            val pcm = pull()
            if (pcm != null) {
                check(pcm.isNotEmpty() && pcm.size <= 960 && pcm.size % 2 == 0) { "Invalid stream PCM size=${pcm.size}" }
                if (sink == null) {
                    sink = open(); openedAt = now; sink!!.initialize(); sink!!.play()
                    log("audio opened ${sink!!.description}")
                }
                pending = pcm; offset = 0; padding = false
            }
        }
        val s = sink ?: return
        // Короткий EOF должен пересечь порог старых AudioTrack даже после underrun.
        // Тишина идёт только после всего PCM; played ограничен written и не сдвигает media PTS.
        if (pending == null && finished() && played < written && padded < s.threshold) {
            val frames = minOf(480L, s.threshold - padded).toInt()
            pending = if (frames == 480) silence else ShortArray(frames * 2)
            offset = 0; padding = true
            if (padded == 0L) log("audio draining threshold=${s.threshold} $description")
        }
        var blocked = false
        pending?.let { pcm ->
            val count = s.write(pcm, offset, pcm.size - offset)
            check(count >= 0 && count <= pcm.size - offset && count % 2 == 0) { "AudioTrack write=$count; $description" }
            blocked = count == 0
            offset += count
            if (padding) padded += count / 2 else written += count / 2
            if (offset == pcm.size) { pending = null; offset = 0; padding = false }
        }
        poll(now)
        // До достаточного заполнения ожидание сети не является зависанием устройства.
        val expectedProgress = written + padded - deviceHead >= s.threshold || blocked
        if (!expectedProgress || s.routeChanging) stalledSince = -1
        else if (stalledSince < 0) stalledSince = now
        else check(now - stalledSince < 2_000_000_000) {
            "AudioTrack made no playback progress for ${(now - stalledSince) / 1_000_000}ms; $description"
        }
    }

    fun close() {
        closing = true
        sink?.close()
        sink = null; pending = null; offset = 0; padding = false
    }
}
