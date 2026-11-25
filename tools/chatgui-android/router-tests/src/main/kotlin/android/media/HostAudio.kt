package android.media

import android.os.Handler

open class AudioDeviceCallback {
    open fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {}
    open fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {}
}

class AudioDeviceInfo(val id: Int, val type: Int, val isSink: Boolean, val isSource: Boolean, val productName: String) {
    companion object {
        const val TYPE_BUILTIN_SPEAKER = 2; const val TYPE_BUILTIN_EARPIECE = 1; const val TYPE_BUILTIN_MIC = 15
        const val TYPE_WIRED_HEADSET = 3; const val TYPE_WIRED_HEADPHONES = 4; const val TYPE_USB_HEADSET = 22
        const val TYPE_USB_DEVICE = 11; const val TYPE_USB_ACCESSORY = 12; const val TYPE_BLUETOOTH_SCO = 7
        const val TYPE_BLUETOOTH_A2DP = 8; const val TYPE_BLE_HEADSET = 26
    }
}

/** Только host-тест: маршруты, режим звонка и управляемые запросы audio focus. */
class AudioManager {
    companion object {
        const val GET_DEVICES_ALL = 3
        const val GET_DEVICES_OUTPUTS = 2
        const val AUDIO_SESSION_ID_GENERATE = 0
        const val RINGER_MODE_SILENT = 0; const val RINGER_MODE_VIBRATE = 1; const val RINGER_MODE_NORMAL = 2
        const val STREAM_RING = 2
        const val MODE_NORMAL = 0; const val MODE_IN_COMMUNICATION = 3
        const val AUDIOFOCUS_GAIN_TRANSIENT = 2; const val AUDIOFOCUS_GAIN = 1
        const val AUDIOFOCUS_LOSS = -1; const val AUDIOFOCUS_LOSS_TRANSIENT = -2; const val AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK = -3
        const val AUDIOFOCUS_REQUEST_GRANTED = 1; const val AUDIOFOCUS_REQUEST_FAILED = 0
        var shared = AudioManager()
    }
    var failRingerMode = false
    var ringerMode = RINGER_MODE_NORMAL
        get() { check(!failRingerMode) { "ringer mode unavailable" }; return field }
    var focusResult = AUDIOFOCUS_REQUEST_GRANTED
    var abandonResult = AUDIOFOCUS_REQUEST_GRANTED
    var failFocus = false
    var failFocusAfter = false
    var failAbandon = false
    var failAbandonAfter = false
    val focusHistory = mutableListOf<AudioFocusRequest>()
    val abandoned = mutableListOf<AudioFocusRequest>()
    val focusOwners = mutableSetOf<AudioFocusRequest>()
    fun requestAudioFocus(request: AudioFocusRequest): Int {
        focusHistory.add(request)
        check(!failFocus) { "focus unavailable" }
        if (focusResult == AUDIOFOCUS_REQUEST_GRANTED) focusOwners.add(request)
        check(!failFocusAfter) { "focus failed after effect" }
        return focusResult
    }
    fun abandonAudioFocusRequest(request: AudioFocusRequest): Int {
        abandoned.add(request)
        check(!failAbandon) { "abandon unavailable" }
        if (abandonResult == AUDIOFOCUS_REQUEST_GRANTED) focusOwners.remove(request)
        check(!failAbandonAfter) { "abandon failed after effect" }
        return abandonResult
    }
    val operations = mutableListOf<String>()
    var failures = emptySet<String>()
    private fun operation(name: String) {
        operations.add(name)
        check(name !in failures) { "audio operation failed: $name" }
    }
    var isBluetoothScoOn = false
        set(value) { operation("sco=$value"); field = value; operation("sco after=$value") }
    var isSpeakerphoneOn = false
        set(value) { operation("speaker=$value"); field = value }
    var mode = 0
        set(value) { operation("mode=$value"); field = value; operation("mode after=$value") }
    val callbacks = mutableSetOf<AudioDeviceCallback>()
    fun interface OnCommunicationDeviceChangedListener { fun onCommunicationDeviceChanged(device: AudioDeviceInfo?) }
    val communicationListeners = mutableSetOf<OnCommunicationDeviceChangedListener>()
    fun addOnCommunicationDeviceChangedListener(executor: java.util.concurrent.Executor, listener: OnCommunicationDeviceChangedListener) {
        operation("communication add"); communicationListeners.add(listener); operation("communication add after")
    }
    fun removeOnCommunicationDeviceChangedListener(listener: OnCommunicationDeviceChangedListener) {
        operation("communication remove"); communicationListeners.remove(listener); operation("communication remove after")
    }
    val callbackHistory = mutableListOf<AudioDeviceCallback>()
    var devices = arrayOf(AudioDeviceInfo(1, 2, true, false, "speaker"), AudioDeviceInfo(2, 15, false, true, "mic"))
    var communicationDevices: List<AudioDeviceInfo>? = null
    var communicationDevice: AudioDeviceInfo? = null
        private set
    var acceptCommunicationDevice = true
    val availableCommunicationDevices: List<AudioDeviceInfo> get() {
        operation("available")
        return communicationDevices ?: devices.filter { it.isSink && it.type != AudioDeviceInfo.TYPE_BLUETOOTH_A2DP }
    }
    fun registerAudioDeviceCallback(callback: AudioDeviceCallback, handler: Handler) {
        callbackHistory.add(callback)
        operation("register")
        check(callbacks.add(callback))
        operation("register after")
    }
    fun unregisterAudioDeviceCallback(callback: AudioDeviceCallback) {
        operation("unregister")
        callbacks.remove(callback) // Android допускает отсутствие такой регистрации.
        operation("unregister after")
    }
    fun getDevices(flags: Int): Array<AudioDeviceInfo> {
        operation("devices"); check(flags == GET_DEVICES_ALL || flags == GET_DEVICES_OUTPUTS)
        return devices.filter { flags == GET_DEVICES_ALL || it.isSink }.toTypedArray()
    }
    fun setCommunicationDevice(device: AudioDeviceInfo): Boolean {
        operation("select=${device.id}")
        if (!acceptCommunicationDevice || device !in availableCommunicationDevices) return false
        communicationDevice = device
        operation("select after=${device.id}")
        return true
    }
    var legacyScoStarted = false
        private set
    fun clearCommunicationDevice() { operation("clear"); communicationDevice = null; operation("clear after") }
    fun startBluetoothSco() { operation("start sco"); legacyScoStarted = true; operation("start sco after") }
    fun stopBluetoothSco() { operation("stop sco"); legacyScoStarted = false; operation("stop sco after") }
}
