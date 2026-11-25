package com.utun.chat.data

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.toList
import org.junit.Assert.*
import org.junit.Test

class SettingsOperationsTest {
    @Test fun acceptedSaveSurvivesUiCancellationAndRestartWaitsForItsCommit() = runTest {
        val owner = SettingsOperations(this) { _, _ -> }
        val committed = CompletableDeferred<Unit>(); val order = mutableListOf<String>()
        val save = owner.submit("name") { committed.await(); order.add("commit") }
        val second = owner.submit("runtime") { order.add("apply") }
        val ui = launch { save.await() }; ui.cancel()
        val restart = async { owner.awaitIdle(); order.add("restart") }
        assertTrue(order.isEmpty()); assertFalse(save.isCancelled)
        committed.complete(Unit); save.await(); second.await(); restart.await()
        assertEquals(listOf("commit", "apply", "restart"), order)
    }
    @Test fun failedPersistenceCannotRunTheFollowingRuntimeUpdateOrKillTheQueue() = runTest {
        val logs = mutableListOf<String>()
        val owner = SettingsOperations(CoroutineScope(coroutineContext + SupervisorJob())) { _, message -> logs.add(message) }
        var runtime = false
        val persist: suspend () -> Unit = { throw java.io.IOException("disk rejected") }
        val failed = owner.submit("volume") { persist(); runtime = true }
        assertTrue(runCatching { failed.await() }.exceptionOrNull() is java.io.IOException)
        assertFalse(runtime); assertTrue(owner.failed.value); assertTrue(logs.any { "disk rejected" in it })
        owner.submit("retry") { runtime = true }.await()
        assertTrue(runtime); owner.dismissFailure(); assertFalse(owner.failed.value)
    }
    @Test fun cancelledBarrierWaiterCannotCancelAnAcceptedChange() = runTest {
        val owner = SettingsOperations(this) { _, _ -> }
        val gate = CompletableDeferred<Unit>(); var saved = false
        val change = owner.submit("stereo") { gate.await(); saved = true }
        val waiter = launch { owner.awaitIdle() }; waiter.cancel()
        gate.complete(Unit); change.await(); assertTrue(saved); assertFalse(change.isCancelled)
    }
    @Test fun failedPreferenceObservationPreservesTheLastValueAndCanRecover() = runTest {
        val owner = SettingsOperations(this) { _, _ -> }; var attempts = 0
        val values = flow {
            if (++attempts == 1) { emit(42); throw java.io.IOException("read failed") }
            emit(43)
        }
        assertEquals(listOf(42, 43), owner.observe("preferences", values).take(2).toList())
        assertTrue(owner.failed.value); assertEquals(2, attempts)
    }
}
