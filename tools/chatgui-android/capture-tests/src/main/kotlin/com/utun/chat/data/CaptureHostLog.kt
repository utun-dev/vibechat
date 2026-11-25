package com.utun.chat.data

import java.util.concurrent.LinkedBlockingQueue

/** Структурированные логи настоящего reader; управление ошибками не заменяется fixture. */
object LogManager {
    data class Entry(val level: String, val category: String, val message: String)
    val entries = LinkedBlockingQueue<Entry>()
    fun addLog(level: String, category: String, message: String) {
        entries.add(Entry(level, category, message))
        System.err.println("[$level] [$category] $message")
    }
}
