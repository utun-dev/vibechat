package com.utun.chat.data

import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.telecom.CallAudioState
import android.telecom.CallEndpoint
import android.telecom.Connection
import android.telecom.ConnectionRequest
import android.telecom.TelecomManager
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие manager/Connection/ConnectionService, управляемые SDK IPC и endpoint outcomes. */
class TelecomLifecycleTest {
    private val owned = mutableListOf<Pair<Long, UtunConnection>>()
    private val events = mutableListOf<String>()
    private val main get() = Looper.getMainLooper()
    private val earpiece = endpoint(CallEndpoint.TYPE_EARPIECE)
    private val speaker = endpoint(CallEndpoint.TYPE_SPEAKER)
    private var failAvailable = false
    private val listener = object : TelecomCallManager.Listener {
        override fun onAnswer(callId: Long) { events.add("answer:$callId") }
        override fun onReject(callId: Long) { events.add("reject:$callId") }
        override fun onDisconnect(callId: Long) { events.add("disconnect:$callId") }
        override fun onIncomingUiAllowed(callId: Long) { events.add("ui:$callId") }
        override fun onConnectionFailed(callId: Long) { events.add("failed:$callId") }
        override fun onConnectionAvailable(callId: Long, connection: UtunConnection) {
            check(!failAvailable) { "listener unavailable" }; events.add("available:$callId")
        }
    }
    @Before fun prepare() {
        Build.VERSION.SDK_INT = 36; Handler.rejectImmediate = false; Handler.rejectDelayed = false
        TelecomManager.shared = TelecomManager(); TelecomManager.failLookup = false; Connection.nextFailures = emptySet(); Connection.created.clear()
        for (name in listOf("ready", "phoneAccountHandle")) TelecomCallManager::class.java.getDeclaredField(name)
            .apply { isAccessible = true }.set(TelecomCallManager, if (name == "ready") false else null)
        TelecomCallManager.setListener(listener); CallController.state.value = null; LogManager.entries.clear()
    }
    @After fun release() {
        Handler.rejectImmediate = false; Handler.rejectDelayed = false; Build.VERSION.SDK_INT = 36
        TelecomCallManager.setListener(null); Connection.created.forEach { it.failures = emptySet() }
        owned.forEach { TelecomCallManager.removeConnection(it.first) }; main.advanceBy(30_000)
        assertTrue(owned.all { it.second.destroyed })
        val pending = TelecomCallManager::class.java.getDeclaredField("closing").apply { isAccessible = true }.get(TelecomCallManager) as Map<*, *>
        assertTrue(pending.isEmpty()); CallController.state.value = null
    }
    private fun endpoint(type: Int) = CallEndpoint("endpoint $type", type, ParcelUuid(java.util.UUID.randomUUID()))
    private fun connection(id: Long = 17) = UtunConnection(id, "ab".repeat(32), "peer").also { owned.add(id to it) }
    private fun current(id: Long = 17, phase: CallPhase = CallPhase.ACTIVE) { CallController.state.value = HostCallState(id, phase) }
    private fun registered(id: Long = 17): UtunConnection { current(id); return connection(id).also { assertTrue(TelecomCallManager.registerConnection(id, it)) } }
    private fun routed() = registered().also { it.onAvailableCallEndpointsChanged(mutableListOf(earpiece, speaker)); it.onCallEndpointChanged(earpiece) }

