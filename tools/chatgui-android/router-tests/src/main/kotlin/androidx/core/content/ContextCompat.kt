package androidx.core.content

import android.content.Context
import android.os.Handler
import android.os.Looper
import java.util.concurrent.Executor

/** Только host-тест: разрешение Context и main-очередь, без AndroidX runtime. */
object ContextCompat {
    const val RECEIVER_NOT_EXPORTED = Context.RECEIVER_NOT_EXPORTED
    fun startForegroundService(context: Context, intent: android.content.Intent) = context.startForegroundService(intent)
    fun registerReceiver(context: Context, receiver: android.content.BroadcastReceiver, filter: android.content.IntentFilter, flags: Int) =
        context.registerReceiver(receiver, filter, flags)
    fun checkSelfPermission(context: Context, permission: String): Int = context.checkSelfPermission(permission)
    fun getMainExecutor(context: Context): Executor = Executor { Handler(Looper.getMainLooper()).post(it) }
}
