package android.media

import android.os.Handler
import java.util.concurrent.LinkedBlockingDeque

interface AudioRouting {
    fun interface OnRoutingChangedListener { fun onRoutingChanged(router: AudioRouting) }
}

/** Только JVM: descriptor может подтвердиться позже, callback сохраняется после удаления для проверки lease identity. */
class AudioTrack(val id: Int) : AudioRouting {
    companion object {
        const val MODE_STREAM = 1
        const val STATE_INITIALIZED = 1; const val PLAYSTATE_STOPPED = 1; const val PLAYSTATE_PLAYING = 3
        const val WRITE_NON_BLOCKING = 1; const val ERROR_DEAD_OBJECT = -6; const val PERFORMANCE_MODE_LOW_LATENCY = 1
        private var nextId = 100
        val created = mutableListOf<AudioTrack>()
        var nextFailures = emptySet<String>()
        var minimumBuffer = 1920
        fun getMinBufferSize(rate: Int, channels: Int, encoding: Int): Int = minimumBuffer
    }
    constructor(attributes: AudioAttributes, format: AudioFormat, bufferSize: Int, mode: Int, session: Int) : this(++nextId) {
        check(mode == MODE_STREAM && bufferSize >= 1920 && session == AudioManager.AUDIO_SESSION_ID_GENERATE)
        check("create" !in nextFailures) { "track creation failed" }
        failures = nextFailures
        audioAttributes = attributes
        created.add(this)
    }
    private var head = 0
    var audioAttributes = AudioAttributes.Builder().build()
    val state: Int get() { access("state"); return if ("uninitialized" in failures) 0 else STATE_INITIALIZED }
    var playState = PLAYSTATE_STOPPED
        private set
    var bufferSizeInFrames = 1920
        private set
    val bufferCapacityInFrames = 3840
    val underrunCount = 0
    val performanceMode = PERFORMANCE_MODE_LOW_LATENCY
    var startThresholdInFrames = 1920
        private set
    val playbackHeadPosition: Int get() { access("head"); return head }
    fun play() { access("play"); playState = PLAYSTATE_PLAYING }
    fun pause() { access("pause"); playState = PLAYSTATE_STOPPED }
    fun setBufferSizeInFrames(value: Int): Int { access("buffer"); bufferSizeInFrames = value; return value }
    fun setStartThresholdInFrames(value: Int): Int { access("threshold"); startThresholdInFrames = value; return value }
    fun getTimestamp(stamp: AudioTimestamp): Boolean { onTimestamp(); access("timestamp"); return false }
    var interruptOnStop = false
    fun stop() { access("stop"); if (interruptOnStop) Thread.currentThread().interrupt() }
    var releaseCalls = 0
        private set
    fun release() {
        releaseCalls++
        check("release" !in failures) { "track release failed" }
        released = true
        check("release after" !in failures) { "track release failed after effect" }
    }
    fun write(samples: ShortArray, offset: Int, count: Int): Int {
        onWrite()
        access("write")
        check(offset >= 0 && offset + count <= samples.size)
        val result = forcedWriteResult ?: minOf(count, maxWriteSamples)
        val write = Write(offset, count, result, samples.copyOfRange(offset, offset + count))
        if (!writes.offerLast(write)) { writes.pollFirst(); writes.offerLast(write) }
        if (result > 0) head += result / 2
        return result
    }
    fun write(samples: ShortArray, offset: Int, count: Int, mode: Int): Int {
        check(mode == WRITE_NON_BLOCKING)
        Thread.sleep(2)
        return write(samples, offset, count)
    }
    data class Write(val offset: Int, val count: Int, val result: Int, val pcm: ShortArray)
    val writes = LinkedBlockingDeque<Write>(64)
    @Volatile var onWrite: () -> Unit = {}
    @Volatile var onTimestamp: () -> Unit = {}
    @Volatile var maxWriteSamples = Int.MAX_VALUE
    @Volatile var forcedWriteResult: Int? = null
    @Volatile var failures = emptySet<String>()
    var acceptPreference = true
    var autoRoute = true
    var emitOnPreference = false
    var released = false
    var accessAfterRelease = 0
    var actual: AudioDeviceInfo? = null
    var preferred: AudioDeviceInfo? = null
    var selectionAttempts = 0
    val history = mutableListOf<AudioRouting.OnRoutingChangedListener>()
    val listeners = mutableMapOf<AudioRouting.OnRoutingChangedListener, Handler>()
    private fun access(operation: String) {
        if (released) accessAfterRelease++
        check(!released) { "track released: $operation" }
        check(operation !in failures) { "track failed: $operation" }
    }
    val audioSessionId: Int get() { access("session"); return id }
    val routedDevice: AudioDeviceInfo? get() { access("routed"); return actual }
    fun addOnRoutingChangedListener(listener: AudioRouting.OnRoutingChangedListener, handler: Handler) {
        access("add listener")
        history.add(listener); listeners[listener] = handler
        access("add listener after")
    }
    fun removeOnRoutingChangedListener(listener: AudioRouting.OnRoutingChangedListener) {
        check("remove listener" !in failures) { "track failed: remove listener" }
        listeners.remove(listener)
        check("remove listener after" !in failures) { "track failed: remove listener after" }
    }
    fun setPreferredDevice(device: AudioDeviceInfo?): Boolean {
        access("prefer"); selectionAttempts++
        if (!acceptPreference) return false
        preferred = device
        if (autoRoute) actual = device
        if (emitOnPreference) listeners.toList().forEach { (listener, handler) -> handler.post { listener.onRoutingChanged(this) } }
        return true
    }
    fun deliver(index: Int) { history[index].onRoutingChanged(this) }
    class Builder {
        private var size = 3840
        fun setAudioAttributes(value: AudioAttributes) = apply {}
        fun setAudioFormat(value: AudioFormat) = apply {}
        fun setBufferSizeInBytes(value: Int) = apply { size = value }
        fun setTransferMode(value: Int) = apply { check(value == MODE_STREAM) }
        fun setPerformanceMode(value: Int) = apply { check(value == PERFORMANCE_MODE_LOW_LATENCY) }
        fun build() = AudioTrack(AudioAttributes.Builder().build(), AudioFormat(), size, MODE_STREAM, 0)
    }
}

