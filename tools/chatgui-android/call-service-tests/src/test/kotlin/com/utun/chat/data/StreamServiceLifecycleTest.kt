package com.utun.chat.data

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import androidx.core.app.NotificationCompat
import com.utun.chat.ChatApplication
import com.utun.chat.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие publisher controller/service; capture и OS-эффекты управляются на границах. */
@OptIn(ExperimentalCoroutinesApi::class)
class StreamServiceLifecycleTest {
    private val dispatcher = StandardTestDispatcher()
    private lateinit var context: Context
    private val services = mutableListOf<StreamService>()
    private val main get() = Looper.getMainLooper()
    private val state get() = requireNotNull(StreamController.state.value)
    @Before fun prepare() {
        assertSame(main, Looper.myLooper()); Dispatchers.setMain(dispatcher)
        context = Context().also { ChatApplication.instance = it }
        ChatApplication.instance.configProvider.streamSubblocks = 8; ChatApplication.instance.configProvider.streamRedundant = 0
        ChatApplication.instance.configProvider.streamRegularLimit = 192
        Handler.rejectImmediate = false; Handler.rejectDelayed = false; PendingIntent.reset(); NotificationCompat.failBuild = false
        NativeLib.ready = true; NativeLib.coreStopping = false; NativeLib.voiceActive = false; NativeLib.streamFailures = emptySet()
        NativeLib.streamStops.clear(); NativeLib.streamEvents.clear(); NativeLib.streamStats = null
        StreamCaptureEngine.nextFailures = emptySet(); StreamCaptureEngine.created.clear(); AppLanguage.failText = false
        CallController.reset(); StreamController.reset(); main.dispatchAll()
    }
    @After fun release() {
        Handler.rejectImmediate = false; Handler.rejectDelayed = false; NativeLib.streamFailures = emptySet()
        NativeLib.ready = true; NativeLib.coreStopping = false; NativeLib.voiceActive = false; AppLanguage.failText = false; NotificationCompat.failBuild = false
        StreamCaptureEngine.created.forEach { it.failures = emptySet() }
        context.powerManager?.let { power -> power.failures = emptySet(); power.locks.forEach { it.failures = emptySet() } }
        context.notificationManager?.failures = emptySet()
        PendingIntent.created.forEach { it.failCancel = false; it.failCancelAfter = false }
        services.forEach { it.failForegroundStop = false; it.failStopSelf = false; it.shutdown(abort = true) }
        if (StreamController.state.value?.ongoing == true && services.isEmpty()) create().onStartCommand(context.serviceStarts.last(), 0, 1)
        services.forEach { it.shutdown(abort = true); it.onDestroy() }; main.advanceBy(30_000)
        dispatcher.scheduler.advanceUntilIdle(); StreamController.requireReleased(); Dispatchers.resetMain()
        assertTrue(context.powerManager?.locks?.none { it.held } != false)
        assertTrue(PendingIntent.created.all { it.cancelled }); assertTrue(StreamCaptureEngine.created.all { it.stopped })
    }
    private fun field(service: StreamService, name: String) = StreamService::class.java.getDeclaredField(name)
        .apply { isAccessible = true }.get(service)
    private fun create() = StreamService().also {
        services.add(it); it.powerManager = context.powerManager; it.notificationManager = context.notificationManager; it.onCreate()
    }
    private fun request(audio: Boolean = false): Intent {
        StreamController.start("07".repeat(32), StreamCaptureConfig(), audio); return context.serviceStarts.last()
    }
    private fun active(): StreamService {
        val intent = request(); val service = create(); NativeLib.streamEvents.add(byteArrayOf(4))
        service.onStartCommand(intent, 0, 1); main.dispatchAll(); dispatcher.scheduler.runCurrent()
        assertEquals(StreamPhase.LIVE, state.phase); return service
    }
    private fun ended(service: StreamService) {
        service.shutdown(abort = true); main.dispatchAll(); assertFalse(StreamController.capturing)
        StreamController.requireReleased()
    }

