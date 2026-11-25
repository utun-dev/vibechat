package com.utun.chat.data

import android.app.Notification
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import com.utun.chat.ChatApplication
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.TimeUnit
import java.util.concurrent.CountDownLatch
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

/** Настоящие service/controller: частичные OS-эффекты и отказы media моделируются на границах, без DSP/камеры. */
@OptIn(ExperimentalCoroutinesApi::class)
class CallServiceLifecycleTest {
    private lateinit var context: Context
    private val dispatcher = StandardTestDispatcher()
    private val services = mutableListOf<CallService>()
    private val gates = mutableListOf<CountDownLatch>()
    private val main get() = Looper.getMainLooper()
    private val power get() = requireNotNull(context.powerManager)
    private val manager get() = requireNotNull(context.notificationManager)
    private val state get() = CallController.state.value

    @Before fun prepare() {
        assertSame(main, Looper.myLooper()); Dispatchers.setMain(dispatcher)
        context = Context().also { ChatApplication.instance = it }; AudioManager.shared = AudioManager()
        NativeLib.ready = true; NativeLib.failActivityRequest = false; NativeLib.acceptActivityRequest = true
        NativeLib.rejectedCallCommands = emptySet()
        AppLanguage.failText = false; AppLanguage.language = "en"; NotificationCompat.failBuild = false
        Handler.rejectImmediate = false; Handler.rejectDelayed = false; PendingIntent.reset()
        MediaAudioController.beforeIdle = {}
        AudioRecorderManager.stopped = java.util.concurrent.CompletableFuture.completedFuture(Unit)
        TelecomCallManager.pending = false
        CallVideoEngine.failCreate = false; CallVideoEngine.created.clear()
        CallController.reset(); RadioAudioEngine.instance().activeGroup.value = null; RadioAudioEngine.instance().restored.clear()
        NativeLib.callCommands.clear(); LogManager.entries.clear(); LogManager.console = true
        CallController.onEvent(12, null)
    }

    @After fun release() {
        AudioRecorderManager.stopped.complete(Unit)
        gates.forEach { it.countDown() }; MediaAudioController.beforeIdle = {}
        TelecomCallManager.pending = false
        NativeLib.failActivityRequest = false
        NativeLib.rejectedCallCommands = emptySet()
        Handler.rejectImmediate = false; Handler.rejectDelayed = false
        AppLanguage.failText = false; NotificationCompat.failBuild = false; CallVideoEngine.failCreate = false
        power.failures = emptySet(); power.locks.forEach { it.failures = emptySet() }
        manager.failures = emptySet(); PendingIntent.created.forEach { it.failCancel = false; it.failCancelAfter = false }
        services.forEach {
            it.audio.failures = emptySet(); it.video?.failures = emptySet(); it.receiverFailures = emptySet()
            it.failForegroundStop = false; it.failStopSelf = false
            it.shutdown()
        }
        CallController.reset(); main.advanceBy(30_000); services.forEach { it.onDestroy() }
        await("call media cleanup must complete before resetting Main") { !CallController.blocksMedia() }
        dispatcher.scheduler.advanceUntilIdle(); Dispatchers.resetMain()
        // Очистка модели после отрицательного контроля старого кода, потерявшего владельцев.
        power.locks.filter { it.held }.forEach { it.release() }
        services.forEach { service -> service.receivers.keys.toList().forEach { service.unregisterReceiver(it) } }
        assertNull(state)
    }

    private fun diagnostic(fragment: String) = synchronized(LogManager.entries) {
        assertTrue("Missing diagnostic: $fragment", LogManager.entries.any { fragment in it.message && it.level in listOf("WARN", "ERROR") })
    }

