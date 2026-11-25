package com.utun.chat.data

import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Handler
import android.os.Looper
import com.utun.chat.ChatApplication
import java.util.concurrent.ExecutionException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие engine/session/workers: отказы запуска, независимая очистка, сохранённые владельцы и stale callbacks. */
class RadioAudioLifecycleTest {
    private lateinit var context: Context
    private lateinit var engine: RadioAudioEngine
    private lateinit var io: ExecutorService
    private val main get() = Looper.getMainLooper()

    @Before fun prepare() {
        assertSame(main, Looper.myLooper())
        context = Context(); ChatApplication.instance = context
        context.configProvider.keepMicrophoneReady = false // Базовые сценарии освобождения без трёхминутного ожидания.
        AudioManager.shared = AudioManager(); CallController.blocked = false
        AndroidMicrophoneCapture.reset(); AudioTrack.created.clear(); AudioTrack.nextFailures = emptySet(); AudioTrack.minimumBuffer = 1920
        Handler.rejectDelayed = false; Handler.rejectImmediate = false
        NativeLib.coreStopping = false; NativeLib.radioFailures = emptySet(); NativeLib.radioCommands.clear()
        LogManager.entries.clear(); LogManager.console = true
        engine = RadioAudioEngine()
        io = RadioAudioEngine::class.java.getDeclaredField("io").apply { isAccessible = true }.get(engine) as ExecutorService
    }

    @After fun release() {
        context.receiverFailures = emptySet(); context.failPowerService = false; context.configProvider.failRadioSettings = false
        AudioTrack.created.forEach { it.failures = emptySet() }; AudioTrack.nextFailures = emptySet(); AudioTrack.minimumBuffer = 1920
        NativeLib.coreStopping = false; NativeLib.radioFailures = emptySet(); Handler.rejectDelayed = false; Handler.rejectImmediate = false
        if (!io.isShutdown) {
            try { engine.stop().get(5, TimeUnit.SECONDS) } catch (e: ExecutionException) {
                if (e.cause !is IllegalArgumentException || context.receivers.isNotEmpty()) throw e
                // Отрицательный контроль: старый код застревает на уже снятом receiver и не доходит до аудио.
                RadioAudioEngine::class.java.getDeclaredField("screenReceiver").apply { isAccessible = true }.set(engine, null)
                engine.stop().get(5, TimeUnit.SECONDS)
            }
            main.advanceBy(10_000); drain()
            io.shutdown(); assertTrue(io.awaitTermination(5, TimeUnit.SECONDS))
        }
        val barrier = Executors.newSingleThreadExecutor()
        try { barrier.submit { MediaAudioController.awaitIdle() }.get(5, TimeUnit.SECONDS) }
        finally { barrier.shutdownNow() }
        // Старый код теряет receiver после частичной регистрации; отрицательный контроль не загрязняет следующий тест.
        context.receivers.keys.toList().forEach { context.unregisterReceiver(it) }
        assertTrue(AudioTrack.created.all { it.released })
        assertTrue(AndroidMicrophoneCapture.opened.all { it.closed })
    }

