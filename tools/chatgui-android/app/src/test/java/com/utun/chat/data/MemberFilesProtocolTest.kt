package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class MemberFilesProtocolTest {
    private val uid = "fedcba9876543210".repeat(4)
    private val owner = "a5".repeat(32)
    private val server = "17".repeat(32)
    private fun buffer(size: Int) = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
    private fun reply(path: String = "", status: Int = 0): ByteArray {
        val wire = MemberFilesProtocol.request(71, owner, MemberFilesProtocol.LIST, path)
        wire[1] = 2; wire[3] = status.toByte()
        return buffer(64 + wire.size).put(MemberFilesProtocol.addressBytes(uid)).put(MemberFilesProtocol.addressBytes(server)).put(wire).array()
    }
    private fun entry(name: String, type: Int = 1, size: Long = 400): ByteArray {
        val bytes = name.toByteArray()
        return buffer(35 + bytes.size).putShort(bytes.size.toShort()).put(type.toByte()).putLong(size).putLong(123)
            .put(ByteArray(16) { 7 }).put(bytes).array()
    }
    @Test fun unicodeNamesAndUnsignedIdentityRoundTrip() {
        val r = MemberFilesProtocol.response(reply("Документы/😀.pdf"))
        assertEquals(uid, r.group); assertEquals(owner, r.owner); assertEquals(server, r.server)
        assertEquals(71L, r.id); assertEquals("Документы/😀.pdf", r.path)
        val e = MemberFilesProtocol.entries(entry("😀.pdf")).single()
        assertEquals("😀.pdf", e.name); assertEquals(400L, e.size); assertEquals(123L, e.mtime)
        assertArrayEquals(ByteArray(16) { 7 }, e.version)
    }
    @Test fun portablePathsAndUtf8ByteLimits() {
        listOf("", "photos/夏.jpg", "Документы/😀.pdf", "a".repeat(255)).forEach { assertTrue(it, MemberFilesProtocol.validPath(it)) }
        listOf("../file", "a//b", "/a", "a/", ".hidden", "a\\b", "CON.txt", "com1", "a.", "a ", "a\u0000b",
            "a".repeat(256), "я".repeat(128), (1..61).joinToString("/") { "a" }, "bad\uD800")
            .forEach { assertFalse(it, MemberFilesProtocol.validPath(it)) }
        assertFalse(MemberFilesProtocol.validPath("", false))
        assertTrue(MemberFilesProtocol.servicePath("Avatars/file")); assertFalse(MemberFilesProtocol.servicePath("photos/avatars"))
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.child("docs", "a/b") }
    }
    @Test fun paginationAndMutationKeepTheSnapshotToken() {
        val token = ByteArray(16) { it.toByte() }
        val wire = MemberFilesProtocol.request(71, owner, MemberFilesProtocol.RENAME, "docs/a", "docs/b", version = token)
        assertArrayEquals(token, wire.copyOfRange(69, 85))
        wire[1] = 2
        val r = MemberFilesProtocol.response(buffer(64 + wire.size).put(MemberFilesProtocol.addressBytes(uid)).put(MemberFilesProtocol.addressBytes(server)).put(wire).array())
        assertEquals("docs/a", r.path); assertArrayEquals(token, r.version)
        val page = MemberFilesProtocol.request(72, owner, MemberFilesProtocol.LIST, "docs", cursor = 82, version = token)
        assertEquals(82L, ByteBuffer.wrap(page).order(ByteOrder.LITTLE_ENDIAN).getLong(61))
        assertThrows(IllegalArgumentException::class.java) {
            MemberFilesProtocol.request(71, owner, MemberFilesProtocol.REMOVE, "")
        }
    }
    @Test fun malformedDirectoryCannotBecomeASuccessfulPartialList() {
        val e = entry("file")
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.entries(e + e) }
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.entries(e.copyOf(e.size - 1)) }
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.entries(entry("../x")) }
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.entries(entry("x", 3)) }
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.entries(entry("x", size = -1)) }
        assertThrows(Exception::class.java) { MemberFilesProtocol.entries(entry("x").also { it[35] = 0xff.toByte() }) }
    }
    @Test fun errorsAndMalformedFramingArePreserved() {
        assertEquals(14, MemberFilesProtocol.response(reply(status = 14)).status)
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.response(reply(status = 15)) }
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.response(reply().copyOf(212)) }
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.response(reply().also { it[206] = 2 }) }
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.response(reply("abc").also { it[213] = 0 }) }
    }
    @Test fun quotaAndProgressValidate64BitValues() {
        val q = MemberFilesProtocol.quota(buffer(40).putLong(12).putLong(1000).putLong(34).putLong(5).putLong(6).array())
        assertEquals(34L, q.reserved); assertEquals(6L, q.directories)
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.quota(ByteArray(40)) }
        val wire = buffer(123).putLong(71).put(MemberFilesProtocol.addressBytes(uid)).put(MemberFilesProtocol.addressBytes(server)).put(MemberFilesProtocol.addressBytes(owner)).putLong(20).putLong(40).putShort(0).put(1).array()
        val progress = MemberFilesProtocol.progress(wire)
        assertFalse(progress.finished); assertEquals(owner, progress.owner); assertEquals(20L, progress.done)
        wire[122] = 2; wire[120] = 12
        assertTrue(MemberFilesProtocol.progress(wire).finished); assertEquals(12, MemberFilesProtocol.progress(wire).status)
        ByteBuffer.wrap(wire).order(ByteOrder.LITTLE_ENDIAN).putLong(104, 41)
        assertThrows(IllegalArgumentException::class.java) { MemberFilesProtocol.progress(wire) }
    }
}
