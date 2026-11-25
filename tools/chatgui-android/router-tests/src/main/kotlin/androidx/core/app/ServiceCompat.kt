package androidx.core.app

import android.app.Notification
import android.app.Service

object ServiceCompat {
    fun startForeground(service: Service, id: Int, notification: Notification, types: Int) {
        service.foregroundTypes = types
        service.startForeground(id, notification)
    }
}
