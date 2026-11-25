package com.utun.chat.data

import android.content.Context
import android.media.MediaPlayer
import android.os.Looper
import com.utun.chat.ChatApplication
import com.utun.chat.R
import java.util.concurrent.atomic.AtomicReference
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие ringback/notification: отказы release до/после эффекта, повторная очистка и устаревшие callbacks. */
class AlertReleaseCleanupTest {
    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        ChatApplication.instance = Context()
        MediaPlayer.reset()
        NotificationSound.enabled = true; NotificationSound.mutedChannels = emptySet()
        LogManager.entries.clear(); LogManager.console = true
    }

    @After fun release() {
        MediaPlayer.created.forEach { it.failures = emptySet() }
        CallRingback.stop()
        MediaPlayer.created.forEach { it.complete() }
        NotificationSound.enabled = false
        NotificationSound.play("cleanup")
        NotificationSound.enabled = true; NotificationSound.mutedChannels = emptySet()
        // Отрицательный контроль старого кода не переносит потерянные host-ресурсы между сценариями.
        MediaPlayer.created.filter { !it.released }.forEach { it.release() }
    }

    private fun diagnostic(fragment: String) = assertTrue("missing diagnostic: $fragment",
        LogManager.entries.any { fragment in it.message && it.level in listOf("WARN", "ERROR") })

    private fun rejectOnWorker(action: () -> Unit) {
        val failure = AtomicReference<Throwable?>()
        Thread({ try { action() } catch (e: Throwable) { failure.set(e) } }, "alert-release-worker").apply { start(); join() }
        assertTrue("must reject before cleanup: ${failure.get()}", failure.get() is IllegalStateException)
    }

    @Test fun ringbackFailedStopRetainsOwnerUntilReleaseSucceedsAndNeverLogsFalseSuccess() {
        for (failure in listOf("release", "release after")) {
            CallRingback.startConnecting()
            val old = MediaPlayer.created.last().also { it.failures = setOf(failure) }
            LogManager.entries.clear()
            CallRingback.stop()
            assertEquals(failure == "release after", old.released)
            assertFalse(LogManager.entries.any { "released resource=" in it.message })
            old.failures = emptySet(); repeat(3) { CallRingback.stop() }
            assertEquals(2, old.releaseCalls); assertTrue(old.released)
            assertEquals(1, LogManager.entries.count { "released resource=" in it.message })
        }
    }

    @Test fun ringbackPhaseChangeCannotOverwriteFailedReleaseAndLateCallbacksDoNotReenterCleanup() {
        CallRingback.startConnecting()
        val old = MediaPlayer.created.single().also { it.failures = setOf("release") }
        CallRingback.startRinging()
        assertEquals(1, MediaPlayer.created.size); assertTrue(old.playing)
        assertTrue(old.playbackError()); old.complete()
        assertEquals(1, old.releaseCalls); diagnostic("start skipped")
        old.failures = emptySet(); CallRingback.startRinging()
        assertEquals(2, MediaPlayer.created.size); assertEquals(2, old.releaseCalls)
        val current = MediaPlayer.created.last()
        assertEquals(R.raw.call_ringing, current.resourceId)
        assertTrue(old.playbackError()); old.complete()
        assertEquals(0, current.releaseCalls)
    }

    @Test fun ringbackSamePhaseAfterFailedStopRetriesCleanupBeforeRestarting() {
        CallRingback.startConnecting()
        val old = MediaPlayer.created.single().also { it.failures = setOf("release") }
        CallRingback.stop(); CallRingback.startConnecting()
        assertEquals(1, MediaPlayer.created.size); assertEquals(2, old.releaseCalls)
        old.failures = emptySet(); CallRingback.startConnecting()
        assertEquals(2, MediaPlayer.created.size); assertEquals(3, old.releaseCalls)
        assertEquals(1, MediaPlayer.created.last().startCalls)
    }

    @Test fun ringbackSetupAndPartialStartFailuresKeepOwnerWhenRollbackAlsoFails() {
        for (failure in listOf("completion listener", "error listener", "loop", "start", "start after")) {
            MediaPlayer.nextFailures = setOf(failure, "release")
            CallRingback.startConnecting()
            val old = MediaPlayer.created.last()
            assertEquals(1, old.releaseCalls); assertFalse(old.released)
            old.playbackError(); old.complete() // Ошибка регистрации listener могла произойти до его установки.
            assertEquals(1, old.releaseCalls)
            old.failures = emptySet(); CallRingback.stop()
            assertTrue(old.released); assertEquals(2, old.releaseCalls)
        }
    }

    @Test fun ringbackErrorCleanupCanBeRetriedWithoutRestartingOrHandlingOldEventsAgain() {
        CallRingback.startRinging()
        val old = MediaPlayer.created.single().also { it.failures = setOf("release") }
        assertTrue(old.playbackError(12, 34))
        assertTrue(old.playbackError()); old.complete()
        assertEquals(1, old.releaseCalls); diagnostic("phase=ringing what=12 extra=34")
        old.failures = emptySet(); CallRingback.stop()
        assertTrue(old.released); assertEquals(2, old.releaseCalls)
        assertEquals(1, MediaPlayer.created.size)
    }

    @Test fun ringbackUnexpectedCompletionCleanupDoesNotPinTheSamePhaseAfterRecovery() {
        CallRingback.startConnecting()
        val old = MediaPlayer.created.single().also { it.failures = setOf("release") }
        old.complete(); old.complete()
        assertEquals(1, old.releaseCalls); diagnostic("loop unexpectedly completed")
        old.failures = emptySet(); CallRingback.startConnecting()
        assertTrue(old.released); assertEquals(2, MediaPlayer.created.size)
        assertEquals(0, MediaPlayer.created.last().releaseCalls)
    }

    @Test fun workerCannotRetryPendingRingbackCleanupOrCreateReplacement() {
        CallRingback.startConnecting()
        val old = MediaPlayer.created.single().also { it.failures = setOf("release") }
        CallRingback.stop(); old.failures = emptySet()
        rejectOnWorker { CallRingback.stop() }; rejectOnWorker { CallRingback.startRinging() }
        assertEquals(1, old.releaseCalls); assertEquals(1, MediaPlayer.created.size)
        CallRingback.stop(); assertTrue(old.released)
    }

    @Test fun notificationCallbackRetriesFailedReleaseAndDoesNotReportSuccessEarly() {
        for (failure in listOf("release", "release after")) {
            NotificationSound.play("old")
            val old = MediaPlayer.created.last().also { it.failures = setOf(failure) }
            LogManager.entries.clear(); old.complete()
            assertFalse(LogManager.entries.any { "notification released ch=old" in it.message })
            old.failures = emptySet(); assertTrue(old.playbackError(3, 4)); old.complete()
            assertTrue(old.released); assertEquals(2, old.releaseCalls)
            assertEquals(1, LogManager.entries.count { "notification released ch=old" in it.message })
        }
    }

    @Test fun mutedOrDisabledPlayRetriesPendingNotificationCleanupWithoutCreatingSound() {
        for (disabled in listOf(false, true)) {
            NotificationSound.play("old")
            val old = MediaPlayer.created.last().also { it.failures = setOf("release") }
            old.complete(); old.failures = emptySet()
            val count = MediaPlayer.created.size
            NotificationSound.enabled = !disabled; NotificationSound.setMuted("next", !disabled)
            NotificationSound.play("next")
            assertTrue(old.released); assertEquals(2, old.releaseCalls)
            assertEquals(count, MediaPlayer.created.size)
            old.complete(); assertTrue(old.playbackError())
            assertEquals(2, old.releaseCalls)
            NotificationSound.enabled = true; NotificationSound.mutedChannels = emptySet()
        }
    }

    @Test fun notificationSetupAndPartialStartFailuresRemainOwnedEvenWithoutInstalledCallbacks() {
        for (failure in listOf("completion listener", "error listener", "start", "start after")) {
            MediaPlayer.nextFailures = setOf(failure, "release")
            NotificationSound.play(failure)
            val old = MediaPlayer.created.last()
            assertEquals(1, old.releaseCalls); assertFalse(old.released)
            old.failures = emptySet(); NotificationSound.enabled = false
            NotificationSound.play("muted retry")
            assertTrue(old.released); assertEquals(2, old.releaseCalls)
            NotificationSound.enabled = true
        }
    }

    @Test fun notificationCleanupFailureDoesNotBlockNewSoundOrReleaseOtherActivePlayers() {
        NotificationSound.play("failed"); NotificationSound.play("healthy")
        val failed = MediaPlayer.created.first().also { it.failures = setOf("release") }
        val healthy = MediaPlayer.created.last()
        failed.complete(); NotificationSound.play("new")
        val next = MediaPlayer.created.last()
        assertEquals(3, MediaPlayer.created.size); assertEquals(2, failed.releaseCalls)
        assertEquals(0, healthy.releaseCalls); assertEquals(0, next.releaseCalls)
        failed.failures = emptySet(); NotificationSound.enabled = false; NotificationSound.play("retry")
        assertTrue(failed.released); assertEquals(0, healthy.releaseCalls); assertEquals(0, next.releaseCalls)
        assertTrue(failed.playbackError()); assertEquals(3, failed.releaseCalls)
        healthy.complete(); next.complete()
        assertEquals(1, healthy.releaseCalls); assertEquals(1, next.releaseCalls)
    }

    @Test fun pendingNotificationsRecoverIndependentlyAndOnlyTheirOriginalChannelsAreLogged() {
        NotificationSound.play("first"); NotificationSound.play("second")
        val first = MediaPlayer.created.first().also { it.failures = setOf("release") }
        val second = MediaPlayer.created.last().also { it.failures = setOf("release") }
        first.complete(); second.complete()
        first.failures = emptySet(); NotificationSound.enabled = false
        LogManager.entries.clear(); NotificationSound.play("third")
        assertTrue(first.released); assertFalse(second.released)
        diagnostic("release failed ch=second")
        assertTrue(LogManager.entries.any { "notification released ch=first" in it.message })
        second.failures = emptySet(); repeat(3) { NotificationSound.play("third") }
        assertEquals(2, first.releaseCalls); assertEquals(3, second.releaseCalls)
        assertEquals(2, MediaPlayer.created.size)
    }

    @Test fun workerIsRejectedBeforeRetryingPendingNotificationCleanup() {
        NotificationSound.play("old")
        val old = MediaPlayer.created.single().also { it.failures = setOf("release") }
        old.complete(); old.failures = emptySet()
        rejectOnWorker { NotificationSound.play("next") }
        assertEquals(1, old.releaseCalls); assertEquals(1, MediaPlayer.created.size)
        NotificationSound.enabled = false; NotificationSound.play("retry")
        assertTrue(old.released)
    }

    @Test fun notificationPendingCleanupPrecedesCreationFailuresOfTheNextSound() {
        for (failure in listOf("null", "create")) {
            MediaPlayer.nextFailures = emptySet(); NotificationSound.play("old")
            val old = MediaPlayer.created.last().also { it.failures = setOf("release") }
            old.complete(); old.failures = emptySet()
            MediaPlayer.nextFailures = setOf(failure)
            val count = MediaPlayer.created.size
            NotificationSound.play("next")
            assertTrue(old.released); assertEquals(count, MediaPlayer.created.size)
            assertEquals(2, old.releaseCalls)
        }
    }

    @Test fun callbacksAfterNotificationRetryCannotRepeatReleaseOrAffectTheNewPlayback() {
        NotificationSound.play("old")
        val old = MediaPlayer.created.single().also { it.failures = setOf("release after") }
        old.complete(); old.failures = emptySet()
        NotificationSound.play("new")
        val current = MediaPlayer.created.last()
        LogManager.entries.clear()
        old.complete(); assertTrue(old.playbackError())
        assertEquals(2, old.releaseCalls); assertEquals(0, current.releaseCalls)
        assertTrue(LogManager.entries.isEmpty())
    }
}
