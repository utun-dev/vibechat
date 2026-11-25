package com.utun.chat.data

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.os.Looper
import android.util.Size
import android.view.Surface
import android.view.OrientationEventListener
import com.utun.chat.R
import com.utun.chat.ChatApplication
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap

data class CallVideoMode(val width: Int, val height: Int, val fps: Int)
data class CallVideoCamera(val id: String, val name: String, val modes: List<CallVideoMode>, val sensor: Int, val front: Boolean,
    val focal35: Float)
data class CallVideoSettings(val enabled: Boolean = false, val camera: String = "", val width: Int = 640,
    val height: Int = 480, val codec: Int = 0, val fps: Int = 30, val screen: Boolean = false) {
    /** Восстановить поддерживаемый режим; включение передачи относится только к текущему звонку. */
    fun restore(state: CallVideoState): CallVideoSettings {
        val format = codec.takeIf { it == 0 || (it in state.codecs && state.cameras.any { camera ->
            state.modes(camera.id, it).isNotEmpty()
        }) } ?: 0
        val selected = state.cameras.firstOrNull { it.id == camera && state.modes(it.id, format).isNotEmpty() }
            ?: state.cameras.firstOrNull { state.modes(it.id, format).isNotEmpty() }
        val modes = selected?.let { state.modes(it.id, format) }.orEmpty()
        val mode = modes.firstOrNull { it.width == width && it.height == height && it.fps == fps }
            ?: modes.filter { it.width == width && it.height == height && it.fps <= fps }.maxByOrNull { it.fps }
            ?: modes.minWithOrNull(compareBy<CallVideoMode> { it.width * it.height }.thenBy { it.fps })
        return copy(enabled = false, camera = selected?.id.orEmpty(), codec = format,
            width = mode?.width ?: 640, height = mode?.height ?: 480, fps = mode?.fps ?: 30)
    }
}
data class CallVideoState(val ready: Boolean = false, val settings: CallVideoSettings = CallVideoSettings(),
    val cameras: List<CallVideoCamera> = emptyList(), val codecs: List<Int> = emptyList(), val bitrate: Int = 0,
    val range: CallVideoBitrateRange = CallVideoBitrateRange(), val status: String = "", val transmitting: Boolean = false,
    val codecModes: Map<Int, List<CallVideoMode>> = emptyMap(), val notice: Int? = null,
    val sharingScreen: Boolean = false, val activeCodec: Int = 0) { // Фактически отправленный codec; 0 до первого принятого TX кадра.
    fun modes(camera: String = settings.camera, codec: Int = settings.codec, screen: Boolean = settings.screen): List<CallVideoMode> {
        val allowed = if (codec == 0) codecModes.values.flatten() else codecModes[codec].orEmpty()
        if (screen) return allowed.distinct()
        return cameras.firstOrNull { it.id == camera }?.modes.orEmpty().filter { it in allowed }
    }
    fun sizes(camera: String = settings.camera, codec: Int = settings.codec) = modes(camera, codec).map { Size(it.width, it.height) }.distinct()
}

/** Сервис сначала сохраняет пассивный engine, затем вызывает start на main.
 * Capture/encode/probe принадлежат TX worker, decode — RX; экран передаёт Surface и выбирает режим.
 * stop на main возвращает true после полного cleanup, false сохраняет владельца у сервиса для повторного stop.
 */
class CallVideoEngine(private val context: Context, private val id: Long) {
    companion object { private val retained = ConcurrentHashMap.newKeySet<CallVideoEngine>() }
    private val createdAt = System.nanoTime()
    private val mutable = MutableStateFlow(CallVideoState())
    val state: StateFlow<CallVideoState> = mutable
    private val thread = HandlerThread("call-video-tx")
    private lateinit var handler: Handler
    @Volatile private var started = false
    private val manager = context.getSystemService(CameraManager::class.java)
    private val player = CallVideoPlayer(id) { message ->
        if (!handler.post { if (!stopped) report(message) }) LogManager.addLog("WARN", "call", "video RX failure report rejected id=$id")
    }
    internal val display: StateFlow<CallVideoFormat?> = player.display
    val preview = StreamPreview()
    private var previewTarget: StreamPreviewTarget? = null
    private var sessionSerial = 0L
    private val sessions = mutableMapOf<Long, Pair<CameraDevice, () -> Unit>>()
    @Volatile private var stopped = false
    @Volatile private var cleanupComplete = false
    @Volatile private var rxReleased = false
    private var cleanupQueued = false // Под монитором engine: один callback, более ранний запрос заменяет таймер.
    private var cleanupDue = 0L
    private var cleanupDelay = 250L
    private var portOwned = false // TX: выставляется до native open, снимается только после close.
    private val ownedReaders = linkedSetOf<ImageReader>()
    private val ownedImages = linkedSetOf<android.media.Image>()
    private val closingCameras = linkedSetOf<CameraDevice>()
    private val closedCameras = linkedSetOf<CameraDevice>() // Отсекает повтор уже подтверждённого SDK callback.
    private val ownedSessions = linkedMapOf<CameraCaptureSession, CameraDevice>()
    private val closingSessions = linkedSetOf<CameraCaptureSession>()
    private val pendingEncoders = linkedSetOf<CallVideoCodec>()
    private var openingToken = 0L
    private var nativeUnavailable = false
    private val cleanupTask = Runnable {
        synchronized(this) { cleanupQueued = false }
        cleanupResources()
    }
    private var encoders = emptyMap<Int, CallVideoCodec.Companion.Encoder>()
    private var caps = ByteArray(64)
    private var wanted = CallVideoSettings()
    private var active: CallVideoSettings? = null
    private var format: CallVideoFormat? = null
    private var encoder: CallVideoCodec? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    private var opening = false
    private var closing = false
    private var cameraToken = 0L
    private var generation = 0L
    private var frame = 0L
    private var lastPts = -1L
    private var keySerial = -1L
    private var needKey = true
    private var request = 0L
    private var pendingRequest = 0L
    private var pendingRate = 0
    private var confirmingRequest = 0L
    private var confirmingPts = -1L
    private var confirmingKey = false
    private var realtime = false
    private var sensorOffset: Long? = null
    private var openedAt = 0L
    private var lastCaptureAt = 0L
    private var reportAt = 0L
    private var encodedBytes = 0L
    private var encodedFrames = 0
    private var encodedKeys = 0
    private var encodedMaxBytes = 0
    private var encodedMaxGapUs = 0L
    private var lastEncodedPts = -1L
    private var rejectedGeneration = 0L
    private var excluded = emptySet<Int>()
    private data class Pending(val metadata: LongArray, val bytes: ByteArray)
    private val pendingFrames = java.util.ArrayDeque<Pending>()
    private var canEncode = false
    private var switching = false
    private var encoderDrained = false
    private var appliedRotation = -1
    private var transformMaxUs = 0L
    private var inputWaitMaxUs = 0L
    private var copyMaxUs = 0L
    private var queueMaxUs = 0L
    private val orientation = CallVideoOrientation()
    private var orientationEnabled = false
    private val orientationListener = object : OrientationEventListener(context) {
        override fun onOrientationChanged(angle: Int) {
            if (!handler.post {
                if (!stopped && orientationEnabled && orientation.observe(angle, SystemClock.elapsedRealtime())) logOrientation()
            }) LogManager.addLog("WARN", "call", "orientation after video worker stopped id=$id")
        }
    }

