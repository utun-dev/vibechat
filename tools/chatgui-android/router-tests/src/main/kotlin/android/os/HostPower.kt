package android.os

/** Host wake lock: тест различает отказ до acquire, после побочного эффекта и освобождение по timeout. */
class PowerManager {
    companion object {
        const val PARTIAL_WAKE_LOCK = 1; const val SCREEN_BRIGHT_WAKE_LOCK = 10
        const val ACQUIRE_CAUSES_WAKEUP = 0x10000000; const val ON_AFTER_RELEASE = 0x20000000
        const val ACTION_DEVICE_IDLE_MODE_CHANGED = "android.os.action.DEVICE_IDLE_MODE_CHANGED"
        const val ACTION_POWER_SAVE_MODE_CHANGED = "android.os.action.POWER_SAVE_MODE_CHANGED"
    }
    var failures = emptySet<String>()
    var isInteractive = true
    var isDeviceIdleMode = false
    var isPowerSaveMode = false
    var ignoringBatteryOptimizations = false
    val optimizationQueries = mutableListOf<String>()
    val locks = mutableListOf<WakeLock>()
    fun isIgnoringBatteryOptimizations(packageName: String): Boolean {
        optimizationQueries.add(packageName)
        check("optimization_query" !in failures) { "battery optimization query unavailable" }
        return ignoringBatteryOptimizations
    }
    fun newWakeLock(flags: Int, tag: String): WakeLock {
        check("create" !in failures) { "wake lock creation unavailable" }
        return WakeLock(flags, tag, failures).also { locks.add(it) }
    }
    class WakeLock(val flags: Int, val tag: String, var failures: Set<String>) {
        var counted = true
            private set
        var timeout = 0L
        var releaseCalls = 0
        var held = false
        val isHeld: Boolean get() { check("query" !in failures) { "wake lock query unavailable" }; return held }
        fun setReferenceCounted(value: Boolean) {
            check("configure" !in failures) { "wake lock configure unavailable" }
            counted = value
        }
        fun acquire() = acquire(0)
        fun acquire(timeout: Long) {
            this.timeout = timeout
            check("acquire_before" !in failures) { "wake lock acquire before effect" }
            held = true
            check("acquire" !in failures) { "wake lock acquire unavailable" }
        }
        fun release() {
            releaseCalls++
            check("release" !in failures) { "wake lock release unavailable" }
            held = false
            check("release_after" !in failures) { "wake lock release failed after effect" }
        }
    }
}
