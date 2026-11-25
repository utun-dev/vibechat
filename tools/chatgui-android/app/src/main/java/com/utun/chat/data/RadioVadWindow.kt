package com.utun.chat.data

/** Фоновое окно обоих детекторов автопередачи (VAD и уровня), длительность задаёт durationMs.
 * В foreground открытое окно не ограничено временем; возвращение в приложение восстанавливает полный бюджет,
 * следующий уход в фон даёт новое окно целиком. Повторная установка того же foreground/enabled его не продлевает.
 * Вызывающий передаёт nowMs в монотонной шкале SystemClock.elapsedRealtime, отдельно учитывает экран и готовность аудио.
 * Изменения и timeLeftMs — только в radio-audio-io; capture-поток может читать volatile deadlineMs и isAllowed. */
class RadioVadWindow(private val durationMs: Long) {
    /** 0 — закрыто, Long.MAX_VALUE — открыто без ограничения foreground, иначе исключающая граница в elapsedRealtime. */
    @Volatile var deadlineMs = 0L
        private set
    var foreground = false
        private set
    private var enabled = false
    private var remainingMs = durationMs

    /** Смена видимости приложения: вход в foreground восстанавливает бюджет, выход запускает его отсчёт. */
    fun setForeground(value: Boolean, nowMs: Long) {
        if (foreground == value) return
        if (value) remainingMs = durationMs
        foreground = value
        updateDeadline(nowMs)
    }

    /** Только переход false→true открывает новое окно; true→false закрывает его. */
    fun setEnabled(value: Boolean, nowMs: Long) {
        if (enabled == value) return
        enabled = value
        if (value) restart(nowMs) else close()
    }

    /** Восстановить полный бюджет (например, экран включился); выключенную настройку enabled не включает. */
    fun restart(nowMs: Long) {
        remainingMs = durationMs
        updateDeadline(nowMs)
    }

    /** Немедленно закрыть окно (например, экран погас), сохранив enabled и foreground.
     * Повторный setEnabled(true) не откроет его; нужны restart либо новый переход в foreground. */
    fun close() {
        remainingMs = 0L
        deadlineMs = 0L
    }

    /** На самой границе deadline окно уже истекло; разрешение экрана/PTT проверяется отдельно. */
    fun isAllowed(nowMs: Long): Boolean = nowMs < deadlineMs

    /** Остаток в фоне или полный бюджет открытого foreground; для закрытого/истёкшего окна — 0. Только io-поток. */
    fun timeLeftMs(nowMs: Long): Long =
        if (deadlineMs == Long.MAX_VALUE) remainingMs else (deadlineMs - nowMs).coerceAtLeast(0L)

    private fun updateDeadline(nowMs: Long) {
        deadlineMs = when {
            !enabled -> 0L
            foreground -> Long.MAX_VALUE
            else -> nowMs + remainingMs
        }
    }
}
