package com.utun.chat.data

import android.content.Context
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Handler
import android.os.Looper
import com.utun.chat.ChatApplication
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие workers/PCM/session; ошибки оборудования и JNI контролируются на границах, без радиоэфира/DSP. */
class RadioWorkerLifecycleTest {
    private lateinit var context: Context
    private lateinit var engine: RadioAudioEngine
    private lateinit var io: ExecutorService
    private val gates = mutableListOf<CountDownLatch>()
    private val main get() = Looper.getMainLooper()

    @Before fun prepare() {
        assertSame(main, Looper.myLooper())
        context = Context(); ChatApplication.instance = context
        context.configProvider.keepMicrophoneReady = false
        AudioManager.shared = AudioManager(); CallController.blocked = false
        AndroidMicrophoneCapture.reset(); AudioTrack.created.clear(); AudioTrack.nextFailures = emptySet(); AudioTrack.minimumBuffer = 1920
        NativeLib.radioFailures = emptySet(); NativeLib.radioPullResult = 0; NativeLib.radioPullResults.clear()
        NativeLib.onRadioFeed = {}; NativeLib.onRadioCaptureStart = {}; NativeLib.radioCommands.clear(); NativeLib.radioFeeds.clear()
        Handler.rejectDelayed = false; Handler.rejectImmediate = false
        LogManager.entries.clear(); LogManager.console = true
        engine = RadioAudioEngine()
        io = RadioAudioEngine::class.java.getDeclaredField("io").apply { isAccessible = true }.get(engine) as ExecutorService
        start(7)
    }

    @After fun release() {
        gates.forEach { it.countDown() }
        NativeLib.onRadioFeed = {}; NativeLib.onRadioCaptureStart = {}; NativeLib.radioFailures = emptySet()
        NativeLib.radioPullResult = 0; NativeLib.radioPullResults.clear()
        Handler.rejectImmediate = false; Handler.rejectDelayed = false
        AudioTrack.created.forEach { it.failures = emptySet(); it.forcedWriteResult = null; it.onWrite = {} }
        engine.stop().get(5, TimeUnit.SECONDS); main.advanceBy(10_000); drain()
        io.shutdown(); assertTrue(io.awaitTermination(5, TimeUnit.SECONDS))
        val barrier = Executors.newSingleThreadExecutor()
        try {
            barrier.submit { MediaAudioController.awaitIdle() }.get(5, TimeUnit.SECONDS)
        } finally { barrier.shutdownNow() }
        assertTrue(AudioTrack.created.all { it.released && it.accessAfterRelease == 0 })
        assertTrue(AndroidMicrophoneCapture.opened.all { it.closed })
    }

