package com.utun.chat.data

import android.bluetooth.BluetoothAdapter
import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import com.utun.chat.ChatApplication
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий router: отказ API, запоздалые события, цели ввода/вывода и независимое владение Telecom. */
class CallAudioRouterRoutingTest {
    private lateinit var audio: AudioManager
    private lateinit var router: CallAudioRouter
    private lateinit var adapter: BluetoothAdapter
    private val speaker = device(1, AudioDeviceInfo.TYPE_BUILTIN_SPEAKER)
    private val earpiece = device(2, AudioDeviceInfo.TYPE_BUILTIN_EARPIECE)
    private val mic = device(3, AudioDeviceInfo.TYPE_BUILTIN_MIC, input = true)
    private val wired = device(4, AudioDeviceInfo.TYPE_USB_HEADSET)
    private val wiredMic = device(5, AudioDeviceInfo.TYPE_USB_HEADSET, input = true)
    private val bt = device(6, AudioDeviceInfo.TYPE_BLUETOOTH_SCO)
    private val btMic = device(7, AudioDeviceInfo.TYPE_BLUETOOTH_SCO, input = true)

    private fun device(id: Int, type: Int, input: Boolean = false) = AudioDeviceInfo(id, type, !input, input, "device-$id")

    @Before fun prepare() {
        Build.VERSION.SDK_INT = 36
        audio = AudioManager().also { AudioManager.shared = it; it.devices = arrayOf(speaker, earpiece, mic) }
        ChatApplication.instance = Context()
        adapter = BluetoothAdapter()
        BluetoothAdapter.current = null
        router = CallAudioRouter()
        LogManager.entries.clear(); LogManager.console = true
    }

    @After fun release() {
        audio.failures = emptySet()
        router.onOutputDevice = null; router.onChanged = null
        router.stop()
        // Неполученные proxy принадлежат отменённому запросу до доставки callback Android.
        adapter.requests.forEachIndexed { index, request -> if (request.proxy in adapter.live) adapter.connect(index) }
        assertTrue("callback leak", audio.callbacks.isEmpty())
        assertTrue("proxy leak", adapter.live.isEmpty())
        Build.VERSION.SDK_INT = 36
    }

    private fun diagnostic(fragment: String) = assertTrue("Missing diagnostic: $fragment",
        LogManager.entries.any { it.message.contains(fragment) && it.level in listOf("WARN", "ERROR") })

    private fun event() = audio.callbacks.single().onAudioDevicesAdded(emptyArray())
    private fun hardwareOperations() = audio.operations.filter {
        it.startsWith("select=") || it.startsWith("sco=") || it.startsWith("speaker=") || it in listOf("clear", "start sco", "stop sco")
    }

    @Test fun failedStartMatrixRollsBackAndAllowsFreshStart() {
        for (failure in listOf("register", "register after", "devices", "available", "select=1")) {
            audio.failures = setOf(failure)
            assertThrows(IllegalStateException::class.java) { router.start() }
            assertTrue(failure, audio.callbacks.isEmpty())
            assertNull(router.currentOutputDevice)
            assertFalse(router.headsetAvailable)
            audio.failures = emptySet()
            router.start()
            assertEquals(speaker, router.currentOutputDevice)
            assertEquals(1, audio.callbacks.size)
            router.stop()
        }
        diagnostic("start failed, releasing partial lifecycle")
    }

    @Test fun rejectedInitialSelectionDoesNotPoisonStartedFlag() {
        audio.acceptCommunicationDevice = false
        assertThrows(IllegalStateException::class.java) { router.start() }
        assertTrue(audio.callbacks.isEmpty())
        audio.acceptCommunicationDevice = true
        router.start()
        assertEquals(speaker, audio.communicationDevice)
        assertEquals(2, audio.callbackHistory.size)
        diagnostic("Communication device rejected")
    }

    @Test fun failedStartInvalidatesPendingBluetoothProxy() {
        BluetoothAdapter.current = adapter
        audio.failures = setOf("devices")
        assertThrows(IllegalStateException::class.java) { router.start() }
        adapter.connect(0)
        assertTrue(adapter.live.isEmpty())
        assertFalse(router.headsetAvailable)
    }

