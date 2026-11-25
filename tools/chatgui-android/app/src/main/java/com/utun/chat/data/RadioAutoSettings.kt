package com.utun.chat.data

/** Сохранённые параметры автопередачи; enabled отделён от выбранного детектора (VAD/LEVEL).
 * OFF используется только для захвата без автоматики и не является сохраняемым mode.
 * valid проверяет весь снимок, включая параметры невыбранного детектора; nativeValues сам проверку не выполняет.
 * Диапазоны совпадают с реестром chat_setting.c, номера режимов — с radio_audio.h.
 * @param levelOn порог включения по уровню, -90..0 dBFS.
 * @param levelOff порог выключения по уровню, -90..0 dBFS, строго ниже levelOn.
 * @param hangoverMs тишина до автоматического отбоя, 50..2000 мс.
 * @param vadThreshold порог вероятности речи Silero, 10..95 процентов.
 * @param vadGain предел усиления отдельного компрессора копии PCM для VAD, 1..60 dB; не усиление передаваемого PCM. */
data class RadioAutoSettings(
    val enabled: Boolean = false,
    val mode: Int = VAD,
    val levelOn: Int = -35,
    val levelOff: Int = -45,
    val hangoverMs: Int = 200,
    val vadThreshold: Int = 50,
    val vadGain: Int = 10
) {
    companion object { const val OFF = 0; const val VAD = 1; const val LEVEL = 2; const val VAD_LEVEL = 3 }
    val usesVad: Boolean get() = mode == VAD || mode == VAD_LEVEL
    fun valid(): Boolean = mode in VAD..VAD_LEVEL && levelOn in -90..0 && levelOff in -90..0 && levelOff < levelOn &&
        hangoverMs in 50..2000 && vadThreshold in 10..95 && vadGain in 1..60

    /** Новая копия [enabled, mode, levelOn, levelOff, hangoverMs, vadThreshold, vadGain] для nativeRadioConfigure.
     * Native проверяет и применяет снимок целиком в uasync, результат приходит отдельно по request ID. */
    fun nativeValues(): IntArray = intArrayOf(if (enabled) 1 else 0, mode, levelOn, levelOff, hangoverMs, vadThreshold, vadGain)
}

/** null — захват не нужен, OFF — PTT/измеритель/ожидание, VAD/LEVEL — с автоматикой.
 * Выключенный экран и неактивная рация исключают любой захват. PTT, измеритель и hold не включают автоматику. */
internal fun radioCaptureMode(active: Boolean, screenOn: Boolean, autoAllowed: Boolean,
                              settings: RadioAutoSettings, manual: Boolean, monitor: Boolean, hold: Boolean = false): Int? {
    if (!active || !screenOn) return null
    val automatic = autoAllowed && settings.enabled
    if (!manual && !automatic && !monitor && !hold) return null
    return if (automatic) settings.mode else RadioAutoSettings.OFF
}
