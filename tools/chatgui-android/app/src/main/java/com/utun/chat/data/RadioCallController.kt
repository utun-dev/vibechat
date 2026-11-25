package com.utun.chat.data

import android.app.NotificationManager
import android.content.Intent
import android.os.Looper
import android.widget.Toast
import androidx.core.app.NotificationCompat
import com.utun.chat.ChatApplication
import com.utun.chat.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Состояния карточки; PREPARING локален, ENDED соответствует FINISHED ядра либо локальному отказу очереди. */
enum class RadioCallPhase { OUTGOING, INCOMING, RINGING, PREPARING, ENDED, WAIT_DECISION, ANSWERING }
/** id — ненулевой uint64 как биты Long, peer — Ed25519, channelId — десятичный uint64.
 * Имена — снимок для UI; incoming фиксирует роль, reason — radio_call_reason, без перевода в модели. */
data class RadioCallState(
    val id: Long, val channelId: String, val peer: String, val peerName: String, val channelName: String,
    val incoming: Boolean, val phase: RadioCallPhase, val reason: Int = 0
)
/** Ожидаемое включение рации Activity; результат возвращается в activated с тем же id. */
data class RadioCallActivation(val id: Long, val channelId: String)

/** На main хранит живое приглашение рации; StateFlow повторяет текущий снимок после пересоздания UI.
 * Ядро владеет deadline/сигналингом, приложение — оповещением и запросом async-включения обычной рации.
 * Входящий ACCEPT отправляется только после activated(success=true) из видимой Activity.
 * Завершение приглашения не выключает обычную рацию. Отказ очереди логируется и завершает локальную попытку. */
object RadioCallController {
    private const val START = 1
    private const val READY = 2
    private const val ACCEPT = 3
    private const val DECLINE = 4
    private const val CANCEL = 5
    private const val FAIL = 6
    private const val BUSY = 7
    private const val CONTINUE = 8
    private const val ANSWER = 9
    private var activationCleanup: (() -> Unit)? = null
    private const val RESULT_NOTIFICATION_ID = 1006 // Отдельно от incoming, FGS звонка/трансляции/оверлея и результата P2P-звонка.
    private data class PendingStart(val id: Long, val group: String, val peer: String)
    private var pendingStart: PendingStart? = null // START принят приложением, первое событие ядра ещё не получено.
    private var clearJob: Job? = null // Только задержка удаления ENDED-карточки; deadline вызова принадлежит ядру.
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow<RadioCallState?>(null)
    val state: StateFlow<RadioCallState?> = _state
    private val _activation = MutableStateFlow<RadioCallActivation?>(null)
    val activation: StateFlow<RadioCallActivation?> = _activation
    private var visible = false
    private var answerOnVisible = 0L
    private var ready = false
    /** На main: занят также между принятым START и первым событием; ENDED-карточка не удерживает активность. */
    fun busy(): Boolean = pendingStart != null || _state.value?.phase?.let { it != RadioCallPhase.ENDED } == true

    /** На main запросить START с ненулевыми ID канала и участника. Busy снимает первое событие либо reset/отказ очереди. */
    fun start(channel: String, peer: String, devices: Boolean = false) {
        checkMain()
        val nativeReady = NativeLib.isReady()
        val phoneCall = CallController.state.value?.ongoing == true
        if (!ready || !nativeReady || busy() || phoneCall) {
            LogManager.addLog("WARN", "radio", "Call start unavailable core=$ready native=$nativeReady busy=${busy()} phoneCall=$phoneCall")
            showOutcome(if (ready && nativeReady) 3 else 5); return
        }
        val group = channel
        if (!MemberFilesProtocol.validAddress(group) || !MemberFilesProtocol.validAddress(peer)) {
            LogManager.addLog("ERROR", "radio", "Invalid call target channel=$channel peer=$peer")
            showOutcome(5); return
        }
        if (_state.value?.phase == RadioCallPhase.ENDED) clear()
        var id = java.security.SecureRandom().nextLong()
        if (id == 0L) id = 1
        pendingStart = PendingStart(id, group, peer)
        syncActivity()
        if (!command(id, START, group, peer, devices)) { pendingStart = null; syncActivity(); showOutcome(5) }
    }

    /** Activity onStart/onStop: отложенный ответ из уведомления допустим только своему ещё входящему вызову. */
    fun setVisible(value: Boolean) {
        checkMain()
        visible = value
        if (value && answerOnVisible != 0L) {
            val id = answerOnVisible; answerOnVisible = 0
            if (_state.value?.id == id) accept()
        }
    }

