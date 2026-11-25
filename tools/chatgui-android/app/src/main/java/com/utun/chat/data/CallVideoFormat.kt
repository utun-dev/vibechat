package com.utun.chat.data

/** Согласованный формат одного поколения видео; не загружает JNI.
 * array/from используют первые девять полей native meta; from допускает дополнительные поля кадра.
 * maximum — бит/с, codec/profile/level имеют значения общего протокола uAV2. */
internal data class CallVideoFormat(val generation: Long, val width: Int, val height: Int, val fps: Int = 30,
    val rotation: Int = 0, val codec: Int = 0, val level: Int = 0, val maximum: Int = 4_000_000, val profile: Int = 0) {
    fun array() = longArrayOf(generation, width.toLong(), height.toLong(), fps.toLong(), rotation.toLong(), codec.toLong(),
        level.toLong(), maximum.toLong(), profile.toLong())
    companion object {
        fun from(a: LongArray) = CallVideoFormat(a[0], a[1].toInt(), a[2].toInt(), a[3].toInt(), a[4].toInt(),
            a[5].toInt(), a[6].toInt(), a[7].toInt(), a[8].toInt())
    }
}
