package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.util.ArrayDeque

class StreamDecodePumpTest {
    private data class Frame(override val pts: Long) : StreamDecodePump.Frame
    private class Codec(val depth: Int) {
        val queued = ArrayDeque<Frame>()
        var released = 0
        var blocked = false
        var finishing = false
        var held = false
        var nextIndex = 0
        val pump = StreamDecodePump<Frame>(
            submit = { if (blocked) false else { queued.addLast(it); true } },
            receive = {
                if (held || (queued.size < depth && !finishing) || queued.isEmpty()) null
                else { held = true; StreamDecodePump.Output(nextIndex++, queued.removeFirst().pts) }
            },
            release = { _, _ -> check(held); held = false; released++ }
        )
    }

    @Test fun fourAndEightFramePipelinesStartWithFrozenPresentationClock() {
        for (depth in listOf(4, 8)) {
            val codec = Codec(depth)
            repeat(depth) { i ->
                assertTrue(codec.pump.wantsInput)
                codec.pump.offer(Frame(i * 66666L), i * 66666L)
                codec.pump.step(0, i * 66666L)
            }
            assertEquals(depth.toLong(), codec.pump.inputCount)
            assertEquals(1L, codec.pump.outputCount)
            assertEquals(1, codec.released)
            assertEquals(0L, codec.pump.firstOutputPts)
            assertEquals((depth - 1) * 66666L, codec.pump.decoderLeadUs)
        }
    }

    @Test fun futureOutputStopsReadsAndNeverReleasesEarly() {
        val codec = Codec(1)
        codec.pump.offer(Frame(100000), 0); codec.pump.step(0, 0)
        assertFalse(codec.pump.wantsInput); assertNotNull(codec.pump.output); assertEquals(0, codec.released)
        codec.pump.step(99999, 15_000_000); codec.pump.checkProgress(15_000_000, false)
        assertEquals(0, codec.released)
        codec.pump.step(100000, 15_000_001)
        assertEquals(1, codec.released); assertTrue(codec.pump.wantsInput)
    }

    @Test fun blockedCodecKeepsOneInputAndReportsStall() {
        val codec = Codec(4); codec.blocked = true
        codec.pump.offer(Frame(0), 0); codec.pump.step(0, 0)
        assertNotNull(codec.pump.input); assertFalse(codec.pump.wantsInput)
        try { codec.pump.checkProgress(10_000_001, false); fail("Stalled decoder must fail") }
        catch (expected: IllegalStateException) { assertTrue(expected.message!!.contains("no output progress")) }
    }

    @Test fun outstandingReadCanArriveAfterFutureOutputWithoutAnotherRead() {
        val codec = Codec(1)
        codec.pump.offer(Frame(100000), 0); codec.pump.step(0, 0)
        codec.pump.offer(Frame(140000), 1)
        assertFalse(codec.pump.wantsInput); codec.pump.step(0, 1)
        assertEquals(1L, codec.pump.inputCount); assertNotNull(codec.pump.input)
        codec.pump.step(100000, 100000)
        assertEquals(2L, codec.pump.inputCount); assertEquals(140000L, codec.pump.output!!.pts)
        codec.pump.step(140000, 140000); assertEquals(2, codec.released)
    }

    @Test fun networkPauseKeepsReferencesAndAllowsIncompletePipelineToResume() {
        val codec = Codec(4)
        repeat(2) { i -> codec.pump.offer(Frame(i * 40000L), i * 40000L); codec.pump.step(0, i * 40000L) }
        codec.pump.checkProgress(20_000_000, false)
        repeat(2) { i ->
            val now = 20_000_000 + i * 40000L
            codec.pump.offer(Frame((i + 2) * 40000L), now); codec.pump.step(0, now); codec.pump.checkProgress(now, false)
        }
        assertEquals(4L, codec.pump.inputCount); assertEquals(1, codec.released)
    }

    @Test fun finishDrainsShortStreamAndPreservesTheLastPicture() {
        val codec = Codec(8)
        repeat(3) { i -> codec.pump.offer(Frame(i * 40000L), i * 40000L); codec.pump.step(0, i * 40000L) }
        assertEquals(0, codec.released); codec.finishing = true
        codec.pump.step(0, 120000); assertEquals(1, codec.released); assertFalse(codec.pump.wantsInput)
        codec.pump.step(40000, 160000); assertEquals(2, codec.released)
        codec.pump.step(80000, 200000); assertEquals(3, codec.released)
    }

    @Test fun drainingDoesNotEraseTheObservedDecoderGuard() {
        val codec = Codec(4)
        repeat(4) { i -> codec.pump.offer(Frame(i * 40000L), i * 40000L); codec.pump.step(0, i * 40000L) }
        assertEquals(120000L, codec.pump.decoderLeadUs); codec.finishing = true
        codec.pump.step(120000, 200000)
        assertEquals(4L, codec.pump.outputCount); assertEquals(120000L, codec.pump.decoderLeadUs)
    }

    @Test fun emptyEosReleasesTheBufferAndClearsInputsWithoutDisplayPictures() {
        var finishing = false; var eof = false; var released = 0
        val pump = StreamDecodePump<Frame>(submit = { true }, receive = {
            if (finishing && !eof) { eof = true; StreamDecodePump.Output(0, 80000, eos = true, empty = true) } else null
        }, release = { _, render -> assertFalse(render); released++ })
        repeat(3) { i -> pump.offer(Frame(i * 40000L), i * 40000L); pump.step(0, i * 40000L) }
        assertEquals(3, pump.inflight); finishing = true; pump.step(0, 120000)
        assertTrue(pump.ended); assertEquals(0, pump.inflight); assertEquals(-1L, pump.oldestSubmittedPts)
        assertEquals(1, released); pump.checkProgress(30_000_000, true)
    }

    @Test fun regressedInputPtsIsRejectedBeforeTheCodecReceivesIt() {
        val codec = Codec(1); codec.pump.offer(Frame(100), 0); codec.pump.step(100, 0)
        codec.pump.offer(Frame(100), 1)
        assertThrows(IllegalStateException::class.java) { codec.pump.step(100, 1) }
        assertTrue(codec.queued.isEmpty()); assertEquals(1L, codec.pump.inputCount)
    }
}
