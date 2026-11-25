package android.media

import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicInteger

class AudioDeviceInfo(val id: Int, val type: Int = 15, val productName: String = "microphone-$id")
object AudioFormat {
    const val CHANNEL_IN_MONO = 16; const val CHANNEL_IN_STEREO = 12; const val ENCODING_PCM_16BIT = 2
    const val CHANNEL_OUT_MONO = 4; const val CHANNEL_OUT_STEREO = 12
}
object MediaRecorder {
    object AudioSource { const val MIC = 1; const val VOICE_COMMUNICATION = 7; const val UNPROCESSED = 9; const val CAMCORDER = 5 }
}
class AudioTimestamp {
    companion object { const val TIMEBASE_MONOTONIC = 0 }
    var nanoTime = 0L
    var framePosition = 0L
}
class AudioRecordingConfiguration(val isClientSilenced: Boolean)

/** Android API model: NON_BLOCKING действительно возвращается без данных, timestamps/route/ошибки задаёт сценарий. */
class AudioRecord(val source: Int, val rate: Int, val mask: Int, encoding: Int, val bufferBytes: Int) {
    data class Chunk(val count: Int? = null, val value: Short = 12, val device: Int? = 7, val silenced: Boolean = false,
        val timeNs: Long? = null, val position: Long? = null, val measured: Boolean = true, val onRead: () -> Unit = {})
    class Plan {
        val chunks = LinkedBlockingQueue<Chunk>()
        @Volatile var failures = emptySet<String>()
        var initialized = true
        var channels: Int? = null
        var failChannelsAt = 0
        val channelQueries = AtomicInteger()
        var acceptInput = true
        var starts = true
        var initialDevice: Int? = 7
        var clock: () -> Long = System::nanoTime
        var holdRead: CountDownLatch? = null
        val readEntered = CountDownLatch(1)
        val stopEntered = CountDownLatch(1)
        val readCalls = AtomicInteger()
        val stopCalls = AtomicInteger()
        val releaseCalls = AtomicInteger()
        val modes = CopyOnWriteArrayList<Int>()
        @Volatile var reader: Thread? = null
        var released = false
    }
    companion object {
        const val STATE_INITIALIZED = 1; const val RECORDSTATE_RECORDING = 3; const val RECORDSTATE_STOPPED = 1
        const val READ_BLOCKING = 0; const val READ_NON_BLOCKING = 1; const val SUCCESS = 0
        const val ERROR_INVALID_OPERATION = -3; const val ERROR_DEAD_OBJECT = -6
        val plans = LinkedBlockingQueue<Plan>()
        val created = CopyOnWriteArrayList<AudioRecord>()
        val minimums = mutableMapOf<Int, Int>()
        val masks = CopyOnWriteArrayList<Int>()
        var constructorFailures = emptySet<Int>()
        fun reset() { plans.clear(); created.clear(); minimums.clear(); masks.clear(); constructorFailures = emptySet() }
        fun getMinBufferSize(rate: Int, mask: Int, encoding: Int): Int {
            check(rate == 48000 && encoding == AudioFormat.ENCODING_PCM_16BIT)
            return minimums[mask] ?: 960
        }
    }
    val plan: Plan
    init {
        masks.add(mask)
        check(mask !in constructorFailures) { "constructor failed mask=$mask" }
        plan = requireNotNull(plans.poll()) { "missing record plan" }
        created.add(this)
        check(encoding == AudioFormat.ENCODING_PCM_16BIT && rate == 48000)
    }
    private var totalSamples = 0L
    private var last: Chunk? = null
    private var actual: Int? = plan.initialDevice
    private var recording = RECORDSTATE_STOPPED
    private fun operation(name: String) { check(name !in plan.failures) { "record operation failed: $name" } }
    val state: Int get() { operation("state"); return if (plan.initialized) STATE_INITIALIZED else 0 }
    val channelCount: Int get() {
        check(plan.channelQueries.incrementAndGet() != plan.failChannelsAt) { "record channel query failed" }
        operation("channels"); return plan.channels ?: if (mask == AudioFormat.CHANNEL_IN_STEREO) 2 else 1
    }
    val recordingState: Int get() { operation("recording"); return recording }
    val routedDevice: AudioDeviceInfo? get() { operation("route"); return actual?.let { AudioDeviceInfo(it) } }
    val activeRecordingConfiguration: AudioRecordingConfiguration get() {
        operation("configuration"); return AudioRecordingConfiguration(last?.silenced == true)
    }
    fun setPreferredDevice(input: AudioDeviceInfo): Boolean { operation("prefer"); return plan.acceptInput }
    fun startRecording() { operation("start"); if (plan.starts) recording = RECORDSTATE_RECORDING; operation("start after") }
    fun read(pcm: ShortArray, offset: Int, size: Int) = read(pcm, offset, size, READ_BLOCKING)
    fun read(pcm: ShortArray, offset: Int, size: Int, mode: Int): Int {
        plan.reader = Thread.currentThread()
        plan.readCalls.incrementAndGet(); plan.modes.add(mode); plan.readEntered.countDown()
        operation("read")
        plan.holdRead?.await()
        val chunk = if (mode == READ_BLOCKING) plan.chunks.take() else plan.chunks.poll() ?: return 0
        chunk.onRead()
        last = chunk; actual = chunk.device
        val count = chunk.count ?: size
        if (count > 0) {
            repeat(minOf(count, size)) { pcm[offset + it] = chunk.value }
            totalSamples += count
        }
        return count
    }
    fun getTimestamp(stamp: AudioTimestamp, timebase: Int): Int {
        check(timebase == AudioTimestamp.TIMEBASE_MONOTONIC); operation("timestamp")
        val chunk = requireNotNull(last)
        if (!chunk.measured) return ERROR_INVALID_OPERATION
        stamp.nanoTime = chunk.timeNs ?: plan.clock()
        stamp.framePosition = chunk.position ?: (totalSamples / channelCount - 960).coerceAtLeast(0)
        return SUCCESS
    }
    fun stop() {
        plan.stopCalls.incrementAndGet(); plan.stopEntered.countDown(); operation("stop")
        recording = RECORDSTATE_STOPPED
        plan.chunks.offer(Chunk(count = ERROR_INVALID_OPERATION))
    }
    fun release() {
        plan.releaseCalls.incrementAndGet(); operation("release"); plan.released = true; recording = RECORDSTATE_STOPPED
    }
}
