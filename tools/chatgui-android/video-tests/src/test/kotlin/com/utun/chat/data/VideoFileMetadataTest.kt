package com.utun.chat.data

import android.media.MediaMetadataRetriever
import android.os.Handler
import android.os.Looper
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

class VideoFileMetadataTest {
    @Before fun prepare() {
        Handler.rejectDelayed = false; MediaMetadataRetriever.created.clear(); MediaMetadataRetriever.nextFailures = emptySet()
    }
    @After fun release() {
        android.os.Build.VERSION.SDK_INT = 36
        Handler.rejectDelayed = false; MediaMetadataRetriever.created.forEach { it.failures = emptySet() }
        Looper.getMainLooper().advanceBy(30_000)
        assertTrue(MediaMetadataRetriever.created.all { it.released })
    }
    @Test fun rotatedMetadataUsesDisplayDimensionsAndReleasesOnSuccess() {
        val info = VideoFileMetadata().use { it.read("file.mp4") }
        assertEquals(720, info.displayWidth); assertEquals(1280, info.displayHeight)
        assertTrue(MediaMetadataRetriever.created.single().released)
    }
    @Test fun readFailurePreservesOriginalExceptionAndSuppressesCleanupError() {
        MediaMetadataRetriever.nextFailures = setOf("source", "release")
        val error = runCatching { VideoFileMetadata().use { it.read("file.mp4") } }.exceptionOrNull()!!
        assertEquals("Metadata source failed", error.message); assertEquals(1, error.suppressed.size)
        val sdk = MediaMetadataRetriever.created.single(); assertFalse(sdk.released)
        sdk.failures = emptySet(); Looper.getMainLooper().advanceBy(250); assertTrue(sdk.released)
    }
    @Test fun closeFailureRetainsRetrieverAndForbidsNewReadsUntilRetryAck() {
        val reader = VideoFileMetadata(); val sdk = MediaMetadataRetriever.created.single(); sdk.failures = setOf("release after")
        assertTrue(runCatching { reader.close() }.isFailure); assertTrue(sdk.released)
        assertTrue(runCatching { reader.read("file.mp4") }.isFailure)
        sdk.failures = emptySet(); Looper.getMainLooper().advanceBy(250); assertEquals(2, sdk.releases)
        reader.close(); assertEquals(2, sdk.releases)
    }
    @Test fun malformedGeometryIsRejectedAndStillReleasesRetriever() {
        val reader = VideoFileMetadata(); MediaMetadataRetriever.created.single().rotation = "45"
        assertTrue(runCatching { reader.use { it.read("file.mp4") } }.isFailure)
        assertTrue(MediaMetadataRetriever.created.single().released)
    }
    @Test fun rejectedTimerStillAllowsExplicitReleaseRetry() {
        val reader = VideoFileMetadata(); val sdk = MediaMetadataRetriever.created.single()
        sdk.failures = setOf("release"); Handler.rejectDelayed = true
        assertTrue(runCatching { reader.close() }.isFailure)
        sdk.failures = emptySet(); Handler.rejectDelayed = false; reader.close(); assertTrue(sdk.released)
    }
    @Test fun thumbnailUsesScaledSdkDecodeAndTransfersOnlySmallBitmap() {
        val preview = VideoFileMetadata().use { reader ->
            MediaMetadataRetriever.created.single().apply { width = "3840"; height = "2160" }
            reader.preview("file.mp4")
        }
        assertEquals(640, preview.bitmap!!.width); assertEquals(360, preview.bitmap.height)
        assertFalse(preview.bitmap.recycled); assertEquals(1, MediaMetadataRetriever.created.single().scaledReads)
    }
    @Test fun api26FallbackRecyclesFullFrameBeforeDeliveringScaledBitmap() {
        android.os.Build.VERSION.SDK_INT = 26
        val preview = VideoFileMetadata().use { it.preview("file.mp4") }
        assertEquals(640, preview.bitmap!!.width); assertEquals(360, preview.bitmap.height)
        assertTrue(MediaMetadataRetriever.created.single().lastFrame!!.recycled); assertFalse(preview.bitmap.recycled)
    }
    @Test fun failedThumbnailExtractionStillReleasesRetriever() {
        MediaMetadataRetriever.nextFailures = setOf("frame")
        assertTrue(runCatching { VideoFileMetadata().use { it.preview("file.mp4") } }.isFailure)
        assertTrue(MediaMetadataRetriever.created.single().released)
    }
    @Test fun submissionRejectsInvalidDurationAndMimeBeforeTransferringTheSource() {
        for ((duration, mime) in listOf("0" to "video/mp4", "86400001" to "video/mp4", "1000" to "video/webm")) {
            val reader = VideoFileMetadata()
            val sdk = MediaMetadataRetriever.created.last().also { it.duration = duration; it.mime = mime }
            assertTrue(runCatching { reader.use { it.submission("file.mp4") } }.isFailure)
            assertTrue(sdk.released)
        }
    }
    @Test fun submissionRetainsFailedReleaseAndPreservesCodedGeometry() {
        val reader = VideoFileMetadata()
        val sdk = MediaMetadataRetriever.created.single().also { it.failures = setOf("release") }
        val metadata = reader.submission("file.mp4")
        assertEquals(1280, metadata.width); assertEquals(720, metadata.height); assertEquals(10_000, metadata.durationMs)
        assertTrue(runCatching { reader.close() }.isFailure); assertFalse(sdk.released)
        sdk.failures = emptySet(); Looper.getMainLooper().advanceBy(250); assertTrue(sdk.released)
    }
}
