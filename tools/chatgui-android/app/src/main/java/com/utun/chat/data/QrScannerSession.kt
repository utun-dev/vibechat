package com.utun.chat.data

import android.content.Context
import android.os.Handler
import android.os.Looper
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner
import com.google.android.gms.tasks.Task
import com.google.mlkit.vision.barcode.BarcodeScanner
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage
import java.util.concurrent.Executor
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/** Пассивный владелец CameraX/ML Kit: UI сначала сохраняет объект, затем вызывает start на main.
 * onScan вызывается на main; true означает принятую ссылку и завершает сканирование. close идемпотентен.
 * ImageProxy живёт до завершения Task; ошибки cleanup сохраняют владельцев с retry 250 мс..30 с даже после ухода UI.
 * CameraProvider общий: снимаются только два use case этой сессии. Монитор защищает process от одновременного close scanner. */
internal class QrScannerSession(private val context: Context, private val lifecycle: LifecycleOwner,
                                private val view: PreviewView, onScan: (List<String>) -> Boolean,
                                onError: (Failure) -> Unit) : AutoCloseable {
    enum class Failure { CAMERA, SCAN }
    companion object {
        private val retained = java.util.concurrent.ConcurrentHashMap.newKeySet<QrScannerSession>()
        private val direct = Executor { it.run() }
    }
    private class Frame(val image: ImageProxy) {
        var task: Task<List<Barcode>>? = null
        var completed = false
    }
    private val main = Handler(Looper.getMainLooper())
    private var resultCallback: ((List<String>) -> Boolean)? = onScan
    private var errorCallback: ((Failure) -> Unit)? = onError
    private var scanner: BarcodeScanner? = null
    private var executor: ExecutorService? = null
    private var preview: Preview? = null
    private var analysis: ImageAnalysis? = null
    private var provider: ProcessCameraProvider? = null
    private var analyzerInstalled = false
    private var binding = false
    private var started = false
    private var closed = false
    private val frames = mutableSetOf<Frame>()
    private var retryDelay = 250L
    private var scheduled = false
    private val retry = Runnable { synchronized(this) { scheduled = false; cleanup() } }

    @Synchronized fun start() {
        check(Looper.myLooper() == Looper.getMainLooper()) { "QR start requires main" }
        check(!started && !closed) { "QR session already started/closed" }
        started = true
        try {
            executor = Executors.newSingleThreadExecutor()
            scanner = BarcodeScanning.getClient(BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build())
            preview = Preview.Builder().build()
            preview!!.surfaceProvider = view.surfaceProvider
            analysis = ImageAnalysis.Builder().setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST).build()
            analyzerInstalled = true
            analysis!!.setAnalyzer(executor!!, ::analyze)
            val future = ProcessCameraProvider.getInstance(context)
            future.addListener({ synchronized(this) {
                if (!closed) try {
                    provider = future.get()
                    binding = true
                    provider!!.bindToLifecycle(lifecycle, CameraSelector.DEFAULT_BACK_CAMERA, preview!!, analysis!!)
                    LogManager.addLog("INFO", "QR", "Camera started session=${System.identityHashCode(this)}")
                } catch (e: Exception) { fail(Failure.CAMERA, "Camera binding failed", e) }
            } }, ContextCompat.getMainExecutor(context))
        } catch (e: Exception) { fail(Failure.CAMERA, "Camera initialization failed", e) }
    }

    @Synchronized private fun analyze(image: ImageProxy) {
        val frame = Frame(image)
        frames.add(frame)
        if (closed) { frame.completed = true; closeFrame(frame); scheduleIfNeeded(); return }
        try {
            val media = image.image
            if (media == null) {
                LogManager.addLog("WARN", "QR", "Camera delivered an empty image")
                frame.completed = true; closeFrame(frame); scheduleIfNeeded(); return
            }
            val task = scanner!!.process(InputImage.fromMediaImage(media, image.imageInfo.rotationDegrees))
            frame.task = task // До регистрации listener: исключение регистрации не отменяет работу ML Kit.
            task.addOnCompleteListener(direct) { complete(frame, it) }
        } catch (e: Exception) {
            if (frame.task == null) { frame.completed = true; closeFrame(frame) }
            post { fail(Failure.SCAN, "Image submission/listener failed", e) }
            scheduleIfNeeded()
        }
    }

    @Synchronized private fun complete(frame: Frame, task: Task<List<Barcode>>) {
        if (frame.completed) return
        frame.completed = true
        closeFrame(frame)
        scheduleIfNeeded()
        post {
            synchronized(this) {
                if (!closed) try {
                    if (task.isCanceled) error("QR recognition canceled")
                    if (!task.isSuccessful) throw task.exception ?: IllegalStateException("QR recognition failed without a cause")
                    if (resultCallback?.invoke(task.result.mapNotNull { it.rawValue }) == true) {
                        LogManager.addLog("INFO", "QR", "Invite accepted session=${System.identityHashCode(this)}")
                        close()
                    }
                } catch (e: Exception) { fail(Failure.SCAN, "QR recognition/result callback failed", e) }
                if (closed) cleanup()
            }
        }
    }

    /** Main: ошибка прекращает захват, но сообщение доставляется до удаления UI callbacks. */
    private fun fail(kind: Failure, message: String, error: Exception) {
        LogManager.addLog("ERROR", "QR", "$message session=${System.identityHashCode(this)}: $error")
        val notify = errorCallback
        close()
        if (notify != null) try { notify(kind) } catch (e: Exception) {
            LogManager.addLog("ERROR", "QR", "Error callback failed: $e")
        }
    }

    @Synchronized private fun post(action: () -> Unit) {
        if (!main.post { action() }) {
            LogManager.addLog("ERROR", "QR", "Main callback rejected; session retained")
            closed = true; resultCallback = null; errorCallback = null
            scheduleIfNeeded()
        }
    }

    @Synchronized override fun close() {
        check(Looper.myLooper() == Looper.getMainLooper()) { "QR close requires main" }
        if (!closed) retained.add(this)
        closed = true
        resultCallback = null; errorCallback = null
        cleanup()
    }

    private fun closeFrame(frame: Frame) {
        try { frame.image.close(); frames.remove(frame) } catch (e: Exception) {
            LogManager.addLog("ERROR", "QR", "Image close failed session=${System.identityHashCode(this)}; retained: $e")
        }
    }

    /** Вызывается под монитором; очистка CameraX/scanner только на main, closeFrame допустим на completion thread. */
    private fun cleanup() {
        if (closed) {
            if (analyzerInstalled) try { analysis!!.clearAnalyzer(); analyzerInstalled = false } catch (e: Exception) {
                LogManager.addLog("ERROR", "QR", "Analyzer removal failed; retained: $e")
            }
            if (binding) try { provider!!.unbind(preview!!, analysis!!); binding = false; provider = null } catch (e: Exception) {
                LogManager.addLog("ERROR", "QR", "Camera unbind failed; retained: $e")
            }
            if (!binding && !analyzerInstalled) { preview = null; analysis = null }
            if (executor != null) try { executor!!.shutdown(); executor = null } catch (e: Exception) {
                LogManager.addLog("ERROR", "QR", "Analyzer executor shutdown failed; retained: $e")
            }
        }
        for (frame in frames.toList()) {
            if (frame.completed || (closed && (frame.task == null || frame.task!!.isComplete))) {
                frame.completed = true; closeFrame(frame)
            }
        }
        if (closed && frames.isEmpty() && scanner != null) try { scanner!!.close(); scanner = null } catch (e: Exception) {
            LogManager.addLog("ERROR", "QR", "Scanner close failed; retained: $e")
        }
        scheduleIfNeeded()
    }

    private fun scheduleIfNeeded() {
        val pending = if (closed) analyzerInstalled || binding || scanner != null || executor != null || frames.isNotEmpty()
            else frames.any { it.completed }
        if (!pending) {
            main.removeCallbacks(retry); scheduled = false
            if (retained.remove(this) && closed) LogManager.addLog("INFO", "QR", "Camera/scanner released session=${System.identityHashCode(this)}")
            return
        }
        retained.add(this)
        if (scheduled) return
        scheduled = main.postDelayed(retry, retryDelay)
        if (!scheduled) LogManager.addLog("ERROR", "QR", "Cleanup retry rejected; owner retained")
        else retryDelay = (retryDelay * 2).coerceAtMost(30_000)
    }
}
