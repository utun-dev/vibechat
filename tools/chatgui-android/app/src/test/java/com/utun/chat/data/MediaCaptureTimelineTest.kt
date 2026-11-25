package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class MediaCaptureTimelineTest {
    private val origin = 1_000_000_000L
    private fun frame(time: Long, value: Int, epoch: Long = 1, sequence: Long = (time - origin) * 48000 / 1_000_000_000) =
        MicrophoneCapture.Frame(ShortArray(1920) { value.toShort() }, time, sequence, generation = epoch)
    private fun pull(timeline: MediaCaptureTimeline) = timeline.pull(timeline.deadlineNs)

    @Test fun captureMarkerDistinguishesHardwareSilenceFromStartupGapsAndRouteReplacement() {
        val timeline = MediaCaptureTimeline(origin)
        timeline.reset(1)
        assertFalse(pull(timeline).hasCapture)
        assertTrue(timeline.offer(frame(origin + 20_000_000, 0)))
        val realSilence = pull(timeline)
        assertTrue(realSilence.hasCapture)
        assertTrue(realSilence.pcm.all { it == 0.toShort() })
        assertFalse(pull(timeline).hasCapture)
        timeline.reset(2)
        assertFalse(pull(timeline).hasCapture)
        assertTrue(timeline.offer(frame(origin + 90_000_000, 0, 2)))
        assertTrue(pull(timeline).hasCapture) // Вторая половина блока уже содержит реальный захват.
    }

    @Test fun switchingKeepsTimeAndSequenceAndRejectsOldDevice() {
        val timeline = MediaCaptureTimeline(origin)
        timeline.reset(1)
        assertTrue(timeline.offer(frame(origin, 11)))
        val first = pull(timeline)
        timeline.reset(2)
        assertFalse(timeline.offer(frame(origin + 20_000_000, 99)))
        val silence = pull(timeline)
        assertTrue(silence.pcm.all { it == 0.toShort() })
        assertTrue(timeline.offer(frame(origin + 40_000_000, 22, 2)))
        val next = pull(timeline)
        assertEquals(first.timeNs + 40_000_000, next.timeNs)
        assertEquals(1920L, next.sequence)
        assertTrue(next.pcm.all { it == 22.toShort() })
    }

    @Test fun differentlyAlignedMicrophoneFramesFillTheirActualTime() {
        val timeline = MediaCaptureTimeline(origin)
        timeline.reset(1)
        assertTrue(timeline.offer(frame(origin + 10_000_000, 1)))
        assertTrue(timeline.offer(frame(origin + 30_000_000, 2)))
        val first = pull(timeline).pcm
        val second = pull(timeline).pcm
        assertTrue(first.take(960).all { it == 0.toShort() })
        assertTrue(first.drop(960).all { it == 1.toShort() })
        assertTrue(second.take(960).all { it == 1.toShort() })
        assertTrue(second.drop(960).all { it == 2.toShort() })
    }

    @Test fun lateSoundIsNeverRetimestampedOverPreviouslySentSilence() {
        val timeline = MediaCaptureTimeline(origin)
        timeline.reset(1)
        repeat(20) { pull(timeline) }
        assertFalse(timeline.offer(frame(origin, 123)))
        assertEquals(1L, timeline.lateFrames)
        val result = pull(timeline)
        assertEquals(origin + 400_000_000, result.timeNs)
        assertTrue(result.pcm.all { it == 0.toShort() })
    }

    @Test fun deviceOpeningDelayProducesContinuousTwentyMillisecondPackets() {
        val timeline = MediaCaptureTimeline(origin)
        timeline.reset(1)
        repeat(1500) { index ->
            if (index % 50 == 0) timeline.reset(index.toLong() + 1)
            val result = pull(timeline)
            assertEquals(origin + index * 20_000_000L, result.timeNs)
            assertEquals(index * 960L, result.sequence)
            assertEquals(1920, result.pcm.size)
        }
    }

    @Test fun stalePacketsCannotGrowTheCaptureQueueWithoutBound() {
        val timeline = MediaCaptureTimeline(origin)
        timeline.reset(1)
        repeat(24) { assertTrue(timeline.offer(frame(origin + it * 20_000_000L, 1))) }
        assertFalse(timeline.offer(frame(origin + 480_000_000L, 2)))
        assertEquals(1L, timeline.lateFrames)
    }

    @Test fun timestampJitterDoesNotInsertSilenceOrChangeAnySample() {
        for (jitter in listOf(20_000L, 100_000L, 350_000L)) {
            val timeline = MediaCaptureTimeline(origin)
            timeline.reset(1)
            repeat(510) { index ->
                val pcm = ShortArray(1920) { ((index * 1920 + it) % 30000 + 1).toShort() }
                val offset = if (index == 0) 0 else if (index % 2 == 0) jitter else -jitter
                assertTrue(timeline.offer(MicrophoneCapture.Frame(pcm, origin + index * 20_000_000L + offset,
                    index * 960L, generation = 1)))
                if (index >= 10) {
                    val output = pull(timeline)
                    val expected = ShortArray(1920) { (((index - 10) * 1920 + it) % 30000 + 1).toShort() }
                    assertArrayEquals("jitter=$jitter block=$index", expected, output.pcm)
                    assertEquals(0L, timeline.missingFrames)
                    assertEquals(0, timeline.ratePpm)
                }
            }
        }
    }

    @Test fun actualSequenceGapIsSilenceAndLaterSoundKeepsItsPosition() {
        val timeline = MediaCaptureTimeline(origin)
        timeline.reset(1)
        assertTrue(timeline.offer(frame(origin, 11)))
        assertTrue(timeline.offer(frame(origin + 40_000_000, 22)))
        assertTrue(pull(timeline).pcm.all { it == 11.toShort() })
        assertTrue(pull(timeline).pcm.all { it == 0.toShort() })
        assertTrue(pull(timeline).pcm.all { it == 22.toShort() })
        assertEquals(960L, timeline.missingFrames)
    }

    @Test fun slowClockDriftIsResampledWithoutHolesOrSharpEdges() {
        for (ppm in listOf(-300.0, 300.0)) {
            val timeline = MediaCaptureTimeline(origin)
            timeline.reset(1)
            var sent = 0
            var previous = 0
            repeat(15000) { output ->
                val until = output * 20_000_000L + MediaCaptureTimeline.RESERVE_NS
                while (sent * 20_000_000.0 * (1 + ppm / 1_000_000) <= until) {
                    val pcm = ShortArray(1920) { channel ->
                        (10000 + 4000 * kotlin.math.sin(2 * Math.PI * 200 * (sent * 960L + channel / 2) / 48000)).toInt().toShort()
                    }
                    assertTrue(timeline.offer(MicrophoneCapture.Frame(pcm,
                        origin + (sent * 20_000_000.0 * (1 + ppm / 1_000_000)).toLong(), sent * 960L, generation = 1)))
                    sent++
                }
                val frame = pull(timeline)
                for (i in 0 until 960) {
                    val sample = frame.pcm[i * 2].toInt()
                    assertTrue("zero/hole ppm=$ppm block=$output", sample >= 5999)
                    if (previous != 0) assertTrue("click ppm=$ppm block=$output", kotlin.math.abs(sample - previous) <= 110)
                    assertEquals(frame.pcm[i * 2], frame.pcm[i * 2 + 1])
                    previous = sample
                }
                assertEquals(0L, timeline.missingFrames)
            }
            assertTrue("clock correction ppm=$ppm actual=${timeline.ratePpm}", kotlin.math.abs(timeline.ratePpm + ppm) < 20)
        }
    }

    @Test fun bluetoothTimestampJumpsDoNotInterruptContinuousPcm() {
        val timeline = MediaCaptureTimeline(origin)
        timeline.reset(1)
        assertTrue(timeline.offer(frame(origin, 11)))
        assertTrue(timeline.offer(frame(origin + 247_000_000, 22, sequence = 960)))
        assertTrue(timeline.offer(frame(origin + 40_000_000, 33, sequence = 1920)))
        assertTrue(pull(timeline).pcm.all { it == 11.toShort() })
        assertTrue(pull(timeline).pcm.all { it == 22.toShort() })
        assertTrue(pull(timeline).pcm.all { it == 33.toShort() })
        assertEquals(2L, timeline.timestampJumps)
        assertEquals(0L, timeline.missingFrames)
        assertEquals(0, timeline.ratePpm)
    }

    @Test fun realCaptureStallReanchorsFreshPcmInsteadOfRejectingEveryFollowingBlock() {
        val timeline = MediaCaptureTimeline(origin)
        timeline.reset(1)
        assertTrue(timeline.offer(MicrophoneCapture.Frame(ShortArray(1920) { 11 }, origin, 0, origin + 20_000_000, 1)))
        assertTrue(pull(timeline).pcm.all { it == 11.toShort() })
        repeat(10) { pull(timeline) }
        assertTrue(timeline.offer(MicrophoneCapture.Frame(ShortArray(1920) { 22 }, origin + 240_000_000,
            960, origin + 260_000_000, 1)))
        assertTrue(pull(timeline).pcm.all { it == 0.toShort() })
        assertTrue(pull(timeline).pcm.all { it == 22.toShort() })
        assertEquals(1L, timeline.timestampJumps)
        assertEquals(0L, timeline.lateFrames)
    }
}
