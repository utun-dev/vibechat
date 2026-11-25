package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.util.ArrayDeque

class StreamAudioOutputTest {
    private class Device(override val threshold: Int = 1) : StreamAudioOutput.Sink {
        override var routeChanging = false
        override val bufferFrames = maxOf(960, threshold)
        override var head = 0L
        override val description: String get() = "fake threshold=$threshold"
        val samples = ArrayList<Short>()
        var limit = 960
        var calls = 0
        var paused = false
        var closed = false
        var failures = emptySet<String>()
        var closeCalls = 0
        override fun write(pcm: ShortArray, offset: Int, count: Int): Int {
            check(!paused && !closed)
            calls++
            if (limit < 0) return limit
            val accepted = minOf(limit, count)
            for (i in offset until offset + accepted) samples.add(pcm[i])
            return accepted
        }
        override fun pause() { paused = true }
        override fun initialize() { check("initialize" !in failures) { "initialization failed" } }
        override fun play() { paused = false; check("play" !in failures) { "play failed" } }
        override fun close() {
            closeCalls++; check("close" !in failures) { "close failed" }; closed = true
            check("close after" !in failures) { "close failed after effect" }
        }
    }
    private class Player(val device: Device = Device()) {
        val input = ArrayDeque<ShortArray>()
        val logs = ArrayList<String>()
        val output = StreamAudioOutput({ device }, { logs.add(it) })
        var finished = false
        var pulls = 0
        fun step(ms: Long, hold: Boolean = false) = output.step(ms * 1_000_000, hold,
            { pulls++; input.pollFirst() }, { finished && input.isEmpty() })
    }

    @Test fun slowStartupDoesNotBlockOtherPlayerWork() {
        val p = Player(); p.input.add(ShortArray(960) { it.toShort() }); p.device.limit = 0
        var videoSteps = 0
        for (ms in 0L..300L step 5) { p.step(ms); videoSteps++ }
        assertEquals(61, videoSteps); assertEquals(61, p.device.calls); assertEquals(1, p.pulls)
        assertEquals(480, p.output.pendingFrames); assertEquals(0L, p.output.played)
        p.device.limit = 960; p.step(305); p.device.head = 480; p.step(310)
        assertTrue(p.output.drained); assertTrue(p.logs.any { it.contains("audio started") })
    }

    @Test fun partialWritesPreserveEverySampleAndOnlyPullOnce() {
        val p = Player(); val pcm = ShortArray(960) { it.toShort() }; p.input.add(pcm)
        p.device.limit = 120
        repeat(8) { p.step(it * 5L) }
        assertEquals(pcm.toList(), p.device.samples); assertEquals(1, p.pulls)
        assertEquals(480L, p.output.written); assertEquals(0L, p.output.played)
        assertEquals(0, p.output.pendingFrames); assertFalse(p.output.drained)
    }

    @Test fun waitingForInitialNetworkDataIsNotDeviceStall() {
        val p = Player(Device(3848)); p.input.add(ShortArray(200)); p.step(0); p.step(10000)
        assertEquals(100L, p.output.written); assertEquals(0L, p.output.played)
    }

    @Test fun feedsHardwareBufferEvenWhenStartThresholdIsSmall() {
        val p = Player(); repeat(4) { p.input.add(ShortArray(960)) }
        p.step(0); p.step(5); p.step(10)
        assertEquals(960L, p.output.written); assertEquals(2, p.pulls)
        p.device.head = 480; p.step(15)
        assertEquals(1440L, p.output.written); assertEquals(3, p.pulls)
    }

    @Test fun clockDoesNotAdvanceWithWallTimeAndStalledDeviceFails() {
        val p = Player(); p.input.add(ShortArray(960)); p.step(0); p.step(1999)
        assertEquals(0L, p.output.played)
        val error = assertThrows(IllegalStateException::class.java) { p.step(2000) }
        assertTrue(error.message!!.contains("no playback progress"))
        assertTrue(error.message!!.contains("written=480 played=0"))
    }

    @Test fun actualHeadProgressRestartsWatchdog() {
        val p = Player(); p.input.add(ShortArray(960)); p.step(0)
        p.device.head = 100; p.step(1900); p.step(3800)
        assertEquals(100L, p.output.played)
        p.device.head = 480; p.step(3900); p.step(10000)
        assertTrue(p.output.drained)
    }

    @Test fun routeSwitchPreservesPlayerAndClockUntilOutputResumes() {
        val p = Player(); p.input.add(ShortArray(960) { 7 }); p.step(0)
        p.device.routeChanging = true
        var videoSteps = 0
        for (ms in 5L..30000L step 5) { p.step(ms); videoSteps++ }
        assertEquals(6000, videoSteps)
        assertFalse(p.device.closed); assertEquals(0L, p.output.played)
        assertEquals(List(960) { 7.toShort() }, p.device.samples)
        p.device.routeChanging = false
        p.device.head = 480; p.step(30005)
        assertTrue(p.output.drained)
    }

