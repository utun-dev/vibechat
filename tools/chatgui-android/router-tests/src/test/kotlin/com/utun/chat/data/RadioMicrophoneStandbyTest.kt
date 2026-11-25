package com.utun.chat.data

import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Handler
import android.os.Looper
import com.utun.chat.ChatApplication
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящие engine, подписки и native-команды; три минуты проверяются управляемыми часами без ожидания. */
class RadioMicrophoneStandbyTest {
    private lateinit var context: Context
    private lateinit var radio: RadioAudioEngine
    private lateinit var io: ExecutorService
    private val clock = AtomicLong(1_000_000)
    private val main get() = Looper.getMainLooper()

    @Before fun prepare() {
        context = Context().also { ChatApplication.instance = it }
        AudioManager.shared = AudioManager(); CallController.blocked = false
        AndroidMicrophoneCapture.reset(); AudioTrack.created.clear(); AudioTrack.nextFailures = emptySet()
        Handler.rejectDelayed = false; Handler.rejectImmediate = false
        NativeLib.radioFailures = emptySet(); NativeLib.radioPullResult = 0; NativeLib.coreStopping = false
        NativeLib.radioCommands.clear(); NativeLib.radioFeeds.clear(); NativeLib.radioPullResults.clear()
        NativeLib.onRadioFeed = {}; NativeLib.onRadioCaptureStart = {}
        LogManager.entries.clear()
        radio = RadioAudioEngine(clock::get)
        io = RadioAudioEngine::class.java.getDeclaredField("io").apply { isAccessible = true }.get(radio) as ExecutorService
    }

    @After fun release() {
        Handler.rejectDelayed = false; Handler.rejectImmediate = false; NativeLib.radioFailures = emptySet()
        radio.stop().get(5, TimeUnit.SECONDS)
        main.advanceBy(400_000); drain()
        io.shutdown(); assertTrue(io.awaitTermination(5, TimeUnit.SECONDS))
        val barrier = Executors.newSingleThreadExecutor()
        try { barrier.submit { MediaAudioController.awaitIdle() }.get(5, TimeUnit.SECONDS) }
        finally { barrier.shutdownNow() }
        assertTrue(context.receivers.isEmpty()); assertTrue(AudioTrack.created.all { it.released })
        assertTrue(AndroidMicrophoneCapture.opened.all { it.closed })
    }

    private fun drain() { io.submit {}.get(5, TimeUnit.SECONDS) }
    private fun advance(ms: Long) { clock.addAndGet(ms); main.advanceBy(ms); drain() }
    private fun start(group: Long = 7) {
        var ready = false
        radio.start("%02x".format(group).repeat(32)) { ready = it }; drain(); main.dispatchAll(); assertTrue(ready)
        radio.onSettingsApplied(NativeLib.radioRequest, true); drain()
    }
    private fun screen(on: Boolean) {
        requireNotNull(context.powerManager).isInteractive = on
        context.receivers.keys.single().onReceive(context, Intent(if (on) Intent.ACTION_SCREEN_ON else Intent.ACTION_SCREEN_OFF))
        drain()
    }
    private fun await(condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (!condition() && System.nanoTime() < deadline) Thread.sleep(2)
        assertTrue("radio did not settle: ${LogManager.entries.takeLast(8)}", condition())
    }
    private fun count(kind: String) = synchronized(NativeLib.radioCommands) { NativeLib.radioCommands.count { it.kind == kind } }

    @Test fun enablingKeepsCaptureForExactlyThreeMinutesWithoutTransmitting() {
        start(); assertTrue(NativeLib.radioCaptureAlive); assertTrue(radio.captureWanted.value)
        await { NativeLib.radioFeeds.isNotEmpty() }
        assertFalse(NativeLib.radioTransmitting()); assertFalse(radio.isTalking())
        advance(RadioAudioEngine.MICROPHONE_READY_MS - 1)
        assertTrue(NativeLib.radioCaptureAlive)
        advance(1)
        assertFalse(NativeLib.radioCaptureAlive); assertFalse(radio.captureWanted.value)
        assertTrue(radio.isActive()); assertFalse(AudioTrack.created.last().released)
    }

    @Test fun pttSurvivesOldDeadlineAndRenewsHoldWithoutReopeningCapture() {
        start(); advance(179_000)
        assertTrue(radio.talkBegin()); drain(); await { NativeLib.radioTransmitting() }
        advance(2000); assertTrue(NativeLib.radioCaptureAlive)
        radio.cancelTalk(); drain(); await { !NativeLib.radioTransmitting() }
        assertEquals(1, count("capture start")); assertEquals(0, count("capture stop"))
        advance(179_999); assertTrue(NativeLib.radioCaptureAlive)
        advance(1); assertFalse(NativeLib.radioCaptureAlive)
    }

