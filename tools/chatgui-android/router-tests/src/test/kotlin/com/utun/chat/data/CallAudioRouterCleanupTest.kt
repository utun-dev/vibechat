package com.utun.chat.data

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothHeadset
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.utun.chat.ChatApplication
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий router: подтверждения Android cleanup, поздние proxy и независимость нового маршрута. */
class CallAudioRouterCleanupTest {
    private lateinit var router: CallAudioRouter
    private lateinit var adapter: BluetoothAdapter
    private val audio get() = AudioManager.shared
    private val main get() = Looper.getMainLooper()

    @Before fun prepare() {
        assertSame(main, Looper.myLooper()); Build.VERSION.SDK_INT = 36
        AudioManager.shared = AudioManager(); ChatApplication.instance = Context()
        adapter = BluetoothAdapter(); BluetoothAdapter.current = adapter
        Handler.rejectDelayed = false
        router = CallAudioRouter(); LogManager.entries.clear(); LogManager.console = true
    }

    @After fun release() {
        audio.failures = emptySet(); adapter.closeFailures = emptySet(); adapter.closeFailuresAfter = emptySet()
        Handler.rejectDelayed = false
        router.stop()
        adapter.requests.forEachIndexed { index, request -> if (request.proxy in adapter.live) adapter.connect(index) }
        router.stop(); main.advanceBy(30_000)
        assertTrue(adapter.live.isEmpty()); assertTrue(audio.callbacks.isEmpty()); assertFalse(router.cleanupPending)
        Build.VERSION.SDK_INT = 36; BluetoothAdapter.current = null
    }

    private fun diagnostic(text: String) = assertTrue("Missing diagnostic: $text", LogManager.entries.any {
        it.level in listOf("ERROR", "WARN") && text in it.message
    })
    private fun stopRejected() = assertThrows(IllegalStateException::class.java) { router.stop() }
    private fun operation(name: String) = audio.operations.count { it == name }

    @Test fun failedUnregisterRetainsTheCallbackAndBlocksAnotherRegistration() {
        router.start(); adapter.connect(0)
        val callback = audio.callbacks.single()
        audio.failures = setOf("unregister")
        stopRejected(); assertTrue(router.cleanupPending)
        assertEquals(setOf(callback), audio.callbacks); assertTrue(adapter.live.isEmpty()); assertNull(audio.communicationDevice)
        assertThrows(IllegalStateException::class.java) { router.start() }
        assertEquals(1, audio.callbackHistory.size); assertEquals(1, adapter.requests.size)
        val operations = audio.operations.size
        callback.onAudioDevicesAdded(emptyArray()); callback.onAudioDevicesRemoved(emptyArray())
        assertEquals(operations, audio.operations.size)
        audio.failures = emptySet(); router.start()
        assertEquals(2, audio.callbackHistory.size); assertFalse(router.cleanupPending)
    }

    @Test fun independentFailuresRetainOnlyOperationsStillAwaitingAcknowledgment() {
        router.start(); adapter.connect(0)
        val proxy = adapter.requests.single().proxy
        adapter.closeFailures = setOf(proxy); audio.failures = setOf("unregister", "clear")
        stopRejected()
        assertEquals(1, adapter.closeAttempts); assertEquals(1, operation("clear")); assertEquals(1, operation("unregister"))
        assertNull(router.currentOutputDevice); assertFalse(router.headsetAvailable)
        audio.failures = setOf("clear"); stopRejected()
        assertTrue(audio.callbacks.isEmpty()); assertEquals(2, operation("unregister"))
        adapter.closeFailures = emptySet(); stopRejected()
        assertTrue(adapter.live.isEmpty()); assertEquals(2, operation("unregister"))
        audio.failures = emptySet(); router.stop()
        assertFalse(router.cleanupPending); assertEquals(3, adapter.closeAttempts); assertEquals(4, operation("clear"))
        assertEquals(2, operation("unregister"))
        diagnostic("device callback unregister failed"); diagnostic("bt owned proxy close failed")
        diagnostic("communication device release failed")
    }