    private fun logOrientation() {
        LogManager.addLog("INFO", "call", "video device orientation id=$id degrees=${orientation.degrees}; display and preview unchanged")
    }

    private fun observeOrientation(enabled: Boolean) {
        if (orientationEnabled == enabled) return
        orientation.resetPending()
        if (!enabled) { orientationListener.disable(); orientationEnabled = false }
        else if (orientationListener.canDetectOrientation()) { orientationEnabled = true; orientationListener.enable() }
        else LogManager.addLog("WARN", "call", "video orientation sensor unavailable id=$id; using ${orientation.degrees}")
        LogManager.addLog("DEBUG", "call", "video orientation monitoring id=$id enabled=$enabled")
    }

    @Volatile internal var projection: ScreenProjection? = null
    private var screenFrame: android.media.Image? = null
    private var repeatScreen: (() -> Unit)? = null
    private fun rotation(settings: CallVideoSettings): Int {
        if (settings.screen) return if (projection?.portrait == true) 90 else 0
        val camera = checkNotNull(mutable.value.cameras.firstOrNull { it.id == settings.camera }) { "Camera unavailable" }
        return orientation.rotation(camera.sensor, camera.front)
    }

    /** Повторный open того же id обновляет CAPS, сохраняя уже работающий RX и очередь кадров. */
    private fun publishCapabilities(decoders: Map<Int, CallVideoCodec.Companion.Decoder>, stage: String) {
        val bytes = ByteBuffer.allocate(64).order(ByteOrder.BIG_ENDIAN)
        repeat(2) { direction -> for (codec in 1..2) {
            val profile = if (direction == 0) encoders[codec]?.profile else decoders[codec]?.profiles
            val level = if (direction == 0) encoders[codec]?.level else decoders[codec]?.level
            if (profile != null && level != null) {
                val size = if (direction == 0) encoders.getValue(codec).sizes.maxBy { it.width * it.height } else decoders.getValue(codec).size
                val fps = if (direction == 0) encoders.getValue(codec).fps else decoders.getValue(codec).fps
                val pps = if (direction == 0) encoders.getValue(codec).pixelsPerSecond else decoders.getValue(codec).pixelsPerSecond
                bytes.putShort(size.width.toShort()).putShort(size.height.toShort()).putShort(fps.toShort()).putInt(pps)
                    .putInt(4_000_000).put(level.toByte()).put(profile.toByte())
            } else bytes.position(bytes.position() + 16)
        } }
        caps = checkNotNull(CallVideoNative.rotatableCaps(bytes.array())) { "Invalid verified video capabilities" }
        portOwned = true
        check(CallVideoNative.open(id, caps)) { "Video port unavailable" }
        LogManager.addLog("INFO", "call", "video startup call=$id stage=$stage elapsedMs=${(System.nanoTime() - createdAt) / 1_000_000} " +
            "encodeCodecs=${encoders.size} decodeCodecs=${decoders.size}")
    }

