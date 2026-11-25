package com.utun.chat.data

/**
 * Выбранный оригинал фиксируется до асинхронной отправки; PM не поддерживает этот reply-контекст.
 * @property channelId ID канала оригинала; отправитель проверяет совпадение с каналом назначения.
 * @property message Снимок оригинала с глобальным messageId; вложенные списки не копируются.
 * @property revision Счётчик выбора в ViewModel: завершение старой отправки не снимает повторно выбранный ответ.
 */
data class ChannelReply(val channelId: String, val message: Message, val revision: Long, val quote: String = "")

/** Непрерывный фрагмент с сохранением пробелов/LF; границы не разрывают UTF-16 surrogate-пару. Пустое выделение даёт null. */
fun quoteFragment(text: String, first: Int, last: Int): String? {
    var start = minOf(first, last)
    var end = maxOf(first, last)
    if (start < 0 || end > text.length || start == end) return null
    if (start > 0 && text[start].isLowSurrogate() && text[start - 1].isHighSurrogate()) start--
    if (end < text.length && text[end - 1].isHighSurrogate() && text[end].isLowSurrogate()) end++
    return text.substring(start, end).takeIf { it.isNotBlank() && '\u0000' !in it }
}

/**
 * Переводит 64 строчных hex-символа в новый массив 32 байтов для JNI; порядок байтов совпадает со строкой.
 * Пустая строка даёт пустой массив (JNI заполнит отсутствующую ссылку нулями); 64 нуля тоже допустимы при конвертации.
 * Для выбора реального оригинала/редактирования нужен [validMessageId]. Неверный непустой формат — IllegalArgumentException.
 */
fun messageIdBytes(id: String): ByteArray {
    if (id.isEmpty()) return byteArrayOf()
    require(id.length == 64 && id.all { it in '0'..'9' || it in 'a'..'f' }) { "Invalid message ID" }
    return ByteArray(32) { id.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
}

/** Только реальный глобальный ID: 32 байта в строчном hex; пустая строка и нулевой ID не обозначают сообщение. */
fun validMessageId(id: String): Boolean = id.length == 64 && id.all { it in '0'..'9' || it in 'a'..'f' } && id.any { it != '0' }
