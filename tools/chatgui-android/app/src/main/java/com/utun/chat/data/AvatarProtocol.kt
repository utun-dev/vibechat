package com.utun.chat.data

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Декодирование локальных JNI-событий аватаров: 49 — файловый ответ, 50 — прогресс передачи,
 * 51/52 — результат публикации ссылки профиля/аватара группы, 68 — снимок аватара группы.
 * Числа LE; UID и request ID хранят исходные 64 бита в Long, включая старший бит.
 * Файловые события разбирает общий MemberFilesProtocol; неверный формат бросает исключение.
 * Здесь проверяется framing: SHA256 байтов и геометрию изображения проверяют AvatarAssets/AvatarRepository.
 */
object AvatarProtocol {
    /** CHAT_EVT_GROUP_AVATAR_SNAPSHOT из chat_event.h; отдельный номер от FEEDBACK_CHANGED=53. */
    const val GROUP_SNAPSHOT_EVENT = 68
    /** Лимит пользовательского WebP-превью, не оригинала. */
    const val MAX_USER = 1024 * 1024
    const val MAX_GROUP = 16 * 1024
    const val MAX_ORIGINAL = 32 * 1024 * 1024
    const val MAX_MANIFEST = 4096
    private val filePattern = Regex("[0-9a-f]{64}\\.json")
    private val objectPattern = Regex("[0-9a-f]{64}\\.(png|jpg|webp|json)")
    /** Имя пользовательского манифеста: 64 lowercase hex + .json, без пути. */
    fun validFile(file: String): Boolean = filePattern.matches(file)
    /** Имя объекта: тот же хеш с .png/.jpg/.webp/.json; содержимое здесь не проверяется. */
    fun validObject(file: String): Boolean = objectPattern.matches(file)
    fun hex(id: Long): String = id.toULong().toString(16).padStart(16, '0')
    fun hash(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun readGroup(b: ByteBuffer): String = ByteArray(32).also { b.get(it) }.toHexString().also {
        require(MemberFilesProtocol.validAddress(it)) { "invalid avatar group" }
    }
    private fun buffer(data: ByteArray) = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)

    /** Снимок GROUP_SNAPSHOT_EVENT: canEdit — наличие локального приватного ключа; failed — ошибка чтения core.
     * Пустое image допустимо при отсутствии/удалении аватара; окончательный допуск публикации проверяет core. */
    data class Group(val id: Long, val group: String, val version: Long, val hash: String,
                     val canEdit: Boolean, val failed: Boolean, val image: ByteArray)
    /** Заголовок 84 байта: id:u64, group:Ed25519, version:u64, hash:32, canEdit/failed:u8, size:u16, затем image до 16 КиБ. */
    fun group(data: ByteArray): Group {
        require(data.size in 84..84 + MAX_GROUP) { "group snapshot length=${data.size}" }
        val b = buffer(data)
        val id = b.long; val group = readGroup(b); val version = b.long
        val hash = ByteArray(32); b.get(hash)
        val canEdit = b.get().toInt(); val failed = b.get().toInt()
        val size = b.short.toInt() and 65535
        require(canEdit in 0..1 && failed in 0..1 && size == b.remaining()) { "invalid group snapshot header" }
        return Group(id, group, version, hash(hash), canEdit != 0, failed != 0, ByteArray(size).also { b.get(it) })
    }

    /** Общий результат: поля, отсутствующие в данном событии, имеют значения по умолчанию.
     * status=0 — успех; 49/50 используют MF-статус, 51/52 — failed (0/1). */
    data class Reply(val id: Long, val group: String, val server: String = "", val owner: String = "",
                     val command: Int = 0, val status: Int = 0, val size: Long = 0, val hash: String = "", val fileType: Int = 0,
                     val cursor: Long = 0, val version: ByteArray = byteArrayOf(), val more: Boolean = false,
                     val path: String = "", val payload: ByteArray = byteArrayOf())
    /** Событие 49: group и server:Ed25519 перед файловым ответом протокола v5; hash переводится в hex. */
    fun response(data: ByteArray): Reply {
        val r = MemberFilesProtocol.response(data)
        return Reply(r.id, r.group, r.server, r.owner, r.command, r.status, r.size, hash(r.hash), r.type,
            r.cursor, r.version, r.more, r.path, r.payload)
    }
    /** Имя JSON-манифеста и mtime файлового сервера (секунды Unix). */
    data class Entry(val file: String, val mtime: Long)
    /** Из LIST оставить только обычные файлы манифестов размером 1..4096 байт, сохраняя порядок страницы. */
    fun entries(reply: Reply): List<Entry> {
        return MemberFilesProtocol.entries(reply.payload).filter { it.type == 1 && validFile(it.name) && it.size in 1..MAX_MANIFEST }
            .map { Entry(it.name, it.mtime) }
    }
    /** Событие 50, ровно 123 байта; null для промежуточного прогресса, Reply только при finished, включая ошибки. */
    fun transfer(data: ByteArray): Reply? {
        val r = MemberFilesProtocol.progress(data)
        return if (r.finished) Reply(r.id, r.group, r.server, r.owner, status = r.status, size = r.total) else null
    }
    /** Событие 51/52, ровно 41 байт: id:u64, group:Ed25519 и failed:u8 (0/1). */
    fun published(data: ByteArray): Reply {
        require(data.size == 41 && data[40].toInt() in 0..1) { "invalid publication result" }
        val b = buffer(data)
        return Reply(b.long, readGroup(b), status = b.get().toInt())
    }
}
