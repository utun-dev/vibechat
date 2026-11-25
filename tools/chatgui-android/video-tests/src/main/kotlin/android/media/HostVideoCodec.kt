package android.media

import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ConcurrentLinkedQueue

/** Минимальный формат для настоящего StreamVideoCodec; данные сохраняются для проверки configure. */
class MediaFormat {
    companion object {
        const val KEY_MAX_FPS_TO_ENCODER = "max-fps-to-encoder"
        const val KEY_REPEAT_PREVIOUS_FRAME_AFTER = "repeat-previous-frame-after"
        const val KEY_MIME = "mime"; const val KEY_WIDTH = "width"; const val KEY_HEIGHT = "height"
        const val KEY_ROTATION = "rotation"; const val KEY_MAX_INPUT_SIZE = "max-input-size"
        const val KEY_LOW_LATENCY = "low-latency"
        const val KEY_COLOR_FORMAT = "color"; const val KEY_PROFILE = "profile"; const val KEY_BIT_RATE = "bitrate"
        const val KEY_FRAME_RATE = "fps"; const val KEY_I_FRAME_INTERVAL = "interval"; const val KEY_MAX_B_FRAMES = "bframes"
        fun createVideoFormat(mime: String, width: Int, height: Int) = MediaFormat()
    }
    val values = mutableMapOf<String, Any>()
    fun setFloat(key: String, value: Float) { values[key] = value }
    fun setLong(key: String, value: Long) { values[key] = value }
    fun setInteger(key: String, value: Int) { values[key] = value }
    fun setByteBuffer(key: String, value: ByteBuffer) { values[key] = value }
    fun getByteBuffer(key: String) = values[key] as? ByteBuffer
    fun containsKey(key: String) = values.containsKey(key)
    fun getString(key: String) = values[key] as? String
    fun getInteger(key: String) = values[key] as Int
}
class MediaCodecInfo(val name: String = "host.decoder") {
    val isEncoder = false
    val supportedTypes = arrayOf("video/avc", "video/hevc")
    fun getCapabilitiesForType(type: String) = CodecCapabilities()
    class CodecCapabilities {
        companion object { const val FEATURE_LowLatency = "low-latency"; const val COLOR_FormatSurface = 0x7f000789 }
        fun isFormatSupported(format: MediaFormat) = true
        fun isFeatureSupported(name: String) = false
    }
    object CodecProfileLevel {
    const val AVCProfileBaseline = 1; const val HEVCProfileMain = 1; const val VP9Profile0 = 1; const val AV1ProfileMain8 = 1
} }
class MediaCodecList(kind: Int) {
    companion object { const val REGULAR_CODECS = 0; var names = listOf("host.decoder") }
    val codecInfos get() = names.map { MediaCodecInfo(it) }.toTypedArray()
    fun findDecoderForFormat(format: MediaFormat) = names.firstOrNull()
}

