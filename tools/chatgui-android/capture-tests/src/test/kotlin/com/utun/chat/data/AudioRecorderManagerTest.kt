package com.utun.chat.data

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import com.utun.chat.ChatApplication
import com.utun.chat.RecorderHostContext
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий lifecycle записи/плеера: отказы Android/JNI и частичные записи PCM без аппаратных устройств. */
class AudioRecorderManagerTest {
    private val manager = AudioRecorderManager()
    @Before fun prepare() {
        AudioRecord.reset(); AudioTrack.reset(); NativeLib.reset(); StreamController.blocked = false
        ChatApplication.instance = RecorderHostContext(); LogManager.entries.clear()
    }
    @After fun release() {
        NativeLib.failCancel = false; NativeLib.failDecoderClose = false
        AudioRecord.created.forEach { it.plan.failures = emptySet(); it.plan.chunks.offer(AudioRecord.Chunk(count = -3)) }
        AudioTrack.created.forEach { it.plan.failures = emptySet() }
        manager.cancelRecording(); manager.stopMicTest(); manager.stopPlayback()
        manager.requestClose().get(3, TimeUnit.SECONDS)
        AudioRecord.created.forEach {
            it.plan.reader?.join(2_000)
            assertFalse("reader survived cleanup", it.plan.reader?.isAlive == true)
            if (!it.plan.released) it.release()
        }
    }
    private fun plan() = AudioRecord.Plan().also { AudioRecord.plans.add(it) }
    private fun waitUntil(done: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2)
        while (!done() && System.nanoTime() < deadline) Thread.sleep(2)
        assertTrue("worker did not reach expected state", done())
    }
    private fun diagnostic(fragment: String) = assertTrue("missing diagnostic: $fragment",
        LogManager.entries.any { it.level in listOf("WARN", "ERROR") && fragment in it.message })

    @Test fun startupFailureMatrixRollsBackAndroidAndNativeOwnership() {
        for (operation in listOf("state", "start", "start after", "recording")) {
            val device = plan().apply { failures = setOf(operation) }
            val result = runCatching { manager.startRecording("7") }
            assertEquals("startup escaped: $operation ${result.exceptionOrNull()}", false, result.getOrNull())
            assertFalse(manager.isActive()); assertFalse(NativeLib.active)
            assertTrue("unreleased device: $operation", device.released)
        }
    }

    @Test fun uninitializedOrNotRecordingDeviceIsRejected() {
        for (ready in listOf(false, true)) {
            val device = plan().apply { initialized = ready; starts = false }
            assertFalse(manager.startRecording("7"))
            assertFalse(NativeLib.active); assertTrue(device.released)
        }
    }

    @Test fun invalidMinimumDoesNotCreateOrPublishARecorder() {
        AudioRecord.minimums[AudioFormat.CHANNEL_IN_MONO] = -2
        plan()
        assertFalse(manager.startRecording("7")); assertFalse(NativeLib.active)
        assertTrue(AudioRecord.created.isEmpty()); diagnostic("minimum")
    }

    @Test fun rejectedNativeTestDoesNotCaptureOrCancelAnotherOwner() {
        NativeLib.active = true
        plan()
        assertFalse(manager.startMicTest())
        assertTrue(AudioRecord.created.isEmpty()); assertTrue(NativeLib.active)
        assertEquals(0, NativeLib.cancels.get())
    }

    @Test fun stoppingAnInactiveTestKeepsTheMessageRecording() {
        plan(); assertTrue(manager.startRecording("7"))
        manager.stopMicTest()
        assertTrue(manager.isActive()); assertTrue(NativeLib.active)
        manager.cancelRecording(); assertFalse(NativeLib.active)
    }
    @Test fun disposingAMicrophoneTestCannotCancelAnUnrelatedMessageRecording() {
        plan(); assertTrue(manager.startRecording("7"))
        manager.requestMicTestStop().get(1, TimeUnit.SECONDS)
        assertTrue(manager.isActive()); assertTrue(NativeLib.active); assertEquals(0, NativeLib.cancels.get())
    }
    @Test fun disposingAnActiveTestClosesTheDeviceWithoutPublishingPcm() {
        val device = plan(); assertTrue(manager.startMicTest())
        manager.requestMicTestStop().get(2, TimeUnit.SECONDS)
        assertFalse(manager.isTesting); assertFalse(NativeLib.active); assertTrue(device.released)
        assertEquals(0, NativeLib.stops.get()); assertEquals(1, NativeLib.cancels.get())
    }

    @Test fun failedFeedAndReadDiscardInsteadOfPublishingPartialVoice() {
        for (feedFailure in listOf(false, true)) {
            NativeLib.acceptFeed = !feedFailure
            val device = plan()
            assertTrue(manager.startRecording("7"))
            device.chunks.add(AudioRecord.Chunk(count = if (feedFailure) 960 else -6))
            waitUntil { if (feedFailure) NativeLib.feeds.isNotEmpty() else device.readCalls.get() > 0 }
            waitUntil { !manager.isActive() }
            assertEquals(-1, manager.stopRecording())
            assertFalse(NativeLib.active); assertEquals(0, NativeLib.stops.get()); assertTrue(device.released)
        }
    }

    @Test fun releaseFailureStillCancelsTheOwnedNativeRecording() {
        val device = plan(); assertTrue(manager.startRecording("7"))
        device.failures = setOf("release")
        val result = runCatching { manager.stopRecording() }
        assertEquals(-1, result.getOrNull()); assertFalse(NativeLib.active)
        assertEquals(1, NativeLib.cancels.get()); diagnostic("release")
        device.failures = emptySet()
        manager.cancelRecording()
        manager.requestClose().get(2, TimeUnit.SECONDS)
        assertTrue("cleanup lost the unreleased device", device.released)
        assertEquals(1, NativeLib.cancels.get())
    }

    @Test fun feedFailureWhileStopIsJoiningCannotPublishPartialPcm() {
        val entered = CountDownLatch(1); val leave = CountDownLatch(1)
        NativeLib.acceptFeed = false
        NativeLib.onFeed = {
            entered.countDown()
            var interrupted = false
            while (true) {
                try { check(leave.await(1, TimeUnit.SECONDS)); break }
                catch (_: InterruptedException) { interrupted = true }
            }
            if (interrupted) Thread.currentThread().interrupt()
        }
        val device = plan(); assertTrue(manager.startRecording("7"))
        device.chunks.add(AudioRecord.Chunk())
        assertTrue(entered.await(1, TimeUnit.SECONDS))
        val stopped = java.util.concurrent.FutureTask { manager.stopRecording() }
        val worker = Thread(stopped, "voice-stop-race").apply { start() }
        try {
            assertTrue(device.stopEntered.await(1, TimeUnit.SECONDS))
            assertFalse(manager.startMicTest())
            leave.countDown()
            assertEquals(-1, stopped.get(1, TimeUnit.SECONDS))
            assertEquals(0, NativeLib.stops.get()); assertEquals(1, NativeLib.cancels.get())
        } finally { leave.countDown(); worker.join(2_000) }
    }

    @Test fun testReaderIsStoppedBeforeJoinAndStopFailureCannotHangIt() {
        val device = plan(); assertTrue(manager.startMicTest())
        assertTrue(device.readEntered.await(1, TimeUnit.SECONDS))
        device.failures = setOf("stop")
        val done = CountDownLatch(1)
        val worker = Thread({ try { manager.stopMicTest() } finally { done.countDown() } }, "voice-test-stop")
        worker.start()
        try { assertTrue("test cleanup waited for a blocked read", done.await(300, TimeUnit.MILLISECONDS)) }
        finally {
            device.failures = emptySet(); device.chunks.offer(AudioRecord.Chunk(count = -3)); worker.join(2_000)
        }
        assertTrue(device.released); assertFalse(NativeLib.active)
    }

    @Test fun partialOutputWritesPreserveEveryDecodedSample() {
        val output = AudioTrack.Plan().also { AudioTrack.plans.add(it); it.results.add(240) }
        NativeLib.decoderReads.add(960)
        val done = CountDownLatch(1)
        manager.playVoiceFile("voice.opus", onComplete = { done.countDown() })
        assertTrue(done.await(2, TimeUnit.SECONDS))
        assertEquals(960, output.accepted.size); assertTrue(output.accepted.all { it == 12.toShort() })
        assertEquals(1, NativeLib.decoderCloses.get())
    }

    @Test fun failedOutputWriteIsNotReportedAsPlayedPcm() {
        AudioTrack.plans.add(AudioTrack.Plan().apply { results.add(-6) })
        NativeLib.decoderReads.add(960)
        val progress = AtomicInteger(); val done = CountDownLatch(1)
        manager.playVoiceFile("voice.opus", onProgress = { _, _ -> progress.incrementAndGet() }, onComplete = { done.countDown() })
        assertTrue(done.await(2, TimeUnit.SECONDS))
        assertEquals(0, progress.get()); assertEquals(1, NativeLib.decoderCloses.get()); diagnostic("write")
    }

    @Test fun trackCleanupFailureDoesNotLeakDecoderOrLoseCompletion() {
        val output = AudioTrack.Plan().also { AudioTrack.plans.add(it); it.failures = setOf("stop", "release") }
        val done = CountDownLatch(1)
        manager.playVoiceFile("voice.opus", onComplete = { done.countDown() })
        assertTrue(done.await(2, TimeUnit.SECONDS))
        assertEquals(1, NativeLib.decoderCloses.get()); assertEquals(1, output.releaseCalls); diagnostic("release")
    }

    @Test fun decoderErrorHasADiagnosticAndClosesItsHandle() {
        AudioTrack.plans.add(AudioTrack.Plan()); NativeLib.decoderReads.add(-1)
        val done = CountDownLatch(1)
        manager.playVoiceFile("voice.opus", onComplete = { done.countDown() })
        assertTrue(done.await(2, TimeUnit.SECONDS)); assertEquals(1, NativeLib.decoderCloses.get()); diagnostic("decode")
    }

    @Test fun disposedViewModelLeavesAProcessOwnerUntilCaptureReleaseIsAcknowledged() {
        val device = plan(); assertTrue(manager.startRecording("7")); device.failures = setOf("release")
        val closed = manager.requestClose()
        waitUntil { NativeLib.cancels.get() == 1 }
        assertFalse(closed.cancel(true))
        assertFalse(closed.isDone); assertFalse(NativeLib.active)
        assertThrows(IllegalStateException::class.java) { AudioRecorderManager.requireReleasedForCore() }
        device.failures = emptySet(); closed.get(2, TimeUnit.SECONDS)
        assertTrue(device.released); AudioRecorderManager.requireReleasedForCore()
    }

    @Test fun nativeCancelFailureRetainsTheCaptureUntilARealAcknowledgement() {
        plan(); assertTrue(manager.startRecording("7")); NativeLib.failCancel = true
        manager.cancelRecording(); assertTrue(NativeLib.active)
        val closed = manager.requestClose(); assertFalse(closed.isDone)
        assertThrows(IllegalStateException::class.java) { AudioRecorderManager.requireReleasedForCore() }
        NativeLib.failCancel = false; closed.get(2, TimeUnit.SECONDS)
        assertFalse(NativeLib.active); AudioRecorderManager.requireReleasedForCore()
    }

    @Test fun playbackReleaseFailureDoesNotLoseItsDeviceAfterCompletionCallback() {
        val output = AudioTrack.Plan().also { it.failures = setOf("release"); AudioTrack.plans.add(it) }
        val completed = CountDownLatch(1)
        manager.playVoiceFile("voice.opus", onComplete = { completed.countDown() })
        assertTrue(completed.await(2, TimeUnit.SECONDS)); assertEquals(1, NativeLib.decoderCloses.get())
        val closed = manager.requestClose(); assertFalse(closed.isDone); assertFalse(output.released)
        output.failures = emptySet(); closed.get(2, TimeUnit.SECONDS)
        assertTrue(output.released); assertTrue(output.releaseCalls >= 2)
    }

    @Test fun releaseAfterEffectStillNeedsAcknowledgementAndDoesNotCloseDecoderTwice() {
        val output = AudioTrack.Plan().also { it.failures = setOf("release after"); AudioTrack.plans.add(it) }
        val completed = CountDownLatch(1)
        manager.playVoiceFile("voice.opus", onComplete = { completed.countDown() })
        assertTrue(completed.await(2, TimeUnit.SECONDS)); assertTrue(output.released)
        val closed = manager.requestClose(); assertFalse(closed.isDone)
        output.failures = emptySet(); closed.get(2, TimeUnit.SECONDS)
        assertEquals(1, NativeLib.decoderCloses.get())
    }

    @Test fun decoderCloseFailureDoesNotSkipTheIndependentTrackRelease() {
        NativeLib.failDecoderClose = true
        val output = AudioTrack.Plan().also { AudioTrack.plans.add(it) }; val completed = CountDownLatch(1)
        manager.playVoiceFile("voice.opus", onComplete = { completed.countDown() })
        assertTrue(completed.await(2, TimeUnit.SECONDS)); assertTrue(output.released)
        val closed = manager.requestClose(); assertFalse(closed.isDone)
        NativeLib.failDecoderClose = false; closed.get(2, TimeUnit.SECONDS)
        assertEquals(1, NativeLib.decoderCloses.get()); assertEquals(1, output.releaseCalls)
    }

    @Test fun blockedNativeFeedCannotHoldMainWhileAnAsynchronousCloseIsPending() {
        val entered = CountDownLatch(1); val leave = CountDownLatch(1)
        NativeLib.onFeed = {
            entered.countDown()
            while (leave.count != 0L) try { leave.await() } catch (_: InterruptedException) {}
        }
        val device = plan(); assertTrue(manager.startRecording("7")); device.chunks.add(AudioRecord.Chunk())
        assertTrue(entered.await(1, TimeUnit.SECONDS))
        val closed = manager.requestClose()
        try {
            assertFalse(closed.isDone); assertFalse(manager.startRecording("8"))
            manager.pausePlayback(); manager.stopPlayback(); manager.cancelRecording(); manager.stopMicTest()
            assertThrows(IllegalStateException::class.java) { AudioRecorderManager.requireReleasedForCore() }
        } finally { leave.countDown() }
        closed.get(2, TimeUnit.SECONDS); assertTrue(device.released); assertFalse(NativeLib.active)
    }
}