    @Test fun observerFailureDuringStartStillReleasesPartialLifecycle() {
        router.onOutputDevice = { error("observer failed") }
        assertThrows(IllegalStateException::class.java) { router.start() }
        assertTrue(audio.callbacks.isEmpty())
        assertNull(router.currentOutputDevice)
        router.onOutputDevice = null
        router.start()
        assertEquals(speaker, router.currentOutputDevice)
    }

    @Test fun unregisterExceptionDoesNotSkipProxyOrRouteCleanup() {
        BluetoothAdapter.current = adapter
        router.start(); adapter.connect(0)
        audio.failures = setOf("unregister after")
        assertThrows(IllegalStateException::class.java) { router.stop() }
        assertTrue(adapter.live.isEmpty())
        assertTrue(audio.callbacks.isEmpty())
        assertNull(audio.communicationDevice)
        assertNull(router.currentOutputDevice)
        assertFalse(router.btInputAvailable)
        assertTrue(router.cleanupPending)
        diagnostic("device callback unregister failed")
    }

    @Test fun failedUnregisterCannotDeliverIntoStoppedLifecycle() {
        router.start()
        val callback = audio.callbacks.single()
        audio.failures = setOf("unregister")
        try {
            assertThrows(IllegalStateException::class.java) { router.stop() }
            val count = audio.operations.size
            callback.onAudioDevicesAdded(arrayOf(wired))
            assertEquals(count, audio.operations.size)
            assertNull(audio.communicationDevice)
            assertNull(router.currentOutputDevice)
        } finally {
            audio.failures = emptySet(); router.stop()
            assertTrue(audio.callbacks.isEmpty()); assertFalse(router.cleanupPending)
        }
        diagnostic("device callback unregister failed")
    }

    @Test fun clearFailureStillClearsAllPublishedSnapshots() {
        audio.devices += arrayOf(wired, wiredMic)
        router.setAutoHeadset(true); router.start()
        assertTrue(router.headsetAvailable)
        audio.failures = setOf("clear")
        assertThrows(IllegalStateException::class.java) { router.stop() }
        assertNull(router.currentOutputDevice); assertNull(router.currentInputDevice)
        assertFalse(router.headsetAvailable); assertFalse(router.wiredInputAvailable); assertFalse(router.btInputAvailable)
        assertTrue(audio.callbacks.isEmpty())
        diagnostic("stop cleanup failed")
    }

    @Test fun oldDeviceCallbackCannotChangeRestartedRouter() {
        router.start()
        val old = audio.callbacks.single()
        router.stop(); router.start()
        audio.devices += arrayOf(wired, wiredMic)
        val count = audio.operations.size
        old.onAudioDevicesAdded(arrayOf(wired))
        old.onAudioDevicesRemoved(arrayOf(speaker))
        assertEquals(count, audio.operations.size)
        assertFalse(router.headsetAvailable)
        event()
        assertTrue(router.headsetAvailable)
    }

    @Test fun deviceQueryFailureDoesNotEscapeCallbackAndLaterEventRecovers() {
        router.setAutoHeadset(true); router.start()
        audio.devices += arrayOf(wired, wiredMic)
        audio.failures = setOf("devices")
        event()
        assertFalse(router.headsetAvailable)
        diagnostic("device refresh failed")
        audio.failures = emptySet(); event()
        assertEquals(AudioRoute.HEADSET, router.route)
        assertEquals(wired, router.currentOutputDevice)
    }

    @Test fun rejectedRouteRetainsTargetsAndIdenticalRetryWorks() {
        router.start()
        var changes = 0
        router.onOutputDevice = { changes++ }
        audio.acceptCommunicationDevice = false
        router.setRoute(AudioRoute.EARPIECE)
        assertEquals(AudioRoute.SPEAKER, router.route)
        assertEquals(speaker, router.currentOutputDevice)
        assertEquals(0, changes)
        audio.acceptCommunicationDevice = true
        router.setRoute(AudioRoute.EARPIECE)
        assertEquals(AudioRoute.EARPIECE, router.route)
        assertEquals(earpiece, router.currentOutputDevice)
        assertEquals(1, changes)
    }

