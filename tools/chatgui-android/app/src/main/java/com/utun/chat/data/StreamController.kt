package com.utun.chat.data

import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import androidx.core.content.ContextCompat
import com.utun.chat.ChatApplication
import com.utun.chat.R
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import java.util.concurrent.atomic.AtomicLong

enum class StreamPhase { STARTING, LIVE, STOPPING, ENDED, FAILED }
data class StreamState(val id: Long, val group: String, val phase: StreamPhase, val error: Int? = null,
                       val config: StreamCaptureConfig? = null, val pendingConfig: StreamCaptureConfig? = null, val notice: Int? = null,
                       val preview: StreamPreview = StreamPreview(), val audioAvailable: Boolean = false, val audioEnabled: Boolean = false) {
    val ongoing get() = phase == StreamPhase.STARTING || phase == StreamPhase.LIVE || phase == StreamPhase.STOPPING
}

/** Один publisher на процесс; StateFlow/service lifecycle — main, camera/audio не держат UI.
 * STOPPING/capturing сохраняют владельца до полного ACK сервиса. requireReleased блокирует native teardown,
 * также до завершения отменённого foreground handshake. Запоздалые callbacks проверяют session ID/owner.
 */
object StreamController {
    private val ids = AtomicLong(System.nanoTime().coerceAtLeast(1))
    fun nextId(): Long = ids.incrementAndGet()
    private val main = Handler(Looper.getMainLooper())
    private val mutable = MutableStateFlow<StreamState?>(null)
    val state: StateFlow<StreamState?> = mutable
    private val mutableTx = MutableStateFlow<StreamTxStats?>(null)
    val txStats: StateFlow<StreamTxStats?> = mutableTx
    fun updateTx(id: Long, data: LongArray) {
        check(Looper.myLooper() == Looper.getMainLooper())
        if (mutable.value?.id == id && mutable.value?.ongoing == true) try { mutableTx.value = StreamTxStats.fromNative(id, data) }
        catch (e: Exception) { LogManager.addLog("ERROR", "stream", "invalid publisher TX snapshot id=$id fields=${data.size}; previous retained: $e") }
    }
    @Volatile var capturing = false
        private set
    private var startGrant: Intent? = null
    internal fun takeGrant(id: Long): Intent? = if (state.value?.id == id) startGrant.also { startGrant = null } else null
    private var service: StreamService? = null
    private val captureReleased = MutableStateFlow(true)
    fun producersReleased(id: Long) {
        if (mutable.value?.id == id) {
            captureReleased.value = true
            LogManager.addLog("INFO", "stream", "publisher capture released id=$id; delivery may drain")
        }
    }
    private val settingsScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** Запись переживает закрытие экрана; DataStore сохраняет выбор одним изменением. */
    fun rememberSettings(config: StreamCaptureConfig? = null, audio: Boolean? = null) {
        settingsScope.launch {
            try { ChatApplication.instance.configProvider.saveStreamCaptureSettings(config?.takeUnless { it.mode.screen }, audio).await() }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) { LogManager.addLog("ERROR", "stream", "capture settings save failed: $e") }
        }
    }

    fun start(group: String, config: StreamCaptureConfig, audio: Boolean, grant: Intent? = null) {
        check(Looper.myLooper() == Looper.getMainLooper())
        if (mutable.value?.ongoing == true) { LogManager.addLog("WARN", "stream", "publisher already owned"); return }
        val id = nextId()
        if (NativeLib.isCoreStopping() || !NativeLib.isReady() || group.isEmpty() || CallController.blocksMedia() || CallController.state.value?.ongoing == true ||
            NativeLib.voiceIsActive()) {
            LogManager.addLog("WARN", "stream", "start rejected: core/call/voice busy group=$group")
            mutable.value = StreamState(id, group, StreamPhase.FAILED, R.string.stream_resources_busy); return
        }
        startGrant = grant
        capturing = true
        captureReleased.value = false
        rememberSettings(config, audio)
        mutable.value = StreamState(id, group, StreamPhase.STARTING, pendingConfig = config, audioAvailable = audio, audioEnabled = audio)
        mutableTx.value = null
        try {
            checkNotNull(ContextCompat.startForegroundService(ChatApplication.instance, Intent(ChatApplication.instance, StreamService::class.java)
                .putExtra("id", id).putExtra("audio", audio))) { "Publisher foreground service not found" }
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "stream", "service start failed id=$id: ${e.message}")
            startGrant = null; capturing = false; mutable.value = StreamState(id, group, StreamPhase.FAILED, R.string.stream_failed)
            captureReleased.value = true
        }
    }

    fun change(config: StreamCaptureConfig, grant: Intent? = null) {
        check(Looper.myLooper() == Looper.getMainLooper())
        val st = mutable.value ?: return
        val owner = service
        if (st.phase != StreamPhase.LIVE || st.pendingConfig != null || owner == null) {
            LogManager.addLog("WARN", "video", "publisher change rejected id=${st.id} phase=${st.phase} pending=${st.pendingConfig}"); return
        }
        if (st.config == config) return
        mutable.value = st.copy(pendingConfig = config, notice = null)
        try { owner.change(config, grant) } catch (e: Exception) {
            LogManager.addLog("ERROR", "stream", "capture change rejected id=${st.id}: $e")
            mutable.value = st.copy(notice = R.string.stream_mode_restored)
        }
    }

    fun setAudioEnabled(enabled: Boolean) {
        check(Looper.myLooper() == Looper.getMainLooper())
        val st = mutable.value ?: return
        if (st.phase != StreamPhase.LIVE || !st.audioAvailable || service == null) {
            LogManager.addLog("WARN", "stream", "audio control unavailable id=${st.id} phase=${st.phase}"); return
        }
        if (st.audioEnabled == enabled) return
        try {
            if (service!!.setAudioEnabled(enabled)) mutable.value = st.copy(audioEnabled = enabled)
            else failed(st.id, "audio control failed enabled=$enabled")
        } catch (e: Exception) { failed(st.id, "audio control failed enabled=$enabled: $e") }
    }

    fun captureReady(id: Long, config: StreamCaptureConfig, restored: Boolean) {
        if (!main.post {
            val st = mutable.value ?: return@post
            if (st.id != id || !st.ongoing || st.phase == StreamPhase.STOPPING) return@post
            mutable.value = st.copy(config = config, pendingConfig = null, notice = if (restored) R.string.stream_mode_restored else null)
            service?.confirmedSource(config)
            rememberSettings(config)
            if (!restored) service?.resizeScreen()
        }) LogManager.addLog("ERROR", "stream", "capture ready post rejected id=$id; publisher owner retained")
    }

    fun serviceReady(id: Long, owner: StreamService): Boolean {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Publisher service handshake requires main" }
        if (mutable.value?.id != id || mutable.value?.phase != StreamPhase.STARTING || (service != null && service !== owner)) {
            LogManager.addLog("WARN", "stream", "stale publisher service rejected id=$id current=${mutable.value?.id} phase=${mutable.value?.phase}")
            return false
        }
        service = owner; return true
    }

    fun opened(id: Long) {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Publisher open callback requires main" }
        val st = mutable.value ?: return
        if (st.id == id && st.phase == StreamPhase.STARTING) {
            mutable.value = st.copy(phase = StreamPhase.LIVE)
            service?.resizeScreen()
        }
    }

    fun failed(id: Long, detail: String, message: Int = R.string.stream_failed) {
        val update = Runnable {
            val st = mutable.value
            if (st == null || st.id != id || !st.ongoing) return@Runnable
            LogManager.addLog("ERROR", "stream", "publisher failed id=$id $detail")
            mutable.value = st.copy(error = message); stop()
        }
        if (Looper.myLooper() == Looper.getMainLooper()) update.run()
        else if (!main.post(update)) LogManager.addLog("ERROR", "stream", "publisher failure post rejected id=$id detail=$detail; owner retained")
    }

    fun stop() {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Publisher stop requires main" }
        val st = mutable.value ?: return
        if (!st.ongoing) return
        if (st.phase != StreamPhase.STOPPING) mutable.value = st.copy(phase = StreamPhase.STOPPING)
        service?.shutdown() // До onStartCommand отменённый start завершит foreground-handshake и остановится.
    }

    /** STOPPING заканчивается только после release capture и завершения foreground-сервиса. */
    suspend fun stopForCall() {
        val st = mutable.value ?: return
        if (!st.ongoing) return
        LogManager.addLog("INFO", "stream", "stopping publisher for call id=${st.id}")
        stop()
        captureReleased.first { it }
        LogManager.addLog("INFO", "stream", "publisher released for call id=${st.id}")
    }

    fun ended(id: Long, owner: StreamService) {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Publisher completion requires main" }
        if (service != null && service !== owner) { LogManager.addLog("WARN", "stream", "foreign publisher completion ignored id=$id"); return }
        val st = mutable.value ?: return
        if (st.id != id || !st.ongoing) return
        if (service === owner) service = null
        startGrant = null
        st.preview.close()
        capturing = false
        captureReleased.value = true
        mutableTx.value = null
        mutable.value = st.copy(phase = if (st.error != null) StreamPhase.FAILED else StreamPhase.ENDED, pendingConfig = null)
        LogManager.addLog("INFO", "stream", "publisher released id=$id")
    }

    fun reset() {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Publisher reset requires main" }
        val st = mutable.value
        if (st?.ongoing == true) {
            mutable.value = st.copy(phase = StreamPhase.STOPPING, error = R.string.stream_failed)
            service?.shutdown(abort = true)
        }
    }

    /** Незавершённый foreground handshake и SDK cleanup запрещают освобождать native publisher. */
    fun requireReleased() {
        check(!capturing && service == null) {
            LogManager.addLog("ERROR", "stream", "core teardown blocked by publisher id=${mutable.value?.id} phase=${mutable.value?.phase}")
            "Stream publisher cleanup pending"
        }
    }

    /** Звонок/голосовое не отбирают микрофон у foreground-трансляции; рация использует общий захват. */
    fun blocksCapture(): Boolean {
        if (!capturing) return false
        LogManager.addLog("WARN", "stream", "microphone/camera owned by stream id=${mutable.value?.id}")
        if (!main.post { Toast.makeText(AppLanguage.context(), R.string.stream_resources_busy, Toast.LENGTH_SHORT).show() })
            LogManager.addLog("ERROR", "stream", "capture busy notice post rejected")
        return true
    }
}
