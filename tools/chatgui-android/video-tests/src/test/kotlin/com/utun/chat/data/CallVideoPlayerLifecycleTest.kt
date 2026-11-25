package com.utun.chat.data

import android.media.MediaCodec
import android.os.Handler
import android.os.Looper
import android.view.Surface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие RX/порядок показа/владение; JNI, аппаратная проба и MediaCodec управляются на границах. */
class CallVideoPlayerLifecycleTest {
    private val players = mutableListOf<CallVideoPlayer>()
    private val surfaces = mutableListOf<Surface>()
    private val gates = mutableListOf<CountDownLatch>()
    private val releases = CopyOnWriteArrayList<AtomicBoolean>()
    private val failures = CopyOnWriteArrayList<String>()
    private val main get() = Looper.getMainLooper()
    private val format = CallVideoFormat(1, 640, 480, codec = 1, level = 31, profile = 1)
    private val key = StreamVideoCodec.H264.keyFrame(byteArrayOf(0, 0, 0, 1, 5, 1),
        byteArrayOf(0, 0, 0, 1, 7, 1, 0, 0, 0, 1, 8, 1))

    @Before fun prepare() {
        CallVideoNative.reset(); MediaCodec.created.clear(); MediaCodec.nextFailures = emptySet()
        LogManager.entries.clear(); Handler.rejectDelayed = false
    }

    @After fun release() {
        Thread.interrupted(); gates.forEach { it.countDown() }; releases.forEach { it.set(false) }
        MediaCodec.created.forEach { it.failures = emptySet() }; surfaces.forEach { it.failures = emptySet() }
        CallVideoNative.failures = emptySet(); CallVideoNative.onPull = {}; CallVideoNative.onCreate = {}
        Handler.rejectDelayed = false
        players.forEach { it.stop() }; main.advanceBy(30_000)
        assertTrue(CallVideoNative.live.isEmpty()); assertTrue(MediaCodec.created.all { it.released })
        assertTrue(surfaces.all { !it.isValid })
    }

