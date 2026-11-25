package com.utun.chat.data

import android.graphics.SurfaceTexture
import android.os.Handler
import android.os.Looper
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit

/** Настоящие preview/leases; camera/GL отсутствуют, отказы Surface/texture задаются на границах. */
class StreamPreviewLifecycleTest {
    private lateinit var preview: StreamPreview
    private val targets = CopyOnWriteArrayList<StreamPreviewTarget>()
    private val textures = mutableListOf<SurfaceTexture>()
    private val main get() = Looper.getMainLooper()
    @Before fun prepare() { preview = StreamPreview(); Handler.rejectDelayed = false; LogManager.entries.clear() }
    @After fun release() {
        Handler.rejectDelayed = false
        targets.forEach { it.surface.failures = emptySet(); it.texture.failures = emptySet() }
        preview.close(); targets.forEach { it.release() }; main.advanceBy(30_000)
        assertTrue(textures.all { it.released }); assertTrue(targets.all { !it.surface.isValid })
    }
    private fun attach(): StreamPreviewTarget {
        val texture = SurfaceTexture(textures.size).also { textures.add(it) }
        return preview.attach(texture).also { targets.add(it) }
    }
    private fun own(target: StreamPreviewTarget?) { if (target != null) targets.add(target) }
    private fun diagnostic(text: String) = assertTrue("Missing diagnostic: $text", LogManager.entries.any {
        text in it.message && it.level in listOf("WARN", "ERROR")
    })

    @Test fun viewDisposeReleasesUiLeaseWithoutWaitingForTextureDestroyed() {
        val owner = StreamPreviewViewResources(preview)
        val texture = SurfaceTexture(100).also { textures.add(it) }
        val target = checkNotNull(owner.attach(texture)).also { targets.add(it) }
        owner.close(); owner.close()
        assertTrue(target.released); assertFalse(target.surface.isValid); assertTrue(texture.released)
        assertFalse(owner.destroy(texture)); assertEquals(1, texture.releaseCalls)
    }

    @Test fun viewDisposeCannotReleaseTextureUnderCameraLease() {
        var session: StreamPreviewTarget? = null
        preview.bind { if (it != null) session = it.also(targets::add) }
        val owner = StreamPreviewViewResources(preview)
        val texture = SurfaceTexture(101).also { textures.add(it) }
        owner.attach(texture)?.let(targets::add)
        owner.close(); assertFalse(texture.released)
        assertFalse(owner.destroy(texture)); assertFalse(texture.released)
        session!!.release(); assertTrue(texture.released)
    }

    @Test fun lateTextureAfterViewDisposeIsOwnedAndReleased() {
        val owner = StreamPreviewViewResources(preview); owner.close()
        val texture = SurfaceTexture(102).also { textures.add(it) }
        assertNull(owner.attach(texture)); assertTrue(texture.released)
        assertFalse(owner.destroy(texture)); assertEquals(1, texture.releaseCalls)
    }

    @Test fun staleTextureDestroyCannotDetachNewViewTarget() {
        var offered: StreamPreviewTarget? = null
        preview.bind { offered = it; own(it) }
        val owner = StreamPreviewViewResources(preview)
        val first = SurfaceTexture(103).also { textures.add(it) }
        val second = SurfaceTexture(104).also { textures.add(it) }
        owner.attach(first)?.let(targets::add); owner.attach(second)?.let(targets::add)
        val current = offered
        assertFalse(owner.destroy(first)); assertSame(current, offered); assertFalse(second.released)
        owner.close(); targets.forEach { it.release() }; assertTrue(second.released)
        assertFalse(owner.destroy(second))
    }

    @Test fun everyOwnerHasAnIdempotentLeaseAndUiDetachMatchesTheSharedSource() {
        var command: StreamPreviewTarget? = null
        preview.bind { command = it; own(it) }
        val ui = attach(); val offered = checkNotNull(command); val session = offered.retain().also { own(it) }
        assertNotSame(ui, offered); assertTrue(ui.sameSource(offered))
        ui.release(); ui.release(); assertTrue(ui.surface.isValid)
        preview.detach(ui); assertNull(command)
        offered.release(); assertTrue(ui.surface.isValid)
        session.release(); assertFalse(ui.surface.isValid); assertEquals(1, ui.surface.releaseCalls)
        assertEquals(1, ui.texture.releaseCalls)
    }

    @Test fun callbackFailureBeforeAcceptanceCannotOrphanItsLease() {
        val ui = attach()
        preview.bind { error("delivery rejected") }
        assertTrue(preview.state.value.failed)
        preview.close(); ui.release()
        assertFalse(ui.surface.isValid); assertTrue(ui.texture.released)
        diagnostic("preview output callback failed")
    }

    @Test fun callbackThatReleasedBeforeThrowingCannotConsumeAnotherOwnersReference() {
        val ui = attach()
        preview.bind { it?.release(); error("delivery failed after release") }
        assertTrue(ui.surface.isValid)
        preview.close(); assertTrue(ui.surface.isValid)
        ui.release(); assertFalse(ui.surface.isValid)
    }

    @Test fun queuedLeaseIsCancelledWhenItsSenderThrowsAfterPosting() {
        var queued: StreamPreviewTarget? = null
        val ui = attach()
        preview.bind { queued = it; own(it); error("post failed after effect") }
        assertTrue(checkNotNull(queued).released)
        preview.close(); ui.release()
        assertFalse(ui.surface.isValid); queued?.release()
        assertEquals(1, ui.surface.releaseCalls)
    }

