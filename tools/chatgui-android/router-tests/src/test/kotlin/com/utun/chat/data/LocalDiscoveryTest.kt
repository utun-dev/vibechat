package com.utun.chat.data

import android.content.Context
import android.os.Looper
import com.utun.chat.ChatApplication
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicReference
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий LAN state holder: бинарные события C, ACK, native-отказы и жизненный цикл MulticastLock. */
class LocalDiscoveryTest {
    private lateinit var context: Context
    private val device = LocalDevice(address(-1L).toHexString(), "Телефон 😀")
    private val link = "utun://opaque-invitation-secret"
    private fun active(value: Boolean) = LocalDiscovery.onEvent(64, byteArrayOf(if (value) 1 else 0))
    private fun address(id: Long): ByteArray = ByteBuffer.allocate(33).put(ByteArray(24)).putLong(id).put(0).array()
    private fun row(id: Long, name: ByteArray): ByteArray = ByteBuffer.allocate(34 + name.size)
        .put(address(id)).put(name.size.toByte()).put(name).array()
    private fun devices(vararg rows: ByteArray): ByteArray =
        ByteBuffer.allocate(2).order(ByteOrder.BIG_ENDIAN).putShort(rows.size.toShort()).array() + rows.fold(byteArrayOf()) { a, b -> a + b }
    private fun invite(id: Long = -1L, name: ByteArray = device.name.toByteArray(), text: ByteArray = link.toByteArray()) =
        row(id, name) + text
    private fun result(request: Long, peer: Long = -1L, status: Int = 1) =
        ByteBuffer.allocate(42).putLong(request).put(address(peer)).put(status.toByte()).array()
    private fun diagnostic(fragment: String) = assertTrue("missing diagnostic $fragment: ${LogManager.entries.takeLast(8)}",
        LogManager.entries.any { it.level in listOf("WARN", "ERROR") && fragment in it.message })

    @Before fun prepare() {
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        context = Context(); ChatApplication.instance = context
        NativeLib.localInvites.clear(); NativeLib.sendLocal = { true }
        LocalDiscovery.onEvent(13, null); LocalDiscovery.clearDelivery()
        while (LocalDiscovery.invitations.value.isNotEmpty()) LocalDiscovery.dismissInvitation()
        LogManager.entries.clear()
    }

    @After fun release() {
        context.failWifiService = false
        context.wifiManager?.locks?.forEach { it.failures = emptySet() }
        LocalDiscovery.onEvent(13, null); LocalDiscovery.clearDelivery()
        context.wifiManager?.locks?.filter { it.held }?.forEach { it.release() }
        NativeLib.sendLocal = { true }
    }

    @Test fun devicesPreserveFullAddressesAndUtf8AndSortByLabel() {
        LocalDiscovery.onEvent(61, devices(row(-1L, device.name.toByteArray()), row(2, "A".toByteArray())))
        assertEquals(listOf(2L, -1L).map { address(it).toHexString() }, LocalDiscovery.devices.value.map { it.address })
        assertEquals("Телефон 😀 [ffffff]", LocalDiscovery.devices.value.last().label)
    }

    @Test fun malformedDeviceFramesPreservePreviousSnapshot() {
        val valid = devices(row(9, "kept".toByteArray()))
        LocalDiscovery.onEvent(61, valid)
        val original = LocalDiscovery.devices.value
        for (bad in listOf(null, byteArrayOf(), byteArrayOf(0), byteArrayOf(0, -127), valid.dropLast(1).toByteArray(), valid + 0)) {
            LocalDiscovery.onEvent(61, bad)
            assertEquals(original, LocalDiscovery.devices.value)
        }
        diagnostic("event")
    }

    @Test fun invalidNamesIdsAndDuplicateDevicesCannotReplaceGoodState() {
        LocalDiscovery.onEvent(61, devices(row(9, "kept".toByteArray())))
        val original = LocalDiscovery.devices.value
        val invalid = listOf(byteArrayOf(), byteArrayOf(0), byteArrayOf(0x1f), byteArrayOf(0x7f), byteArrayOf(-1),
            byteArrayOf(0xc0.toByte(), 0xaf.toByte()), byteArrayOf(0xed.toByte(), 0xa0.toByte(), 0x80.toByte()),
            "я".repeat(64).toByteArray())
        for (name in invalid) {
            LocalDiscovery.onEvent(61, devices(row(1, name)))
            assertEquals("name=${name.toList()}", original, LocalDiscovery.devices.value)
        }
        LocalDiscovery.onEvent(61, devices(row(0, "zero".toByteArray())))
        assertEquals(original, LocalDiscovery.devices.value)
        LocalDiscovery.onEvent(61, devices(row(1, "one".toByteArray()), row(1, "again".toByteArray())))
        assertEquals(original, LocalDiscovery.devices.value)
    }