    @Test fun failuresAfterUnregisterAndCloseStillRequireAnAcknowledgedRetry() {
        router.start(); adapter.connect(0)
        adapter.closeFailuresAfter = setOf(adapter.requests.single().proxy)
        audio.failures = setOf("unregister after")
        stopRejected()
        assertTrue(audio.callbacks.isEmpty()); assertTrue(adapter.live.isEmpty()); assertTrue(router.cleanupPending)
        audio.failures = emptySet(); adapter.closeFailuresAfter = emptySet(); router.stop()
        assertFalse(router.cleanupPending); assertEquals(1, adapter.closes); assertEquals(2, adapter.closeAttempts)
        assertEquals(2, operation("unregister")); assertEquals(1, operation("clear"))
    }

    @Test fun failedStartupPreservesItsOriginalErrorAndItsFailedRollback() {
        audio.failures = setOf("register after", "unregister")
        val failure = assertThrows(IllegalStateException::class.java) { router.start() }
        assertTrue(failure.message!!.contains("register after")); assertEquals(1, failure.suppressed.size)
        assertEquals(1, audio.callbacks.size); assertTrue(router.cleanupPending)
        audio.failures = emptySet(); router.start()
        assertEquals(1, audio.callbacks.size); assertEquals(2, audio.callbackHistory.size)
        diagnostic("startup rollback incomplete")
    }

    @Test fun mainRetryCompletesCleanupWithoutAnotherOwnerCall() {
        router.start(); adapter.connect(0)
        audio.failures = setOf("unregister"); stopRejected()
        main.advanceBy(250); assertTrue(router.cleanupPending)
        audio.failures = emptySet(); main.advanceBy(500)
        assertFalse(router.cleanupPending); assertTrue(audio.callbacks.isEmpty()); assertTrue(adapter.live.isEmpty())
    }

    @Test fun rejectedRetryKeepsExplicitCleanupAvailable() {
        router.start(); adapter.connect(0)
        Handler.rejectDelayed = true; adapter.closeFailures = adapter.live.toSet()
        stopRejected(); diagnostic("cleanup retry rejected; owner retained")
        assertTrue(router.cleanupPending); assertEquals(1, adapter.live.size)
        adapter.closeFailures = emptySet(); router.stop(); assertFalse(router.cleanupPending)
    }

    @Test fun lateProxyCloseFailureIsRetriedAfterTheOwnerAlreadyStopped() {
        router.start(); router.stop()
        adapter.closeFailures = adapter.live.toSet()
        adapter.connect(0)
        assertTrue(router.cleanupPending); assertEquals(1, adapter.live.size)
        adapter.closeFailures = emptySet(); main.advanceBy(250)
        assertTrue(adapter.live.isEmpty()); assertFalse(router.cleanupPending)
        assertEquals(1, operation("clear")); assertTrue(audio.callbacks.isEmpty())
    }

    @Test fun oldProxyRetryDoesNotStopTheNewRouterSession() {
        router.start(); router.stop(); router.start(); adapter.connect(1)
        val current = adapter.requests[1].proxy
        val old = adapter.requests[0].proxy
        adapter.closeFailures = setOf(old); adapter.connect(0)
        assertTrue(router.cleanupPending); assertTrue(router.headsetAvailable)
        val callback = audio.callbacks.single()
        val clears = operation("clear"); val selected = audio.communicationDevice
        adapter.closeFailures = emptySet(); main.advanceBy(250)
        assertEquals(setOf(current), adapter.live); assertTrue(router.headsetAvailable)
        assertEquals(setOf(callback), audio.callbacks); assertEquals(clears, operation("clear"))
        assertSame(selected, audio.communicationDevice); assertFalse(router.cleanupPending)
    }

    @Test fun replacementProxyDoesNotLoseTheOldOwnerOrReactivateItsLateCallback() {
        router.start(); adapter.connect(0)
        val old = adapter.requests[0].proxy
        val replacement = BluetoothHeadset(99).apply { connected = true }
        adapter.live.add(replacement); adapter.closeFailures = setOf(old)
        adapter.requests[0].listener.onServiceConnected(BluetoothProfile.HEADSET, replacement)
        assertTrue(router.cleanupPending); assertEquals(2, adapter.live.size)
        adapter.connect(0) // Повтор старого callback не заменяет уже принятый новый proxy.
        assertTrue(replacement in adapter.live); assertTrue(router.headsetAvailable)
        adapter.closeFailures = emptySet(); main.advanceBy(500)
        assertEquals(setOf(replacement), adapter.live); assertFalse(router.cleanupPending)
        val attempts = adapter.closeAttempts
        adapter.connect(0); assertEquals(attempts, adapter.closeAttempts)
        assertEquals(setOf(replacement), adapter.live)
    }

