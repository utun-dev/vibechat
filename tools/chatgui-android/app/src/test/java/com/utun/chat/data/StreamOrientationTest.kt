package com.utun.chat.data

import org.junit.Assert.assertEquals
import org.junit.Test

class StreamOrientationTest {
    private val manual = StreamOrientation.values().filter { it != StreamOrientation.AUTO }

    @Test fun rearCameraRotationFollowsManualOrientation() {
        assertEquals(listOf(90, 0, 270, 180), manual.map { it.sensorRotation(90, false) })
        assertEquals(listOf(270, 180, 90, 0), manual.map { it.sensorRotation(270, false) })
    }

    @Test fun frontCameraUsesOppositeSensorCompensation() {
        assertEquals(listOf(270, 0, 90, 180), manual.map { it.sensorRotation(270, true) })
        assertEquals(listOf(90, 180, 270, 0), manual.map { it.sensorRotation(90, true) })
    }

    @Test fun camerasWithZeroSensorRotationKeepAllFourDirections() {
        assertEquals(listOf(0, 270, 180, 90), manual.map { it.sensorRotation(0, false) })
        assertEquals(listOf(0, 90, 180, 270), manual.map { it.sensorRotation(0, true) })
    }

    @Test fun autoMatchesVideoCallsForEveryCameraAndDeviceDirection() {
        for (sensor in listOf(0, 90, 180, 270)) for (angle in listOf(0, 90, 180, 270)) for (front in listOf(false, true)) {
            val orientation = CallVideoOrientation()
            orientation.observe(angle, 0)
            assertEquals(orientation.rotation(sensor, front), StreamOrientation.AUTO.sensorRotation(sensor, front, orientation.degrees))
        }
    }

    @Test fun manualOrientationIgnoresDeviceDirection() {
        for (option in manual) for (angle in listOf(0, 90, 180, 270)) for (front in listOf(false, true)) {
            assertEquals(option.sensorRotation(90, front), option.sensorRotation(90, front, angle))
        }
    }

    @Test fun cameraXTargetMatchesStreamForFrontAndRearSensors() {
        for (option in StreamOrientation.entries) for (device in listOf(0, 90, 180, 270)) {
            val target = option.targetDegrees(device)
            assertEquals(if (option == StreamOrientation.AUTO) (360 - device) % 360 else option.degrees, target)
            for (sensor in listOf(0, 90, 180, 270)) {
                assertEquals((sensor - target + 360) % 360, option.sensorRotation(sensor, false, device))
                assertEquals((sensor + target) % 360, option.sensorRotation(sensor, true, device))
            }
        }
    }

    @Test fun newCaptureUsesAutoByDefault() {
        val mode = StreamCaptureMode("camera", 1280, 720, 30, 30, false, "encoder", 2_000_000)
        assertEquals(StreamOrientation.AUTO, StreamCaptureConfig(mode).orientation)
        assertEquals(StreamOrientation.AUTO.degrees, StreamCaptureSettings().rotation)
    }
}
