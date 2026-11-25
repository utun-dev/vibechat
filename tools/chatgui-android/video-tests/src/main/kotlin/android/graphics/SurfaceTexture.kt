package android.graphics

/** Только JVM: физический texture descriptor и отказ до/после release. */
class SurfaceTexture(val id: Int) {
    var released = false
        private set
    var releaseCalls = 0
        private set
    var failures = emptySet<String>()
    fun setDefaultBufferSize(width: Int, height: Int) { check("buffer" !in failures && width > 0 && height > 0) }
    fun release() {
        releaseCalls++; check("release" !in failures) { "texture release failed" }
        released = true; check("release after" !in failures) { "texture release failed after effect" }
    }
}