    @Test fun receivedInvitationIsQueuedWithoutJoiningAndSecretsAreNotLogged() {
        LocalDiscovery.onEvent(62, invite())
        assertEquals(LocalInvitation(device, link), LocalDiscovery.invitations.value.single())
        assertTrue(NativeLib.localInvites.isEmpty())
        assertTrue(LogManager.entries.none { link in it.message })
    }

    @Test fun invalidInvitationFramesCannotEnterTheQueue() {
        val invalid = listOf(byteArrayOf(), row(1, "x".toByteArray()), invite(id = 0), invite(name = byteArrayOf(-1)),
            invite(text = "https://example.test".toByteArray()), invite(text = "utun://".toByteArray()),
            invite(text = "utun://bad\u0000hidden".toByteArray()), invite(text = "utun://".toByteArray() + byteArrayOf(-1)),
            invite(text = ("utun://" + "a".repeat(1100)).toByteArray()))
        for (bad in invalid) {
            LocalDiscovery.onEvent(62, bad)
            assertTrue("size=${bad.size}", LocalDiscovery.invitations.value.isEmpty())
        }
    }

    @Test fun queueIsBoundedAndDismissesOldestFirst() {
        repeat(9) { LocalDiscovery.onEvent(62, invite(id = it + 1L)) }
        assertEquals((1L..8L).map { address(it).toHexString() }, LocalDiscovery.invitations.value.map { it.device.address })
        diagnostic("queue full")
        LocalDiscovery.dismissInvitation()
        assertEquals(address(2L).toHexString(), LocalDiscovery.invitations.value.first().device.address)
    }

    @Test fun matchingDeliveryMapsEveryNativeStatusAndStaysUntilCleared() {
        for (status in 0..2) {
            LocalDiscovery.send(device, link)
            val request = NativeLib.localInvites.last().request
            assertNotEquals(0L, request); assertTrue(LocalDiscovery.delivery.value!!.pending)
            LocalDiscovery.onEvent(63, result(request, status = status))
            val value = LocalDiscovery.delivery.value!!
            assertEquals(device, value.device)
            assertFalse(value.pending); assertEquals(status == 1, value.received); assertEquals(status == 2, value.forbidden)
            if (status != 1) diagnostic("status=$status")
            LocalDiscovery.clearDelivery(); assertNull(LocalDiscovery.delivery.value)
        }
    }

    @Test fun ackMustMatchThePendingPeerAsWellAsTheRequest() {
        LocalDiscovery.send(device, link)
        val pending = LocalDiscovery.delivery.value
        LocalDiscovery.onEvent(63, result(pending!!.request, peer = 22))
        assertEquals(pending, LocalDiscovery.delivery.value)
        diagnostic("peer")
        LocalDiscovery.onEvent(63, result(pending.request))
        assertTrue(LocalDiscovery.delivery.value!!.received)
    }

    @Test fun staleAndMalformedResultsPreservePendingDelivery() {
        LocalDiscovery.send(device, link)
        val pending = LocalDiscovery.delivery.value!!
        for (bad in listOf(result(pending.request + 1), result(pending.request, status = 3), result(pending.request).dropLast(1).toByteArray(),
            result(pending.request) + 0)) {
            LocalDiscovery.onEvent(63, bad)
            assertEquals(pending, LocalDiscovery.delivery.value)
        }
        LocalDiscovery.clearDelivery(); assertEquals(pending, LocalDiscovery.delivery.value)
    }

    @Test fun stopClearsDevicesAndCancelsSendingWithoutRevivingOnLateAck() {
        LocalDiscovery.onEvent(61, devices(row(1, "one".toByteArray())))
        LocalDiscovery.onEvent(62, invite())
        LocalDiscovery.send(device, link)
        val request = LocalDiscovery.delivery.value!!.request
        LocalDiscovery.onEvent(13, null)
        assertTrue(LocalDiscovery.devices.value.isEmpty())
        assertFalse(LocalDiscovery.delivery.value!!.pending)
        assertEquals(1, LocalDiscovery.invitations.value.size)
        LocalDiscovery.onEvent(63, result(request))
        assertFalse(LocalDiscovery.delivery.value!!.received)
    }

    @Test fun enqueueRejectionAndExceptionLeaveAVisibleFailureWithoutHanging() {
        NativeLib.sendLocal = { false }
        LocalDiscovery.send(device, link)
        assertFalse(LocalDiscovery.delivery.value!!.pending); diagnostic("enqueue")
        NativeLib.sendLocal = { throw IllegalStateException("native unavailable: $link") }
        val exception = runCatching { LocalDiscovery.send(device, link) }.exceptionOrNull()
        assertNull("send exception escaped: $exception", exception)
        assertFalse(LocalDiscovery.delivery.value!!.pending); diagnostic("send failed")
        assertTrue(LogManager.entries.none { link in it.message })
    }

    @Test fun busySendIsReportedAndDoesNotReplacePendingRequest() {
        LocalDiscovery.send(device, link)
        val pending = LocalDiscovery.delivery.value
        LocalDiscovery.send(LocalDevice(address(2).toHexString(), "other"), link)
        assertEquals(1, NativeLib.localInvites.size); assertEquals(pending, LocalDiscovery.delivery.value)
        diagnostic("pending")
        assertTrue(LogManager.entries.none { link in it.message })
    }

