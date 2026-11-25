package android.telecom

import android.content.ComponentName
import android.net.Uri
import android.os.Bundle
import android.os.OutcomeReceiver
import android.os.ParcelUuid
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executor

data class PhoneAccountHandle(val component: ComponentName, val id: String)
class PhoneAccount(val handle: PhoneAccountHandle) {
    companion object { const val CAPABILITY_SELF_MANAGED = 1; fun builder(handle: PhoneAccountHandle, name: String) = Builder(handle) }
    class Builder(private val handle: PhoneAccountHandle) { fun setCapabilities(flags: Int) = this; fun build() = PhoneAccount(handle) }
}
class TelecomManager {
    companion object {
        const val PRESENTATION_UNKNOWN = 3; const val EXTRA_OUTGOING_CALL_EXTRAS = "outgoing"; const val EXTRA_PHONE_ACCOUNT_HANDLE = "account"
        var shared: TelecomManager? = TelecomManager(); var failLookup = false
    }
    var failures = emptySet<String>()
    val accounts = mutableListOf<PhoneAccount>()
    val outgoing = mutableListOf<Pair<Uri, Bundle>>()
    val incoming = mutableListOf<Pair<PhoneAccountHandle, Bundle>>()
    fun registerPhoneAccount(account: PhoneAccount) { check("register" !in failures); accounts.add(account); check("register after" !in failures) }
    fun placeCall(uri: Uri, extras: Bundle) { check("place" !in failures); outgoing.add(uri to extras); check("place after" !in failures) }
    fun addNewIncomingCall(handle: PhoneAccountHandle, extras: Bundle) { check("incoming" !in failures); incoming.add(handle to extras) }
}
data class CallEndpoint(val name: String, val endpointType: Int, val identifier: ParcelUuid) {
    companion object { const val TYPE_EARPIECE = 1; const val TYPE_SPEAKER = 2; const val TYPE_BLUETOOTH = 3; const val TYPE_WIRED_HEADSET = 4 }
}
class CallEndpointException(message: String) : Exception(message)
class CallAudioState(val route: Int, val supportedRouteMask: Int, val isMuted: Boolean = false) {
    companion object { const val ROUTE_EARPIECE = 1; const val ROUTE_BLUETOOTH = 2; const val ROUTE_WIRED_HEADSET = 4; const val ROUTE_SPEAKER = 8 }
}
class DisconnectCause(val code: Int) { companion object { const val REJECTED = 1; const val ERROR = 2; const val LOCAL = 3 } }

/** Реальные Connection callbacks и запросы управляются тестом; реального Telecom IPC/аудиоустройств нет. */
open class Connection {
    companion object {
        const val PROPERTY_SELF_MANAGED = 1; const val CAPABILITY_SUPPORT_HOLD = 2; const val CAPABILITY_MUTE = 4
        val created = CopyOnWriteArrayList<Connection>()
        var nextFailures = emptySet<String>()
        fun stateToString(state: Int) = state.toString()
        fun createCanceledConnection() = Connection().also { it.cancelled = true }
        fun createFailedConnection(cause: DisconnectCause) = Connection().also { it.disconnectCause = cause }
    }
    var failures = nextFailures
    var destroyed = false
        private set
    var cancelled = false
    var disconnectCause: DisconnectCause? = null
    var state = 0
        private set
    var callAudioState: CallAudioState? = null
    val legacyRequests = mutableListOf<Int>()
    data class EndpointRequest(val endpoint: CallEndpoint, val executor: Executor, val outcome: OutcomeReceiver<Void, CallEndpointException>) {
        fun succeed() { executor.execute { outcome.onResult(null) } }
        fun fail() { executor.execute { outcome.onError(CallEndpointException("endpoint failed")) } }
    }
    val endpointRequests = mutableListOf<EndpointRequest>()
    init { created.add(this) }
    private fun access(name: String) { check(name !in failures) { "Connection $name failed" } }
    fun setConnectionProperties(value: Int) { access("properties") }
    fun setConnectionCapabilities(value: Int) { access("capabilities") }
    fun setAudioModeIsVoip(value: Boolean) { access("voip") }
    fun setAddress(uri: Uri, presentation: Int) { access("address") }
    fun setCallerDisplayName(name: String, presentation: Int) { access("name") }
    private fun update(value: Int, name: String) { access(name); state = value; onStateChanged(value); access("$name after") }
    fun setInitializing() = update(1, "initializing")
    fun setDialing() = update(2, "dialing")
    fun setRinging() = update(3, "ringing")
    fun setActive() = update(4, "active")
    fun setDisconnected(cause: DisconnectCause) { access("disconnected"); disconnectCause = cause; update(5, "disconnected") }
    fun destroy() { access("destroy"); destroyed = true; access("destroy after") }
    fun setAudioRoute(route: Int) { access("route"); legacyRequests.add(route); access("route after") }
    fun requestCallEndpointChange(endpoint: CallEndpoint, executor: Executor, outcome: OutcomeReceiver<Void, CallEndpointException>) {
        access("endpoint"); endpointRequests.add(EndpointRequest(endpoint, executor, outcome)); access("endpoint after")
    }
    open fun onAnswer() {}
    open fun onReject() {}
    open fun onDisconnect() {}
    open fun onAbort() {}
    open fun onShowIncomingCallUi() {}
    open fun onStateChanged(state: Int) {}
    open fun onCallAudioStateChanged(state: CallAudioState?) {}
    open fun onCallEndpointChanged(endpoint: CallEndpoint) {}
    open fun onAvailableCallEndpointsChanged(endpoints: MutableList<CallEndpoint>) {}
}
class ConnectionRequest(val extras: Bundle?)
open class ConnectionService {
    open fun onCreateIncomingConnectionFailed(account: PhoneAccountHandle?, request: ConnectionRequest?) {}
    open fun onCreateOutgoingConnectionFailed(account: PhoneAccountHandle?, request: ConnectionRequest?) {}
    open fun onCreateIncomingConnection(account: PhoneAccountHandle?, request: ConnectionRequest?): Connection? = null
    open fun onCreateOutgoingConnection(account: PhoneAccountHandle?, request: ConnectionRequest?): Connection? = null
}
