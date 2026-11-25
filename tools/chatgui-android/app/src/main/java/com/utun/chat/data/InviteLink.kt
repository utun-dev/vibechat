package com.utun.chat.data

/* Kotlin-представление invite: wire/base64 и допустимость полей проверяет общий C-кодек через NativeLib.
 * Декодирование не запускает подключение. Для join передавать исходный link: из preview ссылка не собирается.
 * Массивы моделей принадлежат получателю, copy() их не клонирует; после создания использовать только для чтения. */
const val INVITE_KIND_CHANNEL: Int = 0
const val INVITE_KIND_SERVER: Int = 1
const val INVITE_PUBKEY_SIZE: Int = 32
const val INVITE_SNAME_MAX: Int = 64

const val INVITE_PROTO_UDP: Int = 0x01
const val INVITE_PROTO_TCP: Int = 0x02
const val INVITE_PROTO_REALITY: Int = 0x04   // флаг reality-камуфляжа поверх TCP (reality-TCP = 0x06)

/** REALITY-снимок; has=true у непустого native-preview, byte-поля сравниваются по содержимому.
 * shortId — 8 байт, serverPubkey — 32 байта, version — 3 байта; serverName — SNI до 63 UTF-8 байт. */
data class InviteReality(
    val has: Boolean,
    val shortId: ByteArray,
    val serverPubkey: ByteArray,
    val version: ByteArray,
    val serverName: String
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is InviteReality) return false
        return has == other.has && shortId.contentEquals(other.shortId) && serverPubkey.contentEquals(other.serverPubkey) &&
            version.contentEquals(other.version) && serverName == other.serverName
    }
    override fun hashCode(): Int {
        var result = has.hashCode()
        result = result * 31 + shortId.contentHashCode()
        result = result * 31 + serverPubkey.contentHashCode()
        result = result * 31 + version.contentHashCode()
        return result * 31 + serverName.hashCode()
    }
}

/** Адрес узла подключения: family=4/6, address — 4/16 сетевых байт; port=1..65535 в порядке байтов хоста.
 * socketId=0..255, proto — UDP=1/TCP=2/TCP+REALITY=6; поля native-preview проверены общим C-кодеком. */
data class InviteAddr(
    val family: Int,       // 4 or 6
    val address: ByteArray,
    val port: Int,
    val socketId: Int,
    val proto: Int         // INVITE_PROTO_*
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is InviteAddr) return false
        return family == other.family && address.contentEquals(other.address) &&
               port == other.port && socketId == other.socketId && proto == other.proto
    }
    override fun hashCode(): Int = family * 31 + address.contentHashCode() + port * 31 + socketId * 31 + proto
}

/** Предпросмотр приглашения; равенство учитывает все поля и исходную ссылку.
 * CHANNEL: ненулевой joinKey; SERVER: joinKey=0, add_key остаётся в link.
 * joinKey хранят все биты uint64 в Long (для unsigned-вывода использовать toULong()).
 * pubkey — 32 байта X25519 узла подключения, addrs — 1..32 его адресов, link — исходная wire-ссылка. */
data class InviteData(
    val joinKey: Long,
    val pubkey: ByteArray,
    val addrs: List<InviteAddr>,
    val reality: InviteReality? = null,
    val link: String = "",
    val kind: Int = INVITE_KIND_CHANNEL
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is InviteData) return false
        return kind == other.kind && joinKey == other.joinKey && pubkey.contentEquals(other.pubkey) &&
            addrs == other.addrs && reality == other.reality && link == other.link
    }
    override fun hashCode(): Int {
        var result = kind.hashCode()
        result = 31 * result + kind
        result = result * 31 + joinKey.hashCode()
        result = result * 31 + pubkey.contentHashCode()
        result = result * 31 + addrs.hashCode()
        result = result * 31 + (reality?.hashCode() ?: 0)
        return result * 31 + link.hashCode()
    }
}

object InviteLink {
    /** Возвращает собственный preview или null при отказе C-кодека/повреждённом JSON; ошибок подключения здесь нет.
     * Загруженная libutun обязательна. JNI-ответ содержит секретный join_key. */
    fun decode(link: String): InviteData? {
        val encoded = NativeLib.decodeInvite(link) ?: return null
        return try {
            fromNativePreview(encoded, link)
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "INVITE", "invalid native invitation preview: ${e.javaClass.simpleName}"); null
        }
    }

    /** Преобразует проверенный C-кодеком JSON JNI без повторного разбора wire; ошибки полей передаёт decode.
     * key/node — десятичные строки uint64, остальные byte-поля — hex. Числовой JSON/Double для ID не допускается. */
    internal fun fromNativePreview(encoded: String, link: String): InviteData {
        fun bytes(hex: String): ByteArray = hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
        val value = org.json.JSONObject(encoded)
        val joinKey = (value.get("key") as String).toULong().toLong()
        val addresses = value.getJSONArray("addresses")
        val reality = value.optJSONObject("reality")?.let {
            InviteReality(true, bytes(it.getString("short")), bytes(it.getString("pubkey")),
                bytes(it.getString("version")), String(bytes(it.getString("name")), Charsets.UTF_8))
        }
        return InviteData(joinKey, bytes(value.getString("pubkey")),
            (0 until addresses.length()).map { i ->
                val a = addresses.getJSONObject(i)
                InviteAddr(a.getInt("family"), bytes(a.getString("ip")), a.getInt("port"), a.getInt("socket"), a.getInt("proto"))
            }, reality, link, value.getInt("kind"))
    }

    /** Диагностическая подпись адреса/сокета; IPv6 выводится hex без сжатия, строка не является endpoint для подключения. */
    fun addrToString(addr: InviteAddr): String {
        val protoStr = when {
            (addr.proto and INVITE_PROTO_REALITY) != 0 -> "TCP+REALITY"
            addr.proto == INVITE_PROTO_UDP -> "UDP"
            addr.proto == INVITE_PROTO_TCP -> "TCP"
            else -> "?${addr.proto}"
        }
        val ip = if (addr.family == 4 && addr.address.size == 4) {
            val b = addr.address
            "${b[0].toInt() and 0xFF}.${b[1].toInt() and 0xFF}.${b[2].toInt() and 0xFF}.${b[3].toInt() and 0xFF}:${addr.port}"
        } else {
            "ipv6:${addr.address.joinToString("") { "%02x".format(it) }}:${addr.port}"
        }
        return "$protoStr $ip sock=${addr.socketId}"
    }
}
