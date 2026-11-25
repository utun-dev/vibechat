package com.utun.chat.data

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Looper
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import com.utun.chat.ChatApplication
import com.utun.chat.MainActivity
import com.utun.chat.R

/**
 * Одно входящее оповещение P2P/рации: мелодия, пробуждение экрана и уведомление.
 * Контроллеры вызывают show/stopRinging на main; повтор show того же вызова только обновляет текст.
 * Отказ системного уведомления или wake lock не останавливает мелодию и вибрацию.
 * PendingIntent привязан к полному ID и виду вызова, отменяется при stop; запоздалое действие проверяет isRinging.
 * Stop сразу снимает active; ссылки на lock/кнопки и необходимость отмены уведомления сохраняются до успешной очистки.
 * Повтор stop/init/refreshLanguage без active повторяет только незавершённую очистку, без звука и повторного wake.
 * Пока канал не подготовлен, init/refreshLanguage также отменяет возможное уведомление прежнего процесса.
 * Ошибка старого wake lock запрещает заменять его новым: следующий звонок продолжает звук/уведомление без нового lock.
 */
object IncomingCallManager {

    const val CHANNEL_CALL = "incoming_call"
    const val NOTIF_ID = 1001

    const val EXTRA_CALL_ID = "call_id"
    const val EXTRA_CHANNEL_ID = "channel_id"
    const val EXTRA_PEER_ADDRESS = "peer_address"
    const val EXTRA_RADIO_CALL = "radio_call"
    const val EXTRA_ANSWER_RADIO = "answer_radio"

    private val ringtone = RingtonePlayer()

    private data class Alert(val callId: Long, val channelId: String, val peer: String, val radio: Boolean)
    private var initialized = false
    private var active: Alert? = null
    private val pendingIntents = mutableMapOf<PendingIntent, Alert>() // Текущие кнопки и прежние с неподтверждённой отменой.
    private var wakeLock: PowerManager.WakeLock? = null // Сохраняется при ошибке isHeld/release; не перезаписывается.
    private var notificationPending = false // Уведомление могло быть опубликовано либо cancel не подтвердился.

    /** На main создать канал и убрать уведомление прежнего процесса без живого вызова.
     * При повторе без active повторяет незавершённую очистку независимо от состояния канала. */
    fun init() {
        checkMain()
        val first = !initialized
        if (first) initialized = createChannel()
        if (active == null) cleanupStopped(forceNotification = first)
        if (first && initialized) {
            LogManager.addLog("INFO", "InCall", "manager initialized")
        }
    }

