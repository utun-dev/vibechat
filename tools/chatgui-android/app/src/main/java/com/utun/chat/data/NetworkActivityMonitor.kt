package com.utun.chat.data

import android.net.ConnectivityManager
import android.os.Handler
import android.os.Looper
import com.utun.chat.ChatApplication

/**
 * Слушатель внешней сетевой активности (ConnectivityManager.OnNetworkActiveListener).
 *
 * Пробуждает standby-ядро досрочно: когда в фазе SLEEP прошёл min_sleep,
 * приход сетевой активности запускает следующий активный цикл раньше.
 *
 * start/stop вызывает HeadlessService на main thread. API доступен с 21, весь minSdk 26 поддерживается.
 * Callback проходит main-очередь, проверяет свою регистрацию и вызывает JNI, который постит событие в uasync.
 * После stop или нового start старые callbacks игнорируются; повторные start/stop безопасны.
 */
object NetworkActivityMonitor {
    private val handler = Handler(Looper.getMainLooper())
    private var registration: Registration? = null

    private class Registration(val manager: ConnectivityManager) {
        val listener = ConnectivityManager.OnNetworkActiveListener {
            handler.post {
                if (registration === this) NativeLib.notifyNetworkActive()
            }
        }
    }

    /** Зарегистрировать listener; отказ/отсутствие manager логируется и допускает следующую попытку start. */
    fun start() {
        if (registration != null) return
        val cm = ChatApplication.instance.getSystemService(ConnectivityManager::class.java)
        if (cm == null) { LogManager.addLog("ERROR", "NET", "network activity manager unavailable"); return }
        val next = Registration(cm)
        try {
            cm.addDefaultNetworkActiveListener(next.listener)
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "NET", "network activity listener registration failed: ${e.javaClass.simpleName}: ${e.message}")
            return
        }
        registration = next
        LogManager.addLog("INFO", "NET", "network activity listener started")
        val active = try {
            cm.isDefaultNetworkActive
        } catch (e: Exception) {
            LogManager.addLog("WARN", "NET", "network activity query failed: ${e.javaClass.simpleName}: ${e.message}")
            false
        }
        if (active) NativeLib.notifyNetworkActive()
    }

    /** Сначала запретить callbacks, затем снять listener у владельца регистрации; ошибку снятия логировать. */
    fun stop() {
        val current = registration ?: return
        registration = null
        try {
            current.manager.removeDefaultNetworkActiveListener(current.listener)
        } catch (e: Exception) {
            LogManager.addLog("WARN", "NET", "network activity listener removal failed: ${e.javaClass.simpleName}: ${e.message}")
        }
        LogManager.addLog("INFO", "NET", "network activity listener stopped")
    }
}
