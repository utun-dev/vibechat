package com.utun.chat.data

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.advanceTimeBy
import org.junit.Assert.*
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class PhoneMediaDispatcherTest {
    @Test fun waitsForReleaseAndIgnoresDuplicateAcceptance() = runTest {
        val dispatcher = PhoneMediaDispatcher(this)
        val released = CompletableDeferred<Unit>()
        var starts = 0
        dispatcher.enter(1, { released.await() }, { starts++ }, { throw it })
        dispatcher.enter(1, { error("duplicate release") }, { error("duplicate start") }, { throw it })
        runCurrent(); assertTrue(dispatcher.blocked); assertEquals(0, starts)
        released.complete(Unit); runCurrent()
        assertEquals(1, starts); assertTrue(dispatcher.blocked)
        dispatcher.cancel(1); dispatcher.leave(1); assertFalse(dispatcher.blocked)
    }

    @Test fun cancellationKeepsReleaseAliveAndCannotAffectNextCall() = runTest {
        val dispatcher = PhoneMediaDispatcher(this)
        val released = CompletableDeferred<Unit>()
        var starts = 0
        dispatcher.enter(1, { released.await() }, { starts++ }, { throw it })
        runCurrent(); dispatcher.cancel(1)
        assertTrue(dispatcher.blocked); assertFalse(dispatcher.job!!.isCompleted)
        released.complete(Unit); runCurrent(); dispatcher.leave(1)
        dispatcher.enter(2, {}, { starts++ }, { throw it }); runCurrent()
        dispatcher.cancel(1); dispatcher.leave(1)
        assertTrue(dispatcher.blocked); assertEquals(1, starts)
        dispatcher.cancel(2); dispatcher.leave(2)
    }

    @Test fun timeoutRetainsOwnersUntilReleaseActuallyCompletes() = runTest {
        val dispatcher = PhoneMediaDispatcher(this)
        val released = CompletableDeferred<Unit>()
        var failures = 0
        dispatcher.enter(1, { released.await() }, { error("late start") }, { failures++ })
        runCurrent(); advanceTimeBy(10_001); runCurrent()
        assertEquals(1, failures); assertTrue(dispatcher.blocked); assertFalse(dispatcher.job!!.isCompleted)
        released.complete(Unit); runCurrent(); dispatcher.leave(1)
        assertFalse(dispatcher.blocked)
    }

    @Test fun failedReleaseIsRetriedWithoutStartingPhone() = runTest {
        val dispatcher = PhoneMediaDispatcher(this)
        var attempts = 0
        var failures = 0
        dispatcher.enter(1, { if (attempts++ == 0) error("hardware retained") }, { error("failed call started") }, { failures++ })
        runCurrent(); assertTrue(dispatcher.blocked)
        advanceTimeBy(251); runCurrent()
        assertEquals(2, attempts); assertEquals(1, failures)
        dispatcher.leave(1); assertFalse(dispatcher.blocked)
    }
}
