package com.utun.chat.data

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Looper
import com.utun.chat.ChatApplication
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CharacterCodingException

private const val LOCAL_KEY_SIZE = 32
private const val LOCAL_ADDRESS_SIZE = LOCAL_KEY_SIZE + 1

/** Адрес устройства из открытого объявления, сериализованный в hex для сравнения и передачи JNI. */
data class LocalDevice(val address: String, val name: String) {
    val label: String get() = "$name [${address.take(LOCAL_KEY_SIZE * 2).takeLast(6)}]"
}
/** Полученная ссылка, уже проверенная C-кодеком; показ/подключение решает UI, автоматического JOIN нет. */
data class LocalInvitation(val device: LocalDevice, val link: String)
/** Последняя отправка и снимок адресата; received — ACK приёма ссылки, forbidden — отказ получателя, оба не означают JOIN. */
data class LocalDelivery(val request: Long, val device: LocalDevice, val pending: Boolean,
                         val received: Boolean = false, val forbidden: Boolean = false)

/** Состояние приложения переживает Activity и не повторяет приглашения из replay событий ViewModel.
 * Все mutators требуют main, StateFlow можно наблюдать из любого потока. Список устройств заменяется целиком и сортируется по label.
 * Входящих приглашений не более 8; stop очищает устройства/завершает pending, но сохраняет уже полученные приглашения.
 * MulticastLock следует native ACTIVE, а не Activity. Ошибка сохраняет владельца lock для следующего сигнала ACTIVE/STOP.
 * Ссылки/пароли не выводятся в лог. */
object LocalDiscovery {
    private val _devices = MutableStateFlow<List<LocalDevice>>(emptyList())
    val devices = _devices.asStateFlow()
    private val _invitations = MutableStateFlow<List<LocalInvitation>>(emptyList())
    val invitations = _invitations.asStateFlow()
    private val _delivery = MutableStateFlow<LocalDelivery?>(null)
    val delivery = _delivery.asStateFlow()
    private var nextRequest = 0L
    private var multicastLock: WifiManager.MulticastLock? = null

    /** Одна pending-отправка на приложение; C проверяет ссылку/готовность и асинхронно сообщает результат по request+peer.
     * Повтор при pending игнорируется с предупреждением; отказ/исключение JNI оставляет терминальное состояние ошибки. */
    fun send(device: LocalDevice, link: String) {
        checkMain()
        _delivery.value?.takeIf { it.pending }?.let {
            LogManager.addLog("WARN", "discovery", "send ignored: pending request=${it.request} peer=${it.device.address}"); return
        }
        nextRequest++
        if (nextRequest == 0L) nextRequest = 1L
        val outgoing = LocalDelivery(nextRequest, device, pending = true)
        _delivery.value = outgoing
        try {
            if (!NativeLib.sendLocalInvite(device.address.hexToByteArray(), outgoing.request, link)) {
                LogManager.addLog("WARN", "discovery", "invitation enqueue failed request=${outgoing.request} peer=${device.address}")
                _delivery.value = outgoing.copy(pending = false)
            } else LogManager.addLog("DEBUG", "discovery", "invitation queued request=${outgoing.request} peer=${device.address}")
        } catch (error: Exception) {
            // Текст JNI-исключения может содержать ссылку; сохраняем только тип и контекст запроса.
            LogManager.addLog("ERROR", "discovery", "invitation send failed request=${outgoing.request} peer=${device.address}: ${error.javaClass.simpleName}")
            _delivery.value = outgoing.copy(pending = false)
        }
    }

    /** Убрать только терминальный результат; активная отправка продолжает ждать ACK даже при закрытом диалоге. */
    fun clearDelivery() { checkMain(); if (_delivery.value?.pending != true) _delivery.value = null }
    /** Убрать старейшее входящее приглашение, не затрагивая native-транспорт/членство. Пустая очередь допустима. */
    fun dismissInvitation() { checkMain(); _invitations.value = _invitations.value.drop(1) }

