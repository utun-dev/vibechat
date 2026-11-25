package com.utun.chat.data

import android.net.Uri
import android.os.Build
import android.os.OutcomeReceiver
import android.telecom.CallAudioState
import android.telecom.CallEndpoint
import android.telecom.CallEndpointException
import android.telecom.Connection
import android.telecom.DisconnectCause
import android.telecom.TelecomManager
import androidx.core.content.ContextCompat
import com.utun.chat.ChatApplication
import java.util.concurrent.CopyOnWriteArrayList

/**
 * Self-managed Telecom Connection для P2P-звонка uTun.
 *
 * Сам звонок (сигналинг + аудио) полностью ведёт приложение; Connection лишь
 * регистрирует вызов в Telecom для системного управления VoIP аудио. Фактический
 * маршрут подтверждают callbacks CallEndpoint (API 34+) или CallAudioState; успешный запрос не подменяет подтверждение.
 *
 * Владелец — TelecomCallManager (создаётся в UtunConnectionService, хранится в
 * реестре по callId). Ответ/отбой/разрыв приходят сюда из системного ввода
 * (например, кнопка BT-гарнитуры) и делегируются в CallController через TelecomCallManager.Listener.
 */
class UtunConnection(
    private val callId: Long,
    private val peer: String,
    displayName: String
) : Connection() {

    /** Доступные CallEndpoint'ы (для requestCallEndpointChange), обновляются системой. */
    private val availableEndpoints = CopyOnWriteArrayList<CallEndpoint>()
    var onRouteChanged: ((AudioRoute) -> Unit)? = null
    var onEndpointsChanged: (() -> Unit)? = null
    val headsetAvailable: Boolean? get() = if (Build.VERSION.SDK_INT >= 34) {
        if (availableEndpoints.isEmpty()) null else availableEndpoints.any {
            it.endpointType == CallEndpoint.TYPE_BLUETOOTH || it.endpointType == CallEndpoint.TYPE_WIRED_HEADSET
        }
    } else callAudioState?.let { it.supportedRouteMask and (CallAudioState.ROUTE_BLUETOOTH or CallAudioState.ROUTE_WIRED_HEADSET) != 0 }
    var confirmedRoute: AudioRoute? = null
        private set
    private var wantedRoute: AudioRoute? = null
    private var requestedEndpoint: android.os.ParcelUuid? = null
    private var requestSerial = 0L
    private var requestedLegacyRoute = 0
    private var lastRequestedRoute: AudioRoute? = null // SDK result может опередить callback фактического маршрута.
    private var ended = false

    init {
        setConnectionProperties(PROPERTY_SELF_MANAGED)
        setConnectionCapabilities(CAPABILITY_MUTE) // Hold не реализован приложением и не рекламируется Telecom.
        setAudioModeIsVoip(true)
        setAddress(Uri.fromParts("node", peer, null), TelecomManager.PRESENTATION_UNKNOWN)
        if (displayName.isNotEmpty()) setCallerDisplayName(displayName, TelecomManager.PRESENTATION_UNKNOWN)
        setInitializing()
        LogManager.addLog("INFO", "UtunConn",
            "created callId=0x%016x peer=%s name=%s".format(callId, peer, displayName))
    }

    override fun onAnswer() = systemInput("answer") { TelecomCallManager.notifyAnswer(callId) }

    override fun onReject() = systemInput("reject") { TelecomCallManager.notifyReject(callId) }

    override fun onDisconnect() = systemInput("disconnect") { TelecomCallManager.notifyDisconnect(callId) }

    override fun onAbort() = systemInput("abort") { TelecomCallManager.notifyAbort(callId) }
    override fun onShowIncomingCallUi() = systemInput("incoming UI") { TelecomCallManager.notifyIncomingUiAllowed(callId) }

    private fun systemInput(event: String, action: () -> Unit) {
        if (ended) { LogManager.addLog("WARN", "UtunConn", "late system input ignored id=$callId event=$event"); return }
        action()
    }

    override fun onStateChanged(state: Int) {
        LogManager.addLog("DEBUG", "UtunConn", "state -> %s".format(Connection.stateToString(state)))
    }

    override fun onCallAudioStateChanged(audioState: CallAudioState?) {
        if (ended) return
        LogManager.addLog("DEBUG", "UtunConn",
            "audioState route=%s muted=%s".format(audioState?.route ?: -1, audioState?.isMuted ?: false))
        if (Build.VERSION.SDK_INT < 34 && audioState != null) {
            val route = when (audioState.route) {
                CallAudioState.ROUTE_EARPIECE -> AudioRoute.EARPIECE
                CallAudioState.ROUTE_SPEAKER -> AudioRoute.SPEAKER
                CallAudioState.ROUTE_BLUETOOTH, CallAudioState.ROUTE_WIRED_HEADSET -> AudioRoute.HEADSET
                else -> null
            }
            if (route != null) confirmRoute(route)
            else LogManager.addLog("WARN", "UtunConn", "unrecognized audio route=${audioState.route}")
            notifyEndpointsChanged()
            wantedRoute?.let { routeTo(it) }
        }
    }

    override fun onCallEndpointChanged(endpoint: CallEndpoint) {
        if (ended) return
        val route = when (endpoint.endpointType) {
            CallEndpoint.TYPE_EARPIECE -> AudioRoute.EARPIECE
            CallEndpoint.TYPE_SPEAKER -> AudioRoute.SPEAKER
            CallEndpoint.TYPE_BLUETOOTH, CallEndpoint.TYPE_WIRED_HEADSET -> AudioRoute.HEADSET
            else -> null
        }
        if (route != null) confirmRoute(route, endpoint.identifier)
        else LogManager.addLog("WARN", "UtunConn", "unrecognized endpoint type=${endpoint.endpointType}")
    }

    private fun confirmRoute(route: AudioRoute, endpoint: android.os.ParcelUuid? = null) {
        confirmedRoute = route
        if (wantedRoute == route && (requestedEndpoint == null || requestedEndpoint == endpoint)) {
            wantedRoute = null; requestedLegacyRoute = 0
        }
        LogManager.addLog("INFO", "UtunConn", "route confirmed id=$callId route=$route pending=$wantedRoute")
        try { onRouteChanged?.invoke(route) } catch (e: Exception) {
            LogManager.addLog("ERROR", "UtunConn", "route confirmation callback failed id=$callId route=$route: $e")
        }
    }

    override fun onAvailableCallEndpointsChanged(availableEndpoints: MutableList<CallEndpoint>) {
        if (ended) return
        val changed = this.availableEndpoints.map { it.identifier to it.endpointType }.toSet() !=
            availableEndpoints.map { it.identifier to it.endpointType }.toSet()
        this.availableEndpoints.clear()
        this.availableEndpoints.addAll(availableEndpoints)
        if (changed) LogManager.addLog("INFO", "UtunConn",
            "endpoints id=$callId types=${availableEndpoints.map { it.endpointType }} headset=$headsetAvailable confirmed=$confirmedRoute")
        notifyEndpointsChanged()
        wantedRoute?.let { routeTo(it) }
    }

    private fun notifyEndpointsChanged() {
        try { onEndpointsChanged?.invoke() } catch (e: Exception) {
            LogManager.addLog("ERROR", "UtunConn", "endpoint list callback failed id=$callId: $e")
        }
    }

    /** Перевести вызов в звонящее состояние (входящий). */
    fun markRinging() = setRinging()

    /** Вызов пошёл (принят или исходящий дозвонился). */
    fun markActive() = setActive()

    /** Завершение с причиной (коды приложения: 0 — отбой, 2 — отклонено, 7 — ошибка). */
    fun markDisconnected(reason: Int) {
        ended = true; availableEndpoints.clear()
        wantedRoute = null; requestedEndpoint = null; requestedLegacyRoute = 0; lastRequestedRoute = null; ++requestSerial
        onRouteChanged = null; onEndpointsChanged = null
        val code = when (reason) {
            2 -> DisconnectCause.REJECTED
            7 -> DisconnectCause.ERROR
            else -> DisconnectCause.LOCAL
        }
        setDisconnected(DisconnectCause(code))
        LogManager.addLog("INFO", "UtunConn", "disconnected callId=0x%016x reason=%d".format(callId, reason))
    }

    /** Маршрутизация аудио через Telecom (CallEndpoint API на Android 14+, иначе setAudioRoute). */
    fun routeTo(route: AudioRoute) {
        if (ended) { LogManager.addLog("WARN", "UtunConn", "route after disconnect ignored id=$callId route=$route"); return }
        wantedRoute = route
        if (confirmedRoute == route && requestedEndpoint == null && requestedLegacyRoute == 0 &&
            (lastRequestedRoute == null || lastRequestedRoute == route)) { wantedRoute = null; return }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            if (requestedEndpoint != null) return // До ответа сохраняем только последнее желание пользователя.
            val ep = when (route) {
                AudioRoute.EARPIECE -> availableEndpoints.firstOrNull { it.endpointType == CallEndpoint.TYPE_EARPIECE }
                AudioRoute.SPEAKER -> availableEndpoints.firstOrNull { it.endpointType == CallEndpoint.TYPE_SPEAKER }
                AudioRoute.HEADSET -> availableEndpoints.firstOrNull { it.endpointType == CallEndpoint.TYPE_BLUETOOTH }
                    ?: availableEndpoints.firstOrNull { it.endpointType == CallEndpoint.TYPE_WIRED_HEADSET }
            }
            if (ep != null) {
                if (requestedEndpoint == ep.identifier) return
                requestedEndpoint = ep.identifier; lastRequestedRoute = route
                val serial = ++requestSerial
                try {
                    val executor = ContextCompat.getMainExecutor(ChatApplication.instance)
                    requestCallEndpointChange(ep, executor, object : OutcomeReceiver<Void, CallEndpointException> {
                        override fun onResult(result: Void?) {
                            if (requestSerial != serial || ended) { LogManager.addLog("DEBUG", "UtunConn", "stale endpoint result id=$callId serial=$serial"); return }
                            LogManager.addLog("INFO", "UtunConn", "endpoint request completed id=$callId type=${ep.endpointType} confirmed=$confirmedRoute")
                            requestedEndpoint = null
                            wantedRoute?.takeIf { it != route }?.let { routeTo(it) }
                        }
                        override fun onError(error: CallEndpointException) {
                            if (requestSerial != serial || ended) { LogManager.addLog("DEBUG", "UtunConn", "stale endpoint error id=$callId serial=$serial"); return }
                            LogManager.addLog("WARN", "UtunConn", "endpoint request failed id=$callId type=${ep.endpointType}: $error")
                            requestedEndpoint = null; lastRequestedRoute = null
                            val next = wantedRoute?.takeIf { it != route }
                            wantedRoute = null
                            next?.let { routeTo(it) }
                        }
                    })
                    return
                } catch (e: Exception) {
                    if (requestSerial == serial) { requestedEndpoint = null; wantedRoute = null; ++requestSerial }
                    LogManager.addLog("WARN", "UtunConn", "requestCallEndpointChange threw: %s".format(e.message))
                }
            } else {
                LogManager.addLog("WARN", "UtunConn", "route waiting for endpoint id=$callId requested=$route")
            }
            return // На API 34+ один путь управления: CallEndpoint, без параллельного legacy-запроса.
        }
        val supported = callAudioState?.supportedRouteMask ?: run {
            LogManager.addLog("WARN", "UtunConn", "route waiting for audio state id=$callId requested=$route")
            return
        }
        val stateRoute = when (route) {
            AudioRoute.EARPIECE -> CallAudioState.ROUTE_EARPIECE
            AudioRoute.SPEAKER -> CallAudioState.ROUTE_SPEAKER
            AudioRoute.HEADSET -> if (supported and CallAudioState.ROUTE_BLUETOOTH != 0)
                CallAudioState.ROUTE_BLUETOOTH else CallAudioState.ROUTE_WIRED_HEADSET
        }
        if (supported and stateRoute == 0) {
            wantedRoute = null
            LogManager.addLog("WARN", "UtunConn", "route unavailable id=$callId requested=$route supported=$supported")
            return
        }
        if (requestedLegacyRoute == stateRoute) return
        requestedLegacyRoute = stateRoute
        try {
            setAudioRoute(stateRoute)
            LogManager.addLog("INFO", "UtunConn", "audio route -> %d (route=%s)".format(stateRoute, route))
        } catch (e: Exception) {
            requestedLegacyRoute = 0; wantedRoute = null
            LogManager.addLog("WARN", "UtunConn", "setAudioRoute failed: %s".format(e.message))
        }
    }

}
