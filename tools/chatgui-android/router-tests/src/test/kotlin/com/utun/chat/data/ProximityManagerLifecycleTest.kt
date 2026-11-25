package com.utun.chat.data

import android.content.Context
import android.hardware.SensorManager
import android.os.Looper
import android.os.Handler
import java.util.concurrent.atomic.AtomicReference
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий ProximityManager: доставка через main, отказы ОС и события старых регистраций без аппаратного датчика. */
class ProximityManagerLifecycleTest {
    private lateinit var context: Context
    private lateinit var manager: SensorManager
    private lateinit var proximity: ProximityManager

    @Before fun prepare() {
        Handler.rejectDelayed = false
        assertSame(Looper.getMainLooper(), Looper.myLooper())
        context = Context()
        manager = requireNotNull(context.sensorManager)
        proximity = ProximityManager(context)
        LogManager.entries.clear()
        LogManager.console = true
    }

    @After fun release() {
        Handler.rejectDelayed = false
        manager.failRemoval = false
        proximity.stop()
        Looper.getMainLooper().dispatchAll()
    }

    @Test fun repeatedStartAndStopOwnExactlyOneRegistration() {
        assertTrue(proximity.available)
        assertNull(proximity.near.value)
        repeat(3) { proximity.start() }
        assertEquals(1, manager.registerAttempts)
        assertEquals(1, manager.active.size)
        repeat(3) { proximity.stop() }
        assertNull(proximity.near.value)
        assertEquals(1, manager.removeAttempts)
        assertTrue(manager.active.isEmpty())
    }

    @Test fun firstSampleAndMaximumRangeDetermineNearAndFar() {
        proximity.start()
        assertNull(proximity.near.value)
        for ((distance, near) in listOf(0f to true, 2f to true, 5f to false, 0f to true)) {
            manager.deliver(0, distance)
            Looper.getMainLooper().dispatchAll()
            assertEquals(near, proximity.near.value)
        }
        assertEquals(3, LogManager.entries.count { it.category == "Proximity" &&
            (it.message.startsWith("NEAR") || it.message.startsWith("FAR")) })
    }

    @Test fun deviceWithoutSensorUsesFarFallbackUntilStop() {
        manager.sensor = null
        proximity = ProximityManager(context)
        assertFalse(proximity.available)
        repeat(2) { proximity.start() }
        assertEquals(false, proximity.near.value)
        assertEquals(0, manager.registerAttempts)
        assertEquals(1, LogManager.entries.count { it.level == "WARN" && it.category == "Proximity" })
        proximity.stop()
        assertNull(proximity.near.value)
    }

    @Test fun thresholdDeterminesNearBelowSensorMaximum() {
        proximity.start(2f)
        for ((distance, near) in listOf(1.9f to true, 2f to false, 3f to false, 0f to true, 5f to false)) {
            manager.deliver(0, distance)
            Looper.getMainLooper().dispatchAll()
            assertEquals(near, proximity.near.value)
        }
    }

    @Test fun thresholdChangeWaitsForFreshSampleAndRejectsOldCallback() {
        proximity.start(5f)
        manager.deliver(0, 3f); Looper.getMainLooper().dispatchAll()
        assertEquals(true, proximity.near.value)
        proximity.start(2f)
        assertNull(proximity.near.value)
        assertEquals(2, manager.registerAttempts)
        assertEquals(1, manager.active.size)
        manager.deliver(0, 0f); Looper.getMainLooper().dispatchAll()
        assertNull(proximity.near.value)
        manager.deliver(1, 3f); Looper.getMainLooper().dispatchAll()
        assertEquals(false, proximity.near.value)
    }

    @Test fun binarySensorKeepsFarAtMaximumEvenWithLargerThreshold() {
        manager.sensor = android.hardware.Sensor(1f)
        proximity = ProximityManager(context)
        for (threshold in listOf(0.1f, 5f, 10f)) {
            proximity.start(threshold)
            val index = manager.registrations.lastIndex
            manager.deliver(index, 0f); Looper.getMainLooper().dispatchAll()
            assertEquals(true, proximity.near.value)
            manager.deliver(index, 1f); Looper.getMainLooper().dispatchAll()
            assertEquals(false, proximity.near.value)
        }
    }

