package com.utun.chat.data

import android.graphics.SurfaceTexture
import android.media.MediaPlayer
import android.os.Handler
import android.os.Looper
import com.utun.chat.R
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.IdentityHashMap
import java.util.concurrent.ConcurrentHashMap

internal data class VideoFilePlayback(val prepared: Boolean = false, val playing: Boolean = false,
                                      val durationMs: Int = 0, val positionMs: Int = 0, val error: Int? = null)

/** Main: пассивный владелец локального MediaPlayer и texture leases. Сначала сохранить, затем start(path).
 * Команды разрешены после prepared, состояние меняется после успешного вызова SDK; pause сохраняется при смене Surface.
 * UI destroy возвращает false для принятых texture. Release player предшествует снятию его leases;
 * отказ release сохраняет владельца и повторяется через Handler независимо от composition. Native core не используется. */
internal class VideoFilePlayer(onCompleted: () -> Unit) : AutoCloseable {
    companion object { private val retained = ConcurrentHashMap.newKeySet<VideoFilePlayer>() }
    private val main = Handler(Looper.getMainLooper())
    private val id = System.identityHashCode(this)
    private val mutable = MutableStateFlow(VideoFilePlayback())
    val state: StateFlow<VideoFilePlayback> = mutable
    private var completion: (() -> Unit)? = onCompleted
    private var player: MediaPlayer? = null
    private val uiTargets = IdentityHashMap<SurfaceTexture, StreamPreviewTarget>()
    private val playerTargets = mutableSetOf<StreamPreviewTarget>()
    private var bound: SurfaceTexture? = null
    private var wantsPlay = true
    private var started = false
    private var disposed = false
    private var retiring = false
    private var scheduled = false
    private var delay = 250L
    private val retry = Runnable { scheduled = false; cleanup() }

    fun start(path: String) {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Video player start requires main" }
        check(!started && !disposed) { "Video player already started/closed" }
        started = true
        try {
            val owned = MediaPlayer(); player = owned
            owned.setOnPreparedListener { current ->
                if (!retiring && player === current) try {
                    current.setVideoScalingMode(MediaPlayer.VIDEO_SCALING_MODE_SCALE_TO_FIT)
                    mutable.value = mutable.value.copy(prepared = true, durationMs = current.duration.coerceAtLeast(0))
                    LogManager.addLog("INFO", "VideoPlayer", "Prepared owner=$id ${current.videoWidth}x${current.videoHeight} durationMs=${current.duration}")
                    if (wantsPlay && bound != null) play()
                } catch (e: Exception) { fail("prepare callback", e) }
            }
            owned.setOnCompletionListener {
                if (!retiring && player === it) {
                    wantsPlay = false
                    mutable.value = mutable.value.copy(playing = false, positionMs = mutable.value.durationMs)
                    try { completion?.invoke() } catch (e: Exception) { fail("completion callback", e) }
                }
            }
            owned.setOnErrorListener { current, what, extra ->
                if (!retiring && player === current) fail("SDK error", IllegalStateException("what=$what extra=$extra"))
                true
            }
            bound?.let { uiTargets[it] }?.let(::applySurface)
            if (!retiring) { owned.setDataSource(path); owned.prepareAsync() }
        } catch (e: Exception) { fail("open/prepare", e, R.string.video_open_failed) }
    }

    fun attach(texture: SurfaceTexture) {
        check(Looper.myLooper() == Looper.getMainLooper())
        if (uiTargets.containsKey(texture)) return
        val target = try { StreamPreviewTarget(texture) } catch (e: Exception) { fail("create Surface", e); return }
        uiTargets[texture] = target
        LogManager.addLog("DEBUG", "VideoPlayer", "Texture adopted owner=$id texture=${System.identityHashCode(texture)} disposed=$disposed")
        if (disposed) { target.release(); return }
        if (retiring) return // UI сохраняет собственную texture до destroy/close даже после ошибки player.
        bound = texture
        if (player != null) {
            applySurface(target)
            if (!retiring && mutable.value.prepared && wantsPlay) play()
        }
    }

    private fun applySurface(target: StreamPreviewTarget) {
        val offered = target.retain()
        playerTargets.add(offered)
        try {
            player!!.setSurface(target.surface)
            for (previous in playerTargets.toList()) if (previous !== offered) { previous.release(); playerTargets.remove(previous) }
        } catch (e: Exception) { fail("attach Surface", e) }
    }

    fun detach(texture: SurfaceTexture): Boolean {
        check(Looper.myLooper() == Looper.getMainLooper())
        val target = uiTargets.remove(texture) ?: return true
        if (bound === texture) {
            bound = null
            if (player != null && !retiring) try {
                player!!.setSurface(null)
                releasePlayerTargets()
            } catch (e: Exception) { fail("detach Surface", e) }
        }
        target.release()
        return false
    }

    fun play(): Boolean {
        if (!command("play") { it.start() }) return false
        wantsPlay = true; mutable.value = mutable.value.copy(playing = true)
        return true
    }
    fun pause(): Boolean {
        if (!command("pause") { it.pause() }) return false
        wantsPlay = false; mutable.value = mutable.value.copy(playing = false)
        return true
    }
    fun seek(positionMs: Int): Boolean {
        val position = positionMs.coerceIn(0, mutable.value.durationMs)
        if (!command("seek") { it.seekTo(position) }) return false
        mutable.value = mutable.value.copy(positionMs = position)
        return true
    }
    fun pollPosition() {
        if (mutable.value.prepared && !retiring) command("position") {
            mutable.value = mutable.value.copy(positionMs = it.currentPosition.coerceIn(0, mutable.value.durationMs))
        }
    }

    private inline fun command(action: String, block: (MediaPlayer) -> Unit): Boolean {
        check(Looper.myLooper() == Looper.getMainLooper())
        val current = player
        if (current == null || retiring || !mutable.value.prepared) {
            LogManager.addLog("WARN", "VideoPlayer", "Command unavailable action=$action prepared=${mutable.value.prepared} retiring=$retiring")
            return false
        }
        return try { block(current); true } catch (e: Exception) { fail(action, e); false }
    }

    private fun fail(stage: String, error: Exception, message: Int = R.string.video_playback_failed) {
        LogManager.addLog("ERROR", "VideoPlayer", "Operation failed stage=$stage owner=$id; resources retained through release: $error")
        retiring = true; wantsPlay = false; completion = null
        mutable.value = mutable.value.copy(playing = false, prepared = false, error = message)
        cleanup()
    }

    override fun close() {
        check(Looper.myLooper() == Looper.getMainLooper())
        disposed = true; retiring = true; completion = null
        mutable.value = mutable.value.copy(playing = false)
        uiTargets.values.forEach { it.release() }
        cleanup()
    }

    private fun releasePlayerTargets() { playerTargets.forEach { it.release() }; playerTargets.clear() }
    private fun cleanup() {
        if (player != null) try { player!!.release(); player = null } catch (e: Exception) {
            LogManager.addLog("ERROR", "VideoPlayer", "MediaPlayer release failed owner=$id; retained: $e")
        }
        if (player == null) {
            releasePlayerTargets()
            main.removeCallbacks(retry); scheduled = false; retained.remove(this)
            LogManager.addLog("INFO", "VideoPlayer", "Player released owner=$id disposed=$disposed")
        } else {
            retained.add(this)
            if (!scheduled) {
                scheduled = main.postDelayed(retry, delay)
                if (!scheduled) LogManager.addLog("ERROR", "VideoPlayer", "Cleanup timer rejected; owner retained")
                else delay = (delay * 2).coerceAtMost(30_000)
            }
        }
    }
}