    /** События chat_event.h: 61 devices, 62 invite, 63 delivery, 64 active — BE; 13 завершает сервис.
     * Проверяет полный frame/UTF-8 до изменения состояния; ошибочный frame логируется, прежний снимок сохраняется.
     * Delivery применяется только к pending с теми же request и peer; старые результаты не возобновляют завершённую отправку. */
    fun onEvent(type: Int, data: ByteArray?) {
        checkMain()
        if (type == 13) {
            setActive(false); _devices.value = emptyList()
            _delivery.value?.takeIf { it.pending }?.let {
                _delivery.value = it.copy(pending = false)
                LogManager.addLog("WARN", "discovery", "pending invitation cancelled: core stopped request=${it.request} peer=${it.device.address}")
            }
            return
        }
        if (type !in 61..64) return invalid(type, data?.size ?: 0, "unsupported event")
        if (data == null) { LogManager.addLog("WARN", "discovery", "empty event type=$type"); return }
        val input = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN)
        try { when (type) {
            61 -> {
                require(input.remaining() >= 2) { "missing device count" }
                val count = input.short.toInt() and 0xffff
                require(count <= 128) { "invalid device count=$count" }
                val devices = List(count) { readDevice(input) }
                require(!input.hasRemaining()) { "trailing device bytes=${input.remaining()}" }
                require(devices.map { it.address }.toSet().size == count) { "duplicate device IDs" }
                _devices.value = devices.sortedBy { it.label }
                LogManager.addLog("DEBUG", "discovery", "device snapshot applied count=$count")
            }
            62 -> {
                val device = readDevice(input)
                require(input.remaining() in 1..1100) { "invalid invitation bytes=${input.remaining()}" }
                val link = readText(input, input.remaining(), "invitation")
                require(link.startsWith("utun://") && link.length > 7 && '\u0000' !in link) { "invalid invitation prefix/NUL" }
                if (_invitations.value.size >= 8) {
                    LogManager.addLog("WARN", "discovery", "incoming invitation queue full"); return
                }
                _invitations.value += LocalInvitation(device, link)
                LogManager.addLog("INFO", "discovery", "invitation received peer=${device.address} queued=${_invitations.value.size}")
            }
            63 -> {
                require(data.size == Long.SIZE_BYTES + LOCAL_ADDRESS_SIZE + 1) { "invalid delivery length=${data.size}" }
                val request = input.long
                val peer = readAddress(input)
                val status = input.get().toInt() and 0xff
                require(request != 0L && status in 0..2) { "invalid delivery request=$request peer=$peer status=$status" }
                val current = _delivery.value
                if (current?.request != request || !current.pending) {
                    LogManager.addLog("DEBUG", "discovery", "stale delivery ignored request=$request peer=$peer status=$status"); return
                }
                if (current.device.address != peer) {
                    LogManager.addLog("WARN", "discovery", "delivery peer mismatch request=$request expected=${current.device.address} got=$peer")
                    return
                }
                _delivery.value = current.copy(pending = false, received = status == 1, forbidden = status == 2)
                LogManager.addLog(if (status == 1) "INFO" else "WARN", "discovery", "delivery completed request=$request peer=$peer status=$status")
            }
            64 -> {
                require(data.size == 1 && data[0].toInt() in 0..1) { "activity needs one byte 0/1" }
                setActive(data[0].toInt() == 1)
            }
        } } catch (error: Exception) {
            invalid(type, data.size, "${error.javaClass.simpleName}: ${error.message}")
        }
    }

    private fun invalid(type: Int, size: Int, reason: String) {
        LogManager.addLog("WARN", "discovery", "invalid event type=$type bytes=$size: $reason")
    }

    /** Считать непустую UTF-8 строку без замены испорченных байтов; имя поля попадает в диагностику, само значение — нет. */
    private fun readText(input: ByteBuffer, size: Int, field: String): String {
        require(size in 1..input.remaining()) { "truncated $field bytes=$size remaining=${input.remaining()}" }
        return try { ByteArray(size).also { input.get(it) }.decodeToString(throwOnInvalidSequence = true)
        } catch (error: CharacterCodingException) { throw IllegalArgumentException("invalid UTF-8 $field", error) }
    }

    private fun readAddress(input: ByteBuffer): String {
        require(input.remaining() >= LOCAL_ADDRESS_SIZE) { "truncated device address" }
        val bytes = ByteArray(LOCAL_ADDRESS_SIZE).also { input.get(it) }
        require((0 until LOCAL_KEY_SIZE).any { bytes[it] != 0.toByte() } && bytes[LOCAL_KEY_SIZE].toInt() in 0..1) {
            "invalid local address"
        }
        return bytes.toHexString()
    }

    /** Адрес, длина и 1..127 байтов ника. */
    private fun readDevice(input: ByteBuffer): LocalDevice {
        require(input.remaining() >= LOCAL_ADDRESS_SIZE + 1) { "truncated device header bytes=${input.remaining()}" }
        val address = readAddress(input)
        val size = input.get().toInt() and 0xff
        require(size in 1..127) { "invalid device name bytes=$size" }
        val name = readText(input, size, "device name")
        require(name.none { it.code < 0x20 || it.code == 0x7f }) { "device name contains ASCII control characters" }
        return LocalDevice(address, name)
    }

    private fun checkMain() {
        check(Looper.myLooper() === Looper.getMainLooper()) {
            val message = "LocalDiscovery requires main thread: caller=${Thread.currentThread().name}"
            LogManager.addLog("ERROR", "discovery", message); message
        }
    }

    /** Сохраняет владельца до configure/acquire: Android-операция может бросить после побочного эффекта.
     * Не теряет lock при ошибке query/release; следующий ACTIVE/STOP повторит согласование состояния. */
    private fun setActive(active: Boolean) {
        var operation = "lookup"
        try {
            if (active) {
                var lock = multicastLock
                if (lock == null) {
                    operation = "service"
                    val wifi = ChatApplication.instance.getSystemService(Context.WIFI_SERVICE) as? WifiManager
                    if (wifi == null) { LogManager.addLog("WARN", "discovery", "Wi-Fi service unavailable"); return }
                    operation = "create"
                    lock = wifi.createMulticastLock("utun-local-discovery")
                    if (lock == null) { LogManager.addLog("ERROR", "discovery", "MulticastLock creation returned null"); return }
                    multicastLock = lock
                }
                operation = "query"
                if (lock.isHeld) return
                operation = "configure"; lock.setReferenceCounted(false)
                operation = "acquire"; lock.acquire()
                LogManager.addLog("INFO", "discovery", "Wi-Fi multicast receive enabled")
            } else {
                val lock = multicastLock ?: return
                operation = "query"
                if (lock.isHeld) { operation = "release"; lock.release() }
                multicastLock = null
                LogManager.addLog("INFO", "discovery", "Wi-Fi multicast receive disabled")
            }
        } catch (error: Exception) {
            LogManager.addLog("ERROR", "discovery", "multicast lock failed active=$active operation=$operation retained=${multicastLock != null}: $error")
        }
    }
}
