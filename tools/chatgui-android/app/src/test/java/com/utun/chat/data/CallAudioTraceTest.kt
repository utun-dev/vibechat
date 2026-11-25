package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class CallAudioTraceTest {
    private fun sample(t: Long, queueMs: Long = 40, route: Long = 1): LongArray = LongArray(CallAudioTrace.WIDTH).also {
        it[0] = t; it[1] = route; it[2] = 2; it[3] = t * 48 / 1000 + queueMs * 48; it[4] = t * 48 / 1000
        it[18] = 60; it[20] = 960; it[21] = 100
    }

    @Test fun fixedRingCopiesSamplesAndReportsOverflowWithoutLoggingFromProducer() {
        val logs = mutableListOf<String>()
        val trace = CallAudioTrace(1) { _, text -> logs.add(text) }.apply { enabled = true }
        val row = sample(0)
        repeat(600) { row[0] = it * 20_000L; trace.record(row) }
        assertTrue(logs.isEmpty())
        row[0] = -1
        trace.flush(true)
        assertTrue(logs.any { it.contains("lost=88") })
        assertTrue(logs.any { it.contains("triggerUs=1760000") })
    }

    @Test fun sustainedHighQueueProducesOneEventAndRecoveryRearmsIt() {
        val logs = mutableListOf<String>()
        val trace = CallAudioTrace(2) { _, text -> logs.add(text) }.apply { enabled = true }
        repeat(1500) {
            trace.record(sample(it * 20_000L, if (it in 700..899) 40 else 140))
            if (it % 10 == 0) trace.flush()
        }
        trace.flush(true)
        assertEquals(2, logs.count { it.contains(" fault=4 queueUs=") })
        assertEquals(1, logs.count { it.contains(" recovered ") })
        assertEquals(2, logs.count { it.contains("triggerUs=") })
        assertTrue(logs.count { it.contains("audio summary") } in 28..31)
    }

    @Test fun routeEventIncludesHistoryAndPostWindowAndDoesNotRetriggerEveryFrame() {
        val logs = mutableListOf<String>()
        val trace = CallAudioTrace(3) { _, text -> logs.add(text) }.apply { enabled = true }
        repeat(1000) {
            trace.record(sample(it * 20_000L, route = if (it < 750) 1 else 2))
            if (it % 10 == 0) trace.flush()
        }
        trace.flush(true)
        assertEquals(2, logs.count { it.contains("triggerUs=") })
        assertTrue(logs.any { it.contains("triggerUs=15000000 reasons=2") })
        assertTrue(logs.any { it.contains("rows=13000000,") })
        assertTrue(logs.any { it.contains("18000000,2,2,") })
    }

    @Test fun disabledLoggingAndShortCallDoNotLoseControlOfBuffer() {
        val logs = mutableListOf<String>()
        val trace = CallAudioTrace(4) { _, text -> logs.add(text) }
        trace.record(sample(0)); trace.flush(true)
        assertTrue(logs.isEmpty())
        trace.enabled = true
        trace.record(sample(20_000)); trace.flush(true)
        assertTrue(logs.any { it.contains("partial=true") })
        logs.clear(); trace.enabled = false
        trace.record(sample(40_000)); trace.flush(true)
        assertTrue(logs.isEmpty())
    }

    @Test fun deviceSnapshotKeepsItsOwnQueryTimesAndDoesNotOverwritePlaybackMeasurements() {
        val logs = mutableListOf<String>()
        val trace = CallAudioTrace(5) { _, text -> logs.add(text) }.apply { enabled = true }
        val row = sample(1_000_000)
        row[6] = 11; row[25] = 22; row[26] = 33; row[27] = 90; row[28] = 44; row[29] = 150
        trace.device = CallAudioTrace.DeviceSample(3, 2, 0, 7696, 1920, 48000, 900_000_000, true,
            900_000, 950_000, 10_000, 30_000, 10_000)
        val saved = trace.device
        trace.device = CallAudioTrace.DeviceSample() // Уже захваченный снимок остаётся неизменным.
        saved.copyTo(row)
        trace.record(row); trace.flush(true)
        val summary = logs.first { it.contains("audio summary") }
        assertTrue(summary.contains("writeCallUs=11/11/11 headCallUs=22/22/22 progressCallUs=33/33/33"))
        assertTrue(summary.contains("writeSpanUs=90/90/90 jbSnapshotUs=44/44/44 blockUs=150/150/150"))
        assertTrue(summary.contains("deviceAgeUs=50000 stampQueryUs=10000/10000/10000 routeQueryUs=30000/30000/30000"))
        assertEquals(900_000L, row[14]); assertEquals(950_000L, row[31])
    }

    @Test fun firstDeviceSnapshotIsNotReportedAsRouteChangeOrUnderrun() {
        val logs = mutableListOf<String>()
        val trace = CallAudioTrace(6) { _, text -> logs.add(text) }.apply { enabled = true }
        val first = sample(0)
        CallAudioTrace.DeviceSample().copyTo(first)
        trace.record(first)
        trace.record(sample(20_000))
        trace.flush(true)
        assertTrue(logs.any { it.contains("reasons=1 events=1") })
        assertFalse(logs.any { it.contains(" fault=") && it.contains("audio trace") })
    }
}
