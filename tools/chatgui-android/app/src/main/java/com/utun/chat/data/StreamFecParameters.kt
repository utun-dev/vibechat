package com.utun.chat.data

/** Общие бюджеты ядра: defaults и диапазоны соответствуют stream.h/chat_setting.c. */
object StreamFecParameters {
    const val REQUIRED_LIMIT = 192
    data class Budget(val key: String, val defaultValue: Int, val range: IntRange)
    val budgets = listOf(
        Budget("stream_regular_limit", REQUIRED_LIMIT, 1..4096),
        Budget("stream_temporary_limit", 64, 1..1024),
        Budget("stream_background_limit", 16, 1..1024),
        Budget("stream_peer_temporary_limit", 16, 1..1024),
        Budget("stream_peer_limit", 64, 1..1024),
        Budget("stream_control_limit", 128, 8..4096),
        Budget("stream_extra_receive_limit", 16, 0..254)
    )
    fun valid(total: Int?, redundant: Int?, regularLimit: Int = REQUIRED_LIMIT): Boolean = total != null && total in 1..255
        && redundant != null && redundant in 0 until total && total - redundant <= regularLimit
    fun validBudgets(values: Map<String, Int>): Boolean = values.size == budgets.size
        && budgets.all { values[it.key] in it.range }
}
