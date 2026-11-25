package com.utun.chat.data

/** Источник ручной передачи: кнопка чата, плавающая кнопка либо общий владелец volume-клавиш. */
internal enum class RadioPttSource { BUTTON, OVERLAY, VOLUME }

/**
 * Независимые удержания PTT без управления звуком и таймерами; все вызовы сериализует main-поток владельца.
 * Каждый источник занимает одно место: повтор press не увеличивает счётчик. Несколько физических клавиш
 * одного источника вызывающий агрегирует сам и делает release только после отпускания последней.
 * RadioAudioEngine использует release для хвоста передачи, cancel — для немедленной отмены без хвоста.
 */
internal class RadioPttHolds {
    private val held = mutableSetOf<RadioPttSource>() // Только ручные владельцы; VAD/автоматика сюда не входят.

    /** Добавить source идемпотентно; остальные владельцы сохраняются. */
    fun press(source: RadioPttSource) { held.add(source) }

    /** Удалить source; true только при удалении последнего владельца. Неизвестный/повторный release даёт false. */
    fun release(source: RadioPttSource): Boolean = held.remove(source) && held.isEmpty()

    /** Удалить source либо всех при null; вернуть отсутствие владельцев, даже если набор уже был пуст.
     * Поэтому cancel разрешает снять отложенный хвост передачи также после предыдущего release. */
    fun cancel(source: RadioPttSource? = null): Boolean {
        if (source == null) held.clear() else held.remove(source)
        return held.isEmpty()
    }
}
