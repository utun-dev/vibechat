package com.utun.chat.data

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/** Схема настоящего JSON chat_feedback_snapshot; Автор задаётся полным Ed25519. */
class FeedbackTest {
    private fun snapshot() = JSONObject().put("message_id", "ab".repeat(32)).put("pending", false)
        .put("reactions", JSONArray()).put("poll", JSONObject.NULL)
    private fun poll() = JSONObject("""{
        "mode":"multiple","allow_custom_answer":true,"question":"Q",
        "options":[{"id":0,"text":"A","count":2,"percent":100.0},{"id":1,"text":"B","count":1,"percent":50.0}],
        "voters":2,"custom_count":1,"ver":17,"mine":{"choices":[0,1],"custom":""},
        "answers":[{"author_id":"a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1","ver":17,"answer":{"choices":[0,1],"custom":""}},
            {"author_id":"b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2","ver":3,"answer":{"choices":[0],"custom":"extra"}}]
    }""")

    @Test fun pendingMeansTheOriginalMessageIsMissing() {
        val parsed = parseFeedback(snapshot().put("pending", true))
        assertTrue(parsed.pending); assertNull(parsed.poll); assertTrue(parsed.reactions.isEmpty())
    }

    @Test fun reactionAndMultiplePollPreserveCoreCountsAndOwnAnswerVersion() {
        val json = snapshot().put("poll", poll()).put("reactions", JSONArray().put(JSONObject()
            .put("id", 2).put("emoji", "❤️").put("count", 7).put("mine", true)))
        val parsed = parseFeedback(json)
        assertEquals(ReactionState(2, "❤️", 7, true), parsed.reactions.single())
        val state = requireNotNull(parsed.poll)
        assertTrue(state.multiple); assertTrue(state.allowCustom); assertEquals(17L, state.ver)
        assertEquals(listOf(0, 1), state.myChoices); assertEquals(2, state.voters)
        assertEquals("b2".repeat(32), state.answers[1].authorId); assertEquals("extra", state.answers[1].custom)
        assertEquals(150.0, state.options.sumOf { it.percent }, 0.0)
    }

    @Test fun optionalChoiceAndCustomFieldsDefaultToAnEmptyAnswer() {
        val value = poll().put("mine", JSONObject())
        value.getJSONArray("answers").getJSONObject(0).put("answer", JSONObject())
        val parsed = requireNotNull(parseFeedback(snapshot().put("poll", value)).poll)
        assertTrue(parsed.myChoices.isEmpty()); assertEquals("", parsed.myCustom)
        assertTrue(parsed.answers[0].choices.isEmpty()); assertEquals("", parsed.answers[0].custom)
    }

    @Test fun malformedSnapshotDoesNotBecomeAnEmptySuccessfulPoll() {
        assertThrows(Exception::class.java) { parseFeedback(snapshot().put("poll", "invalid")) }
        for (id in listOf("a".repeat(63), "G".repeat(64), "../" + "a".repeat(61))) {
            assertThrows(IllegalArgumentException::class.java) { parseFeedback(snapshot().put("message_id", id)) }
        }
    }

    @Test fun unknownModeAndInvalidAuthorKeysFailForTheCallerToReport() {
        assertThrows(IllegalArgumentException::class.java) { parseFeedback(snapshot().put("poll", poll().put("mode", "other"))) }
        for (node in listOf("0".repeat(64), "ab".repeat(31), "AB".repeat(32), "invalid")) {
            val value = poll()
            value.getJSONArray("answers").getJSONObject(0).put("author_id", node)
            assertThrows(IllegalArgumentException::class.java) { parseFeedback(snapshot().put("poll", value)) }
        }
    }
}
