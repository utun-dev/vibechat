package android.bluetooth

import java.util.UUID

/** Только SDK-граница JVM-теста: логика команд, владение writer и callbacks берутся из production BleRadio. */
class BluetoothDevice(val address: String)
class BluetoothGattCharacteristic(val uuid: UUID = UUID(0, 0))
data class BleTestWrite(val bytes: ByteArray, val mode: Int)
class BluetoothGatt {
    val writes = mutableListOf<BleTestWrite>()
    var closed = false
    var disconnects = 0
    fun writeCharacteristic(characteristic: BluetoothGattCharacteristic, bytes: ByteArray, type: Int): Int {
        check(!closed); writes.add(BleTestWrite(bytes.copyOf(), type)); return 0
    }
    fun disconnect() { disconnects++ }
    fun close() { closed = true }
}
open class BluetoothGattServerCallback {
    open fun onNotificationSent(device: BluetoothDevice, status: Int) {}
}
class BluetoothGattServer {
    val writes = mutableListOf<BleTestWrite>()
    var closed = false
    fun notifyCharacteristicChanged(device: BluetoothDevice, characteristic: BluetoothGattCharacteristic,
                                    confirm: Boolean, bytes: ByteArray): Int {
        check(!closed); writes.add(BleTestWrite(bytes.copyOf(), if (confirm) 1 else 0)); return 0
    }
    fun cancelConnection(device: BluetoothDevice) {}
    fun close() { closed = true }
}
