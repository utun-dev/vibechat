package android.content

import android.media.AudioManager
import android.app.NotificationManager
import android.os.PowerManager
import android.os.Vibrator
import android.net.ConnectivityManager
import android.net.wifi.WifiManager
import android.hardware.SensorManager
import android.telephony.SubscriptionManager
import android.telephony.TelephonyManager
import com.utun.chat.data.HostConfigProvider
import java.io.File

/** Только host-тест: управляемые Android managers, без устройств/разрешений ОС. */
open class Context {
    companion object {
        const val AUDIO_SERVICE = "audio"; const val SENSOR_SERVICE = "sensor"; const val VIBRATOR_SERVICE = "vibrator"
        const val POWER_SERVICE = "power"
        const val WIFI_SERVICE = "wifi"
        const val WINDOW_SERVICE = "window"
        const val RECEIVER_NOT_EXPORTED = 4
    }
    val packageName = "com.utun.chat"
    val mainExecutor = java.util.concurrent.Executor { task -> android.os.Handler(android.os.Looper.getMainLooper()).post(task) }
    var cacheDir = File(System.getProperty("java.io.tmpdir"), "utun-router-host-cache")
    val contentResolver = ContentResolver()
    var vibrator: Vibrator? = Vibrator()
    var powerManager: PowerManager? = PowerManager()
    var failPowerService = false
    val resources = HostResources()
    var windowService: android.view.WindowManager? = android.view.WindowManager()
    val serviceStarts = mutableListOf<Intent>()
    val serviceStops = mutableListOf<Intent>()
    var failServiceStart = false
    var acceptServiceStart = true
    var failServiceStop = false
    var acceptServiceStop = true
    fun startForegroundService(intent: Intent): ComponentName? {
        serviceStarts.add(Intent(intent))
        check(!failServiceStart) { "foreground service start unavailable" }
        return if (acceptServiceStart) ComponentName(packageName, intent.component?.name.orEmpty()) else null
    }
    fun stopService(intent: Intent): Boolean {
        serviceStops.add(Intent(intent))
        check(!failServiceStop) { "service stop unavailable" }
        return acceptServiceStop
    }
    val activityAttempts = mutableListOf<Intent>()
    var onStartActivity: (Intent) -> Unit = {}
    var notificationManager: NotificationManager? = NotificationManager()
    var permissionGranted = true
    var connectivityManager: ConnectivityManager? = ConnectivityManager()
    var wifiManager: WifiManager? = WifiManager()
    var failWifiService = false
    var sensorManager: SensorManager? = SensorManager()
    var subscriptionManager: SubscriptionManager? = SubscriptionManager()
    var telephonyManager: TelephonyManager? = TelephonyManager()
    val configProvider = HostConfigProvider()
    val receivers = mutableMapOf<BroadcastReceiver, IntentFilter>()
    var receiverFailures = emptySet<String>()
    var registrationAttempts = 0
    var unregistrationAttempts = 0
    fun registerReceiver(receiver: BroadcastReceiver, filter: IntentFilter, flags: Int = 0) {
        registrationAttempts++
        check("register" !in receiverFailures) { "receiver registration failed" }
        receivers[receiver] = filter
        check("register after" !in receiverFailures) { "receiver registration failed after effect" }
    }
    fun unregisterReceiver(receiver: BroadcastReceiver) {
        unregistrationAttempts++
        check("unregister" !in receiverFailures) { "receiver unregistration failed" }
        require(receivers.remove(receiver) != null) { "receiver not registered" }
        check("unregister after" !in receiverFailures) { "receiver unregistration failed after effect" }
    }
    val applicationContext: Context get() = this
    fun startActivity(intent: Intent) {
        activityAttempts.add(Intent(intent))
        onStartActivity(intent)
    }
    fun getSystemService(name: String): Any? = when (name) {
        AUDIO_SERVICE -> AudioManager.shared
        SENSOR_SERVICE -> sensorManager
        VIBRATOR_SERVICE -> vibrator
        POWER_SERVICE -> { check(!failPowerService) { "power service lookup unavailable" }; powerManager }
        WIFI_SERVICE -> { check(!failWifiService) { "Wi-Fi service unavailable" }; wifiManager }
        WINDOW_SERVICE -> windowService
        else -> error("Unknown service: $name")
    }
    fun <T> getSystemService(type: Class<T>): T? {
        return type.cast(when (type) {
            ConnectivityManager::class.java -> connectivityManager
            SensorManager::class.java -> sensorManager
            SubscriptionManager::class.java -> subscriptionManager
            TelephonyManager::class.java -> telephonyManager
            NotificationManager::class.java -> notificationManager
            PowerManager::class.java -> { check(!failPowerService) { "power service lookup unavailable" }; powerManager }
            else -> error("Unknown service: $type")
        })
    }
    fun checkSelfPermission(name: String): Int = if (permissionGranted) 0 else -1
}

class HostResources {
    val displayMetrics = HostDisplayMetrics()
    fun getResourceEntryName(id: Int) = "resource_$id"
}
class HostDisplayMetrics { var density = 1f; var widthPixels = 1080 }
