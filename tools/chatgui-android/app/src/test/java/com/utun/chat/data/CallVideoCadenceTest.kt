package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class CallVideoCadenceTest {
    @Test fun fasterCameraPreservesSelectedOutputRate() {
        for (source in listOf(50, 60, 100, 330)) for (fps in listOf(30, 50, 60).filter { it <= source }) {
            val cadence = CallVideoCadence(fps)
            val accepted = (0 until source * 10).count { cadence.accept(1_000_000L + it * 1_000_000L / source) }
            assertTrue("source=$source output=$fps accepted=$accepted", kotlin.math.abs(accepted - fps * 10) <= 1)
        }
    }
    @Test fun jitterAndPauseDoNotCauseHalvingOrBurst() {
        for (fps in listOf(30, 50, 60)) {
            val cadence = CallVideoCadence(fps)
            repeat(300) { assertTrue(cadence.accept(1_000_000L + it * 1_000_000L / fps + if (it % 2 == 0) 700 else -700)) }
            assertTrue(cadence.accept(20_000_000)); assertFalse(cadence.accept(20_000_001))
            assertFalse(cadence.accept(20_000_000)); assertFalse(cadence.accept(Long.MAX_VALUE))
        }
    }
    @Test fun advertisedModesKeepSizeAndRateTogether() {
        val low = CallVideoMode(1280, 720, 60); val high = CallVideoMode(1920, 1080, 30)
        val state = CallVideoState(cameras = listOf(CallVideoCamera("c", "camera", listOf(low, high), 90, false, 26f)),
            codecModes = mapOf(1 to listOf(high), 2 to listOf(low)))
        assertEquals(listOf(low, high), state.modes("c", 0))
        assertEquals(listOf(high), state.modes("c", 1))
        assertEquals(listOf(low), state.modes("c", 2))
        assertFalse(CallVideoMode(1920, 1080, 60) in state.modes("c", 0))
    }
}
