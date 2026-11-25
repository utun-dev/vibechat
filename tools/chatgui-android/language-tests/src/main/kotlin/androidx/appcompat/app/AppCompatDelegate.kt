package androidx.appcompat.app

import androidx.core.os.LocaleListCompat

/** Host AppCompat: отказ до/после публикации requested моделируется отдельно; завершение конфигурации не имитируется. */
object AppCompatDelegate {
    var requested = LocaleListCompat.forLanguageTags("")
    var failRead = false
    var failWrite = false
    var failWriteAfter = false
    val selections = mutableListOf<String>()
    fun getApplicationLocales(): LocaleListCompat {
        check(!failRead) { "application locales read failed" }
        return requested
    }
    fun setApplicationLocales(locales: LocaleListCompat) {
        check(!failWrite) { "application locales write failed" }
        selections.add(locales.toLanguageTags()); requested = locales
        check(!failWriteAfter) { "application locales write failed after effect" }
    }
}
