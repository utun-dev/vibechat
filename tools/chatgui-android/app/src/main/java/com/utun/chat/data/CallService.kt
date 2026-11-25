package com.utun.chat.data

import com.utun.chat.R
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.BroadcastReceiver
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Build
import android.os.IBinder
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import androidx.core.app.Person
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import com.utun.chat.ChatApplication
import com.utun.chat.MainActivity
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Deferred

/**
 * Владелец аудиодвижка и PARTIAL_WAKE_LOCK на время исходящего/принятого звонка.
 * Сервис запускается действием пользователя до сигналинга и захвата микрофона.
 * Не восстанавливает звонок после смерти процесса: native-сессия тогда тоже утрачена.
 * Все изменения — main. Владельцы IPC сохраняются до вызова API и снимаются только после подтверждения.
 * Shutdown возвращает Deferred: video.stop=true подтверждает RX/TX, SDK callbacks и native close.
 * Пока video.stop=false, сервис удерживает engine; отказы всех владельцев независимо повторяются через Handler.
 * До подтверждения очистки process-ссылка удерживает сервис даже после onDestroy; другое media/core stop запрещены.
 */
class CallService : Service() {
    companion object {
        // У старого low-канала Android не позволяет повысить importance после создания.
        private const val CHANNEL = "ongoing_call_lockscreen"
        private const val NOTIFICATION_ID = 1002
        private const val EXTRA_ID = "call_id"
        private const val ACTION_END = "com.utun.chat.action.END_CALL"
        @Volatile private var pendingCleanup: CallService? = null // Сильная ссылка до подтверждённой очистки, включая onDestroy.
        /** Завершение сохраняемого владельца; await разрешён в coroutine, отмена Deferred не подтверждает очистку. */
        val pendingRelease: Deferred<Unit>? get() = pendingCleanup?.released
        /** Потокобезопасный запрет другого медиа, пока есть неподтверждённые владельцы. */
        fun cleanupPending(): Boolean = pendingCleanup != null

        /** На main повторить незавершённую очистку. Ошибка запрещает заменять владельца или останавливать ядро. */
        fun requireReleased() {
            checkMain()
            pendingCleanup?.shutdown()
            TelecomCallManager.requireReleased()
            check(pendingCleanup == null) {
                val message = "Call service cleanup still pending id=${pendingCleanup?.callId}"
                LogManager.addLog("ERROR", "CallService", message)
                message
            }
        }

        /** На main запросить foreground для ненулевого ID; ошибка оставляет сигналинг на стороне CallController. */
        fun start(context: Context, callId: Long) {
            checkMain()
            requireReleased()
            require(callId != 0L) {
                LogManager.addLog("ERROR", "CallService", "service request rejected: zero call ID")
                "Call ID must be nonzero"
            }
            try {
                val component = ContextCompat.startForegroundService(context,
                    Intent(context, CallService::class.java).putExtra(EXTRA_ID, callId))
                check(component != null) { "CallService component unavailable" }
                LogManager.addLog("DEBUG", "CallService", "service request accepted id=$callId component=$component")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "service request failed id=$callId: $e")
                throw e
            }
        }

