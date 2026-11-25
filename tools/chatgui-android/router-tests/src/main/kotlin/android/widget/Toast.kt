package android.widget

import android.content.Context

/** JVM не отображает UI; текст сохраняется для проверки callback запроса разрешения. */
class Toast private constructor(val text: String) {
    companion object {
        const val LENGTH_LONG = 1
        const val LENGTH_SHORT = 0
        val shown = mutableListOf<String>()
        fun makeText(context: Context, text: String, duration: Int) = Toast(text)
        fun makeText(context: Context, resource: Int, duration: Int) = Toast("resource:$resource")
    }
    fun show() { shown.add(text) }
}
