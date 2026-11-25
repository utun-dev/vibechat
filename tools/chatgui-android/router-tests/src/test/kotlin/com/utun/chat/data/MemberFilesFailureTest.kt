package com.utun.chat.data

import android.content.Context
import android.net.Uri
import android.os.Looper
import com.utun.chat.R
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import kotlinx.coroutines.*
import kotlinx.coroutines.test.*
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий repository; только URI, wake lock и JNI задают управляемые отказы/завершения. */
@OptIn(ExperimentalCoroutinesApi::class)
class MemberFilesFailureTest {
    private val dispatcher = StandardTestDispatcher()
    private lateinit var context: Context
    private lateinit var root: File
    private lateinit var repository: MemberFilesRepository
    private val location = FileLocation("07".repeat(32), "08".repeat(32), NativeLib.getMySigningAddress(), "docs")
    private val uri = Uri.parse("content://documents/output")

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        Dispatchers.setMain(dispatcher)
        root = Files.createTempDirectory("utun-member-files-host-").toFile()
        context = Context().also { it.cacheDir = root }
        NativeLib.ready = false
        CoreActivity.visible(false); CoreActivity.call(false); CoreActivity.radioCall(false); CoreActivity.transfer(false); CoreActivity.reset()
        NativeLib.ready = true; NativeLib.failActivityRequest = false; NativeLib.acceptActivityRequest = true
        NativeLib.activityRequests.clear(); NativeLib.fileTransfers.clear(); NativeLib.fileCancels.clear()
        NativeLib.fileRequest = { _, _, _ -> true }; NativeLib.acceptFileTransfer = true
        LogManager.entries.clear()
        repository = MemberFilesRepository(context)
    }

    @After fun release() {
        NativeLib.failActivityRequest = false
        context.powerManager!!.locks.forEach { it.failures = emptySet() }
        repository.coreStopped()
        dispatcher.scheduler.runCurrent()
        context.powerManager!!.locks.filter { it.held }.forEach { it.release() }
        CoreActivity.transfer(false)
        Dispatchers.resetMain()
        root.deleteRecursively()
    }

    private fun pumpUntil(done: () -> Boolean) {
        repeat(1000) {
            dispatcher.scheduler.runCurrent()
            if (done()) return
            Thread.sleep(2)
        }
        fail("repository did not finish: ${repository.operation.value}")
    }
    private fun finished() = repository.operation.value?.active == false
    private fun diagnostic(text: String) = assertTrue("missing diagnostic: $text",
        LogManager.entries.any { it.level in listOf("WARN", "ERROR") && text in it.message })
    private fun export(): FileExport {
        val file = File(root, "ready.bin").also { it.writeBytes(byteArrayOf(1, 2, 3)) }
        return FileExport(91, location, file.name, file, MemberFilesRepository.SAVE)
    }
    private fun terminal(transfer: NativeLib.FileTransfer, status: Int) = ByteBuffer.allocate(123).order(ByteOrder.LITTLE_ENDIAN).apply {
        putLong(transfer.id); put(MemberFilesProtocol.addressBytes(transfer.group)); put(MemberFilesProtocol.addressBytes(transfer.server)); put(MemberFilesProtocol.addressBytes(transfer.owner))
        putLong(0); putLong(0); putShort(status.toShort()); put(2)
    }.array()

    @Test fun backgroundCallerIsRejectedWithADiagnostic() {
        val error = java.util.concurrent.atomic.AtomicReference<Throwable?>()
        val worker = Thread({ try { repository.cancel() } catch (e: Throwable) { error.set(e) } }, "files-test-worker")
        worker.start(); worker.join(2_000)
        assertFalse(worker.isAlive)
        assertTrue(error.get() is IllegalStateException)
        diagnostic("requires main thread: caller=files-test-worker")
        assertNull(repository.operation.value)
    }

    @Test fun rejectedRequestAndTimeoutAreExplicitAndCancelNativeWaiter() = runTest(dispatcher) {
        NativeLib.fileRequest = { _, _, _ -> false }
        assertTrue(runCatching { repository.request(location, MemberFilesProtocol.LIST) }.exceptionOrNull() is IOException)
        assertEquals(1, NativeLib.fileCancels.size)
        NativeLib.fileRequest = { _, _, _ -> true }
        val result = async { runCatching { repository.request(location, MemberFilesProtocol.LIST) } }
        runCurrent(); advanceTimeBy(60_000); runCurrent()
        assertEquals(13, (result.await().exceptionOrNull() as FileStatusException).status)
        assertEquals(2, NativeLib.fileCancels.size)
        diagnostic("request timed out")
    }

    @Test fun rejectedTransferRemovesOwnedDestinationAndReleasesActivity() = runTest(dispatcher) {
        NativeLib.acceptFileTransfer = false
        repository.download(location, MemberFilesProtocol.Entry("remote.bin", 1, 3, 0, ByteArray(16) { 1 }), 0)
        pumpUntil(::finished)
        assertEquals(9, repository.operation.value?.status)
        assertFalse(File(NativeLib.fileTransfers.single().local).parentFile!!.exists())
        assertFalse(context.powerManager!!.locks.single().held)
        assertEquals(listOf(true, false), NativeLib.activityRequests)
        diagnostic("transfer enqueue failed")
    }

    @Test fun cancellationRetainsDestinationUntilNativeClosedItsHandle() = runTest(dispatcher) {
        repository.download(location, MemberFilesProtocol.Entry("remote.bin", 1, 3, 0, ByteArray(16) { 1 }), 0)
        pumpUntil { NativeLib.fileTransfers.isNotEmpty() }
        val transfer = NativeLib.fileTransfers.single()
        val directory = File(transfer.local).parentFile!!
        repository.cancel(); runCurrent()
        assertEquals(listOf(transfer.id), NativeLib.fileCancels)
        assertTrue(directory.exists()); assertTrue(repository.operation.value!!.active)
        repository.onEvent(50, terminal(transfer, MemberFilesProtocol.CANCELLED))
        pumpUntil(::finished)
        assertFalse(directory.exists()); assertTrue(repository.operation.value!!.cancelled)
        assertFalse(context.powerManager!!.locks.single().held)
    }

    @Test fun failedDocumentWriteDeletesPartialProviderDocumentAndKeepsSource() = runTest(dispatcher) {
        val prepared = export()
        var deleted: Uri? = null
        context.contentResolver.output = { throw IOException("provider write rejected") }
        context.contentResolver.delete = { deleted = it; true }
        repository.save(prepared, uri)
        pumpUntil(::finished)
        assertEquals(uri, deleted); assertTrue(prepared.file.isFile)
        assertEquals(9, repository.operation.value?.status)
        assertFalse(repository.operation.value!!.retryable)
        assertFalse(context.powerManager!!.locks.single().held)
        assertEquals(listOf(true, false), NativeLib.activityRequests)
        diagnostic("provider write rejected")
    }

    @Test fun acquireFailureAfterSideEffectStillReleasesOwnedLock() = runTest(dispatcher) {
        context.powerManager!!.failures = setOf("acquire")
        repository.save(export(), uri)
        pumpUntil(::finished)
        assertEquals(9, repository.operation.value?.status)
        assertFalse("failed acquire lost an already-held lock", context.powerManager!!.locks.single().held)
        diagnostic("wake lock acquire unavailable")
    }

    @Test fun releaseFailureDoesNotSkipStandbyCleanupAndNextOperationRetriesLock() = runTest(dispatcher) {
        context.powerManager!!.failures = setOf("release")
        repository.save(export(), uri)
        pumpUntil(::finished)
        val lock = context.powerManager!!.locks.single()
        assertFalse("one repository owner must allow repeated release without reference-count underflow", lock.counted)
        assertTrue(lock.held)
        assertEquals(listOf(true, false), NativeLib.activityRequests)
        diagnostic("wake lock release failed")
        lock.failures = emptySet()
        repository.save(export(), uri)
        pumpUntil(::finished)
        assertEquals(1, context.powerManager!!.locks.size)
        assertFalse(lock.held)
    }

    @Test fun coreStopRetriesWakeReleaseForAnAlreadyFinishedOperation() = runTest(dispatcher) {
        context.powerManager!!.failures = setOf("release")
        repository.save(export(), uri)
        pumpUntil(::finished)
        val lock = context.powerManager!!.locks.single()
        assertTrue(lock.held)
        lock.failures = emptySet()
        repository.coreStopped()
        assertFalse("core stop left a completed operation's lock held", lock.held)
    }

    @Test fun standbyFailureDuringCleanupIsLoggedWithoutLosingCompletedExport() = runTest(dispatcher) {
        context.contentResolver.output = { NativeLib.failActivityRequest = true; java.io.ByteArrayOutputStream() }
        repository.save(export(), uri)
        pumpUntil(::finished)
        assertEquals(R.string.files_completed, repository.operation.value?.stage)
        assertFalse(context.powerManager!!.locks.single().held)
        diagnostic("activity release failed")
        NativeLib.failActivityRequest = false
        CoreActivity.sync()
        assertEquals(listOf(true, false), NativeLib.activityRequests)
    }
}
