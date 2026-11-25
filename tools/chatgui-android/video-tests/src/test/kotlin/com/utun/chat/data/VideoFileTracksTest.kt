package com.utun.chat.data

import android.media.MediaExtractor
import android.media.MediaFormat
import android.os.Handler
import android.os.Looper
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

class VideoFileTracksTest {
    @Before fun prepare() { Handler.rejectDelayed = false; MediaExtractor.created.clear(); MediaExtractor.nextFailures = emptySet() }
    @After fun release() {
        Handler.rejectDelayed = false; MediaExtractor.created.forEach { it.failures = emptySet() }
        Looper.getMainLooper().advanceBy(30_000)
        assertTrue(MediaExtractor.created.all { it.released })
    }
    @Test fun recordedCodecMismatchCannotBeAcceptedAndStillReleasesTheReader() {
        val failure = runCatching { VideoFileTracks().use { it.verify("file.mp4", "video/hevc") } }.exceptionOrNull()!!
        assertTrue(failure.message!!.contains("codec mismatch")); assertTrue(MediaExtractor.created.single().released)
    }
    @Test fun missingVideoTrackAndInvalidGeometryCannotBeReportedAsVerified() {
        for (missing in listOf(true, false)) {
            val reader = VideoFileTracks(); val sdk = MediaExtractor.created.last()
            if (missing) sdk.formats = emptyList() else sdk.formats.single().values[MediaFormat.KEY_WIDTH] = 0
            assertTrue(runCatching { reader.use { it.verify("file.mp4", "video/avc") } }.isFailure)
            assertTrue(sdk.released)
        }
    }
    @Test fun readFailurePreservesTheCauseAndRetainsAnUnreleasedExtractor() {
        MediaExtractor.nextFailures = setOf("source", "release")
        val failure = runCatching { VideoFileTracks().use { it.verify("file.mp4", "video/avc") } }.exceptionOrNull()!!
        assertEquals("Track source rejected", failure.message); assertEquals(1, failure.suppressed.size)
        val sdk = MediaExtractor.created.single(); assertFalse(sdk.released)
        sdk.failures = emptySet(); Looper.getMainLooper().advanceBy(250); assertTrue(sdk.released)
    }
    @Test fun rejectedTimerDoesNotLoseTheExtractorAndAllowsExplicitRetry() {
        val reader = VideoFileTracks(); val sdk = MediaExtractor.created.single().also { it.failures = setOf("release") }
        Handler.rejectDelayed = true; assertTrue(runCatching { reader.close() }.isFailure)
        assertTrue(runCatching { reader.verify("file.mp4", "video/avc") }.isFailure)
        sdk.failures = emptySet(); reader.close(); assertTrue(sdk.released)
    }
}
