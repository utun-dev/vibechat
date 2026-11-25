package com.utun.chat.data

/** Границы 300..4000 десятичных кбит/с, minimum <= maximum; одна настройка "minimum:maximum".
 * Конструктор отвергает неверные границы, parse возвращает null при неверных цифрах/формате/диапазоне. */
data class CallVideoBitrateRange(val minimum: Int = 300, val maximum: Int = 4000) {
    init { require(minimum in 300..4000 && maximum in minimum..4000) }

    fun encode(): String = "$minimum:$maximum"

    companion object {
        fun parse(value: String): CallVideoBitrateRange? {
            val parts = value.split(':')
            if (parts.size != 2 || parts.any { it.isEmpty() || it.any { digit -> digit !in '0'..'9' } }) return null
            val minimum = parts[0].toIntOrNull() ?: return null
            val maximum = parts[1].toIntOrNull() ?: return null
            if (minimum !in 300..4000 || maximum !in minimum..4000) return null
            return CallVideoBitrateRange(minimum, maximum)
        }
    }
}
