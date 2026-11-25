package com.utun.chat.data

import android.content.Context
import android.bluetooth.BluetoothAdapter
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.utun.chat.ChatApplication
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие engine/output/workers; Android-устройства и DSP допускают отказы до/после эффекта. */
class CallAudioLifecycleTest {
    private lateinit var engine: CallAudioEngine
    private val gates = mutableListOf<CountDownLatch>()
    private val main get() = Looper.getMainLooper()
    private val manager get() = AudioManager.shared
    private val failures = mutableListOf<String>()

    @Before fun prepare() {
        assertSame(main, Looper.myLooper())
        Build.VERSION.SDK_INT = 30
        ChatApplication.instance = Context()
        AudioManager.shared = AudioManager()
        BluetoothAdapter.current = null
        AudioTrack.created.clear(); AudioTrack.nextFailures = emptySet(); AudioTrack.minimumBuffer = 1920
        AudioRecord.created.clear(); AudioRecord.nextFailures = emptySet()
        AudioRecord.nextOnStart = {}
        NativeLib.callAudioCommands.clear(); NativeLib.callAudioFailures = emptySet(); NativeLib.onCallFeed = {}
        TelecomCallManager.connection = null
        Handler.rejectImmediate = false; Handler.rejectDelayed = false
        LogManager.entries.clear(); LogManager.console = true
        engine = CallAudioEngine()
        engine.onFailure = { _, detail -> failures.add(detail) }
    }

    @After fun release() {
        gates.forEach { it.countDown() }
        Thread.interrupted()
        NativeLib.callAudioFailures = emptySet(); NativeLib.onCallFeed = {}
        manager.failures = emptySet()
        AudioRecord.created.forEach { it.failures = emptySet() }
        AudioTrack.created.forEach { it.failures = emptySet(); it.onWrite = {}; it.onTimestamp = {} }
        AudioRecord.nextOnStart = {}
        engine.stop(); main.advanceBy(10_000)
        assertTrue(AudioRecord.created.all { it.released })
        assertTrue(AudioTrack.created.all { it.released })
        assertNull(field("captureThread")); assertNull(field("playThread"))
        assertEquals(0L, field("callId"))
        Build.VERSION.SDK_INT = 36
    }

