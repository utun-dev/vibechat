package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class CallAudioTimingTest {
    @Test fun capturePendingIncludesAgeOfHardwareTimestamp() {
        assertEquals(960L, callCapturePendingFrames(4800, 4320, 970_000_000, 1_000_000_000))
        assertEquals(0L, callCapturePendingFrames(4800, 4800, 1_000_000_000, 1_000_000_000))
        assertNull(callCapturePendingFrames(4800, 4320, 100_000_000, 1_000_000_000))
        assertNull(callCapturePendingFrames(4800, -1, 970_000_000, 1_000_000_000))
    }

    @Test fun playbackUsesPresentationTimeAndMonotonicWrittenAnchor() {
        val c = CallPlaybackClock()
        val fallback = c.update(9600, 8640, 1_000_000, 1, 1, -1, -1)!!
        assertFalse(fallback.measured)
        assertEquals(1_020_000L, fallback.atUs)
        val hardware = c.update(10560, 9600, 1_020_000, 1, 1, 4800, 1_000_000)!!
        assertTrue(hardware.measured)
        assertTrue(hardware.position >= fallback.position)
        assertEquals(1_120_000L, hardware.atUs)
        // Повторный timestamp и временный отказ запроса сохраняют тот же физический ход часов.
        assertEquals(1_140_000L, c.update(11520, 10560, 1_040_000, 1, 1, -1, -1)!!.atUs)
        assertNull(c.update(35520, 34560, 1_600_000, 1, 1, -1, -1))
    }

    @Test fun playbackRouteRejectsOldTimestampWithoutFallingBackToHead() {
        val c = CallPlaybackClock()
        assertTrue(c.update(9600, 8640, 1_000_000, 1, 1, 4800, 1_000_000)!!.measured)
        assertNull(c.update(10560, 9600, 1_020_000, 2, 1, 5760, 1_020_000))
        assertEquals(1_060_000L, c.update(11520, 10560, 1_040_000, 2, 2, 10560, 1_040_000)!!.atUs)
    }

    @Test fun playbackTimestampWrapAndInvalidValues() {
        val c = CallPlaybackClock()
        val written = 0x100000000L + 960
        val a = c.update(written, written - 960, 1_000_000, 1, 1, 0, 1_000_000)!!
        assertTrue(a.measured)
        assertEquals(1_020_000L, a.atUs)
        assertNull(c.update(written, -1, 1_000_000, 1, 1, 0, 1_000_000))
        assertFalse(CallPlaybackClock().update(960, 0, 1_000_000, 1, 1, 1920, 1_000_000)!!.measured)
        assertFalse(CallPlaybackClock().update(960, 0, 1_000_000, 1, 1, 0, 10_000_000)!!.measured)
    }

    @Test fun captureFallbackUsesSamplesInsteadOfReadCompletionJitter() {
        val c = CallCaptureClock()
        assertEquals(980_000L, c.firstSampleUs(0, 960, 1_000_000, null, null))
        assertEquals(1_000_000L, c.firstSampleUs(960, 960, 1_035_000, null, null))
        assertFalse(c.measured)
        assertEquals(1_030_000L, c.firstSampleUs(1920, 960, 1_050_000, 2400, 1_040_000))
        assertTrue(c.measured)
        assertEquals(1_050_000L, c.firstSampleUs(2880, 960, 1_080_000, null, null))
        c.reset()
        assertFalse(c.measured)
        assertEquals(1_980_000L, c.firstSampleUs(0, 960, 2_000_000, null, null))
    }

    @Test fun invalidCaptureTimestampDoesNotReplaceAnchor() {
        val c = CallCaptureClock()
        assertEquals(980_000L, c.firstSampleUs(0, 960, 1_000_000, -1, 1_000_000))
        assertEquals(1_000_000L, c.firstSampleUs(960, 960, 1_020_000, 960, 2_000_000))
        assertFalse(c.measured)
    }

    @Test fun headMeasuresPlayedSamplesAndUnwrapsUnsignedCounter() {
        val h = CallPlaybackHead()
        assertEquals(0L, h.update(0, 960))
        assertEquals(240L, h.update(240, 960))
        assertEquals(240L, h.update(240, 1920))
        assertEquals(0x70000000L, h.update(0x70000000, 0x70000000L))
        assertEquals(0xe0000000L, h.update(0xe0000000.toInt(), 0xe0000000L))
        assertEquals(0x100000080L, h.update(128, 0x100000080L))
        assertFalse(h.failed)
    }

    @Test fun resetAndImpossibleHeadFailClosedUntilNewDevice() {
        val h = CallPlaybackHead()
        assertEquals(480L, h.update(480, 960))
        assertNull(h.update(0, 960))
        assertTrue(h.failed)
        assertNull(h.update(960, 960))
        assertNull(CallPlaybackHead().update(1000, 960))
        assertEquals(0L, CallPlaybackHead().update(0, 0))
    }
}
