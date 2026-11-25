package com.utun.chat.data

import android.content.Context
import android.provider.Settings
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/** Настоящий адаптер; host Android задаёт отсутствие/отказ сервиса и запуск системных экранов. */
class BatteryOptimizationPermissionTest {
    private lateinit var context: Context
    @Before fun prepare() { context = Context(); LogManager.entries.clear() }
    private fun error(fragment: String) = assertTrue("missing error $fragment: ${LogManager.entries}",
        LogManager.entries.any { it.level == "ERROR" && fragment in it.message })

    @Test fun eachQueryUsesTheCurrentOperatingSystemStateForThisPackage() {
        val power = context.powerManager!!
        assertEquals(false, BatteryOptimizationPermission.granted(context))
        power.ignoringBatteryOptimizations = true
        assertEquals(true, BatteryOptimizationPermission.granted(context))
        power.ignoringBatteryOptimizations = false
        assertEquals(false, BatteryOptimizationPermission.granted(context))
        assertEquals(List(3) { context.packageName }, power.optimizationQueries)
        assertTrue(context.activityAttempts.isEmpty())
    }

    @Test fun absentPowerServiceIsUnknownAndDoesNotOpenSettings() {
        context.powerManager = null
        assertNull(BatteryOptimizationPermission.granted(context))
        error("Power service unavailable")
        assertTrue(context.activityAttempts.isEmpty())
    }

    @Test fun serviceLookupFailureIsUnknownInsteadOfCrashingTheSettingsScreen() {
        context.failPowerService = true
        assertNull(BatteryOptimizationPermission.granted(context))
        error("power service lookup unavailable")
    }

    @Test fun failedQueryDoesNotReuseAnEarlierGrantAndCanRecover() {
        val power = context.powerManager!!
        power.ignoringBatteryOptimizations = true
        assertEquals(true, BatteryOptimizationPermission.granted(context))
        power.failures = setOf("optimization_query")
        assertNull(BatteryOptimizationPermission.granted(context))
        error("battery optimization query unavailable")
        power.failures = emptySet(); power.ignoringBatteryOptimizations = false
        assertEquals(false, BatteryOptimizationPermission.granted(context))
    }

    @Test fun directRequestTargetsThisPackageButDoesNotGrantAnExemption() {
        assertTrue(BatteryOptimizationPermission.request(context, "setup"))
        val intent = context.activityAttempts.single()
        assertEquals(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, intent.action)
        assertEquals("package:${context.packageName}", intent.data.toString())
        assertFalse(context.powerManager!!.ignoringBatteryOptimizations)
        assertTrue(context.powerManager!!.optimizationQueries.isEmpty())
        assertTrue(LogManager.entries.any { it.level == "INFO" && "source=setup" in it.message })
    }

    @Test fun failedDirectRequestFallsBackToGeneralSettingsWithoutPackageUri() {
        for (failure in listOf(IllegalStateException("no direct handler"), SecurityException("direct request denied"))) {
            context = Context()
            context.onStartActivity = { if (it.action == Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS) throw failure }
            assertTrue(BatteryOptimizationPermission.request(context, "settings"))
            assertEquals(listOf(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS),
                context.activityAttempts.map { it.action })
            assertNull(context.activityAttempts.last().data)
            assertTrue(LogManager.entries.any { it.level == "WARN" && "source=settings" in it.message && failure.toString() in it.message })
        }
    }

    @Test fun bothLaunchFailuresReturnFalseAndReportTheFinalFailure() {
        context.onStartActivity = { throw SecurityException("settings unavailable") }
        assertFalse(BatteryOptimizationPermission.request(context, "setup"))
        assertEquals(2, context.activityAttempts.size)
        assertEquals(2, LogManager.entries.count { it.level == "WARN" })
        error("request failed: source=setup")
        assertFalse(context.powerManager!!.ignoringBatteryOptimizations)
    }

    @Test fun failedRequestDoesNotPreventAnotherUserAttempt() {
        context.onStartActivity = { throw IllegalStateException("handler unavailable") }
        assertFalse(BatteryOptimizationPermission.request(context, "settings"))
        context.onStartActivity = {}
        assertTrue(BatteryOptimizationPermission.request(context, "settings"))
        assertEquals(3, context.activityAttempts.size)
        assertEquals(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, context.activityAttempts.last().action)
    }
}