    private fun player(callback: (String) -> Unit = { failures.add(it) }) = CallVideoPlayer(7, callback).also {
        players.add(it); it.decoders(mapOf(1 to CallVideoCodec.Companion.Decoder())); it.start()
    }
    private fun surface() = Surface().also { surfaces.add(it) }
    private fun await(message: String, condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!condition() && System.nanoTime() < deadline) Thread.sleep(2)
        assertTrue(message, condition())
    }
    private fun worker(player: CallVideoPlayer) = CallVideoPlayer::class.java.getDeclaredField("worker")
        .apply { isAccessible = true }.get(player) as Thread
    private fun frame(generation: Long = 1, pts: Long = generation * 1_000_000) {
        CallVideoNative.frames.add(CallVideoNative.Frame(format.copy(generation = generation), pts, key))
    }
    private fun playing(): Pair<CallVideoPlayer, Surface> {
        val player = player(); val surface = surface(); player.surface(surface) { surface.release() }; frame()
        await("video is visible") { player.display.value != null }
        return player to surface
    }
    private fun diagnostic(fragment: String) = assertTrue("Missing diagnostic: $fragment", LogManager.entries.any {
        fragment in it.message && it.level in listOf("WARN", "ERROR")
    })
    private fun rejected(player: CallVideoPlayer) = assertThrows(IllegalStateException::class.java) { player.stop() }

    @Test fun decoderReleaseFailureBlocksItsSurfaceButNotIndependentNativeCleanup() {
        val (player, output) = playing(); val codec = MediaCodec.created.single()
        codec.failures = setOf("release")
        rejected(player)
        assertTrue(output.isValid); assertEquals(0, output.releaseCalls); assertFalse(codec.released)
        assertTrue(CallVideoNative.live.isEmpty()); assertEquals(1, CallVideoNative.destroys.size)
        diagnostic("video decoder release failed"); diagnostic("video RX cleanup pending")
        codec.failures = emptySet(); player.stop()
        assertFalse(output.isValid); assertEquals(1, output.releaseCalls)
        player.stop(); assertEquals(1, CallVideoNative.destroys.size)
    }

    @Test fun decoderStopFailureStillAttemptsReleaseAndAcknowledgesCleanup() {
        val (player, output) = playing(); MediaCodec.created.single().failures = setOf("stop")
        player.stop()
        assertTrue(MediaCodec.created.single().released); assertFalse(output.isValid)
        diagnostic("video decoder stop")
    }

    @Test fun partiallyConfiguredDecoderIsOwnedUntilReleaseSucceeds() {
        MediaCodec.nextFailures = setOf("configure after", "release")
        val player = player(); val output = surface(); player.surface(output) { output.release() }; frame()
        await("failed decoder RX exits") { failures.isNotEmpty() && !worker(player).isAlive }
        rejected(player)
        assertTrue(output.isValid); assertFalse(MediaCodec.created.single().released)
        MediaCodec.created.single().failures = emptySet(); player.stop(); assertFalse(output.isValid)
    }

    @Test fun releaseFailureAfterEffectStillRequiresAcknowledgmentBeforeSurfaceRelease() {
        val (player, output) = playing(); val codec = MediaCodec.created.single()
        codec.failures = setOf("release after"); rejected(player)
        assertTrue(codec.released); assertTrue(output.isValid)
        codec.failures = emptySet(); player.stop(); assertFalse(output.isValid)
    }

    @Test fun failedSurfaceReplacementRetainsBothOwnersAndDoesNotStartAnotherDecoder() {
        val (player, old) = playing(); old.failures = setOf("release")
        val next = surface(); player.surface(next) { next.release() }
        await("failed replacement exits RX") { !worker(player).isAlive }
        rejected(player)
        assertTrue(old.isValid); assertFalse(next.isValid); assertEquals(1, MediaCodec.created.size)
        old.failures = emptySet(); player.stop(); assertFalse(old.isValid)
        assertEquals(1, next.releaseCalls)
    }

    @Test fun nativeDestroyFailureDoesNotSkipDecoderOrSurfaceRelease() {
        val (player, output) = playing(); CallVideoNative.failures = setOf("destroy")
        rejected(player)
        assertTrue(MediaCodec.created.single().released); assertFalse(output.isValid)
        assertEquals(1, CallVideoNative.live.size); diagnostic("video playout destroy failed")
        CallVideoNative.failures = emptySet(); player.stop(); assertTrue(CallVideoNative.live.isEmpty())
        assertEquals(1, output.releaseCalls)
    }

    @Test fun failedSurfaceReleaseDoesNotSkipTheOtherQueuedOwners() {
        val entered = CountDownLatch(1); val gate = CountDownLatch(1).also { gates.add(it) }
        CallVideoNative.onCreate = { entered.countDown(); gate.await() }
        CallVideoNative.failures = setOf("create zero")
        val player = player(); assertTrue(entered.await(3, TimeUnit.SECONDS))
        val first = surface().also { it.failures = setOf("release") }
        val second = surface(); val third = surface()
        player.surface(first) { first.release() }; player.surface(second) { second.release() }; player.surface(third) { third.release() }
        gate.countDown(); await("allocation failure cleans queued owners") { !worker(player).isAlive }
        rejected(player)
        assertTrue(first.isValid); assertFalse(second.isValid); assertFalse(third.isValid)
        first.failures = emptySet(); player.stop(); assertFalse(first.isValid)
        assertEquals(1, second.releaseCalls); assertEquals(1, third.releaseCalls)
    }

    @Test fun automaticRetryCompletesAFailedReleaseWithoutAnotherCaller() {
        val (player, output) = playing(); output.failures = setOf("release")
        rejected(player); output.failures = emptySet(); main.advanceBy(250)
        assertFalse(output.isValid); assertTrue(CallVideoNative.live.isEmpty())
        player.stop(); assertEquals(1, MediaCodec.created.single().releaseCalls)
    }

    @Test fun rejectedRetryRetainsResourcesForExplicitStop() {
        val (player, output) = playing(); output.failures = setOf("release"); Handler.rejectDelayed = true
        rejected(player); diagnostic("video RX cleanup retry rejected")
        assertTrue(output.isValid)
        output.failures = emptySet(); player.stop(); assertFalse(output.isValid)
    }

    @Test fun lateSurfaceKeepsOwnershipUntilItsReleaseCanSucceed() {
        val player = player(); player.stop()
        val late = surface().also { it.failures = setOf("release") }
        player.surface(late) { late.release() }; rejected(player)
        late.failures = emptySet(); player.stop(); assertFalse(late.isValid)
        diagnostic("video surface received after RX stop")
    }

    @Test fun throwingFailureCallbackCannotSkipCleanup() {
        CallVideoNative.failures = setOf("create zero")
        val player = player { error("failure callback rejected") }
        await("RX exits after callback error") { !worker(player).isAlive }
        player.stop(); diagnostic("failure callback failed")
        assertTrue(CallVideoNative.live.isEmpty())
    }

    @Test fun oldDetachCannotRemoveANewerSurfaceAndPresentationRequiresMatchingPts() {
        val (player, first) = playing()
        val second = surface(); player.surface(second) { second.release() }; frame(2)
        await("second output visible") { player.display.value?.generation == 2L }
        player.detach(first); Thread.sleep(20)
        assertFalse(first.isValid); assertTrue(second.isValid)
        assertFalse(player.presented(second, 123)); assertTrue(player.presented(second, 2_000_000))
        await("presentation acknowledged") { 2_000_000L in CallVideoNative.presented }
    }

    @Test fun generationDrainReplacesDecoderWhileKeepingTheSurfaceOwner() {
        val (player, output) = playing(); frame(2)
        Thread.sleep(20); assertEquals(1, MediaCodec.created.size)
        player.presented(output, 1_000_000)
        await("new generation rendered") { player.display.value?.generation == 2L }
        assertTrue(MediaCodec.created.first().released); assertEquals(2, MediaCodec.created.size)
        assertTrue(output.isValid); assertEquals(0, output.releaseCalls)
    }

    @Test fun decoderApiFailuresRejectTheGenerationAndStillReleaseTheResources() {
        for (operation in listOf("configure", "start", "start after", "input", "input buffer", "queue", "output", "render")) {
            MediaCodec.nextFailures = setOf(operation)
            val player = player(); val output = surface(); player.surface(output) { output.release() }; frame()
            val previous = failures.size
            await("decoder failure $operation") { failures.size > previous }
            player.stop(); assertFalse(output.isValid); assertTrue(MediaCodec.created.last().released)
        }
    }

    private fun blocked(): Pair<CallVideoPlayer, CountDownLatch> {
        val entered = CountDownLatch(1); val gate = CountDownLatch(1).also { gates.add(it) }
        CallVideoNative.onPull = {
            entered.countDown()
            while (gate.count > 0) try { gate.await() } catch (_: InterruptedException) { /* Драйвер не обязан принимать interrupt. */ }
        }
        val player = player(); assertTrue(entered.await(3, TimeUnit.SECONDS))
        return player to gate
    }

    @Test fun blockedWorkerCannotBeAcknowledgedOrLoseItsNativeHandle() {
        val (player, gate) = blocked(); rejected(player)
        assertTrue(worker(player).isAlive); assertEquals(1, CallVideoNative.live.size)
        gate.countDown(); await("worker exits") { !worker(player).isAlive }; player.stop()
        assertTrue(CallVideoNative.live.isEmpty())
    }

    @Test fun interruptedJoinPreservesTheCallingThreadFlagAndKeepsCleanupRetryable() {
        val (player, gate) = blocked(); Thread.currentThread().interrupt()
        try {
            assertThrows(InterruptedException::class.java) { player.stop() }
            assertTrue(Thread.currentThread().isInterrupted); assertTrue(worker(player).isAlive)
        } finally { Thread.interrupted(); gate.countDown() }
        await("interrupted RX finishes after unblock") { !worker(player).isAlive }; player.stop()
        diagnostic("video RX join interrupted")
    }
}
