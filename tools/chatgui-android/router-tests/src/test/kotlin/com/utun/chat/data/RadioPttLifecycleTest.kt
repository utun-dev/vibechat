package com.utun.chat.data

import android.content.Context
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Handler
import android.os.Looper
import com.utun.chat.ChatApplication
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие engine/PCM/session: проверяется native-команда отбоя, а не только UI-флаг PTT. */
class RadioPttLifecycleTest {
    private lateinit var context: Context
    private lateinit var engine: RadioAudioEngine
    private lateinit var io: ExecutorService
    private val main get() = Looper.getMainLooper()

    @Before fun prepare() {
        assertSame(main, Looper.myLooper())
        context = Context(); ChatApplication.instance = context
        context.configProvider.keepMicrophoneReady = false
        AudioManager.shared = AudioManager(); CallController.blocked = false
        AndroidMicrophoneCapture.reset(); AudioTrack.created.clear()
        Handler.rejectDelayed = false; MicrophonePermission.requester = null
        LogManager.entries.clear(); LogManager.console = true
        NativeLib.radioCommands.clear()
        engine = RadioAudioEngine()
        // Барьер собственного io-потока без подмены состояния движка или его capture/updateCapture.
        io = RadioAudioEngine::class.java.getDeclaredField("io").apply { isAccessible = true }.get(engine) as ExecutorService
        start(7)
    }

    @After fun release() {
        Handler.rejectDelayed = false; context.permissionGranted = true
        engine.stop().get(5, TimeUnit.SECONDS)
        main.advanceBy(10_000); drain() // Доставить прежние ACK-timeout callbacks до завершения их io-владельца.
        io.shutdown(); assertTrue(io.awaitTermination(5, TimeUnit.SECONDS))
        val barrier = Executors.newSingleThreadExecutor()
        try { barrier.submit { MediaAudioController.awaitIdle() }.get(5, TimeUnit.SECONDS) }
        finally { barrier.shutdownNow() }
        assertTrue(context.receivers.isEmpty())
        assertTrue(AudioTrack.created.all { it.released })
        assertTrue(AndroidMicrophoneCapture.opened.all { it.closed })
    }