    /** Старый PendingIntent не принимает новый вызов и не восстанавливает завершённый. */
    fun notificationIntent(intent: Intent?) {
        checkMain()
        if (intent?.getBooleanExtra(IncomingCallManager.EXTRA_RADIO_CALL, false) != true) return
        val id = intent.getLongExtra(IncomingCallManager.EXTRA_CALL_ID, 0)
        if (!intent.getBooleanExtra(IncomingCallManager.EXTRA_ANSWER_RADIO, false)) return
        intent.removeExtra(IncomingCallManager.EXTRA_ANSWER_RADIO)
        if (_state.value?.id != id || _state.value?.phase != RadioCallPhase.INCOMING) return
        if (visible) accept() else answerOnVisible = id
    }

    /** На main отправить решение пользователя; активацию разрешит только SELECTED от ядра. */
    fun accept() {
        checkMain()
        val st = _state.value ?: return
        if (!visible || st.phase != RadioCallPhase.INCOMING) return
        if (!MicrophonePermission.granted()) {
            MicrophonePermission.request("Answer radio call id=${st.id}") {
                if (_state.value?.id == st.id && _state.value?.phase == RadioCallPhase.INCOMING) accept()
            }
            return
        }
        LogManager.addLog("INFO", "radio", "Call answer id=${st.id} sharedStream=${StreamController.capturing}")
        IncomingCallManager.stopRinging()
        _state.value = st.copy(phase = RadioCallPhase.ANSWERING)
        if (!command(st.id, ANSWER)) { finish(st, 7); return }
        LogManager.addLog("INFO", "radio", "Call answer queued id=${st.id} channel=${st.channelId}; awaiting selection")
    }

    /** На main после фактического async-запуска аудио. Принимается один результат своей PREPARING-активации;
     * входящий ждёт FINISHED после ACCEPT/FAIL, успешный исходящий снимает карточку сразу. */
    fun activated(id: Long, success: Boolean) {
        checkMain()
        val st = _state.value ?: return
        if (st.id != id || st.phase != RadioCallPhase.PREPARING || _activation.value?.id != id) {
            LogManager.addLog("DEBUG", "radio", "Call activation ignored id=$id success=$success current=${st.id}/${st.phase}")
            return
        }
        LogManager.addLog(if (success) "INFO" else "ERROR", "radio", "Call activation id=$id success=$success incoming=${st.incoming}")
        if (st.incoming) {
            if (!command(id, if (success) ACCEPT else FAIL)) finish(st, 7)
        } else if (success) { activationCleanup = null; clear() } else finish(st, 7)
    }

    /** На main завершить своё приглашение: DECLINE входящего, иначе CANCEL. Локальная очистка не ждёт очереди. */
    fun end(id: Long = _state.value?.id ?: 0L) {
        checkMain()
        val st = _state.value ?: return
        if (id != st.id || st.phase == RadioCallPhase.ENDED) return
        command(id, if (st.incoming && st.phase == RadioCallPhase.INCOMING) DECLINE else CANCEL)
        finish(st, if (st.incoming && st.phase == RadioCallPhase.INCOMING) 2 else 4)
    }

    /** На main при stop/restart ядра снять pending/activation/карточку/таймер; SERVICE_STARTED снова разрешает команды. */
    fun reset() {
        checkMain()
        ready = false
        if (NativeLib.isReady()) {
            if (_state.value?.phase?.let { it != RadioCallPhase.ENDED } == true) end()
            pendingStart?.let { command(it.id, CANCEL) }
        }
        clear()
    }