    @Test fun screenStartConsumesGrantOnceAndSystemStopReleasesPublisher() {
        val screen = StreamCaptureConfig(mode = StreamCaptureMode(screen = true))
        StreamController.start("07".repeat(32), screen, false, Intent())
        val service = create()
        NativeLib.streamEvents.add(byteArrayOf(4))
        service.onStartCommand(context.serviceStarts.last(), 0, 1)
        main.dispatchAll(); dispatcher.scheduler.runCurrent()
        assertNull(StreamController.takeGrant(state.id))
        assertEquals(android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION, service.foregroundTypes)
        val projection = requireNotNull(StreamCaptureEngine.created.single().projection)
        projection.stopped()
        assertTrue(projection.closed)
        ended(service)
    }

    @Test fun coreCleanupRejectsNewPublisherBeforeForegroundOrCameraAcquisition() {
        NativeLib.coreStopping = true
        StreamController.start("07".repeat(32), StreamCaptureConfig(), true)
        assertEquals(StreamPhase.FAILED, state.phase)
        assertFalse(StreamController.capturing)
        assertTrue(context.serviceStarts.isEmpty())
        assertTrue(StreamCaptureEngine.created.isEmpty())
        StreamController.requireReleased()
    }
    @Test fun unsupportedFecStopsBeforeCaptureAndKeepsSpecificError() {
        ChatApplication.instance.configProvider.streamSubblocks = 255
        ChatApplication.instance.configProvider.streamRedundant = 62
        val service = create(); service.onStartCommand(request(), 0, 1); main.dispatchAll()
        assertTrue(StreamCaptureEngine.created.isEmpty())
        assertEquals(StreamPhase.FAILED, state.phase); assertEquals(R.string.stream_capacity_exceeded, state.error)
        assertFalse(StreamController.capturing); assertTrue(context.powerManager!!.locks.none { it.held })
    }
    @Test fun maximumTotalWithEnoughParityStartsAndNativeCapacityFailureIsTerminal() {
        ChatApplication.instance.configProvider.streamSubblocks = 255
        ChatApplication.instance.configProvider.streamRedundant = 63
        val service = active(); assertEquals(1, StreamCaptureEngine.created.size)
        NativeLib.streamEvents.add(byteArrayOf(6, 7)); dispatcher.scheduler.advanceTimeBy(20); dispatcher.scheduler.runCurrent()
        main.dispatchAll(); assertEquals(R.string.stream_capacity_exceeded, state.error)
        ended(service); assertEquals(StreamPhase.FAILED, state.phase)
    }
    @Test fun raisedBudgetAllows255RequiredParts() {
        ChatApplication.instance.configProvider.streamSubblocks = 255
        ChatApplication.instance.configProvider.streamRegularLimit = 256
        val service = active(); assertEquals(1, StreamCaptureEngine.created.size)
        ended(service)
    }