    @Test fun invalidThresholdIsLoggedWithoutRegistering() {
        for (threshold in listOf(0f, -1f, Float.NaN, Float.POSITIVE_INFINITY)) proximity.start(threshold)
        assertEquals(0, manager.registerAttempts)
        assertNull(proximity.near.value)
        assertEquals(4, LogManager.entries.count { it.level == "ERROR" && "Invalid start threshold" in it.message })
    }

    @Test fun missingSensorServiceRemainsUnknown() {
        context.sensorManager = null
        proximity = ProximityManager(context)
        assertFalse(proximity.available)
        proximity.start()
        assertNull(proximity.near.value)
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.category == "Proximity" })
    }

    @Test fun rejectedRegistrationRemainsUnknownAndCanRetry() {
        manager.acceptRegistration = false
        proximity.start()
        assertTrue(proximity.available) // Наличие датчика не обещает успешный start.
        assertNull(proximity.near.value)
        manager.deliver(0, 5f)
        Looper.getMainLooper().dispatchAll()
        assertNull(proximity.near.value)
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.message.contains("registration") })
        manager.acceptRegistration = true
        proximity.start()
        assertEquals(2, manager.registerAttempts)
        manager.deliver(1, 0f)
        Looper.getMainLooper().dispatchAll()
        assertEquals(true, proximity.near.value)
    }

    @Test fun registrationExceptionIsLoggedAndCanRetry() {
        manager.failRegistration = true
        proximity.start()
        assertNull(proximity.near.value)
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.message.contains("registration") })
        manager.failRegistration = false
        proximity.start()
        assertEquals(2, manager.registerAttempts)
        manager.deliver(0, 5f)
        Looper.getMainLooper().dispatchAll()
        assertNull(proximity.near.value)
        manager.deliver(1, 0f)
        Looper.getMainLooper().dispatchAll()
        assertEquals(true, proximity.near.value)
    }

    @Test fun removalExceptionInvalidatesReadingAndOldCallback() {
        proximity.start()
        manager.deliver(0, 0f)
        Looper.getMainLooper().dispatchAll()
        manager.failRemoval = true
        proximity.stop()
        assertNull(proximity.near.value)
        assertTrue(LogManager.entries.any { it.level == "WARN" && it.message.contains("removal") })
        manager.failRemoval = false
        proximity.start()
        manager.deliver(0, 5f)
        Looper.getMainLooper().dispatchAll()
        assertNull(proximity.near.value)
        manager.deliver(1, 0f)
        Looper.getMainLooper().dispatchAll()
        assertEquals(true, proximity.near.value)
    }

    @Test fun restartWaitsForFreshSampleAndIgnoresQueuedOldEvents() {
        proximity.start()
        manager.deliver(0, 5f)
        proximity.stop()
        proximity.start()
        Looper.getMainLooper().dispatchAll()
        assertNull(proximity.near.value)
        manager.deliver(1, 0f)
        Looper.getMainLooper().dispatchAll()
        assertEquals(true, proximity.near.value)
        manager.deliver(1, 5f)
        proximity.stop()
        Looper.getMainLooper().dispatchAll()
        assertNull(proximity.near.value)
    }

    @Test fun callbackUsesMainQueueAndRemovalUsesOriginalManager() {
        proximity.start()
        assertNull(onWorker { manager.deliver(0, 0f) })
        assertNull(proximity.near.value)
        Looper.getMainLooper().dispatchAll()
        assertEquals(true, proximity.near.value)
        val replacement = SensorManager()
        context.sensorManager = replacement
        proximity.stop()
        assertEquals(1, manager.removeAttempts)
        assertEquals(0, replacement.removeAttempts)
    }

    @Test fun backgroundStartIsRejectedBeforeRegistration() {
        assertTrue(onWorker { proximity.start() } is IllegalStateException)
        assertEquals(0, manager.registerAttempts)
        assertNull(proximity.near.value)
        assertTrue(LogManager.entries.any { it.level == "ERROR" && it.message.contains("main thread") })
        proximity.start()
        assertEquals(1, manager.registerAttempts)
    }

    @Test fun backgroundStopIsRejectedBeforeChangingReadingOrRegistration() {
        proximity.start()
        manager.deliver(0, 0f)
        Looper.getMainLooper().dispatchAll()
        assertTrue(onWorker { proximity.stop() } is IllegalStateException)
        assertEquals(true, proximity.near.value)
        assertEquals(0, manager.removeAttempts)
        proximity.stop()
        assertNull(proximity.near.value)
    }

    @Test fun failedRemovalIsRetriedWithoutAnotherStartAndKeepsItsOriginalManager() {
        proximity.start(); manager.failRemoval = true; proximity.stop()
        assertEquals(1, manager.active.size)
        context.sensorManager = SensorManager(); manager.failRemoval = false
        Looper.getMainLooper().advanceBy(250)
        assertTrue(manager.active.isEmpty()); assertEquals(2, manager.removeAttempts)
    }

    @Test fun unresolvedOldRegistrationBlocksNewStart() {
        proximity.start(); manager.failRemoval = true; proximity.stop(); proximity.start()
        assertEquals(1, manager.registerAttempts); assertNull(proximity.near.value)
        manager.failRemoval = false; proximity.start()
        assertEquals(2, manager.registerAttempts); assertEquals(1, manager.active.size)
    }

    @Test fun partialRegistrationFailureIsRetainedUntilUnregisterAck() {
        manager.failRegistrationAfter = true; manager.failRemoval = true; proximity.start()
        assertEquals(1, manager.active.size); assertNull(proximity.near.value)
        manager.failRemoval = false; manager.failRegistrationAfter = false
        Looper.getMainLooper().advanceBy(250)
        assertTrue(manager.active.isEmpty()); proximity.start(); assertEquals(1, manager.active.size)
    }

    @Test fun rejectedRetryTimerStillAllowsExplicitStopToReleaseRegistration() {
        proximity.start(); manager.failRemoval = true; Handler.rejectDelayed = true; proximity.stop()
        assertTrue(LogManager.entries.any { it.level == "ERROR" && "retry rejected" in it.message })
        manager.failRemoval = false; Handler.rejectDelayed = false; proximity.stop()
        assertTrue(manager.active.isEmpty())
    }

    @Test fun sensorLookupExceptionKeepsUnknownAndCanRecoverAtStart() {
        manager.failLookup = true; proximity = ProximityManager(context)
        assertFalse(proximity.available); proximity.start(); assertNull(proximity.near.value)
        manager.failLookup = false; proximity.start(); assertTrue(proximity.available)
        manager.deliver(0, 5f); Looper.getMainLooper().dispatchAll(); assertEquals(false, proximity.near.value)
    }

    @Test fun malformedReadingsRemainUnknownAndAreLoggedOnceUntilValidData() {
        proximity.start()
        val registration = manager.registrations.single()
        for (values in listOf(floatArrayOf(), floatArrayOf(Float.NaN), floatArrayOf(-1f)))
            registration.listener.onSensorChanged(android.hardware.SensorEvent(registration.sensor, values))
        assertNull(proximity.near.value)
        assertEquals(1, LogManager.entries.count { it.level == "WARN" && "Invalid reading" in it.message })
        manager.deliver(0, 0f); Looper.getMainLooper().dispatchAll(); assertEquals(true, proximity.near.value)
    }

    private fun onWorker(action: () -> Unit): Throwable? {
        val result = AtomicReference<Throwable?>()
        val worker = Thread({ try { action() } catch (error: Throwable) { result.set(error) } }, "proximity-test-worker")
        worker.start()
        worker.join(2_000)
        check(!worker.isAlive) { "proximity worker did not finish" }
        return result.get()
    }
}
