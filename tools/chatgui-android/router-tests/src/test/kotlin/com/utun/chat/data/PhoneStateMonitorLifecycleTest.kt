package com.utun.chat.data

import android.content.Context
import android.os.Build
import android.os.Looper
import android.telephony.SubscriptionManager
import android.telephony.TelephonyManager
import com.utun.chat.ChatApplication
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.cancelChildren
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий singleton, Flow настроек и callbacks обеих Android API; каждый тест моделирует отдельный процесс приложения. */
@OptIn(ExperimentalCoroutinesApi::class)
class PhoneStateMonitorLifecycleTest {
    private lateinit var context: Context
    private lateinit var subscriptions: SubscriptionManager
    private lateinit var telephony: TelephonyManager
    private val loop get() = Looper.getMainLooper()

    @Before fun prepare() {
        assertSame(loop, Looper.myLooper())
        Dispatchers.setMain(UnconfinedTestDispatcher())
        Build.VERSION.SDK_INT = 36
        context = Context()
        subscriptions = requireNotNull(context.subscriptionManager)
        telephony = requireNotNull(context.telephonyManager)
        ChatApplication.instance = context
        NativeLib.ready = true; NativeLib.acceptPhoneRequest = true
        PhoneStateMonitor.init()
        loop.dispatchAll()
        NativeLib.phoneRequests.clear(); LogManager.entries.clear(); LogManager.console = true
    }

    @After fun release() {
        NativeLib.ready = false
        context.permissionGranted = false
        subscriptions.failRemove = false
        telephony.subscriptions.values.forEach { it.failRemove = false }
        PhoneStateMonitor.refresh()
        loop.dispatchAll()
        // В приложении scope живёт до завершения процесса; в JVM отменяем только его collector между тестами.
        val scope = PhoneStateMonitor::class.java.getDeclaredField("scope").apply { isAccessible = true }.get(null) as CoroutineScope
        scope.coroutineContext.cancelChildren()
        PhoneStateMonitor::class.java.getDeclaredField("started").apply { isAccessible = true }.setBoolean(null, false)
        Dispatchers.resetMain()
    }

    private fun options(mask: Int, dispatch: Boolean = true) {
        context.configProvider.incomingCallOptions.value = List(5) { mask and (1 shl it) != 0 }
        if (dispatch) loop.dispatchAll()
    }
    private fun phone(id: Int = 10) = telephony.createForSubscriptionId(id)
    private fun state(expected: Int, policy: Int = 1) = assertEquals(expected to policy, NativeLib.phoneRequests.lastOrNull())
    private fun deliver(id: Int, state: Int, index: Int = phone(id).history.lastIndex) {
        phone(id).deliver(index, state); loop.dispatchAll()
    }

    @Test fun initialStateComesFromCallbackAndBusyWinsAcrossSims() {
        subscriptions.ids = listOf(10, 20)
        phone(10).automaticInitial = false; phone(20).automaticInitial = false
        options(1)
        state(0)
        deliver(10, TelephonyManager.CALL_STATE_IDLE); state(0)
        deliver(20, TelephonyManager.CALL_STATE_RINGING); state(2)
        deliver(20, TelephonyManager.CALL_STATE_IDLE); state(1)
        deliver(10, TelephonyManager.CALL_STATE_OFFHOOK); state(2)
    }

    @Test fun minimumAndroidUsesLegacyListenerAndReceivesUpdates() {
        Build.VERSION.SDK_INT = 26
        options(1); state(1)
        deliver(10, TelephonyManager.CALL_STATE_RINGING); state(2)
        deliver(10, TelephonyManager.CALL_STATE_IDLE); state(1)
        options(0)
        assertTrue(phone().active.isEmpty())
    }

    @Test fun failedSynchronousSnapshotCannotDisableLiveCallback() {
        phone().initialState = TelephonyManager.CALL_STATE_OFFHOOK
        phone().failQuery = true
        options(1)
        state(2)
        deliver(10, TelephonyManager.CALL_STATE_IDLE); state(1)
    }

