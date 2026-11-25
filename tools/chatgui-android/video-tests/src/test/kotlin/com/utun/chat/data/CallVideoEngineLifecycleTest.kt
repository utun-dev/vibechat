package com.utun.chat.data

import android.content.Context
import android.hardware.camera2.CameraManager
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.view.OrientationEventListener
import com.utun.chat.ChatApplication
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit
import java.util.concurrent.CountDownLatch
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Реальные TX/RX/camera callbacks; hardware, capability probe и JNI задаются на границах. */
class CallVideoEngineLifecycleTest {
    private val engines = mutableListOf<CallVideoEngine>()
    private val uncaught = CopyOnWriteArrayList<Throwable>()
    private val gates = mutableListOf<CountDownLatch>()
    private val images = mutableListOf<android.media.Image>()
    private val manager get() = CameraManager.shared
    private val main get() = Looper.getMainLooper()
    @Before fun prepare() {
        main.advanceBy(0); assertSame(main, Looper.myLooper())
        CameraManager.shared = CameraManager(); ImageReader.created.clear(); ImageReader.nextFailures = emptySet()
        CallVideoCodec.created.clear(); CallVideoCodec.nextFailures = emptySet(); CallVideoCodec.onProbe = {}
        OrientationEventListener.created.clear(); OrientationEventListener.failures = emptySet()
        ChatApplication.instance.configProvider.failures = emptySet()
        CallVideoNative.reset(); Handler.rejectDelayed = false; Handler.rejectImmediate = false; LogManager.entries.clear()
    }
    private fun thread(engine: CallVideoEngine) = CallVideoEngine::class.java.getDeclaredField("thread")
        .apply { isAccessible = true }.get(engine) as HandlerThread
    private fun await(message: String, condition: () -> Boolean) {
        val end = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!condition() && System.nanoTime() < end) { main.advanceBy(10); Thread.sleep(2) }
        assertTrue(message, condition())
    }
    private fun engine(): CallVideoEngine = CallVideoEngine(Context(), 7).also { engine ->
        engines.add(engine)
        thread(engine).uncaughtExceptionHandler = Thread.UncaughtExceptionHandler { _, error ->
            uncaught.add(error); LogManager.addLog("ERROR", "VideoEngineTest", "uncaught worker error: $error")
        }
        engine.start()
        await("video capabilities ready") { engine.state.value.ready }
    }
    private fun capture(): CallVideoEngine {
        val engine = engine(); engine.select(engine.state.value.settings.copy(enabled = true))
        await("camera is repeating") { manager.sessions.any { it.repeating } }
        return engine
    }
    @After fun release() {
        gates.forEach { it.countDown() }
        Handler.rejectDelayed = false; Handler.rejectImmediate = false
        OrientationEventListener.failures = emptySet(); ChatApplication.instance.configProvider.failures = emptySet()
        CallVideoNative.failures = emptySet(); CallVideoNative.onOpen = {}; CallVideoNative.onClose = {}
        images.forEach { it.failures = emptySet() }
        CallVideoCodec.onProbe = {}; CallVideoCodec.created.forEach { it.failures = emptySet() }
        manager.failures = emptySet(); manager.autoClosed = true
        manager.opened.forEachIndexed { index, open ->
            open.device.failures = emptySet()
            if (!open.delivered && !open.device.closed) manager.deliverOpened(index)
            if (open.device.closed) manager.deliverClosed(index)
        }
        manager.sessions.forEach { it.failures = emptySet() }; ImageReader.created.forEach { it.failures = emptySet() }
        for (engine in engines) {
            await("confirmed video stop") { engine.stop() && !thread(engine).isAlive }
        }
        main.advanceBy(30_000)
        assertTrue("uncaught worker exceptions: $uncaught", uncaught.isEmpty())
        assertTrue(manager.opened.all { it.device.closed }); assertTrue(ImageReader.created.all { it.closed })
        assertTrue(CallVideoCodec.created.all { it.closed }); assertTrue(CallVideoNative.ports.isEmpty())
        assertTrue(OrientationEventListener.created.none { it.enabled }); assertTrue(CallVideoNative.live.isEmpty())
    }

    @Test fun screenStartsWithoutCameraAndReleasesRetainedFrameOnSwitch() {
        val engine = engine()
        val projection = ScreenProjection()
        engine.projection = projection
        val camera = engine.state.value.settings.copy(enabled = true)
        engine.select(camera.copy(screen = true))
        await("screen attached") { projection.surface != null }
        assertTrue(manager.opened.isEmpty())
        val image = android.media.Image()
        ImageReader.created.single().push(image)
        await("screen transmitting") { engine.state.value.transmitting && engine.state.value.sharingScreen }
        val encoder = CallVideoCodec.created.last()
        await("static frame repeated") { encoder.submittedPts.size >= 2 }
        assertTrue(encoder.submittedPts[1] > encoder.submittedPts[0])
        projection.portrait = true
        await("reader resized within the same projection") {
            ImageReader.created.last().width == camera.height && projection.surface === ImageReader.created.last().surface
        }
        engine.select(camera)
        await("camera enabled") { manager.sessions.any { it.repeating } }
        assertTrue(image.closed)
        assertNull(projection.surface)
    }

    @Test fun stopWaitsForCameraClosedBeforeAcknowledgingAndClosingTheNativePort() {
        val engine = capture(); manager.autoClosed = false
        assertFalse(engine.stop())
        await("close requested") { manager.opened.single().device.closed }
        assertFalse(engine.stop()); assertTrue(7L in CallVideoNative.ports)
        manager.deliverClosed(0)
        await("camera closure acknowledged") { engine.stop() }
        assertFalse(7L in CallVideoNative.ports)
    }

    @Test fun failedReaderReleaseDoesNotSkipCameraCloseOrLoseTheReader() {
        val engine = capture(); val reader = ImageReader.created.single().also { it.failures = setOf("close") }
        engine.stop()
        await("camera close despite reader failure") { manager.opened.single().device.closed }
        assertFalse(engine.stop()); assertFalse(reader.closed); assertTrue(7L in CallVideoNative.ports)
        reader.failures = emptySet(); await("reader retry completes stop") { engine.stop() }
        assertTrue(reader.closed)
    }

    @Test fun failedDeviceCloseKeepsOwnershipAndTheNativePortUntilRetry() {
        val engine = capture(); val device = manager.opened.single().device.also { it.failures = setOf("close") }
        engine.stop(); await("device close attempted") { device.closeCalls > 0 }
        assertFalse(device.closed); assertFalse(engine.stop()); assertTrue(7L in CallVideoNative.ports)
        device.failures = emptySet(); await("device close retries") { engine.stop() }
        assertTrue(device.closed)
    }

    @Test fun encoderReleaseErrorCannotAbortIndependentCameraAndSensorCleanup() {
        val engine = capture(); val encoder = CallVideoCodec.created.single().also { it.failures = setOf("close") }
        engine.stop()
        await("camera closes even when encoder release fails") { manager.opened.single().device.closed }
        assertTrue(OrientationEventListener.created.none { it.enabled })
        assertFalse(encoder.closed); assertFalse(engine.stop()); assertTrue(thread(engine).isAlive)
        encoder.failures = emptySet(); await("encoder release retries") { engine.stop() }
    }

    @Test fun rxReleaseFailureDoesNotKeepTheCameraOrEncoderRunning() {
        val engine = capture(); CallVideoNative.failures = setOf("destroy")
        assertThrows(IllegalStateException::class.java) { engine.stop() }
        await("TX camera and encoder close despite RX error") {
            manager.opened.single().device.closed && CallVideoCodec.created.all { it.closed }
        }
        assertTrue(7L in CallVideoNative.ports)
        CallVideoNative.failures = emptySet(); await("RX retry allows final native close") { engine.stop() }
    }

    @Test fun cameraImageReleaseFailureIsContainedAndRetainedUntilRetry() {
        val engine = capture(); val image = android.media.Image().also { images.add(it); it.failures = setOf("close") }
        ImageReader.created.single().push(image)
        await("image failure disables capture") { !engine.state.value.settings.enabled }
        await("image failure closes camera independently") { manager.opened.single().device.closed }
        assertTrue(thread(engine).isAlive); assertFalse(engine.stop()); assertFalse(image.closed)
        image.failures = emptySet(); await("image retry allows acknowledged stop") { engine.stop() }
        assertTrue(image.closed)
    }

    @Test fun settingsSaveFailureIsContainedAndDoesNotApplyAnUncommittedSelection() {
        val engine = engine(); val previous = engine.state.value.settings
        ChatApplication.instance.configProvider.failures = setOf("settings save")
        engine.select(previous.copy(enabled = true))
        await("selection rejection reported") { engine.state.value.notice != null }
        assertTrue(thread(engine).isAlive); assertEquals(previous, engine.state.value.settings)
        assertTrue(manager.opened.isEmpty())
    }

    @Test fun passiveConstructionCanBeStoppedWithoutStartingAnyWorkerOrNativePort() {
        val engine = CallVideoEngine(Context(), 7).also { engines.add(it) }
        assertFalse(thread(engine).isAlive); assertTrue(CallVideoNative.live.isEmpty()); assertTrue(CallVideoNative.ports.isEmpty())
        assertTrue(engine.stop())
        assertThrows(IllegalStateException::class.java) { engine.start() }
    }

    @Test fun startupPostRejectionRemainsOwnedUntilExplicitStopCanBePosted() {
        val engine = CallVideoEngine(Context(), 7).also { engines.add(it) }
        Handler.rejectImmediate = true
        assertThrows(IllegalStateException::class.java) { engine.start() }
        assertThrows(IllegalStateException::class.java) { engine.stop() }
        Handler.rejectImmediate = false; await("partial startup cleanup") { engine.stop() }
        assertTrue(CallVideoNative.ports.isEmpty())
    }

    @Test fun stoppedPendingOpenWaitsForTheLateDeviceAndClosesItWithoutCreatingASession() {
        manager.autoOpened = false
        val engine = engine(); engine.select(engine.state.value.settings.copy(enabled = true))
        await("camera request pending") { manager.opened.isNotEmpty() }
        assertFalse(engine.stop()); assertTrue(7L in CallVideoNative.ports)
        manager.deliverOpened(0)
        await("late camera is released") { engine.stop() }
        assertTrue(manager.opened.single().device.closed); assertTrue(manager.sessions.isEmpty())
    }

    @Test fun synchronousOpenFailureWithErrorCallbackDoesNotLoseItsDevice() {
        manager.failures = setOf("open after")
        val engine = engine(); engine.select(engine.state.value.settings.copy(enabled = true))
        await("failed open device and reader close") {
            manager.opened.singleOrNull()?.device?.closed == true && ImageReader.created.all { it.closed }
        }
        assertTrue(thread(engine).isAlive); assertFalse(engine.state.value.settings.enabled)
        assertTrue(manager.sessions.isEmpty()); assertTrue(ImageReader.created.all { it.closed })
        await("failed open stop acknowledged") { engine.stop() }
    }

    @Test fun failedConfigurationWithoutOnClosedReleasesItsSessionAndStops() {
        manager.nextSessionFailures = setOf("configure")
        val engine = engine(); engine.select(engine.state.value.settings.copy(enabled = true))
        await("configuration failure reported") { manager.sessions.isNotEmpty() && !engine.state.value.settings.enabled }
        await("configuration failure cleanup acknowledged") { engine.stop() }
        assertTrue(manager.sessions.all { it.closed })
    }

    @Test fun failedPreviewConfigurationFallsBackWithoutRetainingTheClosedSession() {
        val engine = engine()
        val target = engine.preview.attach(android.graphics.SurfaceTexture(1))
        try {
            manager.sessionFailures.add(setOf("configure"))
            engine.select(engine.state.value.settings.copy(enabled = true))
            await("capture fallback without preview") { manager.sessions.size == 2 && manager.sessions.last().repeating }
            assertTrue(engine.state.value.settings.enabled)
            await("fallback cleanup completes") { engine.stop() }
            assertTrue(manager.sessions.all { it.closed })
        } finally { engine.preview.detach(target); target.release() }
    }

    @Test fun synchronousSessionFailureKeepsPreviewOutputsUntilTheDeviceAcknowledgesClosure() {
        manager.nextDeviceFailures = setOf("session"); manager.autoClosed = false
        val engine = engine(); val target = engine.preview.attach(android.graphics.SurfaceTexture(2))
        engine.select(engine.state.value.settings.copy(enabled = true))
        await("failed session camera close requested") { manager.opened.singleOrNull()?.device?.closed == true }
        engine.preview.detach(target); target.release()
        assertTrue(target.surface.isValid); assertFalse(engine.stop())
        manager.deliverClosed(0); await("failed session output released") { engine.stop() }
        assertFalse(target.surface.isValid)
    }

    @Test fun closedDeviceCallbacksCannotOverwriteANewerCamera() {
        val engine = capture(); engine.select(engine.state.value.settings.copy(enabled = false))
        await("old camera closes") { manager.opened[0].device.closed && ImageReader.created[0].closed }
        engine.select(engine.state.value.settings.copy(enabled = true))
        await("new camera captures") { manager.opened.size == 2 && manager.sessions.last().repeating }
        val old = manager.opened[0]; old.handler.post {
            old.callback.onOpened(old.device); old.callback.onError(old.device, 1); old.callback.onClosed(old.device)
        }
        Thread.sleep(30)
        assertTrue(engine.state.value.settings.enabled); assertFalse(manager.opened[1].device.closed)
        assertTrue(manager.sessions.last().repeating)
    }

    @Test fun orientationDisableFailureKeepsItsOwnerWhileOtherDevicesClose() {
        val engine = capture(); OrientationEventListener.failures = setOf("disable")
        engine.stop(); await("camera cleanup still runs") { manager.opened.single().device.closed && ImageReader.created.single().closed }
        assertFalse(engine.stop()); assertTrue(OrientationEventListener.created.single().enabled)
        assertTrue(CallVideoCodec.created.all { it.closed }); assertTrue(7L in CallVideoNative.ports)
        OrientationEventListener.failures = emptySet(); await("sensor cleanup retries") { engine.stop() }
    }

    @Test fun sessionCloseFailureIsAcknowledgedByItsCameraClosingWithoutLosingThePreviewLease() {
        val engine = capture(); manager.sessions.single().failures = setOf("close")
        engine.stop(); await("device closes all sessions") { engine.stop() }
        assertTrue(manager.sessions.all { it.closed }); assertTrue(manager.opened.all { it.device.closed })
    }

    @Test fun failedNativeCloseRemainsRetryableAfterAllAndroidResourcesAreReleased() {
        val engine = capture(); CallVideoNative.failures = setOf("close")
        engine.stop(); await("Android capture released") { manager.opened.single().device.closed && ImageReader.created.single().closed }
        assertFalse(engine.stop()); assertTrue(7L in CallVideoNative.ports)
        CallVideoNative.failures = emptySet(); await("native close retries") { engine.stop() }
        assertEquals(1, CallVideoNative.portOperations.count { it == "close:7" })
    }

    @Test fun stopDuringCapabilityProbeCannotCloseThenReopenThePort() {
        val entered = CountDownLatch(1); val gate = CountDownLatch(1).also { gates.add(it) }
        CallVideoCodec.onProbe = { entered.countDown(); gate.await() }
        val engine = CallVideoEngine(Context(), 7).also { engines.add(it); it.start() }
        assertTrue(entered.await(3, TimeUnit.SECONDS)); assertTrue(7L in CallVideoNative.ports)
        assertFalse(engine.stop()); assertTrue(7L in CallVideoNative.ports)
        gate.countDown(); await("probe cancels before native close") { engine.stop() }
        assertEquals(listOf("open:7", "close:7"), CallVideoNative.portOperations.toList())
    }

    @Test fun failedCleanupPostDoesNotAcknowledgeOrLoseTheLiveCamera() {
        val engine = capture(); Handler.rejectImmediate = true
        assertThrows(IllegalStateException::class.java) { engine.stop() }
        assertFalse(manager.opened.single().device.closed); assertTrue(7L in CallVideoNative.ports)
        Handler.rejectImmediate = false; await("cleanup can be posted again") { engine.stop() }
    }

    @Test fun bitrateSaveFailureKeepsTheAppliedNativeRangeAndTheWorkerAlive() {
        val engine = engine(); ChatApplication.instance.configProvider.failures = setOf("range save")
        val range = CallVideoBitrateRange(500, 2000); engine.range(range)
        await("applied range retained") { engine.state.value.range == range && engine.state.value.notice != null }
        assertTrue(thread(engine).isAlive)
    }

    @Test fun missingNativePortIsReportedOnceAndStopsReadinessInsteadOfLeavingVideoReady() {
        val engine = capture(); CallVideoNative.failures = setOf("status missing")
        await("port failure disables video") { !engine.state.value.ready && !engine.state.value.settings.enabled }
        Thread.sleep(30)
        assertEquals(1, LogManager.entries.count { it.level == "ERROR" && "Video native port unavailable" in it.message })
        assertTrue(thread(engine).isAlive)
    }

    @Test fun coreFormatErrorDoesNotPreventAUserFromSelectingANewGeneration() {
        val engine = capture(); CallVideoNative.statusError = 1
        await("bad generation disabled") { !engine.state.value.settings.enabled }
        val first = CallVideoNative.configured
        engine.select(engine.state.value.settings.copy(enabled = true))
        await("new generation configured") { CallVideoNative.configured > first && manager.sessions.last().repeating }
        assertTrue(engine.state.value.settings.enabled)
    }
}
