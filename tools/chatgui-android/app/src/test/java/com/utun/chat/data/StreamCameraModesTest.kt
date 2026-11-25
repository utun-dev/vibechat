package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class StreamCameraModesTest {
    private fun mode(camera: String, width: Int, height: Int, fps: Int) =
        StreamCaptureMode(camera, width, height, fps, fps, fps >= 120, "codec", 2_000_000)

    @Test fun cameraChangePreservesExactMode() {
        val previous = mode("back", 1920, 1080, 120)
        val modes = listOf(mode("front", 1280, 720, 30), mode("front", 1920, 1080, 120))
        assertEquals(modes[1], StreamCameraModes.choose(modes, previous))
    }

    @Test fun cameraChangeUsesLowerRateAtSameResolution() {
        val previous = mode("back", 1920, 1080, 240)
        val modes = listOf(mode("front", 1280, 720, 30), mode("front", 1920, 1080, 30), mode("front", 1920, 1080, 120))
        assertEquals(modes[2], StreamCameraModes.choose(modes, previous))
    }

    @Test fun defaultAndUnsupportedModeUse720p30() {
        val modes = listOf(mode("other", 640, 480, 30), mode("other", 1280, 720, 30), mode("other", 1920, 1080, 120))
        assertEquals(modes[1], StreamCameraModes.choose(modes))
        assertEquals(modes[1], StreamCameraModes.choose(modes, mode("back", 1920, 1080, 30)))
        assertNull(StreamCameraModes.choose(emptyList()))
    }
}
