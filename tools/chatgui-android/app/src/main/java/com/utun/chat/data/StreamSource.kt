package com.utun.chat.data

import org.json.JSONArray

/** Источник идентифицируется участником и эпохой; Long сохраняет все 64 бита uint64.
 * parse читает JSON array: id — Ed25519 в hex, epoch — hex-строка, name — строка, self=1 означает себя.
 * Нулевые ID/epoch, повтор участника и неверный JSON бросают ошибку; caller сохраняет прежний снимок и логирует её. */
data class StreamSource(val id: String, val name: String, val epoch: Long, val self: Boolean) {
    val displayName: String get() = name.ifEmpty { id }

    companion object {
        fun parse(json: String): List<StreamSource> {
            val array = JSONArray(json)
            val seen = mutableSetOf<String>()
            return (0 until array.length()).map { i ->
                val item = array.getJSONObject(i)
                val id = item.getString("id")
                val epoch = item.getString("epoch").toULong(16).toLong()
                require(MemberFilesProtocol.validAddress(id) && epoch != 0L) { "Invalid stream identity" }
                require(seen.add(id)) { "Duplicate stream source=$id" }
                StreamSource(id, item.getString("name"), epoch, item.getInt("self") == 1)
            }
        }
    }
}