    /** Main: владелец сохраняет engine до запуска потоков, чтобы частичный старт всегда получил stop. */
    fun start() {
        check(Looper.myLooper() == Looper.getMainLooper() && !stopped) {
            LogManager.addLog("ERROR", "call", "video start rejected id=$id stopped=$stopped main=${Looper.myLooper() == Looper.getMainLooper()}")
            "Video start requires main and a live engine"
        }
        require(id != 0L) { LogManager.addLog("ERROR", "call", "zero video call id"); "Video call id must be nonzero" }
        if (started) { LogManager.addLog("DEBUG", "call", "video start already requested id=$id"); return }
        started = true
        thread.start(); handler = Handler(thread.looper)
        preview.bind { target ->
            if (!handler.post {
                if (target?.released == true) { LogManager.addLog("WARN", "call", "cancelled preview command id=$id"); return@post }
                if (stopped) { target?.release(); return@post }
                if (target == null && previewTarget == null) return@post
                if (previewTarget?.released != true && target?.sameSource(previewTarget) == true) { target.release(); return@post }
                val previous = previewTarget; previewTarget = target; previous?.release()
                val device = camera; val source = reader
                if (device != null && source != null && !closing) createSession(device, source, cameraToken)
            }) { target?.release(); LogManager.addLog("WARN", "call", "preview command after worker stop id=$id") }
        }
        player.start()
        check(handler.post {
            try {
                LogManager.addLog("INFO", "call", "video startup call=$id stage=prepare elapsedMs=${(System.nanoTime() - createdAt) / 1_000_000}")
                val decoders = CallVideoCodec.probeDecoders { !stopped }
                if (stopped) { LogManager.addLog("INFO", "call", "video decoder preparation cancelled id=$id"); return@post }
                player.decoders(decoders)
                publishCapabilities(decoders, "receive_caps")
                encoders = CallVideoCodec.probe { !stopped }
                if (stopped) { LogManager.addLog("INFO", "call", "video encoder preparation cancelled id=$id"); return@post }
                publishCapabilities(decoders, "duplex_caps")
                val lenses = try { CameraLensManager.enumerate(manager) } catch (e: Exception) {
                    LogManager.addLog("WARN", "call", "camera enumeration failed id=$id: $e")
                    emptyList()
                }
                val cameras = lenses.mapNotNull { lens ->
                    val c = manager.getCameraCharacteristics(lens.cameraId)
                    val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: run {
                        LogManager.addLog("WARN", "call", "video camera has no stream configuration id=${lens.cameraId}")
                        return@mapNotNull null
                    }
                    val rates = c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES).orEmpty()
                    val modes = map.getOutputSizes(ImageFormat.YUV_420_888).orEmpty()
                        .filter { it == Size(640, 480) || it == Size(1280, 720) || it == Size(1920, 1080) }.flatMap { size ->
                            listOf(30, 50, 60).filter { fps -> rates.any { it.upper >= fps } &&
                                map.getOutputMinFrameDuration(ImageFormat.YUV_420_888, size) <= 1_000_000_000L / fps + 1000
                            }.map { CallVideoMode(size.width, size.height, it) }
                        }
                    val supported = modes.filter { mode -> encoders.values.any { encoder -> encoder.modes.any {
                        it.size == Size(mode.width, mode.height) && it.fps == mode.fps
                    } } }
                    LogManager.addLog(if (supported.isEmpty()) "WARN" else "INFO", "call",
                        "video camera id=${lens.cameraId} focal35=${lens.eqFocal} aeRanges=${rates.toList()} yuv=$modes verified=$supported")
                    if (supported.isEmpty()) null else CallVideoCamera(lens.cameraId, "${lens.label} ${lens.mm}".trim(), supported,
                        c.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0,
                        c.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT, lens.eqFocal)
                }
                val available = mutable.value.copy(ready = true, cameras = cameras, codecs = encoders.keys.toList(),
                    codecModes = encoders.mapValues { entry -> entry.value.modes.map { CallVideoMode(it.size.width, it.size.height, it.fps) } })
                val preferences = ChatApplication.instance.configProvider
                val saved = try { preferences.getCallVideoCaptureSettings() }
                catch (e: Exception) { LogManager.addLog("ERROR", "call", "Video settings load failed id=$id: $e"); CallVideoSettings() }
                wanted = saved.restore(available)
                if (saved.camera.isNotEmpty() && saved != wanted)
                    LogManager.addLog("WARN", "call", "Saved video settings adjusted id=$id saved=$saved restored=$wanted")
                mutable.value = available.copy(settings = wanted)
                val savedRange = try { preferences.getCallVideoBitrateRange() }
                catch (e: Exception) { LogManager.addLog("ERROR", "call", "Video bitrate settings load failed id=$id: $e"); CallVideoBitrateRange() }
                if (stopped) { LogManager.addLog("INFO", "call", "Video settings restore cancelled id=$id"); return@post }
                if (CallVideoNative.range(id, savedRange.minimum * 1000, savedRange.maximum * 1000))
                    mutable.value = mutable.value.copy(range = savedRange)
                else LogManager.addLog("WARN", "call", "Saved video bitrate range rejected id=$id range=$savedRange")
                LogManager.addLog("INFO", "call", "Video settings restored id=$id settings=$wanted range=${mutable.value.range}")
                report(if (encoders.isEmpty()) "Receive-only video" else "Video ready")
                tick()
            } catch (e: Exception) {
                if (stopped) LogManager.addLog("INFO", "call", "video setup cancelled id=$id: ${e.message}")
                else fail("Video setup: ${e.message}")
            }
        }) { LogManager.addLog("ERROR", "call", "video setup rejected id=$id; owner must stop partial startup"); "Video setup worker rejected start" }
    }

    fun surface(surface: Surface?, release: () -> Unit = {}) = player.surface(surface, release)
    fun detachSurface(surface: Surface) = player.detach(surface)
    fun presented(surface: Surface, pts: Long) = player.presented(surface, pts)
    fun select(settings: CallVideoSettings) {
        if (!started || stopped) { LogManager.addLog("WARN", "call", "video selection rejected id=$id started=$started stopped=$stopped"); return }
        // Команда ждёт завершения probe в том же worker; пустая камера означает найденный при подготовке режим.
        if (!handler.post {
            if (stopped) { LogManager.addLog("WARN", "call", "video selection after stop id=$id settings=$settings"); return@post }
            val selected = if (!settings.screen && settings.camera.isEmpty()) wanted.copy(enabled = settings.enabled) else settings
            val current = mutable.value
            if (selected.enabled && (!current.ready || current.modes(selected.camera, selected.codec, selected.screen).none {
                it.width == selected.width && it.height == selected.height && it.fps == selected.fps
            })) {
                LogManager.addLog("WARN", "call", "video selection unavailable id=$id ready=${current.ready} " +
                    "requested=$settings resolved=$selected status=${current.status}")
                if (selected.screen && !wanted.screen) projection?.stopAfterFailure()
                mutable.value = current.copy(notice = R.string.call_video_unavailable)
                return@post
            }
            try {
                if (!selected.screen) ChatApplication.instance.configProvider.saveCallVideoCaptureSettings(selected)
                observeOrientation(selected.enabled && !selected.screen)
                wanted = selected; excluded = emptySet()
                mutable.value = current.copy(settings = selected, transmitting = selected.enabled && current.transmitting,
                    activeCodec = if (selected.enabled) current.activeCodec else 0, notice = null)
                LogManager.addLog("INFO", "call", "video selection accepted id=$id requested=$settings resolved=$selected ready=${current.ready}")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "call", "video selection failed id=$id; previous settings retained: $e")
                mutable.value = current.copy(notice = R.string.call_video_unavailable)
                if (!wanted.enabled) try { observeOrientation(false) } catch (cleanup: Exception) {
                    LogManager.addLog("ERROR", "call", "video sensor rollback failed id=$id: $cleanup"); queueCleanup(250)
                }
            }
        }) LogManager.addLog("WARN", "call", "video selection rejected by stopped worker id=$id settings=$settings")
    }
    fun range(value: CallVideoBitrateRange) {
        if (!started || stopped) { LogManager.addLog("WARN", "call", "video range rejected id=$id started=$started stopped=$stopped"); return }
        if (!handler.post {
            if (stopped) { LogManager.addLog("WARN", "call", "Bitrate selection after stop id=$id range=$value"); return@post }
            try {
                if (CallVideoNative.range(id, value.minimum * 1000, value.maximum * 1000)) {
                    mutable.value = mutable.value.copy(range = value)
                    try { ChatApplication.instance.configProvider.saveCallVideoBitrateRange(value) } catch (e: Exception) {
                        LogManager.addLog("ERROR", "call", "video bitrate applied but not saved id=$id range=$value: $e")
                        mutable.value = mutable.value.copy(notice = R.string.call_video_unavailable)
                    }
                    try { NativeLib.setChatSetting("call_video_bitrate_range_kbps", value.encode()) } catch (e: Exception) {
                        LogManager.addLog("ERROR", "call", "video bitrate default update failed id=$id range=$value: $e")
                    }
                    LogManager.addLog("INFO", "call", "Video bitrate selected id=$id range=$value")
                } else LogManager.addLog("WARN", "call", "Bitrate range rejected id=$id range=$value")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "call", "video bitrate selection failed id=$id range=$value: $e")
                mutable.value = mutable.value.copy(notice = R.string.call_video_unavailable)
            }
        }) LogManager.addLog("WARN", "call", "Bitrate selection rejected by stopped worker id=$id range=$value")
    }
    private fun report(message: String) {
        mutable.value = mutable.value.copy(status = message)
        LogManager.addLog("INFO", "call", "video call=$id $message")
    }
    private fun fail(message: String) {
        LogManager.addLog("ERROR", "call", "video call=$id $message")
        if (wanted.screen) projection?.stopAfterFailure()
        wanted = wanted.copy(enabled = false)
        mutable.value = mutable.value.copy(settings = wanted, status = message, transmitting = false, activeCodec = 0,
            notice = R.string.call_video_unavailable)
        closeCamera(); encoder?.let { releaseEncoder(it) }; pendingFrames.clear(); active = null
        if (confirmingRequest != 0L) try { CallVideoNative.applied(id, format?.generation ?: 0, confirmingRequest, 0) }
        catch (e: Exception) { LogManager.addLog("ERROR", "call", "video rate cancellation failed id=$id request=$confirmingRequest: $e") }
        confirmingRequest = 0
        switching = false; encoderDrained = false
        queueCleanup(0)
    }

    private fun tick() {
        if (stopped || nativeUnavailable) return
        try {
            val s = CallVideoNative.status(id) ?: run {
                nativeUnavailable = true; mutable.value = mutable.value.copy(ready = false)
                fail("Video native port unavailable"); return
            }
            if (s[5] != 0L && active != null) { fail("Core rejected video format generation=${format?.generation}"); return }
            if (orientation.confirm(SystemClock.elapsedRealtime())) logOrientation()
            canEncode = s[10] != 0L && s[4] == format?.generation
            if (s[6] >= 300000 && s[7] >= s[6]) mutable.value = mutable.value.copy(range = CallVideoBitrateRange((s[6] / 1000).toInt(), (s[7] / 1000).toInt()))
            if (keySerial != s[0]) { keySerial = s[0]; needKey = true }
            val current = format
            if (current != null && s[8] == current.generation && s[9] == 0L && rejectedGeneration != s[8]) {
                rejectedGeneration = s[8]; excluded = excluded + current.codec
                check(wanted.codec == 0) { "Receiver cannot decode selected codec" }
                report("Receiver rejected video; selecting fallback")
            }
            if (!encoderDrained && s[1] == format?.generation && s[2] > request) {
                request = s[2]; pendingRequest = request; pendingRate = s[3].toInt()
                LogManager.addLog("DEBUG", "call", "video bitrate queued id=$id request=$request target=$pendingRate")
            }
            if (!wanted.enabled) {
                if (active != null) {
                    val pending = if (confirmingRequest != 0L) confirmingRequest else pendingRequest
                    if (pending != 0L) CallVideoNative.applied(id, format?.generation ?: 0, pending, 0)
                    confirmingRequest = 0; pendingRequest = 0
                    closeCamera(); encoder?.let { releaseEncoder(it) }; pendingFrames.clear(); active = null
                    switching = false; encoderDrained = false; report("Video off")
                }
            } else {
                val turn = rotation(wanted)
                val width = if (turn % 180 == 0) wanted.width else wanted.height
                val height = if (turn % 180 == 0) wanted.height else wanted.width
                switching = switching || wanted != active || current?.width != width || current.height != height || current.codec in excluded
                if (switching && !opening && !closing) CallVideoNative.peer(id)?.let { change(it) }
            }
            if (wanted.enabled && !switching && System.nanoTime() - lastCaptureAt > 200_000_000) repeatScreen?.invoke()
            if (wanted.enabled && encoder !in pendingEncoders) drain()
            if (active != null && !opening && !closing && lastCaptureAt != 0L
                && System.nanoTime() - lastCaptureAt > 8_000_000_000) fail("Camera stopped producing frames")
        } catch (e: Exception) { fail("Video worker: ${e.message}") }
        finally {
            if (!stopped && !nativeUnavailable && !handler.postDelayed(::tick, 10)) fail("Video worker rejected next tick")
        }
    }

    /** Проба кандидата не меняет камеру, порт или видимое состояние. */
    private fun prepareEncoder(settings: CallVideoSettings, peer: ByteArray, bitrate: Int): CallVideoCodec? {
        val turn = rotation(settings)
        val width = if (turn % 180 == 0) settings.width else settings.height
        val height = if (turn % 180 == 0) settings.height else settings.width
        for (codec in if (settings.codec == 0) listOf(2, 1) else listOf(settings.codec)) {
            if (codec in excluded) continue
            val implementation = encoders[codec]?.modes?.firstOrNull { it.size == Size(settings.width, settings.height) && it.fps == settings.fps } ?: continue
            val desired = CallVideoFormat(++generation, width, height, fps = settings.fps, codec = codec, profile = implementation.profile)
            val selected = CallVideoNative.choose(caps, peer, desired.array()) ?: continue
            var candidate: CallVideoCodec? = null
            try {
                val verified = CallVideoFormat.from(selected).let { it.copy(level = maxOf(it.level, implementation.level)) }
                check(CallVideoNative.supported(caps, peer, verified.array())) { "Verified encoder level exceeds peer capabilities" }
                candidate = CallVideoCodec(verified, bitrate, implementation.name)
                candidate.prepare { !stopped }
                check(CallVideoNative.supported(caps, peer, candidate.format.array())) { "Actual encoder format exceeds peer capabilities" }
                return candidate
            } catch (e: Exception) {
                candidate?.let { releaseEncoder(it) }
                LogManager.addLog("WARN", "call", "video mode rejected id=$id codec=$codec source=${settings.width}x${settings.height} " +
                    "output=${width}x$height rotation=$turn name=${implementation.name}: ${e.message}")
                check(pendingEncoders.isEmpty()) { "Rejected encoder cleanup pending" }
            }
        }
        return null
    }

    /** Сначала выводим старые входы и освобождаем mailbox; затем публикуем новое поколение.
     * При одном повороте CameraCaptureSession и прямое превью продолжают работать. */
    private fun change(peer: ByteArray) {
        if (!encoderDrained) {
            LogManager.addLog("INFO", "call", "video switch begin id=$id from=$format wanted=$wanted angle=${orientation.degrees}")
            drain(finish = true); encoderDrained = true
        }
        drain()
        if (pendingFrames.isNotEmpty() || CallVideoNative.status(id)?.get(11) != 1L) return
        val previousSettings = active
        val range = mutable.value.range
        val bitrate = (encoder?.bitrate ?: 1_000_000).coerceIn(range.minimum * 1000, range.maximum * 1000)
        encoder?.let { check(releaseEncoder(it)) { "Previous encoder cleanup pending" } }
        val targets = listOfNotNull(wanted, previousSettings?.takeIf { it.screen == wanted.screen }).distinct()
        for (settings in targets) {
            val candidate = prepareEncoder(settings, peer, bitrate) ?: continue
            if (stopped) { releaseEncoder(candidate); return }
            try {
                check(CallVideoNative.format(id, candidate.format.array(), candidate.bitrate)) { "Video format rejected" }
            } catch (e: Exception) { releaseEncoder(candidate); throw e }
            val sourceChanged = previousSettings == null || previousSettings.screen != settings.screen
                || (settings.screen && (reader?.width != candidate.format.width || reader?.height != candidate.format.height))
                || settings.camera != previousSettings.camera || settings.width != previousSettings.width || settings.height != previousSettings.height || settings.fps != previousSettings.fps
            if (sourceChanged) closeCamera()
            encoder = candidate; format = candidate.format; active = settings; wanted = settings
            pendingRequest = 0; confirmingRequest = 0; request = 0; needKey = true
            switching = false; encoderDrained = false; appliedRotation = -1; canEncode = false
            val restored = settings != targets.first()
            mutable.value = mutable.value.copy(bitrate = candidate.bitrate, settings = settings,
                notice = if (restored) R.string.call_video_mode_restored else null)
            LogManager.addLog("INFO", "call", "video switch committed id=$id format=$format rotation=${rotation(settings)} " +
                "cameraRestart=$sourceChanged preview=${preview.state.value.format}")
            if (restored) LogManager.addLog("WARN", "call", "video mode unavailable id=$id requested=${targets.first()} restored=$settings")
            report(if (restored) "Selected mode unavailable; previous settings restored" else
                "${if (candidate.format.codec == 2) "H.265" else "H.264"} · ${candidate.format.width}×${candidate.format.height} · ${candidate.format.fps} fps")
            if (reader == null && !closing && !opening) openCamera()
            return
        }
        fail("No compatible encoder for selected size and orientation")
    }
    @SuppressLint("MissingPermission")
    private fun openCamera() {
        if (stopped || !wanted.enabled || opening || closing) return
        val settings = active ?: return; val token = ++cameraToken
        if (!settings.screen) {
            val c = manager.getCameraCharacteristics(settings.camera)
            realtime = c.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
        }
        sensorOffset = null
        val cadence = CallVideoCadence(settings.fps)
        val output = checkNotNull(format)
        val nextReader = if (settings.screen) ImageReader.newInstance(output.width, output.height, android.graphics.PixelFormat.RGBA_8888, 3)
            else ImageReader.newInstance(settings.width, settings.height, ImageFormat.YUV_420_888, 3)
        ownedReaders.add(nextReader); reader = nextReader
        fun encode(image: android.media.Image) {
            try {
                if (stopped || token != cameraToken || !wanted.enabled) return
                val now = System.nanoTime()
                lastCaptureAt = now
                val offset = if (settings.screen) 0L else if (realtime) now - SystemClock.elapsedRealtimeNanos() else sensorOffset ?: (now - image.timestamp).also {
                    sensorOffset = it; LogManager.addLog("WARN", "call", "camera timestamp calibrated from arrival id=$id offset=$it")
                }
                val pts = if (settings.screen) now / 1000 else (image.timestamp + offset) / 1000
                // Отбираем raw до копирования/encode; частота передачи не зависит от долга приёмника.
                if (pts <= lastPts || !cadence.accept(pts)) return
                drain()
                // До восьми принятых входов/готовых выходов: backpressure действует до кодирования.
                if (switching || !canEncode || pendingFrames.isNotEmpty() || (encoder?.pendingFrames ?: 0) >= 8)
                    return
                val f = format ?: return
                val turn = rotation(settings)
                if (f.width != (if (turn % 180 == 0) settings.width else settings.height)
                    || f.height != (if (turn % 180 == 0) settings.height else settings.width)) {
                    switching = true
                    return
                }
                if (pendingRequest != 0L) {
                    val requestId = pendingRequest; pendingRequest = 0
                    val old = checkNotNull(encoder); val oldRate = old.bitrate; val name = old.name
                    confirmingPts = pts
                    if (old.setBitrate(pendingRate)) {
                        confirmingRequest = requestId; confirmingKey = false
                    } else {
                        drain(finish = true)
                        check(releaseEncoder(old)) { "Bitrate encoder cleanup pending" }
                        try {
                            encoder = CallVideoCodec(f, pendingRate, name)
                            encoder!!.prepare(); check(encoder!!.format == f) { "Bitrate change altered encoder format" }
                            confirmingRequest = requestId; confirmingKey = true; needKey = true
                            report("Applying ${pendingRate / 1000} kbit/s on IDR")
                        } catch (e: Exception) {
                            LogManager.addLog("WARN", "call", "video bitrate replacement failed: ${e.message}; restoring $oldRate")
                            encoder?.let { check(releaseEncoder(it)) { "Replacement encoder cleanup pending" } }
                            encoder = CallVideoCodec(f, oldRate, name)
                            encoder!!.prepare(); check(encoder!!.format == f)
                            confirmingRequest = requestId; confirmingKey = true; needKey = true
                        }
                    }
                }
                val transformStarted = System.nanoTime()
                if (encoder?.submit(image, pts, needKey, if (settings.screen) 0 else turn) == true) {
                    encoder?.let {
                        inputWaitMaxUs = maxOf(inputWaitMaxUs, it.inputWaitUs)
                        copyMaxUs = maxOf(copyMaxUs, it.copyUs); queueMaxUs = maxOf(queueMaxUs, it.queueUs)
                    }
                    lastPts = pts
                    if (needKey) {
                        LogManager.addLog("DEBUG", "call", "video KEY submitted id=$id generation=${f.generation} serial=$keySerial pts=$pts")
                        needKey = false
                    }
                    if (appliedRotation != turn) {
                        LogManager.addLog("INFO", "call", "video pixels rotated id=$id angle=${orientation.degrees} rotation=$turn " +
                            "source=${settings.width}x${settings.height} output=${f.width}x${f.height} generation=${f.generation} pts=$pts")
                        appliedRotation = turn
                    }
                }
                transformMaxUs = maxOf(transformMaxUs, (System.nanoTime() - transformStarted) / 1000)
                drain()
            } catch (e: Exception) { fail("Video encode: ${e.message}") }
        }
        nextReader.setOnImageAvailableListener({ source ->
            val image = try { source.acquireLatestImage() } catch (e: Exception) {
                if (stopped || token != cameraToken) LogManager.addLog("WARN", "call", "closed camera reader token=$token/$cameraToken: $e")
                else fail("Camera image acquisition: $e")
                null
            } ?: return@setOnImageAvailableListener
            ownedImages.add(image)
            if (settings.screen && token == cameraToken && !stopped) {
                screenFrame?.close()
                screenFrame = image
                ownedImages.remove(image)
                encode(image)
                return@setOnImageAvailableListener
            }
            try { encode(image) }
            finally {
                try { image.close(); ownedImages.remove(image) } catch (e: Exception) {
                    LogManager.addLog("ERROR", "call", "camera Image close failed id=$id token=$token; retained: $e")
                    if (!stopped && token == cameraToken) fail("Camera image release: $e")
                    queueCleanup(0)
                }
            }
        }, handler)
        if (settings.screen) {
            repeatScreen = { screenFrame?.let { encode(it) } }
            lastCaptureAt = System.nanoTime()
            checkNotNull(projection).attach(nextReader.surface, output.width, output.height)
            return
        }
        opening = true; openingToken = token; openedAt = System.nanoTime(); lastCaptureAt = openedAt
        try { manager.openCamera(settings.camera, object : CameraDevice.StateCallback() {
            override fun onOpened(device: CameraDevice) {
                if (device in closedCameras) { LogManager.addLog("WARN", "call", "closed camera opened callback ignored id=$id token=$token"); return }
                if (openingToken == token) opening = false
                if (stopped || token != cameraToken) {
                    closingCameras.add(device); closing = true; queueCleanup(0); return
                }
                camera = device
                LogManager.addLog("INFO", "call", "camera opened id=$id device=${device.id} token=$token")
                createSession(device, nextReader, token)
            }
            override fun onDisconnected(device: CameraDevice) { cameraFailure(device, token, "Camera disconnected") }
            override fun onError(device: CameraDevice, error: Int) { cameraFailure(device, token, "Camera error $error") }
            override fun onClosed(device: CameraDevice) {
                if (!closedCameras.add(device)) { LogManager.addLog("DEBUG", "call", "duplicate camera closed ignored id=$id token=$token"); return }
                sessions.values.filter { it.first === device }.map { it.second }.forEach { it() }
                ownedSessions.entries.removeAll { it.value === device }
                closingSessions.removeAll { it !in ownedSessions }
                if (camera === device) camera = null
                closingCameras.remove(device)
                LogManager.addLog("INFO", "call", "camera closed id=$id device=${device.id} token=$token current=$cameraToken")
                queueCleanup(0)
            }
        }, handler) } catch (e: Exception) {
            val awaitsCallback = e is CameraAccessException && e.reason in listOf(CameraAccessException.CAMERA_IN_USE,
                CameraAccessException.CAMERA_DISABLED, CameraAccessException.CAMERA_DISCONNECTED)
            if (openingToken == token && !awaitsCallback) opening = false
            LogManager.addLog("ERROR", "call", "camera open failed id=$id token=$token awaitingCallback=$awaitsCallback: $e")
            queueCleanup(0)
            throw e
        }
    }

    private fun cameraFailure(device: CameraDevice, token: Long, detail: String) {
        if (device in closedCameras) { LogManager.addLog("WARN", "call", "closed camera error ignored id=$id token=$token detail=$detail"); return }
        if (openingToken == token) opening = false
        closingCameras.add(device)
        if (!stopped && token == cameraToken) { camera = device; fail(detail) }
        else { LogManager.addLog("WARN", "call", "stale camera callback id=$id token=$token/$cameraToken detail=$detail"); queueCleanup(0) }
    }

    /** Прямое превью использует второй output той же камеры; encoder и его PTS не меняются. */
    private fun createSession(device: CameraDevice, source: ImageReader, token: Long, withPreview: Boolean = true) {
        val settings = active ?: return
        val serial = ++sessionSerial
        val output = if (withPreview) previewTarget?.retain() else null
        var completed = false
        fun complete() {
            if (completed) return
            completed = true; sessions.remove(serial); output?.release()
            LogManager.addLog("DEBUG", "call", "camera session released id=$id session=$serial remaining=${sessions.size}")
        }
        fun valid() = !stopped && token == cameraToken && serial == sessionSerial
        fun rejected(message: String) {
            if (!valid()) { LogManager.addLog("DEBUG", "call", "stale camera session id=$id session=$serial: $message"); return }
            if (output == null) fail(message) else {
                LogManager.addLog("ERROR", "call", "direct preview failed id=$id: $message; retaining transmission")
                preview.update(preview.state.value.format, failed = true)
                createSession(device, source, token, withPreview = false)
            }
        }
        sessions[serial] = device to ::complete
        var submitted = false
        try {
            val c = manager.getCameraCharacteristics(device.id)
            var previewFormat: StreamPreviewFormat? = null
            if (output != null) {
                val sizes = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)?.getOutputSizes(SurfaceTexture::class.java).orEmpty()
                val size = sizes.filter { it.width <= settings.width && it.height <= settings.height && it.width * settings.height == it.height * settings.width }
                    .maxByOrNull { it.width * it.height } ?: error("No compatible preview size")
                output.texture.setDefaultBufferSize(size.width, size.height)
                previewFormat = StreamPreviewFormat(size.width, size.height,
                    StreamPreviewCamera(c.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0,
                        c.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT), realtime)
                preview.update(previewFormat)
            } else if (withPreview) preview.update(null)
            val targets = listOfNotNull(source.surface, output?.surface)
            submitted = true
            device.createCaptureSession(targets, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (completed) { try { s.close() } catch (e: Exception) { LogManager.addLog("WARN", "call", "closed camera session callback id=$id: $e") }; return }
                    ownedSessions[s] = device
                    if (!valid()) { closingSessions.add(s); queueCleanup(0); return }
                    session = s
                    try {
                        val range = c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES).orEmpty()
                            .filter { it.upper >= settings.fps }.minWithOrNull(compareBy<android.util.Range<Int>> { it.upper }.thenByDescending { it.lower })
                            ?: error("Camera lacks ${settings.fps} fps")
                        val request = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            targets.forEach(::addTarget); set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, range)
                        }.build()
                        s.setRepeatingRequest(request, null, handler)
                        if (output != null) preview.update(previewFormat, active = true)
                        LogManager.addLog("INFO", "call", "camera capture id=$id session=$serial range=$range outputFps=${settings.fps} preview=$previewFormat")
                    } catch (e: Exception) { closingSessions.add(s); queueCleanup(0); rejected("Camera request: ${e.message}") }
                }
                override fun onConfigureFailed(s: CameraCaptureSession) {
                    // SDK уже закрыл сессию; onClosed после этого callback не гарантируется.
                    ownedSessions.remove(s); closingSessions.remove(s); complete()
                    rejected("Camera session configuration failed")
                }
                override fun onClosed(s: CameraCaptureSession) {
                    ownedSessions.remove(s); closingSessions.remove(s)
                    if (session === s) session = null
                    complete(); queueCleanup(0)
                }
            }, handler)
        } catch (e: Exception) {
            if (!submitted) { complete(); rejected("Camera session: ${e.message}") }
            else {
                // SDK может одновременно бросить CameraAccessException и назначить callback сессии.
                // До camera.onClosed сохраняем все выходы даже при отсутствии callback сессии.
                LogManager.addLog("ERROR", "call", "camera session submission failed id=$id session=$serial; awaiting device closure: $e")
                fail("Camera session: ${e.message}")
            }
        }
    }
    private fun drain(finish: Boolean = false) {
        val codec = encoder ?: return; val f = format ?: return
        for (p in if (finish) codec.finish() else codec.drain()) {
            encodedBytes += p.bytes.size; encodedFrames++
            encodedMaxBytes = maxOf(encodedMaxBytes, p.bytes.size)
            if (p.key) encodedKeys++
            if (lastEncodedPts >= 0) encodedMaxGapUs = maxOf(encodedMaxGapUs, p.pts - lastEncodedPts)
            lastEncodedPts = p.pts
            if (confirmingRequest != 0L && (!confirmingKey || p.key) && p.pts >= confirmingPts) {
                CallVideoNative.applied(id, f.generation, confirmingRequest, codec.bitrate); confirmingRequest = 0
                mutable.value = mutable.value.copy(bitrate = codec.bitrate)
                LogManager.addLog("INFO", "call", "video bitrate applied id=$id generation=${f.generation} bitrate=${codec.bitrate} pts=${p.pts}")
            }
            val metadata = f.array() + longArrayOf(++frame, p.pts, if (p.key) 1 else 0)
            pendingFrames.addLast(Pending(metadata, p.bytes))
        }
        while (pendingFrames.isNotEmpty()) {
            val p = pendingFrames.first
            val sent = CallVideoNative.send(id, p.metadata, p.bytes)
            if (sent == 0) break
            check(sent == 1) { "Video TX rejected encoded frame ${p.metadata[9]}" }
            pendingFrames.removeFirst()
            if (wanted.enabled && (!mutable.value.transmitting || mutable.value.activeCodec != f.codec || mutable.value.sharingScreen != (active?.screen == true))) {
                mutable.value = mutable.value.copy(transmitting = true, sharingScreen = active?.screen == true, activeCodec = f.codec)
                LogManager.addLog("INFO", "call", "video transmission started id=$id generation=${f.generation} screen=${active?.screen} codec=${f.codec} frame=${p.metadata[9]}")
            }
        }
        val now = System.nanoTime()
        if (now - reportAt > 1_000_000_000) {
            if (reportAt != 0L) LogManager.addLog("DEBUG", "call", "video output id=$id fps=$encodedFrames " +
                "bps=${encodedBytes * 8_000_000_000 / (now - reportAt)} target=${codec.bitrate} needKey=$needKey " +
                "keys=$encodedKeys maxFrameBytes=$encodedMaxBytes maxCaptureGapUs=$encodedMaxGapUs " +
                "pending=${pendingFrames.size} codecPending=${codec.pendingFrames} canEncode=$canEncode switching=$switching " +
                "submitMaxUs=$transformMaxUs inputWaitMaxUs=$inputWaitMaxUs copyMaxUs=$copyMaxUs queueMaxUs=$queueMaxUs")
            reportAt = now; encodedFrames = 0; encodedBytes = 0; transformMaxUs = 0
            encodedKeys = 0; encodedMaxBytes = 0; encodedMaxGapUs = 0
            inputWaitMaxUs = 0; copyMaxUs = 0; queueMaxUs = 0
        }
    }
    private fun clearScreenFrame() {
        repeatScreen = null
        projection?.detach()
        screenFrame?.close()
        screenFrame = null
    }

    private fun closeCamera() {
        cameraToken++; sessionSerial++
        clearScreenFrame()
        preview.update(null)
        closing = true
        closingSessions.addAll(ownedSessions.keys)
        camera?.let { closingCameras.add(it) }
        queueCleanup(0)
    }

    private fun releaseEncoder(owned: CallVideoCodec): Boolean = try {
        owned.close(); pendingEncoders.remove(owned)
        if (encoder === owned) encoder = null
        true
    } catch (e: Exception) {
        pendingEncoders.add(owned)
        LogManager.addLog("ERROR", "call", "video encoder release failed id=$id; retained for retry: $e")
        queueCleanup(cleanupDelay); false
    }

    /** Вызывается на TX: закрыть независимые владельцы, не теряя refs до SDK ACK/callback. */
    private fun cleanupResources() {
        if (stopped) {
            clearScreenFrame()
            wanted = wanted.copy(enabled = false)
            mutable.value = mutable.value.copy(settings = wanted, transmitting = false, activeCodec = 0)
            closing = true
            closingSessions.addAll(ownedSessions.keys); camera?.let { closingCameras.add(it) }
            pendingFrames.clear(); preview.close(); previewTarget?.release(); previewTarget = null
        }
        if (!wanted.enabled) try { observeOrientation(false) } catch (e: Exception) {
            LogManager.addLog("ERROR", "call", "video orientation disable failed id=$id; retained: $e")
        }
        for (owned in closingSessions.toList()) try { owned.close() } catch (e: Exception) {
            LogManager.addLog("ERROR", "call", "camera session close failed id=$id; retained: $e")
        }
        for (owned in closingCameras.toList()) try { owned.close() } catch (e: Exception) {
            LogManager.addLog("ERROR", "call", "camera device close failed id=$id device=${owned.id}; retained: $e")
        }
        for (owned in ownedImages.toList()) try { owned.close(); ownedImages.remove(owned) } catch (e: Exception) {
            LogManager.addLog("ERROR", "call", "camera Image cleanup failed id=$id; retained: $e")
        }
        if (!opening && camera == null && closingCameras.isEmpty() && (closing || active?.screen != true)) {
            for (owned in ownedReaders.toList()) try {
                owned.close(); ownedReaders.remove(owned)
                if (reader === owned) reader = null
            } catch (e: Exception) { LogManager.addLog("ERROR", "call", "camera reader close failed id=$id; retained: $e") }
            if (ownedReaders.isEmpty() && sessions.isEmpty() && ownedSessions.isEmpty()) closing = false
        }
        if (camera != null && camera !in closingCameras && !opening) closing = false
        val release = pendingEncoders.toMutableSet()
        if (!wanted.enabled) encoder?.let { release.add(it) }
        for (owned in release) releaseEncoder(owned)
        val pending = opening || closing || closingCameras.isNotEmpty() || closingSessions.isNotEmpty() || ownedImages.isNotEmpty() ||
            pendingEncoders.isNotEmpty() || (stopped && !rxReleased) ||
            (!wanted.enabled && (encoder != null || orientationEnabled))
        if (stopped && !pending) {
            if (portOwned) try { CallVideoNative.close(id); portOwned = false } catch (e: Exception) {
                LogManager.addLog("ERROR", "call", "video native port close failed id=$id; retained: $e")
            }
            if (!portOwned) {
                cleanupComplete = true; retained.remove(this)
                synchronized(this) { handler.removeCallbacks(cleanupTask); cleanupQueued = false }
                thread.quitSafely()
                LogManager.addLog("INFO", "call", "video cleanup confirmed id=$id; camera/encoder/RX/native port released")
                return
            }
        }
        if (pending || (stopped && portOwned)) {
            LogManager.addLog("DEBUG", "call", "video cleanup pending id=$id opening=$opening closing=$closing " +
                "devices=${closingCameras.size} sessions=${ownedSessions.size}/${sessions.size} readers=${ownedReaders.size} " +
                "images=${ownedImages.size} encoders=${pendingEncoders.size} sensor=$orientationEnabled rxReleased=$rxReleased port=$portOwned")
            queueCleanup(cleanupDelay); cleanupDelay = (cleanupDelay * 2).coerceAtMost(30_000)
        } else {
            cleanupDelay = 250
            if (reader == null && camera == null && active != null && wanted.enabled) try { openCamera() }
            catch (e: Exception) { fail("Camera reopen: $e") }
        }
    }

    /** Один ближайший запрос; сильный владелец при stop остаётся также при отказе Handler. */
    @Synchronized private fun queueCleanup(delay: Long): Boolean {
        if (cleanupComplete) return true
        val due = System.nanoTime() + delay * 1_000_000
        if (cleanupQueued && cleanupDue <= due) return true
        handler.removeCallbacks(cleanupTask)
        cleanupDue = due
        cleanupQueued = if (delay == 0L) handler.post(cleanupTask) else handler.postDelayed(cleanupTask, delay)
        if (!cleanupQueued) LogManager.addLog("ERROR", "call", "video cleanup request rejected id=$id delayMs=$delay; owner retained")
        return cleanupQueued
    }

    /** Main: запретить producers и подтвердить RX cleanup, затем запросить TX teardown.
     * true — SDK callbacks/ресурсы и native port освобождены; false — очистка выполняется, caller сохраняет engine.
     * Ошибка RX/post бросается caller; native port закрывается TX лишь после прекращения всех его обращений. */
    fun stop(): Boolean {
        check(Looper.myLooper() == Looper.getMainLooper()) {
            LogManager.addLog("ERROR", "call", "video stop outside main id=$id")
            "Video stop requires main"
        }
        if (cleanupComplete) return true
        retained.add(this); stopped = true
        var rxFailure: Exception? = null
        try { player.stop(); rxReleased = true } catch (e: Exception) {
            rxFailure = e
            LogManager.addLog("ERROR", "call", "video RX stop pending id=$id; continuing independent TX cleanup: $e")
        }
        if (!::handler.isInitialized) {
            rxFailure?.let { throw it }
            preview.close(); thread.quitSafely(); cleanupComplete = true; retained.remove(this)
            LogManager.addLog("INFO", "call", "video stop before TX setup id=$id")
            return true
        }
        check(queueCleanup(0)) {
            LogManager.addLog("ERROR", "call", "video stop cleanup rejected id=$id; stop remains retryable")
            "Video cleanup worker rejected stop"
        }
        rxFailure?.let { throw it }
        return cleanupComplete
    }
}
