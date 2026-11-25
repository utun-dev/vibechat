package com.utun.chat.data

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.*
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ConversationSubmissionsTest {
    private val target = ConversationTarget.Pm("123", 2)

    @Test fun losingTheUiWaiterCannotCancelTheBatchOrItsAcknowledgements() = runTest {
        val owner = ConversationSubmissions(this) { _, _ -> }
        val attachment = CompletableDeferred<Boolean>(); val text = CompletableDeferred<Boolean>()
        var textStarted = false; var repliesFinished = 0
        assertTrue(owner.submit(target, "a", { attachment.await() }, { textStarted = true; text.await() }, { repliesFinished++ }, {}))
        val ui = launch { attachment.await() }; runCurrent(); ui.cancel()
        assertFalse(owner.states.value.getValue(target).finished)
        assertFalse(owner.submit(target, "duplicate", null, { true }, {}, {}))
        attachment.complete(true); runCurrent()
        assertTrue(textStarted); assertEquals(true, owner.states.value.getValue(target).attachmentSaved)
        assertNull(owner.states.value.getValue(target).textSaved)
        text.complete(true); runCurrent()
        assertEquals(1, repliesFinished); assertTrue(owner.states.value.getValue(target).finished)
        owner.consume(target, "stale"); assertTrue(owner.states.value.containsKey(target))
        owner.consume(target, "a"); assertTrue(owner.states.value.isEmpty())
    }

    @Test fun failureKeepsTheUnsentPartAndDoesNotFinishReply() = runTest {
        val logs = mutableListOf<String>()
        val owner = ConversationSubmissions(this) { _, message -> logs.add(message) }
        var errors = 0; var replied = false
        owner.submit(target, "a", { true }, { throw IllegalStateException("database") }, { replied = true }, { errors++ })
        runCurrent()
        val result = owner.states.value.getValue(target)
        assertTrue(result.finished); assertEquals(true, result.attachmentSaved); assertEquals(false, result.textSaved)
        assertFalse(replied); assertEquals(1, errors); assertTrue(logs.any { "database" in it })
    }

    @Test fun oldGenerationAndOldReceiptCannotConsumeANewSubmission() = runTest {
        val owner = ConversationSubmissions(this) { _, _ -> }
        owner.submit(target, "a", null, { true }, {}, {})
        val reopened = ConversationTarget.Pm("123", 3)
        val pending = CompletableDeferred<Boolean>()
        owner.submit(reopened, "b", null, { pending.await() }, {}, {})
        owner.consume(reopened, "a"); owner.consume(target, "a")
        assertFalse(owner.states.value.getValue(reopened).finished)
        pending.complete(true); runCurrent(); owner.consume(reopened, "b")
        assertTrue(owner.states.value.isEmpty())
    }

    @Test fun stoppedOwnerDoesNotAcceptFilesOrRunTasks() = runTest {
        val job = Job().also { it.cancel() }
        val owner = ConversationSubmissions(CoroutineScope(coroutineContext + job)) { _, _ -> }
        var ran = false
        assertFalse(owner.submit(target, "a", { ran = true; true }, null, {}, {}))
        assertFalse(ran); assertTrue(owner.states.value.isEmpty())
    }

    @Test fun cardsInTheSameBoardKeepSeparateFilesDraftsAndReceipts() = runTest {
        val owner = ConversationSubmissions(this) { _, _ -> }
        val first = ConversationTarget.Card("7", "a".repeat(64))
        val second = ConversationTarget.Card("7", "b".repeat(64))
        val file = CompletableDeferred<Boolean>()
        var firstTextSent = false
        assertTrue(owner.submit(first, "first", { file.await() }, { firstTextSent = true; true }, {}, {}))
        assertTrue(owner.submit(second, "second", { true }, { false }, {}, {}))
        runCurrent()
        assertFalse(firstTextSent)
        assertFalse(owner.states.value.getValue(first).finished)
        assertEquals(true, owner.states.value.getValue(second).attachmentSaved)
        assertEquals(false, owner.states.value.getValue(second).textSaved)
        owner.consume(first, "second")
        assertTrue(owner.states.value.containsKey(first))
        file.complete(true); runCurrent()
        assertTrue(firstTextSent)
        assertEquals(true, owner.states.value.getValue(first).textSaved)
        owner.consume(first, "first")
        assertEquals(setOf(second), owner.states.value.keys)
    }
}
