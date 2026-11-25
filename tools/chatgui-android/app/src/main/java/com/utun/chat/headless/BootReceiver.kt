package com.utun.chat.headless

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat
import com.utun.chat.ChatApplication
import com.utun.chat.data.LogManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

/** BOOT_COMPLETED после разблокировки: goAsync удерживает broadcast до чтения DataStore и запроса FGS.
 * shouldStartOnBoot требует завершённого setup, включённых service/autostart; finally вызывает finish независимо от исхода.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) {
            LogManager.addLog("WARN", "BOOT", "Unexpected boot receiver action=${intent.action}")
            return
        }
        LogManager.addLog("INFO", "BOOT", "Boot completed broadcast received")
        val pending = goAsync()
        CoroutineScope(Dispatchers.IO).launch {
            try {
                if (!ChatApplication.instance.configProvider.shouldStartOnBoot()) return@launch
                val appContext = context.applicationContext
                val serviceIntent = Intent(appContext, HeadlessService::class.java).setAction(Intent.ACTION_BOOT_COMPLETED)
                checkNotNull(ContextCompat.startForegroundService(appContext, serviceIntent)) { "Boot foreground service not found" }
                LogManager.addLog("INFO", "BOOT", "Boot service start requested: service=${HeadlessService::class.java.name}")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "BOOT", "Boot service start failed: $e")
            } finally {
                try { pending.finish() } catch (e: Exception) { LogManager.addLog("ERROR", "BOOT", "Boot broadcast finish failed: $e") }
            }
        }
    }
}
