package com.utun.chat.ui.screens

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.media.MediaFormat
import android.os.SystemClock
import android.text.format.Formatter
import android.util.Size
import android.view.OrientationEventListener
import android.view.Surface
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.*
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.video.*
import androidx.camera.view.PreviewView
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.PopupProperties
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.utun.chat.R
import com.utun.chat.ChatApplication
import com.utun.chat.data.*
import com.utun.chat.ui.components.ZoomableImage
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.roundToInt

enum class CaptureMode { PHOTO, VIDEO }
private enum class CapturePhase { IDLE, CAPTURING, RECORDING, PAUSED, FINALIZING, CAPTURED, SENDING }
private enum class CameraOption { CAMERA, CODEC, RESOLUTION, ORIENTATION, BITRATE, JPEG_QUALITY }
private enum class CaptureResolution(val label: String, val quality: Quality, val height: Int) {
    SD("480p", Quality.SD, 480), HD("720p", Quality.HD, 720), FHD("1080p", Quality.FHD, 1080)
}

/** CameraX фото/MP4 до 60 с: выбранный use case и cache принадлежат CameraCaptureResources до callback writer.
 * Передача sender происходит только после завершения/проверки файла; закрытие UI не удаляет работающий output. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CameraRecordScreen(initialMode: CaptureMode, onSend: (String, CaptureMode) -> Unit, onClose: () -> Unit) {
    val context = LocalContext.current
    val displayDegrees = StreamDisplayDegrees()
    val lifecycle = LocalLifecycleOwner.current
    val main = remember(context) { ContextCompat.getMainExecutor(context) }
    val scope = rememberCoroutineScope()
    val resources = remember { CameraCaptureResources() }
    val preferences = remember { ChatApplication.instance.configProvider }
    var photoSettings by remember { mutableStateOf(PhotoCaptureSettings()) }
    var videoSettings by remember { mutableStateOf(VideoCaptureSettings()) }
    var settingsReady by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        try {
            photoSettings = preferences.getPhotoCaptureSettings()
            videoSettings = preferences.getVideoCaptureSettings()
            LogManager.addLog("INFO", "Capture", "Settings loaded photo=$photoSettings video=$videoSettings")
        } catch (e: kotlinx.coroutines.CancellationException) { throw e }
        catch (e: Exception) { LogManager.addLog("ERROR", "Capture", "Settings load failed; using available defaults: $e") }
        settingsReady = true
    }
    val send by rememberUpdatedState(onSend)
    var mode by remember { mutableStateOf(initialMode) }
    var phase by remember { mutableStateOf(CapturePhase.IDLE) }
    var closed by remember { mutableStateOf(false) }
    var output by remember { mutableStateOf<File?>(null) }
    var recording by remember { mutableStateOf<Recording?>(null) }
    var pendingSend by remember { mutableStateOf(false) }
    var elapsed by remember { mutableStateOf(0L) }
    var error by remember { mutableStateOf(false) }
    var provider by remember { mutableStateOf<ProcessCameraProvider?>(null) }
    var camera by remember { mutableStateOf<Camera?>(null) }
    var lenses by remember { mutableStateOf<List<LensOption>>(emptyList()) }
    var lens by remember { mutableStateOf<LensOption?>(null) }
    var resolutions by remember { mutableStateOf<List<PhotoResolution>>(emptyList()) }
    var resolution by remember { mutableStateOf<PhotoResolution?>(null) }
    var quality by remember { mutableStateOf(CaptureResolution.HD) }
    var bitrate by remember { mutableIntStateOf(2_000_000) }
    var jpegQuality by remember { mutableIntStateOf(85) }
    var codec by remember { mutableStateOf(StreamVideoCodec.H264) }
    var orientation by remember { mutableStateOf(StreamOrientation.AUTO) }
    var deviceDegrees by remember { mutableIntStateOf((360 - displayDegrees) % 360) }
    var videoOptions by remember(provider, lens?.cameraId) {
        mutableStateOf<Map<StreamVideoCodec, List<CaptureResolution>>?>(null)
    }
    var torch by remember { mutableStateOf(false) }
    var bitmap by remember(output) { mutableStateOf<Bitmap?>(null) }
    var photoSize by remember(output) { mutableStateOf(0 to 0) }
    var hasCamera by remember { mutableStateOf(ContextCompat.checkSelfPermission(context,
        Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) }
    var hasAudio by remember { mutableStateOf(ContextCompat.checkSelfPermission(context,
        Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) }
    val cameraPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
        hasCamera = it; LogManager.addLog(if (it) "INFO" else "WARN", "Capture", "Camera permission granted=$it")
    }
    val audioPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
        hasAudio = it; LogManager.addLog(if (it) "INFO" else "WARN", "Capture", "Microphone permission granted=$it")
    }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) try {
                hasCamera = ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                hasAudio = ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
            } catch (e: Exception) { error = true; LogManager.addLog("ERROR", "Capture", "Permission refresh failed: $e") }
        }
        lifecycle.lifecycle.addObserver(observer)
        onDispose { lifecycle.lifecycle.removeObserver(observer) }
    }
    fun requestCameraPermission() {
        try { cameraPermission.launch(Manifest.permission.CAMERA) }
        catch (e: Exception) { error = true; LogManager.addLog("ERROR", "Capture", "Camera permission launch failed: $e") }
    }
    fun requestAudioPermission() {
        try { audioPermission.launch(Manifest.permission.RECORD_AUDIO) }
        catch (e: Exception) { error = true; LogManager.addLog("ERROR", "Capture", "Microphone permission launch failed: $e") }
    }
    LaunchedEffect(Unit) { if (!hasCamera) requestCameraPermission() }
    LaunchedEffect(mode, hasCamera) {
        if (mode == CaptureMode.VIDEO && hasCamera && !hasAudio) requestAudioPermission()
    }
    val preview = remember { Preview.Builder().build() }
    val imageCapture = remember(resolution, jpegQuality) {
        ImageCapture.Builder().setCaptureMode(ImageCapture.CAPTURE_MODE_MINIMIZE_LATENCY).setJpegQuality(jpegQuality).apply {
            resolution?.let { setResolutionSelector(ResolutionSelector.Builder().setResolutionStrategy(
                ResolutionStrategy(Size(it.width, it.height), ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER)).build()) }
        }.build()
    }
    val videoCapture = remember(quality, bitrate, codec) {
        VideoCapture.withOutput(Recorder.Builder().setQualitySelector(QualitySelector.from(quality.quality))
            .setVideoMimeType(codec.mime).setAudioMimeType(MediaFormat.MIMETYPE_AUDIO_AAC).setTargetVideoEncodingBitRate(bitrate).build())
    }

    fun rememberPhotoSettings() {
        photoSettings = PhotoCaptureSettings(lens?.cameraId.orEmpty(), resolution?.width ?: 0, resolution?.height ?: 0, jpegQuality, orientation)
        preferences.savePhotoCaptureSettings(photoSettings)
    }
    fun rememberVideoSettings() {
        videoSettings = VideoCaptureSettings(lens?.cameraId.orEmpty(), quality.height, codec.id, bitrate, orientation)
        preferences.saveVideoCaptureSettings(videoSettings)
    }

    fun reset() {
        recording = null
        val oldFile = output
        output = null; pendingSend = false; elapsed = 0; phase = CapturePhase.IDLE
        resources.discard(oldFile)
        LogManager.addLog("INFO", "Capture", "Capture discarded mode=$mode")
    }
    fun sendResult() {
        val file = output ?: return
        if (phase != CapturePhase.CAPTURED || file.length() <= 0) {
            LogManager.addLog("ERROR", "Capture", "Cannot send phase=$phase path=${file.path} bytes=${file.length()}")
            error = true; return
        }
        LogManager.addLog("INFO", "Capture", "Handing cache to sender mode=$mode path=${file.path} bytes=${file.length()}")
        try {
            send(file.path, mode)
            resources.transferred(file)
            output = null; phase = CapturePhase.SENDING
        } catch (e: Exception) {
            error = true
            LogManager.addLog("ERROR", "Capture", "Sender rejected cache path=${file.path}: $e")
        }
    }
    BackHandler { reset(); onClose() }

    // Авто работает независимо от автоповорота экрана; для MP4 угол фиксируется при старте записи.
    DisposableEffect(hasCamera, settingsReady, mode, orientation, phase == CapturePhase.IDLE) {
        val tracker = CallVideoOrientation()
        fun updateOrientation() {
            deviceDegrees = tracker.degrees
            LogManager.addLog("DEBUG", "Capture", "Device orientation mode=$mode degrees=$deviceDegrees")
        }
        val listener = object : OrientationEventListener(context) {
            override fun onOrientationChanged(angle: Int) {
                if (!closed && tracker.observe(angle, SystemClock.elapsedRealtime())) updateOrientation()
            }
        }
        val auto = hasCamera && settingsReady && orientation == StreamOrientation.AUTO && phase == CapturePhase.IDLE
        if (auto) {
            resources.ownOrientation(listener)
            try {
                if (listener.canDetectOrientation()) listener.enable()
                else LogManager.addLog("WARN", "Capture", "Orientation sensor unavailable mode=$mode; using deviceDegrees=$deviceDegrees")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "Capture", "Orientation start failed: $e")
                resources.releaseOrientation(listener)
            }
        }
        // Подтверждение нужно и без новых событий: датчик может перестать присылать неизменный угол.
        val confirmation = if (auto) scope.launch {
            while (isActive) {
                delay(250)
                if (tracker.confirm(SystemClock.elapsedRealtime())) updateOrientation()
            }
        } else null
        onDispose { confirmation?.cancel(); resources.releaseOrientation(listener) }
    }

    fun captureRotation(): Int {
        val degrees = orientation.targetDegrees(deviceDegrees)
        val rotation = when (degrees) {
            90 -> Surface.ROTATION_90
            180 -> Surface.ROTATION_180
            270 -> Surface.ROTATION_270
            else -> Surface.ROTATION_0
        }
        LogManager.addLog("INFO", "Capture", "Capture orientation mode=$mode selection=$orientation device=$deviceDegrees " +
            "target=$degrees sensorRotation=${camera?.cameraInfo?.getSensorRotationDegrees(rotation)} camera=${lens?.cameraId}")
        return rotation
    }

    DisposableEffect(Unit) {
        onDispose {
            closed = true
            resources.close()
            recording = null; provider = null; output = null
            LogManager.addLog("INFO", "Capture", "Camera released mode=$mode phase=$phase")
        }
    }
    LaunchedEffect(hasCamera) {
        if (hasCamera) {
            try {
                val future = ProcessCameraProvider.getInstance(context)
                future.addListener({
                if (!closed) try {
                    val ready = future.get()
                    lenses = CameraLensManager.enumerate(ready)
                    provider = ready
                    if (lenses.isEmpty()) { error = true; LogManager.addLog("ERROR", "Capture", "No available cameras") }
                } catch (e: Exception) {
                    error = true; LogManager.addLog("ERROR", "Capture", "Provider initialization failed: ${e.message}")
                }
                }, main)
            } catch (e: Exception) { error = true; LogManager.addLog("ERROR", "Capture", "Provider request failed: $e") }
        }
    }
    LaunchedEffect(mode, lenses, settingsReady) {
        if (!settingsReady || lenses.isEmpty()) return@LaunchedEffect
        camera = null
        lens = if (mode == CaptureMode.PHOTO) photoSettings.resolveCamera(lenses) else videoSettings.resolveCamera(lenses)
        jpegQuality = photoSettings.jpegQuality; bitrate = videoSettings.bitrate
        codec = StreamVideoCodec.fromId(videoSettings.codec)
        orientation = if (mode == CaptureMode.PHOTO) photoSettings.orientation else videoSettings.orientation
        quality = CaptureResolution.entries.first { it.height == videoSettings.height }
        val savedCamera = if (mode == CaptureMode.PHOTO) photoSettings.camera else videoSettings.camera
        if (savedCamera.isNotEmpty() && lens?.cameraId != savedCamera)
            LogManager.addLog("WARN", "Capture", "Saved camera unavailable mode=$mode saved=$savedCamera selected=${lens?.cameraId}")
        LogManager.addLog("INFO", "Capture", "Settings restored mode=$mode camera=${lens?.cameraId} type=${lens?.type} " +
            "jpegQuality=$jpegQuality resolution=${quality.label} codec=${codec.label} bitrate=$bitrate orientation=$orientation")
    }
    LaunchedEffect(provider, lens, mode) {
        val ready = provider; val selected = lens
        if (ready != null && selected != null) {
            if (mode == CaptureMode.PHOTO) {
                try {
                    resolutions = CameraLensManager.photoResolutions(ready, selected.cameraId)
                    resolution = photoSettings.resolveResolution(resolutions)
                    if (photoSettings.width > 0 && (resolution?.width != photoSettings.width || resolution?.height != photoSettings.height))
                        LogManager.addLog("WARN", "Capture", "Saved photo resolution unavailable camera=${selected.cameraId} " +
                            "saved=${photoSettings.width}x${photoSettings.height} selected=${resolution?.label}")
                    LogManager.addLog("INFO", "Capture", "Photo settings restored camera=${selected.cameraId} " +
                        "resolution=${resolution?.label} jpegQuality=$jpegQuality")
                } catch (e: Exception) {
                    resolutions = emptyList(); resolution = null; error = true
                    LogManager.addLog("ERROR", "Capture", "Photo capabilities failed camera=${selected.cameraId}: $e")
                }
                return@LaunchedEffect
            }
            try {
                val info = ready.getCameraInfo(selected.selector())
                val options = withContext(Dispatchers.Default) {
                    listOf(StreamVideoCodec.H264, StreamVideoCodec.H265).associateWith { format ->
                        val supported = Recorder.getVideoCapabilities(info, format.mime)?.getSupportedQualities(DynamicRange.SDR).orEmpty()
                        CaptureResolution.entries.filter { it.quality in supported }
                    }.filterValues { it.isNotEmpty() }
                }
                val nextCodec = codec.takeIf { it in options } ?: options.keys.firstOrNull()
                if (nextCodec == null) {
                    error = true
                    LogManager.addLog("ERROR", "Capture", "No H.264/H.265 recording modes camera=${selected.cameraId}")
                } else {
                    if (nextCodec != codec) LogManager.addLog("WARN", "Capture", "Codec unavailable camera=${selected.cameraId}, using $nextCodec")
                    codec = nextCodec
                    val supported = options.getValue(nextCodec)
                    if (quality !in supported) {
                        val requested = quality
                        quality = CaptureResolution.HD.takeIf { it in supported } ?: supported.last()
                        LogManager.addLog("WARN", "Capture", "Saved video resolution unavailable camera=${selected.cameraId} " +
                            "codec=${codec.label} saved=${requested.label} selected=${quality.label}")
                    }
                }
                videoOptions = options
                LogManager.addLog("INFO", "Capture", "Recording capabilities camera=${selected.cameraId} " +
                    "modes=${options.mapValues { it.value.map(CaptureResolution::label) }}")
            } catch (e: kotlinx.coroutines.CancellationException) { throw e }
            catch (e: Exception) {
                videoOptions = emptyMap(); error = true
                LogManager.addLog("ERROR", "Capture", "Recording capabilities failed camera=${selected.cameraId}: ${e.message}")
            }
        }
    }
    LaunchedEffect(provider, lens, mode, imageCapture, videoCapture, videoOptions, hasCamera, hasAudio, phase == CapturePhase.IDLE) {
        if (!hasCamera) { camera = null; resources.unbind(); return@LaunchedEffect }
        if (phase != CapturePhase.IDLE) return@LaunchedEffect
        val ready = provider ?: return@LaunchedEffect
        val selected = lens ?: return@LaunchedEffect
        camera = null; torch = false
        if (!resources.unbind()) { error = true; return@LaunchedEffect }
        if (mode == CaptureMode.VIDEO && (!hasAudio || videoOptions?.get(codec)?.contains(quality) != true)) return@LaunchedEffect
        try {
            val useCase: UseCase = if (mode == CaptureMode.PHOTO) imageCapture else videoCapture
            resources.binding(ready, preview, useCase)
            camera = ready.bindToLifecycle(lifecycle, selected.selector(), preview, useCase)
            error = false
            LogManager.addLog("INFO", "Capture", "Camera bound mode=$mode lens=${selected.cameraId} " +
                "photo=${resolution?.label} jpegQuality=$jpegQuality video=${quality.label} codec=${codec.label} bitrate=$bitrate")
        } catch (e: Exception) {
            resources.unbind()
            error = true; LogManager.addLog("ERROR", "Capture", "Camera bind failed mode=$mode lens=${selected.cameraId}: ${e.message}")
        }
    }
    LaunchedEffect(phase, output) {
        val file = output
        if (phase == CapturePhase.CAPTURED && mode == CaptureMode.PHOTO && file != null) {
            try {
                val result = withContext(Dispatchers.IO) {
                    PhotoUtil.metadata(file.path) to PhotoUtil.decodeFitted(file.path, 4096)
                }
                photoSize = result.first.width to result.first.height; bitmap = result.second
                if (bitmap == null) { error = true; LogManager.addLog("ERROR", "Capture", "Photo preview decode failed path=${file.path}") }
                else LogManager.addLog("INFO", "Capture", "JPEG preview ready path=${file.path} bytes=${file.length()} " +
                    "quality=$jpegQuality original=${photoSize.first}x${photoSize.second} decoded=${bitmap?.width}x${bitmap?.height}")
            } catch (e: kotlinx.coroutines.CancellationException) { throw e }
            catch (e: Exception) { error = true; LogManager.addLog("ERROR", "Capture", "Photo metadata failed: ${e.message}") }
        }
    }

    fun capturePhoto() {
        if (phase != CapturePhase.IDLE || camera == null) return
        val file = try { File.createTempFile("photo_msg_", ".jpg", context.cacheDir) }
        catch (e: Exception) { error = true; LogManager.addLog("ERROR", "Capture", "Photo cache failed: ${e.message}"); return }
        resources.own(file)
        output = file; phase = CapturePhase.CAPTURING; error = false
        LogManager.addLog("INFO", "Capture", "Photo capture requested camera=${lens?.cameraId} " +
            "resolution=${resolution?.label} jpegQuality=$jpegQuality")
        try {
            imageCapture.targetRotation = captureRotation()
            imageCapture.takePicture(ImageCapture.OutputFileOptions.Builder(file).build(), main, object : ImageCapture.OnImageSavedCallback {
                override fun onImageSaved(results: ImageCapture.OutputFileResults) {
                    if (!resources.completed(file) || closed || output != file) { resources.discard(file); return }
                    phase = CapturePhase.CAPTURED
                    LogManager.addLog("INFO", "Capture", "Photo captured path=${file.path} bytes=${file.length()}")
                }
                override fun onError(exception: ImageCaptureException) {
                    resources.completed(file); resources.discard(file)
                    LogManager.addLog("ERROR", "Capture", "Photo capture failed code=${exception.imageCaptureError}: ${exception.message}")
                    if (closed || output != file) return
                    output = null; phase = CapturePhase.IDLE; error = true
                }
            })
        } catch (e: Exception) {
            resources.completed(file); resources.discard(file); output = null; phase = CapturePhase.IDLE; error = true
            LogManager.addLog("ERROR", "Capture", "Photo capture failed: ${e.message}")
        }
    }
    fun startVideo() {
        if (phase != CapturePhase.IDLE || camera == null || !hasAudio) return
        val file = try { File.createTempFile("video_msg_", ".mp4", context.cacheDir) }
        catch (e: Exception) { error = true; LogManager.addLog("ERROR", "Capture", "Video cache failed: ${e.message}"); return }
        resources.own(file)
        output = file; phase = CapturePhase.CAPTURING; elapsed = 0; error = false
        val expectedMime = codec.mime
        var attemptedStart = false
        LogManager.addLog("INFO", "Capture", "Video capture requested camera=${lens?.cameraId} codec=$expectedMime " +
            "resolution=${quality.label} bitrate=$bitrate")
        try {
            videoCapture.targetRotation = captureRotation()
            val pending = videoCapture.output.prepareRecording(context, FileOutputOptions.Builder(file).build()).withAudioEnabled()
            attemptedStart = true
            recording = pending.start(main) { event ->
                    if (event is VideoRecordEvent.Finalize) {
                        if (event.hasError()) LogManager.addLog(if (closed || output != file) "DEBUG" else "ERROR", "Capture",
                            "Video finalize code=${event.error} discarded=${closed || output != file}: ${event.cause}")
                        resources.completed(file)
                    }
                    if (closed || output != file) {
                        if (event is VideoRecordEvent.Finalize) resources.discard(file)
                    } else when (event) {
                        is VideoRecordEvent.Start -> phase = CapturePhase.RECORDING
                        is VideoRecordEvent.Pause -> phase = CapturePhase.PAUSED
                        is VideoRecordEvent.Resume -> phase = CapturePhase.RECORDING
                        is VideoRecordEvent.Status -> {
                            elapsed = event.recordingStats.recordedDurationNanos / 1_000_000
                            if (elapsed >= 60_000 && phase == CapturePhase.RECORDING) {
                                try {
                                    phase = CapturePhase.FINALIZING; checkNotNull(recording).stop()
                                    LogManager.addLog("INFO", "Capture", "Video auto-stop duration=$elapsed")
                                } catch (e: Exception) {
                                    LogManager.addLog("ERROR", "Capture", "Video auto-stop failed: $e"); reset(); error = true
                                }
                            }
                        }
                        is VideoRecordEvent.Finalize -> {
                            recording = null
                            if (event.hasError()) {
                                resources.discard(file); output = null; pendingSend = false; phase = CapturePhase.IDLE; error = true
                            } else {
                                phase = CapturePhase.FINALIZING
                                scope.launch {
                                    try {
                                        val actual = withContext(Dispatchers.IO) {
                                            VideoFileTracks().use { it.verify(file.path, expectedMime) }
                                        }
                                        if (closed || output != file) return@launch
                                        phase = CapturePhase.CAPTURED
                                        LogManager.addLog("INFO", "Capture", "Video verified format=$actual path=${file.path} " +
                                            "bytes=${file.length()} duration=$elapsed requestedBitrate=$bitrate")
                                        if (pendingSend) { pendingSend = false; sendResult() }
                                    } catch (e: kotlinx.coroutines.CancellationException) { throw e }
                                    catch (e: Exception) {
                                        LogManager.addLog("ERROR", "Capture", "Video verification failed path=${file.path}: ${e.message}")
                                        if (!closed && output == file) { reset(); error = true }
                                    }
                                }
                            }
                        }
                        else -> Unit
                    }
                }
            resources.recording(file, checkNotNull(recording))
        } catch (e: Exception) {
            // Документированные precondition failures start не создают writer; прочие ждут Finalize после unbind.
            if (!attemptedStart || e is IllegalStateException || e is SecurityException || e is IllegalArgumentException) resources.completed(file)
            resources.discard(file); resources.unbind()
            recording = null; camera = null; output = null; phase = CapturePhase.IDLE; error = true
            LogManager.addLog("ERROR", "Capture", "Video start failed: ${e.message}")
        }
    }

    val idle = phase == CapturePhase.IDLE
    var menu by remember(mode, lens?.cameraId) { mutableStateOf<CameraOption?>(null) }
    // Кнопки сами переключают меню; фон и Back закрывают его без перехвата нажатий на соседние кнопки.
    val menuProperties = remember { PopupProperties(focusable = false, dismissOnClickOutside = false) }
    BackHandler(menu != null) { menu = null }
    LaunchedEffect(idle) { if (!idle) menu = null }
    LaunchedEffect(menu) { LogManager.addLog("DEBUG", "Capture", "Toolbar menu=$menu mode=$mode camera=${lens?.cameraId}") }
    Scaffold(modifier = Modifier.pointerInput(menu) { if (menu != null) detectTapGestures { menu = null } }, topBar = {
        TopAppBar(title = {
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                CaptureMode.values().forEach { item ->
                    FilterChip(selected = mode == item, enabled = idle, onClick = { mode = item; error = false },
                        label = { Text(stringResource(if (item == CaptureMode.PHOTO) R.string.photo else R.string.capture_video)) })
                }
            }
        }, navigationIcon = {
            IconButton(onClick = { reset(); onClose() }) { Icon(Icons.Default.Close, stringResource(R.string.close)) }
        }, actions = {
            if (camera?.cameraInfo?.hasFlashUnit() == true) IconButton(enabled = idle || phase == CapturePhase.RECORDING,
                onClick = {
                    val next = !torch
                    val selected = camera
                    try {
                        val request = selected?.cameraControl?.enableTorch(next)
                        request?.addListener({
                            try { request.get(); if (!closed && camera === selected) torch = next }
                            catch (e: Exception) {
                                if (!closed && camera === selected) error = true
                                LogManager.addLog("ERROR", "Capture", "Torch failed: $e")
                            }
                        }, main)
                    } catch (e: Exception) { error = true; LogManager.addLog("ERROR", "Capture", "Torch request failed: $e") }
                }) { Icon(if (torch) Icons.Default.FlashOn else Icons.Default.FlashOff, stringResource(R.string.camera_light)) }
        })
    }) { padding ->
        // Настройки, превью и управление занимают отдельные области; текст не накладывается на изображение.
        Column(Modifier.fillMaxSize().background(Color.Black).padding(padding)) {
            Column(Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surface),
                horizontalAlignment = Alignment.CenterHorizontally) {
                if (idle && settingsReady) {
                    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp),
                        horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
                        Box {
                            val name = lens?.let { "${it.label} ${it.mm}".trim() } ?: "—"
                            CaptureControlButton(stringResource(R.string.stream_camera, name), enabled = lenses.isNotEmpty(),
                                onClick = { menu = if (menu == CameraOption.CAMERA) null else CameraOption.CAMERA }) {
                                val focal = lens?.eqFocal?.takeIf { it > 0 }?.let {
                                    stringResource(R.string.stream_camera_focal_short, it.roundToInt())
                                } ?: "—"
                                CaptureValueIcon(CaptureIconShape.CAMERA, focal, front = lens?.type == LensType.SELFIE)
                            }
                            DropdownMenu(expanded = menu == CameraOption.CAMERA, properties = menuProperties,
                                onDismissRequest = { if (menu == CameraOption.CAMERA) menu = null }) {
                                CaptureMenuHeader(stringResource(R.string.camera))
                                lenses.forEach { item ->
                                    CaptureOptionItem("${item.label} ${item.mm}".trim(), lens == item, onClick = {
                                        menu = null; lens = item
                                        if (mode == CaptureMode.PHOTO) rememberPhotoSettings() else rememberVideoSettings()
                                        LogManager.addLog("INFO", "Capture", "Camera selected id=${item.cameraId} mode=$mode")
                                    })
                                }
                            }
                        }
                        CaptureOrientationControl(orientation, displayDegrees, lens?.type == LensType.SELFIE,
                            enabled = true, expanded = menu == CameraOption.ORIENTATION, properties = menuProperties,
                            onExpandedChange = { menu = if (it) CameraOption.ORIENTATION else null }, onChange = {
                                orientation = it
                                if (mode == CaptureMode.PHOTO) rememberPhotoSettings() else rememberVideoSettings()
                                LogManager.addLog("INFO", "Capture", "Orientation selected mode=$mode orientation=$it")
                            })
                        Box {
                            if (mode == CaptureMode.VIDEO) {
                                CaptureControlButton(stringResource(R.string.stream_video_codec, codec.label),
                                    enabled = !videoOptions.isNullOrEmpty(),
                                    onClick = { menu = if (menu == CameraOption.CODEC) null else CameraOption.CODEC }) {
                                    CaptureValueIcon(CaptureIconShape.CHIP, codec.name)
                                }
                                DropdownMenu(expanded = menu == CameraOption.CODEC, properties = menuProperties,
                                    onDismissRequest = { if (menu == CameraOption.CODEC) menu = null }) {
                                    CaptureMenuHeader(stringResource(R.string.call_video_codec))
                                    videoOptions?.forEach { (item, supported) ->
                                        CaptureOptionItem(item.label, codec == item, onClick = {
                                            menu = null; codec = item
                                            if (quality !in supported) {
                                                quality = CaptureResolution.HD.takeIf { it in supported } ?: supported.last()
                                                LogManager.addLog("WARN", "Capture", "Resolution restored for codec=${item.label}: ${quality.label}")
                                            }
                                            rememberVideoSettings()
                                            LogManager.addLog("INFO", "Capture", "Video codec selected ${item.label}")
                                        })
                                    }
                                }
                            }
                        }
                        Box {
                            val label = if (mode == CaptureMode.PHOTO) resolution?.label ?: "—" else quality.label
                            CaptureControlButton(stringResource(R.string.stream_resolution, label),
                                enabled = if (mode == CaptureMode.PHOTO) resolutions.isNotEmpty() else !videoOptions?.get(codec).isNullOrEmpty(),
                                onClick = { menu = if (menu == CameraOption.RESOLUTION) null else CameraOption.RESOLUTION }) {
                                val value = if (mode == CaptureMode.PHOTO) resolution?.let { "${minOf(it.width, it.height)}p" } ?: "—"
                                    else quality.label
                                CaptureValueIcon(CaptureIconShape.SCREEN, value)
                            }
                            DropdownMenu(expanded = menu == CameraOption.RESOLUTION, properties = menuProperties,
                                onDismissRequest = { if (menu == CameraOption.RESOLUTION) menu = null }) {
                                CaptureMenuHeader(stringResource(R.string.call_video_resolution))
                                if (mode == CaptureMode.PHOTO) resolutions.forEach { item ->
                                    CaptureOptionItem(item.label, resolution == item, onClick = {
                                        menu = null; resolution = item
                                        rememberPhotoSettings()
                                        LogManager.addLog("INFO", "Capture", "Photo resolution selected ${item.label}")
                                    })
                                } else videoOptions?.get(codec).orEmpty().forEach { item ->
                                    CaptureOptionItem(item.label, quality == item, onClick = {
                                        menu = null; quality = item
                                        rememberVideoSettings()
                                        LogManager.addLog("INFO", "Capture", "Video resolution selected ${item.label} codec=${codec.label}")
                                    })
                                }
                            }
                        }
                        Box {
                            if (mode == CaptureMode.VIDEO) {
                                val description = stringResource(R.string.stream_bitrate_value, bitrate / 1_000_000.0)
                                CaptureControlButton(stringResource(R.string.stream_bitrate, description),
                                    onClick = { menu = if (menu == CameraOption.BITRATE) null else CameraOption.BITRATE }) {
                                    CaptureValueIcon(CaptureIconShape.GAUGE, captureBitrateLabel(bitrate))
                                }
                                DropdownMenu(expanded = menu == CameraOption.BITRATE, properties = menuProperties,
                                    onDismissRequest = { if (menu == CameraOption.BITRATE) menu = null }) {
                                    CaptureMenuHeader(stringResource(R.string.capture_bitrate_title))
                                    VideoCaptureSettings.BITRATES.forEach { item ->
                                        CaptureOptionItem(stringResource(R.string.stream_bitrate_value, item / 1_000_000.0), bitrate == item,
                                            onClick = {
                                                menu = null; bitrate = item
                                                rememberVideoSettings()
                                                LogManager.addLog("INFO", "Capture", "Video bitrate selected $item resolution=${quality.label}")
                                            })
                                    }
                                }
                            } else {
                                CaptureControlButton(stringResource(R.string.capture_jpeg_quality, jpegQuality),
                                    onClick = { menu = if (menu == CameraOption.JPEG_QUALITY) null else CameraOption.JPEG_QUALITY }) {
                                    CaptureValueIcon(CaptureIconShape.GAUGE, jpegQuality.toString(), detail = "JPEG")
                                }
                                DropdownMenu(expanded = menu == CameraOption.JPEG_QUALITY, properties = menuProperties,
                                    onDismissRequest = { if (menu == CameraOption.JPEG_QUALITY) menu = null }) {
                                    CaptureMenuHeader(stringResource(R.string.capture_jpeg_quality_title))
                                    PhotoCaptureSettings.QUALITIES.forEach { item ->
                                        CaptureOptionItem(item.toString(), jpegQuality == item, onClick = {
                                            menu = null; jpegQuality = item
                                            rememberPhotoSettings()
                                            LogManager.addLog("INFO", "Capture", "JPEG quality selected $item resolution=${resolution?.label}")
                                        })
                                    }
                                }
                            }
                        }
                    }
                }
                if (error) Text(stringResource(R.string.capture_failed), color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(8.dp))
                if (!hasCamera || (mode == CaptureMode.VIDEO && !hasAudio)) Button(onClick = {
                    if (!hasCamera) requestCameraPermission() else requestAudioPermission()
                }) { Text(stringResource(if (!hasCamera) R.string.camera_permission_required else R.string.camera_microphone_permission)) }
            }
            Box(Modifier.weight(1f).fillMaxWidth().clipToBounds().onSizeChanged {
                LogManager.addLog("DEBUG", "Capture", "Preview area mode=$mode size=${it.width}x${it.height} controlsOutside=true")
            }) {
                if (phase == CapturePhase.CAPTURED && mode == CaptureMode.PHOTO) {
                    bitmap?.let { ZoomableImage(it, Modifier.fillMaxSize()) }
                } else AndroidView(factory = { ctx -> PreviewView(ctx).also {
                    it.scaleType = PreviewView.ScaleType.FIT_CENTER; preview.surfaceProvider = it.surfaceProvider
                } }, modifier = Modifier.fillMaxSize())
            }
            Column(Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surface).padding(horizontal = 16.dp, vertical = 12.dp),
                horizontalAlignment = Alignment.CenterHorizontally) {
                val photoBytes = remember(mode, phase, output) {
                    if (mode == CaptureMode.PHOTO && phase == CapturePhase.CAPTURED) output?.length() ?: 0L else 0L
                }
                val info = if (mode == CaptureMode.VIDEO) quality.label
                    else if (phase == CapturePhase.CAPTURED && photoSize.first > 0)
                        "${photoSize.first}×${photoSize.second}\n${Formatter.formatShortFileSize(context, photoBytes)}" else ""
                BoxWithConstraints(Modifier.fillMaxWidth().heightIn(min = 76.dp)) {
                    val captureEnabled = camera != null && (idle || phase == CapturePhase.RECORDING || phase == CapturePhase.PAUSED)
                    val showDiscard = output != null && phase != CapturePhase.SENDING
                    val showSend = output != null &&
                        (phase == CapturePhase.CAPTURED || phase == CapturePhase.RECORDING || phase == CapturePhase.PAUSED)
                    val shutterSize = 76.dp
                    val shutterWidth = if (captureEnabled) shutterSize else shutterSize / 2
                    val controlsWidth = 48.dp * 2 + 24.dp * 2 + shutterWidth
                    val infoWidth = ((maxWidth - controlsWidth) / 2 - 8.dp).coerceIn(0.dp, 80.dp)
                    LaunchedEffect(phase, captureEnabled, showDiscard, showSend, photoBytes, infoWidth) {
                        LogManager.addLog("DEBUG", "Capture", "Controls phase=$phase panel=$maxWidth shutterVisible=$captureEnabled " +
                            "discardVisible=$showDiscard sendVisible=$showSend centerSlot=$shutterWidth jpegBytes=$photoBytes infoMaxWidth=$infoWidth")
                    }
                    Row(Modifier.align(Alignment.Center).heightIn(min = shutterSize), verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                        if (showDiscard) IconButton(onClick = { reset() }) {
                            Icon(Icons.Default.Delete, stringResource(R.string.retake), tint = MaterialTheme.colorScheme.onSurface)
                        } else Spacer(Modifier.width(48.dp))
                        if (captureEnabled) FilledIconButton(modifier = Modifier.size(shutterSize), shape = CircleShape,
                            onClick = {
                                try { when (phase) {
                                    CapturePhase.IDLE -> if (mode == CaptureMode.PHOTO) capturePhoto() else startVideo()
                                    CapturePhase.RECORDING -> checkNotNull(recording).pause()
                                    CapturePhase.PAUSED -> checkNotNull(recording).resume()
                                    else -> Unit
                                } } catch (e: Exception) { error = true; LogManager.addLog("ERROR", "Capture", "Recording control failed phase=$phase: $e") }
                            }) {
                            Icon(if (mode == CaptureMode.PHOTO) Icons.Default.PhotoCamera else if (phase == CapturePhase.RECORDING)
                                Icons.Default.Pause else Icons.Default.Videocam, stringResource(if (mode == CaptureMode.PHOTO)
                                R.string.take_photo else R.string.record_video), modifier = Modifier.size(36.dp))
                        } else Spacer(Modifier.width(shutterWidth))
                        if (showSend) IconButton(onClick = {
                                if (phase == CapturePhase.CAPTURED) sendResult()
                                else {
                                    val previous = phase
                                    try { pendingSend = true; phase = CapturePhase.FINALIZING; checkNotNull(recording).stop() }
                                    catch (e: Exception) {
                                        pendingSend = false; phase = previous; error = true
                                        LogManager.addLog("ERROR", "Capture", "Recording stop failed: $e")
                                    }
                                }
                            }) {
                            Icon(Icons.AutoMirrored.Filled.Send, stringResource(R.string.send), tint = MaterialTheme.colorScheme.onSurface)
                        } else Spacer(Modifier.width(48.dp))
                    }
                    if (info.isNotEmpty()) Column(Modifier.align(Alignment.CenterEnd).widthIn(max = infoWidth),
                        horizontalAlignment = Alignment.End) {
                        if (mode == CaptureMode.VIDEO) Text(
                            "${elapsed / 60_000}:${(elapsed / 1000 % 60).toString().padStart(2, '0')}",
                            color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 11.sp, lineHeight = 13.sp,
                            maxLines = 1, textAlign = TextAlign.End)
                        Text(info, color = MaterialTheme.colorScheme.onSurfaceVariant,
                            fontSize = 13.sp, lineHeight = 15.sp, maxLines = 2, textAlign = TextAlign.End)
                    }
                }
                if (phase == CapturePhase.CAPTURING || phase == CapturePhase.FINALIZING) CircularProgressIndicator(Modifier.size(24.dp))
            }
        }
    }
}
