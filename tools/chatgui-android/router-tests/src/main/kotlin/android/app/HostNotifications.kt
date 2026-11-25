package android.app

import android.content.Context
import android.content.Intent
import android.net.Uri

class Notification {
    companion object { const val VISIBILITY_PUBLIC = 1 }
    data class Action(val title: String, val intent: PendingIntent)
    var channelId = ""
    var title = ""
    var text = ""
    var category = ""
    var priority = 0
    var ongoing = false
    var autoCancel = false
    var onlyAlertOnce = false
    var contentIntent: PendingIntent? = null
    var fullScreenIntent: PendingIntent? = null
    var bigText: String? = null
    var visibility = 0; var foregroundBehavior = 0; var showWhen = false; var chronometer = false; var whenMs = 0L
    var person: androidx.core.app.Person? = null
    var hangup: PendingIntent? = null
    val actions = mutableListOf<Action>()
}

class NotificationChannel(val id: String, val name: String, val importance: Int) {
    var lockscreenVisibility = 0
    var description = ""
    var vibration = true
    var silent = false
    fun enableVibration(enabled: Boolean) { vibration = enabled }
    fun setSound(sound: Uri?, attributes: Any?) { silent = sound == null }
}

/** Host OS boundaries: отказы канала, разрешений и публикации независимы. */
class NotificationManager {
    companion object { const val IMPORTANCE_HIGH = 4; const val IMPORTANCE_LOW = 2; const val IMPORTANCE_DEFAULT = 3 }
    var failures = emptySet<String>()
    var notificationsEnabled = true
    var fullScreenAllowed = true
    var channelAttempts = 0
    var fullScreenQueries = 0
    val channels = mutableMapOf<String, NotificationChannel>()
    val active = mutableMapOf<Int, Notification>()
    val posted = mutableListOf<Notification>()
    val cancellations = mutableListOf<Int>()
    fun createNotificationChannel(channel: NotificationChannel) {
        channelAttempts++
        check("channel" !in failures) { "channel unavailable" }
        channels[channel.id] = channel
    }
    fun getNotificationChannel(id: String): NotificationChannel? {
        check("channel query" !in failures) { "channel query unavailable" }
        return channels[id]
    }
    fun canUseFullScreenIntent(): Boolean {
        fullScreenQueries++
        check("full screen" !in failures) { "full-screen service unavailable" }
        return fullScreenAllowed
    }
    fun areNotificationsEnabled(): Boolean {
        check("enabled" !in failures) { "notification permission query unavailable" }
        return notificationsEnabled
    }
    fun notify(id: Int, notification: Notification) {
        check("notify" !in failures) { "notification service unavailable" }
        if ("security" in failures) throw SecurityException("notify denied")
        active[id] = notification
        posted.add(notification)
        check("notify_after" !in failures) { "notification publish failed after effect" }
    }
    fun cancel(id: Int) {
        cancellations.add(id)
        check("cancel" !in failures) { "notification cancel unavailable" }
        active.remove(id)
        check("cancel_after" !in failures) { "notification cancel failed after effect" }
    }
}

/** Реестр OS tokens: UPDATE_CURRENT меняет extras существующего token, отменённый token не доставляется. */
class PendingIntent private constructor(val kind: String, val requestCode: Int, intent: Intent, val flags: Int) {
    companion object {
        const val FLAG_IMMUTABLE = 0x04000000
        const val FLAG_UPDATE_CURRENT = 0x08000000
        private data class Key(val kind: String, val request: Int, val component: Class<*>?, val action: String?, val data: Uri?)
        private val tokens = mutableMapOf<Key, PendingIntent>()
        val created = mutableListOf<PendingIntent>()
        var creationAttempts = 0
        var failCreationAt = 0
        fun reset() { tokens.clear(); created.clear(); creationAttempts = 0; failCreationAt = 0 }
        private fun obtain(kind: String, request: Int, intent: Intent, flags: Int): PendingIntent {
            creationAttempts++
            check(creationAttempts != failCreationAt) { "pending intent creation unavailable" }
            check(flags and FLAG_IMMUTABLE != 0)
            val key = Key(kind, request, intent.component, intent.action, intent.data)
            val old = tokens[key]?.takeUnless { it.cancelled }
            if (old != null) {
                if (flags and FLAG_UPDATE_CURRENT != 0) old.intent = Intent(intent)
                return old
            }
            return PendingIntent(kind, request, intent, flags).also { tokens[key] = it; created.add(it) }
        }
        fun getActivity(context: Context, requestCode: Int, intent: Intent, flags: Int) = obtain("activity", requestCode, intent, flags)
        fun getBroadcast(context: Context, requestCode: Int, intent: Intent, flags: Int) = obtain("broadcast", requestCode, intent, flags)
        fun getService(context: Context, requestCode: Int, intent: Intent, flags: Int) = obtain("service", requestCode, intent, flags)
    }
    private var intent = Intent(intent)
    var failCancel = false
    var failCancelAfter = false
    var cancelled = false
        private set
    var cancelCalls = 0
        private set
    fun send(): Intent { check(!cancelled) { "token cancelled" }; return Intent(intent) }
    fun cancel() {
        cancelCalls++
        check(!failCancel) { "pending intent cancel unavailable" }
        cancelled = true
        check(!failCancelAfter) { "pending intent cancel failed after effect" }
    }
}