    @Test fun failedDeviceChangeWithinHeadsetCategoryCanBeRetried() {
        audio.devices += arrayOf(wired, wiredMic, bt, btMic)
        router.start(); router.setRoute(AudioRoute.HEADSET)
        assertEquals(bt, router.currentOutputDevice)
        audio.acceptCommunicationDevice = false
        router.setRoute(AudioRoute.HEADSET, preferWired = true)
        assertEquals(bt, router.currentOutputDevice)
        assertEquals(btMic, router.currentInputDevice)
        audio.acceptCommunicationDevice = true
        router.setRoute(AudioRoute.HEADSET, preferWired = true)
        assertEquals(wired, router.currentOutputDevice)
        assertEquals(wiredMic, router.currentInputDevice)
    }

    @Test fun inputAndOutputTargetsArePublishedTogetherBeforeObservers() {
        audio.devices += arrayOf(wired, wiredMic)
        router.start()
        var inputSeen: AudioDeviceInfo? = null
        router.onOutputDevice = { if (it == wired) inputSeen = router.currentInputDevice }
        router.setRoute(AudioRoute.HEADSET)
        assertEquals(wiredMic, inputSeen)
    }

    @Test fun forcedReapplyRequestsSameCategoryAgain() {
        router.start()
        val before = audio.operations.count { it == "select=1" }
        router.reapply()
        assertEquals(before + 1, audio.operations.count { it == "select=1" })
    }

    @Test fun deviceReplacementRefreshesTargetsWithoutCategoryChange() {
        router.start()
        val replacement = device(10, AudioDeviceInfo.TYPE_BUILTIN_SPEAKER)
        audio.devices = arrayOf(replacement, earpiece, mic)
        event()
        assertEquals(AudioRoute.SPEAKER, router.route)
        assertEquals(replacement, router.currentOutputDevice)
        assertEquals(replacement, audio.communicationDevice)
    }

    @Test fun stoppedSettersStorePreferencesWithoutTouchingHardware() {
        router.setRoute(AudioRoute.EARPIECE)
        router.setAutoHeadset(true)
        router.setGlobalRouting(false)
        router.reapply()
        assertTrue(hardwareOperations().isEmpty())
        router.start()
        assertEquals(AudioRoute.EARPIECE, router.route)
    }

    @Test fun missingEarpiecePublishesSpeakerFallback() {
        audio.devices = arrayOf(speaker, mic)
        router.setPreferredRoute(AudioRoute.EARPIECE)
        router.start()
        assertFalse(router.hasEarpiece)
        assertEquals(AudioRoute.SPEAKER, router.route)
        assertEquals(speaker, router.currentOutputDevice)
    }

    @Test fun automaticHeadsetFollowsManualFallbackAndKeepsPreferredRoute() {
        router.setAutoHeadset(true); router.start()
        router.setRoute(AudioRoute.SPEAKER)
        audio.devices += arrayOf(wired, wiredMic); event()
        assertEquals(AudioRoute.HEADSET, router.route)
        audio.devices = arrayOf(speaker, earpiece, mic); event()
        assertEquals(AudioRoute.SPEAKER, router.route)
        assertEquals(AudioRoute.SPEAKER, router.preferredRoute)
    }

    @Test fun manualHeadsetPreferenceRestoresAfterReconnection() {
        router.setPreferredRoute(AudioRoute.HEADSET); router.start()
        assertEquals(AudioRoute.EARPIECE, router.route)
        audio.devices += arrayOf(wired, wiredMic); event()
        assertEquals(AudioRoute.HEADSET, router.route)
        audio.devices = arrayOf(speaker, earpiece, mic); event()
        assertEquals(AudioRoute.EARPIECE, router.route)
        assertEquals(AudioRoute.HEADSET, router.preferredRoute)
    }

