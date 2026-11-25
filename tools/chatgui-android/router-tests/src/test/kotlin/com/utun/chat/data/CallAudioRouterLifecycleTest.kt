package com.utun.chat.data

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.media.AudioManager
import com.utun.chat.ChatApplication
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/** Проверяется настоящий CallAudioRouter через его публичный API и владение proxy на стороне Android-модели.
 * Fixtures не создают системных ресурсов, не меняют приватные поля router и не моделируют качество звука/Binder. */
class CallAudioRouterLifecycleTest {
    private lateinit var adapter: BluetoothAdapter
    private lateinit var router: CallAudioRouter

    @Before fun prepare() {
        AudioManager.shared = AudioManager()
        ChatApplication.instance = Context()
        adapter = BluetoothAdapter()
        BluetoothAdapter.current = adapter
        LogManager.entries.clear()
        LogManager.console = true
        router = CallAudioRouter()
    }

    @After fun release() {
        router.stop()
        val leaked = adapter.live.toList()
        // Уборка только объектов fixture после отрицательного контроля старого router.
        leaked.forEach { adapter.closeProfileProxy(BluetoothProfile.HEADSET, it) }
        assertEquals("router left owned profile proxies: $leaked", 0, leaked.size)
        assertTrue("device callback remains registered", AudioManager.shared.callbacks.isEmpty())
        assertTrue("unexpected router ERROR: ${LogManager.entries}", LogManager.entries.none { it.level == "ERROR" })
    }

    @Test fun connectedStopReleasesProxy() {
        router.start()
        adapter.connect(0)
        assertTrue(router.headsetAvailable)
        router.stop()
        assertTrue(adapter.live.isEmpty())
        assertEquals(1, adapter.closes)
    }

    @Test fun normalRestartClosesBothProxies() {
        repeat(2) { index -> router.start(); adapter.connect(index); router.stop() }
        assertEquals(2, adapter.requests.size)
        assertEquals(2, adapter.closes)
    }

    @Test fun lateConnectAfterStopClosesProxy() {
        var changes = 0
        router.onChanged = { _, _ -> ++changes }
        router.start()
        router.stop()
        val changesAtStop = changes
        assertEquals(1, adapter.live.size) // Запрос принят, callback ещё не доставлен.
        adapter.connect(0)
        assertTrue("late callback must release its proxy", adapter.live.isEmpty())
        assertEquals(changesAtStop, changes)
        assertFalse(router.headsetAvailable)
    }

    @Test fun disconnectedStopClosesOwnedProxy() {
        router.start()
        adapter.connect(0)
        adapter.disconnect(0)
        assertFalse(router.headsetAvailable)
        assertEquals(1, adapter.live.size) // Потеря сервиса не отменяет владение proxy.
        router.stop()
        assertTrue("disconnected proxy must still be closed", adapter.live.isEmpty())
    }

    @Test fun pendingRestartRejectsOldProxy() {
        router.start()
        router.stop()
        router.start()
        adapter.connect(0)
        assertEquals(setOf(adapter.requests[1].proxy), adapter.live)
        assertFalse("old callback must not expose a headset in the new lifecycle", router.headsetAvailable)
        adapter.connect(1)
        assertTrue(router.headsetAvailable)
        router.stop()
        assertEquals(2, adapter.closes)
    }

    @Test fun serviceReconnectUsesSameProxy() {
        router.start()
        adapter.connect(0)
        adapter.disconnect(0)
        assertFalse(router.headsetAvailable)
        adapter.connect(0)
        assertTrue(router.headsetAvailable)
        assertEquals(1, adapter.requests.size)
        assertEquals(0, adapter.closes)
        router.stop()
        assertEquals(1, adapter.closes)
    }

    @Test fun repeatedStopClosesOnlyOnce() {
        router.stop()
        router.start()
        adapter.connect(0)
        repeat(3) { router.stop() }
        assertEquals(1, adapter.closeAttempts)
    }

    @Test fun oldDisconnectDoesNotClearNewProxy() {
        var changes = 0
        router.onChanged = { _, _ -> ++changes }
        router.start()
        adapter.connect(0)
        router.stop()
        router.start()
        adapter.connect(1)
        val changesBefore = changes
        adapter.disconnect(0) // Уже поставленный в очередь callback старого listener.
        assertTrue("old disconnect must not affect the new headset", router.headsetAvailable)
        assertEquals(changesBefore, changes)
        router.stop()
        assertEquals(2, adapter.closes)
    }

