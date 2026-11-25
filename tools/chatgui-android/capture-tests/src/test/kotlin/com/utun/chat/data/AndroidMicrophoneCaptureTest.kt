package com.utun.chat.data

import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.os.Process
import java.util.concurrent.CountDownLatch
import java.util.concurrent.FutureTask
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий AudioRecord reader: частичные чтения, измеренная шкала, routing и полный lifecycle без микрофона ОС. */
class AndroidMicrophoneCaptureTest {
    private val now = AtomicLong(1_000_000_000L)
    private val input = AudioDeviceInfo(7)
    private val owners = mutableListOf<AndroidMicrophoneCapture>()
    private val frames = LinkedBlockingQueue<MicrophoneCapture.Frame>()
    private val errors = LinkedBlockingQueue<Exception>()
    private val confirmations = AtomicInteger()
    private val confirmed = CountDownLatch(1)

    @Before fun prepare() {
        AudioRecord.reset(); Process.failPriority = false; Build.VERSION.SDK_INT = 36; LogManager.entries.clear()
    }

    @After fun release() {
        AudioRecord.created.forEach { it.plan.failures = emptySet(); it.plan.holdRead?.countDown() }
        owners.forEach { it.close() }
        // Частично созданный объект остаётся в fixture при инъекции ошибки release самого Android API.
        AudioRecord.created.filter { !it.plan.released }.forEach { it.release() }
        assertTrue(AudioRecord.created.all { it.plan.released })
    }

    private fun plan() = AudioRecord.Plan().also { it.clock = now::get; AudioRecord.plans.add(it) }
    private fun open(stereo: Boolean = false, voice: Boolean = false, source: Int = 0,
        accept: (MicrophoneCapture.Frame) -> Boolean = { frames.add(it); true },
        failed: (Exception) -> Unit = { errors.add(it) },
        confirmation: () -> Unit = { confirmations.incrementAndGet(); confirmed.countDown() }): AndroidMicrophoneCapture =
        AndroidMicrophoneCapture.open(input, voice, stereo, source, 44L, accept, failed, confirmation, now::get).also { owners.add(it) }
    private fun frame(): MicrophoneCapture.Frame = requireNotNull(frames.poll(3, TimeUnit.SECONDS)) { "No accepted PCM: ${LogManager.entries}" }
    private fun failure(fragment: String): Exception = requireNotNull(errors.poll(3, TimeUnit.SECONDS)).also {
        assertTrue("Expected $fragment: $it", it.message?.contains(fragment) == true)
    }
    private fun log(fragment: String) = assertTrue("Missing diagnostic $fragment", LogManager.entries.any { it.message.contains(fragment) })

    @Test fun monoPcmIsStereoAndPreservesGenerationAndSequence() {
        val plan = plan(); val capture = open()
        plan.chunks.add(AudioRecord.Chunk(value = 31))
        val first = frame()
        assertEquals(1920, first.pcm.size); assertTrue(first.pcm.all { it == 31.toShort() })
        assertEquals(44L, first.generation); assertEquals(0L, first.sequence)
        now.addAndGet(20_000_000L); plan.chunks.add(AudioRecord.Chunk(value = 42))
        val second = frame(); assertEquals(960L, second.sequence); assertTrue(second.pcm.all { it == 42.toShort() })
        capture.close(); assertEquals(1, confirmations.get())
        assertTrue(plan.modes.all { it == AudioRecord.READ_NON_BLOCKING })
    }

    @Test fun partialStereoReadsProduceOneWholeFrame() {
        val plan = plan(); open(stereo = true)
        plan.chunks.add(AudioRecord.Chunk(count = 480, value = 1))
        plan.chunks.add(AudioRecord.Chunk(count = 1440, value = 2))
        val frame = frame()
        assertTrue(frame.pcm.take(480).all { it == 1.toShort() })
        assertTrue(frame.pcm.drop(480).all { it == 2.toShort() })
        assertEquals(0L, frame.sequence)
    }

