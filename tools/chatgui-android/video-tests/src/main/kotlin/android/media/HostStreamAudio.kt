package android.media

import java.util.concurrent.CopyOnWriteArrayList

class AudioAttributes {
    companion object { const val USAGE_MEDIA = 1; const val CONTENT_TYPE_MOVIE = 1 }
    class Builder {
        fun setUsage(value: Int) = this
        fun setContentType(value: Int) = this
        fun build() = AudioAttributes()
    }
}
class AudioFormat {
    companion object { const val CHANNEL_OUT_STEREO = 12; const val ENCODING_PCM_16BIT = 2 }
    class Builder {
        fun setSampleRate(value: Int) = this
        fun setChannelMask(value: Int) = this
        fun setEncoding(value: Int) = this
        fun build() = AudioFormat()
    }
}
/** Только JVM: SDK output остаётся наблюдаемым до подтверждённого release. */
class AudioTrack {
    companion object {
        const val STATE_INITIALIZED = 1; const val MODE_STREAM = 1; const val WRITE_NON_BLOCKING = 1
        val created = CopyOnWriteArrayList<AudioTrack>()
        var nextFailures = emptySet<String>()
        fun getMinBufferSize(rate: Int, channels: Int, encoding: Int) = 3840
    }
    class Builder {
        fun setAudioAttributes(value: AudioAttributes) = this
        fun setAudioFormat(value: AudioFormat) = this
        fun setBufferSizeInBytes(value: Int) = this
        fun setTransferMode(value: Int) = this
        fun build() = AudioTrack().also { created.add(it) }
    }
    var failures = nextFailures
    var released = false
        private set
    val state get() = STATE_INITIALIZED
    var playState = 0
        private set
    val bufferSizeInFrames = 960
    private var threshold = 1
    val startThresholdInFrames get() = threshold
    var playbackHeadPosition = 0
        private set
    val underrunCount = 0
    fun setStartThresholdInFrames(value: Int) { check("threshold" !in failures); threshold = value }
    fun write(pcm: ShortArray, offset: Int, count: Int, mode: Int): Int {
        check(!released); check("write" !in failures); playbackHeadPosition += count / 2; return count
    }
    fun pause() { check(!released); playState = 0 }
    fun play() { check(!released); check("play" !in failures); playState = 1 }
    fun release() {
        check("release" !in failures) { "audio release failed" }; released = true
        check("release after" !in failures) { "audio release failed after effect" }
    }
}