    private fun field(name: String): Any? = CallAudioEngine::class.java.getDeclaredField(name)
        .apply { isAccessible = true }.get(engine)
    private fun await(message: String, condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!condition() && System.nanoTime() < deadline) { main.dispatchAll(); Thread.sleep(2) }
        main.dispatchAll(); assertTrue(message, condition())
    }
    private fun start(id: Long = 7) {
        assertTrue(engine.start(id))
        await("both call workers ready") { field("captureReady") == true && field("playReady") == true }
    }
    private fun diagnostic(text: String) = synchronized(LogManager.entries) {
        LogManager.entries.any { it.level in listOf("WARN", "ERROR") && text in it.message }
    }
    private fun stops() = NativeLib.callAudioCommands.count { it.first == "stop" }
    private fun rejected(block: () -> Unit) = assertThrows(IllegalStateException::class.java, block)

    @Test fun bothFailedReleasesRetainOwnersAndBlockReplacementUntilAcknowledged() {
        start()
        val record = AudioRecord.created.single().also { it.failures = setOf("release") }
        val track = AudioTrack.created.single().also { it.failures = setOf("release") }
        rejected { engine.stop() }
        assertFalse(engine.isActive()); assertSame(record, field("audioRecord")); assertNotNull(field("audioTrack"))
        assertFalse(record.released); assertFalse(track.released); assertEquals(0, stops())
        assertTrue(diagnostic("record release failed")); assertTrue(diagnostic("track release failed"))
        rejected { engine.start(8) }
        assertEquals(listOf("start" to 7L), NativeLib.callAudioCommands.toList())
        record.failures = emptySet(); track.failures = emptySet()
        engine.stop()
        assertTrue(record.released); assertTrue(track.released); assertEquals(1, stops())
        assertNull(field("audioRecord")); assertNull(field("audioTrack"))
        engine.stop(); assertEquals(1, stops())
        start(8)
    }

    @Test fun outputReleaseRetriesTheUnderlyingTrackInsteadOfClosingTheWrapperEarly() {
        val output = CallAudioOutput.open(manager, 0, false)
        val track = output.track!!
        track.failures = setOf("release")
        rejected { output.release() }
        assertFalse(track.released)
        track.failures = emptySet(); output.release(); output.release()
        assertTrue(track.released); assertEquals(2, track.releaseCalls)
    }

    @Test fun failuresAfterReleaseKeepOwnershipUntilTheNextSuccessfulRelease() {
        start()
        val record = AudioRecord.created.single().also { it.failures = setOf("release after") }
        val track = AudioTrack.created.single().also { it.failures = setOf("release after") }
        rejected { engine.stop() }
        assertTrue(record.released); assertTrue(track.released); assertEquals(0, stops())
        assertSame(record, field("audioRecord")); assertNotNull(field("audioTrack"))
        record.failures = emptySet(); track.failures = emptySet()
        engine.stop(); assertEquals(2, record.releaseCalls); assertEquals(2, track.releaseCalls)
        assertEquals(1, stops())
    }

    @Test fun communicationListenerFailureDoesNotPreventDeviceReleaseAndRemainsRetryable() {
        Build.VERSION.SDK_INT = 36
        manager.devices += AudioDeviceInfo(11, AudioDeviceInfo.TYPE_BUILTIN_EARPIECE, true, false, "earpiece")
        engine.router.setRoute(AudioRoute.EARPIECE)
        start()
        manager.failures = setOf("communication remove")
        rejected { engine.stop() }
        assertTrue(AudioRecord.created.single().released); assertTrue(AudioTrack.created.single().released)
        assertEquals(1, manager.communicationListeners.size); assertEquals(0, stops())
        assertTrue(diagnostic("communication listener removal failed"))
        manager.failures = emptySet(); engine.stop()
        assertTrue(manager.communicationListeners.isEmpty()); assertEquals(1, stops())
    }

    @Test fun partialCommunicationRegistrationRemainsOwnedWhenStartupRollbackFails() {
        Build.VERSION.SDK_INT = 36
        manager.failures = setOf("communication add after", "communication remove")
        rejected { engine.start(7) }
        assertFalse(engine.isActive()); assertEquals(1, manager.communicationListeners.size)
        assertEquals(0, stops()); assertTrue(AudioRecord.created.isEmpty())
        manager.failures = emptySet(); engine.stop()
        assertTrue(manager.communicationListeners.isEmpty()); assertEquals(1, stops())
    }

    @Test fun modeRestoreFailuresBeforeAndAfterTheEffectRemainRetryable() {
        for (failure in listOf("speaker=false", "mode=0", "mode after=0")) {
            start()
            manager.failures = setOf(failure)
            val previousStops = stops()
            rejected { engine.stop() }
            assertEquals(true, field("modeOwned")); assertEquals(previousStops, stops())
            assertTrue(AudioRecord.created.all { it.released }); assertTrue(diagnostic("restore audio mode failed"))
            manager.failures = emptySet(); engine.stop()
            assertEquals(previousStops + 1, stops()); assertEquals(AudioManager.MODE_NORMAL, manager.mode)
        }
    }

    @Test fun nativeStopFailureKeepsCallIdentityAndRetriesAfterDevicesAreReleased() {
        start()
        NativeLib.callAudioFailures = setOf("stop")
        rejected { engine.stop() }
        assertEquals(7L, field("callId")); assertTrue(AudioRecord.created.single().released)
        assertTrue(AudioTrack.created.single().released); assertTrue(diagnostic("native stop failed"))
        NativeLib.callAudioFailures = emptySet(); engine.stop()
        assertEquals(2, stops()); assertEquals(0L, field("callId"))
    }

    @Test fun blockedCaptureKeepsDevicesAndNativeUntilTheWorkerActuallyExits() {
        val entered = CountDownLatch(1)
        val unblock = CountDownLatch(1).also { gates.add(it) }
        NativeLib.onCallFeed = { entered.countDown(); unblock.await() }
        start(); assertTrue(entered.await(3, TimeUnit.SECONDS))
        rejected { engine.stop() }
        assertNotNull(field("captureThread")); assertFalse(AudioRecord.created.single().released)
        assertFalse(AudioTrack.created.single().released); assertEquals(0, stops())
        assertTrue(diagnostic("worker join pending")); assertTrue(diagnostic("cleanup pending"))
        unblock.countDown()
        await("capture exits") { (field("captureThread") as Thread).isAlive.not() }
        engine.stop(); assertEquals(1, stops())
    }

    @Test fun blockedRecordStartupCannotHoldMainBeforeTheBoundedJoin() {
        val entered = CountDownLatch(1)
        val unblock = CountDownLatch(1).also { gates.add(it) }
        AudioRecord.nextOnStart = { entered.countDown(); unblock.await() }
        assertTrue(engine.start(7)); assertTrue(entered.await(3, TimeUnit.SECONDS))
        assertTrue(engine.diagnostics.contains("recordOwned=true"))
        val began = System.nanoTime()
        rejected { engine.stop() }
        assertTrue(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - began) < 1_500)
        assertFalse(AudioRecord.created.single().released); assertEquals(0, stops())
        assertTrue(diagnostic("record stop deferred"))
        unblock.countDown()
        await("record startup exits") { (field("captureThread") as Thread).isAlive.not() }
        engine.stop(); assertEquals(1, stops())
    }

    @Test fun blockedTimestampReporterCannotHoldMainOrReleaseItsOutputEarly() {
        start()
        val entered = CountDownLatch(1)
        val unblock = CountDownLatch(1).also { gates.add(it) }
        val track = AudioTrack.created.single()
        track.onTimestamp = {
            entered.countDown()
            var interrupted = false
            while (unblock.count != 0L) {
                try { unblock.await() } catch (_: InterruptedException) { interrupted = true }
            }
            if (interrupted) Thread.currentThread().interrupt()
        }
        assertTrue(entered.await(3, TimeUnit.SECONDS))
        val began = System.nanoTime()
        rejected { engine.stop() }
        assertTrue(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - began) < 1_500)
        assertFalse(track.released); assertEquals(0, stops()); assertTrue(diagnostic("track pause deferred"))
        unblock.countDown()
        await("reporter exits with playback") { (field("playThread") as Thread).isAlive.not() }
        engine.stop(); assertTrue(track.released); assertEquals(0, track.accessAfterRelease)
        assertEquals(1, stops())
    }

    @Test fun recordSetupFailureRetainsEvenAnUninitializedDeviceUntilReleaseSucceeds() {
        for (failure in listOf("uninitialized", "state", "add listener", "add listener after", "start")) {
            AudioRecord.nextFailures = setOf(failure, "release")
            assertTrue(engine.start(7))
            await("capture setup failure $failure") { failures.isNotEmpty() }
            val record = AudioRecord.created.last()
            rejected { engine.stop() }
            assertSame(record, field("audioRecord")); assertFalse(record.released)
            record.failures = emptySet(); engine.stop(); failures.clear()
        }
    }

    @Test fun trackSetupFailureRetainsEvenAnUninitializedDeviceUntilReleaseSucceeds() {
        for (failure in listOf("uninitialized", "state", "add listener", "add listener after", "play")) {
            AudioTrack.nextFailures = setOf(failure, "release")
            assertTrue(engine.start(7))
            await("playback setup failure $failure") { failures.isNotEmpty() }
            val track = AudioTrack.created.last()
            rejected { engine.stop() }
            assertNotNull(field("audioTrack")); assertFalse(track.released)
            track.failures = emptySet(); engine.stop(); failures.clear()
        }
    }

    @Test fun stopAndPauseFailuresStillReleaseBothDevicesAfterWorkersExit() {
        start()
        AudioRecord.created.single().failures = setOf("stop")
        AudioTrack.created.single().failures = setOf("pause", "remove listener")
        engine.stop()
        assertTrue(AudioRecord.created.single().released); assertTrue(AudioTrack.created.single().released)
        assertTrue(diagnostic("record stop failed") || diagnostic("record stop deferred"))
        assertTrue(diagnostic("track pause failed") || diagnostic("track pause deferred"))
        assertEquals(1, stops())
    }

    @Test fun zeroCallIdCannotAcquireResourcesAndBackgroundStopCannotReleaseThem() {
        assertThrows(IllegalArgumentException::class.java) { engine.start(0) }
        assertTrue(NativeLib.callAudioCommands.isEmpty()); assertTrue(diagnostic("zero call id"))
        start()
        var failure: Exception? = null
        Thread { try { engine.stop() } catch (e: Exception) { failure = e } }.apply { start(); join() }
        assertTrue(failure is IllegalStateException); assertTrue(engine.isActive()); assertEquals(0, stops())
        assertFalse(AudioRecord.created.single().released); assertTrue(diagnostic("stop outside main"))
    }

    @Test fun lateTelecomOwnershipPreventsRestoringTheFallbackModeDuringStop() {
        start()
        engine.bindTelecom(UtunConnection())
        manager.mode = AudioManager.MODE_IN_COMMUNICATION
        engine.stop()
        assertEquals(AudioManager.MODE_IN_COMMUNICATION, manager.mode)
        assertEquals(false, field("modeOwned")); assertEquals(1, stops())
    }

    @Test fun pendingRouterCleanupBlocksNativeStopWithoutSkippingModeOrDeviceRelease() {
        start(); manager.failures = setOf("unregister")
        rejected { engine.stop() }
        assertTrue(AudioRecord.created.single().released); assertTrue(AudioTrack.created.single().released)
        assertEquals(AudioManager.MODE_NORMAL, manager.mode); assertEquals(0, stops())
        assertTrue(engine.router.cleanupPending); assertTrue(diagnostic("router stop failed"))
        rejected { engine.start(8) }
        assertEquals(listOf("start" to 7L), NativeLib.callAudioCommands.toList())
        manager.failures = emptySet(); engine.stop()
        assertFalse(engine.router.cleanupPending); assertEquals(1, stops())
    }

    @Test fun failedTelecomHandoffReportsFailureAndRetainsFallbackForCleanup() {
        Build.VERSION.SDK_INT = 36
        manager.devices += AudioDeviceInfo(11, AudioDeviceInfo.TYPE_BUILTIN_EARPIECE, true, false, "earpiece")
        engine.router.setRoute(AudioRoute.EARPIECE); start()
        manager.failures = setOf("clear")
        engine.bindTelecom(UtunConnection()); main.dispatchAll()
        assertTrue(failures.any { "Telecom routing handoff failed" in it }); assertNull(engine.router.telecom)
        rejected { engine.stop() }; assertEquals(0, stops())
        manager.failures = emptySet(); engine.stop(); assertEquals(1, stops())
    }
}
