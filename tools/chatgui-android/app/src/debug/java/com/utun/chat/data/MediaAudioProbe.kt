package com.utun.chat.data

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.media.AudioDeviceInfo
import android.content.Context
import android.os.Build
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Bundle
import android.widget.TextView

/** Явная аппаратная проверка через adb am instrument. PCM остаётся в памяти; сеть и камера не используются. */
class MediaAudioProbeActivity : Activity() {
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        if (android.os.Build.VERSION.SDK_INT >= 27) { setShowWhenLocked(true); setTurnScreenOn(true) }
        else window.addFlags(android.view.WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or
            android.view.WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON)
        setContentView(TextView(this).apply { text = "Shared media audio test" })
    }
}

/** output=media выбирает USAGE_MEDIA вместо VOICE_COMMUNICATION. Два потребителя проверяют общий PCM/PTS,
 * смену профиля и независимое закрытие подписок; при наличии SCO — receive-only и запуск без микрофона.
 * Требуются разрешённый RECORD_AUDIO и свободная media-подсистема. startup=1 измеряет три старта PTT без сети;
 * micSource=0..3 временно выбирает источник микрофона и восстанавливает настройку после теста. */
class MediaAudioProbe : Instrumentation() {
    private var mediaOutput = false
    private var startup = false
    private var micSource: Int? = null
    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments)
        mediaOutput = arguments?.getString("output") == "media"
        startup = arguments?.getString("startup") == "1"
        micSource = arguments?.getString("micSource")?.toInt()?.also { require(it in 0..3) }
        start()
    }
    override fun onStart() {
        var activity: Activity? = null
        var radio: MicrophoneCapture.Subscription? = null
        var stream: MicrophoneCapture.Subscription? = null
        var track: AudioTrack? = null
        var output: AutoCloseable? = null
        var writer: Thread? = null
        val writing = java.util.concurrent.atomic.AtomicBoolean(true)
        val writerFailure = java.util.concurrent.atomic.AtomicReference<Exception?>(null)
        var saved: MediaAudioProfile? = null
        var savedMicSource: Int? = null
        var problem: Exception? = null
        var interrupted = false
        try {
            activity = startActivitySync(Intent(targetContext, MediaAudioProbeActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            check(MicrophonePermission.granted()) { "RECORD_AUDIO is required for the explicit microphone probe" }
            MediaAudioController.awaitIdle()
            saved = MediaAudioController.state.value.preferred
            micSource?.let { source ->
                val provider = com.utun.chat.ChatApplication.instance.configProvider
                savedMicSource = provider.getRadioMicSource()
                kotlinx.coroutines.runBlocking { provider.setRadioMicSource(source) }
                android.util.Log.i("MediaAudioProbe", "microphone source override=$source saved=$savedMicSource")
            }
            MediaAudioController.select(MediaAudioProfile.PHONE)
            MediaAudioController.awaitIdle()
            val usage = if (mediaOutput) AudioAttributes.USAGE_MEDIA else AudioAttributes.USAGE_VOICE_COMMUNICATION
            val sink = AudioTrack.Builder().setAudioAttributes(AudioAttributes.Builder().setUsage(usage).build())
                .setAudioFormat(AudioFormat.Builder().setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(15360).setTransferMode(AudioTrack.MODE_STREAM).build()
            track = sink
            output = MediaAudioController.attachOutput(sink)
            sink.play()
            writer = Thread({
                try {
                    val silence = ShortArray(960)
                    while (writing.get()) {
                        val written = sink.write(silence, 0, silence.size, AudioTrack.WRITE_NON_BLOCKING)
                        check(written >= 0) { "AudioTrack write failed rc=$written" }
                        Thread.sleep(10)
                    }
                } catch (e: Exception) {
                    if (writing.get()) {
                        writerFailure.set(e)
                        android.util.Log.e("MediaAudioProbe", "output writer failed", e)
                    }
                }
            }, "media-probe-output")
            writer.start()
            // Локальный эквивалент трёх нажатий PTT: уровни PCM и время старта, без radioTalkBegin и сети.
            if (startup) repeat(3) { attempt ->
                val started = System.nanoTime()
                val subscription = MediaAudioController.subscribe("radio")
                try {
                    var firstSignal = false
                    var reportAtMs = 0L
                    while (System.nanoTime() - started < 4_000_000_000L) {
                        val pcm = frame(subscription)
                        val elapsedMs = (System.nanoTime() - started) / 1_000_000
                        val peak = pcm.pcm.maxOf { kotlin.math.abs(it.toInt()) }
                        if (elapsedMs >= reportAtMs || (!firstSignal && peak > 0)) {
                            val rms = kotlin.math.sqrt(pcm.pcm.sumOf { it.toDouble() * it } / pcm.pcm.size)
                            android.util.Log.i("MediaAudioProbe", "startup attempt=$attempt elapsedMs=$elapsedMs " +
                                "generation=${pcm.generation} peak=$peak rms=${rms.toInt()} firstSignal=${!firstSignal && peak > 0} " +
                                "switching=${MediaAudioController.state.value.switching}")
                            reportAtMs = (elapsedMs / 500 + 1) * 500
                        }
                        if (peak > 0) firstSignal = true
                    }
                } finally { subscription.close() }
                Thread.sleep(750)
            }
            val first = MediaAudioController.subscribe("radio"); radio = first
            repeat(15) { frame(first) }
            val second = MediaAudioController.subscribe("stream"); stream = second
            var previous = -1L
            for (profile in listOf(MediaAudioProfile.PHONE, MediaAudioProfile.AUTO, MediaAudioProfile.PHONE)) {
                MediaAudioController.select(profile)
                val until = System.nanoTime() + 10_000_000_000L
                var count = 0
                do {
                    val a = frame(second)
                    if (previous >= 0) check(a.timeNs == previous + 20_000_000L) { "Non-contiguous stream PTS $previous -> ${a.timeNs}" }
                    previous = a.timeNs
                    var b = frame(first)
                    while (b.sequence < a.sequence) b = frame(first)
                    check(a.sequence == b.sequence && a.timeNs == b.timeNs && a.pcm.contentEquals(b.pcm)) { "Consumers received different capture" }
                    val unchanged = b.pcm.copyOf()
                    a.pcm.fill(123)
                    check(b.pcm.contentEquals(unchanged)) { "PCM copies are shared" }
                    check(System.nanoTime() < until) { "Profile did not settle: ${MediaAudioController.state.value}" }
                    count++
                } while (count < 300 || MediaAudioController.state.value.switching)
                check(MediaAudioController.state.value.notice == null && MediaAudioController.state.value.inputName != null) {
                    "Microphone was not confirmed: ${MediaAudioController.state.value}"
                }
                android.util.Log.i("MediaAudioProbe", "profile verified=$profile frames=$count state=${MediaAudioController.state.value}")
            }
            // Непрерывный аппаратный участок длиннее окна диагностической сводки PCM clock.
            repeat(300) {
                val a = frame(second)
                check(a.timeNs == previous + 20_000_000L) { "Steady capture PTS discontinuity" }
                previous = a.timeNs
                var b = frame(first)
                while (b.sequence < a.sequence) b = frame(first)
                check(a.sequence == b.sequence && a.pcm.contentEquals(b.pcm)) { "Steady consumers differ" }
            }
            android.util.Log.i("MediaAudioProbe", "steady hardware capture verified frames=300")
            first.close(); radio = null
            val next = frame(second)
            check(next.timeNs == previous + 20_000_000L) { "Closing radio interrupted stream capture" }
            val remaining = MediaAudioController.subscribe("radio"); radio = remaining
            val beforeDetach = frame(remaining)
            second.close(); stream = null
            val afterDetach = frame(remaining)
            check(afterDetach.timeNs == beforeDetach.timeNs + 20_000_000L && afterDetach.generation == beforeDetach.generation) {
                "Closing stream restarted the remaining radio microphone"
            }
            // Повторяем обычное отпускание PTT именно на Bluetooth, сохраняя работающий выход.
            if (MediaAudioController.state.value.bluetoothAvailable && !MediaAudioController.state.value.wiredAvailable) {
                MediaAudioController.select(MediaAudioProfile.BLUETOOTH)
                repeat(300) { frame(remaining) }
                check(!MediaAudioController.state.value.switching && MediaAudioController.state.value.notice == null) {
                    "Bluetooth did not settle before receive-only test: ${MediaAudioController.state.value}"
                }
            }
            val receiveOnlyType = sink.routedDevice?.type
            remaining.close(); radio = null
            // Оставшийся output продолжает принадлежать общей подсистеме после закрытия последнего input.
            Thread.sleep(250)
            check(sink.playState == AudioTrack.PLAYSTATE_PLAYING) { "Closing input stopped shared output" }
            if (receiveOnlyType == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) {
                check(sink.routedDevice?.type == receiveOnlyType) { "Closing microphone changed SCO playback device" }
                val manager = targetContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                val until = System.nanoTime() + 35_000_000_000L
                var head = sink.playbackHeadPosition
                while (System.nanoTime() < until) {
                    check(manager.isBluetoothScoOn && (mediaOutput || manager.mode == AudioManager.MODE_IN_COMMUNICATION)) {
                        "Receive-only SCO stopped after microphone closed: mode=${manager.mode} sco=${manager.isBluetoothScoOn}"
                    }
                    if (Build.VERSION.SDK_INT >= 31) check(manager.communicationDevice?.id == sink.routedDevice?.id) {
                        "Receive-only SCO communication device differs from AudioTrack"
                    }
                    Thread.sleep(1000)
                    val nextHead = sink.playbackHeadPosition
                    check(nextHead != head) { "Receive-only SCO output stalled" }; head = nextHead
                }
                android.util.Log.i("MediaAudioProbe", "receive-only SCO verified usage=$usage seconds=35 device=${sink.routedDevice?.id} mode=${manager.mode}")
                // Приём должен открывать SCO и без предшествующего PTT/AudioRecord.
                MediaAudioController.select(MediaAudioProfile.PHONE)
                var deadline = System.nanoTime() + 6_000_000_000L
                while (sink.routedDevice?.type != AudioDeviceInfo.TYPE_BUILTIN_SPEAKER || manager.mode != AudioManager.MODE_NORMAL) {
                    check(System.nanoTime() < deadline) { "Speaker route did not release SCO" }; Thread.sleep(100)
                }
                MediaAudioController.select(MediaAudioProfile.BLUETOOTH)
                deadline = System.nanoTime() + 30_000_000_000L
                while (sink.routedDevice?.type != AudioDeviceInfo.TYPE_BLUETOOTH_SCO || !manager.isBluetoothScoOn ||
                    (!mediaOutput && manager.mode != AudioManager.MODE_IN_COMMUNICATION) || MediaAudioController.state.value.switching) {
                    check(System.nanoTime() < deadline) { "Receive-only SCO did not start without microphone" }; Thread.sleep(100)
                }
                check(MediaAudioController.state.value.inputName == null) { "Receive-only routing opened a microphone" }
                head = sink.playbackHeadPosition
                Thread.sleep(2000)
                check(head != sink.playbackHeadPosition && manager.isBluetoothScoOn) { "Cold SCO playback did not progress" }
                android.util.Log.i("MediaAudioProbe", "receive-only SCO cold start verified usage=$usage device=${sink.routedDevice?.id} mode=${manager.mode}")
            }
        } catch (e: Exception) {
            if (e is InterruptedException) interrupted = true
            problem = e
            android.util.Log.e("MediaAudioProbe", "probe failed", e)
        } finally {
            fun cleanup(stage: String, action: () -> Unit) {
                try { action() } catch (e: Exception) {
                    if (e is InterruptedException) interrupted = true
                    android.util.Log.e("MediaAudioProbe", "cleanup failed stage=$stage", e)
                    if (problem == null) problem = e else problem!!.addSuppressed(e)
                }
            }
            writing.set(false)
            cleanup("radio subscription") { radio?.close() }
            cleanup("stream subscription") { stream?.close() }
            cleanup("output stop") { track?.stop() }
            cleanup("writer join") {
                writer?.interrupt(); writer?.join(1000)
                check(writer?.isAlive != true) { "Output writer still running; track/route owners remain attached" }
            }
            if (writer?.isAlive != true) {
                cleanup("output route") { output?.close() }
                cleanup("output release") { track?.release() }
                cleanup("media idle") { MediaAudioController.awaitIdle() }
                cleanup("microphone source restore") {
                    savedMicSource?.let { source -> kotlinx.coroutines.runBlocking {
                        com.utun.chat.ChatApplication.instance.configProvider.setRadioMicSource(source)
                    } }
                }
                cleanup("profile restore") { saved?.let { MediaAudioController.select(it); MediaAudioController.awaitIdle() } }
            }
            cleanup("activity finish") { runOnMainSync { activity?.finish() } }
            writerFailure.get()?.let { failure -> if (problem == null) problem = failure else problem!!.addSuppressed(failure) }
            if (interrupted) Thread.currentThread().interrupt()
        }
        finish(if (problem == null) 0 else 1, Bundle().apply {
            putString("result", problem?.toString() ?: "Shared PCM, CAMCORDER, output route and continuous PTS verified")
        })
    }

    private fun frame(subscription: MicrophoneCapture.Subscription): MicrophoneCapture.Frame {
        val deadline = System.nanoTime() + 3_000_000_000L
        while (System.nanoTime() < deadline) (subscription.poll() as? MicrophoneCapture.Frame)?.let { return it }
        error("Shared microphone frame timeout")
    }
}
