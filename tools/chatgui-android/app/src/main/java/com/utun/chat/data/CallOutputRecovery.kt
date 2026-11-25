package com.utun.chat.data

/** Счётчик восстановления выхода, только playback worker.
 * route вызывается при подтверждении device ID; повтор того же ID сохраняет число ошибок.
 * Новый ID разрешает AAudio заново, после первой ошибки используются только fallback-открытия.
 * Политика сама не открывает устройство и не логирует: это делает CallAudioEngine. */
internal class CallOutputRecovery {
    var deviceId = Int.MIN_VALUE
        private set
    var failures = 0
        private set

    /** Обновить ID и сбросить ошибки лишь при его изменении; true — маршрут изменился. */
    fun route(id: Int): Boolean {
        if (id == deviceId) return false
        deviceId = id; failures = 0
        return true
    }

    /** Учесть ошибку текущего маршрута: true — разрешить повтор (первые две), false — лимит исчерпан. */
    fun failed(): Boolean { failures++; return failures <= 2 }
    /** AAudio разрешён только до первой учтённой ошибки текущего маршрута. */
    val tryFast: Boolean get() = failures == 0
}

/** Перевод локальных позиций выхода в общую шкалу принятых PCM-сэмплов 48 кГц.
 * Playback worker меняет состояние; reporter читает timestamp, а reopen/reset согласованы с ним через trackLock.
 * При reopen старый аппаратный остаток считается пройденным: base=written, новая локальная позиция=0.
 * Незаписанный остаток текущего блока остаётся у worker, native JB и общий written не сбрасываются. */
internal class CallOutputPosition {
    var base = 0L
        private set
    private var head = CallPlaybackHead()

    /** Новый запуск: обнулить base и проверку head. */
    fun reset() { base = 0; head = CallPlaybackHead() }

    /** Замена выхода: written — общий счётчик принятых сэмплов, не меньше прежнего base. */
    fun reopen(written: Long) {
        require(written >= base)
        base = written
        head = CallPlaybackHead()
    }

    /** raw — unsigned 32-bit head нового выхода; null при регрессии/выходе за его принятый PCM. */
    fun played(raw: Int, written: Long): Long? = head.update(raw, written - base)?.let { base + it }
    /** Развернуть локальный timestamp относительно written и добавить base; -1 при отрицательной позиции.
     * Это перевод шкалы: возраст/поколение/допустимость аппаратного якоря проверяет CallPlaybackClock. */
    fun timestamp(local: Long, written: Long): Long {
        if (local < 0) return -1
        var extended = local
        if (local <= 0xffffffffL) {
            extended = ((written - base) and -0x100000000L) or local
            if (extended > written - base) extended -= 0x100000000L
        }
        return if (extended < 0) -1 else base + extended
    }
}
