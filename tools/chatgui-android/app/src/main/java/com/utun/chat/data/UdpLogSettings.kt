package com.utun.chat.data

/** Настройка logreceiver: адрес сохраняется при выключении, enabled не зависит от наличия адреса.
 * validAddress проверяет канонический числовой IPv4 (без DNS/ведущих нулей); active также требует port=1..65535.
 * appendConfig пишет [log_udp] лишь для active; достижимость и отправку проверяет native UDP logger. */
data class UdpLogSettings(val enabled: Boolean = false, val ip: String = DEFAULT_IP, val port: Int = DEFAULT_PORT) {
    val validAddress: Boolean get() = ip.split('.').let { parts -> parts.size == 4 && parts.all { part ->
        part.toIntOrNull()?.let { it in 0..255 && part == it.toString() } == true
    } }
    val active: Boolean get() = enabled && validAddress && port in 1..65535

    /** Добавить сетевой target в INI только при явном включении и заполненном адресе/порте. */
    fun appendConfig(config: StringBuilder) {
        if (active) {
            config.appendLine()
            config.appendLine("[log_udp]")
            config.appendLine("ip=$ip")
            config.appendLine("port=$port")
        }
    }

    companion object {
        const val DEFAULT_IP = "192.168.40.250"
        const val DEFAULT_PORT = 9999
    }
}
