package androidx.core.view

import android.view.View

fun View.doOnLayout(action: (View) -> Unit) {
    if (isLaidOut) action(this) else layouts.add { action(this) }
}
