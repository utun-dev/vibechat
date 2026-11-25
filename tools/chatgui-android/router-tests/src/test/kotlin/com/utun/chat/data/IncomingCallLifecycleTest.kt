package com.utun.chat.data

import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.os.Build
import android.os.Looper
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import com.utun.chat.ChatApplication
import com.utun.chat.MainActivity
import com.utun.chat.R
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие manager/receiver/ringtone: identity OS tokens, отказы служб и жизненный цикл входящего оповещения. */
@OptIn(ExperimentalCoroutinesApi::class)
class IncomingCallLifecycleTest {
    private lateinit var context: Context
    private lateinit var notifications: NotificationManager
    private lateinit var power: PowerManager
    private val receiver = IncomingCallActionReceiver()
    private val current get() = requireNotNull(notifications.active[IncomingCallManager.NOTIF_ID])
    private val dispatcher = StandardTestDispatcher()
    private val radioEndings get() = NativeLib.radioCalls.filter { it.action == 4 }.map { it.id }

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        context = Context().also { ChatApplication.instance = it }
        notifications = requireNotNull(context.notificationManager)
        power = requireNotNull(context.powerManager)
        AudioManager.shared = AudioManager()
        Build.VERSION.SDK_INT = 36
        AppLanguage.language = "en"; AppLanguage.failContext = false; AppLanguage.failText = false
        CallController.media = false; CallController.endings.clear()
        Dispatchers.setMain(dispatcher)
        RadioCallController.reset(); NativeLib.radioCalls.clear(); NativeLib.onRadioCall = { true }
        NotificationCompat.failBuild = false
        PendingIntent.reset(); RingtoneManager.reset(); MediaPlayer.reset()
        // Процесс приложения живёт один раз; JVM-suite моделирует новый Application между сценариями.
        IncomingCallManager::class.java.getDeclaredField("initialized").apply { isAccessible = true }.setBoolean(null, false)
        LogManager.entries.clear(); LogManager.console = true
    }

    @After fun release() {
        AppLanguage.failContext = false; AppLanguage.failText = false
        notifications.failures = emptySet(); NotificationCompat.failBuild = false
        power.failures = emptySet(); power.locks.forEach { it.failures = emptySet() }
        PendingIntent.created.forEach { it.failCancel = false; it.failCancelAfter = false }
        IncomingCallManager.stopRinging()
        RadioCallController.reset(); dispatcher.scheduler.advanceUntilIdle(); Dispatchers.resetMain()
        Looper.getMainLooper().advanceBy(10_000)
        Build.VERSION.SDK_INT = 36
    }

    private fun diagnostic(fragment: String) = assertTrue("Missing diagnostic: $fragment",
        LogManager.entries.any { it.message.contains(fragment) && it.level in listOf("WARN", "ERROR") })

    private fun decline(notification: Notification = current): PendingIntent = notification.actions.first().intent
    private fun callId(intent: Intent) = intent.getLongExtra(IncomingCallManager.EXTRA_CALL_ID, 0L)
    private fun isRadio(intent: Intent) = intent.getBooleanExtra(IncomingCallManager.EXTRA_RADIO_CALL, false)
    private fun deliver(intent: Intent) = receiver.onReceive(context, intent)
    private fun show(id: Long = 11L, radio: Boolean = false) = IncomingCallManager.show(id, "123", "01".repeat(32), radio)
    private fun setRadioCall(id: Long, peerName: String = "Alice", channelName: String = "Team") {
        @Suppress("UNCHECKED_CAST")
        val state = RadioCallController::class.java.getDeclaredField("_state").apply { isAccessible = true }
            .get(null) as MutableStateFlow<RadioCallState?>
        state.value = RadioCallState(id, "123", "01".repeat(32), peerName, channelName, true, RadioCallPhase.INCOMING)
    }
    private fun rejectOnWorker(action: () -> Unit) {
        val error = AtomicReference<Throwable?>()
        Thread({ try { action() } catch (e: Throwable) { error.set(e) } }, "incoming-test-worker").apply { start(); join() }
        assertTrue("Must reject before mutation: ${error.get()}", error.get() is IllegalStateException)
    }

    @Test fun initializationCreatesSilentHighPriorityChannelOnlyAfterSuccess() {
        notifications.failures = setOf("channel")
        IncomingCallManager.init()
        diagnostic("notification channel unavailable")
        assertTrue(notifications.channels.isEmpty())
        notifications.failures = emptySet()
        repeat(3) { IncomingCallManager.init() }
        assertEquals(2, notifications.channelAttempts)
        val channel = notifications.channels.getValue(IncomingCallManager.CHANNEL_CALL)
        assertEquals(NotificationManager.IMPORTANCE_HIGH, channel.importance)
        assertTrue(channel.silent)
        assertFalse(channel.vibration)
        assertEquals("en:${R.string.incoming_calls}", channel.name)
    }

    @Test fun failedStartupChannelCanRecoverWhenCallIsShown() {
        notifications.failures = setOf("channel")
        IncomingCallManager.init()
        notifications.failures = emptySet()
        show()
        assertEquals(2, notifications.channelAttempts)
        assertEquals(11L, callId(requireNotNull(current.contentIntent).send()))
        assertEquals(1, RingtoneManager.created.single().playCalls)
    }

    @Test fun coldStartRemovesOrphanedIncomingNotificationAndKeepsOtherNotifications() {
        notifications.active[IncomingCallManager.NOTIF_ID] = Notification()
        val other = Notification()
        notifications.active[1002] = other
        repeat(3) { IncomingCallManager.init() }
        assertFalse(notifications.active.containsKey(IncomingCallManager.NOTIF_ID))
        assertSame(other, notifications.active[1002])
        assertEquals(listOf(IncomingCallManager.NOTIF_ID), notifications.cancellations)
        assertTrue(RingtoneManager.created.isEmpty())
    }

    @Test fun channelRecoveryWithoutLiveCallRemovesOrphanedNotification() {
        notifications.active[IncomingCallManager.NOTIF_ID] = Notification()
        notifications.failures = setOf("channel")
        IncomingCallManager.init()
        notifications.failures = emptySet()
        IncomingCallManager.refreshLanguage()
        assertFalse(notifications.active.containsKey(IncomingCallManager.NOTIF_ID))
        assertTrue(RingtoneManager.created.isEmpty())
        assertTrue(power.locks.isEmpty())
    }

    @Test fun p2pAlertHasExplicitImmutableActionsAndOwnedTimedWakeLock() {
        show()
        val notification = current
        assertEquals(NotificationCompat.CATEGORY_CALL, notification.category)
        assertEquals(NotificationCompat.PRIORITY_MAX, notification.priority)
        assertTrue(notification.ongoing)
        assertFalse(notification.autoCancel)
        assertTrue(notification.onlyAlertOnce)
        val content = requireNotNull(notification.contentIntent)
        assertEquals(content, notification.fullScreenIntent)
        val launch = content.send()
        assertEquals(MainActivity::class.java, launch.component)
        assertEquals(11L, callId(launch))
        assertEquals("123", launch.getStringExtra(IncomingCallManager.EXTRA_CHANNEL_ID))
        assertEquals("01".repeat(32), launch.getStringExtra(IncomingCallManager.EXTRA_PEER_ADDRESS))
        assertFalse(isRadio(launch))
        assertEquals(1, notification.actions.size)
        assertEquals(IncomingCallActionReceiver::class.java, decline().send().component)
        assertEquals(IncomingCallActionReceiver.ACTION_DECLINE, decline().send().action)
        PendingIntent.created.forEach { assertTrue(it.flags and PendingIntent.FLAG_IMMUTABLE != 0) }
        val lock = power.locks.single()
        assertTrue(lock.held)
        assertEquals(30_000L, lock.timeout)
        assertEquals(PowerManager.SCREEN_BRIGHT_WAKE_LOCK or PowerManager.ACQUIRE_CAUSES_WAKEUP or PowerManager.ON_AFTER_RELEASE, lock.flags)
        assertEquals("utun:incoming_call", lock.tag)
    }

    @Test fun radioAlertPreservesPeerNamesAndDistinctAnswerAction() {
        setRadioCall(11)
        show(radio = true)
        assertEquals("Alice · Team", current.text)
        assertEquals("en:${R.string.radio_call_incoming}", current.title)
        assertEquals(2, current.actions.size)
        val answer = current.actions[1].intent
        assertNotEquals(current.contentIntent, answer)
        assertTrue(isRadio(answer.send()))
        assertTrue(answer.send().getBooleanExtra(IncomingCallManager.EXTRA_ANSWER_RADIO, false))
        assertEquals(11L, callId(answer.send()))
        assertTrue(RingtoneManager.uris.single().toString().endsWith("/${R.raw.radio_incoming}"))
    }

    @Test fun radioWithoutControllerSnapshotUsesChannelFallback() {
        show(radio = true)
        assertEquals("123", current.text)
    }

    @Test fun p2pMediaWarningSurvivesNotificationBuild() {
        CallController.media = true
        show()
        assertEquals("en:999", current.text)
        assertEquals(current.text, current.bigText)
    }

    @Test fun repeatedShowDoesNotRestartSoundWakeLockOrFullScreenAlert() {
        repeat(3) { show() }
        assertEquals(1, RingtoneManager.created.size)
        assertEquals(1, power.locks.size)
        assertEquals(2, PendingIntent.created.size)
        assertEquals(3, notifications.posted.size)
        assertNotNull(notifications.posted.first().fullScreenIntent)
        assertTrue(notifications.posted.drop(1).all { it.fullScreenIntent == null })
    }

    @Test fun languageRefreshUpdatesTextWithoutRepeatingEffectsOrAccumulatingTokens() {
        show(radio = true)
        val content = current.contentIntent
        AppLanguage.language = "ru"
        repeat(3) { IncomingCallManager.refreshLanguage() }
        assertEquals("ru:${R.string.incoming_calls}", notifications.channels.getValue(IncomingCallManager.CHANNEL_CALL).name)
        assertEquals("ru:${R.string.radio_call_incoming}", current.title)
        assertNull(current.fullScreenIntent)
        assertEquals(content, current.contentIntent)
        assertEquals(3, PendingIntent.created.size)
        assertEquals(1, RingtoneManager.created.size)
        assertEquals(1, power.locks.size)
        IncomingCallManager.stopRinging()
        PendingIntent.created.forEach { assertEquals(1, it.cancelCalls) }
    }

    @Test fun languageRefreshWithoutCallPreparesChannelAndClearsOrphanedNotification() {
        notifications.active[IncomingCallManager.NOTIF_ID] = Notification()
        repeat(3) { IncomingCallManager.refreshLanguage() }
        assertEquals(1, notifications.channels.size)
        assertTrue(notifications.posted.isEmpty())
        assertTrue(notifications.active.isEmpty())
        assertEquals(listOf(IncomingCallManager.NOTIF_ID), notifications.cancellations)
        assertTrue(power.locks.isEmpty())
        assertTrue(RingtoneManager.created.isEmpty())
    }

    @Test fun languageChannelFailureDoesNotStopActiveAlertAndNextRefreshRetries() {
        show()
        notifications.failures = setOf("channel")
        IncomingCallManager.refreshLanguage()
        assertEquals(1, notifications.posted.size)
        assertEquals(0, RingtoneManager.created.single().stopCalls)
        notifications.failures = emptySet()
        IncomingCallManager.refreshLanguage()
        assertEquals(2, notifications.posted.size)
        assertEquals(1, RingtoneManager.created.size)
    }

    @Test fun replacingCallCancelsAllPreviousTokensAndEffects() {
        show(radio = true)
        val old = PendingIntent.created.toList()
        show(22)
        old.forEach { assertTrue(it.cancelled); assertEquals(1, it.cancelCalls) }
        assertEquals(listOf(1, 0), RingtoneManager.created.map { it.stopCalls })
        assertEquals(1, power.locks.first().releaseCalls)
        assertEquals(22L, callId(requireNotNull(current.contentIntent).send()))
    }

    @Test fun idempotentStopCancelsActionsSoundWakeLockAndNotification() {
        show(radio = true)
        repeat(3) { IncomingCallManager.stopRinging() }
        assertEquals(1, RingtoneManager.created.single().stopCalls)
        assertEquals(1, power.locks.single().releaseCalls)
        assertEquals(listOf(IncomingCallManager.NOTIF_ID), notifications.cancellations)
        assertTrue(notifications.active.isEmpty())
        PendingIntent.created.forEach { assertTrue(it.cancelled); assertEquals(1, it.cancelCalls) }
        val pulses = requireNotNull(context.vibrator).pulseAttempts
        Looper.getMainLooper().advanceBy(10_000)
        assertEquals(pulses, requireNotNull(context.vibrator).pulseAttempts)
    }

    @Test fun fullWidthAndSignedCallIdsHaveDistinctTokensEvenIfCancellationFails() {
        val ids = listOf(1L, 0x100000001L, Long.MIN_VALUE, Long.MAX_VALUE, -1L)
        val old = mutableListOf<Pair<PendingIntent, Long>>()
        for (id in ids) {
            show(id, radio = true)
            old.add(current.actions[1].intent to id)
            PendingIntent.created.forEach { it.failCancel = true }
        }
        assertEquals(ids.size, old.map { it.first }.distinct().size)
        old.forEach { (token, id) -> assertEquals(id, callId(token.send())) }
        diagnostic("cancel action failed")
    }

    @Test fun radioAndP2pUseSeparateIdentityForSameIdWhenCancellationFails() {
        show()
        val old = requireNotNull(current.contentIntent)
        PendingIntent.created.forEach { it.failCancel = true }
        show(radio = true)
        val next = requireNotNull(current.contentIntent)
        assertNotEquals(old, next)
        assertFalse(isRadio(old.send()))
        assertTrue(isRadio(next.send()))
    }

    @Test fun currentDeclineIsDeliveredOnceToCorrectController() {
        show()
        val intent = decline().send()
        repeat(3) { deliver(intent) }
        assertEquals(listOf(11L to 2), CallController.endings)
        assertTrue(radioEndings.isEmpty())
        setRadioCall(22)
        show(22, radio = true)
        val radioIntent = decline().send()
        repeat(3) { deliver(radioIntent) }
        assertEquals(listOf(22L), radioEndings)
        assertEquals(1, CallController.endings.size)
    }

    @Test fun alreadyDeliveredDeclineCannotHangUpAcceptedCall() {
        show()
        val delayed = decline().send()
        IncomingCallManager.stopRinging() // accept завершает оповещение до обработки следующего main callback.
        deliver(delayed)
        assertTrue(CallController.endings.isEmpty())
        diagnostic("stale DECLINE ignored")
    }

    @Test fun alreadyDeliveredDeclineCannotEndReplacementOrWrongCallKind() {
        show()
        val old = decline().send()
        show(22)
        deliver(old)
        val wrongKind = decline().send().putExtra(IncomingCallManager.EXTRA_RADIO_CALL, true)
        deliver(wrongKind)
        assertTrue(CallController.endings.isEmpty())
        assertTrue(radioEndings.isEmpty())
        assertEquals(22L, callId(decline().send()))
    }

    @Test fun malformedAndUnknownActionsDoNotAffectCurrentCall() {
        show()
        deliver(Intent().apply { action = "unknown" })
        deliver(Intent().apply { action = IncomingCallActionReceiver.ACTION_DECLINE })
        assertTrue(CallController.endings.isEmpty())
        diagnostic("ignored notification action=unknown")
        diagnostic("bad call_id")
        assertEquals(0, RingtoneManager.created.single().stopCalls)
    }

    @Test fun invalidArgumentsDoNotReplaceWorkingAlert() {
        show()
        IncomingCallManager.show(0L, "123", "01".repeat(32))
        IncomingCallManager.show(22L, "", "01".repeat(32))
        IncomingCallManager.show(22L, "123", "00".repeat(32))
        assertEquals(1, RingtoneManager.created.size)
        assertEquals(0, RingtoneManager.created.single().stopCalls)
        assertEquals(11L, callId(decline().send()))
        assertEquals(3, LogManager.entries.count { it.message.startsWith("invalid alert") && it.level == "ERROR" })
    }

    @Test fun permissionPolicyUsesHeadsUpFallbackAndNoModernQueryOnOlderSdk() {
        Build.VERSION.SDK_INT = 33
        notifications.failures = setOf("full screen")
        assertTrue(IncomingCallManager.canFullScreenIntent())
        show()
        assertNotNull(current.fullScreenIntent)
        assertEquals(0, notifications.fullScreenQueries)
        Build.VERSION.SDK_INT = 34
        show(22)
        assertNull(current.fullScreenIntent)
        diagnostic("full-screen permission unavailable")
        notifications.failures = emptySet(); notifications.fullScreenAllowed = false
        show(33)
        assertNull(current.fullScreenIntent)
        assertFalse(IncomingCallManager.canFullScreenIntent())
    }

    @Test fun disabledNotificationsKeepSoundVibrationAndWakeLock() {
        notifications.notificationsEnabled = false
        show()
        assertTrue(notifications.posted.isEmpty())
        assertTrue(PendingIntent.created.isEmpty())
        assertEquals(1, RingtoneManager.created.single().playCalls)
        assertEquals(1, requireNotNull(context.vibrator).pulseAttempts)
        assertTrue(power.locks.single().held)
        diagnostic("notifications disabled")
    }

    @Test fun absentNotificationServiceIsDiagnosedAndDoesNotBlockRinging() {
        context.notificationManager = null
        assertFalse(IncomingCallManager.canFullScreenIntent())
        show()
        assertEquals(1, RingtoneManager.created.single().playCalls)
        assertTrue(power.locks.single().held)
        diagnostic("notification channel unavailable")
        context.notificationManager = notifications
        IncomingCallManager.refreshLanguage()
        assertEquals(1, notifications.posted.size)
    }

    @Test fun notificationFailureMatrixLeavesRingingAndCleansOwnedTokens() {
        for (failure in listOf("channel", "enabled", "notify", "security")) {
            notifications.failures = setOf(failure)
            IncomingCallManager.refreshLanguage()
            show(11L + notifications.channelAttempts)
            assertEquals(1, RingtoneManager.created.last().playCalls)
            assertEquals(0, RingtoneManager.created.last().stopCalls)
            IncomingCallManager.stopRinging()
        }
        assertTrue(PendingIntent.created.all { it.cancelled })
        diagnostic("notification channel unavailable")
        diagnostic("notification permission query unavailable")
        diagnostic("notification service unavailable")
        diagnostic("notify denied")
    }

    @Test fun partialTokenCreationFailureReleasesEveryCreatedTokenAtStop() {
        for (failedAttempt in 1..3) {
            PendingIntent.reset()
            PendingIntent.failCreationAt = failedAttempt
            show(10L + failedAttempt, radio = true)
            assertEquals(failedAttempt - 1, PendingIntent.created.size)
            assertEquals(1, RingtoneManager.created.last().playCalls)
            IncomingCallManager.stopRinging()
            PendingIntent.created.forEach { assertTrue(it.cancelled) }
        }
        diagnostic("pending intent creation unavailable")
    }

    @Test fun notificationBuildAndLanguageFailuresKeepRingingAndCanRetry() {
        NotificationCompat.failBuild = true
        show()
        assertTrue(notifications.posted.isEmpty())
        diagnostic("notification build unavailable")
        NotificationCompat.failBuild = false
        AppLanguage.failText = true
        show(22)
        diagnostic("language text unavailable")
        AppLanguage.failText = false
        AppLanguage.failContext = true
        show(33)
        diagnostic("language context unavailable")
        AppLanguage.failContext = false
        IncomingCallManager.refreshLanguage()
        assertEquals(33L, callId(requireNotNull(current.contentIntent).send()))
        assertEquals(3, RingtoneManager.created.size)
    }

    @Test fun powerServiceFailureKeepsSoundAndNotificationWorking() {
        context.powerManager = null
        show()
        assertEquals(1, notifications.posted.size)
        assertEquals(1, RingtoneManager.created.single().playCalls)
        diagnostic("wake lock failed")
    }

    @Test fun wakeLockCreationAndPartialAcquireFailureDoNotLeakOrStopOtherEffects() {
        for (failure in listOf("create", "acquire")) {
            power.failures = setOf(failure)
            show(if (failure == "create") 11 else 22)
            assertEquals(1, RingtoneManager.created.last().playCalls)
            IncomingCallManager.stopRinging()
        }
        assertEquals(2, notifications.posted.size)
        assertEquals(1, power.locks.size)
        assertEquals(1, power.locks.single().releaseCalls)
        assertFalse(power.locks.single().held)
    }

    @Test fun expiredWakeLockIsNotReleasedAgain() {
        show()
        val lock = power.locks.single()
        lock.held = false // OS timeout.
        IncomingCallManager.stopRinging()
        assertEquals(0, lock.releaseCalls)
        assertTrue(PendingIntent.created.all { it.cancelled })
    }

    @Test fun cleanupFailureMatrixDoesNotSkipOtherOwnedResources() {
        for (failure in listOf("query", "release")) {
            show(if (failure == "query") 11 else 22)
            val lock = power.locks.last().also { it.failures = setOf(failure) }
            val tokens = listOf(requireNotNull(current.contentIntent), decline())
            tokens.first().failCancel = true
            notifications.failures = setOf("cancel")
            val cancellations = notifications.cancellations.size
            repeat(3) { IncomingCallManager.stopRinging() }
            assertEquals(1, RingtoneManager.created.last().stopCalls)
            assertEquals(3, tokens.first().cancelCalls)
            assertTrue(tokens.drop(1).all { it.cancelled && it.cancelCalls == 1 })
            assertTrue(lock.held)
            lock.failures = emptySet(); tokens.first().failCancel = false; notifications.failures = emptySet()
            repeat(3) { IncomingCallManager.stopRinging() }
            assertFalse(lock.held)
            assertTrue(tokens.all { it.cancelled })
            assertEquals(4, tokens.first().cancelCalls)
            assertEquals(cancellations + 4, notifications.cancellations.size)
            assertFalse(notifications.active.containsKey(IncomingCallManager.NOTIF_ID))
        }
        diagnostic("wake lock release failed")
        diagnostic("cancel action failed")
        diagnostic("cancel notification failed")
        assertEquals(8, notifications.cancellations.size)
    }

    @Test fun workerCallsAreRejectedBeforeChangingCurrentAlert() {
        show()
        rejectOnWorker { IncomingCallManager.init() }
        rejectOnWorker { show(22) }
        rejectOnWorker { IncomingCallManager.refreshLanguage() }
        rejectOnWorker { IncomingCallManager.stopRinging() }
        val intent = decline().send()
        rejectOnWorker { deliver(intent) }
        assertEquals(1, notifications.posted.size)
        assertEquals(1, RingtoneManager.created.size)
        assertEquals(0, RingtoneManager.created.single().stopCalls)
        diagnostic("requires main thread")
    }
}
