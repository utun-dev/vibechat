package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class CallOutputRecoveryTest {
    @Test fun routeChangeRestoresFastAttemptButDuplicateConfirmationDoesNot() {
        val p = CallOutputRecovery()
        assertTrue(p.route(3)); assertTrue(p.tryFast)
        assertTrue(p.failed()); assertFalse(p.tryFast)
        assertFalse(p.route(3)); assertFalse(p.tryFast)
        assertTrue(p.failed()); assertFalse(p.failed())
        assertTrue(p.route(2)); assertTrue(p.tryFast); assertEquals(0, p.failures)
        assertTrue(p.route(3)); assertTrue(p.tryFast)
    }

    @Test fun replacementKeepsAcceptedPcmPositionAndPartialBlockRemainder() {
        val p = CallOutputPosition()
        assertEquals(480L, p.played(480, 960))
        // 288 сэмплов следующего блока успели записаться; остаток 672 ещё принадлежит worker.
        val accepted = 960L + 288
        p.reopen(accepted)
        assertEquals(accepted, p.played(0, accepted))
        assertEquals(accepted + 96, p.played(96, accepted + 672))
        assertEquals(accepted + 48, p.timestamp(48, accepted + 672))
        assertEquals(-1L, p.timestamp(-1, accepted + 672))
        assertEquals(accepted + 0x100000030L, p.timestamp(48, accepted + 0x100000100L))
        assertNull(p.played(1000, accepted + 672))
        p.reopen(1920)
        assertEquals(1920L, p.played(0, 1920))
    }
}
