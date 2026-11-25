package com.utun.chat.data

import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.*
import org.junit.Test

/** Настоящая история экрана логов: переполнение, открытие/задержка подписчика и конкурентные производители. */
class LogHistoryTest {
    private fun entry(message: String) = LogEntry(timestamp = 1, level = "INFO", category = "test", message = message)

    @Test fun boundedHistoryKeepsLatestEntriesInReceptionOrder() {
        val history = LogHistory(3)
        repeat(5) { history.add(entry("$it")) }
        assertEquals(listOf("2", "3", "4"), history.getRecent(10).map { it.message })
        assertEquals(listOf("3", "4"), history.getRecent(2).map { it.message })
        assertTrue(history.getRecent(0).isEmpty())
        assertEquals(5L, history.revision.value)
    }

    @Test fun snapshotRemainsUnchangedWhileOldRowsAreEvicted() {
        val history = LogHistory(2)
        history.add(entry("before"))
        val snapshot = history.getRecent(2)
        repeat(20) { history.add(entry("after-$it")) }
        assertEquals(listOf("before"), snapshot.map { it.message })
        assertEquals(listOf("after-18", "after-19"), history.getRecent(2).map { it.message })
    }

    @Test fun identicalMessagesWithTheSameTimestampStillAdvanceRevision() {
        val history = LogHistory(4)
        val repeated = entry("same")
        repeat(4) { history.add(repeated) }
        assertEquals(4L, history.revision.value)
        assertEquals(List(4) { repeated }, history.getRecent(4))
    }

    @Test fun subscriberOpeningAfterLastLogSeesHistoryWithoutAnotherMessage() = runBlocking {
        val history = LogHistory(4)
        history.add(entry("already buffered"))
        assertEquals(1L, withTimeout(2000) { history.revision.first() })
        assertEquals("already buffered", history.getRecent(1).single().message)
    }

    @Test fun stalledSubscriberRecoversBurstBeyondTheOld512EventQueue() = runBlocking {
        val history = LogHistory(4096)
        val resume = CompletableDeferred<Unit>()
        val observed = CompletableDeferred<Pair<Long, List<LogEntry>>>()
        val reader = launch(start = CoroutineStart.UNDISPATCHED) {
            history.revision.collect { revision ->
                if (revision == 0L) resume.await()
                else observed.complete(revision to history.getRecent(4096))
            }
        }
        try {
            repeat(2048) { history.add(entry("$it")) }
            resume.complete(Unit)
            val (revision, snapshot) = withTimeout(2000) { observed.await() }
            assertEquals(2048L, revision)
            assertEquals((0 until 2048).map(Int::toString), snapshot.map { it.message })
        } finally { reader.cancelAndJoin() }
    }

    @Test fun concurrentWritersPreserveEveryEntryAndTheirOwnOrder() {
        val writers = 6
        val count = 1024
        val history = LogHistory(writers * count)
        val start = CountDownLatch(1)
        val executor = Executors.newFixedThreadPool(writers)
        try {
            val tasks = (0 until writers).map { writer ->
                executor.submit {
                    check(start.await(5, TimeUnit.SECONDS))
                    repeat(count) { history.add(entry("$writer:$it")) }
                }
            }
            start.countDown()
            tasks.forEach { it.get(5, TimeUnit.SECONDS) }
            val messages = history.getRecent(writers * count).map { it.message }
            assertEquals((writers * count).toLong(), history.revision.value)
            assertEquals(writers * count, messages.toSet().size)
            repeat(writers) { writer ->
                assertEquals((0 until count).map { "$writer:$it" }, messages.filter { it.startsWith("$writer:") })
            }
        } finally { executor.shutdownNow() }
    }

    @Test fun invalidBoundsAreRejectedAndEmptyHistoryIsReadable() {
        for (capacity in listOf(0, -1)) assertThrows(IllegalArgumentException::class.java) { LogHistory(capacity) }
        val history = LogHistory(1)
        assertTrue(history.getRecent(10).isEmpty())
        assertEquals(0L, history.revision.value)
        assertThrows(IllegalArgumentException::class.java) { history.getRecent(-1) }
    }
}
