package com.utun.chat.data

import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraManager
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.view.OrientationEventListener
import java.nio.ByteBuffer
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий capture worker/SDK callbacks; камера, MediaCodec, PCM и JNI управляются на границах. */
class StreamCaptureEngineLifecycleTest {
    private val engines = mutableListOf<StreamCaptureEngine>()
    private val targets = mutableListOf<StreamPreviewTarget>()
    private val failures = CopyOnWriteArrayList<String>()
    private val uncaught = CopyOnWriteArrayList<Throwable>()
    private val confirmed = CopyOnWriteArrayList<StreamCaptureConfig>()
    private val manager get() = CameraManager.shared
    private val config = StreamCaptureConfig(StreamCaptureMode())
    @Before fun prepare() {
        CameraManager.shared = CameraManager(); MediaCodec.created.clear(); MediaCodec.nextFailures = emptySet()
        MediaCodec.onInput = {}; HostStreams.reset(); Handler.rejectImmediate = false; Handler.rejectDelayed = false
        OrientationEventListener.created.clear(); OrientationEventListener.failures = emptySet()
        MediaAudioController.failures = emptySet(); MediaAudioController.subscriptions.clear()
    }
    private fun worker(engine: StreamCaptureEngine) = StreamCaptureEngine::class.java.getDeclaredField("thread")
        .apply { isAccessible = true }.get(engine) as HandlerThread
    private fun await(message: String, condition: () -> Boolean) {
        val until = System.nanoTime() + TimeUnit.SECONDS.toNanos(4)
        while (!condition() && System.nanoTime() < until) { Looper.getMainLooper().advanceBy(10); Thread.sleep(2) }
        assertTrue(message, condition())
    }
    private fun engine(audio: Boolean = false, preview: StreamPreview = StreamPreview(), start: Boolean = true) =
        StreamCaptureEngine(Context(), 77, "07".repeat(32), config, audio, preview, Any(), { value, _ -> confirmed.add(value) }, { failures.add(it) }).also {
            engines.add(it); worker(it).uncaughtExceptionHandler = Thread.UncaughtExceptionHandler { _, e -> uncaught.add(e) }
            if (start) it.start()
        }
    private fun capture(audio: Boolean = false): StreamCaptureEngine = engine(audio).also {
        await("camera capture configured") { manager.sessions.any { it.repeating } }
    }
    private fun confirm() {
        val codec = MediaCodec.created.last()
        codec.emitFormat(MediaFormat().apply { setByteBuffer("csd-0", ByteBuffer.wrap(byteArrayOf(0, 0, 0, 1, 7, 1, 0, 0, 0, 1, 8, 1))) })
        codec.emitFrame(byteArrayOf(0, 0, 0, 1, 5, 1))
        await("capture KEY acknowledged") { confirmed.isNotEmpty() }
    }
    @After fun release() {
        Handler.rejectImmediate = false; Handler.rejectDelayed = false; OrientationEventListener.failures = emptySet()
        MediaAudioController.failures = emptySet(); MediaAudioController.subscriptions.forEach { it.failures = emptySet() }
        HostStreams.failures = emptySet(); manager.autoClosed = true; manager.failures = emptySet()
        MediaCodec.created.forEach { it.failures = emptySet(); it.inputSurface?.failures = emptySet() }
        manager.opened.forEachIndexed { i, open ->
            open.device.failures = emptySet()
            if (!open.delivered && !open.device.closed) manager.deliverOpened(i)
            if (open.device.closed) manager.deliverClosed(i)
        }
        manager.sessions.forEach { it.failures = emptySet() }
        engines.forEach { engine -> await("capture stop acknowledged") { engine.stop() && !worker(engine).isAlive } }
        targets.forEach { it.release() }; Looper.getMainLooper().advanceBy(30_000); NativeLib.streamStop(77)
        assertTrue("uncaught exceptions=$uncaught", uncaught.isEmpty())
        assertTrue(MediaCodec.created.all { it.released && it.inputSurface?.isValid != true })
        assertTrue(manager.opened.all { it.device.closed }); assertTrue(HostStreams.opuses.isEmpty())
        assertTrue(MediaAudioController.subscriptions.all { it.closed }); assertTrue(OrientationEventListener.created.none { it.enabled })
    }

    @Test fun switchCameraToScreenAndBackPreservesPublisherAndMicrophone() {
        val engine = capture(audio = true)
        confirm()
        val subscription = MediaAudioController.subscriptions.single()
        val projection = ScreenProjection()
        engine.projection = projection
        val screen = config.copy(mode = config.mode.copy(screen = true, camera = ""))
        engine.change(screen)
        await("projection attached") { projection.surface != null }
        confirmed.clear(); confirm()
        assertEquals(screen, confirmed.last())
        engine.change(config)
        await("camera resumed") { manager.sessions.count { it.repeating } >= 1 && manager.opened.size == 2 && MediaCodec.created.last().started }
        confirmed.clear(); confirm()
        assertNull(projection.surface)
        assertSame(subscription, MediaAudioController.subscriptions.single())
        assertFalse(subscription.closed)
        assertEquals(config, confirmed.last())
        assertTrue(failures.isEmpty())
    }

