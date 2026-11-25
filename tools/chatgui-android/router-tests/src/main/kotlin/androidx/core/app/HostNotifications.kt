package androidx.core.app

import android.app.Notification
import android.app.PendingIntent
import android.content.Context

/** Host Builder сохраняет параметры настоящего IncomingCallManager без Android notification runtime. */
object NotificationCompat {
    const val CATEGORY_CALL = "call"
    const val PRIORITY_MAX = 2
    const val VISIBILITY_PUBLIC = 1
    const val FOREGROUND_SERVICE_IMMEDIATE = 1
    var failBuild = false
    class BigTextStyle {
        var text = ""
        fun bigText(value: String) = apply { text = value }
    }
    class CallStyle(val person: Person, val hangup: PendingIntent) {
        companion object { fun forOngoingCall(person: Person, hangup: PendingIntent) = CallStyle(person, hangup) }
    }
    class Builder(context: Context, channelId: String) {
        private val notification = Notification().apply { this.channelId = channelId }
        fun setSmallIcon(value: Int) = this
        fun setContentTitle(value: String) = apply { notification.title = value }
        fun setContentText(value: String) = apply { notification.text = value }
        fun setCategory(value: String) = apply { notification.category = value }
        fun setPriority(value: Int) = apply { notification.priority = value }
        fun setOngoing(value: Boolean) = apply { notification.ongoing = value }
        fun setAutoCancel(value: Boolean) = apply { notification.autoCancel = value }
        fun setOnlyAlertOnce(value: Boolean) = apply { notification.onlyAlertOnce = value }
        fun setContentIntent(value: PendingIntent) = apply { notification.contentIntent = value }
        fun setFullScreenIntent(value: PendingIntent, priority: Boolean) = apply { check(priority); notification.fullScreenIntent = value }
        fun setStyle(value: BigTextStyle) = apply { notification.bigText = value.text }
        fun setStyle(value: CallStyle) = apply { notification.person = value.person; notification.hangup = value.hangup }
        fun setVisibility(value: Int) = apply { notification.visibility = value }
        fun setForegroundServiceBehavior(value: Int) = apply { notification.foregroundBehavior = value }
        fun addPerson(value: Person) = this
        fun setShowWhen(value: Boolean) = apply { notification.showWhen = value }
        fun setUsesChronometer(value: Boolean) = apply { notification.chronometer = value }
        fun setWhen(value: Long) = apply { notification.whenMs = value }
        fun addAction(icon: Int, title: String, intent: PendingIntent) = apply { notification.actions.add(Notification.Action(title, intent)) }
        fun build(): Notification { check(!failBuild) { "notification build unavailable" }; return notification }
    }
}

class Person private constructor(val name: String, val key: String) {
    class Builder {
        private var name = ""; private var key = ""
        fun setName(value: String) = apply { name = value }
        fun setKey(value: String) = apply { key = value }
        fun build() = Person(name, key)
    }
}

class NotificationManagerCompat private constructor(private val context: Context) {
    companion object { fun from(context: Context) = NotificationManagerCompat(context) }
    private val manager get() = requireNotNull(context.notificationManager) { "notification manager unavailable" }
    fun areNotificationsEnabled() = manager.areNotificationsEnabled()
    fun notify(id: Int, notification: Notification) = manager.notify(id, notification)
    fun cancel(id: Int) = manager.cancel(id)
}
