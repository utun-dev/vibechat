package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class CallVideoOrientationTest {
    @Test fun sensorAnglesUseOppositeDirectionToDisplayAngles() {
        for (sensor in listOf(0, 90, 180, 270)) for (angle in listOf(0, 90, 180, 270)) {
            val orientation = CallVideoOrientation()
            assertTrue(orientation.observe(angle, 0))
            assertEquals((sensor + angle) % 360, orientation.rotation(sensor, false))
            assertEquals((sensor - angle + 360) % 360, orientation.rotation(sensor, true))
        }
    }

    @Test fun boundaryJitterDoesNotTurnFrames() {
        val orientation = CallVideoOrientation()
        orientation.observe(0, 0)
        for (angle in listOf(44, 46, 55, 46, 44)) { orientation.observe(angle, 100); assertFalse(orientation.confirm(1000)) }
        orientation.observe(65, 1000)
        assertFalse(orientation.confirm(1249))
        assertTrue(orientation.confirm(1250))
        assertEquals(90, orientation.degrees)
        orientation.observe(40, 1300)
        assertFalse(orientation.confirm(2000))
    }

    @Test fun flatPositionAndCancelledTurnKeepLastDirection() {
        val orientation = CallVideoOrientation()
        orientation.observe(270, 0)
        orientation.observe(350, 100)
        orientation.observe(-1, 200)
        assertFalse(orientation.confirm(500))
        assertEquals(270, orientation.degrees)
        orientation.observe(350, 600)
        assertTrue(orientation.confirm(850))
        assertEquals(0, orientation.degrees)
        orientation.observe(180, 900)
        orientation.resetPending()
        assertFalse(orientation.confirm(1200))
    }
}
