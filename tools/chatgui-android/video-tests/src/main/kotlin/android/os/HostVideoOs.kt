package android.os

import java.util.concurrent.ConcurrentLinkedQueue

/** Только JVM: main очередь и таймеры исполняет тест, worker остаётся настоящим Thread. */
class Looper internal constructor(private val threaded: Boolean = false) {
    private data class Task(val handler: Handler, val action: Runnable, val deadline: Long)
    private val tasks = ConcurrentLinkedQueue<Task>()
    @Volatile private var now = 0L
    @Volatile private var quitting = false
    private val monitor = Object()
    private fun time(): Long = if (threaded) System.nanoTime() / 1_000_000 else now
    internal fun post(handler: Handler, action: Runnable, delay: Long): Boolean {
        synchronized(monitor) {
            if (quitting) return false
            tasks.add(Task(handler, action, time() + delay)); monitor.notifyAll(); return true
        }
    }
    internal fun remove(handler: Handler, action: Runnable) { tasks.removeIf { it.handler === handler && it.action === action } }
    fun advanceBy(delay: Long) {
        now += delay
        while (true) {
            val next = tasks.filter { it.deadline <= now }.minByOrNull { it.deadline } ?: return
            if (tasks.remove(next)) next.action.run()
        }
    }
    internal fun loop() {
        current.set(this)
        while (true) {
            val next = synchronized(monitor) {
                val ready = tasks.filter { it.deadline <= time() }.minByOrNull { it.deadline }
                if (ready == null && quitting) return
                if (ready == null) monitor.wait(2)
                ready
            }
            if (next != null && tasks.remove(next)) next.action.run()
        }
    }
    internal fun quit(): Boolean = synchronized(monitor) { quitting = true; monitor.notifyAll(); true }
    companion object {
        private val main = Looper()
        private val mainThread = Thread.currentThread()
        private val current = ThreadLocal<Looper>()
        fun getMainLooper() = main
        fun myLooper(): Looper? = if (Thread.currentThread() === mainThread) main else current.get()
    }
}
class Handler(private val looper: Looper) {
    companion object { var rejectDelayed = false; var rejectImmediate = false }
    fun post(action: Runnable): Boolean = !rejectImmediate && looper.post(this, action, 0)
    fun postDelayed(action: Runnable, delay: Long): Boolean = !rejectDelayed && looper.post(this, action, delay)
    fun removeCallbacks(action: Runnable) = looper.remove(this, action)
}

class HandlerThread(name: String) : Thread(name) {
    val looper = Looper(true)
    init { isDaemon = true }
    override fun run() { looper.loop() }
    fun quitSafely() = looper.quit()
}
object SystemClock {
    fun elapsedRealtime(): Long = System.nanoTime() / 1_000_000
    fun elapsedRealtimeNanos(): Long = System.nanoTime()
}
object Build { object VERSION { var SDK_INT = 36 }; object VERSION_CODES { const val UPSIDE_DOWN_CAKE = 34 } }
object Process { const val THREAD_PRIORITY_AUDIO = -16; fun setThreadPriority(value: Int) {} }
class Bundle {
    val values = mutableMapOf<String, Any>()
    var failReads = false
    fun putInt(key: String, value: Int) { values[key] = value }
    fun putLong(key: String, value: Long) { values[key] = value }
    fun putString(key: String, value: String) { values[key] = value }
    fun putBundle(key: String, value: Bundle) { values[key] = value }
    fun putParcelable(key: String, value: Any) { values[key] = value }
    fun getLong(key: String, fallback: Long): Long { check(!failReads) { "Bundle decode failed" }; return values[key] as? Long ?: fallback }
    fun getString(key: String): String? { check(!failReads) { "Bundle decode failed" }; return values[key] as? String }
    fun keySet(): Set<String> { check(!failReads) { "Bundle decode failed" }; return values.keys }
}
data class ParcelUuid(val uuid: java.util.UUID)
interface OutcomeReceiver<R, E : Throwable> { fun onResult(result: R?); fun onError(error: E) }
