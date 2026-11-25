package android.media

import android.view.Surface

class MediaExtractor {
    companion object { val created = mutableListOf<MediaExtractor>(); var nextFailures = emptySet<String>() }
    var failures = nextFailures
    var released = false
    var formats = listOf(MediaFormat().apply {
        values[MediaFormat.KEY_MIME] = "video/avc"; values[MediaFormat.KEY_WIDTH] = 1280; values[MediaFormat.KEY_HEIGHT] = 720
    })
    init { created.add(this) }
    val trackCount: Int get() = formats.size
    fun setDataSource(path: String) { check("source" !in failures) { "Track source rejected" } }
    fun getTrackFormat(index: Int) = formats[index]
    fun release() { check("release" !in failures) { "Track release rejected" }; released = true }
}


/** Только управляемая граница MediaPlayer: callbacks и отказы SDK, без декодирования. */
class MediaPlayer {
    companion object {
        const val VIDEO_SCALING_MODE_SCALE_TO_FIT = 1
        val created = mutableListOf<MediaPlayer>()
        var nextFailures = emptySet<String>()
    }
    var failures = nextFailures
    var released = false
    var releases = 0
    var starts = 0
    var pauses = 0
    var seeks = 0
    var surface: Surface? = null
        private set
    var position = 0
    val currentPosition: Int get() { check("position" !in failures && !released); return position }
    val duration: Int get() { check("duration" !in failures); return 10_000 }
    val videoWidth = 1280
    val videoHeight = 720
    var preparedCallback: ((MediaPlayer) -> Unit)? = null
    var completedCallback: ((MediaPlayer) -> Unit)? = null
    var errorCallback: ((MediaPlayer, Int, Int) -> Boolean)? = null
    init { check("construct" !in failures); created.add(this) }
    fun setOnPreparedListener(callback: (MediaPlayer) -> Unit) { preparedCallback = callback }
    fun setOnCompletionListener(callback: (MediaPlayer) -> Unit) { completedCallback = callback }
    fun setOnErrorListener(callback: (MediaPlayer, Int, Int) -> Boolean) { errorCallback = callback }
    fun setVideoScalingMode(mode: Int) { check("scaling" !in failures) }
    fun setDataSource(path: String) { check("source" !in failures) { "MediaPlayer source failed" } }
    fun prepareAsync() { check("prepare" !in failures) { "MediaPlayer prepare failed" } }
    fun start() { starts++; check(!released && "start" !in failures) { "MediaPlayer start failed" } }
    fun pause() { pauses++; check(!released && "pause" !in failures) { "MediaPlayer pause failed" } }
    fun seekTo(value: Int) { seeks++; check(!released && "seek" !in failures); position = value }
    fun setSurface(value: Surface?) {
        check(!released && "surface" !in failures) { "MediaPlayer surface failed" }; surface = value
        check("surface after" !in failures) { "MediaPlayer surface failed after effect" }
    }
    fun release() {
        releases++; check("release" !in failures) { "MediaPlayer release failed" }; released = true; surface = null
        check("release after" !in failures) { "MediaPlayer release failed after effect" }
    }
}

class MediaMetadataRetriever {
    companion object {
        const val METADATA_KEY_VIDEO_WIDTH = 18; const val METADATA_KEY_VIDEO_HEIGHT = 19; const val METADATA_KEY_VIDEO_ROTATION = 24
        const val METADATA_KEY_DURATION = 9; const val METADATA_KEY_MIMETYPE = 12; const val OPTION_CLOSEST_SYNC = 2
        val created = mutableListOf<MediaMetadataRetriever>()
        var nextFailures = emptySet<String>()
    }
    var failures = nextFailures
    var released = false
    var releases = 0
    var width = "1280"; var height = "720"; var rotation = "90"
    var duration = "10000"; var mime = "video/mp4"
    var lastFrame: android.graphics.Bitmap? = null
    var scaledReads = 0
    init { created.add(this) }
    fun setDataSource(path: String) { check("source" !in failures) { "Metadata source failed" } }
    fun extractMetadata(key: Int): String? = when (key) {
        METADATA_KEY_VIDEO_WIDTH -> width; METADATA_KEY_VIDEO_HEIGHT -> height; METADATA_KEY_VIDEO_ROTATION -> rotation
        METADATA_KEY_DURATION -> duration; METADATA_KEY_MIMETYPE -> mime; else -> null
    }
    fun getFrameAtTime(time: Long): android.graphics.Bitmap? {
        check("frame" !in failures)
        return android.graphics.Bitmap(width.toInt(), height.toInt()).also { lastFrame = it }
    }
    fun getScaledFrameAtTime(time: Long, option: Int, maxWidth: Int, maxHeight: Int): android.graphics.Bitmap? {
        scaledReads++; check("frame" !in failures)
        val scale = minOf(maxWidth.toDouble() / width.toInt(), maxHeight.toDouble() / height.toInt(), 1.0)
        return android.graphics.Bitmap((width.toInt() * scale).toInt(), (height.toInt() * scale).toInt()).also { lastFrame = it }
    }
    fun release() {
        releases++; check("release" !in failures) { "Metadata release failed" }; released = true
        check("release after" !in failures) { "Metadata release failed after effect" }
    }
}
