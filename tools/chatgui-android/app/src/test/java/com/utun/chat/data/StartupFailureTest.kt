package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class StartupFailureTest {
    private fun packet(flag: Int, directory: String, message: String): ByteArray {
        val path = directory.toByteArray(); val text = message.toByteArray()
        return byteArrayOf(flag.toByte(), path.size.toByte(), (path.size shr 8).toByte()) + path + text
    }

    @Test fun recreationUsesStructuredDirectoryAndPreservesUtf8Reason() {
        val path = "/данные/profile"
        val message = "База несовместима: /другой/путь/chats.db"
        val failure = requireNotNull(StartupFailure.decode(packet(1, path, message)))
        assertEquals(path, failure.directory); assertEquals(message, failure.message)
        assertTrue(failure.canRecreate); assertFalse(failure.recreating)
    }

    @Test fun operationalErrorsAndMissingDirectoriesCannotOfferDeletion() {
        assertFalse(requireNotNull(StartupFailure.decode(packet(0, "/profile", "access denied"))).canRecreate)
        assertFalse(requireNotNull(StartupFailure.decode(packet(1, "", "schema differs"))).canRecreate)
    }

    @Test fun onlyInvalidKeysOfferReplacement() {
        val key = requireNotNull(StartupFailure.decode(packet(2, "/profile", "invalid key")))
        assertEquals(StartupIssue.INVALID_KEY, key.issue)
        assertFalse(key.canRecreate)
        val readError = requireNotNull(StartupFailure.decode(packet(3, "/profile", "read failed")))
        assertEquals(StartupIssue.KEY_ACCESS, readError.issue)
        assertFalse(readError.canRecreate)
    }

    @Test fun pendingConfirmationSurvivesNewUiAndCannotBeSubmittedTwice() {
        val failure = requireNotNull(StartupFailure.decode(packet(2, "/profile", "invalid key")))
        val pending = requireNotNull(StartupFailure.beginRecovery(failure, false))
        assertTrue(pending.recreating)
        assertNull(StartupFailure.beginRecovery(pending, false))
        assertNull(StartupFailure.beginRecovery(pending, true))
        val writeFailed = pending.copy(recreating = false, recoveryError = "write failed")
        val retry = requireNotNull(StartupFailure.beginRecovery(writeFailed, false))
        assertEquals(StartupIssue.INVALID_KEY, retry.issue)
        assertNull(retry.recoveryError)
        val saved = pending.copy(issue = StartupIssue.KEY_SAVED, recreating = false)
        assertEquals(StartupIssue.KEY_SAVED, StartupFailure.beginRecovery(saved, false)?.issue)
    }

    @Test fun stoppedSettingsRequestStartsOnlyAfterExplicitAction() {
        assertNull(StartupFailure.beginRecovery(null, false))
        val settings = requireNotNull(StartupFailure.beginRecovery(null, true))
        assertTrue(settings.fromSettings && settings.recreating)
        val database = requireNotNull(StartupFailure.decode(packet(1, "/profile", "schema")))
        assertNull(StartupFailure.beginRecovery(database, true))
        assertNull(StartupFailure.beginRecovery(database, false))
    }

    @Test fun malformedPayloadCannotAuthorizeDeletion() {
        for (data in listOf(null, byteArrayOf(1, 0), byteArrayOf(1, 255.toByte(), 127, 1),
            packet(4, "/profile", "failure"), packet(2, "", "invalid key"), packet(1, "/profile\u0000/wrong", "failure"), packet(1, "/profile", "")))
            assertNull(StartupFailure.decode(data))
    }
}
