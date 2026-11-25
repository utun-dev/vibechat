package com.utun.chat.data

import android.graphics.SurfaceTexture
import android.view.Surface
import android.os.Handler
import android.os.Looper
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.concurrent.ConcurrentHashMap

/** Положение сенсора конкретной камеры; ручной поворот относится только к трансляции. */
data class StreamPreviewCamera(val sensor: Int, val front: Boolean) {
    fun rotation(displayDegrees: Int): Int = (sensor + (if (front) displayDegrees else -displayDegrees) + 360) % 360
}

data class StreamPreviewFormat(val width: Int, val height: Int, val camera: StreamPreviewCamera, val realtimeTimestamp: Boolean = false) {
    fun aspect(displayDegrees: Int): Float = if (camera.rotation(displayDegrees) % 180 == 0) width.toFloat() / height else height.toFloat() / width

    /** TextureView уже учитывает SENSOR_ORIENTATION; остаётся компенсация поворота экрана. */
    fun textureRotation(displayDegrees: Int): Int = (360 - displayDegrees) % 360

    fun corners(displayDegrees: Int): FloatArray = when (textureRotation(displayDegrees)) {
        90 -> floatArrayOf(1f, 0f, 1f, 1f, 0f, 1f, 0f, 0f)
        180 -> floatArrayOf(1f, 1f, 0f, 1f, 0f, 0f, 1f, 0f)
        270 -> floatArrayOf(0f, 1f, 0f, 0f, 1f, 0f, 1f, 1f)
        else -> floatArrayOf(0f, 0f, 1f, 0f, 1f, 1f, 0f, 1f)
    }
}

data class StreamPreviewState(val format: StreamPreviewFormat? = null, val active: Boolean = false, val failed: Boolean = false)

/** UI, ожидающая команда camera thread и каждая CameraCaptureSession держат отдельную ссылку. */
internal class StreamPreviewLease(private val close: () -> Unit) {
    private var references = 1
    @Synchronized fun retain() { check(references > 0); references++ }
    @Synchronized fun release() { check(references > 0); if (references == 1) close(); references-- }
}

/** Физический output живёт до последней ссылки; отказ одного Android release не пропускает другой. */
private class StreamPreviewResource(val texture: SurfaceTexture) {
    companion object { private val retained = ConcurrentHashMap.newKeySet<StreamPreviewResource>() }
    val surface = Surface(texture)
    private var surfaceOwned = true
    private var textureOwned = true
    private var closing = false
    private val handler = Handler(Looper.getMainLooper())
    private var delay = 250L
    private var scheduled = false
    private val retry = Runnable { synchronized(this) { scheduled = false; close() } }
    val lease = StreamPreviewLease { close() }

    @Synchronized fun close(retryOnly: Boolean = false) {
        if (retryOnly && !closing) return
        closing = true
        if (!surfaceOwned && !textureOwned) return
        if (surfaceOwned) try { surface.release(); surfaceOwned = false } catch (e: Exception) {
            LogManager.addLog("ERROR", "video", "preview Surface release failed resource=${System.identityHashCode(this)}; retained: $e")
        }
        if (textureOwned) try { texture.release(); textureOwned = false } catch (e: Exception) {
            LogManager.addLog("ERROR", "video", "preview texture release failed resource=${System.identityHashCode(this)}; retained: $e")
        }
        if (!surfaceOwned && !textureOwned) {
            handler.removeCallbacks(retry); scheduled = false; retained.remove(this)
            LogManager.videoTrace("direct preview output released resource=${System.identityHashCode(this)}")
        } else {
            retained.add(this)
            if (scheduled) return
            scheduled = handler.postDelayed(retry, delay)
            if (!scheduled) LogManager.addLog("ERROR", "video", "preview cleanup retry rejected resource=${System.identityHashCode(this)}; retained")
            else delay = (delay * 2).coerceAtMost(30_000)
        }
    }
}

/** Отдельная идемпотентная ссылка UI/команды/сессии на один output; retain возвращает новую ссылку.
 * Последний release передаёт неосвобождённые Android ресурсы process cleanup с retry 250 мс..30 с. */
