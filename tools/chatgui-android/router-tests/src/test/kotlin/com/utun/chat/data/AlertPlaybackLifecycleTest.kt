package com.utun.chat.data

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.os.Build
import android.os.Looper
import com.utun.chat.ChatApplication
import com.utun.chat.R
import java.util.concurrent.atomic.AtomicReference
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Три настоящих проигрывателя: prepare-профиль, владение ресурсами, сбои API и запоздалые callbacks. */
class AlertPlaybackLifecycleTest {
    private lateinit var context: Context
    private lateinit var audio: AudioManager
    private lateinit var ringtone: RingtonePlayer
    private val vibration get() = requireNotNull(context.vibrator)

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        context = Context()
        ChatApplication.instance = context
        audio = AudioManager().also { AudioManager.shared = it }
        Build.VERSION.SDK_INT = 36
        MediaPlayer.reset()
        RingtoneManager.reset()
        NotificationSound.enabled = true
        NotificationSound.mutedChannels = emptySet()
        ringtone = RingtonePlayer()
        LogManager.entries.clear()
        LogManager.console = true
    }

    @After fun release() {
        RingtoneManager.created.forEach { it.failures = emptySet() }
        MediaPlayer.created.forEach { it.failures = emptySet() }
        audio.failAbandon = false; audio.abandonResult = AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        context.vibrator?.failCancel = false
        ringtone.stop()
        CallRingback.stop()
        MediaPlayer.created.forEach { it.complete() }
        NotificationSound.enabled = false; NotificationSound.play("cleanup")
        Looper.getMainLooper().advanceBy(10_000)
        Build.VERSION.SDK_INT = 36
    }

    private fun diagnostic(fragment: String) = assertTrue("Missing diagnostic: $fragment",
        LogManager.entries.any { it.message.contains(fragment) && it.level in listOf("WARN", "ERROR") })

    private fun rejectOnWorker(action: () -> Unit) {
        val failure = AtomicReference<Throwable?>()
        Thread({ try { action() } catch (e: Throwable) { failure.set(e) } }, "alert-test-worker").apply { start(); join() }
        assertTrue("Must reject before touching resources: ${failure.get()}", failure.get() is IllegalStateException)
    }

    @Test fun notificationRespectsGlobalAndChannelMute() {
        NotificationSound.enabled = false
        NotificationSound.play("group")
        NotificationSound.enabled = true
        NotificationSound.setMuted("group", true)
        NotificationSound.play("group")
        assertTrue(MediaPlayer.created.isEmpty())
        NotificationSound.setMuted("group", false)
        NotificationSound.play("group")
        assertEquals(1, MediaPlayer.created.single().startCalls)
    }

    @Test fun notificationUsesPreparedNotificationProfileAndReleasesOnce() {
        NotificationSound.play("group")
        val player = MediaPlayer.created.single()
        assertEquals(R.raw.msg_incoming, player.resourceId)
        assertEquals(AudioAttributes.USAGE_NOTIFICATION, player.effectiveAttributes.usage)
        assertEquals(AudioAttributes.CONTENT_TYPE_SONIFICATION, player.effectiveAttributes.contentType)
        assertFalse(player.isLooping)
        repeat(3) { player.complete() }
        assertTrue(player.playbackError())
        assertEquals(1, player.releaseCalls)
    }

    @Test fun notificationErrorIsHandledAndDoesNotReleaseOverlappingSound() {
        NotificationSound.play("first")
        NotificationSound.play("second")
        val first = MediaPlayer.created[0]
        val second = MediaPlayer.created[1]
        assertTrue(first.playbackError(7, 9))
        first.complete()
        assertEquals(1, first.releaseCalls)
        assertEquals(0, second.releaseCalls)
        diagnostic("ch=first what=7 extra=9")
        second.complete()
        assertEquals(1, second.releaseCalls)
    }

    @Test fun notificationSetupAndStartFailureMatrixReleasesAllocatedPlayer() {
        for (failure in listOf("completion listener", "error listener", "start")) {
            MediaPlayer.nextFailures = setOf(failure)
            NotificationSound.play(failure)
            assertEquals(failure, 1, MediaPlayer.created.last().releaseCalls)
            diagnostic("play failed ch=$failure")
        }
    }

    @Test fun notificationNullAndThrowAreDiagnosedAndCanRetry() {
        for (failure in listOf("null", "create")) {
            MediaPlayer.nextFailures = setOf(failure)
            NotificationSound.play(failure)
            diagnostic("notification unavailable ch=$failure")
        }
        assertTrue(MediaPlayer.created.isEmpty())
        MediaPlayer.nextFailures = emptySet()
        NotificationSound.play("retry")
        assertEquals(1, MediaPlayer.created.single().startCalls)
    }

    @Test fun notificationReleaseFailureIsRetriedUntilConfirmed() {
        MediaPlayer.nextFailures = setOf("release")
        NotificationSound.play("group")
        val player = MediaPlayer.created.single()
        repeat(3) { player.complete() }
        assertTrue(player.playbackError())
        assertEquals(4, player.releaseCalls)
        assertFalse(player.released)
        player.failures = emptySet(); player.complete(); assertTrue(player.playbackError())
        assertEquals(5, player.releaseCalls); assertTrue(player.released)
        diagnostic("release failed ch=group")
    }

    @Test fun notificationRejectsWorkerBeforeCreatingPlayer() {
        rejectOnWorker { NotificationSound.play("group") }
        assertTrue(MediaPlayer.created.isEmpty())
        diagnostic("requires main thread")
    }

    @Test fun ringbackSamePhaseKeepsLoopAndPreparedMediaProfile() {
        repeat(3) { CallRingback.startConnecting() }
        val player = MediaPlayer.created.single()
        assertEquals(R.raw.call_connecting, player.resourceId)
        assertEquals(AudioAttributes.USAGE_MEDIA, player.effectiveAttributes.usage)
        assertEquals(AudioAttributes.CONTENT_TYPE_MUSIC, player.effectiveAttributes.contentType)
        assertTrue(player.isLooping)
        assertEquals(1, player.startCalls)
        assertEquals(0, player.releaseCalls)
    }

    @Test fun ringbackPhaseChangeReleasesOldAndIgnoresItsLateCallbacks() {
        CallRingback.startConnecting()
        val old = MediaPlayer.created.single()
        CallRingback.startRinging()
        repeat(3) { CallRingback.startRinging() }
        val current = MediaPlayer.created.last()
        assertEquals(2, MediaPlayer.created.size)
        assertEquals(R.raw.call_ringing, current.resourceId)
        assertTrue(old.playbackError())
        old.complete()
        assertEquals(1, old.releaseCalls)
        assertEquals(0, current.releaseCalls)
        repeat(3) { CallRingback.stop() }
        assertEquals(1, current.releaseCalls)
    }

    @Test fun ringbackPlaybackErrorReleasesAndAllowsSamePhaseRetry() {
        CallRingback.startRinging()
        val old = MediaPlayer.created.single()
        assertTrue(old.playbackError(12, 34))
        old.complete()
        diagnostic("phase=ringing what=12 extra=34")
        assertEquals(1, old.releaseCalls)
        CallRingback.startRinging()
        assertEquals(2, MediaPlayer.created.size)
        assertEquals(1, MediaPlayer.created.last().startCalls)
    }

    @Test fun ringbackUnexpectedCompletionReleasesAndAllowsRetry() {
        CallRingback.startConnecting()
        val old = MediaPlayer.created.single()
        repeat(2) { old.complete() }
        assertEquals(1, old.releaseCalls)
        diagnostic("loop unexpectedly completed phase=connecting")
        CallRingback.startConnecting()
        assertEquals(2, MediaPlayer.created.size)
    }

    @Test fun ringbackSetupAndStartFailureMatrixReleasesAndCanRetry() {
        for (failure in listOf("completion listener", "error listener", "loop", "start")) {
            MediaPlayer.nextFailures = setOf(failure)
            CallRingback.startConnecting()
            assertEquals(failure, 1, MediaPlayer.created.last().releaseCalls)
            diagnostic("MediaPlayer start failed (connecting)")
        }
        MediaPlayer.nextFailures = emptySet()
        CallRingback.startConnecting()
        assertEquals(1, MediaPlayer.created.last().startCalls)
    }

    @Test fun ringbackNullAndThrowAreDiagnosedWithoutOwningPlayer() {
        for (failure in listOf("null", "create")) {
            MediaPlayer.nextFailures = setOf(failure)
            CallRingback.startConnecting()
            diagnostic("player unavailable phase=connecting")
        }
        assertTrue(MediaPlayer.created.isEmpty())
    }

    @Test fun ringbackReleaseFailureRetainsOldOwnerUntilRecovery() {
        MediaPlayer.nextFailures = setOf("release")
        CallRingback.startConnecting()
        val old = MediaPlayer.created.single()
        MediaPlayer.nextFailures = emptySet()
        CallRingback.startRinging()
        old.complete()
        assertEquals(1, old.releaseCalls)
        assertEquals(1, MediaPlayer.created.size)
        old.failures = emptySet(); CallRingback.startRinging()
        assertEquals(2, old.releaseCalls); assertEquals(2, MediaPlayer.created.size)
        assertEquals(0, MediaPlayer.created.last().releaseCalls)
        diagnostic("release failed resource=")
    }

    @Test fun ringbackRejectsWorkerBeforeChangingExistingLoop() {
        CallRingback.startConnecting()
        val current = MediaPlayer.created.single()
        rejectOnWorker { CallRingback.startRinging() }
        rejectOnWorker { CallRingback.stop() }
        assertEquals(1, MediaPlayer.created.size)
        assertEquals(0, current.releaseCalls)
        diagnostic("requires main thread")
    }

    @Test fun ringtoneModeMatrixAcrossLegacyAndModernApis() {
        for (sdk in listOf(26, 27, 28, 36)) for (mode in 0..2) {
            Build.VERSION.SDK_INT = sdk
            audio.ringerMode = mode
            val modernBefore = RingtoneManager.created.size
            val legacyBefore = MediaPlayer.created.size
            val pulsesBefore = vibration.pulseAttempts
            ringtone.play()
            val sound = if (mode == AudioManager.RINGER_MODE_NORMAL) 1 else 0
            assertEquals("sdk=$sdk mode=$mode modern", modernBefore + if (sdk >= 28) sound else 0, RingtoneManager.created.size)
            assertEquals("sdk=$sdk mode=$mode legacy", legacyBefore + if (sdk < 28) sound else 0, MediaPlayer.created.size)
            assertEquals(pulsesBefore + if (mode == AudioManager.RINGER_MODE_SILENT) 0 else 1, vibration.pulseAttempts)
            ringtone.stop()
        }
    }

    @Test fun modernRingtoneHasRingProfileLoopAndOwnedResourceUri() {
        ringtone.play(1234)
        val sound = RingtoneManager.created.single()
        assertEquals("android.resource://com.utun.chat/1234", RingtoneManager.uris.single().toString())
        assertEquals(AudioAttributes.USAGE_NOTIFICATION_RINGTONE, sound.audioAttributes?.usage)
        assertTrue(sound.isLooping)
        assertEquals(1, sound.playCalls)
        repeat(3) { ringtone.stop() }
        assertEquals(1, sound.stopCalls)
        assertEquals(1, vibration.cancelCalls)
    }

    @Test fun modernSoundFailureMatrixStillVibratesAndStopsPartialRingtone() {
        for (failure in listOf("null", "get", "attributes", "loop", "play")) {
            RingtoneManager.nextFailures = setOf(failure)
            val soundsBefore = RingtoneManager.created.size
            val pulsesBefore = vibration.pulseAttempts
            ringtone.play()
            assertEquals(failure, pulsesBefore + 1, vibration.pulseAttempts)
            if (RingtoneManager.created.size > soundsBefore) assertEquals(failure, 1, RingtoneManager.created.last().stopCalls)
            ringtone.stop()
        }
        diagnostic("getRingtone returned null")
        diagnostic("sound failed")
    }

    @Test fun modernStopFailureIsLoggedAndDoesNotBlockVibrationCleanup() {
        RingtoneManager.nextFailures = setOf("stop")
        ringtone.play()
        repeat(3) { ringtone.stop() }
        val sound = RingtoneManager.created.single()
        assertEquals(3, sound.stopCalls)
        assertTrue(sound.playing)
        sound.failures = emptySet(); repeat(3) { ringtone.stop() }
        assertEquals(4, sound.stopCalls); assertFalse(sound.playing)
        assertEquals(1, vibration.cancelCalls)
        diagnostic("ringtone stop failed")
    }

    @Test fun vibrationUsesFiniteRingPulsesAndStopsTimer() {
        audio.ringerMode = AudioManager.RINGER_MODE_VIBRATE
        ringtone.play()
        Looper.getMainLooper().advanceBy(1999)
        assertEquals(1, vibration.pulseAttempts)
        Looper.getMainLooper().advanceBy(1)
        assertEquals(2, vibration.pulseAttempts)
        vibration.effects.forEach { (effect, attrs) ->
            assertArrayEquals(longArrayOf(0, 1000), effect.timings)
            assertEquals(-1, effect.repeat)
            assertEquals(AudioAttributes.USAGE_NOTIFICATION_RINGTONE, attrs?.usage)
        }
        ringtone.stop()
        Looper.getMainLooper().advanceBy(20_000)
        assertEquals(2, vibration.pulseAttempts)
    }

    @Test fun repeatedRingtonePlayReplacesResourcesAndKeepsSingleTimer() {
        repeat(3) { ringtone.play() }
        assertEquals(listOf(1, 1, 0), RingtoneManager.created.map { it.stopCalls })
        assertEquals(3, vibration.pulseAttempts)
        Looper.getMainLooper().advanceBy(2000)
        assertEquals(4, vibration.pulseAttempts)
    }

    @Test fun firstAndDeferredVibrationFailuresAreCaughtAndStopScheduling() {
        audio.ringerMode = AudioManager.RINGER_MODE_VIBRATE
        for (deferred in listOf(false, true)) {
            vibration.failPulse = !deferred
            ringtone.play()
            val attempts = vibration.pulseAttempts
            vibration.failPulse = true
            Looper.getMainLooper().advanceBy(2000)
            assertEquals(attempts + if (deferred) 1 else 0, vibration.pulseAttempts)
            val failedAttempts = vibration.pulseAttempts
            vibration.failPulse = false
            Looper.getMainLooper().advanceBy(10_000)
            assertEquals(failedAttempts, vibration.pulseAttempts)
            diagnostic("vibration pulse failed")
        }
    }

    @Test fun vibrationServiceAndCapabilityFailuresDoNotStopSound() {
        val original = vibration
        context.vibrator = null
        ringtone.play()
        diagnostic("vibrator unavailable")
        assertEquals(1, RingtoneManager.created.last().playCalls)
        context.vibrator = original
        original.failQuery = true
        ringtone.play()
        assertEquals(1, RingtoneManager.created.last().playCalls)
        assertEquals(0, original.pulseAttempts)
        original.failQuery = false
        original.available = false
        ringtone.play()
        assertEquals(0, original.pulseAttempts)
    }

    @Test fun vibrationCancelFailureIsLoggedAndTimerStillRemoved() {
        ringtone.play()
        vibration.failCancel = true
        repeat(3) { ringtone.stop() }
        Looper.getMainLooper().advanceBy(10_000)
        assertEquals(1, vibration.pulseAttempts)
        assertEquals(3, vibration.cancelCalls)
        vibration.failCancel = false; repeat(3) { ringtone.stop() }
        assertEquals(4, vibration.cancelCalls)
        assertFalse(vibration.active)
        diagnostic("vibration cancel failed")
    }

    @Test fun unknownOrUnavailableRingerModeSuppressesEffectsAndIsDiagnosed() {
        audio.ringerMode = 99
        ringtone.play()
        diagnostic("unknown ringer mode=99")
        audio.failRingerMode = true
        ringtone.play()
        diagnostic("ringer mode unavailable")
        assertTrue(RingtoneManager.created.isEmpty())
        assertEquals(0, vibration.pulseAttempts)
    }

    @Test fun legacyRingProfileAndFocusAreReleasedThroughOriginalManager() {
        Build.VERSION.SDK_INT = 26
        ringtone.play()
        val sound = MediaPlayer.created.single()
        assertEquals(AudioAttributes.USAGE_NOTIFICATION_RINGTONE, sound.effectiveAttributes.usage)
        assertTrue(sound.isLooping)
        assertEquals(AudioAttributes.USAGE_NOTIFICATION_RINGTONE, audio.focusHistory.single().attributes?.usage)
        val replacement = AudioManager().also { AudioManager.shared = it }
        repeat(3) { ringtone.stop() }
        assertEquals(1, sound.releaseCalls)
        assertEquals(audio.focusHistory, audio.abandoned)
        assertTrue(replacement.abandoned.isEmpty())
    }

    @Test fun legacyDeniedFocusSuppressesSoundButKeepsVibration() {
        Build.VERSION.SDK_INT = 27
        audio.focusResult = AudioManager.AUDIOFOCUS_REQUEST_FAILED
        ringtone.play()
        assertTrue(MediaPlayer.created.isEmpty())
        assertEquals(1, vibration.pulseAttempts)
        assertEquals(1, audio.abandoned.size)
        diagnostic("audio focus denied")
    }

    @Test fun legacyFocusExceptionClearsRequestAndKeepsVibration() {
        Build.VERSION.SDK_INT = 26
        audio.failFocus = true
        ringtone.play()
        assertTrue(MediaPlayer.created.isEmpty())
        assertEquals(1, vibration.pulseAttempts)
        assertEquals(1, audio.abandoned.size)
        diagnostic("focus unavailable")
        ringtone.stop()
        assertEquals(1, audio.abandoned.size)
    }

    @Test fun legacyNullAndCreateFailureAbandonFocusAndKeepVibration() {
        Build.VERSION.SDK_INT = 26
        for (failure in listOf("null", "create")) {
            MediaPlayer.nextFailures = setOf(failure)
            ringtone.play()
        }
        assertTrue(MediaPlayer.created.isEmpty())
        assertEquals(2, audio.abandoned.size)
        assertEquals(2, vibration.pulseAttempts)
        diagnostic("MediaPlayer.create returned null")
        diagnostic("create failed")
    }

    @Test fun legacySetupFailureMatrixReleasesSoundAndFocusButKeepsVibration() {
        Build.VERSION.SDK_INT = 27
        for (failure in listOf("completion listener", "error listener", "loop", "start")) {
            MediaPlayer.nextFailures = setOf(failure)
            ringtone.play()
            assertEquals(failure, 1, MediaPlayer.created.last().releaseCalls)
        }
        assertEquals(4, audio.abandoned.size)
        assertEquals(4, vibration.pulseAttempts)
    }

    @Test fun legacyErrorAndUnexpectedCompletionClearSoundWithoutCancellingVibration() {
        Build.VERSION.SDK_INT = 26
        for (error in listOf(false, true)) {
            ringtone.play()
            val player = MediaPlayer.created.last()
            if (error) assertTrue(player.playbackError()) else player.complete()
            assertEquals(1, player.releaseCalls)
            val pulses = vibration.pulseAttempts
            Looper.getMainLooper().advanceBy(2000)
            assertEquals(pulses + 1, vibration.pulseAttempts)
        }
        assertEquals(2, audio.abandoned.size)
        diagnostic("loop unexpectedly completed")
        diagnostic("playback error")
    }

    @Test fun legacyFocusLossStopsOnlySoundAndLateFocusOrPlayerCallbacksCannotStopNewSound() {
        Build.VERSION.SDK_INT = 27
        for (loss in listOf(AudioManager.AUDIOFOCUS_LOSS, AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK)) {
            ringtone.play()
            val oldPlayer = MediaPlayer.created.last()
            val oldFocus = audio.focusHistory.last()
            oldFocus.deliver(loss)
            Looper.getMainLooper().dispatchAll()
            assertEquals(1, oldPlayer.releaseCalls)
            val pulses = vibration.pulseAttempts
            Looper.getMainLooper().advanceBy(2000)
            assertEquals(pulses + 1, vibration.pulseAttempts)
            ringtone.play()
            val current = MediaPlayer.created.last()
            oldFocus.deliver(loss)
            oldFocus.deliver(AudioManager.AUDIOFOCUS_GAIN)
            assertTrue(oldPlayer.playbackError())
            oldPlayer.complete()
            Looper.getMainLooper().dispatchAll()
            assertEquals(0, current.releaseCalls)
            assertEquals(1, oldPlayer.releaseCalls)
        }
    }

    @Test fun legacyReleaseAndAbandonFailuresAreRetriedUntilConfirmed() {
        Build.VERSION.SDK_INT = 26
        MediaPlayer.nextFailures = setOf("release")
        audio.failAbandon = true
        ringtone.play()
        repeat(3) { ringtone.stop() }
        val sound = MediaPlayer.created.single()
        assertEquals(3, sound.releaseCalls)
        assertEquals(3, audio.abandoned.size)
        sound.failures = emptySet(); audio.failAbandon = false
        repeat(3) { ringtone.stop() }
        assertEquals(4, sound.releaseCalls); assertTrue(sound.released)
        assertEquals(4, audio.abandoned.size); assertTrue(audio.focusOwners.isEmpty())
        assertEquals(1, vibration.cancelCalls)
        diagnostic("player release failed")
        diagnostic("abandon focus failed")
    }

    @Test fun legacyRejectedAbandonIsLogged() {
        Build.VERSION.SDK_INT = 27
        audio.abandonResult = AudioManager.AUDIOFOCUS_REQUEST_FAILED
        ringtone.play()
        ringtone.stop()
        diagnostic("abandon focus rejected")
    }

    @Test fun ringtoneRejectsWorkerBeforeStoppingExistingAlert() {
        ringtone.play()
        rejectOnWorker { ringtone.play() }
        rejectOnWorker { ringtone.stop() }
        assertEquals(0, RingtoneManager.created.single().stopCalls)
        assertEquals(0, vibration.cancelCalls)
        assertEquals(1, vibration.pulseAttempts)
        diagnostic("requires main thread")
    }
}
