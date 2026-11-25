package androidx.core.os

/** Host-список запрошенных/effective языков; хранение списка и применение конфигурации в тесте разделены. */
class LocaleListCompat private constructor(private val tags: String) {
    fun toLanguageTags() = tags
    companion object { fun forLanguageTags(tags: String) = LocaleListCompat(tags) }
}