    @Test fun passiveOwnerStopsWithoutStartingWorkersOrNativeResources() {
        val engine = engine(start = false); assertTrue(engine.stop()); assertTrue(HostStreams.ports.isEmpty())
        assertTrue(MediaCodec.created.isEmpty())
    }
    @Test fun sourceRateChangesEncoderWithoutChangingAutoAndManualBitrateRemainsFixed() {
        val engine = capture(); confirm()
        val codec = MediaCodec.created.single(); assertEquals(1_000_000, codec.bitrate)
        HostStreams.videoRateTarget = 800_000; codec.emitFrame(byteArrayOf(0, 0, 0, 1, 5, 1))
        await("automatic target applied") { codec.bitrate == 800_000 }
        assertEquals(config, confirmed.last()); assertEquals(1, MediaCodec.created.size)
        val manual = config.copy(videoBitrate = 900_000); engine.change(manual)
        await("manual bitrate applied") { codec.bitrate == 900_000 && confirmed.last() == manual }
        HostStreams.videoRateTarget = 640_000; codec.emitFrame(byteArrayOf(0, 0, 0, 1, 5, 1))
        Thread.sleep(30); assertEquals(900_000, codec.bitrate); assertTrue(failures.isEmpty())
    }
    @Test fun unsupportedLiveBitrateReplacesEncoderAtTheRecommendedTarget() {
        capture(); confirm()
        val old = MediaCodec.created.single(); old.failures = setOf("parameters")
        HostStreams.videoRateTarget = 800_000; old.emitFrame(byteArrayOf(0, 0, 0, 1, 5, 1))
        await("replacement encoder configured") { MediaCodec.created.size == 2 && MediaCodec.created.last().started }
        assertTrue(old.released); assertEquals(800_000, MediaCodec.created.last().bitrate)
        assertEquals(config, confirmed.last()); assertTrue(failures.isEmpty())
    }
    @Test fun cameraCloseTimeoutCannotReleaseTheEncoderOrInputSurfaceWithoutAck() {
        val engine = capture(); manager.autoClosed = false; assertFalse(engine.stop())
        await("camera close requested") { manager.opened.single().device.closed }
        Thread.sleep(3200)
        val codec = MediaCodec.created.single(); assertFalse(engine.stop()); assertFalse(codec.released); assertTrue(codec.inputSurface!!.isValid)
        manager.deliverClosed(0); await("late camera ACK releases resources") { engine.stop() }
    }
    @Test fun failedCameraCloseRetainsTheDeviceUntilRetry() {
        val engine = capture(); val device = manager.opened.single().device; device.failures = setOf("close")
        engine.stop(); await("close attempted") { device.closeCalls > 0 }; assertFalse(engine.stop()); assertFalse(device.closed)
        device.failures = emptySet(); await("device retry acknowledged") { engine.stop() }
    }
    @Test fun failedEncoderReleaseProtectsTheInputSurfaceAndDoesNotSkipCameraOrSensor() {
        val engine = capture(); val codec = MediaCodec.created.single(); codec.failures = setOf("release")
        engine.stop(); await("camera closes") { manager.opened.single().device.closed }
        assertFalse(engine.stop()); assertTrue(codec.inputSurface!!.isValid); assertTrue(OrientationEventListener.created.none { it.enabled })
        codec.failures = emptySet(); await("encoder close retried") { engine.stop() }
    }
    @Test fun inputSurfaceReleaseFailureAfterEffectRequiresAnAcknowledgedRetry() {
        val engine = capture(); val codec = MediaCodec.created.single(); val surface = codec.inputSurface!!
        surface.failures = setOf("release after"); engine.stop(); await("encoder releases") { codec.released }
        assertFalse(engine.stop()); assertFalse(surface.isValid)
        surface.failures = emptySet(); await("Surface close retry") { engine.stop() }
    }
    @Test fun partialEncoderStartupRetainsItsCodecEvenBeforeCreatingAnInputSurface() {
        MediaCodec.nextFailures = setOf("callback after", "release")
        val engine = engine(); await("setup failure reported") { failures.isNotEmpty() }
        assertFalse(engine.stop()); assertFalse(MediaCodec.created.single().released)
        MediaCodec.created.single().failures = emptySet(); await("partial codec cleanup completes") { engine.stop() }
    }
    @Test fun pendingCameraOpenAndErrorCallbackRemainOwnedAfterStop() {
        manager.autoOpened = false
        val engine = engine(); await("open requested") { manager.opened.isNotEmpty() }; assertFalse(engine.stop())
        manager.deliverOpened(0); await("late opened device cleaned up") { engine.stop() }; assertTrue(manager.sessions.isEmpty())
    }
    @Test fun synchronousOpenFailureWithQueuedOnErrorDoesNotLoseTheCamera() {
        manager.failures = setOf("open after")
        val engine = engine(); await("open failure reported") { failures.isNotEmpty() }
        await("error callback device released") { engine.stop() }; assertTrue(manager.opened.single().device.closed)
    }
    @Test fun previewConfigurationFailureFallsBackAndReleasesTheFailedSession() {
        manager.sessionFailures.add(setOf("configure")); val preview = StreamPreview()
        val target = preview.attach(SurfaceTexture(1)).also { targets.add(it) }; val engine = engine(preview = preview)
        await("preview fallback capture starts") { manager.sessions.size == 2 && manager.sessions.last().repeating }
        assertTrue(failures.isEmpty()); await("fallback cleanup") { engine.stop() }; assertTrue(target.surface.isValid)
    }
    @Test fun microphoneCloseFailureDoesNotPreventCameraAndOpusCleanup() {
        val engine = capture(audio = true); val subscription = MediaAudioController.subscriptions.single()
        subscription.failures = setOf("close"); engine.stop()
        await("independent camera and DSP cleanup") { manager.opened.single().device.closed && HostStreams.opuses.isEmpty() }
        assertFalse(engine.stop()); subscription.failures = emptySet(); await("subscription close retry") { engine.stop() }
    }
    @Test fun opusReleaseFailureKeepsItsWorkerAndCaptureCompletionOwned() {
        val engine = capture(audio = true); await("Opus allocated") { HostStreams.opuses.isNotEmpty() }
        HostStreams.failures = setOf("opus close"); engine.stop()
        await("camera closes independently") { manager.opened.single().device.closed }
        assertFalse(engine.stop()); assertTrue(HostStreams.opuses.isNotEmpty())
        HostStreams.failures = emptySet(); await("Opus retry permits final stop") { engine.stop() }
    }
    @Test fun stopCancelsAPendingModeSwitchUntilTheOldCameraAcknowledgesClosure() {
        val engine = capture(); confirm(); manager.autoClosed = false
        engine.change(config.copy(orientation = StreamOrientation.RIGHT))
        await("switch waits for camera closure") { manager.opened.single().device.closed }
        assertFalse(engine.stop()); manager.deliverClosed(0); await("cancelled switch cleanup") { engine.stop() }
        assertEquals(1, MediaCodec.created.size); assertEquals(1, manager.opened.size)
    }
    @Test fun rejectedCleanupPostCannotAcknowledgeALiveCapture() {
        val engine = capture(); Handler.rejectImmediate = true
        assertThrows(IllegalStateException::class.java) { engine.stop() }
        assertFalse(manager.opened.single().device.closed)
        Handler.rejectImmediate = false; await("explicit cleanup retry") { engine.stop() }
    }

