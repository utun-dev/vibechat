package com.utun.chat.data

import java.util.UUID

/** Кодек listen-секций ConfigProvider, без файловой системы/Android. Полученные wildcard IP вызывающий код заменяет текущими. */
internal object ServerConfig {
    private val sectionName = Regex("(.+)_(v4|v6)(?:_(tcp|udp))?")

    /**
     * Читает [server:interface_v4/v6_tcp/udp]; без суффикса протокола по умолчанию TCP.
     * transport имеет приоритет над суффиксом; порт 1..65535. Остальные секции/поля не относятся к этому кодеку.
     * includeCommented восстанавливает полностью закомментированные секции. Неверная секция пропускается с warn.
     */
    fun parse(lines: Sequence<String>, includeCommented: Boolean, warn: (String) -> Unit): List<ServerEntry> {
        val result = mutableListOf<ServerEntry>()
        val fields = mutableMapOf<String, String>()
        var name: String? = null
        var sectionCommented = false

        fun finishSection() {
            val current = name ?: return
            val match = sectionName.matchEntire(current)
            if (match == null) { warn("saved server '$current': invalid interface/family/protocol section name"); return }
            val protocol = fields["transport"] ?: match.groupValues[3].ifEmpty { "tcp" }
            if (protocol != "tcp" && protocol != "udp") { warn("saved server '$current': unsupported transport"); return }
            val port = fields["addr"]?.substringAfterLast(':', "")?.toIntOrNull()
            if (port == null || port !in 1..65535) { warn("saved server '$current': invalid listen port parsed=$port, expected 1..65535"); return }
            val ip = if (match.groupValues[2] == "v6") "::" else "0.0.0.0"
            result.add(ServerEntry(UUID.randomUUID().toString(), match.groupValues[1], ip, protocol, port))
        }

        for (line in lines) {
            val trimmed = line.trim()
            val commented = trimmed.startsWith('#')
            val body = if (commented) trimmed.removePrefix("#").trim() else trimmed
            if (body.startsWith('[') && body.endsWith(']')) {
                finishSection()
                fields.clear()
                sectionCommented = commented
                name = if (body.startsWith("[server:") && (!commented || includeCommented))
                    body.removeSurrounding("[", "]").removePrefix("server:") else null
            } else if (name != null && commented == sectionCommented) {
                val equals = body.indexOf('=')
                if (equals > 0) fields[body.substring(0, equals).trim()] = body.substring(equals + 1).trim()
            }
        }
        finishSection()
        return result
    }
}
