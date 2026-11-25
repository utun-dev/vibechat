package com.utun.chat.data

import android.media.AudioTrack
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.flow.MutableStateFlow
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

class ScreenProjection(var portrait: Boolean = false) {
    fun stopAfterFailure() { detach() }
    @Volatile var surface: android.view.Surface? = null
    fun attach(target: android.view.Surface, width: Int, height: Int) { surface = target }
    fun detach() { surface = null }
}
typealias RadioCompressorSettings = Any
data class StreamCaptureMode(val camera: String = "0", val width: Int = 640, val height: Int = 480, val fps: Int = 30,
    val minimumFps: Int = 30, val highSpeed: Boolean = false, val codec: String = "host.encoder", val bitrate: Int = 1_000_000,
    val videoCodec: StreamVideoCodec = StreamVideoCodec.H264, val minimumBitrate: Int = 1, val maximumBitrate: Int = 12_000_000,
    val encoderProfile: Int? = 1, val screen: Boolean = false) {
    val label get() = "${width}x$height@$fps"
    val manualBitrateRange get() = minimumBitrate..maximumBitrate
}
object MicrophoneCapture {
    data class Frame(val pcm: ShortArray, val timeNs: Long, val sequence: Long)
    class Subscription : AutoCloseable {
        val queue = LinkedBlockingQueue<Frame>()
        var failures = emptySet<String>()
        var closed = false
        fun poll(): Any? = queue.poll(20, TimeUnit.MILLISECONDS)
        override fun close() { check("close" !in failures); closed = true }
    }
}

enum class AudioRoute { EARPIECE, SPEAKER, HEADSET }
enum class CallPhase { NONE, OUTGOING, RINGING, INCOMING, ACTIVE, ENDED, WAIT_DECISION, PENDING_INCOMING }
data class HostCallState(val callId: Long, val phase: CallPhase) { val ongoing get() = phase != CallPhase.NONE && phase != CallPhase.ENDED }
object CallController { var blocked = false; val state = MutableStateFlow<HostCallState?>(null); fun blocksMedia() = blocked }
object StreamController { private val ids = AtomicLong(100); fun nextId() = ids.incrementAndGet() }
object MediaAudioController {
    data class State(val switching: Boolean = false)
    val state = MutableStateFlow(State())
    var failures = emptySet<String>()
    val outputs = ConcurrentHashMap.newKeySet<AudioTrack>()
    val subscriptions = ConcurrentLinkedQueue<MicrophoneCapture.Subscription>()
    fun subscribe(name: String): MicrophoneCapture.Subscription {
        check("subscribe" !in failures)
        return MicrophoneCapture.Subscription().also { subscriptions.add(it) }
    }
    fun attachOutput(track: AudioTrack): AutoCloseable {
        check("attach" !in failures); outputs.add(track)
        return AutoCloseable { check("detach" !in failures); outputs.remove(track) }
    }
}

/** Native-границы viewer/mixer: fault injection до изменения владельца, никаких JNI/DSP/сети. */
object HostStreams {
    val ports = ConcurrentHashMap.newKeySet<Long>()
    val signals = ConcurrentHashMap.newKeySet<Long>()
    val playbacks = ConcurrentHashMap.newKeySet<Long>()
    val playbackAudio = ConcurrentHashMap<Long, Boolean>()
    val playbackModes = CopyOnWriteArrayList<Boolean>()
    val resyncs = AtomicInteger()
    val audioFeeds = AtomicInteger()
    val opuses = ConcurrentHashMap.newKeySet<Long>()
    val records = ConcurrentLinkedQueue<ByteArray>()
    private val sequence = AtomicLong()
    @Volatile var failures = emptySet<String>()
    @Volatile var audioEnabled = false
    @Volatile var status = false
    @Volatile var videoRateTarget = 0
    @Volatile var onWait: () -> Unit = {}
    @Volatile var onPull: () -> Unit = {}
    fun access(name: String) { check(name !in failures) { "stream $name failed" } }
    fun open(id: Long): Boolean { access("open"); ports.add(id); return true }
    fun stop(id: Long) { access("stop"); ports.remove(id) }
    fun createSignal(): Long { access("signal create"); return sequence.incrementAndGet().also { signals.add(it) } }
    fun closeSignal(handle: Long) { access("signal close"); check(signals.remove(handle)) }
    fun notifySignal(handle: Long) { access("signal notify"); check(handle in signals) }
    fun waitSignal(handle: Long): Int { onWait(); access("signal wait"); check(handle in signals); Thread.sleep(2); return 0 }
    fun createPlayback(audio: Boolean): Long {
        access("playback create")
        return sequence.incrementAndGet().also { playbackAudio[it] = audio; playbacks.add(it); playbackModes.add(audio) }
    }
    fun closePlayback(handle: Long) { access("playback close"); check(playbacks.remove(handle)); playbackAudio.remove(handle) }
    fun createOpus(): Long { access("opus create"); return sequence.incrementAndGet().also { opuses.add(it) } }
    fun closeOpus(handle: Long) { access("opus close"); check(opuses.remove(handle)) }
    fun reset() {
        ports.clear(); signals.clear(); playbacks.clear(); opuses.clear(); records.clear(); failures = emptySet()
        playbackAudio.clear(); playbackModes.clear(); resyncs.set(0); audioFeeds.set(0)
        audioEnabled = false; status = false; videoRateTarget = 0; onWait = {}; onPull = {}
    }
}
