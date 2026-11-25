package com.utun.chat.data

import android.content.Context
import android.net.ConnectivityManager
import android.os.Build
import android.os.Looper
import com.utun.chat.ChatApplication
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий monitor: владение listener, поздние callbacks и исключения Android API; Binder/ОС не моделируются. */
class NetworkActivityMonitorLifecycleTest {
    private lateinit var manager: ConnectivityManager
    private lateinit var context: Context

    @Before fun prepare() {
        Build.VERSION.SDK_INT = 36
        context = Context()
        manager = requireNotNull(context.connectivityManager)
        ChatApplication.instance = context
        NativeLib.networkNotifications.set(0)
        LogManager.entries.clear()
        LogManager.console = true
    }

    @After fun release() {
        // Старая реализация могла бросить исключение посередине stop(); fixture позволяет завершить отрицательный контроль.
        Build.VERSION.SDK_INT = 36
        ChatApplication.instance = context
        context.connectivityManager = manager
        manager.failAdd = false
        manager.failRemove = false
        manager.failQuery = false
        NetworkActivityMonitor.stop()
        Looper.getMainLooper().dispatchAll()
        assertTrue("listener leaked", manager.listeners.isEmpty())
    }

    @Test fun minimumSupportedAndroidStillRegistersListener() {
        Build.VERSION.SDK_INT = 26
        NetworkActivityMonitor.start()
        assertEquals(1, manager.listeners.size)
        manager.deliver(0)
        Looper.getMainLooper().dispatchAll()
        assertEquals(1, NativeLib.networkNotifications.get())
    }

    @Test fun startsAndStopsAreIdempotent() {
        repeat(3) { NetworkActivityMonitor.start() }
        assertEquals(1, manager.addAttempts)
        repeat(3) { NetworkActivityMonitor.stop() }
        assertEquals(1, manager.removeAttempts)
        assertTrue(manager.listeners.isEmpty())
    }

    @Test fun initiallyActiveNetworkWakesCoreOnce() {
        manager.active = true
        NetworkActivityMonitor.start()
        NetworkActivityMonitor.start()
        assertEquals(1, NativeLib.networkNotifications.get())
    }

    @Test fun callbackFromBinderIsDeliveredThroughMainQueue() {
        NetworkActivityMonitor.start()
        val binder = Thread { manager.deliver(0) }
        binder.start()
        binder.join(2_000)
        assertFalse("fixture callback blocked", binder.isAlive)
        assertEquals(0, NativeLib.networkNotifications.get())
        Looper.getMainLooper().dispatchAll()
        assertEquals(1, NativeLib.networkNotifications.get())
    }

    @Test fun queuedCallbackIsIgnoredAfterStop() {
        NetworkActivityMonitor.start()
        manager.deliver(0)
        NetworkActivityMonitor.stop()
        Looper.getMainLooper().dispatchAll()
        assertEquals(0, NativeLib.networkNotifications.get())
    }

    @Test fun previousRegistrationCannotWakeRestartedCore() {
        NetworkActivityMonitor.start()
        NetworkActivityMonitor.stop()
        NetworkActivityMonitor.start()
        manager.deliver(0)
        Looper.getMainLooper().dispatchAll()
        assertEquals(0, NativeLib.networkNotifications.get())
        manager.deliver(1)
        Looper.getMainLooper().dispatchAll()
        assertEquals(1, NativeLib.networkNotifications.get())
    }

    @Test fun stopUnregistersFromManagerThatOwnsListener() {
        NetworkActivityMonitor.start()
        context.connectivityManager = ConnectivityManager()
        NetworkActivityMonitor.stop()
        assertEquals(1, manager.removeAttempts)
        assertTrue(manager.listeners.isEmpty())
    }

    @Test fun missingManagerIsLoggedAndCanBeRetried() {
        context.connectivityManager = null
        NetworkActivityMonitor.start()
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.category == "NET" })
        context.connectivityManager = manager
        NetworkActivityMonitor.start()
        assertEquals(1, manager.listeners.size)
    }

    @Test fun failedRegistrationIsLoggedAndCanBeRetried() {
        manager.failAdd = true
        NetworkActivityMonitor.start()
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.message.contains("registration") })
        manager.failAdd = false
        NetworkActivityMonitor.start()
        assertEquals(2, manager.addAttempts)
        assertEquals(1, manager.listeners.size)
    }

    @Test fun failedInitialQueryKeepsRegisteredListenerUsable() {
        manager.failQuery = true
        NetworkActivityMonitor.start()
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.message.contains("query") })
        manager.deliver(0)
        Looper.getMainLooper().dispatchAll()
        assertEquals(1, NativeLib.networkNotifications.get())
    }

    @Test fun removalFailureDoesNotLeaveMonitorStarted() {
        NetworkActivityMonitor.start()
        manager.failRemove = true
        NetworkActivityMonitor.stop()
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.message.contains("removal") })
        manager.failRemove = false
        manager.listeners.clear() // ОС fixture не выполнила удаление при инъекции исключения.
        NetworkActivityMonitor.start()
        assertEquals(2, manager.addAttempts)
        manager.deliver(0)
        Looper.getMainLooper().dispatchAll()
        assertEquals(0, NativeLib.networkNotifications.get())
        manager.deliver(1)
        Looper.getMainLooper().dispatchAll()
        assertEquals(1, NativeLib.networkNotifications.get())
    }
}
