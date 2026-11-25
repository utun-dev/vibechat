package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class StreamPlaybackTelemetryTest {
    @Test fun nodeNamesUseEightWholeUnicodeCharactersAndOneEllipsis() {
        assertEquals("Источник", StreamBufferNode("Источник", 600000).shortName)
        assertEquals("abcdefgh…", StreamBufferNode("abcdefghijk", 600000).shortName)
        assertEquals("😀abcdefg…", StreamBufferNode("😀abcdefgh", 600000).shortName)
    }

    @Test fun unknownIsDifferentFromZeroAndUnavailableKeepsTheLastTime() {
        assertEquals("?", StreamBufferNode("node", null).time)
        assertEquals("0S", StreamBufferNode("node", 0).time)
        assertEquals("0.6S", StreamBufferNode("node", 600000, stale = true, down = true).time)
        assertEquals("2S", StreamBufferNode("self", 2000000).time)
        assertNull(StreamPlaybackTelemetry(tempoMilli = 1000).speed)
        assertEquals("1.1x", StreamPlaybackTelemetry(tempoMilli = 1100).speed)
        assertEquals("1.12x", StreamPlaybackTelemetry(tempoMilli = 1120).speed)
    }
}
