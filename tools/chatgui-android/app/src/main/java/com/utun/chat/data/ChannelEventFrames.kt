package com.utun.chat.data

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Полные бинарные снимки событий 27/29/30 из chat_event.h. Некорректный кадр бросает
 * IllegalArgumentException до изменения UI; вызывающий обработчик логирует тип события и причину. */
object ChannelEventFrames {
    data class Nodes(val channel: String, val addresses: List<String>)
    data class Candidates(val channel: String, val automatic: String, val addresses: List<String>)
    data class Invite(val channel: String, val request: Long, val link: String?)

    private fun header(data: ByteArray?, tail: Int): Pair<String, ByteBuffer> {
        require(data != null && data.isNotEmpty()) { "Missing channel frame" }
        val length = data[0].toInt() and 255
        require(length > 0 && data.size >= 1 + length + tail) { "Invalid channel frame bytes=${data.size} channelBytes=$length" }
        val channel = String(data, 1, length, Charsets.UTF_8)
        return channel to ByteBuffer.wrap(data, 1 + length, data.size - 1 - length).order(ByteOrder.LITTLE_ENDIAN)
    }

    /** CONNECTING_NODES из интерфейса состояния транспорта. */
    fun nodes(data: ByteArray?): Nodes {
        val (channel, buffer) = header(data, 2)
        val count = buffer.short.toInt() and 65535
        require(buffer.remaining() == count * 32) { "Invalid node list channel=$channel count=$count bytes=${buffer.remaining()}" }
        val addresses = List(count) { MemberFilesProtocol.addressHex(ByteArray(32).also { buffer.get(it) }) }
        require(addresses.all(MemberFilesProtocol::validAddress)) { "Invalid connecting node key channel=$channel" }
        return Nodes(channel, addresses)
    }

    /** Снимок Ed25519 узлов приглашения. Нулевой auto допустим при пустом списке. */
    fun candidates(data: ByteArray?): Candidates {
        val (channel, buffer) = header(data, 34)
        fun readAddress() = MemberFilesProtocol.addressHex(ByteArray(32).also { buffer.get(it) })
        val automatic = readAddress()
        val count = buffer.short.toInt() and 65535
        require(buffer.remaining() == count * 32) { "Invalid invite candidates channel=$channel count=$count bytes=${buffer.remaining()}" }
        val addresses = List(count) { readAddress() }
        require(addresses.all(MemberFilesProtocol::validAddress) && (count == 0 || MemberFilesProtocol.validAddress(automatic))) {
            "Invalid invite candidate key channel=$channel"
        }
        return Candidates(channel, automatic.takeIf(MemberFilesProtocol::validAddress).orEmpty(), addresses)
    }

    /** Пустой link — подтверждённая ошибка C; усечённый/дополненный кадр не завершает запрос. */
    fun invite(data: ByteArray?): Invite {
        val (channel, buffer) = header(data, 10)
        val request = buffer.long
        val length = buffer.short.toInt() and 65535
        require(buffer.remaining() == length) { "Invalid invite link channel=$channel length=$length bytes=${buffer.remaining()}" }
        val bytes = ByteArray(length); buffer.get(bytes)
        return Invite(channel, request, if (length == 0) null else String(bytes, Charsets.UTF_8))
    }
}
