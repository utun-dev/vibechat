package com.utun.chat.data

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.os.Looper
import com.utun.chat.ChatApplication
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий monitor: debounce в виртуальной main-очереди, реальные worker и контролируемая подготовка конфига. */
class NetworkChangeMonitorLifecycleTest {
    private lateinit var context: Context
    private lateinit var manager: ConnectivityManager
    private val loop get() = Looper.getMainLooper()
    private val provider get() = context.configProvider
    private val blocked = mutableListOf<CountDownLatch>()

    @Before fun prepare() {
        context = Context()
        ChatApplication.instance = context
        manager = requireNotNull(context.connectivityManager)
        NativeLib.ready = true
        NativeLib.acceptSocketUpdate = true
        NativeLib.networkNotifications.set(0)
        NativeLib.socketUpdates.clear()
        LogManager.entries.clear()
    }

    @After fun release() {
        context.connectivityManager = manager
        manager.failRemove = false
        NetworkChangeMonitor.stop()
        blocked.forEach { it.countDown() }
        provider.joinWorkers()
        loop.dispatchAll()
        assertTrue("network callback leaked", manager.callbacks.isEmpty())
        NativeLib.ready = true
        NativeLib.acceptSocketUpdate = true
    }

    private fun awaitUpdates(count: Int) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2)
        while (NativeLib.socketUpdates.size < count && System.nanoTime() < deadline) {
            loop.dispatchAll()
            Thread.sleep(1)
        }
        assertEquals("updates=${NativeLib.socketUpdates}, logs=${LogManager.entries.takeLast(8)}", count, NativeLib.socketUpdates.size)
    }

    private fun request(index: Int = manager.callbackHistory.lastIndex) {
        manager.callbackHistory[index].onAvailable(Network())
        loop.dispatchAll()
        loop.advanceBy(500)
    }

    private fun blockFirst(): Pair<CountDownLatch, CountDownLatch> {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        blocked.add(release)
        provider.onBuild = { index ->
            if (index == 1) {
                entered.countDown()
                check(release.await(2, TimeUnit.SECONDS)) { "test did not release first config worker" }
            }
            "config-$index"
        }
        return entered to release
    }

    @Test fun startAndStopOwnExactlyOneCallback() {
        repeat(3) { assertTrue(NetworkChangeMonitor.start()) }
        assertEquals(1, manager.callbacks.size)
        repeat(3) { NetworkChangeMonitor.stop() }
        assertTrue(manager.callbacks.isEmpty())
    }

    @Test fun burstsAreDebouncedBeforePreparingConfig() {
        assertTrue(NetworkChangeMonitor.start())
        manager.callbackHistory[0].onAvailable(Network())
        loop.dispatchAll()
        loop.advanceBy(250)
        manager.callbackHistory[0].onLinkPropertiesChanged(Network(), LinkProperties())
        loop.dispatchAll()
        loop.advanceBy(250)
        assertEquals(0, provider.builds.get())
        loop.advanceBy(250)
        awaitUpdates(1)
        assertEquals(listOf("config-1"), NativeLib.socketUpdates)
    }

    @Test fun oldPlatformCallbackCannotScheduleWorkAfterRestart() {
        val unexpected = CountDownLatch(1)
        provider.onBuild = { unexpected.countDown(); "unexpected" }
        assertTrue(NetworkChangeMonitor.start())
        NetworkChangeMonitor.stop()
        assertTrue(NetworkChangeMonitor.start())
        request(0)
        assertFalse("retired callback started a worker", unexpected.await(200, TimeUnit.MILLISECONDS))
        assertEquals(0, provider.builds.get())
        assertTrue(NativeLib.socketUpdates.isEmpty())
    }

    @Test fun stoppedWorkerCannotUpdateCore() {
        val (entered, release) = blockFirst()
        assertTrue(NetworkChangeMonitor.start())
        request()
        assertTrue(entered.await(2, TimeUnit.SECONDS))
        NetworkChangeMonitor.stop()
        release.countDown()
        provider.joinWorkers()
        loop.dispatchAll()
        assertTrue(NativeLib.socketUpdates.isEmpty())
        assertEquals(0, NativeLib.networkNotifications.get())
    }

    @Test fun restartWaitsForPreviousWorkerBeforeStartingNext() {
        val (entered, release) = blockFirst()
        val nextEntered = CountDownLatch(1)
        val original = provider.onBuild
        provider.onBuild = { index -> if (index == 2) nextEntered.countDown(); original(index) }
        assertTrue(NetworkChangeMonitor.start())
        request()
        assertTrue(entered.await(2, TimeUnit.SECONDS))
        NetworkChangeMonitor.stop()
        assertTrue(NetworkChangeMonitor.start())
        request()
        assertFalse("overlapping config workers", nextEntered.await(200, TimeUnit.MILLISECONDS))
        release.countDown()
        provider.joinWorkers()
        loop.dispatchAll()
        assertTrue(NativeLib.socketUpdates.isEmpty())
        loop.advanceBy(500)
        awaitUpdates(1)
        assertEquals(listOf("config-2"), NativeLib.socketUpdates)
        assertEquals(1, provider.maximumConcurrentBuilds.get())
    }

    @Test fun coreStartedInvalidatesSnapshotAlreadyBeingPrepared() {
        val (entered, release) = blockFirst()
        assertTrue(NetworkChangeMonitor.start())
        request()
        assertTrue(entered.await(2, TimeUnit.SECONDS))
        NetworkChangeMonitor.onCoreStarted()
        release.countDown()
        provider.joinWorkers()
        loop.dispatchAll()
        assertTrue("previous core snapshot was posted", NativeLib.socketUpdates.isEmpty())
        loop.advanceBy(500)
        awaitUpdates(1)
        assertEquals(listOf("config-2"), NativeLib.socketUpdates)
    }

    @Test fun eventsDuringBuildProduceOneAdditionalSnapshot() {
        val (entered, release) = blockFirst()
        assertTrue(NetworkChangeMonitor.start())
        request()
        assertTrue(entered.await(2, TimeUnit.SECONDS))
        repeat(4) { request() }
        release.countDown()
        provider.joinWorkers()
        loop.dispatchAll()
        assertEquals(listOf("config-1"), NativeLib.socketUpdates)
        loop.advanceBy(500)
        awaitUpdates(2)
        assertEquals(listOf("config-1", "config-2"), NativeLib.socketUpdates)
        assertEquals(1, provider.maximumConcurrentBuilds.get())
    }

    @Test fun coreNotReadyWaitsForStartedEvent() {
        assertTrue(NetworkChangeMonitor.start())
        NativeLib.ready = false
        request()
        assertEquals(0, provider.builds.get())
        NativeLib.ready = true
        NetworkChangeMonitor.onCoreStarted()
        loop.dispatchAll()
        loop.advanceBy(500)
        awaitUpdates(1)
    }

    @Test fun preparationFailureIsLoggedAndNextEventCanRetry() {
        val entered = CountDownLatch(1)
        provider.onBuild = { index ->
            entered.countDown()
            if (index == 1) throw IOException("fixture config failure") else "config-$index"
        }
        assertTrue(NetworkChangeMonitor.start())
        request()
        assertTrue(entered.await(2, TimeUnit.SECONDS))
        provider.joinWorkers()
        loop.dispatchAll()
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.message.contains("config") })
        assertTrue(NativeLib.socketUpdates.isEmpty())
        request()
        awaitUpdates(1)
        assertEquals(listOf("config-2"), NativeLib.socketUpdates)
    }

    @Test fun stopUsesManagerOwningRegistration() {
        assertTrue(NetworkChangeMonitor.start())
        context.connectivityManager = ConnectivityManager()
        NetworkChangeMonitor.stop()
        assertTrue(manager.callbacks.isEmpty())
    }

    @Test fun readySnapshotIsNotPostedAfterCoreStops() {
        val (entered, release) = blockFirst()
        assertTrue(NetworkChangeMonitor.start())
        request()
        assertTrue(entered.await(2, TimeUnit.SECONDS))
        NativeLib.ready = false
        release.countDown()
        provider.joinWorkers()
        loop.dispatchAll()
        assertTrue(NativeLib.socketUpdates.isEmpty())
        assertEquals(0, NativeLib.networkNotifications.get())
        NativeLib.ready = true
        NetworkChangeMonitor.onCoreStarted()
        loop.dispatchAll()
        loop.advanceBy(500)
        awaitUpdates(1)
        assertEquals(listOf("config-2"), NativeLib.socketUpdates)
    }

    @Test fun rejectedJniUpdateIsLoggedAndNextEventCanRetry() {
        NativeLib.acceptSocketUpdate = false
        assertTrue(NetworkChangeMonitor.start())
        request()
        awaitUpdates(1)
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.message.contains("rejected") })
        NativeLib.acceptSocketUpdate = true
        request()
        awaitUpdates(2)
        assertEquals(listOf("config-1", "config-2"), NativeLib.socketUpdates)
    }

    @Test fun registrationFailureIsLoggedAndRetryable() {
        manager.failAdd = true
        assertFalse(NetworkChangeMonitor.start())
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.category == "NET" })
        manager.failAdd = false
        assertTrue(NetworkChangeMonitor.start())
    }
}