    private fun drain() { io.submit {}.get(5, TimeUnit.SECONDS) }
    private fun start(group: Long) {
        var ready = false
        engine.start("%02x".format(group).repeat(32)) { ready = it }
        drain(); main.dispatchAll()
        assertTrue("radio must be listening", ready)
        engine.onSettingsApplied(NativeLib.radioRequest, true); drain()
    }
    private fun await(message: String, condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!condition() && System.nanoTime() < deadline) Thread.sleep(2)
        assertTrue(message, condition())
    }
    private fun begin(source: RadioPttSource = RadioPttSource.BUTTON) {
        assertTrue(engine.talkBegin(source)); drain()
        await("native transmission must start") { NativeLib.radioTransmitting() }
    }
    private fun stopped() {
        drain()
        await("native transmission and UI request must stop") { !NativeLib.radioTransmitting() && !engine.isTalking() }
    }
    private fun count(kind: String) = synchronized(NativeLib.radioCommands) { NativeLib.radioCommands.count { it.kind == kind } }
    private fun diagnostic(fragment: String) = assertTrue("missing diagnostic: $fragment",
        synchronized(LogManager.entries) { LogManager.entries.any { fragment in it.message && it.level in listOf("ERROR", "WARN") } })

    @Test fun rejectedReleaseTimerImmediatelyEndsNativeTransmissionAndDoesNotLogScheduled() {
        begin(); Handler.rejectDelayed = true
        engine.talkEnd()
        diagnostic("PTT release timer rejected")
        stopped()
        assertEquals(1, count("capture stop"))
        assertFalse(synchronized(LogManager.entries) { LogManager.entries.any { "scheduled +" in it.message } })
    }

    @Test fun normalReleaseKeepsExactlyTheConfiguredTailBeforeStoppingCapture() {
        begin(); engine.talkEnd()
        main.advanceBy(RadioAudioEngine.RELEASE_HOLD_MS - 1); drain()
        assertTrue(engine.isTalking()); assertTrue(NativeLib.radioTransmitting())
        main.advanceBy(1); stopped()
        assertEquals(1, count("begin")); assertEquals(1, count("capture stop"))
    }

    @Test fun repressDuringTailCancelsTheOldStopWithoutStartingAnotherTransmission() {
        begin(); engine.talkEnd(); main.advanceBy(100)
        begin()
        main.advanceBy(1000); drain()
        assertTrue(engine.isTalking()); assertEquals(1, count("begin")); assertEquals(0, count("capture stop"))
        engine.talkEnd(); main.advanceBy(RadioAudioEngine.RELEASE_HOLD_MS); stopped()
    }

    @Test fun independentOwnersKeepTransmissionUntilTheLastReleaseEvenIfTimersAreRejected() {
        begin(); begin(RadioPttSource.OVERLAY)
        Handler.rejectDelayed = true
        engine.talkEnd(RadioPttSource.BUTTON); drain()
        assertTrue(engine.isTalking()); assertEquals(0, count("capture stop"))
        engine.talkEnd(RadioPttSource.OVERLAY)
        diagnostic("PTT release timer rejected"); stopped()
        assertEquals(1, count("capture stop"))
    }

    @Test fun duplicateAndUnknownReleasesDoNotExtendTheTailOrRequireExtraPressCounts() {
        begin(); begin()
        engine.talkEnd(RadioPttSource.OVERLAY)
        assertTrue(engine.isTalking())
        engine.talkEnd(); main.advanceBy(100); engine.talkEnd()
        main.advanceBy(RadioAudioEngine.RELEASE_HOLD_MS - 100); stopped()
        assertEquals(1, count("begin")); assertEquals(1, count("capture stop"))
    }

    @Test fun cancellationPreservesOtherOwnersAndCancelAllStopsImmediately() {
        begin(); begin(RadioPttSource.VOLUME)
        engine.cancelTalk(RadioPttSource.BUTTON); engine.cancelTalk(RadioPttSource.OVERLAY); drain()
        assertTrue(engine.isTalking()); assertTrue(NativeLib.radioTransmitting())
        engine.cancelTalk(); stopped()
        assertEquals(1, count("capture stop"))
    }

    @Test fun cancellingAlreadyReleasedOwnerRemovesThePendingTailImmediately() {
        begin(); engine.talkEnd()
        engine.cancelTalk(RadioPttSource.BUTTON); stopped()
        main.advanceBy(1000); drain()
        assertEquals(1, count("capture stop"))
    }

    @Test fun stoppingEngineDuringTailRemovesCallbacksAndReleasesOutputAndCapture() {
        begin(); engine.talkEnd()
        engine.stop().get(5, TimeUnit.SECONDS)
        main.advanceBy(1000); drain()
        assertFalse(engine.isActive()); assertFalse(engine.isTalking()); assertFalse(NativeLib.radioTransmitting())
        assertEquals(1, count("capture stop")); assertTrue(AudioTrack.created.single().released)
        assertTrue(context.receivers.isEmpty())
    }

    @Test fun oldTailCannotStopANewGroupOrKeepItsPreviousPttOwners() {
        begin(); engine.talkEnd()
        start(8); begin(RadioPttSource.VOLUME)
        main.advanceBy(1000); drain()
        assertEquals("08".repeat(32), engine.activeGroup.value); assertTrue(NativeLib.radioTransmitting())
        engine.talkEnd(RadioPttSource.BUTTON); drain(); assertTrue(engine.isTalking())
        engine.cancelTalk(RadioPttSource.VOLUME); stopped()
        assertEquals(listOf("07".repeat(32), "08".repeat(32)), synchronized(NativeLib.radioCommands) {
            NativeLib.radioCommands.filter { it.kind == "begin" }.map { it.group }
        })
    }

    @Test fun deniedPermissionDoesNotRetainAnOwnerOrResumeTransmissionAfterPermissionChanges() {
        context.permissionGranted = false
        assertFalse(engine.talkBegin()); assertEquals(1, context.activityAttempts.size)
        assertFalse(engine.isTalking()); assertEquals(0, count("capture start"))
        context.permissionGranted = true; main.advanceBy(1000); drain()
        assertEquals(0, count("begin"))
        begin(RadioPttSource.VOLUME)
        engine.talkEnd(RadioPttSource.VOLUME); main.advanceBy(RadioAudioEngine.RELEASE_HOLD_MS); stopped()
    }

    @Test fun workerPttCommandsAreRejectedBeforeChangingOwnersOrTransmission() {
        begin()
        for (action in listOf<() -> Unit>({ engine.talkBegin(RadioPttSource.OVERLAY) },
            { engine.talkEnd() }, { engine.cancelTalk() })) {
            val failure = AtomicReference<Throwable?>()
            Thread({ try { action() } catch (e: Throwable) { failure.set(e) } }, "radio-ptt-worker").apply { start(); join() }
            assertTrue("PTT must reject worker before mutation: ${failure.get()}", failure.get() is IllegalStateException)
        }
        diagnostic("PTT requires main thread")
        assertTrue(engine.isTalking()); assertEquals(1, count("begin"))
        engine.cancelTalk(RadioPttSource.BUTTON); stopped()
    }

    @Test fun rejectedTailWithLevelMonitorQueuesNativeEndWhileKeepingTheCaptureSubscription() {
        engine.setLevelMonitoring(true); drain()
        begin()
        Handler.rejectDelayed = true; engine.talkEnd()
        diagnostic("PTT release timer rejected"); stopped()
        assertEquals(1, count("end")); assertEquals(1, count("capture start")); assertEquals(0, count("capture stop"))
    }

    @Test fun inactiveEngineRejectsPttWithoutRequestingMicrophoneOrStartingCapture() {
        engine.stop().get(5, TimeUnit.SECONDS); context.permissionGranted = false
        assertFalse(engine.talkBegin())
        diagnostic("PTT begin ignored: inactive")
        assertTrue(context.activityAttempts.isEmpty()); assertEquals(0, count("capture start"))
    }
}
