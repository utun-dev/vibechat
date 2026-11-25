package com.utun.chat.data

import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.MutablePreferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey

/** TCP-монитор ядра: только числовой IPv4 (ACL control server не поддерживает IPv6).
 * Выключение удаляет endpoint из DataStore; значения по умолчанию — только для следующего включения. */
data class MonitoringSettings(val enabled: Boolean = false, val ip: String = "127.0.0.1", val port: Int = 9999) {
    val valid: Boolean get() = port in 1..65535 && ip.split('.').let { parts ->
        parts.size == 4 && parts.all { part -> part.toIntOrNull()?.let { it in 0..255 && part == it.toString() } == true }
    }

    fun writeTo(prefs: MutablePreferences) {
        require(!enabled || valid) { "Monitoring requires a numeric IPv4 address and port 1..65535" }
        prefs[enabledKey] = enabled
        if (enabled) { prefs[ipKey] = ip; prefs[portKey] = port }
        else { prefs.remove(ipKey); prefs.remove(portKey) }
    }

    /** Слушаем только выбранный IP; при включении разрешаем TCP-клиентам подключаться к этому endpoint. */
    fun appendConfig(config: StringBuilder) {
        if (!enabled) return
        require(valid) { "Invalid monitoring endpoint" }
        config.appendLine("[control]")
        config.appendLine("ip=$ip")
        config.appendLine("port=$port")
        config.appendLine("allow=0.0.0.0/0")
        config.appendLine()
    }

    companion object {
        private val enabledKey = booleanPreferencesKey("control_enabled")
        private val ipKey = stringPreferencesKey("control_bind_ip")
        private val portKey = intPreferencesKey("control_port")

        fun read(prefs: Preferences): MonitoringSettings = if (prefs[enabledKey] == true)
            MonitoringSettings(true, prefs[ipKey] ?: "127.0.0.1", prefs[portKey] ?: 9999) else MonitoringSettings()
    }
}
