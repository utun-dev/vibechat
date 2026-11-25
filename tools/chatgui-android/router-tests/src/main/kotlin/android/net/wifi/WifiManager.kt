package android.net.wifi

/** Host-модель MulticastLock: отказы до/после побочного эффекта и проверка повторного использования владельца. */
class WifiManager {
    var failures = emptySet<String>()
    val locks = mutableListOf<MulticastLock>()
    fun createMulticastLock(tag: String): MulticastLock? {
        check("create" !in failures) { "multicast create unavailable" }
        if ("null_create" in failures) return null
        return MulticastLock(tag, failures).also { locks.add(it) }
    }
    class MulticastLock(val tag: String, var failures: Set<String>) {
        var held = false
        var counted = true
            private set
        var acquisitions = 0
        var releases = 0
        val isHeld: Boolean get() { check("query" !in failures) { "multicast query unavailable" }; return held }
        fun setReferenceCounted(value: Boolean) {
            check("configure" !in failures) { "multicast configure unavailable" }
            counted = value
        }
        fun acquire() {
            acquisitions++
            check("acquire_before" !in failures) { "multicast acquire before effect" }
            held = true
            check("acquire_after" !in failures) { "multicast acquire after effect" }
        }
        fun release() {
            releases++
            check("release_before" !in failures) { "multicast release before effect" }
            held = false
            check("release_after" !in failures) { "multicast release after effect" }
        }
    }
}
