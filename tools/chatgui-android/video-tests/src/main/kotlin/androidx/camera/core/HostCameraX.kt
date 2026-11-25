package androidx.camera.core

import android.media.Image
import java.util.concurrent.Executor

open class UseCase
class CameraSelector { companion object { val DEFAULT_BACK_CAMERA = CameraSelector() } }
class Preview : UseCase() {
    var surfaceProvider: Any? = null
    class Builder { fun build() = Preview() }
}
class ImageInfo(val rotationDegrees: Int = 0)
class ImageProxy {
    var failures = emptySet<String>()
    val image: Image? get() { check("image" !in failures); return if ("empty" in failures) null else media }
    val media = Image()
    val imageInfo = ImageInfo()
    var closes = 0
    var closed = false
    fun close() {
        closes++; check("close" !in failures) { "ImageProxy close failed" }; closed = true
        check("close after" !in failures) { "ImageProxy close failed after effect" }
    }
}
class ImageAnalysis : UseCase() {
    companion object {
        const val STRATEGY_KEEP_ONLY_LATEST = 0
        val created = mutableListOf<ImageAnalysis>()
        var nextFailures = emptySet<String>()
    }
    var failures = nextFailures
    var analyzer: ((ImageProxy) -> Unit)? = null
    var executor: Executor? = null
    var clears = 0
    class Builder {
        fun setBackpressureStrategy(strategy: Int) = this
        fun build() = ImageAnalysis().also { created.add(it) }
    }
    fun setAnalyzer(delivery: Executor, listener: (ImageProxy) -> Unit) {
        executor = delivery; analyzer = listener; check("set analyzer" !in failures)
    }
    fun clearAnalyzer() { clears++; check("clear" !in failures); analyzer = null }
}
