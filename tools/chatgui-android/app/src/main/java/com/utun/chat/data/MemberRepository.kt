package com.utun.chat.data

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** Аватар заголовка: первое неудалённое устройство с аватаркой, независимо от online. */
fun deviceGroupAvatar(members: List<ChatMember>): ChatMember? =
    members.firstOrNull { !it.isDeleted && it.avatarServer.isNotEmpty() && it.avatarFile.isNotEmpty() }
        ?: members.firstOrNull { !it.isDeleted }

data class MemberKey(val group: String, val owner: String)

/** Полные снимки CHAT-групп. Все методы вызываются последовательно на main, события не replay-ятся.
 * loaded хранится отдельно: отсутствие участника в полученном списке не требует нового запроса.
 * Pending подавляет повтор 10 секунд; отказ/исключение enqueue разрешает повтор сразу, не очищая последний снимок.
 * Полный кадр применяется атомарно, отдельные update/remove меняют одного участника; stop очищает всё и увеличивает generation. */
class MemberRepository(
    private val enqueue: (String) -> Boolean = NativeLib::requestMemberList,
    private val ready: () -> Boolean = NativeLib::isReady,
    private val clock: () -> Long = android.os.SystemClock::elapsedRealtime,
    private val log: (String, String) -> Unit = { level, text -> LogManager.addLog(level, "Members", text) }
) {
    private val _members = MutableStateFlow<Map<MemberKey, ChatMember>>(emptyMap())
    val members: StateFlow<Map<MemberKey, ChatMember>> = _members
    private val loaded = mutableSetOf<String>()
    private val pending = mutableMapOf<String, Long>()
    var generation = 0L
        private set
    private var active = false

    fun member(group: String, ed: String): ChatMember? = _members.value[MemberKey(group, ed)]
    fun list(group: String): List<ChatMember> = _members.value.filterKeys { it.group == group }.values.toList()
    fun ensure(group: String) { if (group !in loaded) refresh(group) }

    fun refresh(group: String) {
        if (!active || !ready()) return
        if (!MemberFilesProtocol.validAddress(group)) {
            log("ERROR", "Invalid snapshot channel=$group"); return
        }
        val now = clock()
        val started = pending[group]
        if (started != null && now - started < 10_000) return
        if (started != null) log("WARN", "Snapshot response timeout channel=$group elapsedMs=${now - started}")
        pending[group] = now
        val accepted = try { enqueue(group) } catch (e: Exception) {
            pending.remove(group); log("ERROR", "Snapshot enqueue threw channel=$group: $e"); return
        }
        if (!accepted) {
            pending.remove(group); log("ERROR", "Snapshot enqueue failed channel=$group"); return
        }
        log("DEBUG", "Snapshot requested channel=$group loaded=${group in loaded}")
    }

    /** Применяется AppEventHandler до передачи остальных событий подписчикам UI. */
    fun onEvent(type: Int, data: ByteArray?) {
        if (type == 9 || type == 12) { active = true; return }
        if (type == 13) {
            active = false; generation++; loaded.clear(); pending.clear(); _members.value = emptyMap()
            log("INFO", "Core stopped: member snapshots cleared generation=$generation"); return
        }
        if (type == 70) { loaded.toList().forEach(::refresh); return }
        if (type !in listOf(23, 24, 25, 31)) return
        if (!active) { log("DEBUG", "Ignored member event after stop type=$type"); return }
        try {
            require(data != null && data.isNotEmpty()) { "missing header" }
            val length = data[0].toInt() and 255
            require(length > 0 && data.size >= 1 + length) { "invalid channel length=$length" }
            val group = String(data, 1, length, Charsets.UTF_8)
            require(MemberFilesProtocol.validAddress(group)) { "invalid channel=$group" }
            val previous = _members.value
            val updated = previous.toMutableMap()
            if (type == 31) {
                require(data.size == 1 + length) { "invalid channel deletion size" }
                updated.keys.removeAll { it.group == group }; loaded.remove(group); pending.remove(group)
            } else if (type == 25) {
                require(data.size == 1 + length + 32) { "invalid removal size" }
                val ed = MemberFilesProtocol.addressHex(data.copyOfRange(1 + length, data.size))
                require(MemberFilesProtocol.validAddress(ed)) { "missing removal key" }
                updated.remove(MemberKey(group, ed))
            } else {
                val countSize = if (type == 23) 2 else 1
                require(data.size >= 1 + length + countSize) { "missing count" }
                val count = (data[1 + length].toInt() and 255) +
                    if (type == 23) ((data[2 + length].toInt() and 255) shl 8) else 0
                require(type == 23 || count == 1) { "invalid single count=$count" }
                val offset = 1 + length + countSize
                require(data.size == offset + count * MEMBER_WIRE_SIZE) { "invalid size count=$count bytes=${data.size}" }
                if (type == 23) updated.keys.removeAll { it.group == group }
                repeat(count) { i ->
                    val wire = requireNotNull(parseMemberWire(data, offset + i * MEMBER_WIRE_SIZE))
                    val key = MemberKey(group, wire.ed25519)
                    require(MemberFilesProtocol.validAddress(key.owner) && (type != 23 || !updated.containsKey(key))) { "invalid/duplicate key" }
                    updated[key] = wire.toChatMember()
                }
                if (type == 23) {
                    loaded.add(group); pending.remove(group)
                    log("DEBUG", "Snapshot received channel=$group count=$count changed=${updated != previous}")
                }
            }
            for ((key, member) in updated) if (key.group == group && previous[key] != member) {
                log("DEBUG", "Member channel=$group peer=${key.owner} online=${member.online} " +
                    "up=${member.directUp}/${member.indirectUp}/${member.bgpUp} rtt=${member.rtt} " +
                    "phone=${member.phoneStatusKnown}/${member.phoneBusy} deleted=${member.isDeleted}")
            }
            _members.value = updated
        } catch (e: IllegalArgumentException) {
            log("ERROR", "Rejected member event type=$type bytes=${data?.size}: ${e.message}")
        }
    }
}
