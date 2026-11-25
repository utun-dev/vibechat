package com.utun.chat.data

import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.os.Handler
import android.os.Looper
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.BeforeClass
import org.junit.Test

/** Настоящие command/close/pump/completion с управляемыми SDK-границами. JNI только записывает исходящие события.
 * Доступ к private-состоянию оставляет production API без тестовых входов; обе очереди исполняет тест. */
class BleRadioCommandsTest {
    companion object {
        private data class Event(val generation: Long, val token: Long, val type: Int, val arg: Int, val text: String)
        private val events = mutableListOf<Event>()
        @JvmStatic fun recordEvent(generation: Long, token: Long, type: Int, arg: Int, text: String, bytes: ByteArray) {
            events.add(Event(generation, token, type, arg, text))
        }
        @JvmStatic @BeforeClass fun loadJni() {
            assumeTrue(System.getProperty("os.name").startsWith("Linux"))
            System.load(checkNotNull(System.getProperty("utun.bleCommandTestJni")))
        }
    }
    private val radio = BleRadio::class.java
    private val linkClass = Class.forName("${radio.name}\$Link")
    private val handler = Handler(Looper.worker)
    private lateinit var server: BluetoothGattServer
    private fun field(name: String) = radio.getDeclaredField(name).apply { isAccessible = true }
    @Suppress("UNCHECKED_CAST")
    private fun links() = field("links").get(null) as MutableMap<Long, Any>
    private fun value(link: Any, name: String) = linkClass.getDeclaredField(name).apply { isAccessible = true }.get(link)
    private fun invoke(name: String, vararg args: Any) {
        radio.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(BleRadio, *args)
    }
    private fun addLink(token: Long, outgoing: Boolean, device: BluetoothDevice = BluetoothDevice("test:$token")): Any {
        val constructor = linkClass.declaredConstructors.single { it.parameterCount == 15 }.apply { isAccessible = true }
        val characteristic = BluetoothGattCharacteristic()
        val link = constructor.newInstance(token, device, outgoing, if (outgoing) BluetoothGatt() else null,
            characteristic, characteristic, characteristic, 7, 0, 517, true, null, false, 0L, null)
        links()[token] = link
        return link
    }
    private fun send(token: Long, channel: Int, bytes: ByteArray = byteArrayOf(7), generation: Long = 23) {
        assertTrue(BleRadio.command(5, generation, token, "", bytes, channel))
    }
    private fun serverCallback(generation: Long = 23): BluetoothGattServerCallback {
        // Настоящий callback из start(); конструктор фиксирует владельца и generation сервера.
        val callback = Class.forName("${radio.name}\$start\$4\$callback\$1")
        return callback.getDeclaredConstructor(radio, java.lang.Long.TYPE).apply { isAccessible = true }
            .newInstance(BleRadio, generation) as BluetoothGattServerCallback
    }
    @Before fun reset() {
        Looper.worker.reset(); events.clear(); links().clear()
        field("generation").setLong(null, 23); field("writer").set(null, null); field("lastWriter").setLong(null, 0)
        server = BluetoothGattServer(); field("server").set(null, server)
        for (name in listOf("tx", "invite", "ack")) field(name).set(null, BluetoothGattCharacteristic())
        BleRadio.state.value = 1
    }
    private fun checkReady() {
        assertEquals(1, BleRadio.state.value); assertEquals(23L, field("generation").getLong(null))
        assertFalse(server.closed); assertTrue(events.none { it.type == 8 })
    }
    private fun exerciseClosedSend(nativeClose: Boolean, channel: Int) {
        reset()
        val retired = addLink(-1, false)
        val live = addLink(1, true)
        if (nativeClose) assertTrue(BleRadio.command(4, 23, -1, "", byteArrayOf(), 0))
        else handler.post { invoke("close", retired, "test ACL disconnected", true, false) }
        send(-1, channel, byteArrayOf(1, 2))
        send(1, channel, byteArrayOf(3, 4))
        Looper.worker.drain()
        checkReady(); assertEquals(setOf(1L), links().keys)
        val gatt = value(live, "gatt") as BluetoothGatt
        assertEquals(1, gatt.writes.size); assertArrayEquals(byteArrayOf(3, 4), gatt.writes.single().bytes)
        assertEquals(if (nativeClose) 0 else 1, events.count { it.token == -1L && it.type == 5 })
        assertTrue(events.none { it.token == -1L && it.type == 4 })
        assertTrue(LogManager.getRecent(100).any {
            it.level == "DEBUG" && it.message.contains("Send cancelled: link closed generation=23 link=-1 channel=$channel bytes=2")
        })
        invoke("complete", live, 0)
        assertEquals(1, events.count { it.token == 1L && it.type == 4 })
    }
    @Test fun localCloseCancelsQueuedDataInviteAndAckWhileOtherLinkSends() {
        for (channel in 0..2) exerciseClosedSend(false, channel)
    }
    @Test fun nativeCloseCancelsQueuedDataInviteAndAckWithoutExtraCompletion() {
        for (channel in 0..2) exerciseClosedSend(true, channel)
    }
    @Test fun previousGenerationCannotWriteIntoReusedToken() {
        val live = addLink(1, true)
        send(1, 0, byteArrayOf(1))
        field("generation").setLong(null, 24)
        send(1, 0, byteArrayOf(2), 24)
        Looper.worker.drain()
        val writes = (value(live, "gatt") as BluetoothGatt).writes
        assertEquals(1, writes.size); assertArrayEquals(byteArrayOf(2), writes.single().bytes)
        assertEquals(24L, field("generation").getLong(null)); assertEquals(1, BleRadio.state.value)
    }
    @Test fun nameAndRepeatedVisibilityUpdateKeepAdvertisingHandle() {
        field("beacon").set(null, byteArrayOf(1)); field("incoming").setBoolean(null, true)
        val callback = object : android.bluetooth.le.AdvertisingSetCallback() {}
        field("advertisement").set(null, callback)
        assertTrue(BleRadio.command(10, 23, 0, "", "Полное новое имя".toByteArray(Charsets.UTF_8), 0))
        assertTrue(BleRadio.command(2, 23, 0, "", byteArrayOf(9, 8), 1))
        assertTrue(BleRadio.command(6, 0, 0, "", byteArrayOf(), 1))
        Looper.worker.drain()
        assertSame(callback, field("advertisement").get(null))
        assertArrayEquals("Полное новое имя".toByteArray(Charsets.UTF_8), field("localName").get(null) as ByteArray)
        checkReady()
        field("advertisement").set(null, null); field("beacon").set(null, byteArrayOf())
    }
    @Test fun profileClosePreservesIncomingOwnerAndDoesNotDisconnectSharedAcl() {
        val device = BluetoothDevice("shared-address")
        val incoming = addLink(-1, false, device)
        val query = addLink(1, true, device)
        @Suppress("UNCHECKED_CAST")
        val profiles = field("profileTokens").get(null) as MutableSet<Long>
        profiles.add(1)
        assertTrue(BleRadio.command(4, 23, 1, "", byteArrayOf(), 0)); Looper.worker.drain()
        val gatt = value(query, "gatt") as BluetoothGatt
        assertTrue(gatt.closed); assertEquals(0, gatt.disconnects)
        assertSame(incoming, links()[-1]); assertEquals(setOf(-1L), links().keys)
        assertFalse(profiles.contains(1)); checkReady()
        val ordinary = addLink(2, true)
        assertTrue(BleRadio.command(4, 23, 2, "", byteArrayOf(), 0)); Looper.worker.drain()
        assertEquals(1, (value(ordinary, "gatt") as BluetoothGatt).disconnects)
    }
    @Test fun retiredWriterCompletionReleasesSlotWithoutCompletingNewLinkAtSameAddress() {
        val device = BluetoothDevice("same-address")
        val retired = addLink(-1, false, device)
        send(-1, 2); Looper.worker.drain()
        assertSame(retired, field("writer").get(null)); assertEquals(1, Looper.worker.delayedCount)
        assertTrue(BleRadio.command(4, 23, -1, "", byteArrayOf(), 0))
        handler.post { addLink(-2, false, device) }
        send(-1, 2); send(-2, 0, byteArrayOf(9)); Looper.worker.drain()
        checkReady(); assertSame(retired, field("writer").get(null)); assertEquals(1, server.writes.size)
        assertEquals(1, Looper.worker.delayedCount)
        val callback = serverCallback()
        callback.onNotificationSent(device, 0); Looper.worker.drain()
        assertSame(links()[-2], field("writer").get(null)); assertEquals(2, server.writes.size)
        assertArrayEquals(byteArrayOf(9), server.writes.last().bytes); assertTrue(events.none { it.type == 4 })
        callback.onNotificationSent(device, 0); Looper.worker.drain()
        assertEquals(1, events.count { it.type == 4 && it.token == -2L }); assertNull(field("writer").get(null))
        assertEquals(0, Looper.worker.delayedCount); checkReady()
    }
    @Test fun previousGenerationCompletionCannotReleaseCurrentWriter() {
        val device = BluetoothDevice("same-address")
        val previous = serverCallback()
        field("generation").setLong(null, 24)
        val current = addLink(-2, false, device)
        send(-2, 0, generation = 24); Looper.worker.drain()
        previous.onNotificationSent(device, 0); Looper.worker.drain()
        assertSame(current, field("writer").get(null)); assertTrue(events.none { it.type == 4 || it.type == 8 })
        serverCallback(24).onNotificationSent(device, 0); Looper.worker.drain()
        assertNull(field("writer").get(null)); assertEquals(1, events.count { it.type == 4 && it.generation == 24L })
    }
    @Test fun invalidChannelOnLiveLinkStillReportsInvariantFailure() {
        addLink(1, true); send(1, 3); Looper.worker.drain()
        assertEquals(1, events.count { it.type == 8 && it.text.contains("Invalid BLE channel=3") })
        assertEquals(0L, field("generation").getLong(null)); assertTrue(server.closed)
    }
}