    /** До API 34 — true; на новых — право ОС на full-screen intent. При недоступной службе — false. */
    fun canFullScreenIntent(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) return true
        return try {
            val nm = requireNotNull(ChatApplication.instance.getSystemService(NotificationManager::class.java)) {
                "notification manager unavailable"
            }
            nm.canUseFullScreenIntent()
        } catch (e: Exception) {
            LogManager.addLog("WARN", "InCall", "full-screen permission unavailable: $e")
            false
        }
    }

    /** На main проверить владельца ещё звонящего оповещения перед действием из уведомления. */
    fun isRinging(callId: Long, radio: Boolean): Boolean {
        checkMain()
        return active?.let { it.callId == callId && it.radio == radio } == true
    }

    /** На main сразу снять active и запросить независимую очистку звука, wake lock, кнопок и уведомления.
     * Повтор без active повторяет незавершённые операции, включая остановку RingtonePlayer. */
    fun stopRinging() {
        checkMain()
        val previous = active
        active = null
        cleanupStopped(forceNotification = previous != null)
        if (previous != null) {
            LogManager.addLog("INFO", "InCall", "alert cleared callId=${previous.callId} radio=${previous.radio} " +
                "pending_actions=${pendingIntents.size} wake_lock_retained=${wakeLock != null} notification_pending=$notificationPending")
        }
    }

    /** Только без active: не теряет ссылки при отказе и не повторяет уже подтверждённую очистку. */
    private fun cleanupStopped(forceNotification: Boolean) {
        ringtone.stop()
        releaseWakeLock()
        for ((token, owner) in pendingIntents.toList()) {
            try {
                token.cancel()
                pendingIntents.remove(token)
                LogManager.addLog("DEBUG", "InCall", "action cancelled callId=${owner.callId} radio=${owner.radio}")
            } catch (e: Exception) {
                LogManager.addLog("WARN", "InCall", "cancel action failed callId=${owner.callId} radio=${owner.radio}; retained for retry: $e")
            }
        }
        cancelNotification(forceNotification)
    }

    /** На main заменить оповещение. peer — полный Ed25519 звонящего. */
    fun show(callId: Long, channelId: String, peer: String, radio: Boolean = false) {
        checkMain()
        if (callId == 0L || channelId.isEmpty() || !MemberFilesProtocol.validAddress(peer)) {
            LogManager.addLog("ERROR", "InCall", "invalid alert callId=$callId peer=$peer ch=$channelId radio=$radio")
            return
        }
        val next = Alert(callId, channelId, peer, radio)
        if (active == next) {
            showNotification(next, alert = false)
            return
        }
        stopRinging()
        active = next
        LogManager.addLog("INFO", "InCall", "incoming call id=$callId peer=$peer ch=$channelId radio=$radio")
        acquireWakeLock()
        ringtone.play(if (radio) R.raw.radio_incoming else R.raw.ringtone,
            preserveMedia = StreamController.state.value?.ongoing == true || StreamPlayerEngine.viewing.value)
        showNotification(next)
    }

    /** На main обновить язык канала/текущего уведомления без повтора звука, wake lock и full-screen запуска.
     * Без active повторить незавершённую очистку даже при отказе канала; до подготовки канала убрать уведомление прежнего процесса. */
    fun refreshLanguage() {
        checkMain()
        val first = !initialized
        initialized = createChannel()
        val call = active
        if (call == null) cleanupStopped(forceNotification = first)
        else if (initialized) showNotification(call, alert = false)
    }

    /* ── пробуждение устройства ── */

    /** Один не reference-counted lock на оповещение, timeout 30с; старый не перезаписывается при незавершённой очистке. */
    private fun acquireWakeLock() {
        if (wakeLock != null) {
            LogManager.addLog("WARN", "InCall", "wake lock acquisition skipped: cleanup pending callId=${active?.callId}")
            return
        }
        var operation = "service"
        try {
            val pm = ChatApplication.instance.getSystemService(Context.POWER_SERVICE) as PowerManager
            operation = "create"
            val lock = pm.newWakeLock(
                PowerManager.SCREEN_BRIGHT_WAKE_LOCK or PowerManager.ACQUIRE_CAUSES_WAKEUP
                    or PowerManager.ON_AFTER_RELEASE,
                "utun:incoming_call"
            )
            wakeLock = lock
            operation = "configure"; lock.setReferenceCounted(false)
            operation = "acquire"
            lock.acquire(30_000)
            LogManager.addLog("DEBUG", "InCall", "screen wake lock acquired callId=${active?.callId} " +
                "lock=${System.identityHashCode(lock)} timeoutMs=30000")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "InCall", "wake lock failed operation=$operation callId=${active?.callId}: $e")
            releaseWakeLock()
        }
    }

    /** Ссылка сбрасывается только после успешного isHeld/release; timeout не требует повторного release. */
    private fun releaseWakeLock() {
        val lock = wakeLock ?: return
        var operation = "query"
        try {
            if (lock.isHeld) { operation = "release"; lock.release() }
            wakeLock = null
            LogManager.addLog("DEBUG", "InCall", "screen wake lock cleanup complete lock=${System.identityHashCode(lock)}")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "InCall", "wake lock release failed operation=$operation " +
                "lock=${System.identityHashCode(lock)}; retained for retry: $e")
        }
    }

    /* ── уведомление ── */

    private fun createChannel(): Boolean {
        try {
            val ctx = AppLanguage.context()
            val ch = NotificationChannel(CHANNEL_CALL, AppLanguage.text(R.string.incoming_calls), NotificationManager.IMPORTANCE_HIGH).apply {
                description = AppLanguage.text(R.string.incoming_call_notification)
                enableVibration(false)
                setSound(null, null)
            }
            val nm = requireNotNull(ctx.getSystemService(NotificationManager::class.java)) { "notification manager unavailable" }
            nm.createNotificationChannel(ch)
            return true
        } catch (e: Exception) {
            LogManager.addLog("WARN", "InCall", "notification channel unavailable: $e")
            return false
        }
    }

    private fun showNotification(call: Alert, alert: Boolean = true) {
        try {
            init()
            if (!initialized) return
            val (callId, channelId, peer, radio) = call
            val ctx = AppLanguage.context()
            if (!NotificationManagerCompat.from(ctx).areNotificationsEnabled()) {
                LogManager.addLog("WARN", "InCall", "notifications disabled — skip (still ringing) callId=$callId radio=$radio")
                return
            }

            val launchIntent = Intent(ctx, MainActivity::class.java).apply {
                data = Uri.parse("utun-call://incoming/${if (radio) "radio" else "p2p"}/$callId")
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                putExtra(EXTRA_CALL_ID, callId)
                putExtra(EXTRA_CHANNEL_ID, channelId)
                putExtra(EXTRA_PEER_ADDRESS, peer)
                putExtra(EXTRA_RADIO_CALL, radio)
            }
            val contentPi = PendingIntent.getActivity(ctx, 0, launchIntent,
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT).also { pendingIntents[it] = call }

            val declinePi = PendingIntent.getBroadcast(ctx, 0,
                Intent(ctx, IncomingCallActionReceiver::class.java).apply {
                    action = IncomingCallActionReceiver.ACTION_DECLINE
                    data = launchIntent.data
                    putExtra(EXTRA_CALL_ID, callId)
                    putExtra(EXTRA_RADIO_CALL, radio)
                },
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT).also { pendingIntents[it] = call }

            val builder = NotificationCompat.Builder(ctx, CHANNEL_CALL)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle(AppLanguage.text(if (radio) R.string.radio_call_incoming else R.string.incoming_call))
                .setContentText(if (radio) RadioCallController.state.value?.let { "${it.peerName} · ${it.channelName}" }
                    ?: channelId else peer)
                .setCategory(NotificationCompat.CATEGORY_CALL)
                .setPriority(NotificationCompat.PRIORITY_MAX)
                .setOngoing(true)
                .setAutoCancel(false)
                .setOnlyAlertOnce(true)
                .setContentIntent(contentPi)
                .addAction(0, AppLanguage.text(if (radio) R.string.decline else R.string.hang_up), declinePi)

            if (!radio && CallController.hasMedia()) {
                val warning = AppLanguage.text(CallController.mediaWarning(incoming = true))
                builder.setContentText(warning).setStyle(NotificationCompat.BigTextStyle().bigText(warning))
            }

            if (radio) {
                val warnings = mutableListOf<String>()
                if (StreamPlayerEngine.viewing.value) warnings.add(AppLanguage.text(R.string.radio_call_stop_view))
                if (StreamController.state.value?.ongoing == true) warnings.add(AppLanguage.text(R.string.radio_call_keep_stream))
                if (warnings.isNotEmpty()) builder.setStyle(NotificationCompat.BigTextStyle().bigText(warnings.joinToString("\n")))
                val answerPi = PendingIntent.getActivity(ctx, 2, Intent(launchIntent).apply {
                    action = "com.utun.chat.ANSWER_RADIO"
                    putExtra(EXTRA_ANSWER_RADIO, true)
                }, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT).also { pendingIntents[it] = call }
                builder.addAction(0, AppLanguage.text(R.string.accept), answerPi)
            }

            if (alert && canFullScreenIntent()) {
                builder.setFullScreenIntent(contentPi, true)
            } else if (alert) {
                LogManager.addLog("WARN", "InCall", "full-screen intent not allowed — heads-up only")
            }

            val notification = builder.build()
            notificationPending = true
            NotificationManagerCompat.from(ctx).notify(NOTIF_ID, notification)
            LogManager.addLog("INFO", "InCall", "notification posted callId=$callId radio=$radio alert=$alert")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "InCall", "notification failed callId=${call.callId} radio=${call.radio}: $e")
        }
    }

    /** Cancel может завершиться на стороне ОС и всё же бросить: повторяется, пока API не подтвердит успех. */
    private fun cancelNotification(force: Boolean) {
        if (!force && !notificationPending) return
        notificationPending = true
        try {
            NotificationManagerCompat.from(ChatApplication.instance).cancel(NOTIF_ID)
            notificationPending = false
            LogManager.addLog("DEBUG", "InCall", "notification cancelled id=$NOTIF_ID")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "InCall", "cancel notification failed id=$NOTIF_ID; retained for retry: $e")
        }
    }

    private fun checkMain() = check(Looper.myLooper() == Looper.getMainLooper()) {
        LogManager.addLog("ERROR", "InCall", "operation requires main thread")
        "IncomingCallManager requires main thread"
    }
}
