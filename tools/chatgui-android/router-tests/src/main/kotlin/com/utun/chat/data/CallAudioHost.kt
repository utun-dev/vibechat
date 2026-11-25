package com.utun.chat.data

/** Граница Telecom для настоящего CallAudioEngine; DSP заменён только в host NativeLib. */
object TelecomCallManager {
    var connection: UtunConnection? = null
    fun connectionFor(id: Long) = connection
}