    @Test fun legacyScoStopAndFlagClearAreIndependentAndIndividuallyRetryable() {
        Build.VERSION.SDK_INT = 30
        router.start(); adapter.connect(0); router.setRoute(AudioRoute.HEADSET)
        assertTrue(audio.isBluetoothScoOn)
        audio.failures = setOf("stop sco", "sco=false"); stopRejected()
        assertTrue(audio.callbacks.isEmpty()); assertTrue(adapter.live.isEmpty())
        assertEquals(1, operation("stop sco")); assertEquals(1, operation("sco=false"))
        audio.failures = setOf("sco=false"); stopRejected()
        audio.failures = emptySet(); router.stop()
        assertEquals(2, operation("stop sco")); assertEquals(3, operation("sco=false"))
        assertFalse(audio.isBluetoothScoOn); assertFalse(router.cleanupPending)
    }

    @Test fun failedLegacyScoStartRetainsThePossibleEffectForStop() {
        Build.VERSION.SDK_INT = 30
        router.start(); adapter.connect(0)
        audio.failures = setOf("start sco", "stop sco"); router.setRoute(AudioRoute.HEADSET)
        stopRejected(); assertTrue(router.cleanupPending)
        audio.failures = emptySet(); router.stop()
        assertEquals(2, operation("stop sco")); assertFalse(router.cleanupPending)
    }

    @Test fun failedGlobalReleasePreventsTelecomHandoffAndLaterStopLeavesTelecomUntouched() {
        router.start()
        val connection = UtunConnection()
        audio.failures = setOf("clear")
        assertThrows(IllegalStateException::class.java) { router.bindTelecom(connection) }
        assertNull(router.telecom); assertNull(connection.onRouteChanged)
        audio.failures = emptySet(); router.bindTelecom(connection)
        val clears = operation("clear")
        router.stop(); assertEquals(clears, operation("clear")); assertFalse(router.cleanupPending)
    }

    @Test fun communicationSelectionFailureAfterItsEffectRollsBackTheOwnedDevice() {
        audio.failures = setOf("select after=1", "clear")
        assertThrows(IllegalStateException::class.java) { router.start() }
        assertNotNull(audio.communicationDevice); assertTrue(router.cleanupPending); assertTrue(audio.callbacks.isEmpty())
        audio.failures = emptySet(); router.stop()
        assertNull(audio.communicationDevice); assertFalse(router.cleanupPending)
    }

    @Test fun communicationReleaseAfterEffectStillNeedsASuccessfulAcknowledgment() {
        router.start(); adapter.connect(0)
        audio.failures = setOf("clear after"); stopRejected()
        assertNull(audio.communicationDevice); assertTrue(router.cleanupPending)
        audio.failures = emptySet(); router.stop()
        assertFalse(router.cleanupPending); assertEquals(2, operation("clear"))
    }

    @Test fun legacyScoEffectsAndIndependentAcknowledgmentsSurviveAfterEffectFailures() {
        Build.VERSION.SDK_INT = 30
        router.start(); adapter.connect(0)
        audio.failures = setOf("start sco after"); router.setRoute(AudioRoute.HEADSET)
        assertTrue(audio.legacyScoStarted)
        audio.failures = setOf("stop sco after", "sco after=false"); stopRejected()
        assertFalse(audio.legacyScoStarted); assertFalse(audio.isBluetoothScoOn); assertTrue(router.cleanupPending)
        audio.failures = emptySet(); router.stop()
        assertFalse(router.cleanupPending); assertEquals(2, operation("stop sco")); assertEquals(2, operation("sco=false"))
    }

    @Test fun startupCleanupPreservesTheTelecomConnectionConfiguredBeforeStart() {
        router.start(); adapter.connect(0)
        audio.failures = setOf("unregister"); stopRejected()
        val connection = UtunConnection()
        router.bindTelecom(connection)
        audio.failures = emptySet(); router.start()
        assertSame(connection, router.telecom); assertEquals(listOf(AudioRoute.SPEAKER), connection.routeRequests)
        assertFalse(router.cleanupPending)
    }
}
