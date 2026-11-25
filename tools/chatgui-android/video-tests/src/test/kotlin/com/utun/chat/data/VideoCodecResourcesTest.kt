package com.utun.chat.data

import android.media.Image
import android.media.MediaCodec
import android.os.Handler
import android.os.Looper
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

class VideoCodecResourcesTest {
    private val owners = mutableListOf<VideoCodecResources>()
    private val images = mutableListOf<Image>()
    private val main get() = Looper.getMainLooper()
    @Before fun prepare() { MediaCodec.nextFailures = emptySet(); MediaCodec.created.clear(); Handler.rejectDelayed = false }
    @After fun release() {
        Handler.rejectDelayed = false; images.forEach { it.failures = emptySet() }
        MediaCodec.created.forEach { it.failures = emptySet() }; owners.forEach { it.close() }; main.advanceBy(30_000)
        assertTrue(images.all { it.closed }); assertTrue(MediaCodec.created.all { it.released })
    }
    private fun owner() = VideoCodecResources(MediaCodec.createByCodecName("host.codec"), "host.codec").also { owners.add(it) }
    private fun image(owner: VideoCodecResources) = Image().also { images.add(it); owner.own(it) }
    private fun pending(owner: VideoCodecResources) = assertThrows(IllegalStateException::class.java) { owner.close() }

    @Test fun closedImagesAndCodecAreAcknowledgedOnlyOnce() {
        val owner = owner(); val image = image(owner); owner.closeImage(image); owner.close(); owner.close()
        assertEquals(1, image.closeCalls); assertEquals(1, owner.codec.releaseCalls)
    }
    @Test fun imageFailureDoesNotSkipOtherImagesOrCodecRelease() {
        val owner = owner(); val failed = image(owner); val healthy = image(owner); failed.failures = setOf("close")
        pending(owner); assertFalse(failed.closed); assertTrue(healthy.closed); assertTrue(owner.codec.released)
        failed.failures = emptySet(); main.advanceBy(250)
        assertTrue(failed.closed); assertEquals(1, healthy.closeCalls); assertEquals(1, owner.codec.releaseCalls)
    }
    @Test fun stopFailureStillAttemptsAndAcknowledgesRelease() {
        val owner = owner(); owner.codec.failures = setOf("stop"); owner.close(); owner.close()
        assertTrue(owner.codec.released); assertEquals(1, owner.codec.stopCalls); assertEquals(1, owner.codec.releaseCalls)
    }
    @Test fun failedReleaseRetainsOwnerAndPreservesTheFirstScheduledRetry() {
        val owner = owner(); owner.codec.failures = setOf("release"); pending(owner); main.advanceBy(200); pending(owner)
        owner.codec.failures = emptySet(); main.advanceBy(50); assertTrue(owner.codec.released)
        assertEquals(1, owner.codec.stopCalls)
    }
    @Test fun failuresAfterEffectRequireAnAcknowledgedRetry() {
        val owner = owner(); val image = image(owner)
        image.failures = setOf("close after"); owner.codec.failures = setOf("release after"); pending(owner)
        assertTrue(image.closed); assertTrue(owner.codec.released)
        image.failures = emptySet(); owner.codec.failures = emptySet(); main.advanceBy(250)
        owner.close(); assertEquals(2, image.closeCalls); assertEquals(2, owner.codec.releaseCalls)
    }
    @Test fun rejectedTimerRetainsOwnerForExplicitRetry() {
        val owner = owner(); owner.codec.failures = setOf("release"); Handler.rejectDelayed = true; pending(owner)
        owner.codec.failures = emptySet(); Handler.rejectDelayed = false; owner.close(); assertTrue(owner.codec.released)
    }
    @Test fun useKeepsTheOriginalFailureAndSuppressesCleanupFailure() {
        val owner = owner(); owner.codec.failures = setOf("release")
        val original = IllegalArgumentException("configure failed")
        val thrown = assertThrows(IllegalArgumentException::class.java) { owner.use { throw original } }
        assertSame(original, thrown); assertEquals(1, thrown.suppressed.size)
        owner.codec.failures = emptySet(); main.advanceBy(250); assertTrue(owner.codec.released)
    }
}
