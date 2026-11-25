package android.view

/** Только дескриптор вывода; реальных EGL/TextureView/драйверов нет. */
class Surface() {
    constructor(texture: android.graphics.SurfaceTexture) : this()
    @Volatile var isValid = true
    var releaseCalls = 0
        private set
    @Volatile var failures = emptySet<String>()
    fun release() {
        releaseCalls++
        check("release" !in failures) { "Surface release failed" }
        isValid = false
        check("release after" !in failures) { "Surface release failed after effect" }
    }
}
