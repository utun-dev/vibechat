package com.utun.chat.headless

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.*
import org.junit.Test

class CoreShutdownTest {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Unconfined)
    private val released = CompletableDeferred<Unit>()
    private var stops = 0
    private val logs = mutableListOf<Pair<String, String>>()
    private val shutdown = CoreShutdown(scope, { stops++; released.await() }, { level, message -> logs.add(level to message) })

    @After fun release() { released.complete(Unit); scope.cancel() }

    @Test fun newServiceWaitsForCompleteShutdown() = runBlocking {
        shutdown.request()
        val nextService = async(start = CoroutineStart.UNDISPATCHED) { shutdown.await(); "init" }
        assertEquals(1, stops)
        assertFalse(nextService.isCompleted)
        released.complete(Unit)
        assertEquals("init", nextService.await())
        assertTrue(logs.any { it.first == "INFO" && it.second.contains("completed") })
    }

    @Test fun repeatedStopRequestsShareOneOperation() = runBlocking {
        val first = shutdown.request()
        assertSame(first, shutdown.request())
        assertEquals(1, stops)
        released.complete(Unit)
        first.await()
    }

    @Test fun cancellingRetiredServiceDoesNotCancelCleanupOrNextService() = runBlocking {
        shutdown.request()
        val retired = async(start = CoroutineStart.UNDISPATCHED) { shutdown.await() }
        val replacement = async(start = CoroutineStart.UNDISPATCHED) { shutdown.await(); "ready" }
        retired.cancel()
        assertFalse(released.isCancelled)
        assertFalse(replacement.isCompleted)
        released.complete(Unit)
        assertEquals("ready", replacement.await())
    }

    @Test fun awaitWithoutPreviousServiceDoesNotRequestStop() = runBlocking {
        shutdown.await()
        assertEquals(0, stops)
    }

    @Test fun failedStopBlocksInitializationUntilSuccessfulRetry() = runBlocking {
        var attempts = 0
        val retryGate = CompletableDeferred<Unit>()
        val delays = mutableListOf<Long>()
        val retry = CoreShutdown(scope, { check(++attempts > 1) { "join failed" } },
            { level, message -> logs.add(level to message) }, { delays.add(it); retryGate.await() })
        val operation = retry.request()
        val replacement = async(start = CoroutineStart.UNDISPATCHED) { retry.await(); "init" }
        assertFalse(operation.isCompleted); assertFalse(replacement.isCompleted)
        assertSame(operation, retry.request()); assertEquals(1, attempts)
        assertTrue(logs.any { it.first == "ERROR" && it.second.contains("join failed") })
        retryGate.complete(Unit)
        operation.await(); assertEquals("init", replacement.await())
        retry.await()
        assertEquals(2, attempts); assertEquals(listOf(250L), delays)
    }

    @Test fun cancelledSettingsWaiterCannotInterruptAutomaticShutdownRetry() = runBlocking {
        val gate = CompletableDeferred<Unit>(); var attempts = 0
        val retry = CoreShutdown(scope, { check(++attempts > 1) { "release pending" } }, { _, _ -> }, { gate.await() })
        val operation = retry.request()
        val settings = async(start = CoroutineStart.UNDISPATCHED) { retry.await() }
        settings.cancel(); assertFalse(operation.isCancelled); assertEquals(1, attempts)
        gate.complete(Unit); operation.await(); assertEquals(2, attempts)
    }

    @Test fun repeatedFailuresUseBoundedBackoffWithoutRequiringAnotherService() = runBlocking {
        val delays = mutableListOf<Long>(); var attempts = 0
        val retry = CoreShutdown(scope, { check(++attempts > 12) { "release pending" } }, { _, _ -> }, { delays.add(it) })
        retry.request().await()
        assertEquals(13, attempts); assertEquals(250L, delays.first()); assertEquals(30_000L, delays.last())
        assertTrue(delays.all { it in 250L..30_000L })
    }
}
