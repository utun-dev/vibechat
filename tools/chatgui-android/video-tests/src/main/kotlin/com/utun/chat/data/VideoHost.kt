package com.utun.chat.data

import android.media.MediaCodec
import android.media.MediaFormat
import android.media.Image
import android.util.Size
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicLong

/** Только журнал приложения; тест проверяет диагностику настоящего RX. */
object LogManager {
    data class Entry(val level: String, val category: String, val message: String)
    val entries = CopyOnWriteArrayList<Entry>()
    fun addLog(level: String, category: String, message: String) {
        entries.add(Entry(level, category, message)); System.err.println("[$level] [$category] $message")
    }
    fun videoTrace(message: String) { addLog("DEBUG", "video", message) }
    fun videoDebug(message: String) { addLog("DEBUG", "video", message) }
}

/** Граница аппаратной пробы; сам RX и разбор uAV2 KEY компилируются из приложения. */
internal class CallVideoCodec(val format: CallVideoFormat, var bitrate: Int, val name: String) : AutoCloseable {
    data class Packet(val pts: Long, val key: Boolean, val bytes: ByteArray)
    var closed = false
        private set
    var failures = nextFailures
    val pendingFrames get() = packets.size
    val inputWaitUs = 0L; val copyUs = 0L; val queueUs = 0L
    private val packets = ConcurrentLinkedQueue<Packet>()
    init { check("create" !in failures); created.add(this) }
    fun prepare(keepRunning: () -> Boolean = { true }) { check(keepRunning() && "prepare" !in failures) }
    val submittedPts = CopyOnWriteArrayList<Long>()
    fun submit(image: Image, pts: Long, key: Boolean, turn: Int): Boolean {
        check("submit" !in failures); submittedPts.add(pts); packets.add(Packet(pts, key, byteArrayOf(1))); return true
    }
    fun drain(): List<Packet> { check("drain" !in failures); return buildList { while (true) add(packets.poll() ?: break) } }
    fun finish() = drain()
    fun setBitrate(value: Int): Boolean { if ("bitrate" in failures) return false; bitrate = value; return true }
    override fun close() { check("close" !in failures); closed = true; check("close after" !in failures) }
    companion object {
    val created = CopyOnWriteArrayList<CallVideoCodec>()
    @Volatile var nextFailures = emptySet<String>()
    @Volatile var onProbe: () -> Unit = {}
    data class EncoderMode(val size: Size = Size(640, 480), val name: String = "host.encoder", val profile: Int = 1,
        val level: Int = 31, val fps: Int = 30)
    data class Encoder(val modes: List<EncoderMode> = listOf(EncoderMode())) {
        val sizes get() = modes.map { it.size }
        val fps get() = 30; val pixelsPerSecond get() = 640 * 480 * 30; val profile get() = 1; val level get() = 31
    }
    data class DecoderMode(val name: String = "host.decoder", val pipeline: Int = 0) {
        fun supports(format: CallVideoFormat) = format.codec == 1
    }
    data class Decoder(val modes: List<DecoderMode> = listOf(DecoderMode())) {
        val size get() = Size(640, 480); val fps get() = 30; val pixelsPerSecond get() = 640 * 480 * 30
        val profiles get() = 1; val level get() = 31
    }
    fun probe(keepRunning: () -> Boolean): Map<Int, Encoder> { onProbe(); return if (keepRunning()) mapOf(1 to Encoder()) else emptyMap() }
    fun probeDecoders(keepRunning: () -> Boolean): Map<Int, Decoder> = if (keepRunning()) mapOf(1 to Decoder()) else emptyMap()
    fun validateKey(bytes: ByteArray, format: CallVideoFormat) { StreamVideoCodec.fromId(format.codec).unpackKey(bytes) }
    fun decoderFormat(format: CallVideoFormat, decoder: MediaCodec, config: ByteArray): MediaFormat =
        StreamVideoCodec.fromId(format.codec).decoderFormat(format.width, format.height, format.rotation, config)
} }

