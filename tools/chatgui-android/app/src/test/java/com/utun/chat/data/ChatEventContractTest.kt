package com.utun.chat.data

import java.io.File
import org.junit.Assert.*
import org.junit.Test

/** Проверяет настоящие C-объявления и JNI-emitter: один номер не может обозначать разные бинарные форматы. */
class ChatEventContractTest {
    private val events = Regex("(?m)^#define\\s+(CHAT_EVT_[A-Z_]+)\\s+(\\d+)\\b")
        .findAll(File(requireNotNull(System.getProperty("utun.chatEventHeader"))).readText())
        .associate { it.groupValues[1] to it.groupValues[2].toInt() }
    private val emitter = File(requireNotNull(System.getProperty("utun.androidBridgeSource"))).readText()
        .substringAfter("static void android_avatar_snapshot_done(").substringBefore("static void android_avatar_snapshot(")
    private val emitted = requireNotNull(Regex("jni_on_event\\((\\w+),").find(emitter)) {
        "android_avatar_snapshot_done must emit the snapshot event"
    }.groupValues[1]
    private val snapshotEvent get() = emitted.toIntOrNull() ?: requireNotNull(events[emitted])

    @Test fun groupSnapshotAndFeedbackUseDifferentEventNumbers() {
        assertNotEquals("different payloads share the same event number", events.getValue("CHAT_EVT_FEEDBACK_CHANGED"), snapshotEvent)
    }

    @Test fun snapshotEmitterUsesTheSharedEventRegistry() {
        assertTrue("JNI snapshot event must have a named declaration: $emitted", emitted in events)
        assertEquals("duplicate event numbers in chat_event.h", events.size, events.values.toSet().size)
    }

    @Test fun kotlinSnapshotNumberMatchesTheNativeRegistryAndEmitter() {
        assertEquals(events.getValue("CHAT_EVT_GROUP_AVATAR_SNAPSHOT"), AvatarProtocol.GROUP_SNAPSHOT_EVENT)
        assertEquals(snapshotEvent, AvatarProtocol.GROUP_SNAPSHOT_EVENT)
    }
}
