package android.app

import android.content.Context
import android.content.Intent
import android.os.IBinder

/** OS lifecycle контролирует тест: stopSelf лишь запрашивает stop, onDestroy доставляется отдельно. */
open class Service : Context() {
    open fun onConfigurationChanged(newConfig: android.content.res.Configuration) {}
    companion object { const val START_NOT_STICKY = 2; const val STOP_FOREGROUND_REMOVE = 1 }
    var foregroundId = 0
    var foregroundTypes = 0
    var foregroundAttempts = 0
    var failForeground = false
    var failForegroundAfter = false
    var stops = 0
    var foregroundStops = 0
    var failForegroundStop = false
    var failStopSelf = false
    var destroys = 0
    open fun onCreate() {}
    open fun onStartCommand(intent: Intent?, flags: Int, startId: Int) = START_NOT_STICKY
    open fun onDestroy() { destroys++ }
    open fun onBind(intent: Intent?): IBinder? = null
    fun startForeground(id: Int, notification: Notification) {
        foregroundAttempts++
        check(!failForeground) { "foreground promotion unavailable" }
        foregroundId = id
        requireNotNull(notificationManager).notify(id, notification)
        check(!failForegroundAfter) { "foreground promotion failed after effect" }
    }
    fun stopForeground(flags: Int) {
        foregroundStops++
        check(!failForegroundStop) { "foreground removal unavailable" }
        if (flags == STOP_FOREGROUND_REMOVE && foregroundId != 0) notificationManager?.cancel(foregroundId)
    }
    fun stopSelf() { stops++; check(!failStopSelf) { "service stop unavailable" } }
    fun stopSelf(startId: Int) { stopSelf() }
}
