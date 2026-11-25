package com.utun.chat.data

/** Только границы списка каналов; radio engine/overlay/PCM/session — настоящие компоненты приложения. */
class HostRadioChannel(val id: String, val name: String = id)
class HostRadioMember(val nodeId: Long, val name: String, val ed25519: String = nodeId.toULong().toString(16).padStart(64, '0'))
class ChatRepository {
    companion object { var failRadioLookup = false }
    fun getChannels(): List<HostRadioChannel> {
        check(!failRadioLookup) { "channel lookup failed" }
        return listOf(HostRadioChannel("07".repeat(32)), HostRadioChannel("08".repeat(32)))
    }
    fun getMembers(channel: String): List<HostRadioMember> {
        check(!failRadioLookup) { "member lookup failed" }
        return listOf(HostRadioMember(99, "Alice"))
    }
}