/** Конфигурация stereo PCM для настоящего RadioAudioEngine, без устройства вывода. */
class AudioFormat {
    companion object {
        const val CHANNEL_OUT_STEREO = 12; const val CHANNEL_OUT_MONO = 4; const val CHANNEL_IN_MONO = 16
        const val ENCODING_PCM_16BIT = 2
    }
    class Builder {
        fun setSampleRate(value: Int) = apply { check(value == 48000) }
        fun setEncoding(value: Int) = apply { check(value == ENCODING_PCM_16BIT) }
        fun setChannelMask(value: Int) = apply { check(value == CHANNEL_OUT_STEREO || value == CHANNEL_OUT_MONO) }
        fun build() = AudioFormat()
    }
}

class AudioTimestamp {
    companion object { const val TIMEBASE_MONOTONIC = 0 }
    var framePosition = 0L
    var nanoTime = 0L
}

object MediaRecorder { object AudioSource { const val VOICE_COMMUNICATION = 7 } }

/** Настоящий capture worker блокируется в read до кадра/stop; отказы до/после release независимы. */
class AudioRecord(source: Int, rate: Int, channels: Int, encoding: Int, size: Int) : AudioRouting {
    companion object {
        const val STATE_INITIALIZED = 1; const val RECORDSTATE_RECORDING = 3; const val READ_BLOCKING = 0; const val SUCCESS = 0
        val created = mutableListOf<AudioRecord>()
        var nextFailures = emptySet<String>()
        var nextOnStart: () -> Unit = {}
        fun getMinBufferSize(rate: Int, channels: Int, encoding: Int) = 1920
    }
    @Volatile var failures = nextFailures
    @Volatile var released = false
    var releaseCalls = 0
        private set
    private val monitor = Object()
    val state: Int get() { access("state"); return if ("uninitialized" in failures) 0 else STATE_INITIALIZED }
    @Volatile var recordingState = 1
        private set
    var preferredDevice: AudioDeviceInfo? = null
        private set
    val routedDevice: AudioDeviceInfo? get() = preferredDevice
    val listeners = mutableSetOf<AudioRouting.OnRoutingChangedListener>()
    init { check("create" !in failures); created.add(this) }
    private fun access(operation: String) { check(!released && operation !in failures) { "record failed: $operation" } }
    private val onStart = nextOnStart
    fun startRecording() { onStart(); access("start"); recordingState = RECORDSTATE_RECORDING }
    fun stop() { access("stop"); synchronized(monitor) { recordingState = 1; monitor.notifyAll() } }
    fun release() {
        releaseCalls++
        check("release" !in failures) { "record release failed" }
        released = true; synchronized(monitor) { recordingState = 1; monitor.notifyAll() }
        check("release after" !in failures) { "record release failed after effect" }
    }
    fun read(out: ShortArray, offset: Int, count: Int, mode: Int): Int {
        access("read"); check(mode == READ_BLOCKING)
        synchronized(monitor) { if (recordingState == RECORDSTATE_RECORDING) monitor.wait(20) }
        if (recordingState != RECORDSTATE_RECORDING) return -3
        out.fill(1, offset, offset + count); return count
    }
    fun getTimestamp(stamp: AudioTimestamp, base: Int): Int { access("timestamp"); return -3 }
    fun setPreferredDevice(device: AudioDeviceInfo?): Boolean { access("prefer"); preferredDevice = device; return true }
    fun addOnRoutingChangedListener(listener: AudioRouting.OnRoutingChangedListener, handler: Handler) {
        access("add listener"); listeners.add(listener); access("add listener after")
    }
}
