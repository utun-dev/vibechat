package com.utun.chat.data

import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Один Device reader, независимые очереди потребителей; PCM каждого — своя копия.
 * Последний close останавливает/прерывает reader, дожидается join, закрывает Device и уведомляет released.
 * Device.stop или interrupt обязан разблокировать read. Подписка закрывается вне reader/callback read.
 * closed запрещает новые подписки и после read failure; ресурсы освобождает последний владелец.
 * PTT и PCM упорядочены одной очередью (32 места, минимум 8 зарезервированы для управления).
 * changed/released — быстрые observers вне монитора; их исключения логируются и не прерывают cleanup.
 * threadFactory возвращает новый незапущенный Thread. log должен завершаться быстро и не бросать исключений.
 */
class MicrophoneCapture(private val device: Device, private val log: (String, String) -> Unit, private val released: () -> Unit = {},
                        private val changed: () -> Unit = {},
                        private val threadFactory: (Runnable) -> Thread = { Thread(it, "microphone-capture") }) {
    interface Device {
        val channels: Int
        val inputId: Int?
        fun read(): Frame
        fun stop()
        fun close()
    }
    sealed interface Event
    /** hasCapture=false только для целиком сгенерированной тишины; реальные нулевые сэмплы считаются захватом. */
    data class Frame(val pcm: ShortArray, val timeNs: Long, val sequence: Long, val createdNs: Long = System.nanoTime(),
                     val generation: Long = 0, val hasCapture: Boolean = true) : Event
    data class Ptt(val pressed: Boolean) : Event
    private val consumers = mutableListOf<Subscription>()
    @Volatile private var reading = true
    @Volatile private var failure: Exception? = null
    private var worker: Thread? = null
    val closed: Boolean get() = !reading
    val names: Set<String> get() = synchronized(this) { consumers.map { it.name }.toSet() }
    val inputId: Int? get() = device.inputId
    val channels: Int get() = device.channels

    inner class Subscription internal constructor(val name: String) : AutoCloseable {
        private val events = ArrayBlockingQueue<Event>(32)
        @Volatile private var attached = true
        private var dropped = 0L
        val channels: Int get() = device.channels

        @Synchronized internal fun deliver(frame: Frame) {
            if (!attached) return
            if (events.size >= 24 || !events.offer(frame.copy(pcm = frame.pcm.copyOf()))) {
                dropped++
                if (dropped == 1L || dropped % 50L == 0L) log("WARN", "capture queue overflow consumer=$name dropped=$dropped")
            }
        }
        /** Только управляющий поток; восемь мест зарезервированы под команды. */
        @Synchronized fun ptt(pressed: Boolean): Boolean {
            if (!attached) return false
            if (events.offer(Ptt(pressed))) return true
            log("ERROR", "capture control queue full consumer=$name pressed=$pressed")
            return false
        }
        fun poll(): Event? {
            if (!attached) return null
            failure?.let { throw IllegalStateException("Microphone read failed", it) }
            val event = events.poll(20, TimeUnit.MILLISECONDS)
            return if (attached) event else null
        }
        override fun close() {
            check(Thread.currentThread() !== worker) {
                log("ERROR", "capture subscription close from reader rejected consumer=$name")
                "Capture cannot join its own reader"
            }
            synchronized(this) { if (!attached) return; attached = false; events.clear() }
            detach(this)
        }
    }

    fun subscribe(name: String): Subscription {
        val subscription = Subscription(name)
        var startupFailure: Throwable? = null
        val count = synchronized(this) {
            check(reading && failure == null) {
                log("ERROR", "capture subscription rejected consumer=$name reading=$reading failure=$failure")
                "Microphone is closed or failed"
            }
            consumers.add(subscription)
            if (worker == null) try {
                val next = threadFactory(Runnable(::read))
                check(next.state == Thread.State.NEW) { "Capture requires a fresh unstarted worker" }
                worker = next
                next.start()
            } catch (e: Throwable) {
                failure = if (e is Exception) e else IllegalStateException("Capture worker startup failed", e)
                reading = false; consumers.remove(subscription); startupFailure = e
            }
            consumers.size
        }
        startupFailure?.let {
            log("ERROR", "capture worker startup failed consumer=$name: $it")
            releaseDevice()
            throw it
        }
        log("INFO", "capture attached consumer=$name channels=$channels consumers=$count")
        notifyChanged()
        return subscription
    }

    private fun read() {
        try {
            while (reading) {
                val frame = device.read()
                if (!reading) break
                val targets = synchronized(this) { consumers.toList() }
                for (target in targets) target.deliver(frame)
            }
        } catch (e: Exception) {
            val unexpected = synchronized(this) {
                reading.also { if (it) { failure = e; reading = false } }
            }
            if (unexpected) {
                log("ERROR", "capture reader failed: $e")
                notifyChanged()
            }
        }
    }

    private fun notifyChanged() {
        try { changed() } catch (e: Exception) { log("ERROR", "capture changed observer failed: $e") }
    }

    private fun detach(subscription: Subscription) {
        val last = synchronized(this) {
            consumers.remove(subscription)
            log("INFO", "capture detached consumer=${subscription.name} remaining=${consumers.size}")
            consumers.isEmpty().also { if (it) reading = false }
        }
        notifyChanged()
        if (!last) return
        releaseDevice()
    }

    private fun releaseDevice() {
        var interrupted = Thread.interrupted()
        try {
            try { device.stop() } catch (e: Exception) { log("WARN", "capture stop failed: $e") }
            val thread = worker
            thread?.interrupt()
            while (thread?.isAlive == true) try { thread.join() } catch (e: InterruptedException) {
                interrupted = true
                log("WARN", "capture join interrupted; finishing cleanup")
            }
            try { device.close() } catch (e: Exception) { log("ERROR", "capture release failed: $e") }
            try { released() } catch (e: Exception) { log("ERROR", "capture released observer failed: $e") }
        } finally { if (interrupted) Thread.currentThread().interrupt() }
    }
}