    @Test fun rejectedStartupPostLeavesAnOwnedPassiveEngineForStop() {
        val engine = engine(start = false); Handler.rejectImmediate = true
        assertThrows(IllegalStateException::class.java) { engine.start() }
        assertTrue(MediaCodec.created.isEmpty()); assertTrue(HostStreams.ports.isEmpty())
        Handler.rejectImmediate = false; await("partial startup cleanup") { engine.stop() }
    }
    @Test fun synchronousSessionSubmissionFailureRetainsPreviewUntilDeviceClosure() {
        manager.nextDeviceFailures = setOf("session"); manager.autoClosed = false
        val preview = StreamPreview(); val target = preview.attach(SurfaceTexture(3)).also { targets.add(it) }
        val engine = engine(preview = preview); await("failed session closes device") { manager.opened.singleOrNull()?.device?.closed == true }
        preview.detach(target); target.release(); assertTrue(target.surface.isValid); assertFalse(engine.stop())
        manager.deliverClosed(0); await("device ACK releases preview") { engine.stop() }; assertFalse(target.surface.isValid)
    }
    @Test fun closedDeviceCallbacksCannotDisturbTheReplacementCamera() {
        val engine = capture(); confirm(); val old = manager.opened.single()
        engine.change(config.copy(orientation = StreamOrientation.RIGHT))
        await("replacement camera captures") { manager.opened.size == 2 && manager.sessions.last().repeating }
        val replayed = java.util.concurrent.atomic.AtomicBoolean()
        old.handler.post { old.callback.onOpened(old.device); old.callback.onError(old.device, 1); old.callback.onClosed(old.device); replayed.set(true) }
        await("old callbacks processed") { replayed.get() }
        assertTrue(failures.isEmpty()); assertFalse(manager.opened.last().device.closed); assertTrue(manager.sessions.last().repeating)
    }
}