    private fun drain() { io.submit {}.get(5, TimeUnit.SECONDS) }
    private fun start(group: Long = 7, expected: Boolean = true, ack: Boolean = true) {
        val results = mutableListOf<Boolean>()
        engine.start("%02x".format(group).repeat(32)) { assertSame(main, Looper.myLooper()); results.add(it) }
        drain(); main.dispatchAll()
        assertEquals("one main result for group=$group", listOf(expected), results)
        if (expected && ack) { engine.onSettingsApplied(NativeLib.radioRequest, true); drain() }
    }
    private fun failedStop() {
        try { engine.stop().get(5, TimeUnit.SECONDS); fail("cleanup must report its failure") }
        catch (e: ExecutionException) { assertTrue(e.cause is Exception) }
    }
    private fun count(kind: String) = synchronized(NativeLib.radioCommands) { NativeLib.radioCommands.count { it.kind == kind } }
    private fun diagnostic(fragment: String) = assertTrue("missing diagnostic: $fragment",
        synchronized(LogManager.entries) { LogManager.entries.any { fragment in it.message && it.level in listOf("WARN", "ERROR") } })
    private fun await(condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!condition() && System.nanoTime() < deadline) Thread.sleep(2)
        assertTrue("radio state did not settle", condition())
    }

    @Test fun captureReadinessWaitsForHardwareSamplesAndResetsBeforeNextPtt() {
        start(); assertTrue(engine.talkBegin()); drain()
        val before = NativeLib.radioFeeds.size
        await { NativeLib.radioFeeds.size > before }
        assertTrue(engine.isTalking()); assertFalse(engine.captureReady.value)
        val input = AndroidMicrophoneCapture.opened.last()
        val time = System.nanoTime()
        repeat(20) { index ->
            assertTrue(input.accept(MicrophoneCapture.Frame(ShortArray(1920), time + index * 20_000_000L,
                index * 960L, generation = input.generation)))
        }
        await { engine.captureReady.value } // Настоящая цифровая тишина также подтверждает готовность.
        engine.cancelTalk(); drain()
        assertFalse(engine.captureReady.value); assertFalse(engine.isTalking())
        assertTrue(engine.talkBegin()); drain()
        assertFalse(engine.captureReady.value)
    }

    @Test fun coreCleanupRejectsNewRadioBeforeAcquiringAudioDevices() {
        NativeLib.coreStopping = true
        start(expected = false)
        assertFalse(engine.isActive())
        assertTrue(AudioTrack.created.isEmpty())
        assertTrue(AndroidMicrophoneCapture.opened.isEmpty())
        diagnostic("core cleanup")
    }

    @Test fun receiverCleanupFailureDoesNotSkipCaptureOutputOrNativeStopAndCanBeRetried() {
        start(); assertTrue(engine.talkBegin()); drain(); await { NativeLib.radioTransmitting() }
        val track = AudioTrack.created.single()
        val before = count("audio stop")
        context.receiverFailures = setOf("unregister")
        failedStop()
        assertTrue(track.released); assertFalse(NativeLib.radioTransmitting()); assertEquals(before + 1, count("audio stop"))
        assertEquals(1, context.receivers.size); assertFalse(engine.isActive()); assertNull(engine.activeGroup.value)
        context.receiverFailures = emptySet(); engine.stop().get(5, TimeUnit.SECONDS)
        assertTrue(context.receivers.isEmpty()); assertEquals(1, track.releaseCalls)
    }

    @Test fun unregistrationAfterEffectFailureIsConfirmedAbsentOnRetry() {
        start(); val track = AudioTrack.created.single()
        context.receiverFailures = setOf("unregister after")
        failedStop(); assertTrue(context.receivers.isEmpty()); assertTrue(track.released)
        context.receiverFailures = emptySet(); repeat(2) { engine.stop().get(5, TimeUnit.SECONDS) }
        assertEquals(2, context.unregistrationAttempts); assertEquals(1, track.releaseCalls)
    }

    @Test fun registrationFailuresBeforeAndAfterEffectAlwaysReportFalseAndLeaveNoOwners() {
        for (failure in listOf("register", "register after")) {
            context.receiverFailures = setOf(failure)
            start(expected = false)
            assertFalse(engine.isActive()); assertNull(engine.activeGroup.value)
            assertTrue(context.receivers.isEmpty()); assertTrue(AudioTrack.created.all { it.released })
            context.receiverFailures = emptySet(); engine.stop().get(5, TimeUnit.SECONDS)
        }
    }

    @Test fun failedPowerQueryRollsBackTheRegisteredReceiverAndPlayback() {
        context.failPowerService = true
        start(expected = false)
        assertNull(engine.activeGroup.value); assertTrue(context.receivers.isEmpty())
        assertTrue(AudioTrack.created.single().released); diagnostic("audio setup failed")
    }

    @Test fun failedTrackReleaseStillStopsNativeAndRetainsOnlyTheFailedTrackForRetry() {
        start(); val track = AudioTrack.created.single().also { it.failures = setOf("release") }
        val before = count("audio stop")
        failedStop()
        assertEquals(before + 1, count("audio stop")); assertTrue(context.receivers.isEmpty())
        assertFalse(track.released); assertTrue(track.listeners.isEmpty())
        track.failures = emptySet(); repeat(2) { engine.stop().get(5, TimeUnit.SECONDS) }
        assertTrue(track.released); assertEquals(2, track.releaseCalls)
    }

    @Test fun releaseAfterEffectFailureDoesNotCallStopOrReadTheReleasedTrackOnRetry() {
        start(); val track = AudioTrack.created.single().also { it.failures = setOf("release after") }
        failedStop(); assertTrue(track.released)
        track.failures = emptySet(); engine.stop().get(5, TimeUnit.SECONDS)
        assertEquals(2, track.releaseCalls); assertEquals(0, track.accessAfterRelease)
    }

    @Test fun partialSetupAndFailedRollbackStillDeliverFailureAndBlockReplacementUntilCleanupSucceeds() {
        AudioTrack.nextFailures = setOf("play", "release")
        start(expected = false)
        val old = AudioTrack.created.single()
        assertFalse(old.released); assertFalse(engine.isActive())
        AudioTrack.nextFailures = emptySet()
        start(8, expected = false)
        assertEquals(1, AudioTrack.created.size)
        old.failures = emptySet(); engine.stop().get(5, TimeUnit.SECONDS)
        start(8); assertEquals(2, AudioTrack.created.size)
    }

    @Test fun cleanupFailureBeforeSwitchAlwaysCompletesReadinessWithoutStartingTheNewGroup() {
        start(); val old = AudioTrack.created.single()
        context.receiverFailures = setOf("unregister")
        start(8, expected = false)
        assertTrue(old.released); assertFalse(engine.isActive()); assertNull(engine.activeGroup.value)
        assertEquals(1, count("audio start"))
        context.receiverFailures = emptySet(); start(8)
        assertEquals(2, count("audio start"))
    }

    @Test fun invalidMinimumBufferNeverCreatesAnOutputAndStillCompletesReadiness() {
        for (minimum in listOf(-2, 0)) {
            AudioTrack.minimumBuffer = minimum; start(expected = false)
            assertTrue(AudioTrack.created.isEmpty()); assertNull(engine.activeGroup.value)
            diagnostic("minimum buffer")
        }
    }

    @Test fun nativeStartupFailureClearsThePublishedActiveGroup() {
        NativeLib.radioFailures = setOf("start")
        start(expected = false)
        assertFalse(engine.isActive()); assertNull(engine.activeGroup.value); assertTrue(AudioTrack.created.isEmpty())
    }

    @Test fun settingsReadFailureDoesNotPublishAReadyOrActiveRadio() {
        context.configProvider.failRadioSettings = true
        start(expected = false)
        assertFalse(engine.isActive()); assertNull(engine.activeGroup.value); assertTrue(AudioTrack.created.isEmpty())
        diagnostic("radio settings read failed")
    }

    @Test fun nativeStopFailureDoesNotRetainAlreadyReleasedAndroidResources() {
        start(); val track = AudioTrack.created.single()
        NativeLib.radioFailures = setOf("stop"); failedStop()
        assertTrue(track.released); assertTrue(context.receivers.isEmpty())
        NativeLib.radioFailures = emptySet(); engine.stop().get(5, TimeUnit.SECONDS)
        assertEquals(1, track.releaseCalls)
    }

    @Test fun staleReadinessCannotOverwriteTheNewGroupAndEveryCallbackRunsOnce() {
        val results = mutableListOf<Pair<Long, Boolean>>()
        engine.start("07".repeat(32)) { results.add(7L to it) }; drain()
        engine.start("08".repeat(32)) { results.add(8L to it) }; drain(); main.dispatchAll()
        assertEquals(listOf(7L to false, 8L to true), results)
        assertEquals("08".repeat(32), engine.activeGroup.value); assertTrue(engine.isActive())
    }

    @Test fun readinessCallbackExceptionsAreLoggedOnMainAndDoNotLoseResourceOwnership() {
        engine.start("07".repeat(32)) { throw IllegalStateException("caller callback failed") }; drain()
        try { main.dispatchAll(); fail("caller exception must propagate") } catch (e: IllegalStateException) {
            assertEquals("caller callback failed", e.message)
        }
        diagnostic("readiness callback failed")
        engine.stop().get(5, TimeUnit.SECONDS); assertTrue(AudioTrack.created.single().released)
    }

    @Test fun rejectedReadinessDeliveryReleasesTheNewAudioWithoutRunningCallerOnIo() {
        Handler.rejectImmediate = true
        val results = mutableListOf<Boolean>()
        engine.start("07".repeat(32)) { results.add(it) }; drain()
        diagnostic("readiness callback rejected")
        assertTrue(results.isEmpty()); assertFalse(engine.isActive()); assertNull(engine.activeGroup.value)
        assertTrue(AudioTrack.created.single().released); assertTrue(context.receivers.isEmpty())
    }

    @Test fun staleScreenReceiverCannotChangeOrStopTheReplacementGroup() {
        start(); val oldReceiver = context.receivers.keys.single()
        start(8); engine.setLevelMonitoring(true); drain()
        assertEquals(1, count("capture start"))
        oldReceiver.onReceive(context, Intent(Intent.ACTION_SCREEN_OFF)); drain()
        assertEquals(0, count("capture stop")); assertEquals("08".repeat(32), engine.activeGroup.value)
    }

    @Test fun staleSettingsAcknowledgementCannotEnableCaptureOfANewGroup() {
        start(); val old = NativeLib.radioRequest
        start(8, ack = false); engine.setLevelMonitoring(true); engine.onSettingsApplied(old, true); drain()
        assertEquals(0, count("capture start"))
        engine.onSettingsApplied(NativeLib.radioRequest, true); drain()
        assertEquals(1, count("capture start"))
    }

    @Test fun stopRemovesSettingsTimeoutBeforeItsExecutorCanBeTerminated() {
        start(ack = false); engine.stop().get(5, TimeUnit.SECONDS)
        io.shutdown(); assertTrue(io.awaitTermination(5, TimeUnit.SECONDS))
        main.advanceBy(10_000)
        assertTrue(context.receivers.isEmpty())
    }

    @Test fun rejectedSettingsWatchdogIsLoggedButAnAcknowledgementCanStillEnableCapture() {
        Handler.rejectDelayed = true
        start(ack = false); diagnostic("settings timeout rejected")
        engine.onSettingsApplied(NativeLib.radioRequest, true); engine.setLevelMonitoring(true); drain()
        assertEquals(1, count("capture start"))
    }

    @Test fun rejectedBackgroundTimerClosesTheAutomaticWindowAndDoesNotOpenTheMicrophone() {
        context.configProvider.radioSettings = RadioAutoSettings(enabled = true)
        Handler.rejectDelayed = true; start()
        diagnostic("automatic timeout rejected")
        assertEquals(0, count("capture start")); assertTrue(engine.isActive())
    }

    @Test fun rejectedBackgroundTimerPreservesManualPttInsteadOfDisablingItWithAutomation() {
        context.configProvider.radioSettings = RadioAutoSettings(enabled = true)
        start(); Handler.rejectDelayed = true
        assertTrue(engine.talkBegin()); drain(); await { NativeLib.radioTransmitting() }
        diagnostic("automatic timeout rejected")
        assertEquals(RadioAutoSettings.OFF, NativeLib.radioAutoMode()); assertTrue(engine.isTalking())
        engine.cancelTalk(); drain(); await { !NativeLib.radioTransmitting() }
    }

    @Test fun workerLifecycleCallsAreRejectedBeforeChangingThePublishedGroup() {
        start()
        for (action in listOf<() -> Unit>({ engine.start("08".repeat(32)) }, { engine.stop() })) {
            val failure = AtomicReference<Throwable?>()
            Thread({ try { action() } catch (e: Throwable) { failure.set(e) } }, "radio-lifecycle-worker").apply { start(); join() }
            assertTrue("lifecycle must reject worker: ${failure.get()}", failure.get() is IllegalStateException)
        }
        assertEquals("07".repeat(32), engine.activeGroup.value); assertTrue(engine.isActive())
    }

    @Test fun outputDetachFailureNeverReleasesATrackStillOwnedByTheMediaSession() {
        for (afterEffect in listOf(false, true)) {
            start(); val track = AudioTrack.created.last()
            val field = RadioAudioEngine::class.java.getDeclaredField("outputLease").apply { isAccessible = true }
            val lease = field.get(engine) as AutoCloseable
            var reject = true
            field.set(engine, AutoCloseable {
                if (reject && !afterEffect) throw IllegalStateException("detach failed before effect")
                lease.close()
                if (reject) throw IllegalStateException("detach failed after effect")
            })
            val before = count("audio stop")
            try {
                failedStop()
                assertFalse(track.released); assertEquals(before + 1, count("audio stop"))
                assertTrue(context.receivers.isEmpty()); diagnostic("output detach failed")
            } finally { reject = false }
            engine.stop().get(5, TimeUnit.SECONDS)
            assertTrue(track.released); assertEquals(1, track.releaseCalls); assertEquals(0, track.accessAfterRelease)
        }
    }

    @Test fun interruptionDuringPlaybackStopDoesNotSkipJoinLeaseDetachOrNativeStop() {
        start(); val track = AudioTrack.created.single().also { it.interruptOnStop = true }
        val before = count("audio stop")
        engine.stop().get(5, TimeUnit.SECONDS)
        assertTrue(track.released); assertTrue(track.listeners.isEmpty()); assertEquals(0, track.accessAfterRelease)
        assertEquals(before + 1, count("audio stop")); assertTrue(context.receivers.isEmpty())
    }

    @Test fun zeroGroupRequestCannotReplaceAWorkingRadio() {
        start(); val track = AudioTrack.created.single()
        val results = mutableListOf<Boolean>()
        engine.start("") { results.add(it) }; drain(); main.dispatchAll()
        assertEquals(listOf(false), results); assertEquals("07".repeat(32), engine.activeGroup.value)
        assertTrue(engine.isActive()); assertFalse(track.released); assertEquals(1, AudioTrack.created.size)
        diagnostic("zero group ID")
    }

    @Test fun failedRestoreAfterCallDisablesNativeGroupMembershipBeforeClearingThePublishedGroup() {
        context.configProvider.failRadioSettings = true
        engine.restoreAfterCall("07".repeat(32)); drain(); main.dispatchAll(); drain()
        val activation = synchronized(NativeLib.radioCommands) { NativeLib.radioCommands.filter { it.kind.startsWith("active=") } }
        assertEquals(listOf("active=true", "active=false"), activation.map { it.kind })
        assertFalse(engine.isActive()); assertNull(engine.activeGroup.value)
    }

    @Test fun reentrantStartFromFailedReadinessCannotBeClearedByTheOldCallback() {
        context.configProvider.failRadioSettings = true
        val results = mutableListOf<Pair<Long, Boolean>>()
        engine.start("07".repeat(32)) { ready ->
            results.add(7L to ready)
            context.configProvider.failRadioSettings = false
            engine.start("08".repeat(32)) { results.add(8L to it) }
        }
        drain(); main.dispatchAll(); drain(); main.dispatchAll()
        assertEquals(listOf(7L to false, 8L to true), results)
        assertEquals("08".repeat(32), engine.activeGroup.value); assertTrue(engine.isActive())
    }

    @Test fun failedReconfigurationCannotBeUnlockedByAnOldSettingsAcknowledgement() {
        start(); engine.setLevelMonitoring(true); drain()
        val oldRequest = NativeLib.radioRequest
        assertEquals(1, count("capture start"))
        context.configProvider.failRadioSettings = true
        engine.reconfigure(); drain()
        diagnostic("radio reconfigure failed")
        engine.onSettingsApplied(oldRequest, true); engine.setLevelMonitoring(true); drain()
        assertEquals(1, count("capture start")); assertEquals(1, count("capture stop"))
        assertTrue(engine.isActive()); assertFalse(AudioTrack.created.single().released)
        context.configProvider.failRadioSettings = false
        engine.reconfigure(); drain(); engine.onSettingsApplied(NativeLib.radioRequest, true); drain()
        assertEquals(2, count("capture start"))
    }
}
