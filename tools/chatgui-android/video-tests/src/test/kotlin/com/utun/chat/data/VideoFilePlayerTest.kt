package com.utun.chat.data

import android.graphics.SurfaceTexture
import android.media.MediaPlayer
import android.os.Handler
import android.os.Looper
import com.utun.chat.R
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

class VideoFilePlayerTest {
    private val owners = mutableListOf<VideoFilePlayer>()
    private val textures = mutableListOf<SurfaceTexture>()
    private val main get() = Looper.getMainLooper()
    private val sdk get() = MediaPlayer.created.single()
    @Before fun prepare() {
        Handler.rejectDelayed = false; MediaPlayer.created.clear(); MediaPlayer.nextFailures = emptySet(); LogManager.entries.clear()
    }
    @After fun release() {
        Handler.rejectDelayed = false; MediaPlayer.created.forEach { it.failures = emptySet() }
        owners.forEach { it.close() }; main.advanceBy(30_000)
        assertTrue(textures.all { it.released }); assertTrue(MediaPlayer.created.all { it.released })
    }
    private fun owner(completed: () -> Unit = {}): VideoFilePlayer = VideoFilePlayer(completed).also { owners.add(it) }
    private fun texture() = SurfaceTexture(textures.size).also { textures.add(it) }
    private fun ready(): VideoFilePlayer = owner().also { it.start("file.mp4"); it.attach(texture()); sdk.preparedCallback!!(sdk) }
    private fun diagnostic(stage: String) = assertTrue(LogManager.entries.any { it.level == "ERROR" && stage in it.message })

