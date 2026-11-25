package com.utun.chat.data

import android.os.Looper
import java.util.concurrent.atomic.AtomicReference
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий CoreActivity; fixture только записывает JNI-запросы и задаёт готовность/отказ границы native. */
class CoreActivityLifecycleTest {
    @Before fun prepare() {
        Looper.getMainLooper()
        clearOwners()
        NativeLib.activityAttempts = 0
        NativeLib.activityRequests.clear()
        NativeLib.failActivityRequest = false
        NativeLib.acceptActivityRequest = true
        NativeLib.ready = true
        LogManager.entries.clear()
    }

    @After fun release() {
        clearOwners()
        NativeLib.failActivityRequest = false
        NativeLib.acceptActivityRequest = true
        NativeLib.ready = true
    }

    private fun clearOwners() {
        NativeLib.ready = false
        CoreActivity.visible(false)
        CoreActivity.call(false)
        CoreActivity.radioCall(false)
        CoreActivity.transfer(false)
        CoreActivity.reset()
    }

    private fun offMain(action: () -> Unit): Throwable? {
        val result = AtomicReference<Throwable?>()
        val worker = Thread({ try { action() } catch (error: Throwable) { result.set(error) } }, "activity-background-test")
        worker.start(); worker.join(2_000)
        assertFalse("background caller did not finish", worker.isAlive)
        return result.get()
    }

    @Test fun allOwnerCombinationsAreAggregatedAfterOfflineUpdates() {
        for (mask in 0..15) {
            clearOwners()
            NativeLib.activityRequests.clear()
            CoreActivity.visible(mask and 1 != 0)
            CoreActivity.call(mask and 2 != 0)
            CoreActivity.radioCall(mask and 4 != 0)
            CoreActivity.transfer(mask and 8 != 0)
            assertTrue(NativeLib.activityRequests.isEmpty())
            NativeLib.ready = true
            CoreActivity.onCoreStarted()
            assertEquals("owner mask=$mask", listOf(mask != 0), NativeLib.activityRequests)
        }
    }

    @Test fun releasingCallDoesNotSleepAnActiveTransfer() {
        CoreActivity.call(true)
        CoreActivity.transfer(true)
        CoreActivity.call(false)
        assertEquals(listOf(true), NativeLib.activityRequests)
        CoreActivity.transfer(false)
        assertEquals(listOf(true, false), NativeLib.activityRequests)
    }

    @Test fun repeatedSyncDoesNotDuplicateRequests() {
        CoreActivity.visible(true)
        repeat(3) { CoreActivity.visible(true); CoreActivity.sync() }
        assertEquals(listOf(true), NativeLib.activityRequests)
    }

    @Test fun resetPreservesOwnersAndInvalidatesOnlyRequestCache() {
        CoreActivity.radioCall(true)
        CoreActivity.reset()
        assertEquals(listOf(true), NativeLib.activityRequests)
        CoreActivity.sync()
        assertEquals(listOf(true, true), NativeLib.activityRequests)
    }

    @Test fun newCoreReceivesCurrentIntentEvenIfBooleanDidNotChange() {
        CoreActivity.visible(true)
        CoreActivity.onCoreStarted()
        CoreActivity.onCoreStarted()
        assertEquals(listOf(true, true, true), NativeLib.activityRequests)
    }

    @Test fun stoppedCoreDoesNotReceiveRequestsAndRestartGetsLatestIntent() {
        CoreActivity.call(true)
        NativeLib.ready = false
        CoreActivity.call(false)
        CoreActivity.transfer(true)
        CoreActivity.transfer(false)
        assertEquals(listOf(true), NativeLib.activityRequests)
        NativeLib.ready = true
        CoreActivity.onCoreStarted()
        assertEquals(listOf(true, false), NativeLib.activityRequests)
    }

    @Test fun failedNativeCallRetainsIntentForRetry() {
        NativeLib.failActivityRequest = true
        assertTrue(runCatching { CoreActivity.visible(true) }.exceptionOrNull() is IllegalStateException)
        assertTrue(NativeLib.activityRequests.isEmpty())
        NativeLib.failActivityRequest = false
        CoreActivity.sync()
        assertEquals(2, NativeLib.activityAttempts)
        assertEquals(listOf(true), NativeLib.activityRequests)
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.message.contains("activity request") })
    }

    @Test fun rejectedBackgroundOwnerUpdateDoesNotPoisonMainState() {
        val setters = listOf<(Boolean) -> Unit>(CoreActivity::visible, CoreActivity::call, CoreActivity::radioCall, CoreActivity::transfer)
        for ((owner, setter) in setters.withIndex()) {
            clearOwners()
            NativeLib.ready = true
            NativeLib.activityRequests.clear()
            CoreActivity.sync()
            assertTrue("owner=$owner", offMain { setter(true) } is IllegalStateException)
            CoreActivity.sync()
            assertEquals("owner=$owner", listOf(false), NativeLib.activityRequests)
        }
    }

    @Test fun rejectedNativePostDoesNotSuppressRetry() {
        NativeLib.acceptActivityRequest = false
        CoreActivity.visible(true)
        assertTrue(NativeLib.activityRequests.isEmpty())
        NativeLib.acceptActivityRequest = true
        CoreActivity.sync()
        assertEquals(2, NativeLib.activityAttempts)
        assertEquals(listOf(true), NativeLib.activityRequests)
    }

    @Test fun resetRejectsBackgroundCallerWithoutInvalidatingCache() {
        CoreActivity.sync()
        assertTrue(offMain { CoreActivity.reset() } is IllegalStateException)
        CoreActivity.sync()
        assertEquals(listOf(false), NativeLib.activityRequests)
    }

    @Test fun backgroundSyncAndReadySignalCannotPostNativeCommands() {
        assertTrue(offMain { CoreActivity.sync() } is IllegalStateException)
        assertTrue(offMain { CoreActivity.onCoreStarted() } is IllegalStateException)
        assertTrue(NativeLib.activityRequests.isEmpty())
    }
}
