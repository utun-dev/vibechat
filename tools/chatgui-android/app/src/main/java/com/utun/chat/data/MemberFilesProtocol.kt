package com.utun.chat.data

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction
import java.util.concurrent.atomic.AtomicLong

/**
 * Kotlin-кодек member_files v5, общий для файлового браузера и аватаров.
 * Числа LE; адреса Ed25519 хранятся как hex, размеры/счётчики в проверяемых payload должны быть неотрицательными.
 * request кодирует только LIST/STAT/MKDIR/RENAME/REMOVE/QUOTA, без group/server-префикса JNI-события 49.
 * response принимает событие 49 с этим префиксом, progress — локальное событие 50, не сетевой STREAM_DATA.
 * Неверные данные бросают исключение; caller логирует отказ с контекстом запроса. Проверки ACL и смысла команд остаются в core.
 */
object MemberFilesProtocol {
    const val LIST = 1
    const val STAT = 2
    const val MKDIR = 8
    const val RENAME = 9
    const val REMOVE = 10
    const val QUOTA = 16
    const val FILE = 1
    const val DIRECTORY = 2
    const val STALE = 7
    const val BUSY = 6
    const val NOT_FOUND = 2
    const val CANCELLED = 12
    private val ids = AtomicLong(System.nanoTime())
    /** Общий потокобезопасный счётчик запросов браузера/аватаров; нулевой ID пропускается. */
    fun nextId(): Long { var id = ids.incrementAndGet(); if (id == 0L) id = ids.incrementAndGet(); return id }
    private val addressPattern = Regex("[0-9a-f]{64}")
    fun validAddress(hex: String): Boolean = addressPattern.matches(hex) && hex.any { it != '0' }
    fun addressBytes(hex: String): ByteArray {
        require(validAddress(hex)) { "invalid file peer address" }
        return ByteArray(32) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    }
    fun addressHex(bytes: ByteArray): String = if (bytes.all { it == 0.toByte() }) "" else bytes.joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun readAddress(b: ByteBuffer): String = addressHex(ByteArray(32).also { b.get(it) })
    private fun buffer(data: ByteArray) = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    private fun utf8(data: ByteArray): String = Charsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(data)).toString()

    /** Относительный UTF-8 путь до 1024 байт/60 компонентов по 1..255 байт; root разрешает пустой путь.
     * Отклоняет leading '.', trailing '.'/' ', C0/DEL, Windows device names и запрещённые символы; путь не нормализуется. */
    fun validPath(path: String, root: Boolean = true): Boolean {
        if (path.isEmpty()) return root
        if (path.toByteArray().size > 1024 || path.any { it.isSurrogate() } &&
            runCatching { utf8(path.toByteArray()) != path }.getOrDefault(true)) return false
        val components = path.split('/')
        return components.size <= 60 && components.all { name ->
            name.isNotEmpty() && name.toByteArray().size <= 255 && !name.startsWith('.') &&
                !name.endsWith('.') && !name.endsWith(' ') && name.none { it.code < 32 || it.code == 127 || it in "\\:*?\"<>|" } &&
                !Regex("(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])").matches(name.substringBefore('.'))
        }
    }
    /** Первый компонент avatars без учёта регистра; это признак служебного дерева, не проверка валидности пути. */
    fun servicePath(path: String) = path.substringBefore('/').equals("avatars", true)
    /** Добавить одно имя к path и проверить итог; имя со '/' или неверный итог бросают IllegalArgumentException. */
    fun child(path: String, name: String): String {
        require(!name.contains('/') && validPath(name, false)) { "invalid file name" }
        return (if (path.isEmpty()) name else "$path/$name").also { require(validPath(it, false)) { "invalid path" } }
    }

    /** Ответ 49: status=0 — успех, 1..14 — MF-ошибка; mtime в Unix seconds, size в байтах.
     * version — непрозрачные 16 байт снимка, hash — 32 байта; STAT не вычисляет хеш содержимого.
     * cursor/more/payload зависят от команды; контекст id/group/server/owner/command/path сверяет репозиторий. */
    data class Reply(val id: Long, val group: String, val server: String, val owner: String, val command: Int, val status: Int,
                     val size: Long, val mtime: Long, val hash: ByteArray, val type: Int, val cursor: Long,
                     val version: ByteArray, val more: Boolean, val path: String, val payload: ByteArray)
    /** Одна запись LIST: имя без '/', FILE/DIRECTORY, байты/Unix seconds и 16-байтная версия для операций. */
    data class Entry(val name: String, val type: Int, val size: Long, val mtime: Long, val version: ByteArray)
    /** Событие 50: done/total — байты; окончательный status действителен только при finished. */
    data class Progress(val id: Long, val group: String, val server: String, val owner: String, val done: Long,
                        val total: Long, val status: Int, val finished: Boolean)
    /** Общий учёт UID по группам сервера: первые три поля в байтах, последние два — числа опубликованных объектов. */
    data class Quota(val used: Long, val limit: Long, val reserved: Long, val files: Long, val directories: Long)

    /** Заголовок 149 байт и UTF-8 path/target. LIST начинает с cursor=0/version=0, продолжает снимком ответа.
     * RENAME/REMOVE используют версию entry, target допустим только RENAME; прочие wire-поля остаются нулевыми.
     * id ненулевой; допустимость пустого owner и полномочия вызывающего дополнительно проверяются JNI/core. */
    fun request(id: Long, owner: String, command: Int, path: String, target: String = "", cursor: Long = 0,
                version: ByteArray = ByteArray(16)): ByteArray {
        require(id != 0L && command in listOf(LIST, STAT, MKDIR, RENAME, REMOVE, QUOTA))
        require(validPath(path, command in listOf(LIST, STAT, QUOTA)) && version.size == 16 && cursor >= 0)
        require(if (command == RENAME) validPath(target, false) else target.isEmpty())
        val a = path.toByteArray(); val b = target.toByteArray()
        return ByteBuffer.allocate(149 + a.size + b.size).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(5); put(1); put(command.toByte()); position(5); putLong(id); put(if (owner.isEmpty()) ByteArray(32) else addressBytes(owner))
            position(61); putLong(cursor); put(version); position(143)
            putShort(a.size.toShort()); putShort(b.size.toShort()); putShort(0); put(a); put(b)
        }.array()
    }

    /** Разобрать group:Ed25519 и server:Ed25519 и полный v5 RESPONSE с точными длинами и строгим UTF-8 путей.
     * target проверяется, но не возвращается; семантика size/type/version и корректность страниц проверяются caller. */
    fun response(data: ByteArray): Reply {
        require(data.size >= 213) { "file response length=${data.size}" }
        val b = buffer(data); val group = readAddress(b); val server = readAddress(b)
        require(b.get().toInt() == 5 && b.get().toInt() == 2) { "invalid file response protocol/kind" }
        val command = b.get().toInt() and 255; val status = b.short.toInt() and 65535
        val id = b.long; val owner = readAddress(b); b.long; val size = b.long; val cursor = b.long
        val version = ByteArray(16).also { b.get(it) }; b.position(165)
        val hash = ByteArray(32).also { b.get(it) }; val mtime = b.long
        val type = b.get().toInt() and 255; val more = b.get().toInt() and 255
        val a = b.short.toInt() and 65535; val target = b.short.toInt() and 65535; val length = b.short.toInt() and 65535
        require(id != 0L && command in 1..16 && status in 0..14 && more in 0..1 && a <= 1024 && target <= 1024 &&
            length <= 8192 && b.remaining() == a + target + length) { "invalid file response header" }
        val pathBytes = ByteArray(a).also { b.get(it) }; val targetBytes = ByteArray(target).also { b.get(it) }
        require(0.toByte() !in pathBytes && 0.toByte() !in targetBytes) { "NUL in file path" }
        val path = utf8(pathBytes); utf8(targetBytes)
        return Reply(id, group, server, owner, command, status, size, mtime, hash, type, cursor, version, more != 0,
            path, ByteArray(length).also { b.get(it) })
    }

    /** Полный payload страницы LIST: записи с заголовком 35 байт и UTF-8 name; пустая страница допустима.
     * Дубликат имени внутри страницы/усечённая запись/неверное имя отклоняют всю страницу. */
    fun entries(data: ByteArray): List<Entry> {
        val b = buffer(data); val entries = mutableListOf<Entry>(); val names = mutableSetOf<String>()
        while (b.hasRemaining()) {
            require(b.remaining() >= 35) { "truncated directory entry" }
            val length = b.short.toInt() and 65535; val type = b.get().toInt() and 255
            val size = b.long; val mtime = b.long; val version = ByteArray(16).also { b.get(it) }
            require(length in 1..255 && length <= b.remaining() && type in FILE..DIRECTORY && size >= 0 && mtime >= 0)
            val name = utf8(ByteArray(length).also { b.get(it) })
            require(!name.contains('/') && validPath(name, false) && names.add(name)) { "invalid/duplicate directory name" }
            entries += Entry(name, type, size, mtime, version)
        }
        return entries
    }

    /** Ровно 123 байта: id:u64, group/server/owner:Ed25519, done/total:u64, status:u16, state:u8 (1 progress, 2 finished). */
    fun progress(data: ByteArray): Progress {
        require(data.size == 123) { "transfer length=${data.size}" }
        val b = buffer(data); val id = b.long; val group = readAddress(b); val server = readAddress(b); val owner = readAddress(b)
        val done = b.long; val total = b.long; val status = b.short.toInt() and 65535; val state = b.get().toInt()
        require(validAddress(group) && validAddress(server) && validAddress(owner) && id != 0L && state in 1..2 && status in 0..14 && done >= 0 && total >= 0 && done <= total)
        return Progress(id, group, server, owner, done, total, status, state == 2)
    }
    /** QUOTA payload: ровно пять u64 LE, limit > 0; used может превышать limit после снижения квоты. */
    fun quota(data: ByteArray): Quota {
        require(data.size == 40) { "quota length=${data.size}" }
        val b = buffer(data); val q = Quota(b.long, b.long, b.long, b.long, b.long)
        require(q.used >= 0 && q.limit > 0 && q.reserved >= 0 && q.files >= 0 && q.directories >= 0)
        return q
    }
}