    @Test fun perTrackA2dpUsesMediaOutputWithoutGlobalRouting() {
        val a2dp = device(8, AudioDeviceInfo.TYPE_BLUETOOTH_A2DP)
        audio.devices += a2dp
        router.setGlobalRouting(false); router.start()
        assertTrue(router.headsetAvailable)
        router.setRoute(AudioRoute.HEADSET)
        assertEquals(a2dp, router.currentOutputDevice)
        assertNull(router.currentInputDevice)
        router.stop()
        assertTrue(hardwareOperations().isEmpty())
    }

    @Test fun perTrackUsbMicrophoneUsesBuiltInSpeakerAndWiredInput() {
        audio.devices += wiredMic
        router.setGlobalRouting(false); router.start()
        assertTrue(router.headsetAvailable)
        router.setRoute(AudioRoute.HEADSET, preferWired = true)
        assertEquals(speaker, router.currentOutputDevice)
        assertEquals(wiredMic, router.currentInputDevice)
        assertTrue(hardwareOperations().isEmpty())
    }

    @Test fun modernBluetoothWaitsForCommunicationEndpointWithoutLegacySco() {
        BluetoothAdapter.current = adapter
        router.setAutoHeadset(true); router.start(); adapter.connect(0)
        assertEquals(AudioRoute.HEADSET, router.route)
        assertNull(router.currentOutputDevice)
        assertTrue(audio.operations.none { it.contains("sco") || it.startsWith("speaker=") })
        audio.devices += arrayOf(bt, btMic); event()
        assertEquals(bt, router.currentOutputDevice)
        assertEquals(btMic, router.currentInputDevice)
        assertEquals(bt, audio.communicationDevice)
    }

    @Test fun modernBleUsesCommunicationSelection() {
        val le = device(8, AudioDeviceInfo.TYPE_BLE_HEADSET)
        val leMic = device(9, AudioDeviceInfo.TYPE_BLE_HEADSET, input = true)
        audio.devices += arrayOf(le, leMic)
        router.setAutoHeadset(true); router.start()
        assertEquals(le, audio.communicationDevice)
        assertEquals(leMic, router.currentInputDevice)
        assertTrue(audio.operations.none { it.contains("sco") })
    }

    @Test fun legacyBluetoothUsesScoAndCanReturnToSpeaker() {
        Build.VERSION.SDK_INT = 30
        BluetoothAdapter.current = adapter
        router.start(); adapter.connect(0)
        router.setRoute(AudioRoute.HEADSET)
        assertTrue(audio.isBluetoothScoOn)
        assertTrue(audio.operations.contains("start sco"))
        router.setRoute(AudioRoute.SPEAKER)
        assertFalse(audio.isBluetoothScoOn)
        assertTrue(audio.isSpeakerphoneOn)
        assertTrue(audio.operations.none { it.startsWith("select=") })
    }

    @Test fun legacyScoFailureRetainsPreviousCategory() {
        Build.VERSION.SDK_INT = 30
        BluetoothAdapter.current = adapter
        router.start(); adapter.connect(0)
        audio.failures = setOf("start sco")
        router.setRoute(AudioRoute.HEADSET)
        assertEquals(AudioRoute.SPEAKER, router.route)
        diagnostic("start sco")
    }

    @Test fun telecomOwnsRouteAndAvailabilityWithoutGlobalCommands() {
        val connection = UtunConnection().apply { headsetAvailable = true; confirmedRoute = AudioRoute.EARPIECE }
        router.bindTelecom(connection); router.start()
        assertTrue(router.headsetAvailable)
        router.setRoute(AudioRoute.HEADSET)
        assertEquals(AudioRoute.EARPIECE, router.route)
        connection.confirm(AudioRoute.HEADSET)
        assertEquals(AudioRoute.HEADSET, router.route)
        router.stop()
        assertTrue(hardwareOperations().isEmpty())
        assertNull(connection.onRouteChanged); assertNull(connection.onEndpointsChanged)
    }

