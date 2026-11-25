package com.utun.chat.data

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.retryWhen
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** Очередь Application на main: сохранить выбор, затем отправить runtime-обновление без зависимости от UI scope.
 * submit принимает неизменный снимок аргументов caller; отмена await не отменяет запись.
 * awaitIdle ставит барьер после принятых изменений перед формированием restart-конфига. */
class SettingsOperations(private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate),
                         private val log: (String, String) -> Unit = { level, message -> LogManager.addLog(level, "Settings", message) }) {
    private val mutex = Mutex()
    private val _failed = MutableStateFlow(false)
    val failed = _failed.asStateFlow()

    fun submit(name: String, operation: suspend () -> Unit): Deferred<Unit> = scope.async(start = CoroutineStart.UNDISPATCHED) {
        mutex.withLock {
            try {
                operation()
                log("INFO", "Settings operation completed name=$name")
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { reportFailure(name, e); throw e }
        }
    }

    fun reportFailure(name: String, error: Exception) {
        _failed.value = true
        log("ERROR", "Setting failed name=$name: ${error.javaClass.simpleName}: ${error.message}")
    }
    fun dismissFailure() { _failed.value = false }
    suspend fun awaitIdle() { mutex.withLock { } }

    /** Ошибка чтения не публикует defaults поверх последнего значения и не завершает подписку UI. */
    fun <T> observe(name: String, values: Flow<T>): Flow<T> = values.retryWhen { error, attempt ->
        if (error is CancellationException || error !is Exception) false else {
            reportFailure(name, error)
            delay((250L shl attempt.toInt().coerceIn(0, 7)).coerceAtMost(30_000))
            true
        }
    }
}
