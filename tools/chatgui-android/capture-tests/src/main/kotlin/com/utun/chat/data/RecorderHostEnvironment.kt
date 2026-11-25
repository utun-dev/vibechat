package com.utun.chat.data

import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicInteger

/** Native singleton и decoder handles заменены только управляемой границей; recorder работает настоящий. */
object NativeLib {
    @Volatile var active = false
    var acceptStart = true
    var acceptFeed = true
    var onFeed: () -> Unit = {}
    var stopResult = 800
    val starts = CopyOnWriteArrayList<List<Any>>()
    val feeds = CopyOnWriteArrayList<ShortArray>()
    val stops = AtomicInteger()
    val cancels = AtomicInteger()
    val decoderReads = LinkedBlockingQueue<Int>()
    val decoderCloses = AtomicInteger()
    @Volatile var failDecoderClose = false
    @Volatile var failCancel = false
    var decoderInfo = intArrayOf(48000, 1, 960)
    fun reset() {
        active = false; acceptStart = true; acceptFeed = true; stopResult = 800; onFeed = {}
        starts.clear(); feeds.clear(); stops.set(0); cancels.set(0)
        decoderReads.clear(); decoderCloses.set(0); decoderInfo = intArrayOf(48000, 1, 960)
        failDecoderClose = false; failCancel = false
    }
    @Synchronized fun voiceStart(channel: String, reply: String = "", request: Long = 0, generation: Long = 0, quote: String = ""): Boolean {
        starts.add(listOf(channel, reply, request, generation))
        if (!acceptStart || active) return false
        active = true; return true
    }
    fun voiceFeed(pcm: ShortArray): Boolean { feeds.add(pcm.copyOf()); onFeed(); return active && acceptFeed }
    @Synchronized fun voiceStop(): Int { stops.incrementAndGet(); active = false; return stopResult }
    @Synchronized fun voiceCancel() { check(!failCancel) { "native cancel rejected" }; cancels.incrementAndGet(); active = false }
    fun voiceDecodeOpen(path: String, info: IntArray): Long { decoderInfo.copyInto(info); return 1 }
    fun voiceDecodeRead(handle: Long, pcm: ShortArray): Int {
        val count = decoderReads.poll() ?: 0
        if (count > 0) pcm.fill(12, 0, count)
        return count
    }
    fun voiceDecodeClose(handle: Long) { check(!failDecoderClose) { "decoder close rejected" }; decoderCloses.incrementAndGet() }
}

object StreamController { var blocked = false; fun blocksCapture() = blocked }
