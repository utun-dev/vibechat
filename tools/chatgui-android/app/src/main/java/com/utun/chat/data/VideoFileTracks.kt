package com.utun.chat.data

import android.media.MediaExtractor
import android.media.MediaFormat
import android.os.Handler
import android.os.Looper
import java.util.concurrent.ConcurrentHashMap

/** IO caller проверяет дорожки готового файла внутри use; extractor сохраняется до успешного release.
 * Ошибка чтения не маскируется cleanup; повтор close работает также после отказа main-таймера. */
internal class VideoFileTracks : AutoCloseable {
    companion object { private val retained = ConcurrentHashMap.newKeySet<VideoFileTracks>() }
    private var extractor: MediaExtractor? = MediaExtractor()
    private var closing = false
    private val main = Handler(Looper.getMainLooper())
    private var delayMs = 250L
    private var scheduled = false
    private val retry = Runnable {
        synchronized(this) { scheduled = false }
        try { close() } catch (e: Exception) { LogManager.addLog("WARN", "Capture", "Track reader cleanup pending: $e") }
    }

    @Synchronized fun verify(path: String, expectedMime: String): String {
        check(!closing) { "Track reader closing" }
        val current = checkNotNull(extractor) { "Track reader closed" }
        current.setDataSource(path)
        val video = (0 until current.trackCount).map(current::getTrackFormat)
            .firstOrNull { it.getString(MediaFormat.KEY_MIME)?.startsWith("video/") == true }
        checkNotNull(video) { "Recorded file has no video track" }
        val mime = video.getString(MediaFormat.KEY_MIME)
        check(mime == expectedMime) { "Recorded codec mismatch expected=$expectedMime actual=$mime" }
        val rotation = if (video.containsKey(MediaFormat.KEY_ROTATION)) video.getInteger(MediaFormat.KEY_ROTATION) else 0
        val width = video.getInteger(MediaFormat.KEY_WIDTH); val height = video.getInteger(MediaFormat.KEY_HEIGHT)
        check(width > 0 && height > 0 && rotation in listOf(0, 90, 180, 270)) { "Invalid recorded geometry ${width}x$height rotation=$rotation" }
        return "$mime ${width}x${height} rotation=$rotation"
    }

    @Synchronized override fun close() {
        closing = true
        val current = extractor ?: return
        try {
            current.release(); extractor = null
            main.removeCallbacks(retry); scheduled = false; retained.remove(this)
            LogManager.addLog("DEBUG", "Capture", "Track reader released owner=${System.identityHashCode(this)}")
        } catch (e: Exception) {
            retained.add(this)
            LogManager.addLog("ERROR", "Capture", "Track reader release failed; owner retained: $e")
            if (!scheduled) {
                scheduled = main.postDelayed(retry, delayMs)
                if (scheduled) delayMs = (delayMs * 2).coerceAtMost(30_000)
                else LogManager.addLog("ERROR", "Capture", "Track reader cleanup timer rejected; owner retained")
            }
            throw e
        }
    }
}
