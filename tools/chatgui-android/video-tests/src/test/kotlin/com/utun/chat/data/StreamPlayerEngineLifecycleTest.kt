package com.utun.chat.data

import android.media.AudioTrack
import android.media.MediaCodec
import android.os.Looper
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие viewer/video/mixer workers; управляемые границы JNI и Android SDK. */
class StreamPlayerEngineLifecycleTest {
    private val viewers = mutableListOf<StreamPlayerEngine>()
    private val surfaces = mutableListOf<Surface>()
    private val main get() = Looper.getMainLooper()
    @Before fun prepare() {
        main.advanceBy(30_000); assertFalse(StreamPlayerEngine.viewing.value)
        HostStreams.reset(); CallController.blocked = false
        MediaCodec.created.clear(); MediaCodec.nextFailures = emptySet(); MediaCodec.onInput = {}
        AudioTrack.created.clear(); AudioTrack.nextFailures = emptySet(); MediaAudioController.failures = emptySet()
    }
    @After fun release() {
        HostStreams.failures = emptySet(); HostStreams.onPull = {}; HostStreams.onWait = {}
        MediaCodec.created.forEach { it.failures = emptySet() }; AudioTrack.created.forEach { it.failures = emptySet() }
        MediaAudioController.failures = emptySet(); viewers.forEach { it.stop() }
        await("viewer registry cleanup") { !StreamPlayerEngine.viewing.value }
        StreamAudioMixer.awaitIdle(); surfaces.forEach { it.release() }; main.advanceBy(30_000)
        assertTrue(HostStreams.ports.isEmpty()); assertTrue(HostStreams.signals.isEmpty()); assertTrue(HostStreams.playbacks.isEmpty())
        assertTrue(HostStreams.playbackAudio.isEmpty())
        assertTrue(MediaCodec.created.all { it.released }); assertTrue(AudioTrack.created.all { it.released })
        assertTrue(MediaAudioController.outputs.isEmpty())
    }
    private fun await(message: String, condition: () -> Boolean) {
        val until = System.nanoTime() + TimeUnit.SECONDS.toNanos(4)
        while (!condition() && System.nanoTime() < until) { main.advanceBy(10); Thread.sleep(2) }
        main.advanceBy(0); assertTrue(message, condition())
    }
    private fun record(kind: Int, data: ByteArray = byteArrayOf()) = ByteBuffer.allocate(28 + data.size).order(ByteOrder.BIG_ENDIAN)
        .put(kind.toByte()).put(1).putShort(640).putShort(480).putShort(0).putLong(1000).putInt(data.size).putInt(1)
        .put(1).put(0).put(0).put(0).put(data).array()
    private fun keyRecord() = record(1, StreamVideoCodec.H264.keyFrame(byteArrayOf(0, 0, 0, 1, 5, 1),
        byteArrayOf(0, 0, 0, 1, 7, 1, 0, 0, 0, 1, 8, 1)))
    private fun viewer(key: Boolean = false, audio: Boolean = false): StreamPlayerEngine {
        HostStreams.status = true; HostStreams.audioEnabled = audio
        if (key) HostStreams.records.add(keyRecord())
        return StreamPlayerEngine("01".repeat(32), "02".repeat(32), 3).also {
            viewers.add(it); val surface = Surface().also { surfaces.add(it) }; it.start(surface)
        }
    }

