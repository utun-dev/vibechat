package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class CallVideoSettingsTest {
    private val sd = CallVideoMode(640, 480, 30)
    private val hd = CallVideoMode(1280, 720, 60)
    private val fullHd = CallVideoMode(1920, 1080, 30)
    private val state = CallVideoState(ready = true, cameras = listOf(
        CallVideoCamera("front", "Front", listOf(sd, fullHd), 90, true, 25f),
        CallVideoCamera("back", "Back", listOf(sd, hd, fullHd), 90, false, 24f)),
        codecs = listOf(1, 2), codecModes = mapOf(1 to listOf(sd, fullHd), 2 to listOf(sd, hd)))

    @Test fun savedCameraCodecAndCadenceRestoreWithoutEnablingTransmission() {
        val saved = CallVideoSettings(true, "back", 1280, 720, 2, 60)
        assertEquals(saved.copy(enabled = false), saved.restore(state))
    }

    @Test fun unsupportedCadenceKeepsResolutionWithAvailableLowerRate() {
        val saved = CallVideoSettings(camera = "front", width = 1920, height = 1080, codec = 1, fps = 60)
        assertEquals(saved.copy(fps = 30), saved.restore(state))
    }

    @Test fun missingCameraAndCodecChooseUsableAutoMode() {
        val saved = CallVideoSettings(camera = "gone", width = 1920, height = 1080, codec = 5)
        assertEquals(saved.copy(camera = "front", codec = 0), saved.restore(state))
    }

    @Test fun listedCodecWithoutCameraModesFallsBackToAuto() {
        val saved = CallVideoSettings(camera = "back", codec = 2)
        val available = state.copy(codecModes = mapOf(1 to listOf(sd, fullHd), 2 to emptyList()))
        assertEquals(saved.copy(codec = 0), saved.restore(available))
    }

    @Test fun missingModesProduceDisabledReceiveOnlySettings() {
        val restored = CallVideoSettings(true, "gone", 1920, 1080, 2, 60).restore(CallVideoState())
        assertEquals(CallVideoSettings(), restored)
    }
}