    @Test fun nullForegroundRequestFailsWithoutLeavingAStartingPublisher() {
        context.acceptServiceStart = false; request()
        assertEquals(StreamPhase.FAILED, state.phase); assertFalse(StreamController.capturing)
        StreamController.requireReleased()
    }
    @Test fun partialCaptureStartRetainsTheEngineUntilItsStopSucceeds() {
        StreamCaptureEngine.nextFailures = setOf("start", "stop")
        val intent = request(); val service = create(); service.onStartCommand(intent, 0, 1)
        assertNotNull(field(service, "capture")); assertTrue(StreamController.capturing)
        assertThrows(IllegalStateException::class.java) { StreamController.requireReleased() }
        StreamCaptureEngine.created.single().failures = emptySet(); ended(service)
        assertEquals(StreamPhase.FAILED, state.phase)
    }
    @Test fun pendingCaptureBlocksFinAndSurvivesServiceDestruction() {
        val service = active(); val engine = StreamCaptureEngine.created.single(); engine.failures = setOf("pending")
        service.shutdown(abort = true); assertTrue(NativeLib.streamStops.isEmpty()); assertSame(engine, field(service, "capture"))
        service.onDestroy(); assertTrue(StreamController.capturing); assertTrue(NativeLib.streamStops.isEmpty())
        engine.failures = emptySet(); main.advanceBy(250)
        assertFalse(StreamController.capturing); assertEquals(listOf(state.id), NativeLib.streamStops)
    }
    @Test fun nativeStopFailureKeepsTheLockAndPublisherBarrierForRetry() {
        val service = active(); val lock = context.powerManager!!.locks.single(); NativeLib.streamFailures = setOf("stop")
        service.shutdown(abort = true); assertTrue(lock.held); assertTrue(StreamController.capturing)
        assertThrows(IllegalStateException::class.java) { StreamController.requireReleased() }
        NativeLib.streamFailures = emptySet(); ended(service); assertFalse(lock.held)
    }
    @Test fun acquisitionFailureAfterEffectRetainsTheCpuLockUntilReleaseIsAcknowledged() {
        context.powerManager!!.failures = setOf("acquire", "release")
        val intent = request(); val service = create(); service.onStartCommand(intent, 0, 1)
        val lock = context.powerManager!!.locks.single(); assertTrue(lock.held); assertSame(lock, field(service, "lock"))
        lock.failures = emptySet(); ended(service); assertFalse(lock.held)
    }
    @Test fun tokenAndForegroundFailuresDoNotSkipIndependentCleanup() {
        val service = active(); val token = PendingIntent.created.first(); token.failCancel = true
        service.failForegroundStop = true; service.failStopSelf = true; service.shutdown(abort = true)
        assertTrue(context.powerManager!!.locks.none { it.held }); assertTrue(StreamController.capturing)
        assertFalse(token.cancelled); assertTrue(PendingIntent.created.last().cancelled)
        token.failCancel = false; service.failForegroundStop = false; service.failStopSelf = false; ended(service)
    }
    @Test fun normalStopWaitsForNativeFinBeforeReleasingTheService() {
        val service = active(); StreamController.stop()
        assertEquals(StreamPhase.STOPPING, state.phase); assertTrue(StreamController.capturing)
        assertEquals(1, NativeLib.streamStops.size)
        NativeLib.streamEvents.add(byteArrayOf(5)); dispatcher.scheduler.advanceTimeBy(20); dispatcher.scheduler.runCurrent()
        assertEquals(StreamPhase.ENDED, state.phase); assertFalse(StreamController.capturing)
    }
    @Test fun pollingErrorIsContainedAndStopsThePublisher() {
        val service = active(); NativeLib.streamFailures = setOf("pull")
        dispatcher.scheduler.advanceTimeBy(20); dispatcher.scheduler.runCurrent()
        assertEquals(StreamPhase.FAILED, state.phase); assertFalse(StreamController.capturing)
        assertNull(field(service, "capture"))
    }
    @Test fun invalidTxSnapshotPreservesTheLastValidStatistics() {
        active(); val data = LongArray(15); StreamController.updateTx(state.id, data)
        val previous = StreamController.txStats.value; assertNotNull(previous)
        StreamController.updateTx(state.id, longArrayOf(-1)); assertSame(previous, StreamController.txStats.value)
        assertEquals(StreamPhase.LIVE, state.phase)
    }
    @Test fun staleNotificationCannotStopANewPublisherOnTheReusedService() {
        val service = active(); val oldStop = PendingIntent.created.last().send(); ended(service)
        val intent = request(); NativeLib.streamEvents.add(byteArrayOf(4)); service.onStartCommand(intent, 0, 2)
        main.dispatchAll(); dispatcher.scheduler.runCurrent(); assertEquals(StreamPhase.LIVE, state.phase)
        service.onStartCommand(oldStop, 0, 3); assertEquals(StreamPhase.LIVE, state.phase)
    }
}
