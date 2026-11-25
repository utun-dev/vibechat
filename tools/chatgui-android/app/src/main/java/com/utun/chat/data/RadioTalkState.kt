package com.utun.chat.data

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Снимок говорящих одной рации: пустой groupId отключает обработку событий, speakers содержит Ed25519 говорящих.
 * При включении/смене канала вызывающий создаёт новый RadioTalkState(group), при выключении — RadioTalkState().
 * Снимки участников и локальный mute независимы от индикации речи; переданный speakers нельзя мутировать извне.
 * C-ядро хранит одного talker на узел: смена stream даёт новый START без STOP прежнего stream.
 * Поэтому набор учитывает полный ключ, а stream служит диагностике и не создаёт отдельного говорящего.
 */
data class RadioTalkState(val groupId: String = "", val speakers: Set<String> = emptySet()) {
    /** Применить валидное событие парсера; чужая/выключенная группа и повтор состояния возвращают this.
     * START добавляет узел, STOP удаляет только его; прежний снимок остаётся неизменным. */
    fun onTalk(event: RadioTalkEvent): RadioTalkState {
        if (groupId.isEmpty() || event.groupId != groupId || speakers.contains(event.source) == event.on) return this
        return copy(speakers = if (event.on) speakers + event.source else speakers - event.source)
    }
}

/** Одно событие речи: ненулевой ID группы и Ed25519 источника, stream=0..65535, on=true для START и false для STOP. */
data class RadioTalkEvent(val groupId: String, val source: String, val stream: Int, val on: Boolean)

/** Разобрать ровно 67 байт CHAT_EVT_RADIO_TALK: group(32), source(32), stream(2), on(1), little endian.
 * Только on=0/1 и ненулевые group/source; stream допускает весь uint16, включая 0.
 * При null, неверной длине/флаге/нулевом ID вернуть null; диагностику отказа пишет вызывающий ChatViewModel. */
fun parseRadioTalkEvent(data: ByteArray?): RadioTalkEvent? {
    if (data == null || data.size != 67 || data[66].toInt() !in 0..1) return null
    val bytes = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    val group = MemberFilesProtocol.addressHex(ByteArray(32).also { bytes.get(it) })
    val source = MemberFilesProtocol.addressHex(ByteArray(32).also { bytes.get(it) })
    if (group.isEmpty() || source.isEmpty()) return null
    return RadioTalkEvent(group, source, bytes.short.toInt() and 0xffff, data[66].toInt() == 1)
}