    private fun drain() { io.submit {}.get(5, TimeUnit.SECONDS) }
    private fun start(group: Long) {
        var ready = false
        engine.start("%02x".format(group).repeat(32)) { ready = it }; drain(); main.dispatchAll()
        assertTrue("listening group=$group", ready)
        engine.onSettingsApplied(NativeLib.radioRequest, true); drain()
    }
    private fun await(message: String, pumpMain: Boolean = true, condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!condition() && System.nanoTime() < deadline) {
            if (pumpMain) main.dispatchAll()
            Thread.sleep(2)
        }
        if (pumpMain) main.dispatchAll()
        assertTrue(message, condition())
    }
    private fun diagnostic(fragment: String): Boolean = synchronized(LogManager.entries) {
        LogManager.entries.any { fragment in it.message && it.level in listOf("ERROR", "WARN") }
    }
    private fun subscription() = RadioAudioEngine::class.java.getDeclaredField("captureSubscription")
        .apply { isAccessible = true }.get(engine) as? MicrophoneCapture.Subscription
    private fun worker(name: String) = RadioAudioEngine::class.java.getDeclaredField(name).apply { isAccessible = true }.get(engine) as Thread
    private fun begin() {
        assertTrue(engine.talkBegin()); drain()
        await("manual transmission") { NativeLib.radioTransmitting() }
    }
    private fun captureEnded() {
        await("capture must stop while receive remains active") { !NativeLib.radioCaptureAlive && subscription() == null && !engine.isTalking() }
        assertTrue(engine.isActive()); assertFalse(AudioTrack.created.last().released)
    }
    private fun playbackEnded() {
        await("playback failure must release the radio") { !engine.isActive() && engine.activeGroup.value == null && AudioTrack.created.last().released }
        assertTrue(context.receivers.isEmpty()); assertFalse(NativeLib.radioCaptureAlive)
    }
    private fun count(kind: String) = synchronized(NativeLib.radioCommands) { NativeLib.radioCommands.count { it.kind == kind } }

    @Test fun writeAndHeadExceptionsAreLoggedAndReleaseThePlaybackSession() {
        for (failure in listOf("write", "head")) {
            val track = AudioTrack.created.last().also { it.failures = setOf(failure) }
            await("playback diagnostic $failure") { diagnostic("playback failed operation=$failure") }
            playbackEnded(); track.failures = emptySet()
            if (failure == "write") start(7)
        }
    }

    @Test fun zeroNegativeOversizedAndUnalignedWritesStopInsteadOfSpinningOrDroppingPcm() {
        for (result in listOf(0, -6, 1922, 1)) {
            AudioTrack.created.last().forcedWriteResult = result
            await("invalid write result=$result") { diagnostic("invalid playback write") }
            playbackEnded()
            if (result != 1) start(7)
        }
    }

    @Test fun nativePullExceptionIsLoggedAndReleasesPlayback() {
        NativeLib.radioFailures = setOf("pull")
        await("pull diagnostic") { diagnostic("playback failed operation=pull") }
        playbackEnded()
    }

    @Test fun invalidNativePullCountsCannotReplayOldPcm() {
        for (result in listOf(-1, 1922, 1)) {
            NativeLib.radioPullResult = result
            await("invalid pull $result") { diagnostic("invalid native playback count") }
            playbackEnded(); NativeLib.radioPullResult = 0
            if (result != 1) start(7)
        }
    }

    @Test fun shortWritesPreserveEveryPcmFrameAndShortPullsZeroTheirUnusedTail() {
        val track = AudioTrack.created.single()
        track.maxWriteSamples = 128
        await("short writes") { track.writes.size >= 30 }
        val writes = track.writes.toList()
        val first = writes.indexOfFirst { it.offset == 0 && it.result == 128 }
        assertTrue(first >= 0)
        assertEquals((0 until 1920 step 128).toList(), writes.drop(first).take(15).map { it.offset })
        assertTrue(writes.drop(first).take(15).all { it.result == 128 })
        track.maxWriteSamples = Int.MAX_VALUE; track.writes.clear()
        NativeLib.radioPullResults.addAll(listOf(1920, 1000, 0))
        await("full and partial pull") { track.writes.toList().count { it.pcm.firstOrNull() == 7.toShort() } >= 2 }
        val audio = track.writes.toList().filter { it.offset == 0 && it.pcm.firstOrNull() == 7.toShort() }
        assertTrue(audio.first().pcm.all { it == 7.toShort() })
        assertTrue(audio[1].pcm.take(1000).all { it == 7.toShort() }); assertTrue(audio[1].pcm.drop(1000).all { it == 0.toShort() })
        assertTrue(engine.isActive())
    }

    @Test fun rejectedMainQueueCannotLeaveFailedPlaybackOrItsCaptureRunning() {
        begin(); Handler.rejectImmediate = true
        AudioTrack.created.single().forcedWriteResult = -6
        await("cleanup without main", pumpMain = false) { AudioTrack.created.single().released && !NativeLib.radioCaptureAlive }
        await("queue rejection diagnostic", pumpMain = false) { diagnostic("readiness callback rejected") }
    }

    @Test fun unexpectedPlaybackInterruptionIsAnErrorAndReleasesTheSession() {
        worker("playThread").interrupt()
        await("interruption diagnostic") { diagnostic("playback failed operation=sleep") }
        playbackEnded()
    }

    @Test fun normalStopDoesNotReportWorkerCancellationAsAnIoError() {
        begin(); engine.stop().get(5, TimeUnit.SECONDS); main.dispatchAll()
        assertFalse(diagnostic("playback failed")); assertFalse(diagnostic("capture failed"))
    }

    @Test fun aWriteUnblockedByStopIsCancellationEvenWhenItReturnsZero() {
        val entered = CountDownLatch(1)
        val gate = CountDownLatch(1).also { gates.add(it) }
        AudioTrack.created.single().apply {
            onWrite = {
                entered.countDown()
                try { gate.await(5, TimeUnit.SECONDS) } catch (_: InterruptedException) { /* stop разблокировал write. */ }
            }
            forcedWriteResult = 0
        }
        assertTrue(entered.await(5, TimeUnit.SECONDS))
        engine.stop().get(5, TimeUnit.SECONDS); main.dispatchAll()
        assertFalse(diagnostic("playback failed"))
    }

    @Test fun captureStartupFailuresBeforeAndAfterNativeEffectNeverLoseTheSubscription() {
        for (failure in listOf("capture reject", "capture start", "mode", "bad mode", "begin")) {
            NativeLib.radioFailures = setOf(failure)
            assertTrue(engine.talkBegin()); drain()
            await("capture startup diagnostic $failure") { diagnostic("capture startup failed") }
            captureEnded()
            val mediaIo = MediaAudioSession::class.java.getDeclaredField("io").apply { isAccessible = true }
                .get(MediaAudioController) as ExecutorService
            assertTrue(mediaIo.submit<Boolean> { AndroidMicrophoneCapture.opened.all { it.closed } }.get(5, TimeUnit.SECONDS))
            NativeLib.radioFailures = emptySet(); engine.cancelTalk()
        }
    }

    @Test fun failedNativeCaptureStopRetainsItsOwnerForTheNextCleanupAttempt() {
        begin(); NativeLib.radioFailures = setOf("capture stop")
        engine.cancelTalk(); drain()
        assertTrue(NativeLib.radioCaptureAlive); assertTrue(diagnostic("native capture stop failed"))
        val before = count("capture stop")
        NativeLib.radioFailures = emptySet(); engine.setLevelMonitoring(false); drain()
        assertEquals(before + 1, count("capture stop")); captureEnded()
    }

    @Test fun feedExceptionsAndRejectedFramesStopOnlyCaptureAndAllowANewManualAttempt() {
        for (failure in listOf("feed", "feed reject")) {
            begin(); NativeLib.radioFailures = setOf(failure)
            await("feed diagnostic $failure") { diagnostic("capture failed operation=feed") }
            captureEnded(); NativeLib.radioFailures = emptySet(); engine.cancelTalk()
        }
        begin(); assertTrue(engine.isActive())
    }

    @Test fun nativeEndFailureClosesCaptureInsteadOfLeavingManualTransmissionRunning() {
        engine.setLevelMonitoring(true); drain(); begin()
        NativeLib.radioFailures = setOf("end")
        engine.cancelTalk(); drain()
        await("end diagnostic") { diagnostic("capture failed operation=control end") }
        captureEnded()
    }

    @Test fun unexpectedCapturePollInterruptionClosesItsSubscriptionButKeepsPlayback() {
        begin(); worker("captureThread").interrupt()
        await("poll diagnostic") { diagnostic("capture failed operation=poll") }
        captureEnded()
    }

    @Test fun malformedCaptureFramesAreRejectedBeforeCallingNativeFeed() {
        begin()
        val current = requireNotNull(subscription())
        current.deliver(MicrophoneCapture.Frame(ShortArray(2), System.nanoTime(), 0))
        await("frame diagnostic") { diagnostic("invalid capture frame") }
        captureEnded()
    }

    @Test fun nativeBoundaryFailureEndsCaptureAndDoesNotLeakItsWorker() {
        begin()
        await("initial capture frame") { synchronized(NativeLib.radioFeeds) { NativeLib.radioFeeds.isNotEmpty() } }
        NativeLib.radioFailures = setOf("boundary")
        requireNotNull(subscription()).deliver(MicrophoneCapture.Frame(ShortArray(1920), System.nanoTime(), Long.MAX_VALUE, generation = 99))
        await("boundary diagnostic") { diagnostic("capture failed operation=discontinuity") }
        captureEnded()
    }

    @Test fun missingAutomaticDetectorClosesOnlyAutomaticCaptureAndStillAllowsManualPtt() {
        context.configProvider.radioSettings = RadioAutoSettings(enabled = true)
        NativeLib.radioFailures = setOf("detector off")
        engine.reconfigure(); drain(); engine.onSettingsApplied(NativeLib.radioRequest, true); drain()
        await("automatic detector diagnostic") { diagnostic("Automatic transmission unavailable") }
        assertFalse(NativeLib.radioCaptureAlive); assertTrue(engine.isActive())
        begin(); assertEquals(RadioAutoSettings.OFF, NativeLib.radioAutoMode())
    }

    @Test fun latePlaybackFailureCleanupCannotStopAReplacementGroup() {
        val entered = CountDownLatch(1)
        val gate = CountDownLatch(1).also { gates.add(it) }
        io.execute { entered.countDown(); gate.await(5, TimeUnit.SECONDS) }
        assertTrue(entered.await(5, TimeUnit.SECONDS))
        engine.start("08".repeat(32))
        AudioTrack.created.single().failures = setOf("write")
        await("old playback failure", pumpMain = false) { diagnostic("playback failed operation=write") }
        gate.countDown(); drain(); main.dispatchAll()
        assertEquals("08".repeat(32), engine.activeGroup.value); assertTrue(engine.isActive())
        assertFalse(AudioTrack.created.last().released)
    }

    @Test fun anInFlightOldFeedFailureCannotClearANewGroupsManualRequest() {
        val feedEntered = CountDownLatch(1)
        val feedGate = CountDownLatch(1).also { gates.add(it) }
        NativeLib.onRadioFeed = { group ->
            if (group == "07".repeat(32)) { feedEntered.countDown(); feedGate.await(5, TimeUnit.SECONDS); throw IllegalStateException("old feed failed") }
        }
        begin(); assertTrue(feedEntered.await(5, TimeUnit.SECONDS))
        val ioEntered = CountDownLatch(1)
        val ioGate = CountDownLatch(1).also { gates.add(it) }
        io.execute { ioEntered.countDown(); ioGate.await(5, TimeUnit.SECONDS) }
        assertTrue(ioEntered.await(5, TimeUnit.SECONDS))
        engine.start("08".repeat(32)); assertTrue(engine.talkBegin(RadioPttSource.VOLUME))
        feedGate.countDown()
        await("old feed failure", pumpMain = false) { diagnostic("capture failed operation=feed") }
        ioGate.countDown(); drain(); main.dispatchAll()
        NativeLib.onRadioFeed = {}; engine.onSettingsApplied(NativeLib.radioRequest, true); drain()
        await("new manual request") { NativeLib.radioTransmitting() }
        assertEquals("08".repeat(32), engine.activeGroup.value); assertTrue(engine.isTalking())
    }

    @Test fun aLateCaptureStartupFailureCannotClearANewGroupsManualRequest() {
        val entered = CountDownLatch(1)
        val gate = CountDownLatch(1).also { gates.add(it) }
        NativeLib.onRadioCaptureStart = { group ->
            if (group == "07".repeat(32)) { entered.countDown(); gate.await(5, TimeUnit.SECONDS); throw IllegalStateException("old capture startup failed") }
        }
        assertTrue(engine.talkBegin()); assertTrue(entered.await(5, TimeUnit.SECONDS))
        engine.start("08".repeat(32)); assertTrue(engine.talkBegin(RadioPttSource.VOLUME))
        gate.countDown(); drain(); main.dispatchAll()
        NativeLib.onRadioCaptureStart = {}; engine.onSettingsApplied(NativeLib.radioRequest, true); drain()
        await("new manual request after old startup failure") { NativeLib.radioTransmitting() }
        assertEquals("08".repeat(32), engine.activeGroup.value); assertTrue(engine.isTalking())
    }
}
