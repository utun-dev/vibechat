package com.utun.chat.data

/** /proc/net/if_inet6 использует 32 hex-цифры без сжатия нулей; IFA_F_TEMPORARY=0x20.
 * Сопоставляется пара «интерфейс + 16 байтов адреса», прочие адреса/интерфейсы не меняют результат. */
internal fun temporaryIpv6Address(ifName: String, address: ByteArray, rows: Sequence<String>): Boolean {
    require(address.size == 16) { "IPv6 address must contain 16 bytes" }
    val hex = address.joinToString("") { (it.toInt() and 255).toString(16).padStart(2, '0') }
    return rows.any { line ->
        val fields = line.trim().split(Regex("\\s+"), limit = 6)
        fields.size == 6 && fields[0].equals(hex, ignoreCase = true) && fields[5] == ifName &&
            fields[4].toIntOrNull(16)?.let { it and 0x20 != 0 } == true
    }
}
