package com.utun.chat.data

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/** Значения JNI проверяются как строки uint64 для ключа приглашения и hex для X25519. */
class InviteLinkTest {
    private fun reality() = InviteReality(true, ByteArray(8) { 1 }, ByteArray(32) { 2 }, byteArrayOf(1, 2, 3), "example.test")
    private fun invite() = InviteData(13, ByteArray(32) { 3 },
        listOf(InviteAddr(4, byteArrayOf(127, 0, 0, 1), 9999, 7, INVITE_PROTO_TCP or INVITE_PROTO_REALITY)),
        reality(), "utun://original")

    private fun preview(key: String = "13") = """
        {"kind":0,"key":"$key","pubkey":"${"03".repeat(32)}",
         "reality":{"short":"${"01".repeat(8)}","pubkey":"${"02".repeat(32)}","version":"010203","name":"6578616d706c652e74657374"},
         "addresses":[{"family":4,"socket":7,"proto":6,"port":9999,"ip":"7f000001"}]}
    """.trimIndent()

    @Test fun joinKeyAndAllPreviewFieldsSurviveMapping() {
        assertEquals(invite().copy(joinKey = -1), InviteLink.fromNativePreview(preview(ULong.MAX_VALUE.toString()), "utun://original"))
    }

    @Test fun uint64JoinKeyRequiresString() {
        for (value in listOf(19L, 19.0, true, JSONObject.NULL)) {
            val json = JSONObject(preview()).put("key", value)
            assertThrows(Exception::class.java) { InviteLink.fromNativePreview(json.toString(), "utun://original") }
        }
    }

    @Test fun unsignedKeyRejectsOutOfRangeStrings() {
        for (bad in listOf("-1", "18446744073709551616", "1.5", "not-a-number")) {
            assertThrows(Exception::class.java) { InviteLink.fromNativePreview(preview(bad), "utun://original") }
        }
    }

    @Test fun independentSnapshotsHaveStructuralEquality() {
        val first = invite(); val second = invite()
        assertEquals(first, second); assertEquals(first.hashCode(), second.hashCode())
        assertEquals(first.reality, second.reality); assertEquals(first.reality.hashCode(), second.reality.hashCode())
        assertEquals(1, hashSetOf(first, second).size)
    }

    @Test fun everyInvitationFieldParticipatesInEquality() {
        val first = invite()
        val changed = listOf(first.copy(joinKey = 14), first.copy(pubkey = ByteArray(32) { 4 }),
            first.copy(addrs = emptyList()),
            first.copy(reality = null), first.copy(link = "utun://different"), first.copy(kind = INVITE_KIND_SERVER))
        changed.forEach { assertNotEquals(first, it) }
        assertEquals(changed.size + 1, (changed + first).toSet().size)
    }

    @Test fun serverPreviewPreservesKindWithoutExposingServerKey() {
        val json = JSONObject(preview()).put("kind", INVITE_KIND_SERVER).put("key", "0")
        val server = InviteLink.fromNativePreview(json.toString(), "utun://server")
        assertEquals(INVITE_KIND_SERVER, server.kind)
        assertEquals("utun://server", server.link)
    }

    @Test fun everyRealityFieldParticipatesInEquality() {
        val first = reality()
        listOf(first.copy(has = false), first.copy(shortId = ByteArray(8)), first.copy(serverPubkey = ByteArray(32)),
            first.copy(version = byteArrayOf(4, 5, 6)), first.copy(serverName = "other.test")).forEach { assertNotEquals(first, it) }
    }
}
