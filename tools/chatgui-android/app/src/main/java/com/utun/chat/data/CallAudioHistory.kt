package com.utun.chat.data

/** Неизменяемая копия native-истории. Индексы и единицы совпадают с nativeCallAudioHistory.
 * RX: время, глубина, цель, темп, min, max, PCM, недостающий PCM, min/max доступного запаса (0 при паузе).
 * TX: время, ожидающие байты, inflight, транспорт отдельно, байт/с, peer, epoch, счётчик TX. */
class CallAudioHistory private constructor(private val data: LongArray) {
    val nowMs = data[1]
    val rxCount = data[2].toInt()
    val txCount = data[3].toInt()
    val observedUs = data[4]
    val missingUs = data[5]
    fun histogram(index: Int): Float = data[6 + index] / 10000f // Проценты.
    fun rx(index: Int, field: Int): Long = data[67 + index * 10 + field]
    fun tx(index: Int, field: Int): Long = data[67 + rxCount * 10 + index * 8 + field]
    companion object {
        fun read(callId: Long): CallAudioHistory? {
            val raw = NativeLib.callAudioHistory(callId) ?: return null
            if (raw.size < 67 || raw[0] != 2L || raw[2] !in 0L..601L || raw[3] !in 0L..601L ||
                raw.size.toLong() != 67L + raw[2] * 10 + raw[3] * 8) {
                LogManager.addLog("ERROR", "call", "Invalid audio history snapshot size=${raw.size}"); return null
            }
            return CallAudioHistory(raw)
        }
    }
}
