package com.utun.chat.data

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder

data class DeviceInvitation(val id: String, val peer: String, val outgoing: Boolean, val state: Int,
    val pin: String = "", val input: Int = 0, val pinError: Boolean = false) {
    val pending: Boolean get() = state in listOf(1, 2, 3, 8, 9, 10)
}
data class DevgroupState(val name: String = "", val member: Boolean = false, val invitations: List<DeviceInvitation> = emptyList(),
    val pin: String = "", val failed: Boolean = false, val outgoingVisible: Boolean = false,
    val leaving: Boolean = false, val leaveFailed: Boolean = false,
    val renaming: Boolean = false, val renameFailed: Boolean = false,
    val outgoingId: String = "", val accepting: String = "", val choosing: Boolean = false)

/** Только main thread; входящие запросы обновляют меню self, диалог открывается явным действием. PIN не логируется. */
object DevgroupRepository {
    private val mutable = MutableStateFlow(DevgroupState())
    val state = mutable.asStateFlow()
    private var nextId = 0L
    private val pending = mutableMapOf<Long, Int>()
    fun command(action: Int = 0, group: String = "", peer: String = "", text: String = "", pin: String = "") {
        if (action == 0 && pending.containsValue(0)) return
        if (action == 1) mutable.value = mutable.value.copy(outgoingVisible = true, outgoingId = "", pin = "", failed = false, choosing = false)
        if (action == 2) mutable.value = mutable.value.copy(accepting = text, failed = false)
        if (action == 3) mutable.value = mutable.value.copy(renaming = true, renameFailed = false)
        if (action == 4) mutable.value = mutable.value.copy(leaving = true, leaveFailed = false)
        val id = ++nextId
        if (NativeLib.devgroupCommand(action, group, peer, text, pin, id)) pending[id] = action
        else { LogManager.addLog("WARN", "Devgroup", "Core rejected request action=$action"); mutable.value = mutable.value.copy(
            failed = true, accepting = "", leaving = false, leaveFailed = action == 4, renaming = false, renameFailed = action == 3) }
    }
    fun dismissLeaveError() { mutable.value = mutable.value.copy(leaveFailed = false) }
    fun dismissRenameError() { mutable.value = mutable.value.copy(renameFailed = false) }
    fun closeOutgoing() { mutable.value = mutable.value.copy(outgoingVisible = false, outgoingId = "", pin = "", failed = false) }
    fun openPairing(id: String = "") {
        val invitations = mutable.value.invitations.filter { it.pending }
        val selected = if (id.isNotEmpty()) invitations.firstOrNull { it.id == id } else invitations.singleOrNull()
        if (invitations.isEmpty() || (id.isNotEmpty() && selected == null)) {
            LogManager.addLog("WARN", "Devgroup", "No pending pairing to open"); return
        }
        mutable.value = mutable.value.copy(outgoingVisible = true, outgoingId = selected?.id ?: "", pin = "",
            failed = false, choosing = selected == null)
        LogManager.addLog("INFO", "Devgroup", "Pairing opened from self menu")
        command()
    }
    fun onEvent(type: Int, data: ByteArray?) {
        if (type == 13) { pending.clear(); mutable.value = DevgroupState(); return }
        if (type == 12) { command(); return }
        if (type == 70) { command(); return }
        if (type != 71 || data == null || data.size < 9) return
        val id = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).long
        val action = pending.remove(id) ?: return
        if (data[8].toInt() != 1) {
            LogManager.addLog("WARN", "Devgroup", "Core operation failed action=$action")
            mutable.value = mutable.value.copy(failed = true, accepting = "", leaving = false, leaveFailed = action == 4,
                renaming = false, renameFailed = action == 3); return
        }
        val json = JSONObject(String(data, 9, data.size - 9, Charsets.UTF_8))
        if (action == 1) {
            if (mutable.value.outgoingVisible) mutable.value = mutable.value.copy(pin = json.getString("pin"), outgoingId = json.getString("id"), failed = false)
            command()
            return
        }
        val invitations = json.getJSONArray("invitations")
        if (action == 3) LogManager.addLog("INFO", "Devgroup", "Device group renamed")
        mutable.value = mutable.value.copy(name = json.getString("name"), member = json.getString("group").isNotEmpty(),
            invitations = List(invitations.length()) { i -> val entry = invitations.getJSONObject(i)
                DeviceInvitation(entry.getString("id"), entry.getString("peer"), entry.getInt("outgoing") != 0, entry.getInt("state"),
                    entry.optString("pin"), entry.optInt("input"), entry.optInt("pin_error") != 0) },
            leaving = if (action == 4) false else mutable.value.leaving,
            renaming = if (action == 3) false else mutable.value.renaming,
            accepting = if (action == 2) "" else mutable.value.accepting,
            failed = if (action == 0) mutable.value.failed else false)
    }
}
