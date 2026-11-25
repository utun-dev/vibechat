package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

/** Те же draft-функции вызывает Compose; лимиты и смысл выбора соответствуют chat_poll_codec/chat_poll. */
class PollDraftTest {
    private fun poll(multiple: Boolean = false, custom: Boolean = true) = PollState("Question", multiple, custom,
        listOf(PollOption(0, "A", 0, 0.0), PollOption(1, "B", 0, 0.0)), 0, 0, emptyList(), "", emptyList())

    @Test fun inactiveCustomTextDoesNotBlockAStandardSingleChoice() {
        assertEquals(PollVoteDraft(listOf(1), ""), preparePollVote(poll(), listOf(1), "x".repeat(1025), false))
        assertEquals(PollVoteDraft(listOf(0), ""), preparePollVote(poll(custom = false), listOf(0), "x".repeat(1025), false))
    }

    @Test fun publishedCustomTextIsTrimmedBeforeItsUtf8LimitIsChecked() {
        val text = " " + "я".repeat(512) + " "
        assertEquals(PollVoteDraft(emptyList(), "я".repeat(512)), preparePollVote(poll(), emptyList(), text, true))
        assertNull(preparePollVote(poll(), emptyList(), "я".repeat(513), true))
        assertNull(preparePollVote(poll(), emptyList(), " \t ", true))
    }

    @Test fun singleCustomAndMultipleAnswersUseOnlyTheirActiveControls() {
        assertEquals(PollVoteDraft(emptyList(), "Mine"), preparePollVote(poll(), listOf(1), "Mine", true))
        assertEquals(PollVoteDraft(listOf(0, 1), "Mine"), preparePollVote(poll(true), listOf(0, 1), " Mine ", false))
        assertEquals(PollVoteDraft(emptyList(), ""), preparePollVote(poll(), emptyList(), "", false))
        assertNull(preparePollVote(poll(), listOf(0, 1), "", false))
        assertNull(preparePollVote(poll(true), listOf(0, 0), "", false))
        assertNull(preparePollVote(poll(true), listOf(2), "", false))
    }

    @Test fun questionOptionsAndWholePollUseUtf8ByteLimits() {
        assertTrue(validPollCreation("я".repeat(256), listOf("A", "B")))
        assertFalse(validPollCreation("я".repeat(257), listOf("A", "B")))
        assertTrue(validPollCreation("Question", listOf("я".repeat(128), "B")))
        assertFalse(validPollCreation("Question", listOf("я".repeat(129), "B")))
        val options = (0..5).map { "$it" + "a".repeat(255) }
        assertTrue(validPollCreation("q".repeat(494), options)) // 6 + 494 + 6*(2+256) = 2048.
        assertFalse(validPollCreation("q".repeat(495), options))
    }

    @Test fun invalidTextAndOptionSetsAreRejectedBeforeNativeSubmission() {
        for (bad in listOf("a\u0000b", "a\u0001b", "a\uD800b")) {
            assertFalse(validPollCreation(bad, listOf("A", "B")))
            assertFalse(validPollCreation("Question", listOf(bad, "B")))
            assertNull(preparePollVote(poll(), emptyList(), bad, true))
        }
        assertFalse(validPollCreation("Q", listOf("", "B")))
        assertFalse(validPollCreation("Q", listOf("A", "A")))
        assertFalse(validPollCreation("Q", listOf("A")))
        assertFalse(validPollCreation("Q", (0..32).map(Int::toString)))
        assertTrue(validPollCreation("Q\tline\nnext\rline", listOf("😀", "B")))
    }
}
