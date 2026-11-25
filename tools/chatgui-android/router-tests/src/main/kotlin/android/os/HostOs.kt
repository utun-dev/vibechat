package android.os

import java.util.concurrent.ConcurrentLinkedQueue
import android.media.AudioAttributes

/** Host-модель SDK/главной очереди: callbacks и их доставку тест контролирует явно, без настоящего Android Looper. */
object Build {
    object VERSION { var SDK_INT = 36 }
    object VERSION_CODES {
        const val M = 23; const val O = 26; const val P = 28; const val Q = 29; const val R = 30; const val S = 31
        const val TIRAMISU = 33; const val UPSIDE_DOWN_CAKE = 34
    }
}
interface IBinder
class Looper private constructor() {
    private data class Task(val handler: Handler, val runnable: Runnable, val deadline: Long)
    private val tasks = ConcurrentLinkedQueue<Task>()
    @Volatile private var time = 0L
    internal fun post(handler: Handler, task: Runnable, delay: Long): Boolean {
        tasks.add(Task(handler, task, time + delay)); return true
    }
    internal fun remove(handler: Handler, task: Runnable) { tasks.removeIf { it.handler === handler && it.runnable === task } }
    fun advanceBy(milliseconds: Long) { check(milliseconds >= 0); time += milliseconds; dispatchAll() }
    fun dispatchAll() {
        check(myLooper() === this)
        while (true) {
            val next = tasks.filter { it.deadline <= time }.minByOrNull { it.deadline } ?: return
            if (tasks.remove(next)) next.runnable.run()
        }
    }
    companion object {
        private val main = Looper()
        private val owner = Thread.currentThread()
        fun getMainLooper() = main
        fun myLooper(): Looper? = if (Thread.currentThread() === owner) main else null
    }
}
class Handler(val looper: Looper) {
    companion object { var rejectDelayed = false; var rejectImmediate = false }
    fun post(task: Runnable): Boolean = !rejectImmediate && looper.post(this, task, 0)
    fun postDelayed(task: Runnable, delay: Long): Boolean = !rejectDelayed && looper.post(this, task, delay)
    fun removeCallbacks(task: Runnable) = looper.remove(this, task)
}
object SystemClock {
    fun elapsedRealtime(): Long = System.nanoTime() / 1_000_000
    fun elapsedRealtimeNanos(): Long = System.nanoTime()
}

object Process {
    const val THREAD_PRIORITY_AUDIO = -16
    fun setThreadPriority(priority: Int) { check(priority == THREAD_PRIORITY_AUDIO) }
}

/** Конечные vibration pulses без аппаратных эффектов; тест может отказать на любом импульсе. */
class VibrationEffect private constructor(val timings: LongArray, val repeat: Int) {
    companion object { fun createWaveform(timings: LongArray, repeat: Int) = VibrationEffect(timings.copyOf(), repeat) }
}
class Vibrator {
    var available = true
    var failQuery = false
    var failPulse = false
    var failPulseAfter = false
    var failCancel = false
    var failCancelAfter = false
    var active = false
        private set
    var pulseAttempts = 0
    var cancelCalls = 0
    val effects = mutableListOf<Pair<VibrationEffect, AudioAttributes?>>()
    fun hasVibrator(): Boolean { check(!failQuery) { "vibrator query unavailable" }; return available }
    fun vibrate(effect: VibrationEffect) = vibrate(effect, null)
    fun vibrate(effect: VibrationEffect, attributes: AudioAttributes?) {
        pulseAttempts++
        check(!failPulse) { "vibration denied" }
        effects.add(effect to attributes)
        active = true
        check(!failPulseAfter) { "vibration failed after effect" }
    }
    fun cancel() {
        cancelCalls++; check(!failCancel) { "cancel denied" }
        active = false; check(!failCancelAfter) { "cancel failed after effect" }
    }
}
