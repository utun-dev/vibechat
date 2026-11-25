package com.utun.chat.data

object LogManager {
    data class Entry(val level: String, val category: String, val message: String)
    val entries = mutableListOf<Entry>()
    fun addLog(level: String, category: String, message: String) {
        entries.add(Entry(level, category, message)); System.err.println("[$level] [$category] $message")
    }
}

/** Уведомления независимы: host может отказать на любом обновлении, не запуская реальные звонки, службу или звук. */
object HostLanguageNotifications {
    val attempts = mutableListOf<String>()
    val failures = mutableSetOf<String>()
    fun refresh(name: String) {
        attempts.add(name)
        check(name !in failures) { "$name notification unavailable" }
    }
}
object IncomingCallManager { fun refreshLanguage() = HostLanguageNotifications.refresh("incoming call") }
object CallController { fun refreshLanguage() = HostLanguageNotifications.refresh("ongoing call") }
object RadioOverlayService { fun refreshLanguage() = HostLanguageNotifications.refresh("radio") }
