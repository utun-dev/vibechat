package com.utun.chat.data

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Handler
import android.os.Looper
import android.view.Surface

/** Один grant и один virtual display на сеанс. Сервис владеет проекцией, TX worker меняет Surface кодера. */
internal class ScreenProjection(private val context: Context, grant: Intent, private val stopped: () -> Unit) : AutoCloseable {
    private val density get() = context.resources.configuration.densityDpi
    private val main = Handler(Looper.getMainLooper())
    private val projection = checkNotNull(context.getSystemService(MediaProjectionManager::class.java)
        .getMediaProjection(Activity.RESULT_OK, grant))
    private var display: VirtualDisplay? = null
    private var closed = false
    private var contentPortrait: Boolean? = null
    @Volatile var portrait = displayPortrait()
        private set
    var onOrientationChanged: (() -> Unit)? = null

    private fun displayPortrait() = context.resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_PORTRAIT

    fun configurationChanged() { updateOrientation(contentPortrait ?: displayPortrait()) }

    private fun updateOrientation(next: Boolean) {
        if (closed || portrait == next) return
        portrait = next
        onOrientationChanged?.invoke()
    }

    fun stopAfterFailure() {
        if (!main.post { end() })
            LogManager.addLog("ERROR", "video", "screen projection failure callback rejected")
    }

    private fun end() {
        if (closed) return
        try { stopped() } finally { close() }
    }

    private val callback = object : MediaProjection.Callback() {
        override fun onStop() {
            if (closed) return
            LogManager.addLog("INFO", "video", "screen projection stopped by Android")
            end()
        }
        override fun onCapturedContentResize(width: Int, height: Int) {
            contentPortrait = height > width
            updateOrientation(height > width)
            LogManager.addLog("DEBUG", "video", "screen projection content resized to ${width}x$height")
        }
    }
    init {
        try { projection.registerCallback(callback, main) }
        catch (e: Exception) { projection.stop(); throw e }
    }

    @Synchronized fun attach(surface: Surface, width: Int, height: Int) {
        check(!closed) { "Screen projection already closed" }
        val active = display
        if (active == null) display = checkNotNull(projection.createVirtualDisplay("uTun", width, height, density,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, surface, null, null))
        else { active.resize(width, height, density); active.surface = surface }
        if (active == null) LogManager.addLog("INFO", "video", "screen projection started output=${width}x$height")
        else LogManager.videoTrace("screen projection surface attached ${width}x$height")
    }
    @Synchronized fun detach() { display?.surface = null }
    @Synchronized override fun close() {
        if (closed) return
        display?.release(); display = null
        projection.unregisterCallback(callback)
        projection.stop(); closed = true
        LogManager.addLog("INFO", "video", "screen projection released")
    }
}
