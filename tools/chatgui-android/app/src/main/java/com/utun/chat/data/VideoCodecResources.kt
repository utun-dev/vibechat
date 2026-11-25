package com.utun.chat.data

import android.media.Image
import android.media.MediaCodec
import android.os.Handler
import android.os.Looper
import java.util.concurrent.ConcurrentHashMap

/** Один worker владеет codec и его input Images. После close разрешены только повторные попытки cleanup.
 * Ошибка close сохраняет владельца независимо от вызывающего кода, включая неудавшийся конструктор.
 * Retry использует только Android-ресурсы и не обращается к остановленному native-ядру.
 */
internal class VideoCodecResources(val codec: MediaCodec, private val name: String) : AutoCloseable {
    companion object { private val retained = ConcurrentHashMap.newKeySet<VideoCodecResources>() }
    private val handler = Handler(Looper.getMainLooper())
    private val images = linkedSetOf<Image>()
    private var stopped = false
    private var released = false
    private var scheduled = false
    private var delay = 250L
    private val retry = Runnable {
        synchronized(this) { scheduled = false }
        try { close() } catch (e: Exception) { LogManager.addLog("WARN", "video", "codec cleanup retry pending: $e") }
    }

    /** Сохраняем полученный input до любой проверки/копирования; closeImage подтверждает его освобождение. */
    @Synchronized fun own(image: Image): Image { images.add(image); return image }

    @Synchronized fun closeImage(image: Image) {
        if (image !in images) return
        try { image.close(); images.remove(image) } catch (e: Exception) {
            LogManager.addLog("ERROR", "video", "codec input Image close failed name=$name; retained: $e")
            throw e
        }
    }

    @Synchronized override fun close() {
        var failure: Exception? = null
        for (image in images.toList()) try { closeImage(image) } catch (e: Exception) { failure = e }
        if (!released) {
            if (!stopped) try { codec.stop(); stopped = true } catch (e: Exception) {
                LogManager.addLog("WARN", "video", "codec stop failed name=$name; attempting release: $e")
            }
            try { codec.release(); released = true } catch (e: Exception) {
                failure = e
                LogManager.addLog("ERROR", "video", "codec release failed name=$name; retained: $e")
            }
        }
        if (released && images.isEmpty()) {
            handler.removeCallbacks(retry); scheduled = false
            if (retained.remove(this)) LogManager.addLog("INFO", "video", "codec cleanup confirmed name=$name")
            return
        }
        retained.add(this)
        if (!scheduled) {
            scheduled = handler.postDelayed(retry, delay)
            if (!scheduled) LogManager.addLog("ERROR", "video", "codec cleanup retry rejected name=$name; owner retained")
            else { LogManager.addLog("DEBUG", "video", "codec cleanup scheduled name=$name delayMs=$delay"); delay = (delay * 2).coerceAtMost(30_000) }
        }
        throw IllegalStateException("Codec cleanup pending name=$name released=$released images=${images.size}", failure)
    }
}
