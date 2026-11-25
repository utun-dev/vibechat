package com.utun.chat.data

/** Один BLE HandlerThread: сильное владение SDK-ресурсами до успешного close/stop.
 * Отказ одного release не мешает остальным; новый radio запуск разрешён лишь после retry=true. */
internal class BleRadioCleanup(private val log: (String, String) -> Unit) {
    private data class Resource(val name: String, val close: () -> Unit)
    private val pending = java.util.IdentityHashMap<Any, Resource>()
    val isEmpty: Boolean get() = pending.isEmpty()

    fun retain(key: Any, name: String, close: () -> Unit) {
        if (!pending.containsKey(key)) pending[key] = Resource(name, close)
    }

    fun retry(): Boolean {
        for ((key, resource) in pending.entries.map { it.key to it.value }) {
            try {
                resource.close()
                pending.remove(key)
                log("INFO", "Released ${resource.name}")
            } catch (e: RuntimeException) { log("ERROR", "Release failed ${resource.name}; retained for retry: $e") }
        }
        return pending.isEmpty()
    }
}
