package com.utun.chat.data

import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTimestamp
import android.media.MediaRecorder
import android.os.Build
import java.util.concurrent.locks.LockSupport

/**
 * Один AudioRecord, 48 kHz, кадры 20 мс stereo (mono дублируется в два канала).
 * Владелец закрывает reader перед сменой маршрута, вне accept/failed/confirmed callbacks.
 * NON_BLOCKING read позволяет остановиться и при отказе stop; отсутствие полного кадра ограничено deadline.
 * Публикуется только PCM выбранного, не silenced входа со свежим monotonic timestamp.
 * close сериализован и идемпотентен; прерванный join завершается до release, затем флаг interrupt восстанавливается.
 */
internal class AndroidMicrophoneCapture private constructor(
    private val record: AudioRecord, private val input: AudioDeviceInfo, private val generation: Long,
    private val accept: (MicrophoneCapture.Frame) -> Boolean, private val failed: (Exception) -> Unit,
    private val confirmed: () -> Unit, private val nowNs: () -> Long
) : AutoCloseable {
    @Volatile private var running = true
    private val worker = Thread(::read, "media-microphone")
    private val closeLock = Any()
    private var stopRequested = false
    private var released = false

    companion object {
        /**
         * Открыть выбранный input: voice принудительно mono/VOICE_COMMUNICATION, stereo разрешает fallback в mono.
         * micSource: 0 auto, 1 MIC, 2 UNPROCESSED, 3 CAMCORDER. generation передаётся в каждый Frame без изменения.
         * accept возвращает false при переполнении/старом PCM; confirmed вызывается один раз после первого принятого кадра.
         * failed сообщает ошибку worker. nowNs использует ту же monotonic шкалу, что AudioTimestamp.
         */
        fun open(input: AudioDeviceInfo, voice: Boolean, stereo: Boolean, micSource: Int, generation: Long,
                 accept: (MicrophoneCapture.Frame) -> Boolean, failed: (Exception) -> Unit,
                 confirmed: () -> Unit, nowNs: () -> Long = System::nanoTime): AndroidMicrophoneCapture {
            val source = if (voice) MediaRecorder.AudioSource.VOICE_COMMUNICATION else when (micSource) {
                1 -> MediaRecorder.AudioSource.MIC
                2 -> MediaRecorder.AudioSource.UNPROCESSED
                3 -> MediaRecorder.AudioSource.CAMCORDER
                else -> if (stereo) MediaRecorder.AudioSource.CAMCORDER else MediaRecorder.AudioSource.VOICE_COMMUNICATION
            }
            val masks = if (stereo && !voice) intArrayOf(AudioFormat.CHANNEL_IN_STEREO, AudioFormat.CHANNEL_IN_MONO)
                else intArrayOf(AudioFormat.CHANNEL_IN_MONO)
            var failure: Exception? = null
            for (mask in masks) {
                var record: AudioRecord? = null
                try {
                    val min = AudioRecord.getMinBufferSize(48000, mask, AudioFormat.ENCODING_PCM_16BIT)
                    check(min > 0) { "Unsupported capture mask=$mask min=$min" }
                    record = AudioRecord(source, 48000, mask, AudioFormat.ENCODING_PCM_16BIT, maxOf(min * 2, 15360))
                    check(record.state == AudioRecord.STATE_INITIALIZED) { "AudioRecord not initialized mask=$mask" }
                    check(record.channelCount in 1..2) { "Unsupported capture channels=${record.channelCount}" }
                    check(record.setPreferredDevice(input)) { "Input selection rejected id=${input.id}" }
                    record.startRecording()
                    check(record.recordingState == AudioRecord.RECORDSTATE_RECORDING) { "AudioRecord did not start" }
                    LogManager.addLog("INFO", "media_audio", "capture opened generation=$generation input=${input.id}/${input.type} " +
                        "name=${input.productName} source=$source channels=${record.channelCount} rate=48000")
                    return AndroidMicrophoneCapture(record, input, generation, accept, failed, confirmed, nowNs).also { it.worker.start() }
                } catch (e: Exception) {
                    failure = e
                    LogManager.addLog("WARN", "media_audio", "capture open failed input=${input.id} mask=$mask: $e")
                    try { record?.release() } catch (cleanup: Exception) {
                        e.addSuppressed(cleanup)
                        LogManager.addLog("ERROR", "media_audio", "capture open cleanup failed input=${input.id} mask=$mask: $cleanup")
                        throw IllegalStateException("Cannot release failed capture input=${input.id}; fallback aborted", e)
                    }
                }
            }
            throw IllegalStateException("Cannot open microphone ${input.productName}", failure)
        }
    }

    private fun read() {
        try {
            val stamp = AudioTimestamp()
            val channels = record.channelCount
            var total = 0L
            var validSince = 0L
            var ready = false
            var lateSince = 0L
            var timestampMissingSince = 0L
            val startedNs = nowNs()
            val deadline = startedNs + 5_000_000_000L
            var reportedSignal = false
            var reportAtMs = 0L
            var lastFrameNs = nowNs()
            try { android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO) } catch (e: Exception) {
                LogManager.addLog("WARN", "media_audio", "capture audio priority unavailable input=${input.id}: $e")
            }
            while (running) {
                val pcm = ShortArray(960 * channels)
                var used = 0
                val before = record.routedDevice?.id
                while (running && used < pcm.size) {
                    check(!Thread.currentThread().isInterrupted) { "Capture reader interrupted input=${input.id}" }
                    val now = nowNs()
                    check(now < if (ready) lastFrameNs + 1_000_000_000L else deadline) {
                        "Microphone data timeout input=${input.id} ready=$ready samples=$used/${pcm.size}"
                    }
                    val count = record.read(pcm, used, pcm.size - used, AudioRecord.READ_NON_BLOCKING)
                    if (!running) break
                    check(count >= 0 && count <= pcm.size - used && count % channels == 0) {
                        "AudioRecord read=$count input=${input.id} remaining=${pcm.size - used} channels=$channels"
                    }
                    if (count == 0) LockSupport.parkNanos(this, 5_000_000L) else used += count
                }
                if (!running) break
                lastFrameNs = nowNs()
                val first = total; total += 960
                val actual = record.routedDevice?.id
                val silenced = Build.VERSION.SDK_INT >= 29 && record.activeRecordingConfiguration?.isClientSilenced == true
                val valid = before == input.id && actual == input.id && !silenced
                if (!valid) {
                    if (ready) error("Microphone route lost selected=${input.id} actual=$actual silenced=$silenced")
                    validSince = 0
                } else if (validSince == 0L) validSince = nowNs()
                val measured = record.getTimestamp(stamp, AudioTimestamp.TIMEBASE_MONOTONIC) == AudioRecord.SUCCESS
                val time = if (measured) stamp.nanoTime - (stamp.framePosition - first) * 1_000_000_000 / 48000 else 0L
                val now = nowNs()
                val elapsedMs = (now - startedNs) / 1_000_000
                val peak = pcm.maxOf { kotlin.math.abs(it.toInt()) }
                if (elapsedMs >= reportAtMs && reportAtMs <= 2000 || (!reportedSignal && peak > 0)) {
                    val rms = kotlin.math.sqrt(pcm.sumOf { it.toDouble() * it } / pcm.size)
                    LogManager.addLog("DEBUG", "media_audio", "capture startup generation=$generation elapsedMs=$elapsedMs " +
                        "sequence=$first peak=$peak rms=${rms.toInt()} valid=$valid measured=$measured " +
                        "ageUs=${(now - time) / 1000} ready=$ready")
                    reportAtMs = if (elapsedMs < 500) 500 else if (elapsedMs < 1000) 1000 else if (elapsedMs < 2000) 2000 else 3000
                }
                if (peak > 0) reportedSignal = true
                check(ready || now < deadline) { "Microphone confirmation timeout input=${input.id} now=$now deadline=$deadline" }
                if (!valid || !measured || stamp.nanoTime <= 0 || stamp.framePosition < 0 || time < validSince || time > now) {
                    if (timestampMissingSince == 0L) {
                        timestampMissingSince = now
                        LogManager.addLog(if (ready) "WARN" else "DEBUG", "media_audio",
                            "capture waiting metadata input=${input.id} actual=$actual silenced=$silenced ready=$ready " +
                                "measured=$measured position=${stamp.framePosition} timestamp=$time validSince=$validSince now=$now")
                    }
                    check(now < if (ready) timestampMissingSince + 1_000_000_000L else deadline) {
                        "Microphone confirmation timeout selected=${input.id} actual=$actual silenced=$silenced " +
                            "ready=$ready measured=$measured position=${stamp.framePosition} timestamp=$time now=$now"
                    }
                    continue
                }
                timestampMissingSince = 0
                val stereo = if (channels == 2) pcm else ShortArray(1920) { pcm[it / 2] }
                val accepted = accept(MicrophoneCapture.Frame(stereo, time, first, generation = generation))
                if (!accepted) {
                    if (lateSince == 0L) lateSince = nowNs()
                    check(nowNs() - lateSince < 1_000_000_000L) { "Microphone PCM exceeds capture reserve input=${input.id}" }
                    continue
                }
                lateSince = 0
                if (!ready) {
                    ready = true
                    LogManager.addLog("INFO", "media_audio", "capture confirmed generation=$generation input=$actual timeNs=$time")
                    confirmed()
                }
            }
        } catch (e: Exception) {
            if (running) {
                running = false
                LogManager.addLog("ERROR", "media_audio", "capture failed generation=$generation input=${input.id}: $e")
                try { failed(e) } catch (callback: Exception) {
                    LogManager.addLog("ERROR", "media_audio", "capture failure callback threw input=${input.id}: $callback")
                }
            }
        }
    }

    override fun close() {
        check(Thread.currentThread() !== worker) {
            LogManager.addLog("ERROR", "media_audio", "capture close from reader rejected input=${input.id}")
            "Capture cannot join its own reader"
        }
        synchronized(closeLock) {
            if (released) return
            running = false
            LockSupport.unpark(worker)
            if (!stopRequested) {
                stopRequested = true
                try { record.stop() } catch (e: Exception) {
                    LogManager.addLog("WARN", "media_audio", "capture stop input=${input.id}: $e")
                }
            }
            var interrupted = false
            while (worker.isAlive) try { worker.join() } catch (e: InterruptedException) {
                interrupted = true
                LogManager.addLog("WARN", "media_audio", "capture join interrupted input=${input.id}; finishing cleanup")
            }
            try {
                record.release()
                released = true
                LogManager.addLog("INFO", "media_audio", "capture released generation=$generation input=${input.id}")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "media_audio", "capture release failed input=${input.id}; close can retry: $e")
                throw e
            } finally { if (interrupted) Thread.currentThread().interrupt() }
        }
    }
}
