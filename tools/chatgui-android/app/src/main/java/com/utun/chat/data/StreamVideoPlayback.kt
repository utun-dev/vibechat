package com.utun.chat.data

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.Surface
import java.util.ArrayDeque

/** MediaCodec и pump принадлежат только video worker. Audio worker передаёт кадры через ограниченный
 * mailbox; владелец сохраняет пассивный объект до start. reset меняет поколение.
 * close ждёт worker до 250 мс и подтверждает release; ошибка сохраняет decoder для повторного close.
 * Ни один вызов MediaCodec не выполняется под mailbox lock или на потоке подготовки звука. */
internal class StreamVideoPlayback(
    private val id: Long, private val surface: Surface, private val requestedAtUs: Long,
    private val wake: () -> Unit, private val onRendered: () -> Unit
) {
    class UnsupportedCodec(message: String) : IllegalStateException(message)
    data class Frame(override val pts: Long, val key: Boolean, val width: Int, val height: Int,
                     val rotation: Int, val revision: Long, val codec: StreamVideoCodec,
                     val config: ByteArray, val data: ByteArray) : StreamDecodePump.Frame
    data class State(
        val firstOutputPts: Long? = null, val ended: Boolean = false, val inputs: Long = 0,
        val decoded: Long = 0, val shown: Long = 0, val discarded: Long = 0, val inflight: Int = 0,
        val decoderLeadUs: Long = 0, val shownPts: Long = -1, val outputPts: Long? = null
    )
    private data class Clock(val position: Long = -1, val ready: Long = -1, val leadUs: Long = 0)
    private val lock = Object()
    private val input = ArrayDeque<Frame>()
    @Volatile private var running = true
    @Volatile private var generation = 0L
    @Volatile private var clock = Clock()
    @Volatile var state = State()
        private set
    @Volatile var failure: Exception? = null
        private set
    private var inputEnded = false // mailbox lock
    private var activeGeneration = -1L // Далее поля только video worker.
    private var decoderResources: VideoCodecResources? = null
    private val decoder: MediaCodec? get() = decoderResources?.codec
    private var video: StreamDecodePump<Frame> = createVideoPump()
    private val info = MediaCodec.BufferInfo()
    private var videoRevision = 0L
    private var firstInputAtUs = 0L
    private var firstShown = false
    private var lastShownPts = -1L
    private var eosQueued = false
    private val main = Handler(Looper.getMainLooper())
    private val position: Long get() = clock.position
    private val ready: Long get() = clock.ready
    private val worker = Thread(::run, "stream-video-$id")

    fun start() = worker.start()

    val wantsInput: Boolean get() = synchronized(lock) { running && !inputEnded && input.size < 2 }

    fun offer(frame: Frame) = synchronized(lock) {
        check(running && !inputEnded && input.size < 2) { "Video mailbox full or closed id=$id size=${input.size}" }
        input.addLast(frame); lock.notifyAll()
    }

    fun update(position: Long, ready: Long, leadUs: Long) {
        clock = Clock(position, ready, leadUs)
        synchronized(lock) { lock.notifyAll() }
    }

    fun finish() = synchronized(lock) { inputEnded = true; lock.notifyAll() }

    fun reset() = synchronized(lock) {
        generation++; input.clear(); inputEnded = false; state = State(); clock = Clock()
        lock.notifyAll()
        LogManager.addLog("INFO", "video", "viewer video reset id=$id generation=$generation")
    }

    fun close() {
        synchronized(lock) { running = false; lock.notifyAll() }
        check(Thread.currentThread() !== worker) {
            LogManager.addLog("ERROR", "video", "video close attempted self join id=$id"); "Video close cannot join its worker"
        }
        try { worker.join(250) } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            LogManager.addLog("ERROR", "video", "video close join interrupted id=$id; owner retained: $e")
            throw e
        }
        check(!worker.isAlive) {
            LogManager.addLog("ERROR", "video", "video close pending id=$id worker=${worker.state}; owner retained")
            "Video worker cleanup pending id=$id"
        }
        releaseDecoder()
    }

    private fun releaseDecoder() {
        decoderResources?.close(); decoderResources = null
    }

    private fun run() {
        var reported = 0L
        var workMaxUs = 0L
        try {
            LogManager.addLog("INFO", "video", "viewer video worker started id=$id mailbox=2")
            while (running) {
                val requestedGeneration = generation
                if (activeGeneration != requestedGeneration) {
                    releaseDecoder(); video = createVideoPump(); firstInputAtUs = 0
                    eosQueued = false; videoRevision = 0; lastShownPts = -1
                    activeGeneration = requestedGeneration
                }
                val started = System.nanoTime() / 1000
                video.step(position, started)
                val frame = synchronized(lock) {
                    if (activeGeneration == generation && video.wantsInput) input.pollFirst() else null
                }
                if (frame != null) {
                    if (videoRevision != frame.revision) {
                        check(frame.key) { "Video revision does not start at KEY" }
                        releaseDecoder(); video = createVideoPump(); firstInputAtUs = 0
                        eosQueued = false; videoRevision = frame.revision
                    }
                    video.offer(frame, started); video.step(position, System.nanoTime() / 1000)
                }
                val finishing = synchronized(lock) { activeGeneration == generation && inputEnded && input.isEmpty() }
                val codec = decoder
                if (finishing && codec != null && video.input == null && video.output == null && !eosQueued) {
                    val index = codec.dequeueInputBuffer(0)
                    if (index >= 0) {
                        codec.queueInputBuffer(index, 0, 0, video.lastInputPts.coerceAtLeast(0), MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                        eosQueued = true
                    }
                }
                video.step(position, System.nanoTime() / 1000)
                video.checkProgress(System.nanoTime() / 1000, finishing)
                check(!video.ended || video.firstOutputPts != null || video.inputCount == 0L) { "Decoder ended without producing an image" }
                val next = State(video.firstOutputPts, video.ended || (finishing && codec == null),
                    video.inputCount, video.outputCount, video.shown, video.discarded, video.inflight,
                    video.decoderLeadUs, lastShownPts, video.output?.pts)
                val changed = synchronized(lock) {
                    if (activeGeneration == generation && state != next) { state = next; true } else false
                }
                if (changed || frame != null) wake()
                val now = System.nanoTime() / 1000
                workMaxUs = maxOf(workMaxUs, now - started)
                if (now - reported >= 1_000_000) {
                    reported = now
                    LogManager.videoTrace("android video worker id=$id generation=$activeGeneration revision=$videoRevision " +
                        "inputs=${video.inputCount} decoded=${video.outputCount} shown=${video.shown} discarded=${video.discarded} " +
                        "inflight=${video.inflight} decoderLeadUs=${video.decoderLeadUs} workMaxUs=$workMaxUs " +
                        "clock=$position shownPTS=$lastShownPts displayLagUs=${if (lastShownPts >= 0) position - lastShownPts else -1}")
                    workMaxUs = 0
                }
                synchronized(lock) {
                    if (running && activeGeneration == generation && (input.isEmpty() || !video.wantsInput)) {
                        val pending = video.input != null || video.output != null || video.inflight > 0 || (finishing && !next.ended)
                        lock.wait(if (pending) 5 else 50)
                    }
                }
            }
        } catch (e: Exception) {
            val report = running; running = false
            if (report) {
                failure = e
                LogManager.addLog("ERROR", "video", "viewer video worker failed id=$id generation=$activeGeneration: ${e.message}")
                wake()
            }
        } finally {
            running = false
            try { releaseDecoder() } catch (e: Exception) {
                LogManager.addLog("ERROR", "video", "viewer decoder cleanup pending id=$id; retained until close: $e")
            }
            LogManager.addLog("INFO", "video", "viewer video worker exited id=$id decoderPending=${decoderResources != null}")
        }
    }

    private fun createVideoPump() = StreamDecodePump<Frame>(
        submit = { frame ->
            if (decoder == null) {
                val openingAtUs = System.nanoTime() / 1000
                check(frame.key)
                val format = frame.codec.decoderFormat(frame.width, frame.height, frame.rotation, frame.config)
                val codecs = MediaCodecList(MediaCodecList.REGULAR_CODECS)
                val preferred = codecs.findDecoderForFormat(format)
                val candidates = codecs.codecInfos.filter { !it.isEncoder && frame.codec.mime in it.supportedTypes }
                    .sortedBy { if (it.name == preferred) 0 else 1 }
                if (candidates.isEmpty()) throw UnsupportedCodec("Unsupported decoder ${frame.codec.label}")
                var failure: Exception? = null
                for (candidate in candidates) {
                    try {
                        val capabilities = candidate.getCapabilitiesForType(frame.codec.mime)
                        if (!capabilities.isFormatSupported(format)) continue
                        val lowLatency = Build.VERSION.SDK_INT >= 30 &&
                            capabilities.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
                        if (Build.VERSION.SDK_INT >= 30) format.setInteger(MediaFormat.KEY_LOW_LATENCY, if (lowLatency) 1 else 0)
                        val attempt = VideoCodecResources(MediaCodec.createByCodecName(candidate.name), candidate.name)
                        decoderResources = attempt
                        val codec = attempt.codec
                        codec.configure(format, surface, null, 0); codec.start()
                        var firstRendered = true
                        val codecRevision = frame.revision
                        try {
                            codec.setOnFrameRenderedListener({ codec, pts, renderedNs ->
                                if (firstRendered) {
                                    firstRendered = false
                                    val callbackUs = System.nanoTime() / 1000
                                    val renderedUs = renderedNs / 1000
                                    val renderClockValid = renderedUs in requestedAtUs..callbackUs
                                    if (!renderClockValid) LogManager.addLog("WARN", "stream",
                                        "viewer surface timestamp outside request interval id=$id PTS=$pts renderedNs=$renderedNs " +
                                            "requestedUs=$requestedAtUs callbackUs=$callbackUs; using callback time")
                                    LogManager.addLog("INFO", "stream", "android first surface frame id=$id revision=$codecRevision " +
                                        "stage=surface_callback PTS=$pts elapsedUs=${callbackUs - requestedAtUs} " +
                                        "renderElapsedUs=${if (renderClockValid) renderedUs - requestedAtUs else -1} " +
                                        "callbackDelayUs=${if (renderClockValid) callbackUs - renderedUs else -1}")
                                    try { codec.setOnFrameRenderedListener(null, null) }
                                    catch (e: Exception) {
                                        LogManager.addLog("WARN", "stream", "viewer surface diagnostics detach id=$id: ${e.message}")
                                    }
                                }
                            }, main)
                        } catch (e: Exception) {
                            LogManager.addLog("WARN", "stream", "viewer presentation diagnostics unavailable id=$id: ${e.message}")
                        }
                        LogManager.addLog("INFO", "video", "viewer codec id=$id type=${frame.codec.label} decoder=${candidate.name} " +
                            "config=${frame.config.size} lowLatency=$lowLatency revision=$videoRevision " +
                            "elapsedUs=${System.nanoTime() / 1000 - requestedAtUs} openUs=${System.nanoTime() / 1000 - openingAtUs}")
                        break
                    } catch (e: Exception) {
                        failure = e
                        LogManager.addLog("WARN", "video", "viewer decoder unavailable id=$id codec=${candidate.name}: ${e.message}")
                        try { releaseDecoder() } catch (release: Exception) {
                            e.addSuppressed(release)
                            LogManager.addLog("ERROR", "video", "decoder fallback cleanup pending id=$id; aborting replacement: $release")
                            throw e
                        }
                        if (!surface.isValid) throw e
                    }
                }
                if (decoder == null) {
                    if (failure == null) throw UnsupportedCodec("Unsupported decoder format ${frame.codec.label} ${frame.width}x${frame.height}")
                    throw IllegalStateException("No available ${frame.codec.label} decoder", failure)
                }
            }
            val index = decoder!!.dequeueInputBuffer(0)
            if (index < 0) false else {
                val input = decoder!!.getInputBuffer(index) ?: error("Missing decoder input")
                check(frame.data.size <= input.capacity()) { "Decoder input is too small: ${input.capacity()}/${frame.data.size}" }
                input.clear(); input.put(frame.data)
                if (firstInputAtUs == 0L) firstInputAtUs = System.nanoTime() / 1000
                decoder!!.queueInputBuffer(index, 0, frame.data.size, frame.pts, 0); true
            }
        },
        receive = {
            var output: StreamDecodePump.Output? = null
            val codec = decoder
            if (codec != null) {
                for (attempt in 0 until 3) {
                    val index = codec.dequeueOutputBuffer(info, 0)
                    if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        LogManager.addLog("INFO", "video", "viewer output format id=$id revision=$videoRevision ${codec.outputFormat}")
                        continue
                    }
                    if (index >= 0) {
                        val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                        output = StreamDecodePump.Output(index, info.presentationTimeUs, eos, eos && info.size == 0)
                        if (!output.empty && video.firstOutputPts == null) {
                            val decodedAtUs = System.nanoTime() / 1000
                            LogManager.addLog("INFO", "video", "viewer first decoded frame id=$id revision=$videoRevision " +
                                "PTS=${output.pts} inputs=${video.inputCount} decoderLeadUs=${maxOf(0, video.lastInputPts - output.pts)} " +
                                "elapsedUs=${decodedAtUs - requestedAtUs} decodeUs=${decodedAtUs - firstInputAtUs}")
                        }
                    }
                    break
                }
            }
            output
        },
        release = { output, render ->
            val current = running && activeGeneration == generation
            if (render && current && !firstShown) {
                firstShown = true
                LogManager.addLog("INFO", "stream", "android first frame id=$id stage=display_submit " +
                    "elapsedUs=${System.nanoTime() / 1000 - requestedAtUs} PTS=${output.pts} clockUs=$position " +
                    "readyUs=$ready readyAheadUs=${ready - output.pts} avUs=${output.pts - position} " +
                    "deviceLeadUs=${clock.leadUs}")
            }
            decoder!!.releaseOutputBuffer(output.index, render && current)
            if (render && current) { lastShownPts = output.pts; onRendered() }
        }
    )
}
