package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class CameraCaptureSettingsTest {
    private val main = LensOption(LensType.MAIN, "back", 24f)
    private val selfie = LensOption(LensType.SELFIE, "front", 25f)
    private val wide = LensOption(LensType.ULTRA_WIDE, "wide", 11f)
    private val lenses = listOf(selfie, main, wide)

    @Test fun freshPhotoAndVideoChooseSeparateDefaults() {
        assertEquals(main, PhotoCaptureSettings().resolveCamera(lenses))
        assertEquals(85, PhotoCaptureSettings().jpegQuality)
        assertEquals(selfie, VideoCaptureSettings().resolveCamera(lenses))
        assertEquals(720, VideoCaptureSettings().height)
        assertEquals(StreamOrientation.AUTO, PhotoCaptureSettings().orientation)
        assertEquals(StreamOrientation.AUTO, VideoCaptureSettings().orientation)
    }

    @Test fun savedCamerasOverrideDefaultsAndMissingCamerasFallBackPerMode() {
        assertEquals(selfie, PhotoCaptureSettings(camera = "front").resolveCamera(lenses))
        assertEquals(wide, VideoCaptureSettings(camera = "wide").resolveCamera(lenses))
        assertEquals(main, PhotoCaptureSettings(camera = "gone").resolveCamera(lenses))
        assertEquals(selfie, VideoCaptureSettings(camera = "gone").resolveCamera(lenses))
        assertNull(PhotoCaptureSettings().resolveCamera(emptyList()))
        assertNull(VideoCaptureSettings().resolveCamera(emptyList()))
    }

    @Test fun exactPhotoResolutionRestoresAndUnsupportedSizeUsesLargestAvailable() {
        val native = PhotoResolution(4000, 3000)
        val hd = PhotoResolution(1280, 720)
        val sizes = listOf(native, PhotoResolution(640, 480), hd)
        assertEquals(hd, PhotoCaptureSettings(width = 1280, height = 720).resolveResolution(sizes))
        assertEquals(native, PhotoCaptureSettings().resolveResolution(sizes))
        assertEquals(native, PhotoCaptureSettings(width = 8000, height = 6000).resolveResolution(sizes))
        assertNull(PhotoCaptureSettings().resolveResolution(emptyList()))
    }

    @Test fun invalidStoredPresetsAreRejected() {
        for (quality in listOf(10, 25, 101)) assertThrows(IllegalArgumentException::class.java) { PhotoCaptureSettings(jpegQuality = quality) }
        assertThrows(IllegalArgumentException::class.java) { PhotoCaptureSettings(width = 1280) }
        assertThrows(IllegalArgumentException::class.java) { VideoCaptureSettings(height = 2160) }
        assertThrows(IllegalArgumentException::class.java) { VideoCaptureSettings(codec = 5) }
        assertThrows(IllegalArgumentException::class.java) { VideoCaptureSettings(bitrate = -1) }
    }
}
