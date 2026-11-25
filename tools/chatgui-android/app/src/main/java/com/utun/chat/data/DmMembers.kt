package com.utun.chat.data

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Текущий состав закреплённых сторон PM; собственная копия общего member snapshot. */
data class DmMember(val side: Int, val member: ChatMember)

/** Атомарный decode CHAT_EVT_DM_MEMBERS; чужое поколение и неполный/повторный список отклоняются. */
fun parseDmMembers(data: ByteArray, target: ConversationTarget.Pm): List<DmMember> {
    require(data.size >= 12) { "Missing DM member header" }
    val length = data[0].toInt() and 255
    require(length in 1..63 && data.size >= 11 + length) { "Invalid DM member conversation length" }
    require(String(data, 1, length, Charsets.UTF_8) == target.id) { "Different DM member conversation" }
    val header = ByteBuffer.wrap(data, 1 + length, 10).order(ByteOrder.LITTLE_ENDIAN)
    require(header.long == target.generation && target.generation > 0) { "Stale DM member generation" }
    val count = header.short.toInt() and 65535
    require(count <= 128 && data.size == 11 + length + count * (1 + MEMBER_WIRE_SIZE)) { "Invalid DM member count/size" }
    val nodes = mutableSetOf<String>()
    return List(count) { index ->
        val offset = 11 + length + index * (1 + MEMBER_WIRE_SIZE)
        val side = data[offset].toInt() and 255
        require(side in 0..1) { "Invalid DM member side" }
        val wire = requireNotNull(parseMemberWire(data, offset + 1))
        require(wire.ed25519.isNotEmpty() && nodes.add(wire.ed25519)) { "Invalid/duplicate DM member node" }
        DmMember(side, wire.toChatMember())
    }.sortedWith(compareBy<DmMember> { it.side }.thenByDescending { it.member.isSelf }
        .thenByDescending { it.member.online }.thenBy { it.member.ed25519 })
}
