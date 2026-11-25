package com.utun.chat.data

import android.media.MediaCodec
import android.media.MediaCodecList
import android.os.Handler
import android.os.Looper
import android.view.Surface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие mailbox/pump/worker; SDK декодирования управляется на аппаратной границе. */
class StreamVideoPlaybackLifecycleTest {
    private val players = mutableListOf<StreamVideoPlayback>()
    private val gates = mutableListOf<CountDownLatch>()
    private val surface = Surface()
    @Before fun prepare() {
        MediaCodec.created.clear(); MediaCodec.nextFailures = emptySet(); MediaCodec.onInput = {}
        MediaCodecList.names = listOf("host.decoder"); Handler.rejectDelayed = false
    }
    @After fun release() {
        gates.forEach { it.countDown() }; MediaCodec.onInput = {}
        MediaCodec.created.forEach { it.failures = emptySet() }; players.forEach { it.close() }
        Looper.getMainLooper().advanceBy(30_000); surface.release()
        assertTrue(MediaCodec.created.all { it.released })
    }
    private fun player(start: Boolean = true) = StreamVideoPlayback(9, surface, 0, {}, {}).also {
        players.add(it); if (start) it.start()
    }
    private fun frame(pts: Long = 1000) = StreamVideoPlayback.Frame(pts, true, 640, 480, 0, 1,
        StreamVideoCodec.H264, byteArrayOf(), byteArrayOf(1))
    private fun await(message: String, condition: () -> Boolean) {
        val until = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!condition() && System.nanoTime() < until) Thread.sleep(2)
        assertTrue(message, condition())
    }
    private fun playing(): StreamVideoPlayback {
        val player = player(); player.update(1000, 1000, 0); player.offer(frame())
        await("frame decoded") { player.state.decoded == 1L }
        return player
    }

    @Test fun passiveOwnerCanBeClosedWithoutStartingAnyCodec() {
        player(start = false).close(); assertTrue(MediaCodec.created.isEmpty())
    }
    @Test fun releaseFailureKeepsDecoderAndAllowsAcknowledgedRetry() {
        val player = playing(); val codec = MediaCodec.created.single(); codec.failures = setOf("release")
        assertThrows(IllegalStateException::class.java) { player.close() }
        assertTrue(surface.isValid); assertFalse(codec.released)
        codec.failures = emptySet(); player.close(); player.close(); assertTrue(codec.released)
    }
    @Test fun stopFailureStillAcknowledgesSuccessfulCodecRelease() {
        val player = playing(); val codec = MediaCodec.created.single(); codec.failures = setOf("stop")
        player.close(); assertTrue(codec.released)
    }
    @Test fun rejectedCandidateMustBeReleasedBeforeTryingAnotherDecoder() {
        MediaCodecList.names = listOf("host.bad", "host.fallback")
        MediaCodec.nextFailures = setOf("configure after", "release")
        val player = player(); player.offer(frame())
        await("decoder failure reported") { player.failure != null }
        assertEquals(1, MediaCodec.created.size)
        assertThrows(IllegalStateException::class.java) { player.close() }
        MediaCodec.created.single().failures = emptySet(); player.close()
    }
    @Test fun resetCannotDiscardADecoderWhoseReleaseFailed() {
        val player = playing(); val codec = MediaCodec.created.single(); codec.failures = setOf("release")
        player.reset(); await("reset cleanup failure reported") { player.failure != null }
        assertEquals(1, MediaCodec.created.size); assertFalse(player.wantsInput)
        assertThrows(IllegalStateException::class.java) { player.close() }
        codec.failures = emptySet(); player.close()
    }
    @Test fun boundedJoinDoesNotReleaseCodecWhileWorkerStillUsesIt() {
        val entered = CountDownLatch(1); val resume = CountDownLatch(1).also { gates.add(it) }
        MediaCodec.onInput = { entered.countDown(); check(resume.await(3, TimeUnit.SECONDS)) }
        val player = player(); player.offer(frame()); assertTrue(entered.await(3, TimeUnit.SECONDS))
        assertThrows(IllegalStateException::class.java) { player.close() }
        assertFalse(MediaCodec.created.single().released)
        resume.countDown(); player.close(); assertTrue(MediaCodec.created.single().released)
    }
}
