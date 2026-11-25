package android.bluetooth

import android.content.Context
import com.utun.chat.data.LogManager
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

interface BluetoothProfile {
    companion object { const val HEADSET = 1 }
    interface ServiceListener {
        fun onServiceConnected(profile: Int, proxy: BluetoothProfile)
        fun onServiceDisconnected(profile: Int)
    }
}

class BluetoothHeadset(val id: Int) : BluetoothProfile {
    var connected = false
    val connectedDevices: List<Int> get() = if (connected) listOf(id) else emptyList()
    override fun toString() = "host-headset-$id"
}

/** Модель SDK36: proxy регистрируется до callback, disconnect сохраняет владение до closeProfileProxy.
 * Никаких Bluetooth/Binder-ресурсов ОС. Порядок callbacks задаёт тест, поля router не подменяются. */
class BluetoothAdapter {
    companion object { var current: BluetoothAdapter? = null; fun getDefaultAdapter() = current }
    data class Request(val listener: BluetoothProfile.ServiceListener, val proxy: BluetoothHeadset)
    val requests = mutableListOf<Request>()
    val live = mutableSetOf<BluetoothHeadset>()
    var closes = 0
    var closeAttempts = 0
    var rejectRequest = false
    var throwRequest = false
    var closeFailures = emptySet<BluetoothProfile>()
    var closeFailuresAfter = emptySet<BluetoothProfile>()
    var requestEntered: CountDownLatch? = null
    var requestReturn: CountDownLatch? = null

    fun getProfileProxy(context: Context, listener: BluetoothProfile.ServiceListener, profile: Int): Boolean {
        check(profile == BluetoothProfile.HEADSET)
        if (throwRequest) throw IllegalStateException("injected BT request failure")
        if (rejectRequest) return false
        val proxy = BluetoothHeadset(requests.size + 1)
        requests += Request(listener, proxy); live += proxy
        LogManager.addLog("INFO", "HostBluetooth", "request=${proxy.id} live=${live.size} callback=pending")
        requestEntered?.countDown()
        requestReturn?.let { check(it.await(5, TimeUnit.SECONDS)) { "profile request barrier timed out" } }
        return true
    }

    fun connect(index: Int) {
        val request = requests[index]; request.proxy.connected = true
        LogManager.addLog("INFO", "HostBluetooth", "connected callback proxy=${request.proxy.id}")
        request.listener.onServiceConnected(BluetoothProfile.HEADSET, request.proxy)
    }

    fun disconnect(index: Int) {
        val request = requests[index]; request.proxy.connected = false
        LogManager.addLog("INFO", "HostBluetooth", "disconnected callback proxy=${request.proxy.id} ownership retained")
        request.listener.onServiceDisconnected(BluetoothProfile.HEADSET)
    }

    fun closeProfileProxy(profile: Int, proxy: BluetoothProfile?) {
        check(profile == BluetoothProfile.HEADSET)
        if (proxy == null) return
        ++closeAttempts
        check(proxy !in closeFailures) { "BT close failed before effect proxy=$proxy" }
        if (!live.remove(proxy)) return // SDK close идемпотентен; запись удаляется только один раз.
        ++closes
        (proxy as BluetoothHeadset).connected = false
        LogManager.addLog("INFO", "HostBluetooth", "closed proxy=${proxy.id} live=${live.size}")
        check(proxy !in closeFailuresAfter) { "BT close failed after effect proxy=$proxy" }
    }
}
