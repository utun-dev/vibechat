package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.CountDownLatch
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit

class MicrophoneCaptureTest {
    private class Device : MicrophoneCapture.Device {
        override val channels = 2
        override val inputId = 7
        val input = LinkedBlockingQueue<MicrophoneCapture.Frame>()
        val reads = AtomicInteger()
        var closes = 0
        override fun read(): MicrophoneCapture.Frame = input.take().also { reads.incrementAndGet() }
        override fun stop() { input.offer(MicrophoneCapture.Frame(shortArrayOf(), 0, 0)) }
        override fun close() { closes++ }
    }
    private fun frame(subscription: MicrophoneCapture.Subscription): MicrophoneCapture.Frame {
        repeat(50) { (subscription.poll() as? MicrophoneCapture.Frame)?.let { return it } }
        error("Capture frame timeout")
    }

    @Test fun consumersShareCaptureFormatAndTimeButOwnTheirSamples() {
        val device = Device(); val capture = MicrophoneCapture(device, { _, _ -> })
        val stream = capture.subscribe("stream"); val radio = capture.subscribe("radio")
        try {
            device.input.add(MicrophoneCapture.Frame(shortArrayOf(10, 20), 987654321, 960))
            val first = frame(stream); val second = frame(radio)
            assertEquals(2, radio.channels); assertEquals(first.timeNs, second.timeNs); assertEquals(first.sequence, second.sequence)
            first.pcm[0] = 99
            assertArrayEquals(shortArrayOf(10, 20), second.pcm)
            stream.close()
            device.input.add(MicrophoneCapture.Frame(shortArrayOf(30, 40), 987674321, 1920))
            assertArrayEquals(shortArrayOf(30, 40), frame(radio).pcm); assertEquals(0, device.closes)
        } finally { stream.close(); radio.close() }
        assertEquals(1, device.closes); assertTrue(capture.closed)
    }

    @Test fun stoppingRadioKeepsStreamAndPttIsOrderedWithPcm() {
        val device = Device(); val capture = MicrophoneCapture(device, { _, _ -> })
        val stream = capture.subscribe("stream"); val radio = capture.subscribe("radio")
        try {
            assertTrue(radio.ptt(true))
            device.input.add(MicrophoneCapture.Frame(shortArrayOf(1, 2), 1000, 0))
            assertEquals(MicrophoneCapture.Ptt(true), radio.poll())
            assertArrayEquals(shortArrayOf(1, 2), frame(radio).pcm)
            assertTrue(radio.ptt(false)); assertEquals(MicrophoneCapture.Ptt(false), radio.poll())
            radio.close(); assertEquals(0, device.closes)
            assertArrayEquals(shortArrayOf(1, 2), frame(stream).pcm)
        } finally { radio.close(); stream.close() }
        assertEquals(1, device.closes)
    }

    @Test fun slowConsumerHasBoundedQueueAndReservedControlCapacity() {
        val device = Device(); val logs = LinkedBlockingQueue<String>()
        val capture = MicrophoneCapture(device, { _, message -> logs.add(message) })
        val stream = capture.subscribe("stream"); val radio = capture.subscribe("radio")
        try {
            repeat(40) {
                device.input.add(MicrophoneCapture.Frame(shortArrayOf(1, 2), it * 20000000L, it * 960L))
                frame(stream) // Трансляция успевает, рация намеренно не читает.
            }
            assertTrue(logs.any { it.contains("consumer=radio dropped=") })
            assertTrue(radio.ptt(true)); assertTrue(radio.ptt(false))
            repeat(24) { assertTrue(radio.poll() is MicrophoneCapture.Frame) }
            assertEquals(MicrophoneCapture.Ptt(true), radio.poll()); assertEquals(MicrophoneCapture.Ptt(false), radio.poll())
        } finally { radio.close(); stream.close() }
    }

    private class FaultyDevice : MicrophoneCapture.Device {
        override val channels = 2
        override val inputId = 7
        val input = LinkedBlockingQueue<Any>()
        val entered = CountDownLatch(1)
        val stopped = CountDownLatch(1)
        val stops = AtomicInteger()
        val closes = AtomicInteger()
        var failStop = false
        var failClose = false
        var onRead: () -> Unit = {}
        override fun read(): MicrophoneCapture.Frame {
            entered.countDown()
            val event = input.take()
            onRead()
            if (event is Exception) throw event
            return event as MicrophoneCapture.Frame
        }
        override fun stop() {
            stops.incrementAndGet(); stopped.countDown()
            check(!failStop) { "device stop denied" }
            input.offer(MicrophoneCapture.Frame(shortArrayOf(), 0, 0))
        }
        override fun close() { closes.incrementAndGet(); check(!failClose) { "device close denied" } }
    }

    private fun diagnostic(logs: LinkedBlockingQueue<String>, fragment: String) = assertTrue("Missing $fragment", logs.any { it.contains(fragment) })

    @Test fun readFailureClosesCaptureRejectsSubscribeAndDetachedPollIsEmpty() {
        val device = FaultyDevice(); val failed = CountDownLatch(1)
        val capture = MicrophoneCapture(device, { _, text -> if (text.startsWith("capture reader failed")) failed.countDown() })
        val sub = capture.subscribe("radio")
        try {
            device.input.offer(IllegalStateException("read failed"))
            assertTrue(failed.await(3, TimeUnit.SECONDS)); assertTrue(capture.closed)
            assertThrows(IllegalStateException::class.java) { sub.poll() }
            assertThrows(IllegalStateException::class.java) { capture.subscribe("new") }
        } finally { sub.close() }
        assertNull(sub.poll()); assertEquals(1, device.closes.get())
    }

