package android.os

/** Только JVM-тесты: управляемая очередь настоящего BleRadio, без фонового потока и физических таймеров. */
class Looper {
    private val immediate = ArrayDeque<Runnable>()
    private val delayed = mutableListOf<Runnable>()
    fun post(task: Runnable, delay: Long): Boolean {
        if (delay == 0L) immediate.addLast(task) else delayed.add(task)
        return true
    }
    fun remove(task: Runnable) { immediate.removeAll { it === task }; delayed.removeAll { it === task } }
    fun drain() { while (immediate.isNotEmpty()) immediate.removeFirst().run() }
    fun reset() { immediate.clear(); delayed.clear() }
    val delayedCount get() = delayed.size
    companion object { @JvmField val worker = Looper() }
}
class HandlerThread(name: String) : Thread(name) {
    override fun start() {}
    val looper get() = Looper.worker
}
class Handler(val looper: Looper) {
    fun post(task: Runnable) = looper.post(task, 0)
    fun postDelayed(task: Runnable, delay: Long) = looper.post(task, delay)
    fun removeCallbacks(task: Runnable) = looper.remove(task)
}
class Build { class VERSION { companion object { @JvmField var SDK_INT = 36 } } }
class SystemClock { companion object { @JvmStatic fun elapsedRealtime(): Long = 1000 } }
