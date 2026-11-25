package com.utun.chat.data

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.net.Uri
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import com.utun.chat.MainActivity
import com.utun.chat.ChatApplication
import com.utun.chat.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.collect
import java.util.concurrent.ConcurrentHashMap

/** Main-владелец capture/native publisher/CPU lock/notification. Сохраняет refs до ACK, также после onDestroy.
 * Handler повторяет независимые ошибки; FIN следует после capture.stop=true, доставка ждёт до 4 секунд.
 * Live session не восстанавливается после смерти процесса.
 */
class StreamService : Service() {
    companion object { private val retained = ConcurrentHashMap.newKeySet<StreamService>() }
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var capture: StreamCaptureEngine? = null
    private var lock: PowerManager.WakeLock? = null
    private var id = 0L
    private var stopping = false
    private var aborting = false
    private var poll: Job? = null
    private val main = Handler(Looper.getMainLooper())
    private val retry = Runnable { retryQueued = false; cleanup() }
    private var retryDelay = 250L
    private var retryQueued = false
    private var retryDue = 0L
    private var nativeStopped = false
    private var drainUntil = 0L
    private var destroyed = false
    private var foregroundOwned = false
    private var stopRequested = false
    private var controllerEnded = false
    private var lastStartId = 0
    private var openIntent: PendingIntent? = null
    private var stopIntent: PendingIntent? = null

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        projection?.configurationChanged()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        lastStartId = startId
        if (controllerEnded && !destroyed) {
            if (intent?.action == "stop") { stopSelf(startId); return START_NOT_STICKY }
            id = 0; stopping = false; aborting = false; nativeStopped = false; drainUntil = 0
            stopRequested = false; controllerEnded = false
        }
        if (intent?.action == "stop") {
            val requested = intent.getLongExtra("id", 0)
            if (requested != 0L && requested == StreamController.state.value?.id) StreamController.stop()
            else LogManager.addLog("WARN", "stream", "stale notification stop ignored requested=$requested current=${StreamController.state.value?.id}")
            if (id == 0L) finish()
            return START_NOT_STICKY
        }
        if (id != 0L || stopping) {
            LogManager.addLog("WARN", "stream", "duplicate service start ignored id=$id stopping=$stopping")
            return START_NOT_STICKY
        }
        id = intent?.getLongExtra("id", 0) ?: 0
        val audio = intent?.getBooleanExtra("audio", true) ?: true
        try {
            val notifications = checkNotNull(getSystemService(NotificationManager::class.java)) { "NotificationManager unavailable" }
            notifications.createNotificationChannel(NotificationChannel("live_stream", AppLanguage.text(R.string.stream_title), NotificationManager.IMPORTANCE_LOW))
            val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java).setData(Uri.parse("utun-stream:open:$id")),
                PendingIntent.FLAG_IMMUTABLE).also { openIntent = it }
            val stop = PendingIntent.getService(this, 0, Intent(this, StreamService::class.java).setAction("stop")
                .setData(Uri.parse("utun-stream:stop:$id")).putExtra("id", id), PendingIntent.FLAG_IMMUTABLE).also { stopIntent = it }
            val notification = NotificationCompat.Builder(this, "live_stream").setSmallIcon(android.R.drawable.ic_menu_camera)
                .setContentTitle(AppLanguage.text(R.string.stream_live)).setContentIntent(open).setOngoing(true)
                .addAction(android.R.drawable.ic_media_pause, AppLanguage.text(R.string.stream_stop), stop).build()
            liveNotification = notification
            microphone = audio
            val screen = StreamController.state.value?.pendingConfig?.mode?.screen == true
            promoteCapture(screen, !screen)
            if (id == 0L || !StreamController.serviceReady(id, this)) { finish(); return START_NOT_STICKY }
            lock = checkNotNull(getSystemService(PowerManager::class.java)) { "PowerManager unavailable" }
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "utun:live_stream")
            lock!!.setReferenceCounted(false); lock!!.acquire()
            val publisher = StreamController.state.value ?: error("Missing publisher")
            val config = publisher.pendingConfig ?: error("Missing capture configuration")
            val provider = ChatApplication.instance.configProvider
            val n = provider.getInt("chat.stream_subblocks")
            val r = provider.getInt("chat.stream_rs_redundant")
            val regularLimit = provider.getInt("chat.stream_regular_limit")
            if (!StreamFecParameters.valid(n, r, regularLimit)) {
                StreamController.failed(id, "settings rejected n=$n r=$r limit=$regularLimit", R.string.stream_capacity_exceeded)
                shutdown(abort = true); return START_NOT_STICKY
            }
            val audioSettings = if (audio) runBlocking { provider.radioCompressorSettings.first() } else RadioCompressorSettings()
            capture = StreamCaptureEngine(this, id, publisher.group, config, audio, publisher.preview, audioSettings,
                { active, restored -> StreamController.captureReady(id, active, restored) },
                { detail -> StreamController.failed(id, detail) })
            if (screen) enableProjection(checkNotNull(StreamController.takeGrant(id)))
            capture!!.start()
            if (audio) scope.launch {
                try { provider.radioCompressorSettings.collect { capture?.audioCompressorSettings = it } }
                catch (e: CancellationException) { throw e }
                catch (e: Exception) {
                    StreamController.failed(id, "audio compressor settings: ${e.message}"); shutdown(abort = true)
                }
            }
            poll = scope.launch {
                try {
                val deadline = System.nanoTime() + 10_000_000_000
                var opened = false
                var nextStats = 0L
                while (true) {
                    val now = System.nanoTime()
                    if (now >= nextStats) {
                        nextStats = now + 100_000_000
                        NativeLib.streamTxStats(id)?.let { StreamController.updateTx(id, it) }
                    }
                    val event = NativeLib.streamPull(id)
                    if (event != null) when (event[0].toInt()) {
                        4 -> { opened = true; StreamController.opened(id) }
                        5 -> { finish(); return@launch }
                        6 -> {
                            val reason = event.getOrNull(1)?.toInt()
                            StreamController.failed(id, "native session closed reason=$reason",
                                if (reason == 7) R.string.stream_capacity_exceeded else R.string.stream_failed)
                            shutdown(abort = true); return@launch
                        }
                    }
                    if (!NativeLib.isReady() || (!opened && System.nanoTime() > deadline)) {
                        StreamController.failed(id, "core stopped/open timeout"); shutdown(abort = true); return@launch
                    }
                    delay(20)
                }
                } catch (e: CancellationException) { throw e }
                catch (e: Exception) {
                    LogManager.addLog("ERROR", "stream", "publisher polling failed id=$id: $e")
                    StreamController.failed(id, "publisher polling: $e"); shutdown(abort = true)
                }
            }
            LogManager.addLog("INFO", "stream", "foreground started id=$id group=${publisher.group} microphone=$audio CPU lock=${lock?.isHeld}")
        } catch (e: Exception) {
            StreamController.failed(id, "foreground service: ${e.message}")
            shutdown(abort = true)
        }
        return START_NOT_STICKY
    }

    private var liveNotification: android.app.Notification? = null
    private var microphone = false
    private var sourceSwitch = false
    private var projection: ScreenProjection? = null

    private fun promoteCapture(screen: Boolean, camera: Boolean) {
        var type = 0
        if (Build.VERSION.SDK_INT >= 29 && screen) type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
        if (Build.VERSION.SDK_INT >= 30 && camera) type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
        if (Build.VERSION.SDK_INT >= 30 && microphone) type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        foregroundOwned = true
        ServiceCompat.startForeground(this, 1004, checkNotNull(liveNotification), type)
    }

    private fun enableProjection(grant: Intent) {
        val sessionId = id
        projection = ScreenProjection(this, grant) { if (id == sessionId && !stopping) StreamController.stop() }
        projection!!.onOrientationChanged = { resizeScreen() }
        capture!!.projection = projection
    }

    internal fun resizeScreen() {
        val st = StreamController.state.value ?: return
        val config = st.config ?: return
        val portrait = projection?.portrait ?: return
        val mode = config.mode
        if (st.id != id || st.phase != StreamPhase.LIVE || st.pendingConfig != null || !mode.screen || (mode.height > mode.width) == portrait) return
        try {
            val next = StreamCameraModes.screenModes(portrait).firstOrNull {
                it.width == mode.height && it.height == mode.width && it.videoCodec == mode.videoCodec
            }
            if (next != null) StreamController.change(config.withMode(next))
            else LogManager.addLog("INFO", "video", "screen resize keeps supported encoder dimensions id=$id mode=$mode")
        } catch (e: Exception) { LogManager.addLog("WARN", "video", "screen resize unavailable id=$id: $e") }
    }

    fun confirmedSource(config: StreamCaptureConfig) {
        if (stopping) return
        try {
            if (!config.mode.screen) { projection?.close(); projection = null; capture?.projection = null }
            if (sourceSwitch) promoteCapture(config.mode.screen, !config.mode.screen)
            sourceSwitch = false
        } catch (e: Exception) { StreamController.failed(id, "source foreground update failed: $e") }
    }

    fun change(config: StreamCaptureConfig, grant: Intent? = null) {
        check(!stopping && capture != null) { "Publisher stopped" }
        val current = checkNotNull(StreamController.state.value?.config)
        if (current.mode.screen != config.mode.screen) {
            try {
                sourceSwitch = true
                promoteCapture(true, true)
                if (config.mode.screen) enableProjection(checkNotNull(grant))
            } catch (e: Exception) { confirmedSource(current); throw e }
        }
        capture!!.change(config)
    }

    fun setAudioEnabled(enabled: Boolean): Boolean {
        if (stopping || capture == null) {
            LogManager.addLog("WARN", "stream", "service audio change rejected id=$id"); return false
        }
        return capture!!.setAudioEnabled(enabled)
    }

    /** Producers завершатся до FIN; при уничтожении ядра очередь не ждём. */
    fun shutdown(abort: Boolean = false) {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Stream service shutdown requires main" }
        if (controllerEnded) return
        aborting = aborting || abort
        stopping = true; retained.add(this); cleanup()
    }

    private fun finish() {
        shutdown(abort = true)
    }

    /** Main остаётся доступен для SDK callbacks; false от engine сохраняет владельца до следующей попытки. */
    private fun cleanup() {
        capture?.let { owned -> try {
            if (owned.stop()) capture = null
            else LogManager.addLog("DEBUG", "stream", "publisher capture cleanup pending id=$id")
        } catch (e: Exception) { LogManager.addLog("ERROR", "stream", "publisher capture stop failed id=$id; retained: $e") } }
        try { projection?.close(); projection = null }
        catch (e: Exception) { LogManager.addLog("ERROR", "stream", "screen projection close failed id=$id: $e") }
        if (capture == null && projection == null && !nativeStopped) try {
            if (id != 0L) NativeLib.streamStop(id)
            nativeStopped = true; drainUntil = SystemClock.elapsedRealtime() + 4000
            StreamController.producersReleased(id)
            LogManager.addLog("INFO", "stream", "publisher producers released id=$id; FIN requested abort=$aborting")
        } catch (e: Exception) { LogManager.addLog("ERROR", "stream", "publisher native stop failed id=$id; retained: $e") }
        val draining = !aborting && !destroyed && nativeStopped && SystemClock.elapsedRealtime() < drainUntil
        if (capture == null && nativeStopped && !draining) {
            poll?.cancel(); poll = null
            lock?.let { owned -> try { if (owned.isHeld) owned.release(); lock = null } catch (e: Exception) {
                LogManager.addLog("ERROR", "stream", "publisher CPU lock release failed id=$id; retained: $e")
            } }
            openIntent?.let { owned -> try { owned.cancel(); openIntent = null } catch (e: Exception) {
                LogManager.addLog("ERROR", "stream", "publisher open token cancellation failed id=$id; retained: $e")
            } }
            stopIntent?.let { owned -> try { owned.cancel(); stopIntent = null } catch (e: Exception) {
                LogManager.addLog("ERROR", "stream", "publisher stop token cancellation failed id=$id; retained: $e")
            } }
            if (foregroundOwned) try { stopForeground(STOP_FOREGROUND_REMOVE); foregroundOwned = false } catch (e: Exception) {
                LogManager.addLog("ERROR", "stream", "publisher foreground removal failed id=$id; retained: $e")
            }
            if (!stopRequested) try { if (!destroyed) stopSelf(lastStartId); stopRequested = true } catch (e: Exception) {
                LogManager.addLog("ERROR", "stream", "publisher service stop failed id=$id; retained: $e")
            }
            if (lock == null && openIntent == null && stopIntent == null && !foregroundOwned && stopRequested && !controllerEnded) try {
                StreamController.ended(id, this); controllerEnded = true
            } catch (e: Exception) { LogManager.addLog("ERROR", "stream", "publisher completion failed id=$id; retained: $e") }
        }
        if (controllerEnded) {
            main.removeCallbacks(retry); retryQueued = false
            retained.remove(this); retryDelay = 250
            LogManager.addLog("INFO", "stream", "publisher service cleanup confirmed id=$id")
        } else {
            val delay = if (draining) minOf(250, drainUntil - SystemClock.elapsedRealtime()).coerceAtLeast(1) else retryDelay
            val due = SystemClock.elapsedRealtime() + delay
            if (retryQueued && retryDue <= due) return
            if (!draining) retryDelay = (retryDelay * 2).coerceAtMost(30_000)
            main.removeCallbacks(retry); retryDue = due; retryQueued = main.postDelayed(retry, delay)
            if (!retryQueued) LogManager.addLog("ERROR", "stream", "publisher cleanup retry rejected id=$id; owner retained")
            else LogManager.addLog("DEBUG", "stream", "publisher cleanup scheduled id=$id delayMs=$delay capture=${capture != null} drain=$draining")
        }
    }

    override fun onDestroy() {
        destroyed = true; scope.cancel()
        try { shutdown(abort = true); LogManager.addLog("INFO", "stream", "foreground destroyed id=$id pending=${!controllerEnded}") }
        finally { super.onDestroy() }
    }
    override fun onBind(intent: Intent?): IBinder? = null
}