    @Test fun constructorAndCloseBeforeStartDoNotAllocateMediaPlayer() {
        val player = owner(); assertTrue(MediaPlayer.created.isEmpty()); player.close(); player.close()
        assertTrue(MediaPlayer.created.isEmpty())
    }
    @Test fun surfaceBeforeStartIsAdoptedAndPlaybackWaitsForPrepared() {
        val player = owner(); val texture = texture(); player.attach(texture); assertTrue(MediaPlayer.created.isEmpty())
        player.start("file.mp4"); assertNotNull(sdk.surface); assertFalse(player.state.value.playing)
        sdk.preparedCallback!!(sdk)
        assertTrue(player.state.value.playing); assertEquals(10_000, player.state.value.durationMs); assertEquals(1, sdk.starts)
    }
    @Test fun preparedBeforeSurfaceWaitsForOutputWithoutIssuingPrematureStart() {
        val player = owner(); player.start("file.mp4"); sdk.preparedCallback!!(sdk)
        assertTrue(player.state.value.prepared); assertFalse(player.state.value.playing); assertEquals(0, sdk.starts)
        player.attach(texture()); assertTrue(player.state.value.playing)
    }
    @Test fun userPauseSurvivesSurfaceRecreation() {
        val player = ready(); val first = textures.single(); assertTrue(player.pause())
        assertFalse(player.detach(first)); assertTrue(first.released)
        player.attach(texture())
        assertEquals(1, sdk.starts); assertFalse(player.state.value.playing)
    }
    @Test fun staleDetachCannotDisconnectTheCurrentSurface() {
        val player = ready(); val first = textures.single(); val second = texture(); player.attach(second)
        val current = sdk.surface
        assertFalse(player.detach(first)); assertSame(current, sdk.surface); assertTrue(first.released); assertFalse(second.released)
    }
    @Test fun failedStartNeverPublishesPlayingAndStillReleasesSdk() {
        MediaPlayer.nextFailures = setOf("start")
        val player = ready()
        assertFalse(player.state.value.playing); assertEquals(R.string.video_playback_failed, player.state.value.error)
        assertTrue(sdk.released); assertFalse(textures.single().released); diagnostic("stage=play")
    }
    @Test fun failedPauseAndSeekInvalidatePreparedStateWithoutFakeSuccess() {
        val player = ready(); sdk.failures = setOf("pause")
        assertFalse(player.pause()); assertFalse(player.state.value.prepared); assertFalse(player.state.value.playing)
        assertFalse(player.seek(5000)); assertEquals(0, sdk.seeks); diagnostic("stage=pause")
    }
    @Test fun failedSeekRetainsLastPositionAndReportsPlaybackFailure() {
        val player = ready(); assertTrue(player.seek(1000)); sdk.failures = setOf("seek")
        assertFalse(player.seek(2000)); assertEquals(1000, player.state.value.positionMs)
        assertEquals(R.string.video_playback_failed, player.state.value.error); diagnostic("stage=seek")
    }
    @Test fun positionFailureEndsPollingInsteadOfResettingPositionToZero() {
        val player = ready(); assertTrue(player.seek(4000)); sdk.failures = setOf("position")
        player.pollPosition()
        assertEquals(4000, player.state.value.positionMs); assertFalse(player.state.value.playing); diagnostic("stage=position")
    }
    @Test fun releaseFailureKeepsTextureAliveUntilPlayerAcknowledgesRelease() {
        val player = ready(); val texture = textures.single(); sdk.failures = setOf("release")
        player.close(); assertFalse(sdk.released); assertFalse(texture.released)
        sdk.failures = emptySet(); main.advanceBy(250)
        assertTrue(sdk.released); assertTrue(texture.released); diagnostic("release failed")
    }
    @Test fun releaseFailureAfterEffectStillRequiresAcknowledgedRetry() {
        val player = ready(); sdk.failures = setOf("release after"); player.close()
        assertTrue(sdk.released); assertFalse(textures.single().released)
        sdk.failures = emptySet(); main.advanceBy(250)
        assertTrue(textures.single().released); assertEquals(2, sdk.releases)
    }
    @Test fun partialSurfaceBindFailureKeepsUiOutputAndUsesPlayerReleaseBarrier() {
        val player = owner(); player.start("file.mp4"); sdk.failures = setOf("surface after", "release")
        val texture = texture(); player.attach(texture); player.close()
        assertFalse(texture.released)
        sdk.failures = emptySet(); main.advanceBy(250)
        assertTrue(texture.released); diagnostic("stage=attach Surface")
    }
    @Test fun failedDetachCannotReleaseTextureUnderPlayer() {
        val player = ready(); sdk.failures = setOf("surface", "release"); val texture = textures.single()
        assertFalse(player.detach(texture)); assertFalse(texture.released)
        sdk.failures = emptySet(); main.advanceBy(250)
        assertTrue(texture.released); diagnostic("stage=detach Surface")
    }
    @Test fun openFailureReleasesPartiallyInitializedPlayerAndKeepsUiExitAvailable() {
        MediaPlayer.nextFailures = setOf("source")
        val player = owner(); player.start("missing.mp4")
        assertEquals(R.string.video_open_failed, player.state.value.error); assertTrue(sdk.released)
        assertFalse(player.play()); assertEquals(0, sdk.starts)
    }
    @Test fun latePreparedAndCompletionAfterDisposeCannotRestartOrNotifyUi() {
        var completions = 0
        val player = owner { completions++ }; player.start("file.mp4"); player.attach(texture()); player.close()
        sdk.preparedCallback!!(sdk); sdk.completedCallback!!(sdk); sdk.errorCallback!!(sdk, 1, 2)
        assertEquals(0, sdk.starts); assertEquals(0, completions); assertFalse(player.state.value.prepared)
    }
    @Test fun completionUpdatesStateBeforeCallingUiAndCallbackExceptionIsContained() {
        lateinit var player: VideoFilePlayer
        player = owner { assertFalse(player.state.value.playing); error("UI close failed") }
        player.start("file.mp4"); player.attach(texture()); sdk.preparedCallback!!(sdk); sdk.completedCallback!!(sdk)
        assertEquals(10_000, player.state.value.positionMs); assertTrue(sdk.released); diagnostic("completion callback")
    }
    @Test fun lateSurfaceAfterDisposeIsReleasedAndFrameworkMustNotCloseItAgain() {
        val player = ready(); player.close(); val late = texture(); player.attach(late)
        assertTrue(late.released); assertFalse(player.detach(late)); assertEquals(1, late.releaseCalls)
    }
    @Test fun rejectedCleanupTimerKeepsOwnerForExplicitCloseRetry() {
        val player = ready(); sdk.failures = setOf("release"); Handler.rejectDelayed = true; player.close()
        assertFalse(textures.single().released); diagnostic("timer rejected")
        sdk.failures = emptySet(); Handler.rejectDelayed = false; player.close()
        assertTrue(textures.single().released)
    }
    @Test fun seekIsClampedToDurationAndSuccessfulSdkPollUpdatesPosition() {
        val player = ready(); assertTrue(player.seek(Int.MAX_VALUE)); assertEquals(10_000, sdk.position)
        sdk.position = 2000; player.pollPosition(); assertEquals(2000, player.state.value.positionMs)
        assertTrue(player.seek(-100)); assertEquals(0, sdk.position)
    }
}