    @Test fun missingPermissionPublishesUnknownAndKeepsPolicyBits() {
        context.permissionGranted = false
        options(25)
        state(0, 25)
        assertTrue(subscriptions.active.isEmpty())
        assertEquals(0, phone().registerAttempts)
        context.permissionGranted = true
        PhoneStateMonitor.refresh(); loop.dispatchAll()
        state(1, 25)
    }

    @Test fun blockOnlyPoliciesDoNotRequestPhoneObservers() {
        for (mask in listOf(8, 16, 24)) {
            options(mask); state(0, mask)
            assertTrue(subscriptions.active.isEmpty())
            assertEquals(0, phone().registerAttempts)
        }
    }

    @Test fun eachPhonePolicyEnablesObservationIndependently() {
        for (mask in listOf(1, 2, 4)) {
            options(mask); state(1, mask)
            assertEquals(1, phone().active.size)
            options(0)
            assertTrue(phone().active.isEmpty())
        }
    }

    @Test fun invalidCallbackDoesNotPublishIdleAndIsLogged() {
        options(1); state(1)
        deliver(10, 99); state(0)
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.message.contains("Invalid phone state") })
        deliver(10, TelephonyManager.CALL_STATE_IDLE); state(1)
    }

    @Test fun policyChangesReuseLiveObserverAndPreserveBusy() {
        phone().initialState = TelephonyManager.CALL_STATE_OFFHOOK
        options(1)
        NativeLib.phoneRequests.clear()
        options(31)
        state(2, 31)
        assertFalse(NativeLib.phoneRequests.any { it.first == 0 })
        assertEquals(1, phone().registerAttempts)
        assertEquals(1, subscriptions.addAttempts)
    }

    @Test fun refreshReplaysStateWithoutReplacingHealthyWatch() {
        phone().initialState = TelephonyManager.CALL_STATE_OFFHOOK
        options(1)
        NativeLib.phoneRequests.clear()
        PhoneStateMonitor.refresh(); loop.dispatchAll()
        state(2)
        assertEquals(1, phone().registerAttempts)
        assertEquals(0, phone().removeAttempts)
    }

    @Test fun simReplacementIgnoresPreviousRegistration() {
        phone().initialState = TelephonyManager.CALL_STATE_OFFHOOK
        options(1)
        subscriptions.ids = listOf(20)
        subscriptions.deliver(); loop.dispatchAll(); state(1)
        assertTrue(phone(10).active.isEmpty())
        val count = NativeLib.phoneRequests.size
        deliver(10, TelephonyManager.CALL_STATE_OFFHOOK, 0)
        assertEquals(count, NativeLib.phoneRequests.size)
    }

    @Test fun partialRegistrationFailureKeepsHealthySimAndRetriesMissingSim() {
        subscriptions.ids = listOf(10, 20)
        phone(20).failRegister = true
        options(1); state(0)
        deliver(10, TelephonyManager.CALL_STATE_RINGING); state(2)
        phone(20).failRegister = false
        loop.advanceBy(30_000); state(2)
        assertEquals(1, phone(10).registerAttempts)
        deliver(10, TelephonyManager.CALL_STATE_IDLE); state(1)
    }

    @Test fun registrationRecoversWithoutSubscriptionChangeOrActivityResume() {
        phone().failRegister = true
        options(1); state(0)
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.category == "PhoneState" })
        phone().failRegister = false
        loop.advanceBy(30_000)
        state(1)
        assertEquals(1, phone().active.size)
    }

    @Test fun subscriptionListFailureRetriesWithoutExternalEvent() {
        subscriptions.failQuery = true
        options(1); state(0)
        subscriptions.failQuery = false
        loop.advanceBy(30_000)
        state(1)
    }

    @Test fun subscriptionListenerFailureRetriesWithoutExternalEvent() {
        subscriptions.failAdd = true
        options(1); state(0)
        subscriptions.failAdd = false
        loop.advanceBy(30_000)
        state(1)
        assertEquals(1, subscriptions.active.size)
    }

    @Test fun subscriptionCallbackAfterPermissionRevocationStopsObservers() {
        phone().initialState = TelephonyManager.CALL_STATE_OFFHOOK
        options(1); state(2)
        context.permissionGranted = false
        subscriptions.deliver(); loop.dispatchAll()
        state(0)
        assertTrue(subscriptions.active.isEmpty() && phone().active.isEmpty())
    }

    @Test fun subscriptionRemovalFailureIsLoggedWithoutEscapingRefresh() {
        options(1)
        context.permissionGranted = false
        subscriptions.failRemove = true
        PhoneStateMonitor.refresh()
        state(0)
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.message.contains("remov", ignoreCase = true) })
        val count = NativeLib.phoneRequests.size
        subscriptions.deliver(0); loop.dispatchAll()
        assertEquals(count, NativeLib.phoneRequests.size)
    }

    @Test fun removalUsesManagerThatOwnsSubscriptionListener() {
        options(1)
        val replacement = SubscriptionManager()
        context.subscriptionManager = replacement
        context.permissionGranted = false
        PhoneStateMonitor.refresh()
        assertEquals(1, subscriptions.removeAttempts)
        assertEquals(0, replacement.removeAttempts)
        assertTrue(subscriptions.active.isEmpty())
    }

    @Test fun latePhoneCallbackAfterRemovalFailureCannotChangeNewState() {
        options(1)
        phone().failRemove = true
        options(0)
        phone().failRemove = false
        options(1)
        val count = NativeLib.phoneRequests.size
        deliver(10, TelephonyManager.CALL_STATE_OFFHOOK, 0)
        assertEquals(count, NativeLib.phoneRequests.size)
        deliver(10, TelephonyManager.CALL_STATE_RINGING); state(2)
    }

    @Test fun listFailureInvalidatesOldPhoneCallbacksUntilRecovery() {
        options(1)
        subscriptions.failQuery = true
        subscriptions.deliver(); loop.dispatchAll(); state(0)
        val count = NativeLib.phoneRequests.size
        deliver(10, TelephonyManager.CALL_STATE_IDLE, 0)
        assertEquals(count, NativeLib.phoneRequests.size)
        subscriptions.failQuery = false
        loop.advanceBy(30_000); state(1)
    }

    @Test fun disablingPolicyCancelsRegistrationRetry() {
        phone().failRegister = true
        options(1)
        options(24)
        val attempts = phone().registerAttempts
        phone().failRegister = false
        loop.advanceBy(60_000)
        state(0, 24)
        assertEquals(attempts, phone().registerAttempts)
        assertTrue(phone().active.isEmpty() && subscriptions.active.isEmpty())
    }

    @Test fun emptySimSetStaysUnknownAndNewSimIsObserved() {
        subscriptions.ids = emptyList()
        options(1); state(0)
        assertEquals(1, subscriptions.active.size)
        subscriptions.ids = listOf(10)
        subscriptions.deliver(); loop.dispatchAll(); state(1)
    }

    @Test fun missingTelephonyServiceStaysUnknown() {
        context.telephonyManager = null
        options(1); state(0)
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.category == "PhoneState" })
        assertTrue(subscriptions.active.isEmpty())
    }

    @Test fun rejectedNativeCommandIsRetriedByRefresh() {
        NativeLib.acceptPhoneRequest = false
        options(1)
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.message.contains("not queued") })
        NativeLib.acceptPhoneRequest = true
        NativeLib.phoneRequests.clear()
        PhoneStateMonitor.refresh(); state(1)
    }

    @Test fun invalidThreadCannotInitOrRefreshAndIsLogged() {
        for (action in listOf({ PhoneStateMonitor.init() }, { PhoneStateMonitor.refresh() })) {
            val error = AtomicReference<Throwable?>()
            val thread = Thread { try { action() } catch (failure: Throwable) { error.set(failure) } }
            thread.start(); thread.join(2_000)
            assertFalse(thread.isAlive)
            assertTrue(error.get() is IllegalStateException)
        }
        assertEquals(0, subscriptions.addAttempts)
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.message.contains("main thread") })
    }
}