    @Test fun bindingLiveTelecomImmediatelyRefreshesAvailability() {
        router.start()
        assertFalse(router.headsetAvailable)
        val connection = UtunConnection().apply { headsetAvailable = true }
        router.bindTelecom(connection)
        assertTrue(router.headsetAvailable)
        assertNull(router.currentOutputDevice); assertNull(router.currentInputDevice)
        assertEquals(listOf(AudioRoute.SPEAKER), connection.routeRequests)
    }

    @Test fun oldTelecomCallbacksAreIgnoredEvenAfterRebindingSameConnection() {
        val connection = UtunConnection()
        router.bindTelecom(connection); router.start()
        val oldRoute = requireNotNull(connection.onRouteChanged)
        val oldEndpoints = requireNotNull(connection.onEndpointsChanged)
        router.stop(); router.bindTelecom(connection); router.start()
        connection.confirm(AudioRoute.SPEAKER)
        val count = audio.operations.size
        oldRoute(AudioRoute.HEADSET); oldEndpoints()
        assertEquals(AudioRoute.SPEAKER, router.route)
        assertEquals(count, audio.operations.size)
    }

    @Test fun telecomRequestExceptionRollsBackStartupAndCanRetry() {
        val connection = UtunConnection().apply { failRoute = true }
        router.bindTelecom(connection)
        assertThrows(IllegalStateException::class.java) { router.start() }
        assertNull(router.telecom); assertTrue(audio.callbacks.isEmpty())
        connection.failRoute = false
        router.bindTelecom(connection); router.start()
        assertEquals(listOf(AudioRoute.SPEAKER), connection.routeRequests)
    }

    @Test fun callbackAndStopSerializeBeforeClearingSnapshot() {
        router.start()
        val callback = audio.callbacks.single()
        router.setAutoHeadset(true)
        val entered = CountDownLatch(1); val resume = CountDownLatch(1); val stopped = CountDownLatch(1)
        val error = AtomicReference<Throwable?>()
        router.onOutputDevice = { entered.countDown(); check(resume.await(5, TimeUnit.SECONDS)) }
        audio.devices += arrayOf(wired, wiredMic)
        val event = worker(error) { callback.onAudioDevicesAdded(emptyArray()) }
        assertTrue(entered.await(5, TimeUnit.SECONDS))
        val stop = worker(error) { router.stop(); stopped.countDown() }
        try { assertFalse(stopped.await(50, TimeUnit.MILLISECONDS)) } finally { resume.countDown() }
        join(listOf(event, stop), error)
        assertNull(router.currentOutputDevice); assertNull(audio.communicationDevice)
        assertFalse(router.headsetAvailable)
    }

    @Test fun concurrentRouteDeviceEventAndStopLeaveNoHardwareSelection() {
        LogManager.console = false
        repeat(32) {
            router.setPreferredRoute(AudioRoute.SPEAKER); router.start()
            val callback = audio.callbacks.single()
            val ready = CountDownLatch(3); val go = CountDownLatch(1)
            val error = AtomicReference<Throwable?>()
            val actions = listOf<() -> Unit>({ callback.onAudioDevicesAdded(emptyArray()) },
                { router.setRoute(AudioRoute.EARPIECE) }, { router.stop() })
            val workers = actions.map { action -> worker(error) { ready.countDown(); check(go.await(5, TimeUnit.SECONDS)); action() } }
            assertTrue(ready.await(5, TimeUnit.SECONDS)); go.countDown()
            join(workers, error)
            assertTrue(audio.callbacks.isEmpty())
            assertNull(router.currentOutputDevice); assertNull(audio.communicationDevice)
        }
    }

    private fun worker(error: AtomicReference<Throwable?>, body: () -> Unit) = Thread {
        try { body() } catch (failure: Throwable) { error.compareAndSet(null, failure) }
    }.apply { isDaemon = true; start() }

    private fun join(workers: List<Thread>, error: AtomicReference<Throwable?>) {
        workers.forEach { it.join(5000); assertFalse("worker stuck", it.isAlive) }
        error.get()?.let { throw AssertionError("router worker failed", it) }
    }
}
