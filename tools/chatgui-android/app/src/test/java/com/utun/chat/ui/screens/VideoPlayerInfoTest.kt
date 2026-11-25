package com.utun.chat.ui.screens

import com.utun.chat.data.VideoInfo

import org.junit.Assert.assertEquals
import org.junit.Test

class VideoPlayerInfoTest {
    @Test fun portraitRecordingUsesRotatedDimensionsForScreenAndFit() {
        for (rotation in listOf(90, 270)) {
            val info = VideoInfo(1280, 720, rotation)
            assertEquals(720, info.displayWidth)
            assertEquals(1280, info.displayHeight)
            assertEquals(9f / 16f, info.aspect, 0.0001f)
        }
    }

    @Test fun landscapeRecordingKeepsDimensionsForBothDirections() {
        for (rotation in listOf(0, 180)) {
            val info = VideoInfo(1280, 720, rotation)
            assertEquals(1280, info.displayWidth)
            assertEquals(720, info.displayHeight)
            assertEquals(16f / 9f, info.aspect, 0.0001f)
        }
    }
}
