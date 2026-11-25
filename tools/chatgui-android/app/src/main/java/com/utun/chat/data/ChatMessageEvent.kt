package com.utun.chat.data

/** Уведомление CHAT_EVT_MSG_RECEIVED с проверенным Ed25519 автора. */
internal class ChatMessageEvent(val channel: String, val author: ByteArray, val initial: Boolean) {
    companion object {
        fun decode(data: ByteArray?): ChatMessageEvent? {
            val length = data?.firstOrNull()?.toInt()?.and(0xff) ?: 0
            if (data == null || length != 64 || data.size != 1 + length + 33 || data.last().toInt() !in 0..1) {
                LogManager.addLog("WARN", "Events", "Invalid MSG_RECEIVED bytes=${data?.size} channel_bytes=$length")
                return null
            }
            val channel = String(data, 1, length, Charsets.US_ASCII)
            if (channel.any { it !in '0'..'9' && it !in 'a'..'f' } || channel.drop(16).all { it == '0' }) {
                LogManager.addLog("WARN", "Events", "Invalid MSG_RECEIVED channel bytes=$length")
                return null
            }
            return ChatMessageEvent(channel, data.copyOfRange(1 + length, data.lastIndex), data.last().toInt() == 1)
        }
    }
}
