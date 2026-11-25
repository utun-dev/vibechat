package com.utun.chat.data

import android.media.MediaMetadataRetriever
import android.graphics.Bitmap
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.util.concurrent.ConcurrentHashMap

/** Геометрия coded frame с метаданными поворота; display размеры учитывают 90/270°. */
internal data class VideoInfo(val width: Int, val height: Int, val rotation: Int) {
    val displayWidth: Int get() = if (rotation == 90 || rotation == 270) height else width
    val displayHeight: Int get() = if (rotation == 90 || rotation == 270) width else height
    val aspect: Float get() = if (displayHeight > 0) displayWidth.toFloat() / displayHeight else 0f
}
internal data class VideoFilePreview(val bitmap: Bitmap?, val durationMs: Int)
internal data class VideoFileSubmission(val durationMs: Int, val width: Int, val height: Int)

/** Один IO caller читает metadata внутри use; после завершения чтения допускается только повтор close.
 * Неудавшийся release удерживает retriever до SDK ACK, retry не обращается к UI/native core. */
internal class VideoFileMetadata : AutoCloseable {
    companion object { private val retained = ConcurrentHashMap.newKeySet<VideoFileMetadata>() }
    private var retriever: MediaMetadataRetriever? = MediaMetadataRetriever()
    private val main = Handler(Looper.getMainLooper())
    private var delay = 250L
    private var scheduled = false
    private var closing = false
    private val retry = Runnable {
        synchronized(this) { scheduled = false }
        try { close() } catch (e: Exception) { LogManager.addLog("WARN", "VideoPlayer", "Metadata cleanup pending: $e") }
    }
    @Synchronized fun read(path: String): VideoInfo {
        check(!closing) { "Metadata reader closing" }
        val current = checkNotNull(retriever) { "Metadata reader closed" }
        current.setDataSource(path)
        val width = current.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0
        val height = current.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0
        val rotation = current.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
        require(width > 0 && height > 0 && rotation in listOf(0, 90, 180, 270)) { "Invalid video geometry ${width}x$height rotation=$rotation" }
        return VideoInfo(width, height, rotation)
    }
    /** IO: готовый MP4, длительность 1..86400000 мс и coded размеры; caller передаёт исходник без повторного транскода. */
    @Synchronized fun submission(path: String): VideoFileSubmission {
        val info = read(path)
        val current = checkNotNull(retriever)
        require(current.extractMetadata(MediaMetadataRetriever.METADATA_KEY_MIMETYPE) == "video/mp4") { "Expected MP4 video" }
        val duration = current.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toIntOrNull() ?: 0
        require(duration in 1..86_400_000 && info.width <= 16384 && info.height <= 16384) { "Invalid MP4 metadata durationMs=$duration" }
        return VideoFileSubmission(duration, info.width, info.height)
    }
    /** IO: thumbnail не больше 640 по каждой стороне; Bitmap принадлежит caller, duration в мс.
     * API 26 сначала извлекает полный кадр и уменьшает до передачи UI; null bitmap не запрещает воспроизведение. */
    @Synchronized fun preview(path: String): VideoFilePreview {
        check(!closing) { "Metadata reader closing" }
        val current = checkNotNull(retriever) { "Metadata reader closed" }
        current.setDataSource(path)
        val duration = current.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toIntOrNull()?.coerceAtLeast(0) ?: 0
        val raw = if (Build.VERSION.SDK_INT >= 27) current.getScaledFrameAtTime(1_000_000,
            MediaMetadataRetriever.OPTION_CLOSEST_SYNC, 640, 640) else current.getFrameAtTime(1_000_000)
        val image = if (raw != null && maxOf(raw.width, raw.height) > 640) {
            val scale = 640.0 / maxOf(raw.width, raw.height)
            Bitmap.createScaledBitmap(raw, (raw.width * scale).toInt().coerceAtLeast(1),
                (raw.height * scale).toInt().coerceAtLeast(1), true).also { if (it !== raw) raw.recycle() }
        } else raw
        if (image == null) LogManager.addLog("WARN", "VideoPlayer", "Video preview has no frame owner=${System.identityHashCode(this)}")
        return VideoFilePreview(image, duration)
    }
    @Synchronized override fun close() {
        closing = true
        val current = retriever ?: return
        try {
            current.release(); retriever = null
            main.removeCallbacks(retry); scheduled = false; retained.remove(this)
            LogManager.addLog("DEBUG", "VideoPlayer", "Metadata reader released owner=${System.identityHashCode(this)}")
        } catch (e: Exception) {
            retained.add(this)
            LogManager.addLog("ERROR", "VideoPlayer", "Metadata release failed; owner retained: $e")
            if (!scheduled) {
                scheduled = main.postDelayed(retry, delay)
                if (!scheduled) LogManager.addLog("ERROR", "VideoPlayer", "Metadata cleanup timer rejected; owner retained")
                else delay = (delay * 2).coerceAtMost(30_000)
            }
            throw e
        }
    }
}
