package com.utun.chat.data

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.Looper
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Показания приближения для блокировки UI аудиозвонка и выбора громкой связи видеозвонка.
 * start/stop вызываются на main; callbacks доставляются туда же. Повторные вызовы безопасны,
 * события остановленной регистрации игнорируются, новый start ждёт нового показания.
 * near: true — расстояние меньше порога и maximumRange, false — FAR; null — показания ещё нет или монитор остановлен.
 * Без датчика start публикует false (экран не блокируется). Ошибка доступа/регистрации оставляет null.
 * Runtime-разрешения не нужны; отказы ОС логируются, после отказа регистрации start можно повторить.
 * Failed unregister сохраняет listener/manager и повторяется на main через 250 мс..30 с; новый start ждёт его ACK.
 */
class ProximityManager(private val context: Context) {
    companion object { private val retained = java.util.concurrent.ConcurrentHashMap.newKeySet<ProximityManager>() }

    private val main = Handler(Looper.getMainLooper())
    private var sensorManager: SensorManager? = null
    private var sensor: Sensor? = null
    private var lookupFailed = false
    private val retired = mutableMapOf<SensorEventListener, SensorManager>()
    private var scheduled = false
    private var retryDelay = 250L
    private val retry = Runnable { scheduled = false; cleanup() }
    init { lookupSensor() }

    private fun lookupSensor() {
        try {
            sensorManager = context.getSystemService(SensorManager::class.java)
            sensor = sensorManager?.getDefaultSensor(Sensor.TYPE_PROXIMITY)
            lookupFailed = sensorManager == null
        } catch (e: Exception) {
            lookupFailed = true; sensor = null
            LogManager.addLog("ERROR", "Proximity", "Sensor lookup failed; proximity remains unknown: $e")
        }
    }

    // null — первое показание ещё не получено; это не подтверждённое FAR.
    private val _near = MutableStateFlow<Boolean?>(null)
    val near: StateFlow<Boolean?> = _near

    private var listener: SensorEventListener? = null
    private var started = false
    private var thresholdCm = 5f

    /** Датчик обнаружен при создании объекта; успешная регистрация start этим не гарантируется. */
    val available: Boolean
        get() = sensor != null

    /** Начать наблюдение на main с порогом в см; смена порога ждёт нового показания. При отказе остаётся UNKNOWN. */
    fun start(thresholdCm: Float = 5f) {
        checkMain()
        if (!thresholdCm.isFinite() || thresholdCm <= 0) {
            LogManager.addLog("ERROR", "Proximity", "Invalid start threshold=$thresholdCm cm")
            return
        }
        if (started && this.thresholdCm == thresholdCm) return
        if (started) stop()
        this.thresholdCm = thresholdCm
        cleanup()
        if (retired.isNotEmpty()) {
            LogManager.addLog("ERROR", "Proximity", "Sensor start blocked by previous unregister; owner retained")
            return
        }
        if (lookupFailed) lookupSensor()
        if (lookupFailed && sensorManager != null) return
        val manager = sensorManager
        if (manager == null) {
            LogManager.addLog("ERROR", "Proximity", "sensor service unavailable; proximity remains unknown")
            return
        }
        started = true
        val s = sensor
        if (s == null) {
            LogManager.addLog("WARN", "Proximity", "no proximity sensor on this device")
            _near.value = false
            return
        }
        val l = object : SensorEventListener {
            private var badReading = false
            override fun onSensorChanged(event: SensorEvent) {
                if (!started || listener !== this) return // Запоздалое событие предыдущей регистрации не относится к текущему ожиданию.
                val value = event.values.firstOrNull()
                val range = event.sensor.maximumRange
                if (value == null || !value.isFinite() || value < 0 || !range.isFinite() || range <= 0) {
                    _near.value = null
                    if (!badReading) LogManager.addLog("WARN", "Proximity", "Invalid reading value=$value maximumRange=$range; remains unknown")
                    badReading = true; return
                }
                badReading = false
                val nearNow = value < range && value < thresholdCm
                if (_near.value != nearNow) {
                    _near.value = nearNow
                    LogManager.addLog("INFO", "Proximity",
                        "${if (nearNow) "NEAR" else "FAR"} v=$value max=$range threshold=$thresholdCm")
                }
            }

            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
        }
        listener = l
        val registered = try {
            manager.registerListener(l, s, SensorManager.SENSOR_DELAY_NORMAL, main)
        } catch (e: Exception) {
            stop()
            LogManager.addLog("ERROR", "Proximity", "sensor registration failed; proximity remains unknown: ${e.javaClass.simpleName}: ${e.message}")
            return
        }
        if (!registered) {
            stop()
            LogManager.addLog("WARN", "Proximity", "sensor registration failed; proximity remains unknown")
            return
        }
        LogManager.addLog("INFO", "Proximity", "started maxRange=${s.maximumRange} threshold=$thresholdCm cm")
    }

    /** Сначала запретить события регистрации и сбросить показание, затем снять слушатель; только main. */
    fun stop() {
        checkMain()
        val current = listener
        val wasStarted = started
        listener = null
        started = false
        _near.value = null
        if (current != null) retired[current] = checkNotNull(sensorManager)
        cleanup()
        if (wasStarted) LogManager.addLog("INFO", "Proximity", "stopped cleanupPending=${retired.isNotEmpty()}")
    }

    private fun cleanup() {
        for ((old, manager) in retired.toMap()) try {
            manager.unregisterListener(old); retired.remove(old)
            LogManager.addLog("INFO", "Proximity", "Sensor listener removal confirmed owner=${System.identityHashCode(this)}")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "Proximity", "sensor listener removal failed; owner retained: $e")
        }
        if (retired.isEmpty()) {
            main.removeCallbacks(retry); scheduled = false; retained.remove(this); retryDelay = 250L
        } else {
            retained.add(this)
            if (!scheduled) {
                scheduled = main.postDelayed(retry, retryDelay)
                if (!scheduled) LogManager.addLog("ERROR", "Proximity", "Sensor cleanup retry rejected; owner retained")
                else retryDelay = (retryDelay * 2).coerceAtMost(30_000)
            }
        }
    }

    private fun checkMain() {
        check(Looper.myLooper() === Looper.getMainLooper()) {
            val message = "ProximityManager requires main thread: caller=${Thread.currentThread().name}"
            LogManager.addLog("ERROR", "Proximity", message)
            message
        }
    }
}