    @Test fun repeatedActivityUsesOneNonCountedLockAndReleasesOnce() {
        active(true); active(true)
        val lock = context.wifiManager!!.locks.single()
        assertTrue(lock.held); assertFalse(lock.counted); assertEquals(1, lock.acquisitions)
        active(false); LocalDiscovery.onEvent(13, null)
        assertFalse(lock.held); assertEquals(1, lock.releases)
    }

    @Test fun acquireAfterEffectFailureStillHasAnOwnerForStop() {
        context.wifiManager!!.failures = setOf("acquire_after")
        active(true)
        val lock = context.wifiManager!!.locks.single()
        assertTrue(lock.held); diagnostic("acquire")
        LocalDiscovery.onEvent(13, null)
        assertFalse("acquired lock lost after exception", lock.held)
    }

    @Test fun failedConfigureOrAcquireRetriesTheSameLock() {
        for (phase in listOf("configure", "acquire_before")) {
            context.wifiManager!!.failures = setOf(phase)
            val before = context.wifiManager!!.locks.size
            active(true)
            val lock = context.wifiManager!!.locks.last()
            lock.failures = emptySet(); context.wifiManager!!.failures = emptySet()
            active(true)
            assertEquals("phase=$phase", before + 1, context.wifiManager!!.locks.size)
            assertTrue(lock.held); assertFalse(lock.counted)
            active(false)
        }
    }

    @Test fun queryAndReleaseFailuresKeepTheLockForAnotherStopAttempt() {
        for (phase in listOf("query", "release_before", "release_after")) {
            active(true)
            val lock = context.wifiManager!!.locks.last()
            lock.failures = setOf(phase)
            LocalDiscovery.onEvent(13, null)
            diagnostic(if (phase == "query") "query" else "release")
            lock.failures = emptySet()
            LocalDiscovery.onEvent(13, null)
            assertFalse(lock.held)
        }
    }

    @Test fun absentWifiServiceAndCreateFailureAreLoggedAndCanRetry() {
        val wifi = context.wifiManager!!
        context.wifiManager = null; active(true); diagnostic("Wi-Fi service")
        context.wifiManager = wifi; wifi.failures = setOf("create"); active(true); diagnostic("create")
        wifi.failures = emptySet(); active(true); assertTrue(wifi.locks.single().held)
    }

    @Test fun nullMulticastLockIsReportedAtCreationAndCanRetry() {
        val wifi = context.wifiManager!!
        wifi.failures = setOf("null_create")
        active(true)
        assertTrue(wifi.locks.isEmpty()); diagnostic("creation returned null")
        wifi.failures = emptySet()
        active(true)
        assertTrue(wifi.locks.single().held)
    }

    @Test fun backgroundMutatorsAreRejectedBeforeChangingState() {
        LocalDiscovery.onEvent(61, devices(row(1, "kept".toByteArray())))
        LocalDiscovery.onEvent(62, invite()); LocalDiscovery.send(device, link)
        val snapshot = LocalDiscovery.devices.value
        val pending = LocalDiscovery.delivery.value
        val actions = listOf<() -> Unit>({ LocalDiscovery.send(device, link) }, LocalDiscovery::clearDelivery,
            LocalDiscovery::dismissInvitation, { LocalDiscovery.onEvent(61, devices()) })
        for (action in actions) {
            val result = AtomicReference<Throwable?>()
            val thread = Thread { result.set(runCatching(action).exceptionOrNull()) }.apply { start() }
            thread.join(2000); assertFalse(thread.isAlive)
            assertTrue(result.get() is IllegalStateException)
            assertEquals(snapshot, LocalDiscovery.devices.value); assertEquals(pending, LocalDiscovery.delivery.value)
            assertEquals(1, LocalDiscovery.invitations.value.size)
        }
        diagnostic("main thread")
    }

    @Test fun malformedActivityAndUnknownEventsDoNotReleaseTheLock() {
        active(true)
        for (bad in listOf(null, byteArrayOf(), byteArrayOf(2), byteArrayOf(0, 1))) LocalDiscovery.onEvent(64, bad)
        assertTrue(context.wifiManager!!.locks.single().held)
        LocalDiscovery.onEvent(999, byteArrayOf(0)); diagnostic("999")
    }

    @Test fun maximumDeviceCountNameAndInvitationSizesRemainAccepted() {
        val name = ("я".repeat(63) + "a").toByteArray()
        val rows = (1L..128L).map { row(it, name) }.toTypedArray()
        LocalDiscovery.onEvent(61, devices(*rows))
        assertEquals(128, LocalDiscovery.devices.value.size)
        val text = "utun://" + "a".repeat(1093)
        LocalDiscovery.onEvent(62, invite(name = name, text = text.toByteArray()))
        assertEquals(text, LocalDiscovery.invitations.value.single().link)
    }
}
