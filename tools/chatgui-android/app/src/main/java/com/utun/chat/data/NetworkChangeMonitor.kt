package com.utun.chat.data

import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.os.Handler
import android.os.Looper
import com.utun.chat.ChatApplication
import kotlinx.coroutines.runBlocking

/**
 * Объединяет события ConnectivityManager за 500 мс и передаёт снимок сокетов в JNI без рестарта ядра.
 * start/stop/onCoreStarted и состояние очереди — main thread; подготовка конфига — один worker даже через stop/start.
 * stop снимает регистрацию и запрещает публикацию; начатый IO может завершиться. SERVICE_STARTED инвалидирует прежний снимок.
 * true от JNI означает передачу строки, не подтверждение применения: parse/reload выполняется в uasync и логируется там.
 */
object NetworkChangeMonitor {
    private var registration: Registration? = null
    private var coreEpoch = Any()
    private var updating = false
    private var pending = false
    private var lastInterfaces: List<Pair<String, String>>? = null // Наблюдавшиеся интерфейсы при подготовке; не ACK применения.
    private const val DEBOUNCE_MS = 500L
    private val handler = Handler(Looper.getMainLooper())
    private val updateRunnable = Runnable { updateSockets() }

    private class Registration(val manager: ConnectivityManager) {
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = schedule(this@Registration)
            override fun onLost(network: Network) = schedule(this@Registration)
            override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) = schedule(this@Registration)
        }
    }

    private fun schedule(owner: Registration) {
        handler.post {
            if (registration !== owner) return@post
            handler.removeCallbacks(updateRunnable)
            handler.postDelayed(updateRunnable, DEBOUNCE_MS)
        }
    }

    private fun updateSockets() {
        val owner = registration ?: return
        if (!NativeLib.isReady()) {
            LogManager.addLog("DEBUG", "NET", "socket snapshot deferred: core not ready")
            return
        }
        if (updating) {
            if (!pending) LogManager.addLog("DEBUG", "NET", "socket snapshot pending: previous config worker running")
            pending = true
            return
        }
        updating = true
        pending = false
        val token = coreEpoch
        LogManager.addLog("DEBUG", "NET", "network config preparation started")
        try {
            Thread({
                var interfaces: List<Pair<String, String>>? = null
                var config: String? = null
                try {
                    val provider = ChatApplication.instance.configProvider
                    interfaces = provider.getAvailableInterfaces()
                    config = runBlocking { provider.buildConfigText().await() }
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "NET", "network config update failed: ${e.javaClass.simpleName}: ${e.message}")
                } finally {
                    val current = interfaces
                    val text = config
                    handler.post {
                        // stop не сбрасывает updating: следующий жизненный цикл ждёт этот единственный worker.
                        updating = false
                        val replaced = registration !== owner
                        val restarted = coreEpoch !== token
                        val stale = replaced || restarted
                        if (stale) {
                            LogManager.addLog("DEBUG", "NET", "retired network config discarded: registration=$replaced core=$restarted")
                        } else if (text != null && current != null && NativeLib.isReady()) {
                            val previous = lastInterfaces?.joinToString(", ") { (n, ip) -> "$n=$ip" } ?: "n/a"
                            val next = current.joinToString(", ") { (n, ip) -> "$n=$ip" }
                            NativeLib.notifyNetworkActive()
                            if (NativeLib.updateSockets(text)) {
                                lastInterfaces = current
                                LogManager.addLog("INFO", "NET", "socket config handed to JNI: chars=${text.length}; " +
                                    "observed interfaces: [$previous] -> [$next]")
                            } else LogManager.addLog("ERROR", "NET", "socket update rejected by JNI")
                        } else if (text != null && current != null) {
                            LogManager.addLog("DEBUG", "NET", "prepared network config discarded: core not ready")
                        }
                        val active = registration
                        if (active != null && (pending || stale)) {
                            pending = false
                            schedule(active)
                        }
                    }
                }
            }, "utun-network-config").start()
        } catch (e: Exception) {
            updating = false
            LogManager.addLog("ERROR", "NET", "network config worker launch failed: ${e.javaClass.simpleName}: ${e.message}")
        }
    }

    /** Повторный снимок после SERVICE_STARTED; старый worker завершится, но его конфиг в новое ядро не передаётся. */
    fun onCoreStarted() {
        coreEpoch = Any()
        LogManager.addLog("DEBUG", "NET", "socket snapshot invalidated: core started")
        registration?.let { schedule(it) }
    }

    /** Жизненный цикл HeadlessService: зарегистрировать ровно один callback, false — ошибка с возможностью повтора. */
    fun start(): Boolean {
        if (registration != null) return true
        val cm = ChatApplication.instance.getSystemService(ConnectivityManager::class.java)
        if (cm == null) { LogManager.addLog("ERROR", "NET", "ConnectivityManager unavailable"); return false }
        val next = Registration(cm)
        try {
            cm.registerDefaultNetworkCallback(next.callback)
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "NET", "network listener registration failed: ${e.javaClass.simpleName}: ${e.message}")
            return false
        }
        registration = next
        LogManager.addLog("INFO", "NET", "network change listener started")
        return true
    }

    /** Запретить публикацию/поздние callbacks; уже запущенная подготовка IO не прерывается и не дублируется новым start. */
    fun stop() {
        val current = registration ?: return
        registration = null
        pending = false
        lastInterfaces = null
        handler.removeCallbacks(updateRunnable)
        try {
            current.manager.unregisterNetworkCallback(current.callback)
        } catch (e: Exception) {
            LogManager.addLog("WARN", "NET", "network listener removal failed: ${e.javaClass.simpleName}: ${e.message}")
        }
        LogManager.addLog("INFO", "NET", "network change listener stopped")
    }
}
