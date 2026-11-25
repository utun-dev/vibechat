package com.utun.chat.headless

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.utun.chat.AppEventHandler
import com.utun.chat.ChatApplication
import com.utun.chat.R
import com.utun.chat.data.AppLanguage
import com.utun.chat.MainActivity
import com.utun.chat.data.LogManager
import com.utun.chat.data.NativeLib
import com.utun.chat.data.NetworkActivityMonitor
import com.utun.chat.data.NetworkChangeMonitor
import com.utun.chat.data.fileStatusResource
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.cancel
import kotlinx.coroutines.withContext

/** Владеет запуском ядра/сетевых наблюдателей; готовность подтверждается событием native SERVICE_STARTED. */
class HeadlessService : Service() {
    private val destroyed = CompletableDeferred<Unit>()
    private var foregroundReady = false
    private val startupScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val startup = CoreStartup(startupScope, NativeLib::isReady, onReady = {
        LogManager.addLog("INFO", "SRV", "uTun core started")
        NetworkActivityMonitor.start()
        NetworkChangeMonitor.onCoreStarted()
    }, onFailure = { error -> AppEventHandler.serviceFailed(error); stopSelf() }, log = { level, message -> LogManager.addLog(level, "SRV", message) })
    companion object {
        private var currentService: HeadlessService? = null
        // Живёт дольше экземпляра Service: новый init не пересекается с очисткой предыдущего worker.
        private val shutdownScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        private val shutdown = CoreShutdown(shutdownScope, NativeLib::stop,
            log = { level, message -> LogManager.addLog(level, "SRV", message) })

        /** Main thread: дождаться очистки прежнего worker и готовности текущего Service; ошибка/остановка прерывает операцию.
         * Без текущего Service настройки сохраняются для следующего запуска. */
        suspend fun awaitStartup() {
            shutdown.await()
            val service = currentService ?: return
            service.startup.awaitReady()
            check(currentService === service && NativeLib.isReady()) { "service core stopped while awaiting startup" }
        }

        fun onCoreStarted() { currentService?.startup?.coreStarted() }
        fun isActive(): Boolean = currentService?.foregroundReady == true
        fun onCoreStopped() { currentService?.startup?.coreStopped() }
        fun onCoreFailed(reason: String) { currentService?.startup?.fail(IllegalStateException(reason)) }
        fun onCoreRestarting() { currentService?.startup?.restarting() }

        /** Main: остановить также владельца Service и дождаться onDestroy + join native worker перед обслуживанием профиля. */
        suspend fun stopAndJoin() {
            currentService?.let { service -> service.stopSelf(); service.destroyed.await() }
            shutdown.await()
        }

        fun refreshLanguage() {
            currentService?.let { service ->
                try {
                    if (!service.foregroundReady) return
                    service.createNotificationChannel()
                    service.getSystemService(NotificationManager::class.java).notify(1, service.notification())
                } catch (e: Exception) { LogManager.addLog("ERROR", "SRV", "Service notification language update failed: $e") }
            }
        }
        fun refreshFiles() {
            currentService?.let { service ->
                try {
                    if (service.foregroundReady) service.getSystemService(NotificationManager::class.java).notify(1, service.notification())
                } catch (e: Exception) { LogManager.addLog("ERROR", "SRV", "Service file notification update failed: $e") }
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        currentService = this
        try {
            createNotificationChannel()
            startForeground(1, notification())
            foregroundReady = true
            LogManager.addLog("INFO", "SRV", "Core service foreground confirmed")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "SRV", "Core service foreground failed: $e")
            startup.fail(e)
            return
        }

        var startupDirectory = filesDir.absolutePath
        startup.start(prepare = {
            shutdown.await()
            val provider = ChatApplication.instance.configProvider
            val dbDir = withContext(Dispatchers.IO) { provider.getString("db.path") ?: filesDir.absolutePath }
            LogManager.addLog("INFO", "SRV", "uTun service starting, db=$dbDir; network ports come from configured interfaces")
            startupDirectory = dbDir
            check(NetworkChangeMonitor.start()) { "network monitoring unavailable" }
            provider.buildConfigText().await()
        }, submit = { config -> NativeLib.start(startupDirectory, 9999, config) })
    }

    override fun onDestroy() {
        foregroundReady = false
        if (currentService === this) currentService = null
        LogManager.addLog("INFO", "SRV", "uTun service stopping")
        try { startup.stop() } catch (e: Exception) { LogManager.addLog("ERROR", "SRV", "Startup cancellation failed: $e") }
        try { startupScope.cancel() } catch (e: Exception) { LogManager.addLog("ERROR", "SRV", "Startup scope cancellation failed: $e") }
        try { NetworkChangeMonitor.stop() } catch (e: Exception) { LogManager.addLog("ERROR", "SRV", "Network observer stop failed: $e") }
        try { NetworkActivityMonitor.stop() } catch (e: Exception) { LogManager.addLog("ERROR", "SRV", "Network activity stop failed: $e") }
        try { shutdown.request() } finally { destroyed.complete(Unit); super.onDestroy() }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (!foregroundReady) {
            LogManager.addLog("ERROR", "SRV", "Service command rejected before foreground confirmation startId=$startId")
            return START_NOT_STICKY
        }
        if (intent?.action == "com.utun.chat.CANCEL_FILES") {
            ChatApplication.instance.memberFiles.cancel()
            return START_STICKY
        }
        val source = if (intent == null) "sticky_restart" else intent.action ?: "manual"
        LogManager.addLog("INFO", "SRV", "Service start command: source=$source startId=$startId flags=$flags ready=${NativeLib.isReady()}")
        return START_STICKY
    }

    private fun notification(): android.app.Notification {
        val operation = ChatApplication.instance.memberFiles.operation.value
        val intent = Intent(this, MainActivity::class.java)
        operation?.let { op ->
            intent.putExtra("files_group", op.location.group).putExtra("files_server", op.location.server)
                .putExtra("files_owner", op.location.owner).putExtra("files_path", op.location.path)
        }
        val pendingIntent = PendingIntent.getActivity(this, 0, intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val builder = NotificationCompat.Builder(AppLanguage.context(), "utun_channel")
            .setContentTitle("vibechat")
            .setContentText(AppLanguage.text(R.string.service_notification))
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
        if (operation != null) {
            val detail = operation.status?.let { AppLanguage.text(fileStatusResource(it)) } ?: AppLanguage.text(operation.stage)
            builder.setContentText("${operation.name}: $detail")
            if (operation.active) {
                val percent = if (operation.total <= 0L) 0 else (operation.done.toDouble() * 100 / operation.total).toInt().coerceIn(0, 100)
                builder.setProgress(100, percent, operation.total == 0L)
                val cancel = PendingIntent.getService(this, 21, Intent(this, HeadlessService::class.java)
                    .setAction("com.utun.chat.CANCEL_FILES"), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
                builder.addAction(android.R.drawable.ic_menu_close_clear_cancel, AppLanguage.text(R.string.cancel), cancel)
            }
        }
        return builder.build()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                "utun_channel", "vibechat",
                NotificationManager.IMPORTANCE_LOW
            ).apply { description = AppLanguage.text(R.string.service_description) }
            getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }
}
