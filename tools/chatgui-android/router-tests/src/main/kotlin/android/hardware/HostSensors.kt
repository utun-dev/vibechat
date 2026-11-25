package android.hardware

import android.os.Handler
import android.os.Looper

/** Только host-тест: очередь датчика, результаты регистрации и ошибки ОС задаёт тест. */
class Sensor(val maximumRange: Float = 5f) { companion object { const val TYPE_PROXIMITY = 8 } }
class SensorEvent(val sensor: Sensor, val values: FloatArray)
interface SensorEventListener {
    fun onSensorChanged(event: SensorEvent)
    fun onAccuracyChanged(sensor: Sensor?, accuracy: Int)
}

class SensorManager {
    companion object { const val SENSOR_DELAY_NORMAL = 3 }
    data class Registration(val listener: SensorEventListener, val sensor: Sensor, val handler: Handler)
    var sensor: Sensor? = Sensor()
    var acceptRegistration = true
    var failRegistration = false
    var failRegistrationAfter = false
    var failRemoval = false
    var failLookup = false
    var registerAttempts = 0
    var removeAttempts = 0
    val registrations = mutableListOf<Registration>()
    val active = mutableSetOf<SensorEventListener>()

    fun getDefaultSensor(type: Int): Sensor? { check(type == Sensor.TYPE_PROXIMITY && !failLookup); return sensor }
    fun registerListener(listener: SensorEventListener, sensor: Sensor, delay: Int,
        handler: Handler = Handler(Looper.getMainLooper())): Boolean {
        check(delay == SENSOR_DELAY_NORMAL)
        registerAttempts++
        registrations.add(Registration(listener, sensor, handler))
        check(!failRegistration) { "sensor registration unavailable" }
        if (!acceptRegistration) return false
        active.add(listener)
        check(!failRegistrationAfter) { "sensor registration failed after effect" }
        return true
    }
    fun unregisterListener(listener: SensorEventListener) {
        removeAttempts++
        check(!failRemoval) { "sensor removal unavailable" }
        active.remove(listener)
    }

    /** Можно доставить и старую регистрацию: модель события, уже переданного ОС в очередь приложения. */
    fun deliver(index: Int, distance: Float) {
        val registration = registrations[index]
        registration.handler.post {
            registration.listener.onSensorChanged(SensorEvent(registration.sensor, floatArrayOf(distance)))
        }
    }
}
