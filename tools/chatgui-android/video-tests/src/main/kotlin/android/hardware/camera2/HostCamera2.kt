package android.hardware.camera2

import android.graphics.SurfaceTexture
import android.os.Handler
import android.util.Size
import android.util.Range
import android.view.Surface
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ConcurrentLinkedQueue

class CameraAccessException(val reason: Int) : Exception("camera access failure reason=$reason") {
    companion object { const val CAMERA_IN_USE = 4; const val CAMERA_DISABLED = 1; const val CAMERA_DISCONNECTED = 2 }
}

class HostStreamMap {
    fun getOutputSizes(format: Int) = arrayOf(Size(640, 480))
    fun getOutputSizes(type: Class<*>) = arrayOf(Size(640, 480))
    fun getOutputMinFrameDuration(format: Int, size: Size) = 33_333_333L
}
class CameraCharacteristics {
    class Key<T>(val name: String)
    companion object {
        val SCALER_STREAM_CONFIGURATION_MAP = Key<HostStreamMap>("map")
        val CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES = Key<Array<Range<Int>>>("fps")
        val SENSOR_ORIENTATION = Key<Int>("orientation")
        val LENS_FACING = Key<Int>("facing")
        val SENSOR_INFO_TIMESTAMP_SOURCE = Key<Int>("timestamp")
        const val LENS_FACING_FRONT = 0; const val SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME = 1
    }
    val values: MutableMap<Key<*>, Any> = mutableMapOf(SCALER_STREAM_CONFIGURATION_MAP to HostStreamMap(),
        CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES to arrayOf(Range(30, 30)), SENSOR_ORIENTATION to 0,
        LENS_FACING to 1, SENSOR_INFO_TIMESTAMP_SOURCE to SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME)
    @Suppress("UNCHECKED_CAST") fun <T> get(key: Key<T>): T? = values[key] as T?
}
class CaptureRequest {
    class Key<T>
    companion object { val CONTROL_AE_TARGET_FPS_RANGE = Key<Range<Int>>() }
    class Builder {
        fun addTarget(surface: Surface) {}
        fun <T> set(key: Key<T>, value: T) {}
        fun build() = CaptureRequest()
    }
}
object CaptureResult {
    val CONTROL_AE_TARGET_FPS_RANGE = CameraCharacteristics.Key<Range<Int>>("ae range")
    val SENSOR_FRAME_DURATION = CameraCharacteristics.Key<Long>("frame duration")
    val SENSOR_EXPOSURE_TIME = CameraCharacteristics.Key<Long>("exposure")
    val SENSOR_TIMESTAMP = CameraCharacteristics.Key<Long>("timestamp")
}
class TotalCaptureResult { fun <T> get(key: CameraCharacteristics.Key<T>): T? = null }
class CaptureFailure { val frameNumber = 0L; val reason = 0; fun wasImageCaptured() = false }