    @Test fun changedAndReleasedObserverFailuresDoNotLeakLastDevice() {
        val device = FaultyDevice(); val logs = LinkedBlockingQueue<String>(); val releases = AtomicInteger()
        val capture = MicrophoneCapture(device, { _, text -> logs.add(text) },
            released = { releases.incrementAndGet(); error("release observer failed") }, changed = { error("change observer failed") })
        val sub = capture.subscribe("radio")
        repeat(3) { sub.close() }
        assertEquals(1, device.stops.get()); assertEquals(1, device.closes.get()); assertEquals(1, releases.get())
        diagnostic(logs, "changed observer failed"); diagnostic(logs, "released observer failed")
    }

    @Test fun interruptedCallerFinishesCleanupAndRetainsInterruptFlag() {
        val device = FaultyDevice(); val releases = AtomicInteger()
        val capture = MicrophoneCapture(device, { _, _ -> }, released = { releases.incrementAndGet() })
        val sub = capture.subscribe("radio")
        assertTrue(device.entered.await(3, TimeUnit.SECONDS))
        val result = FutureTask {
            Thread.currentThread().interrupt()
            sub.close()
            Thread.currentThread().isInterrupted
        }
        Thread(result, "interrupted-capture-owner").apply { isDaemon = true; start() }
        assertTrue(result.get(3, TimeUnit.SECONDS))
        assertEquals(1, device.closes.get()); assertEquals(1, releases.get())
    }

    @Test fun stopAndCloseFailureStillJoinReaderAndNotifyReleaseOnce() {
        val device = FaultyDevice().apply { failStop = true; failClose = true }
        val logs = LinkedBlockingQueue<String>(); val releases = AtomicInteger()
        val capture = MicrophoneCapture(device, { _, text -> logs.add(text) }, released = { releases.incrementAndGet() })
        val sub = capture.subscribe("radio")
        assertTrue(device.entered.await(3, TimeUnit.SECONDS))
        sub.close()
        assertEquals(1, device.closes.get()); assertEquals(1, releases.get())
        diagnostic(logs, "capture stop failed"); diagnostic(logs, "capture release failed")
    }

    @Test fun workerStartupFailureRollsBackWithoutReturningOrphanSubscription() {
        for (factoryFailure in listOf(false, true)) {
            val device = FaultyDevice(); val releases = AtomicInteger()
            val capture = MicrophoneCapture(device, { _, _ -> }, released = { releases.incrementAndGet() }, threadFactory = { task ->
                if (factoryFailure) error("thread factory denied")
                object : Thread(task) { override fun start() { error("thread start denied") } }
            })
            assertThrows(IllegalStateException::class.java) { capture.subscribe("radio") }
            assertTrue(capture.closed); assertTrue(capture.names.isEmpty())
            assertEquals(1, device.closes.get()); assertEquals(1, releases.get())
        }
    }

    @Test fun alreadyRunningFactoryResultCannotJoinOrInterruptCaller() {
        val device = FaultyDevice()
        val capture = MicrophoneCapture(device, { _, _ -> }, threadFactory = { Thread.currentThread() })
        assertThrows(IllegalStateException::class.java) { capture.subscribe("radio") }
        assertFalse(Thread.currentThread().isInterrupted)
        assertEquals(1, device.closes.get())
    }

    @Test fun readerCannotCloseAndJoinItself() {
        val device = FaultyDevice(); val failed = CountDownLatch(1); val logs = LinkedBlockingQueue<String>()
        val capture = MicrophoneCapture(device, { _, text -> logs.add(text); if (text.startsWith("capture reader failed")) failed.countDown() })
        val sub = capture.subscribe("radio")
        device.onRead = { sub.close() }
        try {
            device.input.offer(MicrophoneCapture.Frame(shortArrayOf(1, 2), 1, 0))
            assertTrue(failed.await(3, TimeUnit.SECONDS))
            assertTrue(capture.closed)
            diagnostic(logs, "close from reader rejected")
        } finally { device.onRead = {}; sub.close() }
        assertEquals(1, device.closes.get())
    }

    @Test fun controlCapacityReportsOverflowAndClosedPttIsRejected() {
        val device = FaultyDevice(); val logs = LinkedBlockingQueue<String>()
        val capture = MicrophoneCapture(device, { _, text -> logs.add(text) })
        val sub = capture.subscribe("radio")
        try {
            repeat(32) { assertTrue(sub.ptt(it % 2 == 0)) }
            assertFalse(sub.ptt(true)); diagnostic(logs, "control queue full")
        } finally { sub.close() }
        assertFalse(sub.ptt(true)); assertNull(sub.poll())
    }

    @Test fun observersExecuteOutsideCaptureMonitor() {
        val device = FaultyDevice(); val locks = LinkedBlockingQueue<Boolean>()
        lateinit var capture: MicrophoneCapture
        capture = MicrophoneCapture(device, { _, _ -> }, released = { locks.add(Thread.holdsLock(capture)) },
            changed = { locks.add(Thread.holdsLock(capture)) })
        capture.subscribe("radio").close()
        assertEquals(3, locks.size); assertTrue(locks.none { it })
    }

    @Test fun concurrentLastClosesReleaseDeviceExactlyOnce() {
        repeat(32) {
            val device = FaultyDevice(); val releases = AtomicInteger()
            val capture = MicrophoneCapture(device, { _, _ -> }, released = { releases.incrementAndGet() })
            val one = capture.subscribe("radio"); val two = capture.subscribe("stream")
            val go = CountDownLatch(1)
            val workers = listOf(one, two).map { sub -> FutureTask { go.await(); sub.close() }.also { task ->
                Thread(task).apply { isDaemon = true; start() }
            } }
            go.countDown(); workers.forEach { it.get(3, TimeUnit.SECONDS) }
            assertEquals(1, device.closes.get()); assertEquals(1, releases.get())
        }
    }
}