    private fun verifyAudioSwitch(initialAudio: Boolean) {
        val viewer = viewer(key = true, audio = initialAudio)
        await("initial playback renders") { viewer.status.value == com.utun.chat.R.string.stream_live }
        assertEquals(listOf(initialAudio), HostStreams.playbackModes.toList())
        assertEquals(0, HostStreams.resyncs.get())
        val ports = HostStreams.ports.toSet(); val signals = HostStreams.signals.toSet()
        val modes = mutableListOf(initialAudio)
        repeat(2) {
            val oldPlayback = HostStreams.playbacks.single()
            val enabled = !modes.last(); modes.add(enabled); HostStreams.audioEnabled = enabled
            await("playback clock switches audio=$enabled") { HostStreams.playbackModes.size == modes.size }
            assertEquals(modes, HostStreams.playbackModes.toList())
            assertFalse(oldPlayback in HostStreams.playbacks)
            assertEquals(listOf(enabled), HostStreams.playbackAudio.values.toList())
            HostStreams.records.add(keyRecord())
            await("video resumes audio=$enabled") { viewer.status.value == com.utun.chat.R.string.stream_live }
            if (enabled) {
                val before = HostStreams.audioFeeds.get(); HostStreams.records.add(record(2, byteArrayOf(1)))
                await("Opus reaches the new audio clock") { HostStreams.audioFeeds.get() > before }
            }
            assertEquals(ports, HostStreams.ports.toSet()); assertEquals(signals, HostStreams.signals.toSet())
            assertTrue(surfaces.single().isValid); assertTrue(StreamPlayerEngine.viewing.value)
            assertEquals(it + 1, HostStreams.resyncs.get())
        }
    }
    @Test fun videoClockCanEnableAudioAndReturnToNoAudio() = verifyAudioSwitch(false)
    @Test fun audioClockCanDisableAudioAndResumeIt() = verifyAudioSwitch(true)
    @Test fun offlineSourceClosesPlaybackWithoutWaitingForBufferedMedia() {
        val viewer = viewer(key = true, audio = true)
        await("playback is live") { viewer.status.value == com.utun.chat.R.string.stream_live }
        HostStreams.records.add(record(6).also { it[1] = 6 })
        await("offline status") { viewer.status.value == com.utun.chat.R.string.stream_source_offline }
        await("offline playback cleanup") { HostStreams.ports.isEmpty() && HostStreams.playbacks.isEmpty() && HostStreams.signals.isEmpty() }
        assertEquals(0, HostStreams.resyncs.get())
        assertTrue(surfaces.single().isValid)
    }
    @Test fun unsupportedFecReportsPermanentErrorAndReleasesPlayback() {
        HostStreams.records.add(record(6).also { it[1] = 7 })
        val viewer = viewer()
        await("capacity status") { viewer.status.value == com.utun.chat.R.string.stream_capacity_exceeded }
        await("capacity cleanup") { HostStreams.ports.isEmpty() && HostStreams.playbacks.isEmpty() && HostStreams.signals.isEmpty() }
        assertEquals(0, HostStreams.resyncs.get()); assertTrue(surfaces.single().isValid)
    }
    @Test fun failedAudioModeSwitchKeepsTheOriginalNativeHandleUntilCleanupAcknowledges() {
        val viewer = viewer(); await("initial playback is ready") { HostStreams.playbacks.size == 1 }
        val original = HostStreams.playbacks.single(); HostStreams.failures = setOf("playback close")
        HostStreams.audioEnabled = true
        await("audio mode release failure is reported") { viewer.status.value == com.utun.chat.R.string.stream_failed }
        assertEquals(setOf(original), HostStreams.playbacks.toSet())
        assertEquals(listOf(false), HostStreams.playbackModes.toList()); assertEquals(0, HostStreams.resyncs.get())
        HostStreams.failures = emptySet()
        await("original playback cleanup completes") { HostStreams.playbacks.isEmpty() }
    }

    @Test fun failedOpenStillCompletesCleanupAndTheReleaseCallback() {
        HostStreams.failures = setOf("open"); val viewer = viewer(); val released = AtomicInteger()
        await("open refusal reported") { viewer.status.value == com.utun.chat.R.string.stream_failed }
        viewer.stop { released.incrementAndGet() }; await("failed open callback") { released.get() == 1 }
        assertTrue(HostStreams.signals.isEmpty())
    }
    @Test fun nativeStopFailureCannotSkipIndependentPlaybackCleanupOrLoseTheSignal() {
        val viewer = viewer(); await("native playback opened") { HostStreams.playbacks.isNotEmpty() }
        HostStreams.failures = setOf("stop"); val released = AtomicInteger(); viewer.stop { released.incrementAndGet() }
        await("independent audio cleanup") { HostStreams.playbacks.isEmpty() }
        assertEquals(0, released.get()); assertTrue(HostStreams.signals.isNotEmpty()); assertTrue(StreamPlayerEngine.viewing.value)
        HostStreams.failures = emptySet(); await("native stop retry acknowledged") { released.get() == 1 }
    }
    @Test fun decoderReleaseFailureKeepsTheSurfaceCallbackAndNotificationOwned() {
        val viewer = viewer(key = true); await("decoder is active") { MediaCodec.created.any { it.started } }
        MediaCodec.created.single().failures = setOf("release")
        val released = AtomicInteger(); viewer.stop { released.incrementAndGet() }
        await("playback closes independently") { HostStreams.playbacks.isEmpty() }
        assertEquals(0, released.get()); assertTrue(HostStreams.signals.isNotEmpty())
        MediaCodec.created.single().failures = emptySet(); await("decoder retry acknowledged") { released.get() == 1 }
    }
    @Test fun audioResetFailureDoesNotDropTheOriginalNativeHandle() {
        val viewer = viewer(); await("playback created") { HostStreams.playbacks.size == 1 }
        val original = HostStreams.playbacks.single(); HostStreams.failures = setOf("playback close")
        HostStreams.records.add(record(3))
        await("failed reset reported") { viewer.status.value == com.utun.chat.R.string.stream_failed }
        assertEquals(setOf(original), HostStreams.playbacks.toSet())
        val released = AtomicInteger(); viewer.stop { released.incrementAndGet() }; assertEquals(0, released.get())
        HostStreams.failures = emptySet(); await("original handle release retries") { released.get() == 1 }
    }