/** Реальные callback-поколения SDK воспроизводит тест; камера/сессия закрываются независимо. */
class CameraManager {
    companion object { var shared = CameraManager() }
    data class Open(val device: CameraDevice, val callback: CameraDevice.StateCallback, val handler: Handler, @Volatile var delivered: Boolean = false)
    val opened = CopyOnWriteArrayList<Open>()
    val sessions = CopyOnWriteArrayList<CameraCaptureSession>()
    val characteristics = CameraCharacteristics()
    var failures = emptySet<String>()
    var nextDeviceFailures = emptySet<String>()
    var nextSessionFailures = emptySet<String>()
    val sessionFailures = ConcurrentLinkedQueue<Set<String>>()
    var autoOpened = true
    var autoClosed = true
    var autoConfigured = true
    fun getCameraCharacteristics(id: String): CameraCharacteristics {
        check("characteristics" !in failures) { "camera characteristics failed" }; return characteristics
    }
    fun openCamera(id: String, callback: CameraDevice.StateCallback, handler: Handler) {
        check("open" !in failures) { "camera open failed" }
        val device = CameraDevice(id, this, callback, handler).also { it.failures = nextDeviceFailures }
        opened.add(Open(device, callback, handler))
        if ("open after" in failures) {
            handler.post { opened.last().delivered = true; callback.onError(device, 1) }
            throw CameraAccessException(CameraAccessException.CAMERA_IN_USE)
        }
        if (autoOpened) deliverOpened(opened.lastIndex)
    }
    fun deliverOpened(index: Int) { val r = opened[index]; r.handler.post { r.delivered = true; r.callback.onOpened(r.device) } }
    fun deliverClosed(index: Int) { val r = opened[index]; r.handler.post { r.callback.onClosed(r.device) } }
}
class CameraDevice(val id: String, private val owner: CameraManager, private val callback: StateCallback, private val handler: Handler) {
    companion object { const val TEMPLATE_RECORD = 3 }
    abstract class StateCallback {
        abstract fun onOpened(device: CameraDevice)
        abstract fun onDisconnected(device: CameraDevice)
        abstract fun onError(device: CameraDevice, error: Int)
        open fun onClosed(device: CameraDevice) {}
    }
    var failures = emptySet<String>()
    var closed = false
        private set
    var closeCalls = 0
        private set
    fun createCaptureRequest(type: Int): CaptureRequest.Builder { check("request" !in failures); return CaptureRequest.Builder() }
    fun createCaptureSession(targets: List<Surface>, callback: CameraCaptureSession.StateCallback, delivery: Handler) =
        createSession(targets, callback, delivery, highSpeed = false)
    fun createConstrainedHighSpeedCaptureSession(targets: List<Surface>, callback: CameraCaptureSession.StateCallback, delivery: Handler) =
        createSession(targets, callback, delivery, highSpeed = true)
    private fun createSession(targets: List<Surface>, callback: CameraCaptureSession.StateCallback, delivery: Handler, highSpeed: Boolean) {
        check("session" !in failures) { "session creation failed" }
        val session = (if (highSpeed) CameraConstrainedHighSpeedCaptureSession(this, callback, delivery)
            else CameraCaptureSession(this, callback, delivery)).also { it.failures = owner.sessionFailures.poll() ?: owner.nextSessionFailures }
        owner.sessions.add(session)
        if (owner.autoConfigured) delivery.post {
            if ("configure" in session.failures || "session after" in failures) session.failConfiguration()
            else callback.onConfigured(session)
        }
        if ("session after" in failures) throw CameraAccessException(CameraAccessException.CAMERA_DISCONNECTED)
    }
    fun close() {
        closeCalls++; check("close" !in failures) { "camera close failed" }
        if (!closed) {
            closed = true
            owner.sessions.filter { it.device === this }.forEach { it.forceClose() }
            if (owner.autoClosed) handler.post { callback.onClosed(this) }
        }
        check("close after" !in failures) { "camera close failed after effect" }
    }
}
open class CameraCaptureSession(val device: CameraDevice, val callback: StateCallback, private val handler: Handler) {
    open class CaptureCallback {
        open fun onCaptureCompleted(session: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {}
        open fun onCaptureFailed(session: CameraCaptureSession, request: CaptureRequest, failed: CaptureFailure) {}
    }
    abstract class StateCallback {
        abstract fun onConfigured(session: CameraCaptureSession)
        abstract fun onConfigureFailed(session: CameraCaptureSession)
        open fun onClosed(session: CameraCaptureSession) {}
    }
    var failures = emptySet<String>()
    var closed = false
        private set
    var repeating = false
        private set
    fun setRepeatingRequest(request: CaptureRequest, listener: Any?, delivery: Handler) {
        check("request" !in failures) { "repeating request failed" }; repeating = true
    }
    fun close() { check("close" !in failures) { "session close failed" }; forceClose(); check("close after" !in failures) }
    internal fun failConfiguration() { closed = true; callback.onConfigureFailed(this) }
    internal fun forceClose() {
        if (closed) return
        closed = true; repeating = false; handler.post { callback.onClosed(this) }
    }
}
class CameraConstrainedHighSpeedCaptureSession(device: CameraDevice, callback: CameraCaptureSession.StateCallback, handler: Handler) :
    CameraCaptureSession(device, callback, handler) {
    fun createHighSpeedRequestList(request: CaptureRequest) = listOf(request)
    fun setRepeatingBurst(requests: List<CaptureRequest>, callback: CaptureCallback, handler: Handler) =
        setRepeatingRequest(requests.first(), callback, handler)
}
