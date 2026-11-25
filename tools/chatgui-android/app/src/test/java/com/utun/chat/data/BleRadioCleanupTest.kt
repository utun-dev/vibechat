package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class BleRadioCleanupTest {
    @Test fun failingReleaseRemainsOwnedWhileOtherResourcesClose() {
        val messages = mutableListOf<String>()
        val cleanup = BleRadioCleanup { level, message -> messages.add("$level $message") }
        var unavailable = true
        var failedCalls = 0
        var otherCalls = 0
        val first = Any()
        cleanup.retain(first, "scan") { failedCalls++; if (unavailable) throw SecurityException("permission revoked") }
        cleanup.retain(Any(), "gatt") { otherCalls++ }
        assertFalse(cleanup.retry())
        assertEquals(1, failedCalls); assertEquals(1, otherCalls)
        assertFalse(cleanup.isEmpty)
        assertTrue(messages.any { it.contains("ERROR") && it.contains("scan") && it.contains("permission revoked") })
        cleanup.retain(first, "replacement") { fail("Original owner replaced") }
        unavailable = false
        assertTrue(cleanup.retry())
        assertEquals(2, failedCalls); assertEquals(1, otherCalls)
        assertTrue(cleanup.isEmpty)
        assertTrue(cleanup.retry()); assertEquals(2, failedCalls)
    }
    @Test fun equalSdkObjectsKeepIndependentOwnership() {
        val cleanup = BleRadioCleanup { _, _ -> }
        val first = String(charArrayOf('x')); val second = String(charArrayOf('x'))
        assertNotSame(first, second)
        var released = 0
        cleanup.retain(first, "first") { released++ }
        cleanup.retain(second, "second") { released++ }
        assertTrue(cleanup.retry())
        assertEquals(2, released)
    }
}
