package android.media

import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingQueue

object AudioManager { const val STREAM_MUSIC = 3 }

/** PCM записывается только в пределах принятой части; аппаратные ошибки задаются через Plan. */
class AudioTrack(stream: Int, rate: Int, mask: Int, encoding: Int, bytes: Int, mode: Int) {
    class Plan {
        var initialized = true
        @Volatile var failures = emptySet<String>()
        val results = LinkedBlockingQueue<Int>()
        val accepted = CopyOnWriteArrayList<Short>()
        var releaseCalls = 0
        @Volatile var released = false
    }
    companion object {
        const val MODE_STREAM = 1; const val STATE_INITIALIZED = 1
        val plans = LinkedBlockingQueue<Plan>()
        val created = CopyOnWriteArrayList<AudioTrack>()
        var minimum = 1920
        fun getMinBufferSize(rate: Int, mask: Int, encoding: Int) = minimum
        fun reset() { plans.clear(); created.clear(); minimum = 1920 }
    }
    val plan = requireNotNull(plans.poll()) { "missing track plan" }
    init { created.add(this) }
    val state get() = if (plan.initialized) STATE_INITIALIZED else 0
    fun play() { check("play" !in plan.failures) { "track play failed" } }
    fun write(pcm: ShortArray, offset: Int, count: Int): Int {
        val result = plan.results.poll() ?: count
        if (result > 0) plan.accepted.addAll(pcm.copyOfRange(offset, offset + result).toList())
        return result
    }
    fun stop() { check("stop" !in plan.failures) { "track stop failed" } }
    fun release() {
        plan.releaseCalls++; check("release" !in plan.failures) { "track release failed" }; plan.released = true
        check("release after" !in plan.failures) { "track release failed after effect" }
    }
}
