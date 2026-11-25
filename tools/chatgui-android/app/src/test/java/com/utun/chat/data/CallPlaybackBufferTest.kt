package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class CallPlaybackBufferTest {
    private class Device(val capacity: Int = 7680, val minimum: Int = 960) {
        val policy = CallPlaybackBuffer()
        var now = 0L
        var played = 0L
        var underruns = 0
        var frames = 1920
        val changes = mutableListOf<CallPlaybackBuffer.Change>()
        init { policy.reset(now, frames, capacity, underruns) }
        fun tick(advance: Boolean = true, duration: Long = 200): CallPlaybackBuffer.Change? {
            now += duration
            if (advance) played += duration * 48
            val change = policy.sample(now, underruns, played) ?: return null
            changes.add(change)
            if (change.frames != frames) {
                frames = change.frames.coerceIn(minimum, capacity)
                policy.applied(now, change, frames, capacity)
            }
            return change
        }
        fun run(ms: Long, advance: Boolean = true) { repeat((ms / 200).toInt()) { tick(advance) } }
    }

    @Test fun stableDeviceReachesOneBlockAndStopsProbing() {
        val d = Device()
        d.run(31_800)
        assertEquals(1920, d.frames)
        assertEquals("stable-probe", d.tick()?.reason)
        assertEquals(960, d.frames)
        d.run(300_000)
        assertEquals(1, d.changes.size)
    }

    @Test fun underrunGrowsImmediatelyAndDelaysNextProbe() {
        val d = Device()
        d.run(35_000)
        d.underruns++
        assertEquals("underrun", d.tick()?.reason)
        assertEquals(1920, d.frames)
        d.run(61_800)
        assertEquals(1920, d.frames)
        assertEquals("stable-probe", d.tick()?.reason)
        assertEquals(960, d.frames)
    }

    @Test fun startupAndRouteUnderrunsDoNotTeachWrongSize() {
        val d = Device()
        d.underruns = 3
        assertNull(d.tick())
        d.run(20_000)
        d.policy.settle(d.now) // Запрос маршрута ещё до framework callback.
        d.underruns = 7
        d.run(1_000, false)
        d.policy.reset(d.now, d.frames, d.capacity, d.underruns) // Подтверждённый новый выход.
        d.underruns = 9
        d.run(31_800)
        assertTrue(d.changes.isEmpty())
        assertEquals("stable-probe", d.tick()?.reason)
    }

    @Test fun acceptedHardwareMinimumIsNotProbedForever() {
        val d = Device(minimum = 1440)
        d.run(32_000)
        assertEquals(1440, d.frames)
        d.run(32_000)
        assertEquals(1440, d.frames)
        assertEquals(2, d.changes.size)
        d.run(300_000)
        assertEquals(2, d.changes.size)
    }

    @Test fun noPlaybackOrMissingObservationsCannotProveStability() {
        val d = Device()
        d.run(60_000, false)
        assertTrue(d.changes.isEmpty())
        d.run(20_000)
        d.tick(duration = 40_000)
        assertTrue(d.changes.isEmpty())
        d.run(29_800)
        assertEquals("stable-probe", d.tick()?.reason)
    }

    @Test fun realStarvationStillGrowsWhenHeadOrReporterStalls() {
        val d = Device()
        d.run(5_000)
        d.underruns++
        assertEquals("underrun", d.tick(false, 2_000)?.reason)
        assertEquals(2880, d.frames)
    }

    @Test fun growthStopsAtCapacityAndRateLimitsWarnings() {
        val d = Device(capacity = 1920)
        d.run(5_000)
        d.underruns++
        assertEquals("capacity-limit", d.tick()?.reason)
        repeat(49) { d.underruns++; assertNull(d.tick()) }
        assertEquals(1, d.changes.size)
        assertEquals(1920, d.frames)
    }

    @Test fun counterResetIsRebasedWithoutGrowingBuffer() {
        val d = Device()
        d.underruns = 10
        d.run(5_000)
        d.underruns = 0
        assertEquals("counter-reset", d.tick()?.reason)
        assertEquals(1920, d.frames)
        d.run(32_000)
        assertEquals(960, d.frames)
    }

    @Test fun firstUnderrunImmediatelyAfterShrinkRejectsProbe() {
        val d = Device()
        d.run(32_000)
        assertEquals(960, d.frames)
        d.underruns++
        assertEquals("underrun", d.tick()?.reason)
        assertEquals(1920, d.frames)
    }

    @Test fun repeatedFailuresBackOffToFiveMinutesButStillAllowRecovery() {
        val d = Device()
        d.run(32_000)
        val delays = listOf(60_000L, 120_000L, 240_000L, 300_000L, 300_000L)
        for (delay in delays) {
            d.underruns++
            assertEquals("underrun", d.tick()?.reason)
            assertEquals(1920, d.frames)
            d.run(delay + 1_800)
            assertEquals(1920, d.frames)
            assertEquals("stable-probe", d.tick()?.reason)
            assertEquals(960, d.frames)
        }
    }
}
