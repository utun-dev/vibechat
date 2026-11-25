package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class StreamPreviewTest {
    @Test fun cameraPreviewUsesSensorAndDisplayOrientation() {
        val rear = StreamPreviewFormat(1280, 720, StreamPreviewCamera(90, false))
        val front = StreamPreviewFormat(1280, 720, StreamPreviewCamera(270, true))
        assertEquals(9f / 16f, rear.aspect(0), 0f)
        assertEquals(16f / 9f, rear.aspect(90), 0f)
        assertEquals(9f / 16f, front.aspect(0), 0f)
        assertEquals(16f / 9f, front.aspect(90), 0f)
        assertArrayEquals(floatArrayOf(0f, 0f, 1f, 0f, 1f, 1f, 0f, 1f), rear.corners(0), 0f)
        assertArrayEquals(rear.corners(0), front.corners(0), 0f)
        assertArrayEquals(floatArrayOf(0f, 1f, 0f, 0f, 1f, 0f, 1f, 1f), rear.corners(90), 0f)
        assertArrayEquals(rear.corners(90), front.corners(90), 0f)
    }

    @Test fun textureViewNeverAppliesSensorRotationTwice() {
        for (front in listOf(false, true)) for (sensor in listOf(0, 90, 180, 270)) {
            val format = StreamPreviewFormat(1280, 720, StreamPreviewCamera(sensor, front))
            assertEquals(0, format.textureRotation(0))
            assertEquals(270, format.textureRotation(90))
            assertEquals(180, format.textureRotation(180))
            assertEquals(90, format.textureRotation(270))
        }
    }

    @Test fun allPreviewRotationsPreserveRectangleCorners() {
        for (front in listOf(false, true)) for (sensor in listOf(0, 90, 180, 270)) for (display in listOf(0, 90, 180, 270)) {
            val format = StreamPreviewFormat(640, 480, StreamPreviewCamera(sensor, front))
            val corners = format.corners(display).toList().chunked(2)
            assertEquals(setOf(listOf(0f, 0f), listOf(1f, 0f), listOf(1f, 1f), listOf(0f, 1f)), corners.toSet())
            assertEquals(if (format.camera.rotation(display) % 180 == 0) 4f / 3f else 3f / 4f, format.aspect(display), 0f)
        }
    }

    @Test fun destroyedViewDoesNotReleaseCameraOrPendingCommandSurface() {
        var released = 0
        val lease = StreamPreviewLease { released++ }
        lease.retain() // Команда, ожидающая camera handler.
        lease.retain() // Сессия камеры.
        lease.release() // UI TextureView уничтожен.
        assertEquals(0, released)
        lease.release() // Отменена ожидающая команда.
        assertEquals(0, released)
        lease.release() // Последняя сессия закрыта.
        assertEquals(1, released)
    }

    @Test fun sessionReplacementKeepsSurfaceUntilBothSessionsClose() {
        var released = 0
        val lease = StreamPreviewLease { released++ }
        lease.retain(); lease.retain()
        lease.release() // Старая сессия.
        lease.release() // UI исчез, новая сессия ещё работает.
        assertEquals(0, released)
        lease.release()
        assertEquals(1, released)
    }

    @Test fun closedSurfaceCannotBeRetainedOrReleasedAgain() {
        var released = 0
        val lease = StreamPreviewLease { released++ }
        lease.release()
        try { lease.retain(); fail("retained a closed surface") } catch (_: IllegalStateException) {}
        try { lease.release(); fail("released a closed surface twice") } catch (_: IllegalStateException) {}
        assertEquals(1, released)
    }
}
