package com.utun.chat.data

import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.os.Build
import android.os.Looper
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import com.utun.chat.ChatApplication
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Реальная очистка менеджера: исключение не отдаёт владение ресурса ОС/GC и не блокирует остальные ресурсы. */
class IncomingCallCleanupTest {
    private lateinit var context: Context
    private lateinit var power: PowerManager
    private lateinit var notifications: NotificationManager
    private fun show(id: Long = 11) = IncomingCallManager.show(id, "123", "01".repeat(32))
    private fun diagnostic(fragment: String) = assertTrue("missing diagnostic $fragment: ${LogManager.entries.takeLast(12)}",
        LogManager.entries.any { it.level in listOf("WARN", "ERROR") && fragment in it.message })
    private fun recover() {
        power.failures = emptySet(); power.locks.forEach { it.failures = emptySet() }
        notifications.failures = emptySet()
        PendingIntent.created.forEach { it.failCancel = false; it.failCancelAfter = false }
    }

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        context = Context(); ChatApplication.instance = context
        power = context.powerManager!!; notifications = context.notificationManager!!
        AudioManager.shared = AudioManager(); Build.VERSION.SDK_INT = 36
        AppLanguage.failContext = false; AppLanguage.failText = false; AppLanguage.language = "en"
        NotificationCompat.failBuild = false; CallController.endings.clear(); CallController.media = false
        PendingIntent.reset(); RingtoneManager.reset(); MediaPlayer.reset()
        IncomingCallManager::class.java.getDeclaredField("initialized").apply { isAccessible = true }.setBoolean(null, false)
        LogManager.entries.clear(); LogManager.console = true
    }

    @After fun release() {
        recover()
        IncomingCallManager.stopRinging()
        // Отрицательный контроль старого кода теряет ссылки; host не должен переносить его утечки в следующий сценарий.
        power.locks.filter { it.held }.forEach { it.release() }
        PendingIntent.created.filter { !it.cancelled }.forEach { it.cancel() }
        Looper.getMainLooper().advanceBy(10_000)
    }

    @Test fun wakeLockQueryAndReleaseFailuresCanBeRetriedAfterAlertWasCleared() {
        for (failure in listOf("query", "release")) {
            show(if (failure == "query") 11 else 22)
            val lock = power.locks.last()
            lock.failures = setOf(failure)
            IncomingCallManager.stopRinging()
            assertFalse(IncomingCallManager.isRinging(if (failure == "query") 11 else 22, false))
            assertTrue(lock.held); diagnostic("wake lock release failed")
            lock.failures = emptySet()
            IncomingCallManager.stopRinging()
            assertFalse("lost lock on $failure", lock.held)
        }
    }

    @Test fun releaseAfterEffectFailureDoesNotReleaseAnUnlockedWakeLockTwice() {
        show()
        val lock = power.locks.single()
        lock.failures = setOf("release_after")
        IncomingCallManager.stopRinging()
        assertFalse(lock.held); assertEquals(1, lock.releaseCalls)
        recover(); IncomingCallManager.stopRinging(); IncomingCallManager.stopRinging()
        assertEquals(1, lock.releaseCalls)
        show(22)
        assertEquals(2, power.locks.size); assertTrue(power.locks.last().held)
    }

    @Test fun replacingAlertCannotOverwriteAWakeLockWhoseCleanupFailed() {
        show()
        val old = power.locks.single()
        old.failures = setOf("release")
        show(22)
        assertEquals(1, power.locks.size)
        assertTrue(IncomingCallManager.isRinging(22, false))
        assertEquals(2, RingtoneManager.created.size)
        diagnostic("acquisition skipped")
        recover(); IncomingCallManager.stopRinging()
        assertFalse(old.held)
        show(33)
        assertEquals(2, power.locks.size); assertTrue(power.locks.last().held)
    }

    @Test fun partialAcquireAndFailedRollbackKeepAnOwnerUntilStopCanRelease() {
        power.failures = setOf("acquire", "release")
        show()
        val lock = power.locks.single()
        assertTrue(lock.held); assertEquals(1, RingtoneManager.created.single().playCalls)
        diagnostic("wake lock failed")
        recover(); IncomingCallManager.stopRinging()
        assertFalse(lock.held)
    }

    @Test fun wakeLocksAreNonCountedAndFailedSetupDoesNotSuppressOtherEffects() {
        for (failure in listOf("configure", "acquire_before")) {
            power.failures = setOf(failure)
            show(if (failure == "configure") 11 else 22)
            assertEquals(1, RingtoneManager.created.last().playCalls)
            assertFalse(power.locks.last().held)
            recover(); IncomingCallManager.stopRinging()
        }
        show(33)
        assertFalse(power.locks.last().counted)
        assertEquals(30_000L, power.locks.last().timeout)
    }

    @Test fun failedActionCancellationIsRetriedWithoutRepeatingSuccessfulCleanup() {
        show()
        val tokens = PendingIntent.created.toList()
        val failed = tokens.first().also { it.failCancel = true }
        IncomingCallManager.stopRinging()
        assertFalse(failed.cancelled); assertTrue(tokens.drop(1).all { it.cancelled })
        failed.failCancel = false
        IncomingCallManager.stopRinging(); IncomingCallManager.stopRinging()
        assertTrue(failed.cancelled); assertEquals(2, failed.cancelCalls)
        assertTrue(tokens.drop(1).all { it.cancelCalls == 1 })
        assertEquals(1, power.locks.single().releaseCalls)
        assertEquals(1, notifications.cancellations.size)
        assertEquals(1, RingtoneManager.created.single().stopCalls)
    }

    @Test fun actionCancellationAfterEffectStillWaitsForASuccessfulAcknowledgment() {
        show()
        val token = PendingIntent.created.first().also { it.failCancelAfter = true }
        IncomingCallManager.stopRinging()
        assertTrue(token.cancelled)
        token.failCancelAfter = false
        IncomingCallManager.stopRinging(); IncomingCallManager.stopRinging()
        assertEquals(2, token.cancelCalls)
    }

    @Test fun notificationCancelFailureCanBeRetriedWithoutAnActiveCall() {
        for (failure in listOf("cancel", "cancel_after")) {
            show(if (failure == "cancel") 11 else 22)
            notifications.failures = setOf(failure)
            val before = notifications.cancellations.size
            IncomingCallManager.stopRinging(); recover()
            IncomingCallManager.stopRinging(); IncomingCallManager.stopRinging()
            assertFalse(notifications.active.containsKey(IncomingCallManager.NOTIF_ID))
            assertEquals(before + 2, notifications.cancellations.size)
        }
    }

    @Test fun publishFailureAfterEffectStillRequiresNotificationCancellation() {
        notifications.failures = setOf("notify_after")
        show()
        assertTrue(notifications.active.containsKey(IncomingCallManager.NOTIF_ID))
        assertEquals(1, RingtoneManager.created.single().playCalls)
        diagnostic("publish failed after effect")
        IncomingCallManager.stopRinging()
        assertFalse(notifications.active.containsKey(IncomingCallManager.NOTIF_ID))
    }

    @Test fun initializedManagerCanRetryCleanupWithoutRestartingEffects() {
        show()
        power.locks.single().failures = setOf("release")
        PendingIntent.created.first().failCancel = true; notifications.failures = setOf("cancel")
        IncomingCallManager.stopRinging(); recover()
        IncomingCallManager.init()
        assertFalse(power.locks.single().held)
        assertTrue(PendingIntent.created.all { it.cancelled }); assertTrue(notifications.active.isEmpty())
        assertEquals(1, RingtoneManager.created.single().playCalls); assertEquals(1, RingtoneManager.created.single().stopCalls)
        val cancellations = notifications.cancellations.size
        repeat(3) { IncomingCallManager.init(); IncomingCallManager.refreshLanguage() }
        assertEquals(cancellations, notifications.cancellations.size)
        assertEquals(2, power.locks.single().releaseCalls)
        assertEquals(2, PendingIntent.created.first().cancelCalls)
    }

    @Test fun languageChannelFailureDoesNotBlockRetryingStoppedAlertCleanup() {
        show()
        power.locks.single().failures = setOf("release")
        PendingIntent.created.first().failCancel = true; notifications.failures = setOf("cancel")
        IncomingCallManager.stopRinging(); recover()
        notifications.failures = setOf("channel")
        IncomingCallManager.refreshLanguage()
        assertFalse(power.locks.single().held)
        assertTrue(PendingIntent.created.all { it.cancelled }); assertTrue(notifications.active.isEmpty())
        assertEquals(1, RingtoneManager.created.single().playCalls)
    }

    @Test fun expiredPendingWakeLockIsForgottenWithoutASecondRelease() {
        show()
        val lock = power.locks.single()
        lock.failures = setOf("release")
        IncomingCallManager.stopRinging()
        lock.failures = emptySet(); lock.held = false // OS timeout.
        IncomingCallManager.stopRinging()
        assertEquals(1, lock.releaseCalls)
        show(22)
        assertEquals(2, power.locks.size); assertTrue(power.locks.last().held)
    }

    @Test fun resourceFailuresResolveIndependentlyAndStaleActionsStayRejected() {
        show()
        val lock = power.locks.single().also { it.failures = setOf("release") }
        val token = PendingIntent.created.last().also { it.failCancel = true }
        val oldAction = token.send()
        notifications.failures = setOf("cancel")
        IncomingCallManager.stopRinging()
        IncomingCallActionReceiver().onReceive(context, oldAction)
        assertTrue(CallController.endings.isEmpty())
        lock.failures = emptySet(); IncomingCallManager.stopRinging(); assertFalse(lock.held)
        token.failCancel = false; IncomingCallManager.stopRinging(); assertTrue(token.cancelled)
        notifications.failures = emptySet(); IncomingCallManager.stopRinging()
        val attempts = notifications.cancellations.size
        IncomingCallManager.stopRinging()
        assertEquals(attempts, notifications.cancellations.size)
        assertTrue(notifications.active.isEmpty()); assertEquals(2, lock.releaseCalls)
    }
}
