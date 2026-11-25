package com.utun.chat.data

import android.content.ComponentName
import android.content.Context
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.telecom.PhoneAccount
import android.telecom.PhoneAccountHandle
import android.telecom.TelecomManager
import com.utun.chat.ChatApplication
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first

/**
 * Мост между жизненным циклом P2P-звонка (CallController) и Telecom.
 *
 * - Регистрирует self-managed PhoneAccount (делает вызовы uTun «настоящими» для системы).
 * - placeCall/addIncomingCall создают Connection через UtunConnectionService.
 * - Хранит Connection по callId и прокидывает state/route из контроллера в Connection.
 * - Обратные события (answer/reject/disconnect из системного ввода) — через Listener в контроллер.
 *
 * init/изменения реестра — main, lookup — также workers. Отклонённые/удалённые connections сохраняются
 * до успешного destroy. Отсутствие Telecom возвращает false из запросов; решение о fallback/завершении принимает controller.
 */
object TelecomCallManager {

    interface Listener {
        fun onAnswer(callId: Long)
        fun onReject(callId: Long)
        fun onDisconnect(callId: Long)
        fun onConnectionAvailable(callId: Long, connection: UtunConnection)
        fun onIncomingUiAllowed(callId: Long)
        fun onConnectionFailed(callId: Long)
    }

    const val EXTRA_CALL_ID = "com.utun.chat.extra.CALL_ID"
    const val EXTRA_PEER_ADDRESS = "com.utun.chat.extra.PEER_ADDRESS"
    const val EXTRA_DISPLAY_NAME = "com.utun.chat.extra.DISPLAY_NAME"

    private const val PHONE_ACCOUNT_ID = "utun_p2p_call"
    private const val TAG = "TelecomCall"

    @Volatile private var listener: Listener? = null

    private val connections = ConcurrentHashMap<Long, UtunConnection>()
    private data class Closing(val id: Long, val reason: Int, var disconnected: Boolean = false)
    private val closing = linkedMapOf<UtunConnection, Closing>() // Main: сильные refs до ACK destroy, включая late/duplicate connections.
    private val reasons = mutableMapOf<UtunConnection, Int>()
    private val disconnected = mutableSetOf<UtunConnection>()
    private var cleanupQueued = false
    private var cleanupDelay = 250L
    private val retiring = MutableStateFlow(false)
    private val cleanupRetry = Runnable { cleanupQueued = false; cleanupConnections() }

    @Volatile private var phoneAccountHandle: PhoneAccountHandle? = null
    @Volatile private var ready = false

    private val mainHandler = Handler(Looper.getMainLooper())

    fun setListener(l: Listener?) { listener = l }

    /** Инициализация из ChatApplication.onCreate: регистрирует PhoneAccount. Идемпотентно. */
    fun init() {
        checkMain()
        if (ready) return
        try {
            val ctx = ChatApplication.instance
            val tm = ctx.getSystemService(Context.TELECOM_SERVICE) as? TelecomManager ?: error("TelecomManager unavailable")
            val handle = PhoneAccountHandle(ComponentName(ctx, UtunConnectionService::class.java), PHONE_ACCOUNT_ID)
            /* registerPhoneAccount идемпотентен (перезапись по handle) и для self-managed
             * аккаунта использует MANAGE_OWN_CALLS. Не читаем account через дополнительные телефонные разрешения. */
            val account = PhoneAccount.builder(handle, "vibechat")
                .setCapabilities(PhoneAccount.CAPABILITY_SELF_MANAGED)
                .build()
            tm.registerPhoneAccount(account)
            phoneAccountHandle = handle
            LogManager.addLog("INFO", TAG, "phone account registered")
            ready = true
        } catch (e: Exception) {
            LogManager.addLog("ERROR", TAG, "phone account registration failed: %s".format(e.message))
        }
    }

    fun isReady(): Boolean = ready

    /** Не передавать глобальную аудиополитику другому media/core, пока SDK удерживает старый Connection. */
    fun cleanupPending(): Boolean = retiring.value

    /** Ожидание не отменяет SDK cleanup; восстановление рации ждёт подтверждённого освобождения Telecom. */
    suspend fun awaitReleased() { retiring.first { !it } }

