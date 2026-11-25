package com.utun.chat.data

import android.app.Notification
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.os.Looper
import android.widget.Toast
import androidx.core.app.NotificationCompat
import com.utun.chat.ChatApplication
import com.utun.chat.R
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие controller/уведомления/гудки; только Android/JNI/repository — управляемые границы, без радиоэфира. */
@OptIn(ExperimentalCoroutinesApi::class)
class RadioCallLifecycleTest {
    private lateinit var context: Context
    private val dispatcher = StandardTestDispatcher()
    private val state get() = RadioCallController.state.value
    private val calls get() = NativeLib.radioCalls

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        Dispatchers.setMain(dispatcher)
        context = Context().also { ChatApplication.instance = it }
        AudioManager.shared = AudioManager()
        NativeLib.ready = true; NativeLib.failActivityRequest = false; NativeLib.acceptActivityRequest = true
        NativeLib.onRadioCall = { true }; CallController.state.value = null; StreamController.captureBlocked = false
        ChatRepository.failRadioLookup = false; MicrophonePermission.requester = null
        AppLanguage.failText = false; AppLanguage.failContext = false; AppLanguage.language = "en"
        NotificationCompat.failBuild = false
        RadioCallController.reset(); RadioCallController.setVisible(false)
        CoreActivity.visible(false); CoreActivity.call(false); CoreActivity.transfer(false); CoreActivity.radioCall(false)
        RingtoneManager.reset(); MediaPlayer.reset(); Toast.shown.clear(); calls.clear()
        NativeLib.activityRequests.clear(); CoreActivity.reset()
        LogManager.entries.clear(); LogManager.console = true
        RadioCallController.onEvent(12, null)
    }

    @After fun release() {
        NativeLib.onRadioCall = { true }; NativeLib.ready = true; NativeLib.failActivityRequest = false
        AppLanguage.failText = false; NotificationCompat.failBuild = false; MicrophonePermission.requester = null
        context.notificationManager?.failures = emptySet(); ChatRepository.failRadioLookup = false
        RadioCallController.reset(); IncomingCallManager.stopRinging(); CallRingback.stop()
        CallController.state.value = null; StreamController.captureBlocked = false
        dispatcher.scheduler.advanceUntilIdle(); Looper.getMainLooper().advanceBy(10_000)
        Dispatchers.resetMain()
        assertNull(state); assertNull(RadioCallController.activation.value)
        assertFalse(RadioCallController.busy())
    }

    private fun key(value: Long) = value.toULong().toString(16).padStart(64, '0')

    private fun event(id: Long = 11, group: Long = 7, peer: Long = 99, phase: Int = 2, reason: Int = 0, incoming: Boolean = true) =
        ByteBuffer.allocate(75).order(ByteOrder.LITTLE_ENDIAN).putLong(id).put(key(group).hexToByteArray())
            .put(if (peer == 0L) ByteArray(32) else MemberFilesProtocol.addressBytes(key(peer)))
            .put(phase.toByte()).put(reason.toByte()).put(if (incoming) 1.toByte() else 0.toByte()).array()
    private fun emit(id: Long = 11, phase: Int = 2, reason: Int = 0, incoming: Boolean = true) =
        RadioCallController.onEvent(65, event(id = id, phase = phase, reason = reason, incoming = incoming))
    private fun outgoing(): Long {
        RadioCallController.start(key(7), key(99))
        val id = calls.last { it.action == 1 }.id
        emit(id, 1, incoming = false)
        assertEquals(RadioCallPhase.OUTGOING, state?.phase)
        return id
    }
    private fun diagnostic(fragment: String) = synchronized(LogManager.entries) {
        assertTrue("Missing diagnostic: $fragment", LogManager.entries.any { fragment in it.message && it.level in listOf("ERROR", "WARN") })
    }
    private fun ended(reason: Int) {
        assertEquals(RadioCallPhase.ENDED, state?.phase); assertEquals(reason, state?.reason)
        assertNull(RadioCallController.activation.value); assertFalse(RadioCallController.busy())
        assertTrue(context.notificationManager?.active?.containsKey(IncomingCallManager.NOTIF_ID) != true)
    }

    @Test fun incomingReadyAndAcceptanceWaitForVisibleActivityAndActualAudio() {
        emit()
        assertEquals(RadioCallPhase.INCOMING, state?.phase); assertEquals("Alice", state?.peerName)
        assertEquals(2, calls.single().action); assertTrue(NativeLib.activityRequests.last())
        RadioCallController.accept(); assertNull(RadioCallController.activation.value)
        RadioCallController.setVisible(true); RadioCallController.accept()
        assertEquals(RadioCallPhase.ANSWERING, state?.phase); assertNull(RadioCallController.activation.value)
        assertEquals(listOf(2, 9), calls.map { it.action })
        emit(phase = 6)
        assertEquals(RadioCallPhase.ANSWERING, state?.phase); assertNull(RadioCallController.activation.value)
        RadioCallController.activated(11, true); assertEquals(listOf(2, 9), calls.map { it.action })
        emit(phase = 7)
        assertEquals(RadioCallPhase.PREPARING, state?.phase); assertEquals(11L, RadioCallController.activation.value?.id)
        val activation = RadioCallController.activation.value
        emit(phase = 7); assertSame(activation, RadioCallController.activation.value)
        emit(phase = 6); assertEquals(RadioCallPhase.PREPARING, state?.phase); assertSame(activation, RadioCallController.activation.value)
        assertEquals(listOf(2, 9), calls.map { it.action })
        RadioCallController.activated(12, true); assertNotNull(RadioCallController.activation.value)
        RadioCallController.activated(11, true); assertEquals(3, calls.last().action)
        emit(phase = 4, reason = 1); assertNull(state); assertFalse(NativeLib.activityRequests.last())
    }

    @Test fun outgoingRingingAndAcceptedResultActivateOnlyTheAcceptedChannel() {
        val id = outgoing()
        assertEquals(key(7), calls.first().group); assertEquals(key(99), calls.first().peer)
        emit(id, 3, incoming = false); assertEquals(RadioCallPhase.RINGING, state?.phase)
        emit(id, 4, 1, false); assertEquals(RadioCallPhase.PREPARING, state?.phase)
        assertEquals(key(7), RadioCallController.activation.value?.channelId)
        RadioCallController.activated(id, true); assertNull(state); assertNull(RadioCallController.activation.value)
    }

    @Test fun aQueuedStartAlreadyOwnsBusyUntilTheCorePublishesItsResult() {
        RadioCallController.start(key(7), key(99)); RadioCallController.start(key(8), key(100))
        assertEquals(1, calls.count { it.action == 1 }); assertTrue(RadioCallController.busy())
        val request = calls.single()
        RadioCallController.onEvent(65, event(request.id, phase = 4, reason = 5, incoming = false))
        assertFalse(RadioCallController.busy()); diagnostic("Call finished")
    }

    @Test fun invalidStartIdentifiersAreLoggedAndNeverReachNative() {
        for ((channel, peer) in listOf("bad" to 99L, "0" to 99L, key(7) to 0L)) {
            LogManager.entries.clear(); RadioCallController.start(channel, key(peer))
            assertTrue(calls.isEmpty()); diagnostic("Invalid call target")
        }
    }

    @Test fun rejectedAndThrowingStartLeaveNoPendingOwner() {
        for (throws in listOf(false, true)) {
            NativeLib.onRadioCall = { if (throws) error("START failed") else false }
            LogManager.entries.clear(); RadioCallController.start(key(7), key(99))
            assertFalse(RadioCallController.busy()); assertNull(state); diagnostic("Call command")
        }
    }

    @Test fun rejectedAndThrowingReadyCloseTheIncomingAlert() {
        for (throws in listOf(false, true)) {
            NativeLib.onRadioCall = { if (it.action != 2) true else if (throws) error("READY failed") else false }
            emit(if (throws) 12 else 11)
            ended(7); diagnostic("Call command")
        }
    }

    @Test fun aFailedDeclineStillEndsLocallyAndReleasesActivity() {
        emit(); NativeLib.onRadioCall = { error("DECLINE failed") }
        RadioCallController.end()
        ended(2); assertFalse(NativeLib.activityRequests.last()); diagnostic("Call command")
    }

    @Test fun aRejectedCancelStillEndsLocally() {
        val id = outgoing(); NativeLib.onRadioCall = { false }
        RadioCallController.end(id); ended(4); diagnostic("Call command")
    }

    @Test fun rejectedAnswerDuringStreamDoesNotKeepTheIncomingRingtone() {
        emit(); RadioCallController.setVisible(true); StreamController.captureBlocked = true
        NativeLib.onRadioCall = { false }; RadioCallController.accept()
        assertEquals(9, calls.last().action); ended(7); diagnostic("Call command")
    }

    @Test fun rejectedAudioAcceptanceEndsWithoutReplayingActivation() {
        emit(); RadioCallController.setVisible(true); RadioCallController.accept()
        emit(phase = 7)
        NativeLib.onRadioCall = { false }; RadioCallController.activated(11, true)
        ended(7); val count = calls.size
        RadioCallController.activated(11, true); assertEquals(count, calls.size); diagnostic("Call command")
    }

    @Test fun failedAudioReportsFailAndNeverAccepts() {
        emit(); RadioCallController.setVisible(true); RadioCallController.accept()
        emit(phase = 7)
        RadioCallController.activated(11, false)
        assertEquals(6, calls.last().action); assertFalse(calls.any { it.action == 3 })
        emit(phase = 4, reason = 7); ended(7)
    }

    @Test fun busyRejectionDoesNotReplaceTheCurrentInvitation() {
        emit(); val original = state; NativeLib.onRadioCall = { false }
        emit(12)
        assertSame(original, state); assertEquals(7, calls.last().action); diagnostic("Call command")
    }

    @Test fun duplicateIncomingDoesNotCreateAnotherAlertOrReady() {
        emit(); val original = state; val posted = requireNotNull(context.notificationManager).posted.size
        repeat(3) { emit() }
        assertSame(original, state); assertEquals(posted, requireNotNull(context.notificationManager).posted.size)
        assertEquals(1, calls.size)
    }

    @Test fun malformedEventsCannotOwnUiOrSendCommands() {
        val invalid = listOf(null, ByteArray(74), ByteArray(76), event(id = 0), event(group = 0), event(peer = 0), event(phase = 0),
            event(phase = 8), event(reason = 10), event(phase = 1), event(phase = 3), event(phase = 5),
            event(phase = 4, reason = 0), event(reason = 1), event().also { it[74] = 2 })
        for (data in invalid) {
            LogManager.entries.clear(); RadioCallController.onEvent(65, data)
            assertNull(state); assertTrue(calls.isEmpty()); diagnostic("Invalid call event")
        }
    }

    @Test fun answeringAndSelectedCannotBelongToAnOutgoingCall() {
        val id = outgoing(); val original = state
        val before = calls.size
        for (phase in listOf(6, 7)) {
            LogManager.entries.clear(); emit(id, phase, incoming = false)
            assertSame(original, state); assertNull(RadioCallController.activation.value)
            assertEquals(before, calls.size); diagnostic("Invalid call event")
        }
    }

    @Test fun answeringAndSelectedFromAnOldCallCannotCreateUiOrCommands() {
        for (phase in listOf(6, 7)) {
            emit(phase = phase)
            assertNull(state); assertNull(RadioCallController.activation.value); assertTrue(calls.isEmpty())
        }
        assertFalse(RadioCallController.busy()); assertTrue(NativeLib.activityRequests.isEmpty())
    }

    @Test fun selectedBeforeAnswerCannotStartAudioAndAnotherDeviceSelectionEndsTheInvitation() {
        emit(); val original = state
        emit(phase = 7)
        assertSame(original, state); assertNull(RadioCallController.activation.value)
        RadioCallController.setVisible(true); RadioCallController.accept()
        assertEquals(RadioCallPhase.ANSWERING, state?.phase)
        emit(phase = 4, reason = 4)
        ended(4)
        emit(phase = 7); RadioCallController.activated(11, true)
        assertNull(RadioCallController.activation.value); assertFalse(calls.any { it.action == 3 })
    }

    @Test fun rejectedAnswerEndsLocallyBeforeAnyAudioActivation() {
        emit(); RadioCallController.setVisible(true)
        NativeLib.onRadioCall = { it.action != 9 }
        RadioCallController.accept()
        assertEquals(9, calls.last().action); ended(7); diagnostic("Call command")
        emit(phase = 7); assertNull(RadioCallController.activation.value)
    }

    @Test fun aMatchingIdWithWrongGroupPeerOrRoleCannotFinishTheCurrentCall() {
        emit(); val original = state
        for (data in listOf(event(group = 8, phase = 4, reason = 4), event(peer = 100, phase = 4, reason = 4),
            event(phase = 4, reason = 4, incoming = false))) {
            RadioCallController.onEvent(65, data); assertSame(original, state)
        }
        diagnostic("identity mismatch")
    }

    @Test fun lateRingingCannotRegressAnOutgoingAudioActivation() {
        val id = outgoing(); emit(id, 4, 1, false)
        val activation = RadioCallController.activation.value
        emit(id, 3, incoming = false)
        assertEquals(RadioCallPhase.PREPARING, state?.phase); assertSame(activation, RadioCallController.activation.value)
    }

    @Test fun continueRequiresWaitDecisionAndLogsAQueueFailure() {
        val id = outgoing(); emit(id, 5, incoming = false)
        assertEquals(RadioCallPhase.WAIT_DECISION, state?.phase)
        NativeLib.onRadioCall = { false }; RadioCallController.continueCall(id)
        ended(5); assertEquals(8, calls.last().action); diagnostic("Call command")
    }

    @Test fun notificationAnswerWaitsForVisibilityAndCannotAcceptAReplacement() {
        emit()
        val intent = Intent().putExtra(IncomingCallManager.EXTRA_RADIO_CALL, true)
            .putExtra(IncomingCallManager.EXTRA_CALL_ID, 11L).putExtra(IncomingCallManager.EXTRA_ANSWER_RADIO, true)
        RadioCallController.notificationIntent(intent); assertNull(RadioCallController.activation.value)
        assertFalse(intent.getBooleanExtra(IncomingCallManager.EXTRA_ANSWER_RADIO, false))
        RadioCallController.end(); emit(12)
        RadioCallController.setVisible(true); assertNull(RadioCallController.activation.value)
        RadioCallController.notificationIntent(Intent(intent).putExtra(IncomingCallManager.EXTRA_ANSWER_RADIO, true))
        assertNull(RadioCallController.activation.value)
    }

    @Test fun aPermissionCallbackFromAnOldCallCannotActivateItsReplacement() {
        emit(); RadioCallController.setVisible(true); context.permissionGranted = false
        var callback: (() -> Unit)? = null
        MicrophonePermission.requester = { _, granted -> callback = granted }
        RadioCallController.accept(); assertNotNull(callback); assertNull(RadioCallController.activation.value)
        RadioCallController.end(); emit(12); context.permissionGranted = true
        callback?.invoke(); assertNull(RadioCallController.activation.value)
    }

    @Test fun outcomeNotificationPreservesForegroundServiceAndPhoneOutcomeNotifications() {
        val manager = requireNotNull(context.notificationManager)
        val call = Notification(); val phone = Notification(); val stream = Notification()
        manager.active[1002] = call; manager.active[1003] = phone; manager.active[1004] = stream
        val id = outgoing(); emit(id, 4, 2, false)
        assertSame(call, manager.active[1002]); assertSame(phone, manager.active[1003]); assertSame(stream, manager.active[1004])
        assertTrue(manager.active.containsKey(1006))
    }

    @Test fun outcomePresentationFailuresCannotStrandTheEndedState() {
        for (failure in listOf("notify", "builder", "language", "manager")) {
            val id = outgoing()
            when (failure) {
                "notify" -> requireNotNull(context.notificationManager).failures = setOf("notify")
                "builder" -> NotificationCompat.failBuild = true
                "language" -> AppLanguage.failText = true
                "manager" -> context.notificationManager = null
            }
            emit(id, 4, 2, false); ended(2)
            dispatcher.scheduler.advanceUntilIdle(); assertNull(state)
            diagnostic("Call result presentation failed")
            NotificationCompat.failBuild = false; AppLanguage.failText = false
            context.notificationManager = android.app.NotificationManager()
        }
    }

    @Test fun anOldEndedTimerCannotClearTheNextCallAndResetDropsPendingActivation() {
        emit(); RadioCallController.end(); dispatcher.scheduler.runCurrent()
        emit(12); dispatcher.scheduler.advanceUntilIdle(); assertEquals(12L, state?.id)
        RadioCallController.setVisible(true); RadioCallController.accept(); emit(12, phase = 7)
        assertNotNull(RadioCallController.activation.value)
        RadioCallController.onEvent(13, null); assertNull(state); assertNull(RadioCallController.activation.value)
        val count = calls.size; RadioCallController.start(key(7), key(99)); assertEquals(count, calls.size)
    }

    @Test fun repositoryFailureUsesFallbackNamesAndStillAcknowledgesTheCall() {
        ChatRepository.failRadioLookup = true; emit()
        assertEquals(key(99), state?.peerName); assertEquals(key(7), state?.channelName)
        assertEquals(2, calls.last().action); diagnostic("Call names unavailable")
    }

    @Test fun activityQueueFailureDoesNotSkipReadyOrLocalCleanup() {
        NativeLib.failActivityRequest = true; emit()
        assertEquals(2, calls.last().action); assertEquals(RadioCallPhase.INCOMING, state?.phase)
        RadioCallController.end(); ended(2); diagnostic("Call activity update failed")
        NativeLib.failActivityRequest = false; CoreActivity.sync(); assertFalse(NativeLib.activityRequests.last())
    }

    @Test fun notificationIntentRejectsWorkerBeforeConsumingItsExtras() {
        emit()
        val intent = Intent().putExtra(IncomingCallManager.EXTRA_RADIO_CALL, true)
            .putExtra(IncomingCallManager.EXTRA_CALL_ID, 11L).putExtra(IncomingCallManager.EXTRA_ANSWER_RADIO, true)
        val error = AtomicReference<Throwable?>()
        Thread { try { RadioCallController.notificationIntent(intent) } catch (e: Throwable) { error.set(e) } }.apply { start(); join() }
        assertTrue(error.get() is IllegalStateException)
        assertTrue(intent.getBooleanExtra(IncomingCallManager.EXTRA_ANSWER_RADIO, false)); diagnostic("requires main thread")
    }

    @Test fun continuedCallUsesTheSameAttemptAndAcceptsTheNextCoreTransitions() {
        val id = outgoing(); emit(id, 5, incoming = false)
        RadioCallController.continueCall(id)
        assertEquals(id, calls.last().id); assertEquals(8, calls.last().action)
        emit(id, 1, incoming = false); emit(id, 3, incoming = false)
        assertEquals(RadioCallPhase.RINGING, state?.phase)
        emit(id, 4, 1, false); RadioCallController.activated(id, true)
        assertNull(state); assertFalse(RadioCallController.busy())
    }

    @Test fun unsignedIdentifiersPreserveAllTheirWireBits() {
        RadioCallController.start(key(-1), key(-2))
        val request = calls.single()
        assertEquals(key(-1), request.group); assertEquals(key(-2), request.peer)
        RadioCallController.onEvent(65, event(request.id, -1, -2, 1, incoming = false))
        assertEquals(key(-1), state?.channelId); assertEquals(key(-2), state?.peer)
    }

    @Test fun resetCancelsAQueuedStartAndOldStartedEventCannotReopenIt() {
        RadioCallController.start(key(7), key(99)); val id = calls.single().id
        RadioCallController.reset()
        assertEquals(5, calls.last().action); assertEquals(id, calls.last().id); assertFalse(RadioCallController.busy())
        RadioCallController.onEvent(12, null); emit(id, 1, incoming = false)
        assertNull(state); diagnostic("without pending start")
    }

    @Test fun anOldResultCannotPublishAnOutcomeOrReleaseTheNextPendingStart() {
        val old = outgoing(); RadioCallController.end()
        RadioCallController.start(key(8), key(100)); val pending = calls.last { it.action == 1 }
        val posted = requireNotNull(context.notificationManager).posted.size
        emit(old, 4, 2, false)
        assertTrue(RadioCallController.busy()); assertNull(state)
        assertEquals(posted, requireNotNull(context.notificationManager).posted.size)
        RadioCallController.onEvent(65, event(pending.id, 8, 100, 1, incoming = false))
        assertEquals(pending.id, state?.id)
    }

    @Test fun anEndedCardsTimerCannotCancelAQueuedReplacementStart() {
        outgoing(); RadioCallController.end(); dispatcher.scheduler.runCurrent()
        RadioCallController.start(key(7), key(99)); val pending = calls.last { it.action == 1 }
        dispatcher.scheduler.advanceUntilIdle()
        assertTrue(RadioCallController.busy())
        emit(pending.id, 1, incoming = false); assertEquals(pending.id, state?.id)
    }

    @Test fun stoppedCoreReportsUnavailableAndAnOngoingPhoneCallReportsBusy() {
        RadioCallController.setVisible(true); NativeLib.ready = false
        RadioCallController.start(key(7), key(99))
        assertEquals("en:${R.string.radio_call_unavailable}", Toast.shown.last()); assertTrue(calls.isEmpty())
        NativeLib.ready = true; CallController.state.value = CallController.State(ongoing = true)
        RadioCallController.start(key(7), key(99))
        assertEquals("en:${R.string.radio_call_busy}", Toast.shown.last()); assertTrue(calls.isEmpty())
    }
}