/** Настоящий RX получает input/output и EOS; тест задаёт отказы платформенных API, без декодирования bitstream. */
class MediaCodec private constructor(val name: String) {
    companion object {
        const val BUFFER_FLAG_END_OF_STREAM = 4; const val INFO_OUTPUT_FORMAT_CHANGED = -2
        const val BUFFER_FLAG_CODEC_CONFIG = 2; const val BUFFER_FLAG_PARTIAL_FRAME = 8; const val BUFFER_FLAG_KEY_FRAME = 1
        const val CONFIGURE_FLAG_ENCODE = 1; const val PARAMETER_KEY_VIDEO_BITRATE = "bitrate"; const val PARAMETER_KEY_REQUEST_SYNC_FRAME = "sync"
        val created = CopyOnWriteArrayList<MediaCodec>()
        @Volatile var nextFailures = emptySet<String>()
        @Volatile var onInput: () -> Unit = {}
        fun createByCodecName(name: String): MediaCodec {
            check("create" !in nextFailures) { "decoder create failed" }
            return MediaCodec(name).also { created.add(it) }
        }
    }
    class BufferInfo { var size = 0; var flags = 0; var presentationTimeUs = 0L; var offset = 0 }
    class CodecException(val diagnosticInfo: String) : Exception(diagnosticInfo)
    abstract class Callback {
        abstract fun onInputBufferAvailable(codec: MediaCodec, index: Int)
        abstract fun onError(codec: MediaCodec, error: CodecException)
        abstract fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat)
        abstract fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: BufferInfo)
    }
    var callback: Callback? = null
        private set
    private var callbackHandler: android.os.Handler? = null
    private val encoded = mutableMapOf<Int, ByteArray>()
    private data class Frame(val pts: Long, val size: Int, val flags: Int)
    private val queued = ConcurrentLinkedQueue<Frame>()
    private val pending = mutableMapOf<Int, Frame>()
    private val input = ByteBuffer.allocate(4096)
    private var index = 0
    @Volatile var failures = nextFailures
    @Volatile var released = false
        private set
    @Volatile var started = false
        private set
    var releaseCalls = 0
        private set
    var stopCalls = 0
        private set
    var configuredSurface: Surface? = null
        private set
    @Volatile var bitrate = 0
        private set
    var inputSurface: Surface? = null
        private set
    var renderedPts = ConcurrentLinkedQueue<Long>()
    val outputFormat = MediaFormat()
    private fun access(operation: String) {
        check(!released) { "decoder already released: $operation" }
        check(operation !in failures) { "decoder failed: $operation" }
    }
    fun configure(format: MediaFormat, surface: Surface?, crypto: Any?, flags: Int) {
        access("configure"); configuredSurface = surface; access("configure after")
        if (format.containsKey(MediaFormat.KEY_BIT_RATE)) bitrate = format.getInteger(MediaFormat.KEY_BIT_RATE)
    }
    fun start() { access("start"); started = true; access("start after") }
    fun setCallback(value: Callback, handler: android.os.Handler) {
        access("callback"); callback = value; callbackHandler = handler; access("callback after")
    }
    fun createInputSurface(): Surface { access("input surface"); return Surface().also { inputSurface = it } }
    fun setParameters(bundle: android.os.Bundle) {
        access("parameters"); (bundle.values[PARAMETER_KEY_VIDEO_BITRATE] as? Int)?.let { bitrate = it }
    }
    fun emitFormat(format: MediaFormat) { callbackHandler!!.post { callback!!.onOutputFormatChanged(this, format) } }
    fun emitFrame(data: ByteArray, pts: Long = System.nanoTime() / 1000, flags: Int = BUFFER_FLAG_KEY_FRAME) {
        callbackHandler!!.post {
            val index = ++index; encoded[index] = data
            callback!!.onOutputBufferAvailable(this, index, BufferInfo().also { it.presentationTimeUs = pts; it.size = data.size; it.flags = flags })
        }
    }
    fun getOutputBuffer(index: Int): ByteBuffer = ByteBuffer.wrap(checkNotNull(encoded[index]))
    fun stop() { stopCalls++; access("stop"); started = false }
    fun release() {
        releaseCalls++; check("release" !in failures) { "decoder release failed" }
        released = true; started = false
        check("release after" !in failures) { "decoder release failed after effect" }
    }
    fun dequeueInputBuffer(timeout: Long): Int { onInput(); access("input"); return 0 }
    fun setOnFrameRenderedListener(listener: ((MediaCodec, Long, Long) -> Unit)?, handler: android.os.Handler?) {}
    fun getInputBuffer(index: Int): ByteBuffer { access("input buffer"); input.clear(); return input }
    fun queueInputBuffer(index: Int, offset: Int, size: Int, pts: Long, flags: Int) {
        access("queue"); queued.add(Frame(pts, size, flags))
    }
    fun dequeueOutputBuffer(info: BufferInfo, timeout: Long): Int {
        access("output")
        val frame = queued.poll() ?: return -1
        info.size = frame.size; info.presentationTimeUs = frame.pts; info.flags = frame.flags
        pending[++index] = frame
        return index
    }
    fun releaseOutputBuffer(index: Int, render: Boolean) {
        access("render")
        if (encoded.remove(index) != null) return
        val frame = checkNotNull(pending.remove(index))
        if (render) renderedPts.add(frame.pts)
    }
}
