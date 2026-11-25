package androidx.core.content

import android.content.Context

/** Host-граница локализованного Context: возвращаем effective ресурсы, не имитируем хранение/применение locale Android. */
object ContextCompat {
    var failContext = false
    fun getContextForLanguage(context: Context): Context {
        check(!failContext) { "language context unavailable" }
        return context
    }
}
