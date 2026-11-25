package com.utun.chat.data

/** Worker-локальный отбор raw по фазе fps=1..60; PTS — монотонные мкс одного запуска камеры.
 * accept=false при неверном/повторном PTS, риске overflow либо раннем кадре (допуск 1/4 периода).
 * Опоздание относительно очередной фазы больше периода переносит её без burst; новая шкала требует нового экземпляра. */
internal class CallVideoCadence(private val fps: Int) {
    private var lastPts = -1L
    private var next = -1L
    fun accept(pts: Long): Boolean {
        if (fps !in 1..60 || pts < 0 || pts <= lastPts || pts > (Long.MAX_VALUE - 1_250_000) / fps) return false
        lastPts = pts
        val scaled = pts * fps
        if (next >= 0 && scaled < next - 250_000) return false
        next = if (next < 0 || scaled - next > 1_000_000) scaled + 1_000_000 else next + 1_000_000
        return true
    }
}