    @Test fun sourceAndChannelPolicyMatchVoiceAndStereoConfiguration() {
        for ((setting, expected) in listOf(0 to MediaRecorder.AudioSource.VOICE_COMMUNICATION, 1 to MediaRecorder.AudioSource.MIC,
            2 to MediaRecorder.AudioSource.UNPROCESSED, 3 to MediaRecorder.AudioSource.CAMCORDER)) {
            plan(); open(source = setting).close()
            assertEquals(expected, AudioRecord.created.last().source)
        }
        plan(); open(stereo = true).close()
        assertEquals(MediaRecorder.AudioSource.CAMCORDER, AudioRecord.created.last().source)
        plan(); open(stereo = true, voice = true).close()
        assertEquals(AudioFormat.CHANNEL_IN_MONO, AudioRecord.created.last().mask)
        assertEquals(MediaRecorder.AudioSource.VOICE_COMMUNICATION, AudioRecord.created.last().source)
    }

    @Test fun rejectedStereoCleansFirstRecordAndFallsBackToMono() {
        val stereo = plan().apply { acceptInput = false }; val mono = plan()
        open(stereo = true).close()
        assertEquals(listOf(AudioFormat.CHANNEL_IN_STEREO, AudioFormat.CHANNEL_IN_MONO), AudioRecord.masks.toList())
        assertEquals(1, stereo.releaseCalls.get()); assertEquals(1, mono.releaseCalls.get())
    }

    @Test fun unsupportedStereoSkipsConstructorAndUsesMono() {
        AudioRecord.minimums[AudioFormat.CHANNEL_IN_STEREO] = -2
        plan(); open(stereo = true).close()
        assertEquals(listOf(AudioFormat.CHANNEL_IN_MONO), AudioRecord.masks.toList())
    }

    @Test fun openFailureMatrixReleasesAllocatedRecord() {
        for (operation in listOf("state", "prefer", "start", "start after", "recording", "channels")) {
            val plan = plan().apply { failures = setOf(operation) }
            assertThrows(IllegalStateException::class.java) { open() }
            assertEquals(operation, 1, plan.releaseCalls.get())
            assertTrue(plan.released)
        }
    }

    @Test fun uninitializedOrNotStartedRecordIsNotPublished() {
        val uninitialized = plan().apply { initialized = false }
        assertThrows(IllegalStateException::class.java) { open() }
        assertTrue(uninitialized.released)
        val stopped = plan().apply { starts = false }
        assertThrows(IllegalStateException::class.java) { open() }
        assertTrue(stopped.released)
        assertTrue(owners.isEmpty())
    }

    @Test fun releaseFailureDuringOpenAbortsFallbackAndKeepsOriginalCause() {
        val plan = plan().apply { acceptInput = false; failures = setOf("release") }
        val error = assertThrows(IllegalStateException::class.java) { open(stereo = true) }
        assertTrue(error.message!!.contains("fallback aborted"))
        assertTrue(error.cause!!.message!!.contains("Input selection rejected"))
        assertEquals(1, error.cause!!.suppressed.size)
        assertEquals(1, AudioRecord.created.size)
        assertEquals(1, plan.releaseCalls.get()); log("capture open cleanup failed")
    }

    @Test fun setupFailureInsideWorkerReachesFailureCallback() {
        plan().failChannelsAt = 3
        open()
        failure("channel query failed")
    }

    @Test fun deniedPriorityStillProducesUsefulPcm() {
        Process.failPriority = true
        val plan = plan(); open(); plan.chunks.add(AudioRecord.Chunk())
        frame(); log("capture audio priority unavailable"); assertTrue(errors.isEmpty())
    }

