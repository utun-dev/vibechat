package com.utun.chat.data

import android.Manifest
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import androidx.core.content.ContextCompat
import com.utun.chat.ChatApplication
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicReference
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * Запись голосового сообщения/тест микрофона и отдельный плеер native Opus-файла.
 * Запись и тест делят один native recorder: успешный voiceStart даёт владение до stop/cancel.
 * Capture lifecycle сериализован монитором менеджера; playback controls/callbacks имеют отдельный короткий lock.
 * Захват — PCM16 48 кГц mono, чтение без блокировки; stop сначала останавливает reader, ждёт join и освобождает Android.
 * Ошибка захвата/очистки запрещает публикацию частичной записи: stopRecording вернёт -1 и отменит native recorder.
 * После отказа worker владелец вызывает stop/cancel; неосвобождённый Android device сохраняется для повтора cleanup.
 * Плеер владеет своим decoder/AudioTrack в worker; прогресс/завершение — быстрые callbacks этого потока.
 * Pause/stop меняют поколение и прерывают worker, старое поколение больше не уведомляет UI.
 * Process registry и cleanup worker сохраняют владельцев до release ACK также после ViewModel.onCleared.
 * Stop core/запуск звонка ждут всех владельцев; file decoder освобождается независимо от отказа AudioTrack.
 */
class AudioRecorderManager {
    companion object {
        const val SAMPLE_RATE = 48000
        const val CHANNELS = 1
        const val FRAME_MS = 20
        const val FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000
        const val MIN_DURATION_MS = 500
        private val owners = ConcurrentHashMap.newKeySet<AudioRecorderManager>()

        /** Main: запросить независимое освобождение всех recorder/player перед звонком. Ожидать futures только на IO. */
        fun stopAll(): List<CompletableFuture<Unit>> = owners.toList().map { it.requestClose() }

        /** Main: незавершённая очистка запрещает разрушать native recorder; CoreShutdown повторяет проверку. */
        fun requireReleasedForCore() {
            val pending = stopAll()
            check(pending.all { it.isDone && !it.isCompletedExceptionally } && owners.isEmpty()) {
                LogManager.addLog("ERROR", "AudioRecorder", "Core teardown blocked: voice resource owners=${owners.size}")
                "Voice resource cleanup pending"
            }
        }
    }

    private enum class CaptureMode { RECORD, TEST }
    @Volatile private var captureMode: CaptureMode? = null
    @Volatile private var audioRecord: AudioRecord? = null
    @Volatile private var recordingThread: Thread? = null
    @Volatile private var captureRunning = false
    @Volatile private var captureFailed = false
    private val closing = AtomicReference<CompletableFuture<Unit>?>(null)
    private val captureLock = ReentrantLock()
    private val playbackWorkers = ConcurrentHashMap.newKeySet<Thread>()
    val isTesting: Boolean get() = captureMode == CaptureMode.TEST && captureRunning

    fun hasPermission(): Boolean {
        val ctx = ChatApplication.instance
        return ContextCompat.checkSelfPermission(ctx, Manifest.permission.RECORD_AUDIO) ==
               PackageManager.PERMISSION_GRANTED
    }

    /** channelId — канал либо dm:conv_id; replyTo относится к каналу, requestId подтверждает commit, generation — к PM.
     * true — Android/native захват запущен; false — отказ с диагностикой и откатом принадлежащих ресурсов. */
    fun startRecording(channelId: String, replyTo: String = "", requestId: Long = 0, generation: Long = 0, replyQuote: String = ""): Boolean {
        if (closing.get()?.isDone == false) { LogManager.addLog("WARN", "AudioRecorder", "record start rejected: cleanup pending"); return false }
        if (!captureLock.tryLock()) { LogManager.addLog("WARN", "AudioRecorder", "record start rejected: capture lifecycle busy"); return false }
        try { return startCapture(CaptureMode.RECORD, channelId, replyTo, requestId, generation, replyQuote) } finally { captureLock.unlock() }
    }