    @Test fun accountLookupAndRegistrationFailuresRemainRetryable() {
        TelecomManager.failLookup = true; TelecomCallManager.init(); assertFalse(TelecomCallManager.isReady())
        TelecomManager.failLookup = false; TelecomManager.shared!!.failures = setOf("register after")
        TelecomCallManager.init(); assertFalse(TelecomCallManager.isReady())
        TelecomManager.shared!!.failures = emptySet(); TelecomCallManager.init(); assertTrue(TelecomCallManager.isReady())
    }
    @Test fun outgoingSdkExtrasReachConnectionServiceWithUnsignedIdsUnchanged() {
        TelecomCallManager.init(); val id = Long.MIN_VALUE + 7; current(id, CallPhase.OUTGOING)
        assertTrue(TelecomCallManager.placeCall(id, "ab".repeat(32), "peer"))
        val extras = TelecomManager.shared!!.outgoing.single().second.values[TelecomManager.EXTRA_OUTGOING_CALL_EXTRAS] as Bundle
        val result = UtunConnectionService().onCreateOutgoingConnection(null, ConnectionRequest(extras))
        assertTrue(result is UtunConnection); owned.add(id to (result as UtunConnection))
        assertSame(result, TelecomCallManager.connectionFor(id)); assertEquals(2, result.state)
    }
    @Test fun sdkRequestFailuresReturnFalseWithoutCrashingTheCaller() {
        TelecomCallManager.init(); TelecomManager.shared!!.failures = setOf("place", "incoming")
        assertFalse(TelecomCallManager.placeCall(17, "ab".repeat(32), "peer")); assertFalse(TelecomCallManager.addIncomingCall(17, "ab".repeat(32), "peer"))
    }
    @Test fun duplicateRegistrationRetainsTheOriginalAndDestroysTheRejectedOwner() {
        val first = registered(); val duplicate = connection()
        assertFalse(TelecomCallManager.registerConnection(17, duplicate)); assertSame(first, TelecomCallManager.connectionFor(17))
        assertTrue(duplicate.destroyed); assertFalse(first.destroyed); assertEquals(listOf("available:17"), events)
    }
    @Test fun lateRegistrationIsRejectedAndRemainsOwnedIfDestroyFails() {
        val late = connection(); late.failures = setOf("destroy")
        assertFalse(TelecomCallManager.registerConnection(17, late)); assertFalse(late.destroyed)
        late.failures = emptySet(); main.advanceBy(250); assertTrue(late.destroyed)
    }
    @Test fun stateFailureDuringRegistrationRetiresTheConnectionAndReportsFailure() {
        current(); val conn = connection(); conn.failures = setOf("active")
        assertFalse(TelecomCallManager.registerConnection(17, conn)); main.advanceBy(0)
        assertTrue(conn.destroyed); assertNull(TelecomCallManager.connectionFor(17)); assertTrue("failed:17" in events)
    }
    @Test fun availableListenerFailureDoesNotLoseTheRegisteredConnection() {
        current(); val conn = connection(); failAvailable = true
        assertFalse(TelecomCallManager.registerConnection(17, conn)); assertTrue(conn.destroyed)
        main.advanceBy(0); assertTrue("failed:17" in events)
    }
    @Test fun disconnectFailureCannotSkipIndependentDestroy() {
        val conn = registered(); conn.failures = setOf("disconnected")
        TelecomCallManager.setDisconnected(17, 2); TelecomCallManager.removeConnection(17)
        assertTrue(conn.destroyed); assertNull(TelecomCallManager.connectionFor(17))
    }
    @Test fun rejectedRetryTimerRetainsTheOwnerForExplicitRemovalRetry() {
        val conn = registered(); conn.failures = setOf("destroy"); Handler.rejectDelayed = true
        TelecomCallManager.setDisconnected(17, 2); TelecomCallManager.removeConnection(17); assertFalse(conn.destroyed)
        assertTrue(TelecomCallManager.cleanupPending())
        assertThrows(IllegalStateException::class.java) { TelecomCallManager.requireReleased() }
        conn.failures = emptySet(); Handler.rejectDelayed = false; TelecomCallManager.removeConnection(17); assertTrue(conn.destroyed)
        TelecomCallManager.requireReleased(); assertFalse(TelecomCallManager.cleanupPending())
    }
    @Test fun destroyFailureAfterEffectStillNeedsAnAcknowledgedRetry() {
        val conn = registered(); conn.failures = setOf("destroy after")
        TelecomCallManager.removeConnection(17); assertTrue(conn.destroyed)
        conn.failures = emptySet(); main.advanceBy(250); TelecomCallManager.removeConnection(17)
    }
    @Test fun systemInputAndRouteRequestsAfterDisconnectAreIgnored() {
        val conn = routed(); TelecomCallManager.setDisconnected(17, 0)
        conn.onAnswer(); conn.onReject(); conn.onDisconnect(); conn.onAbort(); conn.onShowIncomingCallUi(); conn.routeTo(AudioRoute.SPEAKER)
        main.advanceBy(0); assertEquals(listOf("available:17"), events); assertTrue(conn.endpointRequests.isEmpty())
    }
    @Test fun endpointResultDoesNotOptimisticallyConfirmTheAudioRoute() {
        val conn = routed(); conn.routeTo(AudioRoute.SPEAKER); conn.endpointRequests.single().succeed(); main.advanceBy(0)
        assertEquals(AudioRoute.EARPIECE, conn.confirmedRoute)
        conn.onCallEndpointChanged(speaker); assertEquals(AudioRoute.SPEAKER, conn.confirmedRoute)
    }
    @Test fun latestDesiredRouteIsSentAfterTheOutstandingEndpointRequestCompletes() {
        val conn = routed(); conn.routeTo(AudioRoute.SPEAKER); conn.routeTo(AudioRoute.EARPIECE)
        assertEquals(1, conn.endpointRequests.size); conn.endpointRequests.first().succeed(); main.advanceBy(0)
        assertEquals(2, conn.endpointRequests.size); assertEquals(earpiece, conn.endpointRequests.last().endpoint)
        conn.onCallEndpointChanged(speaker); conn.endpointRequests.last().succeed(); main.advanceBy(0); conn.onCallEndpointChanged(earpiece)
        assertEquals(AudioRoute.EARPIECE, conn.confirmedRoute)
    }
    @Test fun outcomeAfterDisconnectCannotRequestAnotherEndpoint() {
        val conn = routed(); conn.routeTo(AudioRoute.SPEAKER); conn.routeTo(AudioRoute.EARPIECE)
        TelecomCallManager.setDisconnected(17, 0); conn.endpointRequests.single().succeed(); main.advanceBy(0)
        assertEquals(1, conn.endpointRequests.size)
    }
    @Test fun routeListenerFailuresDoNotEscapeIntoTheSdk() {
        val conn = registered(); conn.onRouteChanged = { error("route callback failed") }; conn.onEndpointsChanged = { error("endpoints callback failed") }
        conn.onAvailableCallEndpointsChanged(mutableListOf(earpiece)); conn.onCallEndpointChanged(earpiece)
        assertEquals(AudioRoute.EARPIECE, conn.confirmedRoute)
    }
    @Test fun legacyRouteWaitsForAudioStateAndThenUsesTheSupportedHeadset() {
        Build.VERSION.SDK_INT = 33; val conn = registered(); conn.routeTo(AudioRoute.HEADSET); assertTrue(conn.legacyRequests.isEmpty())
        conn.callAudioState = CallAudioState(CallAudioState.ROUTE_SPEAKER, CallAudioState.ROUTE_SPEAKER or CallAudioState.ROUTE_WIRED_HEADSET)
        conn.onCallAudioStateChanged(conn.callAudioState); assertEquals(listOf(CallAudioState.ROUTE_WIRED_HEADSET), conn.legacyRequests)
        conn.callAudioState = CallAudioState(CallAudioState.ROUTE_WIRED_HEADSET, 12); conn.onCallAudioStateChanged(conn.callAudioState)
        assertEquals(AudioRoute.HEADSET, conn.confirmedRoute)
    }