    @Test fun noDataBeforeConfirmationFailsWithoutBlockingRead() {
        val plan = plan(); open()
        assertTrue(plan.readEntered.await(3, TimeUnit.SECONDS))
        now.addAndGet(6_000_000_000L)
        failure("data timeout")
        assertTrue(plan.modes.all { it == AudioRecord.READ_NON_BLOCKING })
    }

    @Test fun stalledDataAfterConfirmationUsesOwnRecoveryDeadline() {
        val plan = plan(); open(); plan.chunks.add(AudioRecord.Chunk())
        frame(); assertTrue(confirmed.await(3, TimeUnit.SECONDS))
        now.addAndGet(1_100_000_000L)
        failure("data timeout")
    }

    @Test fun nativeReadErrorsAreReportedAndDoNotPublishPartialPcm() {
        for (code in listOf(-2, -3, -6)) {
            val plan = plan(); val capture = open()
            plan.chunks.add(AudioRecord.Chunk(count = 240))
            plan.chunks.add(AudioRecord.Chunk(count = code))
            failure("read=$code"); assertTrue(frames.isEmpty()); capture.close()
        }
    }

    @Test fun malformedStereoCountFailsWithContext() {
        val plan = plan(); open(stereo = true)
        plan.chunks.add(AudioRecord.Chunk(count = 1))
        failure("channels=2")
    }

    @Test fun startupRouteOrSilencingMismatchCannotPublishPcm() {
        for ((device, silenced) in listOf(8 to false, 7 to true)) {
            val plan = plan(); val capture = open()
            plan.chunks.add(AudioRecord.Chunk(device = device, silenced = silenced))
            assertTrue(plan.readEntered.await(3, TimeUnit.SECONDS))
            // Следующая точка clock не истекает до того, как metadata mismatch действительно проверен.
            awaitLog("capture waiting metadata")
            assertTrue(frames.isEmpty())
            now.addAndGet(6_000_000_000L); failure("timeout")
            capture.close(); LogManager.entries.clear()
        }
    }

    @Test fun confirmedRouteLossImmediatelyReportsFailure() {
        val plan = plan(); open(); plan.chunks.add(AudioRecord.Chunk()); frame()
        plan.chunks.add(AudioRecord.Chunk(device = 8))
        failure("route lost")
    }

    @Test fun missingTimestampAfterReadyGetsRecoveryBudgetAndCanResume() {
        val plan = plan(); open(); plan.chunks.add(AudioRecord.Chunk()); frame()
        now.addAndGet(100_000_000L); plan.chunks.add(AudioRecord.Chunk(measured = false))
        awaitLog("capture waiting metadata")
        now.addAndGet(20_000_000L); plan.chunks.add(AudioRecord.Chunk())
        val next = frame(); assertEquals(1920L, next.sequence)
        assertTrue(errors.isEmpty()); assertEquals(1, confirmations.get())
    }

    @Test fun timestampRecoveryBudgetExpiresAndIsDiagnosed() {
        val plan = plan(); open(); plan.chunks.add(AudioRecord.Chunk()); frame()
        now.addAndGet(100_000_000L); plan.chunks.add(AudioRecord.Chunk(measured = false))
        awaitLog("capture waiting metadata")
        plan.chunks.add(AudioRecord.Chunk(measured = false, onRead = { now.addAndGet(1_100_000_000L) }))
        failure("confirmation timeout")
    }

    @Test fun futureOrNegativeTimestampCannotConfirmCapture() {
        for (time in listOf(0L, -1L, now.get() + 100_000_000L)) {
            val plan = plan(); val capture = open()
            plan.chunks.add(AudioRecord.Chunk(timeNs = time))
            awaitLog("capture waiting metadata")
            assertTrue(frames.isEmpty()); assertEquals(0, confirmations.get())
            capture.close(); LogManager.entries.clear()
        }
    }

