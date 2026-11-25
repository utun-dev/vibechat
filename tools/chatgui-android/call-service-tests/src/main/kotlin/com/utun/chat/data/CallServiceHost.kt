package com.utun.chat.data

import android.content.Context
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Future
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first

/** Только DSP/camera/Telecom и чужие media. CallService/CallController/CoreActivity компилируются из приложения. */
object AppLanguage {
    var language = "en"
    var failText = false
    fun context() = com.utun.chat.ChatApplication.instance
    fun text(id: Int): String { check(!failText) { "language unavailable" }; return "$language:$id" }
}

class CallAudioEngine {
    val router = CallAudioRouter()
    var onFailure: ((Long, String) -> Unit)? = null
    var failures = emptySet<String>()
    var active = false
    var starts = 0; var stops = 0
    private var muted = false
    val diagnostics: String get() { check("diagnostics" !in failures); return "host active=$active" }
    fun start(id: Long): Boolean {
        starts++
        if ("reject" in failures) return false
        active = true
        check("start" !in failures) { "audio start failed after effect" }
        return true
    }
    fun stop() {
        stops++
        check("stop" !in failures) { "audio stop unavailable" }
        active = false
        check("stop after" !in failures) { "audio stop failed after effect" }
    }
    fun isActive() = active
    fun setMuted(value: Boolean) { muted = value }
    fun isMuted() = muted
    fun bindTelecom(connection: UtunConnection) {}
    fun syncAecForRoute(route: AudioRoute) {}
    fun outputLatency(id: Long) = CallOutputLatency()
}

data class CallVideoSettings(val enabled: Boolean = false, val screen: Boolean = false)
data class CallVideoState(val settings: CallVideoSettings = CallVideoSettings())
class CallVideoEngine(context: Context, val id: Long) {
    companion object { var failCreate = false; val created = mutableListOf<CallVideoEngine>() }
    init { check(!failCreate) { "video constructor unavailable" }; created.add(this) }
    var failures = emptySet<String>()
    var stopped = false; var stops = 0
    val state = MutableStateFlow(CallVideoState())
    var projection: ScreenProjection? = null
    val display = MutableStateFlow<Any?>(null)
    fun start() { check("start" !in failures) { "video startup unavailable" } }
    fun select(settings: CallVideoSettings) { state.value = CallVideoState(settings) }
    fun stop(): Boolean {
        stops++
        check("stop" !in failures) { "video stop unavailable" }
        stopped = true
        check("stop after" !in failures) { "video stop failed after effect" }
        return "pending" !in failures
    }
}

object TelecomCallManager {
    private val closing = MutableStateFlow(false)
    var pending: Boolean get() = closing.value; set(value) { closing.value = value }
    fun cleanupPending() = pending
    fun requireReleased() { check(!pending) { "Telecom connection cleanup pending" } }
    suspend fun awaitReleased() { closing.first { !it } }
    interface Listener {
        fun onConnectionAvailable(callId: Long, connection: UtunConnection)
        fun onAnswer(callId: Long)
        fun onReject(callId: Long)
        fun onDisconnect(callId: Long)
        fun onIncomingUiAllowed(callId: Long)
        fun onConnectionFailed(callId: Long)
    }
    fun setListener(listener: Listener) {}
    fun isReady() = false
    fun placeCall(id: Long, peer: String, name: String) = true
    fun connectionFor(id: Long): UtunConnection? = null
    fun addIncomingCall(id: Long, peer: String, name: String) = true
    fun setActive(id: Long) {}
    fun setDisconnected(id: Long, reason: Int) {}
    fun removeConnection(id: Long) {}
}

class ProximityManager(context: Context) {
    val near = MutableStateFlow<Boolean?>(null)
    fun start(thresholdCm: Float = 5f) {}
    fun stop() { near.value = null }
}
object RadioCallController { fun busy() = false }
object IncomingCallManager {
    const val EXTRA_CALL_ID = "call_id"
    const val CHANNEL_CALL = "incoming_call"
    fun show(id: Long, channel: String, peer: String) {}
    fun stopRinging() {}
}
object CallRingback { fun stop() {}; fun startConnecting() {}; fun startRinging() {} }
object MicrophonePermission {
    fun granted() = true
    fun request(reason: String, callback: () -> Unit = {}) { callback() }
}
class RadioAudioEngine {
    companion object { private val shared = RadioAudioEngine(); fun instance() = shared }
    val activeGroup = MutableStateFlow<String?>(null)
    val restored = mutableListOf<String>()
    fun stopForCall(): Future<*> { activeGroup.value = null; return CompletableFuture.completedFuture(Unit) }
    fun restoreAfterCall(group: String) { restored.add(group); activeGroup.value = group }
}
object StreamPlayerEngine { val viewing = MutableStateFlow(false); fun stopForCall() { viewing.value = false } }
object MediaAudioController { var beforeIdle: () -> Unit = {}; fun awaitIdle() { beforeIdle() } }
object AudioRecorderManager {
    var stopped: CompletableFuture<Unit> = CompletableFuture.completedFuture(Unit)
    fun stopAll() = listOf(stopped)
}
enum class MediaAudioProfile { AUTO, PHONE, WIRED, BLUETOOTH }
