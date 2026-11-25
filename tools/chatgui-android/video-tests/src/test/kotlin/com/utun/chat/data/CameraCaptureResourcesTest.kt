package com.utun.chat.data

import android.os.Handler
import android.os.Looper
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

class CameraCaptureResourcesTest {
    private val root = Files.createTempDirectory("utun-camera-owner").toFile()
    private val owner = CameraCaptureResources()
    @Before fun prepare() { Handler.rejectDelayed = false }
    @After fun release() { Handler.rejectDelayed = false; owner.close(); root.deleteRecursively(); Looper.getMainLooper().advanceBy(30_000) }
    private fun file(name: String = "capture.mp4") = File(root, name).also { it.writeText("camera data") }
    private class Recording : AutoCloseable {
        var fail = false; var closes = 0
        override fun close() { closes++; check(!fail) { "Recording close rejected" } }
    }

    @Test fun discardedVideoWaitsForFinalizeAfterSuccessfulClose() {
        val file = file(); val record = Recording()
        owner.own(file); owner.recording(file, record); owner.discard(file)
        assertEquals(1, record.closes); assertTrue(file.exists())
        owner.close(); assertTrue(file.exists()); assertFalse(owner.completed(file)); assertFalse(file.exists())
    }
    @Test fun discardedPhotoIsNotDeletedUnderTheImageWriter() {
        val file = file("photo.jpg")
        owner.own(file); owner.close(); assertTrue(file.exists())
        file.appendText("late writer output")
        assertFalse(owner.completed(file)); assertFalse(file.exists())
    }
    @Test fun closeFailureKeepsRecordingAndRetriesIndependentlyOfFileCompletion() {
        val file = file(); val record = Recording().also { it.fail = true }
        owner.own(file); owner.recording(file, record); owner.close()
        assertTrue(file.exists()); assertEquals(1, record.closes)
        record.fail = false; Looper.getMainLooper().advanceBy(250)
        assertEquals(2, record.closes); assertTrue(file.exists())
        owner.completed(file); assertFalse(file.exists())
    }
    @Test fun lateRecordingAdoptionAfterCloseStillClosesAndWaitsForFinalize() {
        val file = file(); val record = Recording()
        owner.own(file); owner.close(); owner.recording(file, record)
        assertEquals(1, record.closes); assertTrue(file.exists())
        owner.completed(file); assertFalse(file.exists())
    }
    @Test fun synchronousFinalizeBeforeHandleAdoptionDoesNotResurrectTheWriter() {
        val file = file(); val record = Recording()
        owner.own(file); owner.close(); owner.completed(file); owner.recording(file, record)
        assertFalse(file.exists()); assertEquals(0, record.closes)
    }
    @Test fun cacheTransferredToSenderSurvivesUiCloseAndCannotBeDiscardedTwice() {
        val file = file(); owner.own(file); assertTrue(owner.completed(file)); owner.transferred(file)
        owner.close(); owner.discard(file); assertTrue(file.exists())
    }
    @Test fun unbindFailureRetainsOnlyOwnUseCasesUntilRetryAcknowledgesCleanup() {
        val provider = ProcessCameraProvider(); val own = Preview.Builder().build(); val other = Preview.Builder().build()
        provider.bound.addAll(listOf(own, other)); owner.binding(provider, own)
        provider.failures = setOf("unbind"); owner.close()
        assertTrue(own in provider.bound); assertTrue(other in provider.bound)
        provider.failures = emptySet(); Looper.getMainLooper().advanceBy(250)
        assertFalse(own in provider.bound); assertTrue(other in provider.bound)
    }
    @Test fun failedRebindCleanupAlsoRetriesBeforeTheUiOwnerIsClosed() {
        val provider = ProcessCameraProvider(); val useCase = Preview.Builder().build()
        provider.bound.add(useCase); owner.binding(provider, useCase); provider.failures = setOf("unbind")
        assertFalse(owner.unbind()); provider.failures = emptySet(); Looper.getMainLooper().advanceBy(250)
        assertFalse(useCase in provider.bound); assertTrue(owner.unbind())
    }
    @Test fun rejectedRetryDoesNotLoseTheRecordingOrPreventExplicitClose() {
        val file = file(); val record = Recording().also { it.fail = true }
        owner.own(file); owner.recording(file, record); Handler.rejectDelayed = true; owner.close()
        record.fail = false; owner.close(); assertEquals(2, record.closes); assertTrue(file.exists())
        owner.completed(file); assertFalse(file.exists())
    }
    @Test fun failedDeletionDoesNotPreventAnotherCacheFromBeingReleased() {
        val directory = File(root, "cannot-delete-yet").also { it.mkdir() }
        val child = File(directory, "busy").also { it.writeText("data") }
        val second = file("second.jpg")
        owner.own(directory); owner.completed(directory); owner.own(second); owner.completed(second); owner.close()
        assertTrue(directory.exists()); assertFalse(second.exists())
        child.delete(); Looper.getMainLooper().advanceBy(250); assertFalse(directory.exists())
    }
    @Test fun orientationDisableFailureDoesNotSkipCacheCleanupAndRetriesItsOwnListener() {
        val listener = object : android.view.OrientationEventListener(android.content.Context()) {
            override fun onOrientationChanged(angle: Int) {}
        }
        owner.ownOrientation(listener); listener.enable()
        val file = file("photo.jpg"); owner.own(file); owner.completed(file)
        android.view.OrientationEventListener.failures = setOf("disable")
        try {
            owner.close(); assertFalse(file.exists()); assertTrue(listener.enabled)
        } finally { android.view.OrientationEventListener.failures = emptySet() }
        Looper.getMainLooper().advanceBy(250); assertFalse(listener.enabled)
    }
}