    @Test fun intentionalPausePreservesPartialPcmAndSuspendsWatchdog() {
        val p = Player(); val pcm = ShortArray(960) { it.toShort() }; p.input.add(pcm); p.device.limit = 120
        p.step(0); p.step(5, true); p.step(10000, true)
        assertTrue(p.device.paused); assertEquals(1, p.device.calls); assertEquals(420, p.output.pendingFrames)
        p.device.limit = 960; p.step(10005)
        assertFalse(p.device.paused); assertEquals(pcm.toList(), p.device.samples)
    }

    @Test fun eofWaitsForPendingPcmAndPhysicalPlayback() {
        val p = Player(); p.input.add(ShortArray(960) { 7 }); p.finished = true; p.device.limit = 480
        p.step(0); p.device.head = 240; p.step(5)
        assertFalse(p.output.drained); assertEquals(480L, p.output.written)
        p.device.head = 480; p.step(10)
        assertTrue(p.output.drained); assertEquals(List(960) { 7.toShort() }, p.device.samples)
    }

    @Test fun shortEofPrimesOldDeviceWithoutAdvancingMediaClock() {
        val p = Player(Device(3848)); p.input.add(ShortArray(200) { 7 }); p.finished = true
        repeat(12) { p.step(it * 5L) }
        assertEquals(100L, p.output.written); assertEquals(0L, p.output.played)
        assertEquals(200 + 3848 * 2, p.device.samples.size)
        assertTrue(p.device.samples.drop(200).all { it == 0.toShort() })
        p.device.head = 200; p.step(65)
        assertEquals(100L, p.output.played); assertTrue(p.output.drained)
    }

    @Test fun eofAfterUnderrunAlsoPrimesOldDevice() {
        val p = Player(Device(3848)); p.input.add(ShortArray(960)); p.step(0)
        p.device.head = 480; p.step(5)
        p.input.add(ShortArray(100) { 9 }); p.finished = true
        repeat(12) { p.step(10 + it * 5L) }
        assertEquals(530L, p.output.written)
        assertTrue(p.device.samples.size / 2 - p.device.head >= p.device.threshold)
        p.device.head = 530; p.step(75); assertTrue(p.output.drained)
    }

    @Test fun resetDiscardsPendingAndNewOutputStartsAtZero() {
        val p = Player(); p.input.add(ShortArray(960) { 8 }); p.device.limit = 120; p.step(0); p.output.close()
        assertTrue(p.device.closed); assertEquals(0, p.output.pendingFrames)
        val next = Player(); next.input.add(ShortArray(960) { 9 }); next.step(10)
        assertEquals(480L, next.output.written); assertEquals(0L, next.output.played)
        assertTrue(next.device.samples.all { it == 9.toShort() })
    }

    @Test fun writeErrorContainsDeviceAndQueueState() {
        val p = Player(); p.input.add(ShortArray(960)); p.device.limit = -6
        val error = assertThrows(IllegalStateException::class.java) { p.step(0) }
        assertTrue(error.message!!.contains("write=-6")); assertTrue(error.message!!.contains("pending=480"))
    }

    @Test fun unexpectedHeadResetIsNotInterpretedAsFourBillionPlayedFrames() {
        val p = Player(); p.input.add(ShortArray(960)); p.step(0); p.device.head = 100; p.step(5)
        p.device.head = 0
        assertThrows(IllegalStateException::class.java) { p.step(10) }
    }

    @Test fun failedInitializationAndPlayKeepTheDeviceForCleanup() {
        for (fault in listOf("initialize", "play")) {
            val p = Player(); p.device.failures = setOf(fault); p.input.add(ShortArray(960))
            assertThrows(IllegalStateException::class.java) { p.step(0) }
            p.output.close(); assertTrue(p.device.closed); assertEquals(1, p.device.closeCalls)
        }
    }
    @Test fun failedCloseDoesNotDropTheDeviceOrAllowAnotherWrite() {
        val p = Player(); p.input.add(ShortArray(960)); p.step(0); p.device.failures = setOf("close")
        assertThrows(IllegalStateException::class.java) { p.output.close() }
        assertFalse(p.device.closed)
        assertThrows(IllegalStateException::class.java) { p.step(5) }
        p.device.failures = emptySet(); p.output.close(); p.output.close()
        assertTrue(p.device.closed); assertEquals(2, p.device.closeCalls)
    }
    @Test fun closeFailureAfterEffectStillRequiresAnAcknowledgedRetry() {
        val p = Player(); p.input.add(ShortArray(960)); p.step(0); p.device.failures = setOf("close after")
        assertThrows(IllegalStateException::class.java) { p.output.close() }
        assertTrue(p.device.closed); p.device.failures = emptySet(); p.output.close()
        assertEquals(2, p.device.closeCalls)
    }
}
