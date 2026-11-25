package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class CallVideoBitrateRangeTest {
    @Test fun defaultsAndRoundTrip() {
        assertEquals("300:4000", CallVideoBitrateRange().encode())
        for (range in listOf(CallVideoBitrateRange(), CallVideoBitrateRange(500, 1500), CallVideoBitrateRange(300, 300))) {
            assertEquals(range, CallVideoBitrateRange.parse(range.encode()))
        }
    }

    @Test fun invalidPersistedRangeIsRejected() {
        for (value in listOf("", "299:4000", "300:4001", "4000:300", "300", "300:4000x",
            "-300:4000", "300:+4000", "300: 4000", "999999999999999999999999:4000")) {
            assertNull(value, CallVideoBitrateRange.parse(value))
        }
    }
}
