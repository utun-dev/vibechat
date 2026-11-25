package android.content

import android.net.Uri

data class ComponentName(val packageName: String, val className: String)

/** Host Intent: копируются extras, а identity (как filterEquals) их не включает. */
class Intent() {
    companion object {
        const val FLAG_ACTIVITY_NEW_TASK = 0x10000000
        const val FLAG_ACTIVITY_CLEAR_TOP = 0x04000000
        const val FLAG_ACTIVITY_SINGLE_TOP = 0x20000000
        const val ACTION_SCREEN_ON = "android.intent.action.SCREEN_ON"
        const val ACTION_SCREEN_OFF = "android.intent.action.SCREEN_OFF"
    }
    var action: String? = null
    var data: Uri? = null
    var component: Class<*>? = null
    var flags = 0
    private val extras = mutableMapOf<String, Any>()
    constructor(context: Context, target: Class<*>) : this() { component = target }
    constructor(action: String) : this() { this.action = action }
    constructor(other: Intent) : this() {
        action = other.action; data = other.data; component = other.component; flags = other.flags
        extras.putAll(other.extras)
    }
    fun addFlags(value: Int) = apply { flags = flags or value }
    fun setAction(value: String) = apply { action = value }
    fun setData(value: Uri) = apply { data = value }
    fun putExtra(name: String, value: Any) = apply { extras[name] = value }
    fun getLongExtra(name: String, default: Long): Long = extras[name] as? Long ?: default
    fun getBooleanExtra(name: String, default: Boolean): Boolean = extras[name] as? Boolean ?: default
    fun getStringExtra(name: String): String? = extras[name] as? String
    fun removeExtra(name: String) { extras.remove(name) }
}

abstract class BroadcastReceiver { abstract fun onReceive(context: Context, intent: Intent) }

class IntentFilter {
    val actions = mutableSetOf<String>()
    fun addAction(action: String) { actions.add(action) }
}