    private fun startCapture(mode: CaptureMode, target: String, reply: String = "", request: Long = 0, generation: Long = 0,
                             quote: String = ""): Boolean {
        if (captureMode != null) {
            LogManager.addLog("WARN", "AudioRecorder", "capture start rejected mode=$mode owned=$captureMode running=$captureRunning")
            return false
        }
        if (!releaseCaptureDevice()) return false
        try {
            if (!hasPermission()) {
                LogManager.addLog("WARN", "AudioRecorder", "microphone permission missing mode=$mode")
                return false
            }
            owners.add(this) // До JNI: stopForCore видит также запуск, ещё не закончивший подготовку устройства.
            if (!NativeLib.voiceStart(target, reply, request, generation, quote)) {
                LogManager.addLog("ERROR", "AudioRecorder", "native voiceStart rejected mode=$mode target=$target")
                forgetIfIdle()
                return false
            }
            captureMode = mode; captureFailed = false
            val minimum = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            check(minimum > 0) { "AudioRecord minimum=$minimum" }
            val bytes = minimum.coerceAtLeast(FRAME_SAMPLES * 2) * 2
            val device = AudioRecord(MediaRecorder.AudioSource.MIC, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, bytes)
            audioRecord = device
            check(device.state == AudioRecord.STATE_INITIALIZED) { "AudioRecord not initialized" }
            device.startRecording()
            check(device.recordingState == AudioRecord.RECORDSTATE_RECORDING) { "AudioRecord did not start" }
            captureRunning = true
            recordingThread = Thread({ captureLoop(device, mode) }, "utun-voice-capture").apply { priority = Thread.MAX_PRIORITY }
            recordingThread!!.start()
            LogManager.addLog("INFO", "AudioRecorder", "capture started mode=$mode target=$target bufferBytes=$bytes")
            return true
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "AudioRecorder", "capture start failed mode=$mode target=$target: $e")
            if (captureMode != null) { captureFailed = true; finishCapture(false) }
            forgetIfIdle()
            return false
        }
    }

    private fun captureLoop(device: AudioRecord, mode: CaptureMode) {
        val buffer = ShortArray(FRAME_SAMPLES)
        var total = 0L
        try {
            while (captureRunning) {
                check(device.recordingState == AudioRecord.RECORDSTATE_RECORDING) { "AudioRecord stopped unexpectedly" }
                val read = device.read(buffer, 0, buffer.size, AudioRecord.READ_NON_BLOCKING)
                if (!captureRunning) break
                check(read in 0..buffer.size) { "AudioRecord.read=$read" }
                if (read == 0) { Thread.sleep(2); continue }
                if (!NativeLib.voiceFeed(buffer.copyOf(read))) {
                    captureFailed = true
                    error("native voiceFeed rejected samples=$read")
                }
                total += read
            }
        } catch (e: Exception) {
            if (captureRunning || captureFailed) {
                captureFailed = true
                LogManager.addLog("ERROR", "AudioRecorder", "capture failed mode=$mode samples=$total: $e")
            }
        } finally {
            captureRunning = false
            LogManager.addLog("DEBUG", "AudioRecorder", "capture worker exited mode=$mode samples=$total failed=$captureFailed")
        }
    }

    /** Под монитором lifecycle: native освобождается только после join и попытки Android release. */
    private fun finishCapture(publish: Boolean): Int {
        val mode = captureMode ?: return 0
        captureRunning = false
        recordingThread?.interrupt()
        var failed = captureFailed
        var interrupted = Thread.interrupted()
        var nativeReleased = false
        try {
            try { audioRecord?.stop() }
            catch (e: Exception) { failed = true; LogManager.addLog("WARN", "AudioRecorder", "capture stop failed mode=$mode: $e") }
            while (recordingThread?.isAlive == true) {
                try { recordingThread?.join() }
                catch (e: InterruptedException) {
                    interrupted = true
                    LogManager.addLog("WARN", "AudioRecorder", "capture join interrupted mode=$mode; waiting for worker")
                }
            }
            recordingThread = null
            failed = failed || captureFailed
            if (!releaseCaptureDevice()) failed = true
            val duration = if (publish && !failed) NativeLib.voiceStop().also { if (it < 0) NativeLib.voiceCancel() }
                else { NativeLib.voiceCancel(); if (publish) -1 else 0 }
            nativeReleased = true
            LogManager.addLog(if (duration < 0) "ERROR" else "INFO", "AudioRecorder",
                "capture finished mode=$mode publish=$publish failed=$failed durationMs=$duration")
            return duration
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "AudioRecorder", "native capture finish failed mode=$mode: $e")
            try { NativeLib.voiceCancel(); nativeReleased = true }
            catch (cleanup: Exception) { LogManager.addLog("ERROR", "AudioRecorder", "native capture cancel failed: $cleanup") }
            return -1
        } finally {
            if (nativeReleased) captureMode = null else captureFailed = true
            forgetIfIdle()
            if (!nativeReleased || audioRecord != null) requestClose()
            if (interrupted) Thread.currentThread().interrupt()
        }
    }

    /** Под монитором lifecycle; отказ release сохраняет владение для следующего cleanup/start. */
    private fun releaseCaptureDevice(): Boolean {
        val device = audioRecord ?: return true
        return try {
            device.release(); audioRecord = null
            LogManager.addLog("DEBUG", "AudioRecorder", "capture device released mode=$captureMode")
            true
        } catch (e: Exception) {
            LogManager.addLog("WARN", "AudioRecorder", "capture release failed mode=$captureMode; retained for retry: $e")
            false
        }
    }

    /** Длительность native в мс, -1 — отказ/запись отброшена после ошибки, 0 — нет записи.
     * Успех означает передачу подготовки в native, публикация сообщения подтверждается отдельно событиями. */
    fun stopRecording(): Int = captureLock.withLock {
        if (captureMode == CaptureMode.RECORD) return@withLock finishCapture(true)
        if (captureMode == null) releaseCaptureDevice()
        0
    }
    fun cancelRecording() {
        if (closing.get()?.isDone == false) return
        if (!captureLock.tryLock()) { if (captureMode == CaptureMode.RECORD) requestClose(); return }
        try { if (captureMode == CaptureMode.RECORD) finishCapture(false) else if (captureMode == null) releaseCaptureDevice() }
        finally { captureLock.unlock() }
    }
    fun isActive(): Boolean = captureMode == CaptureMode.RECORD && captureRunning

    /** Индикатор уровня через native recorder; успешный тест нужно завершить stopMicTest без публикации. */
    fun startMicTest(): Boolean {
        if (closing.get()?.isDone == false) { LogManager.addLog("WARN", "AudioRecorder", "microphone test rejected: cleanup pending"); return false }
        if (StreamController.blocksCapture()) {
            LogManager.addLog("WARN", "AudioRecorder", "microphone test rejected: stream owns capture")
            return false
        }
        if (!captureLock.tryLock()) { LogManager.addLog("WARN", "AudioRecorder", "microphone test rejected: capture lifecycle busy"); return false }
        try { return if (isTesting) true else startCapture(CaptureMode.TEST, "__test__") } finally { captureLock.unlock() }
    }
    fun stopMicTest() {
        if (closing.get()?.isDone == false) return
        if (!captureLock.tryLock()) { if (captureMode == CaptureMode.TEST) requestClose(); return }
        try { if (captureMode == CaptureMode.TEST) finishCapture(false) else if (captureMode == null) releaseCaptureDevice() }
        finally { captureLock.unlock() }
    }

    /** Dispose UI: остановить только принадлежащий тесту захват, не затрагивая запись сообщения/плеер без теста. */
    fun requestMicTestStop(): CompletableFuture<Unit> =
        if (captureMode == CaptureMode.TEST) requestClose() else CompletableFuture.completedFuture(Unit)

    @Volatile private var playing = false
    private val playbackLock = Any()
    private var playThread: Thread? = null
    @Volatile private var playGeneration = 0

    private fun forgetIfIdle() = captureLock.withLock {
        if (captureMode == null && audioRecord == null && playbackWorkers.isEmpty()) owners.remove(this)
    }

    /** Не ждёт SDK на caller: process-владелец повторяет stop/release до подтверждения и переживает ViewModel.onCleared. */
    fun requestClose(): CompletableFuture<Unit> {
        val result: CompletableFuture<Unit>
        while (true) {
            val previous = closing.get()
            if (previous != null && !previous.isDone) return previous
            val next = object : CompletableFuture<Unit>() { override fun cancel(mayInterruptIfRunning: Boolean): Boolean = false }
            if (closing.compareAndSet(previous, next)) { result = next; break }
        }
        captureRunning = false; recordingThread?.interrupt(); stopPlayback()
        try {
            Thread({
                var retryMs = 250L
                try {
                    while (true) {
                        val released = captureLock.withLock {
                            if (captureMode != null) finishCapture(false) else releaseCaptureDevice()
                            stopPlayback()
                            captureMode == null && audioRecord == null && playbackWorkers.isEmpty()
                        }
                        if (released) {
                            forgetIfIdle(); result.complete(Unit)
                            LogManager.addLog("INFO", "AudioRecorder", "Voice resources released owner=${System.identityHashCode(this)}")
                            break
                        }
                        LogManager.addLog("WARN", "AudioRecorder", "Voice cleanup pending owner=${System.identityHashCode(this)} retryMs=$retryMs")
                        Thread.sleep(retryMs); retryMs = (retryMs * 2).coerceAtMost(30_000)
                    }
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "AudioRecorder", "Voice cleanup worker failed; owner retained: $e")
                    result.completeExceptionally(e)
                }
            }, "utun-voice-cleanup").start()
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "AudioRecorder", "Voice cleanup worker start failed; owner retained: $e")
            result.completeExceptionally(e)
        }
        return result
    }

    /** Запустить с начала; callbacks приходят из worker только для текущего поколения, completion также при ошибке. */
    fun playVoiceFile(filePath: String, onProgress: ((Float, Int) -> Unit)? = null, onComplete: () -> Unit = {}) {
        playFromPosition(filePath, 0, onProgress, onComplete)
    }

    /** Запустить со startMs, округлённого вниз до 20 мс; seek декодирует и отбрасывает предшествующий PCM.
     * Отрицательный startMs ограничивается нулём; прогресс учитывает только принятые AudioTrack сэмплы. */
    fun playFromPosition(filePath: String, startMs: Int, onProgress: ((Float, Int) -> Unit)? = null, onComplete: () -> Unit = {}) {
        if (closing.get()?.isDone == false) { LogManager.addLog("WARN", "AudioPlayer", "play rejected: cleanup pending"); return }
        synchronized(playbackLock) {
            stopPlayback()
            playing = true
            val myGen = playGeneration

            val worker = Thread {
                var handle = 0L
                var track: AudioTrack? = null
                try {
                    val info = IntArray(3)
                    handle = NativeLib.voiceDecodeOpen(filePath, info)
                    if (handle == 0L) {
                        LogManager.addLog("ERROR", "AudioPlayer", "decode open failed: $filePath")
                        return@Thread
                    }
                    val sampleRate = info[0]; val channels = info[1]; val totalSamples = info[2]
                    check(sampleRate in listOf(8000, 12000, 16000, 24000, 48000) && channels in 1..2 && totalSamples > 0) {
                        "invalid decode info rate=$sampleRate channels=$channels samples=$totalSamples"
                    }

                    // Seek выполняется по полным 20мс кадрам; позиция берётся из фактически пропущенных сэмплов.
                    val frameSamples = sampleRate / 50 * channels
                    val skipFrames = startMs.coerceAtLeast(0) / FRAME_MS
                    val skipBuf = ShortArray(frameSamples)
                    var skippedSamples = 0L
                    for (f in 0 until skipFrames) {
                        if (myGen != playGeneration) return@Thread
                        val dec = NativeLib.voiceDecodeRead(handle, skipBuf)
                        check(dec in 0..skipBuf.size) { "decode seek failed result=$dec" }
                        if (dec == 0) break
                        skippedSamples += dec
                    }

                    val bufSize = AudioTrack.getMinBufferSize(sampleRate,
                        if (channels == 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO,
                        AudioFormat.ENCODING_PCM_16BIT)
                    check(bufSize > 0) { "AudioTrack minimum=$bufSize" }

                    val output = AudioTrack(AudioManager.STREAM_MUSIC, sampleRate,
                        if (channels == 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO,
                        AudioFormat.ENCODING_PCM_16BIT, bufSize, AudioTrack.MODE_STREAM)
                    track = output
                    check(output.state == AudioTrack.STATE_INITIALIZED) { "AudioTrack not initialized" }
                    output.play()
                    LogManager.addLog("INFO", "AudioPlayer", "playback started generation=$myGen rate=$sampleRate channels=$channels " +
                        "skippedSamples=$skippedSamples totalSamples=$totalSamples")

                    val pcm = ShortArray(frameSamples)
                    var readTotal = 0L
                    var read: Int
                    while (playing && myGen == playGeneration) {
                        read = NativeLib.voiceDecodeRead(handle, pcm)
                        check(read in 0..pcm.size) { "decode read failed result=$read" }
                        if (read == 0) break
                        if (myGen != playGeneration) break
                        var offset = 0
                        while (playing && myGen == playGeneration && offset < read) {
                            val written = output.write(pcm, offset, read - offset)
                            check(written in 1..(read - offset)) { "AudioTrack.write=$written remaining=${read - offset}" }
                            offset += written; readTotal += written
                        }
                        val total = (skippedSamples + readTotal).toFloat() / totalSamples.coerceAtLeast(1).toFloat()
                        val eMs = ((skippedSamples + readTotal) * 1000 / (sampleRate * channels)).toInt()
                        synchronized(playbackLock) {
                            if (myGen == playGeneration) onProgress?.invoke(total, eMs)
                        }
                    }
                    val drainMs = (bufSize * 1000L) / (sampleRate.toLong() * channels * 2) + 100
                    if (myGen == playGeneration) Thread.sleep(drainMs)
                } catch (_: InterruptedException) {
                    LogManager.addLog("DEBUG", "AudioPlayer", "playback cancelled")
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "AudioPlayer", "play error: ${e.message}")
                } finally {
                    var retryMs = 250L
                    var notified = false
                    var interrupted = Thread.interrupted()
                    while (true) {
                        track?.let { output ->
                            try { output.stop() } catch (e: Exception) { LogManager.addLog("WARN", "AudioPlayer", "stop failed: $e") }
                            try { output.release(); track = null }
                            catch (e: Exception) { LogManager.addLog("ERROR", "AudioPlayer", "release failed; worker retains output: $e") }
                        }
                        try { if (handle != 0L) { NativeLib.voiceDecodeClose(handle); handle = 0 } }
                        catch (e: Exception) { LogManager.addLog("ERROR", "AudioPlayer", "decode close failed generation=$myGen; retained: $e") }
                        if (!notified) {
                            synchronized(playbackLock) {
                                if (playGeneration == myGen) {
                                    playing = false; playThread = null
                                    try { onComplete() }
                                    catch (e: Exception) { LogManager.addLog("ERROR", "AudioPlayer", "completion callback failed generation=$myGen: $e") }
                                }
                            }
                            notified = true
                        }
                        if (track == null && handle == 0L) break
                        try { Thread.sleep(retryMs) } catch (_: InterruptedException) { interrupted = true }
                        retryMs = (retryMs * 2).coerceAtMost(30_000)
                    }
                    playbackWorkers.remove(Thread.currentThread()); forgetIfIdle()
                    if (interrupted) Thread.currentThread().interrupt()
                    LogManager.addLog("DEBUG", "AudioPlayer", "playback worker exited generation=$myGen current=$playGeneration")
                }
            }
            playThread = worker; playbackWorkers.add(worker); owners.add(this)
            try { worker.start() }
            catch (e: Exception) {
                playbackWorkers.remove(worker); playThread = null; playing = false
                LogManager.addLog("ERROR", "AudioPlayer", "Playback worker start failed: $e")
                try { onComplete() }
                catch (callback: Exception) { LogManager.addLog("ERROR", "AudioPlayer", "Startup failure callback failed: $callback") }
                requestClose()
            }
        }
    }

    /** Асинхронно остановить текущий worker; позицию для следующего playFromPosition хранит вызывающий код. */
    fun pausePlayback() = synchronized(playbackLock) {
        playGeneration++; playing = false
        playThread?.interrupt(); playThread = null
    }

    /** Инвалидировать callbacks и прервать worker; собственные AudioTrack/decoder он освобождает в finally. */
    fun stopPlayback() = synchronized(playbackLock) {
        playGeneration++; playing = false
        playThread?.interrupt(); playThread = null
    }
}
