package com.utun.chat.data

/** Один AudioTrack для всех трансляций. Mixer только копирует и смешивает готовый PCM.
 * Input закрывается после исключения из списка; его короткий lock защищает consumer от release.
 * Выход принадлежит worker до успешного close; новые inputs ждут cleanup, не открывая второй AudioTrack.
 */
internal object StreamAudioMixer {
    data class OutputState(val played: Long = 0, val leadUs: Long = 0)
    @Volatile var output = OutputState()
        private set
    private val lock = Any()
    private val inputs = linkedSetOf<StreamMixedPlayback>()
    private var snapshot = emptyList<StreamMixedPlayback>()
    private var worker: Thread? = null

    fun attach(input: StreamMixedPlayback) = synchronized(lock) {
        check(inputs.add(input))
        snapshot = inputs.toList()
        if (worker == null) {
            output = OutputState()
            val next = Thread(::run, "stream-mixer"); worker = next
            try { next.start() } catch (e: Exception) {
                worker = null; inputs.remove(input); snapshot = inputs.toList()
                LogManager.addLog("ERROR", "stream", "mixer worker start failed input=${input.id}; registration rolled back: $e")
                throw e
            }
        }
        LogManager.addLog("INFO", "stream", "mixer input attached id=${input.id} count=${inputs.size}")
    }

    fun detach(input: StreamMixedPlayback) = synchronized(lock) {
        inputs.remove(input)
        snapshot = inputs.toList()
        LogManager.addLog("INFO", "stream", "mixer input detached id=${input.id} count=${inputs.size}")
    }

    /** После join всех viewers, вне main thread; запуск новых viewers блокирует CallController. */
    fun awaitIdle() {
        val closing = synchronized(lock) {
            check(inputs.isEmpty()) { "Stream mixer still has ${inputs.size} inputs after viewer release" }
            worker
        }
        check(closing !== Thread.currentThread()) { "Mixer cannot join itself" }
        closing?.join(250)
        check(closing?.isAlive != true) {
            LogManager.addLog("ERROR", "stream", "mixer output cleanup pending; worker retains device")
            "Mixer output cleanup pending"
        }
        LogManager.addLog("INFO", "stream", "shared audio output released for call")
    }

    /** Проверка core teardown без join: живой worker удерживает Android output для retry. */
    fun requireReleased() = synchronized(lock) {
        check(inputs.isEmpty() && worker?.isAlive != true) {
            LogManager.addLog("ERROR", "stream", "core teardown blocked by mixer inputs=${inputs.size} worker=${worker?.state}")
            "Stream mixer cleanup pending"
        }
    }

    private fun run() {
        var audio: StreamAudioOutput? = null
        var generated = 0L
        try {
            val device = StreamAudioOutput(log = { LogManager.addLog("INFO", "stream", "mixer $it") }); audio = device
            val mix = StreamMixBuffer()
            try { android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO) }
            catch (e: Exception) { LogManager.addLog("WARN", "stream", "mixer audio priority unavailable: ${e.message}") }
            LogManager.addLog("INFO", "stream", "mixer started: ready PCM only, blockFrames=${mix.frames}")
            while (true) {
                val current = synchronized(lock) {
                    if (inputs.isEmpty()) null else snapshot
                } ?: break
                device.step(System.nanoTime(), hold = false, pull = {
                    mix.clear()
                    var active = false
                    for (input in current) active = input.mix(mix, generated) || active
                    if (active) {
                        generated += mix.frames
                        mix.finish()
                    } else null
                }, finished = { false })
                output = OutputState(device.played, device.leadUs)
                Thread.sleep(5)
            }
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "stream", "shared audio output failed: $e")
            synchronized(lock) {
                for (input in inputs) input.outputFailure = e.message ?: "Audio output failed"
                inputs.clear()
                snapshot = emptyList()
            }
        } finally {
            var delay = 250L
            if (Thread.interrupted()) LogManager.addLog("WARN", "stream", "mixer interrupted before cleanup; finishing device release")
            while (true) try { audio?.close(); break } catch (e: Exception) {
                LogManager.addLog("ERROR", "stream", "mixer output release failed; owner retained delayMs=$delay: $e")
                try { Thread.sleep(delay) } catch (interrupted: InterruptedException) {
                    LogManager.addLog("WARN", "stream", "mixer cleanup wait interrupted; retrying")
                }
                delay = (delay * 2).coerceAtMost(30_000)
            }
            synchronized(lock) {
                output = OutputState(); worker = null
                // Новые inputs могли прийти во время cleanup; до SDK ACK второго устройства не было.
                if (inputs.isNotEmpty()) {
                    val next = Thread(::run, "stream-mixer"); worker = next
                    try { next.start() } catch (e: Exception) {
                        worker = null; inputs.forEach { it.outputFailure = "Mixer worker start failed: $e" }
                        inputs.clear(); snapshot = emptyList()
                        LogManager.addLog("ERROR", "stream", "mixer restart after cleanup failed: $e")
                    }
                }
            }
            LogManager.addLog("INFO", "stream", "shared audio output cleanup confirmed")
        }
    }
}