object NativeLib {
    fun streamPlaybackBuffering(handle: Long): Boolean = false
    fun callAudioClock(id: Long, now: Long, out: LongArray): Boolean { out[5] = 1; return true }
    fun setChatSetting(name: String, value: String) {}
    fun isReady() = true
    fun streamOpen(id: Long, group: String, source: String = "", width: Int = 0, height: Int = 0, rotation: Int = 0,
        audio: Boolean = true, videoCodec: Int = 1, epoch: Long = 0) = HostStreams.open(id)
    fun streamStop(id: Long) = HostStreams.stop(id)
    fun streamSignalCreate(id: Long) = HostStreams.createSignal()
    fun streamSignalClose(handle: Long) = HostStreams.closeSignal(handle)
    fun streamSignalNotify(handle: Long) = HostStreams.notifySignal(handle)
    fun streamSignalWait(handle: Long, timeout: Int) = HostStreams.waitSignal(handle)
    fun streamStatus(id: Long): LongArray? = if (HostStreams.status)
        longArrayOf(10_000_000, 10_000_000, 10_000_000, 1000, 1, if (HostStreams.audioEnabled) 3 else 1) else null
    fun streamPull(id: Long, position: Long, video: Int, audioAhead: Long, count: Int): ByteArray? {
        HostStreams.onPull(); HostStreams.access("pull"); return HostStreams.records.poll()
    }
    fun streamTelemetry(id: Long): ByteArray? = null
    fun streamResync(id: Long): Boolean { HostStreams.access("resync"); HostStreams.resyncs.incrementAndGet(); return true }
    fun streamPlaybackCreate(id: Long, pts: Long, audio: Boolean) = HostStreams.createPlayback(audio)
    fun streamPlaybackClose(handle: Long) = HostStreams.closePlayback(handle)
    fun streamPlaybackEmpty(handle: Long) = true
    fun streamPlaybackNeedsInput(handle: Long) = true
    fun streamPlaybackBuffered(handle: Long) = 0
    fun streamPlaybackPrepare(handle: Long) = 0
    fun streamPlaybackFeed(handle: Long, pts: Long, packet: ByteArray): Int {
        HostStreams.access("feed"); HostStreams.audioFeeds.incrementAndGet(); return 0
    }
    fun streamPlaybackFinish(handle: Long) {}
    fun streamPlaybackPull(handle: Long, pcm: ShortArray, frames: Int, limit: Long) = frames
    fun streamPlaybackSilence(handle: Long, frames: Int) {}
    fun streamPlaybackUpdate(handle: Long, now: Long, played: Long, ready: Long, guard: Long, starved: Boolean, sourceFinished: Boolean = false): LongArray {
        HostStreams.access("update"); return longArrayOf(1000 + played * 1_000_000 / 48_000, 0, 0, 1000, 0, 1, 0, 0, 3, 0, 0)
    }
    fun streamSetAudioEnabled(id: Long, enabled: Boolean): Boolean { HostStreams.access("audio enabled"); return true }
    fun streamVideoRate(id: Long, automatic: Boolean, nominal: Int, minimum: Int): Int {
        HostStreams.access("bitrate")
        return if (automatic && HostStreams.videoRateTarget > 0) HostStreams.videoRateTarget.coerceIn(minimum, nominal) else nominal
    }
    fun streamFeed(id: Long, video: Boolean, pts: Long, key: Boolean, data: ByteArray, revision: Long = 0, width: Int = 0,
        height: Int = 0, rotation: Int = 0, audio: Boolean = true, codec: Int = 1): Int { HostStreams.access("feed"); return 1 }
    fun streamOpusCreate(encode: Boolean) = HostStreams.createOpus()
    fun streamOpusClose(handle: Long) = HostStreams.closeOpus(handle)
    fun streamOpusEncode(handle: Long, pcm: ShortArray, compressor: RadioCompressorSettings): ByteArray? {
        HostStreams.access("opus encode"); check(handle in HostStreams.opuses); return byteArrayOf(1)
    }
}

