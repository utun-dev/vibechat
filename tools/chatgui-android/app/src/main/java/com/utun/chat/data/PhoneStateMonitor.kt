package com.utun.chat.data

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.telephony.PhoneStateListener
import android.telephony.SubscriptionManager
import android.telephony.TelephonyCallback
import android.telephony.TelephonyManager
import androidx.core.content.ContextCompat
import com.utun.chat.ChatApplication
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/** Живое состояние SIM без чтения номеров: BUSY, если занята хотя бы одна SIM; IDLE — только когда все известны и свободны.
 * Нет SIM/разрешения или ещё не пришёл начальный callback — UNKNOWN. Три младших бита policy включают наблюдение,
 * запреты обычных/радиозвонков передаются независимо от READ_PHONE_STATE. init/refresh и состояния принадлежат main.
 * У каждой SIM свой callback: TelephonyCallback с 31, PhoneStateListener на 26..30; начальное значение приходит тем же путём.
 * refresh сохраняет здоровые регистрации; временные отказы повторяются через 1..30с, отключение отменяет повтор.
 * При снятии наблюдения старые callbacks теряют право публикации даже при ошибке ОС.
 * JNI принимает копию асинхронно; SERVICE_STARTED вызывает refresh и повторяет состояние для нового ядра. */
@Suppress("DEPRECATION")
object PhoneStateMonitor {
    private val main = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private class Watch(val id: Int, val manager: TelephonyManager) {
        var state = -1 // UNKNOWN до первого callback; это не Android CALL_STATE_IDLE (0).
        val modern = Build.VERSION.SDK_INT >= 31
        val callback: Any = if (modern) object : TelephonyCallback(), TelephonyCallback.CallStateListener {
            override fun onCallStateChanged(state: Int) = changed(this@Watch, state)
        } else object : PhoneStateListener() {
            override fun onCallStateChanged(state: Int, phoneNumber: String?) = changed(this@Watch, state)
        }
    }
    private class Subscriptions(val manager: SubscriptionManager) {
        val callback = object : SubscriptionManager.OnSubscriptionsChangedListener() {
            override fun onSubscriptionsChanged() { main.post { if (subscriptions === this@Subscriptions) refresh() } }
        }
    }
    private val watches = mutableMapOf<Int, Watch>()
    private var activeIds = emptySet<Int>()
    private var subscriptions: Subscriptions? = null
    private var enabled = false
    private var policy = 0
    private var lastState = -1
    private var started = false
    private var retryDelay = 1_000L
    private val retry = Runnable { refresh() }

    fun granted(): Boolean = ContextCompat.checkSelfPermission(ChatApplication.instance, Manifest.permission.READ_PHONE_STATE) ==
        PackageManager.PERMISSION_GRANTED

    /** Подписаться на пять настроек приложения; повторный init на main не создаёт второй collector. */
    fun init() {
        checkMain()
        if (started) return
        started = true
        LogManager.addLog("INFO", "PhoneState", "SIM monitor initialized")
        val app = ChatApplication.instance
        scope.launch {
            app.configProvider.incomingCallOptions.collect { options ->
                policy = options.withIndex().fold(0) { mask, (index, value) -> mask or if (value) (1 shl index) else 0 }
                enabled = policy and 7 != 0
                refresh()
            }
        }
    }

