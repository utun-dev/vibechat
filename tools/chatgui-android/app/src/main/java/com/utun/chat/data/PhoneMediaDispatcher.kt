package com.utun.chat.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** Main-thread диспетчер перехода к телефону. Отмена запрещает запуск, но не отменяет release владельцев.
 * leave вызывается после освобождения сервиса и Telecom; тайм-аут также не снимает резервирование. */
class PhoneMediaDispatcher(private val scope: CoroutineScope) {
    @Volatile private var owner = 0L
    private var wanted = false
    var job: Job? = null
        private set
    val blocked get() = owner != 0L

    fun enter(id: Long, release: suspend () -> Unit, activate: suspend () -> Unit, failed: (Exception) -> Unit) {
        if (owner == id) return
        check(owner == 0L) { "Phone media already owned by $owner" }
        owner = id; wanted = true
        job = scope.launch(start = CoroutineStart.LAZY) {
            val deadline = launch {
                delay(10_000)
                if (owner == id && wanted) {
                    wanted = false
                    LogManager.addLog("ERROR", "call", "media preparation timeout id=$id; release continues")
                    failed(IllegalStateException("Media preparation timeout"))
                }
            }
            try {
                LogManager.addLog("INFO", "call", "media transition begins id=$id")
                while (true) {
                    try { release(); break } catch (e: Exception) {
                        LogManager.addLog("ERROR", "call", "media release retained id=$id: $e")
                        if (wanted) { wanted = false; failed(e) }
                        delay(250)
                    }
                }
                if (owner == id && wanted) activate()
                else LogManager.addLog("INFO", "call", "media released after cancellation id=$id")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "call", "media transition failed id=$id: $e")
                if (owner == id && wanted) { wanted = false; failed(e) }
            } finally { deadline.cancel() }
        }
        job!!.start()
    }

    fun cancel(id: Long) { if (owner == id) wanted = false }
    fun leave(id: Long) {
        if (owner != id) return
        check(job?.isCompleted != false) { "Media release still running id=$id" }
        owner = 0L; wanted = false
        LogManager.addLog("INFO", "call", "media mode restored id=$id")
    }
}