/** Native playout/encoded FIFO: только управляемые границы, без загрузки JNI/декодирования/сетевого ядра. */
internal object CallVideoNative {
    data class Frame(val format: CallVideoFormat, val pts: Long, val bytes: ByteArray, val key: Boolean = true)
    val frames = ConcurrentLinkedQueue<Frame>()
    val live = ConcurrentHashMap.newKeySet<Long>()
    val rejected = CopyOnWriteArrayList<Long>()
    val accepted = CopyOnWriteArrayList<Long>()
    val presented = CopyOnWriteArrayList<Long>()
    val counter = AtomicLong()
    @Volatile var failures = emptySet<String>()
    @Volatile var onCreate: () -> Unit = {}
    @Volatile var onPull: () -> Unit = {}
    val destroys = CopyOnWriteArrayList<Long>()
    val keyRequests = AtomicLong()
    val ports = ConcurrentHashMap.newKeySet<Long>()
    val portOperations = CopyOnWriteArrayList<String>()
    @Volatile var configured = 0L
    @Volatile var statusError = 0L
    @Volatile var onOpen: () -> Unit = {}
    @Volatile var onClose: () -> Unit = {}
    fun reset() {
        frames.clear(); live.clear(); rejected.clear(); accepted.clear(); presented.clear(); destroys.clear()
        failures = emptySet(); onCreate = {}; onPull = {}; keyRequests.set(0)
        ports.clear(); portOperations.clear(); configured = 0; statusError = 0; onOpen = {}; onClose = {}
    }
    private fun access(operation: String) { check(operation !in failures) { "native $operation failed" } }
    fun playoutCreate(id: Long): Long {
        onCreate(); access("create")
        if ("create zero" in failures) return 0
        return counter.incrementAndGet().also { live.add(it) }
    }
    fun playoutDestroy(handle: Long) { access("destroy"); check(live.remove(handle)); destroys.add(handle) }
    fun playoutReset(handle: Long) { access("reset"); check(handle in live) }
    fun playoutStep(handle: Long, pts: Long, received: Long, now: Long, interval: Int, queued: Int, reserve: Int): Long {
        access("step"); check(handle in live); return 0
    }
    fun playoutPresented(handle: Long, pts: Long, now: Long) { access("presented"); check(handle in live); presented.add(pts) }
    fun requestKey(id: Long) { access("key"); keyRequests.incrementAndGet() }
    fun pull(id: Long, meta: LongArray): ByteArray? {
        onPull(); access("pull")
        val frame = frames.poll() ?: return null
        frame.format.array().copyInto(meta); meta[9] = 1; meta[10] = frame.pts; meta[11] = if (frame.key) 1 else 0
        meta[12] = System.nanoTime() / 1000
        return frame.bytes
    }
    fun decoded(id: Long, generation: Long, success: Boolean) {
        access("decoded"); (if (success) accepted else rejected).add(generation)
    }
    fun rotatableCaps(bytes: ByteArray): ByteArray? { access("caps"); return bytes.copyOf() }
    fun open(id: Long, caps: ByteArray): Boolean { onOpen(); access("open"); ports.add(id); portOperations.add("open:$id"); return true }
    fun close(id: Long) { onClose(); access("close"); ports.remove(id); portOperations.add("close:$id") }
    fun range(id: Long, minimum: Int, maximum: Int): Boolean { access("range"); return "range reject" !in failures }
    fun peer(id: Long): ByteArray? = if (id in ports) ByteArray(64) else null
    fun status(id: Long): LongArray? = if (id in ports && "status missing" !in failures)
        longArrayOf(1, 0, 0, 1_000_000, configured, statusError, 300_000, 4_000_000, 0, 0, 1, 1) else null
    fun choose(local: ByteArray, peer: ByteArray, desired: LongArray): LongArray? = desired.copyOf()
    fun supported(local: ByteArray, peer: ByteArray, desired: LongArray): Boolean = true
    fun format(id: Long, format: LongArray, bitrate: Int): Boolean { access("format"); configured = format[0]; statusError = 0; return true }
    fun applied(id: Long, generation: Long, request: Long, bitrate: Int) { access("applied") }
    fun send(id: Long, metadata: LongArray, bytes: ByteArray): Int { access("send"); return 1 }
}