    @Test fun screenOffClosesCaptureAndWakeRestoresOnlyRemainingBudget() {
        start(); advance(60_000); screen(false)
        assertFalse(NativeLib.radioCaptureAlive); assertFalse(radio.captureReady.value)
        advance(30_000); screen(true)
        assertTrue(NativeLib.radioCaptureAlive); assertFalse(NativeLib.radioTransmitting())
        advance(89_999); assertTrue(NativeLib.radioCaptureAlive)
        advance(1); assertFalse(NativeLib.radioCaptureAlive)
    }

    @Test fun sleepCancelsAllManualOwnersAndNeverResumesTheirTransmission() {
        start(); assertTrue(radio.talkBegin()); assertTrue(radio.talkBegin(RadioPttSource.VOLUME)); drain()
        await { NativeLib.radioTransmitting() }
        screen(false)
        assertFalse(NativeLib.radioCaptureAlive); assertFalse(NativeLib.radioTransmitting()); assertFalse(radio.isTalking())
        assertFalse(radio.talkBegin(RadioPttSource.VOLUME))
        screen(true)
        assertTrue(NativeLib.radioCaptureAlive); assertFalse(NativeLib.radioTransmitting()); assertFalse(radio.isTalking())
        assertTrue(radio.talkBegin(RadioPttSource.VOLUME)); drain()
        await { NativeLib.radioTransmitting() }
    }

    @Test fun wakeAfterExpiryDoesNotRestartMicrophone() {
        start(); screen(false); advance(180_000); screen(true)
        assertFalse(NativeLib.radioCaptureAlive); assertFalse(radio.captureWanted.value)
        assertTrue(radio.talkBegin()); drain(); assertTrue(NativeLib.radioCaptureAlive)
    }

    @Test fun disablingStandbyPreservesPttButClosesCaptureAfterRelease() {
        start(); assertTrue(radio.talkBegin()); drain()
        context.configProvider.keepMicrophoneReady = false; radio.refreshMicrophonePolicy(); drain()
        assertTrue(NativeLib.radioCaptureAlive)
        radio.cancelTalk(); drain(); assertFalse(NativeLib.radioCaptureAlive)
        context.configProvider.keepMicrophoneReady = true; radio.refreshMicrophonePolicy(); drain()
        assertTrue(NativeLib.radioCaptureAlive); assertFalse(NativeLib.radioTransmitting())
        context.configProvider.keepMicrophoneReady = false; radio.refreshMicrophonePolicy(); drain()
        assertFalse(NativeLib.radioCaptureAlive)
    }

    @Test fun previousChannelDeadlineCannotCloseNewChannelsCapture() {
        start(); advance(100_000); start(8)
        advance(80_000); assertTrue(NativeLib.radioCaptureAlive); assertEquals("08".repeat(32), radio.activeGroup.value)
        advance(100_000); assertFalse(NativeLib.radioCaptureAlive)
    }

    @Test fun rejectedTimerDisablesOnlyIdleCaptureAndIsLogged() {
        Handler.rejectDelayed = true; start()
        assertFalse(NativeLib.radioCaptureAlive); assertTrue(radio.isActive())
        assertTrue(LogManager.entries.any { it.level == "ERROR" && "standby timer rejected" in it.message })
        Handler.rejectDelayed = false
        assertTrue(radio.talkBegin()); drain(); assertTrue(NativeLib.radioCaptureAlive)
    }

    @Test fun automaticTransmissionEndRenewsStandbyAfterItsOriginalExpiry() {
        context.configProvider.radioSettings = RadioAutoSettings(enabled = true, mode = RadioAutoSettings.LEVEL)
        start(); advance(180_000)
        assertTrue(NativeLib.radioCaptureAlive)
        NativeLib.radioTalkBegin("07".repeat(32)); await { radio.transmittingGroup.value == "07".repeat(32) }
        NativeLib.radioTalkEnd("07".repeat(32))
        await { LogManager.entries.any { "reason=automatic_tx_end" in it.message } }
        drain()
        context.configProvider.radioSettings = RadioAutoSettings()
        radio.reconfigure(); drain(); radio.onSettingsApplied(NativeLib.radioRequest, true); drain()
        assertTrue(NativeLib.radioCaptureAlive)
        advance(179_999); assertTrue(NativeLib.radioCaptureAlive)
        advance(1); assertFalse(NativeLib.radioCaptureAlive)
    }
}
