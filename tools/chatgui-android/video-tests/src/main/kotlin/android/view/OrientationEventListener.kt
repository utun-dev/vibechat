package android.view

import android.content.Context
import java.util.concurrent.CopyOnWriteArrayList

abstract class OrientationEventListener(context: Context) {
    companion object { val created = CopyOnWriteArrayList<OrientationEventListener>(); var failures = emptySet<String>() }
    var enabled = false
        private set
    init { created.add(this) }
    abstract fun onOrientationChanged(angle: Int)
    fun canDetectOrientation(): Boolean { check("query" !in failures); return true }
    fun enable() { check("enable" !in failures); enabled = true; check("enable after" !in failures) }
    fun disable() { check("disable" !in failures); enabled = false; check("disable after" !in failures) }
}