        private fun checkMain() = check(Looper.myLooper() == Looper.getMainLooper()) {
            val message = "CallService requires main thread: caller=${Thread.currentThread().name}"
            LogManager.addLog("ERROR", "CallService", message)
            message
        }
    }

    val audio = CallAudioEngine()
    var video: CallVideoEngine? = null
        private set
    private var projection: ScreenProjection? = null
    private var screenForeground = false
    private var cameraForeground = false
    private var callId = 0L
    private var lastOwnedCallId = 0L // Для serviceLost после уже подтверждённого shutdown, обнулившего callId.
    private var wakeLock: PowerManager.WakeLock? = null
    private var powerEventsRegistered = false
    private var audioOwned = false
    private var foregroundOwned = false
    private var channelReady = false
    private var stopping = false
    private var destroyed = false
    private var stopRequested = false
    private var lastStartId = 0
    private var released = CompletableDeferred<Unit>()
    private var openIntent: PendingIntent? = null
    private var endIntent: PendingIntent? = null
    private val main = Handler(Looper.getMainLooper())
    private var retryDelayMs = 250L
    private val retryCleanup = Runnable { shutdown() }
    private val powerEvents = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (stopping || destroyed || !powerEventsRegistered) return
            try {
                val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
                LogManager.addLog("INFO", "CallService", "power event=${intent.action} id=$callId interactive=${pm.isInteractive} " +
                    "idle=${pm.isDeviceIdleMode} save=${pm.isPowerSaveMode} lock=${wakeLock?.isHeld} ${audio.diagnostics}")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "power diagnostics failed action=${intent.action} id=$callId: $e")
            }
        }
    }

    override fun onCreate() {
        checkMain()
        super.onCreate()
        try { createChannel() } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "channel creation failed; will retry at start: $e")
        }
        LogManager.addLog("INFO", "CallService", "created")
    }

    private fun createChannel() {
        val manager = requireNotNull(getSystemService(NotificationManager::class.java)) { "notification manager unavailable" }
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL, AppLanguage.text(R.string.ongoing_call), NotificationManager.IMPORTANCE_DEFAULT).apply {
                description = AppLanguage.text(R.string.ongoing_call_description)
                setSound(null, null)
                enableVibration(false)
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            })
        channelReady = true
        try {
            val channel = manager.getNotificationChannel(CHANNEL)
            LogManager.addLog("INFO", "CallService", "notification channel=$CHANNEL enabled=${manager.areNotificationsEnabled()} " +
                "importance=${channel?.importance} lockscreenVisibility=${channel?.lockscreenVisibility}")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "CallService", "channel diagnostics unavailable channel=$CHANNEL: $e")
        }
    }

    /** Обновить канал/своё уведомление на main; после остановки публикация не возобновляется. */
    fun refreshLanguage() {
        checkMain()
        if (stopping || destroyed) return
        try { createChannel(); updateNotification() } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "notification language refresh failed id=$callId: $e")
        }
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        projection?.configurationChanged()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        checkMain()
        lastStartId = startId
        val id = intent?.getLongExtra(EXTRA_ID, 0L) ?: 0L
        if (intent?.action == ACTION_END) {
            if (id == 0L || id != callId || stopping || destroyed || !CallController.wantsService(id)) {
                LogManager.addLog("WARN", "CallService", "stale END ignored id=$id owned=$callId stopping=$stopping")
                if (callId == 0L) shutdown()
            } else try { CallController.end(id) } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "END callback failed id=$id: $e")
                shutdown()
            }
            return START_NOT_STICKY
        }
        if (destroyed || !CallController.wantsService(id)) {
            LogManager.addLog("WARN", "CallService", "stale start id=$id owned=$callId")
            if (!destroyed && callId == 0L) {
                // Даже отменённый startForegroundService должен завершить foreground-handshake.
                try {
                    if (!channelReady) createChannel()
                    promote(false)
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "CallService", "cancelled foreground handshake failed id=$id: $e")
                } finally {
                    shutdown()
                }
            }
            return START_NOT_STICKY
        }
        if (stopping && pendingCleanup === this) {
            LogManager.addLog("ERROR", "CallService", "start blocked by pending cleanup id=$id owned=$callId")
            try { CallController.audioFailed(id, "previous service cleanup pending") } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "cleanup refusal callback failed id=$id: $e")
            }
            return START_NOT_STICKY
        }
        if (callId == id) return START_NOT_STICKY
        if (callId != 0L) {
            LogManager.addLog("ERROR", "CallService", "start conflicts id=$id owned=$callId")
            try { CallController.audioFailed(id, "service already owns another call") } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "ownership conflict callback failed id=$id owned=$callId: $e")
            }
            return START_NOT_STICKY
        }
        callId = id
        lastOwnedCallId = id
        stopping = false; stopRequested = false
        if (released.isCompleted) released = CompletableDeferred()
        audio.onFailure = { failedId, detail ->
            if (failedId == callId && !stopping && !destroyed) try { CallController.audioFailed(failedId, detail) } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "audio failure callback failed id=$failedId: $e")
                shutdown()
            }
        }
        var operation = "channel"
        try {
            if (!channelReady) createChannel()
            operation = "foreground"
            promote(true)
            operation = "wake lock"
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "utun:ongoing_call")
            wakeLock!!.setReferenceCounted(false)
            wakeLock!!.acquire()
            LogManager.addLog("INFO", "CallService",
                "foreground phoneCall|microphone; CPU lock acquired id=$id held=${wakeLock?.isHeld}")
            val filter = IntentFilter().apply {
                addAction(Intent.ACTION_SCREEN_OFF)
                addAction(Intent.ACTION_SCREEN_ON)
                addAction(PowerManager.ACTION_DEVICE_IDLE_MODE_CHANGED)
                addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED)
            }
            operation = "power receiver"
            powerEventsRegistered = true
            ContextCompat.registerReceiver(this, powerEvents, filter, ContextCompat.RECEIVER_NOT_EXPORTED)
            operation = "ready callback"
            if (!CallController.serviceReady(id, this)) shutdown()
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "start failed operation=$operation id=$id: $e")
            shutdown()
            try { CallController.audioFailed(id, "$operation failed: $e") } catch (callback: Exception) {
                LogManager.addLog("ERROR", "CallService", "startup failure callback failed id=$id: $callback")
            }
        }
        return START_NOT_STICKY
    }

    private fun promote(microphone: Boolean) {
        val micType = if (microphone && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE else 0
        val types = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_PHONE_CALL or micType or
                (if (screenForeground) ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION else 0) or
                (if (cameraForeground && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA else 0)
        } else 0
        foregroundOwned = true // startForeground может отказать после эффекта IPC.
        ServiceCompat.startForeground(this, NOTIFICATION_ID, notification(), types)
        LogManager.addLog("INFO", "CallService", "foreground notification id=$callId phase=${CallController.state.value?.phase}")
    }

    /** На main запустить I/O только своего ID с CPU lock. Повтор идемпотентен; отказ откатывает весь сервис. */
    fun startAudio(id: Long): Boolean {
        checkMain()
        if (id == 0L || id != callId || stopping || destroyed) {
            LogManager.addLog("ERROR", "CallService", "audio start rejected id=$id owned=$callId stopping=$stopping destroyed=$destroyed")
            return false
        }
        if (audioOwned && audio.isActive() && video != null) return true
        var operation = "wake lock"
        try {
            check(wakeLock?.isHeld == true) { "CPU lock not held" }
            operation = "route"
            audio.router.setPreferredRoute(ChatApplication.instance.configProvider.getCallAudioRoute())
            operation = "audio"
            audioOwned = true
            check(audio.start(id)) { "audio engine rejected start" }
            operation = "video"
            video = CallVideoEngine(this, id)
            video!!.start()
            LogManager.addLog("INFO", "CallService", "audio/video started id=$id")
            return true
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "audio start failed operation=$operation id=$id: $e")
            shutdown()
            return false
        }
    }

    /** На main согласовать foreground type до смены camera capture; при отказе сохраняется последний подтверждённый флаг. */
    fun setVideoCapture(enabled: Boolean, screen: Boolean = false, grant: Intent? = null): Boolean {
        checkMain()
        if (callId == 0L || stopping || destroyed || !audioOwned || video == null) {
            LogManager.addLog("WARN", "CallService", "camera request rejected id=$callId enabled=$enabled stopping=$stopping")
            return false
        }
        val previous = cameraForeground
        val previousScreen = screenForeground
        return try {
            cameraForeground = enabled && !screen
            screenForeground = enabled && screen
            promote(true)
            if (screenForeground && projection == null) {
                val engine = checkNotNull(video)
                projection = ScreenProjection(this, checkNotNull(grant)) {
                    if (video === engine && !stopping) CallController.setVideo(engine.state.value.settings.copy(enabled = false))
                }
                engine.projection = projection
            } else if (!screenForeground) { projection?.close(); projection = null; video?.projection = null }
            LogManager.addLog("INFO", "CallService", "video foreground confirmed id=$callId enabled=$enabled")
            true
        } catch (e: Exception) {
            cameraForeground = previous; screenForeground = previousScreen
            try { promote(true) } catch (restore: Exception) { e.addSuppressed(restore) }
            LogManager.addLog("ERROR", "CallService", "camera foreground failed id=$callId requested=$enabled lastConfirmed=$previous: $e")
            false
        }
    }

    /** На main обновить свою живую карточку, сохраняя токены и monotonic anchor разговора. */
    fun updateNotification() {
        checkMain()
        if (callId == 0L || stopping || destroyed || !foregroundOwned) return
        try {
            requireNotNull(getSystemService(NotificationManager::class.java)) { "notification manager unavailable" }
                .notify(NOTIFICATION_ID, notification())
            LogManager.addLog("INFO", "CallService", "notification updated id=$callId phase=${CallController.state.value?.phase}")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "notification update failed id=$callId: $e")
        }
    }

    private fun notification(): Notification {
        val st = CallController.state.value?.takeIf { it.callId == callId }
        val active = st?.phase == CallPhase.ACTIVE
        val person = Person.Builder().setName(st?.peerName?.takeIf { it.isNotBlank() } ?: "vibechat")
            .setKey(st?.peerAddress ?: st?.channelId ?: callId.toString()).build()
        val open = openIntent ?: PendingIntent.getActivity(this, 0,
            Intent(this, MainActivity::class.java).setData(Uri.parse("utun-call:open:$callId")),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE).also { openIntent = it }
        val end = endIntent ?: PendingIntent.getService(this, 0,
            Intent(this, CallService::class.java).setAction(ACTION_END)
                .setData(Uri.parse("utun-call:$callId")).putExtra(EXTRA_ID, callId),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE).also { endIntent = it }
        val builder = NotificationCompat.Builder(AppLanguage.context(), CHANNEL)
            .setSmallIcon(android.R.drawable.ic_menu_call)
            .setContentTitle(person.name)
            .setContentText(when (st?.phase) {
                CallPhase.ACTIVE -> AppLanguage.text(R.string.call_talking)
                CallPhase.RINGING -> AppLanguage.text(R.string.call_ringing)
                CallPhase.WAIT_DECISION -> AppLanguage.text(R.string.phone_busy_warning)
                else -> AppLanguage.text(R.string.call_setup)
            })
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .setContentIntent(open)
            .setStyle(NotificationCompat.CallStyle.forOngoingCall(person, end))
            .addPerson(person)
            .setShowWhen(active)
            .setUsesChronometer(active)
        if (active && st != null) {
            // Один источник начала разговора для UI и системного таймера; уведомление не обновляется каждую секунду.
            builder.setWhen(System.currentTimeMillis() - (SystemClock.elapsedRealtime() - st.startedAtElapsedRealtime))
        }
        return builder.build()
    }

    /** На main независимо очистить владельцев. Deferred успешно завершается после подтверждения всех операций.
     * При отказе сохраняется один process-владелец и отменяемый retry 250мс..30с; main не ждёт Deferred. */
    fun shutdown(): Deferred<Unit> {
        checkMain()
        main.removeCallbacks(retryCleanup)
        stopping = true
        try { projection?.close(); projection = null; screenForeground = false }
        catch (e: Exception) { LogManager.addLog("ERROR", "call", "screen projection close failed id=$callId: $e") }
        releaseResources()
        if (!audioOwned && video == null && projection == null) {
            if (foregroundOwned) try {
                stopForeground(STOP_FOREGROUND_REMOVE)
                foregroundOwned = false
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "foreground removal failed id=$callId; retained for retry: $e")
            }
            if (!stopRequested) try {
                if (!destroyed) stopSelf(lastStartId)
                stopRequested = true
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "service stop failed id=$callId; retained for retry: $e")
            }
        }
        val pending = audioOwned || video != null || projection != null || wakeLock != null || powerEventsRegistered ||
            foregroundOwned || openIntent != null || endIntent != null || !stopRequested
        if (pending) {
            check(pendingCleanup == null || pendingCleanup === this) {
                val message = "Two retained call service owners: current=$callId previous=${pendingCleanup?.callId}"
                LogManager.addLog("ERROR", "CallService", message)
                message
            }
            pendingCleanup = this
            val delay = retryDelayMs
            retryDelayMs = (retryDelayMs * 2).coerceAtMost(30_000L)
            if (!main.postDelayed(retryCleanup, delay)) LogManager.addLog("ERROR", "CallService",
                "cleanup retry rejected id=$callId audio=$audioOwned video=${video != null} lock=${wakeLock != null} " +
                    "receiver=$powerEventsRegistered; retained for explicit retry")
            else LogManager.addLog("DEBUG", "CallService", "cleanup retry scheduled id=$callId delayMs=$delay")
        } else {
            val id = callId
            callId = 0L; cameraForeground = false; retryDelayMs = 250L
            if (pendingCleanup === this) pendingCleanup = null
            if (released.complete(Unit)) LogManager.addLog("INFO", "CallService", "resources released id=$id; cleanup confirmed")
        }
        return released
    }

    /** Независимые попытки на main; успешные владельцы не повторяются, ошибочные ссылки сохраняются. */
    private fun releaseResources() {
        val id = callId
        audio.onFailure = null
        audio.router.onChanged = null
        video?.let { owned -> try {
            if (owned.stop()) video = null
            else LogManager.addLog("DEBUG", "CallService", "video cleanup pending id=$id; service retains owner")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "video stop failed id=$id; retained for retry: $e")
        } }
        if (audioOwned) try {
            audio.stop()
            audioOwned = false
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "audio stop failed id=$id; retained for retry: $e")
        }
        if (powerEventsRegistered) {
            try {
                unregisterReceiver(powerEvents)
                powerEventsRegistered = false
            } catch (e: IllegalArgumentException) {
                powerEventsRegistered = false
                LogManager.addLog("WARN", "CallService", "power receiver already absent id=$id: $e")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "power receiver cleanup failed id=$id; retained for retry: $e")
            }
        }
        wakeLock?.let { lock -> try {
            if (lock.isHeld) lock.release()
            wakeLock = null
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "wake lock release failed id=$id; retained for retry: $e")
        } }
        openIntent?.let { token -> try {
            token.cancel(); openIntent = null
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "open action cancellation failed id=$id; retained for retry: $e")
        } }
        endIntent?.let { token -> try {
            token.cancel(); endIntent = null
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallService", "END action cancellation failed id=$id; retained for retry: $e")
        } }
    }

    override fun onDestroy() {
        checkMain()
        if (destroyed) return
        destroyed = true
        val id = lastOwnedCallId
        try {
            shutdown()
            try { CallController.serviceLost(id, this) } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallService", "service lost callback failed id=$id: $e")
            }
            LogManager.addLog("INFO", "CallService", "destroyed id=$id cleanupComplete=${pendingCleanup !== this}")
        } finally { super.onDestroy() }
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
