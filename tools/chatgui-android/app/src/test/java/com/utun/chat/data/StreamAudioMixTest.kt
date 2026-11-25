package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.sin

class StreamAudioMixTest {
    @Test fun reusedPcmOnlyMixesTheNewlyPreparedPrefix() {
        val mix = StreamMixBuffer()
        val pcm = ShortArray(960) { 12000 }
        mix.clear(); assertEquals(480, mix.add(pcm, 480, 120))
        val output = mix.finish()
        for (i in output.indices) assertEquals(if (i < 240) 12000 else 0, output[i].toInt())
        mix.clear(); mix.add(pcm, 480, 0)
        assertTrue(mix.finish().all { it == 0.toShort() })
    }

    @Test fun joiningAndResetUseTheirOwnOutputOrigin() {
        val first = StreamMixTimeline()
        first.append(0, 480); first.append(480, 480)
        val joined = StreamMixTimeline()
        joined.append(960, 480); first.append(960, 480)
        assertEquals(1200, first.played(1200))
        assertEquals(240, joined.played(1200))
        assertEquals(0, joined.played(700)) // Устройство ещё до подключения второго источника.
        val reset = StreamMixTimeline()
        reset.append(1440, 480); first.append(1440, 480)
        assertEquals(1600, first.played(1600))
        assertEquals(160, reset.played(1600))
        assertEquals(480, joined.played(1600))
    }

    @Test fun partialDeviceWritesAdvanceOnlyHeardFrames() {
        val clock = StreamMixTimeline()
        clock.append(2000, 480)
        for (head in listOf(1900L, 2000L, 2120L, 2240L, 2480L, 3000L))
            assertEquals((head - 2000).coerceIn(0, 480), clock.played(head))
    }

    @Test fun differentTonesAreSummedAndSilentInputDoesNotReduceVolume() {
        val a = ShortArray(960) { (4000 * sin(2 * PI * 1000 * (it / 2) / 48000)).toInt().toShort() }
        val b = ShortArray(960) { (2000 * sin(2 * PI * 500 * (it / 2) / 48000)).toInt().toShort() }
        val mix = StreamMixBuffer()
        mix.clear(); mix.add(a); mix.add(b)
        val sum = mix.finish().copyOf()
        for (i in sum.indices) assertTrue(abs(sum[i].toInt() - a[i] - b[i]) <= 1)
        mix.clear(); mix.add(a); mix.add(ShortArray(960))
        assertArrayEquals(a, mix.finish())
    }

    @Test fun limiterKeepsStereoBalanceAndReleasesSmoothly() {
        val mix = StreamMixBuffer()
        val loud = ShortArray(960) { if (it % 2 == 0) 24000 else 12000 }
        mix.clear(); mix.add(loud); mix.add(loud)
        val limited = mix.finish().copyOf()
        assertEquals(32767, limited[0].toInt()); assertEquals(16384, limited[1].toInt())
        mix.clear(); mix.add(loud)
        val quieter = mix.finish()
        assertTrue(quieter.first() < 18000); assertTrue(quieter.last() < 12000)
        assertTrue(quieter[958] > quieter[0])
    }

    @Test fun accidentalHoleInOutputMappingIsRejected() {
        val clock = StreamMixTimeline(); clock.append(1000, 480)
        try { clock.append(1500, 480); fail("Gap must not silently move this source's clock") }
        catch (_: IllegalStateException) {}
    }
}