    /** Main-dispatcher событий: 12/13 — start/stop ядра, 65 — id:8LE + group Ed25519:32 + peer Ed25519:32 + phase/reason/incoming:1.
     * Проверяются размер, ненулевые ID, сочетание роли/фазы/причины и полная identity живого запроса. */
    fun onEvent(type: Int, data: ByteArray?) {
        checkMain()
        if (type == 12) { ready = NativeLib.isReady(); return }
        if (type == 13) { reset(); return }
        if (type != 65 || !ready || !NativeLib.isReady()) return
        if (data == null || data.size != 75 || data[72].toInt() !in 1..7 || data[73].toInt() !in 0..9 || data[74].toInt() !in 0..1) {
            LogManager.addLog("ERROR", "radio", "Invalid call event size=${data?.size} phase=${data?.getOrNull(72)?.toUByte()} " +
                "reason=${data?.getOrNull(73)?.toUByte()} incoming=${data?.getOrNull(74)?.toUByte()}")
            return
        }
        val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        val id = buf.long; val group = MemberFilesProtocol.addressHex(ByteArray(32).also { buf.get(it) })
        val peer = MemberFilesProtocol.addressHex(ByteArray(32).also { buf.get(it) })
        val phase = buf.get().toInt(); val reason = buf.get().toInt(); val incoming = buf.get().toInt() != 0
        if (id == 0L || group.isEmpty() || peer.isEmpty() || (phase == 4 && reason == 0) || (phase != 4 && reason != 0) ||
            (phase in listOf(2, 6, 7) && !incoming) || (phase in listOf(1, 3, 5) && incoming)) {
            LogManager.addLog("ERROR", "radio", "Invalid call event id=$id group=$group peer=$peer phase=$phase reason=$reason incoming=$incoming")
            return
        }
        val current = _state.value
        val pending = pendingStart
        if ((current?.id == id && (current.channelId != group || (current.incoming && current.peer != peer) || current.incoming != incoming)) ||
            (pending?.id == id && (pending.group != group || pending.peer != peer || incoming))) {
            LogManager.addLog("WARN", "radio", "Call event identity mismatch id=$id group=$group peer=$peer incoming=$incoming " +
                "current=$current pending=$pending")
            return
        }
        LogManager.addLog("DEBUG", "radio", "Call event id=$id group=$group peer=$peer phase=$phase reason=$reason " +
            "incoming=$incoming current=${current?.phase} pending=${pending?.id}")
        if (phase == 1 || phase == 2) {
            if (current?.id == id) return
            if (!incoming && pending?.id != id) {
                LogManager.addLog("WARN", "radio", "Call outgoing event without pending start ignored id=$id")
                return
            }
            if (pending?.id == id) pendingStart = null
            if (busy() || CallController.state.value?.ongoing == true) { command(id, BUSY); syncActivity(); return }
            val channelId = group
            var name = peer
            var channelName = channelId
            try {
                val repo = ChatRepository()
                name = repo.getMembers(channelId).firstOrNull { it.ed25519 == peer }?.name?.takeIf { it.isNotEmpty() } ?: name
                channelName = repo.getChannels()?.firstOrNull { it.id == channelId }?.name ?: channelId
            } catch (e: Exception) {
                LogManager.addLog("WARN", "radio", "Call names unavailable id=$id group=$group peer=$peer; using identifiers: $e")
            }
            clearJob?.cancel(); clearJob = null
            _state.value = RadioCallState(id, channelId, peer, name, channelName, incoming,
                if (incoming) RadioCallPhase.INCOMING else RadioCallPhase.OUTGOING)
            syncActivity()
            if (incoming) {
                IncomingCallManager.show(id, channelId, peer, radio = true)
                if (!command(id, READY)) finish(requireNotNull(_state.value), 7)
            } else CallRingback.startConnecting()
            return
        }
        if (current == null || current.id != id) {
            if (phase == 4 && !incoming && pending?.id == id) {
                pendingStart = null
                syncActivity()
                LogManager.addLog(if (reason in listOf(5, 7)) "ERROR" else "INFO", "radio", "Call finished before UI id=$id reason=$reason")
                showOutcome(reason)
            } else LogManager.addLog("DEBUG", "radio", "Call stale event ignored id=$id phase=$phase")
            return
        }
        if (current.phase == RadioCallPhase.ENDED) return
        if (phase == 7 && incoming && current.phase == RadioCallPhase.ANSWERING) {
            _state.value = current.copy(phase = RadioCallPhase.PREPARING)
            _activation.value = RadioCallActivation(id, current.channelId)
            LogManager.addLog("INFO", "radio", "Call audio requested id=$id channel=${current.channelId} incoming=true; selected by caller")
            return
        }
        if (phase == 7 && incoming && current.phase == RadioCallPhase.PREPARING) {
            LogManager.addLog("DEBUG", "radio", "Call repeated selection ignored id=$id; audio activation already pending")
            return
        }
        if (phase == 6 && incoming) return
        if (phase == 5 && !incoming && current.phase == RadioCallPhase.OUTGOING) {
            CallRingback.stop()
            _state.value = current.copy(phase = RadioCallPhase.WAIT_DECISION)
        } else if (phase == 3 && !incoming && current.phase == RadioCallPhase.OUTGOING) {
            _state.value = current.copy(phase = RadioCallPhase.RINGING)
            CallRingback.startRinging()
        } else if (phase == 4 && (incoming || current.phase != RadioCallPhase.PREPARING)) {
            IncomingCallManager.stopRinging(); CallRingback.stop()
            if (reason == 1) {
                if (incoming) { activationCleanup = null; clear() }
                else {
                    val name = if (peer == current.peer) current.peerName else peer
                    _state.value = current.copy(phase = RadioCallPhase.PREPARING, peer = peer, peerName = name)
                    _activation.value = RadioCallActivation(id, current.channelId)
                    LogManager.addLog("INFO", "radio", "Call audio requested id=$id channel=${current.channelId} incoming=false")
                }
            } else finish(current, reason)
        } else LogManager.addLog("WARN", "radio", "Call event transition ignored id=$id current=${current.phase} phase=$phase")
    }