class StreamPreviewTarget private constructor(private val resource: StreamPreviewResource) {
    internal constructor(texture: SurfaceTexture) : this(StreamPreviewResource(texture))
    val texture get() = resource.texture
    val surface get() = resource.surface
    @Volatile internal var released = false
        private set
    @Synchronized internal fun retain(): StreamPreviewTarget {
        check(!released) { LogManager.addLog("ERROR", "video", "retain released preview target"); "Preview target already released" }
        resource.lease.retain()
        return StreamPreviewTarget(resource)
    }
    internal fun sameSource(other: StreamPreviewTarget?) = resource === other?.resource
    @Synchronized fun release() {
        if (!released) { resource.lease.release(); released = true } else resource.close(retryOnly = true)
    }
}

/** Main: собственные ссылки TextureView. close снимает UI leases сразу, даже без callback destroy.
 * Ссылки остаются в карте до destroy, чтобы вернуть false и не дать framework закрыть texture работающей camera session. */
internal class StreamPreviewViewResources(private val preview: StreamPreview) {
    private val targets = java.util.IdentityHashMap<SurfaceTexture, StreamPreviewTarget>()
    private var closed = false
    fun attach(texture: SurfaceTexture): StreamPreviewTarget? {
        targets[texture]?.let { return if (closed) null else it }
        val target = if (closed) StreamPreviewTarget(texture) else preview.attach(texture)
        targets[texture] = target
        if (closed) { target.release(); return null }
        return target
    }
    fun destroy(texture: SurfaceTexture): Boolean {
        val target = targets.remove(texture) ?: return true
        preview.detach(target); target.release()
        return false
    }
    fun close() {
        closed = true
        targets.values.forEach { preview.detach(it); it.release() }
    }
}

/** Прямой выход камеры. UI не владеет camera/codec; listener получает собственную ссылку на target.
 * При исключении listener ссылка отменяется; queued receiver обязан игнорировать released target.
 * Нулевой target снимает output; callback выполняется под монитором и не должен ждать camera worker. */
class StreamPreview {
    private var target: StreamPreviewTarget? = null
    private var owner: ((StreamPreviewTarget?) -> Unit)? = null
    private var closed = false
    private var visible = true
    private var deliveryFailed = false
    private val mutable = MutableStateFlow(StreamPreviewState())
    val state: StateFlow<StreamPreviewState> = mutable

    @Synchronized fun attach(texture: SurfaceTexture): StreamPreviewTarget {
        val next = StreamPreviewTarget(texture)
        if (closed) { LogManager.addLog("WARN", "video", "direct preview attach after capture stopped"); return next }
        val previous = target
        target = next.retain(); publish(); previous?.release()
        LogManager.addLog("INFO", "video", "direct preview attached target=${System.identityHashCode(next)} bound=${owner != null}")
        return next // Первую ссылку освобождает UI в onSurfaceTextureDestroyed, возвращая false.
    }

    @Synchronized fun detach(previous: StreamPreviewTarget) {
        val current = target ?: return
        if (!current.sameSource(previous)) return
        target = null; publish(); current.release()
        mutable.value = mutable.value.copy(active = false)
        LogManager.addLog("INFO", "video", "direct preview detached target=${System.identityHashCode(previous)}")
    }

    @Synchronized internal fun bind(listener: (StreamPreviewTarget?) -> Unit) {
        check(!closed && owner == null) {
            LogManager.addLog("ERROR", "video", "preview bind rejected closed=$closed ownerPresent=${owner != null}")
            "Preview already bound/closed"
        }
        owner = listener; publish()
    }

    private fun publish() {
        val listener = owner ?: return
        val offered = if (visible) target?.retain() else null
        try { listener(offered); deliveryFailed = false } catch (e: Exception) {
            offered?.release(); deliveryFailed = true
            mutable.value = mutable.value.copy(active = false, failed = true)
            LogManager.addLog("ERROR", "video", "preview output callback failed; offered lease cancelled: $e")
        }
    }

    /** Lifecycle отключает output сразу, даже когда Compose уже приостановил recomposition. */
    @Synchronized fun setVisible(value: Boolean) {
        if ((visible == value && !deliveryFailed) || closed) return
        visible = value; publish()
        if (!visible) mutable.value = mutable.value.copy(active = false)
        LogManager.addLog("INFO", "video", "direct preview visibility=$visible target=${target?.let(System::identityHashCode)}")
    }

    @Synchronized internal fun update(format: StreamPreviewFormat?, active: Boolean = false, failed: Boolean = false) {
        if (closed) { LogManager.addLog("WARN", "video", "preview update ignored after close"); return }
        mutable.value = StreamPreviewState(format, active, failed)
    }

    @Synchronized internal fun close() {
        closed = true; owner = null; target?.release(); target = null
        mutable.value = mutable.value.copy(active = false)
    }
}
