package com.utun.chat.data

import com.utun.chat.R

/** Состояние единственной звонковой сессии; не зависит от Activity или ViewModel. */
enum class CallPhase { NONE, OUTGOING, RINGING, INCOMING, ACTIVE, ENDED, WAIT_DECISION, PENDING_INCOMING }

/** Значения CONN_TYPE_* общего ядра. */
enum class CallConnectionType(val labelRes: Int) {
    NONE(R.string.connection_none), DIRECT(R.string.connection_direct),
    REVERSE(R.string.connection_reverse), INDIRECT(R.string.connection_indirect);

    val label: String get() = AppLanguage.text(labelRes)
}

/** Снимок аудиозвонка: задержки/границы в мс, tempoX100=100 означает 1x; dropped/underruns — счётчики кадров. */
data class CallStats(
    val rttMs: Int, val bufferMs: Int, val tempoX100: Int, val dropped: Int,
    val underruns: Int, val minMs: Int, val maxMs: Int, val reserveMs: Int,
    val output: CallOutputLatency = CallOutputLatency()
)

/** Миллисекунды задержки конца записанного PCM: outputMs включает queueMs, поля -1 означают неизвестное значение. */
data class CallOutputLatency(val queueMs: Int = -1, val outputMs: Int = -1)

/** Immutable UI snapshot одной сессии: IDs Long сохраняют uint64, reason=-1 до результата.
 * startedAtElapsedRealtime — локальные монотонные мс для chronometer; durationSeconds обновляется controller.
 * ongoing исключает только NONE/ENDED; preparing означает освобождение media после выбора устройства. */
data class CallState(
    val callId: Long = 0,
    val channelId: String = "",
    val peerAddress: String? = null, // Ed25519. До выбора устройства в DM может отсутствовать.
    val peerName: String = "",
    val phase: CallPhase = CallPhase.NONE,
    val reason: Int = -1,
    val durationSeconds: Int = 0,
    val bgpPath: String = "",
    val stats: CallStats? = null,
    val startedAtElapsedRealtime: Long = 0,
    val connectionType: CallConnectionType = CallConnectionType.NONE,
    val connectionNextHop: Long = 0,
    val devices: Boolean = false, // Вызов devgroup указанного peer.
    val preparing: Boolean = false, // Освобождаем рацию/трансляцию после ACCEPTED, до запуска звонкового аудио.
    val answering: Boolean = false // Согласие отправляется/отправлено; ждём выбора устройства, медиа ещё работают.
) {
    val ongoing: Boolean get() = phase != CallPhase.NONE && phase != CallPhase.ENDED
}
