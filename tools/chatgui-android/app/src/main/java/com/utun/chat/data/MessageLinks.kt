package com.utun.chat.data

import java.net.URI
import java.net.URL

/** Диапазон [start, end) в UTF-16 исходного сообщения и полный адрес без внешней пунктуации. */
data class MessageLink(val start: Int, val end: Int, val url: String)

/** Чистый разбор для подсветки: HTTP/HTTPS без учёта регистра и utun://; сеть и JNI не вызываются. */
object MessageLinks {
    private val pattern = Regex("(?:utun://|(?i:https?://))[^\\s\\p{Z}\\p{Cc}<>\"'`«»“”‘’]+")
    private val invalidEscape = Regex("%(?![0-9A-Fa-f]{2})")

    /** Проверяет полный HTTP/HTTPS URL; порт пустой либо десятичный 0..65535. Некорректный текст — обычный ввод, без логов. */
    fun isWebUrl(url: String): Boolean {
        if (!pattern.matches(url) || invalidEscape.containsMatchIn(url)) return false
        return try {
            val parsed = URL(url)
            if ((parsed.protocol != "http" && parsed.protocol != "https") || parsed.host.isEmpty() || parsed.port !in -1..65535) return false
            // URL допускает -1/+80 и недопустимые символы host; URI проверяет только authority, не ограничивая Unicode/[] в пути.
            val authority = URI("//${parsed.authority}").rawAuthority?.substringAfterLast('@') ?: return false
            val port = if (authority.startsWith("[")) authority.substringAfter(']').substringAfter(':', "")
                else authority.substringAfter(':', "")
            port.all { it in '0'..'9' }
        } catch (_: java.net.MalformedURLException) { false } catch (_: java.net.URISyntaxException) { false }
    }

    /** Возвращает непересекающиеся диапазоны по порядку; отделение хвоста занимает O(n), адрес копируется один раз. */
    fun parse(text: String): List<MessageLink> = pattern.findAll(text).mapNotNull { match ->
        var url = match.value
        run {
            val balance = IntArray(3)
            for (char in url) {
                val opening = "([{".indexOf(char)
                val closing = ")]}".indexOf(char)
                if (opening >= 0) balance[opening]--
                if (closing >= 0) balance[closing]++
            }
            var end = url.length
            while (end > 0) {
                val last = url[end - 1]
                val closing = ")]}".indexOf(last)
                val unmatched = closing >= 0 && balance[closing] > 0
                if (last !in ".,;:!?" && !unmatched) break
                if (closing >= 0) balance[closing]--
                end--
            }
            url = url.substring(0, end)
            if (!url.startsWith("utun://") && !isWebUrl(url)) return@mapNotNull null
        }
        MessageLink(match.range.first, match.range.first + url.length, url)
    }.toList()
}
