package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class AvatarProtocolTest {
    private val uid = "fedcba9876543210".repeat(4)
    private val owner = "a5".repeat(32)
    private val server = "17".repeat(32)
    private fun buffer(size: Int) = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
    private fun group(size: Int = 0): ByteArray = buffer(84 + size).apply {
        putLong(71); put(MemberFilesProtocol.addressBytes(uid)); putLong(123)
        put(ByteArray(32) { 0xa5.toByte() }); put(1); put(0); putShort(size.toShort())
        put(ByteArray(size) { 0x7f })
    }.array()
    private fun response(): ByteArray = buffer(213).apply {
        put(MemberFilesProtocol.addressBytes(uid)); put(MemberFilesProtocol.addressBytes(server))
        put(5); put(2); put(2); putShort(0)
        putLong(71); put(MemberFilesProtocol.addressBytes(owner)); putLong(0); putLong(512)
        put(64 + 141, 1)
    }.array()

    @Test fun fullNodeAndContentAddress() {
        assertEquals(server, MemberFilesProtocol.addressHex(MemberFilesProtocol.addressBytes(server)))
        assertEquals("fedcba9876543210", AvatarProtocol.hex(0xfedcba9876543210uL.toLong()))
        assertTrue(AvatarProtocol.validFile("a".repeat(64) + ".json"))
        assertTrue(AvatarProtocol.validObject("a".repeat(64)+".webp"))
        listOf("A".repeat(64) + ".json", "../" + "a".repeat(64) + ".json", "a.json", "a".repeat(64) + ".jpg")
            .forEach { assertFalse(it, AvatarProtocol.validFile(it)) }
        assertFalse(MemberFilesProtocol.validAddress("fedcba9876543210"))
    }
    @Test fun groupPreservesAllKeyBytes() {
        val different = uid.dropLast(2) + "01"
        val event = group().also { MemberFilesProtocol.addressBytes(different).copyInto(it, 8) }
        assertEquals(different, AvatarProtocol.group(event).group)
        assertEquals(uid, AvatarProtocol.group(group()).group)
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.group(group().also { it.fill(0, 8, 40) }) }
    }
    @Test fun groupLimitAndSignedDeletion() {
        val record = AvatarProtocol.group(group(16384))
        assertEquals(71L, record.id); assertEquals(uid, record.group); assertEquals(123L, record.version)
        assertEquals("a5".repeat(32), record.hash); assertTrue(record.canEdit); assertFalse(record.failed)
        assertEquals(16384, record.image.size)
        assertEquals(0, AvatarProtocol.group(group()).image.size)
    }
    @Test fun rejectTruncatedOversizedAndMalformedGroup() {
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.group(group(1).copyOf(84)) }
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.group(group(16385)) }
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.group(group().also { it[80] = 2 }) }
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.group(group().also { it[81] = -1 }) }
    }
    @Test fun statHasSizeAndTypeWithoutAContentHash() {
        // STAT общего сервера возвращает метаданные; хеш присутствует только после COMMIT/stream-open.
        val reply = AvatarProtocol.response(response())
        assertEquals(uid, reply.group); assertEquals(server, reply.server); assertEquals(owner, reply.owner)
        assertEquals(71L, reply.id); assertEquals(2, reply.command); assertEquals(0, reply.status)
        assertEquals(512L, reply.size); assertEquals(1, reply.fileType); assertEquals("0".repeat(64), reply.hash)
    }
    @Test fun rejectInvalidFileFraming() {
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.response(response().copyOf(212)) }
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.response(response().also { it[64] = 3 }) }
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.response(response().also { it[65] = 1 }) }
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.response(response().also { it[207] = 1 }) }
        val path = response().copyOf(216).also { it[207] = 3; "abc".toByteArray().copyInto(it, 213) }
        assertEquals(71L, AvatarProtocol.response(path).id)
    }
    @Test fun progressMustFinishBeforeResolvingARequest() {
        val progress = buffer(123).apply {
            putLong(71); put(MemberFilesProtocol.addressBytes(uid)); put(MemberFilesProtocol.addressBytes(server)); put(MemberFilesProtocol.addressBytes(owner))
            putLong(0); putLong(512); putShort(0); put(1)
        }.array()
        assertNull(AvatarProtocol.transfer(progress))
        progress[122] = 2
        assertEquals(71L, AvatarProtocol.transfer(progress)?.id)
        progress[122] = 3
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.transfer(progress) }
        progress[122] = 2
        ByteBuffer.wrap(progress).order(ByteOrder.LITTLE_ENDIAN).putLong(104, 513)
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.transfer(progress) }
    }
    @Test fun failedPublicationRemainsAnError() {
        val result = buffer(41).apply { putLong(71); put(MemberFilesProtocol.addressBytes(uid)); put(1) }.array()
        assertEquals(1, AvatarProtocol.published(result).status)
        assertEquals(uid, AvatarProtocol.published(result).group)
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.published(result.copyOf(40)) }
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.published(result.also { it[40] = 2 }) }
    }
    @Test fun catalogDecodesVersionCursorAndOnlyManifestEntries() {
        val name="b".repeat(64)+".json"
        val entry=buffer(35+name.length).apply {
            putShort(name.length.toShort()); put(1); putLong(300); putLong(1234); put(ByteArray(16)); put(name.toByteArray())
        }.array()
        val wire=buffer(213+7+entry.size).apply {
            put(MemberFilesProtocol.addressBytes(uid)); put(MemberFilesProtocol.addressBytes(server)); put(5); put(2); put(1); putShort(0)
            putLong(71); put(MemberFilesProtocol.addressBytes(owner)); putLong(0); putLong(0); putLong(8); put(ByteArray(16) { 7 })
            position(64+141); put(2); put(1); putShort(7); putShort(0); putShort(entry.size.toShort())
            put("avatars".toByteArray()); put(entry)
        }.array()
        val reply=AvatarProtocol.response(wire)
        assertEquals(8L,reply.cursor); assertTrue(reply.more); assertEquals("avatars",reply.path)
        assertArrayEquals(ByteArray(16) { 7 },reply.version)
        assertEquals(listOf(AvatarProtocol.Entry(name,1234)),AvatarProtocol.entries(reply))
        assertThrows(IllegalArgumentException::class.java) { AvatarProtocol.entries(reply.copy(payload=entry.copyOf(34))) }
    }
}
