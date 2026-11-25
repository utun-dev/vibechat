package com.utun.chat.ui.screens

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import org.junit.Assert.assertEquals
import org.junit.Test

class VideoZoomStateTest {
    private val video = Size(1920f, 1080f)
    private val viewport = Size(2400f, 1080f)
    private val center = Offset(1200f, 540f)

    @Test fun pinchKeepsPointUnderFingers() {
        val zoom = VideoZoomState()
        zoom.transform(center + Offset(100f, 50f), Offset.Zero, 2f, video, viewport)
        assertEquals(2f, zoom.scale)
        assertEquals(Offset(-100f, -50f), zoom.offset)
    }

    @Test fun panCannotExposeSpaceBeyondVideoEdges() {
        val zoom = VideoZoomState()
        zoom.transform(center, Offset(10000f, -10000f), 2f, video, viewport)
        assertEquals(Offset(720f, -540f), zoom.offset)
    }

    @Test fun returningToOneCentersVideoAndPreventsDragging() {
        val zoom = VideoZoomState()
        zoom.transform(center, Offset(300f, 300f), 3f, video, viewport)
        zoom.transform(center, Offset(100f, 100f), 0.1f, video, viewport)
        assertEquals(1f, zoom.scale)
        assertEquals(Offset.Zero, zoom.offset)
    }

    @Test fun zoomLimitUsesActualScaleForAnchoring() {
        val zoom = VideoZoomState()
        zoom.transform(center, Offset.Zero, 3f, video, viewport)
        zoom.transform(center + Offset(300f, 0f), Offset.Zero, 2f, video, viewport)
        assertEquals(4f, zoom.scale)
        assertEquals(-100f, zoom.offset.x, 0.001f)
    }

    @Test fun changingVideoOrWindowSizeClampsExistingOffset() {
        val zoom = VideoZoomState()
        zoom.transform(center, Offset(700f, 500f), 2f, video, viewport)
        zoom.constrain(Size(600f, 1080f), viewport)
        assertEquals(Offset(0f, 500f), zoom.offset)
        zoom.constrain(Size(1080f, 607.5f), Size(1080f, 2400f))
        assertEquals(Offset.Zero, zoom.offset)
        assertEquals(2f, zoom.scale)
    }
}
