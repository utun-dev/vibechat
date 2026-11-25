package com.utun.chat.data

/** Один подтверждённый снимок NS для звонка, рации и записи. Ноль в native означает полный обход. */
data class NoiseSuppressionSettings(val enabled: Boolean = true, val suppressDb: Int = DEFAULT_DB) {
    init { require(suppressDb in MIN_DB..MAX_DB) { "Noise suppression outside $MIN_DB..$MAX_DB: $suppressDb" } }
    val nativeLevel: Int get() = if (enabled) suppressDb else 0

    companion object {
        const val DEFAULT_DB = -20
        const val MIN_DB = -40
        const val MAX_DB = -1
    }
}