    @Test fun prolongedBackpressureFailsWithoutFalseConfirmation() {
        val plan = plan(); open(accept = { false })
        plan.chunks.add(AudioRecord.Chunk())
        awaitReads(plan, 2)
        plan.chunks.add(AudioRecord.Chunk(onRead = { now.addAndGet(1_100_000_000L) }))
        failure("exceeds capture reserve"); assertEquals(0, confirmations.get())
    }

    @Test fun closeIsIdempotentEvenWhenStopFails() {
        val plan = plan().apply { failures = setOf("stop") }
        val capture = open(); assertTrue(plan.readEntered.await(3, TimeUnit.SECONDS))
        repeat(3) { capture.close() }
        assertEquals(1, plan.stopCalls.get()); assertEquals(1, plan.releaseCalls.get())
        assertTrue(errors.isEmpty()); log("capture stop input=7")
    }

    @Test fun closePreservesInterruptedCallerAndReleaseCanRetry() {
        val plan = plan().apply { failures = setOf("release") }; val capture = open()
        assertThrows(IllegalStateException::class.java) { capture.close() }
        assertEquals(1, plan.releaseCalls.get())
        plan.failures = emptySet()
        val result = FutureTask {
            Thread.currentThread().interrupt(); capture.close(); Thread.currentThread().isInterrupted
        }
        Thread(result).apply { isDaemon = true; start() }
        assertTrue(result.get(3, TimeUnit.SECONDS))
        assertEquals(1, plan.stopCalls.get()); assertEquals(2, plan.releaseCalls.get())
    }

    @Test fun interruptedJoinWaitsForReaderBeforeRelease() {
        val gate = CountDownLatch(1); val plan = plan().apply { holdRead = gate }
        val capture = open(); assertTrue(plan.readEntered.await(3, TimeUnit.SECONDS))
        val done = FutureTask { capture.close(); Thread.currentThread().isInterrupted }
        val closer = Thread(done).apply { isDaemon = true; start() }
        try {
            assertTrue(plan.stopEntered.await(3, TimeUnit.SECONDS)); closer.interrupt()
            awaitLog("capture join interrupted")
            assertEquals(0, plan.releaseCalls.get())
        } finally { gate.countDown() }
        assertTrue(done.get(3, TimeUnit.SECONDS)); assertEquals(1, plan.releaseCalls.get())
    }

    @Test fun concurrentCloseReleasesExactlyOnce() {
        val plan = plan(); val capture = open(); val go = CountDownLatch(1)
        val calls = List(8) { FutureTask { go.await(); capture.close() }.also { task ->
            Thread(task).apply { isDaemon = true; start() }
        } }
        go.countDown(); calls.forEach { it.get(3, TimeUnit.SECONDS) }
        assertEquals(1, plan.stopCalls.get()); assertEquals(1, plan.releaseCalls.get())
    }

    @Test fun selfCloseInCallbackIsRejectedAndFailureCallbackExceptionIsLogged() {
        val plan = plan(); lateinit var capture: AndroidMicrophoneCapture
        capture = open(accept = { capture.close(); true }, failed = { errors.add(it); error("failure observer denied") })
        plan.chunks.add(AudioRecord.Chunk())
        failure("own reader"); capture.close()
        log("capture close from reader rejected"); log("capture failure callback threw")
    }

    private fun awaitLog(fragment: String) {
        val deadline = System.nanoTime() + 3_000_000_000L
        while (System.nanoTime() < deadline) {
            if (LogManager.entries.any { it.message.contains(fragment) }) return
            java.util.concurrent.locks.LockSupport.parkNanos(1_000_000L)
        }
        error("Missing diagnostic $fragment: ${LogManager.entries}")
    }
    private fun awaitReads(plan: AudioRecord.Plan, count: Int) {
        val deadline = System.nanoTime() + 3_000_000_000L
        while (plan.readCalls.get() < count && System.nanoTime() < deadline) java.util.concurrent.locks.LockSupport.parkNanos(1_000_000L)
        assertTrue(plan.readCalls.get() >= count)
    }
}
