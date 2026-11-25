package com.utun.chat.data

import java.util.UUID

/** Режим автосокетов для ConfigProvider; configValue сохраняется в настройках/INI, protocols задаёт порядок секций. */
enum class AutoSocketProtocols(val configValue: String, val protocols: List<String>) {
    TCP("tcp", listOf("tcp")),
    UDP("udp", listOf("udp")),
    BOTH("tcp+udp", listOf("tcp", "udp"));

    /**
     * Для одного интерфейса и IP-литерала создаёт запись каждого выбранного протокола с новым UUID.
     * Порт берётся из первой saved-записи с тем же socketKey (даже при смене IP), иначе выбирается из 2000..9999.
     * saved не изменяется: неактивные интерфейсы/протоколы сохраняет вызывающий код. Сеть не вызывается, bind/проверки порта здесь нет.
     */
    fun createServers(ifName: String, bindIp: String, saved: List<ServerEntry>): List<ServerEntry> = protocols.map { protocol ->
        val key = Triple(ifName, bindIp.contains(":"), protocol)
        val port = saved.firstOrNull { it.socketKey == key }?.port ?: (2000..9999).random()
        ServerEntry(UUID.randomUUID().toString(), ifName, bindIp, protocol, port)
    }
}

/** Описание listen-сокета для генератора конфига; само не открывает сокет и не проверяет входные поля. */
data class ServerEntry(
    val id: String,          // UUID записи UI; при повторном обнаружении создаётся заново.
    val ifName: String,      // Имя интерфейса: wlan0, rmnet0 и т. п.
    val bindIp: String,      // IP-литерал или wildcard 0.0.0.0/::; ':' определяет семейство IPv6.
    val protocol: String,    // tcp или udp в нижнем регистре.
    val port: Int            // Listen-порт; допустимость обеспечивает вызывающий код/парсер конфига.
) {
    /** Устойчивый ключ сохранения порта; не зависит от UUID и конкретного адреса внутри семейства IP. */
    val socketKey: Triple<String, Boolean, String> get() = Triple(ifName, bindIp.contains(":"), protocol)
    /** Суффикс [server:...]; один сокет на интерфейс, семейство IP и протокол. */
    val sectionName: String get() = "${ifName}_${if (bindIp.contains(":")) "v6" else "v4"}_$protocol"
}
