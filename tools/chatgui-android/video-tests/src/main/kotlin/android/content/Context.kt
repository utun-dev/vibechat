package android.content

open class Context {
    companion object { const val TELECOM_SERVICE = "telecom" }
    fun <T> getSystemService(type: Class<T>): T = type.cast(android.hardware.camera2.CameraManager.shared)
    fun getSystemService(name: String): Any? {
        check(!android.telecom.TelecomManager.failLookup) { "Telecom lookup failed" }
        return android.telecom.TelecomManager.shared
    }
}
data class ComponentName(val context: Context, val target: Class<*>)
