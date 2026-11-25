package com.utun.chat.data

/** Одна ожидаемая навигация: ID принятой worker команды фиксируется до DM_CONV_UPDATED.
 * Чужое событие сохраняет ожидание. Новый запрос/отмена лишает старый timeout права менять состояние. */
class PendingDmOpen {
    private data class Request(val token: Long, val conversation: String, val peer: String, val name: String)
    private var sequence = 0L
    private var request: Request? = null
    val pending: Boolean get() = request != null

    fun begin(conversation: String, peer: String, name: String): Long {
        require(conversation.toULongOrNull()?.let { it != 0UL } == true && MemberFilesProtocol.validAddress(peer))
        val token = ++sequence
        request = Request(token, conversation, peer, name)
        return token
    }
    fun isPending(token: Long): Boolean = request?.token == token
    fun cancel() { request = null }
    fun complete(conversation: String, generation: Long): DmConversation? {
        val current = request ?: return null
        if (current.conversation != conversation || generation <= 0) return null
        request = null
        return DmConversation(convId = conversation, peerEd25519 = current.peer, peerName = current.name, generation = generation)
    }
}
