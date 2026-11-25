package com.utun.chat.data

import android.app.Notification
import android.content.Context
import android.graphics.drawable.GradientDrawable
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.MotionEvent
import android.view.View
import androidx.core.app.NotificationCompat
import com.utun.chat.ChatApplication
import com.utun.chat.R
import java.util.concurrent.ExecutorService
import java.util.concurrent.TimeUnit
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

/** Настоящие service/PTT/PCM/session; Android service/window IPC и его частичные отказы моделируются без устройства. */
@OptIn(ExperimentalCoroutinesApi::class)
class RadioOverlayLifecycleTest {
    private lateinit var context: Context
    private lateinit var radio: RadioAudioEngine
    private lateinit var io: ExecutorService
    private val dispatcher = StandardTestDispatcher()
    private val services = mutableListOf<RadioOverlayService>()
    private var previousEngine: Any? = null
    private val wm get() = requireNotNull(context.windowService)
    private val view get() = wm.windows.keys.single()
    private val main get() = Looper.getMainLooper()

    private fun static(name: String) = RadioOverlayService::class.java.getDeclaredField(name).apply { isAccessible = true }
    private fun engineField(name: String) = RadioAudioEngine::class.java.getDeclaredField(name).apply { isAccessible = true }

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper()); Dispatchers.setMain(dispatcher)
        context = Context().also { ChatApplication.instance = it }; context.configProvider.radioOverlay = true
        context.configProvider.keepMicrophoneReady = false
        AudioManager.shared = AudioManager(); AudioTrack.nextFailures = emptySet(); AudioTrack.minimumBuffer = 1920
        NativeLib.radioFailures = emptySet(); NativeLib.radioPullResult = 0; NativeLib.onRadioFeed = {}; NativeLib.onRadioCaptureStart = {}
        NativeLib.radioCommands.clear()
        CallController.blocked = false; MicrophonePermission.requester = null; AndroidMicrophoneCapture.reset()
        Settings.overlayAllowed = true; Settings.failOverlayQuery = false
        AppLanguage.failContext = false; AppLanguage.failText = false; NotificationCompat.failBuild = false
        Handler.rejectImmediate = false; Handler.rejectDelayed = false
        for (name in listOf("running", "starting", "pendingStop", "restartAfterStop", "activityVisible", "waitingOverlayPermission")) {
            static(name).setBoolean(null, false)
        }
        static("currentService").set(null, null); static("defaultCenter").set(null, null)
        RadioOverlayService.overlayPermissionRequester = null
        previousEngine = engineField("inst").get(null)
        radio = RadioAudioEngine(); engineField("inst").set(null, radio)
        io = engineField("io").get(radio) as ExecutorService
        radio.start("07".repeat(32)); drain(); main.dispatchAll(); radio.onSettingsApplied(NativeLib.radioRequest, true); drain()
        LogManager.entries.clear(); LogManager.console = true
    }

    @After fun release() {
        wm.failures = emptySet(); context.failServiceStop = false; context.acceptServiceStop = true
        context.notificationManager?.failures = emptySet(); AppLanguage.failContext = false; AppLanguage.failText = false
        Settings.failOverlayQuery = false; Settings.overlayAllowed = true
        RadioOverlayService.overlayPermissionRequester = null
        context.configProvider.radioOverlay = false; context.configProvider.radioSettings = RadioAutoSettings()
        RadioOverlayService.stop(context)
        if (static("starting").getBoolean(null)) create() // Доставить onCreate отменённого, уже принятого ОС запроса.
        services.forEach { it.onDestroy() }
        radio.stop().get(5, TimeUnit.SECONDS); main.advanceBy(10_000); drain()
        io.shutdown(); assertTrue(io.awaitTermination(5, TimeUnit.SECONDS))
        engineField("inst").set(null, previousEngine)
        dispatcher.scheduler.advanceUntilIdle(); Dispatchers.resetMain()
        assertTrue(wm.windows.isEmpty()); assertFalse(RadioOverlayService.isShown.value)
    }

    private fun drain() { io.submit {}.get(5, TimeUnit.SECONDS) }
    private fun create(): RadioOverlayService = RadioOverlayService().also {
        services.add(it); it.notificationManager = context.notificationManager; it.windowService = wm
        it.permissionGranted = context.permissionGranted
        it.onCreate(); dispatcher.scheduler.runCurrent()
        if (it.foregroundId != 0 && it.stops == 0) { drain(); radio.onSettingsApplied(NativeLib.radioRequest, true); drain() }
    }
    private fun show(): RadioOverlayService = create().also { it.onStartCommand(null, 0, 1); dispatcher.scheduler.runCurrent() }
    private fun diagnostic(fragment: String) = synchronized(LogManager.entries) {
        assertTrue("Missing diagnostic: $fragment", LogManager.entries.any { fragment in it.message && it.level in listOf("ERROR", "WARN") })
    }
    private fun touch(action: Int, time: Long = 100, down: Long = time, x: Float = 0f, target: View = view): Boolean {
        val handled = target.touch(MotionEvent(action, rawX = x, eventTime = time, downTime = down))
        drain()
        return handled
    }

    @Test fun buttonWaitsInYellowUntilActualCaptureAndReturnsToIdleAfterCancel() {
        show()
        val background = view.background as GradientDrawable
        assertEquals(RadioOverlayService.COLOR_IDLE, background.color)
        assertTrue(background.strokeWidth > 0); assertEquals(RadioOverlayService.COLOR_WAITING, background.strokeColor)
        touch(MotionEvent.ACTION_DOWN); dispatcher.scheduler.runCurrent()
        assertEquals(RadioOverlayService.COLOR_WAITING, background.color)
        val openedBy = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (AndroidMicrophoneCapture.opened.isEmpty() && System.nanoTime() < openedBy) Thread.sleep(2)
        assertTrue(AndroidMicrophoneCapture.opened.isNotEmpty())
        val input = AndroidMicrophoneCapture.opened.last()
        val time = System.nanoTime()
        repeat(20) { index ->
            assertTrue(input.accept(MicrophoneCapture.Frame(ShortArray(1920), time + index * 20_000_000L,
                index * 960L, generation = input.generation)))
        }
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!radio.captureReady.value && System.nanoTime() < deadline) Thread.sleep(2)
        assertTrue(radio.captureReady.value)
        dispatcher.scheduler.runCurrent()
        assertEquals(RadioOverlayService.COLOR_TALKING, background.color)
        assertEquals(0, background.strokeWidth)
        touch(MotionEvent.ACTION_CANCEL); dispatcher.scheduler.runCurrent()
        assertEquals(RadioOverlayService.COLOR_IDLE, background.color)
        assertFalse(radio.captureReady.value)
        assertTrue(background.strokeWidth > 0); assertEquals(RadioOverlayService.COLOR_WAITING, background.strokeColor)
    }

    @Test fun foregroundNotificationNeverReplacesOrRemovesThePhoneServiceNotification() {
        val manager = requireNotNull(context.notificationManager); val phone = Notification()
        manager.active[1002] = phone
        val service = show()
        assertNotEquals(1002, service.foregroundId); assertSame(phone, manager.active[1002])
        service.onDestroy(); assertSame(phone, manager.active[1002])
    }

    @Test fun standbyKeepsMicrophoneServiceWithoutOverlayAndReleasesItWhenDisabled() {
        context.configProvider.radioOverlay = false; context.configProvider.keepMicrophoneReady = true
        Settings.overlayAllowed = false
        radio.refreshMicrophonePolicy(); drain()
        RadioOverlayService.setActivityVisible(true)
        assertEquals(1, context.serviceStarts.size)
        val service = create()
        assertTrue(NativeLib.radioCaptureAlive); assertTrue(wm.windows.isEmpty())
        assertEquals(1, NativeLib.radioCommands.count { it.kind == "capture start" })
        assertEquals(0, service.stops)
        context.configProvider.keepMicrophoneReady = false; radio.refreshMicrophonePolicy(); drain()
        main.dispatchAll(); dispatcher.scheduler.runCurrent()
        assertFalse(NativeLib.radioCaptureAlive)
        assertTrue(context.serviceStops.isNotEmpty())
    }

    @Test fun losingOverlayPermissionDoesNotStopIndependentStandbyCapture() {
        val service = show()
        context.configProvider.keepMicrophoneReady = true; radio.refreshMicrophonePolicy(); drain()
        Settings.overlayAllowed = false
        service.onStartCommand(null, 0, 2); drain()
        assertTrue(wm.windows.isEmpty()); assertTrue(NativeLib.radioCaptureAlive)
        assertTrue(context.serviceStops.isEmpty()); assertEquals(0, service.stops)
    }

    @Test fun startupChannelAndForegroundFailuresAreLoggedAndRequestStop() {
        for (failure in listOf("channel", "foreground", "foreground after", "window")) {
            val service = RadioOverlayService().also { services.add(it); it.notificationManager = context.notificationManager; it.windowService = wm }
            when (failure) {
                "channel" -> requireNotNull(context.notificationManager).failures = setOf("channel")
                "foreground" -> service.failForeground = true
                "foreground after" -> service.failForegroundAfter = true
                "window" -> service.windowService = null
            }
            service.onCreate(); assertTrue(service.stops > 0); diagnostic("service setup failed")
            requireNotNull(context.notificationManager).failures = emptySet(); service.onDestroy()
        }
    }

    @Test fun cancelledStartupCompletesForegroundHandshakeBeforeStopping() {
        RadioOverlayService.setActivityVisible(true); assertEquals(1, context.serviceStarts.size)
        RadioOverlayService.stop(context)
        val service = create()
        assertEquals(1, service.foregroundAttempts); assertTrue(service.stops > 0)
        service.onDestroy(); assertFalse(static("starting").getBoolean(null)); assertFalse(static("running").getBoolean(null))
    }

    @Test fun backgroundStartIsDeferredUntilAnActivityIsVisible() {
        RadioOverlayService.sync(context); assertTrue(context.serviceStarts.isEmpty())
        assertEquals(R.string.overlay_open_app, RadioOverlayService.status.value)
        RadioOverlayService.setActivityVisible(true); assertEquals(1, context.serviceStarts.size)
        RadioOverlayService.sync(context); assertEquals(1, context.serviceStarts.size)
    }

    @Test fun failedServiceRequestCanBeRetried() {
        context.failServiceStart = true; RadioOverlayService.setActivityVisible(true)
        assertFalse(static("starting").getBoolean(null)); diagnostic("Service start failed")
        context.failServiceStart = false; RadioOverlayService.sync(context)
        assertEquals(2, context.serviceStarts.size)
    }

    @Test fun stopRejectionAndExceptionDoNotLatchPendingStopAndCanBeRetried() {
        show(); context.acceptServiceStop = false
        RadioOverlayService.stop(context); assertFalse(static("pendingStop").getBoolean(null)); diagnostic("Service stop")
        context.failServiceStop = true; RadioOverlayService.stop(context)
        assertFalse(static("pendingStop").getBoolean(null))
        context.failServiceStop = false; context.acceptServiceStop = true; RadioOverlayService.stop(context)
        assertEquals(3, context.serviceStops.size); assertTrue(static("pendingStop").getBoolean(null))
    }

    @Test fun addFailureAfterItsEffectIsRolledBackWithoutAnOrphanWindow() {
        wm.failures = setOf("add after"); show()
        assertTrue(wm.windows.isEmpty()); assertFalse(RadioOverlayService.isShown.value); diagnostic("Overlay creation failed")
    }

    @Test fun addFailureBeforeItsEffectTreatsMissingWindowAsConfirmedCleanup() {
        wm.failures = setOf("add"); show()
        assertTrue(wm.windows.isEmpty()); assertFalse(RadioOverlayService.isShown.value)
        diagnostic("Overlay creation failed")
    }

    @Test fun aFailedRemovalRetainsItsWindowForTheNextSync() {
        val service = show(); val old = view; touch(MotionEvent.ACTION_DOWN); assertTrue(radio.isTalking())
        wm.failures = setOf("remove"); context.configProvider.radioOverlay = false
        context.configProvider.radioSettings = RadioAutoSettings(enabled = true)
        service.onStartCommand(null, 0, 2)
        assertFalse(radio.isTalking()); assertTrue(wm.windows.containsKey(old)); assertTrue(RadioOverlayService.isShown.value)
        wm.failures = emptySet(); service.onStartCommand(null, 0, 3)
        assertTrue(wm.windows.isEmpty()); assertFalse(RadioOverlayService.isShown.value)
    }

    @Test fun aRemovalFailureAfterItsEffectCanConfirmAbsenceOnRetry() {
        val service = show(); wm.failures = setOf("remove after")
        context.configProvider.radioOverlay = false; context.configProvider.radioSettings = RadioAutoSettings(enabled = true)
        service.onStartCommand(null, 0, 2); assertTrue(wm.windows.isEmpty())
        wm.failures = emptySet(); service.onStartCommand(null, 0, 3)
        assertFalse(RadioOverlayService.isShown.value); diagnostic("Overlay removal failed")
    }

    @Test fun anOldWindowsTouchesCannotCancelOrHoldTheReplacementPtt() {
        val service = show(); val old = view
        context.configProvider.radioOverlay = false; context.configProvider.radioSettings = RadioAutoSettings(enabled = true)
        service.onStartCommand(null, 0, 2)
        context.configProvider.radioOverlay = true; service.onStartCommand(null, 0, 3)
        touch(MotionEvent.ACTION_DOWN); assertTrue(radio.isTalking())
        touch(MotionEvent.ACTION_CANCEL, target = old); assertTrue(radio.isTalking())
        radio.cancelTalk(); touch(MotionEvent.ACTION_DOWN, target = old); assertFalse(radio.isTalking())
    }

    @Test fun failedDragLayoutCancelsItsPttImmediatelyAndDoesNotEscapeTheListener() {
        val service = show(); touch(MotionEvent.ACTION_DOWN); wm.failures = setOf("update")
        touch(MotionEvent.ACTION_MOVE, time = 150, down = 100, x = 100f)
        assertFalse(radio.isTalking()); assertTrue(service.stops > 0); diagnostic("Overlay touch failed")
    }

    @Test fun cancelClearsLatchedTransmissionImmediately() {
        show(); touch(MotionEvent.ACTION_DOWN); touch(MotionEvent.ACTION_UP, time = 130, down = 100)
        touch(MotionEvent.ACTION_DOWN, time = 180); touch(MotionEvent.ACTION_UP, time = 210, down = 180)
        main.advanceBy(500); assertTrue(radio.isTalking())
        touch(MotionEvent.ACTION_CANCEL, time = 220); assertFalse(radio.isTalking())
    }

    @Test fun screenOffCancelsLatchedPttAndNextPressStartsANewTransmission() {
        show(); touch(MotionEvent.ACTION_DOWN); touch(MotionEvent.ACTION_UP, time = 130, down = 100)
        touch(MotionEvent.ACTION_DOWN, time = 180); touch(MotionEvent.ACTION_UP, time = 210, down = 180)
        main.advanceBy(500); assertTrue(radio.isTalking())
        context.receivers.keys.single().onReceive(context, android.content.Intent(android.content.Intent.ACTION_SCREEN_OFF))
        drain(); dispatcher.scheduler.runCurrent()
        assertFalse(radio.isTalking()); assertFalse(NativeLib.radioCaptureAlive)
        context.receivers.keys.single().onReceive(context, android.content.Intent(android.content.Intent.ACTION_SCREEN_ON))
        drain(); assertFalse(radio.isTalking())
        touch(MotionEvent.ACTION_DOWN, time = 800)
        assertTrue(radio.isTalking())
    }

    @Test fun normalReleaseKeepsItsTailAndRemovingOverlayPreservesOtherPttOwners() {
        val service = show(); touch(MotionEvent.ACTION_DOWN); touch(MotionEvent.ACTION_UP, time = 130, down = 100)
        assertTrue(radio.isTalking()); main.advanceBy(251); assertFalse(radio.isTalking())
        assertTrue(radio.talkBegin(RadioPttSource.BUTTON)); touch(MotionEvent.ACTION_DOWN)
        service.onDestroy(); assertTrue(radio.isTalking()); radio.cancelTalk()
    }

    @Test fun stalePositionCallbackCannotResetAReplacementWindowsPendingPosition() {
        val service = show(); val old = view
        context.configProvider.radioOverlay = false; context.configProvider.radioSettings = RadioAutoSettings(enabled = true)
        service.onStartCommand(null, 0, 2); context.configProvider.radioOverlay = true; service.onStartCommand(null, 0, 3)
        val replacement = view; old.dispatchAnimations()
        RadioOverlayService.setDefaultCenter(400, 500)
        assertEquals(1, replacement.animations.size)
        replacement.dispatchAnimations(); assertEquals(367, wm.windows.getValue(replacement).x)
    }

    @Test fun aDestroyedOwnerRetainsFailedWindowCleanupAndBlocksReplacementUntilRetry() {
        val old = show(); wm.failures = setOf("remove"); old.onDestroy()
        assertNotNull(static("currentService").get(null)); assertTrue(RadioOverlayService.isShown.value)
        RadioOverlayService.setActivityVisible(true); assertTrue(context.serviceStarts.isEmpty())
        wm.failures = emptySet(); RadioOverlayService.sync(context)
        assertTrue(wm.windows.isEmpty()); assertEquals(1, context.serviceStarts.size)
    }

    @Test fun aLateDestroyFromAnOldInstanceCannotStopTheNewOwnerOrItsPtt() {
        val old = show(); old.onDestroy(); val next = show()
        touch(MotionEvent.ACTION_DOWN); old.onDestroy()
        assertTrue(radio.isTalking()); assertTrue(static("running").getBoolean(null))
        assertSame(next, static("currentService").get(null)); assertTrue(RadioOverlayService.isShown.value)
    }

    @Test fun permissionRequesterFailureDoesNotLatchThePendingRequest() {
        Settings.overlayAllowed = false; var requests = 0
        RadioOverlayService.overlayPermissionRequester = { requests++; error("settings launcher unavailable") }
        RadioOverlayService.requestPermissions(); RadioOverlayService.requestPermissions()
        assertEquals(2, requests); assertFalse(static("waitingOverlayPermission").getBoolean(null))
        diagnostic("Overlay permission request failed")
    }

    @Test fun languageRefreshFailureDoesNotEscapeOrReplaceTheCurrentWindow() {
        show(); val old = view; requireNotNull(context.notificationManager).failures = setOf("notify")
        RadioOverlayService.refreshLanguage(); assertSame(old, view); diagnostic("notification refresh failed")
    }

    @Test fun revokedMicrophonePermissionCancelsPttBeforeRequestingStop() {
        show(); touch(MotionEvent.ACTION_DOWN); context.permissionGranted = false
        RadioOverlayService.sync(context)
        assertFalse(radio.isTalking()); assertEquals(1, context.serviceStops.size)
        diagnostic("Microphone permission revoked")
    }

    @Test fun backgroundVadKeepsTheServiceWithoutRequiringAnOverlayPermission() {
        context.configProvider.radioOverlay = false; context.configProvider.radioSettings = RadioAutoSettings(enabled = true)
        Settings.overlayAllowed = false; val service = show()
        assertTrue(wm.windows.isEmpty()); assertEquals(0, service.stops)
        assertTrue(static("running").getBoolean(null))
    }

    @Test fun mainStateMutationsRejectWorkersBeforeStartingOrRequestingPermission() {
        val error = AtomicReference<Throwable?>()
        Thread { try { RadioOverlayService.setActivityVisible(true) } catch (e: Throwable) { error.set(e) } }.apply { start(); join() }
        assertTrue(error.get() is IllegalStateException); assertTrue(context.serviceStarts.isEmpty()); diagnostic("requires main thread")
    }

    @Test fun aRevokedOverlayPermissionDuringTouchStopsAnOtherwiseUnusedMicrophoneService() {
        val service = show(); touch(MotionEvent.ACTION_DOWN); Settings.overlayAllowed = false
        touch(MotionEvent.ACTION_MOVE, time = 110, down = 100)
        main.dispatchAll(); drain() // Сервис освобождается после пересчёта владельцев захвата в io.
        assertTrue(wm.windows.isEmpty()); assertFalse(radio.isTalking())
        assertTrue(context.serviceStops.size + service.serviceStops.size + service.stops > 0)
        assertTrue(static("pendingStop").getBoolean(null)); service.onDestroy()
    }

    @Test fun partialAddAndFailedRollbackKeepTheOwnerUntilRemovalCanBeConfirmed() {
        wm.failures = setOf("add after", "remove"); val service = show()
        assertEquals(1, wm.windows.size); assertTrue(RadioOverlayService.isShown.value)
        service.onDestroy(); assertSame(service, static("currentService").get(null))
        wm.failures = emptySet(); RadioOverlayService.sync(context)
        assertTrue(wm.windows.isEmpty()); assertFalse(RadioOverlayService.isShown.value)
    }

    @Test fun enableRequestedWhileTheOldServiceStopsRestartsOnlyAfterDestroy() {
        RadioOverlayService.setActivityVisible(true); val service = show()
        RadioOverlayService.stop(context); RadioOverlayService.sync(context)
        assertEquals(1, context.serviceStarts.size); assertTrue(static("restartAfterStop").getBoolean(null))
        service.onDestroy(); assertEquals(2, context.serviceStarts.size); assertTrue(static("starting").getBoolean(null))
    }

    @Test fun theNextPressUnlatchesTransmissionWithTheNormalReleaseTail() {
        show(); touch(MotionEvent.ACTION_DOWN); touch(MotionEvent.ACTION_UP, time = 130, down = 100)
        touch(MotionEvent.ACTION_DOWN, time = 180); touch(MotionEvent.ACTION_UP, time = 210, down = 180)
        main.advanceBy(500); assertTrue(radio.isTalking())
        touch(MotionEvent.ACTION_DOWN, time = 700); touch(MotionEvent.ACTION_UP, time = 730, down = 700)
        main.advanceBy(251); drain(); assertFalse(radio.isTalking())
    }

    @Test fun earlyDragReleasesPttAndKeepsItsPositionButALongHoldCannotStartDragging() {
        show(); val button = view; val origin = wm.windows.getValue(button).x
        touch(MotionEvent.ACTION_DOWN); touch(MotionEvent.ACTION_MOVE, time = 150, down = 100, x = 100f)
        main.advanceBy(251); drain(); assertFalse(radio.isTalking())
        val moved = wm.windows.getValue(button).x; assertEquals(origin + 100, moved)
        button.dispatchAnimations(); RadioOverlayService.setDefaultCenter(800, 800); button.dispatchAnimations()
        assertEquals(moved, wm.windows.getValue(button).x)
        touch(MotionEvent.ACTION_UP, time = 180, down = 100)
        touch(MotionEvent.ACTION_DOWN, time = 500)
        touch(MotionEvent.ACTION_MOVE, time = 801, down = 500, x = 200f)
        assertEquals(moved, wm.windows.getValue(button).x); assertTrue(radio.isTalking())
        touch(MotionEvent.ACTION_UP, time = 900, down = 500)
        main.advanceBy(251); drain(); assertFalse(radio.isTalking())
    }

    @Test fun aMissingServiceComponentDoesNotLatchTheOverlayStartingFlag() {
        context.acceptServiceStart = false; RadioOverlayService.setActivityVisible(true)
        assertFalse(static("starting").getBoolean(null)); diagnostic("Service start failed")
        context.acceptServiceStart = true; RadioOverlayService.sync(context)
        assertEquals(2, context.serviceStarts.size)
    }
}
