package com.utun.chat.ui.screens

import org.junit.Assert.*
import org.junit.Test

class VideoFrameFitTest {
    @Test fun fullscreenLandscapeUsesLongSideOfPortraitWindow() {
        val fit = fitVideoFrame(1080f, 2400f, 16f / 9f, true)
        assertEquals(90f, fit.rotation, 0f)
        assertEquals(1920f, fit.width, 0.001f); assertEquals(1080f, fit.height, 0.001f)
    }
    @Test fun thumbnailKeepsLandscapeOrientation() {
        val fit = fitVideoFrame(1080f, 2400f, 16f / 9f, false)
        assertEquals(0f, fit.rotation, 0f)
        assertEquals(1080f, fit.width, 0f); assertEquals(607.5f, fit.height, 0.001f)
    }
    @Test fun matchingOrientationsAndSquareDoNotRotate() {
        assertEquals(0f, fitVideoFrame(2400f, 1080f, 16f / 9f, true).rotation, 0f)
        assertEquals(0f, fitVideoFrame(1080f, 2400f, 9f / 16f, true).rotation, 0f)
        assertEquals(0f, fitVideoFrame(1080f, 2400f, 1f, true).rotation, 0f)
    }
    @Test fun everyFitKeepsAspectAndStaysInsideWindowAfterRotation() {
        for ((w, h) in listOf(1080f to 2400f, 2400f to 1080f, 144f to 104f))
            for (aspect in listOf(16f / 9f, 9f / 16f, 4f / 3f, 1f)) for (turn in listOf(false, true)) {
                val fit = fitVideoFrame(w, h, aspect, turn)
                assertEquals(aspect, fit.width / fit.height, 0.001f)
                assertTrue((if (fit.rotation == 0f) fit.width else fit.height) <= w + 0.001f)
                assertTrue((if (fit.rotation == 0f) fit.height else fit.width) <= h + 0.001f)
            }
    }
}
