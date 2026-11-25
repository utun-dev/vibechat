package com.utun.chat.headless

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.*
import org.junit.Test

/** Настоящий coordinator с управляемым конфигом/событиями: native-поток и Android Service здесь не создаются. */
class CoreStartupTest {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Unconfined)
    private val config = CompletableDeferred<String>()
    private var nativeReady = false
    private var accepted = true
    private var readyCalls = 0
    private var failures = 0
    private val failed = CompletableDeferred<Unit>()
    private var readinessError: Exception? = null
    private val submitted = mutableListOf<String>()
    private val logs = mutableListOf<Pair<String, String>>()
    private fun coordinator(timeout: Long = 5_000) = CoreStartup(scope, { nativeReady },
        { readinessError?.let { throw it }; readyCalls++ }, { failures++; failed.complete(Unit) },
        { level, message -> logs.add(level to message) }, timeoutMillis = timeout)
    private val startup = coordinator()

    @After fun release() { startup.stop(); scope.cancel() }

    private fun start() = startup.start({ config.await() }) { submitted.add(it); accepted }
    private fun waiter() = scope.async(start = CoroutineStart.UNDISPATCHED) { startup.awaitReady(); readyCalls }

    @Test fun acceptedNativeStartDoesNotReleaseSettingsBeforeReady() = runBlocking {
        val first = waiter()
        start()
        assertTrue(submitted.isEmpty())
        config.complete("private config")
        val second = waiter()
        assertEquals(listOf("private config"), submitted)
        assertFalse(first.isCompleted)
        assertFalse(second.isCompleted)
        nativeReady = true
        startup.coreStarted()
        assertEquals(1, first.await())
        assertEquals(1, second.await())
        assertTrue(logs.none { it.second.contains("private config") })
    }

    @Test fun signalWithoutReadyCoreCannotReleaseWaiter() = runBlocking<Unit> {
        start(); config.complete("config")
        val result = waiter()
        startup.coreStarted()
        assertFalse(result.isCompleted)
        assertEquals(0, readyCalls)
        nativeReady = true
        startup.coreStarted()
        result.await()
    }

    @Test fun readyExistingCoreNeedsNoNewNativeEvent() = runBlocking {
        nativeReady = true
        start(); config.complete("config")
        assertEquals(1, waiter().await())
        assertEquals(0, failures)
    }

    @Test fun nativeRejectionFailsCurrentAndFutureWaiters() = runBlocking {
        val first = waiter()
        accepted = false
        start(); config.complete("config")
        assertTrue(runCatching { first.await() }.exceptionOrNull() is IllegalStateException)
        assertTrue(runCatching { startup.awaitReady() }.exceptionOrNull() is IllegalStateException)
        assertEquals(1, failures)
        assertTrue(logs.any { it.first == "ERROR" && it.second.contains("native start rejected") })
    }

    @Test fun configFailureDoesNotSubmitNativeStart() = runBlocking {
        start()
        val error = java.io.IOException("config unavailable")
        config.completeExceptionally(error)
        val result = runCatching { startup.awaitReady() }.exceptionOrNull()
        assertEquals(error.javaClass, result?.javaClass)
        assertEquals(error.message, result?.message)
        assertTrue(submitted.isEmpty())
        assertEquals(1, failures)
    }

    @Test fun stopDuringConfigPreparationPreventsLateStart() = runBlocking {
        start()
        val result = waiter()
        startup.stop()
        assertFalse(config.isCancelled)
        config.complete("late config")
        nativeReady = true
        startup.coreStarted()
        assertTrue(submitted.isEmpty())
        assertEquals(0, readyCalls)
        assertTrue(runCatching { result.await() }.exceptionOrNull() is IllegalStateException)
        assertEquals(0, failures)
    }

    @Test fun stopAfterNativeAcceptanceRejectsLateReadyEvent() = runBlocking {
        start(); config.complete("config")
        val result = waiter()
        startup.stop()
        nativeReady = true
        startup.coreStarted()
        assertTrue(runCatching { result.await() }.exceptionOrNull() is IllegalStateException)
        assertEquals(0, readyCalls)
        assertEquals(0, failures)
    }

    @Test fun cancelledSettingsWaiterDoesNotCancelSharedStartup() = runBlocking {
        start(); config.complete("config")
        val retired = waiter()
        val active = waiter()
        retired.cancel()
        nativeReady = true
        startup.coreStarted()
        assertTrue(retired.isCancelled)
        assertEquals(1, active.await())
        assertEquals(0, failures)
    }

    @Test fun missingNativeReadySignalFailsWithBoundedTimeout() = runBlocking {
        val timed = coordinator(timeout = 20)
        timed.start({ "config" }) { true }
        val error = withTimeout(2_000) { runCatching { timed.awaitReady() }.exceptionOrNull() }
        assertTrue(error is IllegalStateException)
        assertTrue(error!!.message!!.contains("readiness timeout"))
        withTimeout(2_000) { failed.await() }
        assertEquals(1, failures)
    }

    @Test fun readyServiceContinuesResynchronizingLaterCoreInstances() = runBlocking {
        nativeReady = true
        start(); config.complete("config")
        startup.awaitReady()
        startup.coreStarted()
        assertEquals(2, readyCalls)
        assertEquals(0, failures)
    }

    @Test fun initializationFailureCannotBeOverwrittenByLaterSuccess() = runBlocking {
        val error = IllegalStateException("native init rejected")
        startup.fail(error)
        startup.fail(IllegalStateException("duplicate failure"))
        start(); config.complete("config")
        nativeReady = true
        startup.coreStarted()
        val result = runCatching { startup.awaitReady() }.exceptionOrNull()
        assertEquals(error.javaClass, result?.javaClass)
        assertEquals(error.message, result?.message)
        assertTrue(submitted.isEmpty())
        assertEquals(0, readyCalls)
        assertEquals(1, failures)
    }

    @Test fun databaseFailurePreservesUiReasonAndStopsWithoutReadinessTimeout() = runBlocking {
        start(); config.complete("config")
        val reason = "База несовместима\nФайл: /profile/chats.db\nОбъект: peers_42\nБаза не изменена"
        startup.fail(IllegalStateException(reason))
        startup.coreStopped(); nativeReady = true; startup.coreStarted()
        assertEquals(reason, runCatching { startup.awaitReady() }.exceptionOrNull()?.message)
        assertEquals(1, failures); assertEquals(0, readyCalls)
        assertTrue(logs.any { it.first == "ERROR" && reason in it.second })
    }

    @Test fun stoppedServiceCannotReusePreviouslySuccessfulReadiness() = runBlocking {
        nativeReady = true
        start(); config.complete("config")
        startup.awaitReady()
        startup.stop()
        assertTrue(runCatching { startup.awaitReady() }.exceptionOrNull() is IllegalStateException)
    }

    @Test fun monitorSetupMustSucceedBeforeReadinessIsReleased() = runBlocking {
        readinessError = IllegalStateException("monitor setup failed")
        nativeReady = true
        start(); config.complete("config")
        val result = runCatching { startup.awaitReady() }.exceptionOrNull()
        assertEquals(readinessError!!.javaClass, result?.javaClass)
        assertEquals(readinessError!!.message, result?.message)
        assertEquals(0, readyCalls)
        assertEquals(1, failures)
    }

    @Test fun synchronousReadySignalDuringNativeSubmissionIsRetained() = runBlocking {
        startup.start({ config.await() }) {
            nativeReady = true
            startup.coreStarted()
            true
        }
        config.complete("config")
        assertEquals(1, waiter().await())
    }
    @Test fun failingStopCallbackCannotMaskTheStartupFailureOrEscapeToMain() = runBlocking {
        val logs = mutableListOf<String>()
        val owner = CoreStartup(scope, { false }, {}, { error("stopSelf rejected") }, { _, message -> logs.add(message) })
        val failure = IllegalStateException("foreground denied")
        owner.fail(failure)
        val result = runCatching { owner.awaitReady() }.exceptionOrNull()
        assertEquals("foreground denied", result?.message)
        assertTrue(logs.any { "stopSelf rejected" in it })
    }

    @Test fun restartClosesOldReadinessAndCancelledWaiterDoesNotCancelRecovery() = runBlocking {
        nativeReady = true; start(); config.complete("config"); startup.awaitReady()
        nativeReady = false; startup.restarting()
        val retired = waiter(); val active = waiter()
        assertFalse(active.isCompleted); retired.cancel()
        nativeReady = true; startup.coreStarted()
        assertEquals(2, active.await()); assertTrue(retired.isCancelled)
        assertEquals(0, failures); assertEquals(1, submitted.size)
    }

    @Test fun awaitClosesReadinessBeforeDelayedStoppedEventArrives() = runBlocking {
        nativeReady = true; start(); config.complete("config"); startup.awaitReady()
        nativeReady = false
        val pending = waiter(); assertFalse(pending.isCompleted)
        startup.coreStopped()
        nativeReady = true; startup.coreStarted()
        assertEquals(2, pending.await()); assertEquals(0, failures)
    }

    @Test fun unexpectedCoreLossFailsRecoveryWaitersAndStopsTheService() = runBlocking {
        val owner = coordinator(timeout = 20)
        nativeReady = true; owner.start({ "config" }) { true }; owner.awaitReady()
        nativeReady = false; owner.coreStopped()
        val error = withTimeout(2_000) { runCatching { owner.awaitReady() }.exceptionOrNull() }
        withTimeout(2_000) { failed.await() }
        assertTrue(error is IllegalStateException); assertTrue(error!!.message!!.contains("recovery timeout"))
        assertEquals(1, failures)
    }

    @Test fun serviceStopCancelsPendingRecoveryWithoutASecondFailure() = runBlocking {
        val owner = coordinator(timeout = 20)
        nativeReady = true; owner.start({ "config" }) { true }; owner.awaitReady()
        nativeReady = false; owner.restarting(); owner.stop()
        assertTrue(runCatching { owner.awaitReady() }.exceptionOrNull() is IllegalStateException)
        delay(40); nativeReady = true; owner.coreStarted()
        assertEquals(1, readyCalls); assertEquals(0, failures)
    }

    @Test fun recoveryMonitorFailurePropagatesWithoutEscapingItsCoroutine() = runBlocking {
        nativeReady = true; start(); config.complete("config"); startup.awaitReady()
        nativeReady = false; startup.restarting()
        val pending = waiter(); readinessError = IllegalStateException("recovery monitor failed")
        nativeReady = true; startup.coreStarted()
        assertEquals("recovery monitor failed", runCatching { pending.await() }.exceptionOrNull()?.message)
        assertEquals(1, failures)
    }
}
