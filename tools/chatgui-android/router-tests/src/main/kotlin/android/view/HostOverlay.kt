package android.view

import android.content.Context

/** Модель оконного IPC: add/remove могут отказать до или после эффекта; callbacks доставляются вручную. */
class WindowManager {
    class LayoutParams(var width: Int, var height: Int, val type: Int, val flags: Int, val format: Int) {
        companion object {
            const val WRAP_CONTENT = -2; const val TYPE_APPLICATION_OVERLAY = 2038; const val TYPE_PHONE = 2002
            const val FLAG_NOT_FOCUSABLE = 8
        }
        var gravity = 0; var x = 0; var y = 0
    }
    val windows = mutableMapOf<View, LayoutParams>()
    var failures = emptySet<String>()
    var additions = 0; var removals = 0; var updates = 0
    fun addView(view: View, params: LayoutParams) {
        additions++
        check("add" !in failures) { "window add unavailable" }
        windows[view] = params; view.layout = params
        check("add after" !in failures) { "window add failed after effect" }
    }
    fun removeView(view: View) {
        removals++
        check("remove" !in failures) { "window remove unavailable" }
        require(windows.remove(view) != null) { "window not attached" }
        check("remove after" !in failures) { "window remove failed after effect" }
    }
    fun updateViewLayout(view: View, params: LayoutParams) {
        updates++
        check("update" !in failures) { "window layout unavailable" }
        require(windows.containsKey(view)) { "window not attached" }
        windows[view] = params; view.layout = params
    }
}
open class View {
    fun interface OnTouchListener { fun onTouch(view: View, event: MotionEvent): Boolean }
    var background: Any? = null
    var layout: WindowManager.LayoutParams? = null
    var isLaidOut = true
    val width get() = layout?.width ?: 0
    val height get() = layout?.height ?: 0
    var listener: OnTouchListener? = null
    val animations = mutableListOf<Runnable>()
    val layouts = mutableListOf<() -> Unit>()
    fun setOnTouchListener(value: OnTouchListener) { listener = value }
    fun touch(event: MotionEvent) = listener?.onTouch(this, event) ?: false
    fun postOnAnimation(task: Runnable) { animations.add(task) }
    fun dispatchAnimations() { val pending = animations.toList(); animations.clear(); pending.forEach { it.run() } }
    fun getLocationOnScreen(out: IntArray) { out[0] = layout?.x ?: 0; out[1] = layout?.y ?: 0 }
}
class MotionEvent(val action: Int, val rawX: Float = 0f, val rawY: Float = 0f, val eventTime: Long = 1,
                  val downTime: Long = eventTime) {
    companion object { const val ACTION_DOWN = 0; const val ACTION_UP = 1; const val ACTION_MOVE = 2; const val ACTION_CANCEL = 3 }
}
object Gravity { const val CENTER = 17; const val TOP = 48; const val LEFT = 3 }
class ViewConfiguration {
    val scaledTouchSlop = 8
    companion object { fun get(context: Context) = ViewConfiguration(); fun getDoubleTapTimeout() = 300 }
}
class LayoutInflater
