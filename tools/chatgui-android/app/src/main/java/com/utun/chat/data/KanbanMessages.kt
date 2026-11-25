package com.utun.chat.data

import org.json.JSONObject
import java.io.File
import java.util.Base64

/** Адаптер подписанной строки карточки к общим Android bubbles. id=0: у Kanban нет chat row ID; используется messageId. */
data class KanbanMessage(val message: Message, val objectId: String = "")

/** Только отображаемые метаданные manifest; подписи, доступ и соответствие UUID сообщению проверяет общий attachment backend.
 * Ошибки формата/пути бросают исключение; caller логирует их с ID группы и сообщения. Вызывать в IO из-за проверки кеша. */
fun parseKanbanMessage(row: JSONObject, base: File, self: String, deletedText: String): KanbanMessage {
    val id = row.getString("id")
    val author = row.getString("author")
    val created = row.getLong("created_at")
    val updated = row.optLong("updated_at", created)
    val deleted = row.optBoolean("deleted")
    val plain = Message(id = 0, messageId = id, author = author,
        text = if (deleted) deletedText else row.optString("text"), ts = created / 1000, isOutgoing = author == self,
        editedAt = if (!deleted && updated != created) updated / 1000 else 0)
    val type = row.optString("ct")
    if (deleted || type.isEmpty() || type == "text/plain") return KanbanMessage(plain)
    val parts = row.getString("d").split('|')
    require(parts.size >= 7) { "attachment manifest missing fields" }
    val size = parts[1].toLong(); val block = parts[2].toLong(); val count = parts[3].toInt()
    require(size > 0 && block > 0 && count in 1..100 && (size - 1) / block + 1 == count.toLong()) { "invalid block geometry" }
    val objectId = parts[4].lowercase()
    require(objectId.matches(Regex("[0-9a-f]{32}"))) { "invalid attachment UUID" }
    val description = if (parts[0].startsWith('*')) Charsets.UTF_8.newDecoder()
        .decode(java.nio.ByteBuffer.wrap(Base64.getUrlDecoder().decode(parts[0].substring(1)))).toString() else parts[0]
    require(description.isNotEmpty()) { "empty attachment description" }
    val extra = parts.drop(7).associate { it.substringBefore('=') to it.substringAfter('=') }
    val width = extra["w"]?.toInt() ?: 0; val height = extra["h"]?.toInt() ?: 0
    val duration = extra["dur"]?.toInt() ?: 0
    require(width >= 0 && height >= 0 && duration >= 0) { "negative media dimensions/duration" }
    var waveform = emptyList<Float>(); var voiceDuration = 0
    if (type == "audio/opus" && description.startsWith("wf=")) {
        val end = description.indexOf(";dur=")
        require(end >= 3) { "missing voice duration" }
        val alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
        waveform = description.substring(3, end).map {
            val level = alphabet.indexOf(it); require(level >= 0) { "invalid waveform" }; level / 63f
        }
        voiceDuration = description.substring(end + 5).substringBefore(';').toInt()
        require(voiceDuration >= 0) { "negative voice duration" }
    }
    val location = row.optString("file_path")
    var path = ""
    if (location.isNotEmpty()) {
        require(!File(location).isAbsolute) { "absolute media index location" }
        val root = base.canonicalFile; val file = File(root, location).canonicalFile
        require(file != root && file.toPath().startsWith(root.toPath())) { "media index path escapes profile" }
        if (file.isFile && file.length() == size) path = file.path
    }
    return KanbanMessage(plain.copy(contentType = type, text = description, caption = plain.text, fileSize = size,
        filePath = path, downloadState = if (path.isNotEmpty()) "downloaded" else "",
        waveform = waveform, voiceDurationMs = voiceDuration, imageWidth = width, imageHeight = height,
        videoWidth = width, videoHeight = height, videoDurationMs = duration), objectId)
}
