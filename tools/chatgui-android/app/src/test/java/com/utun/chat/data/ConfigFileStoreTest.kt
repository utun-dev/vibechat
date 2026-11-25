package com.utun.chat.data

import java.io.File
import java.io.IOException
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** Настоящие coroutine и файловая система: порядок, владение запросом и целостность файла для стороннего читателя. */
class ConfigFileStoreTest {
    @get:Rule val directory = TemporaryFolder()
    private val logs = Collections.synchronizedList(mutableListOf<Pair<String, String>>())
    private val store = ConfigFileStore { level, message -> logs.add(level to message) }

    @Test fun operationsKeepSubmissionOrderAndRunOutsideCallerThread() = runBlocking {
        val caller = Thread.currentThread()
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val order = mutableListOf<Int>()
        val first = store.submit("first") {
            assertNotSame(caller, Thread.currentThread())
            entered.countDown()
            check(release.await(2, TimeUnit.SECONDS))
            order.add(1)
        }
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            val second = store.submit("second") { order.add(2) }
            val third = store.submit("third") { order.add(3) }
            assertTrue(order.isEmpty())
            release.countDown()
            first.await(); second.await(); third.await()
            assertEquals(listOf(1, 2, 3), order)
        } finally { release.countDown() }
    }

    @Test fun waitingScreenCancellationDoesNotCancelAcceptedSave() = runBlocking {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val file = File(directory.root, "utun.cfg")
        val first = store.submit("blocked") { entered.countDown(); check(release.await(2, TimeUnit.SECONDS)) }
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            lateinit var save: Deferred<Unit>
            val screen = launch(start = CoroutineStart.UNDISPATCHED) {
                save = store.submit("save") { replace(file, "complete") }
                save.await()
            }
            screen.cancelAndJoin()
            assertFalse(save.isCancelled)
            release.countDown()
            first.await(); save.await()
            assertEquals("complete", file.readText())
        } finally { release.countDown() }
    }

    @Test fun failedOperationIsLoggedWithoutCancellingNextRequest() = runBlocking {
        val failed = store.submit<Unit>("failure") { throw IOException("fixture failure") }
        assertTrue(runCatching { failed.await() }.exceptionOrNull() is IOException)
        assertEquals(7, store.submit("next") { 7 }.await())
        assertTrue(logs.any { it.first == "ERROR" && it.second.contains("fixture failure") })
    }

    @Test fun readersSeeOnlyWholeUtf8Versions() = runBlocking {
        val file = File(directory.root, "utun.cfg")
        val versions = listOf("A\n" + "Привет 👋\n".repeat(4096) + "A\n", "B\n" + "Другой текст ✨\n".repeat(4096) + "B\n")
        store.submit("initial") { replace(file, versions[0]) }.await()
        val stop = AtomicBoolean()
        val samples = AtomicInteger()
        val failure = AtomicReference<Throwable?>()
        val entered = CountDownLatch(1)
        val reader = Thread {
            try {
                while (!stop.get()) {
                    val text = file.readText()
                    check(text in versions) { "partial or mixed config observed" }
                    samples.incrementAndGet()
                    entered.countDown()
                }
            } catch (e: Throwable) { failure.set(e); entered.countDown() }
        }
        reader.start()
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            repeat(24) { index -> store.submit("replace") { replace(file, versions[index % 2]) }.await() }
        } finally { stop.set(true); reader.join(2_000) }
        assertFalse(reader.isAlive)
        assertNull(failure.get())
        assertTrue(samples.get() > 0)
        assertTrue(file.readText() in versions)
        assertEquals(listOf("utun.cfg"), directory.root.list()!!.toList())
    }

    @Test fun failedRenamePreservesDestinationAndRemovesTemporaryFile() = runBlocking {
        val target = directory.newFolder("utun.cfg")
        val sentinel = File(target, "old")
        sentinel.writeText("preserved")
        val failed = store.submit("replace-directory") { replace(target, "new config") }
        assertTrue(runCatching { failed.await() }.exceptionOrNull() is IOException)
        assertEquals("preserved", sentinel.readText())
        assertEquals(listOf("utun.cfg"), directory.root.list()!!.toList())
        assertTrue(logs.any { it.first == "ERROR" })
    }

    @Test fun missingParentIsCreatedAndContentIsNotLogged() = runBlocking {
        val target = File(directory.root, "new-db/utun.cfg")
        val text = "my_private_key=synthetic-secret\n"
        store.submit("save") { replace(target, text) }.await()
        assertEquals(text, target.readText())
        assertTrue(logs.any { it.second.contains("config file committed") })
        assertTrue(logs.none { it.second.contains("synthetic-secret") })
    }
}