    @Test fun gapDoesNotDiscardTheFollowingKeyFromTheSameNativeBatch() {
        val viewer = viewer(); await("playback is ready") { HostStreams.playbacks.isNotEmpty() }
        val key = StreamVideoCodec.H264.keyFrame(byteArrayOf(0, 0, 0, 1, 5, 1),
            byteArrayOf(0, 0, 0, 1, 7, 1, 0, 0, 0, 1, 8, 1))
        HostStreams.records.add(record(3) + record(1, key))
        await("KEY after GAP is decoded") { viewer.status.value == com.utun.chat.R.string.stream_live }
        assertEquals(1, MediaCodec.created.size)
    }
    @Test fun signalCloseFailureDoesNotAcknowledgeTheSurfaceOrViewingBarrier() {
        val viewer = viewer(); await("viewer opened") { HostStreams.signals.size == 1 }
        HostStreams.failures = setOf("signal close"); val released = AtomicInteger(); viewer.stop { released.incrementAndGet() }
        await("other native owners closed") { HostStreams.playbacks.isEmpty() && HostStreams.ports.isEmpty() }
        assertEquals(0, released.get()); assertTrue(StreamPlayerEngine.viewing.value)
        HostStreams.failures = emptySet(); await("signal close retry") { released.get() == 1 }
    }
    @Test fun failedReleaseCallbackIsRetriedBeforeTheViewerIsAcknowledged() {
        val viewer = viewer(); await("viewer opened") { HostStreams.signals.size == 1 }
        val attempts = AtomicInteger(); val released = AtomicInteger()
        viewer.stop { if (attempts.incrementAndGet() == 1) error("Surface release failed"); released.incrementAndGet() }
        await("release callback retries") { released.get() == 1 }; assertEquals(2, attempts.get())
    }
    @Test fun failedAudioOutputReleaseBlocksAMixerReplacementUntilTheOldDeviceAcknowledges() {
        val first = viewer(key = true, audio = true); await("first shared output active") { AudioTrack.created.size == 1 }
        val old = AudioTrack.created.single(); old.failures = setOf("release")
        first.stop(); await("first viewer released") { !StreamPlayerEngine.viewing.value }
        assertThrows(IllegalStateException::class.java) { StreamAudioMixer.awaitIdle() }
        assertThrows(IllegalStateException::class.java) { StreamPlayerEngine.stopForCore() }
        val second = viewer(key = true, audio = true); await("second playback ready") { HostStreams.playbacks.isNotEmpty() }
        assertEquals(1, AudioTrack.created.size); assertFalse(old.released)
        old.failures = emptySet(); await("second output opens after old release") { AudioTrack.created.size == 2 }
        assertTrue(old.released); second.stop()
    }

    @Test fun coreTeardownCannotPassAViewerWithUnacknowledgedNativeResources() {
        val viewer = viewer(); await("viewer signal opened") { HostStreams.signals.size == 1 }
        HostStreams.failures = setOf("signal close")
        assertThrows(IllegalStateException::class.java) { StreamPlayerEngine.stopForCore() }
        await("other native owners close") { HostStreams.playbacks.isEmpty() && HostStreams.ports.isEmpty() }
        assertThrows(IllegalStateException::class.java) { StreamPlayerEngine.stopForCore() }
        HostStreams.failures = emptySet(); await("viewer barrier released") { !StreamPlayerEngine.viewing.value }
        StreamPlayerEngine.stopForCore()
    }
    @Test fun stopBeforeStartLeavesTheViewerPassive() {
        val viewer = StreamPlayerEngine("01".repeat(32), "02".repeat(32), 3).also { viewers.add(it) }
        viewer.stop(); val surface = Surface().also { surfaces.add(it) }; viewer.start(surface)
        assertFalse(StreamPlayerEngine.viewing.value); assertTrue(HostStreams.ports.isEmpty())
    }
}