    /** Сверить SIM/разрешение и повторно опубликовать состояние; main, в том числе после старта ядра/возврата из настроек. */
    fun refresh() {
        checkMain()
        main.removeCallbacks(retry)
        if (!enabled || !granted()) {
            clearWatches(); clearSubscriptions(); retryDelay = 1_000L
            publish(0); return
        }
        val app = ChatApplication.instance
        val subManager = app.getSystemService(SubscriptionManager::class.java)
        val telephony = app.getSystemService(TelephonyManager::class.java)
        if (telephony == null || subManager == null) {
            clearWatches(); clearSubscriptions()
            LogManager.addLog("WARN", "PhoneState", "Telephony unavailable phone=${telephony != null} subscriptions=${subManager != null}")
            publish(0); return
        }
        if (subscriptions?.manager !== subManager) {
            clearWatches(); clearSubscriptions()
            val next = Subscriptions(subManager)
            subscriptions = next
            try {
                if (Build.VERSION.SDK_INT >= 30) subManager.addOnSubscriptionsChangedListener(ContextCompat.getMainExecutor(app), next.callback)
                else subManager.addOnSubscriptionsChangedListener(next.callback)
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "PhoneState", "Cannot register SIM listener: $e")
                clearSubscriptions(); scheduleRetry(); publish(0); return
            }
            LogManager.addLog("INFO", "PhoneState", "SIM listener registered")
        }
        val ids = try {
            subManager.activeSubscriptionInfoList.orEmpty().map { it.subscriptionId }.toSet()
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "PhoneState", "Cannot read SIM subscriptions: $e")
            clearWatches(); scheduleRetry(); publish(0); return
        }
        if (ids != activeIds) LogManager.addLog("INFO", "PhoneState", "SIM subscriptions $activeIds -> $ids")
        activeIds = ids
        for (id in watches.keys.toList()) if (id !in ids) watches.remove(id)?.let(::removeWatch)
        var failed = false
        for (id in ids) if (id !in watches) {
            try {
                val watch = Watch(id, telephony.createForSubscriptionId(id))
                watches[id] = watch
                if (watch.modern) watch.manager.registerTelephonyCallback(ContextCompat.getMainExecutor(app), watch.callback as TelephonyCallback)
                else watch.manager.listen(watch.callback as PhoneStateListener, PhoneStateListener.LISTEN_CALL_STATE)
                LogManager.addLog("INFO", "PhoneState", "Phone observer registered SIM=$id api=${Build.VERSION.SDK_INT}")
            } catch (e: Exception) {
                watches.remove(id)?.let(::removeWatch)
                failed = true
                LogManager.addLog("ERROR", "PhoneState", "Cannot register phone observer SIM=$id: $e")
            }
        }
        if (failed) scheduleRetry() else retryDelay = 1_000L
        publishAggregate()
    }

    private fun removeWatch(watch: Watch) {
        try {
            if (watch.modern) watch.manager.unregisterTelephonyCallback(watch.callback as TelephonyCallback)
            else watch.manager.listen(watch.callback as PhoneStateListener, PhoneStateListener.LISTEN_NONE)
        } catch (e: Exception) { LogManager.addLog("WARN", "PhoneState", "Phone observer removal failed SIM=${watch.id}: $e") }
        LogManager.addLog("DEBUG", "PhoneState", "Phone observer invalidated SIM=${watch.id}")
    }

    private fun clearWatches() {
        val previous = watches.values.toList()
        watches.clear(); activeIds = emptySet()
        previous.forEach(::removeWatch)
    }

    private fun clearSubscriptions() {
        val previous = subscriptions ?: return
        subscriptions = null
        try { previous.manager.removeOnSubscriptionsChangedListener(previous.callback) }
        catch (e: Exception) { LogManager.addLog("WARN", "PhoneState", "SIM listener removal failed: $e") }
        LogManager.addLog("DEBUG", "PhoneState", "SIM listener invalidated")
    }

    private fun scheduleRetry() {
        LogManager.addLog("DEBUG", "PhoneState", "SIM observation retry in ${retryDelay}ms")
        main.postDelayed(retry, retryDelay)
        retryDelay = (retryDelay * 2).coerceAtMost(30_000L)
    }

    private fun changed(watch: Watch, state: Int) {
        checkMain()
        if (!enabled || watches[watch.id] !== watch) return
        if (!granted()) { refresh(); return }
        watch.state = when (state) {
            TelephonyManager.CALL_STATE_IDLE, TelephonyManager.CALL_STATE_RINGING, TelephonyManager.CALL_STATE_OFFHOOK -> state
            else -> { LogManager.addLog("WARN", "PhoneState", "Invalid phone state SIM=${watch.id} state=$state"); -1 }
        }
        LogManager.addLog("DEBUG", "PhoneState", "Phone callback SIM=${watch.id} state=${watch.state}")
        publishAggregate()
    }

    private fun publishAggregate() {
        val busy = watches.values.any { it.state == TelephonyManager.CALL_STATE_OFFHOOK || it.state == TelephonyManager.CALL_STATE_RINGING }
        val idle = activeIds.isNotEmpty() && activeIds.all { watches[it]?.state == TelephonyManager.CALL_STATE_IDLE }
        publish(if (busy) 2 else if (idle) 1 else 0)
    }

    private fun publish(state: Int) {
        if (lastState != state) LogManager.addLog("INFO", "PhoneState", "State=$state enabled=$enabled subscriptions=${activeIds.size}")
        lastState = state // Только для диагностики: повторный publish всё равно отправляет актуальное состояние.
        if (!NativeLib.isReady()) {
            LogManager.addLog("DEBUG", "PhoneState", "State deferred state=$state policy=$policy: core is not ready")
            return
        }
        if (!NativeLib.setPhoneState(state, policy))
            LogManager.addLog("WARN", "PhoneState", "State not queued state=$state policy=$policy; next update/refresh will retry")
    }

    private fun checkMain() {
        check(Looper.myLooper() === Looper.getMainLooper()) {
            val message = "PhoneStateMonitor requires main thread: caller=${Thread.currentThread().name}"
            LogManager.addLog("ERROR", "PhoneState", message)
            message
        }
    }
}
