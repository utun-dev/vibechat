import com.utun.chat.data.ChatMember
import com.utun.chat.data.parseMemberWire
import com.utun.chat.data.toChatMember
import com.utun.chat.data.MEMBER_WIRE_SIZE
import java.nio.ByteBuffer
import java.nio.ByteOrder

/* Проверяет реальные классы из app/build/tmp/kotlin-classes/debug после assembleDebug. */
fun main() {
    val offset = 7
    val data = ByteArray(offset + MEMBER_WIRE_SIZE)
    ByteBuffer.wrap(data, offset, MEMBER_WIRE_SIZE).order(ByteOrder.LITTLE_ENDIAN).apply {
        putLong(42L); put(1); put(1); put(0x12); putShort(250); put(1); put(1)
        put("Member".toByteArray())
    }
    data[offset + 182] = 1; data[offset + 183] = 4
    data[offset + 184] = 7; data[offset + 185] = 7
    ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).putShort(offset + 186, 5)
    data[offset + MEMBER_WIRE_SIZE - 1] = 42
    val wire = requireNotNull(parseMemberWire(data, offset))
    check(wire.nodeId == 42L && wire.isSelf && wire.name == "Member")
    val previous = ChatMember(42L, "old", isVerified = true, nodeType = 4, directUp = true,
        indirectUp = true, directPresence = true, indirectPresence = true, bgpPresence = true,
        bgpRtt = 321, bgpNodes = 5, connecting = true)
    val enabled = wire.toChatMember(previous)
    check(enabled.radioActive && enabled.isAdmin && enabled.isStorage && enabled.online)
    check(enabled.isVerified && enabled.nodeType == 4 && enabled.directUp && enabled.indirectUp)
    check(enabled.directPresence && enabled.indirectPresence && enabled.bgpPresence)
    check(enabled.bgpRtt == 250 && enabled.bgpNodes == 5 && enabled.connecting)
    check(enabled.ed25519 == "00".repeat(31) + "2a")
    check(enabled.rtt == 250 && enabled.name == "Member")
    check(!enabled.radioMuted)
    data[offset + 10] = 0x32
    val muted = requireNotNull(parseMemberWire(data, offset)).toChatMember(enabled)
    check(muted.radioMuted && muted.radioActive && muted.isVerified && muted.directUp)

    data[offset + 10] = 2 // Повторное обновление выключает рацию, сохраняя остальные сведения.
    val disabled = requireNotNull(parseMemberWire(data, offset)).toChatMember(muted)
    check(!disabled.radioActive && disabled.isVerified && disabled.directUp && disabled.connecting)
    check(!disabled.radioMuted)
    data[offset + 10] = 8
    check(requireNotNull(parseMemberWire(data, offset)).toChatMember(disabled).isDeleted)
    check(parseMemberWire(data, -1) == null)
    check(parseMemberWire(data, Int.MAX_VALUE) == null)
    check(parseMemberWire(data, offset + 1) == null)
    check(parseMemberWire(ByteArray(MEMBER_WIRE_SIZE - 1)) == null)
    println("PASS: radio subscription/mute; wire offsets; complete live properties; deleted flag; truncated records")
}
