package com.utun.chat.data

/**
 * Снимок адресата до записи, выбора файла и фоновой подготовки; смена экрана не меняет уже начатую отправку.
 * @property id Десятичный ID канала/PM из модели, без префикса; класс не разбирает и не проверяет его.
 */
sealed class ConversationTarget {
    abstract val id: String
    data class Channel(override val id: String) : ConversationTarget()
    /** Постоянный ID карточки фиксируется до выбора файла/камеры; транспортный адрес остаётся ID группы. */
    data class Card(override val id: String, val cardId: String) : ConversationTarget()
    /** generation берётся вместе с ID беседы; после удаления старое поколение не может публиковать в заново открытую PM. */
    data class Pm(override val id: String, val generation: Long) : ConversationTarget()
    /** Target для общего voice/video API. Поколение PM не входит в строку и передаётся native отдельно. */
    val nativeId: String get() = if (this is Pm) "dm:$id" else id
}
