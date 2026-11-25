package com.utun.chat.data

import android.app.PendingIntent
import android.content.Context
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.Vibrator
import com.utun.chat.ChatApplication
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий RingtonePlayer: владение при частичном отказе API, повторная очистка и устаревшие callbacks. */
class RingtoneCleanupTest {
    private lateinit var context: Context
    private lateinit var audio: AudioManager
    private lateinit var vibration: Vibrator
    private lateinit var player: RingtonePlayer

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        context = Context(); ChatApplication.instance = context
        vibration = requireNotNull(context.vibrator)
        audio = AudioManager(); AudioManager.shared = audio
        Build.VERSION.SDK_INT = 36; Handler.rejectDelayed = false
        RingtoneManager.reset(); MediaPlayer.reset(); PendingIntent.reset()
        AppLanguage.failContext = false; AppLanguage.failText = false
        IncomingCallManager::class.java.getDeclaredField("initialized").apply { isAccessible = true }.setBoolean(null, false)
        player = RingtonePlayer()
        LogManager.entries.clear(); LogManager.console = true
    }

    @After fun release() {
        RingtoneManager.created.forEach { it.failures = emptySet() }
        MediaPlayer.created.forEach { it.failures = emptySet() }
        listOf(audio, AudioManager.shared).distinct().forEach {
            it.failAbandon = false; it.failAbandonAfter = false; it.abandonResult = AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        }
        listOfNotNull(vibration, context.vibrator).distinct().forEach {
            it.failCancel = false; it.failCancelAfter = false; it.failPulseAfter = false
        }
        Handler.rejectDelayed = false
        player.stop(); IncomingCallManager.stopRinging()
        // Отрицательный контроль старого кода не должен переносить потерянные host-ресурсы между тестами.
        RingtoneManager.created.filter { it.playing }.forEach { it.stop() }
        MediaPlayer.created.filter { !it.released }.forEach { it.release() }
        Looper.getMainLooper().advanceBy(10_000)
        Build.VERSION.SDK_INT = 36
    }

    private fun diagnostic(fragment: String) = assertTrue("missing diagnostic: $fragment",
        LogManager.entries.any { fragment in it.message && it.level in listOf("WARN", "ERROR") })

    @Test fun modernStopFailureBeforeOrAfterEffectCanBeRetriedWithoutRepeatingVibrationCleanup() {
        for (failure in listOf("stop", "stop after")) {
            player.play()
            val sound = RingtoneManager.created.last().also { it.failures = setOf(failure) }
            val cancellations = vibration.cancelCalls
            player.stop()
            assertEquals(failure == "stop", sound.playing)
            sound.failures = emptySet()
            repeat(3) { player.stop() }
            assertFalse(sound.playing); assertEquals(2, sound.stopCalls)
            assertEquals(cancellations + 1, vibration.cancelCalls)
        }
        diagnostic("ringtone stop failed")
    }

    @Test fun replacementCannotOverwriteModernSoundWhileItsStopKeepsFailing() {
        player.play()
        val old = RingtoneManager.created.single().also { it.failures = setOf("stop") }
        player.play(222)
        assertEquals(1, RingtoneManager.created.size); assertTrue(old.playing)
        assertEquals(2, vibration.pulseAttempts)
        diagnostic("sound skipped")
        old.failures = emptySet(); player.play(333)
        assertFalse(old.playing); assertEquals(2, RingtoneManager.created.size)
        assertTrue(RingtoneManager.uris.last().toString().endsWith("/333"))
    }

    @Test fun legacyReleaseAndAbandonFailuresRecoverIndependently() {
        Build.VERSION.SDK_INT = 26
        player.play()
        val sound = MediaPlayer.created.single().also { it.failures = setOf("release") }
        audio.failAbandon = true
        player.stop()
        assertTrue(sound.playing); assertEquals(1, audio.focusOwners.size)
        sound.failures = emptySet(); player.stop()
        assertTrue(sound.released); assertTrue(audio.focusOwners.isNotEmpty())
        audio.failAbandon = false; repeat(3) { player.stop() }
        assertEquals(2, sound.releaseCalls); assertEquals(3, audio.abandoned.size)
        assertTrue(audio.focusOwners.isEmpty()); assertEquals(1, vibration.cancelCalls)
    }

    @Test fun legacyReleaseAfterEffectWaitsForSuccessfulCleanupConfirmation() {
        Build.VERSION.SDK_INT = 27
        player.play()
        val sound = MediaPlayer.created.single().also { it.failures = setOf("release after") }
        player.stop()
        assertTrue(sound.released); assertFalse(sound.playing)
        sound.failures = emptySet(); repeat(3) { player.stop() }
        assertEquals(2, sound.releaseCalls); assertEquals(1, audio.abandoned.size)
    }

    @Test fun rejectedAbandonRetainsOriginalManagerAndBlocksNewFocusUntilRecovery() {
        Build.VERSION.SDK_INT = 26
        player.play()
        val oldRequest = audio.focusHistory.single()
        audio.abandonResult = AudioManager.AUDIOFOCUS_REQUEST_FAILED
        val replacement = AudioManager().also { AudioManager.shared = it }
        player.play(222)
        assertEquals(1, MediaPlayer.created.size); assertTrue(replacement.focusHistory.isEmpty())
        assertTrue(oldRequest in audio.focusOwners)
        diagnostic("abandon focus rejected"); diagnostic("sound skipped")
        audio.abandonResult = AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        player.play(333)
        assertTrue(audio.focusOwners.isEmpty()); assertEquals(2, audio.abandoned.size)
        assertEquals(1, replacement.focusOwners.size); assertEquals(2, MediaPlayer.created.size)
        oldRequest.deliver(AudioManager.AUDIOFOCUS_LOSS); Looper.getMainLooper().dispatchAll()
        assertEquals(0, MediaPlayer.created.last().releaseCalls)
    }

    @Test fun abandonAfterEffectExceptionRetainsRequestUntilSuccessfulRetry() {
        Build.VERSION.SDK_INT = 27
        player.play(); audio.failAbandonAfter = true; player.stop()
        assertTrue(audio.focusOwners.isEmpty())
        audio.failAbandonAfter = false; repeat(3) { player.stop() }
        assertEquals(2, audio.abandoned.size)
        assertEquals(1, MediaPlayer.created.single().releaseCalls)
    }

    @Test fun partialPlaybackStartAndFailedRollbackKeepTheAllocatedOwner() {
        for (sdk in listOf(27, 36)) {
            Build.VERSION.SDK_INT = sdk
            if (sdk < 28) MediaPlayer.nextFailures = setOf("start after", "release")
            else RingtoneManager.nextFailures = setOf("play after", "stop")
            player.play()
            assertEquals(if (sdk < 28) 1 else 2, vibration.pulseAttempts)
            if (sdk < 28) assertTrue(MediaPlayer.created.single().playing) else assertTrue(RingtoneManager.created.single().playing)
            MediaPlayer.created.forEach { it.failures = emptySet() }
            RingtoneManager.created.forEach { it.failures = emptySet() }
            player.stop()
            if (sdk < 28) assertTrue(MediaPlayer.created.single().released) else assertFalse(RingtoneManager.created.single().playing)
        }
        diagnostic("sound failed")
    }

    @Test fun partialFocusRequestAndFailedRollbackCanBeCleanedWithoutAPlayer() {
        Build.VERSION.SDK_INT = 26
        audio.failFocusAfter = true; audio.failAbandon = true
        player.play()
        assertTrue(MediaPlayer.created.isEmpty()); assertEquals(1, audio.focusOwners.size)
        assertEquals(1, vibration.pulseAttempts)
        audio.failAbandon = false; player.stop()
        assertTrue(audio.focusOwners.isEmpty()); assertEquals(2, audio.abandoned.size)
    }

    @Test fun failedVibrationCancellationRetainsOriginalServiceWithoutRestartingTimer() {
        for (afterEffect in listOf(false, true)) {
            player.play()
            vibration.failCancel = !afterEffect; vibration.failCancelAfter = afterEffect
            val cancellations = vibration.cancelCalls
            player.stop()
            context.vibrator = Vibrator()
            vibration.failCancel = false; vibration.failCancelAfter = false
            repeat(3) { player.stop() }
            Looper.getMainLooper().advanceBy(10_000)
            assertEquals(cancellations + 2, vibration.cancelCalls)
            assertEquals(0, requireNotNull(context.vibrator).cancelCalls)
            assertFalse(vibration.active)
            context.vibrator = vibration
        }
        assertEquals(2, vibration.pulseAttempts)
    }

    @Test fun pendingVibrationCleanupBlocksOnlyNewVibrationAndStopsScheduling() {
        player.play(); vibration.failCancel = true
        player.play(222)
        assertEquals(2, RingtoneManager.created.size)
        assertEquals(1, vibration.pulseAttempts)
        Looper.getMainLooper().advanceBy(10_000)
        assertEquals(1, vibration.pulseAttempts); diagnostic("vibration skipped")
        vibration.failCancel = false; player.play(333)
        Looper.getMainLooper().advanceBy(2000)
        assertEquals(3, vibration.pulseAttempts)
    }

    @Test fun failedPulseAfterEffectAndFailedCancelDoNotLeaveAPeriodicTimer() {
        vibration.failPulseAfter = true; vibration.failCancel = true
        player.play()
        assertTrue(vibration.active)
        Looper.getMainLooper().advanceBy(10_000)
        assertEquals(1, vibration.pulseAttempts)
        vibration.failCancel = false; player.stop()
        assertFalse(vibration.active); assertEquals(2, vibration.cancelCalls)
    }

    @Test fun rejectedVibrationTimerStillRetainsFailedCancellationForStop() {
        Handler.rejectDelayed = true; vibration.failCancel = true
        player.play()
        diagnostic("vibration timer rejected")
        assertEquals(1, vibration.pulseAttempts)
        vibration.failCancel = false; player.stop()
        assertFalse(vibration.active); assertEquals(2, vibration.cancelCalls)
        Looper.getMainLooper().advanceBy(10_000)
        assertEquals(1, vibration.pulseAttempts)
    }

    @Test fun callbacksFromStoppedResourcesDoNotReenterFailedCleanupOrTouchReplacement() {
        Build.VERSION.SDK_INT = 26
        player.play()
        val old = MediaPlayer.created.single().also { it.failures = setOf("release") }
        val focus = audio.focusHistory.single()
        audio.failAbandon = true; player.stop()
        assertTrue(old.playbackError()); old.complete(); focus.deliver(AudioManager.AUDIOFOCUS_LOSS)
        Looper.getMainLooper().dispatchAll()
        assertEquals(1, old.releaseCalls); assertEquals(1, audio.abandoned.size)
        old.failures = emptySet(); audio.failAbandon = false; player.play(222)
        assertTrue(old.playbackError()); old.complete(); focus.deliver(AudioManager.AUDIOFOCUS_LOSS)
        Looper.getMainLooper().dispatchAll()
        assertEquals(0, MediaPlayer.created.last().releaseCalls)
    }

    @Test fun focusLossCanRetainSoundCleanupWhileVibrationContinuesAndGainDoesNotRestartSound() {
        Build.VERSION.SDK_INT = 27
        player.play()
        val sound = MediaPlayer.created.single().also { it.failures = setOf("release") }
        val focus = audio.focusHistory.single()
        focus.deliver(AudioManager.AUDIOFOCUS_LOSS_TRANSIENT); Looper.getMainLooper().dispatchAll()
        focus.deliver(AudioManager.AUDIOFOCUS_GAIN); Looper.getMainLooper().advanceBy(2000)
        assertEquals(2, vibration.pulseAttempts); assertEquals(1, MediaPlayer.created.size)
        sound.failures = emptySet(); player.stop()
        assertFalse(sound.playing); assertEquals(2, sound.releaseCalls)
    }

    @Test fun managerStopInitAndLanguageRefreshRetrySoundAfterActiveCallWasCleared() {
        for (entry in listOf("stop", "init", "language")) {
            IncomingCallManager.show(11L + RingtoneManager.created.size, "123", "01".repeat(32))
            val sound = RingtoneManager.created.last().also { it.failures = setOf("stop") }
            IncomingCallManager.stopRinging()
            assertTrue(sound.playing)
            assertTrue(requireNotNull(context.powerManager).locks.none { it.held })
            assertTrue(requireNotNull(context.notificationManager).active.isEmpty())
            sound.failures = emptySet()
            when (entry) {
                "stop" -> IncomingCallManager.stopRinging()
                "init" -> IncomingCallManager.init()
                else -> IncomingCallManager.refreshLanguage()
            }
            assertFalse("lost sound cleanup via $entry", sound.playing)
            assertEquals(2, sound.stopCalls)
        }
    }
}