    @Test fun pendingTelecomCleanupBlocksNewServiceAndOtherMediaUntilAcknowledged() {
        TelecomCallManager.pending = true
        assertTrue(CallController.blocksMedia())
        assertThrows(IllegalStateException::class.java) { CallService.start(context, 17) }
        assertTrue(context.serviceStarts.isEmpty())
        TelecomCallManager.pending = false; CallService.requireReleased(); assertFalse(CallController.blocksMedia())
    }
    private fun field(service: CallService, name: String) = CallService::class.java.getDeclaredField(name)
        .apply { isAccessible = true }.get(service)
    private fun create(): CallService = CallService().also {
        services.add(it); it.notificationManager = manager; it.powerManager = power
        it.onCreate()
    }
    private fun request(): Long {
        if (state?.ongoing != true) await("previous media owner must finish before starting another call") { !CallController.blocksMedia() }
        CallController.start("7", "01".repeat(32), "Alice")
        return requireNotNull(state).callId
    }
    private fun start(service: CallService, id: Long = request()) {
        assertEquals(android.app.Service.START_NOT_STICKY, service.onStartCommand(Intent().putExtra("call_id", id), 0, 1))
    }
    private fun await(message: String, condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!condition() && System.nanoTime() < deadline) { dispatcher.scheduler.runCurrent(); main.dispatchAll(); Thread.sleep(2) }
        assertTrue(message, condition())
    }
    private fun active(): CallService {
        val service = create(); val id = request(); start(service, id)
        await("service must allow signaling") { NativeLib.callCommands.any { it == "start" to id } }
        CallController.onEvent(36, ByteBuffer.allocate(41).order(ByteOrder.LITTLE_ENDIAN).putLong(id).put(0).put(ByteArray(32) { 1 }).array())
        await("ACCEPTED must finish media release before audio activation") { state?.phase == CallPhase.ACTIVE }
        assertTrue(service.audio.active)
        return service
    }

    @Test fun rejectedOutgoingSignallingEndsAndReleasesReservedService() {
        NativeLib.rejectedCallCommands = setOf("start")
        val service = create(); val id = request(); start(service, id)
        await("rejected call must end") { state?.phase == CallPhase.ENDED }
        assertFalse(power.locks.single().held); assertFalse(service.audio.active)
        assertFalse(CallController.blocksMedia()); diagnostic("start queue rejected")
    }

    @Test fun rejectedAcceptDeclinesTheIncomingCallAndReleasesService() {
        val id = 17L
        CallController.onEvent(34, ByteBuffer.allocate(42).order(ByteOrder.LITTLE_ENDIAN)
            .put(1.toByte()).put('7'.code.toByte()).put(MemberFilesProtocol.addressBytes("63".padStart(64, '0'))).putLong(id).array())
        assertEquals(CallPhase.INCOMING, state?.phase)
        NativeLib.rejectedCallCommands = setOf("accept")
        CallController.accept(); val service = create(); start(service, id)
        await("rejected answer must end") { state?.phase == CallPhase.ENDED }
        assertTrue(NativeLib.callCommands.contains("decline" to id)); assertFalse(power.locks.single().held)
        diagnostic("accept queue rejected")
    }

    @Test fun rejectedContinueKeepsCallerChoiceAvailableForRetry() {
        val service = create(); val id = request(); start(service, id)
        await("initial signaling queued") { NativeLib.callCommands.contains("start" to id) }
        CallController.onEvent(66, ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(id).array())
        assertEquals(CallPhase.WAIT_DECISION, state?.phase)
        NativeLib.rejectedCallCommands = setOf("continue"); CallController.continueCall(id)
        assertEquals(CallPhase.WAIT_DECISION, state?.phase); diagnostic("continue queue rejected")
        NativeLib.rejectedCallCommands = emptySet(); CallController.continueCall(id)
        assertEquals(CallPhase.OUTGOING, state?.phase)
    }

    @Test fun rejectedHangupReleasesAudioButWaitsForQueueAckBeforeRestoringRadio() {
        RadioAudioEngine.instance().activeGroup.value = "07".repeat(32)
        active(); val id = requireNotNull(state).callId
        NativeLib.rejectedCallCommands = setOf("hangup"); CallController.end(); dispatcher.scheduler.runCurrent()
        assertEquals(CallPhase.ENDED, state?.phase); assertFalse(power.locks.single().held)
        assertTrue(CallController.blocksMedia()); assertTrue(RadioAudioEngine.instance().restored.isEmpty())
        val starts = context.serviceStarts.size
        CallController.start("7", "02".repeat(32), "Other")
        assertEquals(starts, context.serviceStarts.size); assertEquals(id, state?.callId)
        dispatcher.scheduler.advanceTimeBy(250); dispatcher.scheduler.runCurrent()
        assertTrue(NativeLib.callCommands.count { it == "hangup" to id } >= 2)
        NativeLib.rejectedCallCommands = emptySet()
        dispatcher.scheduler.advanceTimeBy(500); dispatcher.scheduler.runCurrent()
        await("radio restored after end queue ACK") { RadioAudioEngine.instance().restored == listOf("07".repeat(32)) }
        assertFalse(CallController.blocksMedia()); diagnostic("hangup queue rejected")
    }

    @Test fun nativeEndEventCompletesPendingHangupWithoutAnotherCommand() {
        active(); val id = requireNotNull(state).callId
        NativeLib.rejectedCallCommands = setOf("hangup"); CallController.end()
        assertTrue(CallController.blocksMedia())
        CallController.onEvent(38, ByteBuffer.allocate(9).order(ByteOrder.LITTLE_ENDIAN).putLong(id).put(0.toByte()).array())
        dispatcher.scheduler.runCurrent(); assertFalse(CallController.blocksMedia())
        val attempts = NativeLib.callCommands.size
        dispatcher.scheduler.advanceTimeBy(250); dispatcher.scheduler.runCurrent()
        assertEquals(attempts, NativeLib.callCommands.size)
    }

    @Test fun resetCancelsPendingSignallingRetriesBeforeCoreStop() {
        active(); NativeLib.rejectedCallCommands = setOf("hangup"); CallController.end()
        assertTrue(CallController.blocksMedia()); CallController.reset(); dispatcher.scheduler.runCurrent()
        assertFalse(CallController.blocksMedia()); val attempts = NativeLib.callCommands.size
        dispatcher.scheduler.advanceTimeBy(30_000); dispatcher.scheduler.runCurrent()
        assertEquals(attempts, NativeLib.callCommands.size)
    }

    @Test fun failedRoutePreferenceWriteDoesNotEscapeTheControllerScope() {
        active(); val previous = context.configProvider.getCallAudioRoute()
        context.configProvider.failCallRouteSave = true
        CallController.setRoute(AudioRoute.SPEAKER); dispatcher.scheduler.runCurrent()
        assertEquals(previous, context.configProvider.getCallAudioRoute()); assertEquals(CallPhase.ACTIVE, state?.phase)
        diagnostic("Cannot save preferred route")
    }

    @Test fun lateRouteInputAfterEndCannotChangeNextCallPreference() {
        active(); CallController.end(); dispatcher.scheduler.runCurrent()
        val previous = context.configProvider.getCallAudioRoute()
        CallController.setRoute(AudioRoute.SPEAKER); dispatcher.scheduler.runCurrent()
        assertEquals(previous, context.configProvider.getCallAudioRoute()); diagnostic("route ignored outside active call")
    }

    @Test fun failedBusyOutcomeTranslationDoesNotSkipCallCleanupOrStateExpiry() {
        active(); AppLanguage.failText = true
        CallController.end(reason = 8); dispatcher.scheduler.runCurrent()
        assertEquals(CallPhase.ENDED, state?.phase); assertFalse(power.locks.single().held)
        diagnostic("Busy outcome notification failed")
        dispatcher.scheduler.advanceTimeBy(2500); dispatcher.scheduler.runCurrent(); assertNull(state)
    }

    @Test fun normalStartupOwnsOneLockReceiverAndForegroundBeforeAudio() {
        val service = create(); val id = request(); start(service, id)
        assertTrue(power.locks.single().held); assertFalse(power.locks.single().counted)
        assertEquals(1, service.receivers.size); assertEquals(1002, service.foregroundId)
        assertFalse(service.audio.active); assertTrue(manager.active.getValue(1002).ongoing)
        start(service, id); assertEquals(1, power.locks.size); assertEquals(1, service.foregroundAttempts)
    }

    @Test fun pendingVideoCleanupKeepsTheServiceAndItsCompletionOwnedWithoutAnError() {
        val service = active(); val video = requireNotNull(service.video)
        video.failures = setOf("pending")
        val released = service.shutdown()
        assertFalse(released.isCompleted); assertSame(video, service.video); assertTrue(CallService.cleanupPending())
        assertFalse(service.audio.active); assertFalse(power.locks.single().held)
        assertTrue(LogManager.entries.none { it.level == "ERROR" && "video stop failed" in it.message })
        video.failures = emptySet(); service.shutdown()
        assertTrue(released.isCompleted); assertNull(service.video); assertFalse(CallService.cleanupPending())
    }

    @Test fun staleStartCompletesForegroundHandshakeWithoutTakingTheMicrophoneOrLock() {
        val service = create(); start(service, 333)
        assertEquals(1, service.foregroundAttempts); assertEquals(4, service.foregroundTypes)
        assertTrue(power.locks.isEmpty()); assertTrue(service.stops > 0); assertFalse(manager.active.containsKey(1002))
    }

    @Test fun zeroAndOldEndIntentsCannotEndAnOwnedCall() {
        val service = active(); val id = requireNotNull(state).callId
        for (stale in listOf(0, id + 1)) {
            service.onStartCommand(Intent().setAction("com.utun.chat.action.END_CALL").putExtra("call_id", stale), 0, 2)
            assertEquals(CallPhase.ACTIVE, state?.phase)
        }
        diagnostic("stale END")
    }

    @Test fun failedChannelCreationDoesNotEscapeOnCreateAndCanRecoverAtStart() {
        manager.failures = setOf("channel"); val service = create(); diagnostic("channel")
        manager.failures = emptySet(); start(service)
        assertTrue(power.locks.single().held); assertEquals(2, manager.channelAttempts)
    }

    @Test fun persistentChannelFailureFinishesTheRequestAndStillStopsTheService() {
        manager.failures = setOf("channel"); val service = create(); start(service)
        assertEquals(CallPhase.ENDED, state?.phase); assertTrue(service.stops > 0); assertTrue(power.locks.isEmpty())
    }

    @Test fun diagnosticChannelQueryFailureCannotRejectAnOtherwiseValidForegroundStart() {
        val service = create(); manager.failures = setOf("channel query")
        service.refreshLanguage(); start(service)
        assertTrue(power.locks.single().held); diagnostic("channel diagnostics")
    }

    @Test fun failedRegistrationAfterItsEffectIsRolledBack() {
        val service = create(); service.receiverFailures = setOf("register after"); start(service)
        assertTrue(service.receivers.isEmpty()); assertTrue(power.locks.all { !it.held })
        assertEquals(CallPhase.ENDED, state?.phase); diagnostic("start failed")
    }

    @Test fun failedAcquireAfterItsEffectIsRolledBack() {
        val service = create(); power.failures = setOf("acquire"); start(service)
        assertFalse(power.locks.single().held); assertEquals(CallPhase.ENDED, state?.phase)
        assertTrue(service.stops > 0)
    }

    @Test fun wakeLockReleaseFailureRetainsItsOwnerAndOtherCleanupStillRuns() {
        val service = active(); val lock = power.locks.single(); lock.failures = setOf("release")
        runCatching { service.shutdown() }
        assertSame(lock, field(service, "wakeLock")); assertFalse(service.audio.active); assertTrue(service.receivers.isEmpty())
        lock.failures = emptySet(); service.shutdown(); assertFalse(lock.held); assertNull(field(service, "wakeLock"))
    }

    @Test fun receiverRemovalFailureRetainsRegistrationAndRetriesIt() {
        val service = active(); service.receiverFailures = setOf("unregister")
        runCatching { service.shutdown() }
        assertEquals(true, field(service, "powerEventsRegistered")); assertFalse(service.audio.active)
        assertFalse(power.locks.single().held)
        service.receiverFailures = emptySet(); service.shutdown(); assertTrue(service.receivers.isEmpty())
        assertEquals(false, field(service, "powerEventsRegistered"))
    }

    @Test fun aVideoStopFailureDoesNotSkipAudioReceiverOrWakeLockCleanup() {
        val service = active(); val video = requireNotNull(service.video); video.failures = setOf("stop")
        runCatching { service.shutdown() }
        assertSame(video, service.video); assertFalse(service.audio.active); assertFalse(power.locks.single().held)
        assertTrue(service.receivers.isEmpty()); video.failures = emptySet(); service.shutdown(); assertNull(service.video)
    }

    @Test fun audioStopFailureKeepsItsServiceIdForRetry() {
        val service = active(); val id = requireNotNull(state).callId; service.audio.failures = setOf("stop")
        runCatching { service.shutdown() }
        assertEquals(id, field(service, "callId")); assertFalse(power.locks.single().held); assertTrue(service.receivers.isEmpty())
        service.audio.failures = emptySet(); service.shutdown(); assertFalse(service.audio.active)
    }

    @Test fun partialAudioStartFailureIsLoggedAndRolledBackBeforeReturningFalse() {
        val service = create(); val id = request(); start(service, id)
        service.audio.failures = setOf("start")
        assertFalse(service.startAudio(id)); assertFalse(service.audio.active); assertTrue(power.locks.all { !it.held })
        diagnostic("audio start failed")
    }

    @Test fun videoConstructorFailureRollsBackTheStartedAudio() {
        val service = create(); val id = request(); start(service, id); CallVideoEngine.failCreate = true
        assertFalse(service.startAudio(id)); assertFalse(service.audio.active); assertTrue(service.stops > 0)
        diagnostic("audio start failed")
    }

    @Test fun repeatedAudioStartDoesNotReplaceAnOwnedVideoEngine() {
        val service = active(); val video = service.video; val id = requireNotNull(state).callId
        assertTrue(service.startAudio(id)); assertSame(video, service.video); assertEquals(1, service.audio.starts)
    }

    @Test fun stoppingScreenFromAndroidKeepsTheCallAndMicrophoneActive() {
        val service = active()
        val engine = requireNotNull(service.video)
        CallController.setVideo(CallVideoSettings(enabled = true, screen = true), Intent())
        val projection = requireNotNull(engine.projection)
        assertEquals(0, service.foregroundTypes and android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
        assertTrue(service.foregroundTypes and android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION != 0)
        projection.stopped()
        assertTrue(projection.closed)
        assertFalse(engine.state.value.settings.enabled)
        assertTrue(service.audio.active)
        assertEquals(CallPhase.ACTIVE, state?.phase)
    }

    @Test fun failedCameraPromotionRestoresTheLastConfirmedFlag() {
        val service = active(); assertTrue(service.setVideoCapture(true))
        service.failForeground = true; assertFalse(service.setVideoCapture(false))
        assertEquals(true, field(service, "cameraForeground")); diagnostic("camera foreground failed")
    }

    @Test fun notificationUpdateErrorsDoNotEscapeOrResurrectAStoppedService() {
        val service = active(); manager.failures = setOf("notify")
        service.updateNotification(); diagnostic("notification update failed")
        manager.failures = emptySet(); service.shutdown(); val posted = manager.posted.size
        service.updateNotification(); assertEquals(posted, manager.posted.size)
    }

    @Test fun startupFailureCannotSkipCleanupWhenTheControllerCallbackFails() {
        val service = create(); val id = request()
        // Сбой CoreActivity в finish: сервис не должен зависеть от успешности observer контроллера.
        NativeLib.failActivityRequest = true; service.failForeground = true
        service.onStartCommand(Intent().putExtra("call_id", id), 0, 1)
        assertTrue(service.stops > 0); assertTrue(power.locks.isEmpty()); NativeLib.failActivityRequest = false
    }

    @Test fun notificationActionsAreOwnedTokensAndCannotBeReusedByAnotherCall() {
        val service = active(); val old = requireNotNull(manager.active.getValue(1002).hangup)
        CallController.end(); assertTrue(old.cancelled)
        service.onDestroy(); val next = active(); val current = requireNotNull(manager.active.getValue(1002).hangup)
        assertNotEquals(old, current); assertTrue(current.send().getLongExtra("call_id", 0) != 0L)
        assertTrue(next.audio.active)
    }

    @Test fun publicMutationsRejectWorkerThreadsBeforeSideEffects() {
        val service = create(); val error = AtomicReference<Throwable?>()
        Thread { try { service.setVideoCapture(true) } catch (e: Throwable) { error.set(e) } }.apply { start(); join() }
        assertTrue(error.get() is IllegalStateException); assertEquals(0, service.foregroundAttempts)
        diagnostic("requires main thread")
    }

    @Test fun radioRestorationAndOtherMediaWaitForConfirmedAudioCleanup() {
        val radio = RadioAudioEngine.instance(); radio.activeGroup.value = "07".repeat(32)
        val service = active(); service.audio.failures = setOf("stop")
        CallController.end(); dispatcher.scheduler.runCurrent()
        assertEquals(CallPhase.ENDED, state?.phase); assertTrue(CallController.blocksMedia())
        val completion = requireNotNull(CallService.pendingRelease)
        assertFalse(completion.isCompleted); assertTrue(radio.restored.isEmpty()); assertTrue(manager.active.containsKey(1002))
        assertTrue(runCatching { CallService.requireReleased() }.isFailure)
        service.audio.failures = emptySet(); main.advanceBy(1000); dispatcher.scheduler.runCurrent()
        assertTrue(completion.isCompleted); assertFalse(CallService.cleanupPending())
        await("radio restored after cleanup") { radio.restored == listOf("07".repeat(32)) }
        assertFalse(CallController.blocksMedia())
    }

    @Test fun radioRestorationAlsoWaitsForTheTelecomConnectionCleanup() {
        val radio = RadioAudioEngine.instance(); radio.activeGroup.value = "07".repeat(32)
        active(); TelecomCallManager.pending = true; CallController.end(); dispatcher.scheduler.runCurrent()
        assertEquals(CallPhase.ENDED, state?.phase); assertFalse(CallService.cleanupPending())
        assertTrue(CallController.blocksMedia()); assertTrue(radio.restored.isEmpty())
        TelecomCallManager.pending = false
        await("radio restored after Telecom cleanup") { radio.restored == listOf("07".repeat(32)) }
        assertFalse(CallController.blocksMedia())
    }

    @Test fun unexpectedDestroyStillRetainsFailedCleanupAndEndsTheControllerSession() {
        val service = active(); service.audio.failures = setOf("stop")
        service.onDestroy()
        assertEquals(1, service.destroys); assertEquals(CallPhase.ENDED, state?.phase)
        assertTrue(CallService.cleanupPending()); assertTrue(CallController.blocksMedia())
        service.audio.failures = emptySet(); main.advanceBy(1000)
        assertFalse(service.audio.active); assertFalse(CallService.cleanupPending())
    }

    @Test fun rejectedRetryQueueRetainsAnExplicitCleanupPathAndBlocksCoreShutdown() {
        val service = active(); service.audio.failures = setOf("stop"); Handler.rejectDelayed = true
        val completion = service.shutdown()
        assertFalse(completion.isCompleted); diagnostic("cleanup retry rejected")
        assertTrue(runCatching { CallService.requireReleased() }.isFailure)
        service.audio.failures = emptySet(); CallService.requireReleased()
        assertTrue(completion.isCompleted); assertFalse(CallService.cleanupPending())
    }

    @Test fun aCancelledCompletionCannotBeMistakenForSuccessfulResourceCleanup() {
        val service = active(); service.audio.failures = setOf("stop")
        val completion = service.shutdown(); completion.cancel()
        assertTrue(completion.isCompleted)
        assertTrue(runCatching { CallService.requireReleased() }.isFailure)
        assertTrue(CallService.cleanupPending()); assertTrue(service.audio.active)
    }

    @Test fun foregroundAndStopSelfFailuresDoNotRepeatReleasedMedia() {
        for (foreground in listOf(true, false)) {
            val service = active()
            service.failForegroundStop = foreground; service.failStopSelf = !foreground
            val completion = service.shutdown(); assertFalse(completion.isCompleted)
            assertFalse(service.audio.active); assertFalse(power.locks.last().held); assertTrue(service.receivers.isEmpty())
            assertEquals(1, service.audio.stops)
            service.failForegroundStop = false; service.failStopSelf = false; service.shutdown()
            assertTrue(completion.isCompleted); assertEquals(1, service.audio.stops)
            CallController.end(); service.onDestroy()
        }
    }

    @Test fun actionCancellationFailuresRetainTheTokensUntilAcknowledged() {
        val service = active(); val token = requireNotNull(manager.active.getValue(1002).hangup)
        token.failCancel = true
        val completion = service.shutdown()
        assertFalse(completion.isCompleted); assertSame(token, field(service, "endIntent")); assertTrue(CallService.cleanupPending())
        token.failCancel = false; main.advanceBy(1000)
        assertTrue(completion.isCompleted); assertTrue(token.cancelled); assertNull(field(service, "endIntent"))
    }

    @Test fun failedReleaseAfterItsEffectConfirmsAbsenceOnTheNextAttempt() {
        val service = active(); val lock = power.locks.single()
        lock.failures = setOf("release_after"); service.receiverFailures = setOf("unregister after")
        val completion = service.shutdown()
        assertFalse(lock.held); assertTrue(service.receivers.isEmpty()); assertFalse(completion.isCompleted)
        lock.failures = emptySet(); service.receiverFailures = emptySet(); main.advanceBy(1000)
        assertTrue(completion.isCompleted); assertEquals(1, lock.releaseCalls)
        assertNull(field(service, "wakeLock")); assertEquals(false, field(service, "powerEventsRegistered"))
    }

    @Test fun staleAudioFailureCannotEndANewCallOnAReusedServiceInstance() {
        val service = active(); val oldId = requireNotNull(state).callId; val callback = requireNotNull(service.audio.onFailure)
        CallController.end(); val nextId = request(); start(service, nextId)
        await("new signaling") { NativeLib.callCommands.any { it == "start" to nextId } }
        callback(oldId, "old worker failed")
        assertEquals(nextId, state?.callId); assertTrue(state?.ongoing == true)
        assertTrue(power.locks.last().held)
    }

    @Test fun notificationUpdatesReuseOwnedTokensAndKeepTheConversationClock() {
        val service = active(); val original = manager.active.getValue(1002)
        assertTrue(original.chronometer); assertTrue(original.showWhen); assertEquals("Alice", original.title)
        assertEquals("01".repeat(32), original.person?.key); assertEquals(2, PendingIntent.created.size)
        AppLanguage.language = "ru"; service.refreshLanguage()
        val updated = manager.active.getValue(1002)
        assertSame(original.hangup, updated.hangup); assertSame(original.contentIntent, updated.contentIntent)
        assertTrue(kotlin.math.abs(original.whenMs - updated.whenMs) < 50)
        assertEquals(2, PendingIntent.created.size)
    }

    @Test fun powerDiagnosticFailuresAreLoggedAndDoNotLoseTheOwnedLock() {
        val service = active(); val receiver = service.receivers.keys.single(); val lock = power.locks.single()
        lock.failures = setOf("query")
        receiver.onReceive(service, Intent(Intent.ACTION_SCREEN_OFF))
        assertSame(lock, field(service, "wakeLock")); assertTrue(lock.held); diagnostic("power diagnostics failed")
    }

    @Test fun aMissingPowerServiceRollsBackForegroundAndFinishesStartup() {
        val service = create(); service.powerManager = null; start(service)
        assertEquals(CallPhase.ENDED, state?.phase); assertTrue(service.stops > 0)
        assertFalse(manager.active.containsKey(1002)); diagnostic("start failed operation=wake lock")
    }

    @Test fun aCancelledCallStillBlocksCoreTeardownUntilMediaPreparationLeavesNative() {
        val entered = CountDownLatch(1); val gate = CountDownLatch(1).also { gates.add(it) }
        MediaAudioController.beforeIdle = { entered.countDown(); check(gate.await(5, TimeUnit.SECONDS)) }
        val service = create(); val id = request(); start(service, id)
        await("signaling starts before accepted media transition") { NativeLib.callCommands.contains("start" to id) }
        CallController.onEvent(36, ByteBuffer.allocate(41).order(ByteOrder.LITTLE_ENDIAN).putLong(id).put(0).put(ByteArray(32) { 1 }).array())
        dispatcher.scheduler.runCurrent(); assertTrue(entered.await(5, TimeUnit.SECONDS))
        CallController.reset()
        assertNull(state); assertFalse(CallService.cleanupPending()); assertTrue(CallController.blocksMedia())
        gate.countDown(); await("cancelled media preparation finished") { !CallController.blocksMedia() }
    }

    @Test fun destroyAfterLocalShutdownStillReportsTheOriginalOwnedCallToTheController() {
        val service = active(); val id = requireNotNull(state).callId
        assertTrue(service.shutdown().isCompleted)
        assertEquals(CallPhase.ACTIVE, state?.phase) // Сигналинг ещё принадлежит контроллеру, не аудиосервису.
        service.onDestroy()
        assertEquals(id, state?.callId); assertEquals(CallPhase.ENDED, state?.phase)
    }

    @Test fun aMissingServiceComponentCannotLeaveTheControllerWaitingForOnCreate() {
        context.acceptServiceStart = false
        request()
        assertEquals(CallPhase.ENDED, state?.phase); assertEquals(7, state?.reason)
        diagnostic("service request")
    }
    @Test fun voiceCleanupBlocksAudioAfterAcceptedAndRemainsABarrierAfterCallCancellation() {
        val voice = java.util.concurrent.CompletableFuture<Unit>()
        AudioRecorderManager.stopped = voice
        val service = create(); val id = request(); start(service, id)
        await("voice must continue during signaling") { NativeLib.callCommands.contains("start" to id) }
        CallController.onEvent(36, ByteBuffer.allocate(41).order(ByteOrder.LITTLE_ENDIAN).putLong(id).put(0).put(ByteArray(32) { 1 }).array())
        dispatcher.scheduler.runCurrent()
        assertTrue(state?.preparing == true)
        assertFalse(service.audio.active)
        CallController.reset()
        assertTrue(CallController.blocksMedia()); assertFalse(voice.isCancelled)
        voice.complete(Unit)
        await("voice release after cancelled call") { !CallController.blocksMedia() }
        assertFalse(service.audio.active)
    }
}