    private fun finish(st: RadioCallState, reason: Int) {
        IncomingCallManager.stopRinging(); CallRingback.stop()
        activationCleanup?.invoke(); activationCleanup = null
        _activation.value = null; answerOnVisible = 0
        val ended = st.copy(phase = RadioCallPhase.ENDED, reason = reason)
        _state.value = ended
        syncActivity()
        LogManager.addLog(if (reason in listOf(5, 7)) "ERROR" else "INFO", "radio", "Call finished id=${st.id} reason=$reason")
        clearJob?.cancel()
        clearJob = scope.launch { delay(2500); if (_state.value === ended) clear() }
        if (!st.incoming || reason == 7) showOutcome(reason)
    }

    /** На main подтвердить PHONE_BUSY для своей WAIT_DECISION-попытки; CONTINUE сохраняет id, новый срок задаёт ядро. */
    fun continueCall(id: Long) {
        checkMain()
        val st = _state.value ?: return
        if (st.id != id || st.phase != RadioCallPhase.WAIT_DECISION) return
        _state.value = st.copy(phase = RadioCallPhase.OUTGOING)
        LogManager.addLog("INFO", "radio", "Caller overrides phone busy id=$id")
        if (!command(id, CONTINUE)) { finish(st, 5); return }
        CallRingback.startConnecting()
    }

    private fun clear() {
        if (busy()) { IncomingCallManager.stopRinging(); CallRingback.stop() }
        clearJob?.cancel(); clearJob = null
        activationCleanup?.invoke(); activationCleanup = null
        _state.value = null; _activation.value = null; answerOnVisible = 0; pendingStart = null
        syncActivity()
    }

    /** UI resource для radio_call_reason; неизвестная причина показывается как «недоступен». */
    fun reasonText(reason: Int): Int = when (reason) {
        2 -> R.string.radio_call_declined
        3 -> R.string.radio_call_busy
        4 -> R.string.radio_call_cancelled
        6 -> R.string.radio_call_no_answer
        7 -> R.string.radio_call_audio_error
        8 -> R.string.phone_busy_warning
        9 -> R.string.radio_call_declined
        else -> R.string.radio_call_unavailable
    }

    private fun showOutcome(reason: Int) {
        if (reason == 4 || reason == 1) return
        try {
            val ctx = ChatApplication.instance
            val text = AppLanguage.text(reasonText(reason))
            if (visible) { Toast.makeText(ctx, text, Toast.LENGTH_LONG).show(); return }
            IncomingCallManager.init()
            val manager = requireNotNull(ctx.getSystemService(NotificationManager::class.java)) { "notification manager unavailable" }
            manager.notify(RESULT_NOTIFICATION_ID,
                NotificationCompat.Builder(ctx, IncomingCallManager.CHANNEL_CALL)
                    .setSmallIcon(android.R.drawable.ic_dialog_info).setContentTitle(AppLanguage.text(R.string.radio_call))
                    .setContentText(text).setAutoCancel(true).build())
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "radio", "Call result presentation failed reason=$reason visible=$visible: $e")
        }
    }

    /** Отказ/исключение JNI — отказ постановки команды, а true ещё не означает успех сигналинга. */
    /** На main зарегистрировать отмену только ещё актуальной активации; успех приглашения снимает её без вызова. */
    fun ownActivation(id: Long, cleanup: () -> Unit): Boolean {
        checkMain()
        if (_activation.value?.id != id || _state.value?.phase != RadioCallPhase.PREPARING) return false
        activationCleanup = cleanup
        return true
    }

    private fun command(id: Long, action: Int, group: String? = null, peer: String? = null, devices: Boolean = false): Boolean {
        try {
            if (NativeLib.radioCall(id, action, group, peer, devices)) {
                LogManager.addLog("DEBUG", "radio", "Call command queued id=$id action=$action group=$group peer=$peer")
                return true
            }
            LogManager.addLog("ERROR", "radio", "Call command rejected id=$id action=$action group=$group peer=$peer")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "radio", "Call command failed id=$id action=$action group=$group peer=$peer: $e")
        }
        return false
    }

    /** CoreActivity сохраняет намерение до подтверждения очереди; его отказ не прерывает сигналинг/очистку UI. */
    private fun syncActivity() {
        val active = busy()
        try { CoreActivity.radioCall(active) } catch (e: Exception) {
            LogManager.addLog("ERROR", "radio", "Call activity update failed active=$active id=${_state.value?.id} " +
                "pending=${pendingStart?.id}; retained for next sync: $e")
        }
    }

    private fun checkMain() = check(Looper.myLooper() == Looper.getMainLooper()) {
        val message = "RadioCallController requires main thread: caller=${Thread.currentThread().name}"
        LogManager.addLog("ERROR", "radio", message)
        message
    }
}