/** Независимые Opus/SoundTouch, темп и PTS. Audio worker готовит PCM вне lock; mixer потребляет SPSC.
 * Каждый 10ms блок общего выхода отражён в своей шкале, включая тишину при starvation.
 * Reset создаёт новый Input с новой origin; AudioTrack и остальные источники продолжают играть. */
internal class StreamMixedPlayback(val id: Long, pts: Long, private val audioEnabled: Boolean, private val wake: () -> Unit) {
    data class Diagnostics(val pcmFrames: Int, val underruns: Long, val silenceFrames: Long)
    private val lock = Any()
    private val initialPts = pts
    private var handle = 0L
    private val timeline = StreamMixTimeline()
    private var started = false
    private var ended = false
    private var endFrame: Long? = null
    private var fadeFrames = 0
    private val pcm = ShortArray(960)
    private var underruns = 0L
    private var silentFrames = 0L
    private var observedUnderruns = 0L
    @Volatile var outputFailure: String? = null
    /** Владелец сохраняет пассивный объект до native create и регистрации mixer input. */
    fun start() {
        check(handle == 0L) { "Mixed playback already started id=$id" }
        handle = NativeLib.streamPlaybackCreate(id, initialPts, audioEnabled)
        check(handle != 0L) { "Stream playback create failed id=$id" }
        if (audioEnabled) StreamAudioMixer.attach(this)
    }

    val leadUs: Long get() = if (audioEnabled) StreamAudioMixer.output.leadUs else 0
    val empty: Boolean get() = synchronized(lock) { handle == 0L || NativeLib.streamPlaybackEmpty(handle) }
    val needsInput: Boolean get() = synchronized(lock) { handle != 0L && NativeLib.streamPlaybackNeedsInput(handle) }
    val drained: Boolean get() = synchronized(lock) {
        !audioEnabled || (ended && (endFrame?.let { timeline.played(StreamAudioMixer.output.played) >= it }
            ?: (timeline.frames == 0L && NativeLib.streamPlaybackEmpty(handle))))
    }
    val description: String get() = synchronized(lock) {
        "mixed=$audioEnabled origin=${timeline.origin} written=${timeline.frames} " +
            "played=${timeline.played(StreamAudioMixer.output.played)} leadUs=$leadUs " +
            "pcmFrames=${NativeLib.streamPlaybackBuffered(handle)} underruns=$underruns silentFrames=$silentFrames"
    }
    val diagnostics: Diagnostics get() = synchronized(lock) {
        Diagnostics(NativeLib.streamPlaybackBuffered(handle), underruns, silentFrames)
    }

    // Только audio worker; он же закрывает handle, поэтому DSP не нуждается в consumer lock.
    fun prepare(): Int { check(handle != 0L); return NativeLib.streamPlaybackPrepare(handle) }
    fun feed(pts: Long, packet: ByteArray): Int {
        check(handle != 0L); return NativeLib.streamPlaybackFeed(handle, pts, packet)
    }
    fun finish() {
        synchronized(lock) { check(handle != 0L); ended = true }
        NativeLib.streamPlaybackFinish(handle)
    }

    fun update(now: Long, available: Long, guard: Long, sourceFinished: Boolean = false): LongArray? = synchronized(lock) {
        check(handle != 0L); check(outputFailure == null) { outputFailure!! }
        started = true
        val starved = underruns != observedUnderruns
        observedUnderruns = underruns
        NativeLib.streamPlaybackUpdate(handle, now, timeline.played(StreamAudioMixer.output.played), available, guard, starved, sourceFinished)
    }

    /** Mixer не блокирует чужой input ожиданием media; недостающее заменяется тишиной только этого источника. */
    fun mix(buffer: StreamMixBuffer, firstOutputFrame: Long): Boolean = synchronized(lock) {
        if (handle == 0L || !started || !audioEnabled || outputFailure != null) return false
        try {
            val buffering = NativeLib.streamPlaybackBuffering(handle)
            val count = NativeLib.streamPlaybackPull(handle, pcm, buffer.frames, Long.MAX_VALUE)
            check(count >= 0) { "Ready PCM pull failed" }
            fadeFrames = buffer.add(pcm, fadeFrames, count)
            if (ended && endFrame == null && NativeLib.streamPlaybackEmpty(handle)) endFrame = timeline.frames + count
            if (count < buffer.frames) {
                NativeLib.streamPlaybackSilence(handle, buffer.frames - count)
                if (!ended && !buffering) { underruns++; silentFrames += buffer.frames - count }
            }
            timeline.append(firstOutputFrame, buffer.frames)
            wake()
            true
        } catch (e: Exception) {
            outputFailure = e.message ?: "Audio mixing failed"
            LogManager.addLog("ERROR", "stream", "mixer input failed id=$id: ${e.message}")
            try { wake() } catch (notification: Exception) {
                LogManager.addLog("ERROR", "stream", "mixer failure notification failed id=$id: $notification")
            }
            false
        }
    }

    fun close() {
        if (audioEnabled) StreamAudioMixer.detach(this)
        synchronized(lock) {
            if (handle != 0L) NativeLib.streamPlaybackClose(handle)
            handle = 0
        }
    }
}