    @Test fun failedNullDeliveryDoesNotSkipDetachingTheManagerReference() {
        var command: StreamPreviewTarget? = null
        preview.bind { next ->
            command?.release(); command = next; own(next)
            if (next == null && textures.isNotEmpty()) error("detach callback rejected")
        }
        val ui = attach(); preview.detach(ui); ui.release()
        assertFalse(ui.surface.isValid); assertTrue(preview.state.value.failed)
    }

    @Test fun surfaceReleaseFailureStillReleasesTextureAndRetriesOnlyTheSurface() {
        val ui = attach(); ui.surface.failures = setOf("release")
        preview.close(); ui.release()
        assertTrue(ui.surface.isValid); assertTrue(ui.texture.released); assertTrue(ui.released)
        assertThrows(IllegalStateException::class.java) { ui.retain() }
        ui.surface.failures = emptySet(); main.advanceBy(250)
        assertFalse(ui.surface.isValid); assertEquals(1, ui.texture.releaseCalls)
        diagnostic("preview Surface release failed")
    }

    @Test fun textureReleaseFailureDoesNotRepeatAcknowledgedSurfaceRelease() {
        val ui = attach(); ui.texture.failures = setOf("release")
        preview.close(); ui.release()
        assertFalse(ui.surface.isValid); assertFalse(ui.texture.released)
        ui.texture.failures = emptySet(); ui.release()
        assertTrue(ui.texture.released); assertEquals(1, ui.surface.releaseCalls)
        diagnostic("preview texture release failed")
    }

    @Test fun afterEffectReleaseFailuresKeepPhysicalOwnersUntilAcknowledged() {
        val ui = attach(); ui.texture.failures = setOf("release after"); ui.surface.failures = setOf("release after")
        preview.close(); ui.release()
        assertTrue(ui.texture.released); assertFalse(ui.surface.isValid)
        ui.texture.failures = emptySet(); ui.surface.failures = emptySet(); ui.release()
        assertEquals(2, ui.surface.releaseCalls); assertEquals(2, ui.texture.releaseCalls)
    }

    @Test fun rejectedRetryCanBeCompletedByRepeatingTheReleasedLease() {
        val ui = attach(); ui.surface.failures = setOf("release"); Handler.rejectDelayed = true
        preview.close(); ui.release(); diagnostic("preview cleanup retry rejected")
        ui.surface.failures = emptySet(); ui.release()
        assertFalse(ui.surface.isValid); assertEquals(1, ui.texture.releaseCalls)
    }

    @Test fun hideAndShowUseNewLeasesWithoutClosingTheUiSource() {
        var command: StreamPreviewTarget? = null; var deliveries = 0
        preview.bind { next -> command?.release(); command = next; own(next); deliveries++ }
        val ui = attach(); val first = command
        preview.setVisible(false); assertNull(command); assertTrue(ui.surface.isValid)
        val before = deliveries; preview.setVisible(false); assertEquals(before, deliveries)
        preview.setVisible(true); assertNotSame(first, command); assertTrue(ui.sameSource(command))
        assertTrue(ui.surface.isValid)
    }

    @Test fun deliveryCanBeRetriedWithTheSameVisibilityAfterFailure() {
        var fail = true; var command: StreamPreviewTarget? = null
        val ui = attach()
        preview.bind { next -> if (fail) error("temporarily unavailable"); command = next; own(next) }
        fail = false; preview.setVisible(true)
        assertNotNull(command); assertTrue(ui.sameSource(command))
    }

    @Test fun staleDetachCannotRemoveTheNewTarget() {
        var command: StreamPreviewTarget? = null
        preview.bind { next -> command?.release(); command = next; own(next) }
        val first = attach(); val second = attach()
        preview.detach(first); assertTrue(second.sameSource(command)); assertTrue(second.surface.isValid)
        first.release(); assertFalse(first.surface.isValid)
        preview.detach(second); assertNull(command); second.release(); assertFalse(second.surface.isValid)
    }

    @Test fun lateUpdateCannotReactivateClosedPreviewAndLateAttachBelongsOnlyToUi() {
        preview.close()
        preview.update(StreamPreviewFormat(640, 480, StreamPreviewCamera(0, false)), active = true)
        assertFalse(preview.state.value.active)
        val ui = attach(); ui.release(); assertFalse(ui.surface.isValid)
        assertThrows(IllegalStateException::class.java) { preview.bind {} }
        diagnostic("preview update ignored after close"); diagnostic("preview bind rejected")
    }

    @Test fun genericLeasePreservesItsLastReferenceIfCloseThrows() {
        var failed = true; var completed = 0
        val lease = StreamPreviewLease { check(!failed); completed++ }
        assertThrows(IllegalStateException::class.java) { lease.release() }
        failed = false; lease.release(); assertEquals(1, completed)
        assertThrows(IllegalStateException::class.java) { lease.retain() }
    }

    @Test fun concurrentIndependentLeaseReleasesCloseTheResourceExactlyOnce() {
        val ui = attach(); val gate = CountDownLatch(1)
        val leases = List(16) { ui.retain().also { own(it) } }
        preview.close(); ui.release()
        val threads = leases.map { lease -> Thread { gate.await(); repeat(3) { lease.release() } }.apply { start() } }
        gate.countDown(); threads.forEach { it.join(TimeUnit.SECONDS.toMillis(3)); assertFalse(it.isAlive) }
        assertFalse(ui.surface.isValid); assertEquals(1, ui.surface.releaseCalls); assertEquals(1, ui.texture.releaseCalls)
    }
}