    fun requireReleased() {
        checkMain(); cleanupConnections()
        check(!retiring.value) {
            LogManager.addLog("ERROR", TAG, "Telecom cleanup still pending connections=${closing.size}")
            "Telecom connection cleanup pending"
        }
    }

    /** Создать исходящий вызов в Telecom. Возвращает false, если Telecom недоступен. */
    fun placeCall(callId: Long, peer: String, displayName: String): Boolean {
        val tm = telecomManager() ?: return false
        val handle = phoneAccountHandle ?: return false
        val uri = Uri.fromParts("node", peer, null)
        val callExtras = Bundle().apply {
            putLong(EXTRA_CALL_ID, callId)
            putString(EXTRA_PEER_ADDRESS, peer)
            putString(EXTRA_DISPLAY_NAME, displayName)
        }
        val extras = Bundle().apply {
            putBundle(TelecomManager.EXTRA_OUTGOING_CALL_EXTRAS, callExtras)
            putParcelable(TelecomManager.EXTRA_PHONE_ACCOUNT_HANDLE, handle)
        }
        return try {
            tm.placeCall(uri, extras)
            LogManager.addLog("INFO", TAG, "placeCall id=0x%016x peer=%s".format(callId, peer))
            true
        } catch (e: Exception) {
            LogManager.addLog("WARN", TAG, "placeCall failed id=$callId peer=$peer: $e")
            false
        }
    }

    /** Зарегистрировать входящий вызов в Telecom. Возвращает false, если Telecom недоступен. */
    fun addIncomingCall(callId: Long, peer: String, displayName: String): Boolean {
        val tm = telecomManager() ?: return false
        val handle = phoneAccountHandle ?: return false
        val extras = Bundle().apply {
            putLong(EXTRA_CALL_ID, callId)
            putString(EXTRA_PEER_ADDRESS, peer)
            putString(EXTRA_DISPLAY_NAME, displayName)
        }
        return try {
            tm.addNewIncomingCall(handle, extras)
            LogManager.addLog("INFO", TAG, "addIncomingCall id=0x%016x peer=%s".format(callId, peer))
            true
        } catch (e: Exception) {
            LogManager.addLog("WARN", TAG, "addIncomingCall failed id=$callId peer=$peer: $e")
            false
        }
    }

    /* ── реестр Connection (вызывается из UtunConnectionService) ── */

    fun registerConnection(callId: Long, conn: UtunConnection): Boolean {
        checkMain()
        val st = CallController.state.value
        if (st == null || st.callId != callId || !st.ongoing) {
            LogManager.addLog("WARN", TAG, "late connection for ended/stale call id=$callId current=${st?.callId}")
            retire(callId, conn, 0); return false
        }
        val previous = connections.putIfAbsent(callId, conn)
        if (previous != null) {
            if (previous === conn) return true
            LogManager.addLog("WARN", TAG, "duplicate connection rejected id=$callId; original retained")
            retire(callId, conn, 0); return false
        }
        try {
            when (st.phase) {
                CallPhase.ACTIVE -> conn.markActive()
                CallPhase.INCOMING, CallPhase.PENDING_INCOMING -> conn.markRinging()
                else -> conn.setDialing()
            }
            LogManager.addLog("INFO", TAG, "connection registered id=$callId phase=${st.phase}")
            listener?.onConnectionAvailable(callId, conn)
            return true
        } catch (e: Exception) {
            LogManager.addLog("ERROR", TAG, "connection registration failed id=$callId; retiring owner: $e")
            connections.remove(callId, conn); retire(callId, conn, 7); notifyConnectionFailed(callId)
            return false
        }
    }

    fun connectionFor(callId: Long): UtunConnection? = connections[callId]

    /* ── события из Connection (системный ввод) → контроллер ── */

    fun notifyAnswer(callId: Long) = post { listener?.onAnswer(callId) }

    fun notifyReject(callId: Long) = post { listener?.onReject(callId) }

    fun notifyDisconnect(callId: Long) = post { listener?.onDisconnect(callId) }

    fun notifyAbort(callId: Long) = post { listener?.onDisconnect(callId) }
    fun notifyIncomingUiAllowed(callId: Long) = post { listener?.onIncomingUiAllowed(callId) }
    fun notifyConnectionFailed(callId: Long) = post { listener?.onConnectionFailed(callId) }

