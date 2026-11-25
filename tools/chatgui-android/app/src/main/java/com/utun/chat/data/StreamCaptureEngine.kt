package com.utun.chat.data

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.util.Range
import android.view.OrientationEventListener
import android.view.Surface
import java.io.ByteArrayOutputStream
import java.util.concurrent.ConcurrentHashMap

/** Пассивный владелец: сервис сохраняет его до start на main. Camera/codec принадлежат HandlerThread;
 * микрофон и origin сохраняются при смене видео. Поколения отсеивают старые callbacks.
 * stop=true подтверждает все SDK/native DSP ресурсы; false сохраняет engine для повторного stop.
 */
class StreamCaptureEngine(
    private val context: Context, private val id: Long, private val group: String,
    private val initialConfig: StreamCaptureConfig, private val microphone: Boolean, private val preview: StreamPreview,
    initialAudioCompressor: RadioCompressorSettings,
    private val ready: (StreamCaptureConfig, Boolean) -> Unit, private val failure: (String) -> Unit
) {
    companion object { private val retained = ConcurrentHashMap.newKeySet<StreamCaptureEngine>() }
    private val thread = HandlerThread("stream-camera")
    private lateinit var handler: Handler
    private var started = false
    private val manager = context.getSystemService(CameraManager::class.java)
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoderResources: VideoCodecResources? = null
    private val encoder: MediaCodec? get() = encoderResources?.codec
    @Volatile internal var projection: ScreenProjection? = null
    private var input: Surface? = null
    private var activeConfig: StreamCaptureConfig? = null
    private var previewTarget: StreamPreviewTarget? = null
    private var sessionGeneration = 0L
    private val cameraSessions = mutableMapOf<Long, Pair<CameraDevice, (String) -> Unit>>()
    private var audioCapture: MicrophoneCapture.Subscription? = null
    private var audioThread: Thread? = null
    @Volatile private var audioRunning = false
    @Volatile private var audioMuted = false
    @Volatile var audioCompressorSettings = initialAudioCompressor
    @Volatile private var stopped = false
    @Volatile private var stopping = false
    private var terminalFailure = false
    private val closingCameras = linkedSetOf<CameraDevice>()
    private val closedCameras = linkedSetOf<CameraDevice>()
    private val ownedSessions = linkedMapOf<CameraCaptureSession, CameraDevice>()
    private val closingSessions = linkedSetOf<CameraCaptureSession>()
    private var cleanupQueued = false
    private var cleanupDue = 0L
    private var cleanupDelay = 250L
    private val cleanupTask = Runnable { synchronized(this) { cleanupQueued = false }; cleanupResources() }
    private var opening = false
    private var openingGeneration = 0L
    private var releaseDone: (() -> Unit)? = null
    private var videoReleasing = false
    private var videoReleaseSince = 0L
    private var slowReleaseReported = false
    private var stopVideoRequested = false
    private var generation = 0L
    private var videoRevision = 0L
    private var videoRotation = 0
    private val orientation = CallVideoOrientation()
    private var orientationEnabled = false
    private val orientationListener = object : OrientationEventListener(context) {
        override fun onOrientationChanged(angle: Int) {
            if (!handler.post {
                if (orientationEnabled && orientation.observe(angle, SystemClock.elapsedRealtime())) logOrientation()
            }) LogManager.addLog("WARN", "video", "stream orientation after capture worker stopped id=$id")
        }
    }
    private var lastGood: StreamCaptureConfig? = null
    private var reverting = false
    private var waitingFirst = true
    private var confirming = false
    private var confirmStartNs = 0L
    private var confirmFrames = 0L
    private var switchStartedNs = 0L
    private var originNs = 0L
    private var csd = byteArrayOf()
    private val partial = ByteArrayOutputStream()
    private val partialConfig = ByteArrayOutputStream()
    private var partialPts = 0L
    private var partialKey = false
    private var requireKey = true
    private var lastKeyRequest = 0L
    private var frames = 0L
    private var dropped = 0L
    private var encodedFrames = 0L
    private var bytesSent = 0L
    private var reportNs = 0L
    private var reportFrames = 0L
    private var reportBytes = 0L
    private var lastPts = -1L
    private var appliedBitrate = 0

    private fun logOrientation() {
        LogManager.addLog("INFO", "video", "stream device orientation id=$id degrees=${orientation.degrees} mode=${activeConfig?.orientation}")
    }

    private fun observeOrientation(enabled: Boolean) {
        if (orientationEnabled == enabled) return
        orientation.resetPending()
        if (!enabled) { orientationListener.disable(); orientationEnabled = false }
        else if (orientationListener.canDetectOrientation()) { orientationEnabled = true; orientationListener.enable() }
        else LogManager.addLog("WARN", "video", "stream orientation sensor unavailable id=$id; using ${orientation.degrees}")
        LogManager.videoTrace("stream orientation monitoring id=$id enabled=$enabled")
    }

    fun start() {
        check(Looper.myLooper() == Looper.getMainLooper() && !stopping && !stopped) { "Capture start requires main and a live owner" }
        if (started) { LogManager.addLog("WARN", "stream", "duplicate capture start id=$id"); return }
        started = true; thread.start(); handler = Handler(thread.looper)
        preview.bind { target ->
            if (!handler.post {
                if (target?.released == true) { LogManager.addLog("WARN", "video", "cancelled preview command id=$id"); return@post }
                if (stopping || stopped || terminalFailure) { target?.release(); return@post }
                if (target == null && previewTarget == null) return@post
                if (previewTarget?.released != true && target?.sameSource(previewTarget) == true) { target.release(); return@post }
                val previous = previewTarget; previewTarget = target; previous?.release()
                val config = activeConfig; val device = camera
                LogManager.videoTrace("preview output change id=$id target=${target?.let(System::identityHashCode)} " +
                    "generation=$generation camera=${device != null} opening=$opening")
                if (device != null && config != null && current(generation)) createSession(device, config, generation)
            }) {
                LogManager.addLog("WARN", "video", "preview command rejected id=$id capture handler stopped")
                target?.release()
            }
        }
        check(handler.post {
            if (stopping || stopped) return@post
            try {
                val mode = initialConfig.mode
                val pose = if (mode.screen) StreamPreviewCamera(0, false) else cameraPose(mode.camera)
                val rotation = if (mode.screen) 0 else initialConfig.orientation.sensorRotation(pose.sensor, pose.front, orientation.degrees)
                check(NativeLib.streamOpen(id, group, width = mode.width, height = mode.height,
                    rotation = rotation, audio = microphone, videoCodec = mode.videoCodec.id)) { "Publisher open rejected" }
                originNs = System.nanoTime()
                if (microphone) startAudio()
                configure(initialConfig)
            } catch (e: Exception) { failed("setup: ${e.message}") }
        }) { LogManager.addLog("ERROR", "stream", "capture startup post rejected id=$id; partial owner retained"); "Capture start rejected" }
    }

    fun change(config: StreamCaptureConfig) {
        check(::handler.isInitialized) { "Capture change before start" }
        check(handler.post {
            if (stopping || stopped || terminalFailure || waitingFirst || confirming || releaseDone != null) {
                LogManager.addLog("WARN", "video", "capture change rejected id=$id busy=$waitingFirst stopping=$stopping"); return@post
            }
            if (config == lastGood) { ready(config, false); return@post }
            val previous = lastGood
            if (previous != null && config.mode == previous.mode && config.copy(videoBitrate = previous.videoBitrate) == previous) {
                try {
                    val target = videoRate(config)
                    encoder!!.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, target) })
                    appliedBitrate = target
                    lastGood = config; activeConfig = config; ready(config, false)
                    LogManager.addLog("INFO", "video", "bitrate changed id=$id revision=$videoRevision nominal=${config.bitrate} target=$target")
                    return@post
                } catch (e: Exception) {
                    LogManager.addLog("WARN", "video", "live bitrate refused id=$id target=${config.bitrate}; reconfigure: ${e.message}")
                }
            }
            waitingFirst = true; reverting = false; switchStartedNs = System.nanoTime()
            LogManager.addLog("INFO", "video", "switch capture id=$id camera=${config.mode.camera} ${config.mode.label} orientation=${config.orientation}")
            releaseVideo { configure(config) }
        }) { LogManager.addLog("ERROR", "video", "capture change post rejected id=$id"); "Capture change rejected" }
    }

    private fun cameraPose(cameraId: String): StreamPreviewCamera {
        val c = manager.getCameraCharacteristics(cameraId)
        return StreamPreviewCamera(c.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0,
            c.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT)
    }

    @SuppressLint("MissingPermission")
    private fun configure(config: StreamCaptureConfig) {
        if (stopping || stopped || terminalFailure) return
        val mode = config.mode
        val token = ++generation
        try {
            val target = videoRate(config)
            check(++videoRevision <= 0xffffffffL) { "Video revision exhausted" }
            val cameraPose = if (mode.screen) StreamPreviewCamera(0, false) else cameraPose(mode.camera)
            videoRotation = if (mode.screen) 0 else config.orientation.sensorRotation(cameraPose.sensor, cameraPose.front, orientation.degrees)
            activeConfig = config
            observeOrientation(!mode.screen && config.orientation == StreamOrientation.AUTO)
            preview.update(null)
            LogManager.videoTrace("capture camera id=$id revision=$videoRevision generation=$token camera=${mode.camera} " +
                "sensor=${cameraPose.sensor} front=${cameraPose.front} orientation=${config.orientation} broadcastRotation=$videoRotation")
            csd = byteArrayOf(); partial.reset(); partialConfig.reset(); requireKey = true; lastKeyRequest = 0; waitingFirst = true
            confirming = mode.highSpeed; confirmStartNs = 0
            reportNs = System.nanoTime(); reportFrames = encodedFrames; reportBytes = bytesSent
            if (switchStartedNs == 0L) switchStartedNs = reportNs
            val format = MediaFormat.createVideoFormat(mode.videoCodec.mime, mode.width, mode.height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                if (mode.screen) {
                    setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 200_000L)
                    if (Build.VERSION.SDK_INT >= 29) setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, mode.fps.toFloat())
                }
                mode.encoderProfile?.let { setInteger(MediaFormat.KEY_PROFILE, it) }
                setInteger(MediaFormat.KEY_BIT_RATE, target)
                setInteger(MediaFormat.KEY_FRAME_RATE, mode.fps); setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                if (Build.VERSION.SDK_INT >= 29) setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            }
            encoderResources = VideoCodecResources(MediaCodec.createByCodecName(mode.codec), mode.codec)
            appliedBitrate = target
            encoder!!.also { codec ->
                codec.setCallback(object : MediaCodec.Callback() {
                    override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {}
                    override fun onError(codec: MediaCodec, error: MediaCodec.CodecException) { captureError(token, "encoder: ${error.diagnosticInfo}") }
                    override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                        if (!current(token)) return
                        try {
                            csd = listOf("csd-0", "csd-1").mapNotNull { name -> format.getByteBuffer(name)?.duplicate()?.let { b ->
                                ByteArray(b.remaining()).also { b.get(it) }.let(mode.videoCodec::normalize)
                            } }.fold(byteArrayOf()) { a, b -> a + b }
                            check(csd.size <= StreamVideoCodec.MAX_CONFIG) { "Codec configuration too large" }
                            LogManager.addLog("INFO", "video", "encoder format id=$id generation=$token codec=${codec.name} csd=${csd.size} format=$format")
                        } catch (e: Exception) { captureError(token, "encoder format: ${e.message}") }
                    }
                    override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                        if (!current(token)) return
                        var problem: String? = null
                        try {
                            if (info.size > 0) {
                                val buffer = codec.getOutputBuffer(index) ?: error("Missing video encoder output")
                                buffer.position(info.offset); buffer.limit(info.offset + info.size)
                                val bytes = ByteArray(info.size).also { buffer.get(it) }
                                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                                    check(partialConfig.size() + bytes.size <= StreamVideoCodec.MAX_CONFIG)
                                    partialConfig.write(bytes)
                                    if (info.flags and MediaCodec.BUFFER_FLAG_PARTIAL_FRAME == 0) {
                                        csd = mode.videoCodec.mergeConfig(csd, partialConfig.toByteArray()); partialConfig.reset()
                                    }
                                } else encoded(bytes, info, config, cameraPose, token)
                            }
                            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) problem = "unexpected encoder EOS"
                        } catch (e: Exception) { problem = "encoded frame: ${e.message}" }
                        finally {
                            try { codec.releaseOutputBuffer(index, false) }
                            catch (e: Exception) { problem = "encoder output release: ${e.message}" }
                        }
                        problem?.let { captureError(token, it) }
                    }
                }, handler)
                codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                input = codec.createInputSurface(); codec.start()
            }
            if (mode.screen) {
                checkNotNull(projection).attach(checkNotNull(input), mode.width, mode.height)
                monitorScreen(token, mode)
                check(handler.postDelayed({ if (current(token) && waitingFirst) captureError(token, "screen keyframe timeout") }, 8000))
                return
            }
            opening = true; openingGeneration = token
            try {
                manager.openCamera(mode.camera, object : CameraDevice.StateCallback() {
                    override fun onOpened(device: CameraDevice) {
                        if (device in closedCameras) { LogManager.addLog("WARN", "video", "closed capture camera callback ignored id=$id token=$token"); return }
                        if (openingGeneration == token) opening = false
                        if (!current(token)) { closingCameras.add(device); queueCleanup(0); return }
                        camera = device
                        createSession(device, config, token)
                    }
                    override fun onClosed(device: CameraDevice) {
                        if (!closedCameras.add(device)) { LogManager.videoTrace("duplicate capture camera close id=$id token=$token"); return }
                        if (camera === device) camera = null
                        closingCameras.remove(device)
                        val pending = cameraSessions.values.filter { it.first === device }.map { it.second }
                        LogManager.videoTrace("camera closed id=$id generation=$token retiringSessions=${pending.size}")
                        pending.forEach { it("cameraClosed") }
                        if (session?.let { ownedSessions[it] === device } == true) session = null
                        ownedSessions.entries.removeAll { it.value === device }; closingSessions.removeAll { it !in ownedSessions }
                        queueCleanup(0)
                    }
                    override fun onDisconnected(device: CameraDevice) { cameraFailure(device, token, "camera disconnected") }
                    override fun onError(device: CameraDevice, error: Int) { cameraFailure(device, token, "camera error=$error") }
                }, handler)
            } catch (e: Exception) {
                val awaitsCallback = e is CameraAccessException && e.reason in listOf(CameraAccessException.CAMERA_IN_USE,
                    CameraAccessException.CAMERA_DISABLED, CameraAccessException.CAMERA_DISCONNECTED)
                if (!awaitsCallback && openingGeneration == token) opening = false
                LogManager.addLog("ERROR", "video", "capture camera open failed id=$id token=$token awaitingCallback=$awaitsCallback: $e")
                throw e
            }
            check(handler.postDelayed({ if (current(token) && (waitingFirst || confirming))
                captureError(token, "keyframe/capture rate validation timeout") }, 8000)) { "Capture validation timer rejected" }
        } catch (e: Exception) { captureError(token, "video setup: ${e.message}") }
    }

    /** Статичный экран может перестать обновлять Surface. Обновляем вывод для нового KEY позднему зрителю. */
    private fun monitorScreen(token: Long, mode: StreamCaptureMode) {
        val count = encodedFrames
        check(handler.postDelayed({
            if (current(token)) try {
                if (count == encodedFrames) {
                    val session = checkNotNull(projection)
                    requestKey()
                    session.detach()
                    session.attach(checkNotNull(input), mode.width, mode.height)
                }
                monitorScreen(token, mode)
            } catch (e: Exception) { captureError(token, "screen refresh: $e") }
        }, 1000)) { "Screen refresh timer rejected" }
    }

    private fun current(token: Long) = token == generation && !stopping && !stopped && !terminalFailure && !videoReleasing

    private fun cameraFailure(device: CameraDevice, token: Long, detail: String) {
        if (device in closedCameras) { LogManager.addLog("WARN", "video", "closed capture camera error ignored id=$id token=$token: $detail"); return }
        if (openingGeneration == token) opening = false
        closingCameras.add(device)
        if (current(token)) { camera = device; captureError(token, detail) }
        else LogManager.addLog("WARN", "video", "stale capture camera error id=$id token=$token/$generation: $detail")
        queueCleanup(0)
    }

    /** Меняется только набор Camera2 outputs; encoder, audio и origin не перезапускаются. */
    private fun createSession(device: CameraDevice, config: StreamCaptureConfig, token: Long, withPreview: Boolean = true) {
        val mode = config.mode
        val serial = ++sessionGeneration
        val output = if (withPreview) previewTarget?.retain() else null
        var format: StreamPreviewFormat? = null
        var completed = false
        fun complete(reason: String) {
            if (completed) return
            completed = true; cameraSessions.remove(serial); output?.release()
            LogManager.videoTrace("camera session output released id=$id session=$serial reason=$reason remaining=${cameraSessions.size}")
            if (releaseDone != null || stopping) queueCleanup(0)
        }
        cameraSessions[serial] = device to ::complete
        fun valid() = current(token) && serial == sessionGeneration
        fun rejected(detail: String) {
            if (!valid()) { LogManager.videoTrace("stale camera session failure id=$id session=$serial: $detail"); return }
            if (output == null) { captureError(token, detail); return }
            LogManager.addLog("ERROR", "video", "direct preview failed id=$id session=$serial; keep encoder output: $detail")
            preview.update(format, failed = true)
            createSession(device, config, token, withPreview = false)
        }
        var submitted = false
        try {
            if (output != null) {
                val characteristics = manager.getCameraCharacteristics(mode.camera)
                val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                    ?: error("Missing preview stream map")
                val size = if (mode.highSpeed) android.util.Size(mode.width, mode.height) else
                    map.getOutputSizes(SurfaceTexture::class.java).orEmpty().filter {
                        it.width <= minOf(mode.width, 1280) && it.height <= minOf(mode.height, 720)
                            && it.width.toLong() * mode.height == it.height.toLong() * mode.width
                    }.maxByOrNull { it.width * it.height } ?: error("No compatible camera preview size")
                output.texture.setDefaultBufferSize(size.width, size.height)
                format = StreamPreviewFormat(size.width, size.height, cameraPose(mode.camera),
                    characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME)
                preview.update(format)
            } else if (withPreview) preview.update(preview.state.value.format)
            val targets = listOfNotNull(checkNotNull(input) { "Missing encoder surface" }, output?.surface)
            val callback = object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(configured: CameraCaptureSession) {
                    if (completed) { LogManager.addLog("WARN", "video", "closed capture session callback ignored id=$id serial=$serial"); return }
                    ownedSessions[configured] = device
                    if (!valid()) { closingSessions.add(configured); queueCleanup(0); return }
                    session = configured
                    try {
                        val request = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            targets.forEach(::addTarget)
                            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(mode.minimumFps, mode.fps))
                        }.build()
                        var sensorResults = 0L
                        var sensorReportCount = 0L
                        var sensorReportNs = System.nanoTime()
                        val diagnostics = object : CameraCaptureSession.CaptureCallback() {
                            override fun onCaptureCompleted(session: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
                                if (!valid()) return
                                sensorResults++
                                val now = System.nanoTime()
                                if (now - sensorReportNs < 1_000_000_000) return
                                val fps = (sensorResults - sensorReportCount) * 1_000_000_000.0 / (now - sensorReportNs)
                                LogManager.videoTrace("sensor id=$id generation=$token session=$serial results=$sensorResults " +
                                    "callbackFps=${"%.1f".format(fps)} range=${result.get(CaptureResult.CONTROL_AE_TARGET_FPS_RANGE)} " +
                                    "durationNs=${result.get(CaptureResult.SENSOR_FRAME_DURATION)} " +
                                    "exposureNs=${result.get(CaptureResult.SENSOR_EXPOSURE_TIME)} timestamp=${result.get(CaptureResult.SENSOR_TIMESTAMP)}")
                                sensorReportNs = now; sensorReportCount = sensorResults
                            }
                            override fun onCaptureFailed(session: CameraCaptureSession, request: CaptureRequest,
                                                         failed: android.hardware.camera2.CaptureFailure) {
                                if (valid()) LogManager.addLog("WARN", "video", "sensor capture failed id=$id generation=$token " +
                                    "session=$serial frame=${failed.frameNumber} reason=${failed.reason} captured=${failed.wasImageCaptured()}")
                            }
                        }
                        if (mode.highSpeed) {
                            val highSpeed = configured as CameraConstrainedHighSpeedCaptureSession
                            highSpeed.setRepeatingBurst(highSpeed.createHighSpeedRequestList(request), diagnostics, handler)
                        } else configured.setRepeatingRequest(request, diagnostics, handler)
                        if (output != null) preview.update(format, active = true)
                        LogManager.addLog("INFO", "video", "capture started id=$id generation=$token session=$serial camera=${mode.camera} ${mode.label} " +
                            "range=${mode.minimumFps}..${mode.fps} highSpeed=${mode.highSpeed} encoder=${encoder?.name} " +
                            "preview=${output != null} previewSize=${format?.let { "${it.width}x${it.height}" }} origin=$originNs")
                    } catch (e: Exception) { closingSessions.add(configured); queueCleanup(0); rejected("camera request: ${e.message}") }
                }
                override fun onConfigureFailed(session: CameraCaptureSession) {
                    complete("configureFailed"); rejected("camera session configuration")
                }
                override fun onClosed(closed: CameraCaptureSession) {
                    ownedSessions.remove(closed); closingSessions.remove(closed)
                    if (session === closed) session = null
                    complete("sessionClosed")
                }
            }
            LogManager.videoTrace("camera session configuring id=$id generation=$token session=$serial outputs=${targets.size}")
            submitted = true
            if (mode.highSpeed) device.createConstrainedHighSpeedCaptureSession(targets, callback, handler)
            else device.createCaptureSession(targets, callback, handler)
        } catch (e: Exception) {
            if (!submitted) { complete("exception before submit"); rejected("camera session: ${e.message}") }
            else {
                LogManager.addLog("ERROR", "video", "capture session submit failed id=$id serial=$serial; outputs retained until device closes: $e")
                captureError(token, "camera session: ${e.message}")
            }
        }
    }

    private fun captureError(token: Long, detail: String) {
        if (!current(token)) { LogManager.videoTrace("stale capture error generation=$token: $detail"); return }
        LogManager.addLog("ERROR", "video", "capture error id=$id generation=$token: $detail")
        val previous = lastGood
        if (previous == null || reverting) { failed(detail); return }
        waitingFirst = true; reverting = true
        LogManager.addLog("WARN", "video", "restoring camera=${previous.mode.camera} ${previous.mode.label} orientation=${previous.orientation} id=$id")
        releaseVideo { configure(previous) }
    }

    private fun encoded(bytes: ByteArray, info: MediaCodec.BufferInfo, config: StreamCaptureConfig, pose: StreamPreviewCamera, token: Long) {
        val mode = config.mode
        if (partial.size() == 0) { partialPts = info.presentationTimeUs; partialKey = false }
        check(info.presentationTimeUs == partialPts) { "Partial video frame PTS changed" }
        check(partial.size() + bytes.size <= H264AnnexB.MAX_FRAME) { "Video frame exceeds limit" }
        partial.write(bytes); partialKey = partialKey || info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
        if (info.flags and MediaCodec.BUFFER_FLAG_PARTIAL_FRAME != 0) return
        val data = mode.videoCodec.normalize(partial.toByteArray()); partial.reset()
        val key = partialKey
        val pts = partialPts - originNs / 1000
        check(pts >= 0 && pts > lastPts) { "Video PTS outside timeline pts=$pts last=$lastPts" }
        lastPts = pts; encodedFrames++
        if (orientationEnabled && orientation.confirm(SystemClock.elapsedRealtime())) logOrientation()
        val rotation = if (mode.screen) 0 else config.orientation.sensorRotation(pose.sensor, pose.front, orientation.degrees)
        // Новый угол передаём с KEY и отдельной revision, не перезапуская камеру и микрофон.
        if (rotation != videoRotation) {
            if (key) {
                check(++videoRevision <= 0xffffffffL) { "Video revision exhausted" }
                LogManager.addLog("INFO", "video", "stream rotation id=$id revision=$videoRevision device=${orientation.degrees} " +
                    "sensor=${pose.sensor} front=${pose.front} rotation=$videoRotation->$rotation pts=$pts")
                videoRotation = rotation
            } else requestKey()
        }
        if (requireKey && !key) { dropped++; requestKey() }
        else {
            val output = if (key) mode.videoCodec.keyFrame(data, csd) else data
            if (key) mode.videoCodec.unpackKey(output)
            when (NativeLib.streamFeed(id, true, pts, key, output, videoRevision, mode.width, mode.height, videoRotation, microphone, mode.videoCodec.id)) {
                1 -> {
                    requireKey = false; frames++; bytesSent += output.size
                    if (waitingFirst) {
                        check(key); waitingFirst = false
                        confirmStartNs = System.nanoTime(); confirmFrames = encodedFrames
                        if (!confirming) confirmed(config, videoRevision, pts)
                    }
                }
                0 -> { dropped++; requireKey = true; requestKey() }
                else -> failed("stream publisher detached")
            }
        }
        val now = System.nanoTime()
        if (confirming && confirmStartNs != 0L && now - confirmStartNs >= 3_000_000_000) {
            val actualFps = (encodedFrames - confirmFrames) * 1_000_000_000.0 / (now - confirmStartNs)
            if (actualFps < mode.fps * 0.8) {
                captureError(token, "high speed rate insufficient: camera=${mode.camera} target=${mode.fps} actual=${"%.1f".format(actualFps)} fps")
                return
            }
            confirmed(config, videoRevision, pts)
        }
        if (!waitingFirst && !confirming) adaptBitrate()
        if (now - reportNs >= 1_000_000_000) {
            val seconds = (now - reportNs) / 1_000_000_000.0
            LogManager.videoTrace("capture id=$id revision=$videoRevision targetFps=${mode.fps} " +
                "fps=${"%.1f".format((encodedFrames - reportFrames) / seconds)} bitrate=${((bytesSent - reportBytes) * 8 / seconds).toLong()} " +
                "frames=$frames dropped=$dropped pts=$pts waitingKey=$requireKey")
            reportNs = now; reportFrames = encodedFrames; reportBytes = bytesSent
        }
    }

    /** Конфигурация остаётся исходной: временное снижение не меняет Auto и сохранённый bitrate пользователя. */
    private fun videoRate(config: StreamCaptureConfig): Int = NativeLib.streamVideoRate(id,
        config.videoBitrate == null, config.bitrate, config.mode.minimumBitrate).also { check(it > 0) { "Publisher bitrate unavailable" } }

    private fun adaptBitrate() {
        val config = activeConfig ?: return
        val target = videoRate(config)
        if (target == appliedBitrate || stopping || stopped || terminalFailure || releaseDone != null) return
        try {
            encoder!!.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, target) })
            LogManager.addLog("INFO", "video", "automatic bitrate id=$id $appliedBitrate->$target nominal=${config.bitrate}")
            appliedBitrate = target
        } catch (e: Exception) {
            LogManager.addLog("WARN", "video", "automatic bitrate needs encoder replacement id=$id target=$target: ${e.message}")
            waitingFirst = true; reverting = false; switchStartedNs = System.nanoTime()
            releaseVideo { configure(config) }
        }
    }

    /** Новый скоростной режим подтверждается реальной скоростью; до этого lastGood сохраняется для rollback. */
    private fun confirmed(config: StreamCaptureConfig, revision: Long, pts: Long) {
        val mode = config.mode
        confirming = false; lastGood = config
        LogManager.addLog("INFO", "video", "capture ready id=$id revision=$revision screen=${mode.screen} camera=${mode.camera} ${mode.label} " +
            "orientation=${config.orientation} rotation=$videoRotation switchMs=${(System.nanoTime() - switchStartedNs) / 1_000_000} restored=$reverting pts=$pts")
        ready(config, reverting); reverting = false; switchStartedNs = 0
    }

    private fun requestKey() {
        val now = System.nanoTime()
        if (now - lastKeyRequest < 500_000_000) return
        lastKeyRequest = now
        encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        LogManager.videoTrace("requested video keyframe id=$id encoder=${encoder?.name} revision=$videoRevision")
    }

    /** Новая камера открывается только после onClosed; старые codec callbacks уже отсечены. */
    private fun releaseVideo(done: () -> Unit) {
        if (!videoReleasing) {
            ++generation; sessionGeneration++; videoReleasing = true
            videoReleaseSince = System.nanoTime(); slowReleaseReported = false
            preview.update(preview.state.value.format)
        }
        releaseDone = done; retained.add(this); queueCleanup(0)
    }

    /** Только capture worker: независимый cleanup, refs снимаются после возврата SDK либо callback закрытия. */
    private fun cleanupResources() {
        if (stopping) {
            if (!stopVideoRequested) { stopVideoRequested = true; releaseVideo {} }
            releaseDone = null; audioRunning = false
            try { observeOrientation(false) } catch (e: Exception) {
                LogManager.addLog("ERROR", "video", "capture orientation release failed id=$id; retained: $e")
            }
            preview.close(); previewTarget?.release(); previewTarget = null
            try { audioCapture?.close(); audioCapture = null } catch (e: Exception) {
                LogManager.addLog("ERROR", "stream", "capture microphone subscription close failed id=$id; retained: $e")
            }
            val worker = audioThread
            if (worker?.isAlive == true) try { worker.join(250) } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
                LogManager.addLog("ERROR", "stream", "capture audio join interrupted id=$id; worker retained: $e")
            }
            if (worker?.isAlive != true) audioThread = null
        }
        if (videoReleasing) {
            closingSessions.addAll(ownedSessions.keys); camera?.let { closingCameras.add(it) }
        }
        for (owned in closingSessions.toList()) try { owned.close() } catch (e: Exception) {
            LogManager.addLog("ERROR", "video", "capture session close failed id=$id; retained: $e")
        }
        for (owned in closingCameras.toList()) try { owned.close() } catch (e: Exception) {
            LogManager.addLog("ERROR", "video", "capture camera close failed id=$id device=${owned.id}; retained: $e")
        }
        val cameraClosed = !opening && camera == null && closingCameras.isEmpty() && ownedSessions.isEmpty() && cameraSessions.isEmpty()
        if (videoReleasing && cameraClosed) {
            try { projection?.detach(); encoderResources?.close(); encoderResources = null } catch (e: Exception) {
                LogManager.addLog("ERROR", "video", "capture encoder close failed id=$id; input Surface retained: $e")
            }
            if (encoderResources == null) try { input?.release(); input = null } catch (e: Exception) {
                LogManager.addLog("ERROR", "video", "capture input Surface close failed id=$id; retained: $e")
            }
            if (encoderResources == null && input == null) {
                videoReleasing = false; partial.reset(); partialConfig.reset(); csd = byteArrayOf()
                val done = releaseDone; releaseDone = null
                LogManager.addLog("INFO", "video", "capture video cleanup confirmed id=$id generation=$generation")
                if (!stopping) try { done?.invoke() } catch (e: Exception) { failed("capture continuation: $e") }
            }
        }
        if (stopping && !videoReleasing && !opening && camera == null && closingCameras.isEmpty() &&
            ownedSessions.isEmpty() && cameraSessions.isEmpty() && encoderResources == null && input == null &&
            audioCapture == null && audioThread == null && !orientationEnabled) {
            stopped = true; retained.remove(this)
            synchronized(this) { handler.removeCallbacks(cleanupTask); cleanupQueued = false }
            thread.quitSafely()
            LogManager.addLog("INFO", "stream", "capture cleanup confirmed id=$id frames=$frames dropped=$dropped bytes=$bytesSent")
            return
        }
        val pending = videoReleasing || closingCameras.isNotEmpty() || closingSessions.isNotEmpty() || stopping
        if (pending) {
            val detail = "id=$id stopping=$stopping opening=$opening devices=${closingCameras.size} " +
                "sessions=${ownedSessions.size}/${cameraSessions.size} codec=${encoderResources != null} input=${input != null} " +
                "audio=${audioThread?.state} subscription=${audioCapture != null} sensor=$orientationEnabled"
            if (videoReleasing && !slowReleaseReported && System.nanoTime() - videoReleaseSince >= 3_000_000_000) {
                slowReleaseReported = true
                LogManager.addLog("ERROR", "video", "capture close awaiting SDK acknowledgement; owners retained $detail")
            }
            LogManager.addLog("DEBUG", "stream", "capture cleanup pending $detail delayMs=$cleanupDelay")
            queueCleanup(cleanupDelay); cleanupDelay = (cleanupDelay * 2).coerceAtMost(30_000)
        } else { retained.remove(this); cleanupDelay = 250 }
    }

    @Synchronized private fun queueCleanup(delay: Long): Boolean {
        if (stopped) return true
        val due = System.nanoTime() + delay * 1_000_000
        if (cleanupQueued && cleanupDue <= due) return true
        handler.removeCallbacks(cleanupTask); cleanupDue = due
        cleanupQueued = if (delay == 0L) handler.post(cleanupTask) else handler.postDelayed(cleanupTask, delay)
        if (!cleanupQueued) LogManager.addLog("ERROR", "stream", "capture cleanup post rejected id=$id; owner retained")
        return cleanupQueued
    }
    /** Меняем только свою копию PCM; общий микрофон и рация продолжают работать. */
    fun setAudioEnabled(enabled: Boolean): Boolean {
        if (!microphone || stopping || stopped) {
            LogManager.addLog("WARN", "stream", "audio change without capture id=$id enabled=$enabled"); return false
        }
        val previous = audioMuted
        audioMuted = !enabled
        val accepted = try { NativeLib.streamSetAudioEnabled(id, enabled) } catch (e: Exception) {
            LogManager.addLog("ERROR", "stream", "audio change failed id=$id enabled=$enabled: $e"); false
        }
        if (!accepted) {
            audioMuted = previous
            LogManager.addLog("ERROR", "stream", "audio change refused id=$id enabled=$enabled"); return false
        }
        LogManager.addLog("INFO", "stream", "source audio id=$id enabled=$enabled")
        return true
    }

    @SuppressLint("MissingPermission")
    private fun startAudio() {
        val subscription = MediaAudioController.subscribe("stream")
        audioCapture = subscription
        audioRunning = true
        LogManager.addLog("INFO", "stream", "shared audio subscribed id=$id rate=48000 channels=2 compressor=$audioCompressorSettings")
        audioThread = Thread({
            var opus = 0L
            try {
                opus = NativeLib.streamOpusCreate(true); check(opus != 0L)
                var waitingStamp = true; var last = -1L; var expected = -1L
                while (audioRunning) {
                    val frame = subscription.poll() as? MicrophoneCapture.Frame ?: continue
                    if (!audioRunning) break
                    if (frame.timeNs == 0L) {
                        if (waitingStamp) LogManager.addLog("DEBUG", "stream", "audio timestamp pending id=$id samples=${frame.sequence}")
                        continue
                    }
                    waitingStamp = false
                    if (expected >= 0 && frame.sequence != expected) {
                        LogManager.addLog("WARN", "stream", "audio queue discontinuity id=$id expected=$expected sequence=${frame.sequence}")
                        NativeLib.streamOpusClose(opus); opus = 0
                        opus = NativeLib.streamOpusCreate(true); check(opus != 0L)
                    }
                    expected = frame.sequence + 960
                    val pts = (frame.timeNs - originNs) / 1000
                    if (pts < 0) continue // Общий захват мог начаться раньше эфира; его запас времени ещё дренируется.
                    check(pts >= 0 && pts > last) { "Audio timestamp discontinuity pts=$pts last=$last" }; last = pts
                    if (audioMuted) frame.pcm.fill(0)
                    val packet = NativeLib.streamOpusEncode(opus, frame.pcm, audioCompressorSettings) ?: error("Stream compressor/Opus encode failed")
                    check(NativeLib.streamFeed(id, false, pts, false, packet) >= 0) { "Stream audio publisher detached" }
                }
            } catch (e: Exception) {
                LogManager.addLog(if (audioRunning) "ERROR" else "WARN", "stream", "capture audio worker ended id=$id running=$audioRunning: $e")
                if (audioRunning && !handler.post { failed("audio capture: ${e.message}") })
                    LogManager.addLog("ERROR", "stream", "capture audio failure notification rejected id=$id")
            } finally {
                var delay = 250L
                if (Thread.interrupted()) LogManager.addLog("WARN", "stream", "capture audio interrupted before Opus release id=$id")
                while (opus != 0L) try { NativeLib.streamOpusClose(opus); opus = 0 } catch (e: Exception) {
                    LogManager.addLog("ERROR", "stream", "capture Opus close failed id=$id handle=$opus; retained: $e")
                    try { Thread.sleep(delay) } catch (interrupted: InterruptedException) {
                        LogManager.addLog("WARN", "stream", "capture Opus cleanup wait interrupted id=$id; retrying")
                    }
                    delay = (delay * 2).coerceAtMost(30_000)
                }
            }
        }, "stream-audio-capture")
        audioThread!!.start()
    }

    private fun failed(detail: String) {
        if (stopping || stopped || terminalFailure) return
        terminalFailure = true
        LogManager.addLog("ERROR", "stream", "capture failed id=$id $detail")
        stopping = true; audioRunning = false; retained.add(this); queueCleanup(0)
        try { failure(detail) } catch (e: Exception) { LogManager.addLog("ERROR", "stream", "capture failure callback failed id=$id: $e") }
    }

    /** Producers закрываются до native FIN; уже принятые кадры дренирует native publisher. */
    fun stop(): Boolean {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Capture stop requires main" }
        if (stopped) return true
        stopping = true; audioRunning = false; retained.add(this)
        if (!::handler.isInitialized) {
            preview.close(); stopped = true; retained.remove(this); thread.quitSafely()
            LogManager.addLog("INFO", "stream", "passive capture released before worker startup id=$id")
            return true
        }
        check(queueCleanup(0)) { "Capture cleanup worker rejected stop id=$id" }
        return stopped
    }
}