    @Test fun callbackAndStopAreSerialized() {
        LogManager.console = false
        repeat(64) { index ->
            router.start()
            val ready = CountDownLatch(2)
            val go = CountDownLatch(1)
            val failure = AtomicReference<Throwable?>()
            val workers = listOf(worker(failure) {
                ready.countDown(); check(go.await(5, TimeUnit.SECONDS)); adapter.connect(index)
            }, worker(failure) {
                ready.countDown(); check(go.await(5, TimeUnit.SECONDS)); router.stop()
            })
            assertTrue("workers failed to start", ready.await(5, TimeUnit.SECONDS))
            go.countDown()
            join(workers, failure)
            assertTrue("iteration=$index retains proxies=${adapter.live}", adapter.live.isEmpty())
        }
        assertEquals(64, adapter.closes)
        LogManager.console = true
        LogManager.addLog("INFO", "RouterLifecycleTest", "64 concurrent callback/stop schedules released all proxies")
    }

    @Test fun ioStartWithPendingMainCallbackAndStopCompletes() {
        val entered = CountDownLatch(1)
        val returnRequest = CountDownLatch(1)
        val ready = CountDownLatch(2)
        val failure = AtomicReference<Throwable?>()
        adapter.requestEntered = entered
        adapter.requestReturn = returnRequest
        val starter = worker(failure) { router.start() }
        assertTrue("I/O start did not enter profile request", entered.await(5, TimeUnit.SECONDS))
        val callback = worker(failure) { ready.countDown(); adapter.connect(0) }
        val stopper = worker(failure) { ready.countDown(); router.stop() }
        try {
            assertTrue("callback/stop did not start", ready.await(5, TimeUnit.SECONDS))
        } finally {
            returnRequest.countDown()
        }
        join(listOf(starter, callback, stopper), failure)
        assertTrue(adapter.live.isEmpty())
        assertEquals(1, adapter.closes)
        assertTrue(AudioManager.shared.callbacks.isEmpty())
    }

    @Test fun rejectedRequestLogsWarning() {
        adapter.rejectRequest = true
        router.start()
        assertTrue(adapter.requests.isEmpty())
        assertFalse(router.headsetAvailable)
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.message.contains("accepted=false") })
    }

    @Test fun requestExceptionLogsWarning() {
        adapter.throwRequest = true
        router.start()
        assertTrue(adapter.requests.isEmpty())
        assertFalse(router.headsetAvailable)
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.message.contains("injected BT request failure") })
    }

    @Test fun absentAdapterKeepsSpeakerRoute() {
        BluetoothAdapter.current = null
        router.start()
        assertTrue(adapter.requests.isEmpty())
        assertEquals(AudioRoute.SPEAKER, router.route)
        assertFalse(router.headsetAvailable)
    }

    @Test fun duplicateStartDoesNotRequestAnotherProxy() {
        router.start()
        router.start()
        assertEquals(1, adapter.requests.size)
        assertEquals(1, AudioManager.shared.callbacks.size)
        adapter.connect(0)
        router.stop()
        assertEquals(1, adapter.closes)
    }

    @Test fun revokedPermissionStillReleasesOwnedProxy() {
        router.start()
        adapter.connect(0)
        ChatApplication.instance.permissionGranted = false
        adapter.disconnect(0)
        assertFalse(router.headsetAvailable)
        router.stop()
        assertEquals(1, adapter.closes)
        assertTrue(adapter.live.isEmpty())
    }

    private fun worker(failure: AtomicReference<Throwable?>, body: () -> Unit): Thread = Thread {
        try { body() } catch (error: Throwable) { failure.compareAndSet(null, error) }
    }.apply { isDaemon = true; start() }

    private fun join(workers: List<Thread>, failure: AtomicReference<Throwable?>) {
        workers.forEach { it.join(5000); assertFalse("lifecycle worker did not finish", it.isAlive) }
        failure.get()?.let { throw AssertionError("lifecycle worker failed", it) }
    }
}