    /* ── управление состоянием из контроллера ── */

    fun setRinging(callId: Long) {
        checkMain()
        try { connections[callId]?.markRinging() } catch (e: Exception) { LogManager.addLog("ERROR", TAG, "ringing state failed id=$callId: $e") }
    }

    fun setActive(callId: Long) {
        checkMain()
        try { connections[callId]?.markActive() } catch (e: Exception) { LogManager.addLog("ERROR", TAG, "active state failed id=$callId: $e") }
    }

    fun setDisconnected(callId: Long, reason: Int) {
        checkMain()
        val conn = connections[callId] ?: return
        reasons[conn] = reason
        try { conn.markDisconnected(reason); disconnected.add(conn) } catch (e: Exception) {
            LogManager.addLog("ERROR", TAG, "disconnected state failed id=$callId; owner retained until removal: $e")
        }
    }

    fun removeConnection(callId: Long) {
        checkMain()
        val conn = connections.remove(callId)
        if (conn != null) retire(callId, conn, reasons[conn] ?: 0) else cleanupConnections()
        LogManager.addLog("DEBUG", TAG, "connection removal requested id=$callId pending=${retiring.value}")
    }

    private fun retire(callId: Long, conn: UtunConnection, reason: Int) {
        closing.putIfAbsent(conn, Closing(callId, reason, conn in disconnected))
        retiring.value = true
        LogManager.addLog("DEBUG", TAG, "connection retiring id=$callId owner=${System.identityHashCode(conn)} reason=$reason")
        reasons.remove(conn); disconnected.remove(conn); cleanupConnections()
    }

    private fun cleanupConnections() {
        for ((conn, owner) in closing.toMap()) {
            if (!owner.disconnected) try { conn.markDisconnected(owner.reason); owner.disconnected = true } catch (e: Exception) {
                LogManager.addLog("ERROR", TAG, "retiring disconnect failed id=${owner.id}; attempting destroy independently: $e")
            }
            try {
                conn.destroy(); closing.remove(conn)
                LogManager.addLog("INFO", TAG, "connection destroy confirmed id=${owner.id} owner=${System.identityHashCode(conn)}")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", TAG, "connection destroy failed id=${owner.id} owner=${System.identityHashCode(conn)}; retained: $e")
            }
        }
        retiring.value = closing.isNotEmpty()
        if (!retiring.value) { mainHandler.removeCallbacks(cleanupRetry); cleanupQueued = false; cleanupDelay = 250; return }
        if (!cleanupQueued) {
            cleanupQueued = mainHandler.postDelayed(cleanupRetry, cleanupDelay)
            if (!cleanupQueued) LogManager.addLog("ERROR", TAG, "connection cleanup retry rejected count=${closing.size}; owners retained")
            else { LogManager.addLog("DEBUG", TAG, "connection cleanup scheduled count=${closing.size} delayMs=$cleanupDelay"); cleanupDelay = (cleanupDelay * 2).coerceAtMost(30_000) }
        }
    }

    /* ── helpers ── */

    private fun checkMain() = check(Looper.myLooper() == Looper.getMainLooper()) {
        LogManager.addLog("ERROR", TAG, "Telecom owner mutation outside main")
        "Telecom owner mutation requires main"
    }

    private fun telecomManager(): TelecomManager? {
        if (!ready) { LogManager.addLog("WARN", TAG, "Telecom request before account registration"); return null }
        return try {
            (ChatApplication.instance.getSystemService(Context.TELECOM_SERVICE) as? TelecomManager).also {
                if (it == null) LogManager.addLog("ERROR", TAG, "TelecomManager unavailable after account registration")
            }
        } catch (e: Exception) {
            LogManager.addLog("WARN", TAG, "getTelecomManager failed: %s".format(e.message))
            null
        }
    }

    private fun post(r: () -> Unit) {
        if (!mainHandler.post {
            try { r() } catch (e: Exception) { LogManager.addLog("ERROR", TAG, "Telecom listener callback failed: $e") }
        }) LogManager.addLog("ERROR", TAG, "Telecom callback post rejected")
    }
}
