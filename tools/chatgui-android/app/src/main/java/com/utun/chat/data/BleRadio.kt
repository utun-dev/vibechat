package com.utun.chat.data

import android.Manifest
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.*
import android.content.pm.PackageManager
import android.os.*
import android.location.LocationManager
import android.provider.Settings
import androidx.core.content.ContextCompat
import com.utun.chat.ChatApplication
import kotlinx.coroutines.flow.MutableStateFlow
import java.util.UUID

/** Android radio I/O на одном HandlerThread; протокол/ll_queue принадлежат C worker.
 * Application передаёт подтверждённый снимок настроек; callbacks SDK привязаны к generation.
 * applySettings без аргумента повторяет последний снимок при SERVICE_STARTED/смене Bluetooth. */
@Suppress("MissingPermission", "DEPRECATION")
object BleRadio {
    private var serviceUuid = UUID(0, 0) // Локальный node UUID передаётся общим C codec при start.
    private var serviceFilter = UUID(0, 0)
    private var serviceMask = UUID(0, 0)
    private val rxUuid = UUID.fromString("f61a9021-4abc-45ac-98e1-761df4c13811")
    private val txUuid = UUID.fromString("f61a9022-4abc-45ac-98e1-761df4c13811")
    private val inviteUuid = UUID.fromString("f61a9023-4abc-45ac-98e1-761df4c13811")
    private val ackUuid = UUID.fromString("f61a9024-4abc-45ac-98e1-761df4c13811")
    private val nameUuid = UUID.fromString("f61a9025-4abc-45ac-98e1-761df4c13811")
    private val nodeUuid = UUID.fromString("f61a9026-4abc-45ac-98e1-761df4c13811")
    private val descriptionUuid = UUID.fromString("00002901-0000-1000-8000-00805f9b34fb")
    private var localNode = byteArrayOf()
    private var localName = byteArrayOf()
    private val nameReads = linkedMapOf<String, ByteArray>() // Снимок имени на длинное чтение, до16 клиентов.
    private val cccUuid = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    private val thread by lazy { HandlerThread("vibechat-ble").apply { start() } }
    private val worker by lazy { Handler(thread.looper) }
    @Volatile private var configuredFlags: Int? = null
    private val context get() = ChatApplication.instance
    private val adapter get() = context.getSystemService(BluetoothManager::class.java)?.adapter
    private val cleanup = BleRadioCleanup(::log)
    private var cleanupDelay = 250L
    val state = MutableStateFlow(2) // BLE_CLOSED; тексты выбирает UI.
    val advertisingState = MutableStateFlow(2)
    // До API37 Builder ограничен TX_POWER_MAX=1; API37 добавляет MAX_AVAILABLE=20 (compileSdk пока36).
    val advertisingPowerRequestDbm = if (Build.VERSION.SDK_INT >= 37) 20 else AdvertisingSetParameters.TX_POWER_MAX
    val advertisingPowerDbm = MutableStateFlow<Int?>(null) // Мощность выбранного advertising set; null = неизвестна/не активен.
    private var generation = 0L
    private var incoming = false
    private var server: BluetoothGattServer? = null
    private var tx: BluetoothGattCharacteristic? = null
    private var invite: BluetoothGattCharacteristic? = null
    private var ack: BluetoothGattCharacteristic? = null
    private var scan: ScanCallback? = null
    private var scanner: BluetoothLeScanner? = null
    private var advertisement: AdvertisingSetCallback? = null
    private var advertiser: BluetoothLeAdvertiser? = null
    private var beacon = byteArrayOf()
    private var sequence = 0L
    private var writer: Link? = null
    private var lastWriter = 0L
    private val links = linkedMapOf<Long, Link>()
    private val profileTokens = mutableSetOf<Long>() // Собственные GATT client handles, не владеют общим ACL.
    private val incomingMtu = mutableMapOf<String, Int>()
    private data class ScanEndpoint(val node: String, val connectable: Boolean, val service: Boolean)
    private val scanEndpoints = linkedMapOf<String, ScanEndpoint>() // До64 адресов; лог только нового адреса/изменения свойств.
    private enum class CccPhase { RESET, SUBSCRIBE, DONE }
    private data class Link(val token: Long, val device: BluetoothDevice, val outgoing: Boolean,
        var gatt: BluetoothGatt? = null, var rx: BluetoothGattCharacteristic? = null,
        var invite: BluetoothGattCharacteristic? = null, var ack: BluetoothGattCharacteristic? = null,
        var subscriptions: Int = 0, var channel: Int = 0,
        var mtu: Int = 23, var opened: Boolean = false, var pending: ByteArray? = null, var writing: Boolean = false,
        var busySince: Long = 0,
        var priorityGatt: BluetoothGatt? = null) // Клиентский handle того же ACL: server API не имеет requestConnectionPriority.
    @JvmStatic external fun nativeInstall()
    @JvmStatic external fun nativeConfigure(flags: Int): Boolean
    @JvmStatic private external fun nativeEvent(gen: Long, token: Long, type: Int, arg: Int, text: String, bytes: ByteArray)
    @JvmStatic private external fun nativeDiscovery(service: String): ByteArray?
    fun permissions(): Array<String> = if (Build.VERSION.SDK_INT >= 31) arrayOf(
        Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_ADVERTISE)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    fun permitted() = permissions().all { ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED }
    fun init() {
        worker // Ошибка запуска потока возвращается Application до установки callback в C.
        nativeInstall()
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, intent: Intent) {
                try { applySettings() } catch (e: Exception) { log("ERROR", "Bluetooth state application failed: $e") }
            }
        }
        ContextCompat.registerReceiver(context, receiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED), ContextCompat.RECEIVER_EXPORTED)
    }
    fun applySettings(flags: Int? = configuredFlags) {
        if (flags == null) return // Пока нет подтверждённого чтения DataStore, ничего не включать.
        configuredFlags = flags
        val effective = try { if (permitted() && adapter?.isEnabled == true) flags else 0 }
            catch (e: RuntimeException) { log("WARN", "Bluetooth state unavailable: $e"); 0 }
        if (NativeLib.isReady() && !nativeConfigure(effective)) log("WARN", "Settings could not be queued")
        if (effective == 0) state.value = 2
    }
    /** Только ручная instrumentation: отделить влияние scanning от передачи по уже открытому GATT. */
    fun pauseScanForProbe(): Boolean = worker.post {
        guarded {
            val callback = checkNotNull(scan) { "Probe scan is not running" }
            checkNotNull(scanner).stopScan(callback)
            scan = null; scanner = null
            log("INFO", "Scan paused for probe generation=$generation")
        }
    }
    private fun log(level: String, text: String) { LogManager.addLog(level, "discovery", "BLE $text") }
    private fun wireChannel(uuid: UUID): Int = when (uuid) { rxUuid, txUuid -> 0; inviteUuid -> 1; ackUuid -> 2; else -> -1 }
    private fun emit(type: Int, token: Long = 0, arg: Int = 0, text: String = "", data: ByteArray = byteArrayOf()) {
        nativeEvent(generation, token, type, arg, text, data)
    }
    private fun guarded(block: () -> Unit) {
        try { block() } catch (e: RuntimeException) {
            log("ERROR", "Radio failure: ${e.javaClass.simpleName}: ${e.message}")
            emit(8, text = "${e.javaClass.simpleName}: ${e.message}")
            stop()
        }
    }
    private fun postCallback(gen: Long, source: String, block: () -> Unit) {
        if (!worker.post { if (generation == gen) guarded(block) })
            log("ERROR", "Worker rejected callback=$source generation=$gen")
    }
    @JvmStatic fun command(op: Int, gen: Long, token: Long, text: String, bytes: ByteArray, arg: Int): Boolean {
        val accepted = worker.post {
            guarded {
                when (op) {
                    0 -> if (generation == gen) stop()
                    1 -> {
                        stop(); generation = gen
                        check(bytes.size >= 32) { "Missing BLE public key" }
                        val setup = bytes.copyOfRange(32, bytes.size).toString(Charsets.UTF_8).split('|', limit = 3)
                        check(setup.size == 3) { "Invalid BLE setup metadata" }
                        serviceUuid = UUID.fromString(text); serviceFilter = UUID.fromString(setup[0]); serviceMask = UUID.fromString(setup[1])
                        localNode = bytes.copyOf(32)
                        localName = setup[2].toByteArray(Charsets.UTF_8)
                        check(localName.size <= 63) { "NODE_NAME too long" }
                        check(cleanup.isEmpty) { "Previous BLE cleanup pending" }; start()
                    }
                    6 -> {
                        val changed = incoming != (arg != 0)
                        incoming = arg != 0
                        if (changed && scan != null) advertise()
                    }
                    7 -> { state.value = arg; log("INFO", "State=$arg reason=$text") }
                    8 -> { advertisingState.value = arg }
                    else -> if (generation == gen) when (op) {
                        2 -> {
                            val changed = beacon.isNotEmpty() != (arg != 0)
                            beacon = if (arg != 0) bytes else byteArrayOf()
                            if (changed) advertise()
                        }
                        10 -> { check(bytes.size <= 63) { "NODE_NAME too long" }; localName = bytes.copyOf() }
                        3 -> connect(token, text, UUID.fromString(bytes.toString(Charsets.UTF_8)), arg != 0)
                        4 -> links[token]?.let { close(it, "native close", false) }
                        5 -> {
                            val link = links[token]
                            if (link == null) {
                                // closed уже отменяет pending в C; команды из другой очереди могут прийти после удаления link.
                                log("DEBUG", "Send cancelled: link closed generation=$gen link=$token channel=$arg bytes=${bytes.size}")
                            } else {
                                check(link.pending == null) { "send already pending link=$token" }
                                check(arg in 0..2) { "Invalid BLE channel=$arg" }
                                link.channel = arg; link.pending = bytes
                                pump()
                            }
                        }
                    }
                }
            }
        }
        if (!accepted) log("ERROR", "Worker rejected command=$op")
        return accepted
    }
    private fun start() {
        check(permitted()) { "Bluetooth permission missing" }
        check(adapter?.isEnabled == true) { "Bluetooth disabled" }
        if (Build.VERSION.SDK_INT < 31) {
            val enabled = if (Build.VERSION.SDK_INT >= 28) context.getSystemService(LocationManager::class.java).isLocationEnabled
                else Settings.Secure.getInt(context.contentResolver, Settings.Secure.LOCATION_MODE, 0) != 0
            check(enabled) { "Android 8–11 requires location services for BLE scanning" }
        }
        val gen = generation
        run {
            val callback = object : BluetoothGattServerCallback() {
                private fun post(block: () -> Unit) = postCallback(gen, "GATT server", block)
                override fun onServiceAdded(status: Int, service: BluetoothGattService) = post {
                    check(status == BluetoothGatt.GATT_SUCCESS) { "addService status=$status" }; startScan(); advertise(); emit(1)
                }
                override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) = post {
                    log(if (status == 0) "INFO" else "WARN", "Server ACL address=${device.address} status=$status state=$newState")
                    if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                        links.values.filter { !it.outgoing && it.device.address == device.address }.toList().forEach {
                            close(it, "ACL disconnected status=$status state=$newState")
                        }
                        incomingMtu.remove(device.address); nameReads.remove(device.address)
                    }
                }
                override fun onPhyUpdate(device: BluetoothDevice, txPhy: Int, rxPhy: Int, status: Int) = post {
                    phyResult("Server", device.address, txPhy, rxPhy, status)
                }
                override fun onPhyRead(device: BluetoothDevice, txPhy: Int, rxPhy: Int, status: Int) = post {
                    phyResult("Server read", device.address, txPhy, rxPhy, status)
                }
                override fun onMtuChanged(device: BluetoothDevice, mtu: Int) = post {
                    incomingMtu[device.address] = mtu
                    // После открытия используем уже объявленную core ёмкость, уменьшение требует reconnect.
                    log("DEBUG", "Server MTU address=${device.address} mtu=$mtu")
                }
                override fun onDescriptorWriteRequest(device: BluetoothDevice, requestId: Int, descriptor: BluetoothGattDescriptor,
                    preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray) = post {
                    val expected = if (descriptor.characteristic.uuid == inviteUuid) BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                        else BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    val disabled = value.contentEquals(BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE)
                    val valid = descriptor.uuid == cccUuid && descriptor.characteristic.uuid in listOf(txUuid, inviteUuid, ackUuid) &&
                        !preparedWrite && offset == 0 && (disabled || incoming && value.contentEquals(expected))
                    var link = links.values.find { !it.outgoing && it.device.address == device.address }
                    val accepted = valid && (disabled || link != null || links.size < 16)
                    if (responseNeeded) respond(device, requestId, if (accepted) 0 else BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED)
                    if (accepted && disabled) {
                        log("DEBUG", "Server CCC reset address=${device.address} characteristic=${descriptor.characteristic.uuid} link=${link?.token}")
                        if (link != null) close(link, "subscriptions reset", preserveAcl = true)
                        return@post
                    }
                    if (accepted && link == null) {
                        link = Link(--sequence, device, false, mtu = incomingMtu[device.address] ?: 23)
                        links[link.token] = link
                        emit(9, link.token, text = device.address) // Core ограничивает время ожидания всех трёх CCC.
                        preferFastPhy(device, null)
                        tuneIncomingPriority(link)
                    } else if (!accepted) {
                        log("WARN", "Subscription rejected address=${device.address} characteristic=${descriptor.characteristic.uuid} " +
                            "descriptor=${descriptor.uuid} incoming=$incoming prepared=$preparedWrite response=$responseNeeded " +
                            "offset=$offset value=${value.take(4)} bytes=${value.size} links=${links.size}")
                        if (link != null) close(link, "subscription disabled")
                    }
                    if (accepted && link != null) {
                        link.subscriptions = link.subscriptions or (1 shl wireChannel(descriptor.characteristic.uuid))
                        log("DEBUG", "Server subscribed link=${link.token} characteristic=${descriptor.characteristic.uuid} mode=${value[0]}")
                        if (link.subscriptions == 7 && !link.opened) {
                            link.opened = true; emit(2, link.token, link.mtu, device.address)
                        }
                    }
                }
                override fun onCharacteristicWriteRequest(device: BluetoothDevice, requestId: Int, characteristic: BluetoothGattCharacteristic,
                    preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray) = post {
                    val link = links.values.find { !it.outgoing && it.device.address == device.address }
                    val valid = link != null && link.opened && characteristic.uuid in listOf(rxUuid, inviteUuid, ackUuid) &&
                        !preparedWrite && responseNeeded == (characteristic.uuid == inviteUuid) && offset == 0
                    if (valid) emit(3, link!!.token, arg = wireChannel(characteristic.uuid), data = value.copyOf())
                    if (responseNeeded) respond(device, requestId, if (valid) 0 else BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED)
                    if (!valid) log("WARN", "Write rejected address=${device.address} characteristic=${characteristic.uuid} " +
                        "bytes=${value.size} response=$responseNeeded prepared=$preparedWrite offset=$offset")
                }
                override fun onNotificationSent(device: BluetoothDevice, status: Int) = post {
                    val link = writer
                    if (link == null || link.device.address != device.address)
                        log("WARN", "Stale notification completion address=${device.address} status=$status writer=${link?.token}")
                    else {
                        writer = null; worker.removeCallbacks(writerTimeout)
                        if (links[link.token] === link) complete(link, status)
                        else { log("DEBUG", "Retired notification completed link=${link.token} status=$status"); pump() }
                    }
                }
                override fun onCharacteristicReadRequest(device: BluetoothDevice, requestId: Int, offset: Int,
                    characteristic: BluetoothGattCharacteristic) = post {
                    if (characteristic.uuid !in listOf(nameUuid, nodeUuid)) {
                        log("WARN", "Read rejected address=${device.address} characteristic=${characteristic.uuid}")
                        respond(device, requestId, BluetoothGatt.GATT_READ_NOT_PERMITTED); return@post
                    }
                    if (characteristic.uuid == nameUuid && offset == 0) {
                        if (device.address !in nameReads && nameReads.size >= 16) {
                            log("WARN", "NODE_NAME snapshot capacity=16 replacing address=${nameReads.keys.first()}")
                            nameReads.remove(nameReads.keys.first())
                        }
                        nameReads[device.address] = localName.copyOf()
                    }
                    val bytes = if (characteristic.uuid == nodeUuid) localNode else nameReads[device.address]
                    if (bytes == null || offset !in 0..bytes.size) {
                        log("WARN", "Profile invalid read address=${device.address} offset=$offset bytes=${bytes?.size}")
                        respond(device, requestId, BluetoothGatt.GATT_INVALID_OFFSET); return@post
                    }
                    val mtu = incomingMtu[device.address] ?: 23
                    val chunk = bytes.copyOfRange(offset, minOf(bytes.size, offset + mtu - 1))
                    log("DEBUG", "Profile read address=${device.address} characteristic=${characteristic.uuid} " +
                        "offset=$offset bytes=${chunk.size} total=${bytes.size}")
                    if (server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, chunk) != true)
                        log("ERROR", "Profile read response failed address=${device.address} request=$requestId")
                }
                override fun onDescriptorReadRequest(device: BluetoothDevice, requestId: Int, offset: Int,
                    descriptor: BluetoothGattDescriptor) = post {
                    val value = if (descriptor.uuid == descriptionUuid) when (descriptor.characteristic.uuid) {
                        nameUuid -> "NODE_NAME".toByteArray(Charsets.UTF_8)
                        nodeUuid -> "NODE_ID".toByteArray(Charsets.UTF_8)
                        else -> null
                    } else null
                    if (value == null || offset !in 0..value.size) {
                        log("WARN", "Descriptor read rejected address=${device.address} uuid=${descriptor.uuid} offset=$offset")
                        respond(device, requestId, BluetoothGatt.GATT_READ_NOT_PERMITTED)
                    } else if (server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset,
                        value.copyOfRange(offset, value.size)) != true) log("ERROR", "Description response failed request=$requestId")
                }
                override fun onExecuteWrite(device: BluetoothDevice, requestId: Int, execute: Boolean) = post {
                    log("WARN", "Execute write rejected address=${device.address} request=$requestId execute=$execute")
                    respond(device, requestId, BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED)
                }
            }
            server = checkNotNull(context.getSystemService(BluetoothManager::class.java)?.openGattServer(context, callback))
            val service = BluetoothGattService(serviceUuid, BluetoothGattService.SERVICE_TYPE_PRIMARY)
            service.addCharacteristic(BluetoothGattCharacteristic(rxUuid, BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
                BluetoothGattCharacteristic.PERMISSION_WRITE))
            tx = BluetoothGattCharacteristic(txUuid, BluetoothGattCharacteristic.PROPERTY_NOTIFY, 0).apply {
                addDescriptor(BluetoothGattDescriptor(cccUuid, BluetoothGattDescriptor.PERMISSION_WRITE))
            }
            service.addCharacteristic(tx)
            invite = BluetoothGattCharacteristic(inviteUuid,
                BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_INDICATE,
                BluetoothGattCharacteristic.PERMISSION_WRITE).apply {
                addDescriptor(BluetoothGattDescriptor(cccUuid, BluetoothGattDescriptor.PERMISSION_WRITE))
            }
            service.addCharacteristic(invite)
            ack = BluetoothGattCharacteristic(ackUuid,
                BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
                BluetoothGattCharacteristic.PERMISSION_WRITE).apply {
                addDescriptor(BluetoothGattDescriptor(cccUuid, BluetoothGattDescriptor.PERMISSION_WRITE))
            }
            service.addCharacteristic(ack)
            for ((uuid, label) in listOf(nameUuid to "NODE_NAME", nodeUuid to "NODE_ID")) {
                service.addCharacteristic(BluetoothGattCharacteristic(uuid, BluetoothGattCharacteristic.PROPERTY_READ,
                    BluetoothGattCharacteristic.PERMISSION_READ).apply {
                    addDescriptor(BluetoothGattDescriptor(descriptionUuid, BluetoothGattDescriptor.PERMISSION_READ).apply {
                        value = label.toByteArray(Charsets.UTF_8)
                    })
                })
            }
            check(server!!.addService(service)) { "addService refused" }
        }
    }
    private fun respond(device: BluetoothDevice, id: Int, status: Int) {
        if (server?.sendResponse(device, id, status, 0, null) != true)
            log("ERROR", "sendResponse failed address=${device.address} request=$id status=$status server=${server != null}")
    }
    private fun startScan() {
        val gen = generation
        scan = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val record = result.scanRecord ?: return
                val identity = record.serviceUuids.orEmpty().firstNotNullOfOrNull { service ->
                    nativeDiscovery(service.toString())
                } ?: return
                if (!worker.post { if (generation == gen) guarded {
                    val address = result.device.address
                    val previous = scanEndpoints[address]
                    val node = identity.copyOfRange(3, 11).joinToString("") { "%02x".format(it.toInt() and 255) }
                    val endpoint = ScanEndpoint(node, result.isConnectable, true)
                    if (previous != endpoint) {
                        if (previous == null && scanEndpoints.size >= 64) scanEndpoints.remove(scanEndpoints.keys.first())
                        scanEndpoints[address] = endpoint
                        log("INFO", "Discovery endpoint=$address node=$node connectable=${endpoint.connectable} " +
                            "service_uuid_seen=${endpoint.service} rssi=${result.rssi} bytes=${identity.size}")
                    }
                    if (endpoint.connectable) emit(6, arg = result.rssi, text = address, data = identity)
                } })
                    log("ERROR", "Worker rejected scan result")
            }
            override fun onScanFailed(errorCode: Int) = postCallback(gen, "scan failure") { error("scan error=$errorCode") }
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(serviceFilter), ParcelUuid(serviceMask)).build()
        scanner = checkNotNull(adapter?.bluetoothLeScanner)
        scanner!!.startScan(listOf(filter),
            ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scan)
        log("INFO", "Scan started generation=$gen incoming=$incoming service=$serviceUuid family=$serviceFilter mask=$serviceMask")
    }
    private fun advertise() {
        advertisingPowerDbm.value = null
        advertisement?.let { previous ->
            val owner = checkNotNull(advertiser)
            cleanup.retain(previous, "advertisement") { owner.stopAdvertisingSet(previous) }
            advertisement = null
            check(cleanup.retry()) { "Previous advertising cleanup pending" }
        }
        if (!incoming && beacon.isEmpty()) { emit(7, arg = 1, text = "advertisement disabled"); return }
        val publisher = checkNotNull(adapter?.bluetoothLeAdvertiser) { "Advertising unsupported" }
        advertiser = publisher
        val gen = generation
        val data = AdvertiseData.Builder().setIncludeDeviceName(false)
        if (beacon.isNotEmpty()) data.addServiceUuid(ParcelUuid(serviceUuid))
        val response = AdvertiseData.Builder()
        // Имя читается через NODE_NAME; рекламируется только подключаемый UUID сервиса.
        val callback = object : AdvertisingSetCallback() {
            override fun onAdvertisingSetStarted(set: AdvertisingSet?, txPower: Int, status: Int) = postCallback(gen, "advertising start") {
                if (advertisement === this) {
                    if (status == ADVERTISE_SUCCESS && set != null) {
                        advertisingPowerDbm.value = txPower.takeIf { it in -127..20 }
                        if (advertisingPowerDbm.value == null) log("WARN", "Advertising selected power unavailable value=$txPower")
                        log("INFO", "Advertising ready bytes=${beacon.size} requested=${advertisingPowerRequestDbm}dBm selected=${txPower}dBm (advertising only)")
                        emit(7, text = "published")
                    } else {
                        log("ERROR", "Advertising failed status=$status set=${set != null}")
                        emit(7, arg = if (status == 0) 1 else status, text = "advertising failed=$status")
                    }
                }
            }
            override fun onAdvertisingSetStopped(set: AdvertisingSet?) = postCallback(gen, "advertising stopped") {
                if (advertisement === this) {
                    advertisement = null; advertisingPowerDbm.value = null
                    log("WARN", "Advertising stopped unexpectedly")
                    emit(7, arg = 1, text = "advertising stopped")
                }
            }
        }
        advertisement = callback
        // Legacy 1M остаётся совместимым со сканерами; новый API возвращает выбранную мощность в dBm.
        publisher.startAdvertisingSet(AdvertisingSetParameters.Builder().setLegacyMode(true).setConnectable(true)
            .setScannable(true).setInterval(AdvertisingSetParameters.INTERVAL_LOW).setTxPowerLevel(advertisingPowerRequestDbm).build(),
            data.build(), response.build(), null, null, callback)
    }

    private fun phyResult(role: String, address: String, txPhy: Int, rxPhy: Int, status: Int) {
        fun name(phy: Int) = when (phy) { BluetoothDevice.PHY_LE_1M -> "1M"; BluetoothDevice.PHY_LE_2M -> "2M"
            BluetoothDevice.PHY_LE_CODED -> "CODED"; else -> "unknown($phy)" }
        log(if (status == 0) "INFO" else "WARN", "$role PHY address=$address tx=${name(txPhy)} rx=${name(rxPhy)} status=$status")
    }
    // 1M: на проверенной Android/Linux паре 2M замедляет очередь контроллера. Результат проверяется callbacks.
    private fun preferFastPhy(device: BluetoothDevice, gatt: BluetoothGatt?) {
        try {
            if (gatt != null) gatt.setPreferredPhy(BluetoothDevice.PHY_LE_1M_MASK,
                BluetoothDevice.PHY_LE_1M_MASK, BluetoothDevice.PHY_OPTION_NO_PREFERRED)
            else checkNotNull(server).setPreferredPhy(device, BluetoothDevice.PHY_LE_1M_MASK,
                BluetoothDevice.PHY_LE_1M_MASK, BluetoothDevice.PHY_OPTION_NO_PREFERRED)
            log("INFO", "PHY 1M requested address=${device.address} outgoing=${gatt != null}")
            if (gatt != null) gatt.readPhy() else checkNotNull(server).readPhy(device)
        } catch (e: RuntimeException) { log("WARN", "PHY tuning unavailable address=${device.address}: ${e.message}") }
    }
    private fun requestHighPriority(link: Link, gatt: BluetoothGatt) {
        val accepted = gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
        log(if (accepted) "INFO" else "WARN", "Connection priority HIGH requested link=${link.token} outgoing=${link.outgoing} accepted=$accepted")
    }
    // GATT client/server — роли API. Клиентский handle использует уже открытый физический ACL, без discovery/DATA.
    private fun tuneIncomingPriority(link: Link) {
        val gen = generation
        try {
            val callback = object : BluetoothGattCallback() {
                override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
                    if (generation != gen || links[link.token] !== link || link.priorityGatt !== gatt) return
                    guarded {
                        log(if (status == 0) "DEBUG" else "WARN", "Priority GATT link=${link.token} status=$status state=$newState")
                        if (status == 0 && newState == BluetoothProfile.STATE_CONNECTED) {
                            try { requestHighPriority(link, gatt) }
                            catch (e: RuntimeException) { log("WARN", "Priority tuning failed link=${link.token}: $e") }
                        } else if (status != 0 || newState == BluetoothProfile.STATE_DISCONNECTED) {
                            link.priorityGatt = null
                            cleanup.retain(gatt, "priority GATT link=${link.token}") { gatt.close() }; drainCleanup()
                        }
                    }
                }
            }
            link.priorityGatt = link.device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE,
                BluetoothDevice.PHY_LE_1M_MASK, worker)
            if (link.priorityGatt == null) log("WARN", "Priority GATT unavailable link=${link.token}")
        } catch (e: RuntimeException) { log("WARN", "Incoming priority tuning unavailable link=${link.token}: $e") }
    }
    private fun connect(token: Long, address: String, peerService: UUID, profileOnly: Boolean) {
        check(cleanup.isEmpty) { "Previous BLE cleanup pending" }
        check(links.size < 16) { "link limit" }
        val link = Link(token, checkNotNull(adapter).getRemoteDevice(address), true)
        links[token] = link
        if (profileOnly) profileTokens.add(token)
        val endpoint = scanEndpoints[address]
        log("INFO", "Client connect requested link=$token address=$address seen_connectable=${endpoint?.connectable} " +
            "service_uuid_seen=${endpoint?.service} node=${endpoint?.node} service=$peerService")
        val gen = generation
        val callback = object : BluetoothGattCallback() {
            private var cccPhase = CccPhase.RESET
            private var cccChannel = 0
            private var characteristics = emptyList<BluetoothGattCharacteristic>()
            private var remoteName: BluetoothGattCharacteristic? = null
            private var remoteNode = byteArrayOf()
            private fun valid(gatt: BluetoothGatt) = generation == gen && links[token] === link && link.gatt === gatt
            private fun dispatch(gatt: BluetoothGatt, block: () -> Unit) { if (valid(gatt)) guarded(block) }
            // Сначала обнулить весь набор CCC: общий ACL мог пережить предыдущую сессию или процесс.
            private fun configureCcc(gatt: BluetoothGatt) {
                val characteristic = characteristics[cccChannel]
                val ccc = checkNotNull(characteristic.getDescriptor(cccUuid))
                if (cccPhase == CccPhase.SUBSCRIBE && !gatt.setCharacteristicNotification(characteristic, true)) {
                    close(link, "local subscription refused phase=$cccPhase channel=$cccChannel"); return
                }
                val value = when {
                    cccPhase == CccPhase.RESET -> BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
                    characteristic.uuid == inviteUuid -> BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                    else -> BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                }
                log("DEBUG", "Client CCC request link=$token phase=$cccPhase channel=$cccChannel mode=${value[0]}")
                val status = if (Build.VERSION.SDK_INT >= 33) gatt.writeDescriptor(ccc, value)
                    else { ccc.value = value; if (gatt.writeDescriptor(ccc)) 0 else -1 }
                if (status != 0) close(link, "CCC write refused phase=$cccPhase channel=$cccChannel status=$status")
            }
            override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) = dispatch(gatt) {
                log(if (status == 0) "INFO" else "WARN", "Client link=$token status=$status state=$newState")
                if (status != 0 || newState != BluetoothProfile.STATE_CONNECTED) close(link, "client status=$status state=$newState")
                else {
                    if (!profileOnly) { requestHighPriority(link, gatt); preferFastPhy(link.device, gatt) }
                    if (!gatt.requestMtu(517)) close(link, "requestMtu refused")
                }
            }
            override fun onPhyUpdate(gatt: BluetoothGatt, txPhy: Int, rxPhy: Int, status: Int) = dispatch(gatt) {
                phyResult("Client", address, txPhy, rxPhy, status)
            }
            override fun onPhyRead(gatt: BluetoothGatt, txPhy: Int, rxPhy: Int, status: Int) = dispatch(gatt) {
                phyResult("Client read", address, txPhy, rxPhy, status)
            }
            override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) = dispatch(gatt) {
                if (status == 0) link.mtu = mtu
                log(if (status == 0) "INFO" else "WARN", "Client MTU link=$token requested=517 reported=$mtu effective=${link.mtu} status=$status")
                if (!gatt.discoverServices()) close(link, "discoverServices refused")
            }
            override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) = dispatch(gatt) {
                val service = gatt.getService(peerService)
                val characteristic = service?.getCharacteristic(txUuid)
                val ccc = characteristic?.getDescriptor(cccUuid)
                link.rx = service?.getCharacteristic(rxUuid)
                link.invite = service?.getCharacteristic(inviteUuid)
                link.ack = service?.getCharacteristic(ackUuid)
                log("DEBUG", "Client discovery link=$token status=$status service=${service != null} " +
                    "rx_properties=${link.rx?.properties} tx_properties=${characteristic?.properties} " +
                    "invite_properties=${link.invite?.properties} ack_properties=${link.ack?.properties}")
                if (status != 0 || ccc == null || link.rx == null || link.invite == null || link.ack == null) {
                    close(link, "service discovery failed status=$status service=${service != null} CCC=${ccc != null} " +
                        "RX=${link.rx != null} INVITE=${link.invite != null} ACK=${link.ack != null}"); return@dispatch
                }
                if (link.rx!!.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE == 0) {
                    close(link, "RX lacks Write Without Response properties=${link.rx!!.properties}"); return@dispatch
                }
                if (characteristic.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0) {
                    close(link, "TX lacks Notification properties=${characteristic.properties}"); return@dispatch
                }
                for (control in listOf(checkNotNull(link.invite), checkNotNull(link.ack))) {
                    val controlProperties = if (control.uuid == inviteUuid)
                        BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_INDICATE
                    else BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE or BluetoothGattCharacteristic.PROPERTY_NOTIFY
                    if (control.properties and controlProperties != controlProperties || control.getDescriptor(cccUuid) == null) {
                        close(link, "Control characteristic invalid uuid=${control.uuid} properties=${control.properties}"); return@dispatch
                    }
                }
                characteristics = listOf(characteristic, checkNotNull(link.invite), checkNotNull(link.ack))
                val idCharacteristic = service.getCharacteristic(nodeUuid)
                remoteName = service.getCharacteristic(nameUuid)
                if (idCharacteristic == null || remoteName == null ||
                    listOf(idCharacteristic, remoteName!!).any { it.properties and BluetoothGattCharacteristic.PROPERTY_READ == 0 }) {
                    close(link, "NODE_NAME/NODE_ID missing or unreadable"); return@dispatch
                }
                if (!gatt.readCharacteristic(idCharacteristic)) close(link, "NODE_ID read refused")
            }
            private fun profileRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
                if (status != BluetoothGatt.GATT_SUCCESS) { close(link, "Profile read failed uuid=${characteristic.uuid} status=$status"); return }
                if (characteristic.uuid == nodeUuid && remoteNode.isEmpty()) {
                    if (value.size != 32) {
                        close(link, "Invalid GATT public key size"); return
                    }
                    remoteNode = value.copyOf()
                    if (!gatt.readCharacteristic(checkNotNull(remoteName))) close(link, "NODE_NAME read refused")
                    return
                }
                if (characteristic.uuid != nameUuid || remoteNode.size != 32 || value.size > 63) {
                    close(link, "Unexpected profile read uuid=${characteristic.uuid} bytes=${value.size}"); return
                }
                log("INFO", "Profile read link=${link.token} NODE_ID matches; NODE_NAME UTF8_bytes=${value.size}")
                emit(10, token, data = remoteNode + value)
                if (profileOnly) return
                cccPhase = CccPhase.RESET; cccChannel = 0
                log("INFO", "Client subscriptions preparing link=$token mtu=${link.mtu}: reset all CCC, then subscribe")
                configureCcc(gatt)
            }
            override fun onCharacteristicRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray,
                status: Int) = dispatch(gatt) { profileRead(gatt, characteristic, value, status) }
            @Deprecated("API < 33")
            override fun onCharacteristicRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) = dispatch(gatt) {
                if (Build.VERSION.SDK_INT < 33) profileRead(gatt, characteristic, characteristic.value ?: byteArrayOf(), status)
            }
            override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) = dispatch(gatt) {
                if (cccPhase == CccPhase.DONE || characteristics.isEmpty() || descriptor.uuid != cccUuid ||
                    descriptor.characteristic.uuid != characteristics[cccChannel].uuid || status != 0) {
                    close(link, "CCC completion invalid phase=$cccPhase channel=$cccChannel uuid=${descriptor.characteristic.uuid} status=$status")
                    return@dispatch
                }
                log("DEBUG", "Client CCC complete link=$token phase=$cccPhase channel=$cccChannel status=$status")
                if (++cccChannel == characteristics.size) {
                    cccChannel = 0
                    cccPhase = if (cccPhase == CccPhase.RESET) CccPhase.SUBSCRIBE else CccPhase.DONE
                }
                if (cccPhase != CccPhase.DONE) configureCcc(gatt)
                else {
                    log("INFO", "Client subscriptions ready link=$token DATA=notify INVITE=indicate ACK=notify mtu=${link.mtu}")
                    link.opened = true; emit(2, token, link.mtu, address)
                }
            }
            override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) = dispatch(gatt) {
                complete(link, status)
            }
            override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) = dispatch(gatt) {
                if (link.opened && characteristic.uuid in listOf(txUuid, inviteUuid, ackUuid))
                    emit(3, token, arg = wireChannel(characteristic.uuid), data = value.copyOf())
                else log("DEBUG", "Client notification ignored link=$token phase=$cccPhase uuid=${characteristic.uuid} bytes=${value.size}")
            }
            @Deprecated("API < 33")
            override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) = dispatch(gatt) {
                if (Build.VERSION.SDK_INT < 33 && link.opened && characteristic.uuid in listOf(txUuid, inviteUuid, ackUuid))
                    emit(3, token, arg = wireChannel(characteristic.uuid), data = characteristic.value.copyOf())
            }
        }
        link.gatt = checkNotNull(link.device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE,
            BluetoothDevice.PHY_LE_1M_MASK, worker)) { "connectGatt returned null" }
    }
    private fun pump() {
        if (generation == 0L) return
        val pending = links.values.filter { it.pending != null && !it.writing && it.opened }.sortedBy { it.token }
        val ordered = pending.filter { it.token > lastWriter } + pending.filter { it.token <= lastWriter }
        for (link in ordered) {
            if (!link.outgoing && writer != null) continue
            val bytes = checkNotNull(link.pending)
            val status = if (link.outgoing) {
                val characteristic = checkNotNull(when (link.channel) { 0 -> link.rx; 1 -> link.invite; else -> link.ack })
                val type = if (link.channel == 1) BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    else BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                if (Build.VERSION.SDK_INT >= 33) link.gatt!!.writeCharacteristic(characteristic, bytes, type)
                else {
                    characteristic.value = bytes; characteristic.writeType = type
                    if (link.gatt!!.writeCharacteristic(characteristic)) 0 else -1
                }
            } else {
                val characteristic = checkNotNull(when (link.channel) { 0 -> tx; 1 -> invite; else -> ack })
                if (Build.VERSION.SDK_INT >= 33) server!!.notifyCharacteristicChanged(link.device, characteristic, link.channel == 1, bytes)
                else {
                    characteristic.value = bytes
                    if (server!!.notifyCharacteristicChanged(link.device, characteristic, link.channel == 1)) 0 else -1
                }
            }
            if (status == 0) {
                if (link.busySince != 0L) {
                    log("DEBUG", "ATT resumed link=${link.token} channel=${link.channel} wait_ms=${SystemClock.elapsedRealtime() - link.busySince}")
                    link.busySince = 0
                }
                link.writing = true
                if (!link.outgoing) {
                    writer = link; lastWriter = link.token
                    check(worker.postDelayed(writerTimeout, 20_000)) { "Server completion timer rejected link=${link.token}" }
                }
            }
            else {
                if (Build.VERSION.SDK_INT >= 33 && status != BluetoothStatusCodes.ERROR_GATT_WRITE_REQUEST_BUSY) {
                    log("ERROR", "ATT submission failed link=${link.token} channel=${link.channel} outgoing=${link.outgoing} status=$status")
                    close(link, "ATT submission status=$status"); continue
                }
                if (link.busySince == 0L) {
                    link.busySince = SystemClock.elapsedRealtime()
                    log("DEBUG", "ATT busy link=${link.token} channel=${link.channel} status=$status; fragment retained")
                }
                worker.removeCallbacks(retry)
                check(worker.postDelayed(retry, 50)) { "ATT retry timer rejected link=${link.token} status=$status" }
                break
            }
        }
    }
    // Закрытый link остаётся tombstone до completion: callback Android содержит только device.
    private val writerTimeout = Runnable { writer?.let { link -> guarded {
        error("Server notification completion timeout link=${link.token} address=${link.device.address} " +
            "channel=${link.channel} bytes=${link.pending?.size} retired=${links[link.token] !== link}")
    } } }
    private val retry = Runnable { guarded { pump() } }
    private fun complete(link: Link, status: Int) {
        if (!link.writing) { log("WARN", "Stale completion link=${link.token} channel=${link.channel} status=$status"); return }
        if (status != 0) log("WARN", "ATT completion failed link=${link.token} channel=${link.channel} outgoing=${link.outgoing} status=$status")
        link.writing = false; link.pending = null
        emit(4, link.token, status)
        pump()
    }
    private fun close(link: Link, reason: String, report: Boolean = true, preserveAcl: Boolean = false) {
        links.remove(link.token)
        val profileOnly = profileTokens.remove(link.token)
        log(if (report && !preserveAcl) "WARN" else "INFO", "Closed link=${link.token} address=${link.device.address} outgoing=${link.outgoing} " +
            "opened=${link.opened} channel=${link.channel} pending=${link.pending?.size} writing=${link.writing} reason=$reason")
        link.priorityGatt?.let { gatt ->
            link.priorityGatt = null
            cleanup.retain(gatt, "priority GATT link=${link.token}") { gatt.close() }
        }
        if (link.outgoing) link.gatt?.let { gatt ->
            try { if (!profileOnly) gatt.disconnect() } catch (e: RuntimeException) { log("WARN", "Disconnect failed link=${link.token}: $e") }
            cleanup.retain(gatt, "client GATT link=${link.token}") { gatt.close() }
        } else if (!preserveAcl) server?.let { owner ->
            cleanup.retain(link, "incoming link=${link.token}") { if (server === owner) owner.cancelConnection(link.device) }
        }
        drainCleanup()
        if (report) emit(5, link.token, text = reason)
        pump()
    }
    private fun stop() {
        generation = 0
        advertisingPowerDbm.value = null
        worker.removeCallbacks(retry); worker.removeCallbacks(writerTimeout)
        links.values.toList().forEach { close(it, "radio stopped", false) }
        scan?.let { callback -> scanner?.let { owner -> cleanup.retain(callback, "scan") { owner.stopScan(callback) } } }
        advertisement?.let { callback -> advertiser?.let { owner -> cleanup.retain(callback, "advertisement") { owner.stopAdvertisingSet(callback) } } }
        server?.let { owner -> cleanup.retain(owner, "GATT server") { owner.close() } }
        scan = null; scanner = null; advertisement = null; advertiser = null; server = null
        tx = null; invite = null; ack = null; writer = null; beacon = byteArrayOf()
        incomingMtu.clear(); scanEndpoints.clear(); nameReads.clear(); localNode = byteArrayOf(); localName = byteArrayOf()
        drainCleanup()
    }
    private val cleanupRetry = Runnable {
        if (cleanup.retry()) {
            cleanupDelay = 250
            if (generation == 0L && NativeLib.isReady() && !NativeLib.isCoreStopping()) applySettings()
        } else scheduleCleanup()
    }
    private fun drainCleanup() {
        if (cleanup.retry()) { worker.removeCallbacks(cleanupRetry); cleanupDelay = 250 }
        else scheduleCleanup()
    }
    private fun scheduleCleanup() {
        worker.removeCallbacks(cleanupRetry)
        if (!worker.postDelayed(cleanupRetry, cleanupDelay)) log("ERROR", "Cleanup timer rejected; SDK owners retained")
        cleanupDelay = (cleanupDelay * 2).coerceAtMost(30_000)
    }
}
