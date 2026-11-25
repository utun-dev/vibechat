package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class StreamFecParametersTest {
    @Test fun requiredPartsFitOrdinarySlotsWithoutLimitingTotalTo192() {
        assertTrue(StreamFecParameters.valid(192, 0))
        assertTrue(StreamFecParameters.valid(255, 63))
        assertTrue(StreamFecParameters.valid(255, 254))
        assertFalse(StreamFecParameters.valid(193, 0))
        assertFalse(StreamFecParameters.valid(255, 62))
        assertTrue(StreamFecParameters.valid(255, 0, 256))
        assertFalse(StreamFecParameters.valid(80, 0, 79))
        for ((n, r) in listOf(null to 0, 8 to null, 0 to 0, 256 to 64, 8 to -1, 8 to 8))
            assertFalse("n=$n r=$r", StreamFecParameters.valid(n, r))
    }
    @Test fun budgetLimitsAllowIndependentChildSettingsAndRejectInvalidRanges() {
        val defaults = StreamFecParameters.budgets.associate { it.key to it.defaultValue }
        assertTrue(StreamFecParameters.validBudgets(defaults))
        assertTrue(StreamFecParameters.validBudgets(defaults + ("stream_temporary_limit" to 1)))
        assertFalse(StreamFecParameters.validBudgets(defaults - "stream_regular_limit"))
        for (budget in StreamFecParameters.budgets) {
            assertFalse(StreamFecParameters.validBudgets(defaults + (budget.key to budget.range.first - 1)))
            assertFalse(StreamFecParameters.validBudgets(defaults + (budget.key to budget.range.last + 1)))
        }
    }
}