    @Test fun failedEndpointRequestDoesNotSendAnUnneededRestoreToTheUnchangedRoute() {
        val conn = routed(); conn.routeTo(AudioRoute.SPEAKER); conn.routeTo(AudioRoute.EARPIECE)
        conn.endpointRequests.single().fail(); main.advanceBy(0)
        assertEquals(1, conn.endpointRequests.size); assertEquals(AudioRoute.EARPIECE, conn.confirmedRoute)
    }
    @Test fun endpointConfirmationBeforeItsOutcomePreservesTheLatestDesiredRoute() {
        val conn = routed(); conn.routeTo(AudioRoute.SPEAKER); conn.routeTo(AudioRoute.EARPIECE)
        conn.onCallEndpointChanged(speaker); conn.endpointRequests.first().succeed(); main.advanceBy(0)
        assertEquals(2, conn.endpointRequests.size); assertEquals(earpiece, conn.endpointRequests.last().endpoint)
    }
    @Test fun outcomeFromAThrowingSdkRequestCannotClearANewerEndpointRequest() {
        val conn = routed(); conn.failures = setOf("endpoint after"); conn.routeTo(AudioRoute.SPEAKER)
        conn.failures = emptySet(); conn.routeTo(AudioRoute.EARPIECE); assertEquals(2, conn.endpointRequests.size)
        conn.endpointRequests.first().succeed(); main.advanceBy(0)
        val pending = UtunConnection::class.java.getDeclaredField("requestedEndpoint").apply { isAccessible = true }.get(conn)
        assertEquals(earpiece.identifier, pending)
    }
    @Test fun routeWaitsForAMissingEndpointAndUsesItWhenTheListChanges() {
        val conn = registered(); conn.onAvailableCallEndpointsChanged(mutableListOf(earpiece)); conn.routeTo(AudioRoute.SPEAKER)
        assertTrue(conn.endpointRequests.isEmpty())
        conn.onAvailableCallEndpointsChanged(mutableListOf(earpiece, speaker)); assertEquals(1, conn.endpointRequests.size)
    }
    @Test fun malformedServiceIdentityCancelsTheConnectionAndReportsItsKnownCallId() {
        current(); val extras = Bundle().apply { putLong(TelecomCallManager.EXTRA_CALL_ID, 17) }
        val result = UtunConnectionService().onCreateIncomingConnection(null, ConnectionRequest(extras))
        main.advanceBy(0); assertTrue(result!!.cancelled); assertTrue("failed:17" in events)
    }
    @Test fun undecodableExtrasCannotEscapeFromConnectionServiceCallbacks() {
        val request = ConnectionRequest(Bundle().apply { failReads = true }); val service = UtunConnectionService()
        val result = service.onCreateOutgoingConnection(null, request)
        service.onCreateIncomingConnectionFailed(null, request); service.onCreateOutgoingConnectionFailed(null, request)
        main.advanceBy(0); assertTrue(result!!.cancelled); assertEquals(listOf("failed:0", "failed:0", "failed:0"), events)
    }
    @Test fun constructorFailureReturnsAFailedSdkConnectionAndNotifiesTheController() {
        current(); Connection.nextFailures = setOf("properties")
        val extras = Bundle().apply { putLong(TelecomCallManager.EXTRA_CALL_ID, 17); putString(TelecomCallManager.EXTRA_PEER_ADDRESS, "ab".repeat(32)) }
        val result = UtunConnectionService().onCreateOutgoingConnection(null, ConnectionRequest(extras))
        main.advanceBy(0); assertEquals(android.telecom.DisconnectCause.ERROR, result!!.disconnectCause!!.code)
        assertTrue("failed:17" in events)
    }
    @Test fun registryMutationsOutsideMainFailBeforeTouchingItsOwner() {
        val conn = registered(); val error = java.util.concurrent.atomic.AtomicReference<Throwable>()
        Thread { try { TelecomCallManager.removeConnection(17) } catch (e: Throwable) { error.set(e) } }.apply { start(); join() }
        assertTrue(error.get() is IllegalStateException); assertSame(conn, TelecomCallManager.connectionFor(17)); assertFalse(conn.destroyed)
    }
}
