package com.utun.chat.data

import android.bluetooth.BluetoothAdapter
import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Looper
import com.utun.chat.ChatApplication
import com.utun.chat.R
import java.util.concurrent.Callable
import java.util.concurrent.Delayed
import java.util.concurrent.ExecutionException
import java.util.concurrent.FutureTask
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие session/router/PCM subscriptions; аппаратный reader и AudioTrack управляются host fixtures. */
class MediaAudioSessionTest {
    private lateinit var context: Context
    private lateinit var audio: AudioManager
    private lateinit var clock: ClockExecutor
    private var session: MediaAudioSession? = null
    private val subscriptions = mutableListOf<MicrophoneCapture.Subscription>()
    private val leases = mutableListOf<AutoCloseable>()
    private val tracks = mutableListOf<AudioTrack>()
    private val speaker = AudioDeviceInfo(1, 2, true, false, "speaker")
    private val mic = AudioDeviceInfo(2, 15, false, true, "microphone")
    private val wired = AudioDeviceInfo(3, 22, true, false, "USB output")
    private val wiredMic = AudioDeviceInfo(4, 22, false, true, "USB input")
    private val owner get() = requireNotNull(session)

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        context = Context().also { ChatApplication.instance = it }
        audio = AudioManager().also { AudioManager.shared = it; it.devices = arrayOf(speaker, mic) }
        BluetoothAdapter.current = null; CallController.blocked = false
        AndroidMicrophoneCapture.reset()
        clock = ClockExecutor()
        LogManager.entries.clear(); LogManager.console = true
    }

    @After fun release() {
        audio.failures = emptySet(); context.configProvider.failMediaWrite = false; context.configProvider.failMediaRead = false
        AndroidMicrophoneCapture.failClose = false; AndroidMicrophoneCapture.failOpen = false
        tracks.forEach { it.failures = emptySet() }
        subscriptions.forEach { it.close() }
        leases.forEach { background { it.close() } }
        session?.let { background { it.close() } }
        Looper.getMainLooper().dispatchAll()
        clock.shutdownNow(); assertTrue(clock.awaitTermination(5, TimeUnit.SECONDS))
        assertTrue(audio.callbacks.isEmpty())
        assertTrue(AndroidMicrophoneCapture.opened.all { it.closed })
        CallController.blocked = false
    }

    private fun start() { session = MediaAudioSession(clock); drain() }
    private fun drain() { repeat(4) { clock.submit {}.get(5, TimeUnit.SECONDS) } }
    private fun subscribe(name: String = "radio"): MicrophoneCapture.Subscription = owner.subscribe(name).also { subscriptions.add(it); drain() }
    private fun track(id: Int = tracks.size + 1) = AudioTrack(id).also { tracks.add(it) }
    private fun attach(track: AudioTrack): AutoCloseable = background { owner.attachOutput(track) }.also { leases.add(it); drain() }
    private fun diagnostic(fragment: String) = assertTrue("Missing diagnostic: $fragment",
        LogManager.entries.any { it.message.contains(fragment) && it.level in listOf("ERROR", "WARN") })
    private fun event() { audio.callbacks.single().onAudioDevicesAdded(emptyArray()); drain() }
    private fun select(profile: MediaAudioProfile) { owner.select(profile); drain() }

    @Test fun idleMonitoringDoesNotOpenHardwareOrTakeAudioMode() {
        start()
        assertEquals(1, audio.callbacks.size)
        assertTrue(AndroidMicrophoneCapture.opened.isEmpty())
        assertEquals(AudioManager.MODE_NORMAL, audio.mode)
        assertTrue(audio.operations.none { it == "clear" || it.startsWith("select=") || it.startsWith("mode=") })
        assertEquals(0, clock.activeTimers)
    }

    @Test fun radioOutputRetainsVoiceRouteBetweenPttWithoutRetainingMicrophone() {
        start()
        val output = track().apply {
            audioAttributes = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION).build()
        }
        val lease = attach(output)
        assertEquals(AudioManager.MODE_IN_COMMUNICATION, audio.mode)
        assertTrue(AndroidMicrophoneCapture.opened.isEmpty())
        val modeChanges = audio.operations.count { it.startsWith("mode=") }
        repeat(2) {
            val sub = subscribe()
            assertTrue(AndroidMicrophoneCapture.opened.last().voice)
            sub.close(); drain()
            assertTrue(AndroidMicrophoneCapture.opened.last().closed)
            assertEquals(AudioManager.MODE_IN_COMMUNICATION, audio.mode)
            assertEquals(modeChanges, audio.operations.count { it.startsWith("mode=") })
        }
        background { lease.close() }; drain()
        assertEquals(AudioManager.MODE_NORMAL, audio.mode)
    }

    @Test fun mediaPlaybackKeepsNormalRouteWithoutOpeningMicrophone() {
        start(); attach(track())
        assertEquals(AudioManager.MODE_NORMAL, audio.mode)
        assertTrue(AndroidMicrophoneCapture.opened.isEmpty())
    }

    @Test fun disconnectedBluetoothUsesSpeakerWithoutErrorForStreamAndRadio() {
        context.configProvider.mediaProfile = MediaAudioProfile.BLUETOOTH
        start()
        for (usage in listOf(AudioAttributes.USAGE_MEDIA, AudioAttributes.USAGE_VOICE_COMMUNICATION)) {
            val output = track().apply { audioAttributes = AudioAttributes.Builder().setUsage(usage).build() }
            val lease = attach(output)
            val state = owner.state.value
            assertEquals(MediaAudioProfile.BLUETOOTH, state.preferred)
            assertEquals(MediaAudioProfile.PHONE, state.active)
            assertEquals("speaker", state.outputName)
            assertEquals(MediaAudioNotice.FALLBACK, state.notice)
            assertFalse(requireNotNull(state.notice).isError)
            background { lease.close() }; drain()
        }
        assertTrue(context.configProvider.mediaSaves.isEmpty())
    }

    @Test fun explicitMicSourceKeepsNormalRouteEvenWithRadioOutput() {
        context.configProvider.micSource = 1
        start()
        attach(track().apply {
            audioAttributes = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION).build()
        })
        val sub = subscribe()
        assertFalse(AndroidMicrophoneCapture.opened.last().voice)
        assertEquals(AudioManager.MODE_NORMAL, audio.mode)
        sub.close(); drain()
        assertEquals(AudioManager.MODE_NORMAL, audio.mode)
    }

    @Test fun startupFailureIsBoundedAndExplicitRefreshRestartsMonitor() {
        audio.failures = setOf("register")
        start()
        assertEquals(2, audio.callbackHistory.size)
        assertTrue(audio.callbacks.isEmpty())
        diagnostic("fallback failed")
        audio.failures = emptySet(); owner.refresh(); drain()
        assertEquals(1, audio.callbacks.size)
        assertEquals(3, audio.callbackHistory.size)
    }

    @Test fun storageReadFailureCanRecoverSavedPreference() {
        context.configProvider.mediaProfile = MediaAudioProfile.WIRED
        context.configProvider.failMediaRead = true
        start()
        assertEquals(2, context.configProvider.mediaLoads)
        context.configProvider.failMediaRead = false
        owner.refresh(); drain()
        assertEquals(MediaAudioProfile.WIRED, owner.state.value.preferred)
        assertEquals(3, context.configProvider.mediaLoads)
    }

    @Test fun recoveredStartupUsesFirstKnownDevicesInsteadOfLatchingPhoneFallback() {
        audio.devices += arrayOf(wired, wiredMic)
        audio.failures = setOf("register")
        start()
        audio.failures = emptySet(); owner.refresh(); drain()
        assertEquals(MediaAudioProfile.WIRED, owner.state.value.active)
    }

    @Test fun subscriptionOnControlWorkerStillRequestsCaptureReconciliation() {
        start()
        val sub = clock.submit<MicrophoneCapture.Subscription> { owner.subscribe("radio") }.get(5, TimeUnit.SECONDS)
        subscriptions.add(sub); drain()
        assertEquals(1, AndroidMicrophoneCapture.opened.size)
    }

    @Test fun terminalInputOpenFailureCanRetrySameRouteWithoutOverlappingReaders() {
        AndroidMicrophoneCapture.failOpen = true
        start(); subscribe()
        val attempts = AndroidMicrophoneCapture.attempts
        assertTrue(attempts in 1..2)
        AndroidMicrophoneCapture.failOpen = false; owner.refresh(); drain()
        assertEquals(attempts + 1, AndroidMicrophoneCapture.attempts)
        assertEquals(1, AndroidMicrophoneCapture.opened.count { !it.closed })
        assertFalse(owner.state.value.switching)
    }

    @Test fun radioAndStreamKeepSubscriptionsAcrossCaptureReplacement() {
        start()
        val radio = subscribe()
        val first = AndroidMicrophoneCapture.opened.last()
        val stream = subscribe("stream")
        val second = AndroidMicrophoneCapture.opened.last()
        assertNotSame(first, second); assertTrue(first.closed)
        assertTrue(second.stereo); assertEquals(3, second.micSource)
        stream.close(); drain()
        assertSame(second, AndroidMicrophoneCapture.opened.last())
        assertTrue(radio.ptt(true)); assertTrue(radio.ptt(false))
        assertFalse(second.closed)
        radio.close(); drain()
        assertTrue(second.closed)
        assertEquals(AudioManager.MODE_NORMAL, audio.mode)
    }

    @Test fun oldCaptureCallbacksAndFramesCannotAffectReplacement() {
        start(); subscribe()
        val old = AndroidMicrophoneCapture.opened.last()
        select(MediaAudioProfile.PHONE)
        val current = AndroidMicrophoneCapture.opened.last()
        assertNotSame(old, current)
        val count = AndroidMicrophoneCapture.attempts
        old.failed(IllegalStateException("stale hardware failure")); old.confirmed()
        assertFalse(old.accept(MicrophoneCapture.Frame(ShortArray(1920), System.nanoTime(), 0, generation = old.generation)))
        drain()
        assertEquals(count, AndroidMicrophoneCapture.attempts)
        assertFalse(current.closed)
        assertTrue(LogManager.entries.none { it.message.contains("stale hardware failure") })
    }

    @Test fun outputLeaseIsIdempotentAcrossReattachOfSameTrack() {
        start()
        val output = track()
        val old = attach(output)
        background { old.close() }
        val current = attach(output)
        background { old.close() }
        assertEquals(1, output.listeners.size)
        output.deliver(0); drain()
        assertEquals(1, output.listeners.size)
        background { current.close() }
        assertTrue(output.listeners.isEmpty())
    }

    @Test fun lateRoutingCallbackDoesNotReadReleasedTrack() {
        start()
        val output = track()
        val lease = attach(output)
        background { lease.close() }
        output.released = true
        output.deliver(0); drain()
        assertEquals(0, output.accessAfterRelease)
    }

    @Test fun duplicateTrackRegistrationDoesNotRemoveOriginalListener() {
        start()
        val output = track()
        attach(output)
        assertThrows(IllegalStateException::class.java) { background { owner.attachOutput(output) } }
        assertEquals(1, output.listeners.size)
    }

    @Test fun partialRegistrationAndListenerCleanupFailureKeepOriginalErrorAndReleaseMode() {
        start()
        val output = track().apply { failures = setOf("add listener after", "remove listener") }
        val error = assertThrows(IllegalStateException::class.java) { background { owner.attachOutput(output) } }
        assertTrue(error.message!!.contains("add listener after"))
        diagnostic("output listener cleanup failed"); diagnostic("output registration failed")
        assertEquals(1, output.listeners.size)
        assertThrows(IllegalStateException::class.java) { background { owner.awaitIdle() } }
        output.failures = emptySet()
        clock.fire(); drain(); assertTrue(output.listeners.isEmpty())
        background { owner.awaitIdle() }
    }

    @Test fun detachFailureDoesNotSkipHardwareReleaseOrAffectOtherOutput() {
        start(); val one = track(); val two = track()
        val first = attach(one); attach(two)
        one.failures = setOf("remove listener")
        assertThrows(IllegalStateException::class.java) { background { first.close() } }
        assertEquals(1, two.listeners.size)
        assertEquals(1, one.listeners.size)
        one.failures = emptySet(); background { first.close() }; assertTrue(one.listeners.isEmpty())
        diagnostic("output listener removal failed")
    }

    @Test fun failedRegistrationListenerCleanupRemainsSafeAfterTheCallerReleasesItsTrack() {
        start(); val output = track().apply { failures = setOf("add listener after", "remove listener") }
        assertThrows(IllegalStateException::class.java) { background { owner.attachOutput(output) } }
        output.released = true; output.deliver(0); drain(); assertEquals(0, output.accessAfterRelease)
        output.failures = emptySet(); clock.fire(); drain(); assertTrue(output.listeners.isEmpty())
        background { owner.awaitIdle() }
    }

    @Test fun removalFailureAfterEffectStillNeedsAnAcknowledgedRetry() {
        start(); val output = track(); val lease = attach(output); output.failures = setOf("remove listener after")
        assertThrows(IllegalStateException::class.java) { background { lease.close() } }
        assertTrue(output.listeners.isEmpty())
        assertThrows(IllegalStateException::class.java) { background { owner.close() } }
        output.failures = emptySet(); background { lease.close() }; background { owner.close() }
    }

    @Test fun preferenceWriteFailureDoesNotCommitUnsavedSelection() {
        start()
        select(MediaAudioProfile.PHONE)
        assertEquals(listOf(MediaAudioProfile.PHONE), context.configProvider.mediaSaves)
        context.configProvider.failMediaWrite = true
        select(MediaAudioProfile.WIRED)
        assertEquals(MediaAudioProfile.PHONE, owner.state.value.preferred)
        assertEquals(MediaAudioProfile.PHONE, context.configProvider.mediaProfile)
        context.configProvider.failMediaWrite = false
        select(MediaAudioProfile.WIRED)
        assertEquals(listOf(MediaAudioProfile.PHONE, MediaAudioProfile.WIRED), context.configProvider.mediaSaves)
        diagnostic("media storage write failed")
    }

    @Test fun selectionIsPersistedOnlyAfterPhysicalOutputConfirmation() {
        start()
        val output = track().apply { autoRoute = false }
        attach(output)
        select(MediaAudioProfile.PHONE)
        assertTrue(owner.state.value.switching)
        assertTrue(context.configProvider.mediaSaves.isEmpty())
        output.actual = speaker; output.deliver(0); drain()
        assertFalse(owner.state.value.switching)
        assertEquals(listOf(MediaAudioProfile.PHONE), context.configProvider.mediaSaves)
        assertEquals(0, clock.activeTimers)
    }

    @Test fun confirmationTimeoutIsUniqueCancelledOnSuccessAndOldTimerCannotAffectNewLease() {
        start()
        val output = track().apply { autoRoute = false }
        val first = attach(output)
        assertEquals(1, clock.activeTimers)
        val old = clock.timers.last()
        background { first.close() }; drain()
        attach(output)
        val now = clock.timers.last()
        assertNotSame(old, now)
        clock.replay(old); drain()
        assertTrue(owner.state.value.switching)
        output.actual = speaker; output.deliver(1); drain()
        assertFalse(owner.state.value.switching)
        assertEquals(0, clock.activeTimers)
    }

    @Test fun outputTimeoutRecoversWithoutLosingLeaseAndNextRefreshCanConfirm() {
        context.configProvider.mediaProfile = MediaAudioProfile.BLUETOOTH
        start()
        val output = track().apply { autoRoute = false }
        attach(output)
        assertEquals(6L, clock.timers.last().seconds)
        clock.fire(); drain()
        diagnostic("Audio route confirmation timeout")
        assertEquals(MediaAudioNotice.ROUTE_FAILED, owner.state.value.notice)
        assertTrue(requireNotNull(owner.state.value.notice).isError)
        assertEquals(1, output.listeners.size)
        clock.fire(); drain() // Повторный отказ уже PHONE fallback инвалидирует и закешированный ключ.
        output.autoRoute = true
        owner.refresh(); drain()
        assertFalse(owner.state.value.switching)
        assertEquals(0, clock.activeTimers)
    }

    @Test fun bluetoothOutputUsesLongConfirmationDeadline() {
        val bt = AudioDeviceInfo(5, AudioDeviceInfo.TYPE_BLUETOOTH_A2DP, true, false, "BT output")
        audio.devices += bt
        start()
        attach(track().apply { autoRoute = false })
        assertEquals(30L, clock.timers.last().seconds)
        assertEquals(MediaAudioProfile.BLUETOOTH, owner.state.value.active)
    }

    @Test fun exceptionFromCaptureConfirmationIsLoggedAndRecovered() {
        AndroidMicrophoneCapture.autoConfirm = false
        start(); val output = track(); attach(output); subscribe()
        val capture = AndroidMicrophoneCapture.opened.last()
        output.failures = setOf("routed")
        capture.confirmed(); drain()
        diagnostic("track failed: routed")
        output.failures = emptySet(); owner.refresh(); drain()
        AndroidMicrophoneCapture.opened.last().confirmed(); drain()
        assertFalse(owner.state.value.switching)
    }

    @Test fun exceptionInsideTimerIsLoggedRatherThanLostInFuture() {
        start(); val output = track().apply { autoRoute = false }; attach(output)
        output.failures = setOf("routed")
        clock.fire(); drain()
        diagnostic("track failed: routed")
    }

    @Test fun hardwareCloseFailurePreventsOverlappingReopenAndCanRecover() {
        start(); subscribe()
        val first = AndroidMicrophoneCapture.opened.last()
        AndroidMicrophoneCapture.failClose = true
        select(MediaAudioProfile.PHONE)
        assertEquals(1, AndroidMicrophoneCapture.opened.size)
        assertFalse(first.closed)
        diagnostic("capture release failed")
        AndroidMicrophoneCapture.failClose = false; owner.refresh(); drain()
        assertTrue(first.closed)
        assertEquals(1, AndroidMicrophoneCapture.opened.count { !it.closed })
    }

    @Test fun failedModeAcquisitionAfterSideEffectStillReleasesOwnedMode() {
        start()
        audio.failures = setOf("mode after=3")
        subscribe()
        assertEquals(AudioManager.MODE_NORMAL, audio.mode)
        assertTrue(AndroidMicrophoneCapture.opened.isEmpty())
        diagnostic("mode after=3")
        audio.failures = emptySet(); owner.refresh(); drain()
        assertEquals(1, AndroidMicrophoneCapture.opened.count { !it.closed })
    }

    @Test fun idleHandoffDoesNotRequireDeviceQueryAndDoesNotResetTelecomModeLater() {
        start(); val subscription = subscribe()
        subscription.close(); drain()
        audio.failures = setOf("devices")
        background { owner.awaitIdle() }
        assertEquals(AudioManager.MODE_NORMAL, audio.mode)
        audio.mode = AudioManager.MODE_IN_COMMUNICATION // Telecom теперь владеет режимом.
        audio.failures = emptySet(); owner.refresh(); drain()
        assertEquals(AudioManager.MODE_IN_COMMUNICATION, audio.mode)
    }

    @Test fun idleBarrierRejectsLiveInputOrOutput() {
        start(); val sub = subscribe()
        assertThrows(IllegalStateException::class.java) { background { owner.awaitIdle() } }
        sub.close(); drain()
        val lease = attach(track())
        assertThrows(IllegalStateException::class.java) { background { owner.awaitIdle() } }
        background { lease.close() }; background { owner.awaitIdle() }
    }

    @Test fun closeRejectsLiveLeaseAndClosedSessionIgnoresLateEvents() {
        start(); val output = track(); val lease = attach(output)
        assertThrows(IllegalStateException::class.java) { background { owner.close() } }
        background { lease.close() }; background { owner.close() }
        output.released = true; output.deliver(0)
        owner.refresh(); owner.select(MediaAudioProfile.WIRED)
        assertThrows(IllegalStateException::class.java) { owner.subscribe("radio") }
        assertEquals(0, output.accessAfterRelease)
    }

    @Test fun blockingCallsRejectMainAndControlWorkerBeforeSubmitting() {
        start()
        assertThrows(IllegalStateException::class.java) { owner.attachOutput(track()) }
        assertThrows(IllegalStateException::class.java) { owner.awaitIdle() }
        assertThrows(IllegalStateException::class.java) { owner.close() }
        clock.submit { assertThrows(IllegalStateException::class.java) { owner.awaitIdle() } }.get(5, TimeUnit.SECONDS)
        assertTrue(tracks.all { it.listeners.isEmpty() })
    }

    @Test fun failedRouterCloseKeepsExecutorAvailableAndRejectsNewConsumersUntilRetry() {
        start()
        val callback = audio.callbacks.single()
        audio.failures = setOf("unregister")
        assertThrows(IllegalStateException::class.java) { background { owner.close() } }
        assertFalse(clock.isShutdown); assertEquals(setOf(callback), audio.callbacks)
        assertThrows(IllegalStateException::class.java) { owner.subscribe("radio") }
        assertThrows(IllegalStateException::class.java) { background { owner.attachOutput(track()) } }
        assertThrows(IllegalStateException::class.java) { background { owner.awaitIdle() } }
        val registrations = audio.callbackHistory.size
        owner.refresh(); owner.select(MediaAudioProfile.WIRED); callback.onAudioDevicesAdded(emptyArray()); drain()
        assertEquals(registrations, audio.callbackHistory.size)
        audio.failures = emptySet(); background { owner.close() }
        assertTrue(clock.isShutdown); assertTrue(audio.callbacks.isEmpty())
        diagnostic("session router cleanup failed")
    }

    @Test fun bluetoothCloseFailureKeepsSessionCloseRetryable() {
        val adapter = BluetoothAdapter().also { BluetoothAdapter.current = it }
        try {
            start(); adapter.connect(0); drain()
            adapter.closeFailures = adapter.live.toSet()
            assertThrows(IllegalStateException::class.java) { background { owner.close() } }
            assertFalse(clock.isShutdown); assertEquals(1, adapter.live.size); assertTrue(audio.callbacks.isEmpty())
            adapter.closeFailures = emptySet(); background { owner.close() }
            assertTrue(clock.isShutdown); assertTrue(adapter.live.isEmpty())
        } finally { adapter.closeFailures = emptySet() }
    }

    @Test fun failedHardwareCloseDoesNotSkipRouterCleanupOrDisableRetry() {
        start(); val sub = subscribe()
        AndroidMicrophoneCapture.failClose = true
        sub.close(); drain()
        assertThrows(IllegalStateException::class.java) { background { owner.close() } }
        assertFalse(clock.isShutdown); assertTrue(audio.callbacks.isEmpty())
        assertTrue(AndroidMicrophoneCapture.opened.any { !it.closed })
        AndroidMicrophoneCapture.failClose = false; background { owner.close() }
        assertTrue(clock.isShutdown); assertTrue(AndroidMicrophoneCapture.opened.all { it.closed })
        diagnostic("session hardware cleanup failed")
    }

    @Test fun callReservationBlocksSelectionCaptureAndOutput() {
        start(); CallController.blocked = true
        select(MediaAudioProfile.WIRED)
        assertTrue(context.configProvider.mediaSaves.isEmpty())
        assertThrows(IllegalStateException::class.java) { owner.subscribe("radio") }
        assertThrows(IllegalStateException::class.java) { background { owner.attachOutput(track()) } }
    }

    @Test fun ownRoutingEventsCannotCreatePersistentFailureLoop() {
        start(); val output = track().apply { emitOnPreference = true }; attach(output)
        AndroidMicrophoneCapture.failOpen = true
        subscribe()
        repeat(4) { Looper.getMainLooper().dispatchAll(); drain() }
        val attempts = AndroidMicrophoneCapture.attempts
        repeat(4) { Looper.getMainLooper().dispatchAll(); drain() }
        assertEquals(attempts, AndroidMicrophoneCapture.attempts)
        assertTrue(attempts <= 3)
    }

    @Test fun usbMicOnlyKeepsSpeakerPlaybackAndSelectsMicSource() {
        audio.devices += wiredMic
        start(); subscribe()
        val capture = AndroidMicrophoneCapture.opened.last()
        assertEquals(wiredMic, capture.input)
        assertEquals(1, capture.micSource)
        assertEquals("speaker", owner.state.value.outputName)
    }

    @Test fun unplugReopensOneReaderWhileKeepingSubscriptionAlive() {
        audio.devices += arrayOf(wired, wiredMic)
        start(); val sub = subscribe()
        val first = AndroidMicrophoneCapture.opened.last()
        audio.devices = arrayOf(speaker, mic); event()
        assertTrue(first.closed)
        assertEquals(mic, AndroidMicrophoneCapture.opened.last().input)
        assertTrue(sub.ptt(true))
        assertEquals(1, AndroidMicrophoneCapture.opened.count { !it.closed })
    }

    private fun <T> background(action: () -> T): T {
        val future = FutureTask(Callable { action() })
        Thread(future, "media-test-consumer").apply { isDaemon = true; start() }
        try { return future.get(5, TimeUnit.SECONDS) } catch (e: ExecutionException) {
            var cause: Throwable = e.cause ?: e
            while (cause is ExecutionException && cause.cause != null) cause = requireNotNull(cause.cause)
            throw cause
        }
    }

    /** Реальный single worker; только положительные задержки исполняются вручную, без sleep и ожидания 30 секунд. */
    private class ClockExecutor : ScheduledThreadPoolExecutor(1, { task -> Thread(task, "media-test-control").apply { isDaemon = true } }) {
        class Timer(val action: Runnable, val seconds: Long) : FutureTask<Unit>(action, Unit), ScheduledFuture<Unit> {
            override fun getDelay(unit: TimeUnit) = unit.convert(seconds, TimeUnit.SECONDS)
            override fun compareTo(other: Delayed) = getDelay(TimeUnit.NANOSECONDS).compareTo(other.getDelay(TimeUnit.NANOSECONDS))
        }
        val timers = java.util.Collections.synchronizedList(mutableListOf<Timer>())
        val activeTimers: Int get() = synchronized(timers) { timers.count { !it.isCancelled && !it.isDone } }
        override fun schedule(command: Runnable, delay: Long, unit: TimeUnit): ScheduledFuture<*> {
            if (delay == 0L) return super.schedule(command, delay, unit)
            return Timer(command, unit.toSeconds(delay)).also { timers.add(it) }
        }
        fun fire() { synchronized(timers) { timers.filter { !it.isCancelled && !it.isDone } }.forEach { execute(it) } }
        fun replay(timer: Timer) { execute(timer.action) }
    }
}
