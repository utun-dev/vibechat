package android.media

import android.os.Handler
import android.view.Surface
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CopyOnWriteArrayList

class Image(val timestamp: Long = System.nanoTime()) {
    var failures = emptySet<String>()
    var closeCalls = 0
        private set
    var closed = false
        private set
    fun close() {
        closeCalls++; check("close" !in failures) { "Image close failed" }; closed = true
        check("close after" !in failures) { "Image close failed after effect" }
    }
}

/** Camera2 output без графических буферов; reader/Surface остаются наблюдаемыми при отказе close. */
class ImageReader private constructor(val width: Int, val height: Int) {
    companion object {
        val created = CopyOnWriteArrayList<ImageReader>()
        var nextFailures = emptySet<String>()
        fun newInstance(width: Int, height: Int, format: Int, count: Int) = ImageReader(width, height).also { created.add(it) }
    }
    fun interface OnImageAvailableListener { fun onImageAvailable(source: ImageReader) }
    val surface = Surface()
    private val frames = ConcurrentLinkedQueue<Image>()
    private var listener: OnImageAvailableListener? = null
    private var handler: Handler? = null
    var failures = nextFailures
    var closed = false
        private set
    var closeCalls = 0
        private set
    fun setOnImageAvailableListener(value: OnImageAvailableListener, delivery: Handler) {
        check("listener" !in failures); listener = value; handler = delivery; check("listener after" !in failures)
    }
    fun acquireLatestImage(): Image? {
        check(!closed)
        var last = frames.poll()
        while (true) { val next = frames.poll() ?: return last; last?.close(); last = next }
    }
    fun push(image: Image) { frames.add(image); handler?.post { listener?.onImageAvailable(this) } }
    fun close() {
        closeCalls++; check("close" !in failures) { "reader close failed" }
        if (!closed) { closed = true; surface.release() }
        check("close after" !in failures) { "reader close failed after effect" }
    }
}
